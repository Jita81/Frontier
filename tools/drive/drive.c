/*
 * drive.c - libfe2drive.dylib, a scripted input driver for an UNMODIFIED
 * GLFrontier binary on macOS.
 *
 * Load it with DYLD_INSERT_LIBRARIES (drive.py does this).  It:
 *   1. interposes SDL_PollEvent (dyld __interpose) so that every input action
 *      runs on the game's own thread, at the top of Main_EventHandler's poll
 *      loop, and real keyboard/mouse events from the desktop are dropped
 *      while a script runs (they would otherwise move the ST pointer);
 *   2. resolves the game's exported globals with dlsym(RTLD_DEFAULT) and
 *      exits with status 4 if any is missing;
 *   3. starts a driver thread that reads the script named by FE2_DRIVE_SCRIPT
 *      and plays it step by step, logging each step to stderr as "drive: ...".
 *
 * Without FE2_DRIVE_SCRIPT the library only passes SDL_PollEvent through.
 *
 * Step grammar: see README.md.  The ST scancode names below follow the
 * public Atari ST IKBD scancode set (Atari ST keyboard documentation); the
 * Mac key codes are the public kVK_* constants of HIToolbox/Events.h.
 *
 * This is a test convenience, not an oracle: timing comes from the host
 * clock of a second thread, so frame-exact reproducibility is not promised.
 */
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- SDL 1.2 ABI subset (checked against SDL.h by `make check-layout`) -- */
enum { EV_KEYDOWN = 2, EV_KEYUP = 3, EV_MOUSEMOTION = 4, EV_MOUSEBUTTONDOWN = 5,
       EV_MOUSEBUTTONUP = 6, EV_QUIT = 12 };
typedef struct { uint8_t scancode; int sym; int mod; uint16_t unicode; } drv_keysym;
typedef struct { uint8_t type, which, state; drv_keysym keysym; } drv_keyevent;
typedef union { uint8_t type; drv_keyevent key; unsigned char pad[24]; } drv_event;
_Static_assert(sizeof(drv_event) == 24, "SDL_Event is 24 bytes in SDL 1.2 (LP64)");
_Static_assert(__builtin_offsetof(drv_keyevent, keysym.sym) == 8, "keysym.sym at 8");
_Static_assert(__builtin_offsetof(drv_keyevent, keysym.mod) == 12, "keysym.mod at 12");

enum { KM_LSHIFT = 0x1, KM_RSHIFT = 0x2, KM_LCTRL = 0x40, KM_RCTRL = 0x80,
       KM_LALT = 0x100, KM_RALT = 0x200, KM_LMETA = 0x400, KM_RMETA = 0x800,
       KM_NUM = 0x1000, KM_CAPS = 0x2000, KM_MODE = 0x4000 };

extern int SDL_PollEvent(void *event);   /* bound at load time, see Makefile */

/* ---- the game's INPUT struct (src/input.h), offsets for SDLK_LAST=323 --- */
#define IN_KEY_BUF      323     /* unsigned char key_buf[16]: the IKBD ring */
#define IN_KEYBUF_SIZE  16
#define IN_BUF_TAIL     360     /* int buf_tail: next write index */
#define IN_CUR_MOUSEBUT 372
#define IN_MOTION_X     376     /* int motion_x, motion_y: relative motion */
#define IN_MOTION_Y     380     /* since Call_GetMouseInput last read it */
#define IN_ABS_X        384
#define IN_ABS_Y        388
#define IN_SIZE         400

typedef void (*press_st_key_fn)(unsigned char scancode, int press);
static press_st_key_fn g_press;
static unsigned char  *g_input;
static int            *g_screen_w, *g_screen_h;
static int            *g_view[4];       /* view_x, view_y, view_w, view_h (src/screen.h) */
static unsigned short *g_shortcut_key;  /* &ShortCutKey.Key (src/shortcut.h) */

/* ---- logging ------------------------------------------------------------ */
static struct timespec g_t0;
static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)(t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) / 1e9;
}
static void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(stderr, "drive: [%9.3f] %s\n", now_s(), buf);
	fflush(stderr);
}
static void die(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void die(int code, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(stderr, "drive: [%9.3f] FATAL: %s (exit %d)\n", now_s(), buf, code);
	fflush(stderr);
	_exit(code);
}

/* ---- ST scancode names (Atari ST IKBD set) ------------------------------ */
typedef struct { const char *name; unsigned char code; } st_name;
static const st_name ST_NAMES[] = {
	{"esc", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05},
	{"5", 0x06}, {"6", 0x07}, {"7", 0x08}, {"8", 0x09}, {"9", 0x0a},
	{"0", 0x0b}, {"minus", 0x0c}, {"equals", 0x0d}, {"backspace", 0x0e},
	{"tab", 0x0f}, {"q", 0x10}, {"w", 0x11}, {"e", 0x12}, {"r", 0x13},
	{"t", 0x14}, {"y", 0x15}, {"u", 0x16}, {"i", 0x17}, {"o", 0x18},
	{"p", 0x19}, {"lbracket", 0x1a}, {"rbracket", 0x1b}, {"return", 0x1c},
	{"control", 0x1d}, {"a", 0x1e}, {"s", 0x1f}, {"d", 0x20}, {"f", 0x21},
	{"g", 0x22}, {"h", 0x23}, {"j", 0x24}, {"k", 0x25}, {"l", 0x26},
	{"semicolon", 0x27}, {"apostrophe", 0x28}, {"grave", 0x29},
	{"lshift", 0x2a}, {"backslash", 0x2b}, {"z", 0x2c}, {"x", 0x2d},
	{"c", 0x2e}, {"v", 0x2f}, {"b", 0x30}, {"n", 0x31}, {"m", 0x32},
	{"comma", 0x33}, {"period", 0x34}, {"slash", 0x35}, {"rshift", 0x36},
	{"alternate", 0x38}, {"space", 0x39}, {"capslock", 0x3a},
	{"f1", 0x3b}, {"f2", 0x3c}, {"f3", 0x3d}, {"f4", 0x3e}, {"f5", 0x3f},
	{"f6", 0x40}, {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44},
	{"clrhome", 0x47}, {"up", 0x48}, {"kp_minus", 0x4a}, {"left", 0x4b},
	{"right", 0x4d}, {"kp_plus", 0x4e}, {"down", 0x50}, {"insert", 0x52},
	{"delete", 0x53}, {"iso", 0x60}, {"undo", 0x61}, {"help", 0x62},
	{"kp_lparen", 0x63}, {"kp_rparen", 0x64}, {"kp_slash", 0x65},
	{"kp_star", 0x66}, {"kp7", 0x67}, {"kp8", 0x68}, {"kp9", 0x69},
	{"kp4", 0x6a}, {"kp5", 0x6b}, {"kp6", 0x6c}, {"kp1", 0x6d},
	{"kp2", 0x6e}, {"kp3", 0x6f}, {"kp0", 0x70}, {"kp_period", 0x71},
	{"kp_enter", 0x72},
};

/* ---- SDL 1.2 key names for `sdlkey`: SDLKey value + macOS kVK code ------- */
/* kVK -1: no Mac key code is sent (scancode 0 is kVK_ANSI_A, so 0xff).      */
typedef struct { const char *name; int sym; int kvk; } sdl_name;
static const sdl_name SDL_NAMES[] = {
	{"a", 'a', 0x00}, {"s", 's', 0x01}, {"d", 'd', 0x02}, {"f", 'f', 0x03},
	{"h", 'h', 0x04}, {"g", 'g', 0x05}, {"z", 'z', 0x06}, {"x", 'x', 0x07},
	{"c", 'c', 0x08}, {"v", 'v', 0x09}, {"b", 'b', 0x0b}, {"q", 'q', 0x0c},
	{"w", 'w', 0x0d}, {"e", 'e', 0x0e}, {"r", 'r', 0x0f}, {"y", 'y', 0x10},
	{"t", 't', 0x11}, {"1", '1', 0x12}, {"2", '2', 0x13}, {"3", '3', 0x14},
	{"4", '4', 0x15}, {"6", '6', 0x16}, {"5", '5', 0x17}, {"equals", '=', 0x18},
	{"9", '9', 0x19}, {"7", '7', 0x1a}, {"minus", '-', 0x1b}, {"8", '8', 0x1c},
	{"0", '0', 0x1d}, {"rbracket", ']', 0x1e}, {"o", 'o', 0x1f},
	{"u", 'u', 0x20}, {"lbracket", '[', 0x21}, {"i", 'i', 0x22},
	{"p", 'p', 0x23}, {"return", 13, 0x24}, {"l", 'l', 0x25}, {"j", 'j', 0x26},
	{"apostrophe", '\'', 0x27}, {"k", 'k', 0x28}, {"semicolon", ';', 0x29},
	{"backslash", '\\', 0x2a}, {"comma", ',', 0x2b}, {"slash", '/', 0x2c},
	{"n", 'n', 0x2d}, {"m", 'm', 0x2e}, {"period", '.', 0x2f}, {"tab", 9, 0x30},
	{"space", ' ', 0x31}, {"grave", '`', 0x32}, {"backspace", 8, 0x33},
	{"esc", 27, 0x35}, {"less", '<', 0x0a /* kVK_ISO_Section */},
	{"f1", 282, 0x7a}, {"f2", 283, 0x78}, {"f3", 284, 0x63}, {"f4", 285, 0x76},
	{"f5", 286, 0x60}, {"f6", 287, 0x61}, {"f7", 288, 0x62}, {"f8", 289, 0x64},
	{"f9", 290, 0x65}, {"f10", 291, 0x6d}, {"f11", 292, 0x67}, {"f12", 293, 0x6f},
	{"f13", 294, 0x69}, {"f14", 295, 0x6b}, {"f15", 296, 0x71},
	{"up", 273, 0x7e}, {"down", 274, 0x7d}, {"right", 275, 0x7c},
	{"left", 276, 0x7b}, {"insert", 277, 0x72 /* Help key position */},
	{"home", 278, 0x73}, {"end", 279, 0x77}, {"pageup", 280, 0x74},
	{"pagedown", 281, 0x79}, {"delete", 127, 0x75},
	{"kp0", 256, 0x52}, {"kp1", 257, 0x53}, {"kp2", 258, 0x54},
	{"kp3", 259, 0x55}, {"kp4", 260, 0x56}, {"kp5", 261, 0x57},
	{"kp6", 262, 0x58}, {"kp7", 263, 0x59}, {"kp8", 264, 0x5b},
	{"kp9", 265, 0x5c}, {"kp_period", 266, 0x41}, {"kp_divide", 267, 0x4b},
	{"kp_multiply", 268, 0x43}, {"kp_minus", 269, 0x4e}, {"kp_plus", 270, 0x45},
	{"kp_enter", 271, 0x4c}, {"kp_equals", 272, 0x51}, {"clear", 12, 0x47},
	{"help", 315, 0x72}, {"undo", 322, -1},
	{"numlock", 300, -1}, {"capslock", 301, 0x39},
	{"rshift", 303, 0x3c}, {"lshift", 304, 0x38}, {"rctrl", 305, 0x3e},
	{"lctrl", 306, 0x3b}, {"ralt", 307, 0x3d}, {"lalt", 308, 0x3a},
	{"rmeta", 309, 0x36}, {"lmeta", 310, 0x37}, {"mode", 313, -1},
};

/* modifier words accepted after `sdlkey NAME` */
typedef struct { const char *word; int mod; const char *keyname; } mod_word;
static const mod_word MOD_WORDS[] = {
	{"shift", KM_LSHIFT, "lshift"}, {"rshift", KM_RSHIFT, "rshift"},
	{"ctrl", KM_LCTRL, "lctrl"}, {"rctrl", KM_RCTRL, "rctrl"},
	{"alt", KM_LALT, "lalt"}, {"option", KM_LALT, "lalt"},
	{"ralt", KM_RALT, "ralt"}, {"cmd", KM_LMETA, "lmeta"},
	{"lcmd", KM_LMETA, "lmeta"}, {"rcmd", KM_RMETA, "rmeta"},
	{"mode", KM_MODE, "mode"}, {"num", KM_NUM, NULL}, {"caps", KM_CAPS, NULL},
};

/* ---- ASCII -> ST scancode (US layout) for `text` ------------------------ */
static int ascii_to_st(int ch, int *shift)
{
	static const char plain[] = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./ ";
	static const unsigned char plain_code[] = {
		0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,
		0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,
		0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,
		0x2b,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x34,0x35,0x39 };
	static const char shifted[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
	const char *p;
	*shift = 0;
	if (ch == '\n') return 0x1c;
	if (ch == '\t') return 0x0f;
	if (ch && (p = strchr(plain, ch)) != NULL) return plain_code[p - plain];
	if (ch && (p = strchr(shifted, ch)) != NULL) { *shift = 1; return plain_code[p - shifted]; }
	return -1;
}

/* ---- steps -------------------------------------------------------------- */
enum op { OP_WAIT, OP_KEY, OP_KEYDOWN, OP_KEYUP, OP_TEXT, OP_MOUSE, OP_CLICK,
          OP_SHOT, OP_QUIT, OP_SDLKEY, OP_LOG, OP_PRESS, OP_RELEASE, OP_MOVE };
typedef struct {
	enum op op;
	int line;
	int a, b, c, d;       /* op-specific integers */
	char s[128];          /* text, shot name, log text */
	char src[160];        /* source line, for the log */
} step;

static step *g_steps;
static int g_nsteps;

/* ---- game-thread mailbox ------------------------------------------------ */
enum act { ACT_STKEY, ACT_MOUSE, ACT_BUTTON, ACT_EVENT, ACT_MOVE };
typedef struct { enum act kind; int a, b; drv_event ev; } action;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static action g_act;
static atomic_int g_pending;       /* 1 = g_act waits for the game thread */
static atomic_int g_filtering;     /* drop real input while a script runs */
static atomic_long g_polls, g_dropped;

/* IKBD observation of a fabricated key event: the hook notes the ring tail
 * when it hands the event to Main_EventHandler, and on the next poll (the
 * same while loop, after Keymap_KeyDown/KeyUp ran) reads what was appended. */
static int g_observing, g_obs_tail0, g_obs_n, g_obs_sc0, g_obs_sc1;
static unsigned char g_obs_bytes[IN_KEYBUF_SIZE];

/* Called by the driver thread: hand one action to the game thread and wait
 * until the SDL_PollEvent hook has applied it. */
static void post(const action *a)
{
	struct timespec dl;
	pthread_mutex_lock(&g_mx);
	g_act = *a;
	atomic_store(&g_pending, 1);
	while (atomic_load(&g_pending)) {
		clock_gettime(CLOCK_REALTIME, &dl);
		dl.tv_sec += 10;
		if (pthread_cond_timedwait(&g_cv, &g_mx, &dl) == ETIMEDOUT &&
		    atomic_load(&g_pending))
			die(5, "the game thread has not polled SDL events for 10 s");
	}
	pthread_mutex_unlock(&g_mx);
}

static void set_int(size_t off, int v) { memcpy(g_input + off, &v, sizeof v); }
static int get_int(size_t off) { int v; memcpy(&v, g_input + off, sizeof v); return v; }

/* The hook.  Runs on the game thread (Main_EventHandler's poll loop). */
static int drive_poll(void *event)
{
	atomic_fetch_add(&g_polls, 1);
	if (atomic_load(&g_pending)) {
		int ret = -1;
		pthread_mutex_lock(&g_mx);
		if (g_observing) {
			/* the game has handled the key event we returned last time */
			int t = get_int(IN_BUF_TAIL), i = g_obs_tail0;
			g_obs_n = 0;
			while (i != t && g_obs_n < IN_KEYBUF_SIZE) {
				g_obs_bytes[g_obs_n++] = g_input[IN_KEY_BUF + i];
				i = (i + 1) % IN_KEYBUF_SIZE;
			}
			g_obs_sc1 = g_shortcut_key ? *g_shortcut_key : -1;
			g_observing = 0;
			atomic_store(&g_pending, 0);
			pthread_cond_broadcast(&g_cv);
		} else if (atomic_load(&g_pending)) {
			switch (g_act.kind) {
			case ACT_STKEY:
				g_press((unsigned char)g_act.a, g_act.b);
				break;
			case ACT_MOUSE:
				set_int(IN_ABS_X, g_act.a);
				set_int(IN_ABS_Y, g_act.b);
				break;
			case ACT_BUTTON: {
				int st = get_int(IN_CUR_MOUSEBUT);
				st = g_act.b ? (st | g_act.a) : (st & ~g_act.a);
				set_int(IN_CUR_MOUSEBUT, st);
				break;
			}
			case ACT_MOVE:
				/* what main.c does with an SDL_MOUSEMOTION's xrel/yrel */
				set_int(IN_MOTION_X, get_int(IN_MOTION_X) + g_act.a);
				set_int(IN_MOTION_Y, get_int(IN_MOTION_Y) + g_act.b);
				break;
			case ACT_EVENT:
				if (!event) {      /* SDL_PollEvent(NULL): only "is one pending?" */
					pthread_mutex_unlock(&g_mx);
					return 1;
				}
				memcpy(event, &g_act.ev, sizeof g_act.ev);
				ret = 1;
				if (g_act.ev.type == EV_KEYDOWN || g_act.ev.type == EV_KEYUP) {
					/* complete on the next poll, once the game handled it */
					g_obs_tail0 = get_int(IN_BUF_TAIL);
					g_obs_sc0 = g_shortcut_key ? *g_shortcut_key : -1;
					g_observing = 1;
					pthread_mutex_unlock(&g_mx);
					return 1;
				}
				break;
			}
			atomic_store(&g_pending, 0);
			pthread_cond_broadcast(&g_cv);
		}
		pthread_mutex_unlock(&g_mx);
		if (ret == 1) return 1;
	}
	for (;;) {
		int r = SDL_PollEvent(event);
		if (r <= 0 || !event || !atomic_load(&g_filtering)) return r;
		uint8_t t = ((drv_event *)event)->type;
		if (t >= EV_KEYDOWN && t <= EV_MOUSEBUTTONUP) {
			atomic_fetch_add(&g_dropped, 1);
			continue;              /* real desktop input: drop it */
		}
		return r;
	}
}

__attribute__((used)) static const struct { const void *rep, *orig; } g_interpose
	__attribute__((section("__DATA,__interpose"))) =
	{ (const void *)drive_poll, (const void *)SDL_PollEvent };

/* ---- parsing ------------------------------------------------------------ */
static int parse_int(const char *s, int *out)
{
	char *end;
	long v;
	if (!s || !*s) return 0;
	errno = 0;
	v = strtol(s, &end, 0);
	if (errno || *end) return 0;
	*out = (int)v;
	return 1;
}

static int st_code(const char *s)
{
	size_t i;
	int v;
	if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
		if (parse_int(s, &v) && v > 0 && v < 0x80) return v;
		return -1;
	}
	for (i = 0; i < sizeof ST_NAMES / sizeof ST_NAMES[0]; i++)
		if (!strcmp(s, ST_NAMES[i].name)) return ST_NAMES[i].code;
	return -1;
}

static const sdl_name *sdl_lookup(const char *s)
{
	size_t i;
	for (i = 0; i < sizeof SDL_NAMES / sizeof SDL_NAMES[0]; i++)
		if (!strcmp(s, SDL_NAMES[i].name)) return &SDL_NAMES[i];
	return NULL;
}

static void syntax(int line, const char *src, const char *why)
	__attribute__((noreturn));
static void syntax(int line, const char *src, const char *why)
{
	die(3, "script line %d: %s: \"%s\"", line, why, src);
}

static void parse_script(const char *path)
{
	FILE *f = fopen(path, "r");
	char raw[1024];
	int line = 0, cap = 0;
	if (!f) die(3, "cannot open FE2_DRIVE_SCRIPT %s: %s", path, strerror(errno));
	while (fgets(raw, sizeof raw, f)) {
		char buf[1024], *tok[8], *p, *q;
		int ntok = 0, inq = 0;
		step st;
		line++;
		/* strip a comment that is not inside quotes, and the newline */
		for (p = raw; *p; p++) {
			if (*p == '"') inq = !inq;
			if ((*p == '#' && !inq) || *p == '\n' || *p == '\r') { *p = 0; break; }
		}
		for (p = raw; isspace((unsigned char)*p); p++) ;
		for (q = p + strlen(p); q > p && isspace((unsigned char)q[-1]); q--) ;
		*q = 0;
		if (!*p) continue;
		memset(&st, 0, sizeof st);
		st.line = line;
		snprintf(st.src, sizeof st.src, "%s", p);
		snprintf(buf, sizeof buf, "%s", p);
		/* tokenize: a "quoted string" is one token (quotes removed) */
		p = buf;
		while (*p && ntok < 8) {
			while (isspace((unsigned char)*p)) p++;
			if (!*p) break;
			if (*p == '"') {
				char *w = ++p, *o = p;
				while (*p && *p != '"') {
					if (*p == '\\' && p[1]) {
						p++;
						*o++ = (*p == 'n') ? '\n' : (*p == 't') ? '\t' : *p;
						p++;
					} else *o++ = *p++;
				}
				if (*p != '"') syntax(line, st.src, "unterminated string");
				p++;
				*o = 0;
				tok[ntok++] = w;
			} else {
				tok[ntok++] = p;
				while (*p && !isspace((unsigned char)*p)) p++;
				if (*p) *p++ = 0;
			}
		}
		if (*p) syntax(line, st.src, "too many words");
		if (!strcmp(tok[0], "wait")) {
			st.op = OP_WAIT;
			if (ntok != 2 || !parse_int(tok[1], &st.a) || st.a < 0)
				syntax(line, st.src, "usage: wait <ms>");
		} else if (!strcmp(tok[0], "key") || !strcmp(tok[0], "keydown") ||
		           !strcmp(tok[0], "keyup")) {
			st.op = tok[0][3] == 0 ? OP_KEY : tok[0][3] == 'd' ? OP_KEYDOWN : OP_KEYUP;
			if (ntok < 2 || (st.a = st_code(tok[1])) < 0)
				syntax(line, st.src, "unknown ST key (name, or hex as 0xNN)");
			st.b = 100;
			if (st.op == OP_KEY && ntok == 3) {
				if (!parse_int(tok[2], &st.b) || st.b < 0)
					syntax(line, st.src, "bad hold_ms");
			} else if (ntok != 2) syntax(line, st.src, "usage: key <code> [hold_ms]");
		} else if (!strcmp(tok[0], "text")) {
			st.op = OP_TEXT;
			if (ntok != 2) syntax(line, st.src, "usage: text \"<ascii>\"");
			snprintf(st.s, sizeof st.s, "%s", tok[1]);
			for (q = st.s; *q; q++) {
				int sh;
				if (ascii_to_st((unsigned char)*q, &sh) < 0)
					syntax(line, st.src, "text has a character with no ST key");
			}
		} else if (!strcmp(tok[0], "mouse")) {
			st.op = OP_MOUSE;
			if (ntok != 3 || !parse_int(tok[1], &st.a) || !parse_int(tok[2], &st.b) ||
			    st.a < 0 || st.a > 319 || st.b < 0 || st.b > 199)
				syntax(line, st.src, "usage: mouse <x 0-319> <y 0-199>");
		} else if (!strcmp(tok[0], "click")) {
			int i = 1;
			st.op = OP_CLICK;
			st.a = 0x2;                 /* left (input.c: left=0x2, right=0x1) */
			st.b = 100;
			if (i < ntok && !strcmp(tok[i], "left")) i++;
			else if (i < ntok && !strcmp(tok[i], "right")) { st.a = 0x1; i++; }
			if (i < ntok && (!parse_int(tok[i++], &st.b) || st.b < 0))
				syntax(line, st.src, "usage: click [left|right] [hold_ms]");
			if (i != ntok) syntax(line, st.src, "usage: click [left|right] [hold_ms]");
		} else if (!strcmp(tok[0], "press") || !strcmp(tok[0], "release")) {
			st.op = tok[0][0] == 'p' ? OP_PRESS : OP_RELEASE;
			st.a = 0x2;                 /* left (input.c: left=0x2, right=0x1) */
			if (ntok == 2 && !strcmp(tok[1], "right")) st.a = 0x1;
			else if (!(ntok == 1 || (ntok == 2 && !strcmp(tok[1], "left"))))
				syntax(line, st.src, "usage: press|release [left|right]");
		} else if (!strcmp(tok[0], "move")) {
			/* Call_GetMouseInput drops a motion over 100 in either axis */
			st.op = OP_MOVE;
			st.c = 1;
			st.d = 20;
			if (ntok < 3 || ntok > 5 || !parse_int(tok[1], &st.a) || !parse_int(tok[2], &st.b) ||
			    st.a < -100 || st.a > 100 || st.b < -100 || st.b > 100 ||
			    (ntok >= 4 && (!parse_int(tok[3], &st.c) || st.c < 1)) ||
			    (ntok == 5 && (!parse_int(tok[4], &st.d) || st.d < 0)))
				syntax(line, st.src, "usage: move <dx -100..100> <dy -100..100> [count] [every_ms]");
		} else if (!strcmp(tok[0], "shot")) {
			st.op = OP_SHOT;
			if (ntok != 2) syntax(line, st.src, "usage: shot <name>");
			for (q = tok[1]; *q; q++)
				if (!isalnum((unsigned char)*q) && !strchr("._-", *q))
					syntax(line, st.src, "shot names use [A-Za-z0-9._-]");
			snprintf(st.s, sizeof st.s, "%s", tok[1]);
		} else if (!strcmp(tok[0], "quit")) {
			st.op = OP_QUIT;
			if (ntok != 1) syntax(line, st.src, "usage: quit");
		} else if (!strcmp(tok[0], "sdlkey")) {
			const sdl_name *k;
			int i;
			st.op = OP_SDLKEY;
			if (ntok < 2 || !(k = sdl_lookup(tok[1])))
				syntax(line, st.src, "unknown SDL key name");
			st.a = (int)(k - SDL_NAMES);
			st.c = 100;
			for (i = 2; i < ntok; i++) {
				size_t m;
				int found = 0;
				for (m = 0; m < sizeof MOD_WORDS / sizeof MOD_WORDS[0]; m++)
					if (!strcmp(tok[i], MOD_WORDS[m].word)) {
						st.b |= MOD_WORDS[m].mod;
						found = 1;
					}
				if (!found && !(i == ntok - 1 && parse_int(tok[i], &st.c) && st.c >= 0))
					syntax(line, st.src, "usage: sdlkey <name> [modifier...] [hold_ms]");
			}
		} else if (!strcmp(tok[0], "log")) {
			st.op = OP_LOG;
			snprintf(st.s, sizeof st.s, "%s", st.src + 3 + strspn(st.src + 3, " \t"));
		} else {
			syntax(line, st.src, "unknown step");
		}
		if (g_nsteps == cap) {
			cap = cap ? cap * 2 : 64;
			g_steps = realloc(g_steps, (size_t)cap * sizeof *g_steps);
			if (!g_steps) die(3, "out of memory");
		}
		g_steps[g_nsteps++] = st;
	}
	fclose(f);
	if (!g_nsteps) die(3, "script %s has no steps", path);
}

/* ---- step execution (driver thread) ------------------------------------- */
static void sleep_ms(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) == -1 && errno == EINTR) ;
}

static void st_key(int code, int press)
{
	action a = { ACT_STKEY, code, press, {0} };
	post(&a);
}

/* Fabricate one SDL 1.2 key event, wait until Main_EventHandler has handled
 * it, and log what reached the ST: the bytes appended to the IKBD ring, and
 * the host shortcut key if one was latched instead. */
static void sdl_key_event(const sdl_name *k, int mod, int down)
{
	action a;
	char bytes[IN_KEYBUF_SIZE * 3 + 8] = "none";
	int i, n;
	memset(&a, 0, sizeof a);
	a.kind = ACT_EVENT;
	a.ev.key.type = down ? EV_KEYDOWN : EV_KEYUP;
	a.ev.key.state = down ? 1 : 0;
	a.ev.key.keysym.scancode = k->kvk < 0 ? 0xff : (uint8_t)k->kvk;
	a.ev.key.keysym.sym = k->sym;
	a.ev.key.keysym.mod = mod;
	post(&a);
	for (i = 0, n = 0; i < g_obs_n; i++)
		n += snprintf(bytes + n, sizeof bytes - (size_t)n, "%s%02x", i ? " " : "", g_obs_bytes[i]);
	if (g_obs_sc1 > 0 && g_obs_sc1 != g_obs_sc0)
		dlog("ikbd: %s %s sym=%d kvk=0x%02x mod=0x%04x -> ST %s; shortcut key %d latched",
		     k->name, down ? "down" : "up", k->sym, a.ev.key.keysym.scancode, mod, bytes, g_obs_sc1);
	else
		dlog("ikbd: %s %s sym=%d kvk=0x%02x mod=0x%04x -> ST %s",
		     k->name, down ? "down" : "up", k->sym, a.ev.key.keysym.scancode, mod, bytes);
}

static int g_req_fd = -1, g_ack_fd = -1, g_shot_seq;

static void do_shot(const char *name)
{
	char msg[256], ack[512];
	size_t n = 0;
	int seq = ++g_shot_seq;
	if (g_req_fd < 0 || g_ack_fd < 0) {
		dlog("shot %s SKIPPED: no screenshot service (run under drive.py)", name);
		return;
	}
	snprintf(msg, sizeof msg, "shot %d %s\n", seq, name);
	if (write(g_req_fd, msg, strlen(msg)) != (ssize_t)strlen(msg))
		die(6, "shot request write failed: %s", strerror(errno));
	for (;;) {
		struct pollfd pf = { g_ack_fd, POLLIN, 0 };
		char c;
		int r = poll(&pf, 1, 60000);
		if (r == 0) die(6, "no shot ack within 60 s");
		if (r < 0) { if (errno == EINTR) continue; die(6, "poll: %s", strerror(errno)); }
		r = (int)read(g_ack_fd, &c, 1);
		if (r <= 0) die(6, "screenshot service closed the ack pipe");
		if (c == '\n') break;
		if (n < sizeof ack - 1) ack[n++] = c;
	}
	ack[n] = 0;
	dlog("shot %s -> %s", name, ack);
}

static void *driver_main(void *arg)
{
	int i;
	(void)arg;
	sleep_ms(0);
	atomic_store(&g_filtering, 1);
	dlog("script start: %d steps; real keyboard/mouse input is dropped while it runs", g_nsteps);
	for (i = 0; i < g_nsteps; i++) {
		step *s = &g_steps[i];
		dlog("step %d/%d (line %d): %s", i + 1, g_nsteps, s->line, s->src);
		switch (s->op) {
		case OP_WAIT:
			sleep_ms(s->a);
			break;
		case OP_KEY:
			st_key(s->a, 1);
			sleep_ms(s->b);
			st_key(s->a, 0);
			break;
		case OP_KEYDOWN:
			st_key(s->a, 1);
			break;
		case OP_KEYUP:
			st_key(s->a, 0);
			break;
		case OP_TEXT: {
			const char *q;
			for (q = s->s; *q; q++) {
				int sh, code = ascii_to_st((unsigned char)*q, &sh);
				if (sh) { st_key(0x2a, 1); sleep_ms(40); }
				st_key(code, 1);
				sleep_ms(60);
				st_key(code, 0);
				if (sh) { sleep_ms(40); st_key(0x2a, 0); }
				sleep_ms(60);
			}
			break;
		}
		case OP_MOUSE: {
			int x0 = 0, y0 = 0, w = *g_screen_w, h = *g_screen_h;
			/* Call_GetMouseInput reports 320*(abs_x-view_x)/view_w
			 * (320*abs_x/screen_w with no view): invert it, rounding up
			 * so that integer division gives back x. */
			if (g_view[3] && *g_view[2] > 0 && *g_view[3] > 0) {
				x0 = *g_view[0]; y0 = *g_view[1]; w = *g_view[2]; h = *g_view[3];
			}
			action a = { ACT_MOUSE, x0 + (s->a * w + 319) / 320, y0 + (s->b * h + 199) / 200, {0} };
			post(&a);
			dlog("mouse ST (%d,%d) = host (%d,%d) for screen %dx%d", s->a, s->b, a.a, a.b, w, h);
			break;
		}
		case OP_CLICK: {
			action a = { ACT_BUTTON, s->a, 1, {0} };
			post(&a);
			sleep_ms(s->b);
			a.b = 0;
			post(&a);
			break;
		}
		case OP_PRESS:
		case OP_RELEASE: {
			action a = { ACT_BUTTON, s->a, s->op == OP_PRESS, {0} };
			post(&a);
			break;
		}
		case OP_MOVE: {
			int i;
			for (i = 0; i < s->c; i++) {
				action a = { ACT_MOVE, s->a, s->b, {0} };
				post(&a);
				if (i + 1 < s->c) sleep_ms(s->d);
			}
			break;
		}
		case OP_SHOT:
			do_shot(s->s);
			break;
		case OP_QUIT: {
			action a;
			memset(&a, 0, sizeof a);
			a.kind = ACT_EVENT;
			a.ev.type = EV_QUIT;
			dlog("quit: posting SDL_QUIT; Main_EventHandler calls SDL_Quit() then exit(0)");
			post(&a);
			sleep_ms(10000);
			die(7, "the game did not exit within 10 s of SDL_QUIT");
		}
		case OP_SDLKEY: {
			const sdl_name *k = &SDL_NAMES[s->a];
			size_t m;
			int mod = s->b, self = 0;
			/* SDL 1.2 sets a modifier key's own bit in keysym.mod on its
			 * key-down event and clears it on key-up (SDL_PrivateKeyboard) */
			for (m = 0; m < sizeof MOD_WORDS / sizeof MOD_WORDS[0]; m++)
				if (MOD_WORDS[m].keyname && !strcmp(MOD_WORDS[m].keyname, k->name))
					self = MOD_WORDS[m].mod;
			/* press the modifier keys first, as a real keyboard would */
			for (m = 0; m < sizeof MOD_WORDS / sizeof MOD_WORDS[0]; m++)
				if ((mod & MOD_WORDS[m].mod) && MOD_WORDS[m].keyname &&
				    (m == 0 || MOD_WORDS[m].mod != MOD_WORDS[m - 1].mod)) {
					sdl_key_event(sdl_lookup(MOD_WORDS[m].keyname), mod, 1);
					sleep_ms(30);
				}
			sdl_key_event(k, mod | self, 1);
			sleep_ms(s->c);
			sdl_key_event(k, mod, 0);
			for (m = 0; m < sizeof MOD_WORDS / sizeof MOD_WORDS[0]; m++)
				if ((mod & MOD_WORDS[m].mod) && MOD_WORDS[m].keyname &&
				    (m == 0 || MOD_WORDS[m].mod != MOD_WORDS[m - 1].mod)) {
					sleep_ms(30);
					sdl_key_event(sdl_lookup(MOD_WORDS[m].keyname), 0, 0);
				}
			break;
		}
		case OP_LOG:
			dlog("note: %s", s->s);
			break;
		}
	}
	atomic_store(&g_filtering, 0);
	dlog("script end: %ld polls, %ld real input events dropped; game left running",
	     atomic_load(&g_polls), atomic_load(&g_dropped));
	return NULL;
}

static void *need(const char *sym)
{
	void *p = dlsym(RTLD_DEFAULT, sym);
	if (!p) die(4, "symbol %s not found in the game binary (dlsym: %s)", sym, dlerror());
	return p;
}

__attribute__((constructor)) static void drive_init(void)
{
	const char *script = getenv("FE2_DRIVE_SCRIPT");
	const char *rq = getenv("FE2_DRIVE_REQ_FD"), *ak = getenv("FE2_DRIVE_ACK_FD");
	pthread_t th;
	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	if (!script || !*script) {
		dlog("loaded; FE2_DRIVE_SCRIPT not set, passing SDL events through");
		return;
	}
	g_press = (press_st_key_fn)need("Input_PressSTKey");
	g_input = need("input");
	g_screen_w = need("screen_w");
	g_screen_h = need("screen_h");
	{
		/* optional: games before the 4:3 picture rect have none */
		static const char *vn[4] = { "view_x", "view_y", "view_w", "view_h" };
		int k;
		for (k = 0; k < 4; k++) g_view[k] = dlsym(RTLD_DEFAULT, vn[k]);
		if (!g_view[0] || !g_view[1] || !g_view[2]) g_view[3] = NULL;
	}
	g_shortcut_key = dlsym(RTLD_DEFAULT, "ShortCutKey");   /* optional: log only */
	parse_script(script);
	if (rq && ak) {
		parse_int(rq, &g_req_fd);
		parse_int(ak, &g_ack_fd);
	}
	dlog("loaded; script %s (%d steps); Input_PressSTKey=%p input=%p; shot service %s",
	     script, g_nsteps, (void *)g_press, (void *)g_input,
	     g_req_fd >= 0 ? "on" : "off");
	if (pthread_create(&th, NULL, driver_main, NULL))
		die(4, "pthread_create failed");
	pthread_detach(th);
}
