/*
 * Phase 2 experiment E1 (docs/experiments/EXP-01): FE2_TEXT=hires.
 *
 * Every UI string reaches the host through DrawStr, which rasterises the
 * 8-pixel bitmap font into fe2.s' 320x200 screen buffer. With FE2_TEXT=hires
 * the GL renderer additionally draws the same strings at window resolution
 * with a self-authored stroke font, and hides the bitmap glyphs underneath.
 *
 * fe2.s' memory is never changed. For every glyph DrawStr draws we record
 * which buffer bytes it changed (old and new value). Before the 320x200
 * buffer is uploaded as a texture, a copy of it is "peeled": newest glyph
 * first, a glyph whose bytes still hold the values it wrote is valid; its
 * bytes are put back to what was under it and it is drawn as strokes
 * instead. A glyph anything else has drawn over fails the check and its
 * bitmap pixels simply stay, so the screen is never wrong, only lower
 * resolution. Static screens (stock market, shipyard, bulletin board) draw
 * their text once into a back buffer and Memcpy it to the shown buffer every
 * frame, so Memcpy clones the records of the bytes it copies, and Memset
 * drops the records of the bytes it clears.
 *
 * Off (the default): exp_text_on is 0, DrawStr only tests it, nothing else
 * in this file runs.
 */
#ifdef WITH_GL

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "main.h"
#include "../m68000.h"
#include "screen.h"
#include "exp.h"

int exp_text_on;

extern unsigned int MainRGBPalette[256];
extern unsigned int CtrlRGBPalette[16];

#define SCRBYTES	(320 * 200)
#define MAX_GLYPHS	32768
#define MAX_PIX		128	/* glyph and its shadow: 2 x 8x8 */

struct glyph {
	u32 addr;		/* ST address of the glyph's top-left pixel */
	unsigned char chr, col, shadowed, adv, npix, alive, valid;
	unsigned long born;	/* frame it was drawn or copied */
	unsigned short off[MAX_PIX];	/* byte offsets from addr */
	unsigned char was[MAX_PIX], now[MAX_PIX];
};

static struct glyph *glyphs;
static int nglyphs;

/* the glyph DrawStr is drawing now */
static unsigned char cell_before[9 * 320 + 9];
static int cur_x, cur_y, cur_ok;
static u32 cur_addr;
static unsigned char cur_chr, cur_col, cur_shadowed;

static unsigned long stat_frames, stat_hires, stat_fallback;
/* last frame: glyphs drawn hi-res, and glyphs drawn or copied in the last
 * two frames that had to stay bitmaps because something drew over them */
static int last_hires, last_fallback;

/* ---------------------------------------------------------- stroke font */

/* Self-authored for this experiment (no third-party font data). Each glyph
 * is a list of polylines separated by spaces; a polyline is a run of
 * "xy" digit pairs on a grid x 0-4 (left to right across the glyph's
 * advance) and y 0-6 (cap height, baseline at 6), 7 = descender. A single
 * pair is a dot. Lower case has its own table with x-height at y 2. */
static const char *const stroke[0x60] = {
	/* 20 */ "", "2024 26", "1012 3032", "1016 3036 0242 0444",
	/* 24 */ "400103434606 2027", "0640 00101101 3545463635", "461402102112 0305162646", "2022",
	/* 28 */ "30121436", "10323416", "1135 3115 0343", "2125 0343",
	/* 2c */ "2517", "0343", "26", "0640",
	/* 30 */ "0040460600 0640", "112026", "004043030646", "00404606 1343",
	/* 34 */ "000343 4046", "400003434606", "400006464303", "004016",
	/* 38 */ "0040460600 0343", "430300404606", "21 25", "21 2516",
	/* 3c */ "400346", "0242 0444", "004306", "0040424224 26",
	/* 40 */ "42222444 4440000646", "062046 1434", "06003041423303 3344453606", "40000646",
	/* 44 */ "06003041453606", "40000646 0333", "400006 0333", "400006464323",
	/* 48 */ "0006 4046 0343", "2026 1030 1636", "4045361605", "0006 4003 1246",
	/* 4c */ "000646", "0600234046", "06004640", "0040460600",
	/* 50 */ "0600404303", "0040460600 2446", "0600404303 1346", "400003434606",
	/* 54 */ "0040 2026", "00064640", "002640", "0016233640",
	/* 58 */ "0046 4006", "002340 2326", "00400646", "30101636",
	/* 5c */ "0046", "10303616", "022042", "0747",
	/* 60 */ "1021", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 70 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "30212213242536", "2027", "10212233242516", "0312233441", 0,
};

static const char *const lower[26] = {
	"124246060444", "0006464202", "42020646", "4046060242",		/* a-d */
	"044442020646", "402126 1232", "460602424707", "0006 024246",	/* e-h */
	"2226 21", "222717 21", "0006 420446", "202636",		/* i-l */
	"06024246 2226", "06024246", "0242460602", "0702424606",	/* m-p */
	"4742020646", "0602 033242", "420204444606", "202646 0242",	/* q-t */
	"02064642", "022642", "0216243642", "0246 4206",		/* u-x */
	"020444 424707", "02420646",					/* y-z */
};

static const char *glyph_strokes (int chr, int *small)
{
	*small = 0;
	if (chr >= 'a' && chr <= 'z') return lower[chr - 'a'];
	if (chr < 0x20 || chr >= 0x80) return NULL;
	return stroke[chr - 0x20];
}

/* ------------------------------------------------------------ recording */

void ExpText_Init (void)
{
	glyphs = calloc (MAX_GLYPHS, sizeof (struct glyph));
	exp_text_on = (glyphs != NULL);
}

static void compact (void)
{
	int i, j;
	for (i = j = 0; i < nglyphs; i++)
		if (glyphs[i].alive) {
			if (i != j) glyphs[j] = glyphs[i];
			j++;
		}
	nglyphs = j;
}

static struct glyph *new_glyph (void)
{
	if (nglyphs == MAX_GLYPHS) compact ();
	if (nglyphs == MAX_GLYPHS) {
		/* still full: forget the oldest quarter, they fall back to bitmaps */
		memmove (glyphs, glyphs + MAX_GLYPHS / 4, (MAX_GLYPHS - MAX_GLYPHS / 4) * sizeof (struct glyph));
		nglyphs -= MAX_GLYPHS / 4;
	}
	return &glyphs[nglyphs++];
}

/* called by DrawStr before it draws chr (font index + 0x20) at x,y */
void ExpText_GlyphBegin (unsigned char *scrline, int x, int y, int chr, int col, int shadowed)
{
	int r, small;

	cur_ok = (x >= 0 && x <= 311 && y >= 0 && y <= 191 &&
		  glyph_strokes (chr, &small) != NULL);
	if (!cur_ok) return;
	cur_addr = (u32) ((char *) scrline - STRam) + x;
	cur_x = x;
	cur_y = y;
	cur_chr = chr;
	cur_col = col;
	cur_shadowed = shadowed;
	for (r = 0; r < 9; r++)
		memcpy (cell_before + r * 320, STRam + cur_addr + r * 320, 9);
}

/* called by DrawStr after the glyph (and its shadow) is drawn */
void ExpText_GlyphEnd (int next_x)
{
	struct glyph *g;
	int r, c, n = 0;
	unsigned char *mem = (unsigned char *) STRam + cur_addr;

	if (!cur_ok) return;
	cur_ok = 0;
	for (r = 0; r < 9; r++)
		for (c = 0; c < 9; c++)
			if (mem[r * 320 + c] != cell_before[r * 320 + c]) n++;
	if (!n || n > MAX_PIX) return;
	g = new_glyph ();
	g->addr = cur_addr;
	g->chr = cur_chr;
	g->col = cur_col;
	g->shadowed = cur_shadowed;
	g->adv = next_x - cur_x;
	g->alive = 1;
	g->born = stat_frames;
	g->npix = 0;
	for (r = 0; r < 9; r++)
		for (c = 0; c < 9; c++) {
			int o = r * 320 + c;
			if (mem[o] == cell_before[o]) continue;
			g->off[g->npix] = o;
			g->was[g->npix] = cell_before[o];
			g->now[g->npix++] = mem[o];
		}
}

static int inside (const struct glyph *g, u32 lo, u32 hi)
{
	return g->addr >= lo && g->addr + 8 * 320 + 9 <= hi;
}

/* Memset / MemsetBlue cleared [adr, adr+count) */
void ExpText_Clear (u32 adr, u32 count)
{
	int i;
	for (i = 0; i < nglyphs; i++)
		if (glyphs[i].alive && inside (&glyphs[i], adr, adr + count))
			glyphs[i].alive = 0;
}

/* Memcpy copied count bytes from src to dest */
void ExpText_Memcpy (u32 dest, u32 src, u32 count)
{
	int i, n;

	ExpText_Clear (dest, count);
	n = nglyphs;
	for (i = 0; i < n; i++) {
		struct glyph *g;
		if (!glyphs[i].alive || !inside (&glyphs[i], src, src + count)) continue;
		g = new_glyph ();
		if (nglyphs <= i) break;	/* the store was trimmed under us */
		*g = glyphs[i];
		g->addr += dest - src;
		g->born = stat_frames;
	}
}

/* -------------------------------------------------------------- display */

/* Returns a copy of the 320x200 buffer at VideoBase with every still-valid
 * recorded glyph removed, and marks those glyphs for ExpText_Draw. */
const unsigned char *ExpText_Peel (const unsigned char *raster)
{
	static unsigned char out[SCRBYTES];
	int i, k;

	memcpy (out, raster, SCRBYTES);
	last_hires = last_fallback = 0;
	for (i = nglyphs - 1; i >= 0; i--) {
		struct glyph *g = &glyphs[i];
		long base = (long) g->addr - (long) VideoBase;
		g->valid = 0;
		if (!g->alive || base < 0 || base + 8 * 320 + 9 > SCRBYTES) continue;
		for (k = 0; k < g->npix; k++)
			if (out[base + g->off[k]] != g->now[k]) break;
		if (k < g->npix) {
			if (g->born + 2 >= stat_frames) {
				stat_fallback++;
				last_fallback++;
			}
			continue;
		}
		for (k = 0; k < g->npix; k++)
			out[base + g->off[k]] = g->was[k];
		g->valid = 1;
		stat_hires++;
		last_hires++;
	}
	/* drop glyphs overwritten for good, now and then */
	if ((++stat_frames & 31) == 0) {
		for (i = 0; i < nglyphs; i++) {
			struct glyph *g = &glyphs[i];
			long base = (long) g->addr - (long) VideoBase;
			if (g->alive && !g->valid && base >= 0 && base + 8 * 320 + 9 <= SCRBYTES)
				g->alive = 0;
		}
		compact ();
	}
	return out;
}

static void stroke_glyph (const struct glyph *g, float ox, float oy, int shadow)
{
	const char *s;
	int small;
	float w, x0, y0, sy;
	unsigned int rgb;

	if (!(s = glyph_strokes (g->chr, &small)) || !*s) return;
	/* the bitmap font is adv-1 pixels wide and 7 high (+1 descender);
	 * narrow glyphs (t, r, c, 1) get at least 2.5 pixels so their
	 * strokes do not collapse into one line, centred in the advance */
	w = g->adv - 2.0f;
	x0 = ox + 0.5f;
	if (w < 2.5f) {
		x0 -= (2.5f - w) / 2.0f;
		w = 2.5f;
	}
	y0 = oy + 0.5f + (small ? 2.0f : 0.0f);
	sy = small ? 4.0f / 6.0f : 1.0f;
	if (shadow) {
		rgb = MainRGBPalette[0];
		glColor3ub (0, 0, 0);
	} else {
		rgb = (oy >= 168 ? CtrlRGBPalette[g->col & 15] : MainRGBPalette[g->col]);
		glColor3ub (rgb & 0xff, (rgb >> 8) & 0xff, (rgb >> 16) & 0xff);
	}
	while (*s) {
		int n = 0;
		const char *p = s;
		while (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
			p += 2;
			n++;
		}
		glBegin (n == 1 ? GL_POINTS : GL_LINE_STRIP);
		for (; s < p; s += 2) {
			float gx = x0 + (s[0] - '0') * w / 4.0f;
			float gy = y0 + (s[1] - '0') * sy;
			glVertex2f (gx, 200.0f - gy);
		}
		glEnd ();
		while (*s == ' ') s++;
	}
}

/* draw the glyphs ExpText_Peel removed; called with a 320x200 ortho
 * projection over the whole window (y up) */
void ExpText_Draw (int screen_w, int screen_h)
{
	int i;
	float scale = screen_w / 320.0f;

	glDisable (GL_TEXTURE_2D);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable (GL_LINE_SMOOTH);
	glEnable (GL_POINT_SMOOTH);
	glLineWidth (scale * 0.75f);
	glPointSize (scale * 0.9f);
	for (i = 0; i < nglyphs; i++) {
		const struct glyph *g = &glyphs[i];
		long base = (long) g->addr - (long) VideoBase;
		float x, y;
		if (!g->valid) continue;
		x = base % 320;
		y = base / 320;
		if (g->shadowed) stroke_glyph (g, x + 0.6f, y + 0.6f, 1);
		stroke_glyph (g, x, y, 0);
	}
	glDisable (GL_LINE_SMOOTH);
	glDisable (GL_POINT_SMOOTH);
	glLineWidth (1.0f);
	glPointSize (1.0f);
	glDisable (GL_BLEND);
}

void ExpText_Stats (FILE *f)
{
	fprintf (f, "FE2_TEXT: last frame %d glyphs hi-res, %d fresh glyphs left as bitmap; run: %lu frames, %lu hi-res, %lu bitmap, %d records live\n",
		 last_hires, last_fallback, stat_frames, stat_hires, stat_fallback, nglyphs);
}

#endif /* WITH_GL */
