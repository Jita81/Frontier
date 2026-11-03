/* fx: HD weapon effects for the GL renderer (fx.h, docs/hd/weapons.md).  The put
 * side reads the object drawn and tags its primitive; the draw side turns tagged
 * primitives into sprites and ribbons of two procedural textures.  Beams and
 * flares outlive their frame in a ring aged on the game's clock (held while paused). */
#include <SDL.h>
#include <SDL_opengl.h>
#include "main.h"
#include "../m68000.h"
#include "../host.h"	/* MEM_SIZE */
#include "../fixups.h"	/* __DL5eb6_a6_base */
#include "fx.h"
#include "hull.h"	/* hull_intro_fx */
#define A6		__DL5eb6_a6_base
#define A6_TOOLS_MODE	42	/* byte: negative while a 3D view fills the screen */
#define A6_START_FLAGS	598	/* byte: bit 5 set in the intro and the start menu */
#define A6_VIEW		706	/* byte: 0 front, 1 rear, 2/3 turrets, 4 external */
#define A6_TOD		524	/* long: the game's time of day, a whole day 2^32 */
#define A6_DAY		528	/* long: the game's day */
#define FRAME_OBJ	212	/* long at A6-212 during a put: the object drawn */
#define OBJ_MODEL	90	/* word */
#define OBJ_AGE		122	/* byte: a smoke puff's age, $7f down to 0 */
#define OBJ_LASER	123	/* byte: a beam's laser type */
#define OBJ_HIT		186	/* word: $ffff when a beam hit nothing */
/* the models with effects (docs/hd/weapons.md) */
#define MISSILE(m)	((m) >= 10 && (m) <= 16 && !((m) & 1))	/* homing, smart, naval, nuclear */
#define MISSILE_NOSE_PZ	1	/* a missile model's nose is on its +z axis */
#define M_EXPL		188	/* an explosion */
#define M_LASER		190	/* a laser beam, put each frame of a shot */
#define M_SMOKE		194	/* smoke and sparks, also a missile's trail */
/* The look of each type.  A laser's class is bits 3-6 of its type byte (the
 * game's own damage table is indexed so): 1-3 are pulse lasers ($88 the start
 * ship's, $9a mining), 4-9 beam lasers ($a1 the police's) and accelerators.
 * A laser: hue, core and halo widths in pixels, shimmer in Hz (0 steady), and
 * its shape: a pulse laser's BOLT flies out along the beam, a BEAM laser holds
 * a beam with hot spots running along its core, PLASMA is a beam that crackles.
 * A missile (16 + (model - 10) / 2): flame hue, core and flame widths as a
 * share of its body's length, flicker in Hz.  Procedural: no art. */
enum { BOLT = 1, BEAM, PLASMA };
static const struct look { float rgb[3], core, halo, hz; int shape; } looks[20] = {
	{ { 1, .2f, .1f }, 4, 32, 0, BOLT }, { { 1, .2f, .1f }, 5, 40, 0, BOLT },	/* 0 (none seen), 1: pulse, red */
	{ { 1, .45f, .1f }, 6, 48, 0, BOLT }, { { 1, .8f, .25f }, 4, 30, 8, BOLT },	/* 2: orange; 3: mining, amber, throbs */
	{ { .3f, 1, .4f }, 4, 34, 0, BEAM }, { { .2f, .85f, 1 }, 5, 42, 0, BEAM },	/* 4: beam, green; 5: cyan */
	{ { .35f, .5f, 1 }, 6, 50, 0, BEAM }, { { .7f, .4f, 1 }, 7, 58, 0, BEAM },	/* 6: blue; 7: violet */
	{ { 1, .45f, .9f }, 8, 66, 12, PLASMA }, { { 1, .6f, 1 }, 10, 80, 12, PLASMA },	/* 8, 9 (and 10-15): plasma, crackles */
	[16] = { { .3f, .85f, 1 }, .08f, .22f, 30 }, { { .4f, 1, .55f }, .08f, .24f, 40 },	/* homing blue, smart green */
	{ { 1, .6f, .25f }, .1f, .3f, 25 }, { { 1, .35f, .75f }, .12f, .4f, 20 },	/* naval orange, nuclear magenta */
};

static unsigned int fx_ms;	/* the game's clock in ms: stands still while paused */
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
static int intro_look = -1;	/* its last was an intro laser's beam: its look, else -1 */
static int intro_on;		/* this frame is the intro's (latched in fx_frame_begin) */
static int put_model (unsigned int *rec)
{
	unsigned int a6 = GetReg (REG_A6);
	int m, fx, look = -1;
	intro_fire = 0, intro_look = -1;
	if (flat_fx || !readable (a6 - FRAME_OBJ, 4)) return 0;
	*rec = STMemory_ReadLong (a6 - FRAME_OBJ);
	/* the intro (bit 5 of 598(a6), and its own table): by the part's model,
	 * whose row says what it is (hull_intro_fx); its record is the scene's */
	if (rb (A6 + A6_START_FLAGS) & 0x20) {
		fx = hull_intro_fx (hull_intro_fx_part (), &look);
		if (fx == HULL_FX_FIRE) return (intro_fire = readable (*rec, 256)) ? M_EXPL : 0;
		if (fx == HULL_FX_LASER && intro_on) return intro_look = look, M_LASER;
	}	/* else as the base: the death camera's blasts and smoke, below */
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
/* trap: shade it as the trapezoid it is on screen (texture q scaled by each
 * end's width), so a strip tapering to a point stays even about its line */
static void ribbon (const float *a, const float *b, float wa, float wb, int trap)
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
		float h = (k & 1 ? 1.25f : -1.25f) * (k < 2 ? wa : wb) * px_world (e[2]) / l, q = -e[2] * (trap ? (k < 2 ? wa : wb) : 1);
		glTexCoord4f ((k & 1) * q, 0.5f * q, 0, q);
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
#define SCENE_INTRO	0x1000	/* the intro's, latched at its frame's start */
static int scene = -1;		/* 0 none, SCENE_INTRO, else 1 + the flight view */
static void check_scene (void)
{
	int now = in_flight () ? 1 + rb (A6 + A6_VIEW) : intro_on ? SCENE_INTRO : 0;
	if (now != scene) memset (ring, 0, sizeof ring), scene = now;
}

static unsigned int n_frames, fx_game, intro_n;
static unsigned long long game_was;	/* the game's clock last frame, ms */
#define STEP	20	/* ms: the intro draws one frame a VBL, 50 Hz */
static int drew_3d;		/* this frame has a 3D view */
/* this frame's missile bodies and smoke puffs, for the plumes */
static struct missile { unsigned int rec, kind, n; float v[24][3]; } missiles[4];
static float smoke[256][3];
static int n_missiles, n_smoke;
/* this frame's explosion blobs, and last frame's missiles (fx_circle) */
static struct ball { unsigned int rec, missile; float v[3], R, a; } balls[64];
static float mprev[4][3];
static int n_balls, n_mprev;
/* this frame's missiles' rotations, put side: rot[3 j + k] is the view's
 * k part of the model's axis j (nine 1.15 words at A6-36 while the object is
 * put, as procfx_sky_put reads them); the view's z is GL's -z */
#define FRAME_ROT	36
static struct { unsigned int rec; float rot[9]; } mrot[4];
static int n_mrot;
static void missile_rot (unsigned int rec)
{
	unsigned int a6 = GetReg (REG_A6), i;
	if (flat_missiles || n_mrot == 4 || !readable (a6 - FRAME_ROT, 18)) return;
	for (i = 0; i < (unsigned int) n_mrot; i++) if (mrot[i].rec == rec) return;
	mrot[n_mrot].rec = rec;
	for (i = 0; i < 9; i++) mrot[n_mrot].rot[i] = (short) STMemory_ReadWord (a6 - FRAME_ROT + 2 * i) / 32768.0f;
	n_mrot++;
}
void fx_frame_begin (void)
{
	/* the game's clock in ms: it stands still while paused and runs 10x at
	 * 10x; a replay sees the same clock on the same frame */
	unsigned long long now = (unsigned int) STMemory_ReadLong (A6 + A6_DAY) * 86400000ull +
		((unsigned int) STMemory_ReadLong (A6 + A6_TOD) * 86400000ull >> 32), d = now - game_was;
	game_was = now, n_frames++;
	drew_3d = 1;
	for (n_mprev = 0; n_mprev < n_missiles; n_mprev++) memcpy (mprev[n_mprev], missiles[n_mprev].v[0], sizeof mprev[0]);
	n_missiles = n_smoke = n_balls = n_mrot = 0;
	/* the intro and start menu (bit 5 of 598(a6)), not the death camera's
	 * (also bit 5; its state 0x58); the game data pointer is the game's here */
	intro_on = !flat_fx && (rb (A6 + A6_START_FLAGS) & 0x20) && rb (A6 + A6_STATE) != 0x58;
	check_scene ();
	/* a step back (a game loaded) or over 1 s (a jump, or 100x and up) is 1 s:
	 * longer than any effect lives, and fx_ms stays small enough for floats */
	fx_game += d > 1000 ? 1000 : d;
	/* the intro's time is its frames', so its replays draw the same */
	intro_n = intro_on ? intro_n + 1 : 0;
	fx_ms = intro_on ? STEP * intro_n : fx_game;
}

int fx_tag_for (unsigned int tag[3])
{
	unsigned int rec;
	int m = put_model (&rec);
	if (!MISSILE (m) && m != M_SMOKE && (m != M_EXPL || flat_explosions)) return 0;
	if (MISSILE (m)) missile_rot (rec);
	tag[0] = m, tag[1] = m == M_SMOKE ? rb (rec + OBJ_AGE) : intro_fire, tag[2] = rec;
	return 1;
}

/* the class of the laser being put (its type's bits 3-6, or the intro's
 * look), -1 if none; and the eye-space corner at an address register */
static int laser_class (unsigned int *rec)
{
	if (put_model (rec) != M_LASER) return -1;
	return intro_look >= 0 ? intro_look : rb (*rec + OBJ_LASER) >> 3 & 15;
}
static int corner (int reg, float *v)
{
	int k;
	if (!readable (GetReg (reg) + 4, 12)) return 0;
	for (k = 0; k < 3; k++) v[k] = (k == 2 ? -1.0f : 1.0f) * STMemory_ReadLong (GetReg (reg) + 4 + 4 * k);
	return 1;
}
/* a laser's beam from ends[0] to ends[1]: a halo in its look's hue round a
 * near-white core, a muzzle bloom and, on a hit, a flare (not the intro's,
 * whose record is its scene's) */
static int laser_beam (float ends[2][3], int j, unsigned int rec)
{
	const struct look *lk = &looks[j > 9 ? 9 : j];
	float *n = ends[fabsf (ends[0][2]) > fabsf (ends[1][2])], *f = ends[n == ends[0]], t;	/* the near end, the far */
	int i, k;
	t = n[2] > -1.5f && f[2] < -1.5f ? (-1.5f - n[2]) / (f[2] - n[2]) : 0;	/* the muzzle, off the near plane */
	for (k = 0; k < 3; k++) n[k] += (f[k] - n[k]) * t;
	for (i = 0; i < NR; i++)	/* the beam's second quad this frame */
		if (ring[i].fade && ring[i].t == T_BEAM && ring[i].t0 == fx_ms &&
		    fabsf (ring[i].a[0] - n[0]) + fabsf (ring[i].a[2] - n[2]) < 0.1f * fabsf (f[2] - n[2]) + 1)
			return 1;
	effect_new (n, f, T_BEAM, 0, 0, 90, 0, 0, 0)->lk = lk;
	effect_new (n, n, T_GLOW, lk->halo / 2, 0.3f, 90, 0.6f + 0.4f * lk->rgb[0], 0.6f + 0.4f * lk->rgb[1], 0.6f + 0.4f * lk->rgb[2]);
	if (intro_look < 0 && (STMemory_ReadWord (rec + OBJ_HIT) & 0xffff) != 0xffff)
		effect_new (f, f, T_GLOW, 25, 0.6f, 180, 1, 0.85f, 0.55f);
	return 1;
}
int fx_laser_quad (void)
{
	float v[4][3], ends[2][3];
	unsigned int rec;
	int j = laser_class (&rec), i, k;
	if (j < 0) return 0;
	for (i = 0; i < 4; i++) if (!corner (REG_A0 + i, v[i])) return 0;
	/* the corners go round the quad: its ends are the pair of opposite
	 * edges, 0-1 and 2-3 or 1-2 and 3-0, farther apart in depth */
	i = fabsf (v[0][2] + v[1][2] - v[2][2] - v[3][2]) < fabsf (v[1][2] + v[2][2] - v[3][2] - v[0][2]);
	for (k = 0; k < 3; k++) {
		ends[0][k] = (v[i][k] + v[i + 1][k]) / 2;
		ends[1][k] = (v[i + 2][k] + v[(i + 3) & 3][k]) / 2;
	}
	return laser_beam (ends, j, rec);
}
/* the intro draws its beams as lines; the game's stay as they were */
int fx_laser_line (void)
{
	float ends[2][3];
	unsigned int rec;
	int j = laser_class (&rec);
	if (j < 0 || intro_look < 0 || !corner (REG_A0, ends[0]) || !corner (REG_A1, ends[1])) return 0;
	return laser_beam (ends, j, rec);
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
 * (no other explosion was near it when it started: a death's records share one);
 * *top, the biggest blob it has drawn (a steady reach for its embers), *last,
 * the biggest it drew last frame (a steady size for the player's bursts) */
static int blast (unsigned int rec, float a, const float *v, float R, float *top, float *last)
{
	static struct { unsigned int rec, seen; int fl; float a, v[3], top, cur, last; } bl[16];
	static int n;
	int i, k;
	for (i = 0; i < 16 && bl[i].rec != rec; i++) ;
	if (i == 16 || a < bl[i].a) {	/* a new one */
		if (i == 16) i = n++ & 15;
		bl[i].rec = rec, bl[i].fl = 2, bl[i].top = bl[i].cur = bl[i].last = 0;
		for (k = 0; k < 16; k++)
			if (k != i && bl[k].rec && n_frames - bl[k].seen <= 2 && dist2 (bl[k].v, v) < 64 * R * R) bl[i].fl = 0;
	}
	if (bl[i].seen != n_frames) bl[i].last = bl[i].cur, bl[i].cur = 0;
	bl[i].a = a, bl[i].seen = n_frames, *top = bl[i].top = fmaxf (bl[i].top, R), bl[i].cur = fmaxf (bl[i].cur, R);
	*last = bl[i].last > 0 ? bl[i].last : R;
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

/* a blob's fireball at age a: a body, its capped light and the flash.  How far
 * its fire has burnt, by age and size: a fighter pops and is smoke by late life,
 * a heavy ship burns on, a missile's is quick and hot; a blob the game draws
 * yellow (g, its green) runs a little hotter than a red one */
static void fireball (const float *v, float R, float a, float k, int mis, float g, float spin)
{
	float f = a * (mis ? 2.0f : 1.3f - 0.6f * k) + 0.15f * (0.6f - g), heat, ember, flash, body, fire[3];
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
}

/* An event's embers: 8 (k 0) to 24 (k 1) hot sparks, 5 for a missile's, each
 * on a direction, reach and life hashed from the event's seed and its index.
 * Out fast and slowing, cooling yellow-white to orange to deep red, all out by
 * 50% (k 0) to 70% (k 1) of the explosion's life, 35% for a missile's (the
 * fire is smoke by 77% at k 0).  A streak and a dot, a few pixels: never over
 * a quarter of the flash across. */
static void embers (const float *v, float R, float a, float k, int mis, unsigned int seed)
{
	float L = mis ? 0.35f : 0.5f + 0.2f * k, Rpx = R / px_world (v[2]), w0 = fminf (1.5f + 0.03f * Rpx, 0.3f * Rpx);
	int i, j, c, n = mis ? 5 : (int) (8.5f + 16 * k);
	if (a >= L || v[2] > -1) return;
	blend (1, T_GLOW);
	for (i = 0; i < n; i++) {
		float z = 2 * hashf (seed, 4 * i) - 1, ph = 6.283f * hashf (seed, 4 * i + 1), s = sqrtf (1 - z * z);
		float e = a / (L * (0.6f + 0.4f * hashf (seed, 4 * i + 2))), hot = 1 - e * (mis ? 0.6f : 1);
		float reach = R * (mis ? 1.5f : 2.5f + 3 * k) * (0.5f + 0.5f * hashf (seed, 4 * i + 3)), d[3] = { s * cosf (ph), s * sinf (ph), 0.5f * z }, p[2][3], w;
		if (e >= 1) continue;
		for (j = 0; j < 2; j++) {	/* the streak's tail a moment back, then its head */
			float u = fmaxf (e - 0.07f * (1 - j), 0);
			for (c = 0; c < 3; c++) p[j][c] = v[c] + d[c] * reach * (1 - (1 - u) * (1 - u));
		}
		w = w0 * (1 - 0.5f * e);
		col (0.6f + 0.4f * fminf (2 * hot, 1), 0.08f + 0.87f * powf (hot, 1.5f), 0.75f * hot * hot * hot, GLOW_CAP * (1 - e * e));
		ribbon (p[0], p[1], 0.3f * w, w, 1);
		sprite (p[1], 1.2f * w * px_world (p[1][2]), 0);
	}
	blend_end ();
}

/* The player's own death (704(a6) = $58, the record's owner +236 = $ff): the
 * death timer 722(a6) is set to $3cae6 and run down by the frame's time step
 * until it would go below 0, then the game-over screen (fe2.s:42633, L63b9c):
 * t, 0 to 1, stages it.  Two or three seeded secondary bursts (fireball and
 * embers, no ring) one to two blob radii off, one after another; and a slow
 * bloom over the 3D view, a warm white glow round the blast, additive, at most
 * 0.35 at its middle, rising from t 0.2 to its peak at 0.5, gone by t 0.8. */
#define A6_DEATH_T	722
#define OBJ_OWNER	236
static float death_t (void) { return 1 - fminf ((unsigned int) STMemory_ReadLong (A6 + A6_DEATH_T) / 248550.0f, 1); }
static float bloom (void)
{
	float t = death_t ();
	return flat_explosions || rb (A6 + A6_STATE) != 0x58 || t <= 0.2f || t >= 0.8f ? 0 : 0.35f * sinf (5.236f * (t - 0.2f));
}
static float bloom_at[2];	/* the blast on screen, -1 to 1 */
static void death_bursts (const float *v, float R, float k, float g, unsigned int seed)
{
	float t = death_t ();
	int j, c, n = 2 + (hashf (seed, 99) < 0.5f);
	bloom_at[0] = fminf (fmaxf (v[0] / (-v[2] * 0.32975f * vp_w / vp_h), -1), 1);
	bloom_at[1] = fminf (fmaxf (v[1] / (-v[2] * 0.32975f), -1), 1);
	for (j = 0; j < n; j++) {
		float b = (t - 0.1f - 0.18f * j - 0.1f * hashf (seed, 100 + j)) / 0.3f, m = R * (1 + hashf (seed, 104 + j));
		float z = 2 * hashf (seed, 108 + j) - 1, ph = 6.283f * hashf (seed, 112 + j), s = sqrtf (1 - z * z), p[3];
		if (b < 0 || b >= 1) continue;
		for (c = 0; c < 3; c++) p[c] = v[c] + m * (c == 2 ? 0.5f * z : s * (c ? sinf (ph) : cosf (ph)));
		fireball (p, 0.7f * R, b, k, 0, g, ph + b);
		embers (p, 0.7f * R, b, k, 0, seed * 31 + j + 1);
	}
}

int fx_circle (const int *iv, unsigned int d2, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * ((short) d2 + (d2 >> 16) / 65536.0f) * fabsf (v[2]);
	float a, k, spin, top = R, last = R;
	unsigned int seed;
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
		fl = blast (cur.rec, a, v, R, &top, &last), mis = fl & 1;
	}
	o = &balls[n_balls < 64 ? n_balls++ : 63];
	*o = (struct ball) { cur.rec, mis, { v[0], v[1], v[2] }, R, a };
	/* its turn, seeded by the record and the blob's place in it: steady frame to frame */
	seed = STMemory_ReadWord (cur.rec + 126) & 0xffff;
	spin = hashf (rb (cur.rec + 93) + 7 * n, seed) * 6.283f + a * (3 - 2 * k);
	seed = seed << 8 | rb (cur.rec + 93);
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
	fireball (v, R, a, k, mis, c8 (g), spin);
	if ((fl & 2) && !n) {	/* the event's sprays */
		embers (v, top, a, k, mis, seed);
		if (rb (A6 + A6_STATE) == 0x58 && rb (cur.rec + OBJ_OWNER) == 0xff) death_bursts (v, last, k, c8 (g), seed);
	}
	relight (l);
	return 1;
}

/* A missile's flame, every frame its body is drawn, off its tail: the end its
 * rotation puts aft, else the end nearer its smoke (near_a), else the end
 * farther from the eye.  From its look: hue, width, flicker and, from the
 * width, heat (homing 0.1 to nuclear 1): length, core and glow grow with it. */
static void draw_flame (const struct missile *m, const struct look *lk, const float *a, const float *b, int near_a)
{
	const float *tail = near_a >= 0 ? (near_a ? a : b) : a[2] < b[2] ? a : b, *nose, *h = lk->rgb;
	float L = sqrtf (dist2 (a, b)), heat = (lk->halo - 0.2f) * 5, dot = 0, w, pt[3][3];
	float flick = 0.75f + 0.5f * vnoise (fx_ms * lk->hz / 1000, 1 + (m->rec >> 2 & 63));
	int i, k;
	for (i = 0; i < n_mrot && mrot[i].rec != m->rec; i++) ;
	for (k = 0; i < n_mrot && k < 3; k++)	/* (b - a) on the model's z axis */
		dot += (b[k] - a[k]) * (k == 2 ? -1 : 1) * mrot[i].rot[6 + k];
	if (fabsf (dot) > 0.5f * L) tail = (dot > 0) == MISSILE_NOSE_PZ ? a : b;
	nose = tail == a ? b : a;
	for (k = 0; k < 3; k++) {	/* the flame's end, its hot middle, the white core */
		float u = (tail[k] - nose[k]) * flick;
		pt[0][k] = tail[k] + u * 10 * lk->halo, pt[1][k] = tail[k] + u * 4 * lk->halo, pt[2][k] = tail[k] + u * (0.4f + 0.5f * heat);
	}
	w = fmaxf (3, fminf (lk->halo * L / px_world (tail[2]), 160 * lk->halo)) * (0.85f + 0.15f * flick);
	col (h[0], h[1], h[2], 0.8f);
	ribbon (tail, pt[0], w, 0, 1);
	col (0.5f + 0.5f * h[0], 0.5f + 0.5f * h[1], 0.5f + 0.5f * h[2], 0.9f);
	ribbon (tail, pt[1], 0.6f * w, 0, 1);
	col (0.85f + 0.15f * h[0], 0.85f + 0.15f * h[1], 0.85f + 0.15f * h[2], 1);	/* the nozzle, near white */
	ribbon (tail, pt[2], w * lk->core / lk->halo * (1.5f + heat), 0, 1);
	col (0.6f + 0.4f * h[0], 0.6f + 0.4f * h[1], 0.6f + 0.4f * h[2], 0.55f + 0.35f * heat);
	sprite (tail, (0.9f + 0.9f * heat) * w * px_world (tail[2]), 0);
}

/* A missile burns where its smoke starts: a plume off the end of its body
 * nearer the smoke.  One with no smoke near it is coasting: no plume.
 * (--flat-missiles; else draw_flame.) */
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
	if (!flat_missiles && dist2 (a, b) > 0) { draw_flame (m, lk, a, b, best <= 36 * dist2 (a, b) ? near_a : -1); return; }
	if (!(dist2 (a, b) > 0) || best > 36 * dist2 (a, b)) return;
	tail = near_a ? a : b, nose = near_a ? b : a;
	for (k = 0; k < 3; k++) {
		end[k] = tail[k] + (tail[k] - nose[k]) * 1.3f * flick;
		core[k] = tail[k] + (tail[k] - nose[k]) * 0.5f * flick;
	}
	w = fmaxf (2, fminf (lk->halo * sqrtf (dist2 (a, b)) / px_world (tail[2]), 160 * lk->halo)) * flick;
	col (h[0], h[1], h[2], 0.85f);	/* the flame */
	ribbon (tail, end, w, 0, 0);
	col (0.7f + 0.3f * h[0], 0.7f + 0.3f * h[1], 0.7f + 0.3f * h[2], 1);	/* its white core, the nozzle */
	ribbon (tail, core, w * lk->core / lk->halo, 0, 0);
	sprite (tail, 0.8f * w * px_world (tail[2]), 0);
}

/* the stretch of beam e from u0 to u1 of the way along it on screen, wa and wb
 * pixels wide there, cut to the beam (1/z runs evenly along it on screen) */
static void piece (const struct effect *e, float u0, float u1, float wa, float wb)
{
	float p[2][3], u[2] = { fmaxf (u0, 0), fminf (u1, 1) };
	int i, k;
	if (!(u[1] > u[0])) return;
	for (i = 0; i < 2; i++) {
		float t = u[i] * e->a[2] / ((1 - u[i]) * e->b[2] + u[i] * e->a[2]);
		for (k = 0; k < 3; k++) p[i][k] = e->a[k] + (e->b[k] - e->a[k]) * t;
	}
	ribbon (p[0], p[1], wa + (wb - wa) * (u[0] - u0) / (u1 - u0), wa + (wb - wa) * (u[1] - u0) / (u1 - u0), 1);
}

static void draw_effect (const struct effect *e)
{
	float age = (float) (fx_ms - e->t0) / e->fade, shim;
	const float *c = e->rgb;
	int i;
	if (e->t != T_BEAM) {
		col (c[0], c[1], c[2], 1 - age);
		sprite (e->a, e->size * (1 + e->grow * age) * px_world (e->a[2]), 0);
		return;
	}
	c = e->lk->rgb;
	shim = e->lk->hz ? 0.75f + 0.25f * sinf (fx_ms * e->lk->hz * 0.00628f) : 1;
	if (e->lk->shape == BOLT && !flat_lasers) {	/* a bolt: its head reaches the far end at 70% of its life, */
		float u = 0.1f + 1.3f * age, h = 1 - 0.6f * fminf (u, 1);	/* its tapered trail sinks in after */
		for (i = 0; i < 2; i++) {	/* halo, then near-white core */
			float k = i ? 0.4f : 1, m = i ? 0.6f : 0, w = (i ? 1.4f * e->lk->core : e->lk->halo) * h;
			col (k * c[0] + m, k * c[1] + m, k * c[2] + m, (1 - age * age * age * age) * shim);
			piece (e, u - 0.3f, u, 0, w);
			piece (e, u, u + 0.03f, w, 0);	/* a rounded nose */
		}
		return;
	}
	/* a beam is held bright, then gone: a halo round a near-white core */
	age = (1 - age * age) * shim;
	col (c[0], c[1], c[2], age);
	ribbon (e->a, e->b, e->lk->halo, e->lk->halo * 0.35f, 0);
	col (0.4f * c[0] + 0.6f, 0.4f * c[1] + 0.6f, 0.4f * c[2] + 0.6f, age);
	ribbon (e->a, e->b, e->lk->core, e->lk->core * 0.48f, 0);
	for (i = 0; i < 4 && !flat_lasers; i++) {	/* a beam's two hot spots, glow then core, run out along it, a length in 400 ms */
		float u = fmodf (fx_ms * 0.0025f + 0.5f * (i >> 1), 1), k = i & 1 ? 0.2f : 1, m = 1 - k;
		float w = (i & 1 ? 2.5f * e->lk->core : 0.7f * e->lk->halo) * (1 - 0.65f * u);
		col (k * c[0] + m, k * c[1] + m, k * c[2] + m, 0.8f * age);
		piece (e, u - 0.15f, u, 0, w);
		piece (e, u, u + 0.15f, w, 0);
	}
}

void fx_frame_end (void)
{
	GLint vp[4];
	int i, k, had_3d = drew_3d, l;
	float wash;
	cur.kind = in_complex = drew_3d = 0;
	if (flat_fx) return;
	check_scene ();
	wash = bloom ();
	if (!had_3d || (!scene && !n_balls && !wash)) return;	/* no flight view or explosion this frame */
	glGetIntegerv (GL_VIEWPORT, vp);
	vp_h = vp[3] > 0 ? vp[3] : 480, vp_w = vp[2] > 0 ? vp[2] : 640;
	if (wash > 0) {	/* the player's death: the bloom, a glow round the blast over the 3D view */
		glMatrixMode (GL_PROJECTION), glPushMatrix (), glLoadIdentity ();
		glMatrixMode (GL_MODELVIEW), glPushMatrix (), glLoadIdentity ();
		l = unlit ();
		blend (1, T_GLOW);
		col (1, 0.93f, 0.82f, wash);
		glBegin (GL_QUADS);
		for (k = 0; k < 4; k++) {
			float x = k == 1 || k == 2, y = k > 1;
			glTexCoord2f (x, y);
			glVertex2f (bloom_at[0] + (2 * x - 1) * 2.5f * vp_h / vp_w, bloom_at[1] + (2 * y - 1) * 2.5f);
		}
		glEnd ();
		blend_end ();
		relight (l);
		glPopMatrix (), glMatrixMode (GL_PROJECTION), glPopMatrix (), glMatrixMode (GL_MODELVIEW);
	}
	if (!scene) return;
	blend (1, T_GLOW);
	for (i = 0; i < n_missiles; i++) draw_plume (&missiles[i]);
	for (i = 0; i < NR; i++)
		if (ring[i].fade && fx_ms - ring[i].t0 >= ring[i].fade) ring[i].fade = 0;
		else if (ring[i].fade) draw_effect (&ring[i]);
	blend_end ();
}

void fx_forget (void) { if (tex[0]) glDeleteTextures (T_N, tex); memset (tex, 0, sizeof tex); scene = -1; }
