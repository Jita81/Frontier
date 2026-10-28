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
	/* the brighter of band and sky wins, so overlaps never add up */
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	glEnable (GL_BLEND);
	glBlendEquation (GL_MAX);
	glUseProgram (sky_prog);
	glUniformMatrix3fv (u_sky, 1, GL_TRUE, s);
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

/* the GL context is about to go: compile again in the next one */
void procfx_forget (void)
{
	if (sky_prog) glDeleteProgram (sky_prog);
	sky_prog = 0;
	sky_failed = 0;
}
