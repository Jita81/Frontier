/*
 * skies: see skies.h and docs/hd/rings.md.
 */
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
#include "skies.h"

int flat_rings = 0;

/* the frame of the object being drawn (as procfx.c): its record, the
 * record's seed, and its rotation, nine 1.15 words; a model vector j
 * turns into view axis k by word 3j+k, so the model's y axis, the ring
 * plane's normal, is words 3..5 */
#define FRAME_OBJ	212
#define FRAME_ROT	36
#define OBJ_SEED	118

static unsigned int put_rec;
static double put_c[3], put_r, put_light[3], put_n[3];

static unsigned int frame_record (unsigned int a6)
{
	unsigned int rec;

	if (a6 < FRAME_OBJ || a6 > 0x110000u) return 0;
	rec = (unsigned int) STMemory_ReadLong (a6 - FRAME_OBJ);
	return rec <= 0x110000u - OBJ_SEED - 4 ? rec : 0;
}

void skies_planet_put (const double centre[3], double R, const double light[3])
{
	unsigned int a6 = (unsigned int) GetReg (REG_A6);
	int k;

	put_rec = frame_record (a6);
	for (k = 0; k < 3; k++) put_n[k] = (short) STMemory_ReadWord (a6 - FRAME_ROT + 2*(3+k)) / 32768.0;
	put_c[0] = centre[0]; put_c[1] = centre[1]; put_c[2] = centre[2];
	put_light[0] = light[0]; put_light[1] = light[1]; put_light[2] = light[2];
	put_r = R;
}

int skies_ring_put (struct skies_ring *r)
{
	double len;
	int k;

	if (flat_rings || !put_rec || frame_record ((unsigned int) GetReg (REG_A6)) != put_rec || put_r <= 0) return 0;
	len = sqrt (put_light[0]*put_light[0] + put_light[1]*put_light[1] + put_light[2]*put_light[2]);
	/* GL eye is the view with z turned round; the sun is minus the light */
	for (k = 0; k < 3; k++) {
		double s = k == 2 ? -1 : 1;
		r->c[k] = s * put_c[k] / put_r;
		r->n[k] = s * put_n[k];
		r->sun[k] = len > 0 ? -s * put_light[k] / len : 0;
	}
	r->seed = STMemory_ReadLong (put_rec + OBJ_SEED);
	return 1;
}

/* Each pixel's eye ray, back through the projection, meets the ring plane
 * at q; r = |q - c| in body radii.  Ringlets are the shared noise along r.
 * Each tenth of a radius in r holds a gap with odd 0.4, at a place and of
 * a width drawn from the seed: in r, so every quarter of a band agrees.
 * The shape runs from its least to its greatest r (uBand.xy); both edges
 * fade over a pixel and a half or 4% of the band.  Edge on (uBand.z near
 * 0) r means nothing, so the detail goes and alpha is flat. */
static const char ring_glsl[] =
	"uniform vec4 uVp, uPrj, uBand;\n"
	"uniform vec3 uC, uN, uSun;\n"
	"uniform float uOn;\n"
	"void main () {\n"
	"	if (uOn < 0.5) { gl_FragColor = gl_Color; return; }\n"
	"	vec2 ndc = (gl_FragCoord.xy - uVp.xy) / uVp.zw * 2.0 - 1.0;\n"
	"	vec3 d = vec3 ((ndc.x + uPrj.z) / uPrj.x, (ndc.y + uPrj.w) / uPrj.y, -1.0);\n"
	"	vec3 q = d * dot (uC, uN) / dot (d, uN) - uC;\n"
	"	float r = length (q), w = uBand.y - uBand.x;\n"
	"	float u = clamp ((r - uBand.x) / w, 0.0, 1.0);\n"
	"	float e = max (1.5 * fwidth (r) / w, 0.04);\n"
	"	float edge = smoothstep (0.0, e, u) * smoothstep (0.0, e, 1.0 - u);\n"
	"	float g = pf_fbm (vec3 (r * 24.0 + uBand.w, 0.5, 0.5), 3);\n"
	"	float x = r * 10.0, cell = floor (x);\n"
	"	float hw = 0.15 + 0.2 * pf_hash (vec3 (cell, uBand.w, 19.0));\n"
	"	float gap = pf_hash (vec3 (cell, uBand.w, 7.0)) < 0.4 ?\n"
	"		smoothstep (0.0, hw, abs (fract (x) - 0.2 - 0.6 * pf_hash (vec3 (cell, uBand.w, 13.0)))) : 1.0;\n"
	"	float a = (0.72 + 0.22 * g) * (0.3 + 0.7 * gap) * edge;\n"
	"	float m = (0.95 + 0.2 * g) * (1.0 + 0.08 * max (dot (normalize (q), uSun), 0.0));\n"
	"	gl_FragColor = vec4 (gl_Color.rgb * mix (1.0, m, uBand.z), mix (0.8, a, uBand.z));\n"
	"}\n";

static GLuint ring_prog;
static int ring_failed;
static GLint u_vp, u_prj, u_band, u_on, u_c, u_n, u_sun;
static struct skies_ring cur;
static double cur_prj[4], cur_vp[4], r_lo, r_hi;

int skies_ring_begin (const struct skies_ring *r, const double proj[16], const int vp[4])
{
	if (ring_failed) return 0;
	if (!ring_prog) {
		ring_prog = procfx_program ("rings", ring_glsl);
		if (!ring_prog) { ring_failed = 1; return 0; }
		u_vp = glGetUniformLocation (ring_prog, "uVp");
		u_prj = glGetUniformLocation (ring_prog, "uPrj");
		u_band = glGetUniformLocation (ring_prog, "uBand");
		u_on = glGetUniformLocation (ring_prog, "uOn");
		u_c = glGetUniformLocation (ring_prog, "uC");
		u_n = glGetUniformLocation (ring_prog, "uN");
		u_sun = glGetUniformLocation (ring_prog, "uSun");
	}
	cur = *r;
	cur_prj[0] = proj[0]; cur_prj[1] = proj[5]; cur_prj[2] = proj[8]; cur_prj[3] = proj[9];
	cur_vp[0] = vp[0]; cur_vp[1] = vp[1]; cur_vp[2] = vp[2]; cur_vp[3] = vp[3];
	r_lo = 1e30; r_hi = 0;
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram (ring_prog);
	glUniform4f (u_vp, vp[0], vp[1], vp[2], vp[3]);
	glUniform4f (u_prj, proj[0], proj[5], proj[8], proj[9]);
	glUniform3fv (u_c, 1, r->c);
	glUniform3fv (u_n, 1, r->n);
	glUniform3fv (u_sun, 1, r->sun);
	glUniform1f (u_on, 0);	/* off until a contour shows a ring */
	return 1;
}

/* the polygon is drawn at gluTessEndPolygon, after its last contour, so
 * the band's range is known in time */
void skies_ring_contour (const double (*v)[3], int n)
{
	const float *c = cur.c, *nn = cur.n;
	double cn = c[0]*nn[0] + c[1]*nn[1] + c[2]*nn[2], cl, open;
	int i, k;

	for (i = 0; i < n; i++) {
		double d[3], dn, t, r = 0;
		d[0] = ((v[i][0] - cur_vp[0]) / cur_vp[2] * 2 - 1 + cur_prj[2]) / cur_prj[0];
		d[1] = ((v[i][1] - cur_vp[1]) / cur_vp[3] * 2 - 1 + cur_prj[3]) / cur_prj[1];
		d[2] = -1;
		dn = d[0]*nn[0] + d[1]*nn[1] + d[2]*nn[2];
		if (dn == 0) continue;
		t = cn / dn;
		for (k = 0; k < 3; k++) r += (t*d[k] - c[k]) * (t*d[k] - c[k]);
		r = sqrt (r);
		if (r < r_lo) r_lo = r;
		if (r > r_hi) r_hi = r;
	}
	/* a ring lies well outside its body; anything else stays the game's */
	if (!(r_lo >= 1.02 && r_hi <= 8 && r_hi > r_lo + 0.01)) return;
	cl = sqrt (c[0]*c[0] + c[1]*c[1] + c[2]*c[2]);
	open = cl > 0 ? fabs (cn) / cl : 0;
	open = open < 0.03 ? 0 : open > 0.12 ? 1 : (open - 0.03) / 0.09;
	glUniform4f (u_band, r_lo, r_hi, open * open * (3 - 2*open), (cur.seed % 4093) * 0.37f);
	glUniform1f (u_on, 1);
}

void skies_ring_end (void)
{
	glUseProgram (0);
	glPopAttrib ();
}
