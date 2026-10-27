/*
 * The Controls page: the bindings of bindings.c on a frosted glass page
 * in the HD screens' design system (hdui.c's tokens, fonts and shapes),
 * drawn by the host over the whole picture.  F12 opens and closes it,
 * Esc closes it, and the game menu shows a Controls button that opens it.
 *
 * Nothing here writes ST memory.  While the page is open the game gets
 * no keys and no mouse buttons; on opening, a game in flight that is
 * running is paused with the game's own Esc key, and on closing the time
 * control it was on is clicked again.
 *
 * The whole page is painted with CoreGraphics and CoreText into one
 * texture, again only when something on it changes.
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
#include <SDL.h>

#include "main.h"
#include "input.h"
#include "screen.h"
#include "bindings.h"
#include "keymap.h"
#include "art.h"
#include "hdui.h"
#include "../m68000.h"
#include "../fixups.h"

#define A6		__DL5eb6_a6_base

/* hdui.c's design tokens (0xRRGGBB) */
#define C_BG0		0x0b1016
#define C_BG1		0x141c26
#define C_GLASS		0x101823
#define C_CARD		0x142030
#define C_CONTROL	0x1b2838
#define C_ACCENT	0x3fd0ff
#define C_AMBER		0xffb347
#define C_RED		0xff4d4d
#define C_TEXT		0xe8eef5
#define C_TEXT2		0x8fa3b8
#define C_TEXT2_SEE	0xc2d0e0

/* the page is laid out on a 1600 x 1200 design, scaled to the picture */
#define DW		1600
#define DH		1200

static int is_open, dirty = 1;
static int hover = -1;			/* the hot spot under the pointer */
static int pressed = -1;		/* the hot spot the left button went down on */
static int cap_action = -1, cap_mouse;	/* waiting for a key (0) or a button (1) for an action */
static int modal;			/* M_* */
static int m_action, m_mouse, m_value, m_other;
static char status[160];
static int status_warn;
static int paused_by_us, rate_before = -1;
static int entry_hover, entry_pressed, entry_shown;
static int resume_m = -1, resume_frames, esc_frames;
static int resume_saved_x, resume_saved_y;
static int dbg = -1;

enum { M_NONE, M_CONFLICT, M_RIGHT };

/* hot spots */
enum { H_TOGGLE = 1, H_RESET, H_CLOSE, H_M_SWAP, H_M_CLEAR, H_M_CANCEL, H_M_YES, H_ROW = 100 };
#define H_KEY(i)	(H_ROW + 3 * (i) + 1)
#define H_MOUSE(i)	(H_ROW + 3 * (i) + 2)
#define H_LABEL(i)	(H_ROW + 3 * (i))

struct spot { float x0, y0, x1, y1; int id; };
#define MAX_SPOTS	160
static struct spot spots[MAX_SPOTS];
static int n_spots;

static void spot (float x0, float y0, float x1, float y1, int id)
{
	if (n_spots < MAX_SPOTS) {
		spots[n_spots].x0 = x0; spots[n_spots].y0 = y0;
		spots[n_spots].x1 = x1; spots[n_spots].y1 = y1;
		spots[n_spots].id = id;
		n_spots++;
	}
}

static int spot_at (float x, float y)
{
	int i;
	/* the last added is on top (the modal's buttons) */
	for (i = n_spots - 1; i >= 0; i--)
		if (x >= spots[i].x0 && x < spots[i].x1 && y >= spots[i].y0 && y < spots[i].y1)
			return spots[i].id;
	return -1;
}

static int rb (unsigned long a) { return STMemory_ReadByte (A6 + a) & 0xff; }
static unsigned long rl (unsigned long a) { return (unsigned long) STMemory_ReadLong (A6 + a); }

/* the console time controls' rates, stopped first (bindings.c has the same) */
static const unsigned long rates[] = { 0, 0x3e2, 0x26d6, 0x1845c, 0xf2b98, 0x97b3f0 };

static int rate_index (void)
{
	unsigned long r = rl (10428);
	int m;
	for (m = 0; m < 6; m++)
		if (rates[m] == r) return m;
	return -1;
}

/* in a game (not the intro or the start menu, which keep bit 5 of
 * 598(a6) set) and not docked or landed */
static int flying (void)
{
	int fs = rb (704);
	return !(rb (598) & 0x20) && fs != 0x54 && fs != 0x60 && fs != 0x68;
}

/* the game menu (Esc while paused) is on the screen */
static int game_menu_up (void)
{
	return rb (10469) == 1 && rl (10428) == 0;
}

/* the pointer in design units */
static void pointer (float *x, float *y)
{
	float vx = view_w > 0 ? view_x : 0, vy = view_h > 0 ? view_y : 0;
	float vw = view_w > 0 ? view_w : screen_w, vh = view_h > 0 ? view_h : screen_h;
	*x = (input.abs_x - vx) * DW / vw;
	*y = (input.abs_y - vy) * DH / vh;
}

/* ---------------------------------------------------------------- */
/* Painting */

#ifdef __APPLE__

enum { F_TITLE, F_COND, F_SANS, F_SANS_BOLD, NFACE };
static const char *const face_file[NFACE] = {
	"ui/fonts/Orbitron-Regular.ttf",
	"ui/fonts/Oswald-Regular.ttf",
	"ui/fonts/OpenSans-Regular.ttf",
	"ui/fonts/OpenSans-Bold.ttf",
};
static CTFontDescriptorRef face_desc[NFACE];
static int faces_tried;
static CGColorSpaceRef rgb_space;

static void load_faces (void)
{
	int f;
	if (faces_tried) return;
	faces_tried = 1;
	for (f = 0; f < NFACE; f++) {
		const char *path = art_path (face_file[f]);
		CFURLRef url = CFURLCreateFromFileSystemRepresentation (NULL, (const UInt8 *) path,
								       strlen (path), false);
		if (url) {
			CFArrayRef a = CTFontManagerCreateFontDescriptorsFromURL (url);
			if (a && CFArrayGetCount (a) > 0) {
				face_desc[f] = CFArrayGetValueAtIndex (a, 0);
				CFRetain (face_desc[f]);
			}
			if (a) CFRelease (a);
			CFRelease (url);
		}
	}
}

static CTFontRef font (int f, float size)
{
	if (face_desc[f]) return CTFontCreateWithFontDescriptor (face_desc[f], size, NULL);
	return CTFontCreateWithName (CFSTR ("HelveticaNeue"), size, NULL);
}

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

static CGPathRef rrect (float x, float y, float w, float h, float r)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	CGPathAddRoundedRect (p, NULL, CGRectMake (x, y, w, h), r, r);
	return p;
}

static void vgrad (CGContextRef g, float y0, float y1, unsigned int c0, float a0, unsigned int c1, float a1)
{
	CGFloat comp[8] = { CR (c0), CG_ (c0), CB (c0), a0, CR (c1), CG_ (c1), CB (c1), a1 };
	CGFloat loc[2] = { 0, 1 };
	CGGradientRef gr = CGGradientCreateWithColorComponents (rgb_space, comp, loc, 2);
	CGContextDrawLinearGradient (g, gr, CGPointMake (0, y0), CGPointMake (0, y1),
				     kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
	CGGradientRelease (gr);
}

/* a soft shadow or glow of path p, round it and not in it */
static void halo (CGContextRef g, CGPathRef p, float dy, float blur, unsigned int c, float a)
{
	CGFloat comp[4] = { CR (c), CG_ (c), CB (c), a };
	CGColorRef col = CGColorCreate (rgb_space, comp);
	CGContextSaveGState (g);
	CGContextSetShadowWithColor (g, CGSizeMake (0, dy), blur, col);
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

static void stroke_path (CGContextRef g, CGPathRef p, float lw, unsigned int c, float a)
{
	CGContextAddPath (g, p);
	CGContextSetLineWidth (g, lw);
	stroke_col (g, c, a);
	CGContextStrokePath (g);
}

static void fill_path (CGContextRef g, CGPathRef p, unsigned int c, float a)
{
	CGContextAddPath (g, p);
	fill_col (g, c, a);
	CGContextFillPath (g);
}

/* text in design units, (x, y) on the baseline; align 0 left, 1 centre,
 * 2 right; returns its width */
static float text (CGContextRef g, int face, float size, unsigned int c, float a,
		   float x, float y, const char *s, int align, float track)
{
	CFStringRef str;
	CFAttributedStringRef as;
	CTLineRef line;
	CTFontRef f;
	CFStringRef keys[3];
	CFTypeRef vals[3];
	CFDictionaryRef attr;
	CGColorRef col;
	CGFloat comp[4] = { CR (c), CG_ (c), CB (c), a };
	CFNumberRef kern;
	float w;

	if (!s || !*s) return 0;
	str = CFStringCreateWithCString (NULL, s, kCFStringEncodingUTF8);
	if (!str) return 0;
	f = font (face, size);
	col = CGColorCreate (rgb_space, comp);
	kern = CFNumberCreate (NULL, kCFNumberFloatType, &track);
	keys[0] = kCTFontAttributeName; vals[0] = f;
	keys[1] = kCTForegroundColorAttributeName; vals[1] = col;
	keys[2] = kCTKernAttributeName; vals[2] = kern;
	attr = CFDictionaryCreate (NULL, (const void **) keys, (const void **) vals, 3,
				   &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	as = CFAttributedStringCreate (NULL, str, attr);
	line = CTLineCreateWithAttributedString (as);
	w = (float) CTLineGetTypographicBounds (line, NULL, NULL, NULL);
	if (align == 1) x -= w / 2;
	else if (align == 2) x -= w;
	CGContextSetTextMatrix (g, CGAffineTransformMakeScale (1, -1));
	CGContextSetTextPosition (g, x, y);
	CTLineDraw (line, g);
	CFRelease (line);
	CFRelease (as);
	CFRelease (attr);
	CFRelease (kern);
	CGColorRelease (col);
	CFRelease (f);
	CFRelease (str);
	return w;
}

static float text_width (int face, float size, const char *s)
{
	CFStringRef str;
	CFAttributedStringRef as;
	CTLineRef line;
	CTFontRef f;
	CFStringRef keys[1];
	CFTypeRef vals[1];
	CFDictionaryRef attr;
	float w;

	if (!s || !*s) return 0;
	str = CFStringCreateWithCString (NULL, s, kCFStringEncodingUTF8);
	f = font (face, size);
	keys[0] = kCTFontAttributeName; vals[0] = f;
	attr = CFDictionaryCreate (NULL, (const void **) keys, (const void **) vals, 1,
				   &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	as = CFAttributedStringCreate (NULL, str, attr);
	line = CTLineCreateWithAttributedString (as);
	w = (float) CTLineGetTypographicBounds (line, NULL, NULL, NULL);
	CFRelease (line);
	CFRelease (as);
	CFRelease (attr);
	CFRelease (f);
	CFRelease (str);
	return w;
}

/* a frosted glass panel */
static void glass (CGContextRef g, float x, float y, float w, float h, float r, float a0, float a1)
{
	CGPathRef p = rrect (x, y, w, h, r);
	halo (g, p, 10, 40, 0, 0.55f);
	CGContextSaveGState (g);
	CGContextAddPath (g, p);
	CGContextClip (g);
	vgrad (g, y, y + h, C_GLASS, a0, C_GLASS, a1);
	vgrad (g, y, y + h * 0.3f, 0xffffff, 0.055f, 0xffffff, 0);
	CGContextRestoreGState (g);
	{
		CGPathRef in = rrect (x + 0.75f, y + 0.75f, w - 1.5f, h - 1.5f, r - 0.75f);
		stroke_path (g, in, 1.5f, 0xffffff, 0.14f);
		CGPathRelease (in);
	}
	CGPathRelease (p);
}

/* a button or binding cell: state 0 rest, 1 hover, 2 pressed or active */
static void cell (CGContextRef g, float x, float y, float w, float h, int state, int off, unsigned int edge)
{
	CGPathRef p = rrect (x, y, w, h, 6);
	CGContextSaveGState (g);
	CGContextAddPath (g, p);
	CGContextClip (g);
	vgrad (g, y, y + h, off ? C_CARD : C_CONTROL, off ? 0.45f : 0.92f, off ? C_CARD : C_CARD, off ? 0.40f : 0.88f);
	if (state == 2) {
		fill_col (g, edge, 0.22f);
		CGContextFillRect (g, CGRectMake (x, y, w, h));
	} else if (state == 1)
		vgrad (g, y, y + h, C_ACCENT, 0.16f, C_ACCENT, 0.06f);
	vgrad (g, y, y + h * 0.5f, 0xffffff, 0.06f, 0xffffff, 0);
	CGContextRestoreGState (g);
	if (state >= 1) {
		if (state == 2) halo (g, p, 0, 14, edge, 0.55f);
		stroke_path (g, p, 1.6f, edge, state == 2 ? 1.0f : 0.85f);
	} else
		stroke_path (g, p, 1, 0xffffff, off ? 0.06f : 0.12f);
	CGPathRelease (p);
}

static const char *key_text (int i)
{
	static char buf[32];
	const struct binding_action *a = &binding_actions[i];
	const char *n;
	if (a->kind == K_STEER || a->kind == K_CLICK) return "\xe2\x80\x94";	/* em dash */
	if (!binding_key[i]) return "\xe2\x80\x94";
	n = binding_key_name (binding_key[i]);
	if (n) return n;
	snprintf (buf, sizeof buf, "0x%02x", binding_key[i]);
	return buf;
}

static const char *mouse_text (int i)
{
	if (binding_actions[i].kind == K_CLICK) return "Left";
	if (!binding_mouse[i]) return "\xe2\x80\x94";
	return binding_mouse_name (binding_mouse[i]);
}

static const char *what (int i, int mouse, int value, char *buf, int n)
{
	(void) i;
	if (mouse) snprintf (buf, n, "%s button", binding_mouse_name (value));
	else {
		const char *k = binding_key_name (value);
		if (k) snprintf (buf, n, "the %s key", k);
		else snprintf (buf, n, "key 0x%02x", value);
	}
	return buf;
}

static const char *help_for (int id)
{
	static char buf[300];
	int i;
	if (cap_action >= 0)
		return cap_mouse ? "Press a mouse button for this action (the wheel counts). Esc cancels, Backspace or Delete clears."
				 : "Press a key for this action. Esc cancels, Backspace or Delete clears.";
	switch (id) {
	case H_TOGGLE:
		return "On: the left button fires the laser while it is held, but only in flight with the 3D view on the screen "
		       "and the pointer over it. Everywhere else, and with Shift held, it clicks as usual.";
	case H_RESET: return "Puts every key and button back to the game's defaults and saves that.";
	case H_CLOSE: return "Closes this page (Esc or F12 do the same).";
	case H_M_SWAP: return "Each action takes the other's binding.";
	case H_M_CLEAR: return "The other action loses this binding and is left without one.";
	case H_M_CANCEL: return "Leaves both bindings as they were.";
	case H_M_YES: return "The right button does this action and the ship has no steering button until you bind one.";
	}
	if (id >= H_ROW) {
		i = (id - H_ROW) / 3;
		if (i < 0 || i >= n_binding_actions) return NULL;
		if (id == H_KEY (i)) {
			if (binding_actions[i].kind == K_STEER) return "Steering needs a mouse button: it has no key.";
			if (binding_actions[i].kind == K_CLICK) return "Clicking is the mouse's left button only.";
			snprintf (buf, sizeof buf, "Click, then press the key for %s. %s", binding_actions[i].label,
				  binding_actions[i].help);
			return buf;
		}
		if (id == H_MOUSE (i)) {
			if (binding_actions[i].kind == K_CLICK) return binding_actions[i].help;
			snprintf (buf, sizeof buf, "Click, then press the mouse button for %s. %s", binding_actions[i].label,
				  binding_actions[i].help);
			return buf;
		}
		return binding_actions[i].help;
	}
	return NULL;
}

/* The layout, painted into g (design units, y down) and its hot spots
 * recorded; g may be NULL to lay out only */
static void paint (CGContextRef g)
{
	float px = 70, py = 46, pw = 1460, ph = 1108;
	float colx[2] = { 112, 828 }, colw = 660, rowh = 35, headh = 42;
	float y, cy[2];
	int grp, col, i;
	const char *help;
	static const int group_col[G_COUNT] = { 0, 0, 1, 0, 0, 1 };
	static const int group_order[G_COUNT] = { G_FLIGHT, G_WEAPONS, G_TIME, G_MAPS, G_VIEWS, G_OTHER };

	n_spots = 0;
	if (g) {
		/* the wash over the game's picture */
		vgrad (g, 0, DH, C_BG1, 0.80f, C_BG0, 0.88f);
		glass (g, px, py, pw, ph, 18, 0.90f, 0.84f);
		/* the header band */
		CGContextSaveGState (g);
		{
			CGPathRef p = rrect (px, py, pw, ph, 18);
			CGContextAddPath (g, p);
			CGContextClip (g);
			fill_col (g, 0xffffff, 0.03f);
			CGContextFillRect (g, CGRectMake (px, py, pw, 150));
			fill_col (g, 0xffffff, 0.10f);
			CGContextFillRect (g, CGRectMake (px, py + 150, pw, 1));
			CGPathRelease (p);
		}
		CGContextRestoreGState (g);
		text (g, F_TITLE, 44, C_TEXT, 1, 112, 116, "CONTROLS", 0, 4);
		text (g, F_SANS, 20, C_TEXT2_SEE, 1, 114, 160 - 10 + 20 - 18,
		      "Click a binding, then press a key or a mouse button.  Esc cancels, Backspace clears.  Changes save at once.", 0, 0);
	}

	/* the left-click switch, top right */
	{
		float tx = 1000, ty = 70, tw = 400, th = 56;
		int on = bindings_left_fires_laser ();
		int st = hover == H_TOGGLE ? (pressed == H_TOGGLE ? 2 : 1) : 0;
		spot (tx, ty, tx + tw, ty + th, H_TOGGLE);
		if (g) {
			float sx = tx + tw - 84, sy = ty + 12, sw = 66, sh = 32;
			CGPathRef p, k;
			cell (g, tx, ty, tw, th, st, 0, C_ACCENT);
			text (g, F_SANS_BOLD, 21, C_TEXT, 1, tx + 18, ty + 36, "Left click fires laser", 0, 0);
			text (g, F_COND, 17, on ? C_ACCENT : C_TEXT2_SEE, 1, sx - 12, ty + 35, on ? "ON" : "OFF", 2, 1.5f);
			p = rrect (sx, sy, sw, sh, sh / 2);
			if (on) {
				halo (g, p, 0, 12, C_ACCENT, 0.5f);
				fill_path (g, p, C_ACCENT, 0.95f);
			} else {
				fill_path (g, p, 0x0a0f15, 0.9f);
				stroke_path (g, p, 1.5f, C_TEXT2, 0.7f);
			}
			k = rrect (on ? sx + sw - sh + 4 : sx + 4, sy + 4, sh - 8, sh - 8, (sh - 8) / 2);
			fill_path (g, k, on ? 0x06222e : C_TEXT2_SEE, 1);
			CGPathRelease (k);
			CGPathRelease (p);
		}
	}
	/* close */
	{
		float cx = 1466, cyy = 98, r = 26;
		int st = hover == H_CLOSE ? (pressed == H_CLOSE ? 2 : 1) : 0;
		spot (cx - r, cyy - r, cx + r, cyy + r, H_CLOSE);
		if (g) {
			cell (g, cx - r, cyy - r, 2 * r, 2 * r, st, 0, C_ACCENT);
			CGContextSetLineWidth (g, 2.6f);
			CGContextSetLineCap (g, kCGLineCapRound);
			stroke_col (g, st ? 0x9fe8ff : C_TEXT, 1);
			CGContextMoveToPoint (g, cx - 9, cyy - 9); CGContextAddLineToPoint (g, cx + 9, cyy + 9);
			CGContextMoveToPoint (g, cx + 9, cyy - 9); CGContextAddLineToPoint (g, cx - 9, cyy + 9);
			CGContextStrokePath (g);
		}
	}

	/* the two columns of groups */
	cy[0] = cy[1] = 214;
	if (g) {
		for (col = 0; col < 2; col++) {
			text (g, F_COND, 15, C_TEXT2_SEE, 1, colx[col] + colw - 330 + 90, cy[col], "KEY", 1, 2);
			text (g, F_COND, 15, C_TEXT2_SEE, 1, colx[col] + colw - 70, cy[col], "MOUSE", 1, 2);
		}
	}
	cy[0] = cy[1] = 222;
	for (grp = 0; grp < G_COUNT; grp++) {
		int gg = group_order[grp];
		char up[64];
		int k;
		col = group_col[gg];
		y = cy[col];
		if (g) {
			const char *n = binding_group_name (gg);
			for (k = 0; n[k] && k < 63; k++) up[k] = (n[k] >= 'a' && n[k] <= 'z') ? n[k] - 32 : n[k];
			up[k] = 0;
			text (g, F_COND, 20, C_ACCENT, 1, colx[col] + 2, y + 30, up, 0, 2.5f);
			fill_col (g, C_ACCENT, 0.35f);
			CGContextFillRect (g, CGRectMake (colx[col] + 4 + text_width (F_COND, 20, up) + 10 * 2.5f, y + 23,
							 colw - 14 - text_width (F_COND, 20, up) - 25, 1));
		}
		y += headh;
		for (i = 0; i < n_binding_actions; i++) {
			const struct binding_action *a = &binding_actions[i];
			float kx = colx[col] + colw - 330, kw = 180, mx = colx[col] + colw - 140, mw = 140;
			int hov_row;
			if (a->group != gg) continue;
			spot (colx[col], y, kx - 6, y + rowh, H_LABEL (i));
			spot (kx, y + 2, kx + kw, y + rowh - 2, H_KEY (i));
			spot (mx, y + 2, mx + mw, y + rowh - 2, H_MOUSE (i));
			hov_row = hover == H_LABEL (i) || hover == H_KEY (i) || hover == H_MOUSE (i) || cap_action == i;
			if (g) {
				int koff = a->kind == K_STEER || a->kind == K_CLICK, moff = a->kind == K_CLICK;
				int kst = cap_action == i && !cap_mouse ? 2 : (!koff && hover == H_KEY (i)) ? 1 : 0;
				int mst = cap_action == i && cap_mouse ? 2 : (!moff && hover == H_MOUSE (i)) ? 1 : 0;
				int kdup = 0, mdup = 0;
				if (hov_row) {
					CGPathRef p = rrect (colx[col] - 8, y, colw + 16, rowh, 6);
					fill_path (g, p, C_ACCENT, 0.07f);
					CGPathRelease (p);
					fill_col (g, C_ACCENT, 0.9f);
					CGContextFillRect (g, CGRectMake (colx[col] - 8, y + 6, 3, rowh - 12));
				}
				text (g, F_SANS, 20, C_TEXT, 1, colx[col] + 8, y + 24, a->label, 0, 0);
				if (a->kind == K_KEY && binding_key[i] && binding_key[i] != a->code)
					kdup = 1;	/* changed from the default */
				if (binding_mouse[i] != a->def_mouse) mdup = 1;
				cell (g, kx, y + 3, kw, rowh - 6, kst, koff, C_ACCENT);
				cell (g, mx, y + 3, mw, rowh - 6, mst, moff, C_ACCENT);
				if (kst == 2)
					text (g, F_COND, 18, C_ACCENT, 1, kx + kw / 2, y + 24, "Press a key\xe2\x80\xa6", 1, 0.5f);
				else
					text (g, F_COND, 19, koff ? C_TEXT2 : kdup ? C_AMBER : C_TEXT, koff ? 0.6f : 1,
					      kx + kw / 2, y + 24, key_text (i), 1, 0.5f);
				if (mst == 2)
					text (g, F_COND, 18, C_ACCENT, 1, mx + mw / 2, y + 24, "Press a button\xe2\x80\xa6", 1, 0.3f);
				else
					text (g, F_COND, 19, moff ? C_TEXT2 : mdup ? C_AMBER : binding_mouse[i] ? C_TEXT : C_TEXT2,
					      moff ? 0.8f : 1, mx + mw / 2, y + 24, mouse_text (i), 1, 0.5f);
			}
			y += rowh;
		}
		cy[col] = y + 8;
	}

	/* the footer: help on the left, reset on the right */
	{
		float fy = 1062, bx = 1262, bw = 228, bh = 52;
		int st = hover == H_RESET ? (pressed == H_RESET ? 2 : 1) : 0;
		spot (bx, fy + 6, bx + bw, fy + 6 + bh, H_RESET);
		if (g) {
			fill_col (g, 0xffffff, 0.10f);
			CGContextFillRect (g, CGRectMake (px + 30, fy - 10, pw - 60, 1));
			cell (g, bx, fy + 6, bw, bh, st, 0, C_AMBER);
			text (g, F_SANS_BOLD, 20, st ? C_AMBER : C_TEXT, 1, bx + bw / 2, fy + 39, "Reset to defaults", 1, 0);
			help = help_for (hover);
			if (help) {
				/* an i in a ring, then the help, broken onto two lines if long */
				CGPathRef ring = rrect (112, fy + 8, 26, 26, 13);
				char line1[200], *line2 = NULL;
				stroke_path (g, ring, 1.6f, C_ACCENT, 0.9f);
				CGPathRelease (ring);
				text (g, F_SANS_BOLD, 17, C_ACCENT, 1, 125, fy + 28, "i", 1, 0);
				snprintf (line1, sizeof line1, "%s", help);
				if (text_width (F_SANS, 19, line1) > 1090) {
					int cut = (int) strlen (line1);
					while (cut > 0) {
						while (cut > 0 && line1[cut] != ' ') cut--;
						line1[cut] = 0;
						if (text_width (F_SANS, 19, line1) <= 1090) break;
						line1[cut] = ' ';
						cut--;
					}
					if (cut > 0) line2 = (char *) help + cut + 1;
					else snprintf (line1, sizeof line1, "%s", help);
				}
				text (g, F_SANS, 19, C_TEXT, 1, 150, fy + 28, line1, 0, 0);
				if (line2) text (g, F_SANS, 19, C_TEXT, 1, 150, fy + 54, line2, 0, 0);
			} else if (status[0]) {
				text (g, F_SANS, 19, status_warn ? C_AMBER : C_TEXT2_SEE, 1, 114, fy + 28, status, 0, 0);
			}
			if (paused_by_us && !help)
				text (g, F_SANS, 17, C_TEXT2_SEE, 1, 114, fy + 56,
				      "The game is paused while this page is open; closing it puts the clock back.", 0, 0);
		}
	}

	/* a question over the page */
	if (modal) {
		float mw = 720, mh = 250, mx = (DW - mw) / 2, my = (DH - mh) / 2 - 20;
		char a[80], b[200], c[200];
		const char *names[3];
		int ids[3], nb = 0, k;
		float bw = 200, gap = 16, bx;

		if (modal == M_CONFLICT) {
			what (m_action, m_mouse, m_value, a, sizeof a);
			snprintf (b, sizeof b, "%c%s is already %s.", a[0] >= 'a' && a[0] <= 'z' ? a[0] - 32 : a[0], a + 1,
				  binding_actions[m_other].label);
			snprintf (c, sizeof c, "Give it to %s?", binding_actions[m_action].label);
			names[nb] = "Swap"; ids[nb++] = H_M_SWAP;
			names[nb] = "Clear the other"; ids[nb++] = H_M_CLEAR;
			names[nb] = "Cancel"; ids[nb++] = H_M_CANCEL;
		} else {
			snprintf (b, sizeof b, "The right button steers the ship.");
			snprintf (c, sizeof c, "Bind it to %s? Steering then needs another button.", binding_actions[m_action].label);
			names[nb] = "Bind it anyway"; ids[nb++] = H_M_YES;
			names[nb] = "Cancel"; ids[nb++] = H_M_CANCEL;
		}
		bx = mx + mw - 36 - nb * bw - (nb - 1) * gap;
		for (k = 0; k < nb; k++)
			spot (bx + k * (bw + gap), my + mh - 80, bx + k * (bw + gap) + bw, my + mh - 30, ids[k]);
		if (g) {
			fill_col (g, C_BG0, 0.55f);
			CGContextFillRect (g, CGRectMake (0, 0, DW, DH));
			glass (g, mx, my, mw, mh, 14, 0.96f, 0.92f);
			{
				CGPathRef bar = rrect (mx, my, 6, mh, 3);
				fill_path (g, bar, C_AMBER, 0.95f);
				CGPathRelease (bar);
			}
			text (g, F_COND, 20, C_AMBER, 1, mx + 36, my + 48, modal == M_CONFLICT ? "ALREADY IN USE" : "STEERING BUTTON", 0, 2.5f);
			text (g, F_SANS_BOLD, 23, C_TEXT, 1, mx + 36, my + 92, b, 0, 0);
			text (g, F_SANS, 20, C_TEXT2_SEE, 1, mx + 36, my + 126, c, 0, 0);
			for (k = 0; k < nb; k++) {
				float x0 = bx + k * (bw + gap);
				int st = hover == ids[k] ? (pressed == ids[k] ? 2 : 1) : 0;
				unsigned int edge = ids[k] == H_M_CANCEL ? C_ACCENT : C_AMBER;
				cell (g, x0, my + mh - 80, bw, 50, st, 0, edge);
				text (g, F_SANS_BOLD, 19, st ? edge : C_TEXT, 1, x0 + bw / 2, my + mh - 48, names[k], 1, 0);
			}
		}
	}
}

/* ---------------------------------------------------------------- */
/* The texture */

static GLuint tex;
static int tex_w, tex_h, pic_w, pic_h;

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

static void render (int W, int H)
{
	unsigned char *buf;
	CGContextRef g;

	load_faces ();
	if (!rgb_space) rgb_space = CGColorSpaceCreateDeviceRGB ();
	buf = calloc ((size_t) W * H, 4);
	if (!buf) return;
	g = CGBitmapContextCreate (buf, W, H, 8, W * 4, rgb_space,
				   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	if (!g) {
		free (buf);
		return;
	}
	CGContextTranslateCTM (g, 0, H);
	CGContextScaleCTM (g, W / (float) DW, -H / (float) DH);
	CGContextSetShouldAntialias (g, true);
	CGContextSetShouldSmoothFonts (g, false);
	{
		Uint32 t0 = SDL_GetTicks ();
		paint (g);
		if (dbg > 0) fprintf (stderr, "controls: painted %dx%d in %u ms\n", W, H, SDL_GetTicks () - t0);
	}
	CGContextRelease (g);

	if (!tex || tex_w < W || tex_h < H) {
		if (tex) glDeleteTextures (1, &tex);
		glGenTextures (1, &tex);
		tex_w = pow2 (W);
		tex_h = pow2 (H);
		glBindTexture (GL_TEXTURE_2D, tex);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	glBindTexture (GL_TEXTURE_2D, tex);
	glPushClientAttrib (GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glPopClientAttrib ();
	free (buf);
	pic_w = W;
	pic_h = H;
}

/* the game menu's way in: a small glass pill, top right of the picture */
#define EP_X0	1270
#define EP_Y0	24
#define EP_X1	1576
#define EP_Y1	84
static GLuint entry_tex;
static int entry_tw, entry_th, entry_w, entry_h, entry_state = -1;

static void render_entry (int W, int H, int st)
{
	unsigned char *buf;
	CGContextRef g;
	float k = H / (float) DH;
	int w = (int) ceilf ((EP_X1 - EP_X0) * W / (float) DW), h = (int) ceilf ((EP_Y1 - EP_Y0) * k);

	load_faces ();
	if (!rgb_space) rgb_space = CGColorSpaceCreateDeviceRGB ();
	buf = calloc ((size_t) w * h, 4);
	if (!buf) return;
	g = CGBitmapContextCreate (buf, w, h, 8, w * 4, rgb_space,
				   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	if (g) {
		float ew = EP_X1 - EP_X0, eh = EP_Y1 - EP_Y0;
		CGPathRef p;
		CGContextTranslateCTM (g, 0, h);
		CGContextScaleCTM (g, W / (float) DW, -k);
		p = rrect (2, 2, ew - 4, eh - 4, (eh - 4) / 2);
		CGContextSaveGState (g);
		CGContextAddPath (g, p);
		CGContextClip (g);
		vgrad (g, 0, eh, C_GLASS, 0.92f, C_CARD, 0.88f);
		if (st) vgrad (g, 0, eh, C_ACCENT, st == 2 ? 0.30f : 0.18f, C_ACCENT, 0.06f);
		vgrad (g, 0, eh * 0.5f, 0xffffff, 0.07f, 0xffffff, 0);
		CGContextRestoreGState (g);
		stroke_path (g, p, 1.6f, st ? C_ACCENT : 0xffffff, st ? 0.95f : 0.22f);
		CGPathRelease (p);
		/* a little keyboard: a rounded box and keys */
		{
			CGPathRef kb = rrect (26, 18, 40, 24, 4);
			int r, c2;
			stroke_path (g, kb, 2, C_ACCENT, 1);
			CGPathRelease (kb);
			fill_col (g, C_ACCENT, 1);
			for (r = 0; r < 2; r++)
				for (c2 = 0; c2 < 5; c2++)
					CGContextFillRect (g, CGRectMake (31 + c2 * 6.6f, 23 + r * 6, 3.6f, 3));
			CGContextFillRect (g, CGRectMake (36, 35, 20, 3));
		}
		text (g, F_SANS_BOLD, 22, C_TEXT, 1, 80, 39, "Controls", 0, 0.3f);
		text (g, F_COND, 17, C_TEXT2_SEE, 1, ew - 26, 38, "F12", 2, 1.5f);
		CGContextRelease (g);
	}
	if (!entry_tex || entry_tw < w || entry_th < h) {
		if (entry_tex) glDeleteTextures (1, &entry_tex);
		glGenTextures (1, &entry_tex);
		entry_tw = pow2 (w);
		entry_th = pow2 (h);
		glBindTexture (GL_TEXTURE_2D, entry_tex);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, entry_tw, entry_th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	glBindTexture (GL_TEXTURE_2D, entry_tex);
	glPushClientAttrib (GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glPopClientAttrib ();
	free (buf);
	entry_w = w;
	entry_h = h;
	entry_state = st;
}

#endif /* __APPLE__ */

/* ---------------------------------------------------------------- */
/* Behaviour */

static void set_status (int warn, const char *fmt, const char *a, const char *b)
{
	snprintf (status, sizeof status, fmt, a ? a : "", b ? b : "");
	status_warn = warn;
}

static void changed (void)
{
	bindings_changed ();
	if (bindings_save ()) {
		if (!status_warn || !status[0]) set_status (0, "Saved to controls.ini.", NULL, NULL);
	} else
		set_status (1, "Could not write controls.ini: the change lasts until the game quits.", NULL, NULL);
	dirty = 1;
}

static int get (int i, int mouse) { return mouse ? binding_mouse[i] : binding_key[i]; }
static void put (int i, int mouse, int v)
{
	if (mouse) binding_mouse[i] = v;
	else binding_key[i] = v;
}

/* bind value to action i's key or button, asking first when it is taken */
static void assign (int i, int mouse, int value)
{
	int other;
	status[0] = 0;
	status_warn = 0;
	cap_action = -1;
	dirty = 1;
	if (get (i, mouse) == value) return;
	if (mouse && value == MB_LEFT && binding_actions[i].kind == K_STEER) {
		set_status (1, "The left button stays the click button: steering cannot use it.", NULL, NULL);
		return;
	}
	if (mouse && value == MB_RIGHT && binding_actions[i].kind != K_STEER && modal != M_RIGHT) {
		modal = M_RIGHT;
		m_action = i;
		m_mouse = 1;
		m_value = value;
		return;
	}
	other = mouse ? bindings_mouse_owner (value, i) : bindings_key_owner (value, i);
	if (other >= 0 && binding_actions[other].kind == K_CLICK) other = -1;
	if (other >= 0 && !(modal == M_RIGHT && binding_actions[other].kind == K_STEER)) {
		modal = M_CONFLICT;
		m_action = i;
		m_mouse = mouse;
		m_value = value;
		m_other = other;
		return;
	}
	if (other >= 0) put (other, mouse, 0);	/* steering gives the right button up */
	modal = M_NONE;
	put (i, mouse, value);
	changed ();
}

static void resolve (int id)
{
	int old;
	switch (id) {
	case H_M_SWAP:
		old = get (m_action, m_mouse);
		put (m_action, m_mouse, m_value);
		/* the steering can not take the left button */
		if (m_mouse && old == MB_LEFT && binding_actions[m_other].kind == K_STEER) old = 0;
		put (m_other, m_mouse, old);
		modal = M_NONE;
		changed ();
		break;
	case H_M_CLEAR:
		put (m_other, m_mouse, 0);
		put (m_action, m_mouse, m_value);
		modal = M_NONE;
		changed ();
		break;
	case H_M_YES:
		assign (m_action, m_mouse, m_value);	/* modal is still M_RIGHT: no second warning */
		if (modal == M_RIGHT) modal = M_NONE;
		break;
	case H_M_CANCEL:
		modal = M_NONE;
		dirty = 1;
		break;
	}
}

static void st_point_to_window (int sx, int sy, int *wx, int *wy)
{
	*wx = view_w > 0 ? view_x + (sx * view_w + view_w / 2) / 320 : sx * screen_w / 320;
	*wy = view_h > 0 ? view_y + (sy * view_h + view_h / 2) / 200 : sy * screen_h / 200;
}

static void open_page (void)
{
	if (is_open) return;
	if (dbg < 0) {
		const char *d = getenv ("FE2_CONTROLS_DEBUG");
		dbg = d && *d && *d != '0';
	}
	bindings_release_all ();
	is_open = 1;
	dirty = 1;
	hover = pressed = cap_action = -1;
	modal = M_NONE;
	status[0] = 0;
	paused_by_us = 0;
	rate_before = rate_index ();
	if (dbg)
		fprintf (stderr, "controls: open state=%02x rate=%lx menu=%02x paused16470=%02x view42=%02x clr536=%02x\n",
			 rb (704), rl (10428), rb (10469), rb (16470), rb (42), rb (536));
	/* a running game in flight: pause it the way Esc does */
	if (flying () && rate_before > 0) {
		/* held for a few frames, as a real key press is */
		Input_PressSTKey (0x01, TRUE);
		esc_frames = 4;
		paused_by_us = 1;
	}
	SDL_WM_GrabInput (SDL_GRAB_OFF);
}

static void close_page (void)
{
	if (!is_open) return;
	is_open = 0;
	cap_action = -1;
	modal = M_NONE;
	if (esc_frames > 0) {
		Input_PressSTKey (0x01, FALSE);
		esc_frames = 0;
	}
	if (dbg)
		fprintf (stderr, "controls: close rate=%lx (was index %d, paused by us %d)\n", rl (10428), rate_before,
			 paused_by_us);
	/* put the clock back: click the time control it was on, if the game
	 * is still stopped */
	if (paused_by_us && rate_before > 0 && rl (10428) == 0) {
		int wx, wy;
		resume_m = rate_before;
		resume_saved_x = input.abs_x;
		resume_saved_y = input.abs_y;
		st_point_to_window (5 + 10 * resume_m, 181, &wx, &wy);
		input.abs_x = wx;
		input.abs_y = wy;
		Input_MousePress (SDL_BUTTON_LEFT);
		resume_frames = 3;
	}
	paused_by_us = 0;
}

void controls_page_toggle (void)
{
	if (is_open) close_page ();
	else open_page ();
}

int controls_page_open (void)
{
	return is_open;
}

int controls_page_key (SDL_keysym *k, int down)
{
	int sym = k->sym;
	if (!down) return 1;
	if (cap_action >= 0) {
		if (sym == SDLK_ESCAPE) {
			cap_action = -1;
			dirty = 1;
		} else if (sym == SDLK_BACKSPACE || sym == SDLK_DELETE) {
			int i = cap_action;
			cap_action = -1;
			put (i, cap_mouse, 0);
			changed ();
		} else if (!cap_mouse) {
			int code = Keymap_RemapKeyToSTScanCode (k);
			if (code > 0 && code < 128) assign (cap_action, 0, code);
			else {
				set_status (1, "That key can not reach the game; try another.", NULL, NULL);
				dirty = 1;
			}
		}
		return 1;
	}
	if (modal) {
		if (sym == SDLK_ESCAPE) resolve (H_M_CANCEL);
		return 1;
	}
	if (sym == SDLK_ESCAPE || sym == SDLK_F12) close_page ();
	return 1;
}

static void click (int id)
{
	int i;
	if (modal) {
		if (id == H_M_SWAP || id == H_M_CLEAR || id == H_M_CANCEL || id == H_M_YES) resolve (id);
		return;
	}
	switch (id) {
	case H_TOGGLE:
		bindings_set_left_fires_laser (!bindings_left_fires_laser ());
		status[0] = 0;
		changed ();
		return;
	case H_RESET:
		bindings_reset ();
		status[0] = 0;
		changed ();
		set_status (0, "Every control is back to the default (saved).", NULL, NULL);
		return;
	case H_CLOSE:
		close_page ();
		return;
	}
	if (id >= H_ROW) {
		i = (id - H_ROW) / 3;
		if (i >= n_binding_actions) return;
		if (id == H_KEY (i) && binding_actions[i].kind != K_STEER && binding_actions[i].kind != K_CLICK) {
			cap_action = i;
			cap_mouse = 0;
			status[0] = 0;
			dirty = 1;
		} else if (id == H_MOUSE (i) && binding_actions[i].kind != K_CLICK) {
			cap_action = i;
			cap_mouse = 1;
			status[0] = 0;
			dirty = 1;
		}
	}
}

int controls_page_mouse (int button, int down)
{
	float x, y;

	if (!is_open) {
		/* the game menu's Controls button */
		if (button != SDL_BUTTON_LEFT) return 0;
		if (down && entry_shown && entry_hover) {
			entry_pressed = 1;
			return 1;
		}
		if (!down && entry_pressed) {
			entry_pressed = 0;
			open_page ();
			return 1;
		}
		return 0;
	}
	pointer (&x, &y);
	if (cap_action >= 0 && cap_mouse) {
		/* this press is the binding */
		if (down && button > 0 && button < MB_COUNT) {
			int i = cap_action;
			assign (i, 1, button);
			pressed = -2;	/* swallow the release */
		}
		return 1;
	}
	if (button != SDL_BUTTON_LEFT) return 1;
	if (down) {
		if (cap_action >= 0) {
			/* a click while waiting for a key: stop waiting */
			cap_action = -1;
			dirty = 1;
		}
		pressed = spot_at (x, y);
		dirty = 1;
	} else {
		int id = spot_at (x, y);
		if (pressed >= 0 && id == pressed) click (id);
		pressed = -1;
		dirty = 1;
	}
	return 1;
}

#ifdef __APPLE__
static void quad (GLuint t, int tw, int th, int w, int h, float x0, float y0, float x1, float y1)
{
	float u1 = w / (float) tw, v1 = h / (float) th;
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, t);
	glColor4f (1, 1, 1, 1);
	glBegin (GL_TRIANGLE_STRIP);
	glTexCoord2f (0, 0); glVertex2f (x0, y0);
	glTexCoord2f (u1, 0); glVertex2f (x1, y0);
	glTexCoord2f (0, v1); glVertex2f (x0, y1);
	glTexCoord2f (u1, v1); glVertex2f (x1, y1);
	glEnd ();
	glDisable (GL_TEXTURE_2D);
}
#endif

void controls_page_draw (void)
{
#ifdef __APPLE__
	int fx, fy, fw, fh, W, H;
	float x, y;

	/* the Esc that paused the game: let go after a few frames, and see
	 * whether it did */
	if (esc_frames > 0 && --esc_frames == 0) {
		Input_PressSTKey (0x01, FALSE);
		if (dbg) fprintf (stderr, "controls: paused, rate now %lx\n", rl (10428));
	}
	/* the click that put the clock back: let go after a few frames */
	if (resume_frames > 0 && --resume_frames == 0) {
		Input_MouseRelease (SDL_BUTTON_LEFT);
		input.abs_x = resume_saved_x;
		input.abs_y = resume_saved_y;
		if (dbg) fprintf (stderr, "controls: resumed to index %d, rate now %lx\n", resume_m, rl (10428));
		resume_m = -1;
	}

	if (dbg < 0) {
		const char *d = getenv ("FE2_CONTROLS_DEBUG");
		dbg = d && *d && *d != '0';
	}
	if (dbg) {
		static int last = -1;
		int now = rb (10469) | rb (704) << 8 | (rl (10428) == 0) << 16 | (rb (42) & 0x80) << 17 | (rb (536) & 1) << 25;
		if (now != last)
			fprintf (stderr, "controls: state=%02x menu10469=%02x stopped=%d view42=%02x clr536=%02x\n",
				 rb (704), rb (10469), rl (10428) == 0, rb (42), rb (536));
		last = now;
	}
	entry_shown = !is_open && game_menu_up ();
	if (!is_open && !entry_shown) return;

	gl_picture_rect (&fx, &fy, &fw, &fh);
	if (fw < 32 || fh < 32) return;
	pointer (&x, &y);

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT | GL_TEXTURE_BIT |
		      GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT | GL_POLYGON_BIT | GL_SCISSOR_BIT);
	glViewport (fx, fy, fw, fh);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, fw, fh, 0, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glDisable (GL_LIGHTING);
	glDisable (GL_ALPHA_TEST);
	glDisable (GL_FOG);
	glDisable (GL_SCISSOR_TEST);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);	/* premultiplied */
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	if (is_open) {
		W = fw;
		H = fh;
		if (W > 2400) {	/* past this the page is sharp enough scaled */
			H = H * 2400 / W;
			W = 2400;
		}
		/* what the pointer is on (the spots of the last layout) */
		{
			int id = spot_at (x, y);
			if (modal && (id < H_M_SWAP || id >= H_ROW)) id = -1;
			if (id != hover) {
				hover = id;
				dirty = 1;
			}
		}
		if (dirty || pic_w != W || pic_h != H || !tex) {
			render (W, H);
			dirty = 0;
		}
		quad (tex, tex_w, tex_h, pic_w, pic_h, 0, 0, fw, fh);
		SDL_ShowCursor (SDL_ENABLE);
	} else {
		int st;
		entry_hover = x >= EP_X0 && x < EP_X1 && y >= EP_Y0 && y < EP_Y1;
		st = entry_hover ? (entry_pressed ? 2 : 1) : 0;
		W = fw;
		H = fh;
		if (st != entry_state || !entry_tex || entry_h != (int) ceilf ((EP_Y1 - EP_Y0) * H / (float) DH))
			render_entry (W, H, st);
		quad (entry_tex, entry_tw, entry_th, entry_w, entry_h,
		      EP_X0 * fw / (float) DW, EP_Y0 * fh / (float) DH,
		      EP_X0 * fw / (float) DW + entry_w, EP_Y0 * fh / (float) DH + entry_h);
	}

	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
#endif
}

/* the GL context is going away */
void controls_page_forget (void)
{
#ifdef __APPLE__
	if (tex) glDeleteTextures (1, &tex);
	if (entry_tex) glDeleteTextures (1, &entry_tex);
	tex = entry_tex = 0;
	tex_w = tex_h = entry_tw = entry_th = 0;
	pic_w = pic_h = 0;
	entry_state = -1;
#endif
}
