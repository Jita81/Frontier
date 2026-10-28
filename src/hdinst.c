/*
 * HD instruments (see hdinst.h): the scanner's contacts, the gun sight
 * and the lock boxes, drawn again as smooth vector shapes.
 *
 * Contacts.  The game blits the scanner's bitmap and then draws each
 * contact over it with Call_OldHLine (a stalk above the scanner's plane,
 * then a 4 pixel blip on top) or Call_BackHLine (a stalk below the plane,
 * only where the bitmap is clear, then the blip at its foot).  The lines
 * drawn between the scanner's blit and the next blit are noted, per
 * screen buffer, and joined into contacts (lines that touch, in the order
 * drawn).  A frame the buffer being shown still has most of its contacts'
 * pixels, those pixels are taken out and the newest set of contacts (the
 * one drawn last, whichever buffer: so docked, where the two buffers take
 * turns, they do not jump back and forth) is drawn on the HD scanner's
 * glass, each a thin stalk and a round blip in the game's colour.
 *
 * Gun sight.  The game draws it in one colour at a fixed place: 52 pixels
 * looking forward, the same less its four diagonal ticks (32) looking
 * back.  Where nearly all of either is on the screen in one colour (and
 * the gaps between its ticks are not), they are taken out and the sight
 * is drawn again, the ticks it has, as thin lines with a faint glow.
 * hdtext is told which pixels they are, so a label the sight was drawn
 * over is still read; a label over the sight is moved clear of it.
 *
 * Lock boxes.  Over a 3D view (the GL renderer leaves the view clear in
 * the ST screen; not a station page, which hdback has a picture behind)
 * the target's boxes are the only straight lines but the sight's and the
 * labels'.  A line runs on across a glyph's cell and across pixels another
 * layer took out, as the game drew those over it, so a corner hidden under
 * a label is still found.  Where a horizontal and a vertical line of one
 * colour meet at their ends there is a corner, and a corner bracket in the
 * interface's accent colour is drawn in place of the box.  Each line with
 * a corner, and each of a box's colour the view's edge cuts off, is taken
 * out end to end, all but the glyph ink hdtext redrew; one cut off with
 * no corner in it is drawn again as a faint accent line, the brackets'
 * weight.
 */
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "main.h"
#include "screen.h"
#include "hdtext.h"
#include "hdinst.h"
#include "hdback.h"

#define ST_W		320
#define ST_H		200
#define VIEW_H		168	/* the 3D view's rows; the control panel below */
#define C_ACCENT	0x3fd0ff	/* hdui's accent: selection, focus, active */

/* the HD scanner's glass (hdstill.c), a pixel larger */
#define GLASS_X		160.5f
#define GLASS_Y		184.0f
#define GLASS_RX	79.5f
#define GLASS_RY	14.3f

/* ---------------------------------------------------------------- */
/* the scanner's contacts, as drawn */

#define MAX_PIX		4096
#define MAX_RUN		1024
#define MAX_CON		128

struct run {
	short y, x0, x1, con;
	unsigned char col;
};

/* a contact being drawn: its box, which call draws it, its last line */
struct group {
	short x0, y0, x1, y1;
	short ly, lx0, lx1;
	unsigned char back;
};

struct contact {
	float x, plane, blip;		/* ST: the stalk's x, its ends' y */
	unsigned char col, stalk, dot, below;
};

struct pass {
	unsigned seq;			/* 0: none yet */
	int n_pix, n_run, n_con;
	short px[MAX_PIX], py[MAX_PIX];
	unsigned char pc[MAX_PIX];
	struct run run[MAX_RUN];
	struct group grp[MAX_CON];
	struct contact con[MAX_CON];
};

static struct pass passes[2], rec;
static int armed = -1;			/* the buffer being noted, -1: none */
static unsigned seq;

/* What a contact's lines make, row by row (a row may be drawn in two
 * parts, a 16 pixel word each): the 4 pixel wide rows its blip, the
 * narrower ones its stalk; the stalk's other end is on the plane. */
static void shape (struct pass *p)
{
	int k, i, y;

	for (k = 0; k < p->n_con; k++) {
		struct contact *c = &p->con[k];
		const struct group *g = &p->grp[k];
		short rx0[ST_H - 140], rx1[ST_H - 140];
		int bx0 = 999, by0 = 999, by1 = -1, sy0 = 999, sy1 = -1, ns = 0, col = -1;
		float sx = 0;

		for (y = g->y0; y <= g->y1; y++) {
			rx0[y - 140] = 999;
			rx1[y - 140] = -1;
		}
		for (i = 0; i < p->n_run; i++) {
			const struct run *r = &p->run[i];
			if (r->con != k) continue;
			if (r->x0 < rx0[r->y - 140]) rx0[r->y - 140] = r->x0;
			if (r->x1 > rx1[r->y - 140]) rx1[r->y - 140] = r->x1;
			if (r->y >= VIEW_H && col < 0) col = r->col;
		}
		for (y = g->y0; y <= g->y1; y++) {
			int a = rx0[y - 140], b = rx1[y - 140];
			if (b < 0) continue;
			if (b - a >= 2) {
				if (a < bx0) bx0 = a;
				if (y < by0) by0 = y;
				if (y > by1) by1 = y;
			} else {
				sx += (a + b) * 0.5f;
				ns++;
				if (y < sy0) sy0 = y;
				if (y > sy1) sy1 = y;
			}
		}
		c->col = col >= 0 ? col : 255;
		c->dot = by1 >= 0;
		c->stalk = ns > 0;
		c->below = g->back;
		/* pixel indices: a blip at x0..x0+3 sits on the glass's x0 + 1.5,
		 * as a contact dead ahead on the ship's mark (160.5) */
		c->x = c->dot ? bx0 + 1.5f : ns ? sx / ns : 0;
		c->blip = c->dot ? (by0 + by1 + 1) * 0.5f : sy0;
		c->plane = !c->stalk ? c->blip : c->below ? sy0 + 0.5f : sy1 + 0.5f;
	}
}

static void commit (void)
{
	if (armed >= 0) {
		shape (&rec);
		rec.seq = ++seq;
		passes[armed] = rec;
	}
	armed = -1;
}

void hdinst_blit (int x, int y, int w, int h, unsigned long scr)
{
	commit ();
	if (y == 168 && ((x == 80 && w == 160 && h == 31) || (x == 64 && w == 192 && h == 32))) {
		armed = (scr & 0x100000) != 0;
		rec.n_pix = rec.n_run = rec.n_con = 0;
	}
}

void hdinst_run (unsigned long addr, int n, unsigned mask, int col)
{
	unsigned long base = (addr & 0x100000) ? 0x100000 : 0xf0000;
	int o, y, x, i, x0 = -1, x1 = -1, k;
	struct run *r;
	struct group *g;

	if (armed < 0 || ((addr & 0x100000) != 0) != armed || addr < base) return;
	o = (int) (addr - base);
	y = o / ST_W;
	x = o % ST_W;
	if (y < 140 || y >= ST_H) return;
	for (i = 0; i < n; i++) {
		int xx = x + i;
		if (mask && !(mask & (0x8000u >> i))) continue;
		if (xx < 76 || xx > 246 || rec.n_pix >= MAX_PIX) continue;
		rec.px[rec.n_pix] = xx;
		rec.py[rec.n_pix] = y;
		rec.pc[rec.n_pix++] = col;
		if (x0 < 0) x0 = xx;
		x1 = xx;
	}
	if (x0 < 0 || rec.n_run >= MAX_RUN) return;
	/* The same contact as the last line when drawn by the same call and
	 * either the next row of its column (a stalk is drawn from the plane
	 * to the blip) or beside its rows (the rest of a blip, a word on). */
	k = rec.n_con - 1;
	g = k >= 0 ? &rec.grp[k] : NULL;
	if (g && g->back == (mask != 0) &&
	    ((abs (y - g->ly) == 1 && x0 <= g->lx1 && x1 >= g->lx0) ||
	     (y >= g->y0 && y <= g->y1 && x0 <= g->x1 + 1 && x1 >= g->x0 - 1))) {
		if (x0 < g->x0) g->x0 = x0;
		if (x1 > g->x1) g->x1 = x1;
		if (y < g->y0) g->y0 = y;
		if (y > g->y1) g->y1 = y;
	} else {
		if (rec.n_con >= MAX_CON) return;
		k = rec.n_con++;
		g = &rec.grp[k];
		g->x0 = x0;
		g->y0 = y;
		g->x1 = x1;
		g->y1 = y;
		g->back = mask != 0;
	}
	g->ly = y;
	g->lx0 = x0;
	g->lx1 = x1;
	r = &rec.run[rec.n_run++];
	r->y = y;
	r->x0 = x0;
	r->x1 = x1;
	r->con = k;
	r->col = col;
}

/* ---------------------------------------------------------------- */
/* what is found on the screen shown, each frame */

static int fresh;			/* hdinst_frame ran for the frame drawn */
static struct contact shown[MAX_CON];
static int n_shown;

/* the gun sight: 52 pixels, the first 32 its four straight ticks */
static short sight[52][2];
static int sight_col = -1, sight_n;	/* 52, or 32: no diagonal ticks */
static unsigned char sight_mask[ST_W * ST_H];

/* corner brackets: the corner pixel's centre and the arms (signed, ST) */
#define MAX_LINE	256
#define MAX_BRACKET	64
struct line {
	short a, b, at;		/* a..b along, at across */
	short ea, eb;		/* a and b on under what the game drew over it */
	unsigned char col, used;
};
static struct { float x, y, h, v; } brackets[MAX_BRACKET];
static int n_brackets;
#define MAX_EDGE	16
static struct { float x0, y0, x1, y1; } edges[MAX_EDGE];	/* faint: a box's cut-off sides */
static int n_edges;

static void sight_init (void)
{
	int i, n = 0;

	if (sight[0][0]) return;
	for (i = 0; i < 8; i++) {
		sight[n][0] = 145 + i; sight[n++][1] = 84;
		sight[n][0] = 167 + i; sight[n++][1] = 84;
		sight[n][0] = 160; sight[n++][1] = 69 + i;
		sight[n][0] = 160; sight[n++][1] = 92 + i;
	}
	for (i = 0; i < 5; i++) {
		sight[n][0] = 146 + i; sight[n++][1] = 70 + i;
		sight[n][0] = 173 - i; sight[n++][1] = 70 + i;
		sight[n][0] = 173 - i; sight[n++][1] = 98 - i;
		sight[n][0] = 146 + i; sight[n++][1] = 98 - i;
	}
}

static int on (void)
{
	return !classic_ui && use_renderer == R_GL;
}

void hdinst_mask (const unsigned char *raster)
{
	int count[256], i, c = 0, d = 0;

	/* the buffer being noted is done once it is shown */
	if (armed == ((VideoBase & 0x100000) != 0)) commit ();
	sight_init ();
	for (i = 0; i < 52; i++) sight_mask[sight[i][1] * ST_W + sight[i][0]] = 0;
	sight_col = -1;
	hdtext_skip (NULL);
	if (!on ()) return;
	memset (count, 0, sizeof (count));
	for (i = 0; i < 32; i++) count[raster[sight[i][1] * ST_W + sight[i][0]]]++;
	for (i = 1; i < 256; i++)
		if (count[i] > count[c]) c = i;
	if (c == 255 || count[c] < 25 || raster[84 * ST_W + 160] == c ||
	    raster[84 * ST_W + 156] == c || raster[80 * ST_W + 160] == c)
		return;
	for (i = 32; i < 52; i++) d += raster[sight[i][1] * ST_W + sight[i][0]] == c;
	/* the forward sight (40 of 52 as before), or the rear one: its
	 * straight ticks with no diagonal ones */
	if (count[c] + d >= 40) sight_n = 52;
	else if (d <= 2) sight_n = 32;
	else return;
	sight_col = c;
	for (i = 0; i < sight_n; i++) {
		int o = sight[i][1] * ST_W + sight[i][0];
		sight_mask[o] = raster[o] == c;
	}
	hdtext_skip (sight_mask);
}

/* The HD scanner's glass is on the screen: hdstill took out most of the
 * pixels inside it. */
static int glass_shown (const unsigned char *out)
{
	int x, y, n = 0, clear = 0;

	for (y = 176; y <= 192; y += 4)
		for (x = 104; x <= 216; x += 8) {
			n++;
			clear += out[y * ST_W + x] == 255;
		}
	return clear * 2 >= n;
}

static void contacts (const unsigned char *raster, unsigned char *out)
{
	int disp = (VideoBase & 0x100000) != 0, i, match = 0;
	const struct pass *p = &passes[disp], *newest;

	if (pixel_art || !p->seq || !glass_shown (out))
		return;
	for (i = 0; i < p->n_pix; i++)
		match += raster[p->py[i] * ST_W + p->px[i]] == p->pc[i];
	if (match * 100 < p->n_pix * 30) return;
	for (i = 0; i < p->n_pix; i++) {
		int o = p->py[i] * ST_W + p->px[i];
		if (raster[o] == p->pc[i]) out[o] = 255;
	}
	newest = passes[!disp].seq > p->seq ? &passes[!disp] : p;
	for (i = 0; i < newest->n_con; i++)
		if (newest->con[i].col < 16) shown[n_shown++] = newest->con[i];
}

/* A line's own pixels: its colour, as the game drew them (hdtext has not
 * redrawn them).  It runs on across a glyph's cell (a box's edge under a
 * label) and across what another layer took out or redrew (the sight's
 * pixels, a glyph's ink): the game drew those over it. */
#define OWN(o, c)	(raster[o] == (c) && out[o] == (c))
#define ACROSS(o)	(out[o] != raster[o] || cell[o])
#define OVER(o)		(raster[o] != 255 && ACROSS (o))

/* lines of one colour in the 3D view, ending at their own pixels and at
 * least 5 of them: rows when horiz, else columns */
static int find_lines (const unsigned char *raster, const unsigned char *out,
		       const unsigned char *cell, int horiz, struct line *l)
{
	int n = 0, at, i, len = horiz ? ST_W : VIEW_H, across = horiz ? VIEW_H : ST_W;
#define AT(i) (horiz ? at * ST_W + (i) : (i) * ST_W + at)
	for (at = 0; at < across; at++)
		for (i = 0; i < len; i++) {
			int c = raster[AT (i)], j = i, k = 1, q;
			if (c == 255 || !OWN (AT (i), c)) continue;
			for (q = i + 1; q < len; q++) {
				if (OWN (AT (q), c)) {
					j = q;
					k++;
				} else if (!ACROSS (AT (q)))
					break;
			}
			if (k >= 5 && n < MAX_LINE) {
				l[n].a = l[n].ea = i;
				l[n].b = l[n].eb = j;
				while (l[n].ea > 0 && OVER (AT (l[n].ea - 1))) l[n].ea--;
				while (l[n].eb < len - 1 && OVER (AT (l[n].eb + 1))) l[n].eb++;
				l[n].at = at;
				l[n].col = c;
				l[n++].used = 0;
			}
			i = j;
		}
#undef AT
	return n;
}

/* A box's line goes: every pixel of it the game drew, end to end, but a
 * glyph's ink hdtext redrew.  One cut off by the edge of the view with no
 * corner in it is drawn again as a faint line out to the edge. */
static void box_line (const unsigned char *raster, unsigned char *out, const struct line *l,
		      int horiz, const unsigned char *boxcol)
{
	int i, len = horiz ? ST_W : VIEW_H;
	float a, b, at;

	if (!l->used && !(boxcol[l->col] && (l->ea <= 1 || l->eb >= len - 2))) return;
	for (i = l->a; i <= l->b; i++) {
		int o = horiz ? l->at * ST_W + i : i * ST_W + l->at;
		if (OWN (o, l->col)) out[o] = 255;
	}
	if (l->used || n_edges >= MAX_EDGE) return;
	a = l->ea <= 1 ? 0 : l->a;
	b = l->eb >= len - 2 ? len : l->b + 1;
	at = l->at + 0.5f;
	edges[n_edges].x0 = horiz ? a : at;
	edges[n_edges].y0 = horiz ? at : a;
	edges[n_edges].x1 = horiz ? b : at;
	edges[n_edges++].y1 = horiz ? at : b;
}

static float arm (int len)
{
	float a = 0.3f * len;
	return a > 7 ? 7 : a < 2 ? 2 : a;
}

static void boxes (const unsigned char *raster, unsigned char *out)
{
	static unsigned char cell[ST_W * VIEW_H];
	static struct line h[MAX_LINE], v[MAX_LINE];
	struct hdtext_run *runs;
	unsigned char boxcol[256], full[256][2];
	int nh, nv, i, j, eh, ev, clear = 0, n_runs;

	/* a page over a picture (a station's), not a 3D view */
	if (hdback_active ()) return;
	for (i = 0; i < ST_W * VIEW_H; i++) clear += raster[i] == 255;
	if (clear * 10 < ST_W * VIEW_H * 7) return;
	/* the cells hdtext found glyphs in */
	memset (cell, 0, sizeof (cell));
	n_runs = hdtext_runs (&runs);
	for (i = 0; i < n_runs; i++)
		for (j = 0; j < runs[i].n; j++) {
			int x0 = hdtext_glyph_x (&runs[i], j), x, y;
			for (y = runs[i].y; y <= runs[i].y + 8 && y < VIEW_H; y++)
				for (x = x0; x <= x0 + 8 && x < ST_W; x++)
					if (x >= 0 && y >= 0) cell[y * ST_W + x] = 1;
		}
	nh = find_lines (raster, out, cell, 1, h);
	nv = find_lines (raster, out, cell, 0, v);

	/* the innermost box is a glyph (the font's 8x8 square), drawn as text:
	 * hdtext has taken it out, and it gets brackets like the others */
	memset (boxcol, 0, sizeof (boxcol));
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (r->n != 1 || r->chr0 != 0x81 - 0x20 || r->y + 8 > VIEW_H || r->hide ||
		    n_brackets + 4 > MAX_BRACKET)
			continue;
		r->hide = 1;
		boxcol[r->col] = 1;
		for (j = 0; j < 4; j++) {
			brackets[n_brackets].x = r->x0 + (j & 1 ? 7.5f : 0.5f);
			brackets[n_brackets].y = r->y + (j & 2 ? 7.5f : 0.5f);
			brackets[n_brackets].h = (j & 1 ? -1 : 1) * arm (8);
			brackets[n_brackets++].v = (j & 2 ? -1 : 1) * arm (8);
		}
	}

	/* corners: an end of each meeting, within a pixel or under what the
	 * game drew over the corner (the speed strip's text) */
	for (i = 0; i < nh; i++)
		for (j = 0; j < nv; j++) {
			if (h[i].col != v[j].col) continue;
			for (eh = 0; eh < 2; eh++)
				for (ev = 0; ev < 2; ev++) {
					int h0 = eh ? h[i].b : h[i].ea, h1 = eh ? h[i].eb : h[i].a;
					int v0 = ev ? v[j].b : v[j].ea, v1 = ev ? v[j].eb : v[j].a;
					if (v[j].at < h0 - 1 || v[j].at > h1 + 1 ||
					    h[i].at < v0 - 1 || h[i].at > v1 + 1 || n_brackets >= MAX_BRACKET)
						continue;
					brackets[n_brackets].x = v[j].at + 0.5f;
					brackets[n_brackets].y = h[i].at + 0.5f;
					brackets[n_brackets].h = (eh ? -1 : 1) * arm (h[i].b - h[i].a + 1);
					brackets[n_brackets++].v = (ev ? -1 : 1) * arm (v[j].b - v[j].a + 1);
					h[i].used = v[j].used = 1;
					boxcol[h[i].col] = 1;
				}
		}
	/* a box with no corner in the view: two of its sides edge to edge */
	memset (full, 0, sizeof (full));
	for (i = 0; i < nh; i++)
		if (h[i].ea <= 1 && h[i].eb >= ST_W - 2 && ++full[h[i].col][0] >= 2) boxcol[h[i].col] = 1;
	for (i = 0; i < nv; i++)
		if (v[i].ea <= 1 && v[i].eb >= VIEW_H - 2 && ++full[v[i].col][1] >= 2) boxcol[v[i].col] = 1;

	/* the boxes' lines go, and the ones of the boxes' colour cut off by
	 * the edge of the view */
	for (i = 0; i < nh; i++) box_line (raster, out, &h[i], 1, boxcol);
	for (i = 0; i < nv; i++) box_line (raster, out, &v[i], 0, boxcol);
}
#undef OWN
#undef ACROSS
#undef OVER

/* Each tick of the sight, ST pixel edges: x0, y0, x1, y1 */
static const float ticks[8][4] = {
	{ 145, 84, 153, 85 }, { 167, 84, 175, 85 }, { 160, 69, 161, 77 }, { 160, 92, 161, 100 },
	{ 146, 70, 151, 75 }, { 169, 70, 174, 75 }, { 169, 94, 174, 99 }, { 146, 94, 151, 99 },
};

static int hits_sight (float x0, float y0, float x1, float y1)
{
	int i;
	for (i = 0; i < (sight_n == 52 ? 8 : 4); i++)
		if (x0 < ticks[i][2] && x1 > ticks[i][0] && y0 < ticks[i][3] && y1 > ticks[i][1])
			return 1;
	return 0;
}

/* A label drawn over the sight (the target's) moves up or down, the
 * least it can, to clear it. */
static void labels (void)
{
	struct hdtext_run *runs;
	int n = hdtext_runs (&runs), i, k;

	for (i = 0; i < n; i++) {
		struct hdtext_run *r = &runs[i];
		float w = r->ink1 - r->ink0, m = hdtext_measure (r), xc, x0, x1, y0, y1;

		if (r->hide || r->y + 8 >= VIEW_H) continue;
		if (m > w) w = m;
		xc = (r->ink0 + r->ink1) * 0.5f + r->dx;
		x0 = xc - w * 0.5f - 1.5f;
		x1 = xc + w * 0.5f + 1.5f;
		y0 = r->y + r->dy - 1.5f;
		y1 = r->y + r->dy + 9.5f;
		if (!hits_sight (x0, y0, x1, y1)) continue;
		for (k = 1; k <= 20; k++) {
			if (!hits_sight (x0, y0 - k, x1, y1 - k)) {
				r->dy -= k;
				break;
			}
			if (!hits_sight (x0, y0 + k, x1, y1 + k)) {
				r->dy += k;
				break;
			}
		}
	}
}

void hdinst_frame (const unsigned char *raster, unsigned char *out)
{
	int i;

	fresh = 1;
	n_shown = 0;
	n_brackets = 0;
	n_edges = 0;
	if (!on ()) return;
	contacts (raster, out);
	if (sight_col >= 0) {
		for (i = 0; i < sight_n; i++) {
			int o = sight[i][1] * ST_W + sight[i][0];
			if (raster[o] == sight_col) out[o] = 255;
		}
		labels ();
	}
	/* the boxes are told from text by where hdtext found it: under
	 * --pixel-text it finds none, and the game's boxes stay */
	if (!pixel_text) boxes (raster, out);
}

/* ---------------------------------------------------------------- */
/* drawing: smooth shapes, sized in framebuffer pixels */

static float SX = 5, SY = 6, K = 1;	/* framebuffer pixels a ST pixel; size */
static float R, G, B;

static void rgb (unsigned int c, int pal)
{
	if (pal) {	/* an RGBA palette entry, 0xAABBGGRR */
		R = (c & 255) / 255.0f;
		G = (c >> 8 & 255) / 255.0f;
		B = (c >> 16 & 255) / 255.0f;
	} else {	/* 0xRRGGBB */
		R = (c >> 16 & 255) / 255.0f;
		G = (c >> 8 & 255) / 255.0f;
		B = (c & 255) / 255.0f;
	}
}

static void put (float x, float y, float a)
{
	glColor4f (R, G, B, a);
	glVertex2f (x, ST_H - y);
}

/* A line between ST points: a core w framebuffer pixels wide at alpha a,
 * fading to nothing over f more on each side and past each end. */
static void seg (float x0, float y0, float x1, float y1, float w, float f, float a)
{
	float dx = (x1 - x0) * SX, dy = (y1 - y0) * SY, len = sqrtf (dx * dx + dy * dy);
	float ux = len > 1e-4f ? dx / len : 1, uy = len > 1e-4f ? dy / len : 0;
	float e[4], o[4], px[4][4], py[4][4], al[4][4];
	int i, j;

	e[0] = -f; e[1] = 0; e[2] = len; e[3] = len + f;
	o[0] = -w * 0.5f - f; o[1] = -w * 0.5f; o[2] = w * 0.5f; o[3] = w * 0.5f + f;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			px[i][j] = x0 + (ux * e[i] - uy * o[j]) / SX;
			py[i][j] = y0 + (uy * e[i] + ux * o[j]) / SY;
			al[i][j] = (i == 1 || i == 2) && (j == 1 || j == 2) ? a : 0;
		}
	glBegin (GL_QUADS);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			put (px[i][j], py[i][j], al[i][j]);
			put (px[i + 1][j], py[i + 1][j], al[i + 1][j]);
			put (px[i + 1][j + 1], py[i + 1][j + 1], al[i + 1][j + 1]);
			put (px[i][j + 1], py[i][j + 1], al[i][j + 1]);
		}
	glEnd ();
}

/* A round dot at an ST point: radius r framebuffer pixels at alpha a,
 * fading to nothing over f more. */
static void disc (float x, float y, float r, float f, float a)
{
	int i;
	const int n = 28;

	glBegin (GL_TRIANGLE_FAN);
	put (x, y, a);
	for (i = 0; i <= n; i++) {
		float t = i * 6.2831853f / n;
		put (x + cosf (t) * r / SX, y + sinf (t) * r / SY, a);
	}
	glEnd ();
	glBegin (GL_QUAD_STRIP);
	for (i = 0; i <= n; i++) {
		float t = i * 6.2831853f / n, c = cosf (t), s = sinf (t);
		put (x + c * r / SX, y + s * r / SY, a);
		put (x + c * (r + f) / SX, y + s * (r + f) / SY, 0);
	}
	glEnd ();
}

/* a thin line with a faint glow */
static void stroke (float x0, float y0, float x1, float y1, float a)
{
	seg (x0, y0, x1, y1, 0, 8 * K, 0.18f * a);
	seg (x0, y0, x1, y1, 2.2f * K, 1.1f, a);
}

void hdinst_draw (const unsigned int *pal, const unsigned int *ctrl_pal)
{
	GLint vp[4] = { 0, 0, 0, 0 };
	int i;

	if (!fresh) return;
	fresh = 0;
	if (!n_shown && sight_col < 0 && !n_brackets && !n_edges) return;
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] > 0 && vp[3] > 0) {
		SX = vp[2] / (float) ST_W;
		SY = vp[3] / (float) ST_H;
		K = SY / 6;
	}
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_DEPTH_TEST);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	/* the contacts, on the glass: below the plane a little dimmer */
	for (i = 0; i < n_shown; i++) {
		const struct contact *c = &shown[i];
		float t = (c->x - GLASS_X) / GLASS_RX, hh, y0, y1, d;

		if (t <= -1 || t >= 1) continue;
		hh = GLASS_RY * sqrtf (1 - t * t);
		rgb (ctrl_pal[c->col], 1);
		y0 = c->plane < c->blip ? c->plane : c->blip;
		y1 = c->plane < c->blip ? c->blip : c->plane;
		if (y0 < GLASS_Y - hh) y0 = GLASS_Y - hh;
		if (y1 > GLASS_Y + hh) y1 = GLASS_Y + hh;
		if (c->stalk && y1 > y0) {
			float a = c->below ? 0.6f : 0.95f;
			seg (c->x, y0, c->x, y1, 0, 4 * K, 0.22f * a);
			seg (c->x, y0, c->x, y1, 1.8f * K, 1.0f, a);
		}
		d = (c->blip - GLASS_Y) / GLASS_RY;
		if (c->dot && t * t + d * d <= 1) {
			disc (c->x, c->blip, 4.5f * K, 7 * K, 0.3f);
			disc (c->x, c->blip, 4.5f * K, 1.2f, 1);
		}
	}

	/* the gun sight */
	if (sight_col >= 0) {
		rgb (pal[sight_col], 1);
		stroke (145, 84.5f, 153, 84.5f, 1);
		stroke (167, 84.5f, 175, 84.5f, 1);
		stroke (160.5f, 69, 160.5f, 77, 1);
		stroke (160.5f, 92, 160.5f, 100, 1);
		if (sight_n == 52) {
			stroke (146, 70, 151, 75, 1);
			stroke (174, 70, 169, 75, 1);
			stroke (174, 99, 169, 94, 1);
			stroke (146, 99, 151, 94, 1);
		}
	}

	/* a lock box's sides with no corner in the view: faint, the
	 * brackets' weight, stopping short of the view's edge */
	rgb (C_ACCENT, 0);
	for (i = 0; i < n_edges; i++) {
		float ex = 1.1f / SX, ey = 1.1f / SY;
		seg (fmaxf (edges[i].x0, ex), fmaxf (edges[i].y0, ey),
		     fminf (edges[i].x1, ST_W - ex), fminf (edges[i].y1, VIEW_H - ey),
		     2.2f * K, 1.1f, 0.35f);
	}

	/* the lock boxes' corners: each arm starts half a line's width past
	 * the corner, so the two meet square */
	for (i = 0; i < n_brackets; i++) {
		float x = brackets[i].x, y = brackets[i].y, hw = 1.1f * K;
		float h = brackets[i].h, v = brackets[i].v;
		stroke (x - (h > 0 ? hw : -hw) / SX, y, x + h, y, 1);
		stroke (x, y - (v > 0 ? hw : -hw) / SY, x, y + v, 1);
	}
	glPopAttrib ();
}
