/*
 * procfx: see procfx.h and docs/hd/procedural.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "main.h"
#include "../m68000.h"
#include "procfx.h"

/* ---- shared building blocks ------------------------------------------
 *
 * One noise library for every layer, written once in GLSL 1.20 (the level
 * macOS gives a legacy context).  A layer's shader is this text plus its
 * own main().  Value noise on a 3D lattice, so a pattern laid on a sphere
 * of directions (the sky, a planet, a star) has no seams and no poles.
 * pf_hash is the well known "hash without sine": the same on every GPU. */
static const char noise_glsl[] =
	"#version 120\n"
	"float pf_hash (vec3 p) {\n"
	"	p = fract (p * 0.1031);\n"
	"	p += dot (p, p.zyx + 31.32);\n"
	"	return fract ((p.x + p.y) * p.z);\n"
	"}\n"
	"float pf_noise (vec3 x) {\n"
	"	vec3 i = floor (x), f = fract (x);\n"
	"	f = f * f * (3.0 - 2.0 * f);\n"
	"	return mix (mix (mix (pf_hash (i), pf_hash (i + vec3 (1,0,0)), f.x),\n"
	"	                 mix (pf_hash (i + vec3 (0,1,0)), pf_hash (i + vec3 (1,1,0)), f.x), f.y),\n"
	"	            mix (mix (pf_hash (i + vec3 (0,0,1)), pf_hash (i + vec3 (1,0,1)), f.x),\n"
	"	                 mix (pf_hash (i + vec3 (0,1,1)), pf_hash (i + vec3 (1,1,1)), f.x), f.y), f.z);\n"
	"}\n"
	/* n octaves, each half the size and half the weight: 0..1 */
	"float pf_fbm (vec3 p, int n) {\n"
	"	float s = 0.0, a = 0.5, w = 0.0;\n"
	"	for (int k = 0; k < 6; k++) {\n"
	"		if (k >= n) break;\n"
	"		s += a * pf_noise (p); w += a;\n"
	"		p = p * 2.03 + vec3 (1.7, 9.2, 3.1); a *= 0.5;\n"
	"	}\n"
	"	return s / w;\n"
	"}\n"
	/* the eye ray through this pixel, from the viewport and projection */
	"uniform vec4 uVp;\n"
	"uniform vec2 uFoc;\n"
	"vec3 pf_ray () {\n"
	"	vec2 ndc = (gl_FragCoord.xy - uVp.xy) / uVp.zw * 2.0 - 1.0;\n"
	"	return normalize (vec3 (ndc.x / uFoc.x, ndc.y / uFoc.y, -1.0));\n"
	"}\n";

/* Compile one layer's fragment shader (the noise library plus body) into
 * a program.  No vertex shader: the fixed-function vertex path stays, so
 * the game's own geometry and colours come through unchanged as gl_Color.
 * 0 when GLSL is missing or the source does not compile; the caller then
 * draws the old way. */
static GLuint procfx_program (const char *name, const char *body)
{
	const char *src[2] = { noise_glsl, body };
	GLuint fs, prog;
	GLint ok = 0;
	char log[1024];

	fs = glCreateShader (GL_FRAGMENT_SHADER);
	glShaderSource (fs, 2, src, NULL);
	glCompileShader (fs);
	glGetShaderiv (fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog (fs, sizeof log, NULL, log);
		fprintf (stderr, "procfx: %s shader does not compile, left flat: %s\n", name, log);
		glDeleteShader (fs);
		return 0;
	}
	prog = glCreateProgram ();
	glAttachShader (prog, fs);
	glLinkProgram (prog);
	glDeleteShader (fs);
	glGetProgramiv (prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog (prog, sizeof log, NULL, log);
		fprintf (stderr, "procfx: %s shader does not link, left flat: %s\n", name, log);
		glDeleteProgram (prog);
		return 0;
	}
	return prog;
}

/* read only, inside the game's memory */
static inline int rd_ok (unsigned int addr, unsigned int len)
{
	return addr <= 0x110000u && len <= 0x110000u - addr;
}

/* the object record of the object being drawn, its model word and seed,
 * and its rotation: nine 1.15 words, out[k] = sum_j w[3j+k] * in[j] (see
 * L3b9aa, which turns a model vector into the view with them) */
#define FRAME_OBJ	212
#define FRAME_ROT	36
#define OBJ_MODEL	90
#define OBJ_SEED	118

/* the model word of the object being drawn, -1 out of reach; *rec its
 * record */
static int pf_object (unsigned int *rec)
{
	unsigned int a6 = (unsigned int) GetReg (REG_A6), r;

	if (a6 < FRAME_OBJ || !rd_ok (a6 - FRAME_OBJ, 4)) return -1;
	r = (unsigned int) STMemory_ReadLong (a6 - FRAME_OBJ);
	if (!rd_ok (r, OBJ_SEED + 4)) return -1;
	if (rec) *rec = r;
	return STMemory_ReadWord (r + OBJ_MODEL) & 0xffff;
}

static int pf_rot (float m[9])
{
	unsigned int a6 = (unsigned int) GetReg (REG_A6);
	int i;

	if (a6 < FRAME_ROT || !rd_ok (a6 - FRAME_ROT, 18)) return 0;
	for (i = 0; i < 9; i++)
		m[i] = (short) STMemory_ReadWord (a6 - FRAME_ROT + 2*i) / 32768.0f;
	return 1;
}

/* m (pf_rot) as the eye-to-object matrix: GL eye (x, y, z) is the game's
 * view (x, y, -z) (znode_wrvertex), and the object's frame is the
 * transpose of its rotation applied to that */
static void pf_frame (GLint loc, const float m[9])
{
	float s[9];
	int j;

	for (j = 0; j < 9; j++) s[j] = j % 3 == 2 ? -m[j] : m[j];
	glUniformMatrix3fv (loc, 1, GL_TRUE, s);
}

int is_star (int model) { return model >= 0x98 && model <= 0xae; }

enum light star_light (int model)
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
const char *body_class (int model)
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

/* ---- layer 1: the sky band ------------------------------------------- */

int flat_sky = 0;

/* The background object: L44200_DrawBGStars sets this model word in its
 * object record before it calls L39426_PutGameData3DObj. */
#define SKY_MODEL	0xb0

/* the sky's frame, kept for the corona's streamers too */
static float sky_rot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

int procfx_sky_put (float m[9])
{
	if (pf_object (NULL) != SKY_MODEL || !pf_rot (m)) return 0;
	memcpy (sky_rot, m, sizeof sky_rot);
	return !flat_sky;
}

/* The eye ray of each pixel comes back from gl_FragCoord through the
 * projection the polygon was drawn with; uSky turns it into the sky's own
 * frame, so the pattern is fixed to the sky.  Star clouds are broad noise,
 * the dust lanes a ridge of finer noise, plus a few faint specks.  Cloud
 * sets coverage, which alpha (1 in the polygon, 0 at the strip's far side)
 * scales: thin cloud fades first and bright cloud reaches further out. */
static const char sky_glsl[] =
	"uniform mat3 uSky;\n"
	"void main () {\n"
	"	vec3 d = normalize (uSky * pf_ray ());\n"
	"	float cloud = pf_fbm (d * 3.5 + vec3 (11.0), 4);\n"
	"	float cover = smoothstep (0.10, 0.40, cloud) * gl_Color.a;\n"
	"	if (cover <= 0.0) discard;\n"
	"	float lane = 1.0 - abs (2.0 * pf_fbm (d * 9.0 + vec3 (3.0), 3) - 1.0);\n"
	"	float lum = 0.8 + 1.8 * smoothstep (0.35, 0.80, cloud);\n"
	"	lum *= 1.0 - 0.4 * smoothstep (0.84, 0.98, lane);\n"
	"	vec3 base = gl_Color.rgb;\n"
	"	vec3 core = mix (base, vec3 (0.66, 0.72, 0.98), 0.5);\n"
	"	vec3 col = mix (base, core, smoothstep (0.55, 0.85, cloud)) * lum;\n"
	"	vec3 g = d * 450.0;\n"
	"	float speck = step (0.996, pf_hash (floor (g))) * smoothstep (0.45, 0.15, length (fract (g) - 0.5));\n"
	"	col += speck * (0.25 + 0.6 * smoothstep (0.4, 0.75, cloud)) * vec3 (0.6, 0.65, 0.75);\n"
	"	gl_FragColor = vec4 (col * cover, 1.0);\n"
	"}\n";

static GLuint sky_prog;
static int sky_failed;
static GLint u_sky, u_vp, u_foc;
static double sky_vp_h;

int procfx_sky_begin (const float m[9], const double proj[16], const int vp[4])
{
	if (flat_sky || sky_failed) return 0;
	if (!sky_prog) {
		sky_prog = procfx_program ("sky band", sky_glsl);
		if (!sky_prog) { sky_failed = 1; return 0; }
		u_sky = glGetUniformLocation (sky_prog, "uSky");
		u_vp = glGetUniformLocation (sky_prog, "uVp");
		u_foc = glGetUniformLocation (sky_prog, "uFoc");
	}
	/* the brighter of band and sky wins, so overlaps never add up */
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	glEnable (GL_BLEND);
	glBlendEquation (GL_MAX);
	glUseProgram (sky_prog);
	pf_frame (u_sky, m);
	glUniform4f (u_vp, (float) vp[0], (float) vp[1], (float) vp[2], (float) vp[3]);
	glUniform2f (u_foc, (float) proj[0], (float) proj[5]);
	sky_vp_h = vp[3];
	return 1;
}

/* Outside each outline, a strip FEATHER of the 3D view's height wide
 * carries alpha from 1 to 0, on the side away from the contour's inside
 * whichever way it winds (a hole keeps its hard edge); fans round corners. */
#define FEATHER	0.06

void procfx_sky_feather (const double (*v)[3], int n)
{
	double area = 0, w, a0, a1, turn, a;
	float c[4];
	int i, j, k, m;

	if (n < 3) return;
	for (i = 0; i < n; i++)
		area += v[i][0] * v[(i+1) % n][1] - v[(i+1) % n][0] * v[i][1];
	w = (area > 0 ? FEATHER : -FEATHER) * sky_vp_h;
	glGetFloatv (GL_CURRENT_COLOR, c);
	glBegin (GL_QUAD_STRIP);
	for (i = 0; i <= n; i++) {
		const double *p, *q = v[i % n], *r;
		/* the nearest vertices either side that differ from q (a contour
		 * may end on its first vertex), then the right-hand normals of the
		 * edges into and out of q, and the turn */
		for (j = i + n - 1; p = v[j % n], j > i && p[0] == q[0] && p[1] == q[1]; j--) ;
		for (j = i + 1; r = v[j % n], j < i + n && r[0] == q[0] && r[1] == q[1]; j++) ;
		a0 = atan2 (p[0] - q[0], q[1] - p[1]);
		a1 = atan2 (q[0] - r[0], r[1] - q[1]);
		turn = remainder (a1 - a0, 2 * M_PI);
		m = turn * w > 0 ? (int) ceil (fabs (turn) / 0.3) : 0;
		for (k = 0; k <= m; k++) {
			a = a0 + turn * (m ? (double) k / m : 0.5);
			glColor4f (c[0], c[1], c[2], 1.0f);
			glVertex3d (q[0], q[1], q[2]);
			glColor4f (c[0], c[1], c[2], 0.0f);
			glVertex3d (q[0] + w * cos (a), q[1] + w * sin (a), q[2]);
		}
	}
	glEnd ();
	glColor4fv (c);
}

void procfx_sky_end (void)
{
	glUseProgram (0);
	glPopAttrib ();
}

/* ---- layer 2: the game-over backdrop --------------------------------- */

/* A dark nebula behind the tombstone, in screen space: two layers of the
 * shared noise in the HD pages' night blues, a faint warm wisp, and a
 * vignette.  uFade brings it up from the black the game clears to. */
static const char end_glsl[] =
	"uniform vec4 uVp;\n"
	"uniform float uFade;\n"
	"void main () {\n"
	"	vec2 p = (gl_FragCoord.xy - uVp.xy) / uVp.w;\n"
	"	vec2 c = p - vec2 (0.5 * uVp.z / uVp.w, 0.5);\n"
	"	float neb = pf_fbm (vec3 (p * 2.2, 4.0), 5);\n"
	"	float wisp = pf_fbm (vec3 (p * 4.5 + neb, 9.0), 4);\n"
	"	vec3 col = vec3 (0.043, 0.063, 0.086);\n"
	"	col += vec3 (0.05, 0.11, 0.17) * smoothstep (0.42, 0.80, neb);\n"
	"	col += vec3 (0.07, 0.04, 0.09) * smoothstep (0.55, 0.85, wisp);\n"
	"	col *= 1.0 - 0.8 * smoothstep (0.30, 0.95, length (c));\n"
	"	gl_FragColor = vec4 (col * uFade, 1.0);\n"
	"}\n";

static GLuint end_prog;
static int end_failed;
static GLint u_end_vp, u_end_fade;

void procfx_endgame_backdrop (float fade)
{
	GLint vp[4];

	if (end_failed) return;
	if (!end_prog) {
		end_prog = procfx_program ("game-over backdrop", end_glsl);
		if (!end_prog) { end_failed = 1; return; }
		u_end_vp = glGetUniformLocation (end_prog, "uVp");
		u_end_fade = glGetUniformLocation (end_prog, "uFade");
	}
	glGetIntegerv (GL_VIEWPORT, vp);
	glPushAttrib (GL_ENABLE_BIT | GL_CURRENT_BIT | GL_POLYGON_BIT | GL_TRANSFORM_BIT);
	glDisable (GL_BLEND);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_CULL_FACE);
	glDisable (GL_LIGHTING);
	glDisable (GL_FOG);
	glDisable (GL_ALPHA_TEST);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
	glUseProgram (end_prog);
	glUniform4f (u_end_vp, (float) vp[0], (float) vp[1], (float) vp[2], (float) vp[3]);
	glUniform1f (u_end_fade, fade);
	glRectf (-1, -1, 1, 1);
	glUseProgram (0);
	glPopMatrix ();
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glPopAttrib ();
}

/* ---- glows: additive light on a square about a point ----------------- */

struct glow { const char *name, *glsl; GLuint prog; int failed; };

/* l's program (compiled on first use) bound for additive light, with the
 * eye ray set up; 0, and nothing changed, when it is not available */
static GLuint glow_begin (struct glow *l)
{
	GLint vp[4];
	GLdouble pr[16];

	if (l->failed) return 0;
	if (!l->prog && !(l->prog = procfx_program (l->name, l->glsl))) {
		l->failed = 1;
		return 0;
	}
	glGetIntegerv (GL_VIEWPORT, vp);
	glGetDoublev (GL_PROJECTION_MATRIX, pr);
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE);
	glBlendEquation (GL_FUNC_ADD);
	glUseProgram (l->prog);
	glUniform4f (glGetUniformLocation (l->prog, "uVp"), vp[0], vp[1], vp[2], vp[3]);
	glUniform2f (glGetUniformLocation (l->prog, "uFoc"), pr[0], pr[5]);
	return l->prog;
}

void procfx_glow_end (void)
{
	glUseProgram (0);
	glPopAttrib ();
}

static double smooth (double a, double b, double x)
{
	x = x < a ? 0 : x > b ? 1 : (x - a) / (b - a);
	return x * x * (3 - 2 * x);
}

/* ---- layer 2: the limb glow ------------------------------------------ */

int flat_limb = 0;

void procfx_limb_put (int col, double scale, const double c[3], double r, float limb[4])
{
	double d = sqrt (c[0]*c[0] + c[1]*c[1] + c[2]*c[2]), gam, thp, nr, fr, g;
	const char *k;
	int i;

	limb[0] = limb[1] = limb[2] = limb[3] = 0;
	if (flat_limb || scale <= 1.0 || r <= 0 || d <= r) return;
	k = body_class (pf_object (NULL));
	if (k && (!strcmp (k, "airless") || !strcmp (k, "star"))) return;
	/* gone before the game draws its own air: the near limb inside 178
	 * ST pixels of the screen centre and the far one past 409 less that
	 * (planet_atmosphere_band) */
	gam = atan2 (sqrt (c[0]*c[0] + c[1]*c[1]), c[2]);
	thp = asin (r / d);
	nr = gam - thp < 1.5 ? 256 * tan (gam - thp) : 1e4;
	fr = gam + thp < 1.5 ? 256 * tan (gam + thp) : 1e4;
	g = 1 - smooth (245, 409, nr + fr) * (1 - smooth (178, 230, nr));
	g *= smooth (1.3, 2.0, d / r) * (k && !strcmp (k, "rocky") ? 1.0 : 1.6);
	for (i = 0; i < 3; i++) limb[i] = g * ((col >> (8 - 4*i)) & 15) / 15.0;
	limb[3] = fmin (fmax ((scale - 1.0) * 12, 0.04), 0.12);
}

/* h: how far out from the limb, in widths of the rim; lit on the sun's
 * side, across the terminator a little, and all round when the sun is
 * behind the planet */
static const char limb_glsl[] =
	"uniform vec3 uC, uSun, uCol;\n"
	"uniform vec2 uTh;\n"
	"void main () {\n"
	"	vec3 r = pf_ray ();\n"
	"	float h = (atan (length (cross (r, uC)), dot (r, uC)) / uTh.x - 1.0) / uTh.y;\n"
	"	if (h > 4.0) discard;\n"
	"	vec3 n = r * dot (r, uC) - uC;\n"
	"	float lit = smoothstep (-0.35, 0.5, dot (n, uSun) / max (length (n), 1e-6));\n"
	"	lit += 0.6 * pow (max (dot (r, uSun), 0.0), 24.0);\n"
	"	float g = h < 0.0 ? exp (3.0 * h) : exp (-1.5 * h) * (1.0 - 0.25 * h);\n"
	"	gl_FragColor = vec4 (uCol * g * lit, 1.0);\n"
	"}\n";

static struct glow limb_fx = { "limb glow", limb_glsl };

float procfx_limb_begin (const float limb[4], const double c[3], double r, const double sun[3])
{
	double d = sqrt (c[0]*c[0] + c[1]*c[1] + c[2]*c[2]), thp, gam, h;
	GLuint p;

	if (flat_limb || limb[3] <= 0 || c[2] <= 0 || d <= r) return 0;
	thp = asin (r / d);
	gam = acos (c[2] / d);
	/* the square at the planet's depth that holds the glow's outer cone */
	h = c[2] * (tan (fmin (gam + thp * (1 + 4 * limb[3]), 1.45)) - tan (gam));
	if (h <= 0 || !(p = glow_begin (&limb_fx))) return 0;
	glUniform3f (glGetUniformLocation (p, "uC"), c[0] / d, c[1] / d, -c[2] / d);
	glUniform3f (glGetUniformLocation (p, "uSun"), sun[0], sun[1], -sun[2]);
	glUniform3fv (glGetUniformLocation (p, "uCol"), 1, limb);
	glUniform2f (glGetUniformLocation (p, "uTh"), thp, limb[3]);
	return h;
}

/* ---- layer 3: the corona --------------------------------------------- */

int flat_stars = 0;

void procfx_star_put (struct procfx_star *s)
{
	unsigned int rec;
	int m;

	s->model = 0;
	if (flat_stars || !is_star (m = pf_object (&rec))) return;
	s->model = m;
	s->seed = (unsigned int) STMemory_ReadLong (rec + OBJ_SEED);
}

/* gl_TexCoord[0] runs -1..1 across the square, uDisc is the disc's radius
 * on that scale.  The streamers are noise over the direction away from
 * the disc, turned into the sky's frame (a star's own rotation is not
 * kept where it is drawn), so they stay put when the ship rolls, and
 * seeded by the star. */
static const char corona_glsl[] =
	"uniform mat3 uRot;\n"
	"uniform vec3 uCol, uSeed;\n"
	"uniform float uDisc;\n"
	"void main () {\n"
	"	vec2 q = gl_TexCoord[0].xy;\n"
	"	float r = length (q);\n"
	"	if (r >= 1.0) discard;\n"
	"	float t = clamp ((r - uDisc) / (1.0 - uDisc), 0.0, 1.0);\n"
	"	vec3 u = uRot * vec3 (q / max (r, 1e-4), 0.0);\n"
	"	float s = smoothstep (0.45, 0.8, pf_fbm (u * (2.5 + 1.5 * t) + uSeed, 4));\n"
	"	float g = 0.55 * pow (1.0 - t, 4.0) + 0.6 * s * pow (1.0 - t, 2.0);\n"
	"	gl_FragColor = vec4 (uCol * g, 1.0);\n"
	"}\n";

static struct glow corona_fx = { "corona", corona_glsl };

/* by star_light */
static const float tone[L_MAX][3] = {
	{ 0.9, 0.9, 0.9 }, { 1.0, 0.45, 0.25 }, { 1.0, 0.65, 0.35 },
	{ 1.0, 0.86, 0.58 }, { 0.82, 0.88, 1.0 }, { 0.58, 0.72, 1.0 }
};

float procfx_corona_begin (const struct procfx_star *s, const float c[3], float r)
{
	/* three disc radii, and as far as the game's twinkle reaches */
	float h = 3 * r - 0.016f * c[2];
	const float *k = tone[star_light (s->model)];
	unsigned int z = s->seed;
	GLuint p;

	if (flat_stars || !s->model || c[2] >= 0 || !(p = glow_begin (&corona_fx))) return 0;
	pf_frame (glGetUniformLocation (p, "uRot"), sky_rot);
	glUniform3fv (glGetUniformLocation (p, "uCol"), 1, k);
	glUniform3f (glGetUniformLocation (p, "uSeed"),
		     (z & 1023) / 37.0f, (z >> 10 & 1023) / 41.0f, (z >> 20 & 1023) / 43.0f);
	glUniform1f (glGetUniformLocation (p, "uDisc"), r / h);
	return h;
}

/* the GL context is about to go: compile again in the next one */
void procfx_forget (void)
{
	struct glow *l[] = { &limb_fx, &corona_fx };
	int i;

	if (sky_prog) glDeleteProgram (sky_prog);
	if (end_prog) glDeleteProgram (end_prog);
	sky_prog = end_prog = 0;
	sky_failed = end_failed = 0;
	for (i = 0; i < 2; i++) {
		if (l[i]->prog) glDeleteProgram (l[i]->prog);
		l[i]->prog = 0;
		l[i]->failed = 0;
	}
}
