/*
 * The green ship's cockpit (cockpit.h; docs/hd/hulls.md, "The cockpit").
 * The oval's curves are half ellipses, so the first gives its centre and
 * axes, per instance and in eye space. Everything is convex and made in
 * code; nothing is kept between frames but the pilots' positions.
 */
#include <math.h>
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "cockpit.h"

#define MAX_PILOTS	8
#define SEGS		56
#define RINGS		12
#define TURN		6.2831853f
#define WELL		0.30f	/* the cabin's depth against the minor radius */
#define HEIGHT		0.46f	/* the cap's height, the same: under the frame */
#define HEAD		0.08f	/* the pilot's head radius, the same */
#define PX_LO		0.012f	/* major radius against the view: nothing below */
#define PX_HI		0.028f	/* all of it above */
#define FLAT_LO		0.02f	/* n.v at which the cap lies flat on the oval */
#define FLAT_HI		0.11f	/* and has its full height */
#define EDGE_ON		0.02f	/* |n.v| below this the game keeps its own pilot */

typedef float V3[3];
struct part { V3 c, up; float ru, rs; const float *rgb; };

static struct { V3 e; float rgb[3], took; } pilot[MAX_PILOTS];
static int pilots, arcs;
static V3 arc[4];
/* this cockpit: centre, unit axes (a major and aft, b minor, n out of the
 * hull), to the light, to the eye; radii; how far on; the floor's slide */
static V3 ce, ax, bx, nx, li, eye;
static float ra, rb, fade, floor_at[2];
static float cs[SEGS + 1][2];

static float dot (const V3 a, const V3 b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void cross (V3 o, const V3 a, const V3 b)
{
	o[0] = a[1]*b[2] - a[2]*b[1];
	o[1] = a[2]*b[0] - a[0]*b[2];
	o[2] = a[0]*b[1] - a[1]*b[0];
}
static float norm (V3 v)
{
	float l = sqrtf (dot (v, v));
	if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
	return l;
}
static float dist2 (const V3 a, const V3 b)
{
	V3 d = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
	return dot (d, d);
}
static float smooth (float lo, float hi, float x)
{
	x = (x - lo) / (hi - lo);
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x * x * (3 - 2 * x);
}
/* a point of the oval's frame: u, v in radii, h along n */
static void at (V3 o, float u, float v, float h)
{
	int k;
	for (k = 0; k < 3; k++) o[k] = ce[k] + u * ra * ax[k] + v * rb * bx[k] + h * nx[k];
}

void cockpit_begin (void)
{
	pilots = arcs = 0;
}

void cockpit_pilot_put (const int e[3], int rgb444)
{
	int k;

	if (pilots == MAX_PILOTS) return;
	for (k = 0; k < 3; k++) {
		pilot[pilots].e[k] = e[k];
		pilot[pilots].rgb[k] = ((rgb444 >> (8 - 4 * k)) & 15) / 15.0f;
	}
	pilot[pilots++].took = 0;
}

void cockpit_arc (float cp[4][3])
{
	int i, k;

	if (arcs++ == 0)
		for (i = 0; i < 4; i++)
			for (k = 0; k < 3; k++) arc[i][k] = cp[i][k];
}

int cockpit_replaced (float x, float y, float z)
{
	const V3 v = { x, y, z };
	int i;

	for (i = 0; i < pilots; i++)
		if (dist2 (v, pilot[i].e) < pilot[i].took) return 1;
	return 0;
}

/* the cabin: a dark well seen through the opening. Its floor slides with
 * the view, so it sits below the hull; blacker to the rim, a faint glow of
 * instruments forward */
static void cabin_colour (float u, float v)
{
	float du = u - floor_at[0], dv = v - floor_at[1];
	float fl = 1 - smooth (0.35f, 0.85f, sqrtf (du * du + dv * dv));
	float dim = 1 - 0.9f * smooth (0.72f, 1.0f, sqrtf (u * u + v * v));
	float g = expf (-(u + 0.72f) * (u + 0.72f) / 0.03f - v * v / 0.28f) * dim;

	glColor4f ((0.080f - 0.045f * fl) * dim + 0.06f * g, (0.095f - 0.052f * fl) * dim + 0.18f * g,
		   (0.120f - 0.062f * fl) * dim + 0.20f * g, fade);
}

static void cabin (void)
{
	static const float rho[] = { 0.0f, 0.2f, 0.4f, 0.56f, 0.7f, 0.82f, 0.92f, 1.0f };
	int i, j, l;

	for (i = 0; i < 7; i++) {
		glBegin (GL_TRIANGLE_STRIP);
		for (j = 0; j <= SEGS; j++)
			for (l = 1; l >= 0; l--) {
				float u = rho[i + l] * cs[j][0], v = rho[i + l] * cs[j][1];
				V3 p;
				cabin_colour (u, v);
				at (p, u, v, 0);
				glVertex3fv (p);
			}
		glEnd ();
	}
}

/* below the plane a point shows only through the opening: one the rim
 * would hide comes back onto the rim */
static void sighted (V3 p)
{
	float t = dot (nx, ce) / dot (nx, p), u, v, r;
	V3 x;
	int k;

	if (dot (nx, p) > dot (nx, ce)) return;
	for (k = 0; k < 3; k++) x[k] = t * p[k] - ce[k];
	u = dot (x, ax) / ra;
	v = dot (x, bx) / rb;
	r = sqrtf (u * u + v * v);
	if (r > 0.97f) at (p, u * 0.97f / r, v * 0.97f / r, 0);
}

/* a small lit part: the ellipsoid about axis up, radii ru along and rs
 * across, as the ellipse it shows the eye, shaded as a ball */
static void blob (const struct part *b)
{
	enum { N = 14 };
	V3 v = { -b->c[0], -b->c[1], -b->c[2] }, s, u, p, m;
	float q, ry;
	int j, k;

	norm (v);
	cross (s, b->up, v);
	q = norm (s);
	if (q < 0.01f) {
		cross (s, bx, v);
		norm (s);
	}
	cross (u, v, s);
	ry = sqrtf (b->ru * b->ru * q * q + b->rs * b->rs * (1 - q * q));
	glBegin (GL_TRIANGLE_FAN);
	for (j = -1; j <= N; j++) {
		float a = j * TURN / N, r = j < 0 ? 0 : 1, x = r * cosf (a), y = r * sinf (a), d;
		for (k = 0; k < 3; k++) {
			m[k] = 0.8f * (x * s[k] + y * u[k]) + (1 - 0.8f * r) * v[k];
			p[k] = b->c[k] + x * b->rs * s[k] + y * ry * u[k];
		}
		norm (m);
		d = 0.40f + 0.8f * fmaxf (dot (m, li), 0);
		glColor4f (b->rgb[0] * d, b->rgb[1] * d, b->rgb[2] * d, fade);
		sighted (p);
		glVertex3fv (p);
	}
	glEnd ();
}

/* the seat, then in it the figure about the game's head, far parts first */
static void figure (const float *hd, const float *helmet)
{
	static const float seat[3] = { 0.24f, 0.25f, 0.28f }, suit[3] = { 0.30f, 0.28f, 0.12f };
	static const float visor[3] = { 0.30f, 0.62f, 0.68f };
	/* in head radii from the head: down his back, aft, forward; radii */
	static const struct { float down, aft, fore, ru, rs; const float *rgb; } shape[4] = {
		{ 1.5f, 1.2f, 0, 1.6f, 2.0f, seat }, { 1.45f, 0.5f, 0, 0.95f, 1.5f, suit },
		{ 0, 0, 0, 1, 1, NULL }, { -0.12f, 0, 0.7f, 0.3f, 0.72f, visor }
	};
	const float q = HEAD * rb;
	struct part b[4], t;
	V3 seated, back, aft;
	int i, j, k, n = helmet ? 4 : 1;

	if (!hd) {	/* no pilot: the seat alone, on the centre line, aft */
		at (seated, 0.2f, 0, 3 * q);
		hd = seated;
	}
	for (k = 0; k < 3; k++) {
		back[k] = 0.45f * ax[k] + 0.89f * nx[k];	/* he sits up, leaning back */
		aft[k] = 0.89f * ax[k] - 0.45f * nx[k];
	}
	for (i = 0; i < n; i++) {
		for (k = 0; k < 3; k++) {
			b[i].c[k] = hd[k] + q * (shape[i].aft * aft[k] - shape[i].down * back[k] - shape[i].fore * ax[k]);
			b[i].up[k] = back[k];
		}
		b[i].ru = shape[i].ru * q;
		b[i].rs = shape[i].rs * q;
		b[i].rgb = shape[i].rgb ? shape[i].rgb : helmet;
	}
	for (i = 2; i < n; i++)
		for (j = i; j > 1 && dot (b[j].c, b[j].c) > dot (b[j - 1].c, b[j - 1].c); j--) {
			t = b[j];
			b[j] = b[j - 1];
			b[j - 1] = t;
		}
	for (i = 0; i < n; i++) blob (&b[i]);
}

/* the cap, a squashed half sphere: faint at the centre, a Fresnel rim, the
 * sky in it, and a soft highlight where its normal meets the half vector,
 * kept up on the crown. Premultiplied; front faces only, it is convex */
static void glass (float h, float alpha)
{
	static const int tri[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };
	static V3 p[RINGS + 1][SEGS + 1], m[RINGS + 1][SEGS + 1];
	static float col[RINGS + 1][SEGS + 1][4];
	V3 hv = { eye[0] + li[0], eye[1] + li[1], eye[2] + li[2] }, sp;
	int i, j, k, q;

	norm (hv);
	sp[0] = dot (hv, ax) * ra;
	sp[1] = dot (hv, bx) * rb;
	sp[2] = dot (hv, nx) * h;
	norm (sp);
	if (sp[2] < 0.85f) {
		float f = sqrtf (0.2775f / fmaxf (sp[0] * sp[0] + sp[1] * sp[1], 1e-6f));
		sp[0] *= f;
		sp[1] *= f;
		sp[2] = 0.85f;
	}
	for (i = 0; i <= RINGS; i++) {
		float t = i * TURN / (4 * RINGS), rh = cosf (t), z = sinf (t);
		for (j = 0; j <= SEGS; j++) {
			float u = rh * cs[j][0], w = rh * cs[j][1], c, e, f, sky, a, s;
			V3 v, r;

			at (p[i][j], u, w, h * z);
			for (k = 0; k < 3; k++) {
				m[i][j][k] = u / ra * ax[k] + w / rb * bx[k] + z / h * nx[k];
				v[k] = -p[i][j][k];
			}
			norm (m[i][j]);
			norm (v);
			c = dot (m[i][j], v);
			e = smooth (-0.02f, 0.16f, c);	/* the very edge fades, or it shows as dashes */
			c = fmaxf (c, 0);
			for (k = 0; k < 3; k++) r[k] = 2 * c * m[i][j][k] - v[k];
			sky = smooth (-0.2f, 1.0f, dot (r, nx));
			f = 1 - c;
			f = 0.03f + 0.97f * f * f * f * f * f;
			a = (0.06f + 0.60f * f * e) * alpha;
			s = 0.32f * alpha * expf (-(u - sp[0]) * (u - sp[0]) / 0.05f - (w - sp[1]) * (w - sp[1]) / 0.012f
						  - (z - sp[2]) * (z - sp[2]) / 0.02f);
			col[i][j][0] = (0.30f + 0.45f * sky) * a + s;
			col[i][j][1] = (0.42f + 0.45f * sky) * a + s;
			col[i][j][2] = (0.52f + 0.48f * sky) * a + s;
			col[i][j][3] = a;
		}
	}
	glBegin (GL_TRIANGLES);
	for (i = 0; i < RINGS; i++)
		for (j = 0; j < SEGS; j++) {
			V3 v = { -p[i + 1][j][0], -p[i + 1][j][1], -p[i + 1][j][2] };

			if (dot (v, m[i + 1][j + 1]) <= 0 && dot (v, m[i][j]) <= 0) continue;
			for (q = 0; q < 6; q++) {
				glColor4fv (col[i + tri[q][0]][j + tri[q][1]]);
				glVertex3fv (p[i + tri[q][0]][j + tri[q][1]]);
			}
		}
	glEnd ();
}

void cockpit_draw (const short lt[3], const double proj[16])
{
	const float *hd = NULL, *helmet = NULL;
	float z, nv, h, best = 1e30f, reach;
	int i, k, who = -1;

	if (arcs < 1) return;
	arcs = 0;
	for (k = 0; k < 3; k++) {
		ce[k] = (arc[0][k] + arc[3][k]) / 2;
		ax[k] = (arc[3][k] - arc[0][k]) / 2;
		bx[k] = 0.75f * (arc[1][k] - arc[0][k]);
		li[k] = lt[k];
		eye[k] = -ce[k];
	}
	ra = norm (ax);
	rb = norm (bx);
	cross (nx, bx, ax);
	norm (li);
	z = norm (eye);
	if (!(ra > 0) || !(rb > 0) || !(z > 0)) return;
	nv = dot (nx, eye);
	/* the size of it decides: below PX_LO the game's oval and pilot stay */
	fade = smooth (PX_LO, PX_HI, (float) (ra * proj[5] / 2 / -ce[2]));
	/* the game's pilot of this oval, if it drew one */
	reach = (1.5f * ra) * (1.5f * ra);
	for (i = 0; i < pilots; i++)
		if (dist2 (pilot[i].e, ce) < best) {
			best = dist2 (pilot[i].e, ce);
			who = i;
		}
	if (best < reach) {
		hd = pilot[who].e;
		helmet = pilot[who].rgb;
		pilot[who].took = fade >= 1 && fabsf (nv) > EDGE_ON ? reach : 0;
	}
	if (fade <= 0) return;
	if (cs[1][0] == 0)
		for (i = 0; i <= SEGS; i++) {
			cs[i][0] = cosf (i * TURN / SEGS);
			cs[i][1] = sinf (i * TURN / SEGS);
		}
	h = smooth (FLAT_LO, FLAT_HI, nv);
	z = WELL * rb / fmaxf (nv, 0.2f);	/* along the plane, towards the eye */
	floor_at[0] = dot (ax, eye) * z / ra;
	floor_at[1] = dot (bx, eye) * z / rb;
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_LIGHTING_BIT);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	cabin ();
	if (fabsf (nv) > EDGE_ON) figure (hd, helmet);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	if (h > 0) glass (h * HEIGHT * rb, h * fade);
	glPopAttrib ();
}
