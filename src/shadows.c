/*
 * Contact shadows (shadows.h; docs/hd/procedural.md, "Contact shadows").
 * Landed, the player's ship's faces, struts and wheels are measured along
 * its own axes; the lowest point along its up axis is the ground, and the
 * blob is an ellipse there, moved away from the sun.  Reads only.
 */
#include <math.h>
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "main.h"
#include "../m68000.h"
#include "../host.h"
#include "../fixups.h"
#include "shadows.h"

#define A6		__DL5eb6_a6_base
#define A6_FLIGHT_STATE	704	/* byte */
#define A6_PLAYER	674	/* long: the player's ship object */
#define STATE_LANDED	0x60	/* on a pad (hdback.c) */
#define STATE_LANDED_ROUGH 0x68	/* down off the pads */
#define SHADOW_ALPHA	0.55f
#define SHADOW_SPREAD	1.2f	/* the blob's radii against the ship's */
#define UP		1	/* the ship's up axis: its model y, lowest at the wheels */

static int on, have;
static unsigned int rec_pl;
static float Mx[3][3], org[3], lt[3], lo[3], hi[3];

static int ok (unsigned int a, unsigned int len) { return a >= 0x400 && a <= MEM_SIZE && len <= MEM_SIZE - a; }

void shadows_begin (int hd)
{
	int st = STMemory_ReadByte (A6 + A6_FLIGHT_STATE) & 0xff;

	on = hd && !flat_shadows && (st == STATE_LANDED || st == STATE_LANDED_ROUGH);
	rec_pl = on ? STMemory_ReadLong (A6 + A6_PLAYER) : 0;
	have = 0;
}

/* the object's own frame from a part's: up the links while they carry
 * its record (as gl.c's hull_model_frame) */
static unsigned int top_frame (unsigned int f, unsigned int rec)
{
	int d;

	for (d = 0; d < 8 && ok (f, 4); d++) {
		unsigned int up = STMemory_ReadLong (f);
		if (up == rec) return f;
		if (up <= f || !ok (up - 212, 4) || STMemory_ReadLong (up - 212) != rec) return 0;
		f = up;
	}
	return 0;
}

int shadows_put (int first, int n, int r)
{
	unsigned int a6 = GetReg (REG_A6), va;
	int i, j, k;

	if (!on || !rec_pl || !ok (a6 - 212, 4) || STMemory_ReadLong (a6 - 212) != rec_pl) return 0;
	if (!have) {
		unsigned int f = top_frame (a6, rec_pl);
		if (!f || !ok (f - 212, 212) || !ok (a6 - 198, 6)) return 0;
		for (j = 0; j < 9; j++) Mx[j / 3][j % 3] = (short) STMemory_ReadWord (f - 36 + 2 * j) / 32768.0f;
		for (j = 0; j < 3; j++) {
			org[j] = (float) (int) STMemory_ReadLong (f - 16 + 4 * j);
			lt[j] = (short) STMemory_ReadWord (a6 - 198 + 2 * j);
			lo[j] = 1e30f;
			hi[j] = -1e30f;
		}
		have = 1;
	}
	for (i = 0; i < n; i++) {
		float d[3];
		if (!ok ((va = GetReg (first + i)) + 4, 12)) return 0;
		for (k = 0; k < 3; k++) d[k] = (float) (int) STMemory_ReadLong (va + 4 + 4 * k) - org[k];
		for (j = 0; j < 3; j++) {
			float p = Mx[j][0] * d[0] + Mx[j][1] * d[1] + Mx[j][2] * d[2];
			lo[j] = fminf (lo[j], p - r);
			hi[j] = fmaxf (hi[j], p + r);
		}
	}
	return 1;
}

int shadows_write (void *data)
{
	float *g = data, c[3], ln, lu, h, mid, off, lim, sh[3];
	const float *up = Mx[UP];
	int j, a = 0, b = 2;	/* the ground's axes: beam and length */

	if (!have || !(hi[a] > lo[a]) || !(hi[b] > lo[b]) || !(hi[UP] > lo[UP])) return 0;
	have = 0;
	/* the ground under the ship's middle, h below its origin */
	h = -lo[UP];
	for (j = 0; j < 3; j++)
		c[j] = org[j] - h * up[j] + Mx[a][j] * (lo[a] + hi[a]) / 2 + Mx[b][j] * (lo[b] + hi[b]) / 2;
	/* the sun's push along the ground: the light's travel along the
	 * ground over its fall, times the height of the ship's middle */
	ln = sqrtf (lt[0] * lt[0] + lt[1] * lt[1] + lt[2] * lt[2]);
	lu = ln > 0 ? (lt[0] * up[0] + lt[1] * up[1] + lt[2] * up[2]) / ln : 0;
	mid = (hi[UP] - lo[UP]) / 2;
	for (j = 0; j < 3; j++) sh[j] = lu < -0.05f ? (lt[j] / ln - lu * up[j]) * mid / -lu : 0;
	off = sqrtf (sh[0] * sh[0] + sh[1] * sh[1] + sh[2] * sh[2]);
	lim = fmaxf (hi[a] - lo[a], hi[b] - lo[b]) / 4;
	if (off > lim) for (j = 0; j < 3; j++) sh[j] *= lim / off;
	/* the centre and the two radii in GL's eye space (z negated) */
	for (j = 0; j < 3; j++) {
		float s = j == 2 ? -1.0f : 1.0f;
		g[j] = s * (c[j] + sh[j]);
		g[3 + j] = s * Mx[a][j] * (hi[a] - lo[a]) / 2 * SHADOW_SPREAD;
		g[6 + j] = s * Mx[b][j] * (hi[b] - lo[b]) / 2 * SHADOW_SPREAD;
	}
	g[9] = SHADOW_ALPHA;
	return 1;
}

void shadows_draw (void **data)
{
	const float *g = *data;
	int i, k, r;

	*data = (char *) *data + SHADOW_BYTES;
	glPushAttrib (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_LIGHTING_BIT | GL_CURRENT_BIT);
	glDisable (GL_LIGHTING);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_CULL_FACE);
	glShadeModel (GL_SMOOTH);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
	/* rings at q = k/6, alpha a (1 - q^2): darkest at the middle, 0 at the rim */
	for (k = 0; k < 6; k++) {
		glBegin (GL_TRIANGLE_STRIP);
		for (i = 0; i <= 40; i++) {
			float t = i * (float) M_PI / 20, ct = cosf (t), st = sinf (t);
			for (r = k; r <= k + 1; r++) {
				float q = r / 6.0f, w = 1 - q * q;
				glColor4f (0, 0, 0, g[9] * w);
				glVertex3f (g[0] + q * (ct * g[3] + st * g[6]), g[1] + q * (ct * g[4] + st * g[7]),
					    g[2] + q * (ct * g[5] + st * g[8]));
			}
		}
		glEnd ();
	}
	glPopAttrib ();
}
