/*
 * hdui: the 2D pages redesigned (see hdui.h, and docs/hd/ui.md).
 *
 * Every frame hdui_frame looks at the picture the game drew and at the
 * text hdtext found on it, and decides which page it is: a station page
 * (the services, the market, the shipyard, the bulletin board, the police
 * and the video link), a ship status page (F3: equipment, profile,
 * cargo), the preferences or the start menu.  Anything else is left
 * alone.  For a page it knows it lays the page out again in the design
 * system's components:
 *
 *  - a dark base and frosted glass panels behind it all (hdui_backdrop,
 *    called just after the frame's glClear, so under everything the next
 *    frame draws);
 *  - cards, rows, buttons, check boxes, radios and pills where the game
 *    drew its tick boxes (hdui_draw_under, under the picture: the boxes'
 *    pixels are taken out of the uploaded copy);
 *  - frames and markers over the picture (hdui_draw_over);
 *  - and a role, a colour and a place for every string, which hdtext
 *    sets in the bundled fonts.
 *
 * The components are drawn with CoreGraphics into textures at the
 * window's pixel size, once, and cached.
 *
 * A control may be larger than the game's or in another place.  A click
 * on it is moved, in the host's mouse to ST conversion (hdui_mouse), to a
 * point inside the original control's click region, so the game sees a
 * click where it always did.  Nothing here writes ST memory; only the
 * uploaded copy of the picture changes, and only pixels that are still
 * exactly what the game drew.
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
#include <ctype.h>
#include <math.h>
#include "hdui.h"
#include "hdtext.h"
#include "hdback.h"
#include "hdstill.h"
#include "../m68000.h"
#include "../fixups.h"

/* screen.h's renderers (it needs SDL): only what this file uses */
enum RENDERERS { R_OLD, R_GLWIRE, R_GL, R_MAX };
extern enum RENDERERS use_renderer;
extern int fe2_bgcol;

#define ST_W		320
#define ST_H		200
#define CTRL_Y		168	/* the control panel's rows: never touched */
#define BG		0xe	/* the page colour's palette index */
#define MAX_BUTTONS	160
#define MAX_ELEMS	320
#define MAX_HOT		96

/* ---------------------------------------------------------------- */
/* The design system's tokens (0xRRGGBB) */

#define C_BG0		0x0b1016	/* the base, bottom */
#define C_BG1		0x141c26	/* the base, top */
#define C_GLASS		0x101823	/* glass panels */
#define C_CARD		0x142030	/* cards, rows and pills */
#define C_CONTROL	0x1b2838	/* small buttons */
#define C_ACCENT	0x3fd0ff	/* selection, focus, active */
#define C_AMBER		0xffb347	/* caution */
#define C_RED		0xff4d4d	/* danger */
#define C_TEXT		0xe8eef5
#define C_TEXT2		0x8fa3b8
#define C_TEXT2_SEE	0xc2d0e0	/* C_TEXT2 on frosted glass (see SEE_GLASS_A0) */

/* With a photographic backdrop under the page (hdback_active) the opaque
 * base becomes a thin wash and the panels turn to see-through glass: the
 * wash's alpha, then the glass's alpha at its top and bottom edge and the
 * cards' (the backdrop is already dimmed by hdback, so these stay low) */
#define WASH_A		0.30f
#define SEE_GLASS_A0	0.30f
#define SEE_GLASS_A1	0.22f
#define SEE_CARD_A0	0.32f
#define SEE_CARD_A1	0.24f
/* Under see-through glass and cards hdback's frost is laid first (frost_shape):
 * the picture blurred, its luma held near a fixed mid grey whatever is in it.
 * Over that, the glass is light, and C_TEXT2 (made for the dark opaque
 * glass) is lifted to C_TEXT2_SEE to keep its contrast: on these values,
 * measured on the docked pages at 1600x1200, C_TEXT stays at 5:1 or more
 * and C_TEXT2_SEE at 3.7:1 or more against the glass, over any part of the
 * picture; a hovered row's accent wash costs C_TEXT about 0.8 of that. */

/* a token as hdtext's colour (0xff000000 | 0xBBGGRR) */
static unsigned int text_rgb (unsigned int c)
{
	return 0xff000000u | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
}

/* ---------------------------------------------------------------- */
/* Pages are recognised by what is written on them: hdtext's run hashes
 * (FNV-1a of a run's letters and digits), never the text itself.  The
 * names say what the string is for. */

#define H_STATS_HEAD	0x184e0683u	/* the cargo panel's first column heading */
#define H_STATS_ROOM	0x77a4ee83u	/* its cargo capacity label */
#define H_STATS_CABINS	0xd6ac2b33u	/* its cabins label */
#define H_SVC_LAUNCH	0x2798b8cbu	/* station services: ask to launch */
#define H_SVC_SHIPYARD	0x9bc143ffu	/* station services: the shipyard */
#define H_SVC_BOARD	0x48ce91dau	/* station services: the bulletin board */
#define H_SVC_MARKET	0x7f0e7281u	/* station services: the market */
#define H_YARD_FIT	0x46f62a4cu	/* shipyard: equipment */
#define H_YARD_REPAIR	0xa38ea956u	/* shipyard: repairs */
#define H_YARD_SHIPS	0x9efd91f8u	/* shipyard: ships for sale */
#define H_YARD_POLICE	0x621e4240u	/* shipyard: the police */
#define H_MORE		0xe85541e0u	/* a list's next page */
#define H_SHIPS_HEAD	0xf45abe3eu	/* ships for sale: the buy and view columns' heading */
#define H_REPAIR_HEAD	0x29520bb4u	/* repairs: the list's heading */
#define H_REPLY_HEAD	0xc32da9c8u	/* a conversation's answers heading */
#define H_START_TITLE	0x0f9ffb64u	/* the start menu's title */
#define H_CASH		0x962530d2u	/* the cargo panel's cash label */
#define H_FILE_UP	0xc908770cu	/* a file page's first entry: the parent directory */
#define H_DOCKED	0xf5e33b2bu	/* the console's flight status: docked */
#define H_LANDED	0xbb34a54du	/* ... landed */

static const unsigned int prefs_heads[] = {
	0xdfb0e074u, 0x0662faa5u, 0x9f9c4fd4u, 0xfa2e3895u, 0xd9893ebfu
};

/* ---------------------------------------------------------------- */
/* What the last picture held, and the layout made of it */

struct button {
	short x, y;		/* the box's top left, ST pixels */
	unsigned char face, hi, edge, pressed;
	unsigned char used;	/* the layout replaced it */
};

enum { P_NONE, P_STATION, P_F3, P_PREFS, P_START, P_OTHER, P_DOCK, P_FILE, P_INFO };
static const char *const page_name[] = { "none", "station", "f3", "prefs", "start", "other", "dock", "file", "info" };

/* layers */
enum { L_BACK, L_UNDER, L_OVER };

/* components */
enum { E_QUAD, E_GLASS, E_CARD, E_ROW, E_BTN, E_CHECK, E_RADIO, E_PILL, E_FRAME, E_RULE, E_ICON };

/* line icons */
enum { I_NONE, I_LAUNCH, I_SHIP, I_DOC, I_MARKET, I_SHIELD, I_CHIP, I_WRENCH, I_SWAP,
       I_BACK, I_MORE, I_PLUS, I_MINUS, I_EYE, I_CHEVRON, I_CHECK, I_DROP, I_SPEECH, I_EJECT,
       I_STATION, I_UP, I_GUN };

/* element flags */
#define F_TL		1	/* glass: rounded corners */
#define F_TR		2
#define F_BR		4
#define F_BL		8
#define F_ALL		15
#define F_CHEVRON	16	/* card: a chevron on the right */
#define F_ON		32	/* check box, radio: set */
#define F_RING		64	/* card: a radio ring instead of an icon tile */
#define F_HOVERONLY	128	/* row: drawn only while the pointer is on it */
#define F_NOICON	256	/* pill: the game's own icon shows in its slot */
#define F_BIG		512	/* card: a tile of a grid, a large icon over its label */
#define F_ROUND		1024	/* button: a circle */
#define F_OFF		2048	/* button: not there now (dimmed, no hover) */
#define F_VIEW		4096	/* glass: a 3D view's card, darkened to its edges */
#define F_BLINK		8192	/* quad: blinks (a caret) */

struct elem {
	unsigned char kind, layer, icon, state;	/* state: 0, 1 hover, 2 pressed */
	unsigned short flags;
	short hot;		/* its entry in hot[], or -1 */
	float x0, y0, x1, y1;	/* ST pixels, y down */
	float a, b, c;		/* glass: header band height; card, pill, button: icon centre x, y and size (ST) */
	unsigned int rgb;	/* quad: colour */
	float alpha;		/* quad: alpha */
	unsigned char pressed;	/* the game shows it pressed */
};

/* a place the pointer can be on, and where a click there goes */
struct hot {
	float x0, y0, x1, y1;	/* ST pixels */
	short tx, ty;		/* a point inside the original control */
	short remap;
	unsigned int chain;	/* then a click on the services page's control of this label (H_SVC_*) */
};

static struct button btn[MAX_BUTTONS];
static int n_btn;
static struct elem el[MAX_ELEMS];
static int n_el;
static struct hot hot[MAX_HOT], live[MAX_HOT];
static int n_hot, n_live;
static int page;
static unsigned int page_bg;		/* the page colour it was laid out for */
static unsigned long long page_key;	/* what the layout is, without its states */
static unsigned long long back_drawn;	/* the layout the backdrop of this frame was drawn for */
static int stable;			/* frames the layout has been the same */
static struct hdtext_run *runs;
static int n_runs;

/* the pointer, ST pixels, before any remap */
static int ptr_x = -1, ptr_y = -1, ptr_buttons;

/* the clear areas (the 3D views in a page) of this frame */
static int ca_n, ca[16][4], fr_ca_n, fr_ca[16][4];

static unsigned long long mix_hash (unsigned long long h, const void *p, size_t n)
{
	const unsigned char *b = p;
	while (n--) h = (h ^ *b++) * 1099511628211ULL;
	return h;
}

#define FNV0	1469598103934665603ULL

/* the layer works in the GL renderer, with the new type, unless switched off */
static int layer_on (void)
{
#ifdef __APPLE__
	return !classic_ui && use_renderer == R_GL;
#else
	return 0;
#endif
}

int hdui_layer_on (void)
{
	return layer_on () && !pixel_text;
}

/* FE2_HDUI_DUMP=dir: write each new picture (the screen, its palettes and
 * the copy to be uploaded) there, for looking at a page offline */
static void dump (const unsigned char *raster, const unsigned char *out,
		  const unsigned int *pal, const unsigned int *ctrl_pal)
{
	static const char *dir;
	static int checked, frame, last_frame = -1000, n;
	static unsigned long long last;
	unsigned long long h = FNV0;
	char path[1024];
	FILE *f;

	if (!checked) {
		checked = 1;
		dir = getenv ("FE2_HDUI_DUMP");
	}
	frame++;
	if (!dir) return;
	h = mix_hash (h, raster, ST_W * ST_H);
	h = mix_hash (h, out, ST_W * ST_H);
	h = mix_hash (h, pal, 256 * 4);
	if (h == last || frame - last_frame < 10) return;
	last = h;
	last_frame = frame;
	snprintf (path, sizeof (path), "%s/d%04d-f%06d.raw", dir, n++, frame);
	f = fopen (path, "wb");
	if (!f) return;
	fwrite (raster, 1, ST_W * ST_H, f);
	fwrite (pal, 4, 256, f);
	fwrite (ctrl_pal, 4, 16, f);
	fwrite (out, 1, ST_W * ST_H, f);
	fclose (f);
}

/* FE2_HDUI_LOG=1: each new page's boxes, runs and clear areas on stderr
 * (hashes and places only), for finding pages */
static void page_log (const unsigned char *out, const unsigned int *pal)
{
	static unsigned long long last;
	static int checked, on;
	unsigned long long h = FNV0;
	int i;

	if (!checked) {
		checked = 1;
		on = getenv ("FE2_HDUI_LOG") != 0;
	}
	if (!on) return;
	h = mix_hash (h, &page, sizeof (page));
	h = mix_hash (h, btn, n_btn * sizeof (btn[0]));
	for (i = 0; i < n_runs; i++) {
		h = mix_hash (h, &runs[i], 16);
		h = mix_hash (h, &runs[i].hash, 4);
	}
	h = mix_hash (h, fr_ca, fr_ca_n * sizeof (fr_ca[0]));
	if (h == last) return;
	last = h;
	fprintf (stderr, "HDUI page %s bg=%d bgrgb=%06x btn=%d runs=%d clear=%d elems=%d hot=%d\n",
		 page_name[page], fe2_bgcol, pal[BG] & 0xffffff, n_btn, n_runs, fr_ca_n, n_el, n_hot);
	for (i = 0; i < fr_ca_n; i++)
		fprintf (stderr, "  C %d %d %d %d\n", fr_ca[i][0], fr_ca[i][1], fr_ca[i][2], fr_ca[i][3]);
	for (i = 0; i < n_btn; i++)
		fprintf (stderr, "  B %d %d face=%06x p=%d\n", btn[i].x, btn[i].y,
			 pal[btn[i].face] & 0xffffff, btn[i].pressed);
	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		fprintf (stderr, "  R %3d %3d y=%3d ink=%3d..%3d col=%3d k=%02x h=%08x L=%d c0=%02x\n",
			 r->x0, r->x1, r->y, r->ink0, r->ink1, r->col, r->kind, r->hash, r->letters, r->chr0);
	}
	for (i = 0; i < n_hot; i++)
		fprintf (stderr, "  H %.1f %.1f %.1f %.1f -> %d %d%s\n", hot[i].x0, hot[i].y0, hot[i].x1,
			 hot[i].y1, hot[i].tx, hot[i].ty, hot[i].remap ? "" : " (hover)");
	{
		/* opaque non-text blobs: the rows they cover */
		int y, x, c0, c1, y0 = -1;
		for (y = 0; y < CTRL_Y; y++) {
			c0 = c1 = 0;
			for (x = 0; x < ST_W; x++)
				if (out[y * ST_W + x] != 255) {
					if (x < 160) c0++;
					else c1++;
				}
			if ((c0 >= 40 || c1 >= 40) && y0 < 0) y0 = y;
			if (!(c0 >= 40 || c1 >= 40) && y0 >= 0) {
				fprintf (stderr, "  P rows %d..%d\n", y0, y - 1);
				y0 = -1;
			}
		}
		if (y0 >= 0) fprintf (stderr, "  P rows %d..%d\n", y0, CTRL_Y - 1);
	}
}

int hdui_clear_area (int x1, int y1, int x2, int y2)
{
	int i;

	if (ca_n < 16) {
		ca[ca_n][0] = x1;
		ca[ca_n][1] = y1;
		ca[ca_n][2] = x2;
		ca[ca_n][3] = y2;
		ca_n++;
	}
	/* a view the backdrop under this frame already gave its card: the
	 * card is its background, not black */
	if (!back_drawn || back_drawn != page_key) return 0;
	for (i = 0; i < n_el; i++)
		if (el[i].kind == E_GLASS && (el[i].flags & F_VIEW) && el[i].x0 <= x1 + 2 && el[i].y0 <= y1 + 2 &&
		    el[i].x1 >= x2 - 2 && el[i].y1 >= y2 - 2)
			return 1;
	return 0;
}

/* ---------------------------------------------------------------- */
/* Finding the game's controls */

#define R(x, y)	r[(y) * ST_W + (x)]

/* The game's tick box with its top left at (x, y): a 9x8 square, its top
 * and left edges in one colour, a 6x6 face in another, its right and
 * bottom inner edges in a third, a black shadow one or two pixels deep
 * down and right, in a column of the box's colour on its left. */
static int is_button (const unsigned char *r, int x, int y, struct button *b)
{
	int i, j, face = R (x + 2, y + 1), hi = R (x + 2, y), edge = R (x + 8, y + 1), box = R (x, y);

	if (face == 255 || hi == 255 || edge == 255 || box == 255 ||
	    hi == face || edge == face || hi == edge)
		return 0;
	for (j = y + 1; j <= y + 6; j++)
		for (i = x + 2; i <= x + 7; i++)
			if (R (i, j) != face) return 0;
	for (i = x + 3; i <= x + 7; i++)
		if (R (i, y) != hi) return 0;
	for (j = y + 1; j <= y + 6; j++)
		if (R (x + 1, j) != hi || R (x + 8, j) != edge || R (x + 9, j) != 0) return 0;
	for (i = x + 2; i <= x + 8; i++)
		if (R (i, y + 7) != edge) return 0;
	if (R (x + 9, y + 7) != 0) return 0;
	for (i = x + 2; i <= x + 9; i++)
		if (R (i, y + 8) != 0) return 0;
	for (j = y; j <= y + 9; j++)
		if (R (x, j) != box) return 0;
	b->x = x;
	b->y = y;
	b->face = face;
	b->hi = hi;
	b->edge = edge;
	/* a raised box has a second column of shadow; a pressed one not */
	b->pressed = R (x + 10, y + 3) != 0;
	b->used = 0;
	return 1;
}

/* Take the box out of the picture: its own pixels, and round it those in
 * the page colour (which the backdrop now paints).  Only pixels still as
 * the game drew them. */
static void take_out (const unsigned char *r, unsigned char *out, const struct button *b)
{
	int i, j, box = R (b->x, b->y);

	/* the game clears a 16x11 square in the box colour, then draws the box */
	for (j = b->y; j <= b->y + 10 && j < CTRL_Y; j++) {
		for (i = b->x; i <= b->x + 15 && i < ST_W; i++) {
			int o = j * ST_W + i, p = r[o];
			int own;
			/* where a letter crossed the square the text layer has put
			 * back the square's colour: that goes too */
			if (out[o] == BG) {
				out[o] = 255;
				continue;
			}
			own = i <= b->x + 10 && j <= b->y + 9 && (p == box || p == b->hi || p == b->face ||
						      p == b->edge || p == 0 ||
						      (j == b->y && (i == b->x + 1 || i == b->x + 8)) ||
						      (j == b->y + 7 && i == b->x + 1));
			if (out[o] != p) continue;
			if (p == BG) {
				out[o] = 255;
				continue;
			}
			if (!own || (p == box && box != BG)) continue;
			out[o] = 255;
		}
	}
}

/* The game's arrows for paging a list: 16x10 pictures in the page colour
 * (e), a light (f) and black (0), drawn at the foot of the right of a page
 * where a list continues.  The down arrow (the next rows) shows while
 * there are more below, the up arrow (the rows before) 12 pixels to its
 * right once the list has moved on; the up arrow's left edge overlaps the
 * down arrow's padding, so only the down arrow's first 12 columns are
 * matched. */
static const char *const more_arrow[10] = {
	"eeffffffeeeeeeee",
	"eefeeee0eeeeeeee",
	"eefeeee0eeeeeeee",
	"eefeeee0eeeeeeee",
	"fffeeee0ffeeeeee",
	"0eeeeeeee0eeeeee",
	"e0eeeeee00eeeeee",
	"ee0eeee00eeeeeee",
	"eee0ee00eeeeeeee",
	"eeee000eeeeeeeee",
};

static const char *const less_arrow[10] = {
	"eeeeffeeeeeeeeee",
	"eeefee0eeeeeeeee",
	"eefeeee0eeeeeeee",
	"efeeeeee0eeeeeee",
	"feeeeeeee0eeeeee",
	"e00eeee000eeeeee",
	"eefeeee0eeeeeeee",
	"eefeeee0eeeeeeee",
	"eefeeee0eeeeeeee",
	"eee00000eeeeeeee",
};

#define MORE_UP		12	/* the up arrow's x from the down arrow's */

static int arrow_at (const unsigned char *r, const char *const *pic, int w, int x, int y)
{
	int i, j, light = -1;

	if (x < 0 || y < 0 || x + 16 > ST_W || y + 10 > CTRL_Y) return 0;
	for (j = 0; j < 10; j++)
		for (i = 0; i < w; i++) {
			int p = R (x + i, y + j), t = pic[j][i];
			if (t == 'e' && p != BG) return 0;
			if (t == '0' && p != 0) return 0;
			if (t == 'f') {
				if (light < 0) light = p;
				if (p != light || p == BG || p == 0 || p == 255) return 0;
			}
		}
	return 1;
}

#define MORE_DOWN	1	/* the list goes on below */
#define MORE_BACK	2	/* and back above */

/* Which of the arrows show near the foot of the right of a page (0 if
 * neither), and where the down arrow's cell is (or would be) */
static int find_more (const unsigned char *r, int *mx, int *my)
{
	int x, y;

	for (y = 148; y + 10 <= CTRL_Y; y++)
		for (x = 118; x + 16 <= ST_W; x++) {
			int p = R (x + 2, y), q = R (x + 4, y);
			if (p != BG && p != 255 && arrow_at (r, more_arrow, 12, x, y)) {
				*mx = x;
				*my = y;
				return MORE_DOWN | (arrow_at (r, less_arrow, 16, x + MORE_UP, y) ? MORE_BACK : 0);
			}
			if (q != BG && q != 255 && x >= 118 + MORE_UP && arrow_at (r, less_arrow, 16, x, y)) {
				*mx = x - MORE_UP;
				*my = y;
				return MORE_BACK;
			}
		}
	return 0;
}

/* take the arrows that show out of the picture, where it is still as the
 * game drew it */
static void take_out_more (const unsigned char *r, unsigned char *out, int x, int y, int more)
{
	int i, j, x0 = more & MORE_DOWN ? x : x + MORE_UP, x1 = more & MORE_BACK ? x + MORE_UP + 16 : x + 16;

	for (j = 0; j < 10; j++)
		for (i = x0; i < x1; i++) {
			int o = (y + j) * ST_W + i;
			if (out[o] == r[o]) out[o] = 255;
		}
}

/* Where the text layer painted a letter out in the page colour (the
 * colour it found round it, near a box's square), the backdrop shows
 * instead: on these pages the page colour is the backdrop's now */
static void text_bg_out (const unsigned char *r, unsigned char *out)
{
	int o;

	for (o = 0; o < CTRL_Y * ST_W; o++)
		if (out[o] == BG && r[o] != BG) out[o] = 255;
}

static void find_buttons (const unsigned char *r)
{
	int x, y;

	n_btn = 0;
	for (y = 0; y + 9 < CTRL_Y; y++)
		for (x = 0; x + 10 < ST_W; x++)
			if (n_btn < MAX_BUTTONS && R (x + 2, y + 1) != 255 &&
			    is_button (r, x, y, &btn[n_btn]))
				n_btn++;
}

/* is the box set: shown pressed, or its face a red (the preferences) */
static int box_on (const struct button *b, const unsigned int *pal)
{
	unsigned int c = pal[b->face];
	int red = c & 0xff, green = (c >> 8) & 0xff;
	return b->pressed || (red > 0x60 && red > 2 * green);
}

/* The bounds of the opaque picture (a bitmap: a portrait) in a box, ST
 * pixels, its last row and column included in them; 0 if there is none.
 * The picture is the first band of rows each with something wide in it:
 * the search goes on to its foot, and stops there. */
static int find_blob (const unsigned char *out, int x0, int y0, int x1, int y1, float *bb)
{
	int x, y, lx = ST_W, ly = ST_H, hx = -1, hy = -1;

	for (y = y0; y <= y1; y++) {
		int n = 0, a = ST_W, b = -1;
		for (x = x0; x <= x1; x++)
			if (out[y * ST_W + x] != 255) {
				n++;
				if (x < a) a = x;
				b = x;
			}
		if (n < 24) {
			if (hy >= 0) break;
			continue;
		}
		if (y < ly) ly = y;
		hy = y;
		if (a < lx) lx = a;
		if (b > hx) hx = b;
	}
	if (hy - ly < 24 || hx - lx < 24) return 0;
	bb[0] = lx;
	bb[1] = ly;
	bb[2] = hx + 1;
	bb[3] = hy + 1;
	return 1;
}

/* ---------------------------------------------------------------- */
/* Drawing: framebuffer pixels, y down */

static int fbx, fby;	/* the picture's bottom-left corner in the framebuffer */
static int fbw = 1600, fbh = 1200;	/* the picture: the game's 4:3 screen, no letterbox bars */
float hdui_face_top;	/* hdface.c: where the card's portrait starts, 0 for none */
static float sx = 5, sy = 6, uu = 1;	/* pixels per ST pixel; the design unit (a pixel at 1200 high) */
static unsigned int frame_no;

static float PX (float x) { return floorf (x * sx + 0.5f); }
static float PY (float y) { return floorf (y * sy + 0.5f); }

/* Set up to draw in framebuffer pixels over the game's picture (the
 * whole window, or the 4:3 part of it between letterbox bars), whatever
 * viewport and matrices the caller has; end_2d puts it all back. */
static void begin_2d (void)
{
	gl_picture_rect (&fbx, &fby, &fbw, &fbh);
	if (fbw < 32 || fbh < 32) fbx = fby = 0, fbw = 1600, fbh = 1200;
	sx = fbw / (float) ST_W;
	sy = fbh / (float) ST_H;
	uu = fbh / 1200.0f;

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT | GL_TEXTURE_BIT |
		      GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT | GL_POLYGON_BIT |
		      GL_SCISSOR_BIT);
	glViewport (fbx, fby, fbw, fbh);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, fbw, fbh, 0, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_FALSE);
	glDisable (GL_CULL_FACE);
	glDisable (GL_LIGHTING);
	glDisable (GL_ALPHA_TEST);
	glDisable (GL_FOG);
	/* never over the control panel's rows */
	glEnable (GL_SCISSOR_TEST);
	glScissor (fbx, fby + fbh - (int) PY (CTRL_Y), fbw, (int) PY (CTRL_Y));
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	glDisable (GL_TEXTURE_2D);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);	/* colours are premultiplied */
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void end_2d (void)
{
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
}

static void colour (unsigned int rgb, float a)
{
	glColor4f (((rgb >> 16) & 0xff) / 255.0f * a, ((rgb >> 8) & 0xff) / 255.0f * a,
		   (rgb & 0xff) / 255.0f * a, a);
}

/* a flat rectangle, framebuffer pixels */
static void rect_px (float x0, float y0, float x1, float y1, unsigned int rgb, float a)
{
	colour (rgb, a);
	glBegin (GL_TRIANGLE_STRIP);
	glVertex2f (x0, y0);
	glVertex2f (x1, y0);
	glVertex2f (x0, y1);
	glVertex2f (x1, y1);
	glEnd ();
}

/* ---------------------------------------------------------------- */
/* Components, painted with CoreGraphics into cached textures */

#ifdef __APPLE__

struct paint {
	int kind, state, icon, flags;
	int see;		/* a photographic backdrop is under the page: thinner glass */
	int w, h, pad;		/* the element and the room round it for shadows, pixels */
	float a, b, c;		/* glass: header band height; others: icon centre x, y, size (pixels from the element's top left) */
	float u;
};

struct sprite {
	unsigned long long key;
	GLuint tex;
	int w, h, tw, th;	/* the picture, its texture */
	int pad;
	unsigned int used;	/* the frame it was last drawn in */
};

#define MAX_SPRITES	192
static struct sprite spr[MAX_SPRITES];
static int n_spr, npot = -1;
static CGColorSpaceRef rgb_space;

static CGFloat CR (unsigned int c) { return ((c >> 16) & 0xff) / 255.0; }
static CGFloat CG_ (unsigned int c) { return ((c >> 8) & 0xff) / 255.0; }
static CGFloat CB (unsigned int c) { return (c & 0xff) / 255.0; }

static void fill_col (CGContextRef g, unsigned int c, float a)
{
	CGContextSetRGBFillColor (g, CR (c), CG_ (c), CB (c), a);
}

static void stroke_col (CGContextRef g, unsigned int c, float a)
{
	CGContextSetRGBStrokeColor (g, CR (c), CG_ (c), CB (c), a);
}

/* a rectangle with the corners in flags rounded */
static CGPathRef rrect (float x, float y, float w, float h, float r, int corners)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	float tl = corners & F_TL ? r : 0, tr = corners & F_TR ? r : 0;
	float br = corners & F_BR ? r : 0, bl = corners & F_BL ? r : 0;

	CGPathMoveToPoint (p, NULL, x + tl, y);
	CGPathAddLineToPoint (p, NULL, x + w - tr, y);
	if (tr > 0) CGPathAddArcToPoint (p, NULL, x + w, y, x + w, y + tr, tr);
	CGPathAddLineToPoint (p, NULL, x + w, y + h - br);
	if (br > 0) CGPathAddArcToPoint (p, NULL, x + w, y + h, x + w - br, y + h, br);
	CGPathAddLineToPoint (p, NULL, x + bl, y + h);
	if (bl > 0) CGPathAddArcToPoint (p, NULL, x, y + h, x, y + h - bl, bl);
	CGPathAddLineToPoint (p, NULL, x, y + tl);
	if (tl > 0) CGPathAddArcToPoint (p, NULL, x, y, x + tl, y, tl);
	CGPathCloseSubpath (p);
	return p;
}

static CGPathRef circle (float cx, float cy, float r)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	CGPathAddEllipseInRect (p, NULL, CGRectMake (cx - r, cy - r, 2 * r, 2 * r));
	return p;
}

/* a vertical gradient over the clip */
static void vgrad (CGContextRef g, float y0, float y1, unsigned int c0, float a0, unsigned int c1, float a1)
{
	CGFloat comp[8] = { CR (c0), CG_ (c0), CB (c0), a0, CR (c1), CG_ (c1), CB (c1), a1 };
	CGFloat loc[2] = { 0, 1 };
	CGGradientRef gr = CGGradientCreateWithColorComponents (rgb_space, comp, loc, 2);

	CGContextDrawLinearGradient (g, gr, CGPointMake (0, y0), CGPointMake (0, y1),
				     kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
	CGGradientRelease (gr);
}

/* a soft shadow, or a glow, of path round it and not in it */
static void halo (CGContextRef g, CGPathRef p, float dy, float blur, unsigned int c, float a)
{
	CGFloat comp[4] = { CR (c), CG_ (c), CB (c), a };
	CGColorRef col = CGColorCreate (rgb_space, comp);

	CGContextSaveGState (g);
	CGContextSetShadowWithColor (g, CGSizeMake (0, -dy), blur, col);
	CGContextAddPath (g, p);
	fill_col (g, 0, 1);
	CGContextFillPath (g);
	CGContextRestoreGState (g);
	CGContextSaveGState (g);
	CGContextSetBlendMode (g, kCGBlendModeClear);
	CGContextAddPath (g, p);
	CGContextFillPath (g);
	CGContextRestoreGState (g);
	CGColorRelease (col);
}

/* a line lw wide just inside path's edge, its colour fading from a0 at y0 to a1 at y1 */
static void inner_edge (CGContextRef g, CGPathRef p, float lw, float y0, float y1,
			unsigned int c, float a0, float a1)
{
	CGContextSaveGState (g);
	CGContextAddPath (g, p);
	CGContextSetLineWidth (g, 2 * lw);
	CGContextReplacePathWithStrokedPath (g);
	CGContextClip (g);
	CGContextAddPath (g, p);
	CGContextClip (g);
	vgrad (g, y0, y1, c, a0, c, a1);
	CGContextRestoreGState (g);
}

static void fill_path (CGContextRef g, CGPathRef p, unsigned int c, float a)
{
	CGContextAddPath (g, p);
	fill_col (g, c, a);
	CGContextFillPath (g);
}

static void stroke_path (CGContextRef g, CGPathRef p, float lw, unsigned int c, float a)
{
	CGContextAddPath (g, p);
	CGContextSetLineWidth (g, lw);
	stroke_col (g, c, a);
	CGContextStrokePath (g);
}

/* The line icons, drawn on a 24 unit square centred at (cx, cy) s pixels
 * across, lines lw pixels wide. */
static void icon (CGContextRef g, int which, float cx, float cy, float s, float lw, unsigned int c, float a)
{
	float k = s / 24;

	if (which == I_NONE) return;
	CGContextSaveGState (g);
	CGContextTranslateCTM (g, cx - s / 2, cy - s / 2);
	CGContextScaleCTM (g, k, k);
	CGContextSetLineWidth (g, lw / k);
	CGContextSetLineCap (g, kCGLineCapRound);
	CGContextSetLineJoin (g, kCGLineJoinRound);
	stroke_col (g, c, a);
	fill_col (g, c, a);
#define M(x, y)	CGContextMoveToPoint (g, x, y)
#define L(x, y)	CGContextAddLineToPoint (g, x, y)
	switch (which) {
	case I_LAUNCH:	/* up and away from a pad */
		M (12, 17); L (12, 3.5);
		M (6.5, 9); L (12, 3.5); L (17.5, 9);
		M (4, 21); L (20, 21);
		M (7, 17.5); L (7, 21);
		M (17, 17.5); L (17, 21);
		break;
	case I_SHIP:	/* a ship from above */
		M (12, 2.5); L (19.5, 20.5); L (12, 16.5); L (4.5, 20.5);
		CGContextClosePath (g);
		M (12, 8); L (12, 13);
		break;
	case I_DOC:	/* a notice */
		M (6, 3); L (14.5, 3); L (19, 7.5); L (19, 21); L (6, 21);
		CGContextClosePath (g);
		M (14.5, 3); L (14.5, 7.5); L (19, 7.5);
		M (9.5, 11.5); L (15.5, 11.5);
		M (9.5, 15); L (15.5, 15);
		M (9.5, 18.5); L (13, 18.5);
		break;
	case I_MARKET:	/* a rising trend */
		M (3.5, 17); L (9, 11); L (13, 14.5); L (20.5, 6.5);
		M (15, 6.5); L (20.5, 6.5); L (20.5, 12);
		M (3.5, 21); L (20.5, 21);
		break;
	case I_SHIELD:
		M (12, 2.5); L (19.5, 5.5); L (19.5, 11.5);
		CGContextAddCurveToPoint (g, 19.5, 16.5, 16, 19.5, 12, 21.5);
		CGContextAddCurveToPoint (g, 8, 19.5, 4.5, 16.5, 4.5, 11.5);
		L (4.5, 5.5);
		CGContextClosePath (g);
		M (8.5, 12); L (11, 14.5); L (15.5, 9.5);
		break;
	case I_CHIP:	/* a module */
		CGContextAddRect (g, CGRectMake (7, 7, 10, 10));
		CGContextAddRect (g, CGRectMake (10, 10, 4, 4));
		M (10, 3.5); L (10, 7); M (14, 3.5); L (14, 7);
		M (10, 17); L (10, 20.5); M (14, 17); L (14, 20.5);
		M (3.5, 10); L (7, 10); M (3.5, 14); L (7, 14);
		M (17, 10); L (20.5, 10); M (17, 14); L (20.5, 14);
		break;
	case I_WRENCH:	/* an open-end spanner: a jaw, its slot facing away
			 * from the handle, and the handle */
		{
			float cx = 15, cy = 9, R = 6, w = 2.6f, ph = -0.785398f, a = asinf (w / R);
			float ux = cosf (ph), uy = sinf (ph), nx = -uy, ny = ux, t = sqrtf (R * R - w * w);
			M (cx + R * cosf (ph + a), cy + R * sinf (ph + a));
			CGContextAddArc (g, cx, cy, R, ph + a, ph + 2 * 3.141593f - a, 0);
			L (cx - w * nx, cy - w * ny);
			CGContextAddArc (g, cx, cy, w, ph - 1.570796f, ph - 4.712389f, 1);
			L (cx + t * ux + w * nx, cy + t * uy + w * ny);
			M (cx - R * ux, cy - R * uy); L (4, 20);
		}
		break;
	case I_SWAP:	/* one ship for another */
		M (4, 8); L (19.5, 8);
		M (15.5, 4); L (19.5, 8); L (15.5, 12);
		M (20, 16); L (4.5, 16);
		M (8.5, 12); L (4.5, 16); L (8.5, 20);
		break;
	case I_BACK:
		M (19.5, 12); L (4.5, 12);
		M (10.5, 6); L (4.5, 12); L (10.5, 18);
		break;
	case I_MORE:
		M (6, 9); L (12, 15); L (18, 9);
		break;
	case I_UP:
		M (6, 15); L (12, 9); L (18, 15);
		break;
	case I_PLUS:
		M (12, 5); L (12, 19); M (5, 12); L (19, 12);
		break;
	case I_MINUS:
		M (5, 12); L (19, 12);
		break;
	case I_EYE:
		M (2.5, 12);
		CGContextAddQuadCurveToPoint (g, 12, 2, 21.5, 12);
		CGContextAddQuadCurveToPoint (g, 12, 22, 2.5, 12);
		CGContextClosePath (g);
		CGContextAddEllipseInRect (g, CGRectMake (9, 9, 6, 6));
		break;
	case I_CHEVRON:
		M (9, 5.5); L (15.5, 12); L (9, 18.5);
		break;
	case I_CHECK:
		M (5, 12.5); L (10, 17.5); L (19, 7);
		break;
	case I_GUN:	/* a turret on its deck, the barrel raised */
		M (3.5, 20.5); L (20.5, 20.5);
		M (6, 20.5); L (6, 16);
		CGContextAddArc (g, 11, 16, 5, 3.141593f, 2 * 3.141593f, 0);
		L (16, 20.5);
		M (13.5, 12); L (20, 5.5);
		break;
	case I_DROP:	/* fuel */
		M (12, 3);
		CGContextAddCurveToPoint (g, 14, 6.5, 18.5, 10.5, 18.5, 14.5);
		CGContextAddCurveToPoint (g, 18.5, 18.5, 15.5, 21, 12, 21);
		CGContextAddCurveToPoint (g, 8.5, 21, 5.5, 18.5, 5.5, 14.5);
		CGContextAddCurveToPoint (g, 5.5, 10.5, 10, 6.5, 12, 3);
		CGContextClosePath (g);
		break;
	case I_SPEECH:
		M (6.5, 4.5); L (17.5, 4.5);
		CGContextAddArcToPoint (g, 20.5, 4.5, 20.5, 7.5, 3);
		L (20.5, 13);
		CGContextAddArcToPoint (g, 20.5, 16, 17.5, 16, 3);
		L (12, 16); L (7.5, 20); L (7.5, 16); L (6.5, 16);
		CGContextAddArcToPoint (g, 3.5, 16, 3.5, 13, 3);
		L (3.5, 7.5);
		CGContextAddArcToPoint (g, 3.5, 4.5, 6.5, 4.5, 3);
		CGContextClosePath (g);
		break;
	case I_EJECT:
		M (12, 4.5); L (19, 13); L (5, 13);
		CGContextClosePath (g);
		M (5, 18); L (19, 18);
		break;
	case I_STATION:	/* a ring station end on, its docking slot lit */
		CGContextAddEllipseInRect (g, CGRectMake (2.5, 2.5, 19, 19));
		CGContextAddEllipseInRect (g, CGRectMake (6.5, 6.5, 11, 11));
		CGContextAddRect (g, CGRectMake (9.5, 10.75, 5, 2.5));
		M (12, 2.5); L (12, 6.5);
		M (12, 17.5); L (12, 21.5);
		M (2.5, 12); L (6.5, 12);
		M (17.5, 12); L (21.5, 12);
		break;
	}
#undef M
#undef L
	CGContextStrokePath (g);
	CGContextRestoreGState (g);
}

static void paint_glass (CGContextRef g, const struct paint *p)
{
	float u = p->u, w = p->w, h = p->h;
	CGPathRef path = rrect (0, 0, w, h, 8 * u, p->flags & F_ALL);

	halo (g, path, 4 * u, 18 * u, 0, 0.45f);
	CGContextSaveGState (g);
	CGContextAddPath (g, path);
	CGContextClip (g);
	if (p->see) vgrad (g, 0, h, C_GLASS, SEE_GLASS_A0, C_GLASS, SEE_GLASS_A1);
	else vgrad (g, 0, h, C_GLASS, 0.84f, C_GLASS, 0.72f);
	if (p->flags & F_VIEW) {
		/* under a 3D view: darker to the edges, the model in the light */
		CGFloat comp[8] = { 0, 0, 0, 0.18, 0, 0, 0, 0.62 }, loc[2] = { 0, 1 };
		CGGradientRef gr = CGGradientCreateWithColorComponents (rgb_space, comp, loc, 2);
		CGContextDrawRadialGradient (g, gr, CGPointMake (w / 2, h / 2), 0, CGPointMake (w / 2, h / 2),
					     sqrtf (w * w + h * h) / 2, kCGGradientDrawsAfterEndLocation);
		CGGradientRelease (gr);
	}
	/* a sheen at the top */
	vgrad (g, 0, h * 0.35f, 0xffffff, 0.055f, 0xffffff, 0);
	if (p->a > 0) {
		fill_col (g, 0xffffff, 0.035f);
		CGContextFillRect (g, CGRectMake (0, 0, w, p->a));
		fill_col (g, 0xffffff, 0.10f);
		CGContextFillRect (g, CGRectMake (0, p->a - u, w, u));
	}
	CGContextRestoreGState (g);
	inner_edge (g, path, u, 0, h, 0xffffff, 0.16f, 0.05f);
	CGPathRelease (path);
}

/* a tile of a grid: frosted, a large line icon over its label; the one
 * under the pointer outlined and glowing in the accent */
static void paint_big_card (CGContextRef g, const struct paint *p)
{
	float u = p->u, w = p->w, h = p->h;
	int hov = p->state >= 1, prs = p->state == 2;
	CGPathRef path = rrect (0, 0, w, h, 10 * u, F_ALL), in;

	halo (g, path, 4 * u, 18 * u, 0, 0.45f);
	if (hov) halo (g, path, 0, 22 * u, C_ACCENT, prs ? 0.75f : 0.55f);
	CGContextSaveGState (g);
	CGContextAddPath (g, path);
	CGContextClip (g);
	vgrad (g, 0, h, 0x1a2a3c, 0.80f, C_CARD, 0.70f);
	vgrad (g, 0, h * 0.45f, 0xffffff, hov ? 0.10f : 0.07f, 0xffffff, 0);
	if (hov) vgrad (g, 0, h, C_ACCENT, prs ? 0.22f : 0.12f, C_ACCENT, prs ? 0.14f : 0.04f);
	CGContextRestoreGState (g);
	if (hov) {
		in = rrect (0.75f * u, 0.75f * u, w - 1.5f * u, h - 1.5f * u, 9.25f * u, F_ALL);
		stroke_path (g, in, 1.5f * u, C_ACCENT, 0.95f);
		CGPathRelease (in);
	} else
		inner_edge (g, path, u, 0, h, 0xffffff, 0.20f, 0.06f);
	if (p->icon) {
		/* a soft light behind the icon, then the icon */
		CGPathRef glow = circle (p->a, p->b, p->c * 0.62f);
		CGContextSaveGState (g);
		CGContextAddPath (g, glow);
		CGContextClip (g);
		{
			CGFloat comp[8] = { CR (C_ACCENT), CG_ (C_ACCENT), CB (C_ACCENT), hov ? 0.16f : 0.08f,
					    CR (C_ACCENT), CG_ (C_ACCENT), CB (C_ACCENT), 0 };
			CGFloat loc[2] = { 0, 1 };
			CGGradientRef gr = CGGradientCreateWithColorComponents (rgb_space, comp, loc, 2);
			CGContextDrawRadialGradient (g, gr, CGPointMake (p->a, p->b), 0,
						     CGPointMake (p->a, p->b), p->c * 0.62f, 0);
			CGGradientRelease (gr);
		}
		CGContextRestoreGState (g);
		CGPathRelease (glow);
		icon (g, p->icon, p->a, p->b, p->c, 2.6f * u, hov ? 0x9fe8ff : C_ACCENT, 1);
	}
	CGPathRelease (path);
}

/* a line icon alone */
static void paint_icon (CGContextRef g, const struct paint *p)
{
	float s = p->w < p->h ? p->w : p->h;
	icon (g, p->icon, p->w / 2.0f, p->h / 2.0f, s, 2.2f * p->u, C_ACCENT, 1);
}

static void paint_card (CGContextRef g, const struct paint *p)
{
	float u = p->u, w = p->w, h = p->h;
	int hov = p->state >= 1, prs = p->state == 2;
	CGPathRef path, in;

	if (p->flags & F_BIG) {
		paint_big_card (g, p);
		return;
	}
	path = rrect (0, 0, w, h, 6 * u, F_ALL);

	halo (g, path, 2 * u, 10 * u, 0, 0.40f);
	if (hov) halo (g, path, 0, 16 * u, C_ACCENT, 0.30f);
	CGContextSaveGState (g);
	CGContextAddPath (g, path);
	CGContextClip (g);
	if (p->see) vgrad (g, 0, h, C_CARD, SEE_CARD_A0, C_CARD, SEE_CARD_A1);
	else vgrad (g, 0, h, C_CARD, 0.82f, C_CARD, 0.72f);
	if (prs) {
		fill_col (g, C_ACCENT, 0.20f);
		CGContextFillRect (g, CGRectMake (0, 0, w, h));
	} else if (hov) {
		vgrad (g, 0, h, C_ACCENT, 0.10f, C_ACCENT, 0.03f);
	}
	vgrad (g, 0, h * 0.5f, 0xffffff, 0.04f, 0xffffff, 0);
	if (hov) {
		fill_col (g, C_ACCENT, 1);
		CGContextFillRect (g, CGRectMake (0, 0, 3 * u, h));
	}
	CGContextRestoreGState (g);
	in = rrect (0.5f * u, 0.5f * u, w - u, h - u, 5.5f * u, F_ALL);
	stroke_path (g, in, u, hov ? C_ACCENT : 0xffffff, prs ? 0.9f : hov ? 0.55f : 0.09f);
	CGPathRelease (in);
	if (p->flags & F_RING) {
		/* a radio's ring: the start menu's choices */
		CGPathRef ring = circle (p->a, p->b, p->c / 2);
		stroke_path (g, ring, 1.6f * u, hov ? C_ACCENT : C_TEXT2, hov ? 1 : 0.75f);
		CGPathRelease (ring);
		if (hov) {
			ring = circle (p->a, p->b, p->c * 0.24f);
			fill_path (g, ring, C_ACCENT, 1);
			CGPathRelease (ring);
		}
	} else if (p->icon) {
		float s = p->c;
		CGPathRef tile = rrect (p->a - s / 2, p->b - s / 2, s, s, 6 * u, F_ALL);
		fill_path (g, tile, C_ACCENT, hov ? 0.20f : 0.11f);
		CGPathRelease (tile);
		tile = rrect (p->a - s / 2 + 0.5f * u, p->b - s / 2 + 0.5f * u, s - u, s - u, 5.5f * u, F_ALL);
		stroke_path (g, tile, u, C_ACCENT, hov ? 0.7f : 0.35f);
		CGPathRelease (tile);
		icon (g, p->icon, p->a, p->b, s * 0.62f, 2.2f * u, hov ? C_TEXT : C_ACCENT, 1);
	}
	if (p->flags & F_CHEVRON)
		icon (g, I_CHEVRON, w - 16 * u, h / 2, 18 * u, 2 * u, hov ? C_ACCENT : p->see ? C_TEXT2_SEE : C_TEXT2, hov ? 1 : 0.8f);
	CGPathRelease (path);
}

static void paint_row (CGContextRef g, const struct paint *p)
{
	float u = p->u, w = p->w, h = p->h;
	int hov = p->state >= 1, prs = p->state == 2;
	CGPathRef path = rrect (0, 0, w, h, 4 * u, F_ALL), in;

	CGContextSaveGState (g);
	CGContextAddPath (g, path);
	CGContextClip (g);
	if (prs) fill_col (g, C_ACCENT, 0.22f);
	else if (hov) fill_col (g, C_ACCENT, 0.10f);
	else fill_col (g, 0xffffff, 0.035f);
	CGContextFillRect (g, CGRectMake (0, 0, w, h));
	if (hov) {
		fill_col (g, C_ACCENT, 1);
		CGContextFillRect (g, CGRectMake (0, 0, 2.5f * u, h));
	}
	CGContextRestoreGState (g);
	if (hov) {
		in = rrect (0.5f * u, 0.5f * u, w - u, h - u, 3.5f * u, F_ALL);
		stroke_path (g, in, u, C_ACCENT, prs ? 0.8f : 0.4f);
		CGPathRelease (in);
	}
	if (p->flags & F_CHEVRON)
		icon (g, I_CHEVRON, w - 14 * u, h / 2, 16 * u, 2 * u, hov ? C_ACCENT : p->see ? C_TEXT2_SEE : C_TEXT2, hov ? 1 : 0.7f);
	CGPathRelease (path);
}

static void paint_btn (CGContextRef g, const struct paint *p)
{
	float u = p->u, w = p->w, h = p->h, s = w < h ? w : h;
	int off = p->flags & F_OFF, round = p->flags & F_ROUND;
	int hov = !off && p->state >= 1, prs = !off && (p->state == 2 || (p->flags & F_ON));
	CGPathRef path = round ? circle (w / 2, h / 2, s / 2) : rrect (0, 0, w, h, 5 * u, F_ALL), in;

	halo (g, path, 1 * u, 4 * u, 0, 0.45f);
	if (hov) halo (g, path, 0, 10 * u, C_ACCENT, 0.40f);
	CGContextSaveGState (g);
	CGContextAddPath (g, path);
	CGContextClip (g);
	vgrad (g, 0, h, C_CONTROL, 0.96f, C_CONTROL, 0.86f);
	vgrad (g, 0, h, 0xffffff, 0.06f, 0xffffff, 0);
	if (prs) {
		fill_col (g, C_ACCENT, 0.30f);
		CGContextFillRect (g, CGRectMake (0, 0, w, h));
	}
	CGContextRestoreGState (g);
	in = round ? circle (w / 2, h / 2, s / 2 - 0.5f * u) : rrect (0.5f * u, 0.5f * u, w - u, h - u, 4.5f * u, F_ALL);
	stroke_path (g, in, u, hov || prs ? C_ACCENT : 0xffffff, hov || prs ? 0.85f : off ? 0.06f : 0.14f);
	CGPathRelease (in);
	icon (g, p->icon, w / 2, h / 2, s * (round ? 0.6f : 0.64f), 2 * u, prs ? C_TEXT : hov ? C_ACCENT : C_TEXT,
	      prs || hov ? 1 : off ? 0.25f : 0.88f);
	CGPathRelease (path);
}

static void paint_check (CGContextRef g, const struct paint *p)
{
	float u = p->u, s = (p->w < p->h ? p->w : p->h) - 2 * u;
	float x = (p->w - s) / 2, y = (p->h - s) / 2;
	int hov = p->state >= 1, on = p->flags & F_ON;
	CGPathRef path = rrect (x, y, s, s, 4 * u, F_ALL), in;

	if (on) {
		halo (g, path, 0, hov ? 10 * u : 6 * u, C_ACCENT, hov ? 0.45f : 0.25f);
		fill_path (g, path, C_ACCENT, 1);
		icon (g, I_CHECK, p->w / 2, p->h / 2, s * 0.8f, 2.6f * u, C_BG0, 1);
	} else {
		fill_path (g, path, 0, 0.30f);
		in = rrect (x + 0.75f * u, y + 0.75f * u, s - 1.5f * u, s - 1.5f * u, 3.5f * u, F_ALL);
		stroke_path (g, in, 1.5f * u, hov ? C_ACCENT : C_TEXT2, hov ? 0.95f : 0.6f);
		CGPathRelease (in);
	}
	CGPathRelease (path);
}

static void paint_radio (CGContextRef g, const struct paint *p)
{
	float u = p->u, d = (p->w < p->h ? p->w : p->h) - 2 * u;
	int hov = p->state >= 1, on = p->flags & F_ON;
	CGPathRef ring = circle (p->w / 2, p->h / 2, d / 2 - 0.75f * u), dot;

	fill_path (g, ring, 0, 0.30f);
	if (on) {
		dot = circle (p->w / 2, p->h / 2, d * 0.24f);
		halo (g, dot, 0, 6 * u, C_ACCENT, 0.5f);
		fill_path (g, dot, C_ACCENT, 1);
		CGPathRelease (dot);
	}
	stroke_path (g, ring, 1.5f * u, on || hov ? C_ACCENT : C_TEXT2, on || hov ? 0.95f : 0.6f);
	CGPathRelease (ring);
}

static void paint_pill (CGContextRef g, const struct paint *p)
{
	float u = p->u, w = p->w, h = p->h;
	int hov = p->state >= 1, prs = p->state == 2;
	CGPathRef path = rrect (0, 0, w, h, h / 2, F_ALL), in;

	halo (g, path, 2 * u, 8 * u, 0, 0.40f);
	if (hov) halo (g, path, 0, 14 * u, C_ACCENT, 0.35f);
	CGContextSaveGState (g);
	CGContextAddPath (g, path);
	CGContextClip (g);
	vgrad (g, 0, h, C_CARD, 0.92f, C_CARD, 0.84f);
	vgrad (g, 0, h, 0xffffff, 0.06f, 0xffffff, 0);
	if (prs || hov) {
		fill_col (g, C_ACCENT, prs ? 0.24f : 0.08f);
		CGContextFillRect (g, CGRectMake (0, 0, w, h));
	}
	CGContextRestoreGState (g);
	in = rrect (0.5f * u, 0.5f * u, w - u, h - u, h / 2 - 0.5f * u, F_ALL);
	stroke_path (g, in, u, hov || prs ? C_ACCENT : 0xffffff, prs ? 0.9f : hov ? 0.6f : 0.12f);
	CGPathRelease (in);
	if (p->icon && !(p->flags & F_NOICON))
		icon (g, p->icon, p->a, p->b, p->c, 2.2f * u, hov ? C_TEXT : C_ACCENT, 1);
	CGPathRelease (path);
}

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

/* The texture for a painted component, painted now if it is not cached */
static struct sprite *sprite (const struct paint *p)
{
	unsigned long long key = mix_hash (FNV0, p, sizeof (*p));
	struct sprite *s = NULL;
	unsigned char *buf;
	CGContextRef g;
	int i, W, H;

	for (i = 0; i < n_spr; i++)
		if (spr[i].key == key) {
			spr[i].used = frame_no;
			return &spr[i];
		}
	if (npot < 0) {
		const char *ext = (const char *) glGetString (GL_EXTENSIONS);
		npot = ext && strstr (ext, "GL_ARB_texture_non_power_of_two") != NULL;
	}
	if (!rgb_space) rgb_space = CGColorSpaceCreateDeviceRGB ();
	if (n_spr < MAX_SPRITES)
		s = &spr[n_spr++];
	else {
		/* the one drawn longest ago */
		s = &spr[0];
		for (i = 1; i < n_spr; i++)
			if (spr[i].used < s->used) s = &spr[i];
		glDeleteTextures (1, &s->tex);
	}
	W = p->w + 2 * p->pad;
	H = p->h + 2 * p->pad;
	if (W < 1) W = 1;
	if (H < 1) H = 1;
	memset (s, 0, sizeof (*s));
	s->key = key;
	s->used = frame_no;
	s->w = W;
	s->h = H;
	s->pad = p->pad;
	s->tw = npot ? W : pow2 (W);
	s->th = npot ? H : pow2 (H);
	buf = calloc ((size_t) W * H, 4);
	if (!buf) return NULL;
	g = CGBitmapContextCreate (buf, W, H, 8, W * 4, rgb_space,
				   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	if (g) {
		/* y down, the element's top left at the origin */
		CGContextTranslateCTM (g, 0, H);
		CGContextScaleCTM (g, 1, -1);
		CGContextTranslateCTM (g, p->pad, p->pad);
		CGContextSetShouldAntialias (g, true);
		switch (p->kind) {
		case E_GLASS: paint_glass (g, p); break;
		case E_CARD: paint_card (g, p); break;
		case E_ROW: paint_row (g, p); break;
		case E_BTN: paint_btn (g, p); break;
		case E_CHECK: paint_check (g, p); break;
		case E_RADIO: paint_radio (g, p); break;
		case E_PILL: paint_pill (g, p); break;
		case E_ICON: paint_icon (g, p); break;
		}
		CGContextRelease (g);
	}
	glGenTextures (1, &s->tex);
	glBindTexture (GL_TEXTURE_2D, s->tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPushClientAttrib (GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	if (s->tw == W && s->th == H)
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	else {
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, s->tw, s->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	}
	glPopClientAttrib ();
	free (buf);
	return s;
}

/* draw an element's picture with its top left at framebuffer pixel (x, y) */
static void draw_sprite (const struct sprite *s, float x, float y, float a)
{
	float u1 = s->w / (float) s->tw, v1 = s->h / (float) s->th;

	x -= s->pad;
	y -= s->pad;
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, s->tex);
	glColor4f (a, a, a, a);
	glBegin (GL_TRIANGLE_STRIP);
	glTexCoord2f (0, 0); glVertex2f (x, y);
	glTexCoord2f (u1, 0); glVertex2f (x + s->w, y);
	glTexCoord2f (0, v1); glVertex2f (x, y + s->h);
	glTexCoord2f (u1, v1); glVertex2f (x + s->w, y + s->h);
	glEnd ();
	glDisable (GL_TEXTURE_2D);
}

static void forget_sprites (void)
{
	int i;
	for (i = 0; i < n_spr; i++)
		if (spr[i].tex) glDeleteTextures (1, &spr[i].tex);
	n_spr = 0;
}

#endif /* __APPLE__ */

/* ---------------------------------------------------------------- */
/* The base under a page, and the elements */

static float chan (unsigned int c, int shift) { return ((c >> shift) & 0xff) / 255.0f; }

/* the base's colour at ST (x, y): the dark gradient, a cool light from
 * the top right, a deep blue from the bottom left, a vignette */
static void base_rgb (float x, float y, float *c)
{
	float t = y / CTRL_Y, d, k, nx, ny, v;
	int i;

	for (i = 0; i < 3; i++)
		c[i] = chan (C_BG1, 16 - 8 * i) + (chan (C_BG0, 16 - 8 * i) - chan (C_BG1, 16 - 8 * i)) * t;
	d = hypotf ((x - 300) / 240.0f, (y + 30) / 170.0f);
	k = d < 1 ? (1 - d) * (1 - d) * 0.20f : 0;
	for (i = 0; i < 3; i++) c[i] += chan (C_ACCENT, 16 - 8 * i) * k;
	d = hypotf ((x - 10) / 220.0f, (y - 200) / 150.0f);
	k = d < 1 ? (1 - d) * (1 - d) * 0.55f : 0;
	for (i = 0; i < 3; i++) c[i] += chan (0x10263c, 16 - 8 * i) * k;
	nx = (x - 160) / 160.0f;
	ny = (y - 84) / 84.0f;
	v = 1 - 0.30f * (nx * nx + ny * ny) * 0.5f;
	for (i = 0; i < 3; i++) c[i] *= v;
}

static int cmp_float (const void *a, const void *b)
{
	float p = *(const float *) a, q = *(const float *) b;
	return p < q ? -1 : p > q;
}

/* the cut lines of a grid over 0..max: every step, and the clear areas' edges */
static int grid_lines (float *l, float max, float step, int axis)
{
	int n = 0, i, j;
	float v;

	for (v = 0; v < max; v += step) l[n++] = v;
	l[n++] = max;
	for (i = 0; i < fr_ca_n; i++)
		for (j = 0; j < 2; j++) {
			v = fr_ca[i][axis + 2 * j];
			if (v > 0 && v < max) l[n++] = v;
		}
	qsort (l, n, sizeof (float), cmp_float);
	for (i = j = 0; i < n; i++)
		if (j == 0 || l[i] - l[j - 1] > 0.01f) l[j++] = l[i];
	return j;
}

static int in_clear_area (float x, float y)
{
	int i;
	for (i = 0; i < fr_ca_n; i++)
		if (x >= fr_ca[i][0] && x <= fr_ca[i][2] && y >= fr_ca[i][1] && y <= fr_ca[i][3]) return 1;
	return 0;
}

#ifdef __APPLE__
#include <ImageIO/ImageIO.h>

/* FE2_HDUI_PHOTO=file: a picture in place of the base, for seeing the
 * glass over a photographic backdrop (debugging) */
static GLuint photo_tex;
static int photo_w, photo_h, photo_tried;

static int load_photo (void)
{
	const char *path = getenv ("FE2_HDUI_PHOTO");
	CFURLRef url;
	CGImageSourceRef src;
	CGImageRef img;
	CGContextRef g;
	unsigned char *buf;
	int w = 1024, h = 1024;

	if (photo_tried) return photo_tex != 0;
	photo_tried = 1;
	if (!path || !*path) return 0;
	url = CFURLCreateFromFileSystemRepresentation (NULL, (const UInt8 *) path, strlen (path), false);
	if (!url) return 0;
	src = CGImageSourceCreateWithURL (url, NULL);
	CFRelease (url);
	if (!src) return 0;
	img = CGImageSourceCreateImageAtIndex (src, 0, NULL);
	CFRelease (src);
	if (!img) return 0;
	photo_w = CGImageGetWidth (img);
	photo_h = CGImageGetHeight (img);
	if (!rgb_space) rgb_space = CGColorSpaceCreateDeviceRGB ();
	buf = calloc ((size_t) w * h, 4);
	g = buf ? CGBitmapContextCreate (buf, w, h, 8, w * 4, rgb_space,
					 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
	if (g) {
		CGContextDrawImage (g, CGRectMake (0, 0, w, h), img);
		CGContextRelease (g);
		glGenTextures (1, &photo_tex);
		glBindTexture (GL_TEXTURE_2D, photo_tex);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPushClientAttrib (GL_CLIENT_PIXEL_STORE_BIT);
		glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
		glPopClientAttrib ();
	}
	free (buf);
	CGImageRelease (img);
	return photo_tex != 0;
}

static void draw_photo (void)
{
	/* fill the page's rows, keeping the picture's shape */
	float W = fbw, H = PY (CTRL_Y), ia = photo_w / (float) photo_h, va = W / H;
	float u0 = 0, u1 = 1, v0 = 0, v1 = 1;

	if (ia > va) {
		u0 = 0.5f - va / ia / 2;
		u1 = 1 - u0;
	} else {
		v0 = 0.5f - ia / va / 2;
		v1 = 1 - v0;
	}
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, photo_tex);
	glColor4f (1, 1, 1, 1);
	glBegin (GL_TRIANGLE_STRIP);
	glTexCoord2f (u0, v0); glVertex2f (0, 0);
	glTexCoord2f (u1, v0); glVertex2f (W, 0);
	glTexCoord2f (u0, v1); glVertex2f (0, H);
	glTexCoord2f (u1, v1); glVertex2f (W, H);
	glEnd ();
	glDisable (GL_TEXTURE_2D);
}
#endif

/* The base, opaque, over the page's rows: one batch of triangles, leaving
 * out the clear areas (the 3D views) when skip_views */
static void draw_base (int skip_views)
{
	float xs[64], ys[64], c[4][3], a = 1;
	int nx, ny, i, j, k;

	/* a photographic backdrop is already under the page: the same
	 * gradient, but only a wash over it (premultiplied, see begin_2d) */
	if (hdback_active ()) a = WASH_A;
#ifdef __APPLE__
	if (a == 1 && load_photo ()) {
		draw_photo ();
		return;
	}
#endif
	nx = grid_lines (xs, ST_W, 20, 0);
	ny = grid_lines (ys, CTRL_Y, 12, 1);
	glBegin (GL_TRIANGLES);
	for (j = 0; j + 1 < ny; j++)
		for (i = 0; i + 1 < nx; i++) {
			float x0 = xs[i], x1 = xs[i + 1], y0 = ys[j], y1 = ys[j + 1];
			static const int order[6] = { 0, 1, 2, 2, 1, 3 };
			if (skip_views && in_clear_area ((x0 + x1) / 2, (y0 + y1) / 2)) continue;
			base_rgb (x0, y0, c[0]);
			base_rgb (x1, y0, c[1]);
			base_rgb (x0, y1, c[2]);
			base_rgb (x1, y1, c[3]);
			for (k = 0; k < 6; k++) {
				int v = order[k];
				glColor4f (c[v][0] * a, c[v][1] * a, c[v][2] * a, a);
				glVertex2f (PX (v & 1 ? x1 : x0), PY (v & 2 ? y1 : y0));
			}
		}
	glEnd ();
}

/* is the pointer on hot place i: 0 no, 1 yes, 2 and the left button down */
static int hot_state (int i)
{
	const struct hot *h;

	if (i < 0 || i >= n_hot) return 0;
	h = &hot[i];
	if (ptr_x < h->x0 || ptr_x >= h->x1 || ptr_y < h->y0 || ptr_y >= h->y1) return 0;
	return ptr_buttons & 2 ? 2 : 1;
}

/* a hairline frame with accent corners, framebuffer pixels */
static void frame_px (float x0, float y0, float x1, float y1)
{
	float t = floorf (uu + 0.5f), T = floorf (2 * uu + 0.5f), L = floorf (16 * uu + 0.5f);

	if (t < 1) t = 1;
	if (T < 2) T = 2;
	rect_px (x0, y0, x1, y0 + t, 0xffffff, 0.14f);
	rect_px (x0, y1 - t, x1, y1, 0xffffff, 0.14f);
	rect_px (x0, y0 + t, x0 + t, y1 - t, 0xffffff, 0.14f);
	rect_px (x1 - t, y0 + t, x1, y1 - t, 0xffffff, 0.14f);
	rect_px (x0 - T, y0 - T, x0 + L, y0, C_ACCENT, 0.9f);
	rect_px (x0 - T, y0, x0, y0 + L, C_ACCENT, 0.9f);
	rect_px (x1 - L, y0 - T, x1 + T, y0, C_ACCENT, 0.9f);
	rect_px (x1, y0, x1 + T, y0 + L, C_ACCENT, 0.9f);
	rect_px (x0 - T, y1, x0 + L, y1 + T, C_ACCENT, 0.9f);
	rect_px (x0 - T, y1 - L, x0, y1, C_ACCENT, 0.9f);
	rect_px (x1 - L, y1, x1 + T, y1 + T, C_ACCENT, 0.9f);
	rect_px (x1, y1 - L, x1 + T, y1, C_ACCENT, 0.9f);
}

#ifdef __APPLE__
/* Frosted glass: the shape of a glass panel's or a card's path (rrect: x,
 * y, w, h framebuffer pixels, y down, the corners in `corners` rounded to
 * r), filled with hdback's frost, the blurred picture registered with the
 * sharp one round it.  CoreGraphics antialiases the path's edge in the
 * sprite; here a ramp one pixel wide, from inside the edge (opaque) to
 * outside it (clear), does the same.  The glass's tint sprite goes on top. */
#define FROST_SEGS	6	/* segments in a rounded corner */

/* the path's outline pushed out by d (in when d < 0): 4 (FROST_SEGS + 1)
 * points, clockwise on the screen from the top right corner */
static void frost_outline (float x, float y, float w, float h, float r, int corners,
			   float d, float *pt)
{
	static const int bit[4] = { F_TR, F_BR, F_BL, F_TL };
	static const float ox[4] = { 1, 1, -1, -1 }, oy[4] = { -1, 1, 1, -1 };
	const float cx[4] = { x + w - r, x + w - r, x + r, x + r };
	const float cy[4] = { y + r, y + h - r, y + h - r, y + r };
	const float kx[4] = { x + w, x + w, x, x }, ky[4] = { y, y + h, y + h, y };
	int c, i, n = 0;

	for (c = 0; c < 4; c++)
		for (i = 0; i <= FROST_SEGS; i++, n++) {
			if (corners & bit[c]) {
				float t = (float) M_PI * (c - 1 + i / (float) FROST_SEGS) / 2;
				pt[2 * n] = cx[c] + (r + d) * cosf (t);
				pt[2 * n + 1] = cy[c] + (r + d) * sinf (t);
			} else {
				pt[2 * n] = kx[c] + d * ox[c];
				pt[2 * n + 1] = ky[c] + d * oy[c];
			}
		}
}

static void frost_shape (float x, float y, float w, float h, float r, int corners)
{
	float in[8 * (FROST_SEGS + 1)], out[8 * (FROST_SEGS + 1)], rgb[3];
	int i, n = 4 * (FROST_SEGS + 1);

	if (w < 2 || h < 2 || !hdback_frost_begin (rgb)) return;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	frost_outline (x, y, w, h, r, corners, -0.5f, in);
	frost_outline (x, y, w, h, r, corners, 0.5f, out);
	glColor4f (rgb[0], rgb[1], rgb[2], 1);
	glBegin (GL_TRIANGLE_FAN);
	glVertex2f (x + w / 2, y + h / 2);
	for (i = 0; i <= n; i++) glVertex2f (in[2 * (i % n)], in[2 * (i % n) + 1]);
	glEnd ();
	/* the edge's ramp: premultiplied, so clear is all zero */
	glBegin (GL_TRIANGLE_STRIP);
	for (i = 0; i <= n; i++) {
		glColor4f (rgb[0], rgb[1], rgb[2], 1);
		glVertex2f (in[2 * (i % n)], in[2 * (i % n) + 1]);
		glColor4f (0, 0, 0, 0);
		glVertex2f (out[2 * (i % n)], out[2 * (i % n) + 1]);
	}
	glEnd ();
	hdback_frost_end ();
}
#endif

/* drawing a new page's backdrop over this frame: not a 3D view's card,
 * which would cover the view the game has drawn */
static int skip_view;

static void draw_elems (int layer)
{
	int i, see = hdback_active ();
#ifdef __APPLE__
	int frost = see && hdui_owns_page ();
#endif

	for (i = 0; i < n_el; i++) {
		const struct elem *e = &el[i];
		int state = hot_state (e->hot);
		if (e->layer != layer || (skip_view && (e->flags & F_VIEW))) continue;
		if (e->pressed) state = 2;
		switch (e->kind) {
		case E_QUAD:
			if ((e->flags & F_BLINK) && (frame_no & 32)) break;
			rect_px (PX (e->x0), PY (e->y0), PX (e->x1), PY (e->y1), e->rgb, e->alpha);
			break;
		case E_FRAME:
			if (e->c > 0) {
				/* round a picture (x0..y1: its bounds): the same gap
				 * on all four sides, in framebuffer pixels, as wide
				 * as there is room for on its left (to a) */
				float t = floorf (uu + 0.5f), g, gmax = floorf (4 * uu + 0.5f);
				if (t < 1) t = 1;
				g = PX (e->x0) - PX (e->a) - t;
				if (g > gmax) g = gmax;
				if (g < 1) g = 1;
				frame_px (PX (e->x0) - g - t, PY (e->y0) - g - t, PX (e->x1) + g + t, PY (e->y1) + g + t);
			} else
				frame_px (PX (e->x0), PY (e->y0), PX (e->x1), PY (e->y1));
			break;
		case E_RULE: {
			/* a hairline (c: its thickness, design pixels) */
			float t = floorf ((e->c > 0 ? e->c : 1) * uu + 0.5f), y = PY (e->y0);
			if (t < 1) t = 1;
			rect_px (PX (e->x0), y, PX (e->x1), y + t, e->rgb, e->alpha);
			break;
		}
		default:
#ifdef __APPLE__
		{
			struct paint p;
			struct sprite *s;
			if ((e->flags & F_HOVERONLY) && !state) break;
			memset (&p, 0, sizeof (p));
			p.kind = e->kind;
			p.state = state;
			p.icon = e->icon;
			p.flags = e->flags;
			p.w = (int) floorf ((e->x1 - e->x0) * sx + 0.5f);
			p.h = (int) floorf ((e->y1 - e->y0) * sy + 0.5f);
			p.u = uu;
			p.see = see;
			switch (e->kind) {
			case E_GLASS: p.pad = (int) ceilf (26 * uu); p.a = floorf (e->a * sy + 0.5f); break;
			case E_CARD: p.pad = (int) ceilf ((e->flags & F_BIG ? 32 : 22) * uu); break;
			case E_PILL: p.pad = (int) ceilf (20 * uu); break;
			case E_BTN: p.pad = (int) ceilf (14 * uu); break;
			case E_CHECK: case E_RADIO: p.pad = (int) ceilf (12 * uu); break;
			default: p.pad = 2; break;
			}
			if (e->kind != E_GLASS && e->c > 0) {
				p.a = floorf ((e->a - e->x0) * sx + 0.5f);
				p.b = floorf ((e->b - e->y0) * sy + 0.5f);
				p.c = floorf (e->c * uu + 0.5f);
			}
			s = sprite (&p);
			/* see-through glass and cards: the frost first, in their shape */
			if (s && frost && e->kind == E_GLASS)
				frost_shape (PX (e->x0), PY (e->y0), p.w, p.h, 8 * uu, e->flags & F_ALL);
			else if (s && frost && e->kind == E_CARD)
				frost_shape (PX (e->x0), PY (e->y0), p.w, p.h, 6 * uu, F_ALL);
			if (s) draw_sprite (s, PX (e->x0), PY (e->y0), 1);
		}
#endif
			break;
		}
	}
}

/* For hdtip: the glass panel at framebuffer pixel (x, y) from the
 * picture's top left, w x h pixels, in the caller's 2D set-up (y down,
 * premultiplied blending), at opacity a; frosted when a backdrop is up,
 * as the pages' glass is. */
void hdui_glass_px (float x, float y, int w, int h, float a)
{
#ifdef __APPLE__
	struct paint p;
	struct sprite *s;
	int px, py, pw, ph;
	float u;

	gl_picture_rect (&px, &py, &pw, &ph);
	u = ph >= 32 ? ph / 1200.0f : 1;
	memset (&p, 0, sizeof (p));
	p.kind = E_GLASS;
	p.flags = F_ALL;
	p.w = w;
	p.h = h;
	p.u = u;
	p.see = hdback_active ();
	p.pad = (int) ceilf (26 * u);
	s = sprite (&p);
	if (s && p.see) frost_shape (x, y, w, h, 8 * u, F_ALL);
	if (s) draw_sprite (s, x, y, a);
#else
	(void) x; (void) y; (void) w; (void) h; (void) a;
#endif
}

/* ---------------------------------------------------------------- */
/* Laying the pages out */

#define ON_PAGE(r)	((r)->y + 4 < CTRL_Y)
#define H_GO_BACK	0x82d26104u	/* a page's go back button */
#define H_REFUEL	0x58d0fcb4u	/* the cargo page's refuel button */

static struct elem spare;

static struct elem *add (int kind, int layer, float x0, float y0, float x1, float y1)
{
	struct elem *e = n_el < MAX_ELEMS ? &el[n_el++] : &spare;

	memset (e, 0, sizeof (*e));
	e->kind = kind;
	e->layer = layer;
	e->hot = -1;
	e->x0 = x0;
	e->y0 = y0;
	e->x1 = x1;
	e->y1 = y1;
	return e;
}

static void quad (int layer, float x0, float y0, float x1, float y1, unsigned int rgb, float a)
{
	struct elem *e = add (E_QUAD, layer, x0, y0, x1, y1);
	e->rgb = rgb;
	e->alpha = a;
}

/* a hairline along y from x0 to x1 */
static void rule (int layer, float x0, float x1, float y, unsigned int rgb, float a)
{
	struct elem *e = add (E_RULE, layer, x0, y, x1, y);
	e->rgb = rgb;
	e->alpha = a;
	e->c = 1;
}

/* a place the pointer can be on; a click there goes to (tx, ty) when remap */
static int add_hot (float x0, float y0, float x1, float y1, int tx, int ty, int remap)
{
	struct hot *h;

	if (n_hot >= MAX_HOT) return -1;
	h = &hot[n_hot];
	h->x0 = x0;
	h->y0 = y0;
	h->x1 = x1;
	h->y1 = y1;
	h->tx = tx;
	h->ty = ty;
	h->remap = remap;
	h->chain = 0;
	return n_hot++;
}

static int backdrop_page (void);

static void style (struct hdtext_run *r, int role, unsigned int tok)
{
	/* see-through glass is lighter than the opaque: secondary text lifted */
	if (tok == C_TEXT2 && hdback_active () && backdrop_page ()) tok = C_TEXT2_SEE;
	r->role = role;
	r->rgb = text_rgb (tok);
}

/* text the game wrote in the page colour is not seen on its page: nor here */
static int hide_bg (struct hdtext_run *r)
{
	if (r->col != BG) return 0;
	r->hide = 1;
	return 1;
}

/* the text of a list or a table: quieter in the game's quieter colours */
static void style_body (struct hdtext_run *r)
{
	int quiet = r->col == 11 || r->col == 13;
	if (hide_bg (r)) return;
	if (r->kind & HDT_K_NUMERIC) style (r, HDT_VALUE, quiet ? C_TEXT2 : C_TEXT);
	else style (r, HDT_BODY, quiet ? C_TEXT2 : C_TEXT);
}

/* the label written right of a box: on its line, starting lo..hi pixels right of it */
static struct hdtext_run *label_of (const struct button *b, int lo, int hi)
{
	int i;

	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (r->hide || abs (r->y - (b->y + 1)) > 2) continue;
		if (r->x0 >= b->x + lo && r->x0 <= b->x + hi) return r;
	}
	return NULL;
}

static int classify (void)
{
	int i, k, icons = 0, icon_x = -1, heads = 0, colons = 0, station = 0, f3 = 0, title = 0;

	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r)) continue;
		if (r->hash == H_START_TITLE) return P_START;
		if ((r->kind & HDT_K_ICON) && r->chr0 == 0x61) {
			/* a menu's choices: one column of them */
			if (icons++ && r->x0 != icon_x) icon_x = -2;
			if (icon_x != -2) icon_x = r->x0;
		}
		if (r->hash == H_FILE_UP && r->x0 == 10 && r->y == 30) return P_FILE;
		if (r->hash == H_STATS_HEAD && r->y >= 134 && r->y <= 142 && r->ink0 < 60) station = 1;
		if (r->hash == H_STATS_ROOM && r->y >= 144 && r->y <= 152 && r->ink0 <= 2) station = 1;
		if (r->hash == H_STATS_HEAD && r->ink0 >= 180) f3 = 1;
		if (r->hash == H_STATS_CABINS && r->x0 >= 180) f3 = 1;
		if (r->y >= 3 && r->y <= 7 && r->x0 < 120) title = 1;
		for (k = 0; k < (int) (sizeof (prefs_heads) / sizeof (prefs_heads[0])); k++)
			if (r->hash == prefs_heads[k]) heads++;
		if ((r->kind & HDT_K_COLON) && r->y <= 12) colons++;
	}
	if (icons >= 3 || (icons == 2 && icon_x >= 0)) return P_START;
	if (station) return P_STATION;
	if (f3) return P_F3;
	for (i = 0; i < fr_ca_n; i++)
		if (title && fr_ca[i][0] >= 180 && fr_ca[i][1] <= 4 && fr_ca[i][2] >= 300 &&
		    fr_ca[i][3] >= 120 && fr_ca[i][3] <= 140)
			return P_F3;
	if (heads >= 2 || (n_btn >= 10 && colons >= 2)) return P_PREFS;
	/* a star's information page: its keys in the panel (the market's
	 * is on no other) */
	{
		struct hdstill_panel p;
		if (hdstill_panel (&p) && p.slot[7] == G_MARKET) return P_INFO;
	}
	return P_NONE;
}

/* A star's information page: its text, under the picture of its bodies,
 * on a card */
static void layout_info (unsigned char *out)
{
	int i, x, y, x0 = ST_W, y0 = CTRL_Y, x1 = -1, y1 = -1;

	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r)) continue;
		style (r, HDT_BODY, C_TEXT);
		if (r->ink0 < x0) x0 = r->ink0;
		if (r->ink1 > x1) x1 = r->ink1;
		if (r->y < y0) y0 = r->y;
		if (r->y + 8 > y1) y1 = r->y + 8;
	}
	if (x1 < 0) return;
	x0 = x0 < 7 ? 0 : x0 - 7;
	x1 = x1 + 7 > ST_W ? ST_W : x1 + 7;
	y0 = y0 < 4 ? 0 : y0 - 4;
	y1 = y1 + 4 > CTRL_Y ? CTRL_Y : y1 + 4;
	add (E_CARD, L_UNDER, x0, y0, x1, y1);
	/* the page's black there: the card */
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			if (out[y * ST_W + x] == 0) out[y * ST_W + x] = 255;
}

/* The station's pages.  Across the top a header bar: the station's emblem
 * and the page's title on the left, the cash on the right.  Under it on
 * the left a tall card: the picture and the cargo panel.  On the right
 * the page's controls: a grid of large cards (the services and the
 * shipyard), a table (the market, the ships for sale), a list of rows (a
 * conversation's answers), cards (the bulletin board) or the page's text,
 * all moved down under the header.  A click anywhere on a control goes
 * to a point inside its box; one elsewhere in the right column (where the
 * game's boxes were but the layout's are not) to a place on the page
 * where a click does nothing.  When the right column holds anything the
 * layout does not know (a picture, a 3D view) the page is laid out in
 * place, without moving anything there. */

#define COL_X0		122.5f	/* the right column */
#define COL_X1		319.5f
#define HEAD_Y1		13.5f	/* the header bar's foot */
#define BODY_Y0		15.5f	/* the cards' top */
#define FOOT_Y		150	/* a page's foot: go back, more */

/* A box at the foot of a page with its label right of it, hi pixels at
 * most (go back): a pill.  Go back is moved to the column's left edge (dx
 * from the game's place, when left is set); a click on it still goes to
 * the game's box. */
static struct elem *foot_pill (struct button *b, int hi, int left)
{
	struct hdtext_run *lab = label_of (b, 6, hi);
	int back = lab && lab->hash == H_GO_BACK;
	float dx = back && left ? COL_X0 + 1 - (b->x - 2.5f) : 0;
	float x1 = (lab ? lab->ink1 + 5 : b->x + 14) + dx;
	struct elem *e = add (E_PILL, L_UNDER, b->x - 2.5f + dx, b->y - 1.5f, x1, b->y + 10);

	e->icon = back ? I_BACK : I_CHEVRON;
	e->a = b->x + 4.5f + dx;
	e->b = b->y + 4.25f;
	e->c = 26;
	e->hot = add_hot (e->x0, e->y0, e->x1, e->y1, b->x + 4, b->y + 4, 1);
	if (lab) {
		style (lab, HDT_LABEL, C_TEXT);
		lab->align = HDT_LEFT;
		lab->dx = dx;
	}
	b->used = 1;
	return e;
}

static float my_d0, my_s0, my_k = 1;

/* the game's y in the right column to the layout's */
static float map_y (float y)
{
	return my_d0 + (y - my_s0) * my_k;
}

/* the icon of a service's or the shipyard's card; 0 if it is not one */
static void svc_seen (const struct hdtext_run *lab, int tx, int ty);

static int card_icon (unsigned int h)
{
	switch (h) {
	case H_SVC_LAUNCH: return I_LAUNCH;
	case H_SVC_SHIPYARD: return I_SHIP;
	case H_SVC_BOARD: return I_DOC;
	case H_SVC_MARKET: return I_MARKET;
	case H_YARD_FIT: return I_CHIP;
	case H_YARD_REPAIR: return I_WRENCH;
	case H_YARD_SHIPS: return I_SWAP;
	case H_YARD_POLICE: return I_SHIELD;
	}
	return 0;
}

/* a run moved so its capitals' middle is at y */
static void centre_y (struct hdtext_run *r, float y)
{
	r->dy = y - (r->y + 3.5f);
}

/* is run r a bracket alone */
static int bracket (const struct hdtext_run *r)
{
	char c[4];
	unsigned char sp[4];
	return r->n == 1 && hdtext_chars (r, c, sp, 4) == 1 && (c[0] == '(' || c[0] == ')');
}

/* The grid's descriptions are set again in lines that fit a card: each
 * run is made one run a word (and its brackets runs of their own), and
 * returns the width of a word space in the description's type. */
static float split_words (void)
{
	int i, k, n, cuts[64], nc, spaces = 0;
	float whole = 0, parts = 0;
	char c[128];
	unsigned char sp[128];

	for (i = n_runs - 1; i >= 0; i--) {
		struct hdtext_run *r = &runs[i];
		int j, lab = 0;
		if (!ON_PAGE (r) || r->x0 < 136 || r->y < 8 || r->y >= FOOT_Y) continue;
		for (j = 0; j < n_btn; j++)
			if (btn[j].x >= 122 && btn[j].x <= 126 && abs (r->y - (btn[j].y + 1)) <= 2) lab = 1;
		if (lab) continue;
		n = hdtext_chars (r, c, sp, 128);
		nc = 0;
		for (k = n - 1; k >= 1 && nc < 64; k--) {
			if (k == n - 1 && c[k] == ')' && !sp[k]) cuts[nc++] = k;
			else if (sp[k]) cuts[nc++] = k;
			else if (k == 1 && c[0] == '(') cuts[nc++] = k;
		}
		style (r, HDT_DESC, C_TEXT2);
		whole += hdtext_measure (r);
		for (k = 1; k < n; k++)
			spaces += sp[k];
		/* from the right, so run i stays the first piece */
		for (k = 0; k < nc; k++)
			hdtext_split (i, cuts[k]);
		n_runs = hdtext_runs (&runs);
		for (k = i; k <= i + nc && k < n_runs; k++) {
			style (&runs[k], HDT_DESC, C_TEXT2);
			parts += hdtext_measure (&runs[k]);
		}
	}
	n_runs = hdtext_runs (&runs);
	if (spaces && whole > parts) return (whole - parts) / spaces;
	return 1.3f;
}

/* A service's card in the grid: its icon large, its name under it and
 * its description under that in lines that fit */
static void grid_card (struct button *b, float x0, float y0, float x1, float y1, float space, int next, int ic)
{
	struct hdtext_run *lab = label_of (b, 12, 20);
	int i, j, nw = 0, idx[48], nl = 0, brk[8], hard[48], lasty = -1;
	float cw = x1 - x0, cx = (x0 + x1) / 2, ch = y1 - y0, w[48], lim, Ih, T, top, lw[8];
	struct elem *e;

	/* the words, in order */
	for (i = 0; i < n_runs && nw < 48; i++) {
		struct hdtext_run *r = &runs[i];
		if (r == lab || !ON_PAGE (r) || r->x0 < 136 || r->y < b->y + 5 || r->y >= next - 2) continue;
		if (bracket (r)) {
			r->hide = 1;
			continue;
		}
		if (hide_bg (r)) continue;
		style (r, HDT_DESC, C_TEXT2);
		w[nw] = hdtext_measure (r);
		/* a line of the original that starts with a capital starts a
		 * sentence: a new line here too */
		{
			char c[2];
			unsigned char sp[2];
			hard[nw] = nw && r->y != lasty && hdtext_chars (r, c, sp, 1) == 1 && c[0] >= 'A' && c[0] <= 'Z';
		}
		lasty = r->y;
		idx[nw++] = i;
	}
	/* lines: a sentence at a time, as few as fit and as even as can be,
	 * so none ends on a word alone */
	lim = cw - 12;
	for (i = 0; i < nw && nl < 8; i = j) {
		float para = w[i], target;
		int want, k, z, n;
		for (z = i + 1; z < nw && !hard[z]; z++)
			para += space + w[z];
		want = (int) ceilf (para / lim);
		if (want < 1) want = 1;
		for (target = para / want;; target += 0.5f) {
			float cur = 0;
			if (target > lim) target = lim;
			for (n = 1, k = i; k < z; k++) {
				float add = (cur > 0 ? space : 0) + w[k];
				if (cur > 0 && cur + add > target) {
					n++;
					cur = w[k];
				} else
					cur += add;
			}
			if (n <= want || target >= lim) break;
		}
		{
			float cur = 0;
			brk[nl] = i;
			for (k = i; k < z; k++) {
				float add = (cur > 0 ? space : 0) + w[k];
				if (cur > 0 && cur + add > target && nl < 7) {
					lw[nl++] = cur;
					brk[nl] = k;
					cur = w[k];
				} else
					cur += add;
			}
			lw[nl++] = cur;
		}
		j = z;
	}

	/* the block: icon, name, lines, in the card's middle */
	Ih = ch * 0.30f;
	if (Ih > 22) Ih = 22;
	T = Ih + 11 + (nl ? 2.5f + nl * 5.4f : 0);
	top = y0 + (ch - T) / 2;

	e = add (E_CARD, L_UNDER, x0, y0, x1, y1);
	e->flags = F_BIG;
	e->icon = ic ? ic : lab ? card_icon (lab->hash) : I_DOC;
	e->a = cx;
	e->b = top + Ih / 2;
	e->c = Ih * 6;
	e->hot = add_hot (x0, y0, x1, y1, b->x + 4, b->y + 4, 1);
	b->used = 1;
	if (lab) svc_seen (lab, b->x + 4, b->y + 4);

	if (lab) {
		float lwid;
		style (lab, HDT_LABEL, C_TEXT);
		lab->upper = 1;
		lab->scale = 1;
		lwid = hdtext_measure (lab);
		lab->scale = lwid > 0 && (cw - 10) / lwid < 1.3f ? (cw - 10) / lwid : 1.3f;
		if (lab->scale < 0.8f) lab->scale = 0.8f;
		lab->align = HDT_CENTRE;
		lab->ax = cx;
		lab->maxw = cw - 8;
		centre_y (lab, top + Ih + 8);
	}
	for (j = 0; j < nl; j++) {
		int a = brk[j], z = j + 1 < nl ? brk[j + 1] : nw, k;
		float x = cx - lw[j] / 2;
		for (k = a; k < z; k++) {
			struct hdtext_run *r = &runs[idx[k]];
			r->align = HDT_LEFT;
			r->ax = x;
			r->maxw = w[k] + 2;
			centre_y (r, top + Ih + 16.2f + j * 5.4f);
			x += w[k] + space;
		}
	}
}

/* A table's heads are drawn as one string where they touch: set each
 * apart, before a $ and before a capital that follows a capitalised word */
static void split_heads (void)
{
	int i, k, n, cuts[32], nc, prev;
	char c[96];
	unsigned char sp[96];

	for (i = n_runs - 1; i >= 0; i--) {
		const struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 < 118 || r->y > 4) continue;
		n = hdtext_chars (r, c, sp, 96);
		nc = 0;
		prev = 0;
		for (k = 1; k < n; k++) {
			int start = sp[k] || c[k] == '$';
			if (!start) continue;
			if (c[k] == '$' || (c[k] >= 'A' && c[k] <= 'Z' && ((c[prev] >= 'A' && c[prev] <= 'Z') || c[prev] == '$'))) {
				if (nc < 32) cuts[nc++] = k;
			}
			prev = k;
		}
		/* from the right, so run i stays the first piece */
		while (nc > 0) {
			hdtext_split (i, cuts[--nc]);
			n_runs = hdtext_runs (&runs);
		}
	}
	n_runs = hdtext_runs (&runs);
}

/* the number of a column's runs at x */
static int column_at (int x)
{
	int k, n = 0;

	for (k = 0; k < n_runs; k++)
		if (runs[k].x0 == x && (runs[k].kind & HDT_K_NUMERIC) && runs[k].y >= 8 && runs[k].y < FOOT_Y) n++;
	return n;
}

/* the rows split_merged cut: their pieces are as wide as the game's
 * columns let them run, so they do not set their columns' width */
static unsigned char cut_row[200];

/* A number the game ran on into the next column's ("$1060000$942200",
 * or one space between): cut where the next column starts (or, with no
 * column to go by, before the second '$'), so each piece goes in its
 * column, and look again at what is left on the right */
#define FIG(ch)	(isdigit ((unsigned char) (ch)) || (ch) == '.' || (ch) == ',')
static void split_merged (int ships)
{
	int i, j, k, n;
	char c[64];
	unsigned char sp[64];

	for (i = n_runs - 1; i >= 0; i--) {
		const struct hdtext_run *r = &runs[i];
		int X = -1, H;
		if (!ON_PAGE (r) || r->x0 < 118 || r->y < 8 || r->y >= FOOT_Y || !(r->kind & HDT_K_NUMERIC)) continue;
		for (j = 0; j < n_runs; j++)
			if (runs[j].y != r->y && runs[j].x0 > r->x0 + 4 && runs[j].x0 < r->ink1 && (X < 0 || runs[j].x0 < X) && column_at (runs[j].x0) >= 3)
				X = runs[j].x0;
		n = hdtext_chars (r, c, sp, 64);
		/* nothing below it starts inside it: a head over it may (a
		 * figure and the next column's, one word space apart) */
		H = -1;
		for (j = 0; j < n_runs && X < 0; j++)
			if (runs[j].y <= 4 && runs[j].x0 > r->x0 + 4 && runs[j].x0 < r->ink1 && (H < 0 || runs[j].x0 < H))
				H = runs[j].x0;
		for (k = 1; k < n; k++)
			if (X >= 0 ? hdtext_glyph_x (r, k) >= X :
			    H >= 0 ? sp[k] && hdtext_glyph_x (r, k) >= H - 6 :
			    c[k] == '$' && isalnum ((unsigned char) c[k - 1]))
				break;
		/* on the ships pages the column below may be a ship's
		 * details, starting inside a figure: never cut a figure in
		 * two (all that is right of the cut being figures) */
		if (k < n && X >= 0 && ships && FIG (c[k - 1])) {
			for (j = k; j < n && FIG (c[j]); j++);
			if (j == n) continue;
		}
		if (k < n && hdtext_split (i, k)) {
			n_runs = hdtext_runs (&runs);
			cut_row[runs[i].y] = 1;
			i += 2;
		}
	}
}

/* The heads of the right column, left to right: over their column, or
 * over the boxes they name, or where they were; never on one another and
 * inside the panel, a size down if the row is too full.  A head over a
 * column of figures sits where the game's head sat (centred on it, a
 * little clear of the next head), and the figures are right-set on its
 * right edge: where a column's figures end depends on what is in it
 * today, where its head ends does not, so the column stays put from one
 * page to the next. */
static void place_heads (const float *colx, int ncol)
{
	int i, j, k, n = 0, pass, ord[32], col[32];
	float s = 1, want[32], w[32], left[32];

	for (i = 0; i < n_runs && n < 32; i++) {
		const struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 < 118 || r->y > 4 || r->hide || r->col == BG) continue;
		for (j = n; j > 0 && runs[ord[j - 1]].ink0 > r->ink0; j--)
			ord[j] = ord[j - 1];
		ord[j] = i;
		n++;
	}
	for (pass = 0; pass < 4 && n; pass++) {
		float edge = COL_X0 + 0.5f;
		for (j = 0; j < n; j++) {
			struct hdtext_run *r = &runs[ord[j]];
			float cx = (r->ink0 + r->ink1) / 2.0f;
			r->scale = s;
			w[j] = hdtext_measure (r);
			want[j] = r->ink0;
			col[j] = -1;
			for (k = 0; k < ncol && col[j] < 0; k++)
				if (colx[k] >= r->ink0 - 6 && colx[k] <= r->ink1) {
					/* centred on the game's head when ours is the
					 * narrower: what it leaves goes either side,
					 * not all to the column on its left */
					float slack = r->ink1 - r->ink0 + 1 - w[j];
					float end = r->ink1 - (slack > 0 ? slack / 2 : 0);
					float next = j + 1 < n ? runs[ord[j + 1]].ink0 - 4 : COL_X1 - 2.5f;
					col[j] = k;
					want[j] = (end < next ? end : next) - w[j];
				}
			if (col[j] < 0)
				for (k = 0; k < n_btn; k++)
					if (btn[k].x >= 118 && btn[k].y < FOOT_Y && fabsf (btn[k].x + 5 - cx) <= 6) {
						want[j] = btn[k].x + 5 - w[j] / 2;
						break;
					}
			left[j] = want[j] > edge + 2.5f ? want[j] : edge + 2.5f;
			edge = left[j] + w[j];
		}
		if (edge <= COL_X1 - 2.5f) break;
		if (pass < 3) s *= 0.9f;
	}
	for (j = 0; j < n; j++) {
		struct hdtext_run *r = &runs[ord[j]];
		float room = (j + 1 < n ? left[j + 1] - 2.5f : COL_X1 - 2.5f) - left[j];
		r->align = HDT_LEFT;
		r->ax = left[j];
		r->maxw = room > w[j] + 1 ? w[j] + 1 : room;
		if (col[j] < 0) continue;
		/* right set, so its last letter's ink is on the figures' edge */
		r->align = HDT_RIGHT;
		r->ax = left[j] + w[j];
		for (i = 0; i < n_runs; i++)
			if (runs[i].x0 == colx[col[j]] && runs[i].align == HDT_RIGHT && runs[i].y > 4 &&
			    runs[i].y < FOOT_Y)
				runs[i].ax = left[j] + w[j];
	}
}

/* A list's paging arrows and its next-page label: one pill with a round
 * button for each arrow, the down one first, then the label.  Both
 * buttons are always there, so the pill does not change as the list
 * moves; an arrow the game does not show now is dimmed, and a click on it
 * goes where a click does nothing.  A click on a button goes to the
 * arrow it stands for, one on the label to the down arrow (the next
 * rows), as long as there is one. */
static void more_pager (const unsigned char *raster, unsigned char *out, struct hdtext_run *lab,
			int more, int mx, int my)
{
	float h = 9.5f, w = h * sy / sx, cy = my + 4.75f, y0 = my - 1.5f, y1 = my + 11;
	float x0, cd, cu, tw;
	struct elem *e, *pill;
	int down = more & MORE_DOWN, back = more & MORE_BACK, hh;

	take_out_more (raster, out, mx, my, more);
	lab->align = HDT_LEFT;
	lab->maxw = 60;
	tw = hdtext_measure (lab);
	/* on the column's right edge, go back being on its left; a click
	 * still goes to the game's arrows */
	x0 = COL_X1 - 1 - ((y1 - y0) / 2 * sy / sx + (h + 1.5f) * sy / sx + w / 2 + 3 + tw + 5);
	cd = x0 + (y1 - y0) / 2 * sy / sx;
	cu = cd + (h + 1.5f) * sy / sx;
	lab->ax = cu + w / 2 + 3;
	centre_y (lab, cy);
	pill = add (E_PILL, L_UNDER, x0, y0, lab->ax + tw + 5, y1);
	pill->flags = F_NOICON;

	/* the buttons' places first: the first place a click is in wins */
	hh = add_hot (x0, y0, (cd + cu) / 2, y1, down ? mx + 5 : 1, down ? my + 4 : 1, 1);
	e = add (E_BTN, L_UNDER, cd - w / 2, cy - h / 2, cd + w / 2, cy + h / 2);
	e->icon = I_MORE;
	e->flags = F_ROUND | (down ? 0 : F_OFF);
	e->hot = down ? hh : -1;
	hh = add_hot ((cd + cu) / 2, y0, cu + w / 2 + 1.5f, y1, back ? mx + MORE_UP + 5 : 1, back ? my + 4 : 1, 1);
	e = add (E_BTN, L_UNDER, cu - w / 2, cy - h / 2, cu + w / 2, cy + h / 2);
	e->icon = I_UP;
	e->flags = F_ROUND | (back ? 0 : F_OFF);
	e->hot = back ? hh : -1;
	hh = add_hot (cu + w / 2 + 1.5f, y0, pill->x1, y1, down ? mx + 5 : 1, down ? my + 4 : 1, 1);
	pill->hot = down ? hh : -1;
}

/* The sums on a station page (the cash, a price, a column under a head
 * with a '$'): set with their thousands marked and the minus first, a
 * debt in the caution colour.  The figures stay the game's. */
static void money_runs (void)
{
	float h0[8], h1[8];
	int nh = 0, i, k, n, cur, neg, dig, other, pass;
	char c[48];
	unsigned char sp[48];

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < n_runs; i++) {
			struct hdtext_run *r = &runs[i];
			if (!ON_PAGE (r) || r->hide || (pass == 0) != (r->y <= 4 && r->x0 >= 118)) continue;
			n = hdtext_chars (r, c, sp, 48);
			cur = neg = dig = other = 0;
			for (k = 0; k < n; k++)
				if (c[k] == '$') cur = 1;
				else if (c[k] == '-') neg = 1;
				else if (c[k] >= '0' && c[k] <= '9') dig++;
				else if (c[k] != '.' && c[k] != ',') other = 1;
			if (pass == 0) {
				if (cur && nh < 8) {
					h0[nh] = r->x0;
					h1[nh++] = r->x1;
				}
				continue;
			}
			if (!dig || other || n >= 48) continue;
			for (k = 0; k < nh && !cur; k++)
				if (r->x0 >= 118 && r->y < FOOT_Y && r->ink1 > h0[k] && r->ink0 < h1[k]) cur = 1;
			if (!cur) continue;
			r->money = 1;
			if (neg && r->role) r->rgb = text_rgb (C_AMBER);
		}
}

/* The game's list on a page (the market's goods, the shipyard's
 * upgrades), read and never written.  L76018 adds an entry: 24 bytes
 * from 10602(a6) (movem.l d0-4/a0), the count in 11724(a6); L75d10 shows
 * them from the one at 11722(a6).  The market's (L782e8) have $9831 in
 * d0's low word and $8e00 + the good in a0's; the upgrades' (L76a76)
 * $986e and $9873 + the row of L7655c. */
#define A6		__DL5eb6_a6_base
#define A6_LIST		10602
#define A6_LIST_FIRST	11722
#define A6_LIST_COUNT	11724
#define A6_CABINS	740	/* word: the cabins fitted (L76c02) */
#define A6_SHIP		674	/* long: the player's ship (L76b0e) */
#define LIST_MARKET	0x9831
#define LIST_UPGRADES	0x986e
#define LIST_MOUNTS	0x986f	/* L76c90: an entry a gun mount, $986f on */
#define UPG_ROW0	0x9873
#define UPG_TAB		__DL7655c	/* 12 bytes a row: +8 its kind, +10 its bit or id */
#define ST_MEM_SIZE	0x110000

static int st_ok (unsigned int a, unsigned int n) { return a >= 0x400 && a < ST_MEM_SIZE && n <= ST_MEM_SIZE - a; }
static int st_b (unsigned int a) { return STMemory_ReadByte (a) & 0xff; }
static int st_w (unsigned int a) { return STMemory_ReadWord (a) & 0xffff; }

/* which list the page shows (LIST_MARKET, LIST_UPGRADES, LIST_MOUNTS; 0 if none),
 * its first entry shown and how many there are */
static int game_list (int *first, int *count)
{
	unsigned int a = A6 + A6_LIST;
	int i, n, f, kind, lo, hi;

	if (!st_ok (A6 + A6_LIST_FIRST, 4) || !st_ok (a, 45 * 24)) return 0;
	f = st_w (A6 + A6_LIST_FIRST);
	n = st_w (A6 + A6_LIST_COUNT);
	if (n < 1 || n > 45 || f >= n) return 0;
	kind = st_w (a + 2);
	if (kind == LIST_MOUNTS) {
		if (n > 4) return 0;
		for (i = 0; i < n; i++)
			if (st_w (a + 24 * i + 2) != LIST_MOUNTS + i) return 0;
		*first = f;
		*count = n;
		return kind;
	}
	if (kind == LIST_MARKET) lo = 0x8e00, hi = 0x8e1e;
	else if (kind == LIST_UPGRADES) lo = UPG_ROW0, hi = UPG_ROW0 + 44;
	else return 0;
	for (i = 0; i < n; i++)
		if (st_w (a + 24 * i + 2) != kind || st_w (a + 24 * i + 22) < lo || st_w (a + 24 * i + 22) > hi) return 0;
	*first = f;
	*count = n;
	return kind;
}

/* how many of upgrade entry i the ship has fitted, as the removal code
 * (L76b92's) finds them; -1 for a kind not known here */
static int upg_fitted (int i)
{
	unsigned int row = UPG_TAB + 12 * (st_w (A6 + A6_LIST + 24 * i + 22) - UPG_ROW0), ship;
	int kind, id, n = 0, m;

	if (!st_ok (row, 12) || !st_ok (A6 + A6_SHIP, 4)) return -1;
	kind = st_b (row + 8);
	id = st_b (row + 10);
	ship = (unsigned int) STMemory_ReadLong (A6 + A6_SHIP);
	if (!st_ok (ship, 184)) return -1;
	switch (kind) {
	case 0:		/* L76d5c: a bit of 158(a0) */
		return (STMemory_ReadLong (ship + 158) >> (id & 31)) & 1;
	case 2:		/* L76d84: the cabins */
		return st_w (A6 + A6_CABINS);
	case 4:		/* L76da2: a laser on any of the 167(a0) mounts at 168(a0) */
		if (!id) return -1;
		for (m = 0; m < st_b (ship + 167) && m < 4; m++)
			n += st_b (ship + 168 + m) == id;
		return n;
	case 6:		/* L76dac: the drive, 166(a0) */
		return id && st_b (ship + 166) == id;
	case 8:		/* L76de6: $40 a time in 182(a0) */
		return st_w (ship + 182) / 0x40;
	case 0xa:	/* L76dfe: the eight pylons at 172(a0) */
		if (!id) return -1;
		for (m = 0; m < 8; m++)
			n += st_b (ship + 172 + m) == id;
		return n;
	}
	return -1;
}

/* a number on the page: its digits and point ('-' alone is none: 0);
 * NAN if the run is not one */
static float page_num (const struct hdtext_run *r)
{
	char c[48];
	unsigned char sp[48];
	int n = r ? hdtext_chars (r, c, sp, 48) : 0, k, dig = 0, neg = 0, pt = 0;
	float v = 0, f = 0.1f;

	for (k = 0; k < n; k++)
		if (c[k] >= '0' && c[k] <= '9') {
			if (pt) {
				v += (c[k] - '0') * f;
				f /= 10;
			} else
				v = v * 10 + (c[k] - '0');
			dig = 1;
		} else if (c[k] == '.')
			pt = 1;
		else if (c[k] == '-')
			neg = 1;
		else if (c[k] == 't' && dig)
			break;
		else if (c[k] != '$' && c[k] != ',')
			return NAN;
	return dig ? (neg ? -v : v) : neg && n == 1 ? 0 : NAN;
}

/* the runs of a table's row right of its name, left to right */
static int row_cells (int y, struct hdtext_run **cell, int max)
{
	int i, n = 0;
	for (i = 0; i < n_runs && n < max; i++)
		if (ON_PAGE (&runs[i]) && runs[i].y == y && runs[i].x0 >= 200 && runs[i].x0 < 318) {
			int k = n++;
			for (; k > 0 && cell[k - 1]->x0 > runs[i].x0; k--)
				cell[k] = cell[k - 1];
			cell[k] = &runs[i];
		}
	return n;
}

static void layout_station (const unsigned char *raster, unsigned char *out)
{
	enum { CARDS, TABLE, ROWS, PLAIN } mode;
	static unsigned char cover[CTRL_Y][ST_W];
	int i, j, k, x, y, n124 = 0, n136 = 0, pitch = 999, prev = -1000, first = -1, last = -1;
	int heads = 0, ships = 0, foot = 0, more = 0, mx = 0, my = 0, stray = 0, inplace = 0, grid = 0, pic, odd_more = 0;
	int mounts = 0, yard = 0;
	float bb[4], space = 1.3f, s0 = 1000, s1 = -1000, tmin = 1000, tmax = -1000, title_x, panel_y = 136.5f;
	struct hdtext_run *cash = NULL, *cash_lab = NULL;
	struct elem *e;

	/* what the right column holds */
	for (i = 0; i < n_btn; i++) {
		struct button *b = &btn[i];
		if (b->x < 118) continue;
		if (b->y >= FOOT_Y) {
			foot = 1;
			continue;
		}
		if (b->x >= 122 && b->x <= 126) {
			n124++;
			if (b->y - prev < pitch) pitch = b->y - prev;
			prev = b->y;
		}
		if (b->x >= 134 && b->x <= 138) n136++;
		if (first < 0 || b->y < first) first = b->y;
		if (b->y > last) last = b->y;
	}
	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 < 118 || r->col == BG) continue;
		if (r->y <= 4) heads++;
		else if (r->y < FOOT_Y) {
			if (r->y < tmin) tmin = r->y;
			if (r->y + 8 > tmax) tmax = r->y + 8;
		} else
			foot = 1;
		if (r->hash == H_SHIPS_HEAD) ships = 1;
	}
	more = find_more (raster, &mx, &my);
	if (more) foot = 1;
	mode = n136 ? TABLE : n124 >= 2 && pitch >= 30 ? CARDS : n124 ? ROWS : PLAIN;

	/* anything else there (a picture, a 3D view): in place */
	memset (cover, 0, sizeof (cover));
	for (i = 0; i < n_btn; i++)
		for (y = btn[i].y; y <= btn[i].y + 10 && y < CTRL_Y; y++)
			for (x = btn[i].x; x <= btn[i].x + 15 && x < ST_W; x++)
				cover[y][x] = 1;
	if (more)
		for (y = my; y < my + 10; y++)
			for (x = more & MORE_DOWN ? mx : mx + MORE_UP; x < (more & MORE_BACK ? mx + MORE_UP + 16 : mx + 16); x++)
				cover[y][x] = 1;
	for (y = 0; y < FOOT_Y; y++)
		for (x = 121; x < ST_W; x++)
			if (out[y * ST_W + x] != 255 && !cover[y][x]) stray++;
	for (i = 0; i < fr_ca_n; i++)
		if (fr_ca[i][2] > 121 && fr_ca[i][1] < FOOT_Y) inplace = 1;
	if (stray >= 8) inplace = 1;

	/* the services and the shipyard: a grid */
	if (mode == CARDS && !inplace && n124 <= 6) {
		grid = 1;
		for (i = 0; i < n_btn; i++) {
			struct button *b = &btn[i];
			struct hdtext_run *lab;
			if (b->x < 122 || b->x > 126 || b->y >= FOOT_Y) continue;
			lab = label_of (b, 12, 20);
			if (!lab || !card_icon (lab->hash)) grid = 0;
			else if (lab->hash == H_YARD_FIT || lab->hash == H_YARD_REPAIR || lab->hash == H_YARD_SHIPS ||
				 lab->hash == H_YARD_POLICE)
				yard = 1;
		}
		if (grid) space = split_words ();
	}
	/* the gun mounts to choose from (L76c90's list): a card each, as
	 * the grid's, the game's own boxes their clicks */
	if (mode != TABLE && !grid && !inplace && !heads && !foot) {
		int f, n;
		if (game_list (&f, &n) == LIST_MOUNTS && f == 0 && n == n124) mounts = 1;
	}
	/* two tall rows that are not the grid: a conversation's answers,
	 * read as rows, not cards (the bulletin board has three or more) */
	if (mode == CARDS && !grid && n124 < 3) mode = ROWS;
	memset (cut_row, 0, sizeof cut_row);
	if (mode == TABLE && !grid) {
		split_merged (ships);
		split_heads ();
	}

	/* the map of the right column */
	if (inplace) {
		my_d0 = my_s0 = 0;
		my_k = 1;
	} else {
		float d0 = heads ? 28 : 17, d1 = foot ? 149 : 165;
		if (first >= 0) {
			s0 = first - (mode == CARDS ? 3 : 1);
			s1 = last + (mode == CARDS ? 30.5f : 9);
		}
		if (tmin < s0) s0 = tmin - 1;
		if (tmax > s1) s1 = tmax + 1;
		if (s1 <= s0) {
			s0 = 0;
			s1 = 1;
		}
		my_d0 = d0;
		my_s0 = s0;
		my_k = (d1 - d0) / (s1 - s0);
		if (my_k > 1) my_k = 1;
	}

	/* the header bar */
	for (i = 0; i < n_runs && !inplace; i++) {
		struct hdtext_run *r = &runs[i];
		if (r->hash == H_CASH && r->y >= 142 && r->y <= 152 && r->ink0 >= 60 && r->x0 < 118)
			cash_lab = r;
	}
	for (i = 0; i < n_runs && cash_lab; i++) {
		struct hdtext_run *r = &runs[i];
		if (r != cash_lab && (r->kind & HDT_K_NUMERIC) && r->x0 < 118 && r->y >= cash_lab->y + 8 &&
		    r->y <= cash_lab->y + 12 && r->ink0 <= cash_lab->ink1 && r->ink1 >= cash_lab->ink0)
			cash = r;
	}
	/* flush with the window's left edge: the picture under it is one
	 * pixel from it, and its frame needs the room */
	e = add (E_GLASS, L_BACK, 0, 0.5f, inplace ? 120.5f : COL_X1, HEAD_Y1);
	e->flags = F_ALL;
	/* the shipyard and its mount chooser: back to the services, as the
	 * console's comms icon goes (the game has no box for it) */
	title_x = 0;
	if (yard || mounts) {
		float h = 9, w = h * sy / sx;
		e = add (E_BTN, L_UNDER, 3, 7 - h / 2, 3 + w, 7 + h / 2);
		e->icon = I_BACK;
		e->flags = F_ROUND;
		e->hot = add_hot (2, 1, 4 + w, 13, 57, 193, 1);
		title_x = w + 3;
	}
	e = add (E_ICON, L_UNDER, 3 + title_x, 2, 13 + title_x, 12);
	e->icon = I_STATION;
	quad (L_BACK, 16 + title_x, 11.75f, 46 + title_x, 12.25f, C_ACCENT, 0.9f);
	title_x += 16;
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 >= 100 || r->y < 2 || r->y > 8 || hide_bg (r)) continue;
		style (r, HDT_TITLE, C_TEXT);
		r->scale = inplace ? 1 : 1.2f;
		r->align = HDT_LEFT;
		r->ax = title_x;
		r->maxw = (inplace ? 118 : 230) - title_x;
		centre_y (r, 6.75f);
		title_x += hdtext_measure (r) + 1.6f;
	}
	if (cash) {
		float vw;
		style (cash, HDT_VALUE, C_ACCENT);
		cash->scale = 1.25f;
		cash->align = HDT_RIGHT;
		cash->ax = 314;
		cash->maxw = 70;
		centre_y (cash, 7);
		/* measured as it is set (its thousands marked), and no
		 * wider than it may be drawn, so the label's gap holds */
		cash->money = 1;
		vw = hdtext_measure (cash);
		if (vw > cash->maxw) vw = cash->maxw;
		style (cash_lab, HDT_SECTION, C_TEXT2);
		cash_lab->align = HDT_RIGHT;
		cash_lab->ax = 314 - vw - 3.5f;
		cash_lab->maxw = 40;
		centre_y (cash_lab, 7);
	}

	/* the card on the left: the picture and the cargo panel; a 3D view
	 * in it (a ship's) on a card of its own */
	e = add (E_GLASS, L_BACK, 0, BODY_Y0, 120.5f, 166.5f);
	e->flags = F_ALL;
	for (i = 0; i < fr_ca_n; i++)
		if (fr_ca[i][2] <= 121 && fr_ca[i][1] >= BODY_Y0) {
			e = add (E_GLASS, L_BACK, fr_ca[i][0] + 1, fr_ca[i][1], fr_ca[i][2], fr_ca[i][3] + 0.5f);
			e->flags = F_ALL | F_VIEW;
			/* the black edge the game draws beside it went with its black */
			for (y = fr_ca[i][1]; y < fr_ca[i][3] && y < CTRL_Y; y++)
				if (out[y * ST_W + fr_ca[i][2]] == 0) out[y * ST_W + fr_ca[i][2]] = 255;
		}
	pic = find_blob (out, 0, 15, 119, FOOT_Y - 1, bb);
	if (pic) {
		/* the picture flush with the card's top, as with its sides:
		 * where the game left the screen over it empty, a matte as
		 * dark as a portrait's backdrop, framed with it */
		if (!inplace && bb[1] - BODY_Y0 - bb[0] >= 1 && bb[1] - BODY_Y0 < 12) {
			quad (L_BACK, bb[0], BODY_Y0 + bb[0], bb[2], bb[1], 0x000000, 0.6f);
			bb[1] = BODY_Y0 + bb[0];
			/* a portrait starts there too, over the matte */
			hdui_face_top = bb[1];
		}
		e = add (E_FRAME, L_OVER, bb[0], bb[1], bb[2], bb[3]);
		e->a = 0;
		e->c = 1;
		/* the cargo panel: clear of the frame */
		panel_y = bb[3] + 2;
	}
	quad (L_BACK, 1, panel_y, 119.5f, 165.5f, 0xffffff, 0.03f);
	rule (L_BACK, 4, 117, panel_y, 0xffffff, 0.10f);
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 >= 118 || r->role || r == cash || r == cash_lab || hide_bg (r)) continue;
		if (r->y >= 134 && r->y <= 142)
			style (r, HDT_SECTION, C_TEXT2);
		else if (r->y > 142 && r->col == 13) {
			style (r, HDT_LABEL, C_TEXT2);
			/* off the card's edge */
			if (r->x0 <= 3 && !inplace) {
				r->align = HDT_LEFT;
				r->ax = 5;
			}
		}
		else if (r->y > 142 && r->col == 12)
			style (r, HDT_VALUE, C_TEXT);
		else if (r->y < 134) {
			/* on the picture (a call's caption): in the
			 * game's own colour, as the picture's words are */
			r->role = HDT_BODY;
			r->rgb = 0;
		} else
			style (r, HDT_BODY, C_TEXT);
	}

	/* the right */
	if (grid || mounts) {
		int nc = 0, rows, idx[6];
		float cw = (COL_X1 - COL_X0 - 3) / 2, y1 = foot ? 149 : 166.5f, ch, in = mounts ? 2.5f : 0;
		for (i = 0; i < n_btn && nc < 6; i++)
			if (btn[i].x >= 122 && btn[i].x <= 126 && btn[i].y < FOOT_Y) idx[nc++] = i;
		rows = (nc + 1) / 2;
		/* the mounts: a short card each, at the top */
		ch = mounts ? 44 : (y1 - BODY_Y0 - 3 * (rows - 1)) / rows;
		/* ... in a panel, as the other pages' rows are */
		if (mounts) {
			e = add (E_GLASS, L_BACK, COL_X0, BODY_Y0, COL_X1, 166.5f);
			e->flags = F_ALL;
			cw -= in;
		}
		for (j = 0; j < nc; j++) {
			struct button *b = &btn[idx[j]];
			int next = j + 1 < nc ? btn[idx[j + 1]].y : FOOT_Y;
			float x0 = COL_X0 + in + (j & 1) * (cw + 3), y0 = BODY_Y0 + in + (j / 2) * (ch + 3);
			grid_card (b, x0, y0, x0 + cw, y0 + ch, space, next, mounts ? I_GUN : 0);
		}
	} else {
		float colx[16], colmax[16], money = NAN, room = NAN;
		int one_ship = 0, ncol = 0, row = 0, lasty = -1000, list = 0, lfirst = 0, lcount = 0, no_plus = 0, no_minus = 0, fitted = -1;

		e = add (E_GLASS, L_BACK, COL_X0, inplace ? 0.5f : BODY_Y0, COL_X1, 166.5f);
		e->flags = F_ALL;
		e->a = heads ? 11.5f : 0;
		for (i = 0; i < n_btn && ships; i++)
			one_ship += btn[i].x >= 118 && btn[i].x <= 126 && btn[i].y < FOOT_Y;
		one_ship = one_ship == 1;

		/* the one ship viewed: its row's figures, right of its name,
		 * columns first (under the heads the list's are under), on
		 * the list's grid in its face */
		for (i = 0; i < n_runs && one_ship && mode == TABLE; i++) {
			const struct hdtext_run *r = &runs[i];
			if (!ON_PAGE (r) || r->x0 < 200 || r->y >= FOOT_Y || !(r->kind & HDT_K_DIGIT) || ncol >= 16) continue;
			for (j = 0; j < n_btn && abs (btn[j].y + 1 - r->y) > 2; j++);
			for (k = 0; k < ncol && colx[k] != r->x0; k++);
			if (j == n_btn || k < ncol) continue;
			colx[ncol] = r->x0;
			colmax[ncol++] = r->ink1;
		}

		/* a table's numeric columns: right aligned on their widest, if
		 * nothing on their right comes nearer */
		for (i = 0; i < n_runs && mode == TABLE; i++) {
			const struct hdtext_run *r = &runs[i];
			int n = 0, ok = 1;
			float mx1 = 0;
			if (!ON_PAGE (r) || r->x0 < 118 || r->y < 8 || r->y >= FOOT_Y || !(r->kind & HDT_K_NUMERIC)) continue;
			for (k = 0; k < ncol; k++)
				if (colx[k] == r->x0) break;
			if (k < ncol || ncol >= 16) continue;
			for (j = 0; j < n_runs; j++)
				if (runs[j].x0 == r->x0 && runs[j].y >= 8 && runs[j].y < FOOT_Y &&
				    (runs[j].kind & HDT_K_NUMERIC) && !cut_row[runs[j].y]) {
					n++;
					if (runs[j].ink1 > mx1) mx1 = runs[j].ink1;
				}
			for (j = 0; j < n_runs && ok; j++) {
				const struct hdtext_run *q = &runs[j];
				if (q->x0 != r->x0 || q->y < 8 || q->y >= FOOT_Y || !(q->kind & HDT_K_NUMERIC) || cut_row[q->y]) continue;
				for (k = 0; k < n_runs; k++)
					if (runs[k].y == q->y && runs[k].ink0 > q->ink1 && runs[k].ink0 < mx1 + 4)
						ok = 0;
			}
			if (n >= 3 && ok) {
				colx[ncol] = r->x0;
				colmax[ncol++] = mx1;
			}
		}
		for (i = 0; i < n_runs; i++) {
			struct hdtext_run *r = &runs[i];
			if (!ON_PAGE (r) || r->x0 < 118 || r->y >= FOOT_Y) continue;
			if (hide_bg (r)) continue;
			if (r->y <= 4) {
				style (r, HDT_SECTION, C_TEXT2);
				if (!inplace) centre_y (r, BODY_Y0 + 5.75f);
				continue;
			}
			style_body (r);
			centre_y (r, map_y (r->y + 3.5f));
			for (k = 0; k < ncol; k++)
				if (colx[k] == r->x0) {
					r->align = HDT_RIGHT;
					r->ax = colmax[k];
					r->scale = 0.92f;
				}
			/* a cut piece keeps its size: the game's columns
			 * crowded it, ours have room */
			if (r->y >= 8 && cut_row[r->y] && (r->kind & HDT_K_NUMERIC)) r->maxw = hdtext_measure (r) + 1;
		}
		place_heads (colx, ncol);
		/* the view of one ship (the shipyard's): its details as a key
		 * and value table, the keys at one indent, the values in one
		 * face from one edge, where the game began them */
		if (ships && !inplace) {
			float kx = 1000, vx = 1000;
			for (i = 0; i < n_runs; i++) {
				const struct hdtext_run *r = &runs[i];
				if (!ON_PAGE (r) || r->hide || r->x0 < 128 || r->x0 > 140 || r->y <= first + 8 || r->y >= 118) continue;
				for (k = 0; k < n_btn && abs (btn[k].y + 1 - r->y) > 2; k++);
				if (k < n_btn) continue;
				for (j = 0; j < n_runs; j++)
					if (runs[j].y == r->y && runs[j].x0 >= 200 && runs[j].x0 < vx) vx = runs[j].x0;
				if (r->x0 < kx) kx = r->x0;
			}
			for (i = 0; i < n_runs && vx < 1000; i++) {
				struct hdtext_run *r = &runs[i];
				if (!ON_PAGE (r) || r->hide || r->x0 < 128 || r->y <= first + 8 || r->y >= 124) continue;
				if (r->x0 <= 140) {
					/* a key, or the head of what follows */
					if (r->y < 118) style (r, HDT_BODY, C_TEXT2);
					r->align = HDT_LEFT;
					r->ax = kx;
					r->maxw = (r->y < 118 ? vx - 4 : COL_X1 - 2) - kx;
				} else if (r->x0 >= 200 && r->y < 118) {
					style (r, HDT_VALUE, C_TEXT);
					r->scale = 1;
					r->align = HDT_LEFT;
					r->ax = vx;
					r->maxw = COL_X1 - 2.5f - vx;
				}
			}
		}
		/* the market's and the upgrades' rows: what the page says the
		 * cash and the hold's room are, to show a + that cannot buy or
		 * fit and a - with nothing to sell or take out as off (a click
		 * on one still goes to the game, which says why) */
		if (mode == TABLE && !inplace) list = game_list (&lfirst, &lcount);
		if (list) {
			for (i = 0; i < n_runs; i++)
				if (runs[i].hash == H_STATS_ROOM && runs[i].y >= 144 && runs[i].y <= 152 && runs[i].ink0 <= 2) {
					for (j = 0; j < n_runs; j++)
						if (runs[j].y == runs[i].y && runs[j].x0 > runs[i].ink1 && runs[j].x0 < 56) {
							room = page_num (&runs[j]);
							break;
						}
					break;
				}
			money = page_num (cash);
		}
		if (mode == TABLE || mode == ROWS) {
			for (i = 0; i < n_btn; i++) {
				struct button *b = &btn[i];
				float h, w, cx = b->x + 5, cy = map_y (b->y + 4.25f);
				int hh;
				if (b->x < 118 || b->y >= FOOT_Y) continue;
				if (b->y != lasty && list) {
					/* the row's cells: the market's price, stock and
					 * cargo; the upgrades' prices and weight */
					struct hdtext_run *cell[6], *name = NULL;
					int nc = row_cells (b->y + 1, cell, 6), idx = lfirst + row;
					float price = nc ? page_num (cell[0]) : NAN, end = nc ? page_num (cell[nc - 1]) : NAN;
					no_plus = no_minus = 0;
					fitted = -1;
					if (idx >= lcount || nc < 2) list = 0;
					else if (list == LIST_MARKET) {
						/* none in stock ('-') the game will not sell */
						no_plus = price > money || room == 0 || (nc >= 3 && page_num (cell[1]) == 0);
						no_minus = end == 0;
					} else {
						fitted = upg_fitted (idx);
						no_plus = price > money || end > room;
						no_minus = fitted == 0;
					}
					/* a fitted upgrade: a tick after its name, a pip
					 * for each one past the first */
					for (j = 0; j < n_runs && fitted > 0 && list; j++)
						if (ON_PAGE (&runs[j]) && runs[j].y == b->y + 1 && runs[j].x0 >= 146 && runs[j].x0 < 200) {
							name = &runs[j];
							break;
						}
					if (name) {
						float nx = (name->ax > 0 ? name->ax : name->ink0) + name->dx + hdtext_measure (name) + 2.5f;
						float ny = map_y (b->y + 4.25f), ih = 5.5f, iw = ih * sy / sx;
						e = add (E_ICON, L_OVER, nx, ny - ih / 2, nx + iw, ny + ih / 2);
						e->icon = I_CHECK;
						for (k = 1; k < fitted && k < 8; k++)
							quad (L_OVER, nx + iw + 0.5f + (k - 1) * 2.2f, ny - 0.6f, nx + iw + 1.7f + (k - 1) * 2.2f, ny + 0.6f,
							      C_ACCENT, 0.9f);
					}
				}
				if (b->y != lasty) {
					float r0 = map_y (b->y - 1), r1 = map_y (b->y + 9);
					lasty = b->y;
					if (mode == TABLE) {
						if (row & 1) quad (L_UNDER, COL_X0 + 1, r0, COL_X1 - 1, r1, 0xffffff, 0.025f);
						e = add (E_ROW, L_UNDER, COL_X0 + 1, r0 + 0.25f, COL_X1 - 1, r1 - 0.25f);
						e->flags = F_HOVERONLY;
						e->hot = add_hot (COL_X0 + 1, r0, COL_X1 - 1, r1, 0, 0, 0);
					} else {
						e = add (E_ROW, L_UNDER, COL_X0 + 1, r0 + 0.25f, COL_X1 - 1, r1 - 0.25f);
						e->hot = add_hot (COL_X0, r0, COL_X1, r1, b->x + 4, b->y + 4, 1);
					}
					row++;
				}
				h = 8.5f * my_k;
				if (h < 6.5f) h = 6.5f;
				w = h * sy / sx;
				if (w > 9.5f) w = 9.5f;
				if (mode == ROWS)
					hh = e->hot;
				else if (inplace)
					hh = add_hot (b->x, b->y, b->x + 10, b->y + 9, 0, 0, 0);
				else
					hh = add_hot (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, b->x + 4, b->y + 4, 1);
				e = add (E_BTN, L_UNDER, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
				e->icon = mode == ROWS ? I_CHEVRON : b->x <= 126 ? I_PLUS : ships ? I_EYE : I_MINUS;
				e->pressed = b->pressed;
				e->hot = hh;
				if (list && (b->x <= 126 ? no_plus : no_minus)) e->flags |= F_OFF;
				b->used = 1;
			}
			/* a long list: where the rows shown are in it, a thin
			 * bar on the column's right edge */
			if (list && row && lcount > row) {
				float t0 = map_y (first - 1), t1 = map_y (last + 9), k0, k1;
				k0 = t0 + (t1 - t0) * lfirst / lcount;
				k1 = t0 + (t1 - t0) * (lfirst + row < lcount ? lfirst + row : lcount) / lcount;
				quad (L_OVER, COL_X1 - 2.6f, t0, COL_X1 - 1.6f, t1, 0xffffff, 0.08f);
				quad (L_OVER, COL_X1 - 2.6f, k0, COL_X1 - 1.6f, k1, C_ACCENT, 0.85f);
			}
		} else if (mode == CARDS) {
			/* cards the layout does not know (the bulletin board's) */
			for (i = 0; i < n_btn; i++) {
				struct button *b = &btn[i];
				float y0, y1;
				if (b->x < 122 || b->x > 126 || b->y >= FOOT_Y) continue;
				y0 = map_y (b->y - 3);
				y1 = map_y (b->y + 30.5f);
				for (j = i + 1; j < n_btn; j++)
					if (btn[j].x == b->x && btn[j].y > b->y && map_y (btn[j].y - 3) - 1.5f < y1) {
						y1 = map_y (btn[j].y - 3) - 1.5f;
						break;
					}
				e = add (E_CARD, L_UNDER, COL_X0 + 0.5f, y0, COL_X1 - 0.5f, y1);
				e->icon = I_DOC;
				e->a = 131;
				e->b = map_y (b->y + 4.25f);
				e->c = 64;
				e->hot = add_hot (COL_X0, y0, COL_X1, y1, b->x + 4, b->y + 4, 1);
				b->used = 1;
				/* the advert's lines (and its icon with them) in the
				 * card's middle, however many there are */
				{
					float t0 = 1e9f, t1 = -1e9f, d;
					int nxt = FOOT_Y;
					for (j = 0; j < n_btn; j++)
						if (btn[j].x == b->x && btn[j].y > b->y && btn[j].y < nxt) nxt = btn[j].y;
					for (j = 0; j < n_runs; j++) {
						const struct hdtext_run *r = &runs[j];
						if (!ON_PAGE (r) || r->hide || r->x0 < 128 || r->y < b->y - 3 || r->y >= nxt - 3) continue;
						if (r->y < t0) t0 = r->y;
						if (r->y + 7 > t1) t1 = r->y + 7;
					}
					if (t1 > t0) {
						d = (y0 + y1) / 2 - (map_y (t0) + map_y (t1)) / 2;
						for (j = 0; j < n_runs; j++) {
							struct hdtext_run *r = &runs[j];
							if (ON_PAGE (r) && !r->hide && r->x0 >= 128 && r->y >= b->y - 3 && r->y < nxt - 3) r->dy += d;
						}
						e->b += d;
					}
				}
			}
		}
	}

	/* the foot: go back on the left, more on the right (unless the game's
	 * own arrows show, where a pill would be on the left) */
	for (i = 0; i < n_runs; i++)
		if (ON_PAGE (&runs[i]) && runs[i].y >= FOOT_Y && runs[i].x0 >= 118 && runs[i].hash == H_MORE && !more) odd_more = 1;
	for (i = 0; i < n_btn; i++)
		if (btn[i].x >= 118 && btn[i].y >= FOOT_Y) foot_pill (&btn[i], 16, !odd_more && !inplace);
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->y < FOOT_Y || r->x0 < 118) continue;
		if (r->hash == H_MORE) {
			style (r, HDT_LABEL, C_TEXT);
			if (more)
				more_pager (raster, out, r, more, mx, my);
			else {
				/* arrows it does not know: they show, and a click
				 * on the pill goes where it is */
				e = add (E_PILL, L_UNDER, COL_X0 - 1, r->y - 1.5f, r->ink1 + 5, r->y + 10);
				e->flags = F_NOICON;
				e->hot = add_hot (e->x0, e->y0, e->x1, e->y1, 0, 0, 0);
			}
		} else if (!r->role)
			style_body (r);
	}

	money_runs ();

	/* the rest of the right column: nothing (where the game's boxes were);
	 * the foot too, its pills having moved off the game's boxes */
	if (!inplace) add_hot (118, 0, ST_W, foot && odd_more ? FOOT_Y : CTRL_Y, 1, 1, 1);
}

/* A label and its value the game printed as one string ("Name:value",
 * "Name: value"): two runs, the value a little clear of the colon */
static void split_colons (float right)
{
	int i, k, n;
	char c[96];
	unsigned char sp[96];

	for (i = n_runs - 1; i >= 0; i--) {
		const struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 >= right || r->y < 10 || !(r->kind & HDT_K_LETTER)) continue;
		n = hdtext_chars (r, c, sp, 96);
		for (k = 1; k < n - 1; k++)
			if (c[k] == ':') break;
		/* after a letter: not a time */
		if (k >= n - 1 || (c[k - 1] | 32) < 'a' || (c[k - 1] | 32) > 'z' || !hdtext_split (i, k + 1))
			continue;
		n_runs = hdtext_runs (&runs);
		runs[i + 1].align = HDT_LEFT;
		runs[i + 1].ax = runs[i + 1].ink0 + 3;
	}
}

/* A row of the cargo panel the game printed with its count in the same
 * string ("label 45"): two runs, so the count goes in its column */
static void split_counts (float right)
{
	int i, k, j, n;
	char c[64];
	unsigned char sp[64];

	for (i = n_runs - 1; i >= 0; i--) {
		const struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 < right || r->y <= 142 || !(r->kind & HDT_K_LETTER)) continue;
		n = hdtext_chars (r, c, sp, 64);
		for (k = n - 1; k >= 1 && !sp[k]; k--)
			;
		if (k < 1) continue;
		for (j = k; j < n && c[j] >= '0' && c[j] <= '9'; j++)
			;
		if (j == n && hdtext_split (i, k)) n_runs = hdtext_runs (&runs);
	}
}

/* The cargo panel under the 3D view: each column's figures right-set
 * under its head, and the cash as the station pages show it: its label
 * a head, its figure in the accent, at the panel's right */
static void f3_panel (float right)
{
	struct hdtext_run *head[8], *cash_lab = NULL, *cash = NULL;
	float head_y = 138, hr[8], head_x1 = 0, fig_x0 = 320;
	int i, k, nh = 0;

	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->x0 < right || r->y < 132) continue;
		if (r->y <= 142 && nh < 8) {
			head[nh] = r;
			hr[nh] = r->ink0 + hdtext_measure (r);
			if (hr[nh] > head_x1) head_x1 = hr[nh];
			head_y = r->y;
			nh++;
		} else if (r->hash == H_CASH && r->y > 142)
			cash_lab = r;
	}
	for (i = 0; i < n_runs && cash_lab; i++) {
		struct hdtext_run *r = &runs[i];
		if (r != cash_lab && ON_PAGE (r) && (r->kind & HDT_K_NUMERIC) && r->y >= cash_lab->y + 8 &&
		    r->y <= cash_lab->y + 12 && r->ink0 <= cash_lab->ink1 && r->ink1 >= cash_lab->ink0)
			cash = r;
	}
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r == cash || r->x0 < right || r->y <= 142 || !(r->kind & HDT_K_NUMERIC) ||
		    (r->kind & HDT_K_LETTER))
			continue;
		for (k = 0; k < nh; k++)
			if (r->ink1 >= head[k]->ink0 - 2 && r->ink0 <= head[k]->ink1 + 2) {
				/* its room is its head's, not what was left of
				 * the game's place for it once the head is set
				 * narrower */
				r->align = HDT_RIGHT;
				r->ax = hr[k];
				r->maxw = hr[k] - head[k]->ink0 + 6;
				if (hr[k] - hdtext_measure (r) < fig_x0) fig_x0 = hr[k] - hdtext_measure (r);
				break;
			}
	}
	/* the rows' names clear of the card's edge, as the sheet's are, and
	 * never into the first column of figures */
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		float x0 = right + 7;
		if (!ON_PAGE (r) || r == cash_lab || r->x0 < right || r->x0 > right + 6 || r->y <= 142 ||
		    (r->kind & HDT_K_NUMERIC) || !(r->kind & HDT_K_LETTER))
			continue;
		r->align = HDT_LEFT;
		r->ax = x0;
		if (fig_x0 - 3 - x0 > 8) r->maxw = fig_x0 - 3 - x0;
	}
	if (cash_lab && cash) {
		style (cash_lab, HDT_SECTION, C_TEXT2);
		cash_lab->align = HDT_RIGHT;
		cash_lab->ax = 314;
		centre_y (cash_lab, head_y + 3.5f);
		style (cash, HDT_VALUE, C_ACCENT);
		cash->align = HDT_RIGHT;
		cash->ax = 314;
		if (head_x1 > 0) cash->maxw = 314 - head_x1 - 4;
		centre_y (cash, cash_lab->y + 3.5f);
	}
}

/* The F3 pages' left sheet as a spec sheet.  Each column of labels
 * (runs ending in a colon) is one grid: the labels at its left, every
 * value at one x clear of the widest label.  A second value on a label's
 * row is the row above run long (the game wraps it there): it goes under
 * that row's value, quieter, and the sheet below moves down for it.  A
 * group of rows the game put at the sheet's foot, under a list, comes up
 * under a heading of ours with the list after it; a heading with nothing
 * under it says so.  Our words, never the game's: hdtext_add. */
#define SHEET_X		5	/* the sheet's left column: 20 px inside the glass at 1600 */
#define SUB_DY		8	/* a value's second line */
#define ROW_DY		10	/* the game's row pitch */
#define LIST_DX		5	/* a list item's step in, its dash in the step */
static int sheet_add[8], sheet_head[8], n_sheet_add;	/* our runs; a heading or not */

static void sheet_text (const char *s, float x, float y, float right, int head)
{
	int i = hdtext_add (s, (int) x, (int) y, 15);

	if (i < 0 || n_sheet_add >= 8) return;
	n_runs = hdtext_runs (&runs);
	runs[i].align = HDT_LEFT;
	runs[i].ax = x;
	runs[i].maxw = right - 4 - x;
	sheet_head[n_sheet_add] = head;
	sheet_add[n_sheet_add++] = i;
}

static int on_sheet (const struct hdtext_run *r, float right)
{
	return ON_PAGE (r) && r->y >= 10 && r->x0 < right && !(r->kind & HDT_K_ICON) && !(r->col == BG && !r->shadow);
}

/* A heading: in the page colour by its shadow, or all capitals without
 * figures, but not after a row's label on its line (that is the row's
 * value, whatever its case) */
static int is_head (const struct hdtext_run *r)
{
	int k;

	if (r->col == BG && r->shadow) return 1;
	if (!(r->kind & HDT_K_CAPS) || (r->kind & HDT_K_NUMERIC)) return 0;
	for (k = 0; k < n_runs; k++)
		if (runs[k].y == r->y && (runs[k].kind & HDT_K_COLON) && runs[k].x0 < r->x0 && ON_PAGE (&runs[k]))
			return 0;
	return 1;
}

static void f3_sheet (float right)
{
	static float ty[1024];	/* each run's new y; hdtext finds at most 1024 */
	float lx[4], vx[4], top_end = 0, shift_y = 1000, list0 = 1000;
	short gx[4], row_y[32];
	int i, k, g, ng = 0, nrow = 0, foot = 0, n0 = n_runs < 1024 ? n_runs : 1024;

	n_sheet_add = 0;
	/* the label columns, each as wide as its widest label */
	for (i = 0; i < n0; i++) {
		struct hdtext_run *r = &runs[i];
		ty[i] = r->y;
		if (!on_sheet (r, right) || !(r->kind & HDT_K_COLON)) continue;
		for (g = 0; g < ng && abs (gx[g] - r->x0) > 3; g++)
			;
		if (g == ng) {
			if (ng == 4) continue;
			gx[ng] = r->x0;
			lx[ng] = r->x0 <= 10 ? SHEET_X : r->x0;
			vx[ng++] = 0;
		}
		r->role = HDT_LABEL;
		r->align = HDT_LEFT;
		r->ax = lx[g];
		if (lx[g] + hdtext_measure (r) + 8 > vx[g]) vx[g] = lx[g] + hdtext_measure (r) + 8;
		if (g == 0 && nrow < 32) row_y[nrow++] = r->y;
	}
	/* each value at its column's x; a second on a row under the row above's */
	for (i = 0; i < n0; i++) {
		struct hdtext_run *r = &runs[i], *lab = NULL, *above = NULL;
		float lim = right - 4;
		if (!on_sheet (r, right) || (r->kind & HDT_K_COLON) || is_head (r)) continue;
		for (k = 0; k < n0; k++)
			if (runs[k].y == r->y && (runs[k].kind & HDT_K_COLON) && on_sheet (&runs[k], right) &&
			    runs[k].x0 < r->x0 && (!lab || runs[k].x0 > lab->x0))
				lab = &runs[k];
		for (g = 0; lab && g < ng && abs (gx[g] - lab->x0) > 3; g++)
			;
		if (!lab || g == ng) continue;
		for (k = 0; k < ng; k++)
			if (lx[k] > lx[g] && lx[k] - 4 < lim) lim = lx[k] - 4;
		r->align = HDT_LEFT;
		r->ax = vx[g];
		r->maxw = lim - vx[g];
		if (i == 0 || runs[i - 1].ax != vx[g] || runs[i - 1].y != r->y) continue;
		for (k = 0; k < n0; k++)
			if ((runs[k].kind & HDT_K_COLON) && on_sheet (&runs[k], right) && abs (gx[g] - runs[k].x0) <= 3 &&
			    runs[k].y < r->y && (!above || runs[k].y > above->y))
				above = &runs[k];
		if (!above) continue;
		r->role = HDT_DESC;
		ty[i] = above->y + SUB_DY;
		if (r->y < shift_y) shift_y = r->y;
	}
	/* under a value's second line the sheet moves down */
	for (i = 0; i < n0; i++)
		if (on_sheet (&runs[i], right) && runs[i].role != HDT_DESC && runs[i].y >= shift_y) ty[i] += SUB_DY;
	/* rows the game put at the foot, under a list: up under a heading of
	 * ours, the list after them */
	for (k = 1; k < nrow && !foot; k++)
		if (row_y[k] - row_y[k - 1] > 3 * ROW_DY) {
			top_end = row_y[k - 1] + (shift_y <= row_y[k - 1] ? SUB_DY : 0);
			foot = k;
		}
	if (foot) {
		float yh = top_end + 14, ylist = yh + ROW_DY * (nrow - foot + 1);
		for (i = 0; i < n0; i++)
			if (on_sheet (&runs[i], right) && runs[i].y > row_y[foot - 1] && runs[i].y < row_y[foot] &&
			    runs[i].y < list0)
				list0 = runs[i].y;
		for (i = 0; i < n0; i++) {
			if (!on_sheet (&runs[i], right)) continue;
			for (k = foot; k < nrow && row_y[k] != runs[i].y; k++)
				;
			if (k < nrow) ty[i] = yh + ROW_DY * (k - foot + 1);
			else if (runs[i].y >= list0 && runs[i].y < row_y[foot]) {
				/* the list: on the heading's pitch; an item without a
				 * value is a list item, a step in, not a row's name */
				ty[i] = ylist + runs[i].y - list0;
				if (!(runs[i].kind & HDT_K_COLON) && !is_head (&runs[i])) {
					runs[i].role = HDT_BODY;
					runs[i].align = HDT_LEFT;
					runs[i].ax = (runs[i].x0 <= 10 ? SHEET_X : runs[i].x0) + LIST_DX;
					runs[i].maxw = right - 4 - runs[i].ax;
				}
			}
		}
		sheet_text ("Fitted equipment", SHEET_X, yh, right, 1);
	}
	/* in place; the left column clear of the glass's edge */
	for (i = 0; i < n0; i++) {
		struct hdtext_run *r = &runs[i];
		if (!on_sheet (r, right)) continue;
		r->dy = ty[i] - r->y;
		if (r->x0 <= 4 && !r->ax) {
			r->align = HDT_LEFT;
			r->ax = SHEET_X;
		}
	}
	/* a heading with nothing in its column under it, to the next */
	for (i = 0; i < n0; i++) {
		const struct hdtext_run *r = &runs[i];
		float next = 166;
		int below = 0;
		if (!on_sheet (r, right) || r->col == BG || !is_head (r)) continue;
		for (k = 0; k < n0; k++) {
			const struct hdtext_run *q = &runs[k];
			if (k == i || !on_sheet (q, right) || q->y <= r->y || q->x0 < r->x0 - 6 || q->x0 >= r->x0 + 100)
				continue;
			if (is_head (q)) {
				if (q->y < next) next = q->y;
			} else if (q->y < next)
				below = 1;
		}
		if (!below) sheet_text ("None on record", r->ax ? r->ax : r->ink0, r->y + r->dy + ROW_DY, right, 0);
	}
}

/* The ship's status pages (F3): one glass sheet, the 3D view on a card */
static void layout_f3 (unsigned char *out)
{
	int i, j, k, n0, view = -1, top_head = 1000;
	float right = 319;
	struct elem *e;

	(void) out;
	for (i = 0; i < fr_ca_n; i++)
		if (fr_ca[i][0] >= 180 && fr_ca[i][1] <= 4 && fr_ca[i][2] >= 300) view = i;
	if (view >= 0) {
		right = fr_ca[view][0] - 2.5f;
		/* the view on a card of its own, over the cargo panel's */
		e = add (E_GLASS, L_BACK, right + 3, 0.5f, 318, fr_ca[view][3] + 1);
		e->flags = F_ALL | F_VIEW;
	}
	split_colons (right);
	if (view >= 0) split_counts (right);
	n0 = n_runs;
	f3_sheet (right);
	e = add (E_GLASS, L_BACK, 1, 0.5f, right, 166.5f);
	e->flags = F_ALL;
	e->a = 14;
	quad (L_BACK, 8, 13.75f, 32, 14.5f, C_ACCENT, 0.9f);
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (ON_PAGE (r) && r->y > 10 && r->x0 < right && (r->kind & HDT_K_CAPS) &&
		    !(r->kind & HDT_K_NUMERIC) && r->col != BG && r->y < top_head)
			top_head = r->y;
	}
	if (view >= 0)
		for (i = 0; i < n_runs; i++)
			if (ON_PAGE (&runs[i]) && runs[i].x0 >= right && runs[i].y >= 132) {
				e = add (E_GLASS, L_BACK, right + 3, 133.5f, 318, 166.5f);
				e->flags = F_ALL;
				break;
			}
	for (i = 0; i < n0; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r)) continue;
		if (r->col == BG && r->shadow && r->x0 < right) {
			/* in the page colour, seen by its shadow alone: a column's
			 * heading.  The game heads the hold's second column too;
			 * with nothing in it, the heading is said once */
			for (k = 0; k < i; k++)
				if (runs[k].y == r->y && runs[k].hash == r->hash) break;
			if (k < i) {
				for (j = 0; j < n_runs; j++)
					if (ON_PAGE (&runs[j]) && runs[j].y > r->y && runs[j].y < 132 && runs[j].col != BG &&
					    runs[j].x0 >= r->x0 && runs[j].x0 <= r->x0 + 40) break;
				if (j == n_runs) {
					r->hide = 1;
					continue;
				}
			}
			style (r, HDT_SECTION, C_TEXT2);
			continue;
		}
		if (hide_bg (r)) continue;
		if (r->x0 >= right) {
			if (r->y >= 132 && r->y <= 142) style (r, HDT_SECTION, C_TEXT2);
			else if (r->kind & HDT_K_NUMERIC) style (r, HDT_VALUE, C_TEXT);
			else style (r, HDT_LABEL, C_TEXT2);
		} else if (r->y >= 2 && r->y <= 7 && r->x0 < 120) {
			style (r, HDT_TITLE, C_TEXT);
			r->align = HDT_LEFT;
			r->ax = 8;
			r->maxw = right - 12;
		} else if (is_head (r)) {
			style (r, HDT_SECTION, C_ACCENT);
			if (r->y > top_head) {
				for (k = 0; k < i; k++)
					if (runs[k].y == r->y && (runs[k].kind & HDT_K_CAPS) && runs[k].x0 < right) break;
				if (k == i) rule (L_BACK, 5, right - 4, r->y + r->dy - 3.5f, 0xffffff, 0.10f);
			}
		} else if ((r->kind & HDT_K_COLON) || (r->role == HDT_LABEL && r->x0 < right))
			style (r, HDT_LABEL, C_TEXT2);
		else if (r->role == HDT_BODY && r->x0 < right) {
			style (r, HDT_BODY, C_TEXT);
			quad (L_BACK, r->ax - LIST_DX + 0.5f, r->y + r->dy + 3.25f, r->ax - 2.5f, r->y + r->dy + 3.75f,
			      C_ACCENT, 0.9f);
		} else
			/* a value on the sheet reads as its neighbours do: the
			 * condensed figures are the panel's; a second line is quieter */
			style (r, r->role == HDT_DESC ? HDT_DESC : HDT_BODY, r->role == HDT_DESC ? C_TEXT2 : C_TEXT);
	}
	for (i = 0; i < n_sheet_add; i++) {
		struct hdtext_run *r = &runs[sheet_add[i]];
		if (!sheet_head[i]) style (r, HDT_BODY, C_TEXT2);
		else {
			style (r, HDT_SECTION, C_ACCENT);
			rule (L_BACK, 5, right - 4, r->y - 3.5f, 0xffffff, 0.10f);
		}
	}
	if (view >= 0) f3_panel (right);
	for (i = 0; i < n_btn; i++) {
		struct button *b = &btn[i];
		if (b->x <= 10) {
			/* a row of the hold: jettison, in place (no larger target: it throws cargo away) */
			e = add (E_ROW, L_UNDER, 2, b->y - 1.5f, right - 2, b->y + 10);
			e = add (E_BTN, L_UNDER, b->x + 0.5f, b->y, b->x + 9.5f, b->y + 8.5f);
			e->icon = I_EJECT;
			e->pressed = b->pressed;
			e->hot = add_hot (b->x, b->y, b->x + 10, b->y + 9, 0, 0, 0);
		} else {
			struct hdtext_run *lab = label_of (b, 8, 22);
			float x1 = lab ? lab->ink1 + 5 : b->x + 14;
			e = add (E_PILL, L_UNDER, b->x - 2.5f, b->y - 1.5f, x1, b->y + 10);
			e->icon = lab && lab->hash == H_REFUEL ? I_DROP : I_CHEVRON;
			e->a = b->x + 4.5f;
			e->b = b->y + 4.25f;
			e->c = 26;
			e->pressed = b->pressed;
			e->hot = add_hot (e->x0, e->y0, e->x1, e->y1, b->x + 4, b->y + 4, 1);
			if (lab) style (lab, HDT_LABEL, C_TEXT);
		}
		b->used = 1;
	}
}

/* The preferences: headings over groups of check boxes and radios */
static void layout_prefs (const unsigned int *pal)
{
	struct hdtext_run *head[16];
	int i, j, k, nh = 0;
	struct elem *e;

	e = add (E_GLASS, L_BACK, 1.5f, 1.5f, 318.5f, 166.5f);
	e->flags = F_ALL;
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		int is_head = (r->kind & HDT_K_COLON) != 0;
		if (!ON_PAGE (r) || hide_bg (r)) continue;
		for (k = 0; k < (int) (sizeof (prefs_heads) / sizeof (prefs_heads[0])); k++)
			if (r->hash == prefs_heads[k]) is_head = 1;
		if (is_head && nh < 16) {
			head[nh++] = r;
			style (r, HDT_SECTION, C_ACCENT);
		} else {
			float mw = 318 - r->ink0;
			style (r, HDT_BODY, C_TEXT);
			for (k = 0; k < n_btn; k++)
				if (btn[k].x > r->ink0 && btn[k].y <= r->y + 7 && btn[k].y + 9 >= r->y &&
				    btn[k].x - 3 - r->ink0 < mw)
					mw = btn[k].x - 3 - r->ink0;
			r->maxw = mw;
		}
	}
	for (i = 0; i < nh; i++) {
		float end = 316;
		for (k = 0; k < nh; k++)
			if (k != i && abs (head[k]->y - head[i]->y) <= 8 && head[k]->ink0 > head[i]->ink0 &&
			    head[k]->ink0 - 8 < end)
				end = head[k]->ink0 - 8;
		rule (L_BACK, head[i]->ink0, end, head[i]->y + 9.5f, 0xffffff, 0.10f);
	}
	/* groups: boxes one above another */
	for (i = 0; i < n_btn; i++) {
		int n = 1, on = 0, radio, last;
		if (btn[i].used) continue;
		last = btn[i].y;
		on = box_on (&btn[i], pal);
		for (j = 0; j < n_btn; j++)
			if (j != i && btn[j].x == btn[i].x && btn[j].y > last && btn[j].y - last <= 15) {
				last = btn[j].y;
				n++;
				on += box_on (&btn[j], pal);
				j = -1;	/* again from the top for the next one down */
			}
		if (nh >= 4)
			radio = btn[i].x < head[1]->x0 && last < head[3]->y;
		else
			radio = n >= 2 && on == 1;
		for (j = 0; j < n_btn; j++) {
			struct button *b = &btn[j];
			struct hdtext_run *lab;
			float x1, nx = 318;
			int h;
			if (b->used || b->x != btn[i].x || b->y < btn[i].y || b->y > last) continue;
			lab = label_of (b, 9, 22);
			for (k = 0; k < n_btn; k++)
				if (btn[k].x > b->x && abs (btn[k].y - b->y) <= 6 && btn[k].x - 2 < nx) nx = btn[k].x - 2;
			if (lab) {
				float w = hdtext_measure (lab);
				if (lab->maxw > 0 && w > lab->maxw) w = lab->maxw;
				x1 = lab->ink0 + w + 3;
			} else
				x1 = b->x + 12;
			if (x1 > nx) x1 = nx;
			h = add_hot (b->x - 2, b->y - 1, x1, b->y + 9, b->x + 4, b->y + 4, 1);
			e = add (E_ROW, L_UNDER, b->x - 2, b->y - 1, x1, b->y + 9);
			e->flags = F_HOVERONLY;
			e->hot = h;
			e = add (radio ? E_RADIO : E_CHECK, L_UNDER, b->x + 0.5f, b->y, b->x + 9, b->y + 8.5f);
			if (box_on (b, pal)) e->flags |= F_ON;
			e->hot = h;
			b->used = 1;
		}
	}
}

/* Load and save: a directory's files in columns of rows, and on the save
 * page the name to save under (a box the game clears black while it is
 * typed in, rubbing letters out in black) with its OK */
#define NAME_X0		138	/* the name's box, ST */
#define NAME_X1		193

static void layout_file (const unsigned char *raster, unsigned char *out, const unsigned int *pal)
{
	struct hdtext_run *name = NULL;
	struct elem *e;
	int i, x, y, ok = 0, counter = 0, box = raster[14 * ST_W + NAME_X1 - 4];
	float title_x = 10;

	/* the game's frame counter (its F key) over the title: not shown */
	if ((counter = hdtext_hide_counter ())) n_runs = hdtext_runs (&runs);
	e = add (E_GLASS, L_BACK, 1.5f, 1.5f, 318.5f, 166.5f);
	e->flags = F_ALL;
	for (i = 0; i < n_btn; i++)
		if (btn[i].y < 10) {
			foot_pill (&btn[i], 24, 0)->icon = I_CHECK;
			ok = 1;
		}
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->role || hide_bg (r)) continue;
		if (r->y <= 8 && ok && r->x0 >= NAME_X0 && r->x0 < NAME_X1) {
			if (box != BG && (pal[r->col] & 0xffffff) == (pal[box] & 0xffffff)) r->hide = 1;
			else name = r;
		} else if (r->y <= 8) {
			/* the title, from the left */
			style (r, HDT_TITLE, C_TEXT);
			r->align = HDT_LEFT;
			r->ax = title_x;
			r->maxw = (ok ? NAME_X0 - 4 : 230) - title_x;
			title_x += hdtext_measure (r) + 1.6f;
		} else if (r->y < 28) {
			style (r, HDT_BODY, C_TEXT2);
		} else if ((r->x0 - 10) % 62 == 0) {
			/* a file (or the way up): the game's columns are 62 wide */
			e = add (E_ROW, L_UNDER, r->x0 - 4, r->y - 1, r->x0 + 57, r->y + 9);
			e->flags = F_HOVERONLY;
			e->hot = add_hot (e->x0, e->y0, e->x1, e->y1, r->x0 + 4, r->y + 3, 1);
			style (r, HDT_BODY, r->col == 3 ? C_TEXT2 : C_TEXT);
			r->align = HDT_LEFT;
			r->maxw = 56;
		}
	}
	/* what is left of the counter */
	if (counter)
		for (y = 2; y <= 13; y++)
			for (x = 0; x < NAME_X0; x++)
				if (out[y * ST_W + x] != BG) out[y * ST_W + x] = 255;
	if (!ok) return;
	for (y = 4; y <= 15; y++)
		memset (out + y * ST_W + NAME_X0 + 2, 255, NAME_X1 - NAME_X0 - 4);
	e = add (E_ROW, L_UNDER, NAME_X0, 2.5f, NAME_X1, 16.5f);
	e->hot = add_hot (e->x0, e->y0, e->x1, e->y1, 155, 9, 1);
	e->pressed = box != BG;	/* being typed in: focused */
	if (name) {
		style (name, HDT_BODY, C_TEXT);
		name->align = HDT_LEFT;
		name->maxw = NAME_X1 - 3 - name->ink0;
	}
	if (e->pressed) {
		float w = name ? hdtext_measure (name) : 0, cx;
		if (name && w > name->maxw) w = name->maxw;
		cx = name ? name->ink0 + w + 0.8f : NAME_X0 + 4;
		e = add (E_QUAD, L_OVER, cx, 4.5f, cx + 0.8f, 14);
		e->rgb = C_ACCENT;
		e->alpha = 1;
		e->flags = F_BLINK;
	}
}

/*
 * Docked, with the 3D view up (the hangar, or the ship from outside): the
 * console says docked and nothing is written over the view but the speed
 * line at its foot.  Station services are a click away on the console
 * (its comms icon), and nothing on the view said so.  Here the view
 * carries the station's home: at its top left a card with the station's
 * name (the speed line gives it: what the ship is relative to, set again
 * from that line's glyphs, which stays as it was), a click on it a click
 * on that icon, and under it a card for each of the services (market,
 * shipyard, bulletin board, launch), in the services page's style, each
 * opening the services page and then the service on it (svc_chain).
 * The view itself is the hangar, lit (gl.c).
 */
#define COMMS_X		57	/* the console's comms icon: station services when docked */
#define COMMS_Y		193

/* The services' labels: the game's own words, found once in its memory by
 * their letters' hash (read only; nothing of them is in this source), or
 * taken from the services page when it is seen. */
static const unsigned int svc_hash[] = { H_SVC_MARKET, H_SVC_SHIPYARD, H_SVC_BOARD, H_SVC_LAUNCH };
#define N_SVC	(int) (sizeof (svc_hash) / sizeof (svc_hash[0]))
static char svc_label[N_SVC][40];
static short svc_tx[N_SVC], svc_ty[N_SVC];
static unsigned int svc_at[N_SVC];	/* hdui_frame count when seen on the services page */
static unsigned int ui_frames;		/* hdui_frame calls */

extern signed char m68kram[];
#define ST_MEM_TOP	0x110000

static int svc_index (unsigned int h)
{
	int i;
	for (i = 0; i < N_SVC; i++)
		if (svc_hash[i] == h) return i;
	return -1;
}

static void svc_find_labels (void)
{
	static int done;
	const unsigned char *m = (const unsigned char *) m68kram;
	int i, k, left = 0;

	if (done) return;
	done = 1;
	for (i = 0; i < N_SVC; i++)
		if (!svc_label[i][0]) left++;
	for (i = 1; i < ST_MEM_TOP && left; i++) {
		unsigned int h = 2166136261u;
		if (m[i] < 0x20 || m[i] > 0x7e || (m[i - 1] >= 0x20 && m[i - 1] <= 0x7e)) continue;
		for (k = i; k < ST_MEM_TOP && k - i < 39 && m[k] >= 0x20 && m[k] <= 0x7e; k++) {
			int c = m[k], j;
			if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) continue;
			h = (h ^ (unsigned) (c >= 'A' && c <= 'Z' ? c + 32 : c)) * 16777619u;
			/* the whole string, not a prefix of a longer one */
			if (k + 1 < ST_MEM_TOP && m[k + 1] >= 0x20 && m[k + 1] <= 0x7e) continue;
			j = svc_index (h);
			if (j >= 0 && !svc_label[j][0]) {
				memcpy (svc_label[j], m + i, k + 1 - i);
				svc_label[j][k + 1 - i] = 0;
				left--;
			}
		}
	}
}

/* a service's card on the services page: its label, and where it is */
static void svc_seen (const struct hdtext_run *lab, int tx, int ty)
{
	int i = svc_index (lab->hash), n, k, o = 0;
	char c[40];
	unsigned char sp[40];

	if (i < 0) return;
	svc_tx[i] = tx;
	svc_ty[i] = ty;
	svc_at[i] = ui_frames;
	if (svc_label[i][0]) return;
	n = hdtext_chars (lab, c, sp, sizeof (c));
	for (k = 0; k < n && o < (int) sizeof (svc_label[i]) - 2; k++) {
		if (k && sp[k]) svc_label[i][o++] = ' ';
		svc_label[i][o++] = c[k];
	}
	svc_label[i][o] = 0;
}

/* nothing over the 3D view but the console.  0: not bare; 2: bare and
 * the console says docked (or landed); 1: bare. */
static int bare_view (int landed)
{
	int i, st = 1;

	if (n_btn) return 0;
	/* a page's 3D view; not a comms face down in the console */
	for (i = 0; i < fr_ca_n; i++)
		if (fr_ca[i][1] < CTRL_Y - 2) return 0;
	for (i = 0; i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		if ((r->hash == H_DOCKED || (landed && r->hash == H_LANDED)) &&
		    r->y >= CTRL_Y - 2 && r->y <= CTRL_Y + 4 && r->x0 >= 240)
			st = 2;
		else if (ON_PAGE (r) && r->y < FOOT_Y)
			return 0;
	}
	return st;
}

/* a card on the view: icon, label (run i), chevron; a click goes to the
 * comms icon, and then to the service chain names */
static void dock_card (float x0, float y0, float x1, float y1, int icon, int i, float scale, unsigned int chain)
{
	struct hdtext_run *r = &runs[i];
	struct elem *e;
	int h;

	style (r, HDT_LABEL, C_TEXT);
	r->scale = scale;
	r->align = HDT_LEFT;
	r->ax = x0 + 21;
	r->maxw = x1 - 12 - r->ax;
	centre_y (r, (y0 + y1) / 2);
	e = add (E_CARD, L_UNDER, x0, y0, x1, y1);
	e->icon = icon;
	e->flags = F_CHEVRON;
	e->a = x0 + 10;
	e->b = (y0 + y1) / 2;
	e->c = 72;
	h = add_hot (e->x0, e->y0, e->x1, e->y1, COMMS_X, COMMS_Y, 1);
	e->hot = h;
	if (h >= 0) hot[h].chain = chain;
}

/* For hdback's concourse (hdui.h), from this frame */
static int conc_ok;
static char conc_date[32];

static void concourse_scan (void)
{
	char c[32];
	unsigned char sp[32];
	int i, k, n, o = 0;

	conc_ok = page == P_DOCK ? 2 : page == P_NONE ? bare_view (1) : 0;
	for (i = 0; conc_ok == 2 && i < n_runs; i++) {
		const struct hdtext_run *r = &runs[i];
		/* the console's clock and date: the date after the first space */
		if (r->y < CTRL_Y - 2 || r->y > CTRL_Y + 4 || r->x0 >= 60 || r->letters < 8) continue;
		n = hdtext_chars (r, c, sp, sizeof (c));
		for (k = 1; k < n && !sp[k]; k++)
			;
		for (; k < n && o < (int) sizeof (conc_date) - 2; k++) {
			if (o && sp[k]) conc_date[o++] = ' ';
			conc_date[o++] = c[k];
		}
		break;
	}
	conc_date[o] = 0;
}

int hdui_concourse (const char **date)
{
	*date = conc_date;
	return conc_ok;
}

static void layout_dock (void)
{
	char c[96], name[100];
	unsigned char sp[96];
	int i, k, n, o = 0, ni = -1, li[N_SVC], col = 0;
	float x0 = 4, y0 = 4, y1 = 22, w, x1, wmax;

	/* the speed line's last part: after its colon, the name of what the ship is relative to */
	for (i = n_runs - 1; i >= 0; i--) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->y < FOOT_Y || r->x0 < 160) continue;
		n = hdtext_chars (r, c, sp, sizeof (c));
		for (k = 0; k < n - 1 && c[k] != ':'; k++)
			;
		if (k >= n - 1) return;
		for (k++; k < n && o < (int) sizeof (name) - 2; k++) {
			if (o && sp[k]) name[o++] = ' ';
			name[o++] = c[k];
		}
		name[o] = 0;
		col = r->col;
		break;
	}
	if (!o) return;
	svc_find_labels ();
	ni = hdtext_add (name, 0, 2, col);
	for (k = 0; k < N_SVC; k++)
		li[k] = svc_label[k][0] ? hdtext_add (svc_label[k], 0, 30 + 23 * k, col) : -1;
	n_runs = hdtext_runs (&runs);
	if (ni < 0) return;

	/* one width for the column: the widest label's */
	runs[ni].role = HDT_LABEL;
	runs[ni].scale = 1.3f;
	wmax = hdtext_measure (&runs[ni]);
	for (k = 0; k < N_SVC; k++)
		if (li[k] >= 0) {
			runs[li[k]].role = HDT_LABEL;
			runs[li[k]].upper = 1;
			runs[li[k]].scale = 1;
			w = hdtext_measure (&runs[li[k]]);
			if (w > wmax) wmax = w;
		}
	x1 = x0 + 21 + wmax + 14;
	if (x1 < x0 + 92) x1 = x0 + 92;
	if (x1 > x0 + 150) x1 = x0 + 150;

	dock_card (x0, y0, x1, y1, I_STATION, ni, 1.3f, 0);
	for (k = 0, y0 = 27; k < N_SVC; k++) {
		if (li[k] < 0) continue;
		dock_card (x0, y0, x1, y0 + 19, card_icon (svc_hash[k]), li[k], 1, svc_hash[k]);
		runs[li[k]].upper = 1;
		y0 += 22;
	}
}

/*
 * A service's card on the view: the click (on the comms icon) opens the
 * services page; once that page has stood for a frame or two with the
 * service's card on it, a click on that card is made for the player, a
 * press and a release.  Only after the player's own click, never in an
 * oracle replay (hdui_mouse is not called there), and given up after a
 * few seconds or on any click of the player's.
 */
static unsigned int svc_chain;		/* the service to open, 0 none */
static unsigned int chain_start;	/* ui_frames when it was asked for */

static int svc_chain_step (short *ax, short *ay, int buttons)
{
	static int phase, polls;
	static unsigned int phase_at;
	int i = svc_index (svc_chain);

	if (!svc_chain || i < 0) return buttons;
	if ((buttons & 2) && !phase) {
		/* the player is clicking: theirs */
		svc_chain = 0;
		return buttons;
	}
	if (!phase) {
		if (ui_frames - chain_start > 240) {
			svc_chain = 0;
			return buttons;
		}
		if (page != P_STATION || stable < 2 || svc_at[i] != ui_frames) return buttons;
		phase = 1;
		phase_at = ui_frames;
		polls = 0;
	}
	*ax = svc_tx[i];
	*ay = svc_ty[i];
	polls++;
	if (phase == 1) {
		if (ui_frames - phase_at >= 3 && polls >= 4) {
			phase = 2;
			phase_at = ui_frames;
			polls = 0;
		}
		return buttons | 2;
	}
	if (ui_frames - phase_at >= 3 && polls >= 4) {
		phase = 0;
		svc_chain = 0;
	}
	return buttons & ~2;
}

/* The start menu: each choice a card */
static void layout_start (void)
{
	int i, j;
	float x0 = ST_W, x1 = 0;
	struct hdtext_run *lab[16], *ic[16];
	int n = 0;

	for (i = 0; i < n_runs && n < 16; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || !(r->kind & HDT_K_ICON) || r->chr0 != 0x61) continue;
		ic[n] = r;
		lab[n] = NULL;
		for (j = 0; j < n_runs; j++)
			if (abs (runs[j].y - r->y) <= 1 && runs[j].x0 >= r->x1 - 1 && !(runs[j].kind & HDT_K_ICON)) {
				lab[n] = &runs[j];
				break;
			}
		if (r->x0 < x0) x0 = r->x0;
		if (lab[n] && lab[n]->ink1 > x1) x1 = lab[n]->ink1;
		n++;
	}
	if (x1 < x0 + 40) x1 = x0 + 40;
	for (i = 0; i < n; i++) {
		struct elem *e = add (E_CARD, L_UNDER, x0 - 8, ic[i]->y - 4, x1 + 10, ic[i]->y + 12);
		e->flags = F_RING;
		e->a = ic[i]->x0 + 4.5f;
		e->b = ic[i]->y + 4;
		e->c = 30;
		e->hot = add_hot (e->x0, e->y0, e->x1, e->y1, ic[i]->x0 + 4, ic[i]->y + 4, 1);
		ic[i]->hide = 1;
		if (lab[i]) {
			style (lab[i], HDT_LABEL, C_TEXT);
			lab[i]->align = HDT_LEFT;
			lab[i]->dx = 3;
		}
	}
	for (i = 0; i < n_runs; i++)
		if (runs[i].hash == H_START_TITLE) break;
	if (i == n_runs) return;	/* a menu in flight (comms): the HUD under it stays */
	for (i = 0; i < n_runs; i++) {
		struct hdtext_run *r = &runs[i];
		if (!ON_PAGE (r) || r->role) continue;
		if (r->hash == H_START_TITLE) style (r, HDT_TITLE, C_TEXT);
		else if (r->y >= 140) {
			style (r, HDT_DESC, C_TEXT2);
			r->align = HDT_CENTRE;
		}
	}
}

/* what the layout is, without the states that change while it is shown */
static unsigned long long layout_key (void)
{
	unsigned long long h = FNV0;
	int i;

	/* not the page colour: a fade changes it every frame, and the
	 * places to click must not go dead while the layout stands still
	 * (the backdrop checks the colour itself) */
	h = mix_hash (h, &page, sizeof (page));
	for (i = 0; i < n_el; i++) {
		struct elem e = el[i];
		e.flags &= ~F_ON;
		e.state = 0;
		e.pressed = 0;
		h = mix_hash (h, &e, sizeof (e));
	}
	return mix_hash (h, hot, n_hot * sizeof (hot[0]));
}

/* ---------------------------------------------------------------- */
/* The entry points */

void hdui_frame (const unsigned char *raster, unsigned char *out,
		 const unsigned int *pal, const unsigned int *ctrl_pal)
{
	unsigned long long key;
	int i;

	memcpy (fr_ca, ca, sizeof (ca));
	fr_ca_n = ca_n;
	hdui_face_top = 0;
	ca_n = 0;
	n_el = n_hot = 0;
	page = P_NONE;
	if (!layer_on () || pixel_text) {
		conc_ok = 0;
		n_live = stable = n_runs = n_btn = 0;
		page_key = 0;
		page_log (out, pal);
		dump (raster, out, pal, ctrl_pal);
		return;
	}
	ui_frames++;
	n_runs = hdtext_runs (&runs);
	find_buttons (raster);
	page = classify ();
	if (page == P_NONE && bare_view (0) == 2) page = P_DOCK;
	concourse_scan ();	/* before layout_dock adds its runs */
	switch (page) {
	case P_DOCK: layout_dock (); if (!n_el) page = P_NONE; break;
	case P_STATION: layout_station (raster, out); break;
	case P_F3: layout_f3 (out); break;
	case P_PREFS: layout_prefs (pal); break;
	case P_START: layout_start (); break;
	case P_FILE: layout_file (raster, out, pal); break;
	case P_INFO: layout_info (out); break;
	}
	for (i = 0; i < n_btn; i++)
		if (btn[i].used) take_out (raster, out, &btn[i]);
	if (backdrop_page ()) text_bg_out (raster, out);
	page_bg = pal[BG];
	key = page == P_NONE ? 0 : layout_key ();
	if (key != page_key) {
		page_key = key;
		stable = 0;
		n_live = 0;
	} else if (stable < 1000000)
		stable++;
	if (stable >= 1 && key) {
		memcpy (live, hot, n_hot * sizeof (hot[0]));
		n_live = n_hot;
	}
	page_log (out, pal);
	dump (raster, out, pal, ctrl_pal);
}

int hdui_mouse (short *ax, short *ay, int buttons)
{
	static int latch = -1, was_down;
	static short tx, ty;
	static unsigned int chain;
	int down = (buttons & 2) != 0, x = *ax, y = *ay, i;

	ptr_x = x < 0 ? 0 : x >= ST_W ? ST_W - 1 : x;
	ptr_y = y < 0 ? 0 : y >= ST_H ? ST_H - 1 : y;
	ptr_buttons = buttons;
	if (!layer_on ()) {
		latch = -1;
		was_down = down;
		svc_chain = 0;
		return buttons;
	}
	if (down && !was_down) {
		latch = -1;
		if (y < CTRL_Y)
			for (i = 0; i < n_live; i++)
				if (live[i].remap && x >= live[i].x0 && x < live[i].x1 &&
				    y >= live[i].y0 && y < live[i].y1) {
					latch = i;
					tx = live[i].tx;
					ty = live[i].ty;
					chain = live[i].chain;
					break;
				}
	}
	was_down = down;
	if (latch >= 0) {
		*ax = tx;
		*ay = ty;
		if (!down) {
			latch = -1;
			/* released: the service it names, once the page is up */
			if (chain) {
				svc_chain = chain;
				chain_start = ui_frames;
			}
			chain = 0;
		}
		return buttons;
	}
	return svc_chain_step (ax, ay, buttons);
}

static int backdrop_page (void)
{
	return page == P_STATION || page == P_F3 || page == P_PREFS || page == P_FILE;
}

int hdui_owns_page (void)
{
	return layer_on () && !pixel_text && page_key && backdrop_page ();
}

int hdui_still_page (void)
{
	return layer_on () && page == P_FILE;
}

void hdui_backdrop (const unsigned int *pal, int bgcol)
{
	back_drawn = 0;
	if (!hdui_owns_page ()) return;
	if (bgcol != BG || pal[BG] != page_bg) return;
	begin_2d ();
	draw_base (0);
	draw_elems (L_BACK);
	end_2d ();
	back_drawn = page_key;
}

void hdui_draw_under (const unsigned int *pal, const unsigned int *ctrl_pal)
{
	(void) pal;
	(void) ctrl_pal;
	frame_no++;
	if (!layer_on () || page == P_NONE || !n_el) return;
	begin_2d ();
	if (backdrop_page () && back_drawn != page_key) {
		/* a new page: its backdrop was not drawn under this frame */
		draw_base (1);
		skip_view = 1;
		draw_elems (L_BACK);
		skip_view = 0;
	}
	draw_elems (L_UNDER);
	end_2d ();
}

void hdui_draw_over (const unsigned int *pal, const unsigned int *ctrl_pal)
{
	int i;

	(void) pal;
	(void) ctrl_pal;
	if (!layer_on () || page == P_NONE) return;
	for (i = 0; i < n_el; i++)
		if (el[i].layer == L_OVER) break;
	if (i == n_el) return;
	begin_2d ();
	draw_elems (L_OVER);
	end_2d ();
}

void hdui_forget (void)
{
#ifdef __APPLE__
	forget_sprites ();
	if (photo_tex) glDeleteTextures (1, &photo_tex);
	photo_tex = 0;
	photo_tried = 0;
#endif
}
