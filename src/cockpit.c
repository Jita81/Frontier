/*
 * The green ship's cockpit (cockpit.h; docs/hd/hulls.md, "The cockpit").
 * The oval's two curves are half ellipses, so the first gives its centre
 * and axes, per instance and in eye space. Everything here is convex and
 * made in code; nothing is kept between frames but the pilots' positions.
 */
#include <math.h>
#include <string.h>
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "cockpit.h"

#define MAX_PILOTS	8
#define SEGS		36
#define HEIGHT		0.62f	/* the cap's height against the minor radius */
#define HEAD		0.055f	/* the pilot's head radius, the same */
#define PX_LO		0.012f	/* major radius against the view's height: nothing below */
#define PX_HI		0.028f	/* all of it above */
#define FLAT_LO		0.01f	/* n.v at which the cap lies flat on the oval */
#define FLAT_HI		0.09f	/* and has its full height */

typedef float V3[3];

static struct { V3 e; float rgb[3]; int took; } pilot[MAX_PILOTS];
static int pilots, arcs;
static V3 arc[4];
/* this cockpit: centre, unit axes (a major, b minor, n out of the hull),
 * radii, unit light, unit forward along a */
static V3 ce, ax, bx, nx, li;
static float ra, rb, fade;

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
	if (pilots == MAX_PILOTS) return;
	pilot[pilots].e[0] = e[0];
	pilot[pilots].e[1] = e[1];
	pilot[pilots].e[2] = e[2];
	pilot[pilots].rgb[0] = ((rgb444 >> 8) & 15) / 15.0f;
	pilot[pilots].rgb[1] = ((rgb444 >> 4) & 15) / 15.0f;
	pilot[pilots].rgb[2] = (rgb444 & 15) / 15.0f;
	pilot[pilots++].took = 0;
}

void cockpit_arc (float cp[4][3])
{
	int k;
	if (arcs++ == 0) for (k = 0; k < 4; k++) memcpy (arc[k], cp[k], sizeof arc[k]);
}

int cockpit_replaced (float x, float y, float z)
{
	const float v[3] = { x, y, z };
	float best = 1e30f;
	int i, took = 0;

	for (i = 0; i < pilots; i++) {
		float d = 0;
		int k;
		for (k = 0; k < 3; k++) d += (v[k] - pilot[i].e[k]) * (v[k] - pilot[i].e[k]);
		if (d < best) {
			best = d;
			took = pilot[i].took;
		}
	}
	return took;
}

/* the cabin: dark, blacker to the rim, a faint glow of instruments forward */
static void cabin_colour (float u, float v)
{
	float r2 = u * u + v * v, dim = 1 - r2, g;

	dim = dim < 0 ? 0 : sqrtf (dim);
	g = (u + 0.70f) * (u + 0.70f) / 0.035f + v * v / 0.30f;
	g = exp (-g) * dim;
	glColor4f (0.040f * dim + 0.10f * g, 0.050f * dim + 0.24f * g, 0.066f * dim + 0.25f * g, fade);
}

static void cabin (void)
{
	static const float rho[] = { 0.0f, 0.3f, 0.55f, 0.75f, 0.9f, 1.0f };
	int i, j;

	for (i = 0; i < 5; i++) {
		glBegin (GL_TRIANGLE_STRIP);
		for (j = 0; j <= SEGS; j++) {
			float c = cosf (j * 2 * (float) M_PI / SEGS), s = sinf (j * 2 * (float) M_PI / SEGS);
			V3 p;
			cabin_colour (rho[i + 1] * c, rho[i + 1] * s);
			at (p, rho[i + 1] * c, rho[i + 1] * s, 0);
			glVertex3fv (p);
			cabin_colour (rho[i] * c, rho[i] * s);
			at (p, rho[i] * c, rho[i] * s, 0);
			glVertex3fv (p);
		}
		glEnd ();
	}
}

/* a small lit part: an ellipse facing the eye, ru along up and rs across,
 * shaded as the ball it stands for */
static void blob_colour (const V3 c, const V3 s, const V3 u, float x, float y, const float *rgb)
{
	float z2 = 1 - x * x - y * y, z = z2 > 0 ? sqrtf (z2) : 0, d;
	V3 m, v = { -c[0], -c[1], -c[2] };
	int k;

	norm (v);
	for (k = 0; k < 3; k++) m[k] = x * s[k] + y * u[k] + z * v[k];
	d = dot (m, li);
	d = 0.42f + 0.75f * (d > 0 ? d : 0);
	glColor4f (rgb[0] * d, rgb[1] * d, rgb[2] * d, fade);
}

static void blob (const V3 c, const V3 up, float ru, float rs, const float *rgb)
{
	enum { N = 14 };
	V3 v = { -c[0], -c[1], -c[2] }, s, u, p;
	float q, ry;
	int j, k;

	norm (v);
	cross (s, up, v);
	q = norm (s);	/* the sine of the angle from the eye to up */
	if (q < 0.01f) return;
	cross (u, v, s);
	ry = sqrtf (ru * ru * q * q + rs * rs * (1 - q * q));
	glBegin (GL_TRIANGLE_FAN);
	blob_colour (c, s, u, 0, 0, rgb);
	glVertex3fv (c);
	for (j = 0; j <= N; j++) {
		float a = j * 2 * (float) M_PI / N, x = 0.999f * cosf (a), y = 0.999f * sinf (a);
		blob_colour (c, s, u, x, y, rgb);
		for (k = 0; k < 3; k++) p[k] = c[k] + x * rs * s[k] + y * ry * u[k];
		glVertex3fv (p);
	}
	glEnd ();
}

/* the figure, about the game's head: seat, suit, helmet, visor, far to near */
static void figure (const float *hd, const float *helmet)
{
	static const float seat[3] = { 0.20f, 0.21f, 0.24f }, suit[3] = { 0.62f, 0.60f, 0.30f }, visor[3] = { 0.06f, 0.10f, 0.14f };
	const float q = HEAD * rb;
	V3 up, fw, pp[3], vp;
	int k, front;

	/* the spine leans back; forward is towards -a (the eye before the ship) */
	for (k = 0; k < 3; k++) {
		fw[k] = -ax[k];
		up[k] = nx[k] - 0.3f * fw[k];
	}
	norm (up);
	for (k = 0; k < 3; k++) {
		pp[0][k] = hd[k] - 2.1f * q * up[k] - 1.1f * q * fw[k];	/* seat */
		pp[1][k] = hd[k] - 2.3f * q * up[k];			/* suit */
		pp[2][k] = hd[k];					/* helmet */
		vp[k] = hd[k] + 0.5f * q * fw[k] + 0.1f * q * up[k];	/* visor */
	}
	front = -(fw[0] * hd[0] + fw[1] * hd[1] + fw[2] * hd[2]) > 0;	/* the eye before him */
	if (front) {
		blob (pp[0], up, 2.8f * q, 1.5f * q, seat);
		blob (pp[1], up, 1.8f * q, 1.25f * q, suit);
		blob (pp[2], up, q, q, helmet);
		blob (vp, up, 0.55f * q, 0.72f * q, visor);
	} else {
		blob (vp, up, 0.55f * q, 0.72f * q, visor);
		blob (pp[2], up, q, q, helmet);
		blob (pp[1], up, 1.8f * q, 1.25f * q, suit);
		blob (pp[0], up, 2.8f * q, 1.5f * q, seat);
	}
}

/* the glass at one point of the cap: faint at the centre, a Fresnel rim,
 * the sky in it and a soft highlight, premultiplied */
static void glass_colour (const V3 p, const V3 m)
{
	V3 v = { -p[0], -p[1], -p[2] }, h, r;
	float c, f, sky, sp, a;
	int k;

	norm (v);
	c = dot (m, v);
	c = c < 0 ? 0 : c;
	f = 1 - c;
	f = 0.03f + 0.97f * f * f * f * f;
	for (k = 0; k < 3; k++) {
		r[k] = 2 * c * m[k] - v[k];
		h[k] = v[k] + li[k];
	}
	norm (h);
	sky = smooth (-0.2f, 1.0f, dot (r, nx));
	sp = dot (h, m);
	sp = sp > 0 ? powf (sp, 60.0f) : 0;
	a = (0.07f + 0.62f * f) * fade;
	glColor4f (((0.30f + 0.45f * sky) * a + 0.9f * sp * fade),
		   ((0.42f + 0.45f * sky) * a + 0.9f * sp * fade),
		   ((0.52f + 0.48f * sky) * a + 0.9f * sp * fade), a);
}

static void glass (float h)
{
	enum { RINGS = 8 };
	float hm = h > 0.05f * rb ? h : 0.05f * rb;
	int i, j, k;

	for (i = 0; i < RINGS; i++) {
		float t0 = i * (float) M_PI / (2 * RINGS), t1 = (i + 1) * (float) M_PI / (2 * RINGS);
		glBegin (GL_TRIANGLES);
		for (j = 0; j < SEGS; j++) {
			static const int tri[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };
			float a0 = j * 2 * (float) M_PI / SEGS, a1 = (j + 1) * 2 * (float) M_PI / SEGS;
			V3 p[6], m[6], v, fc;
			int q;

			for (q = 0; q < 6; q++) {
				float t = tri[q][0] ? t1 : t0, a = tri[q][1] ? a1 : a0;
				float rh = cosf (t), z = sinf (t), c = cosf (a), s = sinf (a);
				at (p[q], rh * c, rh * s, h * z);
				for (k = 0; k < 3; k++)
					m[q][k] = rh * c / ra * ax[k] + rh * s / rb * bx[k] + z / hm * nx[k];
				norm (m[q]);
			}
			/* front faces only: the cap is convex */
			for (k = 0; k < 3; k++) {
				fc[k] = (p[0][k] + p[2][k]) / 2;
				v[k] = -fc[k];
			}
			if (dot (v, m[2]) <= 0 && dot (v, m[0]) <= 0) continue;
			for (q = 0; q < 6; q++) {
				glass_colour (p[q], m[q]);
				glVertex3fv (p[q]);
			}
		}
		glEnd ();
	}
}

void cockpit_draw (const short lt[3], const double proj[16])
{
	V3 v, A, B;
	float z, nv, h;
	const float *hd = NULL, *helmet = NULL;
	float best;
	int i, k;

	if (arcs < 1) return;
	arcs = 0;
	for (k = 0; k < 3; k++) {
		ce[k] = (arc[0][k] + arc[3][k]) / 2;
		A[k] = (arc[3][k] - arc[0][k]) / 2;
		B[k] = 0.75f * (arc[1][k] - arc[0][k]);
		ax[k] = A[k];
		bx[k] = B[k];
		li[k] = lt[k];
		v[k] = -ce[k];
	}
	ra = norm (ax);
	rb = norm (bx);
	cross (nx, bx, ax);
	norm (li);
	z = norm (v);
	if (!(ra > 0) || !(rb > 0) || !(z > 0)) return;
	/* the size of it decides: below PX_LO the game's oval and pilot stay */
	fade = smooth (PX_LO, PX_HI, (float) (ra * proj[5] / 2 / -ce[2]));
	/* the game's pilot of this oval, if it drew one */
	best = (1.5f * ra) * (1.5f * ra);
	for (i = 0; i < pilots; i++) {
		float d = 0;
		for (k = 0; k < 3; k++) d += (pilot[i].e[k] - ce[k]) * (pilot[i].e[k] - ce[k]);
		if (d < best) {
			best = d;
			hd = pilot[i].e;
			helmet = pilot[i].rgb;
			pilot[i].took = 0;
		}
	}
	if (fade <= 0) return;
	for (i = 0; i < pilots; i++)
		if (hd == pilot[i].e) pilot[i].took = fade >= 1;
	nv = dot (nx, v);
	h = HEIGHT * rb * smooth (FLAT_LO, FLAT_HI, nv);
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_LIGHTING_BIT);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_LIGHTING);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	cabin ();
	if (hd) figure (hd, helmet);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glass (h);
	glPopAttrib ();
}
