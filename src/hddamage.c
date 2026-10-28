/*
 * Hit feedback for the HD flight view (see hddamage.h).
 *
 * Each frame: the player's shields and hull, read where the game keeps
 * them (the figures its own HUD and the F3 page show, with the same
 * arithmetic).  A drop since the last frame is a hit: the edges of the
 * view glow red and fade out within FLASH_MS.  The game does not keep
 * who fired, so the glow has no direction.  While the shields are down,
 * for LINGER_MS after a hit, and whenever the hull is under a quarter, a
 * small glass card at the top right of the view shows both as bars, the
 * figure just hit lit red, and a warning line when the hull is critical.
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
#include "hddamage.h"
#include "hdui.h"
#include "screen.h"
#include "bindings.h"
#include "../m68000.h"
#include "../fixups.h"

#define A6			__DL5eb6_a6_base
#define A6_PLAYER		674	/* long: the player's ship object */
#define A6_GAME_DATA		14470	/* long: the game data, by object type */
#define OBJ_TYPE		90	/* word: the object's type, into the game data */
#define OBJ_SHIELD		180	/* word: shields now (the damage routine takes hits here first) */
#define OBJ_SHIELD_MAX		182	/* word: shields full, 64 a generator */
#define OBJ_HULL		184	/* word: hull left; full is four times the type's hull mass */

#define FLASH_MS	400	/* the edge glow, gone */
#define HOT_MS		1500	/* the figure hit stays lit */
#define LINGER_MS	6000	/* the card stays after the last hit */
#define CRIT_TENTHS	250	/* hull under 25.0%: the warning */

/* hdui's tokens (C_ACCENT, C_AMBER, C_RED, C_TEXT, C_TEXT2) */
#define T_ACCENT	0x3fd0ff
#define T_AMBER		0xffb347
#define T_RED		0xff4d4d
#define T_TEXT		0xe8eef5
#define T_TEXT2		0x8fa3b8

static int rw (p68K a) { return STMemory_ReadWord (a) & 0xffff; }
static unsigned int rl (p68K a) { return (unsigned int) STMemory_ReadLong (a); }

static unsigned int last_pl;
static int last_sh = -1, last_hull = -1, was_flying, log_on = -1;
static Uint32 flash_at, sh_hit, hull_hit, last_hit, last_ms;
static float flash_a, card_a;

/* The figures as the game shows them: shields in whole percent (none
 * fitted: -1), the hull in tenths of a percent (L7e142, F3's condition) */
static void read_ship (unsigned int pl, int *sh, int *sh_pct, int *hull, int *hull_t)
{
	unsigned int gd = rl (A6 + A6_GAME_DATA), max = rw (pl + OBJ_SHIELD_MAX);
	p68K ty = gd + rw (gd + (short) rw (pl + OBJ_TYPE));
	int mass;

	ty += (short) rw (ty + 28);
	mass = rw (ty + 6);
	*sh = rw (pl + OBJ_SHIELD);
	*sh_pct = max ? (int) (*sh * 100u / max) : -1;
	*hull = rw (pl + OBJ_HULL);
	/* the killing hit leaves the word wrapped round (L44c96: sub.w, bls) */
	if (mass && *hull > 4 * mass) *hull = 0;
	*hull_t = mass ? (int) (*hull * 250u / mass) : 1000;
}

/* ---------------------------------------------------------------- */
/* The card's text, painted with CoreText into one texture */

static GLuint tex;
static int tex_w, tex_h, tex_tw, tex_th;
static char tex_key[96];

#ifdef __APPLE__
static void text (CGContextRef g, const char *s, CTFontRef f, unsigned int rgb, float x, float y, int right)
{
	CFStringRef str = CFStringCreateWithCString (NULL, s, kCFStringEncodingUTF8);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB ();
	CGFloat c[4] = { ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, 1 };
	CGColorRef col = CGColorCreate (cs, c);
	CFStringRef keys[2] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
	CFTypeRef vals[2] = { f, col };
	CFDictionaryRef at = CFDictionaryCreate (NULL, (const void **) keys, (const void **) vals, 2,
						 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef as = CFAttributedStringCreate (NULL, str, at);
	CTLineRef l = CTLineCreateWithAttributedString (as);

	if (right) x -= (float) CTLineGetTypographicBounds (l, NULL, NULL, NULL);
	CGContextSetTextPosition (g, x, y);
	CTLineDraw (l, g);
	CFRelease (l);
	CFRelease (as);
	CFRelease (at);
	CGColorRelease (col);
	CGColorSpaceRelease (cs);
	CFRelease (str);
}
#endif

/* Card layout, in design units (u: a pixel at 1200 high) */
#define CARD_W		250
#define PAD		14
#define ROW		38	/* row pitch */
#define BAR_Y		24	/* a row's bar, below its top */
#define BAR_H		5
#define CRIT_H		26

static int card_h (int rows, int crit) { return 2 * PAD + rows * ROW - (ROW - BAR_Y - BAR_H) + (crit ? CRIT_H : 0); }

/* rows: shields (when fitted) then hull */
static int paint (int sh_pct, int hull_t, int hot_sh, int hot_hull, float u)
{
#ifdef __APPLE__
	char key[sizeof (tex_key)], v[16];
	int rows = sh_pct >= 0 ? 2 : 1, crit = hull_t < CRIT_TENTHS, W, H, r;
	unsigned char *buf;
	CGColorSpaceRef cs;
	CGContextRef g;
	CTFontRef fl, fv;

	snprintf (key, sizeof (key), "%d|%d|%d|%d|%d", sh_pct, hull_t / 10, hot_sh, hot_hull, (int) (u * 1000));
	if (tex && !strcmp (key, tex_key)) return 1;
	W = (int) ceilf (CARD_W * u);
	H = (int) ceilf (card_h (rows, crit) * u);
	fl = CTFontCreateWithName (CFSTR ("DINAlternate-Bold"), 15 * u, NULL);
	fv = CTFontCreateWithName (CFSTR ("DINAlternate-Bold"), 19 * u, NULL);
	buf = calloc ((size_t) W * H, 4);
	cs = CGColorSpaceCreateDeviceRGB ();
	g = buf && fl && fv ? CGBitmapContextCreate (buf, W, H, 8, W * 4, cs,
						     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
	if (g) {
		CGContextSetShouldAntialias (g, true);
		CGContextSetShouldSmoothFonts (g, false);
		/* CoreGraphics is y up: baselines measured down from H */
		for (r = 0; r < rows; r++) {
			int hull = r == rows - 1, hot = hull ? hot_hull || crit : hot_sh;
			float y = H - (PAD + r * ROW + 17) * u;
			if (hull) snprintf (v, sizeof (v), "%d%%", hull_t / 10);
			else snprintf (v, sizeof (v), "%d%%", sh_pct);
			text (g, hull ? "HULL" : "SHIELDS", fl, T_TEXT2, PAD * u, y, 0);
			text (g, v, fv, hot ? T_RED : T_TEXT, W - PAD * u, y, 1);
		}
		if (crit) text (g, "HULL CRITICAL", fl, T_RED, PAD * u, (PAD + 2) * u, 0);
		CGContextRelease (g);
	}
	CGColorSpaceRelease (cs);
	if (fl) CFRelease (fl);
	if (fv) CFRelease (fv);
	if (!g) {
		free (buf);
		return 0;
	}
	if (!tex) glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	for (tex_tw = 1; tex_tw < W; tex_tw <<= 1) ;
	for (tex_th = 1; tex_th < H; tex_th <<= 1) ;
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
#else
	return 0;
#endif
}

/* premultiplied colour */
static void col (unsigned int rgb, float a)
{
	glColor4f (((rgb >> 16) & 0xff) / 255.0f * a, ((rgb >> 8) & 0xff) / 255.0f * a, (rgb & 0xff) / 255.0f * a, a);
}

static void rect (float x, float y, float w, float h)
{
	glBegin (GL_QUADS);
	glVertex2f (x, y); glVertex2f (x + w, y); glVertex2f (x + w, y + h); glVertex2f (x, y + h);
	glEnd ();
}

/* The red glow: a frame from the edges of x0..x1, y0..y1 (y down) to d
 * inside them, a at the edge and nothing inside */
static void glow (float x0, float y0, float x1, float y1, float d, float a)
{
	float o[5][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
	float in[5][2] = { { x0 + d, y0 + d }, { x1 - d, y0 + d }, { x1 - d, y1 - d }, { x0 + d, y1 - d }, { x0 + d, y0 + d } };
	int i;

	glBegin (GL_QUAD_STRIP);
	for (i = 0; i < 5; i++) {
		col (T_RED, a);
		glVertex2f (o[i][0], o[i][1]);
		glColor4f (0, 0, 0, 0);
		glVertex2f (in[i][0], in[i][1]);
	}
	glEnd ();
}

void hddamage_draw (int fb_w, int fb_h)
{
	unsigned int pl = rl (A6 + A6_PLAYER);
	int flying, sh, sh_pct, hull, hull_t, px, py, pw, ph, show, hot_sh, hot_hull, rows, crit;
	float u, dt, a, x, y, view_top;
	Uint32 now = SDL_GetTicks ();

	if (classic_ui || pixel_art || use_renderer == R_OLD) return;
	flying = pl && bindings_in_flight_view (0, 0) && !controls_page_open ();
	dt = last_ms ? (now - last_ms) / 1000.0f : 0;
	last_ms = now;
	if (!pl) {
		last_pl = 0;
		return;
	}
	read_ship (pl, &sh, &sh_pct, &hull, &hull_t);
	/* a drop since the last frame of the same flight is a hit; anything
	 * else (a new ship, a game loaded, a repair) only moves the marks */
	if (flying && was_flying && pl == last_pl) {
		if (sh < last_sh) sh_hit = last_hit = now;
		if (hull < last_hull) hull_hit = last_hit = now;
		if (sh < last_sh || hull < last_hull) {
			flash_at = now;
			flash_a = hull < last_hull ? 0.55f : 0.35f;
		}
	}
	/* FE2_DMG_LOG set: each change of the figures to stderr, to check
	 * them against the F3 page */
	if (log_on < 0) log_on = getenv ("FE2_DMG_LOG") != NULL;
	if (log_on && (sh != last_sh || hull != last_hull || flying != was_flying || pl != last_pl))
		fprintf (stderr, "DMG t=%u pl=%x fly=%d sh=%d/%d hull=%d raw=%d ht=%d\n", now, pl, flying, sh, rw (pl + OBJ_SHIELD_MAX), hull, rw (pl + OBJ_HULL), hull_t);
	last_pl = pl, last_sh = sh, last_hull = hull, was_flying = flying;
	if (!flying) {
		card_a = 0;
		flash_at = 0;
		return;
	}

	crit = hull_t < CRIT_TENTHS;
	show = (sh_pct >= 0 && sh_pct < 100) || (last_hit && now - last_hit < LINGER_MS) || crit;
	card_a += (show ? dt / 0.15f : -dt / 0.3f);
	if (card_a < 0) card_a = 0;
	if (card_a > 1) card_a = 1;
	a = flash_at && now - flash_at < FLASH_MS ? 1 - (now - flash_at) / (float) FLASH_MS : 0;
	if (!a) flash_at = 0;
	if (!a && !card_a) return;

	gl_picture_rect (&px, &py, &pw, &ph);
	if (pw < 32 || ph < 32) return;
	u = ph / 1200.0f;

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT | GL_TEXTURE_BIT |
		      GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT | GL_POLYGON_BIT |
		      GL_SCISSOR_BIT | GL_LIGHTING_BIT);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
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
	glDisable (GL_TEXTURE_2D);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	glShadeModel (GL_SMOOTH);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	if (a) {
		/* the whole window above the panel: the 3D view, wide or not */
		glViewport (0, 0, fb_w, fb_h);
		glMatrixMode (GL_PROJECTION);
		glLoadIdentity ();
		glOrtho (0, fb_w, fb_h, 0, -1, 1);
		glMatrixMode (GL_MODELVIEW);
		view_top = fb_h - (py + 32.0f * ph / 200);
		glow (0, 0, fb_w, view_top, 0.16f * (fb_w < view_top ? fb_w : view_top), flash_a * a * a);
	}

	rows = sh_pct >= 0 ? 2 : 1;
	hot_sh = sh_hit && now - sh_hit < HOT_MS;
	hot_hull = hull_hit && now - hull_hit < HOT_MS;
	if (card_a && paint (sh_pct, hull_t, hot_sh, hot_hull, u)) {
		float c = card_a, m = floorf (16 * u), bw = (CARD_W - 2 * PAD) * u;
		int r;

		glViewport (px, py, pw, ph);
		glMatrixMode (GL_PROJECTION);
		glLoadIdentity ();
		glOrtho (0, pw, ph, 0, -1, 1);
		glMatrixMode (GL_MODELVIEW);
		x = pw - m - tex_w, y = m;
		hdui_glass_px (x, y, tex_w, tex_h, c);
		/* the bars: shields in the accent, the hull white, amber under
		 * half, red (pulsing) when critical */
		for (r = 0; r < rows; r++) {
			int hull_row = r == rows - 1;
			float f = (hull_row ? hull_t / 1000.0f : sh_pct / 100.0f), by = y + (PAD + r * ROW + BAR_Y) * u;
			unsigned int k = !hull_row ? T_ACCENT : crit ? T_RED : hull_t < 500 ? T_AMBER : T_TEXT;
			float ka = hull_row && crit ? 0.65f + 0.35f * sinf (now * 0.008f) : 1;
			if (f > 1) f = 1;
			if (f < 0) f = 0;
			col (T_TEXT2, 0.22f * c);
			rect (x + PAD * u, by, bw, BAR_H * u);
			col (k, ka * c);
			rect (x + PAD * u, by, floorf (bw * f + 0.5f), BAR_H * u);
		}
		glEnable (GL_TEXTURE_2D);
		glBindTexture (GL_TEXTURE_2D, tex);
		glColor4f (c, c, c, c);
		glBegin (GL_TRIANGLE_STRIP);
		glTexCoord2f (0, 0); glVertex2f (x, y);
		glTexCoord2f (tex_w / (float) tex_tw, 0); glVertex2f (x + tex_w, y);
		glTexCoord2f (0, tex_h / (float) tex_th); glVertex2f (x, y + tex_h);
		glTexCoord2f (tex_w / (float) tex_tw, tex_h / (float) tex_th); glVertex2f (x + tex_w, y + tex_h);
		glEnd ();
	}

	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
}

void hddamage_forget (void)
{
	if (tex) glDeleteTextures (1, &tex);
	tex = 0;
	tex_key[0] = 0;
}
