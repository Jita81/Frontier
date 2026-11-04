/*
 * puffs: the game's own low clouds (model 198) lit as cumulus, see
 * docs/hd/clouds.md, "Low clouds".
 *
 * Far away a low cloud is a few outlines of Bezier curves (complex
 * shapes), close by a few quads and triangles.  The game's fill draws
 * every one of them as it always did (the same tessellation, the same
 * vertices, so the same pixels covered); a fragment program bound around
 * it shades inside that coverage.  The put side (gl.c) tags the shape or
 * face with NU_PUFF; nothing else is added to the stream.  A new file
 * because it is one shader with its own bind, as skies.c is for the rings.
 * Off (puff_on): the old renderer, --flat-clouds, --classic-ui, --pixel-*.
 */
#include <math.h>
#include <string.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "main.h"
#include "screen.h"
#include "procfx.h"
#include "skies.h"

int puff_on (void)
{
	return use_renderer == R_GL && !flat_clouds && !classic_ui && !pixel_art && !pixel_text && !pixel_faces;
}

/*
 * A shape is a lobe (upright) or a sheet (flat in the model's y).  q is
 * the pixel's place in the shape's bound on screen, (0,0) at its middle
 * and 1 at its edges (uC).  The lobes are not flat (their points stray
 * from any one plane by up to a quarter of their size), so a lobe is lit
 * as the front half of an ellipsoid filling that bound and facing the
 * eye, normal (q, sqrt (1 - |q|^2)) in the frame of the ray to it: uL is
 * the sun in that frame, uK.z the mean of the light over the bound, so
 * the lit side gains what the shaded side loses; the top (uU, the model's
 * up on screen) a little brighter.  A sheet is a little darker towards
 * its middle.  uR is the outline's radius in 32 directions: the edge
 * softens over its last pixels (uL.w 0 for a shape too small for it).
 * A face (uK.x 0) keeps its colour and takes only the mottle, from its
 * place in the model's space (gl_TexCoord[1]).  uK.y the seed, uK.w the
 * day (the sun above the cloud's horizon): by night only darker.
 */
#define RIM_BINS 32
#define WRAP 0.6		/* wrap lighting: the shaded side is not black */
#define FACE_R 20.0		/* a lobe's radius in the model's space */
static const char puff_glsl[] =
	"uniform vec4 uC, uK, uL;\n"
	"uniform vec2 uU;\n"
	"uniform float uR[32];\n"
	"#define K_SUN 0.30\n"	/* the sun's swing about the mean */
	"#define K_TOP 0.12\n"	/* tops over undersides */
	"#define K_MID 0.12\n"	/* a sheet's middle */
	"#define K_MOT 0.16\n"	/* the mottle, +-8% */
	"#define RIM_PX 3.0\n"	/* the soft edge, pixels */
	"#define WRAP 0.6\n"
	"void main () {\n"
	"	float f = 1.0, a = 1.0, day = uK.w;\n"
	"	vec3 p = gl_TexCoord[1].xyz;\n"
	"	if (uK.x > 0.5) {\n"
	"		vec2 q = (gl_FragCoord.xy - uC.xy) * uC.zw;\n"
	"		float r = length (q), r2 = min (r * r, 1.0);\n"
	"		if (uK.x < 1.5) {\n"
	"			float lam = clamp ((dot (vec3 (q, sqrt (1.0 - r2)), uL.xyz) + WRAP) / (1.0 + WRAP), 0.0, 1.0);\n"
	"			f += day * (K_SUN * (lam - uK.z) + K_TOP * dot (q, uU));\n"
	"		} else\n"
	"			f -= K_MID * (0.5 - r2);\n"
	"		float t = atan (q.y, q.x) * (16.0 / 3.14159265) + 15.5, i = floor (t);\n"
	"		float R = max (uR[int (mod (i, 32.0))], uR[int (mod (i + 1.0, 32.0))]);\n"
	"		a = mix (1.0, mix (0.15, 1.0, smoothstep (0.0, RIM_PX, (R - r) / max (fwidth (r), 1e-5))), uL.w);\n"
	"		p = vec3 (q * 2.5, 0.0);\n"
	"	}\n"
	"	f *= 1.0 + K_MOT * (pf_fbm (p + uK.y, 2) - 0.5);\n"
	"	gl_FragColor = vec4 (gl_Color.rgb * clamp (f, 0.6, 1.0 + 0.3 * day), a);\n"
	"}\n";

static GLuint prog;
static GLint u_c, u_k, u_l, u_u, u_r;
static int failed, bound, pending;
static struct puff_tag cur;
static float corner[4][3];

/* once for a run of a record's shapes and faces, not for each one */
static int puff_bind (void)
{
	if (bound) return 1;
	if (failed) return 0;
	if (!prog) {
		prog = procfx_program ("puffs", puff_glsl);
		if (!prog) { failed = 1; return 0; }
		u_c = glGetUniformLocation (prog, "uC");
		u_u = glGetUniformLocation (prog, "uU");
		u_k = glGetUniformLocation (prog, "uK");
		u_l = glGetUniformLocation (prog, "uL");
		u_r = glGetUniformLocation (prog, "uR");
	}
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram (prog);
	bound = 1;
	return 1;
}

void puff_end (void)
{
	if (!bound) return;
	glUseProgram (0);
	glPopAttrib ();
	bound = 0;
}

void puff_draw_tag (void **data)
{
	memcpy (&cur, *data, sizeof cur);
	*data += sizeof cur;
	pending = 1;
}

/* Before every primitive but the ones inside a shape: 1 when the tag just
 * read is for it (kind 0 a complex shape, 1 a face); the shader goes with
 * any other one */
int puff_next (int kind)
{
	int use = pending && cur.kind == kind;

	pending = 0;
	if (!use) puff_end ();
	return use;
}

/* a GL eye point in the model's space */
static void puff_obj (const double e[3], double w[3])
{
	double v[3] = { e[0] - cur.o[0], e[1] - cur.o[1], -e[2] - cur.o[2] };
	int j;

	for (j = 0; j < 3; j++) w[j] = cur.M[3*j] * v[0] + cur.M[3*j+1] * v[1] + cur.M[3*j+2] * v[2];
}

/* the model's axis j in GL eye space */
static void puff_axis (int j, double a[3])
{
	double l = sqrt (cur.M[3*j] * cur.M[3*j] + cur.M[3*j+1] * cur.M[3*j+1] + cur.M[3*j+2] * cur.M[3*j+2]);

	a[0] = cur.M[3*j] / l; a[1] = cur.M[3*j+1] / l; a[2] = -cur.M[3*j+2] / l;
}

static double dot3 (const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross3 (const double a[3], const double b[3], double c[3])
{
	c[0] = a[1] * b[2] - a[2] * b[1]; c[1] = a[2] * b[0] - a[0] * b[2]; c[2] = a[0] * b[1] - a[1] * b[0];
}

/* the day: the sun above the cloud's horizon; the seed: its place, so a
 * cloud's mottle stays with it and two clouds differ */
static float puff_day (void)
{
	double y[3], l[3] = { cur.L[0], cur.L[1], cur.L[2] }, s;

	puff_axis (1, y);
	s = (dot3 (l, y) + 0.1) / 0.3;
	s = s < 0 ? 0 : s > 1 ? 1 : s;
	return (float) (s * s * (3 - 2 * s));
}

static float puff_seed (const double w[3])
{
	unsigned int h = cur.seed * 0x9e3779b1u;
	int j;

	for (j = 0; j < 3; j++) h = (h ^ (unsigned int) (int) floor (w[j] / 4.0)) * 0x85ebca6bu;
	return (float) ((h >> 16) % 1021) * 0.37f;
}

/*
 * The lobes of a cloud are faces of one solid: two that meet share their
 * edge's points exactly.  Where they lie on either side of it on screen
 * the edge is inside the cloud, and a soft edge there shows the sky
 * through as a light seam; so an edge softens only where, last frame,
 * no two shapes lay on either side of either of its ends (nx ny: the
 * shape's inward normal there, of any length).  A point not seen last
 * frame is hard: a cloud coming into view has hard edges for one frame.
 * The table's clock is frames drawn (puff_frame), never time.
 */
#define PT_BITS 15
static struct { unsigned int key, id, at, fl; float nx, ny; } pt[1 << PT_BITS];	/* fl: 1 inside this frame, 2 last frame, 4 seen last frame */
static unsigned int frame = 2, shapes;

void puff_frame (void)
{
	puff_end ();
	frame++;
}

/* the GL context is about to go: compile again in the next one */
void puff_forget (void)
{
	if (prog) glDeleteProgram (prog);
	prog = 0;
	failed = 0;
}

static int puff_shared (const double w[3], float nx, float ny)
{
	unsigned int h = cur.seed * 0x9e3779b1u, i, k, m = (1 << PT_BITS) - 1;
	int j;

	for (j = 0; j < 3; j++) h = (h ^ (unsigned int) (int) floor (w[j] * 2 + 0.5)) * 0x85ebca6bu;
	h |= 1;
	for (k = 0; k < 8 && pt[(h + k) & m].key != h; k++);
	if (k == 8)
		for (k = 0; k < 8 && pt[(h + k) & m].at + 1 >= frame; k++);
	if (k == 8) return 1;
	i = (h + k) & m;
	if (pt[i].key != h) pt[i].key = h, pt[i].at = 0;
	if (pt[i].at != frame)
		pt[i].fl = pt[i].at + 1 == frame ? 4 | (pt[i].fl & 1) << 1 : 0, pt[i].nx = nx, pt[i].ny = ny, pt[i].id = shapes, pt[i].at = frame;
	else if (pt[i].id != shapes && nx * pt[i].nx + ny * pt[i].ny < 0)
		pt[i].fl |= 1;
	return pt[i].fl != 4;
}

/* the direction of (x, y) to about 0.005 rad, a fortieth of a rim bin:
 * a shape takes a couple of hundred of these */
static double puff_atan2 (double y, double x)
{
	double ax = fabs (x), ay = fabs (y), hi = fmax (ax, ay), a, s, r;

	if (hi == 0) return 0;
	a = fmin (ax, ay) / hi;
	s = a * a;
	r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
	if (ay > ax) r = M_PI / 2 - r;
	if (x < 0) r = M_PI - r;
	return y < 0 ? -r : r;
}

/*
 * One shape, just before the game's tessellator draws it: its n points e
 * in GL eye space, mv pj vp what gluProject takes them through.  0 when
 * the game's flat fill is to be used: a shape at or behind the near
 * plane, one bigger than 4 screens or too small to see.
 */
#define MAX_PTS 1024
int puff_shape (const double (*e)[3], int n, const double mv[16], const double pj[16], const int vp[4])
{
	static double win[MAX_PTS][2];
	double C[3] = { 0, 0, 0 }, X[3], Y[3], Z[3], M[3], w[3], lo[3], hi[3], P[16];
	double c[2], s[2], x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30, l, d;
	float R[RIM_BINS], H[RIM_BINS];
	static char hard[MAX_PTS];
	int i, j, k, sheet;

	if (n < 3 || n > MAX_PTS) return 0;
	for (i = 0; i < 16; i++)
		for (P[i] = 0, k = 0; k < 4; k++) P[i] += pj[i % 4 + 4 * k] * mv[k + 4 * (i / 4)];
	for (i = 0; i < n; i++) {
		const double *p = e[i];
		double cw = P[3] * p[0] + P[7] * p[1] + P[11] * p[2] + P[15];
		if (p[2] > -1.0 || cw <= 0) return 0;
		win[i][0] = vp[0] + vp[2] * 0.5 * (1 + (P[0] * p[0] + P[4] * p[1] + P[8] * p[2] + P[12]) / cw);
		win[i][1] = vp[1] + vp[3] * 0.5 * (1 + (P[1] * p[0] + P[5] * p[1] + P[9] * p[2] + P[13]) / cw);
		x0 = fmin (x0, win[i][0]); x1 = fmax (x1, win[i][0]);
		y0 = fmin (y0, win[i][1]); y1 = fmax (y1, win[i][1]);
		puff_obj (p, w);
		for (j = 0; j < 3; j++) {
			lo[j] = i ? fmin (lo[j], w[j]) : w[j];
			hi[j] = i ? fmax (hi[j], w[j]) : w[j];
			C[j] += p[j] / n;
		}
	}
	if ((x1 - x0) * (y1 - y0) > 4.0 * vp[2] * vp[3] || x1 - x0 < 1 || y1 - y0 < 1) return 0;
	shapes++;
	for (l = 0, i = 0; i < n; i++) l += win[i][0] * win[(i+1)%n][1] - win[(i+1)%n][0] * win[i][1];
	for (i = 0; i < n; i++) {
		double tx = win[(i+1)%n][0] - win[(i+n-1)%n][0], ty = win[(i+1)%n][1] - win[(i+n-1)%n][1];
		puff_obj (e[i], w);
		hard[i] = puff_shared (w, (float) (l > 0 ? -ty : ty), (float) (l > 0 ? tx : -tx));
	}
	sheet = hi[1] - lo[1] < 0.02 * fmax (hi[0] - lo[0], hi[2] - lo[2]);
	c[0] = (x0 + x1) / 2; s[0] = (x1 - x0) / 2;
	c[1] = (y0 + y1) / 2; s[1] = (y1 - y0) / 2;

	/* the outline's radius in q in each direction, from its edges; a
	 * direction whose outline is a shared edge stays hard (R far out) */
	for (k = 0; k < RIM_BINS; k++) R[k] = H[k] = 0;
	for (i = 0; i < n; i++)
		for (k = 0; k < 4; k++) {
			double qx = (win[i][0] + (win[(i+1)%n][0] - win[i][0]) * k / 4.0 - c[0]) / s[0];
			double qy = (win[i][1] + (win[(i+1)%n][1] - win[i][1]) * k / 4.0 - c[1]) / s[1];
			j = (int) floor ((puff_atan2 (qy, qx) / M_PI + 1) * RIM_BINS / 2) % RIM_BINS;
			R[j] = fmax (R[j], sqrt (qx * qx + qy * qy));
			if (hard[i] && hard[(i+1)%n]) H[j] = fmax (H[j], sqrt (qx * qx + qy * qy));
		}
	for (k = 0; k < RIM_BINS; k++)
		if (H[k] > 0.9 * R[k]) R[k] = 1e3;
	for (k = 0; k < 2 * RIM_BINS; k++)
		if (R[k % RIM_BINS] == 0) R[k % RIM_BINS] = R[(k + RIM_BINS - 1) % RIM_BINS];

	/* the frame of the ray to the shape: Z towards the eye, X across, Y up */
	l = sqrt (dot3 (C, C));
	for (j = 0; j < 3; j++) Z[j] = -C[j] / l;
	{ double up[3] = { 0, 1, 0 }; cross3 (up, Z, X); }
	l = sqrt (dot3 (X, X));
	for (j = 0; j < 3; j++) X[j] /= l;
	cross3 (Z, X, Y);
	puff_axis (1, M);
	if (!puff_bind ()) return 0;
	{
		double L[3] = { cur.L[0], cur.L[1], cur.L[2] }, lz = dot3 (L, Z), lc;
		double ux = dot3 (M, X), uy = dot3 (M, Y), ul = sqrt (ux * ux + uy * uy);
		lc = (0.667 * lz + WRAP) / (1 + WRAP);
		for (j = 0; j < 3; j++) w[j] = (lo[j] + hi[j]) / 2;
		glUniform4f (u_c, (float) c[0], (float) c[1], (float) (1 / s[0]), (float) (1 / s[1]));
		glUniform4f (u_k, sheet ? 2 : 1, puff_seed (w), (float) fmin (fmax (lc, 0), 1), puff_day ());
		d = fmin (fmax ((fmin (s[0], s[1]) - 4) / 8, 0), 1);	/* the rim: none under 8 px across */
		glUniform4f (u_l, (float) dot3 (L, X), (float) dot3 (L, Y), (float) lz, (float) d);
		glUniform2f (u_u, ul > 0 ? (float) (ux / ul) : 0, ul > 0 ? (float) (uy / ul) : 1);
		glUniform1fv (u_r, RIM_BINS, R);
	}
	return 1;
}

/* A face (its n corners p in GL eye space): 0 when flat; else
 * puff_corner (i) before each corner's glVertex */
int puff_face (const double (*p)[3], int n)
{
	static const double zero[3] = { 0, 0, 0 };
	int i, k;

	for (i = 0; i < n; i++) {
		double w[3];
		puff_obj (p[i], w);
		for (k = 0; k < 3; k++) corner[i][k] = (float) (w[k] * (2.5 / FACE_R));
	}
	if (!puff_bind ()) return 0;
	glUniform4f (u_k, 0, puff_seed (zero), 0, puff_day ());
	return 1;
}

void puff_corner (int i)
{
	glMultiTexCoord3f (GL_TEXTURE1, corner[i][0], corner[i][1], corner[i][2]);
}
