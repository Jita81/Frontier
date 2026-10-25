/*
 * Phase 2 experiment harness (see docs/experiments/).
 *
 * Everything in here is default OFF and only reads the environment:
 *
 *   FE2_TRACE_HCALLS=1    log every host call (index, name, registers, decoded
 *                         text) to FE2_TRACE_FILE (default stderr), one block
 *                         per frame, plus a per-frame histogram line. Idle
 *                         (0x1f, the vbl busy-wait) is only counted unless
 *                         the value is 2
 *   FE2_TRACE_ONLY=12,1d  only log these host calls (hex, comma separated);
 *                         the per-frame 'H' histogram line is dropped too
 *   FE2_SCRIPT=<file>     scripted input and screenshots, see below
 *   FE2_SHOT_DIR=<dir>    where 'shot' writes its PPM files (default ".")
 *   FE2_PROVENANCE=1      measurement only: track who wrote each pixel of the
 *                         screen buffers; 'shot' then also writes
 *                         <name>.prov.ppm and <name>.prov.txt
 *
 * With none of them set, Exp_Init returns without touching hcalls[] and the
 * other entry points return on their first test, so the game runs exactly
 * as without this file.
 *
 * Script lines are "<when> <command> [args]"; <when> is an absolute frame
 * number or +N frames after the previous command ran. Commands:
 *   key <hex>            press and release an ST scancode
 *   keydown/keyup <hex>
 *   mouse <x> <y>        absolute pointer position in 320x200 ST coordinates
 *   click l|r            press a button, released 3 frames later
 *   shot <name>          write <name>.ppm (frame as shown) and <name>.st.ppm
 *                        (the 320x200 8-bit UI buffer fe2.s drew, index 255
 *                        shown as magenta in GL builds)
 *   wait <text>          hold the script until a drawn string contains text
 *   trace on|off         switch the hcall log (needs FE2_TRACE_HCALLS)
 *   mark <text>          write a marker line into the trace
 *   quit
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "../m68000.h"
#include "screen.h"
#include "input.h"
#include "exp.h"

#define EXP_MAX_HCALLS	128

extern const int hcalls_count;

int exp_any;

static HOSTCALL orig_hcalls[EXP_MAX_HCALLS];
static FILE *trace_fp;
static int trace_on;
static int trace_level;	/* 1: all calls but Idle (counted only), 2: Idle too */
static unsigned char trace_only[EXP_MAX_HCALLS];	/* FE2_TRACE_ONLY=12,1d,1e */
static int trace_only_on;
static unsigned long frame_no;
static unsigned long calls_this_frame[EXP_MAX_HCALLS];

static const char *hcall_names[EXP_MAX_HCALLS] = {
	"SetExceptionHandler", "Memset", "MemsetBlue", "BlitCursor",
	"RestoreUnderCursor", "BlitBmp", "OldHLine", "HostUpdate",
	"Memcpy", "PutPix", "BackHLine", "FillLine",
	"SetMainPalette", "SetCtrlPalette", "SetScreenBase", "-",
	"DumpRegs", "MakeExtPalette", "PlaySFX", "GetMouseInput",
	"GetKeyboardEvent", "-", "HLine", "-",
	"NotifyMousePos", "InformScreens", "-", "DrawStrShadowed",
	"DrawStr", "PlayMusic", "StopMusic", "Idle",
	"DumpDebug", "IsMusicPlaying", "Fread", "Fwrite",
	"Fdelete", "Fopendir", "Freaddir", "Fclosedir",
	[0x60] = "Nu_PutTriangle", "Nu_PutQuad", "Nu_PutLine", "Nu_PutPoint",
	"Nu_PutTwinklyCircle", "Nu_PutColoredPoint", "Nu_PutBezierLine",
	"Nu_ComplexStart", "Nu_ComplexSNext", "Nu_ComplexSBegin",
	"Nu_ComplexEnd", "Nu_3DViewInit", "Nu_InsertZNode",
	"Nu_ComplexStartInner", "Nu_ComplexBezier", "Nu_DrawScreen",
	"Nu_PutTeardrop", "Nu_PutCircle", "Nu_PutOval", "Nu_IsGLRenderer",
	"Nu_GLClearArea", "Nu_QueueDrawStr", "Nu_PutCylinder", "Nu_PutBlob",
	"Nu_PutPlanet", "Nu_Put2DLine",
};

#define HC_DRAWSTRSHADOWED	0x1b
#define HC_DRAWSTR		0x1c
#define HC_QUEUEDRAWSTR		0x75

/* ---------------------------------------------------------------- text */

/* Text seen this frame and last, for 'wait'. */
#define SEEN_MAX	8192
static char seen_text[2][SEEN_MAX];
static int seen_len[2], seen_cur;

/* Decode an fe2 string (control codes below 0x20) into readable text. */
static void decode_str (const unsigned char *s, char *out, int outlen)
{
	int n = 0, c;
	char tmp[24];

	while ((c = *s++) && n < outlen - 16) {
		tmp[0] = 0;
		if (c == '\r') strcpy (tmp, "{nl}");
		else if (c == 1) { sprintf (tmp, "{c%d}", *s); if (*s) s++; }
		else if (c == 0x1e) { sprintf (tmp, "{x%d}", *s * 2); if (*s) s++; }
		else if (c == 0x1f) {
			int x = *s, y = s[0] ? s[1] : 0;
			sprintf (tmp, "{@%d,%d}", x * 2, y);
			if (*s) s++;
			if (*s) s++;
		}
		else if (c < 0x20) sprintf (tmp, "{%02x}", c);
		else if (c == '"' || c == '\\') sprintf (tmp, "\\%c", c);
		else if (c > 0x7e) sprintf (tmp, "{%02x}", c);
		else { tmp[0] = c; tmp[1] = 0; }
		strcpy (out + n, tmp);
		n += strlen (tmp);
	}
	out[n] = 0;
}

static void note_text (const char *text)
{
	int l = strlen (text);
	char *buf = seen_text[seen_cur];
	if (seen_len[seen_cur] + l + 2 >= SEEN_MAX) return;
	memcpy (buf + seen_len[seen_cur], text, l);
	seen_len[seen_cur] += l;
	buf[seen_len[seen_cur]++] = '\n';
	buf[seen_len[seen_cur]] = 0;
}

static int text_seen (const char *needle)
{
	return strstr (seen_text[0], needle) || strstr (seen_text[1], needle);
}

static void write_ppm (const char *name, const unsigned char *rgb, int w, int h);
static char shot_dir[512] = ".";

/* ---------------------------------------------------------- provenance */

/* FE2_PROVENANCE=1 (measurement only): remember, for every byte of the
 * screen buffers, who wrote it last: a host call (tag 2+index) or fe2.s
 * itself between host calls (TAG_FE2S). The buffers are diffed against a
 * mirror around every host call that may write them. */
#define TAG_NONE	0
#define TAG_FE2S	1
#define SCRBYTES	(320 * 200)
#define MAX_TRACKED	4

static int prov_on;
static unsigned long fe2s_direct_total;
static int prov_last_call;	/* index of the host call about to run */	/* bytes ever tagged TAG_FE2S, any buffer */
static struct tracked {
	u32 base;
	unsigned char mirror[SCRBYTES];
	unsigned char tag[SCRBYTES];
} tracked[MAX_TRACKED];
static int ntracked;

static int may_write_screen (int n)
{
	switch (n) {
	case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6:
	case 0x8: case 0x9: case 0xa: case 0xb: case 0x16:
	case 0x1b: case 0x1c: case 0x22: case 0x6f: case 0x74:
		return 1;
	}
	return 0;
}

static struct tracked *find_tracked (u32 adr)
{
	int i;
	for (i = 0; i < ntracked; i++)
		if (adr >= tracked[i].base && adr < tracked[i].base + SCRBYTES)
			return &tracked[i];
	return NULL;
}

static void track (u32 base)
{
	struct tracked *t;
	/* screen pointers read before fe2.s sets them hold junk (e.g. 0x46);
	 * real ST screen bases are 256-byte aligned and above low memory */
	if (base < 0x10000 || (base & 0xff) || ntracked >= MAX_TRACKED) return;
	for (t = tracked; t < tracked + ntracked; t++)
		if (t->base == base) return;
	t = &tracked[ntracked++];
	t->base = base;
	memcpy (t->mirror, STRam + base, SCRBYTES);
	memset (t->tag, TAG_NONE, SCRBYTES);
}

static void track_screens (void)
{
	if (VideoBase) track (VideoBase);
	if (physcreen) track (STMemory_ReadLong (physcreen));
	if (physcreen2) track (STMemory_ReadLong (physcreen2));
	if (logscreen) track (STMemory_ReadLong (logscreen));
	if (logscreen2) track (STMemory_ReadLong (logscreen2));
}

static void prov_sync (int tag)
{
	int i, j;
	for (i = 0; i < ntracked; i++) {
		struct tracked *t = &tracked[i];
		unsigned char *mem = (unsigned char *) STRam + t->base;
		if (!memcmp (mem, t->mirror, SCRBYTES)) continue;
		int n = 0, lo = SCRBYTES, hi = -1;
		for (j = 0; j < SCRBYTES; j++) {
			if (mem[j] != t->mirror[j]) {
				t->mirror[j] = mem[j];
				t->tag[j] = tag;
				if (tag == TAG_FE2S) {
					n++;
					if (j < lo) lo = j;
					hi = j;
				}
			}
		}
		if (n) {
			fe2s_direct_total += n;
			fprintf (stderr, "FE2_PROVENANCE: F%lu buffer 0x%x: %d bytes by fe2.s, rows %d-%d (pc 0x%x)\n",
				 frame_no, (unsigned) t->base, n, lo / 320, hi / 320, (unsigned) prov_last_call);
		}
	}
}

/* Memcpy between tracked buffers keeps the source's provenance. */
static void prov_memcpy (u32 dest, u32 src, u32 count)
{
	u32 i;
	for (i = 0; i < count; i++) {
		struct tracked *d = find_tracked (dest + i), *s = find_tracked (src + i);
		if (!d) continue;
		d->tag[dest + i - d->base] = s ? s->tag[src + i - s->base] : 2 + 0x8;
		d->mirror[dest + i - d->base] = STRam[dest + i];
	}
}

static void prov_before (int n)
{
	prov_last_call = n;
	track_screens ();
	prov_sync (TAG_FE2S);
}

static void prov_after (int n, u32 sp)
{
	if (n == 0x8) {
		prov_memcpy (STMemory_ReadLong (sp), STMemory_ReadLong (sp + 4),
			     STMemory_ReadLong (sp + 8));
	}
	prov_sync (2 + n);
}

static const char *tag_class (int tag)
{
	switch (tag) {
	case TAG_NONE: return "initial";
	case TAG_FE2S: return "fe2s-direct";
	case 2+0x1: case 2+0x2: case 2+0x74: return "host-clear";
	case 2+0x5: return "host-BlitBmp";
	case 2+0x3: case 2+0x4: return "host-cursor";
	case 2+0x6: case 2+0x9: case 2+0xa: case 2+0xb: case 2+0x16: return "host-span(fe2s-geometry)";
	case 2+0x8: return "host-Memcpy(untracked src)";
	case 2+0x1b: case 2+0x1c: return "host-text";
	case 2+0x6f: return "host-text(queued)";
	case 2+0x22: return "host-Fread";
	}
	return "host-other";
}

static void tag_rgb (int tag, unsigned char *p)
{
	const char *c = tag_class (tag);
	p[0] = p[1] = p[2] = 128;
	if (!strcmp (c, "fe2s-direct")) { p[0] = 255; p[1] = 0; p[2] = 0; }
	else if (!strncmp (c, "host-text", 9)) { p[0] = 0; p[1] = 255; p[2] = 0; }
	else if (!strcmp (c, "host-BlitBmp")) { p[0] = 0; p[1] = 96; p[2] = 255; }
	else if (!strncmp (c, "host-span", 9)) { p[0] = 255; p[1] = 255; p[2] = 0; }
	else if (!strncmp (c, "host-Memcpy", 11)) { p[0] = 0; p[1] = 255; p[2] = 255; }
	else if (!strcmp (c, "initial")) { p[0] = 64; p[1] = 64; p[2] = 64; }
}

/* Write <base>.prov.ppm (colour = who drew each visible UI pixel) and
 * <base>.prov.txt (pixel counts per class) for the displayed buffer. */
static void write_provenance (const char *base)
{
	static unsigned char rgb[SCRBYTES * 3];
	static const char *classes[] = { "fe2s-direct", "host-text", "host-text(queued)",
		"host-BlitBmp", "host-span(fe2s-geometry)", "host-Memcpy(untracked src)",
		"host-clear", "host-cursor", "host-Fread", "host-other", "initial" };
	unsigned long count[11] = { 0 }, visible = 0;
	struct tracked *t;
	char name[300];
	FILE *f;
	int i, k;

	track_screens ();
	prov_sync (2 + 0x6f);
	if (!(t = find_tracked (VideoBase))) return;
	for (i = 0; i < SCRBYTES; i++) {
		unsigned char *p = rgb + i * 3;
		int transparent = 0;
#ifdef WITH_GL
		transparent = (t->mirror[i] == 255);
#endif
		if (transparent) { p[0] = p[1] = p[2] = 0; continue; }
		visible++;
		tag_rgb (t->tag[i], p);
		for (k = 0; k < 11; k++)
			if (!strcmp (classes[k], tag_class (t->tag[i]))) { count[k]++; break; }
	}
	snprintf (name, sizeof (name), "%s.prov.ppm", base);
	write_ppm (name, rgb, 320, 200);
	snprintf (name, sizeof (name), "%s/%s.prov.txt", shot_dir, base);
	if ((f = fopen (name, "w"))) {
		fprintf (f, "frame %lu buffer 0x%x visible(non-transparent) %lu of %d\n",
			 frame_no, (unsigned) VideoBase, visible, SCRBYTES);
		for (k = 0; k < 11; k++)
			if (count[k]) fprintf (f, "%-28s %6lu  %5.1f%%\n", classes[k], count[k],
					       visible ? 100.0 * count[k] / visible : 0.0);
		fclose (f);
	}
}

/* --------------------------------------------------------------- trace */

static void trace_call (int n)
{
	char text[1024];
	int is_str = (n == HC_DRAWSTR || n == HC_DRAWSTRSHADOWED ||
		      n == HC_QUEUEDRAWSTR);

	calls_this_frame[n]++;
	text[0] = 0;
	if (is_str) {
		decode_str ((unsigned char *) STRam + GetReg (REG_A0), text, sizeof (text));
		note_text (text);
	}
	if (trace_on && trace_fp && (n != 0x1f || trace_level > 1) &&
	    (!trace_only_on || trace_only[n])) {
		u32 sp = GetReg (REG_A7);
		fprintf (trace_fp, "F%lu %02x %s d0=%x d1=%x d2=%x d3=%x d4=%x d5=%x a0=%x a1=%x sp+2=[%08x %08x %08x]",
			frame_no, n, hcall_names[n] ? hcall_names[n] : "?",
			GetReg (REG_D0), GetReg (REG_D1), GetReg (REG_D2),
			GetReg (REG_D3), GetReg (REG_D4), GetReg (REG_D5),
			GetReg (REG_A0), GetReg (REG_A1),
			STMemory_ReadLong (sp), STMemory_ReadLong (sp + 4),
			STMemory_ReadLong (sp + 8));
		if (is_str) fprintf (trace_fp, " str=\"%s\"", text);
		fputc ('\n', trace_fp);
	}
	if (prov_on && may_write_screen (n)) {
		u32 sp = GetReg (REG_A7);
		prov_before (n);
		orig_hcalls[n] ();
		prov_after (n, sp);
	} else {
		orig_hcalls[n] ();
	}
#ifdef WITH_GL
	/* E1 FE2_TEXT: glyph records follow the bytes Memcpy moves and Memset
	 * clears (the stack arguments are still in place after the call) */
	if (exp_text_on && (n == 0x01 || n == 0x02 || n == 0x08)) {
		u32 sp = GetReg (REG_A7);
		if (n == 0x08)
			ExpText_Memcpy (STMemory_ReadLong (sp), STMemory_ReadLong (sp + 4),
					STMemory_ReadLong (sp + 8));
		else
			ExpText_Clear (STMemory_ReadLong (sp + 4), STMemory_ReadLong (sp));
	}
#endif
}

#define TW(n) static void tw_##n (void) { trace_call (n); }
TW(0) TW(1) TW(2) TW(3) TW(4) TW(5) TW(6) TW(7)
TW(8) TW(9) TW(10) TW(11) TW(12) TW(13) TW(14) TW(15)
TW(16) TW(17) TW(18) TW(19) TW(20) TW(21) TW(22) TW(23)
TW(24) TW(25) TW(26) TW(27) TW(28) TW(29) TW(30) TW(31)
TW(32) TW(33) TW(34) TW(35) TW(36) TW(37) TW(38) TW(39)
TW(40) TW(41) TW(42) TW(43) TW(44) TW(45) TW(46) TW(47)
TW(48) TW(49) TW(50) TW(51) TW(52) TW(53) TW(54) TW(55)
TW(56) TW(57) TW(58) TW(59) TW(60) TW(61) TW(62) TW(63)
TW(64) TW(65) TW(66) TW(67) TW(68) TW(69) TW(70) TW(71)
TW(72) TW(73) TW(74) TW(75) TW(76) TW(77) TW(78) TW(79)
TW(80) TW(81) TW(82) TW(83) TW(84) TW(85) TW(86) TW(87)
TW(88) TW(89) TW(90) TW(91) TW(92) TW(93) TW(94) TW(95)
TW(96) TW(97) TW(98) TW(99) TW(100) TW(101) TW(102) TW(103)
TW(104) TW(105) TW(106) TW(107) TW(108) TW(109) TW(110) TW(111)
TW(112) TW(113) TW(114) TW(115) TW(116) TW(117) TW(118) TW(119)
TW(120) TW(121) TW(122) TW(123) TW(124) TW(125) TW(126) TW(127)
static const HOSTCALL tw_table[EXP_MAX_HCALLS] = {
	tw_0, tw_1, tw_2, tw_3, tw_4, tw_5, tw_6, tw_7,
	tw_8, tw_9, tw_10, tw_11, tw_12, tw_13, tw_14, tw_15,
	tw_16, tw_17, tw_18, tw_19, tw_20, tw_21, tw_22, tw_23,
	tw_24, tw_25, tw_26, tw_27, tw_28, tw_29, tw_30, tw_31,
	tw_32, tw_33, tw_34, tw_35, tw_36, tw_37, tw_38, tw_39,
	tw_40, tw_41, tw_42, tw_43, tw_44, tw_45, tw_46, tw_47,
	tw_48, tw_49, tw_50, tw_51, tw_52, tw_53, tw_54, tw_55,
	tw_56, tw_57, tw_58, tw_59, tw_60, tw_61, tw_62, tw_63,
	tw_64, tw_65, tw_66, tw_67, tw_68, tw_69, tw_70, tw_71,
	tw_72, tw_73, tw_74, tw_75, tw_76, tw_77, tw_78, tw_79,
	tw_80, tw_81, tw_82, tw_83, tw_84, tw_85, tw_86, tw_87,
	tw_88, tw_89, tw_90, tw_91, tw_92, tw_93, tw_94, tw_95,
	tw_96, tw_97, tw_98, tw_99, tw_100, tw_101, tw_102, tw_103,
	tw_104, tw_105, tw_106, tw_107, tw_108, tw_109, tw_110, tw_111,
	tw_112, tw_113, tw_114, tw_115, tw_116, tw_117, tw_118, tw_119,
	tw_120, tw_121, tw_122, tw_123, tw_124, tw_125, tw_126, tw_127,
};

static void install_trace (void)
{
	int i;
	for (i = 0; i < hcalls_count && i < EXP_MAX_HCALLS; i++) {
		orig_hcalls[i] = hcalls[i];
		if (hcalls[i]) hcalls[i] = tw_table[i];
	}
}

/* -------------------------------------------------------------- script */

#define MAX_CMDS	512
struct cmd {
	int rel;		/* when is relative to the previous command */
	unsigned long when;
	char op[16];
	char arg[128];
};
static struct cmd cmds[MAX_CMDS];
static int ncmds, cmd_pos;
static unsigned long last_cmd_frame;
static int script_on;

static char shot_pending[256];

static struct { unsigned char code; unsigned long at; } key_up_q[32];
static int nkey_up;
static unsigned long click_release_at;
static int click_mask;

static void load_script (const char *path)
{
	FILE *f = fopen (path, "r");
	char line[256];

	if (!f) {
		fprintf (stderr, "FE2_SCRIPT: cannot open %s\n", path);
		return;
	}
	while (fgets (line, sizeof (line), f) && ncmds < MAX_CMDS) {
		struct cmd *c = &cmds[ncmds];
		char when[32];
		int off = 0;

		line[strcspn (line, "\r\n")] = 0;
		if (line[0] == '#' || line[0] == 0) continue;
		if (sscanf (line, "%31s %15s %n", when, c->op, &off) < 2) continue;
		strncpy (c->arg, line + off, sizeof (c->arg) - 1);
		c->rel = (when[0] == '+');
		c->when = strtoul (when + c->rel, NULL, 10);
		ncmds++;
	}
	fclose (f);
	script_on = 1;
}

static void write_ppm (const char *name, const unsigned char *rgb, int w, int h)
{
	char path[1024];
	FILE *f;

	snprintf (path, sizeof (path), "%s/%s", shot_dir, name);
	if (!(f = fopen (path, "wb"))) {
		fprintf (stderr, "FE2 shot: cannot write %s\n", path);
		return;
	}
	fprintf (f, "P6\n%d %d\n255\n", w, h);
	fwrite (rgb, 3, w * h, f);
	fclose (f);
}

/* The 320x200 8-bit buffer as fe2.s drew it: rows 0-167 use the main
 * palette, 168-199 the control panel palette. */
static void write_st_buffer (const char *base)
{
	static unsigned char rgb[320 * 200 * 3];
	unsigned char *scr = VideoRaster, *p = rgb;
	char name[300];
	int x, y;

	if (!scr) return;
	for (y = 0; y < 200; y++) {
		for (x = 0; x < 320; x++, scr++) {
			unsigned short st = (y < 168) ? MainPalette[*scr] : CtrlPalette[*scr & 15];
#ifdef WITH_GL
			if (*scr == 255) { *p++ = 255; *p++ = 0; *p++ = 255; continue; }
#endif
			*p++ = ((st >> 8) & 0xf) * 17;
			*p++ = ((st >> 4) & 0xf) * 17;
			*p++ = (st & 0xf) * 17;
		}
	}
	snprintf (name, sizeof (name), "%s.st.ppm", base);
	write_ppm (name, rgb, 320, 200);
}

static void run_cmd (struct cmd *c)
{
	unsigned int code;

	if (!strcmp (c->op, "key")) {
		code = strtoul (c->arg, NULL, 16);
		Input_PressSTKey (code, TRUE);
		if (nkey_up < 32) {
			key_up_q[nkey_up].code = code;
			key_up_q[nkey_up++].at = frame_no + 2;
		}
	} else if (!strcmp (c->op, "keydown")) {
		Input_PressSTKey (strtoul (c->arg, NULL, 16), TRUE);
	} else if (!strcmp (c->op, "keyup")) {
		Input_PressSTKey (strtoul (c->arg, NULL, 16), FALSE);
	} else if (!strcmp (c->op, "mouse")) {
		int x = 0, y = 0;
		sscanf (c->arg, "%d %d", &x, &y);
		input.abs_x = x * screen_w / 320;
		input.abs_y = y * screen_h / 200;
	} else if (!strcmp (c->op, "click")) {
		click_mask = (c->arg[0] == 'r') ? 1 : 2;
		input.cur_mousebut_state |= click_mask;
		click_release_at = frame_no + 3;
	} else if (!strcmp (c->op, "shot")) {
		strncpy (shot_pending, c->arg, sizeof (shot_pending) - 1);
	} else if (!strcmp (c->op, "trace")) {
		trace_on = (trace_fp && !strcmp (c->arg, "on"));
	} else if (!strcmp (c->op, "mark")) {
		if (trace_fp) fprintf (trace_fp, "MARK F%lu %s\n", frame_no, c->arg);
	} else if (!strcmp (c->op, "poke")) {
		/* provenance positive control: write one row of 16 pixels into the
		 * displayed buffer outside any screen-writing host call, the way
		 * a 68k rasteriser inside fe2.s would */
		int x = 0, y = 0, col = 0, i;
		sscanf (c->arg, "%d %d %d", &x, &y, &col);
		for (i = 0; i < 16 && x + i < 320; i++)
			STRam[VideoBase + y * 320 + x + i] = col;
	} else if (!strcmp (c->op, "quit")) {
		if (prov_on)
			fprintf (stderr, "FE2_PROVENANCE: %lu bytes written by fe2.s outside host calls (all tracked buffers, whole run)\n",
				 fe2s_direct_total);
		if (trace_fp) fflush (trace_fp);
#ifdef WITH_GL
		if (exp_text_on) ExpText_Stats (stderr);
#endif
		fprintf (stderr, "FE2_SCRIPT: quit at frame %lu\n", frame_no);
		SDL_Quit ();
		exit (0);
	}
	if (trace_fp && trace_on)
		fprintf (trace_fp, "CMD F%lu %s %s\n", frame_no, c->op, c->arg);
}

static void step_script (void)
{
	int i;

	for (i = 0; i < nkey_up; ) {
		if (frame_no >= key_up_q[i].at) {
			Input_PressSTKey (key_up_q[i].code, FALSE);
			key_up_q[i] = key_up_q[--nkey_up];
		} else i++;
	}
	if (click_release_at && frame_no >= click_release_at) {
		input.cur_mousebut_state &= ~click_mask;
		click_release_at = 0;
	}
	while (cmd_pos < ncmds) {
		struct cmd *c = &cmds[cmd_pos];
		unsigned long due = c->rel ? last_cmd_frame + c->when : c->when;

		if (frame_no < due) break;
		if (!strcmp (c->op, "wait") && !text_seen (c->arg)) break;
		run_cmd (c);
		last_cmd_frame = frame_no;
		cmd_pos++;
	}
}

/* ------------------------------------------------------------- entries */

void Exp_Init (void)
{
	const char *s;

	if ((s = getenv ("FE2_SHOT_DIR")) && *s)
		strncpy (shot_dir, s, sizeof (shot_dir) - 1);
	if ((s = getenv ("FE2_TRACE_HCALLS")) && *s && strcmp (s, "0")) {
		const char *path = getenv ("FE2_TRACE_FILE");
		trace_fp = (path && *path) ? fopen (path, "w") : stderr;
		if (!trace_fp) trace_fp = stderr;
		trace_on = 1;
		trace_level = atoi (s) > 1 ? 2 : 1;
		exp_any = 1;
		if ((s = getenv ("FE2_TRACE_ONLY")) && *s) {
			char *e;
			do {
				unsigned long id = strtoul (s, &e, 16);
				if (e == s) break;
				if (id < EXP_MAX_HCALLS) trace_only[id] = 1;
				s = (*e == ',') ? e + 1 : e;
			} while (*e == ',');
			trace_only_on = 1;
		}
	}
	if ((s = getenv ("FE2_PROVENANCE")) && *s && strcmp (s, "0")) {
		prov_on = 1;
		exp_any = 1;
	}
	if ((s = getenv ("FE2_SCRIPT")) && *s) {
		load_script (s);
		exp_any = 1;
	}
#ifdef WITH_GL
	if ((s = getenv ("FE2_TEXT")) && *s && strcmp (s, "0")) {
		ExpText_Init ();
		exp_any = 1;
	}
#endif
	/* the script's 'wait' reads strings from the trace wrappers */
	if (exp_any) install_trace ();
}

void Exp_HostUpdate (void)
{
	if (!exp_any) return;
	if (script_on) step_script ();
}

void Exp_EndFrame (void)
{
	int i;

	if (!exp_any) return;
	if (trace_fp && trace_on && !trace_only_on) {
		fprintf (trace_fp, "H F%lu", frame_no);
		for (i = 0; i < EXP_MAX_HCALLS; i++)
			if (calls_this_frame[i])
				fprintf (trace_fp, " %s:%lu", hcall_names[i] ? hcall_names[i] : "?", calls_this_frame[i]);
		fputc ('\n', trace_fp);
	}
	memset (calls_this_frame, 0, sizeof (calls_this_frame));
	if (shot_pending[0]) {
		unsigned char *rgb;
		int w, h;
		char name[300];

		if (Screen_ReadRGB (&rgb, &w, &h) == 0) {
			snprintf (name, sizeof (name), "%s.ppm", shot_pending);
			write_ppm (name, rgb, w, h);
			free (rgb);
		}
		write_st_buffer (shot_pending);
		if (prov_on) write_provenance (shot_pending);
		fprintf (stderr, "FE2_SCRIPT: shot %s at frame %lu\n", shot_pending, frame_no);
#ifdef WITH_GL
		if (exp_text_on) ExpText_Stats (stderr);
#endif
		shot_pending[0] = 0;
	}
	frame_no++;
	seen_cur ^= 1;
	seen_len[seen_cur] = 0;
	seen_text[seen_cur][0] = 0;
}
