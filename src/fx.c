/* fx: HD weapon effects for the GL renderer (fx.h, docs/hd/weapons.md).  The put
 * side reads the object drawn and tags its primitive; the draw side turns tagged
 * primitives into sprites and ribbons of two procedural textures.  Beams and
 * flares outlive their frame in a ring aged on game time (held while paused). */
#include <SDL.h>
#include <SDL_opengl.h>
#include "main.h"
#include "../m68000.h"
#include "../host.h"	/* MEM_SIZE */
#include "../fixups.h"	/* __DL5eb6_a6_base */
#include "fx.h"
#include "hull.h"	/* hull_intro_part */
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
/* the models with effects (docs/hd/weapons.md) */
#define MISSILE(m)	((m) >= 10 && (m) <= 16 && !((m) & 1))	/* homing, smart, naval, nuclear */
#define M_EXPL		188	/* an explosion */
#define M_LASER		190	/* a laser beam, put each frame of a shot */
#define M_SMOKE		194	/* smoke and sparks, also a missile's trail */
#define I_FIRE(w)	((w) == 114 || (w) == 116)	/* the intro's fireballs (parts in its table) */
/* The look of each type.  A laser's class is bits 3-6 of its type byte (the
 * game's own damage table is indexed so): 1-3 are pulse lasers ($88 the start
 * ship's, $9a mining), 4-9 beam lasers ($a1 the police's) and accelerators.
 * A laser: hue, core and halo widths in pixels, shimmer in Hz (0 steady).
 * A missile (16 + (model - 10) / 2): flame hue, core and flame widths as a
 * share of its body's length, flicker in Hz.  Procedural: no art. */
static const struct look { float rgb[3], core, halo, hz; } looks[20] = {
	{ { 1, .2f, .1f }, 4, 32, 0 }, { { 1, .2f, .1f }, 5, 40, 0 },	/* 0 (none seen), 1: pulse, red */
	{ { 1, .45f, .1f }, 6, 48, 0 }, { { 1, .8f, .25f }, 4, 30, 8 },	/* 2: orange; 3: mining, amber, throbs */
	{ { .3f, 1, .4f }, 4, 34, 0 }, { { .2f, .85f, 1 }, 5, 42, 0 },	/* 4: beam, green; 5: cyan */
	{ { .35f, .5f, 1 }, 6, 50, 0 }, { { .7f, .4f, 1 }, 7, 58, 0 },	/* 6: blue; 7: violet */
	{ { 1, .45f, .9f }, 8, 66, 12 }, { { 1, .6f, 1 }, 10, 80, 12 },	/* 8, 9 (and 10-15): plasma, crackles */
	[16] = { { .3f, .85f, 1 }, .08f, .22f, 30 }, { { .4f, 1, .55f }, .08f, .24f, 40 },	/* homing blue, smart green */
	{ { 1, .6f, .25f }, .1f, .3f, 25 }, { { 1, .35f, .75f }, .12f, .4f, 20 },	/* naval orange, nuclear magenta */
};

static unsigned int fx_ms;	/* game time in ms: stands still while paused */
static int rb (unsigned int a) { return STMemory_ReadByte (a) & 0xff; }
static int readable (unsigned int a, unsigned int n) { return a <= MEM_SIZE && n <= MEM_SIZE - a; }
/* a game is on (not the intro or start menu) and a 3D view fills the screen
 * (not a map or a 2D page); asked at put time, as this changes mid frame */
static int in_flight (void) { return !(rb (A6 + A6_START_FLAGS) & 0x20) && (signed char) rb (A6 + A6_TOOLS_MODE) < 0; }
/* the model of the object being put, 0 for none; its record in *rec.  Besides
 * the flight view, the death camera (state $58 at 704(a6)) is a 3D view too:
 * there its explosions (every one, not only the player's) and smoke are fx's. */
#define A6_STATE	704
static int intro_fire;	/* put_model's last tag was the intro's fireball */
static int put_model (unsigned int *rec)
{
	unsigned int a6 = GetReg (REG_A6);
	int m;
	if (flat_fx || !readable (a6 - FRAME_OBJ, 4)) return 0;
	*rec = STMemory_ReadLong (a6 - FRAME_OBJ);
	/* the intro's scenes (bit 5 of 598(a6), and its own table): by the part's model */
	intro_fire = 0;
	if ((rb (A6 + A6_START_FLAGS) & 0x20) && I_FIRE (hull_intro_part ()))
		return (intro_fire = readable (*rec, 256)) ? M_EXPL : 0;
	if ((signed char) rb (A6 + A6_TOOLS_MODE) >= 0) return 0;
	m = readable (*rec, 256) ? STMemory_ReadWord (*rec + OBJ_MODEL) & 0xffff : 0;
	return in_flight () || ((m == M_EXPL || m == M_SMOKE) && rb (A6 + A6_STATE) == 0x58) ? m : 0;
}
/* an integer hash, 0..1 (never the game's random numbers) */
static float hashf (unsigned int a, unsigned int b)
{
	unsigned int h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
	h = (h ^ h >> 15) * 0x2c1b3c6du; return ((h ^ h >> 12) & 0xffff) / 65535.0f;
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

enum { T_GLOW, T_PUFF, T_BALL, T_N, T_BEAM = -1 };	/* T_BEAM: a ring entry drawn as a beam */
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
			float r = sqrtf (u * u + v * v), a = t == T_GLOW ? 0.55f * expf (-4 * r * r) + 0.45f * expf (-28 * r * r)
				: t == T_PUFF ? (r < 1 ? powf (1 - r, 1.2f) * (0.3f + 1.1f * n) : 0)
				: (0.9f - r + 0.2f * (n - 0.5f)) / 0.25f;	/* two gaussians; noise in a soft disc; a full disc, ragged */
			px[2 * i] = t == T_GLOW ? 255 : 255 * (t == T_PUFF ? 0.65f + 0.35f * n : 0.8f + 0.2f * n);
			px[2 * i + 1] = a > 1 ? 255 : a < 0 ? 0 : 255 * a;
		}
		glBindTexture (GL_TEXTURE_2D, tex[t]);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 64, 64, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, px);
	}
}
static int vp_w = 640, vp_h = 480;	/* the 3D viewport, last frame */
/* the world size of one pixel at depth z: 2 tan (36.5 / 2 degrees) */
static float px_world (float z) { return fabsf (z) * 0.6595f / vp_h; }
static float c8 (int v) { return (v & 255) / 255.0f; }	/* the reader's chars are signed */
static void col (float r, float g, float b, float a) { glColor4f (r, g, b, a > 1 ? 1 : a); }

static float dist2 (const float *a, const float *b)
	{ return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]); }
/* lighting off for a sprite drawn in the hull pass (on, GL ignores glColor);
 * unlit () returns how it was, for relight () */
static int unlit (void) { int l = glIsEnabled (GL_LIGHTING); glDisable (GL_LIGHTING); return l; }
static void relight (int l) { if (l) glEnable (GL_LIGHTING); }
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
static void blend_end (void) { glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable (GL_BLEND); glDisable (GL_TEXTURE_2D); }
/* a square of the bound texture facing the eye, never over half the view */
static void sprite (const float *p, float r, float rot)
{
	float c = cosf (rot), s = sinf (rot); int k;
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
	float s[3], l; int k;
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

#define NR	32
static struct effect {
	unsigned int t0, fade;	/* fade 0: a free slot */
	int t;			/* T_BEAM or a glow, size pixels across growing by grow */
	float a[3], b[3], size, grow, rgb[3];
	const struct look *lk;	/* a beam's */
} ring[NR];

/* a new entry, over the oldest when the ring is full; b is a beam's far end */
static struct effect *effect_new (const float *p, const float *b, int t, float size, float grow, unsigned int fade,
			float cr, float cg, float cb)
{
	struct effect *e = &ring[0];
	int i;
	for (i = 1; i < NR && e->fade; i++)
		if (!ring[i].fade || ring[i].t0 < e->t0) e = &ring[i];
	*e = (struct effect) { .t0 = fx_ms, .fade = fade, .t = t, .size = size, .grow = grow, .rgb = { cr, cg, cb } };
	memcpy (e->a, p, sizeof e->a);
	memcpy (e->b, b, sizeof e->b);
	return e;
}

/* a new screen or view: nothing from the last one stays */
static int scene = -1;		/* 0 no flight view, else 1 + the view */
static void check_scene (void)
{
	int now = in_flight () ? 1 + rb (A6 + A6_VIEW) : 0;
	if (now != scene) memset (ring, 0, sizeof ring), scene = now;
}

static unsigned int last_tick, n_frames;
static int drew_3d;		/* this frame has a 3D view */
/* this frame's missile bodies and smoke puffs, for the plumes */
static struct missile { unsigned int rec, kind, n; float v[24][3]; } missiles[4];
static float smoke[256][3];
static int n_missiles, n_smoke;
/* this frame's explosion blobs, and last frame's missiles (fx_circle) */
static struct ball { unsigned int rec, missile; float v[3], R, a; } balls[64];
static float mprev[4][3];
static int n_balls, n_mprev;
void fx_frame_begin (void)
{
	unsigned int now = SDL_GetTicks (), d = now - last_tick;
	last_tick = now, n_frames++;
	drew_3d = 1;
	for (n_mprev = 0; n_mprev < n_missiles; n_mprev++) memcpy (mprev[n_mprev], missiles[n_mprev].v[0], sizeof mprev[0]);
	n_missiles = n_smoke = n_balls = 0;
	check_scene ();
	if (STMemory_ReadLong (A6 + A6_TIME_RATE) != 0) fx_ms += d > 50 ? 50 : d;
}

int fx_tag_for (unsigned int tag[3])
{
	unsigned int rec;
	int m = put_model (&rec);
	if (!MISSILE (m) && m != M_SMOKE && (m != M_EXPL || flat_explosions)) return 0;
	tag[0] = m, tag[1] = m == M_SMOKE ? rb (rec + OBJ_AGE) : intro_fire, tag[2] = rec;
	return 1;
}

/* a laser's beam: a halo in its type's hue round a near-white core, a muzzle
 * bloom and, on a hit, a flare */
int fx_laser_quad (void)
{
	float v[4][3], ends[2][3], *n, *f, t;
	const struct look *lk;
	unsigned int rec, i, k;
	int j = put_model (&rec) == M_LASER ? rb (rec + OBJ_LASER) >> 3 & 15 : -1;	/* its class */
	if (j < 0) return 0;
	lk = &looks[j > 9 ? 9 : j];
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
	t = n[2] > -1.5f && f[2] < -1.5f ? (-1.5f - n[2]) / (f[2] - n[2]) : 0;	/* the muzzle, off the near plane */
	for (k = 0; k < 3; k++) n[k] += (f[k] - n[k]) * t;
	for (i = 0; i < NR; i++)	/* the beam's second quad this frame */
		if (ring[i].fade && ring[i].t == T_BEAM && ring[i].t0 == fx_ms &&
		    fabsf (ring[i].a[0] - n[0]) + fabsf (ring[i].a[2] - n[2]) < 0.1f * fabsf (f[2] - n[2]) + 1)
			return 1;
	effect_new (n, f, T_BEAM, 0, 0, 90, 0, 0, 0)->lk = lk;
	effect_new (n, n, T_GLOW, lk->halo / 2, 0.3f, 90, 0.6f + 0.4f * lk->rgb[0], 0.6f + 0.4f * lk->rgb[1], 0.6f + 0.4f * lk->rgb[2]);
	if ((STMemory_ReadWord (rec + OBJ_HIT) & 0xffff) != 0xffff)
		effect_new (f, f, T_GLOW, 25, 0.6f, 180, 1, 0.85f, 0.55f);
	return 1;
}

static struct { unsigned int kind, aux, rec; } cur;	/* the tag in force */
static int in_complex;
void fx_draw_tag (void **data) { memcpy (&cur, *data, sizeof cur); *data = (char *) *data + sizeof cur; }
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
	if (!MISSILE (cur.kind)) return 0;
	while (m < missiles + n_missiles && m->rec != cur.rec) m++;
	if (m == missiles + 4) return 0;
	if (m == missiles + n_missiles) n_missiles++, m->rec = cur.rec, m->kind = cur.kind, m->n = 0;
	for (i = 0; i < 3 && m->n < 24; i++)
		memcpy (m->v[m->n++], p[i], sizeof m->v[0]);
	return 0;
}

/* smoke: a grey puff of noise that spreads and thins as it ages */
int fx_blob (const int *iv, int rad, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * (rad < 2 ? 2 : rad) * fabsf (v[2]), a = (cur.aux & 0x7f) / 127.0f;
	float grey = (c8 (r) + c8 (g) + c8 (b)) / 3 * 0.7f + 0.15f;
	int l;
	if (cur.kind != M_SMOKE) return 0;
	if (n_smoke < 256) memcpy (smoke[n_smoke++], v, sizeof v);
	l = unlit ();
	blend (0, T_PUFF);
	col (grey, grey * 0.97f, grey * 0.93f, 0.8f + 0.2f * a);
	sprite (v, R * (2.3f - 0.6f * a), hashf (cur.rec, rad) * 6.283f + fx_ms * 0.0004f);
	blend_end ();
	relight (l);
	return 1;
}

/* An explosion (model 188) is a copy of the ship that died (fe2.s:27095) or
 * of the missile that went off (L75852).  +88 is a size step, -1 to +1 (seen
 * -1 and 0; the renderer already scales its circles by it): fx maps it to k,
 * 0 to 1.  Long +122 is its clock, run down from $ffffffff by a fixed step a
 * frame (L74d8e), so its age replays exactly and every explosion lasts the
 * same game time; k shapes how it burns, not how long.  The game draws
 * each blob as rings of circles, the outer first: fx draws a blob once, as a
 * layered fireball at its outer ring, and drops the inner rings. */
#define OBJ_SIZE	88
#define OBJ_CLOCK	122
#define GLOW_CAP	0.6f	/* the most light a blob adds: a hull behind it still reads */
/* What started an explosion, kept by record: 1 a missile (a heuristic: it
 * starts where a missile was last frame), 2 it has the event's shock ring
 * (no other explosion was near it when it started: a death's records share one) */
static int blast (unsigned int rec, float a, const float *v, float R)
{
	static struct { unsigned int rec, seen; int fl; float a, v[3]; } bl[16];
	static int n;
	int i, k;
	for (i = 0; i < 16 && bl[i].rec != rec; i++) ;
	if (i == 16 || a < bl[i].a) {	/* a new one */
		if (i == 16) i = n++ & 15;
		bl[i].rec = rec, bl[i].fl = 2;
		for (k = 0; k < 16; k++)
			if (k != i && bl[k].rec && n_frames - bl[k].seen <= 2 && dist2 (bl[k].v, v) < 64 * R * R) bl[i].fl = 0;
	}
	bl[i].a = a, bl[i].seen = n_frames;
	memcpy (bl[i].v, v, sizeof bl[i].v);
	for (k = 0; a < 0.02f && k < n_mprev; k++)
		if (dist2 (mprev[k], v) < 0.01f * v[2] * v[2]) bl[i].fl |= 1;
	return bl[i].fl;
}

/* the shock ring, t 0 to 1: a soft annulus facing the eye round the first
 * blob, out fast, faint (the fire is the brightest thing), gone before it
 * reaches the view's edge.  Put with the explosion: a nearer hull, painted
 * after it, covers it (the 3D view is a painter's, with no depth test). */
static void shock_ring (const float *c, float r0, float t)
{
	float H = 0.5f * vp_h * px_world (c[2]), edge = fminf (H * vp_w / vp_h - fabsf (c[0]), H - fabsf (c[1]));
	float r = r0 * (1 + 5 * t), w = r * (0.12f + 0.1f * t);
	float al = 0.3f * (1 - t) * (1 - t) * fminf (fmaxf ((edge - r - w) / (0.25f * H), 0), 1);
	int j, n;
	if (t >= 1 || c[2] > -1 || al <= 0) return;
	blend (1, T_GLOW);
	glDisable (GL_TEXTURE_2D);
	for (j = 0; j < 2; j++) {
		glBegin (GL_TRIANGLE_STRIP);
		for (n = 0; n <= 32; n++) {
			float x = cosf (n * 0.19635f), y = sinf (n * 0.19635f), q = r + (j - 1) * w;
			col (0.8f, 0.88f, 1, j ? al : 0);
			glVertex3f (c[0] + x * q, c[1] + y * q, c[2]);
			col (0.8f, 0.88f, 1, j ? 0 : al);
			glVertex3f (c[0] + x * (q + w), c[1] + y * (q + w), c[2]);
		}
		glEnd ();
	}
	blend_end ();
}

int fx_circle (const int *iv, unsigned int d2, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * ((short) d2 + (d2 >> 16) / 65536.0f) * fabsf (v[2]);
	float a, k, f, heat, ember, flash, spin, body, fire[3];
	struct ball *o;
	int fl, mis, i, n = 0, l;
	if (cur.kind != M_EXPL) return 0;
	for (i = 0; i < n_balls; i++)	/* an inner ring of a blob of this record drawn already */
		if (balls[i].rec == cur.rec && (n++, R <= balls[i].R && dist2 (balls[i].v, v) < 0.25f * balls[i].R * balls[i].R))
			return 1;
	if (cur.aux)	/* the intro's fireball: its record is the scene's, with no clock */
		a = 0.1f, k = 0.5f, fl = mis = 0;	/* hot, before smoke; no shock ring */
	else {
		a = 1 - (unsigned int) STMemory_ReadLong (cur.rec + OBJ_CLOCK) / 4294967296.0f;	/* its age, 0 to 1 */
		k = fminf (fmaxf (((short) STMemory_ReadWord (cur.rec + OBJ_SIZE) + 1) / 2.0f, 0), 1);	/* +88 -1 to 0, +1 to 1 */
		fl = blast (cur.rec, a, v, R), mis = fl & 1;
	}
	o = &balls[n_balls < 64 ? n_balls++ : 63];
	*o = (struct ball) { cur.rec, mis, { v[0], v[1], v[2] }, R, a };
	/* its turn, seeded by the record and the blob's place in it: steady frame to frame */
	spin = hashf (rb (cur.rec + 93) + 7 * n, STMemory_ReadWord (cur.rec + 126) & 0xffff) * 6.283f + a * (3 - 2 * k);
	l = unlit ();
	if (c8 (b) > 2 * c8 (r)) {	/* a blue spark: kept small */
		o->R = 0;
		blend (1, T_GLOW);
		col (0.55f, 0.7f, 1, GLOW_CAP * (1 - a));
		sprite (v, R, 0);
		blend_end ();
		relight (l);
		return 1;
	}
	if ((fl & 2) && !n) shock_ring (v, R * (mis ? 0.9f : 1.3f), a / 0.25f);
	/* how far its fire has burnt, by age and size: a fighter pops and is smoke
	 * by late life, a heavy ship burns on, a missile's is quick and hot; a blob
	 * the game draws yellow runs a little hotter than a red one */
	f = a * (mis ? 2.0f : 1.3f - 0.6f * k) + 0.15f * (0.6f - c8 (g));
	flash = 1 - a / (mis ? 0.04f : 0.03f + 0.03f * k);	/* white-yellow only at the start */
	heat = fmaxf (0, 1 - 1.6f * f);			/* yellow 1 to red 0 */
	ember = fminf (fmaxf ((f - 0.55f) / 0.45f, 0), 1);	/* fire 0 to smoke 1 */
	body = 0.95f - 0.4f * fminf (fmaxf ((a - 0.33f) / 0.67f, 0), 1);	/* near opaque for its first third */
	fire[0] = 1, fire[1] = 0.3f + 0.5f * heat + 0.15f * mis, fire[2] = 0.06f + 0.25f * heat * heat + 0.2f * mis;
	R *= mis ? 0.85f : 1;
	blend (0, T_BALL);	/* the body: a full disc at least the game's, fire darkening to soot */
	col (fire[0] + (0.3f - fire[0]) * ember, fire[1] + (0.29f - fire[1]) * ember, fire[2] + (0.28f - fire[2]) * ember, body);
	sprite (v, 1.55f * R * (1 + 0.25f * ember), spin);
	blend (1, T_PUFF);	/* its light, capped */
	col (fire[0], fire[1], fire[2], GLOW_CAP * (1 - ember) * (0.5f + 0.5f * heat));
	sprite (v, 1.2f * R, -spin);
	if (flash > 0) {	/* the flash, the size of the core */
		blend (1, T_GLOW);
		col (1, 0.92f, 0.6f, GLOW_CAP * flash);
		sprite (v, 1.5f * R, 0);
	}
	blend_end ();
	relight (l);
	return 1;
}

/* A missile burns where its smoke starts: a plume off the end of its body
 * nearer the smoke.  One with no smoke near it is coasting: no plume. */
static void draw_plume (const struct missile *m)
{
	const struct look *lk = &looks[16 + (m->kind - 10) / 2];
	const float *a = m->v[0], *b = m->v[0], *tail, *nose, *h = lk->rgb;
	float best = 1e30f, flick = 0.8f + 0.4f * hashf (m->rec, fx_ms * lk->hz / 1000), w, end[3], core[3];
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
	tail = near_a ? a : b, nose = near_a ? b : a;
	for (k = 0; k < 3; k++) {
		end[k] = tail[k] + (tail[k] - nose[k]) * 1.3f * flick;
		core[k] = tail[k] + (tail[k] - nose[k]) * 0.5f * flick;
	}
	w = fmaxf (2, fminf (lk->halo * sqrtf (dist2 (a, b)) / px_world (tail[2]), 160 * lk->halo)) * flick;
	col (h[0], h[1], h[2], 0.85f);	/* the flame */
	ribbon (tail, end, w, 0);
	col (0.7f + 0.3f * h[0], 0.7f + 0.3f * h[1], 0.7f + 0.3f * h[2], 1);	/* its white core, the nozzle */
	ribbon (tail, core, w * lk->core / lk->halo, 0);
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
	c = e->lk->rgb;
	age = (1 - age * age) * (e->lk->hz ? 0.75f + 0.25f * sinf (fx_ms * e->lk->hz * 0.00628f) : 1);
	col (c[0], c[1], c[2], age);
	ribbon (e->a, e->b, e->lk->halo, e->lk->halo * 0.35f);
	col (0.4f * c[0] + 0.6f, 0.4f * c[1] + 0.6f, 0.4f * c[2] + 0.6f, age);
	ribbon (e->a, e->b, e->lk->core, e->lk->core * 0.48f);
}

void fx_frame_end (void)
{
	GLint vp[4];
	int i, had_3d = drew_3d;
	cur.kind = in_complex = drew_3d = 0;
	if (flat_fx) return;
	check_scene ();
	if (!had_3d || (!scene && !n_balls)) return;	/* no flight view or explosion this frame */
	glGetIntegerv (GL_VIEWPORT, vp);
	vp_h = vp[3] > 0 ? vp[3] : 480, vp_w = vp[2] > 0 ? vp[2] : 640;
	if (!scene) return;
	blend (1, T_GLOW);
	for (i = 0; i < n_missiles; i++) draw_plume (&missiles[i]);
	for (i = 0; i < NR; i++)
		if (ring[i].fade && fx_ms - ring[i].t0 >= ring[i].fade) ring[i].fade = 0;
		else if (ring[i].fade) draw_effect (&ring[i]);
	blend_end ();
}

void fx_forget (void) { if (tex[0]) glDeleteTextures (T_N, tex); memset (tex, 0, sizeof tex); scene = -1; }
