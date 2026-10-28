/*
 * HD stills for the GL renderer (see hdstill.h).
 *
 * The game draws its fixed pictures (the title logo, the control panel
 * along the bottom and its icons, the keys beside the lines of its lists)
 * with Call_BlitBmp.  hdstill_blit notes
 * each one: where it went, its pixels and a hash of its planes, which
 * names it.  Then, as for hdtext, nothing recorded is trusted: every frame
 * hdstill_frame looks for each recorded bitmap in the screen being shown,
 * newest first, and only one that is still there is redrawn, and only in
 * the palette its art was drawn for (or that palette faded towards black).
 * Its pixels are taken out of the copy uploaded to GL and new art, drawn
 * in code with CoreGraphics at the window's own pixel size, goes in their
 * place.  Pixels something else has drawn over it (the scanner's contacts,
 * the gauges) stay as they are, on top of the new art.
 *
 * The control panel is one scene, drawn in ST coordinates: each of its
 * bitmaps is that scene seen through the bitmap's own rectangle, in the
 * state the bitmap shows (which time key is lit, which icons are in the
 * keys, scanner or empty bay, ...).  So a small bitmap blitted over a big
 * one (a new time key lit, a new icon) joins it without a seam.
 *
 * Nothing here writes ST memory, so the oracle sees what it always saw;
 * nothing changes where anything is, so every click lands where it did.
 *
 * FE2_HD_DEBUG=1 logs, once each, every bitmap seen on the screen with its
 * hash, size and place and whether it was replaced, and the running
 * coverage (bitmaps replaced of those seen).  FE2_HD_DEBUG=2 adds the
 * colours each is shown in.  FE2_HD_DUMP=dir writes the ones not replaced
 * as PPMs, for working out what they are (pictures of the game: never put
 * them in the repository).
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
#include "hdstill.h"

/* screen.h's renderers (it needs SDL): only what this file uses */
enum RENDERERS { R_OLD, R_GLWIRE, R_GL, R_MAX };
extern enum RENDERERS use_renderer;
extern int classic_ui;

#define ST_W		320
#define ST_H		200
#define CTRL_Y		168	/* the control panel's rows have their own palette */
#define MAX_REC		256	/* bitmaps remembered at once */
#define MISS_LIMIT	3000	/* frames a bitmap may be off the screen */
#define MAX_TEX		96	/* art textures kept */
#define MAX_BODY	32	/* bodies on a star's information page */
#define DEFER_PCT	25	/* another layer took out more than this: leave it be
				 * (text over a bitmap takes out a few pixels) */

typedef unsigned long long u64;

/* A bitmap the game blitted */
struct rec {
	short x, y, w, h;	/* ST pixels; w is a whole number of 16 */
	u64 hash;
	unsigned char *pix;	/* w x h palette indices, top row first */
	unsigned int used;	/* the palette indices it uses, a bit each */
	unsigned char bg;	/* its commonest colour */
	short art;		/* its new art in arts[], or -1 */
	unsigned seq;		/* when it was last blitted */
	int misses;
	unsigned char logged;	/* FE2_HD_DEBUG has reported it: 1 kept, 2 replaced, 4 deferred */
};
static struct rec recs[MAX_REC];
static int n_recs;
static unsigned seq;

/* what the last hdstill_frame found to draw: an art's texture at a place,
 * faded */
static struct shown {
	short x, y, w, h, tex;
	short vh;		/* the rows of it drawn: a key on the main screen's
				 * last line stops where the panel starts */
	float k;
	unsigned bg;		/* main screen: the colour behind the art (RGBA), or 0 */
	unsigned char body;	/* a body (struct body): nothing of the screen goes over it */
} shown[MAX_REC + MAX_BODY];
static int n_shown;

/* A star or planet the game blitted on a star's information page, drawn
 * again as a sphere in its place.  Framebuffer pixels from the screen's
 * top left. */
static struct body {
	u64 key;		/* its bitmap and the colours it is shown in */
	short x, y, w, h;	/* the new art's rectangle, ST pixels */
	float cx, cy, r;	/* its disc */
	unsigned lit, base;	/* its colours (0xRRGGBB) */
	int star;
	float ring, ring_a;	/* a ring's half length and its angle, or 0 */
	unsigned ring_c;
} bodies[MAX_BODY];
static const struct body *body_now;	/* the one build () draws */

static int debug = -1;

/* the control panel as the last hdstill_frame found it (hdstill_panel) */
static struct hdstill_panel panel_now;

/* ------------------------------------------------------------------ */
/* what each bitmap is */

/* the icons in the function keys (G_ in hdstill.h) */
#define GX(g)		((g) | G_OFF)

enum { K_PANEL, K_LOGO, K_MISSILE, K_SELECT, K_MORE, K_OPT_ON, K_OPT_OFF };
#define ON_MAIN(k)	((k) >= K_SELECT)	/* in the main screen's rows, on its background */
#define IS_OPT(k)	((k) >= K_OPT_ON)	/* an option's key, its own colours */
/* F_MAPKEYS, F_SCANNER, F_BUTTONS: hdstill.h */
#define F_INFO		8	/* a star's information page: its bodies drawn too */

struct art {
	u64 hash;
	short w, h;		/* ST pixels */
	unsigned char kind, flags;
	signed char v;		/* K_PANEL: the time key lit, 0 paused, 1..5
				 * 1x..10000x, -1 none (time stands still in
				 * the game's menu); K_MISSILE: 1 points right */
	unsigned char g[5];	/* icons of the keys from the one at its left */
	const char *name;
};

static const struct art arts[] = {
	/* the title */
	{ 0x3f9a2076d5aaf20cULL, 192, 30, K_LOGO, 0, 0, { 0 }, "logo" },
	/* the control panel: left, middle (empty bay, scanner), right */
	{ 0xd462418cc5fb9cefULL, 64, 32, K_PANEL, 0, 1, { G_FRONT, G_GALMAP, G_SHIP, G_PHONE }, "panel left" },
	{ 0x8c5cbda1ba536b26ULL, 192, 32, K_PANEL, 0, 1, { 0 }, "panel bay" },
	{ 0xd0914d8efaba6503ULL, 192, 32, K_PANEL, F_SCANNER, 1, { 0 }, "panel scanner" },
	{ 0x3c121296a6949971ULL, 64, 32, K_PANEL, 0, 1, { G_EMPTY, G_EMPTY, G_EMPTY, G_EMPTY }, "panel right" },
	{ 0x7613bb2798831967ULL, 64, 21, K_PANEL, F_MAPKEYS, 1, { G_TARGET, G_CROSS, G_WRENCH, G_EMPTY }, "map keys" },
	{ 0xb06c15212855f36fULL, 160, 31, K_PANEL, F_SCANNER, 1, { 0 }, "scanner" },
	{ 0x4318a0ae151e124bULL, 32, 9, K_PANEL, 0, 1, { 0 }, "gauge slats" },
	{ 0x4c422cea87bde062ULL, 32, 7, K_PANEL, 0, 1, { 0 }, "clock display" },
	{ 0x54027ee0fd28d162ULL, 80, 9, K_PANEL, 0, 1, { 0 }, "date display" },
	/* the time keys, one lit */
	{ 0xb36c8c03d129537eULL, 64, 7, K_PANEL, 0, 0, { 0 }, "time paused" },
	{ 0x37f2a14a3b9cf84aULL, 64, 7, K_PANEL, 0, 1, { 0 }, "time 1x" },
	{ 0x78d5a56f7a26d984ULL, 64, 7, K_PANEL, 0, 2, { 0 }, "time 10x" },
	{ 0x357098c98500a5fcULL, 64, 7, K_PANEL, 0, 3, { 0 }, "time 100x" },
	{ 0xf5c133448a7deb62ULL, 64, 7, K_PANEL, 0, 4, { 0 }, "time 1000x" },
	{ 0x923aaa5390ce6e86ULL, 64, 7, K_PANEL, 0, 5, { 0 }, "time 10000x" },
	{ 0x4aa42c3944061a02ULL, 64, 7, K_PANEL, 0, -1, { 0 }, "time none (game menu)" },
	/* the function keys' icons */
	{ 0x2aebac07e74854c4ULL, 16, 11, K_PANEL, 0, 1, { G_FRONT }, "icon front view" },
	{ 0xe6c6c1396bf3e64aULL, 16, 11, K_PANEL, 0, 1, { G_REAR }, "icon rear view" },
	{ 0x9d760acacd6fac21ULL, 16, 11, K_PANEL, 0, 1, { G_EXT }, "icon external view" },
	{ 0x79b76cd975ebff1fULL, 16, 11, K_PANEL, 0, 1, { G_GALMAP }, "icon galaxy map" },
	{ 0xd3ec95863ae8f35bULL, 16, 11, K_PANEL, 0, 1, { G_SYSMAP }, "icon system map" },
	{ 0x230ef774688b02e6ULL, 16, 11, K_PANEL, 0, 1, { G_SHIP }, "icon ship" },
	{ 0x54664eec90e7f24aULL, 16, 11, K_PANEL, 0, 1, { G_CARGO }, "icon cargo" },
	{ 0xf41a4b6cbf549286ULL, 16, 11, K_PANEL, 0, 1, { G_CMDR }, "icon commander" },
	{ 0x2bdfae244adcdcbcULL, 16, 11, K_PANEL, 0, 1, { G_ARM }, "icon missile" },
	{ 0x7b3ff1e79e9c3765ULL, 16, 11, K_PANEL, 0, 1, { G_BEAM }, "icon beam" },
	{ 0xd3ba8a0e288b9151ULL, 16, 11, K_PANEL, 0, 1, { G_EMPTY }, "icon empty" },
	{ 0x23a4b63e9aa289e4ULL, 16, 11, K_PANEL, 0, 1, { G_BURST }, "icon burst" },
	{ 0x96349724541eb108ULL, 16, 11, K_PANEL, 0, 1, { G_ROUTE }, "icon route" },
	{ 0x38afdfb87c76cf68ULL, 16, 11, K_PANEL, 0, 1, { G_TAKEOFF }, "icon take off (landed)" },
	{ 0x3fa31e1416775b84ULL, 16, 11, K_PANEL, 0, 1, { GX (G_LIST) }, "icon list crossed (landed)" },
	/* a map option turned off: its key without the cross, over the strip */
	{ 0x27098800ad01b1c9ULL, 16, 11, K_PANEL, 0, 1, { G_PERSON }, "icon person (option off)" },
	{ 0x1aae938aed8b65ccULL, 16, 11, K_PANEL, 0, 1, { G_GALMAP }, "icon galaxy map (option off)" },
	{ 0x2f8394d95032ca79ULL, 16, 11, K_PANEL, 0, 1, { G_ALPHA }, "icon alpha (option off)" },
	{ 0x204ff007744c5988ULL, 16, 11, K_PANEL, 0, 1, { G_WRENCH }, "icon wrench (option off)" },
	{ 0x02fb7f3f0820e6baULL, 16, 11, K_PANEL, 0, 1, { G_DOC }, "icon doc (option off)" },
	{ 0x980ec8842f7b354bULL, 64, 11, K_PANEL, 0, 1, { G_TARGET, G_CROSS, G_WRENCH, G_HYPER }, "icons 7-10 galaxy map" },
	{ 0xaa1cc440b258bd81ULL, 64, 11, K_PANEL, 0, 1, { G_TARGET, G_CROSS, G_EMPTY, G_SECTOR }, "icons 7-10 galaxy map zoomed out" },
	{ 0x86a58df0cc99b241ULL, 64, 11, K_PANEL, 0, 1, { GX (G_PERSON), GX (G_GALMAP), GX (G_ROUTE), GX (G_ALPHA) }, "icons 7-10 galaxy map info" },
	{ 0x9b5c4f4ac5879452ULL, 64, 11, K_PANEL, 0, 1, { G_TARGET, G_CROSS, GX (G_WRENCH), GX (G_DOC) }, "icons 7-10 system map info" },
	{ 0xd81a763c56d5b42eULL, 64, 11, K_PANEL, 0, 1, { G_PERSON, G_EMPTY, G_LAUNCH, G_LIST }, "icons 7-10 docked" },
	/* a star's information page: keys 6-10 and the two rows above them,
	 * which reach into the second display (the title's last line) */
	{ 0xa0feef8ca46dcdc7ULL, 80, 13, K_PANEL, F_INFO, 1, { G_SECTOR, G_TRADE, G_PERSON, G_EMPTY, G_ORRERY }, "info keys 6-10" },
	{ 0x9b7f1f4805f6d7e0ULL, 64, 11, K_PANEL, 0, 1, { G_LOAD, G_SAVE, G_DELETE, G_EXIT }, "icons 7-10 game menu" },
	/* keys in the bay */
	{ 0xcc2e57fc156a3812ULL, 16, 13, K_PANEL, 0, 1, { G_ORRERY }, "bay key orrery" },
	{ 0xf987ebdcb79f4c99ULL, 128, 13, K_PANEL, F_BUTTONS, 1, { 0 }, "bay key row" },
	/* missiles on the pylons */
	{ 0xaaf820507366f84fULL, 16, 13, K_MISSILE, 0, 0, { 0 }, "missile" },
	{ 0xb26e9633233689eaULL, 16, 13, K_MISSILE, 0, 1, { 0 }, "missile mirrored" },
	/* the main screen's lists (services, market, bulletin board, ...):
	 * the key beside each line, and the one that pages on */
	{ 0x0918566adf8551c1ULL, 16, 11, K_SELECT, 0, 0, { 0 }, "list key" },
	{ 0x611c48f3a6e83567ULL, 16, 10, K_MORE, 0, 0, { 0 }, "list more" },
	/* the game menu's preferences: the key beside each option, lit when
	 * it is on; the dialogs' OK key is the unlit one */
	{ 0xb621cd768cb602b1ULL, 16, 11, K_OPT_ON, 0, 0, { 0 }, "option on" },
	{ 0x6f22978287ae5e79ULL, 16, 11, K_OPT_OFF, 0, 0, { 0 }, "option off" },
};
#ifdef __APPLE__
static const int n_arts = sizeof (arts) / sizeof (arts[0]);
#else
static const int n_arts = 0;	/* the art is drawn with CoreGraphics */
#endif

/* The palettes the art was drawn for, as the game shows them when no
 * fade or flash is on (0xAABBGGRR).  The control panel's has two forms:
 * its entries 8-11 are the scanner's reds, or skin tones while a face is
 * in the bay. */
#define RGB(r, g, b) (0xff000000u | (unsigned) (b) << 16 | (unsigned) (g) << 8 | (unsigned) (r))
static const unsigned int pal_scan[16] = {
	RGB (0, 0, 0), RGB (0xf0, 0xf0, 0xf0), RGB (0xf0, 0, 0), RGB (0xf0, 0x80, 0),
	RGB (0, 0xf0, 0), RGB (0xd0, 0xd0, 0), RGB (0, 0, 0xf0), RGB (0xf0, 0, 0xf0),
	RGB (0x40, 0, 0), RGB (0x60, 0, 0), RGB (0x80, 0, 0), RGB (0xa0, 0, 0),
	RGB (0x40, 0x40, 0x40), RGB (0x60, 0x60, 0x60), RGB (0x80, 0x80, 0x80), RGB (0xa0, 0xa0, 0xa0),
};
static const unsigned int pal_skin[16] = {
	RGB (0, 0, 0), RGB (0xf0, 0xf0, 0xf0), RGB (0xf0, 0, 0), RGB (0xf0, 0x80, 0),
	RGB (0, 0xf0, 0), RGB (0xd0, 0xd0, 0), RGB (0, 0, 0xf0), RGB (0xf0, 0, 0xf0),
	RGB (0x40, 0x40, 0x20), RGB (0x80, 0x60, 0x40), RGB (0xa0, 0x80, 0x60), RGB (0xd0, 0xa0, 0x80),
	RGB (0x40, 0x40, 0x40), RGB (0x60, 0x60, 0x60), RGB (0x80, 0x80, 0x80), RGB (0xa0, 0xa0, 0xa0),
};
/* the lists' screens: only the colours their keys use count */
static const unsigned int pal_list[16] = {
	[0] = RGB (0, 0, 0), [14] = RGB (0x60, 0x40, 0xa0), [15] = RGB (0xa0, 0xa0, 0xa0),
};
/* the preferences screen and the dialogs: the colours of their keys */
static const unsigned int pal_opt[16] = {
	[0] = RGB (0, 0, 0), [1] = RGB (0xd0, 0xd0, 0xd0), [2] = RGB (0xf0, 0, 0),
	[10] = RGB (0x80, 0, 0), [11] = RGB (0xa0, 0, 0), [12] = RGB (0x20, 0x20, 0x20),
	[13] = RGB (0x40, 0x40, 0x40), [15] = RGB (0xa0, 0xa0, 0xa0),
};
static const unsigned int pal_logo[16] = {
	RGB (0, 0, 0), RGB (0xf0, 0xf0, 0xf0), RGB (0xf0, 0xf0, 0xf0), RGB (0xd0, 0xd0, 0xd0),
	RGB (0xd0, 0xd0, 0xd0), RGB (0xa0, 0xa0, 0xa0), RGB (0, 0, 0), RGB (0x20, 0x20, 0x20),
	RGB (0x20, 0x20, 0x20), RGB (0x40, 0x40, 0x40), RGB (0x40, 0x40, 0x40), RGB (0x60, 0x60, 0x60),
	RGB (0x60, 0x60, 0x60), RGB (0x80, 0x80, 0x80), RGB (0x80, 0x80, 0x80), RGB (0xa0, 0xa0, 0xa0),
};

/* each art's texture at a place, made for the current framebuffer size */
static struct tex {
	GLuint id;
	short art, x, y;	/* art -1: a body */
	u64 key;		/* a body's */
	float s1, t1;		/* the art's far corner in it */
	unsigned used;		/* the frame it was last shown in */
} texs[MAX_TEX];
static float built_sx, built_sy;
static unsigned frame_no;

/* which function key is at (x, y): 1..4 left, 5..6 in the bay, 7..10 right */
static int slot_at (int x, int y)
{
	if (y >= 186 && x >= 224 && x < 256) return 5 + (x - 224) / 16;
	if (x < 64) return 1 + x / 16;
	if (x >= 256) return 7 + (x - 256) / 16;
	return 0;
}

/* ------------------------------------------------------------------ */
/* the art, drawn in code */

#ifdef __APPLE__

/* the scene's state for one art: icons in keys 1..10 (5 and 6 are in the
 * bay and only there when blitted), the time key lit, F_ flags */
static struct look {
	int time, flags;
	unsigned char slot[11];
} L;

/* drawing: the context, the art's top left in ST pixels, device pixels
 * an ST pixel, the art's size in device pixels and U, a device pixel of a
 * 1600x1200 window (what the design's sizes are in) */
static CGContextRef C;
static CGColorSpaceRef CS;
static float OX, OY, SX, SY, AW, AH, U;
#define X(x) (((x) - OX) * SX)
#define Y(y) (((y) - OY) * SY)

/* colours are 0xRRGGBB here */
static void fillc (unsigned c, float a)
{
	CGContextSetRGBFillColor (C, (c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, a);
}

static void strokec (unsigned c, float a)
{
	CGContextSetRGBStrokeColor (C, (c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, a);
}

/* a shade (or with no offset a glow) for what is drawn next; dy > 0 is
 * down.  Its offset is in the bitmap's own space, which is y up. */
static void shade (float dx, float dy, float blur, unsigned c, float a)
{
	CGFloat v[4] = { (c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, a };
	CGColorRef k = CGColorCreate (CS, v);
	CGContextSetShadowWithColor (C, CGSizeMake (dx, -dy), blur, k);
	CGColorRelease (k);
}

static void no_shade (void)
{
	CGContextSetShadowWithColor (C, CGSizeZero, 0, NULL);
}

static CGGradientRef gradient (int n, const unsigned *c, const float *a, const float *loc)
{
	CGFloat comp[4 * 8], l[8];
	int i;
	if (n > 8) n = 8;
	for (i = 0; i < n; i++) {
		comp[4 * i] = (c[i] >> 16 & 255) / 255.f;
		comp[4 * i + 1] = (c[i] >> 8 & 255) / 255.f;
		comp[4 * i + 2] = (c[i] & 255) / 255.f;
		comp[4 * i + 3] = a[i];
		l[i] = loc ? loc[i] : n > 1 ? i / (n - 1.f) : 0;
	}
	return CGGradientCreateWithColorComponents (CS, comp, l, n);
}

/* over the clip, a linear gradient from (x0, y0) to (x1, y1) */
static void lgrad (float x0, float y0, float x1, float y1, int n,
		   const unsigned *c, const float *a, const float *loc)
{
	CGGradientRef g = gradient (n, c, a, loc);
	CGContextDrawLinearGradient (C, g, CGPointMake (x0, y0), CGPointMake (x1, y1),
				     kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
	CGGradientRelease (g);
}

/* over the clip, a radial gradient about (x, y) out to r */
static void rgrad (float x, float y, float r, int n, const unsigned *c, const float *a, const float *loc)
{
	CGGradientRef g = gradient (n, c, a, loc);
	CGContextDrawRadialGradient (C, g, CGPointMake (x, y), 0, CGPointMake (x, y), r,
				     kCGGradientDrawsAfterEndLocation);
	CGGradientRelease (g);
}

#define CS2(a, b) (const unsigned[]) { a, b }
#define CS3(a, b, c) (const unsigned[]) { a, b, c }
#define AS2(a, b) (const float[]) { a, b }
#define AS3(a, b, c) (const float[]) { a, b, c }

/* paths, in device pixels */
static CGMutablePathRef rrect (float x0, float y0, float x1, float y1, float r)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	float w = x1 - x0, h = y1 - y0;
	if (w <= 0 || h <= 0) return p;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	if (r < 0) r = 0;
	CGPathAddRoundedRect (p, NULL, CGRectMake (x0, y0, w, h), r, r);
	return p;
}

/* a polygon of n points (x, y, ...) with its corners rounded to r */
static CGMutablePathRef poly (const float *pt, int n, float r)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	int i;
	CGPathMoveToPoint (p, NULL, (pt[0] + pt[2]) / 2, (pt[1] + pt[3]) / 2);
	for (i = 1; i <= n; i++) {
		int a = i % n, b = (i + 1) % n;
		CGPathAddArcToPoint (p, NULL, pt[2 * a], pt[2 * a + 1], pt[2 * b], pt[2 * b + 1], r);
	}
	CGPathCloseSubpath (p);
	return p;
}

static void fill_path (CGPathRef p, unsigned c, float a)
{
	fillc (c, a);
	CGContextAddPath (C, p);
	CGContextFillPath (C);
}

/* fill p with a gradient top (y0) to bottom (y1) */
static void fill_grad (CGPathRef p, float y0, float y1, int n, const unsigned *c, const float *a, const float *loc)
{
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, y0, 0, y1, n, c, a, loc);
	CGContextRestoreGState (C);
}

static void stroke_path (CGPathRef p, float w, unsigned c, float a)
{
	strokec (c, a);
	CGContextSetLineWidth (C, w);
	CGContextAddPath (C, p);
	CGContextStrokePath (C);
}

/* a rectangle round p and the art */
static CGRect around (CGPathRef p)
{
	return CGRectInset (CGRectUnion (CGPathGetBoundingBox (p), CGRectMake (0, 0, AW, AH)), -60 * U, -60 * U);
}

/* a line w wide just inside p's edge */
static void stroke_inside (CGPathRef p, float w, unsigned c, float a)
{
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	stroke_path (p, 2 * w, c, a);
	CGContextRestoreGState (C);
}

/* a line w wide just outside p's edge */
static void stroke_outside (CGPathRef p, float w, unsigned c, float a)
{
	CGContextSaveGState (C);
	CGContextAddRect (C, around (p));
	CGContextAddPath (C, p);
	CGContextEOClip (C);
	stroke_path (p, 2 * w, c, a);
	CGContextRestoreGState (C);
}

/* a shadow cast into p from its edge, dy down (< 0: up), or a glow when
 * the colour is light */
static void inner_shadow (CGPathRef p, float blur, float dy, unsigned c, float a)
{
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	shade (0, dy, blur, c, a);
	CGContextAddRect (C, around (p));
	CGContextAddPath (C, p);
	fillc (c, 1);
	CGContextEOFillPath (C);
	CGContextRestoreGState (C);
}

/* a glow round p, outside it */
static void outer_glow (CGPathRef p, float blur, unsigned c, float a)
{
	CGContextSaveGState (C);
	CGContextAddRect (C, around (p));
	CGContextAddPath (C, p);
	CGContextEOClip (C);
	shade (0, 0, blur, c, a);
	fill_path (p, c, 1);
	CGContextRestoreGState (C);
}

static unsigned ihash (unsigned a)
{
	a ^= a >> 16;
	a *= 0x7feb352dU;
	a ^= a >> 15;
	a *= 0x846ca68bU;
	a ^= a >> 16;
	return a;
}

/* type */
static CTFontRef font (const char *name)
{
	static struct { const char *name; CTFontRef f; } cache[8];
	static int n;
	CFStringRef s;
	CTFontRef f;
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp (cache[i].name, name)) return cache[i].f;
	s = CFStringCreateWithCString (NULL, name, kCFStringEncodingUTF8);
	f = CTFontCreateWithName (s, 100, NULL);
	CFRelease (s);
	if (n < 8) {
		cache[n].name = name;
		cache[n++].f = f;
	}
	return f;
}

/* the outlines of s at size, its first glyph's origin at (x, base), each
 * glyph track further apart; *width its width */
static CGMutablePathRef text_path (CTFontRef f, const char *s, float size, float track,
				   float x, float base, float *width)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	UniChar ch[32];
	CGGlyph gl[32];
	CGSize adv[32];
	int n = strlen (s), i;
	float sc = size / 100, cx = x;

	if (n > 32) n = 32;
	for (i = 0; i < n; i++) ch[i] = (unsigned char) s[i];
	CTFontGetGlyphsForCharacters (f, ch, gl, n);
	CTFontGetAdvancesForGlyphs (f, kCTFontOrientationDefault, gl, adv, n);
	for (i = 0; i < n; i++) {
		CGAffineTransform t = CGAffineTransformMake (sc, 0, 0, -sc, cx, base);
		CGPathRef g = CTFontCreatePathForGlyph (f, gl[i], &t);
		if (g) {
			CGPathAddPath (p, NULL, g);
			CGPathRelease (g);
		}
		cx += adv[i].width * sc + (i < n - 1 ? track : 0);
	}
	if (width) *width = cx - x;
	return p;
}

/* does the ST rectangle reach into the art (give or take a glow)? */
static int in_art (float x0, float y0, float x1, float y1)
{
	return x1 > OX - 3 && x0 < OX + AW / SX + 3 && y1 > OY - 3 && y0 < OY + AH / SY + 3;
}

/* ---- icons ---- */

/* glyph space: (-1, -1) to (1, 1) about the icon's middle, y down */
static float LW;	/* a line, in glyph units */

static void glyph_begin (float cx, float cy, float s)
{
	CGContextSaveGState (C);
	CGContextTranslateCTM (C, cx, cy);
	CGContextScaleCTM (C, s / 2, s / 2);
	LW = 1.6f * U / (s / 2);
	CGContextSetLineCap (C, kCGLineCapRound);
	CGContextSetLineJoin (C, kCGLineJoinRound);
}

static void glyph_end (void)
{
	CGContextRestoreGState (C);
}

static void gcirc (float x, float y, float r)
{
	CGContextAddEllipseInRect (C, CGRectMake (x - r, y - r, 2 * r, 2 * r));
}

static void gseg (float x0, float y0, float x1, float y1)
{
	CGContextMoveToPoint (C, x0, y0);
	CGContextAddLineToPoint (C, x1, y1);
}

static void gstroke (unsigned c, float a, float w)
{
	strokec (c, a);
	CGContextSetLineWidth (C, w * LW);
	CGContextStrokePath (C);
}

static void gfill (unsigned c, float a)
{
	fillc (c, a);
	CGContextFillPath (C);
}

static void glow (unsigned c, float a, float blur)
{
	shade (0, 0, blur * U, c, a);
}

/* a ball lit from the top left */
static void ball (float x, float y, float r, unsigned light, unsigned dark)
{
	CGContextSaveGState (C);
	gcirc (x, y, r);
	CGContextClip (C);
	rgrad (x - 0.35f * r, y - 0.35f * r, 1.4f * r, 2, CS2 (light, dark), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
}

static void glow_ball (float x, float y, float r, unsigned light, unsigned dark, unsigned halo, float blur)
{
	CGContextSaveGState (C);
	glow (halo, 0.9f, blur);
	gcirc (x, y, r);
	gfill (dark, 1);
	CGContextRestoreGState (C);
	ball (x, y, r, light, dark);
}

/* dots along a line */
static void dotted (float x0, float y0, float x1, float y1, int n, float r, unsigned c, float a)
{
	int i;
	for (i = 0; i < n; i++) {
		float t = (float) i / (n - 1);
		gcirc (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r);
	}
	gfill (c, a);
}

/* a missile along +x, tail at -l/2, nose at l/2, r its radius; flame
 * out of the tail */
static void missile_shape (float l, float r)
{
	float t = -l / 2, n = l / 2, b = n - 0.24f * l;
	CGMutablePathRef p = CGPathCreateMutable ();

	/* the flame */
	CGContextSaveGState (C);
	glow (0xff8a2a, 0.9f, 3.5f);
	CGContextAddEllipseInRect (C, CGRectMake (t - 0.36f * l, -0.75f * r, 0.42f * l, 1.5f * r));
	CGContextClip (C);
	no_shade ();
	lgrad (t - 0.36f * l, 0, t, 0, 3, CS3 (0xff4d1a, 0xffb347, 0xfff3c0), AS3 (0, 0.85f, 1), AS3 (0, 0.55f, 1));
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0xffb347, 1, 2.5f);
	CGContextAddEllipseInRect (C, CGRectMake (t - 0.12f * l, -0.35f * r, 0.16f * l, 0.7f * r));
	gfill (0xfff6d8, 1);
	CGContextRestoreGState (C);

	/* fins */
	CGContextMoveToPoint (C, t + 0.06f * l, -r);
	CGContextAddLineToPoint (C, t - 0.02f * l, -2.3f * r);
	CGContextAddLineToPoint (C, t + 0.16f * l, -2.3f * r);
	CGContextAddLineToPoint (C, t + 0.34f * l, -r);
	CGContextClosePath (C);
	CGContextMoveToPoint (C, t + 0.06f * l, r);
	CGContextAddLineToPoint (C, t - 0.02f * l, 2.3f * r);
	CGContextAddLineToPoint (C, t + 0.16f * l, 2.3f * r);
	CGContextAddLineToPoint (C, t + 0.34f * l, r);
	CGContextClosePath (C);
	gfill (0xe8404a, 1);

	/* body and nose */
	CGPathMoveToPoint (p, NULL, t + 0.04f * l, -r);
	CGPathAddLineToPoint (p, NULL, b, -r);
	CGPathAddQuadCurveToPoint (p, NULL, n - 0.06f * l, -r, n, 0);
	CGPathAddQuadCurveToPoint (p, NULL, n - 0.06f * l, r, b, r);
	CGPathAddLineToPoint (p, NULL, t + 0.04f * l, r);
	CGPathCloseSubpath (p);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, -r, 0, r, 3, CS3 (0xf4f8fc, 0xc2ceda, 0x5d6c7c), AS3 (1, 1, 1), AS3 (0, 0.4f, 1));
	/* a red band behind the nose */
	fillc (0xe8404a, 1);
	CGContextFillRect (C, CGRectMake (b - 0.07f * l, -r, 0.05f * l, 2 * r));
	CGContextRestoreGState (C);
	/* the nozzle */
	CGContextAddRect (C, CGRectMake (t, -0.8f * r, 0.06f * l, 1.6f * r));
	gfill (0x3a4654, 1);
	CGPathRelease (p);
}

/* a view cone from a craft, opening right (m 1) or left (m -1) */
static void g_view (float m)
{
	CGContextMoveToPoint (C, m * -0.42f, 0);
	CGContextAddLineToPoint (C, m * 0.95f, -0.78f);
	CGContextAddLineToPoint (C, m * 0.95f, 0.78f);
	CGContextClosePath (C);
	CGContextSaveGState (C);
	CGContextClip (C);
	lgrad (m * -0.42f, 0, m * 0.95f, 0, 2, CS2 (0x4d7dff, 0x4d7dff), AS2 (0.35f, 0.04f), NULL);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0x4d7dff, 0.8f, 2);
	dotted (m * -0.3f, -0.07f, m * 0.95f, -0.78f, 5, 0.075f, 0x7ea2ff, 1);
	dotted (m * -0.3f, 0.07f, m * 0.95f, 0.78f, 5, 0.075f, 0x7ea2ff, 1);
	CGContextRestoreGState (C);
	/* the craft, seen from behind: hull and two engines */
	{
		CGMutablePathRef p = rrect (m * -0.72f - 0.13f, -0.46f, m * -0.72f + 0.13f, 0.46f, 0.1f);
		CGContextSaveGState (C);
		glow (0xe8eef5, 0.5f, 2);
		fill_path (p, 0xe8eef5, 1);
		CGContextRestoreGState (C);
		CGPathRelease (p);
	}
	gcirc (m * -0.72f, -0.2f, 0.07f);
	gcirc (m * -0.72f, 0.2f, 0.07f);
	gfill (0xff4d4d, 1);
}

static void g_ext (void)
{
	CGMutablePathRef p;
	/* the rays */
	CGContextSaveGState (C);
	glow (0x4d7dff, 0.8f, 2);
	dotted (0.02f, -0.18f, 0.95f, -0.85f, 5, 0.07f, 0x7ea2ff, 1);
	dotted (0.02f, 0.0f, 0.95f, 0.42f, 5, 0.07f, 0x7ea2ff, 1);
	CGContextRestoreGState (C);
	/* the post and its foot */
	gseg (-0.45f, 0.18f, -0.45f, 0.86f);
	gseg (-0.75f, 0.86f, -0.15f, 0.86f);
	gstroke (0x8fa3b8, 1, 1.3f);
	/* the camera */
	p = rrect (-0.86f, -0.4f, -0.12f, 0.16f, 0.1f);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, -0.4f, 0, 0.16f, 2, CS2 (0xe8eef5, 0x8fa3b8), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	CGContextMoveToPoint (C, -0.12f, -0.24f);
	CGContextAddLineToPoint (C, 0.08f, -0.36f);
	CGContextAddLineToPoint (C, 0.08f, 0.1f);
	CGContextAddLineToPoint (C, -0.12f, 0.0f);
	CGContextClosePath (C);
	gfill (0xc9d4e0, 1);
	gcirc (-0.5f, -0.12f, 0.1f);
	gfill (0x1a2633, 1);
}

static void g_galmap (void)
{
	CGMutablePathRef p = rrect (-0.8f, -0.8f, 0.8f, 0.8f, 0.14f);
	fill_path (p, 0x3ddc84, 0.10f);
	CGContextSaveGState (C);
	glow (0x3ddc84, 0.7f, 2);
	CGContextAddPath (C, p);
	gseg (0, -0.8f, 0, 0.8f);
	gseg (-0.8f, 0, 0.8f, 0);
	gstroke (0x3ddc84, 1, 1.15f);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	glow_ball (-0.42f, -0.42f, 0.11f, 0xff9a9a, 0xff4d4d, 0xff4d4d, 2);
	glow_ball (0.42f, -0.36f, 0.09f, 0xffd79a, 0xffb347, 0xffb347, 2);
	glow_ball (-0.38f, 0.42f, 0.09f, 0xffffff, 0xc9d4e0, 0xe8eef5, 2);
	glow_ball (0.4f, 0.4f, 0.06f, 0xffffff, 0xc9d4e0, 0xe8eef5, 1.5f);
}

/* orbits about a sun at (x, y), within the icon */
static void orbits (float x, float y, const float *r, int n)
{
	int i;
	CGContextSaveGState (C);
	CGContextAddRect (C, CGRectMake (-1.05f, -1.05f, 2.1f, 2.1f));
	CGContextClip (C);
	for (i = 0; i < n; i++) gcirc (x, y, r[i]);
	gstroke (0x8fa3b8, 0.55f, 0.8f);
	CGContextRestoreGState (C);
}

static void g_sysmap (void)
{
	static const float r[] = { 0.62f, 1.08f, 1.55f };
	orbits (-0.62f, 0.58f, r, 3);
	glow_ball (-0.62f, 0.58f, 0.3f, 0xfff3a0, 0xffa53a, 0xffb347, 5);
	ball (-0.07f, 0.2f, 0.08f, 0x9ab8ff, 0x4d7dff);
	ball (-0.16f, -0.43f, 0.09f, 0xd8e0ea, 0x8fa3b8);
	glow_ball (0.66f, -0.36f, 0.15f, 0xffffff, 0xb8c6d4, 0xe8eef5, 2);
}

/* a craft seen from above, pointing +x */
static void craft (void)
{
	static const float pt[] = {
		0.95f, 0, 0.25f, -0.17f, -0.36f, -0.64f, -0.5f, -0.6f, -0.3f, -0.16f,
		-0.58f, -0.12f, -0.58f, 0.12f, -0.3f, 0.16f, -0.5f, 0.6f, -0.36f, 0.64f,
		0.25f, 0.17f
	};
	CGMutablePathRef p = CGPathCreateMutable ();
	int i;
	CGPathMoveToPoint (p, NULL, pt[0], pt[1]);
	for (i = 1; i < 11; i++) CGPathAddLineToPoint (p, NULL, pt[2 * i], pt[2 * i + 1]);
	CGPathCloseSubpath (p);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, -0.64f, 0, 0.64f, 3, CS3 (0xe4eaf1, 0xa9b6c4, 0x5d6c7c), AS3 (1, 1, 1), AS3 (0, 0.45f, 1));
	CGContextRestoreGState (C);
	CGContextAddPath (C, p);
	gstroke (0xffffff, 0.45f, 0.6f);
	CGPathRelease (p);
	CGContextAddEllipseInRect (C, CGRectMake (0.16f, -0.075f, 0.34f, 0.15f));
	gfill (0x16202b, 1);
	CGContextAddEllipseInRect (C, CGRectMake (0.3f, -0.05f, 0.12f, 0.05f));
	gfill (0x3fd0ff, 0.8f);
}

static void g_ship (void)
{
	CGContextSaveGState (C);
	CGContextRotateCTM (C, (float) -M_PI / 4);
	CGContextScaleCTM (C, 0.92f, 0.92f);
	/* exhaust out of the tail */
	CGContextSaveGState (C);
	glow (0xff8a2a, 0.95f, 4);
	CGContextAddEllipseInRect (C, CGRectMake (-1.05f, -0.1f, 0.5f, 0.2f));
	gfill (0xffb347, 1);
	CGContextRestoreGState (C);
	CGContextAddEllipseInRect (C, CGRectMake (-0.85f, -0.05f, 0.3f, 0.1f));
	gfill (0xfff3c0, 1);
	craft ();
	CGContextRestoreGState (C);
}

static void g_cargo (void)
{
	/* a crate seen from above the front left corner */
	CGContextMoveToPoint (C, 0, -0.66f);
	CGContextAddLineToPoint (C, 0.78f, -0.3f);
	CGContextAddLineToPoint (C, 0, 0.06f);
	CGContextAddLineToPoint (C, -0.78f, -0.3f);
	CGContextClosePath (C);
	gfill (0xe8eef5, 0.92f);
	CGContextMoveToPoint (C, -0.78f, -0.3f);
	CGContextAddLineToPoint (C, 0, 0.06f);
	CGContextAddLineToPoint (C, 0, 0.86f);
	CGContextAddLineToPoint (C, -0.78f, 0.5f);
	CGContextClosePath (C);
	CGContextSaveGState (C);
	CGContextClip (C);
	lgrad (-0.78f, 0, 0, 0, 2, CS2 (0xff5a5a, 0xd83a44), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	CGContextMoveToPoint (C, 0, 0.06f);
	CGContextAddLineToPoint (C, 0.78f, -0.3f);
	CGContextAddLineToPoint (C, 0.78f, 0.5f);
	CGContextAddLineToPoint (C, 0, 0.86f);
	CGContextClosePath (C);
	gfill (0x9e2a33, 1);
	/* straps and edges */
	gseg (-0.39f, -0.12f, -0.39f, 0.68f);
	gseg (0.39f, -0.12f, 0.39f, 0.68f);
	gstroke (0x000000, 0.28f, 0.9f);
	gseg (0, 0.06f, 0, 0.86f);
	gseg (-0.78f, -0.3f, 0, 0.06f);
	gseg (0, 0.06f, 0.78f, -0.3f);
	gstroke (0xffffff, 0.35f, 0.6f);
}

static void g_cmdr (void)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	/* shoulders */
	CGPathMoveToPoint (p, NULL, -0.82f, 0.96f);
	CGPathAddCurveToPoint (p, NULL, -0.82f, 0.45f, -0.6f, 0.3f, -0.22f, 0.26f);
	CGPathAddLineToPoint (p, NULL, 0.22f, 0.26f);
	CGPathAddCurveToPoint (p, NULL, 0.6f, 0.3f, 0.82f, 0.45f, 0.82f, 0.96f);
	CGPathCloseSubpath (p);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, 0.26f, 0, 0.96f, 2, CS2 (0x3a4858, 0x1b2430), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	CGContextAddPath (C, p);
	gstroke (0x8fa3b8, 0.5f, 0.6f);
	CGPathRelease (p);
	/* epaulettes and badge */
	p = rrect (-0.7f, 0.33f, -0.36f, 0.46f, 0.05f);
	fill_path (p, 0xffb347, 1);
	CGPathRelease (p);
	p = rrect (0.36f, 0.33f, 0.7f, 0.46f, 0.05f);
	fill_path (p, 0xffb347, 1);
	CGPathRelease (p);
	CGContextSaveGState (C);
	glow (0xe8d84a, 0.8f, 1.5f);
	gcirc (0.36f, 0.66f, 0.08f);
	gfill (0xe8d84a, 1);
	CGContextRestoreGState (C);
	/* head and cap */
	ball (0, -0.2f, 0.34f, 0xf3a6ff, 0xc24ee0);
	CGContextMoveToPoint (C, -0.36f, -0.36f);
	CGContextAddCurveToPoint (C, -0.36f, -0.82f, 0.36f, -0.82f, 0.36f, -0.36f);
	CGContextClosePath (C);
	gfill (0x1b2430, 1);
	gseg (-0.46f, -0.36f, 0.46f, -0.36f);
	gstroke (0x8fa3b8, 1, 1);
}

static void g_phone (void)
{
	CGMutablePathRef p;
	CGContextSaveGState (C);
	glow (0xff4d4d, 0.6f, 2.5f);
	CGContextMoveToPoint (C, -0.6f, -0.12f);
	CGContextAddQuadCurveToPoint (C, 0, -0.98f, 0.6f, -0.12f);
	gstroke (0xff4d4d, 1, 4.2f);
	p = rrect (-0.92f, -0.22f, -0.38f, 0.06f, 0.1f);
	CGPathAddRoundedRect (p, NULL, CGRectMake (0.38f, -0.22f, 0.54f, 0.28f), 0.1f, 0.1f);
	fill_path (p, 0xff4d4d, 1);
	CGPathRelease (p);
	CGContextRestoreGState (C);
	{
		int i, j;
		for (j = 0; j < 2; j++)
			for (i = 0; i < 3; i++) gcirc (-0.36f + 0.36f * i, 0.42f + 0.32f * j, 0.07f);
		gfill (0x8fa3b8, 1);
	}
}

static void g_target (void)
{
	CGContextSaveGState (C);
	glow (0xe8d84a, 0.6f, 2);
	gcirc (0, 0, 0.56f);
	gstroke (0xe8d84a, 1, 1.25f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0x3ddc84, 0.6f, 1.5f);
	gseg (0, -0.96f, 0, -0.7f);
	gseg (0, 0.7f, 0, 0.96f);
	gseg (-0.96f, 0, -0.7f, 0);
	gseg (0.7f, 0, 0.96f, 0);
	gstroke (0x3ddc84, 1, 1.2f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0xff4d4d, 0.9f, 2.5f);
	gseg (-0.18f, 0, 0.18f, 0);
	gseg (0, -0.18f, 0, 0.18f);
	gstroke (0xff4d4d, 1, 1.6f);
	CGContextRestoreGState (C);
	glow_ball (0.8f, -0.66f, 0.09f, 0xffd79a, 0xffb347, 0xffb347, 2);
}

static void g_cross (void)
{
	CGContextSaveGState (C);
	glow (0x3ddc84, 0.6f, 1.5f);
	gseg (-0.96f, 0, -0.44f, 0);
	gseg (0.44f, 0, 0.96f, 0);
	gseg (0, -0.96f, 0, -0.44f);
	gseg (0, 0.44f, 0, 0.96f);
	gstroke (0x3ddc84, 1, 1.2f);
	gseg (-0.62f, -0.62f, -0.62f, 0.62f);
	gseg (0.62f, -0.62f, 0.62f, 0.62f);
	gstroke (0x3ddc84, 0.45f, 0.8f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0xe8d84a, 0.6f, 2);
	gcirc (0, 0, 0.32f);
	gstroke (0xe8d84a, 1, 1.2f);
	CGContextRestoreGState (C);
	gcirc (0, 0, 0.07f);
	gfill (0x3ddc84, 1);
	glow_ball (0.8f, -0.62f, 0.09f, 0xffd79a, 0xffb347, 0xffb347, 2);
}

static void g_wrench (void)
{
	/* the jaw's opening, cut out of the head */
	CGMutablePathRef cut = CGPathCreateMutable ();
	CGAffineTransform t = CGAffineTransformRotate (CGAffineTransformMakeTranslation (0.4f, -0.4f), (float) -M_PI / 4);
	CGPathAddRect (cut, &t, CGRectMake (0.0f, -0.14f, 0.7f, 0.28f));
	CGContextSaveGState (C);
	CGContextAddRect (C, CGRectMake (-2, -2, 4, 4));
	CGContextAddPath (C, cut);
	CGContextEOClip (C);
	glow (0xe8eef5, 0.45f, 2);
	gseg (-0.7f, 0.7f, 0.2f, -0.2f);
	gstroke (0xe8eef5, 1, 3.2f);
	gcirc (0.4f, -0.4f, 0.38f);
	gfill (0xe8eef5, 1);
	CGContextRestoreGState (C);
	CGPathRelease (cut);
	gcirc (-0.62f, 0.62f, 0.07f);
	gfill (0x1b2430, 1);
}

static void g_hyper (void)
{
	int i;
	CGContextSaveGState (C);
	glow (0xb8c6d4, 0.6f, 2.5f);
	for (i = 0; i < 3; i++) {
		CGContextSaveGState (C);
		CGContextRotateCTM (C, (float) (i * 2 * M_PI / 3));
		CGContextMoveToPoint (C, 0.14f, 0);
		CGContextAddCurveToPoint (C, 0.55f, -0.05f, 0.85f, 0.3f, 0.6f, 0.78f);
		gstroke (i ? 0xb8c6d4 : 0xe8eef5, 1, 1.6f);
		CGContextRestoreGState (C);
	}
	CGContextRestoreGState (C);
	glow_ball (0, 0, 0.14f, 0xffffff, 0xb8c6d4, 0xe8eef5, 3);
}

static void g_person (void)
{
	CGContextSaveGState (C);
	glow (0xe8d84a, 0.6f, 2);
	CGContextAddEllipseInRect (C, CGRectMake (-0.78f, 0.0f, 1.56f, 0.82f));
	gstroke (0xe8d84a, 1, 1.25f);
	CGContextRestoreGState (C);
	CGContextMoveToPoint (C, -0.5f, 0.36f);
	CGContextAddCurveToPoint (C, -0.5f, -0.18f, 0.5f, -0.18f, 0.5f, 0.36f);
	CGContextClosePath (C);
	CGContextSaveGState (C);
	CGContextClip (C);
	lgrad (0, -0.1f, 0, 0.36f, 2, CS2 (0xd04ef0, 0x8a2fa8), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	ball (0, -0.5f, 0.25f, 0xf3a6ff, 0xc24ee0);
	gcirc (-0.78f, 0.41f, 0.1f);
	gcirc (0.78f, 0.41f, 0.1f);
	gfill (0xe05cff, 1);
}

static void g_launch (void)
{
	/* the clamps open, the craft goes up */
	CGContextSaveGState (C);
	glow (0xe8eef5, 0.45f, 2);
	CGContextMoveToPoint (C, -0.88f, -0.6f);
	CGContextAddLineToPoint (C, -0.32f, 0.62f);
	CGContextAddLineToPoint (C, 0.24f, -0.6f);
	gstroke (0xe8eef5, 1, 1.6f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0x3ddc84, 0.8f, 2.5f);
	gseg (0.6f, 0.82f, 0.6f, -0.5f);
	gstroke (0x3ddc84, 1, 1.8f);
	CGContextMoveToPoint (C, 0.6f, -0.95f);
	CGContextAddLineToPoint (C, 0.3f, -0.48f);
	CGContextAddLineToPoint (C, 0.9f, -0.48f);
	CGContextClosePath (C);
	gfill (0x3ddc84, 1);
	CGContextRestoreGState (C);
}

static void g_list (void)
{
	CGContextSaveGState (C);
	glow (0xff4d4d, 0.8f, 2);
	gcirc (-0.68f, -0.56f, 0.12f);
	gfill (0xff4d4d, 1);
	glow (0x4d7dff, 0.8f, 2);
	gcirc (-0.68f, 0, 0.1f);
	gfill (0x4d7dff, 1);
	glow (0x3ddc84, 0.8f, 2);
	gcirc (-0.68f, 0.56f, 0.1f);
	gfill (0x3ddc84, 1);
	CGContextRestoreGState (C);
	gseg (-0.36f, -0.56f, 0.88f, -0.56f);
	gseg (-0.36f, 0, 0.6f, 0);
	gseg (-0.36f, 0.56f, 0.4f, 0.56f);
	gstroke (0xe8eef5, 0.9f, 1.3f);
}

static void g_arm (void)
{
	CGContextSaveGState (C);
	CGContextTranslateCTM (C, 0.12f, -0.12f);
	CGContextRotateCTM (C, (float) -M_PI / 4);
	missile_shape (1.75f, 0.13f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0xff4d4d, 0.8f, 2);
	gseg (-0.78f, 0.34f, -0.36f, 0.76f);
	gseg (-0.36f, 0.34f, -0.78f, 0.76f);
	gstroke (0xff4d4d, 1, 1.5f);
	CGContextRestoreGState (C);
}

static void g_beam (void)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	/* the beam */
	CGContextSaveGState (C);
	CGContextMoveToPoint (C, -0.14f, -0.1f);
	CGContextAddLineToPoint (C, 0.14f, -0.1f);
	CGContextAddLineToPoint (C, 0.34f, 0.9f);
	CGContextAddLineToPoint (C, -0.34f, 0.9f);
	CGContextClosePath (C);
	CGContextClip (C);
	lgrad (0, -0.1f, 0, 0.9f, 2, CS2 (0xfff09a, 0xe8d84a), AS2 (1, 0.08f), NULL);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	glow (0xe8d84a, 0.9f, 3);
	gseg (0, -0.05f, 0, 0.55f);
	gstroke (0xfff6c8, 1, 1.2f);
	CGContextRestoreGState (C);
	/* the dome */
	CGPathMoveToPoint (p, NULL, -0.86f, -0.12f);
	CGPathAddCurveToPoint (p, NULL, -0.82f, -0.98f, 0.82f, -0.98f, 0.86f, -0.12f);
	CGPathCloseSubpath (p);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, -0.8f, 0, -0.12f, 2, CS2 (0xe4eaf1, 0x6d7d8f), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	CGContextAddEllipseInRect (C, CGRectMake (-0.92f, -0.2f, 1.84f, 0.18f));
	gfill (0x8fa3b8, 1);
}

static void g_burst (void)
{
	int i;
	CGContextSaveGState (C);
	glow (0xff4d4d, 0.8f, 2.5f);
	for (i = 0; i < 8; i++) {
		float a = i * (float) M_PI / 4, c = cosf (a), s = sinf (a);
		gseg (0.34f * c, 0.34f * s, 0.64f * c, 0.64f * s);
	}
	gstroke (0xff4d4d, 1, 1.3f);
	CGContextRestoreGState (C);
	for (i = 0; i < 8; i += 2) {
		float a = (i + 1) * (float) M_PI / 4;
		glow_ball (0.8f * cosf (a), 0.8f * sinf (a), 0.08f, 0xffd79a, 0xffb347, 0xffb347, 2);
	}
	glow_ball (0, 0, 0.2f, 0xff9a9a, 0xff4d4d, 0xff4d4d, 4);
}

static void g_orrery (void)
{
	static const float r[] = { 0.5f, 0.86f, 1.22f };
	orbits (-0.6f, 0.0f, r, 3);
	glow_ball (-0.6f, 0.0f, 0.26f, 0xfff3a0, 0xffa53a, 0xffb347, 5);
	ball (-0.12f, -0.2f, 0.07f, 0x9ab8ff, 0x4d7dff);
	ball (0.24f, 0.22f, 0.12f, 0xf0d0b0, 0xb08a68);
	ball (0.58f, -0.24f, 0.1f, 0xd8e0ea, 0x8fa3b8);
}

/* an arrow from (x0, y0) to its head at (x1, y1) */
static void arrow (float x0, float y0, float x1, float y1, unsigned c)
{
	float dx = x1 - x0, dy = y1 - y0, l = sqrtf (dx * dx + dy * dy), ux = dx / l, uy = dy / l;
	float hl = 0.4f, hw = 0.28f;
	CGContextSaveGState (C);
	glow (c, 0.8f, 2);
	gseg (x0, y0, x1 - 0.6f * hl * ux, y1 - 0.6f * hl * uy);
	gstroke (c, 1, 1.9f);
	CGContextMoveToPoint (C, x1, y1);
	CGContextAddLineToPoint (C, x1 - hl * ux - hw * uy, y1 - hl * uy + hw * ux);
	CGContextAddLineToPoint (C, x1 - hl * ux + hw * uy, y1 - hl * uy - hw * ux);
	CGContextClosePath (C);
	gfill (c, 1);
	CGContextRestoreGState (C);
}

/* a red cross over an icon: not to be had */
static void red_x (float r)
{
	CGContextSaveGState (C);
	glow (0xff4d4d, 0.8f, 2);
	gseg (-r, -r, r, r);
	gseg (-r, r, r, -r);
	gstroke (0xff4d4d, 1, 1.8f);
	CGContextRestoreGState (C);
}

/* a disk: a case with its top right corner cut off, a shutter along the
 * top and a label below it */
static void disk (float x0, float y0, float x1, float y1)
{
	float w = x1 - x0, h = y1 - y0, c = 0.22f * w;
	float pt[10] = { x0, y0, x1 - c, y0, x1, y0 + c, x1, y1, x0, y1 };
	CGMutablePathRef p = poly (pt, 5, 0.07f * w);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, y0, 0, y1, 2, CS2 (0x6b95ff, 0x2a4cb8), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	CGContextAddPath (C, p);
	gstroke (0xffffff, 0.4f, 0.6f);
	CGPathRelease (p);
	/* the shutter and the window in it */
	p = rrect (x0 + 0.24f * w, y0, x1 - 0.3f * w, y0 + 0.36f * h, 0.03f * w);
	fill_path (p, 0xc9d4e0, 1);
	CGPathRelease (p);
	p = rrect (x0 + 0.46f * w, y0 + 0.07f * h, x0 + 0.58f * w, y0 + 0.28f * h, 0.02f * w);
	fill_path (p, 0x2a4cb8, 1);
	CGPathRelease (p);
	/* the label and its lines */
	p = rrect (x0 + 0.14f * w, y0 + 0.5f * h, x1 - 0.14f * w, y1, 0.04f * w);
	fill_path (p, 0xe8eef5, 1);
	CGPathRelease (p);
	gseg (x0 + 0.24f * w, y0 + 0.66f * h, x1 - 0.26f * w, y0 + 0.66f * h);
	gseg (x0 + 0.24f * w, y0 + 0.82f * h, x1 - 0.4f * w, y0 + 0.82f * h);
	gstroke (0x8fa3b8, 1, 0.7f);
}

/* the game menu: a game off the disk, a game onto it, one off it for good */
static void g_load (void)
{
	disk (-0.94f, -0.56f, 0.12f, 0.56f);
	arrow (-0.02f, 0, 0.96f, 0, 0x3fd0ff);
}

static void g_save (void)
{
	disk (-0.94f, -0.56f, 0.12f, 0.56f);
	arrow (0.96f, 0, 0.06f, 0, 0x3fd0ff);
}

static void g_delete (void)
{
	disk (-0.62f, -0.66f, 0.62f, 0.66f);
	red_x (0.6f);
}

/* out of the game: a doorway and an arrow out through it */
static void g_exit (void)
{
	CGContextMoveToPoint (C, 0.24f, -0.42f);
	CGContextAddLineToPoint (C, 0.24f, -0.84f);
	CGContextAddLineToPoint (C, -0.78f, -0.84f);
	CGContextAddLineToPoint (C, -0.78f, 0.84f);
	CGContextAddLineToPoint (C, 0.24f, 0.84f);
	CGContextAddLineToPoint (C, 0.24f, 0.42f);
	gstroke (0xc9d4e0, 1, 1.4f);
	arrow (-0.4f, 0, 0.96f, 0, 0x3fd0ff);
}

/* the map drawn out to a wider sector: a lattice turned on its corner */
static void g_sector (void)
{
	CGMutablePathRef p = rrect (-0.6f, -0.6f, 0.6f, 0.6f, 0.08f);
	CGContextSaveGState (C);
	CGContextRotateCTM (C, (float) M_PI / 4);
	fill_path (p, 0x3ddc84, 0.10f);
	CGContextSaveGState (C);
	glow (0x3ddc84, 0.7f, 2);
	CGContextAddPath (C, p);
	gstroke (0x3ddc84, 1, 1.15f);
	CGContextRestoreGState (C);
	gseg (-0.2f, -0.6f, -0.2f, 0.6f);
	gseg (0.2f, -0.6f, 0.2f, 0.6f);
	gseg (-0.6f, -0.2f, 0.6f, -0.2f);
	gseg (-0.6f, 0.2f, 0.6f, 0.2f);
	gstroke (0x3ddc84, 0.5f, 0.8f);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	glow_ball (0, -0.42f, 0.1f, 0xff9a9a, 0xff4d4d, 0xff4d4d, 2);
	glow_ball (0.38f, 0.14f, 0.08f, 0xffd79a, 0xffb347, 0xffb347, 2);
	glow_ball (-0.3f, 0.26f, 0.07f, 0xffffff, 0xc9d4e0, 0xe8eef5, 1.5f);
}

/* a course: from where we are, by a waypoint, to the star picked */
static void g_route (void)
{
	static const CGFloat dash[2] = { 0.1f, 0.2f };
	CGContextSaveGState (C);
	glow (0x3fd0ff, 0.7f, 2);
	CGContextMoveToPoint (C, -0.68f, 0.62f);
	CGContextAddCurveToPoint (C, -0.1f, 0.74f, -0.4f, -0.04f, 0.02f, 0);
	CGContextAddCurveToPoint (C, 0.42f, 0.04f, 0.3f, -0.6f, 0.68f, -0.62f);
	CGContextSetLineDash (C, 0, dash, 2);
	gstroke (0x3fd0ff, 1, 1.25f);
	CGContextRestoreGState (C);
	glow_ball (-0.68f, 0.62f, 0.13f, 0xffffff, 0xc9d4e0, 0xe8eef5, 2);
	glow_ball (0.02f, 0, 0.09f, 0xffd79a, 0xffb347, 0xffb347, 2);
	glow_ball (0.7f, -0.64f, 0.15f, 0xff9a9a, 0xff4d4d, 0xff4d4d, 2.5f);
}

/* off the ground: the warning sign of engines about to fire */
static void g_takeoff (void)
{
	float pt[6] = { 0, -0.92f, 0.86f, 0.5f, -0.86f, 0.5f };
	CGMutablePathRef p = poly (pt, 3, 0.12f);
	CGContextSaveGState (C);
	glow (0xffb347, 0.7f, 2);
	CGContextAddPath (C, p);
	gstroke (0xffb347, 1, 1.4f);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	gseg (0, -0.48f, 0, 0.04f);
	gstroke (0xffe2b8, 1, 1.5f);
	gcirc (0, 0.26f, 0.08f);
	gfill (0xffe2b8, 1);
	/* the flame under it */
	CGContextSaveGState (C);
	glow (0xff8a2a, 0.9f, 3);
	CGContextMoveToPoint (C, -0.3f, 0.64f);
	CGContextAddQuadCurveToPoint (C, 0, 1.12f, 0.3f, 0.64f);
	CGContextClosePath (C);
	gfill (0xffb347, 1);
	CGContextRestoreGState (C);
	CGContextMoveToPoint (C, -0.13f, 0.64f);
	CGContextAddQuadCurveToPoint (C, 0, 0.9f, 0.13f, 0.64f);
	CGContextClosePath (C);
	gfill (0xfff3c0, 1);
}

/* trade: a rising price line over a baseline, and a coin */
static void g_trade (void)
{
	gseg (-0.76f, 0.7f, 0.76f, 0.7f);
	gstroke (0x8fa3b8, 0.6f, 0.9f);
	CGContextSaveGState (C);
	glow (0xff7ad9, 0.7f, 2);
	CGContextMoveToPoint (C, -0.72f, 0.46f);
	CGContextAddLineToPoint (C, -0.28f, 0.06f);
	CGContextAddLineToPoint (C, 0.04f, 0.3f);
	CGContextAddLineToPoint (C, 0.66f, -0.44f);
	CGContextSetLineJoin (C, kCGLineJoinRound);
	gstroke (0xff7ad9, 1, 1.5f);
	CGContextRestoreGState (C);
	glow_ball (0.66f, -0.44f, 0.1f, 0xffd0f2, 0xff7ad9, 0xff7ad9, 2);
	glow_ball (-0.42f, -0.5f, 0.24f, 0xfff3a0, 0xd9a400, 0xffd24d, 2);
	gseg (-0.42f, -0.64f, -0.42f, -0.36f);
	gstroke (0x7a5a00, 0.9f, 0.9f);
}

/* a page of notes, its corner folded */
static void g_doc (void)
{
	float pt[10] = { -0.62f, -0.86f, 0.3f, -0.86f, 0.62f, -0.54f, 0.62f, 0.86f, -0.62f, 0.86f };
	CGMutablePathRef p = poly (pt, 5, 0.06f);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, -0.86f, 0, 0.86f, 2, CS2 (0xe8eef5, 0xb8c6d4), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	CGContextMoveToPoint (C, 0.3f, -0.86f);
	CGContextAddLineToPoint (C, 0.3f, -0.54f);
	CGContextAddLineToPoint (C, 0.62f, -0.54f);
	CGContextClosePath (C);
	gfill (0x8fa3b8, 1);
	gseg (-0.4f, -0.3f, 0.36f, -0.3f);
	gseg (-0.4f, 0.04f, 0.4f, 0.04f);
	gseg (-0.4f, 0.38f, 0.2f, 0.38f);
	gstroke (0x4d5d70, 1, 0.9f);
}

/* the names on the map: a letter */
static void g_alpha (void)
{
	CGContextSaveGState (C);
	glow (0xe8eef5, 0.45f, 2);
	CGContextMoveToPoint (C, -0.66f, 0.82f);
	CGContextAddLineToPoint (C, 0, -0.84f);
	CGContextAddLineToPoint (C, 0.66f, 0.82f);
	gseg (-0.38f, 0.22f, 0.38f, 0.22f);
	gstroke (0xe8eef5, 1, 1.7f);
	CGContextRestoreGState (C);
}

/* icon g at (cx, cy), s device pixels across; with G_OFF, dimmed and
 * crossed out */
static void glyph (int g, float cx, float cy, float s)
{
	int off = g & G_OFF;
	g &= ~G_OFF;
	if (g <= G_EMPTY) return;
	glyph_begin (cx, cy, s);
	if (off) {
		CGContextSetAlpha (C, 0.42f);
		CGContextBeginTransparencyLayer (C, NULL);
	}
	switch (g) {
	case G_FRONT: g_view (1); break;
	case G_REAR: g_view (-1); break;
	case G_EXT: g_ext (); break;
	case G_GALMAP: g_galmap (); break;
	case G_SYSMAP: g_sysmap (); break;
	case G_SHIP: g_ship (); break;
	case G_CARGO: g_cargo (); break;
	case G_CMDR: g_cmdr (); break;
	case G_PHONE: g_phone (); break;
	case G_TARGET: g_target (); break;
	case G_CROSS: g_cross (); break;
	case G_WRENCH: g_wrench (); break;
	case G_HYPER: g_hyper (); break;
	case G_PERSON: g_person (); break;
	case G_LAUNCH: g_launch (); break;
	case G_LIST: g_list (); break;
	case G_ARM: g_arm (); break;
	case G_BEAM: g_beam (); break;
	case G_BURST: g_burst (); break;
	case G_ORRERY: g_orrery (); break;
	case G_LOAD: g_load (); break;
	case G_SAVE: g_save (); break;
	case G_DELETE: g_delete (); break;
	case G_EXIT: g_exit (); break;
	case G_SECTOR: g_sector (); break;
	case G_ROUTE: g_route (); break;
	case G_TAKEOFF: g_takeoff (); break;
	case G_DOC: g_doc (); break;
	case G_ALPHA: g_alpha (); break;
	case G_TRADE: g_trade (); break;
	}
	if (off) {
		CGContextEndTransparencyLayer (C);
		CGContextSetAlpha (C, 1);
		red_x (0.62f);
	}
	glyph_end ();
}

/* ---- the control panel ---- */

enum { KEY_NORMAL, KEY_LIT, KEY_CAUTION };

/* a glass key, ST edges */
static void key (float x0, float y0, float x1, float y1, int state)
{
	CGMutablePathRef p = rrect (X (x0), Y (y0), X (x1), Y (y1), 4 * U);

	if (state == KEY_LIT || state == KEY_CAUTION) {
		unsigned c = state == KEY_LIT ? 0x3fd0ff : 0xffb347;
		outer_glow (p, 6 * U, c, 0.55f);
		if (state == KEY_LIT)
			fill_grad (p, Y (y0), Y (y1), 2, CS2 (0x14465c, 0x0a2533), AS2 (1, 1), NULL);
		else
			fill_grad (p, Y (y0), Y (y1), 2, CS2 (0x5a3e12, 0x2e2008), AS2 (1, 1), NULL);
		inner_shadow (p, 5 * U, 0, c, 0.45f);
		stroke_inside (p, 1.2f * U, c, 0.95f);
	} else {
		fill_grad (p, Y (y0), Y (y1), 2, CS2 (0x253140, 0x161d27), AS2 (1, 1), NULL);
		inner_shadow (p, 0, 1 * U, 0xffffff, 0.12f);
		inner_shadow (p, 3 * U, -1.5f * U, 0x000000, 0.45f);
		stroke_inside (p, 1 * U, 0xffffff, 0.05f);
	}
	CGPathRelease (p);
}

/* a recessed tray for keys */
static void tray (float x0, float y0, float x1, float y1)
{
	CGMutablePathRef p = rrect (X (x0), Y (y0), X (x1), Y (y1), 6 * U);
	stroke_outside (p, 1 * U, 0xffffff, 0.07f);
	fill_grad (p, Y (y0), Y (y1), 2, CS2 (0x05080c, 0x0a0f15), AS2 (1, 1), NULL);
	inner_shadow (p, 4 * U, 1.5f * U, 0x000000, 0.9f);
	CGPathRelease (p);
}

/* a key's number, bottom right */
static void key_number (int n, float x1, float y1, float h)
{
	char s[4];
	CTFontRef f = font ("DINAlternate-Bold");
	float cap = 0.27f * h, size = cap / (CTFontGetCapHeight (f) / 100), w;
	CGMutablePathRef p;

	snprintf (s, sizeof (s), "%d", n);
	p = text_path (f, s, size, 0, 0, 0, &w);
	CGPathRelease (p);
	p = text_path (f, s, size, 0, x1 - w - 0.09f * h, y1 - 0.12f * h, NULL);
	fill_path (p, 0x8fa3b8, 0.95f);
	CGPathRelease (p);
}

/* key n with icon g, ST edges */
static void icon_key (int n, int g, float x0, float y0, float x1, float y1)
{
	float w = X (x1) - X (x0), h = Y (y1) - Y (y0), s = fminf (0.68f * w, 0.78f * h);
	if (!in_art (x0, y0, x1, y1)) return;
	key (x0, y0, x1, y1, KEY_NORMAL);
	key_number (n, X (x1), Y (y1), h);
	glyph (g, X (x0) + 0.43f * w, Y (y0) + 0.47f * h, s);
}

/* a triangle pointing right (d 1) or left (d -1), its tip at x */
static void tri (float x, float cy, float w, float h, int d)
{
	float pt[6] = { x, cy, x - d * w, cy - h / 2, x - d * w, cy + h / 2 };
	CGMutablePathRef p = poly (pt, 3, 0.12f * h);
	CGContextAddPath (C, p);
	CGPathRelease (p);
}

/* n triangles in a key centred at (cx, cy), gh high, at most kw wide */
static void tris (float cx, float cy, float gh, float kw, int n, int d)
{
	float tw = 0.78f * gh, step = 0.62f * tw, total = tw + (n - 1) * step, sc = 1, x;
	int i;
	if (total > kw) sc = kw / total;
	tw *= sc;
	step *= sc;
	total *= sc;
	gh *= fminf (1, 0.3f + 0.7f * sc);
	x = cx - d * total / 2;
	for (i = 0; i < n; i++) tri (x + d * (tw + i * step), cy, tw, gh, d);
}

/* a time key's sign: 0 pause, 1..5 the speeds */
static void time_sign (int i, float cx, float cy, float kw, float kh, unsigned c, float a)
{
	float gh = 0.42f * kh;
	CGMutablePathRef p;

	fillc (c, a);
	if (i == 0) {
		p = rrect (cx - 0.38f * gh, cy - gh / 2, cx - 0.12f * gh, cy + gh / 2, 0.08f * gh);
		CGPathAddRoundedRect (p, NULL, CGRectMake (cx + 0.12f * gh, cy - gh / 2, 0.26f * gh, gh), 0.08f * gh, 0.08f * gh);
		CGContextAddPath (C, p);
		CGPathRelease (p);
	} else if (i < 5) {
		tris (cx, cy, gh, 0.8f * kw, i, 1);
	} else {
		/* flat out: a bar into a head */
		float hw = 0.36f * kw;
		p = rrect (cx - hw, cy - 0.17f * gh, cx + hw - 0.5f * gh, cy + 0.17f * gh, 0.08f * gh);
		CGContextAddPath (C, p);
		CGPathRelease (p);
		tri (cx + hw, cy, 0.7f * gh, 1.1f * gh, 1);
	}
	CGContextFillPath (C);
}

static void time_keys (void)
{
	/* lit < 0: none lit, time stands still while the game's menu is up,
	 * and the keys are dimmed */
	int i, lit = L.time;
	if (!in_art (0, 178, 64, 187)) return;
	tray (0.35f, 178.5f, 60.65f, 186.45f);
	for (i = 0; i < 6; i++) {
		float x0 = 10 * i + 0.95f, x1 = 10 * i + 10.05f, y0 = 179.15f, y1 = 185.8f;
		int state = lit < 0 ? KEY_NORMAL : lit == 0 ? (i == 0 ? KEY_CAUTION : KEY_NORMAL)
			: i == lit ? KEY_LIT : KEY_NORMAL;
		unsigned c = lit < 0 ? 0x8fa3b8 : state == KEY_LIT ? 0xffffff
			: state == KEY_CAUTION ? 0xffe2b8 : 0xc9d4e0;

		CGContextSaveGState (C);
		{	/* the lit key's glow stays in the tray */
			CGMutablePathRef p = rrect (X (0.35f), Y (178.5f), X (60.65f), Y (186.45f), 6 * U);
			CGContextAddPath (C, p);
			CGContextClip (C);
			CGPathRelease (p);
		}
		key (x0, y0, x1, y1, state);
		if (state != KEY_NORMAL) shade (0, 0, 4 * U, state == KEY_LIT ? 0x3fd0ff : 0xffb347, 0.9f);
		time_sign (i, (X (x0) + X (x1)) / 2, (Y (y0) + Y (y1)) / 2,
			   X (x1) - X (x0), Y (y1) - Y (y0), c, lit < 0 ? 0.55f : 1);
		CGContextRestoreGState (C);
	}
}

/* the map's time keys: back fast to forward fast */
static void map_keys (void)
{
	static const float edge[7] = { 261, 271, 281, 290, 299, 309, 319 };
	int i;
	if (!in_art (261, 178, 320, 187)) return;
	tray (261.35f, 178.5f, 319.7f, 186.45f);
	for (i = 0; i < 6; i++) {
		float x0 = edge[i] + 0.9f, x1 = edge[i + 1] + 0.1f, y0 = 179.15f, y1 = 185.8f;
		float kw = X (x1) - X (x0), kh = Y (y1) - Y (y0);
		int n = i < 3 ? 3 - i : i - 2, d = i < 3 ? -1 : 1;
		key (x0, y0, x1, y1, KEY_NORMAL);
		fillc (0xc9d4e0, 1);
		tris ((X (x0) + X (x1)) / 2, (Y (y0) + Y (y1)) / 2, 0.42f * kh, 0.8f * kw, n, d);
		CGContextFillPath (C);
	}
}

static void icon_keys (void)
{
	int i;
	if (in_art (0, 186, 65, 200)) {
		tray (0.3f, 187.2f, 64.4f, 199.65f);
		for (i = 0; i < 4; i++)
			icon_key (i + 1, L.slot[i + 1], 16 * i + 0.95f, 187.95f, 16 * i + 15.95f, 198.9f);
	}
	if (in_art (255, 186, 320, 200)) {
		tray (256.2f, 187.2f, 319.7f, 199.65f);
		for (i = 0; i < 4; i++)
			icon_key (i + 7, L.slot[i + 7], 256 + 16 * i + 0.95f, 187.95f,
				  fminf (256 + 16 * i + 15.95f, 319.05f), 198.9f);
	}
}

/* keys 5 and 6, raised out of the bay's floor when the game puts them there */
static void bay_keys (void)
{
	int i;
	for (i = 5; i <= 6; i++) {
		float x0 = 224 + 16 * (i - 5) + 0.6f, x1 = x0 + 14.9f, y0 = 186.6f, y1 = 198.8f;
		CGMutablePathRef p;
		if (L.slot[i] == G_ABSENT || !in_art (x0, y0, x1, y1)) continue;
		p = rrect (X (x0), Y (y0), X (x1), Y (y1), 4 * U);
		CGContextSaveGState (C);
		shade (0, 2 * U, 6 * U, 0x000000, 0.9f);
		fill_path (p, 0x161d27, 1);
		CGContextRestoreGState (C);
		CGPathRelease (p);
		icon_key (i, L.slot[i], x0, y0, x1, y1);
	}
}

/* the row of keys along the bay's floor */
static void buttons (void)
{
	static const float edge[8] = { 96, 112, 128, 144, 176, 192, 208, 223 };
	CGMutablePathRef p;
	int i;
	if (!in_art (96, 186, 224, 199)) return;
	p = rrect (X (96.4f), Y (186.4f), X (223.6f), Y (199), 6 * U);
	CGContextSaveGState (C);
	shade (0, 2 * U, 8 * U, 0x000000, 0.9f);
	fill_grad (p, Y (186.4f), Y (199), 2, CS2 (0x1c2530, 0x10161e), AS2 (1, 1), NULL);
	CGContextRestoreGState (C);
	fill_grad (p, Y (186.4f), Y (199), 2, CS2 (0x1c2530, 0x10161e), AS2 (1, 1), NULL);
	inner_shadow (p, 0, 1 * U, 0xffffff, 0.14f);
	CGPathRelease (p);
	for (i = 0; i < 7; i++)
		key (edge[i] + 0.95f, 187.9f, edge[i + 1] + 0.05f, 198.55f, KEY_NORMAL);
}

/* a dark glass display, ST edges */
static void display (float x0, float y0, float x1, float y1)
{
	CGMutablePathRef p;
	if (!in_art (x0, y0, x1, y1)) return;
	p = rrect (X (x0), Y (y0), X (x1), Y (y1), 5 * U);
	stroke_outside (p, 1 * U, 0xffffff, 0.08f);
	fill_grad (p, Y (y0), Y (y1), 2, CS2 (0x070b10, 0x0d151e), AS2 (1, 1), NULL);
	/* a sheen across the glass's top */
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, Y (y0), 0, Y (y0) + 0.45f * (Y (y1) - Y (y0)), 2, CS2 (0xffffff, 0xffffff), AS2 (0.045f, 0), NULL);
	CGContextRestoreGState (C);
	inner_shadow (p, 4 * U, 1.5f * U, 0x000000, 0.95f);
	stroke_inside (p, 1 * U, 0x8fd8ff, 0.06f);
	CGPathRelease (p);
}

/* a narrow dark slot */
static void groove (const float *pt, int n)
{
	float d[16];
	int i;
	CGMutablePathRef p;
	for (i = 0; i < n; i++) {
		d[2 * i] = X (pt[2 * i]);
		d[2 * i + 1] = Y (pt[2 * i + 1]);
	}
	p = poly (d, n, 1 * U);
	stroke_outside (p, 1 * U, 0xffffff, 0.08f);
	fill_path (p, 0x04070a, 1);
	inner_shadow (p, 2 * U, 1 * U, 0x000000, 0.9f);
	CGPathRelease (p);
}

/* the gunmetal the panel is made of, and its fixtures */
static void frame (void)
{
	float y0 = Y (CTRL_Y), y1 = Y (ST_H), ay = OY * SY, ax = OX * SX, w = ST_W * SX;
	int r, h = (int) ceilf (AH);

	CGContextSaveGState (C);
	CGContextAddRect (C, CGRectMake (X (0), y0, X (ST_W) - X (0), y1 - y0));
	CGContextClip (C);
	lgrad (0, y0, 0, y1, 3, CS3 (0x2b3746, 0x1c2531, 0x121820), AS3 (1, 1, 1), AS3 (0, 0.4f, 1));
	/* brushed: faint streaks, the same in every art they cross */
	for (r = 0; r < h; r++) {
		int ar = (int) floorf (ay + r);
		float x = 0;
		int k = 0;
		while (x < w) {
			unsigned hh = ihash ((unsigned) ar * 7919u + (unsigned) k++ * 104729u);
			float len = (50 + (hh & 255)) * U, a = ((hh >> 8) & 255) / 255.f;
			if (x + len > ax && x < ax + AW) {
				if (hh & 0x10000) CGContextSetRGBFillColor (C, 1, 1, 1, 0.03f * a);
				else CGContextSetRGBFillColor (C, 0, 0, 0, 0.06f * a);
				CGContextFillRect (C, CGRectMake (x - ax, r, len, 1));
			}
			x += len;
		}
	}
	/* the top edge catches the light */
	lgrad (0, y0, 0, y0 + 10 * U, 2, CS2 (0xffffff, 0xffffff), AS2 (0.07f, 0), NULL);
	fillc (0xffffff, 0.2f);
	CGContextFillRect (C, CGRectMake (X (0), y0, X (ST_W) - X (0), 1 * U));
	fillc (0x000000, 0.3f);
	CGContextFillRect (C, CGRectMake (X (0), y0 + 1 * U, X (ST_W) - X (0), 1 * U));
	CGContextRestoreGState (C);
}

static CGMutablePathRef bay_path (void)
{
	float pt[12] = {
		X (89), Y (169), X (233), Y (169), X (255), Y (183.67f),
		X (255), Y (199), X (66), Y (199), X (66), Y (184.33f)
	};
	return poly (pt, 6, 5 * U);
}

static void bay (void)
{
	CGMutablePathRef p;
	if (!in_art (64, 168, 256, 200)) return;
	p = bay_path ();
	stroke_outside (p, 1.2f * U, 0xffffff, 0.1f);
	fill_grad (p, Y (169), Y (199), 2, CS2 (0x020407, 0x070b11), AS2 (1, 1), NULL);
	inner_shadow (p, 7 * U, 2.5f * U, 0x000000, 1);
	stroke_inside (p, 1 * U, 0x8fd8ff, 0.05f);
	CGPathRelease (p);
}

/* three slanted channels the gauges fill */
static void slats (void)
{
	int i;
	if (!in_art (234, 169, 266, 178)) return;
	for (i = 0; i < 3; i++) {
		float c0 = 238.25f + 5 * i, c1 = c0 + 13.5f, hw = 1.5f;
		float pt[8] = { c0 - hw, 169.2f, c0 + hw, 169.2f, c1 + hw, 177.8f, c1 - hw, 177.8f };
		groove (pt, 4);
	}
}

static void scanner (void)
{
	float cx = X (160.5f), cy = Y (184), rx = 78.5f * SX, ry = 13.3f * SY;
	CGRect e = CGRectMake (cx - rx, cy - ry, 2 * rx, 2 * ry);
	static const float rows[] = { 171, 173, 175, 178, 181, 184, 188, 192, 196 };
	CGMutablePathRef p = CGPathCreateMutable ();
	int i;

	if (!in_art (80, 168, 241, 199)) {
		CGPathRelease (p);
		return;
	}
	CGPathAddEllipseInRect (p, NULL, e);
	/* a violet halo, then the glass */
	CGContextSaveGState (C);
	shade (0, 0, 9 * U, 0x9b6bff, 0.55f);
	stroke_path (p, 2 * U, 0x9b6bff, 0.2f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	CGContextSaveGState (C);
	CGContextTranslateCTM (C, cx, cy);
	CGContextScaleCTM (C, rx, ry);
	rgrad (0, 0, 1, 3, CS3 (0x123e52, 0x0a1d2a, 0x10364a), AS3 (0.9f, 0.85f, 0.95f), AS3 (0, 0.7f, 1));
	CGContextRestoreGState (C);
	/* the grid: rows going away, lines to the vanishing point */
	for (i = 0; i < 9; i++) {
		CGContextMoveToPoint (C, cx - rx, Y (rows[i] + 0.5f));
		CGContextAddLineToPoint (C, cx + rx, Y (rows[i] + 0.5f));
	}
	for (i = -8; i <= 8; i++) {
		float vx = X (160.5f), vy = Y (131), bx = X (160.5f + 11 * i), by = Y (186);
		float t = (Y (200) - vy) / (by - vy);
		CGContextMoveToPoint (C, vx, vy);
		CGContextAddLineToPoint (C, vx + (bx - vx) * t, Y (200));
	}
	strokec (0x3fd0ff, 0.26f);
	CGContextSetLineWidth (C, 1 * U);
	CGContextStrokePath (C);
	inner_shadow (p, 8 * U, 0, 0x3fd0ff, 0.45f);
	CGContextRestoreGState (C);
	/* the rim */
	CGContextSaveGState (C);
	shade (0, 0, 5 * U, 0x3fd0ff, 0.9f);
	stroke_path (p, 1.4f * U, 0x3fd0ff, 0.95f);
	CGContextRestoreGState (C);
	CGPathRelease (p);
	/* the field of view, from the ship */
	CGContextSetLineCap (C, kCGLineCapRound);
	CGContextSetLineJoin (C, kCGLineJoinRound);
	CGContextMoveToPoint (C, X (131.5f), Y (172));
	CGContextAddLineToPoint (C, X (160.5f), Y (181.5f));
	CGContextAddLineToPoint (C, X (189), Y (172));
	strokec (0xc9d4e0, 0.6f);
	CGContextSetLineWidth (C, 1.2f * U);
	CGContextStrokePath (C);
	CGContextSaveGState (C);
	shade (0, 0, 4 * U, 0x3fd0ff, 1);
	fillc (0xe8faff, 1);
	CGContextAddEllipseInRect (C, CGRectMake (X (160.5f) - 2 * U, Y (181.5f) - 2 * U, 4 * U, 4 * U));
	CGContextFillPath (C);
	CGContextRestoreGState (C);
}

static void panel (void)
{
	static const float notch_r[8] = { 263.0f, 170.6f, 263.9f, 170.6f, 263.9f, 177.4f, 261.9f, 177.4f };
	static const float chamfer[8] = { 260.7f, 179.4f, 261.6f, 179.4f, 257.3f, 185.8f, 256.3f, 185.8f };

	frame ();
	display (0.6f, 168.8f, 75.4f, 177.7f);
	display (264.6f, 168.8f, 319.3f, 177.7f);
	if (in_art (261, 169, 265, 178)) groove (notch_r, 4);
	if (in_art (256, 179, 262, 186)) groove (chamfer, 4);
	if (L.flags & F_MAPKEYS) map_keys ();
	else display (262.0f, 178.6f, 319.3f, 185.8f);
	bay ();
	slats ();
	if (L.flags & F_SCANNER) scanner ();
	time_keys ();
	icon_keys ();
	bay_keys ();
	if (L.flags & F_BUTTONS) buttons ();
}

/* ---- the missiles on their pylons ---- */

static void missile (int right)
{
	/* nose up and out, flame down and in; inside x+1..x+14, so two
	 * side by side do not overlap */
	float cx = X (OX + 7.5f), cy = Y (OY + 6.5f), s = fminf (13 * SX, 13 * SY);
	CGContextSaveGState (C);
	CGContextTranslateCTM (C, cx, cy);
	CGContextRotateCTM (C, right ? (float) -M_PI / 4 : (float) (-3 * M_PI / 4));
	CGContextTranslateCTM (C, 0.12f * s, 0);
	missile_shape (0.92f * s, 0.075f * s);
	CGContextRestoreGState (C);
}

/* ---- the main screen's list keys ---- */

/* a raised glass key with a soft shadow where the old one cast a hard
 * black one; its rim takes the grey's part */
static void list_glass (CGPathRef p, float y0, float y1)
{
	CGContextSaveGState (C);
	shade (1.2f * U, 2.5f * U, 5 * U, 0x000000, 0.7f);
	fill_path (p, 0x0b1016, 1);
	CGContextRestoreGState (C);
	fill_grad (p, y0, y1, 2, CS2 (0x2e3b4b, 0x151c26), AS2 (1, 1), NULL);
	inner_shadow (p, 0, 1.2f * U, 0xffffff, 0.16f);
	inner_shadow (p, 3 * U, -1.5f * U, 0x000000, 0.35f);
	stroke_inside (p, 1.1f * U, 0x8fa3b8, 0.8f);
}

/* the key beside a line of a list: a square, x+1..x+9 */
static void list_key (void)
{
	float y0 = Y (OY + 0.2f), y1 = Y (OY + 8.4f);
	CGMutablePathRef p = rrect (X (OX + 1), y0, X (OX + 9.4f), y1, 4 * U);
	list_glass (p, y0, y1);
	CGPathRelease (p);
}

/* the key beside an option, where the list keys go: lit and ticked when
 * the option is on.  No glow out of it: the screen's top row is right
 * above the first. */
static void opt_key (int on)
{
	float x0 = X (OX + 1), x1 = X (OX + 9.4f), y0 = Y (OY + 0.2f), y1 = Y (OY + 8.4f);
	float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	CGMutablePathRef p = rrect (x0, y0, x1, y1, 4 * U);
	if (!on) {
		list_glass (p, y0, y1);
		CGPathRelease (p);
		return;
	}
	fill_grad (p, y0, y1, 2, CS2 (0x14465c, 0x0a2533), AS2 (1, 1), NULL);
	inner_shadow (p, 5 * U, 0, 0x3fd0ff, 0.45f);
	stroke_inside (p, 1.2f * U, 0x3fd0ff, 0.95f);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	shade (0, 0, 3 * U, 0x3fd0ff, 0.9f);
	CGContextMoveToPoint (C, cx - 0.27f * w, cy + 0.02f * h);
	CGContextAddLineToPoint (C, cx - 0.07f * w, cy + 0.22f * h);
	CGContextAddLineToPoint (C, cx + 0.28f * w, cy - 0.22f * h);
	CGContextSetLineCap (C, kCGLineCapRound);
	CGContextSetLineJoin (C, kCGLineJoinRound);
	CGContextSetLineWidth (C, 3 * U);
	strokec (0xe8faff, 1);
	CGContextStrokePath (C);
	CGContextRestoreGState (C);
	CGPathRelease (p);
}

/* the arrow down by "more": shaft x+2..x+8, head x..x+10 */
static void list_more (void)
{
	float pt[14] = {
		X (OX + 2.3f), Y (OY + 0.2f), X (OX + 7.7f), Y (OY + 0.2f),
		X (OX + 7.7f), Y (OY + 4.3f), X (OX + 9.8f), Y (OY + 4.3f),
		X (OX + 5), Y (OY + 9.6f), X (OX + 0.2f), Y (OY + 4.3f),
		X (OX + 2.3f), Y (OY + 4.3f),
	};
	CGMutablePathRef p = poly (pt, 7, 2.5f * U);
	list_glass (p, pt[1], pt[9]);
	CGPathRelease (p);
}

/* ---- the title ---- */

static void logo (void)
{
	CTFontRef f = font ("AvenirNext-Heavy");
	float W = AW, H = AH, R = 0.76f * W, pcx = X (160), pcy = Y (171.5f) + R;
	float size, cap, w, track, base, sw;
	CGMutablePathRef p, t;

	/* the planet: a dark world whose lit limb arcs behind the name */
	p = CGPathCreateMutable ();
	CGPathAddEllipseInRect (p, NULL, CGRectMake (pcx - R, pcy - R, 2 * R, 2 * R));
	CGContextSaveGState (C);
	shade (0, 0, 16 * U, 0x9b6bff, 0.5f);
	stroke_path (p, 5 * U, 0x6b4bcf, 0.3f);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextClip (C);
	lgrad (0, pcy - R, 0, H, 3, CS3 (0x16314a, 0x0a1622, 0x05090e), AS3 (1, 0.95f, 0), AS3 (0, 0.35f, 1));
	inner_shadow (p, 14 * U, 0, 0x3fd0ff, 0.5f);
	CGContextRestoreGState (C);
	/* the limb, brightest towards the star */
	CGContextSaveGState (C);
	CGContextAddPath (C, p);
	CGContextSetLineWidth (C, 1.6f * U);
	CGContextReplacePathWithStrokedPath (C);
	CGContextClip (C);
	lgrad (X (90), 0, X (240), 0, 3, CS3 (0xbff0ff, 0x3fd0ff, 0x9b6bff), AS3 (1, 0.85f, 0.25f), NULL);
	CGContextRestoreGState (C);
	CGPathRelease (p);

	/* the orbit, a thin ring tilted across */
	p = CGPathCreateMutable ();
	{
		CGAffineTransform m = CGAffineTransformRotate (CGAffineTransformMakeTranslation (X (158), Y (186.5f)), -0.13f);
		float rx = 90 * SX, ry = 10.5f * SY;
		CGPathAddEllipseInRect (p, &m, CGRectMake (-rx, -ry, 2 * rx, 2 * ry));
	}
	CGContextSaveGState (C);
	shade (0, 0, 4 * U, 0x3fd0ff, 0.8f);
	CGContextAddPath (C, p);
	CGContextSetLineWidth (C, 1.3f * U);
	CGContextReplacePathWithStrokedPath (C);
	CGContextClip (C);
	no_shade ();
	lgrad (X (70), Y (196), X (230), Y (172), 3, CS3 (0xbff0ff, 0x3fd0ff, 0x3fd0ff), AS3 (0.95f, 0.5f, 0.08f), NULL);
	CGContextRestoreGState (C);
	CGPathRelease (p);

	/* the name */
	cap = 12.5f * SY;
	size = cap / (CTFontGetCapHeight (f) / 100);
	track = 0.2f * size;
	t = text_path (f, "FRONTIER", size, track, 0, 0, &w);
	CGPathRelease (t);
	sw = 176 * SX;
	if (w > sw) {
		size *= sw / w;
		track *= sw / w;
		cap *= sw / w;
		w = sw;
	}
	base = Y (182.5f) + cap / 2;
	t = text_path (f, "FRONTIER", size, track, X (160) - w / 2, base, NULL);
	CGContextSaveGState (C);
	shade (0, 3 * U, 8 * U, 0x000000, 0.85f);
	fill_path (t, 0x0b1016, 1);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	shade (0, 0, 14 * U, 0x3fd0ff, 0.35f);
	fill_path (t, 0x0b1016, 1);
	CGContextRestoreGState (C);
	CGContextSaveGState (C);
	CGContextAddPath (C, t);
	CGContextClip (C);
	{
		static const float loc[5] = { 0, 0.46f, 0.5f, 0.78f, 1 };
		lgrad (0, base - cap, 0, base, 5,
		       (const unsigned[]) { 0xffffff, 0xd4dee8, 0x8696a8, 0xa9b7c6, 0xe2e9f0 },
		       (const float[]) { 1, 1, 1, 1, 1 }, loc);
	}
	CGContextRestoreGState (C);
	stroke_path (t, 0.8f * U, 0xffffff, 0.25f);
	CGPathRelease (t);

	/* a star glints at the top left */
	{
		float sx = X (83), sy = Y (176), a = 7 * U, b = 1.1f * U;
		CGContextSaveGState (C);
		CGContextAddEllipseInRect (C, CGRectMake (sx - 6 * U, sy - 6 * U, 12 * U, 12 * U));
		CGContextClip (C);
		rgrad (sx, sy, 6 * U, 2, CS2 (0xbff0ff, 0x3fd0ff), AS2 (0.7f, 0), NULL);
		CGContextRestoreGState (C);
		CGContextSaveGState (C);
		shade (0, 0, 4 * U, 0xbff0ff, 1);
		fillc (0xffffff, 1);
		CGContextMoveToPoint (C, sx - 2.2f * a, sy);
		CGContextAddLineToPoint (C, sx, sy - b);
		CGContextAddLineToPoint (C, sx + 2.2f * a, sy);
		CGContextAddLineToPoint (C, sx, sy + b);
		CGContextClosePath (C);
		CGContextMoveToPoint (C, sx, sy - a);
		CGContextAddLineToPoint (C, sx + b, sy);
		CGContextAddLineToPoint (C, sx, sy + a);
		CGContextAddLineToPoint (C, sx - b, sy);
		CGContextClosePath (C);
		CGContextFillPath (C);
		CGContextRestoreGState (C);
	}
}

static void draw_art (const struct art *a, int x, int y)
{
	int i, s;
	switch (a->kind) {
	case K_LOGO:
		logo ();
		break;
	case K_MISSILE:
		missile (a->v);
		break;
	case K_SELECT:
		list_key ();
		break;
	case K_MORE:
		list_more ();
		break;
	case K_OPT_ON:
	case K_OPT_OFF:
		opt_key (a->kind == K_OPT_ON);
		break;
	default:
		memset (&L, 0, sizeof (L));
		L.time = a->v;
		L.flags = a->flags;
		for (i = 1; i <= 10; i++) L.slot[i] = i == 5 || i == 6 ? G_ABSENT : G_EMPTY;
		s = slot_at (x, y);
		for (i = 0; i < 5 && s > 0 && s + i <= 10; i++)
			if (a->g[i] != G_ABSENT) L.slot[s + i] = a->g[i];
		panel ();
		break;
	}
}

/* ---- the bodies on a star's information page ---- */

static unsigned mix (unsigned a, unsigned b, float t)
{
	unsigned c = 0;
	int ch;
	for (ch = 0; ch < 24; ch += 8)
		c |= (unsigned) ((a >> ch & 255) * (1 - t) + (b >> ch & 255) * t + 0.5f) << ch;
	return c;
}

/* the half of the ring behind the planet, or the half in front of it */
static void ring_half (const struct body *b, float cx, float cy, int front)
{
	float l = b->ring;
	CGContextSaveGState (C);
	CGContextTranslateCTM (C, cx, cy);
	CGContextRotateCTM (C, b->ring_a);
	CGContextClipToRect (C, CGRectMake (-l, front ? 0 : -l, 2 * l, l));
	CGContextAddEllipseInRect (C, CGRectMake (-l, -0.2f * l, 2 * l, 0.4f * l));
	CGContextAddEllipseInRect (C, CGRectMake (-0.74f * l, -0.13f * l, 1.48f * l, 0.26f * l));
	CGContextEOClip (C);
	lgrad (-l, 0, l, 0, 3, CS3 (mix (b->ring_c, 0xffffff, 0.3f), b->ring_c, mix (b->ring_c, 0, 0.6f)),
	       AS3 (0.95f, 0.85f, 0.6f), NULL);
	CGContextRestoreGState (C);
}

static void draw_body (const struct body *b)
{
	float cx = b->cx - OX * SX, cy = b->cy - OY * SY, r = b->r;
	static const float glow_at[3] = { 0.5f, 0.68f, 1 };

	if (b->star) {
		/* the corona, then the disc: a hot core, its limb darker */
		rgrad (cx, cy, 2 * r, 3, CS3 (b->lit, b->base, b->base), AS3 (0.6f, 0.2f, 0), glow_at);
		CGContextSaveGState (C);
		shade (0, 0, 0.3f * r, b->lit, 0.9f);
		gcirc (cx, cy, r);
		gfill (b->base, 1);
		CGContextRestoreGState (C);
		CGContextSaveGState (C);
		gcirc (cx, cy, r);
		CGContextClip (C);
		rgrad (cx - 0.15f * r, cy - 0.15f * r, 1.15f * r, 3,
		       CS3 (mix (b->lit, 0xffffff, 0.6f), b->lit, mix (b->base, 0, 0.25f)), AS3 (1, 1, 1), NULL);
		CGContextRestoreGState (C);
		return;
	}
	if (b->ring) ring_half (b, cx, cy, 0);
	/* lit from the star, at the page's left */
	CGContextSaveGState (C);
	gcirc (cx, cy, r);
	CGContextClip (C);
	rgrad (cx - 0.45f * r, cy - 0.3f * r, 1.5f * r, 3,
	       CS3 (mix (b->lit, 0xffffff, 0.3f), b->lit, b->base), AS3 (1, 1, 1), AS3 (0, 0.4f, 1));
	lgrad (cx - 0.1f * r, cy, cx + 0.8f * r, cy, 2, CS2 (0, 0), AS2 (0, 0.9f), NULL);
	CGContextRestoreGState (C);
	/* air on the day side's rim */
	CGContextSaveGState (C);
	gcirc (cx, cy, r);
	gcirc (cx, cy, 0.9f * r);
	CGContextEOClip (C);
	lgrad (cx - r, cy, cx + 0.3f * r, cy, 2, CS2 (mix (b->lit, 0xffffff, 0.5f), b->lit), AS2 (0.6f, 0), NULL);
	CGContextRestoreGState (C);
	if (b->ring) ring_half (b, cx, cy, 1);
}

#endif /* __APPLE__ */

/* ------------------------------------------------------------------ */

static u64 fnv (u64 h, const unsigned char *p, size_t n)
{
	while (n--) {
		h ^= *p++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int find_art (u64 hash, int w, int h)
{
	int i;
	for (i = 0; i < n_arts; i++)
		if (arts[i].hash == hash && arts[i].w == w && arts[i].h == h)
			return i;
	return -1;
}

void hdstill_blit (const unsigned char *planes, int width_words, int height,
		   int x, int y, unsigned int bmp, unsigned int scr)
{
	int w = width_words * 16, plane_incr = 2 * height * width_words;
	int i, row, col, count[16], best;
	unsigned char size[4];
	struct rec *r = NULL;
	u64 hash;

	(void) bmp;
	(void) scr;
	if (pixel_art || width_words <= 0 || height <= 0 || x < 0 || y < 0 ||
	    x + w > ST_W || y + height > ST_H)
		return;
	size[0] = width_words >> 8;
	size[1] = width_words;
	size[2] = height >> 8;
	size[3] = height;
	hash = fnv (0xcbf29ce484222325ULL, size, 4);
	hash = fnv (hash, planes, (size_t) plane_incr * 4);

	for (i = 0; i < n_recs; i++)
		if (recs[i].hash == hash && recs[i].x == x && recs[i].y == y) {
			r = &recs[i];
			break;
		}
	if (!r) {
		if (n_recs < MAX_REC)
			r = &recs[n_recs++];
		else {
			/* the one blitted longest ago makes room */
			r = &recs[0];
			for (i = 1; i < n_recs; i++)
				if (recs[i].seq < r->seq) r = &recs[i];
			free (r->pix);
		}
		memset (r, 0, sizeof (*r));
		r->pix = malloc ((size_t) w * height);
		if (!r->pix) {
			*r = recs[--n_recs];
			return;
		}
		r->x = x;
		r->y = y;
		r->w = w;
		r->h = height;
		r->hash = hash;
		r->art = find_art (hash, w, height);
		/* the planes as palette indices */
		memset (count, 0, sizeof (count));
		for (row = 0; row < height; row++) {
			for (col = 0; col < w; col++) {
				int o = 2 * (row * width_words + col / 16), b = 15 - (col & 15), p = 0, k;
				for (k = 0; k < 4; k++) {
					int word = (planes[o + k * plane_incr] << 8) | planes[o + k * plane_incr + 1];
					p |= ((word >> b) & 1) << k;
				}
				r->pix[row * w + col] = p;
				r->used |= 1u << p;
				count[p]++;
			}
		}
		for (best = 0, i = 1; i < 16; i++)
			if (count[i] > count[best]) best = i;
		r->bg = best;
	}
	r->seq = ++seq;
	r->misses = 0;
}

/* How far cur is faded from ref, the palette the art was drawn for, in
 * the colours the bitmap uses: 1 for the same, k < 1 for each k times as
 * bright (to within a step of the ST's), -1 for anything else (a flash,
 * another palette), which keeps the game's own pixels. */
static float fade_from (unsigned int used, const unsigned int *cur, const unsigned int *ref)
{
	float sc = 0, sr = 0, k;
	int i, ch, same = 1;

	for (i = 0; i < 16; i++) {
		if (!(used & (1u << i))) continue;
		if ((cur[i] & 0xffffff) != (ref[i] & 0xffffff)) same = 0;
		for (ch = 0; ch < 24; ch += 8) {
			int c = (cur[i] >> ch) & 0xff, f = (ref[i] >> ch) & 0xff;
			if (c > f) return -1;
			sc += c;
			sr += f;
		}
	}
	if (same) return 1;
	if (sr <= 0) return -1;
	k = sc / sr;
	for (i = 0; i < 16; i++) {
		if (!(used & (1u << i))) continue;
		for (ch = 0; ch < 24; ch += 8) {
			int c = (cur[i] >> ch) & 0xff, f = (ref[i] >> ch) & 0xff;
			if (fabsf (c - k * f) > 0x30) return -1;
		}
	}
	return k;
}

static float fade (const struct rec *r, const unsigned int *pal, const unsigned int *ctrl_pal)
{
	float k;
	/* the lists' keys are in the main screen's rows, the rest in the
	 * control panel's.  A list key goes on its screen's own background,
	 * whatever colour that is (each screen has its own): only the key's
	 * colours need be the ones its art was drawn for.  The last line's
	 * keys reach a row into the panel: that row is left to the panel. */
	if (ON_MAIN (arts[r->art].kind))
		return r->y < CTRL_Y ? fade_from (r->used & ~(1u << r->bg), pal,
			IS_OPT (arts[r->art].kind) ? pal_opt : pal_list) : -1;
	if (r->y < CTRL_Y) return -1;
	if (arts[r->art].kind == K_LOGO) return fade_from (r->used, ctrl_pal, pal_logo);
	k = fade_from (r->used, ctrl_pal, pal_scan);
	return k > 0 ? k : fade_from (r->used, ctrl_pal, pal_skin);
}

/* FE2_HD_DEBUG: the bitmaps seen and how many have been replaced */
static struct { u64 hash; unsigned char replaced; } seen[512];
static int n_seen;

static void note_seen (const struct rec *r, int replaced)
{
	int i, n_rep = 0, changed = 0;
	for (i = 0; i < n_seen; i++)
		if (seen[i].hash == r->hash) break;
	if (i == n_seen) {
		if (n_seen == 512) return;
		seen[n_seen].hash = r->hash;
		seen[n_seen++].replaced = 0;
		changed = 1;
	}
	if (replaced && !seen[i].replaced) {
		seen[i].replaced = 1;
		changed = 1;
	}
	if (!changed) return;
	for (i = 0; i < n_seen; i++) n_rep += seen[i].replaced;
	fprintf (stderr, "hdstill: coverage %d of %d bitmaps seen replaced\n", n_rep, n_seen);
}

/* FE2_HD_DEBUG=2 adds the colours the bitmap is shown in, index=rgb */
static void log_rec (const struct rec *r, const char *what, const unsigned int *pal)
{
	int i;

	fprintf (stderr, "hdstill: %s %016llx %dx%d at %d,%d%s%s", what,
		 r->hash, r->w, r->h, r->x, r->y,
		 r->art >= 0 ? " " : "", r->art >= 0 ? arts[r->art].name : "");
	if (debug >= 2)
		for (i = 0; i < 16; i++)
			if (r->used & (1u << i))
				fprintf (stderr, " %d=%02x%02x%02x", i, pal[i] & 0xff,
					 (pal[i] >> 8) & 0xff, (pal[i] >> 16) & 0xff);
	fputc ('\n', stderr);
}

/* FE2_HD_DUMP=dir: each bitmap seen and not replaced, as a PPM in the
 * palette it was shown in, for working out what it is.  Pictures of the
 * game: never keep them in the repository. */
static void dump (const struct rec *r, const unsigned int *pal)
{
	const char *dir = getenv ("FE2_HD_DUMP");
	char path[1024];
	FILE *f;
	int i;

	if (!dir) return;
	snprintf (path, sizeof (path), "%s/%016llx-%dx%d-%d-%d.ppm", dir, r->hash, r->w, r->h, r->x, r->y);
	f = fopen (path, "wb");
	if (!f) return;
	fprintf (f, "P6\n%d %d\n255\n", r->w, r->h);
	for (i = 0; i < r->w * r->h; i++) {
		unsigned int c = pal[r->pix[i]];
		fputc (c & 0xff, f);
		fputc ((c >> 8) & 0xff, f);
		fputc ((c >> 16) & 0xff, f);
	}
	fclose (f);
}

static int cmp_seq (const void *a, const void *b)
{
	const struct rec *p = *(const struct rec * const *) a, *q = *(const struct rec * const *) b;
	return p->seq < q->seq ? 1 : p->seq > q->seq ? -1 : 0;
}

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

/* Draw art a at ST (x, y) into t at sx x sy framebuffer pixels an ST
 * pixel.  0 if it could not be made. */
static int build (struct tex *t, int a, int x, int y, float sx, float sy)
{
#ifdef __APPLE__
	const struct art *ar = a >= 0 ? &arts[a] : NULL;
	int aw = ar ? ar->w : body_now->w, ah = ar ? ar->h : body_now->h;
	int w = (int) ceilf (aw * sx), h = (int) ceilf (ah * sy);
	int tw = pow2 (w), th = pow2 (h);
	unsigned char *buf;
	CGContextRef c;
	GLint was = 0;

	if (w <= 0 || h <= 0 || tw > 8192 || th > 8192) return 0;
	buf = calloc ((size_t) tw * th, 4);
	if (!buf) return 0;
	if (!CS) CS = CGColorSpaceCreateDeviceRGB ();
	c = CGBitmapContextCreate (buf, tw, th, 8, tw * 4, CS,
				   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	if (!c) {
		free (buf);
		return 0;
	}
	/* device pixels, y down from the art's top left, which is the
	 * buffer's first row */
	CGContextTranslateCTM (c, 0, th);
	CGContextScaleCTM (c, 1, -1);
	CGContextSetShouldAntialias (c, true);
	CGContextSetInterpolationQuality (c, kCGInterpolationHigh);
	CGContextClipToRect (c, CGRectMake (0, 0, aw * sx, ah * sy));
	C = c;
	OX = x;
	OY = y;
	SX = sx;
	SY = sy;
	AW = aw * sx;
	AH = ah * sy;
	U = (sx / 5 + sy / 6) / 2;
	if (ar) draw_art (ar, x, y);
	else draw_body (body_now);
	C = NULL;
	CGContextRelease (c);

	glGetIntegerv (GL_TEXTURE_BINDING_2D, &was);
	glPushClientAttrib (GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	glGenTextures (1, &t->id);
	glBindTexture (GL_TEXTURE_2D, t->id);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPopClientAttrib ();
	glBindTexture (GL_TEXTURE_2D, was);
	free (buf);
	t->art = a;
	t->x = x;
	t->y = y;
	t->s1 = AW / tw;
	t->t1 = AH / th;
	return t->id != 0;
#else
	(void) t; (void) a; (void) x; (void) y; (void) sx; (void) sy;
	return 0;
#endif
}

/* the texture of art a at (x, y) (a -1: body_now, by its key), made if
 * need be; -1 if there is none */
static int get_tex (int a, int x, int y, u64 key)
{
	int i, slot = -1;

	if (built_sx <= 0 || built_sy <= 0) return -1;
	for (i = 0; i < MAX_TEX; i++)
		if (texs[i].id && texs[i].art == a && texs[i].x == x && texs[i].y == y && texs[i].key == key) {
			texs[i].used = frame_no;
			return i;
		}
	/* a free one, or the one shown longest ago (not one shown this frame) */
	for (i = 0; i < MAX_TEX; i++) {
		if (!texs[i].id) {
			slot = i;
			break;
		}
		if (texs[i].used != frame_no && (slot < 0 || texs[i].used < texs[slot].used)) slot = i;
	}
	if (slot < 0) return -1;
	if (texs[slot].id) glDeleteTextures (1, &texs[slot].id);
	texs[slot].id = 0;
	if (!build (&texs[slot], a, x, y, built_sx, built_sy)) {
		if (texs[slot].id) glDeleteTextures (1, &texs[slot].id);
		texs[slot].id = 0;
		return -1;
	}
	texs[slot].key = key;
	texs[slot].used = frame_no;
	return slot;
}

/* hdstill_frame found r on the screen (newest first): what it shows of
 * the control panel, unless a newer bitmap already said */
static void note_panel (const struct rec *r)
{
	const struct art *a = &arts[r->art];
	int i, s;

	if (a->kind == K_MISSILE) {
		panel_now.n_missiles++;
		return;
	}
	if (a->kind != K_PANEL) return;
	panel_now.valid = 1;
	s = r->y >= CTRL_Y ? slot_at (r->x, r->y) : 0;
	for (i = 0; i < 5 && s > 0 && s + i <= 10; i++)
		if (a->g[i] != G_ABSENT && !panel_now.slot[s + i]) panel_now.slot[s + i] = a->g[i];
	/* the bay (its middle) and the second display (the map's keys) */
	if (!(panel_now.seen & 1) && r->x <= 160 && r->x + r->w > 160 && r->y <= 180 && r->y + r->h > 180) {
		panel_now.seen |= 1;
		panel_now.flags |= a->flags & F_SCANNER;
	}
	if (!(panel_now.seen & 2) && r->x <= 290 && r->x + r->w > 290 && r->y <= 181 && r->y + r->h > 181) {
		panel_now.seen |= 2;
		panel_now.flags |= a->flags & F_MAPKEYS;
	}
}

/* 0xAABBGGRR to 0xRRGGBB */
static unsigned rgb_of (unsigned p)
{
	return (p & 255) << 16 | (p >> 8 & 255) << 8 | (p >> 16 & 255);
}

static int lum (unsigned c)
{
	return 2 * (c >> 16 & 255) + 5 * (c >> 8 & 255) + (c & 255);
}

/* which pixels of the main screen each body candidate has (its index + 1) */
static unsigned char body_pix[ST_W * CTRL_Y];

/* The bodies on a star's information page: the unknown bitmaps on its
 * black (colour 0), cand[i] owning the pixels body_pix says are i + 1's.  Each is
 * drawn again as a sphere where its disc was (a star with its corona, a
 * planet lit from the left with its night side and any ring), and its
 * pixels go, for what is under the page (its black, a card) to show. */
static void add_bodies (struct rec **cand, int n, const unsigned char *raster,
			unsigned char *out, const unsigned int *pal)
{
	float sx = built_sx, sy = built_sy;
	int i;

	for (i = 0; i < n && i < MAX_BODY; i++) {
		struct rec *r = cand[i];
		struct body *b = &bodies[i];
		int cnt[16], ring, xx, yy, x0 = ST_W, y0 = ST_H, x1 = -1, y1 = -1, best = -1, n_px = 0, c, tex;
		float e, rs = 0, gs = 0, bs = 0;
		u64 key = r->hash;

#define MINE(xx, yy) (body_pix[(r->y + (yy)) * ST_W + r->x + (xx)] == i + 1 && \
		      r->pix[(yy) * r->w + (xx)] != 0 && \
		      raster[(r->y + (yy)) * ST_W + r->x + (xx)] == r->pix[(yy) * r->w + (xx)])
		memset (cnt, 0, sizeof (cnt));
		for (yy = 0; yy < r->h; yy++)
			for (xx = 0; xx < r->w; xx++) {
				if (!MINE (xx, yy)) continue;
				cnt[r->pix[yy * r->w + xx] & 15]++;
				if (xx < x0) x0 = xx;
				if (xx > x1) x1 = xx;
				if (yy < y0) y0 = yy;
				if (yy > y1) y1 = yy;
			}
		if (x1 < 0) continue;
		/* a body is solid about its middle, lit or night side (a
		 * pair of arcs about two stars is not) */
		if (x1 - x0 >= 12) {
			int bw = (x1 - x0 + 1) / 3, bh = (y1 - y0 + 1) / 3;
			for (yy = y0 + bh; yy < y0 + 2 * bh; yy++)
				for (xx = x0 + bw; xx < x0 + 2 * bw; xx++)
					n_px += body_pix[(r->y + yy) * ST_W + r->x + xx] == i + 1 && r->pix[yy * r->w + xx] == 0;
			if (4 * n_px > 3 * bw * bh) continue;
		}
		n_px = 0;
		/* a ring across it corner to corner (the ringed giant): its
		 * colour the top left corner's, its disc a third of the box
		 * about the middle */
		ring = x1 - x0 >= 12 && y1 - y0 >= 12 &&
		       MINE (x0, y0 + 1) + MINE (x0 + 1, y0) + MINE (x0 + 1, y0 + 1) &&
		       !MINE (x1, y0) && !MINE (x0, y1);
		for (yy = y0; ring && yy < y0 + 4; yy++)
			for (xx = x0; xx < x0 + 4; xx++) {
				unsigned q;
				if (!MINE (xx, yy)) continue;
				q = rgb_of (pal[r->pix[yy * r->w + xx] & 15]);
				rs += q >> 16 & 255;
				gs += q >> 8 & 255;
				bs += q & 255;
				n_px++;
			}
#undef MINE
		memset (b, 0, sizeof (*b));
		b->cx = (r->x + (x0 + x1 + 1) / 2.f) * sx;
		b->cy = (r->y + (y0 + y1 + 1) / 2.f) * sy;
		b->r = ((x1 - x0 + 1) * sx + (y1 - y0 + 1) * sy) / 4;
		if (ring) b->r *= 0.66f;
		/* the stars are the ones in the page's bright red */
		b->star = cnt[4] > 0 && y1 - y0 >= 16;
		if (b->star) b->r *= 0.92f;	/* inside its flares */
		/* a giant cut off by the screen's left edge: its widest rows
		 * are its middle, and it is as tall as it is wide about them */
		if (r->x + x0 == 0 && (y1 - y0 + 1) * sy > 1.25f * (x1 - x0 + 1) * sx) {
			int ys = 0, nr = 0;
			for (yy = y0; yy <= y1; yy++)
				if (r->pix[yy * r->w + x1] || r->pix[yy * r->w + x1 - 1]) {
					ys += yy;
					nr++;
				}
			b->cy = (r->y + ys / (float) nr + 0.5f) * sy;
			b->r = fmaxf (b->cy - (r->y + y0) * sy, (r->y + y1 + 1) * sy - b->cy) * (b->star ? 0.92f : 1);
			b->cx = (r->x + x1 + 1) * sx - b->r;
		}
		/* its colours: the commonest, and the brightest of the ones
		 * it has much of */
		for (c = 0; c < 16; c++) {
			if (!cnt[c]) continue;
			key = fnv (key, (const unsigned char *) &pal[c], 4);
			if (best < 0 || cnt[c] > cnt[best]) best = c;
		}
		b->base = rgb_of (pal[best]);
		b->lit = b->base;
		for (c = 0; c < 16; c++) {
			if (cnt[c] * 8 >= cnt[best] && lum (rgb_of (pal[c])) > lum (b->lit)) b->lit = rgb_of (pal[c]);
		}
		if (b->star) {
			/* a star: all its fire's colours together, hot toward a
			 * warm white in its core */
			float w = 0, ch[3] = { 0, 0, 0 };
			for (c = 0; c < 16; c++) {
				unsigned q = rgb_of (pal[c]);
				ch[0] += cnt[c] * (q >> 16 & 255);
				ch[1] += cnt[c] * (q >> 8 & 255);
				ch[2] += cnt[c] * (q & 255);
				w += cnt[c];
			}
			b->base = (unsigned) (ch[0] / w) << 16 | (unsigned) (ch[1] / w) << 8 | (unsigned) (ch[2] / w);
			b->lit = mix (b->base, 0xffe6a0, 0.6f);
		}
		if (ring && n_px) {
			b->ring = 0.5f * hypotf ((x1 - x0 + 1) * sx, (y1 - y0 + 1) * sy);
			b->ring_a = atan2f ((y1 - y0 + 1) * sy, (x1 - x0 + 1) * sx);
			b->ring_c = (unsigned) (rs / n_px) << 16 | (unsigned) (gs / n_px) << 8 | (unsigned) (bs / n_px);
		}
		/* its rectangle: room for the corona or the ring, on the
		 * main screen */
		e = fmaxf (b->r * (b->star ? 2 : 1.08f), b->ring) + 2;
		x0 = (int) floorf ((b->cx - e) / sx);
		y0 = (int) floorf ((b->cy - e) / sy);
		x1 = (int) ceilf ((b->cx + e) / sx);
		y1 = (int) ceilf ((b->cy + e) / sy);
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > ST_W) x1 = ST_W;
		if (y1 > CTRL_Y) y1 = CTRL_Y;
		if (x1 <= x0 || y1 <= y0) continue;
		b->x = x0;
		b->y = y0;
		b->w = x1 - x0;
		b->h = y1 - y0;
		b->key = key;
		body_now = b;
		tex = get_tex (-1, x0, y0, key);
		body_now = NULL;
		if (tex < 0) continue;
		for (yy = 0; yy < r->h; yy++)
			for (xx = 0; xx < r->w; xx++) {
				int o = (r->y + yy) * ST_W + r->x + xx, p = r->pix[yy * r->w + xx];
				if (body_pix[o] == i + 1 && p != 0 && raster[o] == p) out[o] = 255;
			}
		{
			struct shown *s = &shown[n_shown++];
			memset (s, 0, sizeof (*s));
			s->x = x0;
			s->y = y0;
			s->w = x1 - x0;
			s->h = s->vh = y1 - y0;
			s->tex = tex;
			s->k = 1;
			s->body = 1;
		}
	}
}

int hdstill_panel (struct hdstill_panel *p)
{
	*p = panel_now;
	return panel_now.valid;
}

void hdstill_frame (const unsigned char *raster, unsigned char *out,
		    const unsigned int *pal, const unsigned int *ctrl_pal)
{
	static unsigned char cover[ST_W * ST_H];
	struct rec *order[MAX_REC], *cand[MAX_BODY];
	GLint vp[4] = { 0, 0, 0, 0 };
	int i, j, xx, yy, n_cand = 0, info = 0;
	int bodies_on = !classic_ui && use_renderer == R_GL;

	n_shown = 0;
	memset (&panel_now, 0, sizeof (panel_now));
	if (pixel_art) return;
	if (debug < 0) debug = getenv ("FE2_HD_DEBUG") ? atoi (getenv ("FE2_HD_DEBUG")) : 0;
	frame_no++;

	/* the art is drawn for the framebuffer's size: the view is the
	 * whole of it */
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] > 0 && vp[3] > 0 &&
	    (vp[2] / (float) ST_W != built_sx || vp[3] / (float) ST_H != built_sy)) {
		hdstill_forget ();
		built_sx = vp[2] / (float) ST_W;
		built_sy = vp[3] / (float) ST_H;
	}

	/* forget what has been off the screen too long */
	for (i = 0; i < n_recs; ) {
		if (recs[i].misses > MISS_LIMIT) {
			free (recs[i].pix);
			recs[i] = recs[--n_recs];
			continue;
		}
		i++;
	}
	for (i = 0; i < n_recs; i++) order[i] = &recs[i];
	qsort (order, n_recs, sizeof (order[0]), cmp_seq);

	/* newest first: a bitmap is on the screen when nearly all the pixels
	 * no newer one on the screen covers are its own, most of all the
	 * ones that are not its background */
	memset (cover, 0, sizeof (cover));
	if (bodies_on) memset (body_pix, 0, sizeof (body_pix));
	for (i = 0; i < n_recs; i++) {
		struct rec *r = order[i];
		const unsigned int *shown_pal = r->y >= CTRL_Y ? ctrl_pal : pal;
		int vis = 0, match = 0, dvis = 0, dmatch = 0, taken = 0, on, defer, solid, onmain, tex = -1, ci = 0;
		float k = -1;

		for (yy = 0; yy < r->h; yy++) {
			for (xx = 0; xx < r->w; xx++) {
				int o = (r->y + yy) * ST_W + r->x + xx, p = r->pix[yy * r->w + xx], ok;
				if (cover[o]) continue;
				/* drawn over and taken out by another HD layer
				 * (text): that layer draws it again on top */
				ok = raster[o] == p || (out[o] != raster[o] && out[o] != 255);
				vis++;
				match += ok;
				taken += out[o] == 255;
				if (p != r->bg) {
					dvis++;
					dmatch += ok;
				}
			}
		}
		/* one colour all through (a plain bar): every pixel, as there
		 * is nothing but background to tell it by */
		solid = r->used == 1u << r->bg;
		on = vis > 0 && match * 100 >= vis * 85 &&
		     (solid ? match == vis : dvis > 0 && dmatch * 100 >= dvis * 75);
		if (!on) {
			r->misses++;
			continue;
		}
		r->misses = 0;
		if (r->art >= 0) note_panel (r);
		/* a layer before this one (the restyled screens) has taken
		 * it out: theirs */
		defer = taken * 100 > vis * DEFER_PCT;
		if (r->art >= 0 && !defer) {
			k = fade (r, pal, ctrl_pal);
			if (k > 0 && (tex = get_tex (r->art, r->x, r->y, 0)) < 0) k = -1;
		}
		if (debug) {
			int what = defer ? 4 : k > 0 ? 2 : 1;
			if (!(r->logged & what)) {
				r->logged |= what;
				log_rec (r, defer ? "deferred" : r->art < 0 ? "unknown" : k > 0 ? "replaced" : "kept (palette)",
					 shown_pal);
				if (!defer && k <= 0) dump (r, shown_pal);
			}
			note_seen (r, k > 0);
		}
		onmain = r->art >= 0 && ON_MAIN (arts[r->art].kind);
		if (k > 0 && (arts[r->art].flags & F_INFO)) info = 1;
		/* maybe a body on a star's information page */
		if (bodies_on && r->art < 0 && !defer && (r->used & 1) && r->y + r->h <= CTRL_Y && n_cand < MAX_BODY) {
			cand[n_cand++] = r;
			ci = n_cand;
		}
		for (yy = 0; yy < r->h; yy++) {
			for (xx = 0; xx < r->w; xx++) {
				int o = (r->y + yy) * ST_W + r->x + xx, p = r->pix[yy * r->w + xx];
				if (cover[o]) continue;
				if (ci) body_pix[o] = ci;
				/* a key replaced on the main screen leaves the row it
				 * reaches into the panel to the panel: where it is the
				 * panel's own pixel, the panel's art shows */
				if (k > 0 && onmain && r->y + yy >= CTRL_Y) continue;
				cover[o] = 1;
				if (k > 0 && (raster[o] == p || (out[o] != raster[o] && out[o] != 255)))
					out[o] = 255;
			}
		}
		if (k > 0) {
			struct shown *s = &shown[n_shown++];
			s->x = r->x;
			s->y = r->y;
			s->w = r->w;
			s->h = r->h;
			s->tex = tex;
			s->k = k;
			/* its background goes too: the art goes on it */
			s->bg = onmain ? pal[r->bg] | 0xff000000u : 0;
			s->vh = onmain && r->y + r->h > CTRL_Y ? CTRL_Y - r->y : r->h;
			s->body = 0;
		}
	}
	if (info) add_bodies (cand, n_cand, raster, out, pal);
	/* oldest first, so newer art goes on top */
	for (i = 0, j = n_shown - 1; i < j; i++, j--) {
		struct shown t = shown[i];
		shown[i] = shown[j];
		shown[j] = t;
	}
}

void hdstill_draw (const unsigned int *pal, const unsigned int *ctrl_pal)
{
	GLint screen = 0, scr_w = 0, scr_h = 0;
	int i;

	(void) pal;
	(void) ctrl_pal;
	if (pixel_art || !n_shown) return;
	/* the screen's texture is the one bound: the GL renderer just drew it */
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &screen);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &scr_w);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &scr_h);

	glEnable (GL_TEXTURE_2D);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);	/* premultiplied */
	for (i = 0; i < n_shown; i++) {
		const struct shown *d = &shown[i];
		const struct tex *t = &texs[d->tex];
		float x0 = d->x, x1 = d->x + d->w, y0 = ST_H - d->y, y1 = ST_H - d->y - d->vh, t1;

		if (d->tex < 0 || !t->id) continue;
		t1 = t->t1 * d->vh / d->h;	/* the art's top rows only */
		if (d->bg) {
			glDisable (GL_TEXTURE_2D);
			glColor4ub (d->bg & 255, d->bg >> 8 & 255, d->bg >> 16 & 255, 255);
			glBegin (GL_QUADS);
			glVertex2f (x0, y0);
			glVertex2f (x1, y0);
			glVertex2f (x1, y1);
			glVertex2f (x0, y1);
			glEnd ();
			glEnable (GL_TEXTURE_2D);
		}
		glBindTexture (GL_TEXTURE_2D, t->id);
		glColor4f (d->k, d->k, d->k, 1);
		glBegin (GL_QUADS);
		glTexCoord2f (0, 0);
		glVertex2f (x0, y0);
		glTexCoord2f (t->s1, 0);
		glVertex2f (x1, y0);
		glTexCoord2f (t->s1, t1);
		glVertex2f (x1, y1);
		glTexCoord2f (0, t1);
		glVertex2f (x0, y1);
		glEnd ();
	}

	/* what has been drawn over the bitmaps goes back on top: the screen
	 * again, where it is not transparent */
	glBindTexture (GL_TEXTURE_2D, screen);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub (255, 255, 255, 255);
	if (scr_w > 0 && scr_h > 0) {
		glBegin (GL_QUADS);
		for (i = 0; i < n_shown; i++) {
			const struct shown *d = &shown[i];
			float x0 = d->x, x1 = d->x + d->w, y0 = d->y, y1 = d->y + d->vh;
			if (d->body) continue;
			glTexCoord2f (x0 / scr_w, y0 / scr_h);
			glVertex2f (x0, ST_H - y0);
			glTexCoord2f (x1 / scr_w, y0 / scr_h);
			glVertex2f (x1, ST_H - y0);
			glTexCoord2f (x1 / scr_w, y1 / scr_h);
			glVertex2f (x1, ST_H - y1);
			glTexCoord2f (x0 / scr_w, y1 / scr_h);
			glVertex2f (x0, ST_H - y1);
		}
		glEnd ();
	}
	glDisable (GL_BLEND);
}

void hdstill_forget (void)
{
	int i;
	for (i = 0; i < MAX_TEX; i++) {
		if (texs[i].id) glDeleteTextures (1, &texs[i].id);
		texs[i].id = 0;
	}
	built_sx = built_sy = 0;
	n_shown = 0;
}
