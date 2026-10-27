/*
 * hdface: see hdface.h.
 *
 * The game puts a face together from bitmap parts (shoulders, ears, chin,
 * mouth, nose, eyes, hair, perhaps a cap), each blitted opaquely into the
 * face box, then sets main palette colours 1-4 to one of eight hair (and
 * clothing) ramps; the skin is a fixed ramp in colours 5-14.  So:
 *
 *  - hdface_blit collects the part blits that land in the face box into a
 *    group and paints them, in order, onto a black copy of the box: the
 *    composite, exactly what the box shows once the face is drawn.
 *  - The person is the ordered list of (position, size, pixel hash) of the
 *    parts plus the hair ramp.  From the parts and the composite come the
 *    attributes a portrait is matched on (sex; hair, a cap or a shaved
 *    head; glasses, sunglasses or neither; hair length), from the palette
 *    the hair colour (the game paints the clothes and the cap in it too).
 *  - A portrait is picked from the pool listed in art/faces/manifest.txt:
 *    the best attribute match, ties broken by the person's hash, so the
 *    same person gets the same portrait on every screen, frame and run.
 *  - hdface_frame takes the box out of the uploaded screen only while the
 *    raster's box holds exactly the composite and the skin ramp is the
 *    game's own (or a uniformly darkened copy of it: the portrait is
 *    darkened to match).  Anything else - a half drawn face, another
 *    screen, a palette the game is cycling - shows the game's pixels.
 *
 * Messages (a station's clearance, a pilot's hail, the police) show no
 * face of their own: the console gets one of two fixed 32x30 thumbnails,
 * the same for everybody.  The sender is known though - the message
 * routine keeps its registers in the game's variables, the sender object
 * among them - and a face is a function of (seed, flags) (L4fe58).  So
 * when the thumbnail is blitted, the parts the face generator would pick
 * for that person are read from its tables (ST memory, read only) and go
 * through the same analysis and choice as a face drawn in the box: the
 * thumbnail becomes the very portrait that person gets on a big screen.
 * The person is the sender object's (its own seed), never the message's
 * text argument, so one sender keeps one face.  A station speaks with
 * the seed and flags its station services face is drawn with: every
 * message from a station, clearance, fee or fine, is the person behind
 * its counter.  The thumbnail is replaced only while the console shows
 * exactly the game's thumbnail, and a gutter of the game's black edge is
 * kept between the portrait and the text.  Under --classic-ui and
 * --pixel-faces every face, box and thumbnail, stays the game's.
 *
 * Portraits keep their shape: the part of the picture shown is cut to the
 * aspect its rectangle has on the screen (the GL viewport), never
 * stretched.
 *
 * Nothing here writes ST memory or changes what the 68k code sees.
 */
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdface.h"
#include "art.h"
#include "../m68000.h"
#include "../fixups.h"

/* gl.c: --classic-ui keeps the game's own faces, box and thumbnail (the
 * old look), as --pixel-faces does */
extern int classic_ui;

#define ST_W		320
#define ST_H		200
/* the face box's black interior, x0 <= x < x1, y0 <= y < y1 */
#define BX0		1
#define BY0		22
#define BX1		119
#define BY1		136
#define BW		(BX1 - BX0)
#define BH		(BY1 - BY0)
#define MAXPARTS	16
#define MAXPOOL		96
#define MANIFEST	"faces/manifest.txt"
#define MAXTEX		8	/* portraits kept uploaded (4 MB each, plus mipmaps) */
#define ST_MEM_SIZE	0x110000

/* The message thumbnail: one of two fixed 32x30 bitmaps (L78d10 for an
 * even d3, L78ef4 for an odd one) at (89,169) in the console, perhaps
 * with a 32 wide overlay at (88,169) over its top (L790d8, a cap, for
 * flags & 6 == 2; L7919c, a crown, for 6).  The face is in their left
 * part; the message's first letters go over their (black) right edge from
 * x 116.  TX0..TX1 x TY0..TY1 is the face's part of both.  The portrait
 * covers x0..PX1 only: the game's black edge, PX1..TX1, stays as the
 * gutter between the picture and the text (the game's own face ends at
 * about x 110). */
#define TX0		88
#define TY0		169
#define TX1		116
#define TY1		199
#define TW		(TX1 - TX0)
#define TH		(TY1 - TY0)
#define THUMB_X		89
#define OVER_X		88
#define PX1		112

/* the game's state, read only (fe2.s) */
#define A6			__DL5eb6_a6_base
#define A6_MSG_REGS		11690	/* the message's d0-3/d7/a0/a2 (movem.l, L760ba) */
#define A6_MSG_ARG		(A6_MSG_REGS + 12)	/* d3: a text argument (a seed, a fee, a fine) */
#define A6_MSG_FLAGS		(A6_MSG_REGS + 18)	/* d7's low word: face flags; bit 0, a0 is no object */
#define A6_MSG_SENDER		(A6_MSG_REGS + 20)	/* a0: the sender (its name at +246) */
#define A6_FACE_SEED		11736	/* long: the big face's d0 (L4fe58) */
#define A6_FACE_FLAGS		11740	/* word: its d1 */
#define A6_BIG_SPACE		14466	/* long: the object slots (as hdback.c) */
#define OBJ_OFFSETS		__DL44b02	/* word per index: slot offset */
#define OBJ_SLOTS		115
#define OBJ_SIZE		286
#define OBJ_MODEL		90	/* word */
#define OBJ_INDEX		93	/* byte: its own slot */
#define OBJ_SEED		118	/* long */
#define MODEL_PORT_FIRST	0x54	/* orbital stations .. surface starports */
#define MODEL_PORT_LAST		0x6c
#define FLAGS_STAFF		0x1c	/* station services: grey, a cap */
#define FLAGS_POLICE		0x3a	/* the same counter while the police deal with you */

enum { EYES_NONE, EYES_GLASSES, EYES_SHADES };

/* hair ramps in the order the game indexes them: colours 1-4 as ST $0RGB */
static const unsigned short ramps[8][4] = {
	{ 0x442, 0x331, 0x220, 0x110 },	/* brown */
	{ 0x522, 0x411, 0x300, 0x200 },	/* auburn */
	{ 0x772, 0x661, 0x440, 0x330 },	/* blonde */
	{ 0x555, 0x444, 0x333, 0x222 },	/* grey */
	{ 0x444, 0x333, 0x222, 0x111 },	/* black */
	{ 0x633, 0x522, 0x411, 0x200 },	/* red */
	{ 0x252, 0x141, 0x030, 0x020 },	/* green */
	{ 0x337, 0x226, 0x115, 0x003 },	/* blue */
};
static const char *ramp_names[8] = {
	"brown", "auburn", "blonde", "grey", "black", "red", "green", "blue"
};
/* ramps that read alike: warm browns, blonde, greys, green, blue */
static const int ramp_group[8] = { 0, 0, 1, 2, 2, 0, 3, 4 };
/* skin, colours 5-14, as ST $0RGB */
static const unsigned short skin[10] = {
	0x400, 0x420, 0x840, 0xa62, 0xa84, 0xda6, 0xfd8, 0xffd, 0xa8d, 0x64a
};

typedef struct {
	int x, y, w, h, cols;
	int dark;		/* eye strip: share of the eyes (per 1000) in black or hair colours */
	unsigned long long hash;
} part_t;

/* a face: its parts as drawn into the box, and what is read from them */
typedef struct {
	part_t parts[MAXPARTS];
	int nparts, bad;
	unsigned char comp[BH][BW];
	int analysed, is_face, male, cap, bald, eyes, longhair, eye_dark;
	unsigned long long body_hash;
	int ramp, pick;
} face_t;

typedef struct {
	char file[64];
	int male, ramp, cap, bald, eyes, longhair;
	int bad;		/* would not load */
	unsigned char *pix;	/* RGBA, top row first, until uploaded */
	int w, h;
	GLuint tex;
	int tw, th;
	int used;		/* frame it was last drawn */
} portrait_t;

static int frame_no;
static FILE *logf_;

/* the group being built / last built in the box */
static face_t live = { .ramp = -1, .pick = -1 };
static int group_frame = -1;

/* the message thumbnail last blitted */
static struct {
	int valid;			/* a thumbnail blit seen */
	unsigned char comp[TH][TW];	/* what it put on the screen ... */
	unsigned char mask[TH][TW];	/* ... and where */
	int overlay;
	int flags, station, pick;
} thumb = { .pick = -1 };

/* the pool */
static portrait_t pool[MAXPOOL];
static int npool = -1;

/* what hdface_draw draws this frame */
static int shown = -1, thumb_shown = -1;
static float shade = 1.0f;

static unsigned long long fnv (unsigned long long h, const unsigned char *p, int n)
{
	while (n--) { h ^= *p++; h *= 0x100000001b3ULL; }
	return h;
}

static unsigned long long fnv_int (unsigned long long h, int v)
{
	unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
	return fnv (h, b, 4);
}

static void log_init (void)
{
	static int done;
	const char *p;
	if (done) return;
	done = 1;
	p = getenv ("FE2_HDFACE_LOG");
	if (p && *p) logf_ = fopen (p, "w");
}

/* ST memory, read only */
static int rb (unsigned int a) { return STMemory_ReadByte (a) & 0xff; }
static int rw (unsigned int a) { return STMemory_ReadWord (a) & 0xffff; }
static unsigned int rl (unsigned int a) { return (unsigned int) STMemory_ReadLong (a); }
static int st_ok (unsigned int a, unsigned int n) { return a >= 0x400 && a < ST_MEM_SIZE && n <= ST_MEM_SIZE - a; }

/* ---- the pool ---- */

static void load_manifest (void)
{
	FILE *f;
	char line[256];
	npool = 0;
	f = fopen (art_path (MANIFEST), "r");
	if (!f) return;
	while (npool < MAXPOOL && fgets (line, sizeof line, f)) {
		char file[64], sex[8], hair[16], eyestr[16], len[16];
		int c, i;
		portrait_t *p = &pool[npool];
		if (line[0] == '#' || line[0] == '\n') continue;
		if (sscanf (line, "%63s %7s %15s %d %15s %15s", file, sex, hair, &c, eyestr, len) != 6)
			continue;
		memset (p, 0, sizeof *p);
		snprintf (p->file, sizeof p->file, "faces/%s", file);
		p->male = sex[0] == 'm';
		p->ramp = -1;
		for (i = 0; i < 8; i++)
			if (!strcmp (hair, ramp_names[i])) p->ramp = i;
		if (p->ramp < 0) continue;
		p->cap = c == 1;
		p->bald = c == 2;
		p->eyes = !strcmp (eyestr, "shades") ? EYES_SHADES :
			  !strcmp (eyestr, "glasses") ? EYES_GLASSES : EYES_NONE;
		p->longhair = !strcmp (len, "long");
		npool++;
	}
	fclose (f);
}

static int score (const portrait_t *p, const face_t *f)
{
	int s = 0;
	if (p->male == f->male) s += 1000;
	if (p->cap == f->cap) s += 100;
	if (p->bald == f->bald) s += 100;
	if (p->eyes == f->eyes) s += 50;
	if (f->cap || f->bald) {
		/* no hair to see: the ramp is the cap's or the clothes' colour */
		if (ramp_group[p->ramp] == ramp_group[f->ramp]) s += 4;
	} else {
		if (p->ramp == f->ramp) s += 4;
		else if (ramp_group[p->ramp] == ramp_group[f->ramp]) s += 2;
		/* the game's men all wear their hair short: a portrait with it
		 * to the collar is as good a match */
		if (!f->male && p->longhair == f->longhair) s += 1;
	}
	return s;
}

/* the portrait for f (analysed, its ramp known), or -1 */
static int choose (const face_t *f)
{
	int i, best = -1, n = 0, k;
	unsigned long long id = fnv_int (f->body_hash, f->ramp);
	if (npool < 0) load_manifest ();
	for (i = 0; i < npool; i++) {
		int s;
		if (pool[i].bad) continue;
		s = score (&pool[i], f);
		if (s > best) { best = s; n = 0; }
		if (s == best) n++;
	}
	if (best < 0) return -1;
	/* fold the hash so every bit of it counts */
	id ^= id >> 33;
	id *= 0xff51afd7ed558ccdULL;
	id ^= id >> 33;
	k = (int) (id % (unsigned long long) n);
	for (i = 0; i < npool; i++)
		if (!pool[i].bad && score (&pool[i], f) == best && k-- == 0) return i;
	return -1;
}

static int ensure_pixels (int i)
{
	portrait_t *p = &pool[i];
	if (p->bad) return 0;
	if (p->tex || p->pix) return 1;
	p->pix = art_load_rgba (p->file, &p->w, &p->h);
	if (!p->pix || p->w <= 0 || p->h <= 0) {
		free (p->pix);
		p->pix = NULL;
		p->bad = 1;
		if (logf_) fprintf (logf_, "f%d cannot load %s\n", frame_no, p->file);
		return 0;
	}
	return 1;
}

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

/* Upload p, padded to powers of two by repeating its last column and row,
 * with mipmaps: the thumbnail shows a portrait at a tenth of its size, and
 * the smaller levels it is drawn from then hold nothing but the picture. */
static void upload (portrait_t *p)
{
	int i, n = 0, old = -1, y;
	unsigned char *pad;
	/* keep a few: drop the one drawn longest ago */
	for (i = 0; i < npool; i++)
		if (pool[i].tex) {
			n++;
			if (old < 0 || pool[i].used < pool[old].used) old = i;
		}
	if (n >= MAXTEX) {
		glDeleteTextures (1, &pool[old].tex);
		pool[old].tex = 0;
	}
	p->tw = pow2 (p->w);
	p->th = pow2 (p->h);
	pad = malloc ((size_t) p->tw * p->th * 4);
	if (pad)
		for (y = 0; y < p->th; y++) {
			unsigned char *row = pad + (size_t) y * p->tw * 4;
			const unsigned char *src = p->pix + (size_t) (y < p->h ? y : p->h - 1) * p->w * 4;
			memcpy (row, src, (size_t) p->w * 4);
			for (i = p->w; i < p->tw; i++) memcpy (row + i * 4, src + (p->w - 1) * 4, 4);
		}
	glGenTextures (1, &p->tex);
	glBindTexture (GL_TEXTURE_2D, p->tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#ifdef GL_GENERATE_MIPMAP
	glTexParameteri (GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, pad ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
#else
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
#endif
	if (pad) {
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, p->tw, p->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pad);
		free (pad);
	} else {
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, p->tw, p->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, p->w, p->h, GL_RGBA, GL_UNSIGNED_BYTE, p->pix);
	}
	free (p->pix);
	p->pix = NULL;
}

/* ---- collecting the parts ---- */

static void face_reset (face_t *f)
{
	f->nparts = 0;
	f->bad = 0;
	memset (f->comp, 0, sizeof f->comp);
	f->analysed = 0;
	f->is_face = 0;
	f->ramp = -1;
	f->pick = -1;
	f->male = f->cap = f->bald = f->eyes = f->longhair = f->eye_dark = 0;
}

static int in_box (int width_words, int height, int x, int y)
{
	return width_words > 0 && height > 0 &&
	       x >= BX0 && y >= BY0 && x + width_words * 16 <= BX1 && y + height <= BY1;
}

/* a part drawn at (x, y), inside the box: paint it into f's composite and
 * note it */
static void add_part (face_t *f, const unsigned char *planes, int width_words, int height, int x, int y)
{
	int w = width_words * 16, plane = 2 * height * width_words, i, j, k, used = 0, dk = 0, n = 0;
	part_t *pt;

	f->analysed = 0;
	f->ramp = -1;
	f->pick = -1;
	if (f->nparts == MAXPARTS) { f->bad = 1; return; }
	for (j = 0; j < height; j++)
		for (i = 0; i < w; i++) {
			int wo = (j * width_words + i / 16) * 2, b = 15 - (i & 15), c = 0;
			for (k = 0; k < 4; k++) {
				int v = (planes[k * plane + wo] << 8) | planes[k * plane + wo + 1];
				c |= ((v >> b) & 1) << k;
			}
			f->comp[y + j - BY0][x + i - BX0] = (unsigned char) c;
			used |= 1 << c;
			/* the eyes themselves, below the brows */
			if (j >= 2 && j < 13 && i >= 6 && i < 58) {
				n++;
				if (c <= 4) dk++;
			}
		}
	pt = &f->parts[f->nparts++];
	pt->x = x;
	pt->y = y;
	pt->w = w;
	pt->h = height;
	pt->cols = used;
	pt->dark = n ? dk * 1000 / n : 0;
	pt->hash = fnv (0xcbf29ce484222325ULL, planes, 4 * plane);
}

/* Sex, headgear, eyewear, hair length and the parts' hash, from the group.
 * Every face ends in the same seven parts, eyes last (shoulders, the two
 * ears, chin, mouth, nose, eyes); what comes around them tells the head:
 * hair parts before them, a cap after them, or one bare crown before them
 * (a shaved head). */
static void analyse (face_t *f)
{
	int i, ei = -1, cs, vm = 0, vf = 0;
	unsigned long long h = 0xcbf29ce484222325ULL;

	f->analysed = 1;
	f->is_face = 0;
	if (f->bad || f->nparts < 7) return;
	for (i = 0; i < f->nparts; i++) {
		const part_t *p = &f->parts[i];
		h = fnv_int (h, p->x);
		h = fnv_int (h, p->y);
		h = fnv_int (h, p->w);
		h = fnv_int (h, p->h);
		h = fnv (h, (const unsigned char *) &p->hash, sizeof p->hash);
		/* eyes: the 64 wide strip at eye level */
		if (p->w == 64 && p->y >= 58 && p->y <= 68 && ei < 0) {
			ei = i;
			if (p->x == 24) vm++;
			if (p->x == 25) vf++;
		}
		/* ears, nose */
		if (p->w == 16 && p->y >= 76 && p->y <= 84) {
			if (p->h == 31) vm++;
			else vf++;
		}
		if (p->w == 32 && p->h >= 15 && p->y >= 78 && p->y <= 84) {
			if (p->y == 80) vm++;
			if (p->y == 81) vf++;
		}
	}
	if (ei < 6) return;
	f->body_hash = h;
	f->male = vm > vf;
	cs = ei - 6;	/* where the seven start */
	/* a part over the top of the head drawn after the eyes: a cap */
	f->cap = 0;
	for (i = ei + 1; i < f->nparts; i++)
		if (f->parts[i].y < 40 && f->parts[i].h >= 20) f->cap = 1;
	/* one part before the seven, all skin: a bare crown */
	f->bald = !f->cap && cs == 1 && !(f->parts[0].cols & 0x1e);
	/* hair wider than the head falls past it */
	f->longhair = 0;
	if (!f->cap && !f->bald)
		for (i = 0; i < cs; i++)
			if (f->parts[i].x < 16 || f->parts[i].x + f->parts[i].w > 100) f->longhair = 1;
	/* glasses make the strip taller; sunglasses black out the eyes
	 * (the brows are in the hair colours, so only the eyes count) */
	f->eye_dark = f->parts[ei].dark;
	f->eyes = f->parts[ei].h >= 21 ? EYES_GLASSES : f->eye_dark >= 750 ? EYES_SHADES : EYES_NONE;
	f->is_face = 1;
}

/* ---- a face the game would draw, put together without drawing it ---- */

static unsigned int ror (unsigned int v, int n)
{
	return (v >> n) | (v << (32 - n));
}

/* the part at displacement disp in table a1, as L4ff38 blits it */
static void virtual_part (face_t *f, unsigned int a1, int disp)
{
	unsigned int a0, bmp;
	int x, y, ww, h;
	if (!st_ok (a1 + disp, 2)) { f->bad = 1; return; }
	a0 = a1 + (unsigned int) (int) (short) rw (a1 + disp);
	if (!st_ok (a0, 8)) { f->bad = 1; return; }
	x = (short) rw (a0);
	y = (short) rw (a0 + 2);
	bmp = a0 + 4;
	ww = rw (bmp);
	h = rw (bmp + 2);
	/* the live path keeps only parts inside the box: so does this */
	if (!in_box (ww, h, x, y)) return;
	if (!st_ok (bmp + 4, 8u * ww * h)) { f->bad = 1; return; }
	add_part (f, (const unsigned char *) STRam + bmp + 4, ww, h, x, y);
}

/* The face L4fe58 draws for d0 = seed, d1 = flags: the same parts in the
 * same order, the hair ramp it sets.  Reads the generator's tables; the
 * screen and the palette are left alone. */
static void compose (face_t *f, unsigned int d0, int d1)
{
	unsigned int a1 = (d0 & 1) ? __DL56f0c : __DL558c6;
	int d3 = d1 & 6, ramp_from;

	face_reset (f);
	if (d3) {
		if (d3 == 6) virtual_part (f, a1, 78 + 6);
		d0 = ror (d0, 7);
	} else {
		virtual_part (f, a1, 64 + (d0 & 0xe));
		d0 = ror (d0, 4);
		virtual_part (f, a1, -64 + (d0 & 0xe));
		d0 = ror (d0, 3);
	}
	virtual_part (f, a1, 48 + (d0 & 0xe));
	d0 = ror (d0, 3);
	virtual_part (f, a1, -32 + (d0 & 0xe));
	virtual_part (f, a1, -16 + (d0 & 0xe));
	d0 = ror (d0, 3);
	virtual_part (f, a1, 0 + (d0 & 0xe));
	d0 = ror (d0, 3);
	virtual_part (f, a1, 16 + (d0 & 0xe));
	d0 = ror (d0, 3);
	virtual_part (f, a1, 32 + (d0 & 0xe));
	d0 = ror (d0, 3);
	virtual_part (f, a1, -48 + (d0 & 0xe));
	ramp_from = (d1 & 0xffff) ? d1 : (int) (d0 & 0xffff);
	if (d3 == 2 || d3 == 4) virtual_part (f, a1, 78 + d3);
	analyse (f);
	f->ramp = (ramp_from & 0x38) >> 3;
}

/* ---- the message thumbnail ---- */

/* is p one of the game's objects (it sits in the slot its own index
 * names)? */
static int is_object (unsigned int p)
{
	unsigned int big = rl (A6 + A6_BIG_SPACE);
	int idx;

	if (!st_ok (p, OBJ_SIZE) || !st_ok (big, 4)) return 0;
	idx = rb (p + OBJ_INDEX);
	if (idx >= OBJ_SLOTS) return 0;
	return big + (unsigned int) (int) (short) rw (OBJ_OFFSETS + 2 * idx) == p;
}

/* Who the thumbnail stands for, as a face: (seed, flags).  The person is
 * the sender, not the message: d3 is only what the text prints (the
 * station's seed for a clearance, the fee for a docking fee, the amount
 * of a fine), so the seed is read from the sender itself.
 *  - A starport: the person at its station services counter, for every
 *    message it sends (clearance, fees, fines): its seed and the flags
 *    that page draws its face with (as it last did here, else $1c).
 *  - Any other object (a ship, a pilot): its own seed, so the same sender
 *    is always the same face, and the face flags the message gives (the
 *    game passes face flags in d7: $3a the police uniform, 0 none).
 *  - No sender object (d7 bit 0: a0 is a text argument): the message's
 *    own d3 and flags; stable for that message. */
static void thumb_person (void)
{
	unsigned int arg = rl (A6 + A6_MSG_ARG), sender = rl (A6 + A6_MSG_SENDER), seed;
	int d7 = rw (A6 + A6_MSG_FLAGS), model, ff, obj;
	const char *who;
	face_t f;

	thumb.station = 0;
	obj = !(d7 & 1) && is_object (sender);
	if (obj) {
		seed = rl (sender + OBJ_SEED);
		model = rw (sender + OBJ_MODEL);
		thumb.station = model >= MODEL_PORT_FIRST && model <= MODEL_PORT_LAST;
	} else
		seed = arg;
	if (thumb.station) {
		ff = rw (A6 + A6_FACE_FLAGS);
		thumb.flags = rl (A6 + A6_FACE_SEED) == seed && (ff == FLAGS_STAFF || ff == FLAGS_POLICE)
			? ff : FLAGS_STAFF;
		who = "station";
	} else {
		thumb.flags = d7 & 0x3e;
		who = obj ? "object" : "anon";
	}
	compose (&f, seed, thumb.flags);
	thumb.pick = f.is_face ? choose (&f) : -1;
	if (logf_) {
		fprintf (logf_, "f%d thumb seed=%08x flags=%02x d7=%04x d3=%08x %s sender=%06x %016llx %s %s %s eyes=%d -> %s\n",
			 frame_no, seed, thumb.flags, d7, arg, who, sender,
			 f.is_face ? fnv_int (f.body_hash, f.ramp) : 0ULL,
			 f.is_face ? (f.male ? "m" : "f") : "-",
			 f.is_face && f.ramp >= 0 ? ramp_names[f.ramp] : "-",
			 !f.is_face ? "-" : f.cap ? "cap" : f.bald ? "bald" : "hair", f.eyes,
			 thumb.pick >= 0 ? pool[thumb.pick].file : "-");
		fflush (logf_);
	}
}

/* is this blit the thumbnail or its overlay?  (then note it) */
static int thumb_blit (const unsigned char *planes, int width_words, int height,
		       int x, int y, unsigned int bmp)
{
	int i, j, k, w = width_words * 16, plane = 2 * height * width_words, over;

	if (y != TY0 || width_words != 2) return 0;
	if (x == THUMB_X && height == TH && (bmp == __DL78d10 || bmp == __DL78ef4)) over = 0;
	else if (x == OVER_X && height <= TH && (bmp == __DL790d8 || bmp == __DL7919c)) over = 1;
	else return 0;
	if (!over) {
		/* a new message (blitted to both screens: the second the same) */
		memset (thumb.comp, 0, sizeof thumb.comp);
		memset (thumb.mask, 0, sizeof thumb.mask);
		thumb.overlay = 0;
		thumb.valid = 1;
		thumb_person ();
	} else if (!thumb.valid)
		return 1;
	else
		thumb.overlay = 1;
	for (j = 0; j < height; j++)
		for (i = 0; i < w; i++) {
			int wo = (j * width_words + i / 16) * 2, b = 15 - (i & 15), c = 0;
			for (k = 0; k < 4; k++) {
				int v = (planes[k * plane + wo] << 8) | planes[k * plane + wo + 1];
				c |= ((v >> b) & 1) << k;
			}
			if (x + i < TX1 && y + j < TY1) {
				thumb.comp[y + j - TY0][x + i - TX0] = (unsigned char) c;
				thumb.mask[y + j - TY0][x + i - TX0] = 1;
			}
		}
	return 1;
}

/* ---- the blits ---- */

void hdface_blit (const unsigned char *planes, int width_words, int height,
		  int x, int y, unsigned int bmp, unsigned int scr)
{
	int i, w = width_words * 16;

	(void) scr;
	if (pixel_faces || classic_ui) return;
	log_init ();
	if (thumb_blit (planes, width_words, height, x, y, bmp)) return;
	/* only whole parts inside the face box */
	if (!in_box (width_words, height, x, y)) return;
	/* a new group: the first part after a frame was shown, or a part
	 * landing where one of this group already did (a redraw) */
	if (group_frame != frame_no) live.nparts = 0;
	for (i = 0; i < live.nparts; i++)
		if (live.parts[i].x == x && live.parts[i].y == y && live.parts[i].w == w &&
		    live.parts[i].h == height) {
			live.nparts = 0;
			break;
		}
	if (live.nparts == 0) {
		face_reset (&live);
		group_frame = frame_no;
	}
	add_part (&live, planes, width_words, height, x, y);
}

/* ---- the palette ---- */

static unsigned int st_rgb (unsigned short c)
{
	/* as the renderer builds MainRGBPalette: BBGGRR, nibbles << 4 */
	return (((c >> 8) & 0xf) << 4) | ((((c >> 4) & 0xf) << 4) << 8) | (((c & 0xf) << 4) << 16);
}

static int chan (unsigned int c, int k)
{
	return (c >> (8 * k)) & 0xff;
}

/* how bright pal[first..first+n) is against want[] (1 = as is), or -1 if
 * it is not that ramp darkened uniformly */
static float ramp_scale (const unsigned int *pal, int first, const unsigned short *want, int n)
{
	int i, k, have = 0, full = 0;
	float s;
	for (i = 0; i < n; i++)
		for (k = 0; k < 3; k++) {
			have += chan (pal[first + i], k);
			full += chan (st_rgb (want[i]), k);
		}
	if (!full) return -1;
	s = have / (float) full;
	if (s > 1.02f) return -1;
	for (i = 0; i < n; i++)
		for (k = 0; k < 3; k++) {
			float d = chan (pal[first + i], k) - s * chan (st_rgb (want[i]), k);
			if (d > 24 || d < -24) return -1;
		}
	return s;
}

static int match_ramp (const unsigned int *pal, float s)
{
	int r, i, k;
	for (r = 0; r < 8; r++) {
		int ok = 1;
		for (i = 0; i < 4 && ok; i++)
			for (k = 0; k < 3; k++) {
				float d = chan (pal[1 + i], k) - s * chan (st_rgb (ramps[r][i]), k);
				if (d > 8 || d < -8) { ok = 0; break; }
			}
		if (ok) return r;
	}
	return -1;
}

/* ---- once a frame ---- */

/* FE2_HDFACE_LOG: is the face put together from the game's own (seed,
 * flags) the person the box shows?  (Wherever the game keeps them there;
 * the bulletin board's adverts keep theirs elsewhere.) */
static void log_selfcheck (void)
{
	face_t v;
	unsigned int seed = rl (A6 + A6_FACE_SEED);
	int flags = rw (A6 + A6_FACE_FLAGS);
	compose (&v, seed, flags);
	fprintf (logf_, "f%d check seed=%08x flags=%02x parts=%s ramp=%s\n", frame_no, seed, flags,
		 v.is_face && v.body_hash == live.body_hash ? "same" : "differ",
		 v.ramp == live.ramp ? "same" : "differ");
}

static void thumb_frame (const unsigned char *raster, unsigned char *out)
{
	int x, y, x0;

	if (!thumb.valid || thumb.pick < 0) return;
	/* only while the console shows exactly the game's thumbnail */
	for (y = 0; y < TH; y++)
		for (x = 0; x < TW; x++)
			if (thumb.mask[y][x] && raster[(TY0 + y) * ST_W + TX0 + x] != thumb.comp[y][x])
				return;
	if (!ensure_pixels (thumb.pick)) {
		thumb.pick = -1;
		return;
	}
	x0 = thumb.overlay ? OVER_X : THUMB_X;
	for (y = TY0; y < TY1; y++)
		memset (out + y * ST_W + x0, 255, PX1 - x0);
	thumb_shown = thumb.pick;
}

void hdface_frame (const unsigned char *raster, unsigned char *out,
		   const unsigned int *pal, const unsigned int *ctrl_pal)
{
	int y, r;
	float s;

	(void) ctrl_pal;
	frame_no++;
	shown = -1;
	thumb_shown = -1;
	if (pixel_faces || classic_ui) return;
	log_init ();
	thumb_frame (raster, out);
	if (live.nparts == 0) return;
	if (!live.analysed) analyse (&live);
	if (!live.is_face) return;
	/* the skin ramp is fixed: anything but it (or it darkened) is not
	 * the face's palette */
	s = ramp_scale (pal, 5, skin, 10);
	if (s <= 0.02f) return;
	r = match_ramp (pal, s);
	if (r >= 0 && r != live.ramp) {
		live.ramp = r;
		live.pick = -1;
	}
	if (live.ramp < 0) return;
	if (live.pick < 0) {
		live.pick = choose (&live);
		if (logf_) {
			fprintf (logf_, "f%d face %016llx %s %s %s eyes=%s dark=%d %s -> %s\n",
				 frame_no, fnv_int (live.body_hash, live.ramp), live.male ? "m" : "f",
				 ramp_names[live.ramp],
				 live.cap ? "cap" : live.bald ? "bald" : "hair",
				 live.eyes == EYES_SHADES ? "shades" : live.eyes == EYES_GLASSES ? "glasses" : "none",
				 live.eye_dark, live.longhair ? "long" : "short",
				 live.pick >= 0 ? pool[live.pick].file : "-");
			log_selfcheck ();
			fflush (logf_);
		}
		if (live.pick < 0) return;
	}
	/* only while the box shows exactly the face put together */
	for (y = BY0; y < BY1; y++)
		if (memcmp (raster + y * ST_W + BX0, live.comp[y - BY0], BW)) return;
	if (!ensure_pixels (live.pick)) {
		live.pick = choose (&live);
		return;
	}
	for (y = BY0; y < BY1; y++)
		memset (out + y * ST_W + BX0, 255, BW);
	shown = live.pick;
	shade = s > 1.0f ? 1.0f : s;
}

/* Draw portrait i over ST x0..x1, y0..y1, showing the part of it from
 * (sx0, sy0) to (sx1, sy1) (shares of its width and height) cut down to
 * the shape the rectangle has on the screen: the sides lose the same, the
 * top a quarter of what goes and the bottom the rest (the head stays). */
static void draw_portrait (int i, int x0, int y0, int x1, int y1,
			   float sx0, float sy0, float sx1, float sy1, float bright)
{
	portrait_t *p = &pool[i];
	GLint vp[4];
	float want = 1.0f, have, u0, v0, u1, v1, cut;

	if (!p->tex) {
		if (!p->pix) return;
		upload (p);
	}
	p->used = frame_no;
	/* the rectangle's shape in window pixels: the ortho view is 320x200
	 * over the viewport */
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] > 0 && vp[3] > 0)
		want = ((x1 - x0) * vp[2] / 320.0f) / ((y1 - y0) * vp[3] / 200.0f);
	have = ((sx1 - sx0) * p->w) / ((sy1 - sy0) * p->h);
	if (have > want) {
		cut = (sx1 - sx0) * (1.0f - want / have);
		sx0 += cut * 0.5f;
		sx1 -= cut * 0.5f;
	} else if (have < want) {
		cut = (sy1 - sy0) * (1.0f - have / want);
		sy0 += cut * 0.25f;
		sy1 -= cut * 0.75f;
	}
	u0 = sx0 * p->w / (float) p->tw;
	u1 = sx1 * p->w / (float) p->tw;
	v0 = sy0 * p->h / (float) p->th;
	v1 = sy1 * p->h / (float) p->th;
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, p->tex);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f (bright, bright, bright, 1.0f);
	glBegin (GL_TRIANGLE_STRIP);
		glTexCoord2f (u0, v1);
		glVertex2i (x0, ST_H - y1);
		glTexCoord2f (u1, v1);
		glVertex2i (x1, ST_H - y1);
		glTexCoord2f (u0, v0);
		glVertex2i (x0, ST_H - y0);
		glTexCoord2f (u1, v0);
		glVertex2i (x1, ST_H - y0);
	glEnd ();
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub (255, 255, 255, 255);
}

void hdface_draw (const unsigned int *pal, const unsigned int *ctrl_pal)
{
	(void) pal;
	(void) ctrl_pal;
	if (pixel_faces || classic_ui) return;
	/* the whole portrait, head and shoulders */
	if (shown >= 0)
		draw_portrait (shown, BX0, BY0, BX1, BY1, 0.0f, 0.0f, 1.0f, 1.0f, shade);
	/* the thumbnail is the size of a stamp: the head */
	if (thumb_shown >= 0)
		draw_portrait (thumb_shown, thumb.overlay ? OVER_X : THUMB_X, TY0, PX1, TY1,
			       0.15f, 0.05f, 0.85f, 0.77f, 1.0f);
}

void hdface_forget (void)
{
	int i;
	for (i = 0; i < npool; i++) {
		if (pool[i].tex) glDeleteTextures (1, &pool[i].tex);
		pool[i].tex = 0;
		free (pool[i].pix);
		pool[i].pix = NULL;
	}
	shown = -1;
	thumb_shown = -1;
}
