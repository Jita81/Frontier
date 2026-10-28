/*
 * HD text for the GL renderer (see hdtext.h).
 *
 * The 68k code puts its text on the screen with DrawStr, then moves,
 * clears, covers and double buffers that screen as it likes, and the GL
 * renderer only ever sees the finished 320x200 picture.  So nothing
 * recorded is trusted: a glyph is redrawn only while every one of its
 * pixels is still on the screen in its colour, and one that has been off
 * it for MISS_LIMIT frames is forgotten.  A glyph drawn over another one
 * replaces it.
 *
 * The glyphs found are joined into runs (words drawn one after another on
 * a line in one colour) and each run is typeset again in a role of the
 * design system (docs/hd/ui.md) in the bundled fonts (art/ui/fonts),
 * with a soft shadow, at its place or where hdui moves it.  Under
 * --classic-ui the old typesetting draws each glyph where it was.
 *
 * Nothing here writes ST memory, so the oracle sees what it always saw.
 */
#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <ApplicationServices/ApplicationServices.h>
#else
#include <GL/gl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hdtext.h"
#include "hdui.h"
#include "art.h"

#define ST_W		320
#define ST_H		200
#define CTRL_Y		168	/* the control panel's rows have their own palette */
#define NGLYPHS		113	/* font codes 0x20..0x90 */
#define MAX_ACTIVE	4096	/* glyph positions tracked at once */
#define MISS_LIMIT	8	/* frames a glyph may be off the screen */
#define DEFAULT_FONT	"DINAlternate-Bold"	/* FE2_HD_FONT overrides */
#define MAX_RUNS	1024	/* runs found in a frame */

/* Recorded at a glyph's top left pixel: two per position, for a place
 * that alternates between two glyphs (a highlighted menu item). */
struct ent {
	unsigned char chr, col, flags, misses, age;	/* age: frames since drawn */
	unsigned short name;	/* a map name's number (hdtext_queue); 0: other text */
};
#define F_USED		1
#define F_SHADOW	2
#define F_QUEUED	4	/* a 3D view name gl.c drew just now */

static struct ent slots[ST_W * ST_H][2];
static unsigned char listed[ST_W * ST_H];
static int active[MAX_ACTIVE], n_active;
int hdtext_map, hdtext_queue;
/* the ink of the names gl.c drew over the screen this frame */
static unsigned char over[ST_W * ST_H];

/* what the last hdtext_frame found on the screen */
static struct shown {
	short x, y;
	unsigned char chr, col, shadow;
	unsigned short name;
	float dx;		/* layout's shift, framebuffer pixels */
} shown[MAX_ACTIVE];
static int n_shown;

/* the glyph texture: NGLYPHS cells of cw x ch pixels, 16 to a row, each
 * covering the ST box from (-1, -1) to (9, 10) round its glyph */
static GLuint tex;
static int built_w, built_h, cw, ch, tw, th;
/* each glyph's ink width, ink left of its origin and advance in the
 * texture, framebuffer pixels */
static float ink_w[NGLYPHS], ink_l[NGLYPHS], adv[NGLYPHS];

static int ink (const unsigned char *g, int c, int row)
{
	return c >= 0 && c < 8 && row >= 0 && row < 8 && (g[row] & (0x80 >> c));
}

void hdtext_glyph (const unsigned char *font, int x, int y, int col, int chr,
		   int shadowed)
{
	const unsigned char *g = font + chr * 10;
	unsigned char flags = F_USED | (shadowed ? F_SHADOW : 0);
	struct ent *s;
	int o = y * ST_W + x, ox, oy, k;

	if (pixel_text || x < 0 || x >= ST_W || y < 0 || y > ST_H - 8 ||
	    chr < 0 || chr >= NGLYPHS || col == 255)
		return;
	/* a space draws nothing, so it covers nothing */
	if (!(g[0] | g[1] | g[2] | g[3] | g[4] | g[5] | g[6] | g[7])) return;

	if (hdtext_map && hdtext_queue) {
		/* a name on the map, drawn by gl.c just before the frame
		 * is checked: it is on the screen, and it replaces nothing,
		 * since every name is drawn again each frame */
		flags |= F_QUEUED;
		for (oy = 0; oy < 9 && y + oy < ST_H; oy++)
			for (ox = 0; ox < 9 && x + ox < ST_W; ox++)
				if (ink (g, ox, oy) || (shadowed && ink (g, ox - 1, oy - 1)))
					over[(y + oy) * ST_W + x + ox] = 1;
	} else for (oy = y - 7; oy <= y + 7; oy++) {
		if (oy < 0 || oy >= ST_H || (hdtext_map && abs (oy - y) > 2)) continue;
		for (ox = x - 8; ox <= x + 8; ox++) {
			if (ox < 0 || ox >= ST_W || (ox == x && oy == y)) continue;
			s = slots[oy * ST_W + ox];
			for (k = 0; k < 2; k++)
				if ((s[k].flags & F_USED) && ox < x + g[9] &&
				    x < ox + font[s[k].chr * 10 + 9])
					s[k].flags = 0;
		}
	}

	if (!listed[o]) {
		if (n_active >= MAX_ACTIVE) return;
		listed[o] = 1;
		active[n_active++] = o;
	}
	s = slots[o];
	if ((s[1].flags & F_USED) && s[1].chr == chr && s[1].col == col && s[1].flags == flags) {
		struct ent t = s[0];
		s[0] = s[1];
		s[1] = t;
	} else if (!((s[0].flags & F_USED) && s[0].chr == chr && s[0].col == col && s[0].flags == flags)) {
		s[1] = s[0];
		s[0].chr = chr;
		s[0].col = col;
		s[0].flags = flags;
	}
	s[0].misses = 0;
	s[0].age = 0;
	s[0].name = flags & F_QUEUED ? hdtext_queue : 0;
}

/* Every pixel of the glyph is in its colour, and something in its box is
 * not: a block of solid colour is not a glyph.  A pixel under a map name
 * gl.c drew this frame is explained.  lenient: it was drawn just now, so
 * up to half of it may be covered by what was drawn over it since (the
 * old renderer's grid and stalks), but not by the background: a glyph
 * whose pixels went back to the sky has moved, it is not covered. */
static int on_screen (const unsigned char *g, const unsigned char *r, int x, int y, int col,
		      int lenient)
{
	int row, c, pixels = 0, covered = 0, other = 0, bg = -1, n = 0;
	unsigned char count[256];

	if (lenient) {
		memset (count, 0, sizeof (count));
		for (row = 0; row < 8; row++)
			for (c = 0; c < 8 && x + c < ST_W; c++) {
				int p = r[(y + row) * ST_W + x + c];
				if (!ink (g, c, row) && p != col && ++count[p] > n) {
					n = count[p];
					bg = p;
				}
			}
	}
	for (row = 0; row < 8; row++) {
		for (c = 0; c < 8 && x + c < ST_W; c++) {
			int i = (y + row) * ST_W + x + c, p = r[i];
			if (ink (g, c, row)) {
				if (p == col) pixels++;
				else if (over[i]) continue;
				else if (!lenient || p == bg) return 0;
				else covered++;
			} else if (c < g[9] && p != col)
				other = 1;
		}
	}
	return pixels && other && covered <= pixels;
}

/* Paint the glyph (and its shadow) out of out with the colour behind it:
 * the commonest one round it that is not the glyph's own colour. */
static void erase (const unsigned char *g, const unsigned char *r, unsigned char *out,
		   int x, int y, int col, int shadow)
{
	int count[256], best = 255, n = 0, xx, yy;

	memset (count, 0, sizeof (count));
	for (yy = y - 1; yy <= y + 9; yy++) {
		for (xx = x - 1; xx <= x + g[9]; xx++) {
			int p;
			if (xx < 0 || xx >= ST_W || yy < 0 || yy >= ST_H) continue;
			if (ink (g, xx - x, yy - y) || (shadow && ink (g, xx - x - 1, yy - y - 1)))
				continue;
			p = r[yy * ST_W + xx];
			if (p != col && ++count[p] > n) {
				n = count[p];
				best = p;
			}
		}
	}
	for (yy = y; yy <= y + 8 && yy < ST_H; yy++) {
		for (xx = x; xx <= x + 8 && xx < ST_W; xx++) {
			int i = yy * ST_W + xx;
			if (ink (g, xx - x, yy - y) ||
			    (shadow && r[i] == 0 && ink (g, xx - x - 1, yy - y - 1)))
				out[i] = best;
		}
	}
}


static void build_runs (const unsigned char *font);

/* FE2_HDTEXT_DEBUG=1: say on stderr why a glyph that was being redrawn
 * is not any more (the first pixel that stopped matching), the frame it
 * goes, so a glyph left in pixels can be traced */
static void why_missed (const unsigned char *g, const unsigned char *r, int x, int y, int col, int chr)
{
	static int checked, on;
	int row, c, other = 0;

	if (!checked) {
		checked = 1;
		on = getenv ("FE2_HDTEXT_DEBUG") != 0;
	}
	if (!on) return;
	for (row = 0; row < 8; row++)
		for (c = 0; c < 8 && x + c < ST_W; c++) {
			int p = r[(y + row) * ST_W + x + c];
			if (ink (g, c, row) && p != col) {
				fprintf (stderr, "HDTEXT miss %d,%d chr=%02x col=%d: ink at %d,%d is %d\n",
					 x, y, chr + 0x20, col, x + c, y + row, p);
				return;
			}
			if (!ink (g, c, row) && c < g[9] && p != col) other = 1;
		}
	fprintf (stderr, "HDTEXT miss %d,%d chr=%02x col=%d: %s\n", x, y, chr + 0x20, col,
		 other ? "?" : "solid block");
}

/* note a glyph that is on the screen and take its pixels out */
static void show (const unsigned char *font, const unsigned char *r, unsigned char *out,
		  int x, int y, const struct ent *e)
{
	struct shown *d = &shown[n_shown];

	if (n_shown >= MAX_ACTIVE) return;
	n_shown++;
	d->x = x;
	d->y = y;
	d->chr = e->chr;
	d->col = e->col;
	d->shadow = (e->flags & F_SHADOW) != 0;
	d->name = e->name;
	erase (font + d->chr * 10, r, out, x, y, d->col, d->shadow);
}

void hdtext_frame (const unsigned char *font, const unsigned char *r, unsigned char *out)
{
	int i, k;

	memcpy (out, r, ST_W * ST_H);
	n_shown = 0;
	for (i = 0; i < n_active; ) {
		int o = active[i], x = o % ST_W, y = o / ST_W, hit = -1;
		struct ent *s = slots[o];

		for (k = 0; k < 2; k++) {
			int lenient = hdtext_map && s[k].age < 2;
			if (!(s[k].flags & F_USED)) continue;
			if (s[k].age < 255) s[k].age++;
			if (s[k].flags & F_QUEUED) {
				/* drawn this frame (two names can put a glyph
				 * in one place: both are shown), or gone */
				if (s[k].age != 1) s[k].flags = 0;
				else if (hit < 0) hit = k;
				else show (font, r, out, x, y, &s[k]);
				continue;
			}
			if (hit < 0 && on_screen (font + s[k].chr * 10, r, x, y, s[k].col, lenient)) {
				hit = k;
				s[k].misses = 0;
			} else if (++s[k].misses > MISS_LIMIT)
				s[k].flags = 0;
		}
		if (hit < 0 && (s[0].flags & F_USED) && s[0].misses == 1)
			why_missed (font + s[0].chr * 10, r, x, y, s[0].col, s[0].chr);
		if (hit >= 0) show (font, r, out, x, y, &s[hit]);
		if (!(s[0].flags & F_USED) && !(s[1].flags & F_USED)) {
			listed[o] = 0;
			active[i] = active[--n_active];
			continue;
		}
		i++;
	}
	memset (over, 0, sizeof (over));
	build_runs (font);
}

/* The original glyph, smoothed: bilinear coverage of its pixels through a
 * smoothstep, which rounds the corners off.  For the icons, and for any
 * glyph with no letter to typeset. */
static void draw_bitmap (const unsigned char *g, unsigned char *cell, float sx, float sy)
{
	int px, py;

	for (py = 0; py < ch; py++) {
		for (px = 0; px < cw; px++) {
			float u = (px + 0.5f) / sx - 1.5f, v = (py + 0.5f) / sy - 1.5f;
			int u0 = (int) floorf (u), v0 = (int) floorf (v);
			float fu = u - u0, fv = v - v0, a;

			a = ink (g, u0, v0) * (1 - fu) * (1 - fv) + ink (g, u0 + 1, v0) * fu * (1 - fv) +
			    ink (g, u0, v0 + 1) * (1 - fu) * fv + ink (g, u0 + 1, v0 + 1) * fu * fv;
			a = a < 0.3f ? 0 : a > 0.7f ? 1 : (a - 0.3f) / 0.4f;
			a = a * a * (3 - 2 * a);
			cell[py * tw + px] = (unsigned char) (a * 255);
		}
	}
}

/* The glyph's ink columns in the original font */
static int ink_cols (const unsigned char *g, int *lo, int *hi)
{
	int c, bits = g[0] | g[1] | g[2] | g[3] | g[4] | g[5] | g[6] | g[7];

	*lo = 8;
	*hi = -1;
	for (c = 0; c < 8; c++)
		if (bits & (0x80 >> c)) {
			if (c < *lo) *lo = c;
			*hi = c;
		}
	return *hi >= 0;
}

#ifdef __APPLE__
/* The letter a font code stands for: ASCII, but for the accented ones the
 * font keeps on some punctuation codes.  0 for the icons (0x7b-0x81) and
 * the small hex digits (0x8b-0x90). */
static UniChar letter (int chr)
{
	static const UniChar accents[] = { 0xe7, 0xe9, 0xf9, 0xee, 0xe2, 0xf4, 0xea, 0xfb, 0xe0 };
	int c = chr + 0x20;

	switch (c) {
	case 0x26: return 0xf6;		/* o umlaut */
	case 0x3c: return 0xf1;		/* n tilde */
	case 0x3e: return 0xe8;		/* e grave */
	case 0x40: return 0xa9;		/* copyright */
	case 0x5b: return 0xdc;		/* U umlaut */
	case 0x5d: return 0xdf;		/* sharp s */
	case 0x5e: return 0xb0;		/* degree */
	case 0x60: return 0xe4;		/* a umlaut */
	}
	if (c >= 0x82 && c <= 0x8a) return accents[c - 0x82];
	return c > 0x20 && c < 0x7b ? c : 0;
}

static int cmp_float (const void *a, const void *b)
{
	float d = *(const float *) a - *(const float *) b;
	return d < 0 ? -1 : d > 0;
}

/* Typeset every letter into its cell: cap height 6.2 of the original's 7 rows,
 * baseline under its row 6, each centred on the original's ink and no
 * wider than it plus 0.6 of a pixel, so the game's own spacing holds. */
static void draw_type (const unsigned char *font, unsigned char *buf, float sx, float sy,
		       unsigned char *typed)
{
	const char *name = getenv ("FE2_HD_FONT");
	CFStringRef cfname;
	CTFontRef f0, f;
	CGContextRef ctx;
	CGGlyph glyph[NGLYPHS];
	CGRect bb[NGLYPHS];
	float fit[NGLYPHS], k;
	int c, n = 0, lo, hi;

	cfname = CFStringCreateWithCString (NULL, name ? name : DEFAULT_FONT, kCFStringEncodingUTF8);
	if (!cfname) return;
	f0 = CTFontCreateWithName (cfname, 100.0, NULL);
	CFRelease (cfname);
	if (!f0) return;
	f = CTFontCreateCopyWithAttributes (f0, 100.0 * 6.2 * sy / CTFontGetCapHeight (f0), NULL, NULL);
	CFRelease (f0);
	if (!f) return;
	ctx = CGBitmapContextCreate (buf, tw, th, 8, tw, NULL, kCGImageAlphaOnly);
	if (!ctx) {
		CFRelease (f);
		return;
	}
	CGContextSetShouldAntialias (ctx, true);
	CGContextSetShouldSmoothFonts (ctx, false);
	CGContextSetGrayFillColor (ctx, 1.0, 1.0);
	CGContextSetTextMatrix (ctx, CGAffineTransformIdentity);

	/* one squeeze for the whole font, so its letters keep their shapes
	 * next to each other: what the median letter needs */
	for (c = 0; c < NGLYPHS; c++) {
		UniChar u = letter (c);
		glyph[c] = 0;
		if (!u || !ink_cols (font + c * 10, &lo, &hi) ||
		    !CTFontGetGlyphsForCharacters (f, &u, &glyph[c], 1)) {
			glyph[c] = 0;
			continue;
		}
		CTFontGetBoundingRectsForGlyphs (f, kCTFontOrientationHorizontal, &glyph[c], &bb[c], 1);
		if (bb[c].size.width <= 0) {
			glyph[c] = 0;
			continue;
		}
		if ((c >= 0x10 && c <= 0x19) || (c >= 0x21 && c <= 0x3a) ||
		    (c >= 0x41 && c <= 0x5a))	/* 0-9 A-Z a-z */
			fit[n++] = (hi - lo + 1.6f) * sx / bb[c].size.width;
	}
	qsort (fit, n, sizeof (float), cmp_float);
	k = n ? fit[n / 2] : 1;
	if (k > 1) k = 1;

	for (c = 0; c < NGLYPHS; c++) {
		CGPoint zero = { 0, 0 };
		CGSize a;
		float kc = k, left = (c % 16) * cw, top = th - (c / 16) * ch;

		if (!glyph[c]) continue;
		ink_cols (font + c * 10, &lo, &hi);
		if (kc * bb[c].size.width > (hi - lo + 1.6f) * sx)
			kc = (hi - lo + 1.6f) * sx / bb[c].size.width;
		CGContextSaveGState (ctx);
		CGContextClipToRect (ctx, CGRectMake (left, top - ch, cw, ch));
		CGContextTranslateCTM (ctx, left + ((lo + hi + 1) / 2.0f + 1) * sx -
				       kc * (bb[c].origin.x + bb[c].size.width / 2), top - 8 * sy);
		CGContextScaleCTM (ctx, kc, 1);
		CTFontDrawGlyphs (f, &glyph[c], &zero, 1, ctx);
		CGContextRestoreGState (ctx);
		typed[c] = 1;
		ink_w[c] = kc * bb[c].size.width;
		ink_l[c] = kc * bb[c].origin.x;
		CTFontGetAdvancesForGlyphs (f, kCTFontOrientationHorizontal, &glyph[c], &a, 1);
		adv[c] = kc * a.width;
	}
	CGContextRelease (ctx);
	CFRelease (f);
}
#endif /* __APPLE__ */

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

static void legacy_forget (void)
{
	if (tex) glDeleteTextures (1, &tex);
	tex = 0;
	built_w = built_h = 0;
}

static void build (const unsigned char *font, int vw, int vh)
{
	float sx = vw / (float) ST_W, sy = vh / (float) ST_H;
	unsigned char typed[NGLYPHS], *buf;
	int c, lo, hi;

	legacy_forget ();
	cw = (int) ceilf (10 * sx);
	ch = (int) ceilf (11 * sy);
	tw = pow2 (16 * cw);
	th = pow2 (((NGLYPHS + 15) / 16) * ch);
	buf = calloc ((size_t) tw * th, 1);
	if (!buf) return;
	memset (typed, 0, sizeof (typed));
#ifdef __APPLE__
	draw_type (font, buf, sx, sy, typed);
#endif
	for (c = 0; c < NGLYPHS; c++)
		if (!typed[c]) {
			draw_bitmap (font + c * 10, buf + (c / 16) * ch * tw + (c % 16) * cw, sx, sy);
			ink_w[c] = ink_cols (font + c * 10, &lo, &hi) ? (hi - lo + 1) * sx : 0;
			ink_l[c] = lo * sx;
			adv[c] = font[c * 10 + 9] * sx;
		}

	glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA, tw, th, 0, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free (buf);
	built_w = vw;
	built_h = vh;
}

static int cmp_shown (const void *a, const void *b)
{
	const struct shown *p = a, *q = b;
	/* two map names on one line are two runs, not one mixed up */
	return p->y != q->y ? p->y - q->y : p->name != q->name ? p->name - q->name : p->x - q->x;
}

/* Set each word with the font's own spacing.  A word is a run of glyphs
 * the game drew one advance apart on one line in one colour: its first
 * and last letters keep the original's ink edges, so columns still line
 * up, and the tracking between takes up the difference.  (Centred one by
 * one on the original's letters, a narrow letter leaves a hole.) */
static void legacy_layout (const unsigned char *font, float sx)
{
	int i, j, k, lo, hi;

	qsort (shown, n_shown, sizeof (shown[0]), cmp_shown);
	for (i = 0; i < n_shown; i = j) {
		float left, right, track, pos;
		int a = shown[i].chr, z;

		for (j = i + 1; j < n_shown && shown[j].y == shown[i].y && shown[j].col == shown[i].col &&
			     shown[j].x == shown[j - 1].x + font[shown[j - 1].chr * 10 + 9]; j++)
			;
		for (k = i; k < j; k++) shown[k].dx = 0;
		if (j - i < 2) continue;
		z = shown[j - 1].chr;
		ink_cols (font + a * 10, &lo, &hi);
		left = (shown[i].x + lo) * sx;
		ink_cols (font + z * 10, &lo, &hi);
		right = (shown[j - 1].x + hi + 1) * sx;
		/* the word's natural width, first ink to last */
		for (pos = -ink_l[a], k = i; k < j - 1; k++) pos += adv[shown[k].chr];
		track = (right - left - (pos + ink_l[z] + ink_w[z])) / (j - i - 1);
		if (track < -0.3f * sx) track = -0.3f * sx;
		for (pos = left - ink_l[a], k = i; k < j; k++) {
			int c = shown[k].chr;
			/* the cell centres the ink on the original's; move it */
			ink_cols (font + c * 10, &lo, &hi);
			shown[k].dx = pos + ink_l[c] + ink_w[c] / 2 - (shown[k].x + (lo + hi + 1) / 2.0f) * sx;
			pos += adv[c] + track;
		}
	}
}

/* --classic-ui: the typesetting from before the redesign, glyph by glyph */
static void legacy_draw (const unsigned char *font, const unsigned int *pal, const unsigned int *ctrl_pal,
			 const GLint *vp)
{
	float sx, sy;
	int i, pass;

	if (!tex || vp[2] != built_w || vp[3] != built_h) build (font, vp[2], vp[3]);
	if (!tex) return;
	sx = vp[2] / (float) ST_W;
	sy = vp[3] / (float) ST_H;
	legacy_layout (font, sx);

	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, tex);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBegin (GL_QUADS);
	/* all the shadows first, then all the glyphs */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < n_shown; i++) {
			const struct shown *d = &shown[i];
			const unsigned int *p = d->y + 4 >= CTRL_Y && d->col < 16 ? ctrl_pal : pal;
			unsigned int rgba = p[pass ? d->col : 0];
			/* the cell's top left, on a whole framebuffer pixel */
			float px = floorf ((d->x - 1) * sx + d->dx + 0.5f), py = floorf ((d->y - 1) * sy + 0.5f);
			float s0 = (d->chr % 16) * cw / (float) tw, t0 = (d->chr / 16) * ch / (float) th;
			float s1 = s0 + cw / (float) tw, t1 = t0 + ch / (float) th;

			if (!pass) {
				if (!d->shadow) continue;
				px += floorf (sx * 0.5f + 0.5f);
				py += floorf (sy * 0.5f + 0.5f);
			}
			glColor4ub (rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, 255);
			glTexCoord2f (s0, t0);
			glVertex2f (px / sx, ST_H - py / sy);
			glTexCoord2f (s1, t0);
			glVertex2f ((px + cw) / sx, ST_H - py / sy);
			glTexCoord2f (s1, t1);
			glVertex2f ((px + cw) / sx, ST_H - (py + ch) / sy);
			glTexCoord2f (s0, t1);
			glVertex2f (px / sx, ST_H - (py + ch) / sy);
		}
	}
	glEnd ();
	glDisable (GL_BLEND);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub (255, 255, 255, 255);
}

/* ---------------------------------------------------------------- */
/* Runs: what hdui styles and the new typesetting sets */

static struct hdtext_run runs[MAX_RUNS];
static int n_runs;
/* the alignment and anchor each run gets when hdui says nothing */
static unsigned char auto_align[MAX_RUNS];
static float auto_ax[MAX_RUNS];
static unsigned char run_name[MAX_RUNS];	/* a map name (see declutter) */
static const unsigned char *runs_font;

/* 0x7b and 0x7c are units, a letter with a superscript minus one: they
 * belong to the run they follow and are typeset with it (unit_letter) */
static int is_icon (int chr)
{
	int c = chr + 0x20;
	return (c >= 0x7d && c <= 0x81) || c >= 0x8b;
}

static int cmp_run_x0 (const void *a, const void *b)
{
	const struct hdtext_run *p = *(struct hdtext_run * const *) a, *q = *(struct hdtext_run * const *) b;
	return p->x0 != q->x0 ? p->x0 - q->x0 : p->y - q->y;
}

/* run r is glyphs i..j-1 of the frame's list: what it is and holds */
static void fill_run (struct hdtext_run *r, const unsigned char *font, int i, int j)
{
	int k, lo, hi, letters = 0, upper = 0, lower = 0, digits = 0, numeric = 0;
	unsigned int h = 2166136261u;

	memset (r, 0, sizeof (*r));
	r->x0 = shown[i].x;
	r->x1 = shown[j - 1].x + font[shown[j - 1].chr * 10 + 9];
	r->y = shown[i].y;
	ink_cols (font + shown[i].chr * 10, &lo, &hi);
	r->ink0 = shown[i].x + (hi >= 0 ? lo : 0);
	ink_cols (font + shown[j - 1].chr * 10, &lo, &hi);
	r->ink1 = shown[j - 1].x + (hi >= 0 ? hi + 1 : 0);
	r->first = i;
	r->n = j - i;
	r->col = shown[i].col;
	r->shadow = shown[i].shadow;
	r->chr0 = shown[i].chr;
	for (k = i; k < j; k++) {
		int c = shown[k].chr + 0x20;
		if (c >= '0' && c <= '9') digits++;
		else if (c >= 'A' && c <= 'Z') upper++;
		else if (c >= 'a' && c <= 'z') lower++;
		else if (c == '$' || c == '.' || c == ',' || c == '-' || c == '%' || c == '+' || c == ':' || c == '/')
			numeric++;
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			h = (h ^ (unsigned char) (c >= 'A' && c <= 'Z' ? c + 32 : c)) * 16777619u;
			letters++;
		}
	}
	r->hash = h;
	r->letters = letters;
	if (digits) r->kind |= HDT_K_DIGIT;
	if (upper + lower) r->kind |= HDT_K_LETTER;
	if (digits && upper + lower <= 2 && digits >= 2 * (upper + lower)) r->kind |= HDT_K_NUMERIC;
	if (upper >= 2 && !lower) r->kind |= HDT_K_CAPS;
	if (shown[j - 1].chr + 0x20 == ':') r->kind |= HDT_K_COLON;
	if (is_icon (shown[i].chr)) r->kind |= HDT_K_ICON;
}

/* Glyphs on one line in one colour, each starting where the one before
 * ended or a word space on, are a run; an icon is a run of its own. */
static void build_runs (const unsigned char *font)
{
	static struct hdtext_run *byx[MAX_RUNS];
	int space = font[9] > 0 ? font[9] : 4;
	int i, j, k;

	runs_font = font;
	n_runs = 0;
	qsort (shown, n_shown, sizeof (shown[0]), cmp_shown);
	for (i = 0; i < n_shown && n_runs < MAX_RUNS; i = j) {
		j = i + 1;
		if (!is_icon (shown[i].chr))
			while (j < n_shown && shown[j].y == shown[i].y && shown[j].col == shown[i].col &&
			       shown[j].name == shown[i].name &&
			       shown[j].shadow == shown[i].shadow && !is_icon (shown[j].chr) &&
			       shown[j].x >= shown[j - 1].x + font[shown[j - 1].chr * 10 + 9] &&
			       shown[j].x <= shown[j - 1].x + font[shown[j - 1].chr * 10 + 9] + space + 1)
				j++;
		fill_run (&runs[n_runs], font, i, j);
		run_name[n_runs] = shown[i].name != 0;
		auto_align[n_runs] = HDT_LEFT;
		auto_ax[n_runs] = runs[n_runs].ink0;
		n_runs++;
	}

	/* a text centred on the screen stays centred */
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		float c = (r->ink0 + r->ink1) / 2.0f;
		if (fabsf (c - ST_W / 2.0f) <= 1.0f && r->ink1 - r->ink0 >= 24) {
			auto_align[i] = HDT_CENTRE;
			auto_ax[i] = c;
		}
	}

	/* a column of three or more numbers left aligned at one x is set
	 * right aligned at the column's right, as tables are */
	for (i = 0; i < n_runs; i++) byx[i] = &runs[i];
	qsort (byx, n_runs, sizeof (byx[0]), cmp_run_x0);
	for (i = 0; i < n_runs; i = j) {
		int count = 0, right = 0;
		j = i + 1;
		while (j < n_runs && byx[j]->x0 == byx[i]->x0) j++;
		for (k = i; k < j; k++)
			if ((byx[k]->kind & HDT_K_NUMERIC) && byx[k]->y + 8 < CTRL_Y) {
				count++;
				if (byx[k]->ink1 > right) right = byx[k]->ink1;
			}
		if (count < 3) continue;
		/* not if that would run a number into what follows it */
		for (k = i; k < j && count; k++) {
			int m;
			if (!(byx[k]->kind & HDT_K_NUMERIC) || byx[k]->y + 8 >= CTRL_Y) continue;
			for (m = 0; m < n_runs; m++)
				if (runs[m].y == byx[k]->y && runs[m].ink0 > byx[k]->ink1 && runs[m].ink0 < right + 4)
					count = 0;
		}
		if (!count) continue;
		for (k = i; k < j; k++)
			if ((byx[k]->kind & HDT_K_NUMERIC) && byx[k]->y + 8 < CTRL_Y) {
				int n = byx[k] - runs;
				auto_align[n] = HDT_RIGHT;
				auto_ax[n] = right;
			}
	}
}

int hdtext_runs (struct hdtext_run **r)
{
	*r = runs;
	return n_runs;
}

int hdtext_chars (const struct hdtext_run *r, char *c, unsigned char *sp, int max)
{
	const unsigned char *font = runs_font;
	int space = font && font[9] > 0 ? font[9] : 4;
	int k, n = 0;

	for (k = r->first; k < r->first + r->n && n < max; k++, n++) {
		int ch = shown[k].chr + 0x20;
		c[n] = ch > 0x20 && ch < 0x7f ? ch : '?';
		sp[n] = 0;
		if (k > r->first && font) {
			int gap = shown[k].x - (shown[k - 1].x + font[shown[k - 1].chr * 10 + 9]);
			sp[n] = gap >= space - 1 && gap > 1;
		}
	}
	return n;
}

int hdtext_glyph_x (const struct hdtext_run *r, int k)
{
	return shown[r->first + k].x;
}

int hdtext_split (int i, int k)
{
	int first, n, tail;

	if (i < 0 || i >= n_runs || n_runs >= MAX_RUNS || !runs_font) return 0;
	first = runs[i].first;
	n = runs[i].n;
	if (k < 1 || k >= n) return 0;
	tail = n_runs - i - 1;
	memmove (&runs[i + 2], &runs[i + 1], tail * sizeof (runs[0]));
	memmove (&auto_align[i + 2], &auto_align[i + 1], tail * sizeof (auto_align[0]));
	memmove (&auto_ax[i + 2], &auto_ax[i + 1], tail * sizeof (auto_ax[0]));
	n_runs++;
	fill_run (&runs[i], runs_font, first, first + k);
	fill_run (&runs[i + 1], runs_font, first + k, first + n);
	auto_align[i] = auto_align[i + 1] = HDT_LEFT;
	auto_ax[i] = runs[i].ink0;
	auto_ax[i + 1] = runs[i + 1].ink0;
	return 1;
}

/* ---------------------------------------------------------------- */
/* The new typesetting: the design system's roles in the bundled fonts */

#ifdef __APPLE__

enum { F_TITLE, F_COND, F_SANS, F_SANS_BOLD, NFACE };

static const char *const face_file[NFACE] = {
	"ui/fonts/Orbitron-Regular.ttf",
	"ui/fonts/Oswald-Regular.ttf",
	"ui/fonts/OpenSans-Regular.ttf",
	"ui/fonts/OpenSans-Bold.ttf",
};

struct role {
	unsigned char face;
	float cap;		/* cap height, ST pixels */
	float min_px;		/* and never under this many framebuffer pixels */
	float track;		/* letter spacing, cap heights */
	unsigned char upper;	/* in capitals */
	unsigned char tabular;	/* digits one width */
	float shadow;		/* its soft shadow's alpha */
	float drop;		/* and offset, cap heights */
};

static const struct role role_tab[HDT_NROLES] = {
	[HDT_AUTO]    = { F_COND,      3.9f, 8.0f, 0.03f, 0, 1, 0.85f, 0.05f },
	[HDT_TITLE]   = { F_TITLE,     4.4f, 9.0f, 0.10f, 0, 0, 0.55f, 0.08f },
	[HDT_SECTION] = { F_COND,      3.3f, 7.0f, 0.16f, 1, 0, 0.45f, 0.08f },
	[HDT_LABEL]   = { F_COND,      4.3f, 8.0f, 0.03f, 0, 1, 0.50f, 0.07f },
	[HDT_BODY]    = { F_SANS,      3.5f, 7.0f, 0.00f, 0, 1, 0.50f, 0.07f },
	[HDT_DESC]    = { F_SANS,      3.0f, 6.5f, 0.00f, 0, 1, 0.45f, 0.07f },
	[HDT_VALUE]   = { F_COND,      4.1f, 8.0f, 0.04f, 0, 1, 0.50f, 0.07f },
	[HDT_PANEL]   = { F_COND,      4.3f, 8.0f, 0.03f, 0, 1, 0.70f, 0.06f },
	[HDT_MAP]     = { F_COND,      3.9f, 8.0f, 0.03f, 0, 1, 0.90f, 0.05f },
	[HDT_STRONG]  = { F_SANS_BOLD, 3.5f, 7.0f, 0.00f, 0, 1, 0.50f, 0.07f },
};

/* the characters the fonts are asked for: printable ASCII, the font's
 * accented letters and their capitals, and the minus sign */
static const UniChar extra_chars[] = {
	0xf6, 0xf1, 0xe8, 0xa9, 0xdc, 0xdf, 0xb0, 0xe4, 0xe7, 0xe9, 0xf9, 0xee, 0xe2, 0xf4, 0xea, 0xfb, 0xe0,
	0xd6, 0xd1, 0xc8, 0xc4, 0xc7, 0xc9, 0xd9, 0xce, 0xc2, 0xd4, 0xca, 0xdb, 0xc0,
	0x2212,		/* minus, for a unit's superscript */
};
#define N_ASCII		94	/* 0x21..0x7e */
#define NCH		(N_ASCII + (int) (sizeof (extra_chars) / sizeof (extra_chars[0])))

static int slot_of (UniChar u)
{
	int i;
	if (u >= 0x21 && u <= 0x7e) return u - 0x21;
	for (i = 0; i < NCH - N_ASCII; i++)
		if (extra_chars[i] == u) return N_ASCII + i;
	return -1;
}

static UniChar char_of (int slot)
{
	return slot < N_ASCII ? 0x21 + slot : extra_chars[slot - N_ASCII];
}

static UniChar upper_of (UniChar u)
{
	if (u >= 'a' && u <= 'z') return u - 32;
	if (u >= 0xe0 && u <= 0xfe && u != 0xf7) return u - 32;
	return u;
}

/* A face's metrics with a cap height of 100 pixels */
struct face {
	int tried;
	CTFontDescriptorRef desc;
	float size100;			/* the point size for that */
	float adv[NCH], lsb[NCH], rink[NCH];	/* advance, ink left and right of the pen */
	unsigned char has[NCH];
	float space, digit;		/* a space's advance, the widest digit's */
};
static struct face faces[NFACE];

static CTFontRef face_font (int f, float size)
{
	if (faces[f].desc) return CTFontCreateWithFontDescriptor (faces[f].desc, size, NULL);
	return CTFontCreateWithName (CFSTR (DEFAULT_FONT), size, NULL);
}

static struct face *face_load (int f)
{
	struct face *fc = &faces[f];
	CTFontRef font;
	int s;

	if (fc->tried) return fc->size100 > 0 ? fc : NULL;
	fc->tried = 1;
	{
		const char *path = art_path (face_file[f]);
		CFURLRef url = CFURLCreateFromFileSystemRepresentation (NULL, (const UInt8 *) path,
								       strlen (path), false);
		if (url) {
			CFArrayRef a = CTFontManagerCreateFontDescriptorsFromURL (url);
			if (a && CFArrayGetCount (a) > 0) {
				fc->desc = CFArrayGetValueAtIndex (a, 0);
				CFRetain (fc->desc);
			}
			if (a) CFRelease (a);
			CFRelease (url);
		}
		if (!fc->desc)
			fprintf (stderr, "hdtext: no %s, using %s\n", path, DEFAULT_FONT);
	}
	font = face_font (f, 100);
	if (!font) return NULL;
	if (CTFontGetCapHeight (font) <= 0) {
		CFRelease (font);
		return NULL;
	}
	fc->size100 = 100.0f * 100.0f / (float) CTFontGetCapHeight (font);
	CFRelease (font);
	font = face_font (f, fc->size100);
	if (!font) {
		fc->size100 = 0;
		return NULL;
	}
	for (s = 0; s < NCH; s++) {
		UniChar u = char_of (s);
		CGGlyph g = 0;
		CGSize a;
		CGRect bb;
		if (!CTFontGetGlyphsForCharacters (font, &u, &g, 1) || !g) continue;
		CTFontGetAdvancesForGlyphs (font, kCTFontOrientationHorizontal, &g, &a, 1);
		CTFontGetBoundingRectsForGlyphs (font, kCTFontOrientationHorizontal, &g, &bb, 1);
		fc->has[s] = 1;
		fc->adv[s] = a.width;
		fc->lsb[s] = bb.origin.x;
		fc->rink[s] = bb.origin.x + bb.size.width;
		if (u >= '0' && u <= '9' && a.width > fc->digit) fc->digit = a.width;
	}
	{
		UniChar u = ' ';
		CGGlyph g = 0;
		CGSize a = { 30, 0 };
		if (CTFontGetGlyphsForCharacters (font, &u, &g, 1))
			CTFontGetAdvancesForGlyphs (font, kCTFontOrientationHorizontal, &g, &a, 1);
		fc->space = a.width;
	}
	CFRelease (font);
	return fc;
}

/* A face at one size: its glyphs in a texture, and the same blurred for
 * their soft shadows */
struct atlas {
	int face, key;			/* key: cap height in quarter pixels */
	GLuint tex, blur;
	unsigned used;
	struct {
		short x, y, w, h;	/* the cell in the texture, pixels */
		short left, top;	/* the cell's left of the pen, top above the baseline */
	} g[NCH];
	int tw, th;
};
#define MAX_ATLAS	32
static struct atlas atl[MAX_ATLAS];
static int n_atl;
static unsigned use_clock, frame_clock;

static void box_blur (unsigned char *a, int w, int h, int r)
{
	int *acc = malloc (sizeof (int) * (size_t) (w > h ? w : h));
	unsigned char *line = malloc ((size_t) (w > h ? w : h));
	int x, y, i, pass;

	if (!acc || !line || r < 1) {
		free (acc);
		free (line);
		return;
	}
	for (pass = 0; pass < 3; pass++) {
		for (y = 0; y < h; y++) {
			unsigned char *p = a + (size_t) y * w;
			int sum = 0;
			memcpy (line, p, w);
			for (i = -r; i <= r; i++) sum += i >= 0 && i < w ? line[i] : 0;
			for (x = 0; x < w; x++) {
				p[x] = sum / (2 * r + 1);
				sum += (x + r + 1 < w ? line[x + r + 1] : 0) - (x - r >= 0 ? line[x - r] : 0);
			}
		}
		for (x = 0; x < w; x++) {
			int sum = 0;
			for (y = 0; y < h; y++) line[y] = a[(size_t) y * w + x];
			for (i = -r; i <= r; i++) sum += i >= 0 && i < h ? line[i] : 0;
			for (y = 0; y < h; y++) {
				a[(size_t) y * w + x] = sum / (2 * r + 1);
				sum += (y + r + 1 < h ? line[y + r + 1] : 0) - (y - r >= 0 ? line[y - r] : 0);
			}
		}
	}
	free (acc);
	free (line);
}

static int blur_radius (float cap)
{
	int r = (int) (cap * 0.07f + 0.5f);
	return r < 1 ? 1 : r;
}

static GLuint alpha_texture (const unsigned char *buf, int w, int h)
{
	GLuint t;

	glGenTextures (1, &t);
	glBindTexture (GL_TEXTURE_2D, t);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

static void atlas_free (struct atlas *a)
{
	if (a->tex) glDeleteTextures (1, &a->tex);
	if (a->blur) glDeleteTextures (1, &a->blur);
	a->tex = a->blur = 0;
}

static struct atlas *atlas_get (int f, int key)
{
	struct face *fc = face_load (f);
	struct atlas *a = NULL;
	float cap = key / 4.0f;
	CTFontRef font;
	CGGlyph glyph[NCH];
	CGRect bb[NCH];
	CGContextRef ctx;
	unsigned char *buf;
	int i, s, pad, x, y, rowh, area = 0, ok;

	for (i = 0; i < n_atl; i++)
		if (atl[i].face == f && atl[i].key == key) {
			atl[i].used = ++use_clock;
			return atl[i].tex ? &atl[i] : NULL;
		}
	if (!fc) return NULL;
	if (n_atl < MAX_ATLAS)
		a = &atl[n_atl++];
	else {
		for (a = &atl[0], i = 1; i < n_atl; i++)
			if (atl[i].used < a->used) a = &atl[i];
		/* never one this frame has placed glyphs from */
		if (a->used > frame_clock) return NULL;
		atlas_free (a);
	}
	memset (a, 0, sizeof (*a));
	a->face = f;
	a->key = key;
	a->used = ++use_clock;

	font = face_font (f, fc->size100 * cap / 100.0f);
	if (!font) return NULL;
	pad = 3 * blur_radius (cap) + 2;
	for (s = 0; s < NCH; s++) {
		UniChar u = char_of (s);
		glyph[s] = 0;
		bb[s] = CGRectZero;
		if (fc->has[s] && CTFontGetGlyphsForCharacters (font, &u, &glyph[s], 1))
			CTFontGetBoundingRectsForGlyphs (font, kCTFontOrientationHorizontal, &glyph[s], &bb[s], 1);
		if (glyph[s]) {
			a->g[s].left = (short) floorf (bb[s].origin.x) - pad;
			a->g[s].w = (short) (ceilf (bb[s].origin.x + bb[s].size.width) - floorf (bb[s].origin.x)) + 2 * pad;
			a->g[s].top = (short) ceilf (bb[s].origin.y + bb[s].size.height) + pad;
			a->g[s].h = (short) (ceilf (bb[s].origin.y + bb[s].size.height) - floorf (bb[s].origin.y)) + 2 * pad;
			area += a->g[s].w * a->g[s].h;
		}
	}
	a->tw = pow2 ((int) sqrtf (area * 1.4f) + 1);
	if (a->tw < 64) a->tw = 64;
	for (x = y = rowh = 0, s = 0; s < NCH; s++) {
		if (!glyph[s]) continue;
		if (x + a->g[s].w > a->tw) {
			x = 0;
			y += rowh;
			rowh = 0;
		}
		a->g[s].x = x;
		a->g[s].y = y;
		x += a->g[s].w;
		if (a->g[s].h > rowh) rowh = a->g[s].h;
	}
	a->th = pow2 (y + rowh);
	buf = calloc ((size_t) a->tw * a->th, 2);
	if (!buf) {
		CFRelease (font);
		return NULL;
	}
	ctx = CGBitmapContextCreate (buf, a->tw, a->th, 8, a->tw, NULL, kCGImageAlphaOnly);
	ok = ctx != NULL;
	if (ctx) {
		CGContextSetShouldAntialias (ctx, true);
		CGContextSetShouldSmoothFonts (ctx, false);
		CGContextSetGrayFillColor (ctx, 1.0, 1.0);
		CGContextSetTextMatrix (ctx, CGAffineTransformIdentity);
		for (s = 0; s < NCH; s++) {
			CGPoint pen;
			if (!glyph[s]) continue;
			/* the bitmap's first row is its top; CG counts up from the bottom */
			pen.x = a->g[s].x - a->g[s].left;
			pen.y = a->th - (a->g[s].y + a->g[s].top);
			CTFontDrawGlyphs (font, &glyph[s], &pen, 1, ctx);
		}
		CGContextRelease (ctx);
	}
	CFRelease (font);
	if (ok) {
		unsigned char *blurred = buf + (size_t) a->tw * a->th;
		memcpy (blurred, buf, (size_t) a->tw * a->th);
		box_blur (blurred, a->tw, a->th, blur_radius (cap));
		a->tex = alpha_texture (buf, a->tw, a->th);
		a->blur = alpha_texture (blurred, a->tw, a->th);
	}
	free (buf);
	return a->tex ? a : NULL;
}

/* the size the last frame was drawn at, framebuffer pixels to an ST pixel */
static float last_sx = 5, last_sy = 6;

/* The superscript of a unit (per hour, per second): a minus sign and a
 * one at SUP of the run's size, their baseline SUP_RISE cap heights up */
#define SUP_MINUS	0xe000
#define SUP_ONE		0xe001
#define SUP		0.62f
#define SUP_RISE	0.52f

static UniChar unit_letter (int chr)
{
	int c = chr + 0x20;
	return c == 0x7b ? 'h' : c == 0x7c ? 's' : 0;
}

/* the slot a character is set from, and its size */
static int set_slot (UniChar u, float *z)
{
	*z = 1;
	if (u == SUP_MINUS || u == SUP_ONE) {
		*z = SUP;
		return slot_of (u == SUP_MINUS ? 0x2212 : '1');
	}
	return slot_of (u);
}

/* A run as the characters to set: the game's glyphs as letters, a word
 * space where it left a gap.  Returns how many; src is the glyph each
 * came from (-1 for a space). */
#define MAX_CHARS	256
static int run_chars (const struct hdtext_run *r, int upper, UniChar *u, short *src)
{
	const unsigned char *font = runs_font;
	int space = font && font[9] > 0 ? font[9] : 4;
	int k, j, n = 0, quote = -1;

	for (k = r->first; k < r->first + r->n && n < MAX_CHARS - 4; k++) {
		UniChar c = letter (shown[k].chr), unit = unit_letter (shown[k].chr);
		if (k > r->first && font) {
			int gap = shown[k].x - (shown[k - 1].x + font[shown[k - 1].chr * 10 + 9]);
			if (gap >= space - 1 && gap > 1) {
				u[n] = ' ';
				src[n++] = -1;
			}
		}
		/* on the map, a letter or two quoted between words with no
		 * gap (a star's class letter in the info line): the pixel
		 * quotes are wide enough to read as gaps, set ones are not,
		 * so space them */
		if (hdtext_map && c == '\'' && k != quote && n && u[n - 1] != ' ')
			for (j = k + 2; j <= k + 3 && j < r->first + r->n; j++)
				if (letter (shown[j].chr) == '\'') {
					quote = j;
					u[n] = ' ';
					src[n++] = -1;
					break;
				}
		if (k == quote + 1 && quote >= 0 && n && u[n - 1] != ' ') {
			u[n] = ' ';
			src[n++] = -1;
		}
		if (unit) {
			/* a unit keeps its small letter, even in capitals */
			u[n] = unit;
			src[n++] = k;
			u[n] = SUP_MINUS;
			src[n++] = k;
			u[n] = SUP_ONE;
			src[n++] = k;
			continue;
		}
		if (!c) continue;
		u[n] = upper ? upper_of (c) : c;
		src[n++] = k;
	}
	return n;
}

static int resolve_role (const struct hdtext_run *r)
{
	if (r->role && r->role < HDT_NROLES) return r->role;
	return r->y + 4 >= CTRL_Y ? HDT_PANEL : HDT_MAP;
}

/* The run's width set at cap height cap with spacing sq: ink left to ink
 * right, framebuffer pixels; *lsb is the first letter's ink left of the pen */
static float run_width (const struct face *fc, const struct role *ro, const UniChar *u, int n,
			float cap, float *lsb)
{
	float k = cap / 100.0f, pen = 0, right = 0;
	int i, first = 1;

	*lsb = 0;
	for (i = 0; i < n; i++) {
		float z, adv, off = 0;
		int s = set_slot (u[i], &z);
		if (u[i] == ' ' || s < 0 || !fc->has[s]) {
			pen += fc->space * k + ro->track * cap;
			continue;
		}
		adv = fc->adv[s];
		/* a tabular digit is centred in the widest digit's advance */
		if (ro->tabular && z == 1 && u[i] >= '0' && u[i] <= '9') {
			off = (fc->digit - adv) / 2;
			adv = fc->digit;
		}
		if (first) {
			*lsb = pen + (off + fc->lsb[s]) * k * z;
			first = 0;
		}
		right = pen + (off + fc->rink[s]) * k * z;
		pen += (adv * k + ro->track * cap) * z;
	}
	return right - *lsb;
}

static float role_cap (const struct role *ro, const struct hdtext_run *r, float sy)
{
	float scale = r->scale > 0 ? r->scale : 1, cap = ro->cap * sy * scale;
	return cap < ro->min_px * scale ? ro->min_px * scale : cap;
}

float hdtext_measure (const struct hdtext_run *r)
{
	const struct role *ro = &role_tab[resolve_role (r)];
	struct face *fc = face_load (ro->face);
	UniChar u[MAX_CHARS];
	short src[MAX_CHARS];
	float lsb;
	int n;

	if (!fc) return r->ink1 - r->ink0;
	n = run_chars (r, ro->upper || r->upper, u, src);
	return run_width (fc, ro, u, n, role_cap (ro, r, last_sy), &lsb) / last_sx;
}

/* a glyph to draw */
struct placed {
	struct atlas *a;
	short slot;
	float X, Y, w;		/* the cell's top left and width, framebuffer pixels */
	unsigned int rgb;
	float alpha, shadow, drop;
	float halo;		/* a dark ring this far round it (map text); 0: none */
};
#define MAX_PLACED	8192
static struct placed placed[MAX_PLACED];
static int n_placed;

/* The nearest run's edge on the same line: right of this one (dir 1) or
 * left (dir -1), ST pixels, or the screen's edge */
static float neighbour (int i, int dir)
{
	const struct hdtext_run *r = &runs[i];
	float edge = dir > 0 ? ST_W - 1 : 1;
	int k;

	for (k = 0; k < n_runs; k++) {
		const struct hdtext_run *q = &runs[k];
		if (k == i || q->hide || abs (q->y - r->y) > 4) continue;
		if (dir > 0 && q->ink0 >= r->ink1 && q->ink0 + q->dx < edge) edge = q->ink0 + q->dx;
		if (dir < 0 && q->ink1 <= r->ink0 && q->ink1 + q->dx > edge) edge = q->ink1 + q->dx;
	}
	return edge;
}

static void place_run (int i, float sx, float sy, const unsigned int *pal, const unsigned int *ctrl_pal)
{
	struct hdtext_run *r = &runs[i];
	int rl = resolve_role (r), align = r->align ? r->align : auto_align[i];
	const struct role *ro = &role_tab[rl];
	struct face *fc = face_load (ro->face);
	struct atlas *a, *sup = NULL;
	UniChar u[MAX_CHARS];
	short src[MAX_CHARS];
	float cap, w, lsb, avail, ax, left, pen, Y, sq = 1, k, alpha = r->alpha > 0 ? r->alpha : 1;
	unsigned int rgb;
	int n, step, c;

	if (!fc) return;
	n = run_chars (r, ro->upper || r->upper, u, src);
	if (!n) return;
	cap = role_cap (ro, r, sy);

	/* where it is anchored, and how much room it has */
	if (r->align) {
		ax = align == HDT_RIGHT ? r->ink1 : align == HDT_CENTRE ? (r->ink0 + r->ink1) / 2.0f : r->ink0;
		if (r->align == auto_align[i]) ax = auto_ax[i];
	} else
		ax = auto_ax[i];
	if (r->ax > 0) ax = r->ax;
	ax += r->dx;
	if (r->maxw > 0)
		avail = r->maxw;
	else if (align == HDT_RIGHT)
		avail = ax - neighbour (i, -1) - 1.5f;
	else if (align == HDT_CENTRE) {
		float lft = ax - neighbour (i, -1), rgt = neighbour (i, 1) - ax;
		avail = 2 * (lft < rgt ? lft : rgt) - 3;
	} else
		avail = neighbour (i, 1) - ax - 1.5f;
	if (avail < 4) avail = 4;
	avail *= sx;

	/* a size step or two down if it does not fit; then a little narrower */
	w = run_width (fc, ro, u, n, cap, &lsb);
	for (step = 0; w > avail && step < 3; step++) {
		cap *= 0.92f;
		w = run_width (fc, ro, u, n, cap, &lsb);
	}
	cap = floorf (cap * 4 + 0.5f) / 4;
	w = run_width (fc, ro, u, n, cap, &lsb);
	if (w > avail) {
		sq = avail / w;
		if (sq < 0.72f) sq = 0.72f;
	}
	a = atlas_get (ro->face, (int) (cap * 4 + 0.5f));
	if (!a) return;

	left = align == HDT_RIGHT ? ax * sx - w * sq : align == HDT_CENTRE ? ax * sx - w * sq / 2 : ax * sx;
	/* the capitals' middle where the original's was */
	Y = floorf ((r->y + 3.5f + r->dy) * sy + cap / 2 + 0.5f);
	if (r->rgb)
		rgb = r->rgb;
	else
		rgb = (r->y + 4 >= CTRL_Y && r->col < 16 ? ctrl_pal : pal)[r->col];
	k = cap / 100.0f;
	pen = left - lsb * sq;
	/* a left-set run that starts with a letter hangs on a stem: its pen
	 * is where an H's would put the stem at the anchor, so an A, W or T
	 * overhangs the way type does instead of each line's first ink
	 * sitting on the column and the diagonals looking indented */
	if (align != HDT_RIGHT && align != HDT_CENTRE) {
		float z0, zh, lead = 0;
		int c0, s0 = -1, sh = set_slot ('H', &zh);
		for (c0 = 0; c0 < n; c0++) {
			s0 = set_slot (u[c0], &z0);
			if (u[c0] != ' ' && s0 >= 0 && fc->has[s0]) break;
			lead += fc->space * k + ro->track * cap;
		}
		if (c0 < n && sh >= 0 && fc->has[sh] && z0 == 1 &&
		    ((u[c0] >= 'A' && u[c0] <= 'Z') || (u[c0] >= 'a' && u[c0] <= 'z')))
			pen = left - (lead + fc->lsb[sh] * k) * sq;
	}
	for (c = 0; c < n && n_placed < MAX_PLACED; c++) {
		struct placed *p;
		struct atlas *ga = a;
		float z, adv, off = 0, rise = 0;
		int s = set_slot (u[c], &z);
		if (z != 1) {
			/* a superscript, from the atlas at its own size */
			if (!sup) sup = atlas_get (ro->face, (int) (cap * z * 4 + 0.5f));
			ga = sup;
			rise = floorf (SUP_RISE * cap + 0.5f);
		}
		if (u[c] == ' ' || s < 0 || !fc->has[s] || !ga || !ga->g[s].w) {
			pen += (fc->space * k + ro->track * cap) * sq;
			continue;
		}
		adv = fc->adv[s];
		if (ro->tabular && z == 1 && u[c] >= '0' && u[c] <= '9') {
			off = (fc->digit - adv) / 2 * k;
			adv = fc->digit;
		}
		p = &placed[n_placed++];
		p->a = ga;
		p->slot = s;
		p->X = floorf (pen + off * sq + 0.5f) + ga->g[s].left * sq;
		p->Y = Y - ga->g[s].top - rise;
		p->w = ga->g[s].w * sq;
		p->rgb = rgb;
		p->alpha = alpha;
		p->shadow = ro->shadow * alpha;
		p->drop = floorf (ro->drop * cap + 0.5f);
		if (p->drop < 1) p->drop = 1;
		/* text over the map: a halo, so it reads over stars and lines */
		p->halo = hdtext_map && r->y + 8 < CTRL_Y ? ceilf (0.08f * cap) : 0;
		pen += (adv * k + ro->track * cap) * z * sq;
	}
}

static void quad (float X, float Y, float W, float H, float s0, float t0, float s1, float t1,
		  float sx, float sy)
{
	glTexCoord2f (s0, t0);
	glVertex2f (X / sx, ST_H - Y / sy);
	glTexCoord2f (s1, t0);
	glVertex2f ((X + W) / sx, ST_H - Y / sy);
	glTexCoord2f (s1, t1);
	glVertex2f ((X + W) / sx, ST_H - (Y + H) / sy);
	glTexCoord2f (s0, t1);
	glVertex2f (X / sx, ST_H - (Y + H) / sy);
}

/* The icons the redesign draws as vectors, at full resolution: the tick
 * box, the four diagonal arrows and the small hex letters.  The rest of the
 * font's icons keep the smoothed bitmap (draw_icons). */
static int vector_icon (int chr)
{
	int c = chr + 0x20;
	return (c >= 0x7d && c <= 0x81) || (c >= 0x8b && c <= 0x90);
}

/* An icon (and a glyph no font has): the original glyph smoothed, from
 * the old texture */
static void draw_icons (const unsigned char *font, const unsigned int *pal, const unsigned int *ctrl_pal,
			float sx, float sy)
{
	int i, k, any = 0;

	for (i = 0; i < n_runs; i++)
		if ((runs[i].kind & HDT_K_ICON) && !runs[i].hide && !vector_icon (runs[i].chr0)) any = 1;
	if (!any) return;
	if (!tex || built_w != (int) (sx * ST_W + 0.5f) || built_h != (int) (sy * ST_H + 0.5f))
		build (font, (int) (sx * ST_W + 0.5f), (int) (sy * ST_H + 0.5f));
	if (!tex) return;
	glBindTexture (GL_TEXTURE_2D, tex);
	glBegin (GL_QUADS);
	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		if (!(r->kind & HDT_K_ICON) || r->hide || vector_icon (r->chr0)) continue;
		for (k = r->first; k < r->first + r->n; k++) {
			const struct shown *d = &shown[k];
			unsigned int rgba = r->rgb ? r->rgb : (d->y + 4 >= CTRL_Y && d->col < 16 ? ctrl_pal : pal)[d->col];
			float s0 = (d->chr % 16) * cw / (float) tw, t0 = (d->chr / 16) * ch / (float) th;
			float px = floorf ((d->x - 1 + r->dx) * sx + 0.5f), py = floorf ((d->y - 1 + r->dy) * sy + 0.5f);
			glColor4ub (rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, 255);
			quad (px, py, cw, ch, s0, t0, s0 + cw / (float) tw, t0 + ch / (float) th, sx, sy);
		}
	}
	glEnd ();
}

#define MAX_VICON	48
static struct vicon {
	GLuint tex;
	int chr, w, h;
	unsigned used;
} vic[MAX_VICON];
static int n_vic;
static unsigned vic_clock;

static void vicon_forget (void)
{
	int i;
	for (i = 0; i < n_vic; i++)
		if (vic[i].tex) glDeleteTextures (1, &vic[i].tex);
	n_vic = 0;
}

/* glyph chr drawn into a w x h alpha bitmap covering ST pixels x-1..x+9,
 * y-1..y+9 of a glyph at (x, y); CG's y counts up, so it is flipped */
static void vicon_paint (CGContextRef g, int chr, int w, int h, float sx, float sy)
{
	int c = chr + 0x20;
	float u = sx < sy ? sx : sy;

	CGContextTranslateCTM (g, 0, h);
	CGContextScaleCTM (g, 1, -1);
	CGContextSetGrayFillColor (g, 1, 1);
	CGContextSetGrayStrokeColor (g, 1, 1);
	CGContextSetLineJoin (g, kCGLineJoinRound);
	CGContextSetLineCap (g, kCGLineCapRound);
	if (c == 0x81) {
		/* the box: a crisp rounded square in the 8x8 cell */
		float side = 7.2f * u, lw = side * 0.11f, cx = 5 * sx, cy = 5 * sy;
		CGRect b;
		if (lw < 1.5f) lw = 1.5f;
		b = CGRectMake (cx - side / 2 + lw / 2, cy - side / 2 + lw / 2, side - lw, side - lw);
		CGContextSetLineWidth (g, lw);
		{
			CGPathRef path = CGPathCreateWithRoundedRect (b, side * 0.16f, side * 0.16f, NULL);
			CGContextAddPath (g, path);
			CGContextStrokePath (g);
			CGPathRelease (path);
		}
	} else if (c >= 0x7d && c <= 0x80) {
		/* a diagonal arrow: its head in the corner it points to */
		int left = c == 0x7d || c == 0x7f, up = c == 0x7d || c == 0x7e;
		float x0 = 1 * sx, y0 = 1 * sy, x1 = 8 * sx, y1 = 8 * sy;
		float tx = left ? x0 : x1, ty = up ? y0 : y1;	/* the tip */
		float ex = left ? x1 : x0, ey = up ? y1 : y0;	/* the tail */
		float hx = (left ? 1 : -1) * 4.6f * sx, hy = (up ? 1 : -1) * 4.6f * sy;
		CGContextMoveToPoint (g, tx, ty);
		CGContextAddLineToPoint (g, tx + hx, ty);
		CGContextAddLineToPoint (g, tx, ty + hy);
		CGContextClosePath (g);
		CGContextFillPath (g);
		CGContextSetLineWidth (g, 1.7f * u);
		CGContextMoveToPoint (g, tx + hx * 0.4f, ty + hy * 0.4f);
		CGContextAddLineToPoint (g, ex - (ex - tx) * 0.1f, ey - (ey - ty) * 0.1f);
		CGContextStrokePath (g);
	} else {
		/* a small hex letter, A to F, in the condensed face */
		struct face *fc = face_load (F_COND);
		UniChar ch = (UniChar) ('A' + c - 0x8b);
		CGGlyph gl = 0;
		CTFontRef font;
		CGRect bb;
		CGPoint pen;
		float cap = 6.0f * sy;
		if (!fc) return;
		font = face_font (F_COND, fc->size100 * cap / 100.0f);
		if (!font) return;
		if (CTFontGetGlyphsForCharacters (font, &ch, &gl, 1) && gl) {
			CTFontGetBoundingRectsForGlyphs (font, kCTFontOrientationHorizontal, &gl, &bb, 1);
			/* text is drawn the right way up: undo the flip around it */
			CGContextSaveGState (g);
			CGContextScaleCTM (g, 1, -1);
			CGContextTranslateCTM (g, 0, -h);
			pen.x = 1 * sx + (3.5f * sx - bb.size.width) / 2 - bb.origin.x;
			pen.y = h - (1 * sy + 7 * sy);
			CGContextSetTextMatrix (g, CGAffineTransformIdentity);
			CTFontDrawGlyphs (font, &gl, &pen, 1, g);
			CGContextRestoreGState (g);
		}
		CFRelease (font);
	}
}

static struct vicon *vicon_get (int chr, float sx, float sy)
{
	int w = (int) ceilf (10 * sx), h = (int) ceilf (10 * sy), i;
	struct vicon *v = NULL;
	unsigned char *buf;
	CGContextRef g;

	for (i = 0; i < n_vic; i++)
		if (vic[i].chr == chr && vic[i].w == w && vic[i].h == h) {
			vic[i].used = ++vic_clock;
			return vic[i].tex ? &vic[i] : NULL;
		}
	if (n_vic < MAX_VICON)
		v = &vic[n_vic++];
	else {
		for (v = &vic[0], i = 1; i < n_vic; i++)
			if (vic[i].used < v->used) v = &vic[i];
		if (v->tex) glDeleteTextures (1, &v->tex);
	}
	memset (v, 0, sizeof (*v));
	v->chr = chr;
	v->w = w;
	v->h = h;
	v->used = ++vic_clock;
	buf = calloc ((size_t) w * h, 1);
	if (!buf) return NULL;
	g = CGBitmapContextCreate (buf, w, h, 8, w, NULL, kCGImageAlphaOnly);
	if (g) {
		CGContextSetShouldAntialias (g, true);
		vicon_paint (g, chr, w, h, sx, sy);
		CGContextRelease (g);
		v->tex = alpha_texture (buf, w, h);
	}
	free (buf);
	return v->tex ? v : NULL;
}

static void draw_vector_icons (const unsigned int *pal, const unsigned int *ctrl_pal, float sx, float sy)
{
	int i, k;

	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		if (!(r->kind & HDT_K_ICON) || r->hide || !vector_icon (r->chr0)) continue;
		for (k = r->first; k < r->first + r->n; k++) {
			const struct shown *d = &shown[k];
			unsigned int rgba = r->rgb ? r->rgb : (d->y + 4 >= CTRL_Y && d->col < 16 ? ctrl_pal : pal)[d->col];
			struct vicon *v = vicon_get (d->chr, sx, sy);
			float px, py, a = r->alpha > 0 ? r->alpha : 1;
			if (!v) continue;
			px = floorf ((d->x - 1 + r->dx) * sx + 0.5f);
			py = floorf ((d->y - 1 + r->dy) * sy + 0.5f);
			glBindTexture (GL_TEXTURE_2D, v->tex);
			glColor4ub (rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, (unsigned char) (255 * (a > 1 ? 1 : a)));
			glBegin (GL_QUADS);
			quad (px, py, v->w, v->h, 0, 0, 1, 1, sx, sy);
			glEnd ();
		}
	}
}

/* On the map, names that would overlap: text that is not a name, then
 * the name nearer the middle, keeps its place; the other moves a line
 * down or up if that is clear, or is left out, as a printed map leaves
 * out what it has no room for. */
static float clutter_key[MAX_RUNS];

static int cmp_clutter (const void *a, const void *b)
{
	float d = clutter_key[*(const short *) a] - clutter_key[*(const short *) b];
	return d < 0 ? -1 : d > 0;
}

static void declutter (void)
{
	static short order[MAX_RUNS];
	static float box[MAX_RUNS][4];
	static const float move[3] = { 0, 8, -8 };
	int i, j, k, m, n = 0, kept = 0;

	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		float cx = (r->ink0 + r->ink1) / 2.0f - ST_W / 2, cy = r->y - CTRL_Y / 2;
		if (r->hide || r->y + 8 >= CTRL_Y || (r->kind & HDT_K_ICON)) continue;
		clutter_key[i] = run_name[i] ? 1 + cx * cx + cy * cy : 0;
		order[n++] = i;
	}
	qsort (order, n, sizeof (order[0]), cmp_clutter);
	for (k = 0; k < n; k++) {
		struct hdtext_run *r = &runs[order[k]];
		float w = hdtext_measure (r), x0;
		if (w < r->ink1 - r->ink0) w = r->ink1 - r->ink0;
		x0 = auto_align[order[k]] == HDT_CENTRE ? auto_ax[order[k]] - w / 2 : r->ink0;
		x0 += r->dx;
		for (m = 0; m < (run_name[order[k]] ? 3 : 1); m++) {
			float y0 = r->y + r->dy + move[m];
			for (j = 0; j < kept; j++)
				if (x0 < box[j][2] && box[j][0] < x0 + w &&
				    y0 < box[j][3] && box[j][1] < y0 + 8)
					break;
			if (j == kept || !run_name[order[k]]) break;
		}
		if (m == 3) {
			r->hide = 1;
			continue;
		}
		r->dy += move[m];
		box[kept][0] = x0;
		box[kept][1] = r->y + r->dy;
		box[kept][2] = x0 + w;
		box[kept][3] = r->y + r->dy + 8;
		kept++;
	}
}

static void new_draw (const unsigned char *font, const unsigned int *pal, const unsigned int *ctrl_pal,
		      float sx, float sy)
{
	int i, pass;
	struct atlas *bound;

	n_placed = 0;
	frame_clock = use_clock;
	if (hdtext_map) declutter ();
	for (i = 0; i < n_runs; i++)
		if (!runs[i].hide && !(runs[i].kind & HDT_K_ICON))
			place_run (i, sx, sy, pal, ctrl_pal);

	glEnable (GL_TEXTURE_2D);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	/* all the shadows first, then all the glyphs */
	for (pass = 0; pass < 2; pass++) {
		bound = NULL;
		for (i = 0; i < n_placed; i++) {
			const struct placed *p = &placed[i];
			const struct atlas *a = p->a;
			float s0 = a->g[p->slot].x / (float) a->tw, t0 = a->g[p->slot].y / (float) a->th;
			float s1 = (a->g[p->slot].x + a->g[p->slot].w) / (float) a->tw;
			float t1 = (a->g[p->slot].y + a->g[p->slot].h) / (float) a->th;

			if (p->a != bound) {
				if (bound) glEnd ();
				bound = p->a;
				glBindTexture (GL_TEXTURE_2D, pass ? bound->tex : bound->blur);
				glBegin (GL_QUADS);
			}
			if (!pass) {
				static const signed char hx[8] = { -1, -1, -1, 0, 0, 1, 1, 1 },
							 hy[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
				int d;
				if (p->shadow <= 0) continue;
				glColor4f (0, 0, 0, 0.5f * p->alpha);
				for (d = 0; p->halo > 0 && d < 8; d++)
					quad (p->X + p->halo * hx[d], p->Y + p->halo * hy[d],
					      p->w, a->g[p->slot].h, s0, t0, s1, t1, sx, sy);
				glColor4f (0, 0, 0, p->shadow > 1 ? 1 : p->shadow);
				quad (p->X, p->Y + p->drop, p->w, a->g[p->slot].h, s0, t0, s1, t1, sx, sy);
				/* a strong shadow is laid twice */
				if (p->shadow > 0.75f) quad (p->X, p->Y + p->drop, p->w, a->g[p->slot].h, s0, t0, s1, t1, sx, sy);
			} else {
				glColor4ub (p->rgb & 0xff, (p->rgb >> 8) & 0xff, (p->rgb >> 16) & 0xff,
					    (unsigned char) (255 * (p->alpha > 1 ? 1 : p->alpha)));
				quad (p->X, p->Y, p->w, a->g[p->slot].h, s0, t0, s1, t1, sx, sy);
			}
		}
		if (bound) glEnd ();
	}
	draw_icons (font, pal, ctrl_pal, sx, sy);
	draw_vector_icons (pal, ctrl_pal, sx, sy);
	glDisable (GL_BLEND);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub (255, 255, 255, 255);
}
#else
float hdtext_measure (const struct hdtext_run *r)
{
	return r->ink1 - r->ink0;
}
#endif /* __APPLE__ */

void hdtext_draw (const unsigned char *font, const unsigned int *pal, const unsigned int *ctrl_pal)
{
	GLint vp[4] = { 0, 0, 0, 0 };

	if (pixel_text || !n_shown) return;
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] <= 0 || vp[3] <= 0) return;
#ifdef __APPLE__
	if (!classic_ui) {
		last_sx = vp[2] / (float) ST_W;
		last_sy = vp[3] / (float) ST_H;
		new_draw (font, pal, ctrl_pal, last_sx, last_sy);
		return;
	}
#endif
	legacy_draw (font, pal, ctrl_pal, vp);
}

void hdtext_forget (void)
{
	legacy_forget ();
#ifdef __APPLE__
	{
		int i;
		for (i = 0; i < n_atl; i++) atlas_free (&atl[i]);
		n_atl = 0;
		vicon_forget ();
	}
#endif
}
