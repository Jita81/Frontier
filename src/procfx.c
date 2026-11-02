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
#include "../fixups.h"
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
	"}\n";

/* Compile one layer's fragment shader (the noise library plus body) into
 * a program.  No vertex shader: the fixed-function vertex path stays, so
 * the game's own geometry and colours come through unchanged as gl_Color.
 * 0 when GLSL is missing or the source does not compile; the caller then
 * draws the old way. */
GLuint procfx_program (const char *name, const char *body)
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

/* ---- layer 1: the sky band ------------------------------------------- */

int flat_sky = 0;

/* The background object: L44200_DrawBGStars sets this model word in its
 * object record before it calls L39426_PutGameData3DObj. */
#define SKY_MODEL	0xb0
/* the object record of the object being drawn, and its rotation: nine
 * 1.15 words, out[k] = sum_j w[3j+k] * in[j] (see L3b9aa, which turns a
 * model vector into the view with them) */
#define FRAME_OBJ	212
#define FRAME_ROT	36
#define OBJ_MODEL	90
#define OBJ_SEED	118

/* the record of the object being drawn, readable up to its seed; 0 for none */
static unsigned int pf_record (void)
{
	unsigned int a6 = (unsigned int) GetReg (REG_A6), rec;

	if (a6 < FRAME_OBJ || !rd_ok (a6 - FRAME_OBJ, 4)) return 0;
	rec = (unsigned int) STMemory_ReadLong (a6 - FRAME_OBJ);
	return rd_ok (rec, OBJ_SEED + 4) ? rec : 0;
}

/* the sky's frame and whether this 3D view has one (the player's flight
 * view): the corona's streamers are fixed to it and it gates the corona */
static float sky_rot[9];
int procfx_sky_seen;

int procfx_sky_put (float m[9])
{
	unsigned int a6 = (unsigned int) GetReg (REG_A6), rec = pf_record ();
	int i;

	if (!rec || !rd_ok (a6 - FRAME_ROT, 18)) return 0;
	if ((STMemory_ReadWord (rec + OBJ_MODEL) & 0xffff) != SKY_MODEL) return 0;
	for (i = 0; i < 9; i++)
		m[i] = (short) STMemory_ReadWord (a6 - FRAME_ROT + 2*i) / 32768.0f;
	memcpy (sky_rot, m, sizeof sky_rot);
	procfx_sky_seen = 1;
	return !flat_sky;
}

/* m (as procfx_sky_put) as the eye-to-object matrix: GL eye (x, y, z) is
 * the game's view (x, y, -z) (znode_wrvertex), and the object's frame is
 * the transpose of its rotation applied to that */
static void pf_frame (GLint loc, const float m[9])
{
	float s[9];
	int j;

	for (j = 0; j < 9; j++) s[j] = j % 3 == 2 ? -m[j] : m[j];
	glUniformMatrix3fv (loc, 1, GL_TRUE, s);
}

/* The eye ray of each pixel comes back from gl_FragCoord through the
 * projection the polygon was drawn with; uSky turns it into the sky's own
 * frame, so the pattern is fixed to the sky.  Star clouds are broad noise,
 * the dust lanes a ridge of finer noise, plus a few faint specks.  Cloud
 * sets coverage, which alpha (1 in the polygon, 0 at the strip's far side)
 * scales: thin cloud fades first and bright cloud reaches further out. */
static const char sky_glsl[] =
	"uniform mat3 uSky;\n"
	"uniform vec4 uVp;\n"
	"uniform vec2 uFoc;\n"
	"void main () {\n"
	"	vec2 ndc = (gl_FragCoord.xy - uVp.xy) / uVp.zw * 2.0 - 1.0;\n"
	"	vec3 d = normalize (uSky * vec3 (ndc.x / uFoc.x, ndc.y / uFoc.y, -1.0));\n"
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

/* ---- layer 3: planets ------------------------------------------------ */

int flat_planets = 0;

/* gl.c binds the textures of up to four light zones, each baked by
 * planet.c from the body's own features and colours, on units 0-3, and
 * gives every vertex: gl_TexCoord[0] the texture position, [1] cos(sun)
 * and the view cosine mu there (procfx_planet_attr), [2] the model
 * direction.  Each zone weighs in over its cos(sun) range with soft edges
 * that sum to 1, so the game's day, twilight and night colours blend
 * across the terminator.  On top: a lit body's day side falls off by up
 * to a fifth towards the terminator (full zone colour where the sun is
 * high), the limb darkens, fine noise fixed to the body (seeded by it)
 * fades in octave by octave only once that octave's cells span several
 * pixels, and not at grazing angles, so a far planet stays clean and
 * nothing shimmers; with an atmosphere the lit limb turns towards the
 * innermost layer's colour. */
static const char planet_glsl[] =
	"uniform sampler2D uZ0, uZ1, uZ2, uZ3;\n"
	"uniform vec4 uLo, uHi, uRim;\n"
	"uniform vec3 uLook, uSeed;\n"
	"uniform float uSoft;\n"
	"void main () {\n"
	"	float c = gl_TexCoord[1].x, mu = clamp (gl_TexCoord[1].y, 0.0, 1.0);\n"
	"	vec4 w = smoothstep (uLo - uSoft, uLo + uSoft, vec4 (c))\n"
	"	       - smoothstep (uHi - uSoft, uHi + uSoft, vec4 (c));\n"
	"	vec2 st = gl_TexCoord[0].st;\n"
	"	vec3 col = w.x * texture2D (uZ0, st).rgb + w.y * texture2D (uZ1, st).rgb\n"
	"	         + w.z * texture2D (uZ2, st).rgb + w.w * texture2D (uZ3, st).rgb;\n"
	"	float day = mix (1.0, smoothstep (-uSoft, uSoft, c), uLook.x);\n"
	"	col *= mix (1.0, 0.8 + 0.2 * smoothstep (0.0, 0.7, c), day * uLook.x);\n"
	"	col *= mix (1.0 - uLook.y, 1.0, sqrt (mu));\n"
	"	vec3 p = gl_TexCoord[2].xyz * 256.0 + uSeed;\n"
	"	float px = length (fwidth (p)), n = 0.0, a = 1.0;\n"
	"	for (int k = 0; k < 3; k++) {\n"
	"		float f = 1.0 - smoothstep (0.1, 0.4, px);\n"
	"		if (f <= 0.0) break;\n"
	"		n += a * f * (2.0 * pf_noise (p) - 1.0);\n"
	"		p = p * 2.03 + vec3 (1.7, 9.2, 3.1); px *= 2.03; a *= 0.5;\n"
	"	}\n"
	"	col *= 1.0 + uLook.z * smoothstep (0.05, 0.3, mu) * n;\n"
	"	col = mix (col, uRim.rgb, uRim.a * 0.6 * pow (1.0 - mu, 4.0) * day);\n"
	"	gl_FragColor = vec4 (col, 1.0);\n"
	"}\n";

static GLuint planet_prog;
static int planet_failed;
static GLint u_lo, u_hi, u_rim, u_look, u_seed, u_soft;

int procfx_planet_begin (const float lo[4], const float hi[4], float soft,
			 const float rim[4], const float look[3], unsigned int seed)
{
	static const char *zn[4] = { "uZ0", "uZ1", "uZ2", "uZ3" };
	int i;

	if (flat_planets || planet_failed) return 0;
	if (!planet_prog) {
		planet_prog = procfx_program ("planet", planet_glsl);
		if (!planet_prog) { planet_failed = 1; return 0; }
		glUseProgram (planet_prog);
		for (i = 0; i < 4; i++) glUniform1i (glGetUniformLocation (planet_prog, zn[i]), i);
		u_lo = glGetUniformLocation (planet_prog, "uLo");
		u_hi = glGetUniformLocation (planet_prog, "uHi");
		u_rim = glGetUniformLocation (planet_prog, "uRim");
		u_look = glGetUniformLocation (planet_prog, "uLook");
		u_seed = glGetUniformLocation (planet_prog, "uSeed");
		u_soft = glGetUniformLocation (planet_prog, "uSoft");
	}
	glUseProgram (planet_prog);
	glUniform4fv (u_lo, 1, lo);
	glUniform4fv (u_hi, 1, hi);
	glUniform4fv (u_rim, 1, rim);
	glUniform3fv (u_look, 1, look);
	glUniform3f (u_seed, seed & 0xff, (seed >> 8) & 0xff, (seed >> 16) & 0xff);
	glUniform1f (u_soft, soft);
	return 1;
}

void procfx_planet_bind (int unit, unsigned int tex)
{
	glActiveTexture (GL_TEXTURE0 + unit);
	glBindTexture (GL_TEXTURE_2D, tex);
	glActiveTexture (GL_TEXTURE0);
}

void procfx_planet_attr (double c, double mu, const double m[3])
{
	glMultiTexCoord2d (GL_TEXTURE1, c, mu);
	glMultiTexCoord3dv (GL_TEXTURE2, m);
}

void procfx_planet_end (void)
{
	glUseProgram (0);
}

/* ---- the game-over backdrop ----------------------------------------- */

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

/* ---- the star corona ------------------------------------------------ */

int flat_stars = 0;

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

/* only while the player's 3D view fills the screen (a6+42, as hdtip.c) */
void procfx_star_put (struct procfx_star *s)
{
	unsigned int rec = pf_record ();
	int m;

	s->model = 0;
	if (flat_stars || !rec || (signed char) STMemory_ReadByte (__DL5eb6_a6_base + 42) >= 0) return;
	if (!is_star (m = STMemory_ReadWord (rec + OBJ_MODEL) & 0xffff)) return;
	s->model = m;
	s->seed = (unsigned int) STMemory_ReadLong (rec + OBJ_SEED);
}

/* gl_TexCoord[0] runs -1..1 across the square, uC is the star's centre
 * and uDisc the disc's radius on that scale.  The streamers are noise
 * over the direction away from the star (the eye ray less its part along
 * the star's), turned into the sky's frame (a star's own rotation is not
 * kept where it is drawn) so they stay put when the ship rolls, seeded by
 * the star.  Deep tone by the disc. */
static const char corona_glsl[] =
	"uniform mat3 uRot;\n"
	"uniform vec3 uC, uCol, uSeed;\n"
	"uniform float uDisc;\n"
	"void main () {\n"
	"	vec2 q = gl_TexCoord[0].xy;\n"
	"	float r = length (q);\n"
	"	if (r >= 1.0) discard;\n"
	"	float t = clamp ((r - uDisc) / (1.0 - uDisc), 0.0, 1.0);\n"
	"	vec3 e = normalize (uC + vec3 (q, 0.0)), n = normalize (uC);\n"
	"	vec3 d = e - n * dot (e, n), u = uRot * (d / max (length (d), 1e-6));\n"
	"	float s = smoothstep (0.45, 0.8, pf_fbm (u * (2.5 + 1.5 * t) + uSeed, 4));\n"
	"	float g = min (0.55 * pow (1.0 - t, 4.0) + 0.6 * s * pow (1.0 - t, 2.0), 1.0);\n"
	"	gl_FragColor = vec4 (mix (uCol, uCol * uCol, (1.0 - t) * (1.0 - t)) * g, 1.0);\n"
	"}\n";

static GLuint corona_prog;
static int corona_failed;
static GLint u_rot, u_c, u_col, u_cseed, u_disc;

/* by star_light */
static const float tone[L_MAX][3] = {
	{ 0.9, 0.9, 0.9 }, { 1.0, 0.42, 0.22 }, { 1.0, 0.62, 0.30 },
	{ 1.0, 0.92, 0.72 }, { 0.82, 0.88, 1.0 }, { 0.58, 0.72, 1.0 }
};

float procfx_corona_begin (const struct procfx_star *s, const float c[3], float r)
{
	/* three disc radii, and as far as the game's twinkle reaches */
	float h = 3 * r - 0.016f * c[2];
	unsigned int z = s->seed;

	if (flat_stars || !s->model || !procfx_sky_seen || c[2] >= 0 || corona_failed) return 0;
	if (!corona_prog) {
		corona_prog = procfx_program ("corona", corona_glsl);
		if (!corona_prog) { corona_failed = 1; return 0; }
		u_rot = glGetUniformLocation (corona_prog, "uRot");
		u_c = glGetUniformLocation (corona_prog, "uC");
		u_col = glGetUniformLocation (corona_prog, "uCol");
		u_cseed = glGetUniformLocation (corona_prog, "uSeed");
		u_disc = glGetUniformLocation (corona_prog, "uDisc");
	}
	/* screen blend: light over a bright disc tints it, never burns it white */
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_COLOR);
	glBlendEquation (GL_FUNC_ADD);
	glUseProgram (corona_prog);
	pf_frame (u_rot, sky_rot);
	glUniform3f (u_c, c[0] / h, c[1] / h, c[2] / h);
	glUniform3fv (u_col, 1, tone[star_light (s->model)]);
	glUniform3f (u_cseed, (z & 1023) / 37.0f, (z >> 10 & 1023) / 41.0f, (z >> 20 & 1023) / 43.0f);
	glUniform1f (u_disc, r / h);
	return h;
}

void procfx_corona_end (void)
{
	glUseProgram (0);
	glPopAttrib ();
}

/* the GL context is about to go: compile again in the next one */
void procfx_forget (void)
{
	if (sky_prog) glDeleteProgram (sky_prog);
	if (planet_prog) glDeleteProgram (planet_prog);
	if (end_prog) glDeleteProgram (end_prog);
	if (corona_prog) glDeleteProgram (corona_prog);
	sky_prog = planet_prog = end_prog = corona_prog = 0;
	sky_failed = planet_failed = end_failed = corona_failed = 0;
}
