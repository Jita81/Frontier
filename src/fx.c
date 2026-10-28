/* fx: HD lasers, missiles, explosions, smoke, shields, mines, ECM/bomb (docs/hd/weapons.md).  The object being put is read
 * (never written) from ST memory and rides to the draw side as an NU_FX op.  Procedural: 3 textures, an afterglow ring, a hash. */
#include <SDL.h>
#include <SDL_opengl.h>
#include "main.h"
#include "../m68000.h"
#include "../host.h"	/* MEM_SIZE */
#include "../fixups.h"	/* __DL5eb6_a6_base: 10428(a6) is the game's time rate, 0 when stopped */
#include "fx.h"
int flat_fx = 0;
#define MISSILE(m)	((m) >= M_MISSILE && (m) <= M_NUKE && !((m) & 1))
#define MINE(m)		((m) == 6 || (m) == 8)
#define NR		32
enum { M_MISSILE = 10, M_NUKE = 16, M_EXPL = 188, M_LASER = 190, M_SHIELD = 192, M_SMOKE = 194, M_RING = 200 };
enum { T_GLOW, T_PUFF, T_RING, T_N, T_FLASH = T_N };
static unsigned int fx_ms, last_tick; static int vp_h = 480, log_on = -1, in_cx, live; static GLuint tex[T_N];
static struct { unsigned int kind, aux, rec; } cur;
static struct { unsigned int rec, kind, n; float v[24][3]; } ms[4]; static float sm[256][3]; static int nms, nsm;	/* missiles, smoke */
static int readable (unsigned int a, unsigned int n) { return a <= MEM_SIZE && n <= MEM_SIZE - a; }
static float hashf (unsigned int a, unsigned int b)	/* 0..1 */
	{ unsigned int h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu; h = (h ^ h >> 15) * 0x2c1b3c6du; return ((h ^ h >> 12) & 0xffff) / 65535.0f; }
static int obj (unsigned int *rec) { unsigned int a6 = GetReg (REG_A6);	/* the drawn object (A6-212 holds its record): model, or 0 */
	return readable (a6 - 212, 4) && readable (*rec = STMemory_ReadLong (a6 - 212), 192) ? STMemory_ReadWord (*rec + 90) & 0xffff : 0; }
static void fx_log (int what, int v) { static unsigned char seen[2][256];	/* FE2_FX_LOG=1: each model and laser type met, once */
	if ((log_on < 0 ? log_on = getenv ("FE2_FX_LOG") != NULL : log_on) && v < 256 && !seen[what][v] && ++seen[what][v]) fprintf (stderr, "fx: %s $%02x\n", what ? "laser type" : "model", v); }
static float vnoise (float x, float y)	/* smooth value noise on the hash lattice */
{	int i = (int) x, j = (int) y;
	float u = (x - i) * (x - i) * (3 - 2 * (x - i)), v = (y - j) * (y - j) * (3 - 2 * (y - j)), a, b;
	a = hashf (i, j) + (hashf (i + 1, j) - hashf (i, j)) * u; b = hashf (i, j + 1) + (hashf (i + 1, j + 1) - hashf (i, j + 1)) * u;
	return a + (b - a) * v;
}
/* glow: two gaussians; puff: two octaves of value noise in a soft disc; ring */
static void make_tex (void)
{
	static unsigned char px[64 * 64 * 2]; int t, i;
	glGenTextures (T_N, tex);
	for (t = 0; t < T_N; t++) {
		for (i = 0; i < 64 * 64; i++) {
			float u = (i % 64 + 0.5f) / 32 - 1, v = (i / 64 + 0.5f) / 32 - 1, r2 = u * u + v * v, r = sqrtf (r2), a;
			float n = 0.65f * vnoise (3 * u + 8, 3 * v + 8) + 0.35f * vnoise (7 * u + 8, 7 * v + 8);
			a = t == T_GLOW ? 0.55f * expf (-4 * r2) + 0.45f * expf (-28 * r2)
			  : r >= 1 ? 0 : t == T_RING ? expf (-125 * (r - 0.8f) * (r - 0.8f)) : powf (1 - r, 1.2f) * (0.3f + 1.1f * n);
			px[2 * i] = t == T_PUFF ? 255 * (0.65f + 0.35f * n) : 255;
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
static void use_tex (int t)	/* texture times colour, whatever env the panel left */
	{ if (!tex[0]) make_tex (); glEnable (GL_TEXTURE_2D); glBindTexture (GL_TEXTURE_2D, tex[t]); glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); }
static void col (float r, float g, float b, float a) { glColor4f (r, g, b, a > 1 ? 1 : a); }
static void sprite (const float *p, float r, float rot, int t)
{
	float c, s, x, y, lim = 0.5f * vp_h * px_world (p[2]);
	int k;
	if (p[2] > -1) return;
	r = r > lim ? lim : r; c = cosf (rot) * r; s = sinf (rot) * r;	/* never more than the screen */
	use_tex (t); glBegin (GL_QUADS);
	for (k = 0; k < 4; k++) x = k == 1 || k == 2 ? 1 : -1, y = k < 2 ? -1 : 1, glTexCoord2f (x > 0, y > 0), glVertex3f (p[0] + x * c - y * s, p[1] + x * s + y * c, p[2]);
	glEnd ();
}
/* a glowing strip from a to b, wa and wb pixels wide at its ends; s/q runs screen-affine so a 2000:1 depth span keeps its core */
static void ribbon (const float *a, const float *b, float wa, float wb)
{
	float p[2][3], d[3], s[3], l, t, h;
	int i, k;
	if (memcpy (p[0], a, sizeof (p[0])), memcpy (p[1], b, sizeof (p[1])), p[0][2] > -1 && p[1][2] > -1) return;
	for (k = 0; k < 2; k++) if (p[k][2] > -1) {	/* clip at the near plane */
		t = (-1 - p[!k][2]) / (p[k][2] - p[!k][2]);
		for (i = 0; i < 3; i++) p[k][i] = p[!k][i] + (p[k][i] - p[!k][i]) * t;
	}
	for (i = 0; i < 3; i++) d[i] = p[1][i] - p[0][i];
	s[0] = d[1] * p[0][2] - d[2] * p[0][1]; s[1] = d[2] * p[0][0] - d[0] * p[0][2]; s[2] = d[0] * p[0][1] - d[1] * p[0][0];
	if (!((l = sqrtf (s[0] * s[0] + s[1] * s[1] + s[2] * s[2])) > 0)) return;
	use_tex (T_GLOW); glBegin (GL_TRIANGLE_STRIP);
	for (k = 0; k < 4; k++) {
		h = (k & 1 ? 1.25f : -1.25f) * (k < 2 ? wa : wb) * px_world (p[k >> 1][2]) / l;
		t = -p[k >> 1][2]; glTexCoord4f ((k & 1) * t, .5f * t, 0, t); glVertex3f (p[k >> 1][0] + s[0] * h, p[k >> 1][1] + s[1] * h, p[k >> 1][2] + s[2] * h);
	}
	glEnd ();
}
/* --- what outlives its frame: beams (t -1), sprites and the bomb flash, all additive --- */
static struct ent { unsigned int rec, t0, fade; int t, px; float a[3], b[3], size, grow, w0, w1, jit, rgb[3]; } ring[NR];
static struct ent *ent_new (const float *p, float size, int px, int t, unsigned int fade, float grow, float r, float g, float b)
{	struct ent *e = &ring[0]; int i;
	for (i = 1; i < NR && e->fade; i++) if (!ring[i].fade || ring[i].t0 < e->t0) e = &ring[i];
	memset (e, 0, sizeof (*e)); memcpy (e->a, p, sizeof (e->a));
	e->t0 = fx_ms; e->fade = fade; e->size = size; e->px = px; e->t = t; e->grow = grow; e->rgb[0] = r; e->rgb[1] = g; e->rgb[2] = b;
	return e;
}
/* when an object was first seen (it is fresh if that is now) */
static struct age { unsigned int rec, born, seen; int model; } ages[32];
static unsigned int born_at (unsigned int rec, int model, int *fresh)
{	struct age *s = &ages[0]; int i;
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
{	unsigned int now = SDL_GetTicks (), d = now - last_tick;
	last_tick = now; live = 1; nms = nsm = 0;
	if (d > 250) memset (ring, 0, sizeof (ring));	/* back from a 2D screen: nothing stale */
	if (STMemory_ReadLong (__DL5eb6_a6_base + 10428)) fx_ms += d > 50 ? 50 : d;	/* time stopped: all holds */
}
int fx_tag_for (unsigned int tag[3])
{
	unsigned int rec;
	int m = flat_fx ? 0 : obj (&rec);
	if (!MISSILE (m) && !MINE (m) && m != M_EXPL && m != M_LASER && m != M_SHIELD && m != M_SMOKE && m != M_RING) return 0;
	fx_log (0, m);
	tag[0] = m; tag[2] = rec;
	tag[1] = m == M_SMOKE ? STMemory_ReadByte (rec + 122) & 0xff : m == M_RING ? STMemory_ReadWord (rec + 116) & 0xffff : 0;
	return 1;
}
/* lasers by the class in bits 3-6 of the firer's laser byte (fe2.s L45f1e decodes it the same way):
 * 1-2 pulse, 3 mining, 4-7 beam, 8-9 plasma.  Halo px, core px x2, afterglow ms/10, shimmer %, core white % */
static const unsigned char sty[10][5] = { { 10, 4, 9, 0, 60 }, { 10, 4, 9, 0, 60 }, { 14, 6, 11, 0, 65 }, { 8, 3, 6, 40, 50 },
	{ 6, 3, 4, 5, 70 }, { 8, 4, 4, 5, 72 }, { 10, 5, 4, 5, 75 }, { 13, 6, 4, 5, 80 }, { 18, 10, 14, 10, 90 }, { 24, 14, 14, 10, 92 } };
int fx_laser_quad (void)
{
	float v[4][3], n[3], f[3], c[3], t, m;
	unsigned int rec, a;
	int i, j, k, type, rgb = GetReg (REG_D6);
	const unsigned char *s;
	struct ent *e;
	if (flat_fx || obj (&rec) != M_LASER) return 0;
	for (i = 0; i < 4; i++) {
		if (!readable (a = GetReg (REG_A0 + i) + 4, 12)) return 0;
		for (k = 0; k < 3; k++) v[i][k] = (k == 2 ? -1.0f : 1.0f) * STMemory_ReadLong (a + 4 * k);
	}
	for (i = 0; i < 4; i++) for (j = i + 1; j < 4; j++)	/* the near end's two corners first */
		if (fabsf (v[j][2]) < fabsf (v[i][2])) for (k = 0; k < 3; k++) { t = v[i][k]; v[i][k] = v[j][k]; v[j][k] = t; }
	for (k = 0; k < 3; k++) { n[k] = (v[0][k] + v[1][k]) / 2; f[k] = (v[2][k] + v[3][k]) / 2; }
	for (i = 0; i < NR; i++)	/* the beam's second quad this frame */
		if (ring[i].fade && ring[i].t < 0 && ring[i].t0 == fx_ms &&
		    fabsf (ring[i].a[0] - n[0]) + fabsf (ring[i].a[2] - n[2]) < 0.1f * fabsf (f[2] - n[2]) + 1) return 1;
	fx_log (1, type = STMemory_ReadByte (rec + 123) & 0xff);
	s = sty[(type >> 3 & 15) < 10 ? type >> 3 & 15 : 0];
	for (k = 0, m = 0; k < 3; k++) if ((c[k] = ((rgb >> (8 - 4 * k)) & 15) / 15.0f) > m) m = c[k];
	for (k = 0; k < 3; k++) c[k] = m > 0 ? c[k] / m : 1;	/* the game's hue at full strength */
	e = ent_new (n, s[4] / 100.0f, 0, -1, s[2] * 10, s[0] >= 18 ? 0.4f : 0, c[0], c[1], c[2]);
	memcpy (e->b, f, sizeof (f));
	e->w0 = s[0]; e->w1 = s[1] / 2.0f; e->jit = s[3] / 100.0f;
	if (n[2] > -1.5f && f[2] < -1.5f) for (t = (-1.5f - n[2]) / (f[2] - n[2]), k = 0; k < 3; k++) n[k] += (f[k] - n[k]) * t;
	ent_new (n, 2 * s[0], 1, T_GLOW, e->fade, 0.3f, c[0] * .4f + .6f, c[1] * .4f + .6f, c[2] * .4f + .6f);	/* muzzle */
	if ((STMemory_ReadWord (rec + 186) & 0xffff) == 0xffff) return 1;
	ent_new (f, 2.5f * s[0], 1, T_GLOW, 180, 0.6f, 1, 0.85f, 0.55f);	/* hit flare */
	for (i = 0; s == sty[3] && i < 4; i++) {	/* mining: sparks off the rock */
		for (k = 0; k < 3; k++) v[0][k] = f[k] + (hashf (fx_ms + i, k) - .5f) * 30 * px_world (f[2]);
		ent_new (v[0], 4, 1, T_GLOW, 250 + 60 * i, 0.5f, 1, .75f, .35f);
	}
	return 1;
}
/* --- draw side (vp_h is last frame's 3D viewport height) --- */
void fx_draw_tag (void **data) { memcpy (&cur, *data, sizeof (cur)); *data = (char *) *data + sizeof (cur); }
int fx_tagged (void) { return cur.kind != 0; }
void fx_prim_done (int mark)
{
	if (mark == 1 && cur.kind) {	/* a shape: its fill is drawn at its end, glow it */
		if (in_cx = 1, cur.kind == M_LASER || cur.kind == M_EXPL || cur.kind == M_SHIELD || cur.kind == M_RING) blend (1);
	} else if (!in_cx || mark == 2) {
		if (in_cx) blend_end ();
		in_cx = 0; cur.kind = 0;
	}
}
static void beacon (const float *p)	/* a mine: a slow red blink, once a frame, over the view */
{	int i;
	if (!MINE (cur.kind) || (fx_ms + cur.rec * 77) % 1100 > 220) return;
	for (i = 0; i < NR; i++) if (ring[i].fade && ring[i].rec == cur.rec && ring[i].t0 == fx_ms) return;
	ent_new (p, 10, 1, T_GLOW, 1, 0, 1, .15f, .1f)->rec = cur.rec;
}
static int shield (const float *v, float R)	/* toward 0x8cf: a soft glow and a turning rim */
{	if (cur.kind != M_SHIELD) return 0;
	blend (1); col (.53f, .8f, 1, .45f); sprite (v, 1.4f * R, 0, T_GLOW);
	col (.6f, .85f, 1, .9f); sprite (v, 1.25f * R, fx_ms * 0.001f, T_RING); blend_end (); return 1;
}
static float d2 (const float *a, const float *b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]); }
static void body (const float *p)	/* a missile's body vertex, kept for its plume */
{	int j;
	for (j = 0; j < nms && ms[j].rec != cur.rec; j++) ;
	if (j == nms && nms < 4) nms++, ms[j].rec = cur.rec, ms[j].kind = cur.kind, ms[j].n = 0;
	if (j < nms && ms[j].n < 24) memcpy (ms[j].v[ms[j].n++], p, sizeof (ms[j].v[0])); }
int fx_tri (const float *a, const float *b, const float *c, const int *rgb)
{	float k = cur.kind == M_SHIELD ? 0.5f : 0;
	if (MISSILE (cur.kind)) body (a), body (b), body (c);
	beacon (a);
	if (cur.kind != M_EXPL && cur.kind != M_LASER && cur.kind != M_RING && cur.kind != M_SHIELD) return 0;
	blend (1); col (rgb[0] / 255.0f * (1 - k) + .53f * k, rgb[1] / 255.0f * (1 - k) + .8f * k, rgb[2] / 255.0f * (1 - k) + k, 0.7f);
	glBegin (GL_TRIANGLES); glVertex3fv (a); glVertex3fv (b); glVertex3fv (c); glEnd (); blend_end (); return 1;
}
static const float plume[4][3] = { { .3f, .85f, 1 }, { .4f, 1, .55f }, { .75f, .5f, 1 }, { 1, .55f, .2f } };
int fx_line (const int *ia, const int *ib, int r, int g, int b)
{
	float a[3], c[3], m[3], f;
	int fresh, i;
	r &= 255; g &= 255; b &= 255;	/* the reader's chars are signed */
	if (!cur.kind || cur.kind == M_SMOKE || cur.kind == M_SHIELD || cur.kind == M_LASER) return 0;
	for (i = 0; i < 3; i++) { a[i] = ia[i]; c[i] = ib[i]; m[i] = (a[i] + c[i]) / 2; }
	if (MINE (cur.kind)) { beacon (a); return 0; }
	if (cur.kind == M_RING) {	/* ECM: a fast cyan ring; bomb: a slow white one and a flash */
		born_at (cur.rec, cur.kind, &fresh);
		f = sqrtf ((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2])) / 2;
		if (fresh && (cur.aux & 0x20)) ent_new (m, f, 0, T_RING, 300, 3, 0.4f, 0.9f, 1);
		if (fresh && (cur.aux & 0x40)) ent_new (m, f, 0, T_RING, 700, 6, 1, 1, 1), ent_new (m, 0, 0, T_FLASH, 450, 0, 1, .95f, .85f);
	}
	blend (!MISSILE (cur.kind));	/* missile trim stays opaque, the rest glows */
	col (r / 255.0f, g / 255.0f, b / 255.0f, 0.9f); ribbon (a, c, MISSILE (cur.kind) ? 2 : 3, MISSILE (cur.kind) ? 2 : 3); blend_end (); return 1;
}
/* fire is additive, smoke is not: an explosion cools to smoke */
static void puff (const float *p, float rad, int r, int g, int b, float a, unsigned int born, unsigned int seed)
{
	int hot = r >= 0xc0 && g >= 0x40 && fx_ms - born < 1200;
	float grey = (r + g + b) / (3 * 255.0f) * 0.7f + 0.15f;
	blend (hot);
	col (hot ? r / 255.0f : grey, hot ? g / 255.0f : grey * .97f, hot ? b / 255.0f : grey * .93f, a);
	sprite (p, rad, hashf (cur.rec, seed) * 6.283f + fx_ms * 0.0004f, T_PUFF); blend_end ();
}
int fx_circle (const int *iv, unsigned int d2, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * ((short) d2 + (d2 >> 16) / 65536.0f) * fabsf (v[2]);
	const float *h = plume[(cur.kind - M_MISSILE) / 2 & 3];
	unsigned int born; int fresh, i;
	r &= 255; g &= 255; b &= 255;	/* the reader's chars are signed */
	beacon (v);
	if (shield (v, R)) return 1;
	if (cur.kind == M_EXPL) {	/* fire: a flash and a shock ring when first seen */
		born = born_at (cur.rec, cur.kind, &fresh);
		if (fresh) ent_new (v, 3 * R, 0, T_GLOW, 250, 0, 1, 0.95f, 0.8f)->rec = cur.rec, ent_new (v, 0.5f * R, 0, T_RING, 600, 7, 1, 0.7f, 0.4f)->rec = cur.rec;
		for (i = 0; i < NR; i++) if (ring[i].fade && ring[i].rec == cur.rec) memcpy (ring[i].a, v, sizeof (v));
		puff (v, 1.5f * R, r, g, b, 0.7f, born, d2);
		return 1;
	}
	if (cur.kind != M_SMOKE && !MISSILE (cur.kind)) return 0;
	blend (1);	/* sparks; a far missile: a spark in its exhaust colour */
	if (MISSILE (cur.kind)) col (h[0], h[1], h[2], 0.9f); else col (r / 255.0f, g / 255.0f, b / 255.0f, 0.6f);
	sprite (v, 3 * fmaxf (R, 4 * px_world (v[2])), 0, T_GLOW); blend_end (); return cur.kind == M_SMOKE;
}
int fx_blob (const int *iv, int rad, int r, int g, int b)
{
	float v[3] = { iv[0], iv[1], iv[2] }, R = 0.002f * (rad < 2 ? 2 : rad) * fabsf (v[2]), a = (cur.aux & 0x7f) / 127.0f;
	int fresh;
	beacon (v);
	if (shield (v, R)) return 1;
	if (cur.kind == M_EXPL) { puff (v, 1.5f * R, r, g, b, 0.7f, born_at (cur.rec, cur.kind, &fresh), rad); return 1; }
	if (cur.kind != M_SMOKE) return 0;
	if (nsm < 256) memcpy (sm[nsm++], v, sizeof (v));
	puff (v, R * (1.6f - 0.6f * a), r, g, b, 0.85f * a + 0.1f, 0, rad);	/* smoke spreads as it ages */
	if (cur.aux > 0x60) { blend (1); col (1, 0.6f, 0.2f, a); sprite (v, 8 * px_world (v[2]), 0, T_GLOW); blend_end (); }
	return 1;
}
void fx_frame_end (void)
{
	GLint vp[4]; float age, j, w, *c, *a, *b, *t, best, L, s, e[3], g[3]; int i, k, n, near;
	if (cur.kind = in_cx = 0, flat_fx || !live) return;	/* no 3D view this frame (a 2D screen): nothing over it */
	live = 0; glGetIntegerv (GL_VIEWPORT, vp); vp_h = vp[3] > 0 ? vp[3] : 480;
	blend (1);
	for (i = 0; i < nms; i++) {	/* a missile burns where its smoke starts: a plume off the body's end nearer the smoke */
		const float *h = plume[(ms[i].kind - M_MISSILE) / 2 & 3];
		for (a = b = ms[i].v[0], n = ms[i].n, k = 0; k < n * n; k++) if (d2 (ms[i].v[k / n], ms[i].v[k % n]) > d2 (a, b)) a = ms[i].v[k / n], b = ms[i].v[k % n];
		for (best = 1e30f, near = k = 0; k < nsm; k++) if ((j = fminf (d2 (sm[k], a), d2 (sm[k], b))) < best) best = j, near = d2 (sm[k], a) < d2 (sm[k], b);
		if (best > 36 * (L = d2 (a, b)) || !(L > 0)) continue;	/* no smoke near it: the motor is cold */
		t = near ? a : b; c = near ? b : a; s = 1 + (ms[i].kind - M_MISSILE) * 0.1f; j = 0.8f + 0.4f * hashf (ms[i].rec, fx_ms / 33);
		for (k = 0; k < 3; k++) e[k] = t[k] + (t[k] - c[k]) * 1.3f * j, g[k] = t[k] + (t[k] - c[k]) * 0.5f * j;
		w = fmaxf (2, fminf (0.22f * sqrtf (L) / px_world (t[2]), 36)) * s * j;
		col (h[0], h[1], h[2], 0.85f); ribbon (t, e, w, 0);
		if (ms[i].kind == M_NUKE) col (1, 1, 1, 1); else col (h[0] * .3f + .7f, h[1] * .3f + .7f, h[2] * .3f + .7f, 1);
		ribbon (t, g, w * (ms[i].kind == M_NUKE ? .6f : .35f), 0); sprite (t, 0.8f * w * px_world (t[2]), 0, T_GLOW);	/* core, nozzle */
		if (ms[i].kind == M_NUKE && (fx_ms / 500) & 1) col (1, .3f, .1f, 1), sprite (t, 1.6f * w * px_world (t[2]), 0, T_GLOW);	/* armed */
	}
	for (i = 0; i < NR; i++) {
		struct ent *e = &ring[i];
		if (!e->fade || fx_ms - e->t0 >= e->fade) { e->fade = 0; continue; }
		age = (float) (fx_ms - e->t0) / e->fade; c = e->rgb;
		if (e->t < 0) {	/* a beam: bloom and halo in the game's hue round a near-white core */
			j = (1 + e->jit * (2 * hashf (i, fx_ms / 16) - 1)) * (1 + e->grow * age); w = e->size; age = age * age;	/* held bright, then gone */
			col (c[0], c[1], c[2], 0.9f * (1 - age)); ribbon (e->a, e->b, e->w0 * j * 5, e->w0 * j * 2);
			col (c[0], c[1], c[2], 1 - age); ribbon (e->a, e->b, e->w0 * j * 1.6f, e->w0 * j * 0.6f);
			col (c[0] * (1 - w) + w, c[1] * (1 - w) + w, c[2] * (1 - w) + w, 1 - age); ribbon (e->a, e->b, e->w1 * j * 2.5f, e->w1 * j * 1.2f);
		} else if (e->t == T_FLASH) {	/* the bomb: the whole view whites out */
			glDisable (GL_TEXTURE_2D); col (c[0], c[1], c[2], 0.7f * (1 - age) * (1 - age));
			glBegin (GL_QUADS); glVertex3f (-3, -3, -2); glVertex3f (3, -3, -2); glVertex3f (3, 3, -2); glVertex3f (-3, 3, -2); glEnd ();
		} else
			col (c[0], c[1], c[2], 1 - age), sprite (e->a, e->size * (1 + e->grow * age) * (e->px ? px_world (e->a[2]) : 1), 0, e->t);
	}
	blend_end ();
}
