/*
 * hdback: see hdback.h and docs/hd/backdrop.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "hdback.h"
#include "art.h"
#include "../m68000.h"
#include "../fixups.h"

/* the game's top of memory (host.h) */
#define ST_MEM_SIZE	0x110000

extern int fe2_bgcol;

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
#define OBJ_OFFSETS		__DL44b02	/* word per index: slot offset */

#define OBJ_SIZE		286
#define OBJ_SLOTS		115	/* the offset table has 0..114 */
#define OBJ_PARENT		68	/* byte: index of the body it orbits */
#define OBJ_MODEL		90	/* word */
#define OBJ_INDEX		93	/* byte: its own index */
#define OBJ_SEED		118	/* long */
#define OBJ_TEMP		182	/* word: kelvin */
#define OBJ_FLAGS		280	/* byte */

#define STATE_DOCKED		0x54
#define STATE_LANDED		0x60
#define STATE_LANDED_ROUGH	0x68

#define PAGE_COLOUR		0xe	/* fe2_bgcol after a flat page fill */

enum kind { K_NONE, K_ORBITAL, K_SURFACE };
static const char *kind_name[] = { "none", "orbital", "surface" };

enum light { L_NONE, L_RED, L_ORANGE, L_YELLOW, L_WHITE, L_BLUE, L_MAX };
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

static int is_star (int model) { return model >= 0x98 && model <= 0xae; }

static enum light star_light (int model)
{
	switch (model) {
	case 0x98:		/* brown dwarf */
	case 0x9a:		/* M dwarfs */
	case 0x9c:		/* red giant */
		return L_RED;
	case 0x9e:		/* K */
		return L_ORANGE;
	case 0xa0:		/* G */
		return L_YELLOW;
	case 0xa2:		/* F */
	case 0xa4:		/* A */
	case 0xa8:		/* bright giant */
	case 0xaa:		/* supergiant */
	case 0xae:		/* white dwarf */
		return L_WHITE;
	case 0xa6:		/* B */
	case 0xac:		/* blue supergiant */
		return L_BLUE;
	}
	return L_NONE;
}

/* the look of a body, by the model the game draws it with */
static const char *body_class (int model)
{
	switch (model) {
	case 0x7a: case 0x80: return "airless";	/* bare rock, no air */
	case 0x7e: return "rocky";		/* thin dusty air, desert */
	case 0x84: return "corrosive";		/* thick hot or ammonia murk */
	case 0x86: return "methane";		/* cold orange smog */
	case 0x88: case 0x8a: return "terran";	/* oxygen, oceans, cloud */
	case 0x8c: return "ice";		/* oxygen but frozen */
	case 0x8e: return "ocean";		/* corrosive water world */
	case 0x92: case 0x94: case 0x96: return "gasgiant";
	}
	return is_star (model) ? "star" : NULL;
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
 * with no picture borrows the one the manifest names, to be tinted. */
static const struct art *pick (const struct where *w, enum light *drawn)
{
	const struct art *found[MAX_ART];
	char cls[2][24];
	enum light l;
	unsigned int h;
	int c, n, tries, n_cls = 0;

	if (!manifest_read) read_manifest ();
	if (!w->kind || !w->cls || !w->light) return NULL;
	if (w->moon)
		snprintf (cls[n_cls++], sizeof cls[0], "%s-moon", w->cls);
	snprintf (cls[n_cls++], sizeof cls[0], "%s", w->cls);
	for (c = 0; c < n_cls; c++) {
		l = w->light;
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

static GLuint tex;
static int tex_w, tex_h, img_w, img_h;
static char tex_file[64];	/* what tex holds, or failed to load */

void hdback_forget (void)
{
	if (tex) glDeleteTextures (1, &tex);
	tex = 0;
	tex_file[0] = 0;
}

static int pow2 (int n)
{
	int p = 1;
	while (p < n) p <<= 1;
	return p;
}

static int load (const char *file)
{
	char rel[96];
	unsigned char *rgba;
	int w, h;

	if (!strcmp (file, tex_file)) return tex != 0;
	hdback_forget ();
	snprintf (tex_file, sizeof tex_file, "%s", file);
	snprintf (rel, sizeof rel, "backdrops/%s", file);
	rgba = art_load_rgba (rel, &w, &h);
	if (!rgba) {
		fprintf (stderr, "hdback: cannot read %s\n", art_path (rel));
		return 0;
	}
	img_w = w;
	img_h = h;
	tex_w = pow2 (w);
	tex_h = pow2 (h);
	glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free (rgba);
	return 1;
}

/* --- drawing ---------------------------------------------------------- */

/* the picture's brightness under the page, and the dark glass over it
 * (#0b1016) that keeps the game's text readable */
#define BACK_GAIN	0.80f
#define SCRIM_ALPHA	0.50f

static int debug = -1;

static void log_where (const struct where *w, int page,
		       const struct art *a, enum light drawn)
{
	static char last[512];
	char buf[512];

	snprintf (buf, sizeof buf,
		  "hdback: state=$%02x mode=$%02x bgcol=%d page=%d port=#%d model=$%02x flags=$%02x %s"
		  " body=#%d model=$%02x %dK class=%s chain=%s star=#%d model=$%02x light=%s"
		  " -> key %s-%s-%s %s%s%s",
		  w->state, rb (A6 + A6_SPACE_MODE), fe2_bgcol, page,
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

void hdback_draw (int fb_w, int fb_h)
{
	static int off = -1;
	struct where w;
	const struct art *a = NULL;
	enum light drawn = L_NONE;
	static const float no_tint[3] = { 1.0f, 1.0f, 1.0f };
	const float *t;
	float s, dw, dh, x0, y0, u, v, k;
	GLint env;
	int page;

	if (off < 0) {
		const char *e = getenv ("FE2_HD_BACKDROP");
		const char *d = getenv ("FE2_HD_DEBUG");
		off = e && !strcmp (e, "0");
		debug = d && *d && strcmp (d, "0");
	}
	if (pixel_art || off) return;

	locate (&w);
	/* a flat page: the page colour fill, and no 3D view filling the
	 * screen each frame (the 3D view, galactic map and system view do) */
	page = fe2_bgcol == PAGE_COLOUR && !(rb (A6 + A6_SPACE_MODE) & 0x01);
	if (w.kind) a = pick (&w, &drawn);
	if (debug && w.state) log_where (&w, page, a, drawn);
	if (!page || !a || !load (a->file)) return;

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

	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f (0x0b / 255.0f, 0x10 / 255.0f, 0x16 / 255.0f, SCRIM_ALPHA);
	glBegin (GL_TRIANGLE_STRIP);
		glVertex2f (0, 0);
		glVertex2f (fb_w, 0);
		glVertex2f (0, fb_h);
		glVertex2f (fb_w, fb_h);
	glEnd ();

	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glPopAttrib ();
}
