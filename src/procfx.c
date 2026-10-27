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

int procfx_sky_put (float m[9])
{
	unsigned int a6, rec;
	int i;

	if (flat_sky) return 0;
	a6 = (unsigned int) GetReg (REG_A6);
	if (a6 < FRAME_OBJ || !rd_ok (a6 - FRAME_OBJ, 4) || !rd_ok (a6 - FRAME_ROT, 18)) return 0;
	rec = (unsigned int) STMemory_ReadLong (a6 - FRAME_OBJ);
	if (!rd_ok (rec + OBJ_MODEL, 2)) return 0;
	if ((STMemory_ReadWord (rec + OBJ_MODEL) & 0xffff) != SKY_MODEL) return 0;
	for (i = 0; i < 9; i++)
		m[i] = (short) STMemory_ReadWord (a6 - FRAME_ROT + 2*i) / 32768.0f;
	return 1;
}

/* The eye ray of each pixel comes back from gl_FragCoord through the
 * projection the polygon was drawn with, and uSky turns it into the
 * background object's own frame: a fixed direction in the sky gets a
 * fixed pattern.  Star clouds are broad noise, the dust lanes a ridge of
 * finer noise, and a few faint specks follow the clouds.  On average the
 * band keeps the game's colour.  Alpha is the vertex colour's: 1 inside
 * the polygon, falling to 0 across the feather strip below. */
static const char sky_glsl[] =
	"uniform mat3 uSky;\n"
	"uniform vec4 uVp;\n"
	"uniform vec2 uFoc;\n"
	"void main () {\n"
	"	vec2 ndc = (gl_FragCoord.xy - uVp.xy) / uVp.zw * 2.0 - 1.0;\n"
	"	vec3 d = normalize (uSky * vec3 (ndc.x / uFoc.x, ndc.y / uFoc.y, -1.0));\n"
	"	float cloud = pf_fbm (d * 3.5 + vec3 (11.0), 5);\n"
	"	float lane = 1.0 - abs (2.0 * pf_fbm (d * 9.0 + vec3 (3.0), 4) - 1.0);\n"
	"	float lum = 0.6 + 1.3 * smoothstep (0.35, 0.80, cloud);\n"
	"	lum *= 1.0 - 0.4 * smoothstep (0.84, 0.98, lane);\n"
	"	vec3 base = gl_Color.rgb;\n"
	"	vec3 core = mix (base, vec3 (0.66, 0.72, 0.98), 0.5);\n"
	"	vec3 col = mix (base, core, smoothstep (0.55, 0.85, cloud)) * lum;\n"
	"	float speck = step (0.996, pf_hash (floor (d * 900.0)));\n"
	"	col += speck * (0.25 + 0.6 * smoothstep (0.4, 0.75, cloud)) * vec3 (0.6, 0.65, 0.75);\n"
	"	gl_FragColor = vec4 (col, gl_Color.a);\n"
	"}\n";

static GLuint sky_prog;
static int sky_failed;
static GLint u_sky, u_vp, u_foc;
static double sky_vp_h;

int procfx_sky_begin (const float m[9], const double proj[16], const int vp[4])
{
	float s[9];
	int j;

	if (flat_sky || sky_failed) return 0;
	if (!sky_prog) {
		sky_prog = procfx_program ("sky band", sky_glsl);
		if (!sky_prog) { sky_failed = 1; return 0; }
		u_sky = glGetUniformLocation (sky_prog, "uSky");
		u_vp = glGetUniformLocation (sky_prog, "uVp");
		u_foc = glGetUniformLocation (sky_prog, "uFoc");
	}
	/* GL eye (x, y, z) is the game's view (x, y, -z) (znode_wrvertex);
	 * the object's frame is the transpose of its rotation applied to
	 * that.  Row j of the eye-to-sky matrix: */
	for (j = 0; j < 3; j++) {
		s[3*j + 0] = m[3*j + 0];
		s[3*j + 1] = m[3*j + 1];
		s[3*j + 2] = -m[3*j + 2];
	}
	glPushAttrib (GL_ENABLE_BIT);
	glEnable (GL_BLEND);	/* the blend function is set once, in gl.c */
	glUseProgram (sky_prog);
	glUniformMatrix3fv (u_sky, 1, GL_TRUE, s);
	glUniform4f (u_vp, (float) vp[0], (float) vp[1], (float) vp[2], (float) vp[3]);
	glUniform2f (u_foc, (float) proj[0], (float) proj[5]);
	sky_vp_h = vp[3];
	return 1;
}

/* The polygon's outline is a hard edge on a dark sky.  Outside it, a strip
 * FEATHER of the screen height wide runs from the band's colour (alpha 1)
 * to nothing (alpha 0); the shader is still bound, so the clouds carry on
 * into it.  Where two patches of the band meet, the strip lies over the
 * same colour and does not show.  The strip goes out on the side away
 * from the contour's own inside, whichever way the contour winds. */
#define FEATHER	0.03

void procfx_sky_feather (const double (*v)[3], int n)
{
	double area = 0, w;
	float c[4];
	int i;

	if (n < 3) return;
	for (i = 0; i < n; i++)
		area += v[i][0] * v[(i+1) % n][1] - v[(i+1) % n][0] * v[i][1];
	w = (area > 0 ? FEATHER : -FEATHER) * sky_vp_h;
	glGetFloatv (GL_CURRENT_COLOR, c);
	glBegin (GL_QUAD_STRIP);
	for (i = 0; i <= n; i++) {
		const double *p = v[(i + n - 1) % n], *q = v[i % n], *r = v[(i + 1) % n];
		double ex = q[0] - p[0], ey = q[1] - p[1], fx = r[0] - q[0], fy = r[1] - q[1];
		double le = hypot (ex, ey), lf = hypot (fx, fy), nx = 0, ny = 0, l;

		/* the mean of the right-hand normals of the two edges at q */
		if (le > 0) { nx += ey / le; ny -= ex / le; }
		if (lf > 0) { nx += fy / lf; ny -= fx / lf; }
		l = hypot (nx, ny);
		if (l > 0) { nx /= l; ny /= l; }
		glColor4f (c[0], c[1], c[2], 1.0f);
		glVertex3d (q[0], q[1], q[2]);
		glColor4f (c[0], c[1], c[2], 0.0f);
		glVertex3d (q[0] + w * nx, q[1] + w * ny, q[2]);
	}
	glEnd ();
	glColor4fv (c);
}

void procfx_sky_end (void)
{
	glUseProgram (0);
	glPopAttrib ();
}
