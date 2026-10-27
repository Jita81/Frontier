/* fx: HD lasers, missiles, explosions, smoke, shields, ECM/bomb rings (docs/hd/weapons.md).
 * The object being put is read (never written) from ST memory and rides to the draw side
 * as an NU_FX tag op.  Procedural: three 64x64 textures, a 16-slot afterglow ring, a hash. */
#include <SDL.h>
#include <SDL_opengl.h>
#include "main.h"
#include "../m68000.h"
#include "fx.h"

int flat_fx = 0;
#define ST_TOP		0x110000
#define MISSILE(m)	((m) >= M_MISSILE && (m) <= M_NUKE && !((m) & 1))
enum { M_MISSILE = 10, M_NUKE = 16, M_EXPL = 188, M_LASER = 190, M_SHIELD = 192, M_SMOKE = 194, M_RING = 200 };
enum { T_GLOW, T_PUFF, T_RING, T_N };
static unsigned int fx_ms, last_tick;
static int vp_h = 480, log_on = -1, in_cx;
static GLuint tex[T_N];
static struct { unsigned int kind, aux, rec; } cur;
static int readable (unsigned int a, unsigned int n) { return a <= ST_TOP && n <= ST_TOP - a; }
static unsigned int hash (unsigned int a, unsigned int b)
{
	unsigned int h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
	h ^= h >> 15; h *= 0x2c1b3c6du;
	return h ^ (h >> 12);
}
static float hashf (unsigned int a, unsigned int b) { return (hash (a, b) & 0xffff) / 65535.0f; }
/* the object being drawn (A6-212 holds its record): its model word, or 0 */
static int obj (unsigned int *rec)
{
	unsigned int a6 = GetReg (REG_A6);
	if (!readable (a6 - 212, 4) || !readable (*rec = STMemory_ReadLong (a6 - 212), 192)) return 0;
	return STMemory_ReadWord (*rec + 90) & 0xffff;
}
/* FE2_FX_LOG=1: each model and laser type byte met, once */
static void fx_log (int what, int v)
{
	static unsigned char seen[2][256];
	if (log_on < 0) log_on = getenv ("FE2_FX_LOG") != NULL;
	if (log_on && v < 256 && !seen[what][v]) seen[what][v] = 1, fprintf (stderr, "fx: %s $%02x\n", what ? "laser type" : "model", v);
}
/* glow: two gaussians; puff: a warped-sine cloud in a soft disc; ring */
static void make_tex (void)
{
	static unsigned char px[64 * 64 * 2];
	int t, i;
	glGenTextures (T_N, tex);
	for (t = 0; t < T_N; t++) {
		for (i = 0; i < 64 * 64; i++) {
			float u = (i % 64 + 0.5f) / 32 - 1, v = (i / 64 + 0.5f) / 32 - 1, r2 = u * u + v * v, r = sqrtf (r2), a;
			float n = 0.5f + 0.3f * sinf (6 * u + 2 * sinf (5 * v + 1)) * cosf (6 * v + 2 * sinf (4 * u)) + 0.2f * sinf (13 * u - 11 * v + 3 * sinf (9 * u));
			a = t == T_GLOW ? 0.55f * expf (-4 * r2) + 0.45f * expf (-28 * r2)
			  : r >= 1 ? 0 : t == T_RING ? expf (-125 * (r - 0.8f) * (r - 0.8f)) : powf (1 - r, 1.2f) * (0.45f + 0.8f * n);
			px[2 * i] = t == T_PUFF ? 255 * (0.7f + 0.3f * n) : 255;
			px[2 * i + 1] = a > 1 ? 255 : 255 * a;
		}
		glBindTexture (GL_TEXTURE_2D, tex[t]);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, 64, 64, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, px);
	}
}
void fx_forget (void) { if (tex[0]) glDeleteTextures (T_N, tex); memset (tex, 0, sizeof (tex)); }
/* --- drawing, in eye space (the 3D pass's modelview is identity) --- */
static float px_world (float z) { return fabsf (z) * 0.6595f / vp_h; }	/* 2 tan 18.25 deg */
static void blend (int add) { glEnable (GL_BLEND); glBlendFunc (GL_SRC_ALPHA, add ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); }
static void blend_end (void) { glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable (GL_BLEND); glDisable (GL_TEXTURE_2D); }
static void use_tex (int t) { if (!tex[0]) make_tex (); glEnable (GL_TEXTURE_2D); glBindTexture (GL_TEXTURE_2D, tex[t]); }
static void col (float r, float g, float b, float a) { glColor4f (r, g, b, a > 1 ? 1 : a); }
static void sprite (const float *p, float r, float rot, int t)
{
	float c, s, lim = 0.5f * vp_h * px_world (p[2]);
	if (p[2] > -1) return;
	if (r > lim) r = lim;	/* never more than the screen */
	c = cosf (rot) * r; s = sinf (rot) * r;
	use_tex (t);
	glBegin (GL_QUADS);
	glTexCoord2f (0, 0); glVertex3f (p[0] - c + s, p[1] - s - c, p[2]);
	glTexCoord2f (1, 0); glVertex3f (p[0] + c + s, p[1] + s - c, p[2]);
	glTexCoord2f (1, 1); glVertex3f (p[0] + c - s, p[1] + s + c, p[2]);
	glTexCoord2f (0, 1); glVertex3f (p[0] - c - s, p[1] - s + c, p[2]);
	glEnd ();
}
/* a glowing strip from a to b, wa and wb pixels wide at its ends */
static void ribbon (const float *a, const float *b, float wa, float wb)
{
	float p[2][3], d[3], s[3], l, t, h;
	int i, k;
	memcpy (p[0], a, sizeof (p[0])); memcpy (p[1], b, sizeof (p[1]));
	if (p[0][2] > -1 && p[1][2] > -1) return;
	for (k = 0; k < 2; k++) if (p[k][2] > -1) {	/* clip at the near plane */
		t = (-1 - p[!k][2]) / (p[k][2] - p[!k][2]);
		for (i = 0; i < 3; i++) p[k][i] = p[!k][i] + (p[k][i] - p[!k][i]) * t;
	}
	for (i = 0; i < 3; i++) d[i] = p[1][i] - p[0][i];
	s[0] = d[1] * p[0][2] - d[2] * p[0][1]; s[1] = d[2] * p[0][0] - d[0] * p[0][2]; s[2] = d[0] * p[0][1] - d[1] * p[0][0];
	if (!((l = sqrtf (s[0] * s[0] + s[1] * s[1] + s[2] * s[2])) > 0)) return;
	use_tex (T_GLOW);
	glBegin (GL_TRIANGLE_STRIP);
	for (k = 0; k < 4; k++) {
		h = (k & 1 ? 1.25f : -1.25f) * (k < 2 ? wa : wb) * px_world (p[k >> 1][2]) / l;
		glTexCoord2f (k & 1, .5f);
		glVertex3f (p[k >> 1][0] + s[0] * h, p[k >> 1][1] + s[1] * h, p[k >> 1][2] + s[2] * h);
	}
	glEnd ();
}
/* --- what outlives its frame: beams (t -1) and sprites, all additive --- */
struct ent { unsigned int rec, t0, fade; int t, px; float a[3], b[3], size, grow, w0, w1, jit, rgb[3]; };
static struct ent ring[16];
static struct ent *ent_new (const float *p, float size, int px, int t, unsigned int fade, float grow, float r, float g, float b)
{
	struct ent *e = &ring[0];
	int i;
	for (i = 1; i < 16 && e->fade; i++) if (!ring[i].fade || ring[i].t0 < e->t0) e = &ring[i];
	memset (e, 0, sizeof (*e));
	memcpy (e->a, p, sizeof (e->a));
	e->t0 = fx_ms; e->fade = fade; e->size = size; e->px = px; e->t = t; e->grow = grow; e->rgb[0] = r; e->rgb[1] = g; e->rgb[2] = b;
	return e;
}
/* when an object was first seen (it is fresh if that is now) */
struct age { unsigned int rec, born, seen; int model; };
static struct age ages[32];
static unsigned int born_at (unsigned int rec, int model, int *fresh)
{
	struct age *s = &ages[0];
	int i;
	for (i = 0; i < 32; i++) {
		if (ages[i].rec == rec && ages[i].model == model && fx_ms - ages[i].seen < 500)
			{ ages[i].seen = fx_ms; *fresh = 0; return ages[i].born; }
		if (ages[i].seen < s->seen) s = &ages[i];
	}
	s->rec = rec; s->model = model; s->born = s->seen = fx_ms; *fresh = 1;
	return fx_ms;
}
/* --- put side --- */
void fx_frame_begin (void)
{
	unsigned int now = SDL_GetTicks (), d = now - last_tick;
	last_tick = now;
	fx_ms += d > 50 ? 50 : d;	/* a long stall (a pause) does not age anything */
}
int fx_tag_for (unsigned int tag[3])
{
	unsigned int rec;
	int m = flat_fx ? 0 : obj (&rec);
	if (!MISSILE (m) && m != M_EXPL && m != M_LASER && m != M_SHIELD && m != M_SMOKE && m != M_RING) return 0;
	fx_log (0, m);
	tag[0] = m; tag[2] = rec;
	tag[1] = m == M_SMOKE ? STMemory_ReadByte (rec + 122) & 0xff : m == M_RING ? STMemory_ReadWord (rec + 116) & 0xffff : 0;
	return 1;
}
/* a laser: each type byte its own beam (halo and core px, afterglow ms, shimmer), a muzzle bloom, a hit flare */
int fx_laser_quad (void)
{
	float v[4][3], n[3], f[3], c[3], t;
	unsigned int rec, a, h;
	int i, j, k, type, rgb = GetReg (REG_D6);
	struct ent *e;
	if (flat_fx || obj (&rec) != M_LASER) return 0;
	for (i = 0; i < 4; i++) {
		if (!readable (a = GetReg (REG_A0 + i) + 4, 12)) return 0;
		for (k = 0; k < 3; k++) v[i][k] = (k == 2 ? -1.0f : 1.0f) * STMemory_ReadLong (a + 4 * k);
	}
	for (i = 0; i < 4; i++) for (j = i + 1; j < 4; j++)	/* the near end's two corners first */
		if (fabsf (v[j][2]) < fabsf (v[i][2])) for (k = 0; k < 3; k++) { t = v[i][k]; v[i][k] = v[j][k]; v[j][k] = t; }
	for (k = 0; k < 3; k++) { n[k] = (v[0][k] + v[1][k]) / 2; f[k] = (v[2][k] + v[3][k]) / 2; }
	for (i = 0; i < 16; i++)	/* the beam's second quad this frame */
		if (ring[i].fade && ring[i].t < 0 && ring[i].t0 == fx_ms &&
		    fabsf (ring[i].a[0] - n[0]) + fabsf (ring[i].a[2] - n[2]) < 0.1f * fabsf (f[2] - n[2]) + 1) return 1;
	fx_log (1, type = STMemory_ReadByte (rec + 123) & 0xff);
	for (k = 0; k < 3; k++) c[k] = ((rgb >> (8 - 4 * k)) & 15) / 15.0f;
	e = ent_new (n, 0, 0, -1, 60, 0, c[0], c[1], c[2]);
	memcpy (e->b, f, sizeof (f));
	h = hash (type, 0x1a5e);
	e->w0 = 9 + (h & 3) * 1.5f; e->w1 = 1.5f + ((h >> 2) & 3) * 0.4f; e->fade = 50 + ((h >> 4) & 3) * 15; e->jit = 0.15f;
	if (type == 0x88) { e->w0 = 10; e->w1 = 2; e->fade = 90; e->jit = 0; }	/* 1MW pulse */
	if (type == 0x9a) { e->w0 = 8; e->w1 = 1.5f; e->fade = 60; e->jit = 0.4f; }	/* mining: a ragged beam */
	ent_new (n, 18, 1, T_GLOW, e->fade, 0, c[0] * .5f + .5f, c[1] * .5f + .5f, c[2] * .5f + .5f);	/* muzzle */
	if ((STMemory_ReadWord (rec + 186) & 0xffff) != 0xffff) ent_new (f, 24, 1, T_GLOW, 150, 0.6f, 1, 0.8f, 0.5f);	/* hit */
	return 1;
}
/* --- draw side (vp_h is last frame's 3D viewport height) --- */
void fx_draw_tag (void **data)
{
	unsigned int *p = *data;
	cur.kind = p[0]; cur.aux = p[1]; cur.rec = p[2];
	*data = p + 3;
}
void fx_prim_done (int mark)
{
	if (mark == 1 && cur.kind) {	/* a shape: its fill is drawn at its end, glow it */
		in_cx = 1;
		if (cur.kind == M_LASER || cur.kind == M_EXPL || cur.kind == M_SHIELD || cur.kind == M_RING) blend (1);
		return;
	}
	if (in_cx && mark != 2) return;
	if (in_cx) blend_end ();
	in_cx = 0; cur.kind = 0;
}
int fx_tri (const float *a, const float *b, const float *c, const int *rgb)
{
	if (cur.kind != M_EXPL && cur.kind != M_LASER && cur.kind != M_RING) return 0;
	blend (1); col (rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 0.7f);
	glBegin (GL_TRIANGLES); glVertex3fv (a); glVertex3fv (b); glVertex3fv (c); glEnd ();
	blend_end ();
	return 1;
}
int fx_line (const int *ia, const int *ib, int r, int g, int b)
{
	float a[3], c[3], m[3], s, f;
	int fresh, i;
	if (!cur.kind || cur.kind == M_SMOKE || cur.kind == M_SHIELD || cur.kind == M_LASER) return 0;
	for (i = 0; i < 3; i++) { a[i] = ia[i]; c[i] = ib[i]; m[i] = (a[i] + c[i]) / 2; }
	if (MISSILE (cur.kind) && !r && g == 0xe0 && b == 0xe0) {	/* the exhaust: a plume */
		s = 1 + (cur.kind - M_MISSILE) * 0.075f + (cur.kind == M_NUKE) * 0.175f;	/* 1 1.15 1.3 1.4 */
		f = 0.8f + 0.4f * hashf (cur.rec, fx_ms / 33);
		blend (1);
		col (0.3f, 0.9f, 1, 0.8f); ribbon (a, c, 7 * s * f, 0);
		col (0.9f, 1, 1, 1); ribbon (a, c, 3 * s, 0);
		col (0.6f, 0.95f, 1, 0.9f); sprite (a, 7 * s * f * px_world (a[2]), 0, T_GLOW);
		if (cur.kind == M_NUKE && (fx_ms / 500) & 1) { col (1, .35f, .1f, 1); sprite (a, 12 * px_world (a[2]), 0, T_GLOW); }	/* armed */
		blend_end ();
		return 1;
	}
	if (cur.kind == M_RING) {	/* ECM: a fast cyan ring; bomb: a slow white one */
		born_at (cur.rec, cur.kind, &fresh);
		f = sqrtf ((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2])) / 2;
		if (fresh && (cur.aux & 0x20)) ent_new (m, f, 0, T_RING, 300, 3, 0.4f, 0.9f, 1);
		if (fresh && (cur.aux & 0x40)) ent_new (m, f, 0, T_RING, 700, 6, 1, 1, 1);
	}
	blend (!MISSILE (cur.kind));	/* missile trim stays opaque, the rest glows */
	col (r / 255.0f, g / 255.0f, b / 255.0f, 0.9f); ribbon (a, c, MISSILE (cur.kind) ? 2 : 3, MISSILE (cur.kind) ? 2 : 3);
	blend_end ();
	return 1;
}
/* fire is additive, smoke is not: an explosion cools to smoke */
static void puff (const float *p, float rad, int r, int g, int b, float a, unsigned int born, unsigned int seed)
{
	int hot = r >= 0xc0 && g >= 0x40 && fx_ms - born < 1200;
	float grey = (r + g + b) / (3 * 255.0f) * 0.7f + 0.15f;
	blend (hot);
	col (hot ? r / 255.0f : grey, hot ? g / 255.0f : grey * .97f, hot ? b / 255.0f : grey * .93f, a);
	sprite (p, rad, hashf (cur.rec, seed) * 6.283f + fx_ms * 0.0004f, T_PUFF);
	blend_end ();
}
int fx_circle (const int *iv, unsigned int d2, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * ((short) d2 + (d2 >> 16) / 65536.0f) * fabsf (v[2]);
	unsigned int born;
	int fresh, i;
	if (cur.kind == M_EXPL) {	/* fire: a flash and a shock ring when first seen */
		born = born_at (cur.rec, cur.kind, &fresh);
		if (fresh) ent_new (v, 3 * R, 0, T_GLOW, 250, 0, 1, 0.95f, 0.8f)->rec = cur.rec;
		if (fresh) ent_new (v, 0.5f * R, 0, T_RING, 600, 7, 1, 0.7f, 0.4f)->rec = cur.rec;
		for (i = 0; i < 16; i++) if (ring[i].fade && ring[i].rec == cur.rec) memcpy (ring[i].a, v, sizeof (v));
		puff (v, 1.5f * R, r, g, b, 0.7f, born, d2);
		return 1;
	}
	if (cur.kind != M_SMOKE && !MISSILE (cur.kind)) return 0;
	blend (1); col (r / 255.0f, g / 255.0f, b / 255.0f, 0.6f);	/* sparks, a far missile: a halo */
	sprite (v, 3 * fmaxf (R, 4 * px_world (v[2])), 0, T_GLOW);
	blend_end ();
	return cur.kind == M_SMOKE;
}
int fx_blob (const int *iv, int rad, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * (rad < 2 ? 2 : rad) * fabsf (v[2]), a = (cur.aux & 0x7f) / 127.0f;
	int fresh;
	if (cur.kind == M_EXPL) { puff (v, 1.5f * R, r, g, b, 0.7f, born_at (cur.rec, cur.kind, &fresh), rad); return 1; }
	if (cur.kind != M_SMOKE) return 0;
	puff (v, R * (1.6f - 0.6f * a), r, g, b, 0.85f * a + 0.1f, 0, rad);	/* smoke spreads as it ages */
	if (cur.aux > 0x60) { blend (1); col (1, 0.6f, 0.2f, a); sprite (v, 8 * px_world (v[2]), 0, T_GLOW); blend_end (); }
	return 1;
}
void fx_frame_end (void)
{
	GLint vp[4];
	float age, j, *c;
	int i;
	glGetIntegerv (GL_VIEWPORT, vp);
	vp_h = vp[3] > 0 ? vp[3] : 480;
	blend (1);
	for (i = 0; i < 16; i++) {
		struct ent *e = &ring[i];
		if (!e->fade || fx_ms - e->t0 >= e->fade) { e->fade = 0; continue; }
		age = (float) (fx_ms - e->t0) / e->fade; c = e->rgb;
		if (e->t < 0) {	/* a beam: a halo in the game's colour round a near-white core */
			j = 1 + e->jit * (2 * hashf (i, fx_ms / 16) - 1);
			col (c[0], c[1], c[2], 0.95f * (1 - age)); ribbon (e->a, e->b, e->w0 * j * 1.4f, e->w0 * j * 0.6f);
			col (c[0] * .4f + .6f, c[1] * .4f + .6f, c[2] * .4f + .6f, 1 - age); ribbon (e->a, e->b, e->w1 * j * 2, e->w1 * j * 0.8f);
		} else {
			col (c[0], c[1], c[2], 1 - age);
			sprite (e->a, e->size * (1 + e->grow * age) * (e->px ? px_world (e->a[2]) : 1), 0, e->t);
		}
	}
	blend_end ();
	cur.kind = 0; in_cx = 0;
}
