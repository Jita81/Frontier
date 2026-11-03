/*
 * Scenery round a surface port (props.h; docs/hd/props.md).  Props stand on
 * the port's own flat green ground, clear of its buildings, roads, apron,
 * pads and lakes, placed once from the first eight drawn frames measured in
 * the port's model space (+y up, 16384 units a storey).  Each prop is one
 * node in the port's z tree at the z the engine gives a building at that
 * depth, so the painter's order holds.  Reads ST memory only.
 */
#include <math.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "main.h"
#include "../m68000.h"
#include "../host.h"
#include "../fixups.h"
#include "props.h"

#define A6		__DL5eb6_a6_base
#define A6_BIG_SPACE	14466		/* long: the object slots */
#define OBJ_OFFSETS	__DL44b02	/* word per slot: its offset */
#define STOREY		16384.0f	/* model units: a storey of the port's buildings */
#define CELL		(3 * STOREY)	/* the placement grid */
#define MARGIN		(2 * STOREY)	/* clear of roads, apron and buildings */
#define FLAT		(STOREY / 8)	/* a ground shape's y-range is under this */
#define CLOUD		(16 * STOREY)	/* above the ground: clouds */
#define ZK		0.141f		/* engine z per view depth unit (measured) */
#define WINDOW		8		/* drawn frames measured before the freeze */
#define NPROPS		200
#define SV		32		/* outline points kept per flat shape */

static int on, logging = -1, frames;
static int ok (unsigned int a, unsigned int len) { return a >= 0x400 && a <= MEM_SIZE && len <= MEM_SIZE - a; }
static int rw (unsigned int a) { return (short) STMemory_ReadWord (a); }
static unsigned int rl (unsigned int a) { return STMemory_ReadLong (a); }
static float smooth (float a, float b, float x) { x = fminf (1, fmaxf (0, (x - a) / (b - a))); return x * x * (3 - 2 * x); }

/* --- the frame's port records and puts, in view space ----------------- */
struct rec { unsigned int p, seed, bseed; int word, kind, s, h10; float R[9], o[3], lt[3]; };
struct prim { struct rec *r; int n, solid, col, v0; void *tree; };
static struct rec rc[48];
static struct prim pr[2048], *cur;
static float vp[32768][3];
static int nrc, npr, nvp, shape_open, shape_col;
static unsigned int shape_st;

/* the record drawn at a6: kind 1 a surface port on a terran body, 2 on an
 * ice body, 0 anything else; one look per record a frame */
static struct rec *rec_of (unsigned int a6)
{
	unsigned int p, f, b, big, gd, mh;
	struct rec *r;
	int j, i, body;
	if (!ok (a6 - 212, 4) || !ok (p = rl (a6 - 212), 286)) return NULL;
	for (j = nrc - 1; j >= 0; j--) if (rc[j].p == p) return &rc[j];
	if (nrc == 48) return NULL;
	r = memset (&rc[nrc++], 0, sizeof *r);
	r->p = p;
	r->word = STMemory_ReadWord (p + 90) & 0xffff;
	if (r->word < 90 || r->word > 108 || (r->word & 1) || !(STMemory_ReadByte (p + 280) & 16)) return r;
	for (f = a6, j = 0; j < 8 && ok (f, 4) && rl (f) != p; j++)	/* the object's own frame (as shadows.c) */
		f = rl (f) > f && ok (rl (f) - 212, 4) && rl (rl (f) - 212) == p ? rl (f) : 0;
	if (!ok (f, 4) || rl (f) != p || !ok (f - 212, 212) || !ok (a6 - 198, 6)) return r;
	r->seed = rl (p + 118);
	r->s = rw (f - 208);
	for (j = 0; j < 9; j++) r->R[j] = rw (f - 36 + 2 * j) / 32768.0f;
	for (j = 0; j < 3; j++) r->o[j] = (float) (int) rl (f - 16 + 4 * j), r->lt[j] = rw (a6 - 198 + 2 * j);
	big = rl (A6 + A6_BIG_SPACE);	/* the body it stands on: its parent slot */
	i = STMemory_ReadByte (p + 68) & 0xff;
	if (i >= 115 || !ok (big, 4) || !ok (b = big + (short) rw (OBJ_OFFSETS + 2 * i), 286)) return r;
	body = STMemory_ReadWord (b + 90) & 0xffff;
	r->bseed = rl (b + 118);
	gd = rl (__DL973c_game_data);
	if (!ok (gd + r->word, 2) || !ok (mh = gd + (STMemory_ReadWord (gd + r->word) & 0xffff), 12)) return r;
	r->h10 = rw (mh + 10);
	if (r->h10 < -4 || r->h10 > 24 || r->s < -8 || r->s > 24) return r;
	r->kind = body == 0x8c ? 2 : body == 0x88 || body == 0x8a || body == 0x90;
	return r;
}

static void close_shape (void)
{
	if (cur && (cur->n < 3 || (shape_st <= MEM_SIZE - 2 && (STMemory_ReadWord (shape_st) & 0xffff) != 0x2a)))
		npr--, nvp = cur->v0;	/* too short, or aborted part way */
	cur = NULL, shape_open = 0;
}

void props_begin (int hd)
{
	if (!(on = hd && props_opt_in && !flat_props)) return;
	if (logging < 0) logging = getenv ("FE2_PROPS_LOG") != NULL;
	nrc = npr = nvp = shape_open = 0, cur = NULL;
}

void props_shape (unsigned int rgb444)
{
	if (!on) return;
	close_shape ();
	shape_open = 1, shape_col = rgb444 & 0xfff, shape_st = rl (__DL385c8_primitives_end);
}

void props_put (int first, int n, int solid, void *tree)
{
	struct rec *r;
	struct prim *q;
	unsigned int va;
	int i, k;
	if (!on) return;
	if (solid) close_shape ();
	else if (!shape_open) return;
	r = rec_of (GetReg (REG_A6));
	if (!r || !r->kind || (cur && cur->r != r)) { if (!solid && !cur) shape_open = 0; return; }
	if (!(q = cur)) {
		if (npr == 2048) { shape_open = 0; return; }
		q = &pr[npr++];
		*q = (struct prim) { r, 0, solid, solid ? GetReg (REG_D6) & 0xfff : shape_col, nvp, tree };
		if (!solid) cur = q;
	}
	for (i = 0; i < n && nvp < 32768 && q->n < 128; i++, nvp++, q->n++) {
		if (!ok ((va = GetReg (first + i)) + 4, 12)) break;
		for (k = 0; k < 3; k++) vp[nvp][k] = (float) (int) rl (va + 4 + 4 * k);
	}
	if (solid && q->n < 3) npr--, nvp = q->v0;
}

/* --- the kit: Claude-authored primitive rows (docs/hd/props.md) ------
 * a part is a frustum of `sides`, radii rb..rt from yb to yt (storeys),
 * at x offset xo, leaning lean degrees about z; a bottom cap when cap is
 * set.  Variants: 0-1 pines, 2-3 shrubs, 4-5 ice spires. */
static const struct part { int v, sides, cap; float rb, rt, yb, yt, xo, lean; int rgb; } kit[] = {
	{ 0, 4, 0, .16f, .10f, -.10f, .90f, 0, 0, 0x642 },	/* pine A: trunk */
	{ 0, 6, 1, 1.10f, 0, .60f, 2.40f, 0, 0, 0x262 },	/* lower tier */
	{ 0, 6, 1, .75f, 0, 1.70f, 3.40f, 0, 0, 0x373 },	/* upper tier */
	{ 1, 4, 0, .14f, .09f, -.10f, .80f, 0, 0, 0x532 },	/* pine B */
	{ 1, 6, 1, 1.00f, 0, .50f, 2.10f, 0, 0, 0x253 },
	{ 1, 6, 1, .65f, 0, 1.50f, 3.00f, 0, 0, 0x364 },
	{ 2, 6, 0, 0, .55f, -.05f, .25f, 0, 0, 0x463 },	/* shrub A: bicone */
	{ 2, 6, 0, .55f, 0, .25f, .60f, 0, 0, 0x473 },
	{ 3, 6, 0, 0, .45f, -.05f, .20f, 0, 0, 0x552 },	/* shrub B */
	{ 3, 6, 0, .45f, 0, .20f, .50f, 0, 0, 0x562 },
	{ 4, 5, 1, .40f, 0, -.10f, 2.20f, 0, 0, 0xcdf },	/* spire A: cone */
	{ 4, 4, 1, .20f, 0, -.10f, 1.20f, .35f, -18, 0xbcf },	/* leaning shard */
	{ 5, 5, 1, .35f, 0, -.10f, 1.60f, 0, 0, 0xdef },	/* spire B */
	{ 5, 4, 1, .18f, 0, -.10f, .90f, -.28f, 22, 0xace },
};
static float mt[6][32][3][3], mn[6][32][3], mh[6];
static int mc[6][32], mnt[6], kit_built;

/* a triangle of variant v, wound and normalled away from the part's axis */
static void mesh_tri (int v, const float *a, const float *b, const float *c, const float *ctr, int rgb)
{
	float u[3], w[3], n[3], g[3], l;
	int j, t = mnt[v], f;
	for (j = 0; j < 3; j++) u[j] = b[j] - a[j], w[j] = c[j] - a[j], g[j] = (a[j] + b[j] + c[j]) / 3 - ctr[j];
	n[0] = u[1] * w[2] - u[2] * w[1], n[1] = u[2] * w[0] - u[0] * w[2], n[2] = u[0] * w[1] - u[1] * w[0];
	if (t == 32 || (l = sqrtf (n[0] * n[0] + n[1] * n[1] + n[2] * n[2])) < 1e-9f) return;
	f = n[0] * g[0] + n[1] * g[1] + n[2] * g[2] < 0;
	memcpy (mt[v][t][0], a, 12), memcpy (mt[v][t][1], f ? c : b, 12), memcpy (mt[v][t][2], f ? b : c, 12);
	for (j = 0; j < 3; j++) mn[v][t][j] = (f ? -n[j] : n[j]) / l;
	mc[v][t] = rgb, mnt[v]++;
}

static void build_kit (void)
{
	unsigned int i;
	int k, j;
	for (i = 0; i < sizeof kit / sizeof kit[0]; i++) {
		const struct part *q = &kit[i];
		float ring[2][6][3], ap[2][3], ctr[3], c = cosf (q->lean * (float) M_PI / 180), s = sinf (q->lean * (float) M_PI / 180);
		for (k = 0; k < q->sides; k++) for (j = 0; j < 2; j++) {
			float a = 2 * (float) M_PI * (k + .5f) / q->sides, r = j ? q->rt : q->rb;
			ring[j][k][0] = q->xo + r * cosf (a), ring[j][k][1] = j ? q->yt : q->yb, ring[j][k][2] = r * sinf (a);
		}
		ap[0][0] = ap[1][0] = q->xo, ap[0][1] = q->yb, ap[1][1] = q->yt, ap[0][2] = ap[1][2] = 0;
		for (k = 0; k < 14; k++) {	/* lean about z through the part's foot */
			float *p = k < 12 ? ring[k / 6][k % 6] : ap[k - 12], x = p[0] - q->xo, y = p[1] - q->yb;
			p[0] = q->xo + x * c - y * s, p[1] = q->yb + x * s + y * c;
		}
		for (j = 0; j < 3; j++) ctr[j] = (ap[0][j] + ap[1][j]) / 2;
		for (k = 0; k < q->sides; k++) {
			int k1 = (k + 1) % q->sides, b = q->rb > 0;
			if (b && q->rt > 0) mesh_tri (q->v, ring[0][k], ring[0][k1], ring[1][k1], ctr, q->rgb),
					    mesh_tri (q->v, ring[0][k], ring[1][k1], ring[1][k], ctr, q->rgb);
			else mesh_tri (q->v, ring[!b][k], ring[!b][k1], ap[b], ctr, q->rgb);
			if (q->cap && b && k > 1) mesh_tri (q->v, ring[0][0], ring[0][k - 1], ring[0][k], ctr, q->rgb);
		}
		mh[q->v] = fmaxf (mh[q->v], fmaxf (ap[1][1], ring[1][0][1]));
	}
	kit_built = 1;
}

/* --- the port, measured and then frozen ------------------------------ */
struct shp { float y, b[4]; int green, n; float p[SV][2]; };	/* a flat shape: its outline */
struct prop { float x, y, z, size, tint, yaw, lean, kt; int v; unsigned int h, zn; float kp; };
static struct { unsigned int p, word, seed; int win, frozen; } key;
static struct shp sh[384];
static float kb[512][4];	/* xz boxes, grown by the margin, of all that stands */
static struct prop pp[NPROPS], cand[4096];
static int nsh, nkb, npp;
static struct rec *P;		/* this frame's port */
static float fsc, eye[3], L[3], day, fpx, pj[16];	/* the 3D view's last focal length (px) and projection */
static void *ptree;

static int green (int c) { return (c >> 4 & 15) > (c >> 8 & 15) && (c >> 4 & 15) > (c & 15); }
static int in_poly (const struct shp *s, float x, float z)
{
	int i, j, c = 0;
	if (x < s->b[0] || x > s->b[2] || z < s->b[1] || z > s->b[3]) return 0;
	for (i = 0, j = s->n - 1; i < s->n; j = i++)
		if ((s->p[i][1] > z) != (s->p[j][1] > z) &&
		    x < (s->p[j][0] - s->p[i][0]) * (z - s->p[i][1]) / (s->p[j][1] - s->p[i][1]) + s->p[i][0]) c = !c;
	return c;
}
static unsigned int mix (unsigned int h) { h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; return h ^ h >> 16; }
static unsigned int hsh (int i, int j, unsigned int salt) { return mix (mix (mix (key.seed ^ salt) ^ P->bseed) + (unsigned int) i * 0x9e3779b1u) ^ mix ((unsigned int) j * 0x85ebca77u); }
static float u01 (unsigned int h) { return (h >> 8) / 16777216.0f; }
static float vnoise (float x, float z)
{
	int i = (int) floorf (x), j = (int) floorf (z);
	float fx = smooth (0, 1, x - i), fz = smooth (0, 1, z - j);
	float a = u01 (hsh (i, j, 7)), b = u01 (hsh (i + 1, j, 7)), c = u01 (hsh (i, j + 1, 7)), d = u01 (hsh (i + 1, j + 1, 7));
	return (a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz;
}
static int by_d (const void *a, const void *b)	/* nearer the port's centre first; ties by hash */
{ const struct prop *p = a, *q = b; float d = hypotf (p->x, p->z) - hypotf (q->x, q->z); return d < 0 ? -1 : d > 0 ? 1 : (p->h > q->h) - (p->h < q->h); }

/* a root, or one of its four margin probes, off its own green square or on
 * a road, the apron, a pad or a lake, or in a building's grown box */
static int blocked (int g, float x, float z)
{
	int k, j;
	for (j = 0; j < 5; j++) {
		float px = x + (j == 1) * MARGIN - (j == 2) * MARGIN, pz = z + (j == 3) * MARGIN - (j == 4) * MARGIN;
		if (!in_poly (&sh[g], px, pz)) return 1;
		for (k = 0; k < nsh; k++) if (!sh[k].green && sh[k].y >= sh[g].y - FLAT && in_poly (&sh[k], px, pz)) return 1;
		for (k = 0; k < nkb; k++) if (px >= kb[k][0] && px <= kb[k][2] && pz >= kb[k][1] && pz <= kb[k][3]) return 1;
	}
	return 0;
}

static void place (void)
{
	int n = 0, g, i, j, k, ng = 0;
	for (g = 0; g < nsh; g++) {
		if (!sh[g].green) continue;
		ng++;
		for (i = (int) ceilf (sh[g].b[0] / CELL); i * CELL <= sh[g].b[2]; i++) for (j = (int) ceilf (sh[g].b[1] / CELL); j * CELL <= sh[g].b[3]; j++) {
			unsigned int h = hsh (i, j, 1);
			float x = (i + .8f * u01 (h) - .4f) * CELL, z = (j + .8f * u01 (mix (h)) - .4f) * CELL;
			if (n == 4096 || vnoise (i / 3.0f, j / 3.0f) < .45f || (P->kind == 2 && u01 (hsh (i, j, 3)) > .3f) || blocked (g, x, z)) continue;
			for (k = 0; k < g && !(sh[k].green && in_poly (&sh[k], x, z)); k++) ;
			if (k < g) continue;	/* a square seen twice: its first copy has it */
			h = hsh (i, j, 2);
			cand[n++] = (struct prop) { x, sh[g].y - .05f * STOREY, z, .7f + .6f * u01 (h), .92f + .16f * u01 (mix (h ^ 1)),
						   6.2832f * u01 (mix (h ^ 2)), (P->kind == 2) * (16 * u01 (mix (h ^ 3)) - 8), 0,
						   P->kind == 2 ? 4 + (h & 1) : u01 (mix (h ^ 4)) < .7f ? (h & 1) : 2 + (h & 1), h, 0 };
		}
	}
	qsort (cand, n, sizeof cand[0], by_d);
	memcpy (pp, cand, (npp = n < NPROPS ? n : NPROPS) * sizeof pp[0]);
	if (logging) fprintf (stderr, "props: freeze: port $%02x %s, h10 %d s %d: %d flat shapes (%d green), %d boxes, %d candidates, %d placed\n",
			      P->word, P->kind == 2 ? "ice" : "terran", P->h10, P->s, nsh, ng, nkb, n, npp);
}

/* one prim of the port, to model space, into the census */
static void measure (const struct prim *q)
{
	float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f }, y0 = 1e30f, y1 = -1e30f, (*v)[3] = &vp[q->v0];
	int k, j, flat;
	struct shp *s;
	for (k = 0; k < q->n; k++) {
		float d[3], *m = v[k];
		for (j = 0; j < 3; j++) d[j] = m[j] - P->o[j];
		for (j = 0; j < 3; j++) m[j] = fsc * (P->R[3 * j] * d[0] + P->R[3 * j + 1] * d[1] + P->R[3 * j + 2] * d[2]);
		y0 = fminf (y0, m[1]), y1 = fmaxf (y1, m[1]);
		b[0] = fminf (b[0], m[0]), b[1] = fminf (b[1], m[2]), b[2] = fmaxf (b[2], m[0]), b[3] = fmaxf (b[3], m[2]);
	}
	if (y0 > CLOUD) return;
	if (!(flat = y1 - y0 < FLAT)) b[0] -= MARGIN, b[1] -= MARGIN, b[2] += MARGIN, b[3] += MARGIN;
	for (k = 0; k < (flat ? nsh : nkb); k++) {	/* seen before: a box, or a shape of its kind */
		float *e = flat ? sh[k].b : kb[k], dy = flat ? fabsf (sh[k].y - y1) + (sh[k].green != green (q->col)) * CLOUD : 0;
		if (dy + fabsf (e[0] - b[0]) + fabsf (e[1] - b[1]) + fabsf (e[2] - b[2]) + fabsf (e[3] - b[3]) < STOREY / 16) return;
	}
	if (!flat) { if (nkb < 512) memcpy (kb[nkb++], b, sizeof b); return; }
	if (nsh == 384) return;
	s = &sh[nsh++];
	s->y = y1, s->green = green (q->col), memcpy (s->b, b, sizeof b);
	for (s->n = k = 0; k < q->n; k += (q->n + SV - 1) / SV, s->n++) s->p[s->n][0] = v[k][0], s->p[s->n][1] = v[k][2];
}

int props_write (void)
{
	int i, j, live = 0;
	float l;
	if (!on) return 0;
	close_shape ();
	frames++;
	/* the port latched keeps its props; another drawn alongside is ignored */
	for (P = NULL, i = 0; i < nrc; i++)
		if (rc[i].kind && (!P || (rc[i].p == key.p && rc[i].word == (int) key.word && rc[i].seed == key.seed))) P = &rc[i];
	for (ptree = NULL, i = 0; P && i < npr && !ptree; i++) if (pr[i].r == P && pr[i].solid) ptree = pr[i].tree;
	if (!P || !ptree) return 0;
	if (P->p != key.p || P->word != (int) key.word || P->seed != key.seed)
		key.p = P->p, key.word = P->word, key.seed = P->seed, key.win = key.frozen = nsh = nkb = npp = 0;
	if (key.frozen && !npp) return 0;	/* nowhere to stand */
	fsc = ldexpf (1, P->h10 - P->s);
	for (j = 0; j < 3; j++) {
		eye[j] = -fsc * (P->R[3 * j] * P->o[0] + P->R[3 * j + 1] * P->o[1] + P->R[3 * j + 2] * P->o[2]);
		L[j] = P->R[3 * j] * P->lt[0] + P->R[3 * j + 1] * P->lt[1] + P->R[3 * j + 2] * P->lt[2];
	}
	l = sqrtf (L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	for (j = 0; j < 3; j++) L[j] = l > 0 ? L[j] / l : 0;
	day = smooth (0, .15f, -L[1]);
	if (!key.frozen) {
		for (i = 0; i < npr; i++) if (pr[i].r == P) measure (&pr[i]);
		if (++key.win == WINDOW) place (), key.frozen = 1;
		return 0;
	}
	if (!kit_built) build_kit ();
	for (i = 0; i < npp; i++) {	/* each prop's z: the engine's for a building at its depth */
		struct prop *p = &pp[i];
		float H = mh[p->v] * p->size * STOREY, m[3] = { p->x, p->y + .5f * H, p->z }, v[3], r = 1.2f * H / fsc;
		for (j = 0; j < 3; j++) v[j] = P->o[j] + (P->R[j] * m[0] + P->R[3 + j] * m[1] + P->R[6 + j] * m[2]) / fsc;
		p->kt = fminf (1, p->kt + 1.0f / 16);	/* fade in over 16 drawn frames, out under a pixel and a half tall */
		p->kp = p->kt * (fpx > 0 ? smooth (.75f, 1.5f, fpx * H / (hypotf (hypotf (p->x - eye[0], p->z - eye[2]), p->y - eye[1]) + 1)) : 1);
		p->zn = v[2] <= 0 || p->kp <= 0 || (fpx > 0 && (fabsf (pj[0] * v[0] - pj[8] * v[2]) - pj[0] * r > v[2] || fabsf (pj[5] * v[1] - pj[9] * v[2]) - pj[5] * r > v[2])) ? 0	/* off screen */
			: v[2] * ZK < 1 ? 1 : (unsigned int) fminf (v[2] * ZK, 4e9f);
		live += p->zn != 0;
	}
	return live ? npp : 0;
}

int props_node (int i, void **tree, unsigned int *z)
{
	if (!on || i < 0 || i >= npp || !pp[i].zn) return 0;
	return *tree = ptree, *z = pp[i].zn, 1;
}

void props_draw (void **data)
{
	int i = *(int *) *data, j, k, t, m = 0, ord[32];
	static int fpx_frame = -1;
	const struct prop *p = &pp[i];
	float w[32][3][3], nn[32][3], dep[32], s, cy, sy, cl, sl;
	*data = (char *) *data + PROPS_BYTES;
	if (!on || !P || i < 0 || i >= npp) return;
	if (fpx_frame != frames) {	/* the 3D view's projection, once a frame, for the next frame's culling */
		GLint vpt[4];
		glGetIntegerv (GL_VIEWPORT, vpt), glGetFloatv (GL_PROJECTION_MATRIX, pj);
		fpx = pj[5] * vpt[3] / 2, fpx_frame = frames;
	}
	if (p->kp <= 0) return;
	s = p->kp * p->size * STOREY, cy = cosf (p->yaw), sy = sinf (p->yaw);
	cl = cosf (p->lean * (float) M_PI / 180), sl = sinf (p->lean * (float) M_PI / 180);
	for (t = 0; t < mnt[p->v]; t++) {
		float g[3] = { 0, 0, 0 }, r[3];
		for (j = 0; j < 4; j++) {	/* three corners, then the normal */
			const float *a = j < 3 ? mt[p->v][t][j] : mn[p->v][t];
			float x = a[0], y = a[1] * cl - a[2] * sl, z = a[1] * sl + a[2] * cl, f = j < 3 ? s : 1;
			r[0] = f * (x * cy - z * sy), r[1] = f * y, r[2] = f * (x * sy + z * cy);
			if (j == 3) memcpy (nn[t], r, sizeof r);
			else for (k = 0; k < 3; k++) w[t][j][k] = r[k] + (k == 0 ? p->x : k == 1 ? p->y : p->z), g[k] += w[t][j][k] / 3;
		}
		if (nn[t][0] * (g[0] - eye[0]) + nn[t][1] * (g[1] - eye[1]) + nn[t][2] * (g[2] - eye[2]) >= 0) continue;	/* back face */
		dep[t] = (g[0] - eye[0]) * (g[0] - eye[0]) + (g[1] - eye[1]) * (g[1] - eye[1]) + (g[2] - eye[2]) * (g[2] - eye[2]);
		for (j = m++; j > 0 && dep[ord[j - 1]] < dep[t]; j--) ord[j] = ord[j - 1];	/* back to front */
		ord[j] = t;
	}
	GLboolean lt = glIsEnabled (GL_LIGHTING), tx = glIsEnabled (GL_TEXTURE_2D), bl = glIsEnabled (GL_BLEND), cf = glIsEnabled (GL_CULL_FACE);
	glDisable (GL_LIGHTING), glDisable (GL_TEXTURE_2D), glDisable (GL_BLEND), glDisable (GL_CULL_FACE);
	glBegin (GL_TRIANGLES);
	for (i = 0; i < m; i++) {	/* lit as the port's own faces: an ambient floor, a diffuse sun */
		int c = mc[p->v][t = ord[i]];
		float amb = .18f + .17f * day, df = fmaxf (0, -(nn[t][0] * L[0] + nn[t][1] * L[1] + nn[t][2] * L[2]));
		float lit = p->tint * (amb + (1 - amb) * df * day) / 15;
		glColor3f (fminf (1, (c >> 8 & 15) * lit), fminf (1, (c >> 4 & 15) * lit), fminf (1, (c & 15) * lit));
		for (j = 0; j < 3; j++) {
			float v[3];
			for (k = 0; k < 3; k++)	/* back to view space: o + R^T m / scale */
				v[k] = P->o[k] + (P->R[k] * w[t][j][0] + P->R[3 + k] * w[t][j][1] + P->R[6 + k] * w[t][j][2]) / fsc;
			glVertex3f (v[0], v[1], -v[2]);
		}
	}
	glEnd ();
	if (lt) glEnable (GL_LIGHTING);
	if (tx) glEnable (GL_TEXTURE_2D);
	if (bl) glEnable (GL_BLEND);
	if (cf) glEnable (GL_CULL_FACE);
}
