/*
 * Game-boundary trace oracle: docs/07-game-intact-oracle.md.
 *
 * The 68000 core (fe2.s -> fe2.s.c, gen/) is never edited.  The oracle sits
 * on the one seam the game has with the host, the hcalls[] table, and swaps
 * every non-NULL entry for a trampoline (src/oracle_tramp.c) at start-up.
 * The trampolines hash what crosses the seam (the "golden streams" of 07
 * section 4.1), drive VBLs in lockstep with the Idle-epoch rule (3.3),
 * replay or record input at the ABI (3.5) and serve a sorted save
 * directory (3.2, T-0.8).  A watchdog thread (3.4) injects a VBL into a
 * spin that makes no hcall, and marks the trace INVALID when the run makes
 * no progress.
 *
 * Nothing here runs unless an oracle flag is given: oracle_install()
 * returns at once and hcalls[] is untouched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>

#include <SDL.h>

#include "main.h"
#include "../host.h"
#include "hostcall.h"
#include "input.h"
#include "keymap.h"
#include "screen.h"
#include "shortcut.h"
#include "oracle.h"

extern HOSTCALL hcalls[];
extern const int hcalls_len;
extern const HOSTCALL oracle_tramps[128];
extern BOOL bDisableSound;

struct oracle_state oracle;

typedef uint64_t u64;

/* ------------------------------------------------------------------ */
/* configuration (from flags)                                          */

static const char *tape_path;      /* replay: read; record: write */
static const char *out_path;       /* golden streams */
static const char *script_path;    /* record: scripted SDL input */
static long seed;
static int seed_given;
static int pace;
static long detail_a = -1, detail_b = -1;
static long sample_every = 1;      /* g/m/f lines every N VBLs */
static long hang_v = -1;           /* --oracle-test-hang */
static int hang_kind;              /* 0 = hcall, 1 = novbl */
static double timeout_s = 0;       /* whole-run wall limit, 0 = none */
static double stall_s = 10;        /* no VBL progress for this long */
static double hcall_s = 10;        /* one hcall longer than this */

/* ------------------------------------------------------------------ */
/* FNV-1a 64, little-endian fixed-width serialisation (07 4.1, I-20)    */

#define FNV_OFF   0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static inline u64 fnv (u64 h, const void *p, size_t n)
{
	const unsigned char *b = p;
	while (n--) { h ^= *b++; h *= FNV_PRIME; }
	return h;
}

static inline u64 fnv32 (u64 h, u32 v)
{
	unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 };
	return fnv (h, b, 4);
}

static inline u64 fnv16 (u64 h, unsigned v)
{
	unsigned char b[2] = { v, v >> 8 };
	return fnv (h, b, 2);
}

/* ------------------------------------------------------------------ */
/* state                                                                */

static HOSTCALL orig[128];
static FILE *out;
static FILE *tape_out;
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *mode_name = "?";

static volatile u32 hu_count;      /* HostUpdate calls = VBLs seen (v) */
static u32 hu_at_last_idle;
static u32 n_flagged;
static volatile u32 n_wd;
static u32 key_ovf;
static u32 a6_first, a6_set, a6_changes;
static u32 n_draw;
static atomic_int exiting;

/* Shared with the watchdog thread.  hcall_enter_ns is the entry time of the
 * hcall in progress, 0 when none is; last_hcall_ns is the time of the last
 * hcall entry or exit.  The main thread stores them with release order
 * after taking the timestamp; the watchdog loads them with acquire order
 * BEFORE it reads the clock, so it never sees a time later than its own
 * "now" (an unsigned now - then could otherwise wrap to a huge age and trip
 * hung-hcall on a healthy run). */
static _Atomic u64 last_hcall_ns, hcall_enter_ns;
static volatile int cur_hc;

/* cumulative hashes, one per stream */
enum { S_G, S_M, S_C, S_S, S_R, S_F, S_P, S_N };
static const char stream_tag[S_N] = { 'g', 'm', 'c', 's', 'r', 'f', 'p' };
static u64 cum[S_N];
static u64 gv;                     /* g hash of the current VBL */
static u32 gv_seq;                 /* hcall records in the current VBL */

/* Screens masked from the RAM hash (07 4.1).  A base is registered when it
 * is handed over for display (VideoBase at Nu_DrawScreen, 0x6f entry, the
 * same bytes the f stream hashes), not at every SetScreenBase call: the
 * game's first SetScreenBase, before A6 is set, passes a base that is never
 * displayed and whose 64000 bytes hold the game's A6 variable block, and
 * masking it left that block out of m. */
#define MAX_SCREENS 32
static u32 screens[MAX_SCREENS];
static int n_screens;

/* Idle call-site histogram (07 3.3) */
#define MAX_SITES 32
static u32 idle_site[MAX_SITES], idle_n[MAX_SITES];
static int n_sites, idle_site_overflow;

/* watchdog injections */
#define MAX_WD 64
static u32 wd_v[MAX_WD];

static u64 t0_ns, start_ns;

static u64 now_ns (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (u64) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* tape (fe2rep 1, 07 section 6)                                        */

struct krec { u32 v; int n; unsigned char code[16]; };
struct mrec { u32 v; int ord; int dx, dy, ax, ay, b; };

static struct krec *ktape; static int nk, kcap, kpos;
static struct mrec *mtape; static int nm, mcap, mpos;
static long tape_end_v = -1;
static char tape_mode[16] = "";
static long tape_seed;

/* record: last values returned by 0x13, for the "no m line" rule */
static int last_ax, last_ay, last_b;
static u32 mouse_ord;

static void die2 (const char *fmt, ...)
{
	va_list ap;
	fprintf (stderr, "oracle: ");
	va_start (ap, fmt);
	vfprintf (stderr, fmt, ap);
	va_end (ap);
	fputc ('\n', stderr);
	exit (2);
}

static void tape_load (const char *path)
{
	FILE *f = fopen (path, "r");
	char line[1024];
	int lineno = 0, header = 0;
	long last_v = -1, keys_in_v = 0;
	if (!f) die2 ("cannot open tape %s: %s", path, strerror (errno));
	while (fgets (line, sizeof line, f)) {
		char *p = line, *tok;
		lineno++;
		line[strcspn (line, "\r\n")] = 0;
		if (!*p || *p == '#') continue;
		if (tape_end_v >= 0)
			die2 ("tape lint: line %d: record after end", lineno);
		if (!header) {
			char req[256] = "";
			int w, h;
			if (sscanf (p, "fe2rep 1 mode=%15s seed=%ld size=%dx%d requires=%255s",
				    tape_mode, &tape_seed, &w, &h, req) != 5)
				die2 ("tape lint: line %d: bad or missing fe2rep 1 header", lineno);
			if (strcmp (req, "-"))
				die2 ("tape lint: requires=%s: this build has no h actions", req);
			header = 1;
			continue;
		}
		tok = strtok (p, " \t");
		if (!tok) continue;
		if (!strcmp (tok, "k") || !strcmp (tok, "m") || !strcmp (tok, "end") || !strcmp (tok, "h")) {
			char *vs = strtok (NULL, " \t"), *e;
			long v;
			if (!vs) die2 ("tape lint: line %d: missing v", lineno);
			v = strtol (vs, &e, 10);
			if (*e || v < 0) die2 ("tape lint: line %d: bad v", lineno);
			if (v < last_v) die2 ("tape lint: line %d: v decreases", lineno);
			if (v != last_v) keys_in_v = 0;
			last_v = v;
			if (!strcmp (tok, "h"))
				die2 ("tape lint: line %d: h action not supported by this build", lineno);
			if (!strcmp (tok, "end")) {
				if (v == 0) die2 ("tape lint: line %d: end 0 replays nothing", lineno);
				tape_end_v = v;
				continue;
			}
			if (!strcmp (tok, "k")) {
				struct krec r = { (u32) v, 0, {0} };
				char *c;
				while ((c = strtok (NULL, " \t"))) {
					unsigned long code = strtoul (c, &e, 16);
					if (*e || code > 0xff) die2 ("tape lint: line %d: bad key code", lineno);
					if (r.n >= 15 || ++keys_in_v > 15)
						die2 ("tape lint: line %d: more than 15 keys in VBL %ld", lineno, v);
					r.code[r.n++] = code;
				}
				if (!r.n) die2 ("tape lint: line %d: k with no keys", lineno);
				if (nk == kcap) ktape = realloc (ktape, (kcap = kcap ? 2*kcap : 256) * sizeof *ktape);
				ktape[nk++] = r;
			} else {
				struct mrec r;
				char *o = strtok (NULL, " \t");
				char *rest = strtok (NULL, "");
				r.v = v;
				if (!o) die2 ("tape lint: line %d: m needs ord", lineno);
				r.ord = !strcmp (o, "*") ? -1 : atoi (o);
				if (!rest || sscanf (rest, "%d %d %d %d %d", &r.dx, &r.dy, &r.ax, &r.ay, &r.b) != 5)
					die2 ("tape lint: line %d: m needs dx dy ax ay buttons", lineno);
				if (r.ax < 0 || r.ax > 319 || r.ay < 0 || r.ay > 199)
					die2 ("tape lint: line %d: ax/ay out of range", lineno);
				if (nm == mcap) mtape = realloc (mtape, (mcap = mcap ? 2*mcap : 256) * sizeof *mtape);
				mtape[nm++] = r;
			}
		} else {
			die2 ("tape lint: line %d: unknown record '%s'", lineno, tok);
		}
	}
	fclose (f);
	if (!header) die2 ("tape lint: no header");
	if (tape_end_v < 0) die2 ("tape lint: no end record");
}

/* ------------------------------------------------------------------ */
/* output                                                               */

static void emit (int stream, u32 v, const char *fmt, ...)
{
	char buf[512];
	int n, show;
	va_list ap;
	va_start (ap, fmt);
	n = vsnprintf (buf, sizeof buf, fmt, ap);
	va_end (ap);
	if (n < 0) return;
	if (n >= (int) sizeof buf) n = sizeof buf - 1;
	cum[stream] = fnv (cum[stream], buf, n);
	show = 1;
	if ((stream == S_G || stream == S_M || stream == S_F || stream == S_P) && sample_every > 1)
		show = (v % sample_every) == 0;
	if (detail_a >= 0 && v >= detail_a && v <= detail_b) show = 1;
	if (show) {
		pthread_mutex_lock (&out_lock);
		fprintf (out, "%c %u %s\n", stream_tag[stream], v, buf);
		pthread_mutex_unlock (&out_lock);
	}
}

/* ------------------------------------------------------------------ */
/* G-trace (07 4.1): the whole boundary state at each hcall entry/exit  */

static void g_record (int hc, int phase)
{
	u64 h = FNV_OFF;
	u32 a7 = Regs[15]._u32, cnt = 0, i;
	h = fnv32 (h, hc);
	h = fnv32 (h, phase);
	for (i = 0; i < 16; i++) h = fnv32 (h, Regs[i]._u32);
	h = fnv32 (h, N); h = fnv32 (h, nZ); h = fnv32 (h, V); h = fnv32 (h, C); h = fnv32 (h, X);
	h = fnv32 (h, bN); h = fnv32 (h, bnZ); h = fnv32 (h, bV); h = fnv32 (h, bC); h = fnv32 (h, bX);
	h = fnv32 (h, rdest);
	h = fnv32 (h, exceptions_pending);
	h = fnv32 (h, exceptions_pending_nums[0]);
	h = fnv32 (h, exceptions_pending_nums[1]);
	if (a7 < MEM_SIZE) cnt = (MEM_SIZE - a7 < 32) ? MEM_SIZE - a7 : 32;
	h = fnv32 (h, cnt);
	if (cnt) h = fnv (h, m68kram + a7, cnt);

	gv = fnv32 (fnv32 (gv, (u32) h), (u32) (h >> 32));
	gv_seq++;
	if (detail_a >= 0 && hu_count >= detail_a && hu_count <= detail_b) {
		pthread_mutex_lock (&out_lock);
		fprintf (out, "G %u %u %02x %c %016llx\n", hu_count, gv_seq, hc,
			 phase ? 'x' : 'e', (unsigned long long) h);
		pthread_mutex_unlock (&out_lock);
	}
}

static void g_flush (void)
{
	emit (S_G, hu_count, "%016llx", (unsigned long long) gv);
	gv = FNV_OFF;
	gv_seq = 0;
}

/* ------------------------------------------------------------------ */
/* RAM (m) and checkpoints (c), at HostUpdate entry                     */

static void add_screen (u32 base)
{
	int i;
	for (i = 0; i < n_screens; i++) if (screens[i] == base) return;
	if (n_screens < MAX_SCREENS) screens[n_screens++] = base;
}

static int cmp_u32 (const void *a, const void *b)
{
	u32 x = *(const u32 *) a, y = *(const u32 *) b;
	return x < y ? -1 : x > y;
}

static u64 ram_hash (void)
{
	/* masked ranges: each screen (64000 bytes) and bmp_under_mouse */
	u32 lo[MAX_SCREENS + 1], hi[MAX_SCREENS + 1], pos = 0;
	u32 pairs[2 * (MAX_SCREENS + 1)];
	int n = 0, i;
	u64 h = FNV_OFF;
	for (i = 0; i < n_screens; i++) { pairs[2*n] = screens[i]; pairs[2*n+1] = screens[i] + 64000; n++; }
	if (a6_set) { pairs[2*n] = a6_first + 16480; pairs[2*n+1] = a6_first + 16480 + 256; n++; }
	qsort (pairs, n, 2 * sizeof (u32), cmp_u32);
	for (i = 0; i < n; i++) { lo[i] = pairs[2*i]; hi[i] = pairs[2*i+1]; }
	for (i = 0; i < n; i++) {
		u32 a = lo[i] < MEM_SIZE ? lo[i] : MEM_SIZE;
		u32 b = hi[i] < MEM_SIZE ? hi[i] : MEM_SIZE;
		if (a > pos) h = fnv (h, m68kram + pos, a - pos);
		if (b > pos) pos = b;
	}
	if (pos < MEM_SIZE) h = fnv (h, m68kram + pos, MEM_SIZE - pos);
	return h;
}

static u32 rd32 (u32 a) { return a + 4 <= MEM_SIZE ? (u32) STMemory_ReadLong (a) : 0; }
static u32 rd16 (u32 a) { return a + 2 <= MEM_SIZE ? (u16) STMemory_ReadWord (a) : 0; }

static void vbl_state (u32 v)
{
	u32 a6 = Regs[14]._u32;
	if (!a6_set) { a6_first = a6; a6_set = 1; }
	else if (a6 != a6_first) a6_changes++;
	emit (S_M, v, "%016llx", (unsigned long long) ram_hash ());
	emit (S_C, v, "%08x %08x %08x %04x", rd32 (a6_first + 524), rd32 (a6_first + 548),
	      rd32 (a6_first + 552), rd16 (a6_first + 14482));
}

static u64 hash_ram_range (u32 a, u32 len)
{
	if (a >= MEM_SIZE) return FNV_OFF;
	if (len > MEM_SIZE - a) len = MEM_SIZE - a;
	return fnv (FNV_OFF, m68kram + a, len);
}

static u64 hash_cstr (u32 a)
{
	u64 h = FNV_OFF;
	int i;
	for (i = 0; i < 64 && a + i < MEM_SIZE; i++) {
		unsigned char c = m68kram[a + i];
		if (!c) break;
		h = fnv (h, &c, 1);
	}
	return h;
}

static void frame_hash (u32 v)
{
	u64 h = FNV_OFF;
	int i;
	if (VideoBase + 64000 <= MEM_SIZE) h = fnv (h, m68kram + VideoBase, 64000);
	h = fnv32 (h, (u32) VideoBase);
	h = fnv32 (h, len_main_palette);
	for (i = 0; i < len_main_palette && i < 256; i++) h = fnv16 (h, MainPalette[i]);
	for (i = 0; i < 16; i++) h = fnv16 (h, CtrlPalette[i]);
	emit (S_F, v, "%016llx", (unsigned long long) h);
}

static void present_hash (u32 v)
{
	u64 h = FNV_OFF;
	if (VideoRaster && VideoBase + 64000 <= MEM_SIZE)
		h = fnv (h, VideoRaster, 64000);
	emit (S_P, v, "%016llx", (unsigned long long) h);
}

/* ------------------------------------------------------------------ */
/* watchdog (07 3.4)                                                    */

static void invalid (const char *reason)
{
	char buf[160];
	snprintf (buf, sizeof buf, "invalid reason=%s v=%u hc=%02x\n",
		  reason, hu_count, cur_hc & 0xff);
	pthread_mutex_lock (&out_lock);
	if (out) { fputs (buf, out); fflush (out); }
	if (tape_out) fflush (tape_out);
	fprintf (stderr, "oracle: %s", buf);
	_exit (3);
}

static u64 machine_hash (void)
{
	u64 h = fnv (FNV_OFF, m68kram, MEM_SIZE);
	int i;
	for (i = 0; i < 16; i++) h = fnv32 (h, Regs[i]._u32);
	h = fnv32 (h, N); h = fnv32 (h, nZ); h = fnv32 (h, V); h = fnv32 (h, C); h = fnv32 (h, X);
	return h;
}

/* now - then, never wrapping: 0 if then is not before now */
static inline u64 age_ns (u64 now, u64 then)
{
	return now > then ? now - then : 0;
}

static void *watchdog (void *arg)
{
	u32 last_v = hu_count, inj_since_progress = 0;
	u64 last_progress = now_ns (), prev = 0;
	int stable = 0;
	(void) arg;
	for (;;) {
		u64 t, enter, last;
		usleep (100000);
		if (atomic_load (&exiting)) return NULL;
		/* load the shared times first, then read the clock (see above) */
		enter = atomic_load_explicit (&hcall_enter_ns, memory_order_acquire);
		last = atomic_load_explicit (&last_hcall_ns, memory_order_acquire);
		t = now_ns ();
		if (timeout_s > 0 && age_ns (t, start_ns) > (u64) (timeout_s * 1e9)) invalid ("timeout");
		if (enter && age_ns (t, enter) > (u64) (hcall_s * 1e9)) invalid ("hung-hcall");
		if (hu_count != last_v) {
			last_v = hu_count;
			last_progress = t;
			inj_since_progress = 0;
		}
		if (age_ns (t, last_progress) > (u64) (stall_s * 1e9)) invalid ("no-vbl-progress");
		if (!enter && age_ns (t, last) > 250000000ULL && exceptions_pending == 0) {
			u64 h = machine_hash ();
			stable = (stable && h == prev) ? stable + 1 : 1;
			prev = h;
			if (stable >= 3) {
				FlagException (0);
				pthread_mutex_lock (&out_lock);
				fprintf (out, "w %u hc=%02x\n", hu_count, cur_hc & 0xff);
				pthread_mutex_unlock (&out_lock);
				if (n_wd < MAX_WD) wd_v[n_wd] = hu_count;
				n_wd++;
				stable = 0;
				if (++inj_since_progress > 3) invalid ("watchdog-no-progress");
			}
		} else {
			stable = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* sorted save directory (07 3.2 "Directory order", T-0.8)              */

static char **dir_ent;
static int dir_n, dir_pos;
static char dir_name[1024];

static int cmp_str (const void *a, const void *b)
{
	return strcmp (*(char * const *) a, *(char * const *) b);
}

static void dir_free (void)
{
	int i;
	for (i = 0; i < dir_n; i++) free (dir_ent[i]);
	free (dir_ent);
	dir_ent = NULL;
	dir_n = dir_pos = 0;
}

static void oracle_fopendir (void)
{
	u32 p = GetReg (REG_A2);
	int i, cap = 0;
	DIR *d;
	struct dirent *e;
	for (i = 0; i < 63 && p + i < MEM_SIZE; i++) {
		dir_name[i] = m68kram[p + i];
		if (!dir_name[i]) break;
	}
	dir_name[i] = 0;
	dir_free ();
	d = opendir (dir_name);
	if (!d) { SetReg (REG_D0, -1); return; }
	while ((e = readdir (d))) {
		if (!strcmp (e->d_name, ".") || !strcmp (e->d_name, "..")) continue;
		if (dir_n == cap) dir_ent = realloc (dir_ent, (cap = cap ? 2*cap : 32) * sizeof *dir_ent);
		dir_ent[dir_n++] = strdup (e->d_name);
	}
	closedir (d);
	qsort (dir_ent, dir_n, sizeof *dir_ent, cmp_str);
	SetReg (REG_D0, 0);
}

static void oracle_freaddir (void)
{
	char name[14], path[2048];
	struct stat st;
	u32 p;
	int i;
	if (dir_pos >= dir_n) { SetReg (REG_D0, -1); return; }
	strncpy (name, dir_ent[dir_pos], sizeof name);
	name[sizeof name - 1] = 0;
	snprintf (path, sizeof path, "%s/%s", dir_name, dir_ent[dir_pos]);
	dir_pos++;
	memset (&st, 0, sizeof st);
	stat (path, &st);
	p = GetReg (REG_A0);
	for (i = 0; i < (int) sizeof name; i++) STMemory_WriteByte (p + i, name[i]);
	SetReg (REG_D2, S_ISDIR (st.st_mode) ? 0x10 : 0);
	SetReg (REG_D1, (int) st.st_size);
	SetReg (REG_D0, 0);
}

static void oracle_fclosedir (void)
{
	dir_free ();
}

/* ------------------------------------------------------------------ */
/* scripted live input for record mode (--oracle-script)                */

struct sev { u32 v; SDL_Event ev; };
static struct sev *script; static int nscript, script_pos;

static void script_load (const char *path)
{
	FILE *f = fopen (path, "r");
	char line[256];
	int cap = 0, lineno = 0;
	if (!f) die2 ("cannot open script %s: %s", path, strerror (errno));
	while (fgets (line, sizeof line, f)) {
		struct sev s;
		char kind[16], st[8];
		int a, b, c, d;
		unsigned sc;
		lineno++;
		if (line[0] == '#' || line[0] == '\n') continue;
		memset (&s, 0, sizeof s);
		if (sscanf (line, "%u %15s", &s.v, kind) != 2) die2 ("script line %d: bad", lineno);
		if (!strcmp (kind, "key") && sscanf (line, "%*u %*s %d %x %7s", &a, &sc, st) == 3) {
			s.ev.type = strcmp (st, "up") ? SDL_KEYDOWN : SDL_KEYUP;
			s.ev.key.state = s.ev.type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
			s.ev.key.keysym.sym = a;
			s.ev.key.keysym.scancode = sc;
		} else if (!strcmp (kind, "motion") && sscanf (line, "%*u %*s %d %d %d %d", &a, &b, &c, &d) == 4) {
			s.ev.type = SDL_MOUSEMOTION;
			s.ev.motion.x = a; s.ev.motion.y = b;
			s.ev.motion.xrel = c; s.ev.motion.yrel = d;
		} else if (!strcmp (kind, "button") && sscanf (line, "%*u %*s %d %7s %d %d", &a, st, &b, &c) == 4) {
			s.ev.type = strcmp (st, "up") ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			s.ev.button.button = a;
			s.ev.button.state = s.ev.type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
			s.ev.button.x = b; s.ev.button.y = c;
		} else if (!strcmp (kind, "quit")) {
			s.ev.type = SDL_QUIT;
		} else {
			die2 ("script line %d: bad", lineno);
		}
		if (nscript && s.v < script[nscript-1].v) die2 ("script line %d: v decreases", lineno);
		if (nscript == cap) script = realloc (script, (cap = cap ? 2*cap : 64) * sizeof *script);
		script[nscript++] = s;
	}
	fclose (f);
}

/* ------------------------------------------------------------------ */
/* special hcalls                                                       */

static void replay_pump (void)
{
	/* The replay half of Main_EventHandler: keep SDL_QUIT, drop all live
	 * keyboard, mouse and joystick input (07 3.2, 3.5).  The joystick is
	 * never polled, so it cannot reach the game either. */
	SDL_Event ev;
	while (SDL_PollEvent (&ev)) {
		if (ev.type == SDL_QUIT) {
			SDL_Quit ();
			exit (0);
		}
	}
}

static void push_key (unsigned char code)
{
	int pending = (input.buf_tail - input.buf_head + SIZE_KEYBUF) % SIZE_KEYBUF;
	if (pending >= SIZE_KEYBUF - 1) key_ovf++;
	Input_PressSTKey (code, TRUE);  /* code already carries the break bit */
}

static void hostupdate (void)
{
	u32 v;
	g_flush ();                     /* close VBL hu_count */
	v = ++hu_count;
	mouse_ord = 0;
	g_record (0x07, 0);
	vbl_state (v);

	if (hang_v >= 0 && hang_kind == 0 && v >= (u32) hang_v) {
		for (;;) pause ();      /* deliberately hung hcall: --oracle-test-hang */
	}

	if (oracle.mode == ORACLE_REPLAY) {
		Keymap_DebounceAllKeys ();
		ShortCut_CheckKeys ();
		replay_pump ();
		if ((long) v == tape_end_v) {
			SDL_Quit ();
			exit (0);
		}
		while (kpos < nk && ktape[kpos].v < v) kpos++;
		while (kpos < nk && ktape[kpos].v == v) {
			int i;
			for (i = 0; i < ktape[kpos].n; i++) push_key (ktape[kpos].code[i]);
			kpos++;
		}
	} else {
		int tail0 = input.buf_tail, head0 = input.buf_head, n, i, pending0;
		/* record ignores Ctrl-E: the renderer is fixed by the tape header */
		if (ShortCutKey.Key == SDLK_e) {
			fprintf (stderr, "oracle: record mode ignores the renderer toggle\n");
			ShortCutKey.Key = 0;
		}
		while (script_pos < nscript && script[script_pos].v <= v) {
			SDL_PushEvent (&script[script_pos].ev);
			script_pos++;
		}
		orig[0x07] ();
		n = (input.buf_tail - tail0 + SIZE_KEYBUF) % SIZE_KEYBUF;
		pending0 = (tail0 - head0 + SIZE_KEYBUF) % SIZE_KEYBUF;
		if (pending0 + n >= SIZE_KEYBUF) key_ovf += pending0 + n - (SIZE_KEYBUF - 1);
		if (n) {
			fprintf (tape_out, "k %u", v);
			for (i = 0; i < n; i++)
				fprintf (tape_out, " %02x", input.key_buf[(tail0 + i) % SIZE_KEYBUF]);
			fputc ('\n', tape_out);
		}
	}
	g_record (0x07, 1);
}

static void mouse_input (void)
{
	u32 movp = rd32 (Regs[15]._u32), absp = rd32 (Regs[15]._u32 + 4);
	int dx, dy, ax, ay, b;
	u32 v = hu_count, ord = mouse_ord++;
	g_record (0x13, 0);
	if (movp + 6 > MEM_SIZE || absp + 4 > MEM_SIZE) {
		/* not a shape the host accepts either; keep the original's behaviour */
		orig[0x13] ();
		g_record (0x13, 1);
		return;
	}
	if (oracle.mode == ORACLE_RECORD) {
		u16 x0 = rd16 (movp), y0 = rd16 (movp + 2);
		orig[0x13] ();
		dx = (s16) (rd16 (movp) - x0);
		dy = (s16) (rd16 (movp + 2) - y0);
		ax = (s16) rd16 (absp);
		ay = (s16) rd16 (absp + 2);
		b = rd16 (movp + 4) & ~0xf8 & 0xffff;
		if (dx || dy || ax != last_ax || ay != last_ay || b != last_b)
			fprintf (tape_out, "m %u %u %d %d %d %d %d\n", v, ord, dx, dy, ax, ay, b);
	} else {
		int i, found = 0;
		dx = dy = 0; ax = last_ax; ay = last_ay; b = last_b;
		while (mpos < nm && mtape[mpos].v < v) mpos++;
		for (i = mpos; i < nm && mtape[i].v == v; i++) {
			if (mtape[i].ord == (int) ord || mtape[i].ord == -1) { found = 1; break; }
		}
		if (found) {
			dx = mtape[i].dx; dy = mtape[i].dy;
			ax = mtape[i].ax; ay = mtape[i].ay; b = mtape[i].b;
		}
		STMemory_WriteWord (movp, rd16 (movp) + dx);
		STMemory_WriteWord (movp + 2, rd16 (movp + 2) + dy);
		STMemory_WriteWord (absp, ax);
		STMemory_WriteWord (absp + 2, ay);
		STMemory_WriteWord (movp + 4, 0xf8 | b);
	}
	last_ax = ax; last_ay = ay; last_b = b;
	g_record (0x13, 1);
}

static void idle (void)
{
	u32 site = rd32 (Regs[15]._u32);
	int i;
	g_record (0x1f, 0);
	if (pace) {
		u64 due = t0_ns + (u64) hu_count * 20000000ULL, t = now_ns ();
		if (due > t) {
			struct timespec ts = { (time_t) ((due - t) / 1000000000ULL), (long) ((due - t) % 1000000000ULL) };
			nanosleep (&ts, NULL);
		}
	}
	/* the Idle-epoch rule (07 3.3) */
	if (!(hang_v >= 0 && hang_kind == 1 && hu_count >= (u32) hang_v)) {
		if (exceptions_pending_nums[0] == 0 && hu_count == hu_at_last_idle) {
			if (exception_handlers[0]) n_flagged++;
			FlagException (0);
		}
	}
	hu_at_last_idle = hu_count;
	for (i = 0; i < n_sites && idle_site[i] != site; i++) ;
	if (i < n_sites) idle_n[i]++;
	else if (n_sites < MAX_SITES) { idle_site[n_sites] = site; idle_n[n_sites++] = 1; }
	else idle_site_overflow++;
	orig[0x1f] ();
	g_record (0x1f, 1);
}

/* ------------------------------------------------------------------ */
/* the one entry point every trampoline calls                           */

void oracle_call (int n)
{
	u64 t = now_ns ();
	cur_hc = n;
	atomic_store_explicit (&last_hcall_ns, t, memory_order_release);
	atomic_store_explicit (&hcall_enter_ns, t, memory_order_release);

	switch (n) {
	case 0x07: hostupdate (); break;
	case 0x13: mouse_input (); break;
	case 0x1f: idle (); break;
	default:
		g_record (n, 0);
		if (n == 0x23) {
			/* S_save: length D7, bytes at A4, name at D1 */
			u32 len = Regs[7]._u32;
			emit (S_S, hu_count, "%08x %016llx %016llx", len,
			      (unsigned long long) hash_ram_range (Regs[12]._u32, len),
			      (unsigned long long) hash_cstr (Regs[1]._u32));
		}
		if (n == 0x6f) {
			add_screen ((u32) VideoBase);
			frame_hash (hu_count);
		}
		if (n == 0x25) oracle_fopendir ();
		else if (n == 0x26) oracle_freaddir ();
		else if (n == 0x27) oracle_fclosedir ();
		else orig[n] ();
		if (n == 0x22) {
			u32 len = Regs[0]._u32;
			emit (S_R, hu_count, "%08x %016llx", len,
			      (unsigned long long) hash_ram_range (Regs[12]._u32, len));
		}
		if (n == 0x6f) { present_hash (hu_count); n_draw++; }
		g_record (n, 1);
		break;
	}
	atomic_store_explicit (&last_hcall_ns, now_ns (), memory_order_release);
	atomic_store_explicit (&hcall_enter_ns, 0, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* exit: stat and end lines (07 4.1 counters, 6 "end")                  */

static void oracle_atexit (void)
{
	int i, early;
	if (atomic_exchange (&exiting, 1)) return;
	g_flush ();
	pthread_mutex_lock (&out_lock);
	fprintf (out, "stat flagged=%u hostupdate=%u wd=%u wdv=", n_flagged, hu_count, n_wd);
	for (i = 0; i < (int) n_wd && i < MAX_WD; i++) fprintf (out, "%s%u", i ? "," : "", wd_v[i]);
	if (!n_wd) fputc ('-', out);
	fprintf (out, " keyovf=%u a6=%08x a6chg=%u draws=%u screens=%d idle=",
		 key_ovf, a6_first, a6_changes, n_draw, n_screens);
	for (i = 0; i < n_sites; i++) fprintf (out, "%s%06x:%u", i ? "," : "", idle_site[i], idle_n[i]);
	if (!n_sites) fputc ('-', out);
	if (idle_site_overflow) fprintf (out, ",+%d", idle_site_overflow);
	fputc ('\n', out);
	fprintf (out, "end %u", hu_count);
	for (i = 0; i < S_N; i++) fprintf (out, " %c=%016llx", stream_tag[i], (unsigned long long) cum[i]);
	fputc ('\n', out);
	early = oracle.mode == ORACLE_REPLAY && (long) hu_count != tape_end_v;
	if (early)
		fprintf (out, "invalid reason=early-exit v=%u hc=%02x\n", hu_count, cur_hc & 0xff);
	fclose (out);
	out = NULL;
	pthread_mutex_unlock (&out_lock);
	if (tape_out) {
		fprintf (tape_out, "end %u\n", hu_count);
		fclose (tape_out);
		tape_out = NULL;
	}
	if (early) {
		/* a quit (window close, SIGTERM via SDL_QUIT) before the tape's
		 * end: the trace is incomplete, so exit 3 like the watchdog */
		fprintf (stderr, "oracle: invalid reason=early-exit v=%u (tape end %ld)\n",
			 hu_count, tape_end_v);
		fflush (NULL);
		_exit (3);
	}
}

static void oracle_segv (int sig)
{
	static const char msg[] = "oracle: SIGSEGV in oracle mode\n";
	ssize_t r = write (2, msg, sizeof msg - 1);
	(void) r; (void) sig;
	_exit (139);
}

/* ------------------------------------------------------------------ */
/* flags and installation                                               */

static void usage (void)
{
	printf ("Oracle options (docs/07-game-intact-oracle.md); all default off:\n"
		"  --oracle-replay TAPE    replay a fe2rep 1 tape in lockstep\n"
		"  --oracle-record TAPE    record live input to TAPE in lockstep\n"
		"  --oracle-out FILE       golden stream output (required)\n"
		"  --oracle-script FILE    record: inject scripted SDL input\n"
		"  --oracle-detail A:B     per-hcall G lines for VBLs A..B\n"
		"  --oracle-sample N       g/m/f/p lines every N VBLs (default 1)\n"
		"  --oracle-timeout S      INVALID after S seconds wall time\n"
		"  --oracle-stall S        INVALID after S s with no VBL (default 10)\n"
		"  --oracle-test-hang V[:novbl]  test: hang at VBL V\n"
		"  --seed N                srand(N) (default 0 in oracle modes)\n"
		"  --pace                  sleep to 50 Hz (changes no stream)\n");
}

static long need_num (int i, int argc, char *argv[])
{
	char *e;
	long v;
	if (i >= argc) die2 ("%s needs a value", argv[i-1]);
	v = strtol (argv[i], &e, 10);
	if (*e) die2 ("%s: bad number %s", argv[i-1], argv[i]);
	return v;
}

void oracle_parse_args (int *argc, char *argv[])
{
	int i, j = 1, fullscreen = 0;
	for (i = 1; i < *argc; i++) {
		char *a = argv[i];
		if (!strcmp (a, "--oracle-replay") || !strcmp (a, "--oracle-record")) {
			if (oracle.mode) die2 ("only one of --oracle-replay/--oracle-record");
			if (++i >= *argc) die2 ("%s needs a tape", a);
			oracle.mode = a[11] == 'p' ? ORACLE_REPLAY : ORACLE_RECORD;
			tape_path = argv[i];
			oracle.active = 1;
		} else if (!strcmp (a, "--oracle-out")) {
			if (++i >= *argc) die2 ("--oracle-out needs a file");
			out_path = argv[i]; oracle.active = 1;
		} else if (!strcmp (a, "--oracle-script")) {
			if (++i >= *argc) die2 ("--oracle-script needs a file");
			script_path = argv[i]; oracle.active = 1;
		} else if (!strcmp (a, "--oracle-detail")) {
			if (++i >= *argc || sscanf (argv[i], "%ld:%ld", &detail_a, &detail_b) != 2)
				die2 ("--oracle-detail needs A:B");
			oracle.active = 1;
		} else if (!strcmp (a, "--oracle-sample")) {
			sample_every = need_num (++i, *argc, argv);
			if (sample_every < 1) die2 ("--oracle-sample must be >= 1");
			oracle.active = 1;
		} else if (!strcmp (a, "--oracle-timeout")) {
			timeout_s = need_num (++i, *argc, argv); oracle.active = 1;
		} else if (!strcmp (a, "--oracle-stall")) {
			stall_s = need_num (++i, *argc, argv); oracle.active = 1;
		} else if (!strcmp (a, "--oracle-test-hang")) {
			char kind[16] = "";
			if (++i >= *argc || sscanf (argv[i], "%ld:%15s", &hang_v, kind) < 1)
				die2 ("--oracle-test-hang needs V[:novbl]");
			hang_kind = !strcmp (kind, "novbl");
			if (*kind && strcmp (kind, "novbl") && strcmp (kind, "hcall"))
				die2 ("--oracle-test-hang kind must be hcall or novbl");
			oracle.active = 1;
		} else if (!strcmp (a, "--seed")) {
			seed = need_num (++i, *argc, argv); seed_given = 1; oracle.active = 1;
		} else if (!strcmp (a, "--pace")) {
			pace = 1; oracle.active = 1;
		} else if (!strcmp (a, "--oracle-help")) {
			usage ();
			exit (0);
		} else {
			if (!strcmp (a, "--fullscreen") || !strcmp (a, "-f")) fullscreen = 1;
			argv[j++] = a;
		}
	}
	argv[j] = NULL;
	*argc = j;
	if (!oracle.active) return;
	if (fullscreen) die2 ("--fullscreen is refused in oracle modes");
	if (!oracle.mode) die2 ("an oracle flag needs --oracle-replay or --oracle-record");
	if (!out_path) die2 ("--oracle-out FILE is required");
	if (script_path && oracle.mode != ORACLE_RECORD) die2 ("--oracle-script is for record mode");
	/* pins (07 3.2): no music, no sfx, fixed seed */
	bDisableSound = TRUE;
	if (oracle.mode == ORACLE_REPLAY) tape_load (tape_path);
	if (!seed_given && oracle.mode == ORACLE_REPLAY) seed = tape_seed;
	srand ((unsigned) seed);
	if (script_path) script_load (script_path);
}

int oracle_lockstep (void)
{
	return oracle.active && (oracle.mode == ORACLE_REPLAY || oracle.mode == ORACLE_RECORD);
}

void oracle_install (void)
{
	int i;
	pthread_t th;
	if (!oracle.active) return;

	if (hcalls_len > 128) die2 ("hcalls[] has %d entries, trampolines cover 128", hcalls_len);
#ifdef WITH_GL
	mode_name = use_renderer == R_OLD ? "gl-rold" : use_renderer == R_GL ? "gl-rgl" : "gl-rwire";
#else
	mode_name = "soft";
#endif
	out = fopen (out_path, "w");
	if (!out) die2 ("cannot write %s: %s", out_path, strerror (errno));
	if (oracle.mode == ORACLE_RECORD) {
		tape_out = fopen (tape_path, "w");
		if (!tape_out) die2 ("cannot write %s: %s", tape_path, strerror (errno));
		fprintf (tape_out, "fe2rep 1 mode=%s seed=%ld size=%dx%d requires=-\n",
			 mode_name, seed, screen_w, screen_h);
	} else if (strcmp (tape_mode, mode_name)) {
		fprintf (stderr, "oracle: warning: tape mode=%s replayed in mode=%s; "
			 "expect renderer-dependent divergence (07 3.7)\n", tape_mode, mode_name);
	}
	fprintf (out, "# fe2gold 1 mode=%s oracle=%s seed=%ld", mode_name,
		 oracle.mode == ORACLE_REPLAY ? "replay" : "record", seed);
	if (oracle.mode == ORACLE_REPLAY) fprintf (out, " tape_end=%ld", tape_end_v);
	fputc ('\n', out);

	for (i = 0; i < S_N; i++) cum[i] = FNV_OFF;
	gv = FNV_OFF;

	/* the seam: copy the table, swap every non-NULL entry (07 2, T-0.2) */
	memcpy (orig, hcalls, hcalls_len * sizeof *hcalls);
	for (i = 0; i < hcalls_len; i++)
		if (hcalls[i]) hcalls[i] = oracle_tramps[i];

	atexit (oracle_atexit);
	signal (SIGSEGV, oracle_segv);
	start_ns = t0_ns = now_ns ();
	atomic_store (&last_hcall_ns, start_ns);
	if (pthread_create (&th, NULL, watchdog, NULL)) die2 ("cannot start the watchdog");
	pthread_detach (th);
}
