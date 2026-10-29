/*
 * hdback: see hdback.h and docs/hd/backdrop.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "hdback.h"
#include "hdui.h"
#include "art.h"
#include "procfx.h"
#include "hdtext.h"
#include <SDL.h>
#include "../m68000.h"
#include "../fixups.h"

/* the game's top of memory (host.h) */
#define ST_MEM_SIZE	0x110000

extern int fe2_bgcol;
extern unsigned short MainPalette[256];	/* ST $0rgb words, read only */

#define ST_W		320
#define PAGE_ROWS	168	/* rows 168-199 are the console */
#define CLEAR		255	/* the transparent index in the GL upload */

/* --- where the ship is: read only ------------------------------------
 *
 * Offsets into the game's state block and its objects, found by reading
 * the disassembly (labels in docs/hd/backdrop.md).  All reads, no writes. */
#define A6			__DL5eb6_a6_base
#define A6_FLIGHT_STATE		704	/* byte */
#define A6_PORT_INDEX		672	/* byte: the starport docked with */
#define A6_PORT_PTR		678	/* long: its object */
#define A6_BIG_SPACE		14466	/* long: the object slots */
#define A6_SPACE_MODE		536	/* byte: bit 0 set while a 3D view fills each frame */
#define A6_VIEW_MODE		706	/* byte: A6_plr_3dview_mode */
#define OBJ_OFFSETS		__DL44b02	/* word per index: slot offset */

#define OBJ_SIZE		286
#define OBJ_SLOTS		115	/* the offset table has 0..114 */
#define OBJ_PARENT		68	/* byte: index of the body it orbits */
#define OBJ_MODEL		90	/* word */
#define OBJ_INDEX		93	/* byte: its own index */
#define OBJ_SEED		118	/* long */
#define OBJ_TEMP		182	/* word: kelvin */
#define OBJ_NAME		246	/* its name, NUL-ended (GetFmtStr writes it) */
#define OBJ_FLAGS		280	/* byte */

#define STATE_DOCKED		0x54
#define STATE_LANDED		0x60
#define STATE_LANDED_ROUGH	0x68

#define PAGE_COLOUR		0xe	/* fe2_bgcol after a flat page fill */

enum kind { K_NONE, K_ORBITAL, K_SURFACE };
static const char *kind_name[] = { "none", "orbital", "surface" };

static const char *light_name[] = { "none", "red", "orange", "yellow", "white", "blue" };

struct where {
	int state;
	int port_idx, port_model, port_flags;
	unsigned int port_seed;
	int body_idx, body_model, body_temp;
	int star_idx, star_model;
	enum kind kind;
	const char *cls;	/* body class, NULL: no picture fits */
	int moon;		/* the body is a moon of a gas giant */
	enum light light;
	char chain[64];		/* the parents walked, for the log */
};

static int rb (p68K a) { return STMemory_ReadByte (a) & 0xff; }
static int rw (p68K a) { return STMemory_ReadWord (a) & 0xffff; }
static unsigned int rl (p68K a) { return (unsigned int) STMemory_ReadLong (a); }

static int sane (unsigned int p)
{
	return p >= 0x400 && p < ST_MEM_SIZE - OBJ_SIZE;
}

/* the object in slot idx, 0 if out of reach */
static unsigned int obj_ptr (int idx)
{
	unsigned int big = rl (A6 + A6_BIG_SPACE);
	unsigned int p;

	if (idx < 0 || idx >= OBJ_SLOTS || !sane (big)) return 0;
	p = big + (short) rw (OBJ_OFFSETS + 2 * idx);
	return sane (p) ? p : 0;
}

/* the class any port falls back to: a body outside the table, or one
 * with no picture of its own (a star, a surface port on a giant) */
#define DEFAULT_CLASS "airless"

/* the look of a body, by the model the game draws it with */
static const char *body_class (int model)
{
	switch (model) {
	case 0x7a: case 0x7c: case 0x80: return "airless";	/* bare rock, no air ($7c: asteroids) */
	case 0x7e: case 0x82: return "rocky";	/* thin dusty air, desert ($82: hot rock, Sirius) */
	case 0x84: return "corrosive";		/* thick hot or ammonia murk */
	case 0x86: return "methane";		/* cold orange smog */
	case 0x88: case 0x8a: case 0x90: return "terran";	/* oxygen, oceans, cloud ($90: Earth) */
	case 0x8c: return "ice";		/* oxygen but frozen */
	case 0x8e: return "ocean";		/* corrosive water world */
	case 0x92: case 0x94: case 0x96: return "gasgiant";
	}
	return is_star (model) ? "star" : DEFAULT_CLASS;
}

static void locate (struct where *w)
{
	unsigned int port, p;
	int i, idx;

	memset (w, 0, sizeof *w);
	w->state = rb (A6 + A6_FLIGHT_STATE);
	if (w->state != STATE_DOCKED && w->state != STATE_LANDED &&
	    w->state != STATE_LANDED_ROUGH)
		return;
	port = rl (A6 + A6_PORT_PTR);
	if (!sane (port)) return;
	w->port_idx = rb (A6 + A6_PORT_INDEX);
	w->port_model = rw (port + OBJ_MODEL);
	w->port_flags = rb (port + OBJ_FLAGS);
	w->port_seed = rl (port + OBJ_SEED);
	if (!(w->port_flags & 0x10)) return;		/* not a starport */

	if (w->port_model >= 0x54 && w->port_model <= 0x58)
		w->kind = K_ORBITAL;
	else if (w->port_model >= 0x5a && w->port_model <= 0x6c)
		w->kind = K_SURFACE;
	else
		return;

	w->body_idx = rb (port + OBJ_PARENT);
	p = obj_ptr (w->body_idx);
	if (!p) return;
	w->body_model = rw (p + OBJ_MODEL);
	w->body_temp = rw (p + OBJ_TEMP);

	/* the star: up the parents to the first one */
	idx = w->body_idx;
	for (i = 0; i < 8 && p; i++) {
		int m = rw (p + OBJ_MODEL);
		size_t l = strlen (w->chain);
		snprintf (w->chain + l, sizeof w->chain - l, "%s#%d:$%02x",
			  i ? "<" : "", idx, m);
		if (i == 1 && m >= 0x92 && m <= 0x96)
			w->moon = 1;	/* the giant is in the sky */
		if (is_star (m)) {
			w->star_idx = idx;
			w->star_model = m;
			break;
		}
		idx = rb (p + OBJ_PARENT);
		p = obj_ptr (idx);
		if (!p) {
			l = strlen (w->chain);
			snprintf (w->chain + l, sizeof w->chain - l, "<#%d:?", idx);
		}
	}
	w->light = star_light (w->star_model);
	w->cls = body_class (w->body_model);
	/* a surface port stands on a solid world only */
	if (w->kind == K_SURFACE && w->cls &&
	    (!strcmp (w->cls, "gasgiant") || !strcmp (w->cls, "star")))
		w->cls = NULL;
}

/* --- the manifest ----------------------------------------------------- */

#define MAX_ART 96

struct art {
	enum kind kind;
	char cls[16];
	enum light light;
	char file[64];
};

static struct art arts[MAX_ART];
static int n_arts;
static float tint[L_MAX][3];
static enum light use_light[L_MAX];	/* no picture for a light: borrow */
static int manifest_read;

static enum light light_by_name (const char *s)
{
	int i;
	for (i = 1; i < L_MAX; i++)
		if (!strcmp (s, light_name[i])) return i;
	return L_NONE;
}

static void read_manifest (void)
{
	FILE *f;
	char line[256], a[32], b[32], c[32], d[80];
	float r, g, bl;
	int i;

	manifest_read = 1;
	for (i = 0; i < L_MAX; i++) {
		tint[i][0] = tint[i][1] = tint[i][2] = 1.0f;
		use_light[i] = L_NONE;
	}
	f = fopen (art_path ("backdrops/manifest.txt"), "r");
	if (!f) return;
	while (fgets (line, sizeof line, f)) {
		if (line[0] == '#') continue;
		if (sscanf (line, "tint %31s %f %f %f", a, &r, &g, &bl) == 4) {
			enum light l = light_by_name (a);
			if (l) { tint[l][0] = r; tint[l][1] = g; tint[l][2] = bl; }
		} else if (sscanf (line, "borrow %31s %31s", a, b) == 2) {
			use_light[light_by_name (a)] = light_by_name (b);
		} else if (sscanf (line, "%31s %31s %31s %79s", a, b, c, d) == 4 &&
			   n_arts < MAX_ART) {
			struct art *e = &arts[n_arts];
			e->kind = !strcmp (a, "orbital") ? K_ORBITAL :
				  !strcmp (a, "surface") ? K_SURFACE : K_NONE;
			snprintf (e->cls, sizeof e->cls, "%s", b);
			e->light = light_by_name (c);
			snprintf (e->file, sizeof e->file, "%s", d);
			if (e->kind && e->light) n_arts++;
		}
	}
	fclose (f);
}

/* the pictures of one key */
static int find (enum kind kind, const char *cls, enum light l,
		 const struct art **found)
{
	int i, n = 0;

	for (i = 0; i < n_arts; i++)
		if (arts[i].kind == kind && arts[i].light == l &&
		    !strcmp (arts[i].cls, cls))
			found[n++] = &arts[i];
	return n;
}

/* the picture for w and the light it was made under, NULL if none.  A
 * moon tries its CLASS-moon pictures (the giant in the sky) first; a light
 * with no picture borrows the one the manifest names, to be tinted; a class
 * with no picture (and an unknown light) falls back to DEFAULT_CLASS. */
static const struct art *pick (const struct where *w, enum light *drawn)
{
	const struct art *found[MAX_ART];
	char cls[3][24];
	enum light l;
	unsigned int h;
	int c, n, tries, n_cls = 0;

	if (!manifest_read) read_manifest ();
	if (!w->kind) return NULL;
	if (w->moon && w->cls)
		snprintf (cls[n_cls++], sizeof cls[0], "%s-moon", w->cls);
	if (w->cls)
		snprintf (cls[n_cls++], sizeof cls[0], "%s", w->cls);
	/* every docked page gets a picture: last, the default class */
	snprintf (cls[n_cls++], sizeof cls[0], "%s", DEFAULT_CLASS);
	for (c = 0; c < n_cls; c++) {
		l = w->light ? w->light : L_YELLOW;
		for (tries = 0; tries < 2 && l; tries++, l = use_light[l]) {
			n = find (w->kind, cls[c], l, found);
			if (!n) continue;
			/* the same port always gets the same one */
			h = w->port_seed * 2654435761u ^ (unsigned int) w->port_idx * 40503u;
			h ^= h >> 15;
			*drawn = l;
			return found[h % n];
		}
	}
	return NULL;
}

/* --- the texture ------------------------------------------------------ */

static int debug = -1;	/* FE2_HD_DEBUG, read by hdback_draw */

static GLuint tex;
static int tex_w, tex_h, img_w, img_h;
static char tex_file[64];	/* what tex holds, or failed to load */

/* the frost under hdui's glass: the same picture, a quarter the size,
 * blurred and graded (make_frost), in a texture of its own */
static GLuint frost_tex;
static int frost_tw, frost_th;

static GLuint conc_tex;		/* the concourse's picture, its own */
static const char *conc_file;

static void forget_page (void)
{
	if (tex) glDeleteTextures (1, &tex);
	if (frost_tex) glDeleteTextures (1, &frost_tex);
	tex = frost_tex = 0;
	tex_file[0] = 0;
}

void hdback_forget (void)
{
	forget_page ();
	if (conc_tex) glDeleteTextures (1, &conc_tex);
	conc_tex = 0;
	conc_file = NULL;
}

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

/* --- the frost ---------------------------------------------------------
 *
 * Frosted glass shows what is behind it blurred.  The blur is made once
 * per picture, here, never per frame: the picture boxed down by FROST_DOWN,
 * then three box blurs each way (three boxes are close to a gaussian), the
 * edges clamped.  The radius is chosen so the blur's sigma is FROST_SIGMA
 * pixels of a FROST_REF_W wide window, the picture covering it (it scales
 * with the window, like the picture): wide enough that what is left of the
 * picture is soft shapes and colour, not things.
 *
 * Then the grade, which holds the brightness and lets the colour through.
 * Each texel's brightness (Rec. 709 luma) is taken as its swing from the
 * whole frost's mean, scaled by FROST_FLAT and rolled off (tanh) so it never
 * passes FROST_SPAN, around a fixed FROST_MID: whatever the picture, a dark
 * hangar or a sunlit planet, the frost's luma stays within FROST_MID +-
 * FROST_SPAN (times the light's tint, which only darkens).  That bound is
 * what hdui's text contrast on the glass rests on.  The colour (each
 * channel's difference from the luma) is kept at FROST_SAT, so the planet
 * shows through as blue and the hangar as grey, at the same brightness. */
#define FROST_DOWN	4
#define FROST_SIGMA	24.0f	/* window pixels ... */
#define FROST_REF_W	1600.0f	/* ... at this window width */
#define FROST_MID	0.33f	/* the frost's luma (0..1) ... */
#define FROST_SPAN	0.14f	/* ... never further than this from it ... */
#define FROST_FLAT	0.75f	/* ... the picture's swings scaled by this first */
#define FROST_SAT	1.00f	/* the colour kept */

/* one line of n RGB texels, every stride floats: a box 2r+1 wide, edges clamped */
static void box_line (float *p, int n, int stride, int r, float *tmp)
{
	float s[3] = { 0, 0, 0 }, k = 1.0f / (2 * r + 1);
	int i, c, j;

	for (j = -r; j <= r; j++) {
		int q = j < 0 ? 0 : j >= n ? n - 1 : j;
		for (c = 0; c < 3; c++) s[c] += p[q * stride + c];
	}
	for (i = 0; i < n; i++) {
		int add = i + r + 1, sub = i - r;
		if (add > n - 1) add = n - 1;
		if (sub < 0) sub = 0;
		for (c = 0; c < 3; c++) {
			tmp[i * 3 + c] = s[c] * k;
			s[c] += p[add * stride + c] - p[sub * stride + c];
		}
	}
	for (i = 0; i < n; i++)
		for (c = 0; c < 3; c++) p[i * stride + c] = tmp[i * 3 + c];
}

static void make_frost (const unsigned char *rgba, int w, int h)
{
	int bw = (w + FROST_DOWN - 1) / FROST_DOWN, bh = (h + FROST_DOWN - 1) / FROST_DOWN;
	float sigma, *f, *tmp;
	double mean;
	unsigned char *out;
	int x, y, i, c, r, pass;
	GLint bound;

	/* sigma in downsampled texels; three boxes of 2r+1 give sigma^2 = ((2r+1)^2 - 1) / 4 */
	sigma = FROST_SIGMA * w / FROST_REF_W / FROST_DOWN;
	r = (int) floor ((sqrt (4 * sigma * sigma + 1) - 1) / 2 + 0.5);
	if (r < 1) r = 1;
	frost_tw = pow2 (bw);
	frost_th = pow2 (bh);
	f = malloc ((size_t) bw * bh * 3 * sizeof (float));
	tmp = malloc ((size_t) (bw > bh ? bw : bh) * 3 * sizeof (float));
	out = malloc ((size_t) frost_tw * frost_th * 4);
	if (!f || !tmp || !out) {
		free (f); free (tmp); free (out);
		return;
	}

	/* box down */
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++) {
			float s[3] = { 0, 0, 0 };
			int n = 0, px, py;
			for (py = y * FROST_DOWN; py < (y + 1) * FROST_DOWN && py < h; py++)
				for (px = x * FROST_DOWN; px < (x + 1) * FROST_DOWN && px < w; px++, n++)
					for (c = 0; c < 3; c++)
						s[c] += rgba[((size_t) py * w + px) * 4 + c];
			for (c = 0; c < 3; c++) f[(y * bw + x) * 3 + c] = s[c] / (n * 255.0f);
		}
	/* blur: rows, then columns, three times */
	for (pass = 0; pass < 3; pass++) {
		for (y = 0; y < bh; y++) box_line (f + y * bw * 3, bw, 3, r, tmp);
		for (x = 0; x < bw; x++) box_line (f + x * 3, bh, bw * 3, r, tmp);
	}
	/* the frost's mean brightness, then grade (see above), and fill the
	 * power-of-two texture, the last row and column carried on into the
	 * spare so the edge clamps to the picture's edge */
	for (i = 0, mean = 0; i < bw * bh; i++)
		mean += 0.2126 * f[i * 3] + 0.7152 * f[i * 3 + 1] + 0.0722 * f[i * 3 + 2];
	mean /= (double) bw * bh;
	for (y = 0; y < frost_th; y++)
		for (x = 0; x < frost_tw; x++) {
			const float *s = f + ((y < bh ? y : bh - 1) * bw + (x < bw ? x : bw - 1)) * 3;
			float l = 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
			float g = FROST_MID + FROST_SPAN * tanhf (FROST_FLAT * (l - (float) mean) / FROST_SPAN);
			unsigned char *o = out + ((size_t) y * frost_tw + x) * 4;
			for (c = 0; c < 3; c++) {
				float v = g + (s[c] - l) * FROST_SAT;
				o[c] = (unsigned char) (v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f);
			}
			o[3] = 255;
		}
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &bound);
	glGenTextures (1, &frost_tex);
	glBindTexture (GL_TEXTURE_2D, frost_tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, frost_tw, frost_th, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture (GL_TEXTURE_2D, (GLuint) bound);
	if (debug > 0)
		fprintf (stderr, "hdback: frost %dx%d (texture %dx%d), box radius %d x3, sigma %.1f texels, mean luma %.3f\n",
			 bw, bh, frost_tw, frost_th, r, sqrt (((2 * r + 1) * (2 * r + 1) - 1) / 4.0), mean);
	free (f);
	free (tmp);
	free (out);
}

/* art/rel into *t (its picture w x h at the top left of a tw x th
 * texture); the pixels, for the caller to free, NULL if unreadable */
static unsigned char *upload (const char *rel, GLuint *t, int *w, int *h, int *tw, int *th)
{
	unsigned char *rgba = art_load_rgba (rel, w, h);
	GLint bound;

	if (!rgba) {
		fprintf (stderr, "hdback: cannot read %s\n", art_path (rel));
		return NULL;
	}
	*tw = pow2 (*w);
	*th = pow2 (*h);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &bound);
	glGenTextures (1, t);
	glBindTexture (GL_TEXTURE_2D, *t);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, *tw, *th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture (GL_TEXTURE_2D, (GLuint) bound);
	return rgba;
}

static int load (const char *file)
{
	char rel[96];
	unsigned char *rgba;

	if (!strcmp (file, tex_file)) return tex != 0;
	forget_page ();
	snprintf (tex_file, sizeof tex_file, "%s", file);
	snprintf (rel, sizeof rel, "backdrops/%s", file);
	rgba = upload (rel, &tex, &img_w, &img_h, &tex_w, &tex_h);
	if (!rgba) return 0;
	/* the frost is for hdui's glass, and hdui is off under --classic-ui
	 * (set once, at start): no frost to make there */
	make_frost (rgba, img_w, img_h);
	free (rgba);
	return 1;
}

/* --- drawing ---------------------------------------------------------- */

/* the picture's brightness under the page, and the dark glass over it
 * (#0b1016) that keeps the game's text readable */
#define BACK_GAIN	0.80f
#define SCRIM_ALPHA	0.50f

/* ... and when hdui owns the page (its wash and glass dim it instead) */
#define UI_GAIN		0.95f
#define UI_SCRIM_ALPHA	0.0f

static int drew;	/* hdback_draw put the picture down for this frame */
static int shown;	/* the same, kept until the next hdback_draw (hdback_active) */

/* where hdback_draw put the picture, window pixels (y up), and the tint
 * of the light it drew it under (the frost's colour) */
static float pic_x0, pic_y0, pic_dw, pic_dh, pic_tint[3];

int hdback_active (void)
{
	return shown;
}

int hdback_frost_begin (float *rgb)
{
	GLdouble P[16], M[16], C[16];
	GLint vp[4];
	GLdouble ax, ay, az, aw, bx, by, bz, bw, su, tv, sp[4], tp[4];
	int i, j, k;

	if (!shown || !frost_tex) return 0;

	/* object to window pixels for what the caller draws now: the
	 * viewport after projection x modelview, an affine (orthographic) map */
	glGetDoublev (GL_PROJECTION_MATRIX, P);
	glGetDoublev (GL_MODELVIEW_MATRIX, M);
	glGetIntegerv (GL_VIEWPORT, vp);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			C[i * 4 + j] = 0;
			for (k = 0; k < 4; k++) C[i * 4 + j] += P[k * 4 + j] * M[i * 4 + k];
		}
	/* window x = ax x + ay y + az z + aw, window y = bx x + by y + bz z + bw */
	ax = vp[2] * 0.5 * C[0];
	ay = vp[2] * 0.5 * C[4];
	az = vp[2] * 0.5 * C[8];
	aw = vp[0] + vp[2] * 0.5 * (C[12] + 1);
	bx = vp[3] * 0.5 * C[1];
	by = vp[3] * 0.5 * C[5];
	bz = vp[3] * 0.5 * C[9];
	bw = vp[1] + vp[3] * 0.5 * (C[13] + 1);

	/* hdback_draw's texture coordinates at a window pixel: across the
	 * picture from its left, and down it from its top (the picture's
	 * first row is its top), in the frost texture's own size and padding */
	su = (double) img_w / FROST_DOWN / frost_tw / pic_dw;
	tv = (double) img_h / FROST_DOWN / frost_th / pic_dh;
	sp[0] = ax * su; sp[1] = ay * su; sp[2] = az * su; sp[3] = (aw - pic_x0) * su;
	tp[0] = -bx * tv; tp[1] = -by * tv; tp[2] = -bz * tv; tp[3] = (pic_y0 + pic_dh - bw) * tv;

	glPushAttrib (GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT);
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, frost_tex);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexGeni (GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGeni (GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGendv (GL_S, GL_OBJECT_PLANE, sp);
	glTexGendv (GL_T, GL_OBJECT_PLANE, tp);
	glEnable (GL_TEXTURE_GEN_S);
	glEnable (GL_TEXTURE_GEN_T);
	glDisable (GL_TEXTURE_GEN_R);
	glDisable (GL_TEXTURE_GEN_Q);
	for (i = 0; i < 3; i++) rgb[i] = pic_tint[i];
	return 1;
}

void hdback_frost_end (void)
{
	glPopAttrib ();		/* texgen, binding, enables, env, colour */
}

static void log_where (const struct where *w, int page,
		       const struct art *a, enum light drawn)
{
	static char last[512];
	char buf[512];

	snprintf (buf, sizeof buf,
		  "hdback: state=$%02x mode=$%02x bgcol=%d pal14=$%03x page=%d port=#%d model=$%02x flags=$%02x %s"
		  " body=#%d model=$%02x %dK class=%s chain=%s star=#%d model=$%02x light=%s"
		  " -> key %s-%s-%s %s%s%s",
		  w->state, rb (A6 + A6_SPACE_MODE), fe2_bgcol, MainPalette[PAGE_COLOUR], page,
		  w->port_idx, w->port_model, w->port_flags, kind_name[w->kind],
		  w->body_idx, w->body_model, w->body_temp, w->cls ? w->cls : "-", w->chain,
		  w->star_idx, w->star_model, light_name[w->light],
		  kind_name[w->kind], w->cls ? w->cls : "-", light_name[w->light],
		  a ? a->file : "(no picture)",
		  a && drawn != w->light ? " tint " : "",
		  a && drawn != w->light ? light_name[w->light] : "");
	if (strcmp (buf, last)) {
		fprintf (stderr, "%s\n", buf);
		snprintf (last, sizeof last, "%s", buf);
	}
}

static int off = -1;	/* FE2_HD_BACKDROP=0 */

static void read_env (void)
{
	const char *e = getenv ("FE2_HD_BACKDROP");
	const char *d = getenv ("FE2_HD_DEBUG");
	off = e && !strcmp (e, "0");
	debug = d && *d && strcmp (d, "0");
}

void hdback_draw (int fb_w, int fb_h)
{
	struct where w;
	const struct art *a = NULL;
	enum light drawn = L_NONE;
	static const float no_tint[3] = { 1.0f, 1.0f, 1.0f };
	const float *t;
	float s, dw, dh, x0, y0, u, v, k, scrim;
	GLint env;
	int page;

	if (off < 0) read_env ();
	drew = shown = 0;
	/* --classic-ui keeps the game's own flat pages */
	if (pixel_art || classic_ui || off) return;

	locate (&w);
	/* a flat page: the page colour fill, and no 3D view filling the
	 * screen each frame (the 3D view, galaxy map and system view do) */
	page = fe2_bgcol == PAGE_COLOUR && !(rb (A6 + A6_SPACE_MODE) & 0x01);
	if (w.kind) a = pick (&w, &drawn);
	if (debug && w.state) log_where (&w, page, a, drawn);
	if (!page || !a || !load (a->file)) return;
	drew = shown = 1;

	/* the light the picture was made under, graded to the star's */
	t = drawn != w.light ? tint[w.light] : no_tint;

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
		      GL_TEXTURE_BIT | GL_VIEWPORT_BIT | GL_CURRENT_BIT |
		      GL_TRANSFORM_BIT);
	glGetTexEnviv (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
	glViewport (0, 0, fb_w, fb_h);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, fb_w, 0, fb_h, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_FALSE);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glDisable (GL_BLEND);

	/* cover the window, keep the shape, centred */
	s = (float) fb_w / img_w;
	if ((float) fb_h / img_h > s) s = (float) fb_h / img_h;
	dw = img_w * s;
	dh = img_h * s;
	x0 = (fb_w - dw) * 0.5f;
	y0 = (fb_h - dh) * 0.5f;
	u = (float) img_w / tex_w;
	v = (float) img_h / tex_h;
	k = BACK_GAIN;
	scrim = SCRIM_ALPHA;
	if (hdui_owns_page ()) {
		k = UI_GAIN;
		scrim = UI_SCRIM_ALPHA;
	}
	pic_x0 = x0;
	pic_y0 = y0;
	pic_dw = dw;
	pic_dh = dh;
	pic_tint[0] = t[0];
	pic_tint[1] = t[1];
	pic_tint[2] = t[2];

	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, tex);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f (t[0] * k, t[1] * k, t[2] * k, 1.0f);
	glBegin (GL_TRIANGLE_STRIP);
		glTexCoord2f (0, v); glVertex2f (x0, y0);
		glTexCoord2f (u, v); glVertex2f (x0 + dw, y0);
		glTexCoord2f (0, 0); glVertex2f (x0, y0 + dh);
		glTexCoord2f (u, 0); glVertex2f (x0 + dw, y0 + dh);
	glEnd ();
	glDisable (GL_TEXTURE_2D);

	if (scrim > 0) {
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor4f (0x0b / 255.0f, 0x10 / 255.0f, 0x16 / 255.0f, scrim);
		glBegin (GL_TRIANGLE_STRIP);
			glVertex2f (0, 0);
			glVertex2f (fb_w, 0);
			glVertex2f (0, fb_h);
			glVertex2f (fb_w, fb_h);
		glEnd ();
	}

	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
}

/* --- the concourse ----------------------------------------------------
 *
 * Docked or landed at a port, looking out of the ship (front or rear
 * view), nothing else on the view: the port's arrivals concourse instead
 * of the 3D bay, behind hdui's station-home cards, with the port's own
 * name and the date lit on its two blank signs.  Cross-fades back to the
 * 3D view on a launch or another view; gone at once for a page or a map.
 * Four pictures (art/hangar, docs/hd/hangar.md), the opening in their
 * ceilings tinted to the star's light. */

#define CONC_FADE	0.3f	/* seconds */

enum { C_ORBITAL, C_TERRAN, C_AIRLESS, C_DUSK };

/* each picture: where its ceiling opening ends, and its signs' corners,
 * x,y of TL TR BR BL, the yellow then the blue (fractions of it) */
static const struct cpic { const char *file; float ceil, sign[2][8]; } cpics[] = {
	{ "concourse-orbital.jpg", 0.17f,
	  { { .4695f, .3833f, .6047f, .3819f, .6055f, .4472f, .4695f, .4500f },
	    { .6562f, .3819f, .7945f, .3806f, .7953f, .4458f, .6562f, .4472f } } },
	{ "concourse-terran.jpg", 0.20f,
	  { { .4555f, .3139f, .5969f, .3139f, .5977f, .4264f, .4547f, .4250f },
	    { .6711f, .3125f, .8117f, .3139f, .8125f, .4222f, .6703f, .4250f } } },
	{ "concourse-airless.jpg", 0.22f,
	  { { .4906f, .3333f, .6586f, .3208f, .6586f, .3958f, .4906f, .4042f },
	    { .7320f, .3181f, .8961f, .3097f, .8961f, .3833f, .7312f, .3917f } } },
	{ "concourse-dusk.jpg", 0.25f,
	  { { .4250f, .3653f, .6102f, .3653f, .6102f, .4292f, .4250f, .4306f },
	    { .6813f, .3653f, .8586f, .3653f, .8586f, .4306f, .6813f, .4306f } } },
};

/* orbital; on the ground: airless, a dusk (a red star, or a thick or
 * thin poisonous sky), else a blue sky with a city */
static const struct cpic *conc_pick (const struct where *w, float *rgb)
{
	static const float red[3] = { 1.00f, 0.72f, 0.62f };
	const char *c = w->cls ? w->cls : "-";
	int pic = C_ORBITAL;
	const float *t;

	if (w->kind == K_SURFACE)
		pic = !strcmp (c, "airless") ? C_AIRLESS :
		      w->light == L_RED || !strcmp (c, "corrosive") || !strcmp (c, "methane") ||
		      !strcmp (c, "rocky") ? C_DUSK : C_TERRAN;
	/* the backdrops' grading for the star's light; a red sun, red light
	 * (a dusk is red already) */
	if (!manifest_read) read_manifest ();
	t = w->light != L_RED ? tint[w->light] : pic == C_ORBITAL ? red : tint[L_NONE];
	memcpy (rgb, t, sizeof (float[3]));
	return &cpics[pic];
}

/* the port's own name, from its object (read only) */
static void port_name (char *s, int max)
{
	unsigned int port = rl (A6 + A6_PORT_PTR);
	int i, ch;

	for (i = 0; i < max - 1 && sane (port) && (ch = rb (port + OBJ_NAME + i)) >= ' ' && ch != 0x7f; i++)
		s[i] = (char) ch;
	s[i] = 0;
}

static int conc_tw, conc_th, conc_iw, conc_ih;

static int conc_load (const char *file)
{
	char rel[96];

	if (file == conc_file) return conc_tex != 0;
	if (conc_tex) glDeleteTextures (1, &conc_tex);
	conc_tex = 0;
	conc_file = file;
	snprintf (rel, sizeof rel, "hangar/%s", file);
	free (upload (rel, &conc_tex, &conc_iw, &conc_ih, &conc_tw, &conc_th));
	return conc_tex != 0;
}

/* a texture over the quad q (x,y of TL TR BR BL, pixels), in perspective:
 * each corner's q is from where the diagonals cross */
static void sign_quad (const float *q, GLuint t)
{
	static const float st[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	float d1x = q[4] - q[0], d1y = q[5] - q[1], d2x = q[6] - q[2], d2y = q[7] - q[3];
	float den = d1x * d2y - d1y * d2x, a, b, dist[4], w;
	int i;

	if (fabsf (den) < 1e-6f) return;
	a = ((q[2] - q[0]) * d2y - (q[3] - q[1]) * d2x) / den;	/* along TL-BR */
	b = ((q[2] - q[0]) * d1y - (q[3] - q[1]) * d1x) / den;	/* along TR-BL */
	if (a <= 0 || a >= 1 || b <= 0 || b >= 1) return;
	dist[0] = a; dist[2] = 1 - a; dist[1] = b; dist[3] = 1 - b;
	glBindTexture (GL_TEXTURE_2D, t);
	glBegin (GL_TRIANGLE_FAN);
	for (i = 0; i < 4; i++) {
		w = (dist[i] + dist[(i + 2) & 3]) / dist[(i + 2) & 3];
		glTexCoord4f (st[i][0] * w, st[i][1] * w, 0, w);
		glVertex2f (q[2 * i], q[2 * i + 1]);
	}
	glEnd ();
}

/* the lettering on sign k: the light bleeding round it (bloom), then the
 * letters, dark on the yellow lightbox, white hot on the blue one */
static void sign_text (int k, const char *s, const float *q, float fade)
{
	static const float bloom[2][4] = { { 1.00f, 0.95f, 0.70f, 0.45f }, { 0.62f, 0.55f, 1.00f, 0.80f } };
	static const float ink[2][4] = { { 0.20f, 0.15f, 0.12f, 0.92f }, { 1.00f, 0.98f, 1.00f, 1.00f } };
	unsigned int t, g;
	int w = (int) ((hypotf (q[2] - q[0], q[3] - q[1]) + hypotf (q[4] - q[6], q[5] - q[7])) / 2);
	int h = (int) ((hypotf (q[6] - q[0], q[7] - q[1]) + hypotf (q[4] - q[2], q[5] - q[3])) / 2);

	if (!*s || !hdtext_sign (k, s, w, h, &t, &g)) return;
	glBlendFunc (GL_SRC_ALPHA, GL_ONE);
	glColor4f (bloom[k][0], bloom[k][1], bloom[k][2], bloom[k][3] * fade);
	sign_quad (q, g);
	if (!k) glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f (ink[k][0], ink[k][1], ink[k][2], ink[k][3] * fade);
	sign_quad (q, t);
}

static int conc_up;	/* the concourse took the gunsight out this frame */

int hdback_concourse_up (void)
{
	return conc_up;
}

/* out: the screen hdui and the rest have edited, before it is drawn */
void hdback_concourse (int fb_w, int fb_h, unsigned char *out)
{
	static float fade, rgb[3];
	static const struct cpic *p;
	static Uint32 last;
	static char name[24], date[32];
	const char *dt;
	struct where w;
	Uint32 now = SDL_GetTicks ();
	float dts = last ? (now - last) / 1000.0f : 0, s, dw, dh, x0, y0, u, v, y, c, q[8], ph;
	int px, py, pw, ph_i, top, k, i, bare;

	last = now;
	conc_up = 0;
	if (dts > 0.1f) dts = 0.1f;
	if (off < 0) read_env ();
	if (pixel_art || classic_ui || off) return;
	bare = hdui_concourse (&dt);
	locate (&w);
	if (bare == 2 && w.kind && w.state != STATE_LANDED_ROUGH && rb (A6 + A6_VIEW_MODE) <= 1 &&
	    (rb (A6 + A6_SPACE_MODE) & 0x01)) {
		p = conc_pick (&w, rgb);
		port_name (name, sizeof name);
		snprintf (date, sizeof date, "%s", dt);
		fade += dts / CONC_FADE;
	} else
		/* a launch: fades; a page or a map: gone at once */
		fade = bare && (rb (A6 + A6_SPACE_MODE) & 0x01) ? fade - dts / CONC_FADE : 0;
	if (fade > 1) fade = 1;
	if (fade <= 0 || !p || !conc_load (p->file)) {
		fade = 0;
		return;
	}
	if ((conc_up = fade > 0.5f))	/* the gunsight off the signs and the door */
		for (i = 60; i < 108; i++) memset (out + i * ST_W + 136, CLEAR, 48);

	/* over the 3D view and the bars beside it, down to the console */
	gl_picture_rect (&px, &py, &pw, &ph_i);
	top = py + 32 * ph_i / 200;
	pw = fb_w;
	ph = (float) (fb_h - top);
	s = (float) pw / conc_iw;
	if (ph / conc_ih > s) s = ph / conc_ih;
	dw = conc_iw * s;
	dh = conc_ih * s;
	x0 = (pw - dw) * 0.5f;
	y0 = (ph - dh) * 0.35f;	/* a wide screen keeps more ceiling */
	u = (float) conc_iw / conc_tw;
	v = (float) conc_ih / conc_th;

	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_TEXTURE_BIT |
		      GL_VIEWPORT_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT);
	glViewport (0, top, pw, (int) ph);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, pw, ph, 0, -1, 1);		/* pixels, y down */
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glDisable (GL_FOG);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable (GL_TEXTURE_2D);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glBindTexture (GL_TEXTURE_2D, conc_tex);

	/* the picture, its ceiling opening tinted to the world or sky above */
	{
		const float ys[4] = { 0, p->ceil, p->ceil + 0.08f, 1 };
		glBegin (GL_QUAD_STRIP);
		for (i = 0; i < 4; i++) {
			c = i < 2 ? 1 : 0;
			glColor4f (1 + (rgb[0] - 1) * c, 1 + (rgb[1] - 1) * c, 1 + (rgb[2] - 1) * c, fade);
			y = y0 + ys[i] * dh;
			glTexCoord2f (0, ys[i] * v); glVertex2f (x0, y);
			glTexCoord2f (u, ys[i] * v); glVertex2f (x0 + dw, y);
		}
		glEnd ();
	}

	/* the signs: the port's name on the yellow one, the date on the blue */
	for (k = 0; k < 2; k++) {
		for (i = 0; i < 4; i++) {
			q[2 * i] = x0 + p->sign[k][2 * i] * dw;
			q[2 * i + 1] = y0 + p->sign[k][2 * i + 1] * dh;
		}
		sign_text (k, k ? date : name, q, fade);
	}

	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
}

/* --- the page colour the game draws itself --------------------------- */

/* The page fill (Call_MemsetBlue) leaves the transparent index, but the
 * game also blits the page colour as the margin of a button or an arrow.
 * On the flat page that margin is the page; over the picture it would be a
 * purple block.  So: flood from the page fill through the page colour, 4
 * neighbours, and clear what is reached.  Page colour walled in by
 * something else (a button's face inside its bevel, an arrow inside its
 * box) is part of that thing and stays.  The flood walks through `out`,
 * the copy the other layers have already edited, so a glyph hdtext took
 * out of the page joins the page; it starts only from pixels the game
 * itself filled (255 in the raster). */
void hdback_frame (const unsigned char *raster, unsigned char *out)
{
	static unsigned short q[ST_W * PAGE_ROWS];
	static unsigned char hit[ST_W * PAGE_ROWS];
	static int last_cleared = -1, last_kept = -1;
	int head = 0, tail = 0, i, cleared = 0, kept = 0;

	if (!drew) return;
	drew = 0;	/* one frame's picture, one frame's hook */
	if (fe2_bgcol != PAGE_COLOUR || (rb (A6 + A6_SPACE_MODE) & 0x01))
		return;

	memset (hit, 0, sizeof hit);
	for (i = 0; i < ST_W * PAGE_ROWS; i++)
		if (raster[i] == CLEAR) {
			hit[i] = 1;
			q[tail++] = (unsigned short) i;
		}
	while (head < tail) {
		int p = q[head++], x = p % ST_W, nb[4], k;

		nb[0] = x > 0 ? p - 1 : -1;
		nb[1] = x < ST_W - 1 ? p + 1 : -1;
		nb[2] = p - ST_W;
		nb[3] = p + ST_W < ST_W * PAGE_ROWS ? p + ST_W : -1;
		for (k = 0; k < 4; k++) {
			int j = nb[k];
			if (j < 0 || hit[j]) continue;
			if (out[j] != PAGE_COLOUR && out[j] != CLEAR) continue;
			hit[j] = 1;
			q[tail++] = (unsigned short) j;
		}
	}
	for (i = 0; i < ST_W * PAGE_ROWS; i++) {
		if (out[i] != PAGE_COLOUR) continue;
		if (hit[i]) {
			out[i] = CLEAR;
			cleared++;
		} else
			kept++;
	}
	if (debug && (cleared != last_cleared || kept != last_kept)) {
		fprintf (stderr, "hdback: page colour drawn by the game: %d cleared, %d kept (enclosed)\n",
			 cleared, kept);
		last_cleared = cleared;
		last_kept = kept;
	}
}
