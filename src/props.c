/*
 * Scenery round a surface port (props.h; docs/hd/props.md).  Only a
 * surface port's own model frame has an up axis (model +y), a ground (its
 * flat complex shapes) and a metric (its buildings' storeys), so props
 * stand only there.  Every put of a frame is kept in view space; at the
 * frame's end the port's are taken into its model space, the first eight
 * frames measure the port, and then a seeded ring is placed once and for
 * all on its flat ground outside the buildings.  Reads ST memory only.
 */
#include <math.h>
#include <sys/time.h>
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
#include "props.h"

#define A6		__DL5eb6_a6_base
#define A6_FLIGHT_STATE	704	/* byte */
#define A6_BIG_SPACE	14466	/* long: the object slots */
#define OBJ_OFFSETS	__DL44b02	/* word per slot: its offset */
#define STOREY		16384.0f	/* model units (a port's buildings) */
#define PROPS_CELL	3.0f	/* the placement grid, storeys */
#define FLAT		(STOREY / 8)	/* a ground shape's y-range under this */
#define CLOUD		(16 * STOREY)	/* above y_g: clouds, not port */
#define F0		3.0f	/* k_eye: eye distance in Rc, full .. */
#define F1		6.0f	/* .. gone */
#define WINDOW		8	/* drawn frames measured before the freeze */
#define RAMP		(1.0f / 16)	/* k_t step a drawn frame */
#define NPROPS		200
#define MAXV		128	/* vertices kept per prim */
#define SV		64	/* per stored ground shape */

static int on, logging = -1, frames, gather;
static int ok (unsigned int a, unsigned int len) { return a >= 0x400 && a <= MEM_SIZE && len <= MEM_SIZE - a; }
static int rw (unsigned int a) { return (short) STMemory_ReadWord (a); }
static unsigned int rl (unsigned int a) { return STMemory_ReadLong (a); }
static float clampf (float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float smooth (float a, float b, float x) { x = clampf ((x - a) / (b - a)); return x * x * (3 - 2 * x); }

/* --- the frame's records and puts, in view space -------------------- */
struct rec { unsigned int p, seed, bseed; int word, kind, s, h10, body; float R[9], o[3], lt[3]; };
struct prim { struct rec *r; int n, solid, col, v0; unsigned int z; void *tree; };
static struct rec rc[48];
static struct prim pr[2048], *cur;
static float vp[32768][3];
static int nrc, npr, nvp, shape_open, shape_col, overflow;
static unsigned int shape_st;

/* the object's own frame from a part's (as shadows.c's top_frame) */
static unsigned int top_frame (unsigned int f, unsigned int rec)
{
	int d;
	for (d = 0; d < 8 && ok (f, 4); d++) {
		unsigned int up = rl (f);
		if (up == rec) return f;
		if (up <= f || !ok (up - 212, 4) || rl (up - 212) != rec) return 0;
		f = up;
	}
	return 0;
}

/* the record drawn at a6: kind 1 a surface port on a terran body, 2 on an
 * ice body, 0 anything else, -1 unreadable; one look per record a frame */
static struct rec *rec_of (unsigned int a6)
{
	unsigned int p, f, b, big, gd;
	struct rec *r;
	int j, i;

	if (!ok (a6 - 212, 4) || !ok (p = rl (a6 - 212), 286)) return NULL;
	for (j = nrc - 1; j >= 0; j--) if (rc[j].p == p) return &rc[j];
	if (nrc == 48) return NULL;
	r = memset (&rc[nrc++], 0, sizeof *r);
	r->p = p;
	r->kind = -1;
	if (!(f = top_frame (a6, p)) || !ok (f - 212, 212) || !ok (a6 - 198, 6)) return r;
	r->word = STMemory_ReadWord (p + 90) & 0xffff;
	r->seed = rl (p + 118);
	r->s = rw (f - 208);
	for (j = 0; j < 9; j++) r->R[j] = rw (f - 36 + 2 * j) / 32768.0f;
	for (j = 0; j < 3; j++) r->o[j] = (float) (int) rl (f - 16 + 4 * j), r->lt[j] = rw (a6 - 198 + 2 * j);
	r->kind = 0;
	if (r->word < 90 || r->word > 108 || (r->word & 1) || !(STMemory_ReadByte (p + 280) & 16)) return r;
	/* the body it stands on: its parent slot (as hdback.c's obj_ptr) */
	big = rl (A6 + A6_BIG_SPACE);
	i = STMemory_ReadByte (p + 68) & 0xff;
	if (i >= 115 || !ok (big, 4) || !ok (b = big + (short) rw (OBJ_OFFSETS + 2 * i), 286)) return r;
	r->body = STMemory_ReadWord (b + 90) & 0xffff;
	r->bseed = rl (b + 118);
	gd = rl (__DL973c_game_data);
	if (!ok (gd + r->word, 2) || !ok (gd + (STMemory_ReadWord (gd + r->word) & 0xffff) + 10, 2)) return r;
	r->h10 = rw (gd + (STMemory_ReadWord (gd + r->word) & 0xffff) + 10);
	if (r->h10 < -4 || r->h10 > 24 || r->s < -8 || r->s > 24) return r;
	r->kind = r->body == 0x8c ? 2 : r->body == 0x88 || r->body == 0x8a || r->body == 0x90;
	return r;
}

static void close_shape (void)
{
	if (cur && (cur->n < 3 || (shape_st <= MEM_SIZE - 2 && (STMemory_ReadWord (shape_st) & 0xffff) != 0x2a)))
		npr--, nvp = cur->v0;	/* too short, or aborted part way */
	cur = NULL;
	shape_open = 0;
}

void props_begin (int hd)
{
	on = hd && props_opt_in && !flat_props;
	if (!on) return;
	if (logging < 0) logging = getenv ("FE2_PROPS_LOG") != NULL;
	nrc = npr = nvp = 0;
	cur = NULL;
	shape_open = 0;
}

void props_shape (unsigned int rgb444)
{
	if (!on) return;
	close_shape ();
	shape_open = 1;
	shape_col = rgb444 & 0xfff;
	shape_st = rl (__DL385c8_primitives_end);
}

void props_put (int first, int n, int solid, unsigned int z, void *tree)
{
	struct rec *r;
	struct prim *q;
	unsigned int va;
	int i, k;

	if (!on) return;
	if (solid) close_shape ();
	else if (!shape_open) return;
	r = rec_of (GetReg (REG_A6));
	if (!r || r->kind < 0 || (!r->kind && !gather) || (cur && cur->r != r)) { if (!solid && !cur) shape_open = 0; return; }
	if (!(q = cur)) {
		if (npr == 2048) { overflow++; shape_open = 0; return; }
		q = &pr[npr++];
		*q = (struct prim) { r, 0, solid, solid ? GetReg (REG_D6) & 0xfff : shape_col, nvp, z, tree };
		if (!solid) cur = q;
	}
	for (i = 0; i < n && nvp < 32768 && q->n < MAXV; i++, nvp++, q->n++) {
		if (!ok ((va = GetReg (first + i)) + 4, 12)) break;
		for (k = 0; k < 3; k++) vp[nvp][k] = (float) (int) rl (va + 4 + 4 * k);
	}
	if (solid && q->n < 3) npr--, nvp = q->v0;
}

/* --- the port, measured and then frozen ------------------------------ */
struct shp { float y0, y1, b[4], area; int col, n, comp; float p[SV][2]; };
struct box { float b[4], top; int type; };	/* type 0 hill, 1 water, 2 terrain */
struct prop { float x, y, z, d, kt, size, tint, yaw, lean; int v, set, dead; unsigned int h; };
static struct { unsigned int p, word, seed; int win, frozen; } key;
static struct shp sh[256];
static struct box bx[48];
static struct prop pp[NPROPS];
static int nsh, nbx, npp, clouds_far, last_drew;
static float E, Rc, yg, cloud_base, last_eye;
/* this frame */
static struct rec *P;
static float fsc, eye[3], L[3], day, keye, fpx;
static void *ftree[2];
static unsigned int fz[2];
static int fok[2], drawn[2], tris;
static double t_us;

static int blue (int c) { return (c & 15) > (c >> 8 & 15) && (c & 15) > (c >> 4 & 15); }

/* a view vertex of record r into the port's model space */
static void to_model (const struct rec *r, const float *v, float *m)
{
	float d[3], k = ldexpf (1, P->s - r->s);
	int j;
	for (j = 0; j < 3; j++) d[j] = v[j] * k - P->o[j];
	for (j = 0; j < 3; j++) m[j] = fsc * (P->R[3 * j] * d[0] + P->R[3 * j + 1] * d[1] + P->R[3 * j + 2] * d[2]);
}

static int in_poly (const struct shp *s, float x, float z)
{
	int i, j, c = 0;
	for (i = 0, j = s->n - 1; i < s->n; j = i++)
		if ((s->p[i][1] > z) != (s->p[j][1] > z) &&
		    x < (s->p[j][0] - s->p[i][0]) * (z - s->p[i][1]) / (s->p[j][1] - s->p[i][1]) + s->p[i][0]) c = !c;
	return c;
}

/* where the xz segment a->b first enters box b4, in 0..1; -1 if never */
static float seg_box (float ax, float az, float bx_, float bz, const float *b)
{
	float t0 = 0, t1 = 1, d[2] = { bx_ - ax, bz - az }, a[2] = { ax, az };
	int k;
	for (k = 0; k < 2; k++) {
		float lo = b[k], hi = b[k + 2];
		if (fabsf (d[k]) < 1e-6f) { if (a[k] < lo || a[k] > hi) return -1; continue; }
		float u = (lo - a[k]) / d[k], w = (hi - a[k]) / d[k];
		if (u > w) { float t = u; u = w; w = t; }
		t0 = fmaxf (t0, u), t1 = fminf (t1, w);
		if (t0 > t1) return -1;
	}
	return t0;
}

static int in_box (float x, float z, const float *b) { return x >= b[0] && x <= b[2] && z >= b[1] && z <= b[3]; }

static void add_box (const float *b, float top, int type)
{
	int i, m = -1, cnt = 0;
	float g = type == 2 ? 0 : STOREY, a = (b[2] - b[0]) * (b[3] - b[1]), am = 1e30f;
	for (i = 0; i < nbx; i++) {
		if ((bx[i].type == 2) != (type == 2)) continue;
		cnt++;
		if (bx[i].type == type && b[0] - g >= bx[i].b[0] && b[1] - g >= bx[i].b[1] && b[2] + g <= bx[i].b[2] && b[3] + g <= bx[i].b[3]) {
			bx[i].top = fmaxf (bx[i].top, top);
			return;
		}
		float ai = (bx[i].b[2] - bx[i].b[0]) * (bx[i].b[3] - bx[i].b[1]);
		if (ai < am) am = ai, m = i;
	}
	if (cnt >= (type == 2 ? 16 : 32)) { if (a <= am) return; i = m; } else i = nbx++;
	bx[i] = (struct box) { { b[0] - g, b[1] - g, b[2] + g, b[3] + g }, top, type };
}

/* --- the kit: Claude-authored primitive rows (docs/hd/props.md) ------
 * a part is a frustum of `sides`, radii rb..rt from yb to yt (storeys),
 * at x offset xo, leaning lean degrees about z; a bottom cap when rb > 0
 * and cap is set.  Variants: 0-1 pines, 2-3 shrubs, 4-5 ice spires. */
static const struct part { int v, sides, cap; float rb, rt, yb, yt, xo, lean; int rgb; } kit[] = {
	{ 0, 4, 0, .16f, .10f, -.10f, .90f, 0, 0, 0x642 },	/* pine A: trunk */
	{ 0, 6, 1, 1.10f, 0, .60f, 2.40f, 0, 0, 0x262 },	/* lower tier */
	{ 0, 6, 1, .75f, 0, 1.70f, 3.40f, 0, 0, 0x373 },	/* upper tier */
	{ 1, 4, 0, .14f, .09f, -.10f, .80f, 0, 0, 0x532 },	/* pine B: trunk */
	{ 1, 6, 1, 1.00f, 0, .50f, 2.10f, 0, 0, 0x253 },
	{ 1, 6, 1, .65f, 0, 1.50f, 3.00f, 0, 0, 0x364 },
	{ 2, 6, 0, 0, .55f, -.05f, .25f, 0, 0, 0x463 },	/* shrub A: bicone */
	{ 2, 6, 0, .55f, 0, .25f, .60f, 0, 0, 0x473 },
	{ 3, 6, 0, 0, .45f, -.05f, .20f, 0, 0, 0x552 },	/* shrub B */
	{ 3, 6, 0, .45f, 0, .20f, .50f, 0, 0, 0x562 },
	{ 4, 5, 1, .60f, 0, -.10f, 4.00f, 0, 0, 0xcdf },	/* spire A: cone */
	{ 4, 4, 1, .30f, 0, -.10f, 2.00f, .50f, -18, 0xbcf },	/* leaning shard */
	{ 5, 5, 1, .50f, 0, -.10f, 2.60f, 0, 0, 0xdef },	/* spire B */
	{ 5, 4, 1, .25f, 0, -.10f, 1.40f, -.40f, 22, 0xace },
};
static float mt[6][32][3][3], mn[6][32][3], mh[6];
static int mc[6][32], mnt[6], kit_built;

static void mesh_tri (int v, const float *a, const float *b, const float *c, const float *ctr, int rgb)
{
	float u[3], w[3], n[3], g[3], l;
	int j, t = mnt[v];
	if (t == 32) return;
	for (j = 0; j < 3; j++) u[j] = b[j] - a[j], w[j] = c[j] - a[j], g[j] = (a[j] + b[j] + c[j]) / 3 - ctr[j];
	n[0] = u[1] * w[2] - u[2] * w[1], n[1] = u[2] * w[0] - u[0] * w[2], n[2] = u[0] * w[1] - u[1] * w[0];
	if ((l = sqrtf (n[0] * n[0] + n[1] * n[1] + n[2] * n[2])) < 1e-9f) return;
	if (n[0] * g[0] + n[1] * g[1] + n[2] * g[2] < 0) l = -l, memcpy (mt[v][t][1], c, 12), memcpy (mt[v][t][2], b, 12);
	else memcpy (mt[v][t][1], b, 12), memcpy (mt[v][t][2], c, 12);
	memcpy (mt[v][t][0], a, 12);
	for (j = 0; j < 3; j++) mn[v][t][j] = n[j] / l;
	mc[v][t] = rgb;
	mnt[v]++;
}

static void build_kit (void)
{
	unsigned int i;
	int k, j;
	for (i = 0; i < sizeof kit / sizeof kit[0]; i++) {
		const struct part *q = &kit[i];
		float ring[2][6][3], ap[2][3], ctr[3], c = cosf (q->lean * (float) M_PI / 180), s = sinf (q->lean * (float) M_PI / 180);
		for (k = 0; k < q->sides; k++) for (j = 0; j < 2; j++) {
			float a = 2 * (float) M_PI * (k + .5f) / q->sides, r = j ? q->rt : q->rb, y = j ? q->yt : q->yb;
			ring[j][k][0] = q->xo + r * cosf (a), ring[j][k][1] = y, ring[j][k][2] = r * sinf (a);
		}
		ap[0][0] = ap[1][0] = q->xo, ap[0][1] = q->yb, ap[1][1] = q->yt, ap[0][2] = ap[1][2] = 0;
		/* lean about z through the part's foot */
		for (k = 0; k < 2 * 6 + 2; k++) {
			float *p = k < 12 ? ring[k / 6][k % 6] : ap[k - 12], x = p[0] - q->xo, y = p[1] - q->yb;
			p[0] = q->xo + x * c - y * s, p[1] = q->yb + x * s + y * c;
		}
		for (j = 0; j < 3; j++) ctr[j] = (ap[0][j] + ap[1][j]) / 2;
		for (k = 0; k < q->sides; k++) {
			int k1 = (k + 1) % q->sides;
			if (q->rb > 0 && q->rt > 0) mesh_tri (q->v, ring[0][k], ring[0][k1], ring[1][k1], ctr, q->rgb),
						    mesh_tri (q->v, ring[0][k], ring[1][k1], ring[1][k], ctr, q->rgb);
			else mesh_tri (q->v, q->rb > 0 ? ring[0][k] : ring[1][k], q->rb > 0 ? ring[0][k1] : ring[1][k1], ap[q->rb > 0], ctr, q->rgb);
			if (q->cap && q->rb > 0 && k > 1) mesh_tri (q->v, ring[0][0], ring[0][k - 1], ring[0][k], ctr, q->rgb);
		}
		mh[q->v] = fmaxf (mh[q->v], fmaxf (ap[1][1], ring[1][0][1]));
	}
	kit_built = 1;
}

/* --- placement, once at the freeze ------------------------------------ */
static unsigned int mix (unsigned int h) { h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16; return h; }
static unsigned int hsh (int i, int j, unsigned int salt) { return mix (mix (mix (key.seed ^ salt) ^ P->bseed) + (unsigned int) i * 0x9e3779b1u) ^ mix ((unsigned int) j * 0x85ebca77u); }
static float u01 (unsigned int h) { return (h >> 8) / 16777216.0f; }
static float vnoise (float x, float z)
{
	int i = (int) floorf (x), j = (int) floorf (z);
	float fx = smooth (0, 1, x - i), fz = smooth (0, 1, z - j);
	float a = u01 (hsh (i, j, 7)), b = u01 (hsh (i + 1, j, 7)), c = u01 (hsh (i, j + 1, 7)), d = u01 (hsh (i + 1, j + 1, 7));
	return (a + (b - a) * fx) * (1 - fz) + (c + (d - c) * fx) * fz;
}
static int by_d (const void *a, const void *b)
{
	const struct prop *p = a, *q = b;
	return p->d < q->d ? -1 : p->d > q->d ? 1 : p->h < q->h ? -1 : p->h > q->h;
}

static struct prop cand[4096];
static void place (void)
{
	int n = 0, i, j, k, m = (int) ceilf (2.5f * Rc / (PROPS_CELL * STOREY)), far = 0;
	for (k = 0; k < nsh; k++) for (i = 0; i < sh[k].n && sh[k].y1 - sh[k].y0 < FLAT && !blue (sh[k].col); i++)
		far |= hypotf (sh[k].p[i][0], sh[k].p[i][1]) > 1.15f * Rc;
	if (!far) m = -1;	/* nothing to stand on beyond the disc */
	for (i = -m; i <= m; i++) for (j = -m; j <= m; j++) {
		unsigned int h;
		if (hypotf (i, j) * PROPS_CELL * STOREY < 1.15f * Rc - PROPS_CELL * STOREY) continue;	/* inside the disc */
		h = hsh (i, j, 1);
		float x = (i + .8f * u01 (h) - .4f) * PROPS_CELL * STOREY, z = (j + .8f * u01 (mix (h)) - .4f) * PROPS_CELL * STOREY;
		float d = sqrtf (x * x + z * z), y = -1e30f;
		int top = -1;
		if (d < 1.15f * Rc || d > 2.5f * Rc || vnoise (i / 3.0f, j / 3.0f) < 0.45f || n == 4096) continue;
		for (k = 0; k < nsh; k++)	/* the topmost ground shape under it */
			if (sh[k].y1 - sh[k].y0 < FLAT && in_box (x, z, sh[k].b) && in_poly (&sh[k], x, z) && sh[k].y1 > y) y = sh[k].y1, top = k;
		if (top < 0 || blue (sh[top].col)) continue;
		for (k = 0; k < nbx; k++) if (in_box (x, z, bx[k].b) || (bx[k].type == 0 && seg_box (0, 0, x, z, bx[k].b) >= 0)) break;
		if (k < nbx) continue;
		h = hsh (i, j, 2);
		cand[n++] = (struct prop) { x, y - .05f * STOREY, z, d, 0, .7f + .6f * u01 (h), .92f + .16f * u01 (mix (h ^ 1)),
					   6.2832f * u01 (mix (h ^ 2)), (P->kind == 2) * (16 * u01 (mix (h ^ 3)) - 8),
					   P->kind == 2 ? 4 + (h & 1) : u01 (mix (h ^ 4)) < .7f ? (h & 1) : 2 + (h & 1), 0, 0, h };
	}
	qsort (cand, n, sizeof cand[0], by_d);
	npp = n < NPROPS ? n : NPROPS;
	memcpy (pp, cand, npp * sizeof pp[0]);
	if (logging) fprintf (stderr, "props: freeze: E %.2f Rc %.2f storeys, y_g %.0f, %d ground shapes, %d boxes, %d candidates, %d placed%s\n",
			      E / STOREY, Rc / STOREY, yg, nsh, nbx, n, npp, npp ? "" : " (no flat port ground beyond the disc)");
}

/* --- each frame: measure, freeze, fade --------------------------------- */
static struct { float y0, y1, b[4], r; } pm[2048];
static double now_us (void) { struct timeval t; gettimeofday (&t, NULL); return t.tv_sec * 1e6 + t.tv_usec; }
static float area (const float (*p)[3], int n) { float a = 0; int i; for (i = 0; i < n; i++) a += p[i][0] * p[(i + 1) % n][2] - p[(i + 1) % n][0] * p[i][2]; return fabsf (a) / 2; }
#define COMPANION(w)	((w) == 32)	/* the pad: inside the clear zone */
#define CLOUDS(w)	((w) == 198)

static void store (const struct prim *q, int i)
{
	int k, st = (q->n + SV - 1) / SV;
	struct shp *s;
	const float (*v)[3] = (const float (*)[3]) vp[q->v0];
	for (k = 0; k < nsh; k++)
		if (sh[k].col == q->col && fabsf (sh[k].y0 - pm[i].y0) < STOREY / 64 && fabsf (sh[k].b[0] - pm[i].b[0]) < STOREY / 64 &&
		    fabsf (sh[k].b[1] - pm[i].b[1]) < STOREY / 64 && fabsf (sh[k].b[2] - pm[i].b[2]) < STOREY / 64) return;
	if (nsh == 256) return;
	s = &sh[nsh++];
	s->y0 = pm[i].y0, s->y1 = pm[i].y1, s->col = q->col, s->comp = q->r != P, s->area = area (v, q->n);
	memcpy (s->b, pm[i].b, sizeof s->b);
	for (s->n = k = 0; k < q->n; k += st, s->n++) s->p[s->n][0] = v[k][0], s->p[s->n][1] = v[k][2];
}

static void census_log (int ng, int ns, void *gt, void *bt);

int props_write (void)
{
	double t0;
	struct rec *other = NULL;
	int i, j, k, mask = 0, ng = 0, ns = 0;
	float sr = 0, pad = 0, fyg, ba = 0, ob[4] = { 1e30f, 1e30f, -1e30f, -1e30f }, ed;
	void *gt = NULL, *bt = NULL, *ct = NULL;
	unsigned int gz = 0, bz = 0, bzh = 0;

	if (!on) return 0;
	t0 = logging ? now_us () : 0;
	close_shape ();
	frames++;
	fok[0] = fok[1] = 0;
	for (P = NULL, i = 0; i < nrc; i++)
		if (rc[i].kind > 0 && rc[i].p == key.p && rc[i].word == (int) key.word && rc[i].seed == key.seed) P = &rc[i];
	for (i = 0; i < nrc; i++) if (rc[i].kind > 0 && &rc[i] != P) { if (!P) P = &rc[i]; else if (!other) other = &rc[i]; }
	gather = P != NULL;
	if (!P) {
		if (logging && last_drew) fprintf (stderr, "props: frame %d: the port stopped drawing, last eye %.2f Rc\n", frames, last_eye);
		last_drew = 0;
		return 0;
	}
	if (P->p != key.p || P->word != (int) key.word || P->seed != key.seed) {
		key.p = P->p, key.word = P->word, key.seed = P->seed, key.win = key.frozen = 0;
		nsh = nbx = npp = clouds_far = 0;
		E = Rc = yg = 0;
		cloud_base = 1e30f;
	}
	fsc = ldexpf (1, P->h10 - P->s);
	for (j = 0; j < 3; j++) {
		eye[j] = -fsc * (P->R[3 * j] * P->o[0] + P->R[3 * j + 1] * P->o[1] + P->R[3 * j + 2] * P->o[2]);
		L[j] = P->R[3 * j] * P->lt[0] + P->R[3 * j + 1] * P->lt[1] + P->R[3 * j + 2] * P->lt[2];
	}
	ed = sqrtf (L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	for (j = 0; j < 3; j++) L[j] = ed > 0 ? L[j] / ed : 0;
	day = smooth (0, .15f, -L[1]);
	/* every kept put into the port's model space */
	for (i = 0; i < npr; i++) {
		struct prim *q = &pr[i];
		pm[i].y0 = pm[i].b[0] = pm[i].b[1] = 1e30f, pm[i].y1 = pm[i].b[2] = pm[i].b[3] = pm[i].r = -1e30f;
		for (k = q->v0; k < q->v0 + q->n; k++) {
			float m[3];
			to_model (q->r, vp[k], m);
			memcpy (vp[k], m, sizeof m);
			pm[i].y0 = fminf (pm[i].y0, m[1]), pm[i].y1 = fmaxf (pm[i].y1, m[1]);
			pm[i].b[0] = fminf (pm[i].b[0], m[0]), pm[i].b[1] = fminf (pm[i].b[1], m[2]);
			pm[i].b[2] = fmaxf (pm[i].b[2], m[0]), pm[i].b[3] = fmaxf (pm[i].b[3], m[2]);
		}
		if (q->r == P && !q->solid && pm[i].y1 - pm[i].y0 < FLAT && !blue (q->col) && area ((const float (*)[3]) vp[q->v0], q->n) > ba)
			ba = area ((const float (*)[3]) vp[q->v0], q->n), fyg = pm[i].y1;
	}
	if (key.frozen || ba == 0) fyg = yg;
	for (i = 0; i < npr; i++) {
		struct prim *q = &pr[i];
		int mine = q->r == P, flat = pm[i].y1 - pm[i].y0 < FLAT;
		for (k = q->v0; k < q->v0 + q->n; k++)
			if (vp[k][1] <= fyg + CLOUD) pm[i].r = fmaxf (pm[i].r, sqrtf (vp[k][0] * vp[k][0] + vp[k][2] * vp[k][2]));
		if ((mine || CLOUDS (q->r->word)) && pm[i].y0 > fyg + CLOUD) {
			cloud_base = fminf (cloud_base, pm[i].y0);
			if (!ct) ct = q->tree;
			continue;
		}
		if (mine && q->solid) {
			sr = fmaxf (sr, pm[i].r), ns++;
			if (!bt || (q->tree == bt && q->z < bz)) bt = q->tree, bz = q->z;
			if (q->tree == bt && q->z > bzh) bzh = q->z;
		} else if (mine && flat) {
			ng++;
			if (!gt || (q->tree == gt && q->z < gz)) gt = q->tree, gz = q->z;
		} else if (COMPANION (q->r->word)) pad = fmaxf (pad, pm[i].r);
		if (q->r == other) for (k = 0; k < 2; k++) ob[k] = fminf (ob[k], pm[i].b[k]), ob[k + 2] = fmaxf (ob[k + 2], pm[i].b[k + 2]);
		if (!mine || q->solid) continue;
		if (!key.frozen) store (q, i);
		else if (flat && blue (q->col)) add_box (pm[i].b, pm[i].y1, 1);
		else if (!flat) add_box (pm[i].b, pm[i].y1, (pm[i].b[2] - pm[i].b[0]) * (pm[i].b[3] - pm[i].b[1]) <= (float) M_PI * 4.9275f * Rc * Rc / 4 ? 0 : 2);
		else if (pm[i].r > 0 && (pm[i].b[2] - pm[i].b[0]) * (pm[i].b[3] - pm[i].b[1]) <= 4 * E * E) pad = fmaxf (pad, pm[i].r);
	}
	if (ct && ct == gt) clouds_far = 1;
	if (logging && key.win == WINDOW - 1 && !key.frozen) census_log (ng, ns, gt, bt);
	if (!key.frozen) {
		E = fmaxf (E, sr);
		Rc = fmaxf (Rc, fmaxf (sr, pad));
		if (++key.win < WINDOW) goto out;
		/* the freeze: y_g, the clear zone, the boxes, the ring */
		for (ba = 0, i = 0; i < nsh; i++) {
			float a = 0, r = .5f * hypotf (sh[i].b[2] - sh[i].b[0], sh[i].b[3] - sh[i].b[1]);
			if (sh[i].y1 - sh[i].y0 >= FLAT || blue (sh[i].col)) continue;
			for (k = 0; k < nsh; k++) if (fabsf (sh[k].y1 - sh[i].y1) < FLAT && sh[k].y1 - sh[k].y0 < FLAT && !blue (sh[k].col)) a += sh[k].area;
			if (a > ba) ba = a, yg = sh[i].y1;
			if (r <= E) for (k = 0; k < sh[i].n; k++) Rc = fmaxf (Rc, hypotf (sh[i].p[k][0], sh[i].p[k][1]));
		}
		Rc *= 1.15f;
		for (i = 0; i < nsh; i++)
			if (sh[i].y1 - sh[i].y0 < FLAT ? blue (sh[i].col) : sh[i].y0 <= yg + CLOUD)
				add_box (sh[i].b, sh[i].y1, sh[i].y1 - sh[i].y0 < FLAT ? 1 :
					 (sh[i].b[2] - sh[i].b[0]) * (sh[i].b[3] - sh[i].b[1]) <= (float) M_PI * 4.9275f * Rc * Rc / 4 ? 0 : 2);
		place ();
		key.frozen = 1;
	} else Rc = fmaxf (Rc, 1.15f * fmaxf (sr, pad));
	/* the fade: distance, then each prop's ramp */
	ed = sqrtf (eye[0] * eye[0] + (eye[1] - yg) * (eye[1] - yg) + eye[2] * eye[2]);
	last_eye = Rc > 0 ? ed / Rc : 0;
	keye = 1 - smooth (F0, F1, last_eye);
	for (i = 0; i < npp; i++) {
		struct prop *p = &pp[i];
		float rr = 1.2f * p->size * STOREY, dx = p->x - eye[0], dz = p->z - eye[2], top = p->y + mh[p->v] * p->size * STOREY;
		float t = clampf (-(eye[0] * dx + eye[2] * dz) / (dx * dx + dz * dz + 1e-9f));
		int hid = 0;
		if (hypotf (p->x, p->z) < Rc || (other && hypotf (p->x - (ob[0] + ob[2]) / 2, p->z - (ob[1] + ob[3]) / 2) < .575f * hypotf (ob[2] - ob[0], ob[3] - ob[1]))) p->dead = 1;
		for (k = 0; k < nbx && !p->dead; k++) {
			float te;
			if (in_box (p->x, p->z, bx[k].b)) p->dead = 1;
			else if (bx[k].type == 0 && (te = seg_box (eye[0], eye[2], p->x, p->z, bx[k].b)) > 0 && eye[1] + te * (top - eye[1]) < bx[k].top) hid = 1;
		}
		p->set = hypotf (eye[0] + t * dx, eye[2] + t * dz) < Rc + rr;	/* 1: the segment crosses the disc: far */
		p->set = !p->set;	/* sets: 0 far, 1 near */
		p->d = sqrtf (dx * dx + dz * dz + (p->y - eye[1]) * (p->y - eye[1]));
		if (p->dead || hid || (eye[1] > cloud_base && (clouds_far || p->set))) p->kt = fmaxf (0, p->kt - RAMP);
		else p->kt = fminf (1, p->kt + RAMP);
		if (p->kt > 0 && keye > 0) mask |= 1 << p->set;
	}
	ftree[0] = gt, fz[0] = gz - 1, fok[0] = gt && gz > 0;
	if (gt && gt == bt && bzh >= gz) fz[0] = bzh + 1, fok[0] = bzh + 1 != 0;	/* interleaved: before every building */
	ftree[1] = bt, fz[1] = bz - 1, fok[1] = bt && bz > 0;
	mask &= fok[0] | fok[1] << 1;
out:
	last_drew = 1;
	if (logging) {
		t_us += now_us () - t0;
		if (frames % 30 == 0) {
			fprintf (stderr, "props: frame %d: eye %.2f Rc, %.1f storeys up, k_eye %.2f, day %.2f, props far %d near %d, %d tris, write %.1f us; port drew, %s, cloud base %.1f storeys%s\n",
				 frames, last_eye, (eye[1] - yg) / STOREY, keye, day, drawn[0], drawn[1], tris, t_us / 30,
				 key.frozen ? "frozen" : "measuring", cloud_base < 1e29f ? (cloud_base - yg) / STOREY : -1, overflow ? ", OVERFLOW" : "");
			t_us = 0;
		}
	}
	drawn[0] = drawn[1] = tris = 0;
	return mask;
}

int props_node (int set, void **tree, unsigned int *z)
{
	if (!on || set < 0 || set > 1 || !fok[set]) return 0;
	*tree = ftree[set], *z = fz[set];
	return 1;
}

static int by_far (const void *a, const void *b) { float x = ((const float *) a)[0], y = ((const float *) b)[0]; return x < y ? 1 : x > y ? -1 : 0; }

void props_draw (void **data)
{
	int set = *(int *) *data, i, j, k, t, n = 0;
	static float order[NPROPS][2];
	GLint vpt[4];
	GLfloat pj[16];

	*data = (char *) *data + PROPS_BYTES;
	if (!on || !P) return;
	if (!kit_built) build_kit ();
	glGetIntegerv (GL_VIEWPORT, vpt);
	glGetFloatv (GL_PROJECTION_MATRIX, pj);
	fpx = pj[5] * vpt[3] / 2;
	for (i = 0; i < npp; i++) if (pp[i].set == set && pp[i].kt > 0) order[n][0] = pp[i].d, order[n++][1] = i;
	qsort (order, n, sizeof order[0], by_far);
	glPushAttrib (GL_ENABLE_BIT | GL_CURRENT_BIT | GL_LIGHTING_BIT | GL_COLOR_BUFFER_BIT);
	glDisable (GL_LIGHTING);
	glDisable (GL_TEXTURE_2D);
	glDisable (GL_BLEND);
	glDisable (GL_CULL_FACE);
	glBegin (GL_TRIANGLES);
	for (i = 0; i < n; i++) {
		const struct prop *p = &pp[(int) order[i][1]];
		float H = mh[p->v] * p->size * STOREY, k_ = keye * p->kt * smooth (.75f, 1.5f, fpx * H / (p->d + 1)), s = k_ * p->size * STOREY;
		float cy = cosf (p->yaw), sy = sinf (p->yaw), cl = cosf (p->lean * (float) M_PI / 180), sl = sinf (p->lean * (float) M_PI / 180);
		float w[32][3][3], nn[32][3], dep[32][2];
		int m = 0;
		if (k_ <= 0) continue;
		drawn[set]++;
		for (t = 0; t < mnt[p->v]; t++) {
			float g[3] = { 0, 0, 0 }, r[3];
			for (j = 0; j < 4; j++) {
				const float *a = j < 3 ? mt[p->v][t][j] : mn[p->v][t];
				float x = a[0], y = a[1] * cl - a[2] * sl, z = a[1] * sl + a[2] * cl, f = j < 3 ? s : 1;
				r[0] = f * (x * cy - z * sy), r[1] = f * y, r[2] = f * (x * sy + z * cy);
				if (j == 3) memcpy (nn[m], r, sizeof r);
				else for (k = 0; k < 3; k++) w[m][j][k] = r[k] + (k == 0 ? p->x : k == 1 ? p->y : p->z), g[k] += w[m][j][k] / 3;
			}
			if (nn[m][0] * (g[0] - eye[0]) + nn[m][1] * (g[1] - eye[1]) + nn[m][2] * (g[2] - eye[2]) >= 0) continue;
			dep[m][0] = (g[0] - eye[0]) * (g[0] - eye[0]) + (g[1] - eye[1]) * (g[1] - eye[1]) + (g[2] - eye[2]) * (g[2] - eye[2]);
			dep[m][1] = t;
			m++;
		}
		/* each prop's faces back to front; the triangles in w/nn follow dep's order */
		for (t = 1; t < m; t++) for (j = t; j > 0 && dep[j][0] > dep[j - 1][0]; j--) {
			float d[2], ww[3][3], n3[3];
			memcpy (d, dep[j], 8), memcpy (dep[j], dep[j - 1], 8), memcpy (dep[j - 1], d, 8);
			memcpy (ww, w[j], 36), memcpy (w[j], w[j - 1], 36), memcpy (w[j - 1], ww, 36);
			memcpy (n3, nn[j], 12), memcpy (nn[j], nn[j - 1], 12), memcpy (nn[j - 1], n3, 12);
		}
		for (t = 0; t < m; t++) {
			int c = mc[p->v][(int) dep[t][1]];
			float amb = .18f + .17f * day, df = fmaxf (0, -(nn[t][0] * L[0] + nn[t][1] * L[1] + nn[t][2] * L[2]));
			float lit = p->tint * (amb + (1 - amb) * df * day) / 15;
			glColor3f (fminf (1, (c >> 8 & 15) * lit), fminf (1, (c >> 4 & 15) * lit), fminf (1, (c & 15) * lit));
			for (j = 0; j < 3; j++) {
				float v[3];
				for (k = 0; k < 3; k++)	/* back to view space: o + R^T m / scale */
					v[k] = P->o[k] + (P->R[k] * w[t][j][0] + P->R[3 + k] * w[t][j][1] + P->R[6 + k] * w[t][j][2]) / fsc;
				glVertex3f (v[0], v[1], -v[2]);
			}
			tris++;
		}
	}
	glEnd ();
	glPopAttrib ();
}

/* FE2_PROPS_LOG, once per port: the record, its groups and its companions */
static void census_log (int ng, int ns, void *gt, void *bt)
{
	int i, j, g;
	static const char *gname[] = { "flat complex", "sloped complex", "solid" };
	fprintf (stderr, "props: frame %d: port slot %08x model $%02x (%d) seed %08x parent body $%02x %s, h10 %d s %d, state $%02x, prims %d, ground tree %p, solid tree %p\n",
		 frames, P->p, P->word, P->word, P->seed, P->body, P->kind == 2 ? "ice" : "terran", P->h10, P->s,
		 STMemory_ReadByte (A6 + A6_FLIGHT_STATE) & 0xff, npr, gt, bt);
	for (g = 0; g < 3; g++) {
		float y0 = 1e30f, y1 = -1e30f, r = 0, cx = 0, cz = 0;
		unsigned int z0 = ~0u, z1 = 0;
		int n = 0, nc = 0, cols[8], trees = 0;
		void *t0 = NULL;
		for (i = 0; i < npr; i++) {
			int gi = pr[i].solid ? 2 : pm[i].y1 - pm[i].y0 >= FLAT;
			if (pr[i].r != P || gi != g) continue;
			n++, y0 = fminf (y0, pm[i].y0), y1 = fmaxf (y1, pm[i].y1), r = fmaxf (r, pm[i].r);
			cx += (pm[i].b[0] + pm[i].b[2]) / 2, cz += (pm[i].b[1] + pm[i].b[3]) / 2;
			if (pr[i].z < z0) z0 = pr[i].z;
			if (pr[i].z > z1) z1 = pr[i].z;
			if (!t0) t0 = pr[i].tree; else if (pr[i].tree != t0) trees++;
			for (j = 0; j < nc && cols[j] != pr[i].col; j++) ;
			if (j == nc && nc < 8) cols[nc++] = pr[i].col;
		}
		fprintf (stderr, "props:   %s: %d, y %.2f..%.2f storeys, xz radius %.2f storeys, centre offset %.2f %.2f, tree %p (+%d in others), z %u..%u, colours",
			 gname[g], n, y0 / STOREY, y1 / STOREY, r / STOREY, n ? cx / n / STOREY : 0, n ? cz / n / STOREY : 0, t0, trees, z0, z1);
		for (j = 0; j < nc; j++) fprintf (stderr, " %03x", cols[j]);
		fprintf (stderr, "\n");
	}
	for (j = 0; j < nrc; j++) {
		float y0 = 1e30f, y1 = -1e30f, b[4] = { 1e30f, 1e30f, -1e30f, -1e30f }, dmin = 1e30f;
		unsigned int z0 = ~0u, z1 = 0;
		void *t0 = NULL;
		int n = 0;
		if (&rc[j] == P || rc[j].kind < 0) continue;
		for (i = 0; i < npr; i++) {
			if (pr[i].r != &rc[j]) continue;
			n++, y0 = fminf (y0, pm[i].y0), y1 = fmaxf (y1, pm[i].y1);
			for (g = 0; g < 4; g++) b[g] = g < 2 ? fminf (b[g], pm[i].b[g]) : fmaxf (b[g], pm[i].b[g]);
			dmin = fminf (dmin, hypotf ((pm[i].b[0] + pm[i].b[2]) / 2, (pm[i].b[1] + pm[i].b[3]) / 2));
			if (pr[i].z < z0) z0 = pr[i].z;
			if (pr[i].z > z1) z1 = pr[i].z;
			if (!t0) t0 = pr[i].tree;
		}
		if (n && dmin < 4 * E + 4 * STOREY)
			fprintf (stderr, "props:   companion model $%02x (%d) kind %d s %d: %d prims, tree %p, z %u..%u, x %.2f..%.2f z %.2f..%.2f y %.2f..%.2f storeys\n",
				 rc[j].word, rc[j].word, rc[j].kind, rc[j].s, n, t0, z0, z1, b[0] / STOREY, b[2] / STOREY, b[1] / STOREY, b[3] / STOREY, y0 / STOREY, y1 / STOREY);
	}
}
