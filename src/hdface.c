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
#define MAXTEX		8	/* portraits kept uploaded (4 MB each) */

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

/* the group being built / last built */
static part_t parts[MAXPARTS];
static int nparts, group_frame = -1, group_bad;
static unsigned char comp[BH][BW];
static int analysed, is_face, male, cap, bald, eyes, longhair, eye_dark;
static unsigned long long body_hash;
static int ramp = -1, pick = -1;

/* the pool */
static portrait_t pool[MAXPOOL];
static int npool = -1;

/* what hdface_draw draws this frame */
static int shown = -1;
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

static int score (const portrait_t *p)
{
	int s = 0;
	if (p->male == male) s += 1000;
	if (p->cap == cap) s += 100;
	if (p->bald == bald) s += 100;
	if (p->eyes == eyes) s += 50;
	if (cap || bald) {
		/* no hair to see: the ramp is the cap's or the clothes' colour */
		if (ramp_group[p->ramp] == ramp_group[ramp]) s += 4;
	} else {
		if (p->ramp == ramp) s += 4;
		else if (ramp_group[p->ramp] == ramp_group[ramp]) s += 2;
		/* the game's men all wear their hair short: a portrait with it
		 * to the collar is as good a match */
		if (!male && p->longhair == longhair) s += 1;
	}
	return s;
}

static int choose (unsigned long long id)
{
	int i, best = -1, n = 0, k;
	for (i = 0; i < npool; i++) {
		int s;
		if (pool[i].bad) continue;
		s = score (&pool[i]);
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
		if (!pool[i].bad && score (&pool[i]) == best && k-- == 0) return i;
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

static void upload (portrait_t *p)
{
	int i, n = 0, old = -1;
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
	glGenTextures (1, &p->tex);
	glBindTexture (GL_TEXTURE_2D, p->tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, p->tw, p->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, p->w, p->h, GL_RGBA, GL_UNSIGNED_BYTE, p->pix);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free (p->pix);
	p->pix = NULL;
}

/* ---- collecting the parts ---- */

void hdface_blit (const unsigned char *planes, int width_words, int height,
		  int x, int y, unsigned int bmp, unsigned int scr)
{
	int w = width_words * 16, plane = 2 * height * width_words, i, j, k, used = 0, dk = 0, n = 0;
	part_t *pt;

	(void) bmp;
	(void) scr;
	if (pixel_faces) return;
	log_init ();
	/* only whole parts inside the face box */
	if (width_words <= 0 || height <= 0) return;
	if (x < BX0 || y < BY0 || x + w > BX1 || y + height > BY1) return;
	/* a new group: the first part after a frame was shown, or a part
	 * landing where one of this group already did (a redraw) */
	if (group_frame != frame_no) nparts = 0;
	for (i = 0; i < nparts; i++)
		if (parts[i].x == x && parts[i].y == y && parts[i].w == w && parts[i].h == height) {
			nparts = 0;
			break;
		}
	if (nparts == 0) {
		memset (comp, 0, sizeof comp);
		group_bad = 0;
		group_frame = frame_no;
	}
	analysed = 0;
	ramp = -1;
	pick = -1;
	if (nparts == MAXPARTS) { group_bad = 1; return; }
	for (j = 0; j < height; j++)
		for (i = 0; i < w; i++) {
			int wo = (j * width_words + i / 16) * 2, b = 15 - (i & 15), c = 0;
			for (k = 0; k < 4; k++) {
				int v = (planes[k * plane + wo] << 8) | planes[k * plane + wo + 1];
				c |= ((v >> b) & 1) << k;
			}
			comp[y + j - BY0][x + i - BX0] = (unsigned char) c;
			used |= 1 << c;
			/* the eyes themselves, below the brows */
			if (j >= 2 && j < 13 && i >= 6 && i < 58) {
				n++;
				if (c <= 4) dk++;
			}
		}
	pt = &parts[nparts++];
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
static void analyse (void)
{
	int i, ei = -1, cs, vm = 0, vf = 0;
	unsigned long long h = 0xcbf29ce484222325ULL;

	analysed = 1;
	is_face = 0;
	if (group_bad || nparts < 7) return;
	for (i = 0; i < nparts; i++) {
		const part_t *p = &parts[i];
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
	body_hash = h;
	male = vm > vf;
	cs = ei - 6;	/* where the seven start */
	/* a part over the top of the head drawn after the eyes: a cap */
	cap = 0;
	for (i = ei + 1; i < nparts; i++)
		if (parts[i].y < 40 && parts[i].h >= 20) cap = 1;
	/* one part before the seven, all skin: a bare crown */
	bald = !cap && cs == 1 && !(parts[0].cols & 0x1e);
	/* hair wider than the head falls past it */
	longhair = 0;
	if (!cap && !bald)
		for (i = 0; i < cs; i++)
			if (parts[i].x < 16 || parts[i].x + parts[i].w > 100) longhair = 1;
	/* glasses make the strip taller; sunglasses black out the eyes
	 * (the brows are in the hair colours, so only the eyes count) */
	eye_dark = parts[ei].dark;
	eyes = parts[ei].h >= 21 ? EYES_GLASSES : eye_dark >= 750 ? EYES_SHADES : EYES_NONE;
	is_face = 1;
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

void hdface_frame (const unsigned char *raster, unsigned char *out,
		   const unsigned int *pal, const unsigned int *ctrl_pal)
{
	int y, r;
	float s;

	(void) ctrl_pal;
	frame_no++;
	shown = -1;
	if (pixel_faces || nparts == 0) return;
	log_init ();
	if (!analysed) analyse ();
	if (!is_face) return;
	/* the skin ramp is fixed: anything but it (or it darkened) is not
	 * the face's palette */
	s = ramp_scale (pal, 5, skin, 10);
	if (s <= 0.02f) return;
	r = match_ramp (pal, s);
	if (r >= 0 && r != ramp) {
		ramp = r;
		pick = -1;
	}
	if (ramp < 0) return;
	if (pick < 0) {
		unsigned long long id = fnv_int (body_hash, ramp);
		if (npool < 0) load_manifest ();
		pick = choose (id);
		if (logf_) {
			fprintf (logf_, "f%d face %016llx %s %s %s eyes=%s dark=%d %s -> %s\n",
				 frame_no, id, male ? "m" : "f", ramp_names[ramp],
				 cap ? "cap" : bald ? "bald" : "hair",
				 eyes == EYES_SHADES ? "shades" : eyes == EYES_GLASSES ? "glasses" : "none",
				 eye_dark, longhair ? "long" : "short",
				 pick >= 0 ? pool[pick].file : "-");
			fflush (logf_);
		}
		if (pick < 0) return;
	}
	/* only while the box shows exactly the face put together */
	for (y = BY0; y < BY1; y++)
		if (memcmp (raster + y * ST_W + BX0, comp[y - BY0], BW)) return;
	if (!ensure_pixels (pick)) {
		pick = choose (fnv_int (body_hash, ramp));
		return;
	}
	for (y = BY0; y < BY1; y++)
		memset (out + y * ST_W + BX0, 255, BW);
	shown = pick;
	shade = s > 1.0f ? 1.0f : s;
}

void hdface_draw (const unsigned int *pal, const unsigned int *ctrl_pal)
{
	portrait_t *p;
	float u, v;

	(void) pal;
	(void) ctrl_pal;
	if (pixel_faces || shown < 0) return;
	p = &pool[shown];
	if (!p->tex) {
		if (!p->pix) return;
		upload (p);
	}
	p->used = frame_no;
	u = p->w / (float) p->tw;
	v = p->h / (float) p->th;
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, p->tex);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f (shade, shade, shade, 1.0f);
	glBegin (GL_TRIANGLE_STRIP);
		glTexCoord2f (0, v);
		glVertex2i (BX0, ST_H - BY1);
		glTexCoord2f (u, v);
		glVertex2i (BX1, ST_H - BY1);
		glTexCoord2f (0, 0);
		glVertex2i (BX0, ST_H - BY0);
		glTexCoord2f (u, 0);
		glVertex2i (BX1, ST_H - BY0);
	glEnd ();
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub (255, 255, 255, 255);
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
}
