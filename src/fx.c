/*
 * fx: HD weapon effects for the GL renderer: see fx.h and docs/hd/weapons.md.
 * The put side reads the object being drawn and tags its primitive; the draw
 * side turns tagged primitives into sprites and ribbons of two procedural
 * textures.  A beam and its flares outlive their frame in a ring aged on game
 * time: held while paused, gone with any change of screen or view.
 */
#include <SDL.h>
#include <SDL_opengl.h>

#include "main.h"
#include "../m68000.h"
#include "../host.h"	/* MEM_SIZE */
#include "../fixups.h"	/* __DL5eb6_a6_base */
#include "fx.h"

/* ---- the game's state, read only ---------------------------------------- */

#define A6		__DL5eb6_a6_base
#define A6_TOOLS_MODE	42	/* byte: negative while a 3D view fills the screen */
#define A6_START_FLAGS	598	/* byte: bit 5 set in the intro and the start menu */
#define A6_VIEW		706	/* byte: 0 front, 1 rear, 2/3 turrets, 4 external */
#define A6_TIME_RATE	10428	/* long: 0 while the game is paused */
#define FRAME_OBJ	212	/* long at A6-212 during a put: the object drawn */
#define OBJ_MODEL	90	/* word */
#define OBJ_AGE		122	/* byte: a smoke puff's age, $7f down to 0 */
#define OBJ_LASER	123	/* byte: a beam's laser type */
#define OBJ_HIT		186	/* word: $ffff when a beam hit nothing */

/* the models with effects: those seen in flight (docs/hd/weapons.md) */
#define M_MISSILE	10	/* the homing missile */
#define M_LASER		190	/* a laser beam, put each frame of a shot */
#define M_SMOKE		194	/* smoke and sparks, also a missile's trail */
#define PULSE_LASER	0x88	/* the one laser type the look was tuned on */

static int rb (unsigned int a) { return STMemory_ReadByte (a) & 0xff; }
static int readable (unsigned int a, unsigned int n) { return a <= MEM_SIZE && n <= MEM_SIZE - a; }

/* a game is on (not the intro or start menu) and a 3D view fills the screen
 * (not a map or a 2D page); asked at put time, as this changes mid frame */
static int in_flight (void) { return !(rb (A6 + A6_START_FLAGS) & 0x20) && (signed char) rb (A6 + A6_TOOLS_MODE) < 0; }

/* the model of the object being put, 0 for none; its record in *rec */
static int put_model (unsigned int *rec)
{
	unsigned int a6 = GetReg (REG_A6);
	if (flat_fx || !in_flight () || !readable (a6 - FRAME_OBJ, 4)) return 0;
	*rec = STMemory_ReadLong (a6 - FRAME_OBJ);
	return readable (*rec, 192) ? STMemory_ReadWord (*rec + OBJ_MODEL) & 0xffff : 0;
}

/* an integer hash, 0..1 (never the game's random numbers) */
static float hashf (unsigned int a, unsigned int b)
{
	unsigned int h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
	h = (h ^ h >> 15) * 0x2c1b3c6du;
	return ((h ^ h >> 12) & 0xffff) / 65535.0f;
}

/* smooth value noise on the hash lattice */
static float vnoise (float x, float y)
{
	int i = (int) x, j = (int) y;
	float u = (x - i) * (x - i) * (3 - 2 * (x - i)), v = (y - j) * (y - j) * (3 - 2 * (y - j));
	float a = hashf (i, j) + (hashf (i + 1, j) - hashf (i, j)) * u;
	float b = hashf (i, j + 1) + (hashf (i + 1, j + 1) - hashf (i, j + 1)) * u;
	return a + (b - a) * v;
}

enum { T_GLOW, T_PUFF, T_N, T_BEAM = -1 };	/* T_BEAM: a ring entry drawn as a beam */
static GLuint tex[T_N];

static void make_textures (void)
{
	static unsigned char px[64 * 64 * 2];
	int t, i;
	glGenTextures (T_N, tex);
	for (t = 0; t < T_N; t++) {
		for (i = 0; i < 64 * 64; i++) {
			float u = (i % 64 + 0.5f) / 32 - 1, v = (i / 64 + 0.5f) / 32 - 1;
			float n = 0.65f * vnoise (3 * u + 8, 3 * v + 8) + 0.35f * vnoise (7 * u + 8, 7 * v + 8);
			float r = sqrtf (u * u + v * v), a = 0;
			if (t == T_GLOW)	/* two gaussians */
				a = 0.55f * expf (-4 * r * r) + 0.45f * expf (-28 * r * r);
			else if (r < 1)		/* noise in a soft disc */
				a = powf (1 - r, 1.2f) * (0.3f + 1.1f * n);
			px[2 * i] = t == T_PUFF ? 255 * (0.65f + 0.35f * n) : 255;
			px[2 * i + 1] = a > 1 ? 255 : 255 * a;
		}
		glBindTexture (GL_TEXTURE_2D, tex[t]);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 64, 64, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, px);
	}
}

/* ---- drawing, in eye space (the 3D pass's modelview is identity) --------- */

static int vp_h = 480;		/* the 3D viewport's height, last frame */

/* the world size of one pixel at depth z: 2 tan (36.5 / 2 degrees) */
static float px_world (float z) { return fabsf (z) * 0.6595f / vp_h; }
static float c8 (int v) { return (v & 255) / 255.0f; }	/* the reader's chars are signed */
static void col (float r, float g, float b, float a) { glColor4f (r, g, b, a > 1 ? 1 : a); }

static float dist2 (const float *a, const float *b)
{
	return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
}

/* additive light, or plain alpha for smoke; texture times colour */
static void blend (int additive, int t)
{
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
	if (!tex[0]) make_textures ();
	glEnable (GL_TEXTURE_2D);
	glBindTexture (GL_TEXTURE_2D, tex[t]);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void blend_end (void)
{
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable (GL_BLEND);
	glDisable (GL_TEXTURE_2D);
}

/* a square of the bound texture facing the eye, never over half the view */
static void sprite (const float *p, float r, float rot)
{
	float c = cosf (rot), s = sinf (rot);
	int k;
	if (p[2] > -1) return;
	r = fminf (r, 0.5f * vp_h * px_world (p[2]));
	glBegin (GL_QUADS);
	for (k = 0; k < 4; k++) {
		float x = k == 1 || k == 2 ? 1 : -1, y = k < 2 ? -1 : 1;
		glTexCoord2f (x > 0, y > 0);
		glVertex3f (p[0] + (x * c - y * s) * r, p[1] + (x * s + y * c) * r, p[2]);
	}
	glEnd ();
}

/* a strip of glow from a to b, wa and wb pixels wide at its ends; texture q is
 * the depth, so a beam spanning 2000:1 in depth keeps its core */
static void ribbon (const float *a, const float *b, float wa, float wb)
{
	float s[3], l;
	int k;
	if (a[2] > -1 || b[2] > -1) return;
	/* across the strip: the line (b - a) crossed with the eye's ray to a */
	s[0] = b[1] * a[2] - b[2] * a[1];
	s[1] = b[2] * a[0] - b[0] * a[2];
	s[2] = b[0] * a[1] - b[1] * a[0];
	l = sqrtf (s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
	if (!(l > 0)) return;
	glBegin (GL_TRIANGLE_STRIP);
	for (k = 0; k < 4; k++) {
		const float *e = k < 2 ? a : b;
		float h = (k & 1 ? 1.25f : -1.25f) * (k < 2 ? wa : wb) * px_world (e[2]) / l;
		glTexCoord4f ((k & 1) * -e[2], 0.5f * -e[2], 0, -e[2]);
		glVertex3f (e[0] + s[0] * h, e[1] + s[1] * h, e[2] + s[2] * h);
	}
	glEnd ();
}

static unsigned int fx_ms;	/* game time in ms: stands still while paused */

#define NR	32
static struct effect {
	unsigned int t0, fade;	/* fade 0: a free slot */
	int t;			/* T_BEAM or a glow, size pixels across growing by grow */
	float a[3], b[3], size, grow, rgb[3];
} ring[NR];

/* a new entry, over the oldest when the ring is full; b is a beam's far end */
static void effect_new (const float *p, const float *b, int t, float size, float grow, unsigned int fade,
			float cr, float cg, float cb)
{
	struct effect *e = &ring[0];
	int i;
	for (i = 1; i < NR && e->fade; i++)
		if (!ring[i].fade || ring[i].t0 < e->t0) e = &ring[i];
	*e = (struct effect) { .t0 = fx_ms, .fade = fade, .t = t, .size = size, .grow = grow, .rgb = { cr, cg, cb } };
	memcpy (e->a, p, sizeof e->a);
	memcpy (e->b, b, sizeof e->b);
}

/* a new screen or view: nothing from the last one stays */
static int scene = -1;		/* 0 no flight view, else 1 + the view */
static void check_scene (void)
{
	int now = in_flight () ? 1 + rb (A6 + A6_VIEW) : 0;
	if (now != scene) memset (ring, 0, sizeof ring);
	scene = now;
}

/* ---- the put side ---------------------------------------------------------- */

static unsigned int last_tick;
static int drew_3d;		/* this frame has a 3D view */

/* this frame's missile bodies and smoke puffs, for the plumes */
static struct missile { unsigned int rec, n; float v[24][3]; } missiles[4];
static float smoke[256][3];
static int n_missiles, n_smoke;

void fx_frame_begin (void)
{
	unsigned int now = SDL_GetTicks (), d = now - last_tick;
	last_tick = now;
	drew_3d = 1;
	n_missiles = n_smoke = 0;
	check_scene ();
	if (STMemory_ReadLong (A6 + A6_TIME_RATE) != 0)
		fx_ms += d > 50 ? 50 : d;
}

int fx_tag_for (unsigned int tag[3])
{
	unsigned int rec;
	int m = put_model (&rec);
	if (m != M_MISSILE && m != M_SMOKE) return 0;
	tag[0] = m;
	tag[1] = m == M_SMOKE ? rb (rec + OBJ_AGE) : 0;
	tag[2] = rec;
	return 1;
}

/* a pulse laser's beam: a halo in the game's hue (D6) round a near-white core,
 * a muzzle bloom and, on a hit, a flare; other laser types keep the base look */
int fx_laser_quad (void)
{
	float v[4][3], ends[2][3], *n, *f, c[3], t;
	unsigned int rec, i, k;
	int rgb = GetReg (REG_D6), j;
	if (put_model (&rec) != M_LASER || rb (rec + OBJ_LASER) != PULSE_LASER) return 0;
	for (i = 0; i < 4; i++) {
		if (!readable (GetReg (REG_A0 + i) + 4, 12)) return 0;
		for (k = 0; k < 3; k++)
			v[i][k] = (k == 2 ? -1.0f : 1.0f) * STMemory_ReadLong (GetReg (REG_A0 + i) + 4 + 4 * k);
	}
	/* the corners go round the quad: its ends are the pair of opposite
	 * edges, 0-1 and 2-3 or 1-2 and 3-0, farther apart in depth */
	j = fabsf (v[0][2] + v[1][2] - v[2][2] - v[3][2]) < fabsf (v[1][2] + v[2][2] - v[3][2] - v[0][2]);
	for (k = 0; k < 3; k++) {
		ends[0][k] = (v[j][k] + v[j + 1][k]) / 2;
		ends[1][k] = (v[j + 2][k] + v[(j + 3) & 3][k]) / 2;
	}
	n = ends[fabsf (ends[0][2]) > fabsf (ends[1][2])];	/* the near end */
	f = ends[n == ends[0]];
	if (n[2] > -1.5f && f[2] < -1.5f) {	/* the muzzle, kept off the near plane */
		t = (-1.5f - n[2]) / (f[2] - n[2]);
		for (k = 0; k < 3; k++)
			n[k] += (f[k] - n[k]) * t;
	}
	for (i = 0; i < NR; i++)	/* the beam's second quad this frame */
		if (ring[i].fade && ring[i].t == T_BEAM && ring[i].t0 == fx_ms &&
		    fabsf (ring[i].a[0] - n[0]) + fabsf (ring[i].a[2] - n[2]) < 0.1f * fabsf (f[2] - n[2]) + 1)
			return 1;
	for (k = 0; k < 3; k++)		/* the game's colour, $0rgb */
		c[k] = ((rgb >> (8 - 4 * k)) & 15) / 15.0f;
	effect_new (n, f, T_BEAM, 0, 0, 90, c[0], c[1], c[2]);
	effect_new (n, n, T_GLOW, 20, 0.3f, 90, 0.6f + 0.4f * c[0], 0.6f + 0.4f * c[1], 0.6f + 0.4f * c[2]);
	if ((STMemory_ReadWord (rec + OBJ_HIT) & 0xffff) != 0xffff)
		effect_new (f, f, T_GLOW, 25, 0.6f, 180, 1, 0.85f, 0.55f);
	return 1;
}

/* ---- the draw side ---------------------------------------------------------- */

static struct { unsigned int kind, aux, rec; } cur;	/* the tag in force */
static int in_complex;

void fx_draw_tag (void **data)
{
	memcpy (&cur, *data, sizeof cur);
	*data = (char *) *data + sizeof cur;
}

int fx_tagged (void) { return cur.kind != 0; }

/* a tag holds for one primitive, or for a shape to its end */
void fx_prim_done (int mark)
{
	if (mark == 1 && cur.kind) in_complex = 1;
	else if (!in_complex || mark == 2) in_complex = cur.kind = 0;
}

/* a missile's body, kept for its plume, is drawn as it was */
int fx_tri (const float *a, const float *b, const float *c)
{
	const float *p[3] = { a, b, c };
	struct missile *m = missiles;
	int i;
	if (cur.kind != M_MISSILE) return 0;
	while (m < missiles + n_missiles && m->rec != cur.rec) m++;
	if (m == missiles + 4) return 0;
	if (m == missiles + n_missiles) {
		n_missiles++;
		m->rec = cur.rec;
		m->n = 0;
	}
	for (i = 0; i < 3 && m->n < 24; i++)
		memcpy (m->v[m->n++], p[i], sizeof m->v[0]);
	return 0;
}

/* smoke: a grey puff of noise that spreads and thins as it ages */
int fx_blob (const int *iv, int rad, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] };
	float R = 0.002f * (rad < 2 ? 2 : rad) * fabsf (v[2]), a = (cur.aux & 0x7f) / 127.0f;
	float grey = (c8 (r) + c8 (g) + c8 (b)) / 3 * 0.7f + 0.15f;
	if (cur.kind != M_SMOKE) return 0;
	if (n_smoke < 256) memcpy (smoke[n_smoke++], v, sizeof v);
	blend (0, T_PUFF);
	col (grey, grey * 0.97f, grey * 0.93f, 0.85f * a + 0.1f);
	sprite (v, R * (1.6f - 0.6f * a), hashf (cur.rec, rad) * 6.283f + fx_ms * 0.0004f);
	blend_end ();
	return 1;
}

/* A missile burns where its smoke starts: a plume off the end of its body
 * nearer the smoke.  One with no smoke near it is coasting: no plume. */
static void draw_plume (const struct missile *m)
{
	const float *a = m->v[0], *b = m->v[0], *tail, *nose;
	float best = 1e30f, flick = 0.8f + 0.4f * hashf (m->rec, fx_ms / 33), w, end[3], core[3];
	unsigned int i, k;
	int near_a = 0;
	for (i = 0; i < m->n; i++)	/* the body's long axis */
		for (k = i + 1; k < m->n; k++)
			if (dist2 (m->v[i], m->v[k]) > dist2 (a, b)) a = m->v[i], b = m->v[k];
	for (k = 0; k < (unsigned int) n_smoke; k++) {
		float da = dist2 (smoke[k], a), db = dist2 (smoke[k], b);
		if (fminf (da, db) < best) best = fminf (da, db), near_a = da < db;
	}
	if (!(dist2 (a, b) > 0) || best > 36 * dist2 (a, b)) return;
	tail = near_a ? a : b;
	nose = near_a ? b : a;
	for (k = 0; k < 3; k++) {
		end[k] = tail[k] + (tail[k] - nose[k]) * 1.3f * flick;
		core[k] = tail[k] + (tail[k] - nose[k]) * 0.5f * flick;
	}
	w = fmaxf (2, fminf (0.22f * sqrtf (dist2 (a, b)) / px_world (tail[2]), 36)) * flick;
	col (0.3f, 0.85f, 1, 0.85f);	/* the flame */
	ribbon (tail, end, w, 0);
	col (0.79f, 0.955f, 1, 1);	/* its white core and the nozzle */
	ribbon (tail, core, w * 0.35f, 0);
	sprite (tail, 0.8f * w * px_world (tail[2]), 0);
}

static void draw_effect (const struct effect *e)
{
	float age = (float) (fx_ms - e->t0) / e->fade;
	const float *c = e->rgb;
	if (e->t != T_BEAM) {
		col (c[0], c[1], c[2], 1 - age);
		sprite (e->a, e->size * (1 + e->grow * age) * px_world (e->a[2]), 0);
		return;
	}
	/* a beam is held bright, then gone: a halo round a near-white core */
	age = age * age;
	col (c[0], c[1], c[2], 1 - age);
	ribbon (e->a, e->b, 40, 14);
	col (0.4f * c[0] + 0.6f, 0.4f * c[1] + 0.6f, 0.4f * c[2] + 0.6f, 1 - age);
	ribbon (e->a, e->b, 5, 2.4f);
}

void fx_frame_end (void)
{
	GLint vp[4];
	int i, had_3d = drew_3d;
	cur.kind = in_complex = drew_3d = 0;
	if (flat_fx) return;
	check_scene ();
	if (!scene || !had_3d) return;	/* no flight view this frame */
	glGetIntegerv (GL_VIEWPORT, vp);
	vp_h = vp[3] > 0 ? vp[3] : 480;
	blend (1, T_GLOW);
	for (i = 0; i < n_missiles; i++)
		draw_plume (&missiles[i]);
	for (i = 0; i < NR; i++)
		if (ring[i].fade && fx_ms - ring[i].t0 >= ring[i].fade) ring[i].fade = 0;
		else if (ring[i].fade) draw_effect (&ring[i]);
	blend_end ();
}

void fx_forget (void)
{
	if (tex[0]) glDeleteTextures (T_N, tex);
	memset (tex, 0, sizeof tex);
	scene = -1;
}
