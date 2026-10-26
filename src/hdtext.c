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
 * Nothing here writes ST memory, so the oracle sees what it always saw.
 */
#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <ApplicationServices/ApplicationServices.h>
#else
#include <GL/gl.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hdtext.h"

#define ST_W		320
#define ST_H		200
#define CTRL_Y		168	/* the control panel's rows have their own palette */
#define NGLYPHS		113	/* font codes 0x20..0x90 */
#define MAX_ACTIVE	4096	/* glyph positions tracked at once */
#define MISS_LIMIT	8	/* frames a glyph may be off the screen */
#define DEFAULT_FONT	"DINAlternate-Bold"	/* FE2_HD_FONT overrides */

/* Recorded at a glyph's top left pixel: two per position, for a place
 * that alternates between two glyphs (a highlighted menu item). */
struct ent {
	unsigned char chr, col, flags, misses;
};
#define F_USED		1
#define F_SHADOW	2

static struct ent slots[ST_W * ST_H][2];
static unsigned char listed[ST_W * ST_H];
static int active[MAX_ACTIVE], n_active;

/* what the last hdtext_frame found on the screen */
static struct shown {
	short x, y;
	unsigned char chr, col, shadow;
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

	for (oy = y - 7; oy <= y + 7; oy++) {
		if (oy < 0 || oy >= ST_H) continue;
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
}

/* Every pixel of the glyph is in its colour, and something in its box is
 * not: a block of solid colour is not a glyph. */
static int on_screen (const unsigned char *g, const unsigned char *r, int x, int y, int col)
{
	int row, c, pixels = 0, other = 0;

	for (row = 0; row < 8; row++) {
		for (c = 0; c < 8 && x + c < ST_W; c++) {
			int p = r[(y + row) * ST_W + x + c];
			if (ink (g, c, row)) {
				if (p != col) return 0;
				pixels++;
			} else if (c < g[9] && p != col)
				other = 1;
		}
	}
	return pixels && other;
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

void hdtext_frame (const unsigned char *font, const unsigned char *r, unsigned char *out)
{
	int i, k;

	memcpy (out, r, ST_W * ST_H);
	n_shown = 0;
	for (i = 0; i < n_active; ) {
		int o = active[i], x = o % ST_W, y = o / ST_W, hit = -1;
		struct ent *s = slots[o];

		for (k = 0; k < 2; k++) {
			if (!(s[k].flags & F_USED)) continue;
			if (hit < 0 && on_screen (font + s[k].chr * 10, r, x, y, s[k].col)) {
				hit = k;
				s[k].misses = 0;
			} else if (++s[k].misses > MISS_LIMIT)
				s[k].flags = 0;
		}
		if (hit >= 0) {
			struct shown *d = &shown[n_shown++];
			d->x = x;
			d->y = y;
			d->chr = s[hit].chr;
			d->col = s[hit].col;
			d->shadow = (s[hit].flags & F_SHADOW) != 0;
			erase (font + d->chr * 10, r, out, x, y, d->col, d->shadow);
		}
		if (!(s[0].flags & F_USED) && !(s[1].flags & F_USED)) {
			listed[o] = 0;
			active[i] = active[--n_active];
			continue;
		}
		i++;
	}
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

static void build (const unsigned char *font, int vw, int vh)
{
	float sx = vw / (float) ST_W, sy = vh / (float) ST_H;
	unsigned char typed[NGLYPHS], *buf;
	int c, lo, hi;

	hdtext_forget ();
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
	return p->y != q->y ? p->y - q->y : p->x - q->x;
}

/* Set each word with the font's own spacing.  A word is a run of glyphs
 * the game drew one advance apart on one line in one colour: its first
 * and last letters keep the original's ink edges, so columns still line
 * up, and the tracking between takes up the difference.  (Centred one by
 * one on the original's letters, a narrow letter leaves a hole.) */
static void layout (const unsigned char *font, float sx)
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

void hdtext_draw (const unsigned char *font, const unsigned int *pal, const unsigned int *ctrl_pal)
{
	GLint vp[4] = { 0, 0, 0, 0 };
	float sx, sy;
	int i, pass;

	if (pixel_text || !n_shown) return;
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] <= 0 || vp[3] <= 0) return;
	if (!tex || vp[2] != built_w || vp[3] != built_h) build (font, vp[2], vp[3]);
	if (!tex) return;
	sx = vp[2] / (float) ST_W;
	sy = vp[3] / (float) ST_H;
	layout (font, sx);

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

void hdtext_forget (void)
{
	if (tex) glDeleteTextures (1, &tex);
	tex = 0;
	built_w = built_h = 0;
}
