/*
 * E3 ship redesign experiment (docs/experiments/EXP-03-ship-redesign.md).
 * GL build only, default OFF.
 *
 *   FE2_MESH_LOG=<file>    log, per frame, every 3D model instance the game
 *                          projected: model index, view position, rotation,
 *                          header scale and size, the primitive count and
 *                          the model-space box of the vertices it emitted
 *   FE2_SHIP_MESH=<index>  replace the model with this index (the word at
 *                          90(object) divided by 2) by a generated
 *                          placeholder mesh. The game's own primitives for
 *                          that model are not recorded; one mesh node is
 *                          put in the z-sorted display list instead, placed
 *                          with the game's view position and rotation,
 *                          sized to the game's own model-space box and
 *                          coloured with the game's dominant face colour
 *
 * How the host knows which model a primitive belongs to, without any fe2.s
 * change: fe2.s emits the Nu_* primitives while it interprets a model's
 * bytecode. L39426_PutGameData3DObj links a frame (link a6,#-220) straight
 * after "movea.l a6,a4", so its saved a6 equals its -212 request pointer;
 * sub-objects link further frames on top (saved a6 = parent frame). The host
 * walks the a6 chain to that top-level frame, which holds:
 *   -36(a6)  9 words: rotation, column j = view image of model axis j (1.15)
 *   -16(a6)  3 longs: view-space position of the model origin
 *   -4(a6)   model header address (header word 10 = scale, word 14 = size)
 *   -212(a6) the draw request; word 90 = model table offset
 * Primitives from any other code path have no such frame and are not touched.
 * The swap is only made while the camera is outside the game's own clipping
 * radius (size << scale), so a view from inside the hull keeps the original.
 *
 * With neither variable set expmesh_on is 0 and ExpMesh_Prim is never called.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "main.h"
#include "../m68000.h"
#include "../host.h"
#include "exp.h"

int expmesh_on;

static FILE *log_fp;
static int target = -1;		/* model index to replace, -1: none */
static unsigned long frame_no;

#define MAX_INST	96
struct inst {
	u32 a6, obj;
	int model;		/* index (table offset / 2) */
	int pos[3];		/* view space, as fe2.s holds it (z forward) */
	float axis[3][3];	/* unit view-space direction of each model axis */
	int scale, size;	/* header words 10 and 14 */
	short light[3];		/* view-space light vector, -198(a6) */
	int prims;
	float lo[3], hi[3];	/* model-space box of the emitted vertices */
	int nvert;
	int cols[16], colcount[16], ncols;
	int swap;		/* target model, camera outside it: draw ours */
	int replaced;		/* a mesh node was put in the display list */
	unsigned int z;
};
static struct inst insts[MAX_INST];
static int ninst;
static struct inst *cur;

static int in_ram (u32 a, int len)
{
	return a >= 0x100 && a + len <= MEM_SIZE;
}

static float rdf (u32 a) { return (float) STMemory_ReadLong (a); }

static int same_inst (const struct inst *p, u32 a6, u32 obj)
{
	int i;
	if (p->a6 != a6 || p->obj != obj) return 0;
	if (p->model != (unsigned short) STMemory_ReadWord (obj + 90) / 2) return 0;
	for (i = 0; i < 3; i++)
		if (p->pos[i] != STMemory_ReadLong (a6 - 16 + 4*i)) return 0;
	return 1;
}

/* the frame in a6 is a PutGameData3DObj frame with a consistent object */
/* Primitives are emitted from whatever frame is current: PutGameData3DObj's
 * own (link a6,#-220 straight after "movea.l a6,a4", so its saved a6 equals
 * its -212 request pointer) or a sub-object frame the bytecode interpreter
 * links on top of it (saved a6 = parent frame, -212 copied from the parent).
 * Walk the chain to the top-level frame: that one holds the request matrix at
 * -36, the request position at -16 and the model header at -4. */
static u32 top_frame (void)
{
	u32 f = GetReg (REG_A6), req;
	int depth;

	for (depth = 0; depth < 16; depth++) {
		if (!in_ram (f - 220, 224)) return 0;
		req = STMemory_ReadLong (f - 212);
		if (!in_ram (req, 96)) return 0;
		if (STMemory_ReadLong (f) == req) return f;
		f = STMemory_ReadLong (f);
	}
	return 0;
}

static struct inst *frame_inst (void)
{
	u32 a6 = top_frame (), obj, hdr;
	int i, j;
	struct inst *p;

	if (!a6) return NULL;
	obj = STMemory_ReadLong (a6 - 212);
	hdr = STMemory_ReadLong (a6 - 4);
	if (!in_ram (hdr, 16)) return NULL;
	/* the same stack buffer can carry several objects in turn: the model and
	 * position are part of the key */
	if (cur && same_inst (cur, a6, obj)) return cur;
	for (i = 0; i < ninst; i++)
		if (same_inst (&insts[i], a6, obj)) return cur = &insts[i];
	if (ninst == MAX_INST) return NULL;
	p = &insts[ninst++];
	memset (p, 0, sizeof (*p));
	p->a6 = a6;
	p->obj = obj;
	p->model = (unsigned short) STMemory_ReadWord (obj + 90) / 2;
	for (i = 0; i < 3; i++) p->pos[i] = STMemory_ReadLong (a6 - 16 + 4*i);
	for (j = 0; j < 3; j++) {
		float n = 0;
		for (i = 0; i < 3; i++) {
			p->axis[j][i] = (short) STMemory_ReadWord (a6 - 36 + 2*(3*j + i));
			n += p->axis[j][i] * p->axis[j][i];
		}
		n = sqrtf (n);
		if (n > 0) for (i = 0; i < 3; i++) p->axis[j][i] /= n;
	}
	p->scale = STMemory_ReadWord (hdr + 10);
	p->size = STMemory_ReadWord (hdr + 14);
	for (i = 0; i < 3; i++) p->light[i] = (short) STMemory_ReadWord (a6 - 198 + 2*i);
	/* swap only when the camera is outside the game's own clipping radius
	 * (header size << scale); otherwise, e.g. the rear view from inside the
	 * player's hull, the original polygons are kept */
	if (p->model == target) {
		double r = ldexp ((double) p->size, p->scale), d = 0;
		for (i = 0; i < 3; i++) d += (double) p->pos[i] * p->pos[i];
		p->swap = sqrt (d) > r;
	}
	return cur = p;
}

static void add_vertex (struct inst *p, u32 vptr)
{
	float d[3], m;
	int i, j;

	if (!in_ram (vptr + 4, 12)) return;
	for (i = 0; i < 3; i++) d[i] = rdf (vptr + 4 + 4*i) - p->pos[i];
	for (j = 0; j < 3; j++) {
		m = p->axis[j][0]*d[0] + p->axis[j][1]*d[1] + p->axis[j][2]*d[2];
		if (!p->nvert || m < p->lo[j]) p->lo[j] = m;
		if (!p->nvert || m > p->hi[j]) p->hi[j] = m;
	}
	p->nvert++;
}

static void add_colour (struct inst *p, int rgb)
{
	int i;
	rgb &= 0xfff;
	for (i = 0; i < p->ncols; i++)
		if (p->cols[i] == rgb) { p->colcount[i]++; return; }
	if (p->ncols < 16) {
		p->cols[p->ncols] = rgb;
		p->colcount[p->ncols++] = 1;
	}
}

/* Called by the harness wrapper before GL host call n (0x60..0x79) runs.
 * Returns 1 when the original call must be skipped. */
int ExpMesh_Prim (int n)
{
	struct inst *p;

	if (n == 0x6b) {		/* Nu_3DViewInit: new frame of primitives */
		ninst = 0;
		cur = NULL;
		return 0;
	}
	/* DrawScreen, IsGLRenderer, GLClearArea, QueueDrawStr, Put2DLine are not
	 * model primitives */
	if (n == 0x6f || n == 0x73 || n == 0x74 || n == 0x75 || n == 0x79) return 0;
	p = frame_inst ();
	if (!p) return 0;
	p->prims++;
	switch (n) {
	case 0x60:		/* PutTriangle a0-a2, colour d6 */
		add_vertex (p, GetReg (REG_A0));
		add_vertex (p, GetReg (REG_A1));
		add_vertex (p, GetReg (REG_A2));
		add_colour (p, GetReg (REG_D6));
		break;
	case 0x61:		/* PutQuad a0-a3, colour d6 */
		add_vertex (p, GetReg (REG_A0));
		add_vertex (p, GetReg (REG_A1));
		add_vertex (p, GetReg (REG_A2));
		add_vertex (p, GetReg (REG_A3));
		add_colour (p, GetReg (REG_D6));
		break;
	case 0x67:		/* ComplexStart, colour d6 */
		add_colour (p, GetReg (REG_D6));
		break;
	case 0x68: case 0x69:	/* ComplexSNext / SBegin a0 */
		add_vertex (p, GetReg (REG_A0));
		break;
	case 0x6c:		/* InsertZNode d4 */
		if (p->swap && !p->replaced) {
			p->replaced = 1;
			p->z = GetReg (REG_D4);
			Nu_ExtMeshNode (p->z, p - insts);
		}
		break;
	}
	return p->swap;
}

/* the game's own most-used face colour (rgb444) for this instance */
static int dominant_colour (const struct inst *p)
{
	int k, best = 0;
	for (k = 1; k < p->ncols; k++)
		if (p->colcount[k] > p->colcount[best]) best = k;
	return p->ncols ? p->cols[best] : 0x888;
}

void ExpMesh_EndFrame (void)
{
	int i;
	struct inst *p;

	if (log_fp) {
		for (i = 0; i < ninst; i++) {
			p = &insts[i];
			fprintf (log_fp, "F%lu model=%d pos=%d,%d,%d scale=%d size=%d prims=%d verts=%d"
				" box=%.0f..%.0f,%.0f..%.0f,%.0f..%.0f ax0=%.3f,%.3f,%.3f col=%03x%s\n",
				frame_no, p->model, p->pos[0], p->pos[1], p->pos[2],
				p->scale, p->size, p->prims, p->nvert,
				p->lo[0], p->hi[0], p->lo[1], p->hi[1], p->lo[2], p->hi[2],
				p->axis[0][0], p->axis[0][1], p->axis[0][2],
				dominant_colour (p), p->replaced ? " REPLACED" : "");
		}
	}
	frame_no++;
}

/* ---------------------------------------------------- placeholder mesh */

/* A convex faceted hull generated here: rings along model z from tail to
 * nose, each an 8-sided flattened section. It is normalised to the box
 * [-1,1]^3 and stretched to the game's own box at draw time. */
#define RINGS	6
#define SEGS	8
static float hull[RINGS * SEGS + 2][3];
static int hull_tri[2 * SEGS * (RINGS - 1) + 2 * SEGS][3];
static int hull_ntri;

static void build_hull (void)
{
	static const float zr[RINGS][2] = {	/* (z, radius) tail..nose */
		{ -1.00f, 0.55f }, { -0.70f, 0.95f }, { -0.30f, 1.00f },
		{ 0.15f, 0.85f }, { 0.55f, 0.55f }, { 0.85f, 0.22f },
	};
	int r, s, nv;
	const int tail = RINGS * SEGS, nose = RINGS * SEGS + 1;

	for (r = 0; r < RINGS; r++)
		for (s = 0; s < SEGS; s++) {
			float a = (s + 0.5f) * 2.0f * (float) M_PI / SEGS;
			hull[r*SEGS + s][0] = zr[r][1] * cosf (a);
			hull[r*SEGS + s][1] = zr[r][1] * sinf (a);
			hull[r*SEGS + s][2] = zr[r][0];
		}
	hull[tail][0] = hull[tail][1] = 0; hull[tail][2] = -1.0f;
	hull[nose][0] = hull[nose][1] = 0; hull[nose][2] = 1.0f;
	nv = 0;
	for (r = 0; r + 1 < RINGS; r++)
		for (s = 0; s < SEGS; s++) {
			int a = r*SEGS + s, b = r*SEGS + (s+1) % SEGS;
			int c = a + SEGS, d = b + SEGS;
			hull_tri[nv][0] = a; hull_tri[nv][1] = b; hull_tri[nv][2] = d; nv++;
			hull_tri[nv][0] = a; hull_tri[nv][1] = d; hull_tri[nv][2] = c; nv++;
		}
	for (s = 0; s < SEGS; s++) {
		int a = s, b = (s+1) % SEGS;
		hull_tri[nv][0] = tail; hull_tri[nv][1] = b; hull_tri[nv][2] = a; nv++;
		a += (RINGS-1)*SEGS; b += (RINGS-1)*SEGS;
		hull_tri[nv][0] = nose; hull_tri[nv][1] = a; hull_tri[nv][2] = b; nv++;
	}
	hull_ntri = nv;
}

/* Called from the GL display list walk (gl.c NU_EXTMESH). Faces are drawn
 * only when they face the camera, which is enough for a convex hull since
 * the 3D view has no depth buffer (it is painter-sorted by znode). */
void ExpMesh_Draw (int idx)
{
	struct inst *p;
	float v[RINGS * SEGS + 2][3], ctr[3], c[3], half[3], mid[3], lg[3], ln;
	int i, j, k, base;

	if (idx < 0 || idx >= ninst) return;
	p = &insts[idx];
	if (p->nvert < 3) return;
	base = dominant_colour (p);
	c[0] = ((base >> 8) & 15) / 15.0f;
	c[1] = ((base >> 4) & 15) / 15.0f;
	c[2] = (base & 15) / 15.0f;
	for (j = 0; j < 3; j++) {
		half[j] = 0.5f * (p->hi[j] - p->lo[j]);
		mid[j] = 0.5f * (p->hi[j] + p->lo[j]);
	}
	/* model -> view (fe2.s axes) -> GL (z negated, as znode_wrvertex does);
	 * hull x,y,z map to model axes 0,1,2 */
	for (i = 0; i < RINGS * SEGS + 2; i++) {
		float m[3], w[3];
		for (j = 0; j < 3; j++) m[j] = mid[j] + half[j] * hull[i][j];
		for (k = 0; k < 3; k++)
			w[k] = p->pos[k] + p->axis[0][k]*m[0] + p->axis[1][k]*m[1] + p->axis[2][k]*m[2];
		v[i][0] = w[0]; v[i][1] = w[1]; v[i][2] = -w[2];
	}
	for (k = 0; k < 3; k++) {
		ctr[k] = 0;
		for (i = 0; i < RINGS * SEGS + 2; i++) ctr[k] += v[i][k];
		ctr[k] /= RINGS * SEGS + 2;
	}
	lg[0] = -p->light[0]; lg[1] = -p->light[1]; lg[2] = p->light[2];
	ln = sqrtf (lg[0]*lg[0] + lg[1]*lg[1] + lg[2]*lg[2]);
	if (ln > 0) for (k = 0; k < 3; k++) lg[k] /= ln;

	glDisable (GL_LIGHTING);
	glDisable (GL_TEXTURE_2D);
	glBegin (GL_TRIANGLES);
	for (i = 0; i < hull_ntri; i++) {
		float *a = v[hull_tri[i][0]], *b = v[hull_tri[i][1]], *d = v[hull_tri[i][2]];
		float e1[3], e2[3], n[3], nl, dot, shade;
		for (k = 0; k < 3; k++) { e1[k] = b[k] - a[k]; e2[k] = d[k] - a[k]; }
		n[0] = e1[1]*e2[2] - e1[2]*e2[1];
		n[1] = e1[2]*e2[0] - e1[0]*e2[2];
		n[2] = e1[0]*e2[1] - e1[1]*e2[0];
		/* the view transform may flip handedness: orient each normal
		 * away from the hull centre rather than trusting the winding */
		if (n[0]*(a[0]-ctr[0]) + n[1]*(a[1]-ctr[1]) + n[2]*(a[2]-ctr[2]) < 0)
			for (k = 0; k < 3; k++) n[k] = -n[k];
		/* camera at the origin: keep faces whose normal points at it */
		if (n[0]*a[0] + n[1]*a[1] + n[2]*a[2] >= 0) continue;
		nl = sqrtf (n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
		if (nl == 0) continue;
		dot = (n[0]*lg[0] + n[1]*lg[1] + n[2]*lg[2]) / nl;
		shade = 0.45f + 0.75f * (dot > 0 ? dot : 0);
		glColor3f (fminf (1, c[0]*shade), fminf (1, c[1]*shade), fminf (1, c[2]*shade));
		glVertex3fv (a);
		glVertex3fv (b);
		glVertex3fv (d);
	}
	glEnd ();
}

void ExpMesh_Init (void)
{
	const char *s;

	if ((s = getenv ("FE2_MESH_LOG")) && *s) {
		log_fp = fopen (s, "w");
		if (log_fp) expmesh_on = 1;
	}
	if ((s = getenv ("FE2_SHIP_MESH")) && *s) {
		target = atoi (s);
		build_hull ();
		expmesh_on = 1;
		fprintf (stderr, "FE2_SHIP_MESH: model %d is replaced by a %d-triangle generated hull\n",
			target, hull_ntri);
	}
}
