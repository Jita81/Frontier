/*
 * Hover help for the HD control panel (see hdtip.h).
 *
 * Each frame: where the pointer is on the game's 320x200 screen, which
 * part of the panel that is (the game's own click table first, then the
 * displays and gauges it has no click for), and, once the pointer has
 * rested on the same part for 0.4 s, a glass label above it: the part's
 * name, a line on what it does, the key that does the same (looked up in
 * the game's key map, so it follows the map, never typed in here) and,
 * when the game would refuse it now, why, from the same state the
 * game's own code tests.  Where the state cannot tell for sure, the
 * label says only what the part is.
 *
 * Nothing here writes to ST memory: the oracle hashes it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <ApplicationServices/ApplicationServices.h>
#else
#include <GL/gl.h>
#endif

#include "main.h"
#include "hdtip.h"
#include "hdui.h"
#include "hdstill.h"
#include "hdback.h"
#include "screen.h"
#include "input.h"
#include "../m68000.h"
#include "../fixups.h"

/* ---------------------------------------------------------------- */
/* The game's state, read only */

#define A6			__DL5eb6_a6_base
#define A6_TOOLS_MODE		42	/* byte: negative while a 3D view fills the screen */
#define A6_MISSILE_SEL		671	/* byte: the pylon selected, 1..8, 0 none */
#define A6_FLAGS673		673	/* byte: bit 6 far enough from any body to jump */
#define A6_PLAYER		674	/* long: the player's ship object */
#define A6_NEAREST_M		692	/* long: metres to the nearest body */
#define A6_FLIGHT_STATE		704	/* byte: 0 flight, 4 autopilot, 8 hyperspace, $48/$4c launch, $50.. docked, $5c.. landed */
#define A6_SHIP_FLAGS		705	/* byte: bit 0 engines off */
#define A6_VIEW			706	/* byte: 0 front, 1 rear, 2/3 turrets, 4 external */
#define A6_AP_TARGET		708	/* byte: something the autopilot can fly to */
#define A6_HULL_TEMP		866	/* word */
#define A6_DEST_SET		977	/* byte: a destination system picked */
#define A6_DEST_SYS		1044	/* long: the system picked, 0 the one you are in */
#define A6_COMBAT		10476	/* byte: bit 1 a ship nearby is active (time keys refused) */
#define A6_ICON_POS		14514	/* the click table: x0, y0, x1, y1, code words */

#define OBJ_EQUIP		158	/* long: bit 5 an autopilot fitted */
#define OBJ_LASER_TEMP		190	/* word: firing refused over $f000 */
#define OBJ_STATE		211	/* byte: $84 docked */
#define OBJ_FUEL		242	/* long: the fuel in the unit being burnt */

#define KEYMAP_LOWER		__DL41bd6_keymap_lower	/* 128 bytes by ST scan code */
#define KEYMAP_UPPER		__DL41c56_keymap_upper	/* with Shift */

static int rb (p68K a) { return STMemory_ReadByte (a) & 0xff; }
static int rw (p68K a) { return STMemory_ReadWord (a) & 0xffff; }
static unsigned int rl (p68K a) { return (unsigned int) STMemory_ReadLong (a); }

static int in_3d (void) { return (signed char) rb (A6 + A6_TOOLS_MODE) < 0; }
static unsigned int player (void) { return rl (A6 + A6_PLAYER); }

/* ---------------------------------------------------------------- */
/* Keys, from the game's key map */

static const char *scan_name (int sc)
{
	static const char *const fkey[10] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10" };
	if (sc >= 0x3b && sc <= 0x44) return fkey[sc - 0x3b];
	switch (sc) {
	case 0x01: return "Esc";
	case 0x0e: return "Backspace";
	case 0x0f: return "Tab";
	case 0x1c: return "Return";
	case 0x39: return "Space";
	case 0x48: return "Up";
	case 0x50: return "Down";
	case 0x4b: return "Left";
	case 0x4d: return "Right";
	case 0x52: return "Insert";
	case 0x53: return "Delete";
	case 0x61: return "Undo";
	case 0x62: return "Help";
	}
	return NULL;
}

/* The key that gives code, "" when the map has none */
static void key_for (int code, char *out, size_t n)
{
	int pass, sc;

	out[0] = 0;
	for (pass = 0; pass < 2; pass++) {
		p68K map = pass ? KEYMAP_UPPER : KEYMAP_LOWER;
		for (sc = 1; sc < 128; sc++) {
			const char *name;
			char ch[2];
			if (rb (map + sc) != code) continue;
			name = scan_name (sc);
			if (!name) {
				int c = rb (KEYMAP_LOWER + sc);
				if (c <= 0x20 || c >= 0x7f) continue;
				ch[0] = (c >= 'a' && c <= 'z') ? c - 32 : c;
				ch[1] = 0;
				name = ch;
			}
			snprintf (out, n, "%s%s", pass ? "Shift+" : "", name);
			return;
		}
	}
}

/* ---------------------------------------------------------------- */
/* What the pointer is on */

struct tip {
	int id;			/* 0 nothing */
	int x0, y0, x1, y1;	/* the part, ST pixels, inclusive */
	int code;		/* the game's click code, -1 none */
	char title[64], key[24], body[200], why[200];
};

enum {
	T_NONE, T_HIT, T_SLAT_FUEL, T_SLAT_LASER, T_SLAT_HULL, T_STATUS, T_VIEWNAME,
	T_CLOCK, T_ACTUAL, T_SET, T_RELATIVE
};

static const char *view_name (int v)
{
	switch (v) {
	case 0: return "front";
	case 1: return "rear";
	case 2: case 3: return "turret";
	case 4: return "external";
	}
	return NULL;
}

/* The game's click table: the last entry under (x, y) wins, as in the game */
static int hit_code (int x, int y, int *r)
{
	int i, code = -1;

	for (i = 0; i < 96; i++) {
		p68K e = A6 + A6_ICON_POS + 10 * i;
		short x0 = (short) rw (e), y0 = (short) rw (e + 2);
		short x1 = (short) rw (e + 4), y1 = (short) rw (e + 6);
		if (x0 < 0) break;
		if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
			code = rw (e + 8) & 0xff;
			r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
		}
	}
	return code;
}

static void set (char *d, size_t n, const char *s) { snprintf (d, n, "%s", s); }
#define SET(f, s)	set (t->f, sizeof (t->f), s)

static const char *glyph_title (int g)
{
	switch (g & ~G_OFF) {
	case G_FRONT: case G_REAR: case G_EXT: return "Views";
	case G_GALMAP: return "Galaxy map";
	case G_SYSMAP: return "System map";
	case G_SHIP: return "Ship and commander";
	case G_CARGO: return "Cargo";
	case G_CMDR: return "Commander";
	case G_PHONE: return "Communications";
	case G_TARGET: return "Zoom in";
	case G_CROSS: return "Zoom out";
	case G_WRENCH: return "Map display options";
	case G_LOAD: return "Load game";
	case G_SAVE: return "Save game";
	case G_EXIT: return "Leave the game";
	}
	return NULL;
}

/* F7..F10 over a 3D view, from the state the game's handlers test */
static void flight_key (struct tip *t, int slot, const struct hdstill_panel *p)
{
	int st = rb (A6 + A6_FLIGHT_STATE), fl = rb (A6 + A6_SHIP_FLAGS);
	unsigned int pl = player ();
	int docked = st >= 0x50 && st < 0x5c, landed = st >= 0x5c;

	switch (slot) {
	case 7:
		if (docked) {
			SET (title, "Engines");
			SET (body, "Start the engines and fly.");
			char k[24];
			key_for (0xf4, k, sizeof (k));
			snprintf (t->why, sizeof (t->why), "Docked: ask the station for launch clearance on Communications%s%s%s first.",
				  k[0] ? " (" : "", k, k[0] ? ")" : "");
		} else if (landed) {
			SET (title, "Take off");
			SET (body, "Lift off from the surface.");
		} else if (st == 0x48 || st == 0x4c) {
			SET (title, "Engines");
			SET (body, "Launching: the station is flying you out.");
		} else if (st == 4) {
			SET (title, "Manual control");
			SET (body, "Switch the autopilot off and fly by hand.");
		} else if (!(fl & 1)) {
			SET (title, "Engines off");
			SET (body, "Cut the engines and drift.");
		} else if (rb (A6 + A6_AP_TARGET) && (rl (pl + OBJ_EQUIP) & 0x20)) {
			SET (title, "Autopilot");
			SET (body, "Fly to the selected target automatically.");
		} else {
			SET (title, "Engines on");
			SET (body, "Start the engines again.");
		}
		break;
	case 8:
		SET (title, "Hyperspace");
		SET (body, "Jump to the system picked on the galaxy map.");
		if (p->valid && (p->slot[8] & ~G_OFF) == G_HYPER) break;
		if (docked)
			SET (why, "Not while docked: launch first.");
		else if (landed)
			SET (why, "Not while landed: take off first.");
		else if (st == 8)
			SET (why, "Already in hyperspace.");
		else if (st > 0x0c)
			SET (why, "Only in open flight: wait until you are clear of the station.");
		else if (!rb (A6 + A6_DEST_SET)) {
			char k[24];
			key_for (0xf2, k, sizeof (k));
			snprintf (t->why, sizeof (t->why), "No destination: pick a system on the galaxy map%s%s%s.",
				  k[0] ? " (" : "", k, k[0] ? ")" : "");
		}
		else if (!(rb (A6 + A6_FLAGS673) & 0x40))
			SET (why, "Too close to a nearby body: the drive needs 12,000 km clear.");
		else if (!rl (A6 + A6_DEST_SYS))
			SET (why, "The system picked is the one you are in.");
		else
			SET (why, "The hyperdrive cannot make this jump: out of range or not enough fuel.");
		break;
	case 9:
		SET (title, "Undercarriage");
		SET (body, "Lower or raise the landing gear.");
		if (rb (pl + OBJ_STATE) == 0x84) SET (why, "Not while docked.");
		break;
	case 10:
		SET (title, "Object labels");
		SET (body, "Show or hide the names beside objects in view.");
		break;
	}
}

static void slot_tip (struct tip *t, int slot, const struct hdstill_panel *p)
{
	int g = p->valid ? p->slot[slot] : G_ABSENT;
	const char *name;

	if (slot >= 7 && in_3d ()) {
		flight_key (t, slot, p);
		return;
	}
	switch (slot) {
	case 1:
		SET (title, "Views");
		if (in_3d () && view_name (rb (A6 + A6_VIEW)))
			snprintf (t->body, sizeof (t->body), "Change the view: now the %s view.",
				  view_name (rb (A6 + A6_VIEW)));
		else
			SET (body, "Back to the view from the ship; again to change it.");
		return;
	case 2:
		SET (title, (g & ~G_OFF) == G_SYSMAP ? "System map" : "Maps");
		SET (body, "The galaxy and system maps; again to switch between them.");
		return;
	case 3:
		SET (title, "Ship and commander");
		SET (body, "Your ship, its equipment and cargo, and your record.");
		return;
	case 4:
		SET (title, "Communications");
		SET (body, "Talk to stations and ships: launch and docking clearance, messages.");
		return;
	}
	name = glyph_title (g);
	if (name) {
		SET (title, name);
		if (g & G_OFF) SET (why, "Not available on this screen now.");
	}
}

static void time_tip (struct tip *t, int code)
{
	static const char *const name[] = { "Normal time", "Time x10", "Time x100", "Time x1,000", "Time x10,000" };
	int st = rb (A6 + A6_FLIGHT_STATE);

	if (code == 0xfe) {
		SET (title, "Pause");
		SET (body, "Stop the clock.");
		return;
	}
	SET (title, name[code - 0xc1]);
	if (code == 0xc1)
		SET (body, "Run the game at normal speed.");
	else
		snprintf (t->body, sizeof (t->body), "Run the clock %s times faster.",
			  name[code - 0xc1] + 6);
	if (code >= 0xc2 && st != 8 && (rb (A6 + A6_COMBAT) & 2))
		SET (why, "Not now: a ship nearby is active, so time cannot be sped up.");
}

static int find (int x, int y, struct tip *t)
{
	struct hdstill_panel p;
	int r[4], code;

	memset (t, 0, sizeof (*t));
	t->code = -1;
	hdstill_panel (&p);
	code = hit_code (x, y, r);

	/* the speed readouts along the 3D view's bottom */
	if (y >= 156 && y < 168 && code < 0 && in_3d ()) {
		t->y0 = 156, t->y1 = 167;
		if (x < 90) {
			t->id = T_ACTUAL, t->x0 = 0, t->x1 = 89;
			SET (title, "Actual speed");
			SET (body, "How fast you are moving, relative to the body named on the right.");
		} else if (x < 200) {
			t->id = T_SET, t->x0 = 90, t->x1 = 199;
			SET (title, "Set speed");
			SET (body, "The speed the engines hold you at.");
		} else {
			t->id = T_RELATIVE, t->x0 = 200, t->x1 = 319;
			SET (title, "Relative to");
			SET (body, "The body your speed is measured against: the nearest large one.");
		}
		return t->id;
	}
	if (y < 168) return 0;

	/* the gauges: three slats slanting down to the right, 5 pixels apart,
	 * picked by where the pointer's slant line meets the top row */
	if (p.valid && x >= 235 && x <= 264 && y >= 169 && y <= 178 &&
	    x - (y - 169.5f) * 1.75f >= 235 && x - (y - 169.5f) * 1.75f <= 252) {
		float top = x - (y - 169.5f) * 1.75f;
		unsigned int pl = player ();
		int ch = top < 241 ? 1 : top < 246 ? 2 : 3;
		t->x0 = 235, t->y0 = 169, t->x1 = 264, t->y1 = 178;
		if (ch == 1) {
			t->id = T_SLAT_FUEL;
			SET (title, "Fuel gauge");
			SET (body, "Fuel left in the unit being burnt; a new unit is used when it runs out.");
		} else if (ch == 2) {
			t->id = T_SLAT_LASER;
			SET (title, "Laser temperature");
			SET (body, "Rises as the lasers fire.");
			if (pl && rw (pl + OBJ_LASER_TEMP) > 0xf000)
				SET (why, "Too hot to fire: let the lasers cool.");
			else if (pl && rl (pl + OBJ_FUEL) == 0)
				SET (why, "No fuel in the drive: the lasers cannot fire.");
		} else {
			t->id = T_SLAT_HULL;
			SET (title, "Hull temperature");
			SET (body, "Rises close to stars and planets; keep it out of the top.");
		}
		return t->id;
	}
	/* the two displays on the right */
	if (p.valid && x >= 264 && y >= 169 && y <= 177) {
		int st = rb (A6 + A6_FLIGHT_STATE);
		t->id = T_STATUS, t->x0 = 264, t->y0 = 169, t->x1 = 319, t->y1 = 177;
		SET (title, "Flight status");
		SET (body, st == 4 ? "The autopilot is flying the ship." :
			   st >= 0x50 && st < 0x5c ? "Docked at a station." :
			   st >= 0x5c ? "On the ground." :
			   st == 8 ? "In hyperspace." :
			   st == 0x48 || st == 0x4c ? "Launching." :
			   "Who is flying the ship, and how.");
		return t->id;
	}
	if (p.valid && !(p.flags & F_MAPKEYS) && x >= 262 && y >= 178 && y <= 185 && code < 0) {
		const char *v = view_name (rb (A6 + A6_VIEW));
		t->id = T_VIEWNAME, t->x0 = 262, t->y0 = 178, t->x1 = 319, t->y1 = 185;
		SET (title, "View");
		if (in_3d () && v)
			snprintf (t->body, sizeof (t->body), "Which way you are looking: the %s view.", v);
		else
			SET (body, "Which way you are looking.");
		key_for (0xf1, t->key, sizeof (t->key));
		return t->id;
	}

	if (code >= 0) {
		t->id = T_HIT + 0x100 * code, t->code = code;
		t->x0 = r[0], t->y0 = r[1], t->x1 = r[2], t->y1 = r[3];
		if (code < 0x10 || (code >= 0x18 && code < 0xc1)) return t->id = 0;
		if (code >= 0x10 && code <= 0x17) {
			int n = code - 0x10 + 1;
			snprintf (t->title, sizeof (t->title), "Missile %d", n);
			SET (body, rb (A6 + A6_MISSILE_SEL) == n ? "Selected: click again to deselect it." :
				   "Click to select it for firing.");
			return t->id;
		}
		if (code >= 0xc1 && code <= 0xc5) {
			time_tip (t, code);
		} else if (code == 0xfe) {
			time_tip (t, code);
		} else if (code >= 0xf1 && code <= 0xf4) {
			slot_tip (t, code - 0xf0, &p);
		} else if (code >= 0xf7 && code <= 0xfa) {
			slot_tip (t, code - 0xf0, &p);
		} else if (code == 0xf5) {
			if (p.flags & F_SCANNER) {
				SET (title, "Scanner");
				SET (body, "Ships and objects around you: stalks show above or below your plane.");
			} else if (p.n_missiles) {
				SET (title, "Missile pylons");
				SET (body, "Click a missile to select it.");
			} else {
				SET (title, "Bay display");
				SET (body, "The scanner, the missile pylons or messages.");
			}
		}
		if (!t->title[0]) return t->id = 0;
		if (!t->key[0]) key_for (code, t->key, sizeof (t->key));
		return t->id;
	}

	if (p.valid && x <= 75 && y >= 169 && y <= 177) {
		t->id = T_CLOCK, t->x0 = 0, t->y0 = 169, t->x1 = 75, t->y1 = 177;
		SET (title, "Date and time");
		SET (body, "The game's clock. The keys below set how fast it runs.");
		return t->id;
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* The label, painted with CoreText */

#define SHOW_MS		400
#define FADE_MS		120

static int log_on = -1;
static struct tip cur;
static Uint32 since;
static int had_focus = 1, hide_until_move, hide_x, hide_y, last_buttons;
static float anchor_x;

#ifdef __APPLE__

static GLuint tex;
static int tex_w, tex_h, tex_tw, tex_th;
static char tex_key[600];

static CTLineRef mkline (const char *s, long from, long len, CTFontRef f, unsigned int rgb)
{
	CFStringRef str = CFStringCreateWithCString (NULL, s, kCFStringEncodingUTF8);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB ();
	CGFloat c[4] = { ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, 1 };
	CGColorRef col = CGColorCreate (cs, c);
	CFStringRef keys[2] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
	CFTypeRef vals[2] = { f, col };
	CFDictionaryRef at = CFDictionaryCreate (NULL, (const void **) keys, (const void **) vals, 2,
						 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef as;
	CTLineRef l;

	if (len < 0) len = CFStringGetLength (str) - from;
	{
		CFStringRef sub = CFStringCreateWithSubstring (NULL, str, CFRangeMake (from, len));
		as = CFAttributedStringCreate (NULL, sub, at);
		CFRelease (sub);
	}
	l = CTLineCreateWithAttributedString (as);
	CFRelease (as);
	CFRelease (at);
	CGColorRelease (col);
	CGColorSpaceRelease (cs);
	CFRelease (str);
	return l;
}

static float line_w (CTLineRef l, float *asc, float *desc)
{
	CGFloat a, d, lead;
	double w = CTLineGetTypographicBounds (l, &a, &d, &lead);
	if (asc) *asc = a;
	if (desc) *desc = d;
	return (float) w;
}

#define MAX_LINES	12
struct pline { CTLineRef l; float x, y; };	/* y: the baseline, from the top */

/* Break s into lines of at most w pixels */
static int wrap (const char *s, CTFontRef f, unsigned int rgb, float w, CTLineRef *out, int max)
{
	CFStringRef str;
	CFAttributedStringRef as;
	CFDictionaryRef at;
	CTTypesetterRef ts;
	CFStringRef keys[1] = { kCTFontAttributeName };
	CFTypeRef vals[1] = { f };
	long start = 0, len;
	int n = 0;

	if (!s[0]) return 0;
	str = CFStringCreateWithCString (NULL, s, kCFStringEncodingUTF8);
	at = CFDictionaryCreate (NULL, (const void **) keys, (const void **) vals, 1,
				 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	as = CFAttributedStringCreate (NULL, str, at);
	ts = CTTypesetterCreateWithAttributedString (as);
	len = CFStringGetLength (str);
	while (start < len && n < max) {
		long k = CTTypesetterSuggestLineBreak (ts, start, w);
		if (k <= 0) k = len - start;
		out[n++] = mkline (s, start, k, f, rgb);
		start += k;
	}
	CFRelease (ts);
	CFRelease (as);
	CFRelease (at);
	CFRelease (str);
	return n;
}

static void rrect (CGContextRef g, float x, float y, float w, float h, float r)
{
	CGMutablePathRef p = CGPathCreateMutable ();
	CGPathAddRoundedRect (p, NULL, CGRectMake (x, y, w, h), r, r);
	CGContextAddPath (g, p);
	CGPathRelease (p);
}

/* Paint the label's text into tex; its size in tex_w x tex_h */
static int paint (const struct tip *t, float u, int max_w, int see)
{
	char key[sizeof (tex_key)];
	CTFontRef ft, fk, fb, fw;
	CTLineRef title, keyl = NULL, body[MAX_LINES], why[MAX_LINES];
	int nb, nw, i, W, H;
	float px = floorf (16 * u), py = floorf (12 * u), lh = floorf (25 * u), tri = floorf (22 * u);
	float ta, td, tw, kw = 0, ka, kd, kcw = 0, kch = floorf (26 * u), wmax, top, wrapw;
	unsigned int c2 = see ? 0xc2d0e0 : 0x8fa3b8;
	unsigned char *buf;
	CGColorSpaceRef cs;
	CGContextRef g;

	snprintf (key, sizeof (key), "%s|%s|%s|%s|%d|%d|%d", t->title, t->key, t->body, t->why,
		  (int) (u * 1000), max_w, see);
	if (tex && !strcmp (key, tex_key)) return 1;

	ft = CTFontCreateWithName (CFSTR ("DINAlternate-Bold"), 25 * u, NULL);
	fk = CTFontCreateWithName (CFSTR ("DINAlternate-Bold"), 18 * u, NULL);
	fb = CTFontCreateWithName (CFSTR ("AvenirNext-Medium"), 19 * u, NULL);
	fw = CTFontCreateWithName (CFSTR ("AvenirNext-DemiBold"), 19 * u, NULL);
	if (!ft || !fk || !fb || !fw) {
		if (ft) CFRelease (ft);
		if (fk) CFRelease (fk);
		if (fb) CFRelease (fb);
		if (fw) CFRelease (fw);
		return 0;
	}
	wrapw = max_w - 2 * px;
	title = mkline (t->title, 0, -1, ft, 0xe8eef5);
	tw = line_w (title, &ta, &td);
	if (t->key[0]) {
		keyl = mkline (t->key, 0, -1, fk, 0x3fd0ff);
		kw = line_w (keyl, &ka, &kd);
		kcw = ceilf (kw + 16 * u);
	}
	nb = wrap (t->body, fb, c2, wrapw, body, MAX_LINES);
	nw = wrap (t->why, fw, 0xffb347, wrapw - tri, why, MAX_LINES);

	wmax = tw + (keyl ? 14 * u + kcw : 0);
	for (i = 0; i < nb; i++) {
		float w = line_w (body[i], NULL, NULL);
		if (w > wmax) wmax = w;
	}
	for (i = 0; i < nw; i++) {
		float w = tri + line_w (why[i], NULL, NULL);
		if (w > wmax) wmax = w;
	}
	W = (int) ceilf (wmax + 2 * px);
	if (W < 140 * u) W = (int) (140 * u);
	if (W > max_w) W = max_w;
	{
		float row = ta + td;
		if (keyl && kch > row) row = kch;
		H = (int) ceilf (py + row + (nb ? 6 * u + nb * lh : 0) + (nw ? 8 * u + nw * lh : 0) + py);
	}

	buf = calloc ((size_t) W * H, 4);
	cs = CGColorSpaceCreateDeviceRGB ();
	g = buf ? CGBitmapContextCreate (buf, W, H, 8, W * 4, cs,
					 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
	if (g) {
		float row = ta + td, y;
		if (keyl && kch > row) row = kch;
		CGContextSetShouldAntialias (g, true);
		CGContextSetShouldSmoothFonts (g, false);
		/* CoreGraphics is y up: tops are measured down from H */
		top = py;
		y = top + (row - (ta + td)) / 2 + ta;
		CGContextSetTextPosition (g, px, H - y);
		CTLineDraw (title, g);
		if (keyl) {
			float kx = W - px - kcw, ky = top + (row - kch) / 2;
			rrect (g, kx + 0.5f, H - ky - kch + 0.5f, kcw - 1, kch - 1, 6 * u);
			CGContextSetRGBFillColor (g, 0x3f / 255.0 * 0.14, 0xd0 / 255.0 * 0.14, 1.0 * 0.14, 0.14);
			CGContextFillPath (g);
			rrect (g, kx + 0.5f, H - ky - kch + 0.5f, kcw - 1, kch - 1, 6 * u);
			CGContextSetRGBStrokeColor (g, 0x3f / 255.0, 0xd0 / 255.0, 1.0, 0.75);
			CGContextSetLineWidth (g, fmaxf (1, 1.5f * u));
			CGContextStrokePath (g);
			CGContextSetTextPosition (g, kx + (kcw - kw) / 2, H - (ky + (kch - (ka + kd)) / 2 + ka));
			CTLineDraw (keyl, g);
		}
		y = top + row;
		if (nb) y += 6 * u;
		for (i = 0; i < nb; i++, y += lh) {
			CGContextSetTextPosition (g, px, H - (y + 0.76f * lh));
			CTLineDraw (body[i], g);
		}
		if (nw) y += 8 * u;
		for (i = 0; i < nw; i++, y += lh) {
			if (i == 0) {
				/* a caution triangle, drawn here */
				float s = 15 * u, cx = px + s / 2, cy = y + lh / 2;
				CGMutablePathRef p = CGPathCreateMutable ();
				CGPathMoveToPoint (p, NULL, cx, H - (cy - s / 2));
				CGPathAddLineToPoint (p, NULL, cx + s / 2, H - (cy + s / 2.3f));
				CGPathAddLineToPoint (p, NULL, cx - s / 2, H - (cy + s / 2.3f));
				CGPathCloseSubpath (p);
				CGContextAddPath (g, p);
				CGContextSetRGBFillColor (g, 1.0, 0xb3 / 255.0, 0x47 / 255.0, 1);
				CGContextFillPath (g);
				CGPathRelease (p);
				CGContextSetRGBFillColor (g, 0x10 / 255.0, 0x18 / 255.0, 0x23 / 255.0, 1);
				CGContextFillRect (g, CGRectMake (cx - 0.9f * u, H - (cy + 0.25f * s), 1.8f * u, 0.38f * s));
				CGContextFillRect (g, CGRectMake (cx - 0.9f * u, H - (cy + 0.40f * s), 1.8f * u, 1.8f * u));
			}
			CGContextSetTextPosition (g, px + tri, H - (y + 0.76f * lh));
			CTLineDraw (why[i], g);
		}
		CGContextRelease (g);
	}
	CGColorSpaceRelease (cs);
	CFRelease (title);
	if (keyl) CFRelease (keyl);
	for (i = 0; i < nb; i++) CFRelease (body[i]);
	for (i = 0; i < nw; i++) CFRelease (why[i]);
	CFRelease (ft);
	CFRelease (fk);
	CFRelease (fb);
	CFRelease (fw);
	if (!buf) return 0;

	if (!tex) glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	tex_tw = 1, tex_th = 1;
	while (tex_tw < W) tex_tw <<= 1;
	while (tex_th < H) tex_th <<= 1;
	glPushClientAttrib (GL_CLIENT_PIXEL_STORE_BIT);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, tex_tw, tex_th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	glPopClientAttrib ();
	free (buf);
	tex_w = W, tex_h = H;
	snprintf (tex_key, sizeof (tex_key), "%s", key);
	return 1;
}

#endif /* __APPLE__ */

static void log_tip (const struct tip *t, int x, int y)
{
	unsigned int pl = player ();
	struct hdstill_panel p;

	hdstill_panel (&p);
	fprintf (stderr, "hdtip: at %d,%d id %#x code %#x \"%s\" key \"%s\" why \"%s\" | 704=%#x 705=%#x 706=%d "
		 "42=%d 671=%d 673=%#x 692=%u 708=%d 977=%d 1044=%u 10476=%#x obj211=%#x obj158=%#x "
		 "obj190=%#x obj242=%#x 866=%d panel=%d flags=%d slot7-10=%d,%d,%d,%d missiles=%d\n",
		 x, y, t->id, t->code, t->title, t->key, t->why,
		 rb (A6 + A6_FLIGHT_STATE), rb (A6 + A6_SHIP_FLAGS), rb (A6 + A6_VIEW),
		 (signed char) rb (A6 + A6_TOOLS_MODE), rb (A6 + A6_MISSILE_SEL), rb (A6 + A6_FLAGS673),
		 rl (A6 + A6_NEAREST_M), rb (A6 + A6_AP_TARGET), rb (A6 + A6_DEST_SET), rl (A6 + A6_DEST_SYS),
		 rb (A6 + A6_COMBAT), pl ? rb (pl + OBJ_STATE) : 0, pl ? rl (pl + OBJ_EQUIP) : 0,
		 pl ? rw (pl + OBJ_LASER_TEMP) : 0, pl ? rl (pl + OBJ_FUEL) : 0, rw (A6 + A6_HULL_TEMP),
		 p.valid, p.flags, p.slot[7], p.slot[8], p.slot[9], p.slot[10], p.n_missiles);
}

void hdtip_draw (void)
{
#ifdef __APPLE__
	struct tip t;
	int px, py, pw, ph, focus, x, y, buttons, W, H, margin, max_w;
	float stx, sty, sx, sy, u, a, tx, ty, ex0, ey0, ey1;
	Uint32 now;

	if (classic_ui || pixel_art || use_renderer == R_OLD) return;
	if (log_on < 0) {
		const char *e = getenv ("FE2_HDTIP_LOG");
		log_on = e && *e && *e != '0';
	}
	gl_picture_rect (&px, &py, &pw, &ph);
	if (pw < 32 || ph < 32) return;

	/* hidden when the window loses the pointer, until it moves again */
	focus = (SDL_GetAppState () & SDL_APPMOUSEFOCUS) != 0;
	if (had_focus && !focus) {
		hide_until_move = 1;
		hide_x = input.abs_x, hide_y = input.abs_y;
	}
	had_focus = focus;
	if (hide_until_move) {
		if (input.abs_x == hide_x && input.abs_y == hide_y) {
			cur.id = 0;
			return;
		}
		hide_until_move = 0;
	}

	stx = view_w > 0 ? 320.0f * (input.abs_x - view_x) / view_w : 320.0f * input.abs_x / screen_w;
	sty = view_h > 0 ? 200.0f * (input.abs_y - view_y) / view_h : 200.0f * input.abs_y / screen_h;
	x = (int) floorf (stx), y = (int) floorf (sty);
	now = SDL_GetTicks ();
	if (x < 0 || x > 319 || y < 0 || y > 199 || !find (x, y, &t)) {
		cur.id = 0;
		return;
	}
	/* a button held: the player is busy with the part, not reading */
	buttons = input.cur_mousebut_state;
	if (buttons || last_buttons) since = now;
	last_buttons = buttons;
	if (t.id != cur.id) {
		since = now;
		anchor_x = stx;
		if (log_on) log_tip (&t, x, y);
	}
	cur = t;
	if (buttons || now - since < SHOW_MS) return;
	a = (now - since - SHOW_MS) / (float) FADE_MS;
	if (a > 1) a = 1;

	u = ph / 1200.0f;
	sx = pw / 320.0f;
	sy = ph / 200.0f;
	margin = (int) (12 * u);
	max_w = (int) (560 * u);
	if (max_w > pw - 2 * margin) max_w = pw - 2 * margin;

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT | GL_TEXTURE_BIT |
		      GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT | GL_POLYGON_BIT |
		      GL_SCISSOR_BIT);
	glViewport (px, py, pw, ph);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, pw, ph, 0, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_FALSE);
	glDisable (GL_CULL_FACE);
	glDisable (GL_LIGHTING);
	glDisable (GL_ALPHA_TEST);
	glDisable (GL_FOG);
	glDisable (GL_SCISSOR_TEST);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	glDisable (GL_TEXTURE_2D);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	if (paint (&t, u, max_w, hdback_active ())) {
		W = tex_w, H = tex_h;
		/* above the part, under the pointer; below it when there is no room */
		ex0 = t.x0 * sx;
		ey0 = t.y0 * sy;
		ey1 = (t.y1 + 1) * sy;
		(void) ex0;
		tx = floorf (anchor_x * sx - W / 2.0f);
		if (tx < margin) tx = margin;
		if (tx > pw - margin - W) tx = pw - margin - W;
		/* over the view, clear of the panel's own readouts */
		if (ey0 > 168 * sy) ey0 = 168 * sy;
		ty = floorf (ey0 - 10 * u - H);
		if (ty < margin) ty = floorf (ey1 + 10 * u);
		hdui_glass_px (tx, ty, W, H, a);
		glEnable (GL_TEXTURE_2D);
		glBindTexture (GL_TEXTURE_2D, tex);
		glColor4f (a, a, a, a);
		glBegin (GL_TRIANGLE_STRIP);
		glTexCoord2f (0, 0); glVertex2f (tx, ty);
		glTexCoord2f (W / (float) tex_tw, 0); glVertex2f (tx + W, ty);
		glTexCoord2f (0, H / (float) tex_th); glVertex2f (tx, ty + H);
		glTexCoord2f (W / (float) tex_tw, H / (float) tex_th); glVertex2f (tx + W, ty + H);
		glEnd ();
		glDisable (GL_TEXTURE_2D);
	}

	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
#endif
}

void hdtip_forget (void)
{
#ifdef __APPLE__
	if (tex) glDeleteTextures (1, &tex);
	tex = 0;
	tex_key[0] = 0;
#endif
}
