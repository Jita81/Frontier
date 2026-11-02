/*
 * Planet surface maps for the GL renderer.
 *
 * How the ST renderer draws a planet surface
 * ------------------------------------------
 *
 * fe2.s L3cd9c_ProjectPlanet does not texture anything: after the planet
 * header (flags + base colours, atmosphere layers) the model carries a
 * byte coded list of surface features (read from L3d3f0 on):
 *
 *	feature := code frac vertex+ 0		polygon (continent, sea...)
 *		 | code|$80 x y z angle		circle (ice cap, crater...)
 *	list	:= feature* 0
 *	vertex	:= x y z			signed bytes, model space
 *		 | x y z 0 r			bounding circle, not drawn
 *
 * Every feature is projected onto the planet (L3da98_ProjectPlanetCoord),
 * turned into a 2D outline and scan converted with an even-odd rule where
 * each edge XORs its feature 'code' into the current colour index
 * (L35b28_DrawPlanet): the colour of a point is the XOR of the codes of
 * all the features containing it, so a lake inside a continent can give
 * the sea colour back. The index selects one of 16 colours prepared in
 * 0-30(a3), the base colours in the model plus the light's tints.
 *
 * Day and night work the same way (end of L3d3f0): when bit 7 of the
 * model flags is set the terminator is drawn as one more 'feature', a
 * great circle perpendicular to the light, XORing $20 over the sunward
 * hemisphere. Bit 6 adds a small circle 75.5 degrees from the sub-solar
 * point, and bit 5 another one at 82.8 degrees, giving the characteristic
 * bands of decreasing brightness towards the terminator. psurf_light_code
 * gives the code those circles XOR in for a given sun angle.
 *
 * Polygon edges are not straight: when it is on screen, an edge is split
 * recursively (L3de2e) and each midpoint is pushed around by a random
 * fraction of the planet's model axes, which is what makes the coast
 * lines fractal. The random numbers come from a simple generator seeded
 * by the planet object (118(object)): each edge takes the next value, and
 * each subdivision pushes and restores the generator state, so the
 * displacement of a given midpoint only depends on its position in the
 * subdivision tree and not on how far the tree was expanded. The ST only
 * expands it as far as the screen resolution needs, we expand it as far
 * as the texture resolution needs, and get the very same coast lines.
 *
 * The magnitude of the displacements works out independent of the view:
 * the level L midpoint of an edge is displaced by 32767 >> d6 along each
 * chosen axis, relative to endpoints of length R_units, with
 *	d6 = frac + s + L - 6 - detail/2	(+1 at random)
 *	R_units = m << 16 >> s
 * where m is the normalized radius word of the model and s the display
 * scale shift (192(a3)), which cancels out. We simply use s = 16.
 *
 * What this file does
 * -------------------
 *
 * psurf_get parses the feature list once per planet. psurf_raster then
 * rasterizes the XOR'ed feature codes over any square gnomonic patch of
 * the sphere (a cube face for the whole planet seen from space, or a
 * small patch around the point under the camera when flying low), using
 * the same even-odd XOR rule as the ST. In a gnomonic projection great
 * circle arcs are straight lines, so the (subdivided) edges are exact
 * line segments there.
 *
 * Which side of a spherical polygon is 'inside' is not defined by the
 * outline alone. The ST takes whatever encloses the screen projection;
 * we take the side that does not contain the point opposite the feature's
 * centre, which is the same thing for any feature smaller than a
 * hemisphere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../m68000.h"
#include "planet.h"
/* label addresses, for the W1 body lookup only: the planet tests build
 * this file without the game data, and then the lookup finds no body */
#if defined (__has_include)
#if __has_include ("../fixups.h")
#include "../fixups.h"
#endif
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* safety net only: the texel size stops the recursion long before */
#define MAX_LEVEL	28
#define MAX_POLYS	256
#define MAX_POLY_VERTS	256
/* S-11: hard limits. A feature list longer than this many bytes is cut
 * there (the largest real ones are a few hundred bytes); an outline that
 * needs more leaves than this gives up on the patch. */
#define PSURF_MAX_BYTES		(64 << 10)
#define PSURF_MAX_LEAVES	(1 << 20)

struct PSPoly {
	int code;
	int frac;		/* 0: no fractal displacement */
	int circle;
	/* circle: points are coff + crad * (unit vector perpendicular to cdir) */
	double cdir[3], coff[3], crad;
	/* polygon: unit vertices and the generator state of each edge */
	int n;
	double (*v)[3];
	unsigned int *state;
	/* a point outside the feature, see the header comment */
	double ref[3];
};

struct PInfo;

struct PSurf {
	/* W1 surface classes, NULL until asked for */
	struct PInfo *info;
	unsigned int feat_addr, seed;
	int radius_word, detail;
	/* normalized radius mantissa, 0x4000..0x7fff */
	int m;
	int npolys;
	struct PSPoly polys[MAX_POLYS];
};

/* ---- small vector helpers ------------------------------------------- */

static inline double dot3 (const double a[3], const double b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static inline void cross3d (const double a[3], const double b[3], double o[3])
{
	o[0] = a[1]*b[2] - a[2]*b[1];
	o[1] = a[2]*b[0] - a[0]*b[2];
	o[2] = a[0]*b[1] - a[1]*b[0];
}

static inline double norm3 (double a[3])
{
	double l = sqrt (dot3 (a, a));
	if (l > 0.0) { a[0] /= l; a[1] /= l; a[2] /= l; }
	return l;
}

static inline double angle3 (const double a[3], const double b[3])
{
	/* atan2 form: accurate for tiny angles, unlike acos */
	double c[3];
	cross3d (a, b, c);
	return atan2 (sqrt (dot3 (c, c)), dot3 (a, b));
}

/* any unit vector perpendicular to unit vector n */
static void perp3 (const double n[3], double o[3])
{
	double t[3] = { 1.0, 0.0, 0.0 };
	if (fabs (n[0]) > 0.6) { t[0] = 0.0; t[1] = 1.0; }
	cross3d (n, t, o);
	norm3 (o);
}

/* ---- the ST's random number generator (L3ddc4, L3de2e) --------------- */

static inline unsigned int rng_step (unsigned int s)
{
	return s + ((s << 16) | (s >> 16));
}

/* ---- parsing --------------------------------------------------------- */

static inline int rdbyte (unsigned int *p)
{
	return (signed char) STMemory_ReadByte ((*p)++);
}

static void poly_finish (struct PSPoly *p)
{
	double c[3] = { 0.0, 0.0, 0.0 };
	int i;

	for (i = 0; i < p->n; i++) {
		c[0] += p->v[i][0]; c[1] += p->v[i][1]; c[2] += p->v[i][2];
	}
	if (norm3 (c) == 0.0) c[2] = 1.0;
	p->ref[0] = -c[0]; p->ref[1] = -c[1]; p->ref[2] = -c[2];
}

static void add_circle (struct PSurf *s, int code, const double dir[3], int angle_byte,
			unsigned int *state)
{
	struct PSPoly *p;
	/* BAM, 65536 = full turn */
	double a = (double) (angle_byte << 6) * 2.0 * M_PI / 65536.0;
	double sn = sin (a), cs = cos (a);
	double u[3], w[3];
	int i;

	/* L3dc2c: circles smaller than 100 units (planet radius <= 0x4000)
	 * are not drawn, and then do not use random numbers either. The
	 * display radius varies a little with distance; use a typical one. */
	if (sn * s->m * 0.5 * 1.0215 < 100.0) return;
	/* four edges, never displaced (174(a3) is cleared for circles) */
	for (i = 0; i < 4; i++) *state = rng_step (*state);

	if (s->npolys >= MAX_POLYS) return;
	p = &s->polys[s->npolys];
	memset (p, 0, sizeof (*p));
	/* S-11: out of memory: leave the feature out */
	p->v = malloc (4 * sizeof (*p->v));
	if (!p->v) return;
	s->npolys++;
	p->code = code;
	p->circle = 1;
	memcpy (p->cdir, dir, sizeof (p->cdir));
	p->coff[0] = dir[0]*cs; p->coff[1] = dir[1]*cs; p->coff[2] = dir[2]*cs;
	p->crad = sn;
	/* 4 base points, like L3dcbc */
	p->n = 4;
	perp3 (dir, u);
	cross3d (dir, u, w);
	for (i = 0; i < 3; i++) {
		p->v[0][i] = p->coff[i] + sn*u[i];
		p->v[1][i] = p->coff[i] - sn*w[i];
		p->v[2][i] = p->coff[i] - sn*u[i];
		p->v[3][i] = p->coff[i] + sn*w[i];
	}
	/* the side of the circle away from its centre is outside, as long as
	 * the circle is smaller than a hemisphere */
	p->ref[0] = -dir[0]; p->ref[1] = -dir[1]; p->ref[2] = -dir[2];
	if (cs < 0.0) {
		/* larger than a hemisphere: the centre is outside instead */
		memcpy (p->ref, dir, sizeof (p->ref));
	}
}

static void parse (struct PSurf *s)
{
	unsigned int p = s->feat_addr;
	unsigned int state = s->seed;
	double verts[MAX_POLY_VERTS][3];
	int d7;

	/* P-2: no feature list: a plain planet */
	if (!p) return;

	/* S-11: the list comes from 68000 memory: never read more than
	 * PSURF_MAX_BYTES of it (p - feat_addr counts them, wrapping or not) */
	for (;;) {
		if (p - s->feat_addr >= PSURF_MAX_BYTES) break;
		d7 = rdbyte (&p);
		if (d7 == 0) break;

		if (d7 & 0x80) {
			/* l3db7c: circle(s) on the planet surface */
			double dir[3];
			int ang;
			dir[0] = rdbyte (&p);
			dir[1] = rdbyte (&p);
			dir[2] = rdbyte (&p);
			ang = rdbyte (&p) & 0xff;
			if (norm3 (dir) == 0.0) continue;
			if (d7 & 0x40) {
				/* the opposite circle too (both polar caps), first */
				double o[3] = { -dir[0], -dir[1], -dir[2] };
				add_circle (s, d7 & 0x1c, o, ang, &state);
			}
			add_circle (s, d7 & 0x1c, dir, ang, &state);
		} else {
			/* L3d41e: polygon */
			struct PSPoly *poly;
			int frac, n = 0, x, y, z, i;

			frac = rdbyte (&p);
			x = rdbyte (&p);
			for (;;) {
				if (p - s->feat_addr >= PSURF_MAX_BYTES) return;
				y = rdbyte (&p);
				z = rdbyte (&p);
				if (n == 0 && STMemory_ReadByte (p) == 0) {
					/* bounding circle of the feature, used by the ST to
					 * tell whether the camera is inside it */
					p += 2;
					x = rdbyte (&p);
					continue;
				}
				if (n < MAX_POLY_VERTS) {
					verts[n][0] = x; verts[n][1] = y; verts[n][2] = z;
					if (norm3 (verts[n]) > 0.0) n++;
				}
				x = rdbyte (&p);
				if (x == 0) break;
			}
			if (n < 2) {
				state = rng_step (state);
				continue;
			}
			if (s->npolys >= MAX_POLYS) break;
			poly = &s->polys[s->npolys];
			memset (poly, 0, sizeof (*poly));
			poly->v = malloc (n * sizeof (*poly->v));
			poly->state = malloc (n * sizeof (*poly->state));
			if (!poly->v || !poly->state) {
				/* S-11: out of memory: stop here, keep what we have */
				free (poly->v);
				free (poly->state);
				break;
			}
			s->npolys++;
			poly->code = d7 & 0x3c;
			poly->frac = frac;
			poly->n = n;
			memcpy (poly->v, verts, n * sizeof (*poly->v));
			/* one generator step per edge, closing edge included */
			for (i = 0; i < n; i++) {
				poly->state[i] = state;
				state = rng_step (state);
			}
			poly_finish (poly);
		}
	}
}

/* S-9: least recently used goes first, and the cache is twice the GL
 * texture cache (PLANET_TEX_CACHE in gl.c), so that the surfaces of the
 * planets whose textures are kept stay parsed. gl.c never keeps a pointer
 * to a surface: it asks psurf_get by key each time. */
#define PSURF_CACHE	12
static struct PSurf *cache[PSURF_CACHE];
static unsigned int cache_lru[PSURF_CACHE];
static unsigned int cache_clock;

static void psurf_free (struct PSurf *s)
{
	int i;
	free (s->info);
	for (i = 0; i < s->npolys; i++) {
		free (s->polys[i].v);
		free (s->polys[i].state);
	}
	free (s);
}

/*
 * l3cdd8 normalizes the radius word: it doubles it until bit 15 is set,
 * then halves it once. Bit 15 of the word itself is lost by the first
 * doubling, so that is the same as shifting w & 0x7fff up until its top
 * bit is bit 14, at most 14 times. The ST never finishes for 0x8000 (it
 * keeps doubling zero) and skips 0; both give 0x4000 here (S-10).
 */
int psurf_norm_radius (int radius_word)
{
	int m = radius_word & 0x7fff, i;

	if (m == 0) return 0x4000;
	for (i = 0; i < 15 && m < 0x4000; i++) m <<= 1;
	return m;
}

struct PSurf *psurf_get (unsigned int feat_addr, unsigned int seed, int radius_word, int detail)
{
	struct PSurf *s;
	int i, victim = 0;

	for (i = 0; i < PSURF_CACHE; i++) {
		s = cache[i];
		if (s && s->feat_addr == feat_addr && s->seed == seed &&
		    s->radius_word == radius_word && s->detail == detail) {
			cache_lru[i] = ++cache_clock;
			return s;
		}
		if (!cache[i] || (cache[victim] && cache_lru[i] < cache_lru[victim]))
			victim = i;
	}

	s = calloc (1, sizeof (*s));
	/* S-11: the caller draws no surface */
	if (!s) return NULL;
	s->feat_addr = feat_addr;
	s->seed = seed;
	s->radius_word = radius_word;
	s->detail = detail;
	s->m = psurf_norm_radius (radius_word);
	parse (s);

	if (cache[victim]) psurf_free (cache[victim]);
	cache[victim] = s;
	cache_lru[victim] = ++cache_clock;
	return s;
}

unsigned int psurf_features_end (unsigned int feat_addr, unsigned int limit)
{
	unsigned int p = feat_addr;
	int d7, x, y, z, n;

	if (limit <= feat_addr || limit - feat_addr > PSURF_MAX_BYTES) return 0;
	/* the walk parse() makes; every read is at an address below limit */
	for (;;) {
		if (p >= limit) return 0;
		d7 = rdbyte (&p);
		if (d7 == 0) return p;
		if (d7 & 0x80) {
			p += 4;
			continue;
		}
		if (limit - p < 2) return 0;
		p++;
		x = rdbyte (&p);
		n = 0;
		for (;;) {
			/* y, z and the byte after them */
			if (limit - p < 3) return 0;
			y = rdbyte (&p);
			z = rdbyte (&p);
			if (n == 0 && STMemory_ReadByte (p) == 0) {
				p += 2;
				if (p >= limit) return 0;
				x = rdbyte (&p);
				continue;
			}
			if (x || y || z) n++;
			x = rdbyte (&p);
			if (x == 0) break;
		}
	}
}

/* ---- outline generation ---------------------------------------------- */

struct Leaf {
	double a[3], b[3];
};

struct Ctx {
	struct PSurf *s;
	/* patch cone: centre and half angle */
	double c[3], cone;
	/* stop splitting below this arc length */
	double leaf_ang;
	struct Leaf *leaves;
	int nleaves, maxleaves;
	/* S-11: out of memory or past PSURF_MAX_LEAVES */
	int failed;
};

static void emit (struct Ctx *x, const double a[3], const double b[3])
{
	if (x->failed) return;
	if (x->nleaves == x->maxleaves) {
		struct Leaf *nl = NULL;
		int nmax = x->maxleaves ? 2*x->maxleaves : 4096;
		if (nmax > PSURF_MAX_LEAVES) nmax = PSURF_MAX_LEAVES;
		if (nmax > x->maxleaves)
			nl = realloc (x->leaves, nmax * sizeof (struct Leaf));
		if (!nl) {
			x->failed = 1;
			return;
		}
		x->leaves = nl;
		x->maxleaves = nmax;
	}
	memcpy (x->leaves[x->nleaves].a, a, sizeof (x->leaves[0].a));
	memcpy (x->leaves[x->nleaves].b, b, sizeof (x->leaves[0].b));
	x->nleaves++;
}

/* Can an arc from a to b (plus a displacement of up to 'slack' radians)
 * come near the patch? */
static int arc_near_patch (struct Ctx *x, const double a[3], const double b[3],
			   double len, double slack)
{
	double mid[3] = { a[0]+b[0], a[1]+b[1], a[2]+b[2] };
	if (norm3 (mid) == 0.0) return 1;
	return angle3 (mid, x->c) < x->cone + 0.5*len + slack;
}

/* L3de2e: split a polygon edge, with the ST's fractal displacement */
static void split_edge (struct Ctx *x, const struct PSPoly *p,
			const double a[3], const double b[3], unsigned int state, int level)
{
	const struct PSurf *s = x->s;
	double len = angle3 (a, b);
	double slack = 0.0, amp = 0.0, mid[3];
	unsigned int s1, s2;
	int d6pre = 0;

	if (x->failed) return;
	if (p->frac) {
		d6pre = 2*(p->frac + 16 + level) - 12 - s->detail;
		if (d6pre >= 0) {
			/* largest possible displacement here and below it:
			 * sqrt(3) axes, halving at each level */
			amp = 32767.0 / ldexp (1.0, d6pre >> 1);
			slack = 2.0 * 1.7320508 * amp / (2.0 * s->m);
		}
	}

	if (len < x->leaf_ang || level >= MAX_LEVEL ||
	    !arc_near_patch (x, a, b, len, slack)) {
		emit (x, a, b);
		return;
	}

	/* L3ddc4 steps the generator on every edge */
	s1 = rng_step (state);
	s2 = s1;
	mid[0] = s->m * (a[0] + b[0]);
	mid[1] = s->m * (a[1] + b[1]);
	mid[2] = s->m * (a[2] + b[2]);

	if (p->frac && d6pre >= 0) {
		unsigned int d7 = (s1 << 16) | (s1 >> 16);
		unsigned int w = d7 & 0xffff;
		int d6 = d6pre >> 1, axis;
		double step;

		s2 = s1 + d7;
		/* N flag of the swap */
		if (d7 & 0x80000000) d6++;
		step = 32767.0 / ldexp (1.0, d6);
		for (axis = 0; axis < 3; axis++) {
			int bit = (w >> 14) & 1;
			w = (w << 1) & 0xffff;
			if (bit) {
				int neg = (w >> 14) & 1;
				w = (w << 1) & 0xffff;
				/* the rows of the rotation matrix at -36(a6) are the
				 * model axes seen in viewing space */
				mid[axis] += neg ? -step : step;
			}
		}
	}
	if (norm3 (mid) == 0.0) {
		emit (x, a, b);
		return;
	}

	split_edge (x, p, a, mid, s2, level + 1);
	split_edge (x, p, mid, b, rng_step (s2), level + 1);
}

/* circles are split along the small circle, without displacement */
static void split_circle (struct Ctx *x, const struct PSPoly *p,
			  const double a[3], const double b[3], int level)
{
	double len = angle3 (a, b);
	double mid[3], d[3];
	int i;

	if (x->failed) return;
	if (len < x->leaf_ang || level >= MAX_LEVEL ||
	    !arc_near_patch (x, a, b, len, 0.5*len)) {
		emit (x, a, b);
		return;
	}
	for (i = 0; i < 3; i++) d[i] = (a[i] - p->coff[i]) + (b[i] - p->coff[i]);
	if (norm3 (d) == 0.0) {
		emit (x, a, b);
		return;
	}
	for (i = 0; i < 3; i++) mid[i] = p->coff[i] + p->crad * d[i];
	norm3 (mid);
	split_circle (x, p, a, mid, level + 1);
	split_circle (x, p, mid, b, level + 1);
}

/* ---- rasterization ----------------------------------------------------- */

/* Do the great circle arcs p1-p2 and q1-q2 (each shorter than half a
 * circle) cross? */
static int arcs_cross (const double p1[3], const double p2[3],
		       const double q1[3], const double q2[3])
{
	double n1[3], n2[3], X[3], sp[3], sq[3];
	double d1, d2, d3, d4;

	cross3d (p1, p2, n1);
	d1 = dot3 (n1, q1);
	d2 = dot3 (n1, q2);
	if ((d1 > 0.0) == (d2 > 0.0)) return 0;
	cross3d (q1, q2, n2);
	d3 = dot3 (n2, p1);
	d4 = dot3 (n2, p2);
	if ((d3 > 0.0) == (d4 > 0.0)) return 0;
	/* each arc crosses the other's great circle once, at +X or -X */
	cross3d (n1, n2, X);
	sp[0] = p1[0]+p2[0]; sp[1] = p1[1]+p2[1]; sp[2] = p1[2]+p2[2];
	sq[0] = q1[0]+q2[0]; sq[1] = q1[1]+q2[1]; sq[2] = q1[2]+q2[2];
	return (dot3 (X, sp) > 0.0) == (dot3 (X, sq) > 0.0);
}

/* Is direction q inside the (approximated) outline leaves[first..last)? */
static int inside (const struct Leaf *leaves, int first, int last,
		   const double q[3], const double ref[3])
{
	double mid[3], path[3][3];
	int i, j, n = 0;

	/* walk from q to ref, via the midpoint so both arcs stay short */
	mid[0] = q[0] + ref[0]; mid[1] = q[1] + ref[1]; mid[2] = q[2] + ref[2];
	if (norm3 (mid) < 1e-6) perp3 (q, mid);
	memcpy (path[0], q, sizeof (path[0]));
	memcpy (path[1], mid, sizeof (path[1]));
	memcpy (path[2], ref, sizeof (path[2]));

	for (i = first; i < last; i++)
		for (j = 0; j < 2; j++)
			n += arcs_cross (leaves[i].a, leaves[i].b, path[j], path[j+1]);
	return n & 1;
}

struct Seg {
	double x0, y0, x1, y1;
	int code;
};

struct Cross {
	float x;
	int code;
};

static int cmp_cross (const void *a, const void *b)
{
	float xa = ((const struct Cross *) a)->x, xb = ((const struct Cross *) b)->x;
	return (xa > xb) - (xa < xb);
}

struct YCross {
	double y;
	int code;
};

static int cmp_ycross (const void *a, const void *b)
{
	double ya = ((const struct YCross *) a)->y, yb = ((const struct YCross *) b)->y;
	return (ya > yb) - (ya < yb);
}

/* a double to an int in [lo, hi], NaN giving lo (S-21: the cast of an
 * out of range or NaN double is undefined) */
static int clamp_int (double v, int lo, int hi)
{
	if (!(v > lo)) return lo;
	if (v > hi) return hi;
	return (int) v;
}

static int w1_enabled (void);
static void w1_after_raster (struct PSurf *s, const struct PPatch *pt, const unsigned char *codes);

int psurf_raster (struct PSurf *s, const struct PPatch *pt, unsigned char *codes)
{
	struct Ctx x;
	struct Seg *segs = NULL;
	struct Cross *cross = NULL;
	struct YCross *yc = NULL;
	int *row_count = NULL, *row_start = NULL;
	int nsegs = 0, maxsegs = 0, nyc = 0, maxyc = 0;
	int n = pt->n, i, j, k, pi, base_code = 0;
	double texel = 2.0 * pt->half / n;
	double xl = -pt->half - texel, yb = -pt->half - texel;
	double q0[3];
	int *left = NULL;

	if (n <= 0) return -1;
	/* S-11: anything going wrong leaves an empty (code 0) patch */
	memset (codes, 0, (size_t) n * n);
	if (!s) return -1;

	memset (&x, 0, sizeof (x));
	x.s = s;
	memcpy (x.c, pt->c, sizeof (x.c));
	/* the extended square [xl, half]^2 has to fit in the cone */
	x.cone = atan ((pt->half + 2*texel) * 1.4142136) + 0.01;
	/* texel size at the patch centre, the smallest one */
	x.leaf_ang = texel;

	for (i = 0; i < 3; i++) q0[i] = pt->c[i] + xl*pt->u[i] + yb*pt->v[i];
	norm3 (q0);

	for (pi = 0; pi < s->npolys; pi++) {
		const struct PSPoly *p = &s->polys[pi];
		int first = x.nleaves;

		for (i = 0; i < p->n; i++) {
			const double *a = p->v[i], *b = p->v[(i+1) % p->n];
			if (p->circle) split_circle (&x, p, a, b, 0);
			else split_edge (&x, p, a, b, p->state[i], 0);
		}
		if (inside (x.leaves, first, x.nleaves, q0, p->ref))
			base_code ^= p->code;

		/* project what can reach the square into the patch plane */
		for (i = first; i < x.nleaves; i++) {
			const double *a = x.leaves[i].a, *b = x.leaves[i].b;
			double ca = dot3 (a, pt->c), cb = dot3 (b, pt->c);
			struct Seg *sg;

			if (ca < 0.05 || cb < 0.05) continue;
			if (nsegs == maxsegs) {
				int nmax = maxsegs ? 2*maxsegs : 4096;
				struct Seg *ns = realloc (segs, nmax * sizeof (*segs));
				if (!ns) {
					x.failed = 1;
					break;
				}
				segs = ns;
				maxsegs = nmax;
			}
			sg = &segs[nsegs++];
			sg->x0 = dot3 (a, pt->u) / ca;
			sg->y0 = dot3 (a, pt->v) / ca;
			sg->x1 = dot3 (b, pt->u) / cb;
			sg->y1 = dot3 (b, pt->v) / cb;
			sg->code = p->code;
		}
	}
	free (x.leaves);
	if (x.failed) goto fail;

	/* colour at the start of every row: walk up the line x = xl from q0 */
	left = malloc (n * sizeof (int));
	if (!left) goto fail;
	for (i = 0; i < nsegs; i++) {
		struct Seg *sg = &segs[i];
		double y;
		if ((sg->x0 <= xl) == (sg->x1 <= xl)) continue;
		y = sg->y0 + (xl - sg->x0) * (sg->y1 - sg->y0) / (sg->x1 - sg->x0);
		if (y <= yb) continue;
		if (nyc == maxyc) {
			int nmax = maxyc ? 2*maxyc : 256;
			struct YCross *ny = realloc (yc, nmax * sizeof (*yc));
			if (!ny) goto fail;
			yc = ny;
			maxyc = nmax;
		}
		yc[nyc].y = y;
		yc[nyc++].code = sg->code;
	}
	qsort (yc, nyc, sizeof (*yc), cmp_ycross);
	{
		int c = base_code;
		k = 0;
		for (j = 0; j < n; j++) {
			double y = -pt->half + (j + 0.5) * texel;
			while (k < nyc && yc[k].y < y) c ^= yc[k++].code;
			left[j] = c;
		}
	}
	free (yc);
	yc = NULL;

	/* bucket the crossings of every row's centre line, then fill */
	row_count = calloc (n + 1, sizeof (int));
	row_start = malloc ((n + 1) * sizeof (int));
	if (!row_count || !row_start) goto fail;
	for (k = 0; k < 2; k++) {
		for (i = 0; i < nsegs; i++) {
			struct Seg *sg = &segs[i];
			double ylo = sg->y0 < sg->y1 ? sg->y0 : sg->y1;
			double yhi = sg->y0 < sg->y1 ? sg->y1 : sg->y0;
			int j0 = clamp_int (ceil ((ylo + pt->half) / texel - 0.5), 0, n);
			int j1 = clamp_int (floor ((yhi + pt->half) / texel - 0.5), -1, n-1);
			for (j = j0; j <= j1; j++) {
				double y = -pt->half + (j + 0.5) * texel;
				double xc;
				if ((sg->y0 <= y) == (sg->y1 <= y)) continue;
				xc = sg->x0 + (y - sg->y0) * (sg->x1 - sg->x0) / (sg->y1 - sg->y0);
				/* left of the start line: already in left[j] */
				if (xc <= xl) continue;
				if (k == 0) {
					row_count[j]++;
					continue;
				}
				cross[row_start[j]].x = (float) xc;
				cross[row_start[j]++].code = sg->code;
			}
		}
		if (k == 0) {
			int tot = 0;
			for (j = 0; j < n; j++) {
				row_start[j] = tot;
				tot += row_count[j];
			}
			cross = malloc ((tot + 1) * sizeof (*cross));
			if (!cross) goto fail;
		}
	}
	/* row_start[j] now points at the end of row j */
	for (j = 0; j < n; j++) {
		/* row j's crossings run from the previous row's end to its own */
		int start = j ? row_start[j-1] : 0, end = row_start[j];
		int c = left[j], xi = 0;
		unsigned char *row = codes + j*n;

		qsort (cross + start, end - start, sizeof (*cross), cmp_cross);
		for (i = start; i < end; i++) {
			int xe = clamp_int (ceil ((cross[i].x + pt->half) / texel - 0.5), 0, n);
			for (; xi < xe; xi++) row[xi] = c;
			c ^= cross[i].code;
		}
		for (; xi < n; xi++) row[xi] = c;
	}

	free (cross);
	free (row_count);
	free (row_start);
	free (left);
	free (segs);
	/* W1: off unless asked for, and never changes codes */
	if (w1_enabled ()) w1_after_raster (s, pt, codes);
	return 0;

fail:
	memset (codes, 0, (size_t) n * n);
	free (cross);
	free (row_count);
	free (row_start);
	free (left);
	free (yc);
	free (segs);
	return -1;
}

int psurf_light_code (int flags, double cos_sun)
{
	if (!(flags & 0x80)) return 0;
	if (cos_sun <= 0.0) return 0;
	if (!(flags & 0x40)) return 0x20;
	/* small circles 75.5 and 82.8 degrees from the sub-solar point */
	if (flags & 0x20) {
		if (cos_sun > 0.25) return 0x30;
		if (cos_sun > 0.125) return 0x10;
		return 0x20;
	}
	if (cos_sun > 0.25) return 0x10;
	return 0x20;
}

/* ---- W1: surface classes ---------------------------------------------
 *
 * Which parts of a planet are water, land, ice or craters, worked out at
 * run time from the game's own data: the colour each feature code is
 * drawn in (the model header), the kind of body (the model the object is
 * drawn with) and its temperature. See docs/hd/planet-classes.md.
 * Everything here only reads 68000 memory, and nothing here runs unless
 * FE2_DUMP_PLANET is set (FE2_DEBUG_SURFCLASS adds masks to it) or a
 * caller asks for psurf_code_class / psurf_classify. */

#define PS_MEM_SIZE	0x110000	/* host.h MEM_SIZE */
#define PS_OBJ_SIZE	286
#define PS_OBJ_SLOTS	115
#define PS_OBJ_MODEL	90		/* word */
#define PS_OBJ_SEED	118		/* long */
#define PS_OBJ_TEMP	182		/* word: kelvin */
#define PS_A6_SLOTS	14466		/* long: the object slots */
#define PS_MAX_DEBUG_PATCHES	12	/* ground masks per surface */

enum { B_UNKNOWN, B_AIRLESS, B_ROCKY, B_CORROSIVE, B_METHANE, B_TERRAN,
       B_ICE, B_OCEAN, B_GAS, B_STAR };
static const char *const body_name[] = { "unknown", "airless", "rocky",
	"corrosive", "methane", "terran", "ice", "ocean", "gasgiant", "star" };
static const char *const class_name[] = { "other", "water", "land", "ice", "crater" };

struct PInfo {
	unsigned int model, obj;	/* 68k addresses, 0: not found */
	int model_word, body, temp;	/* temp -1: unknown */
	int flags, sel, sel_src;	/* sel_src: 0 none, 1 fixed, 2 object, 3 global */
	int col[16];			/* rgb444 of each code index, -1 unknown */
	unsigned char cls[16];
	int npoly[16], ncirc[16], npair[16];	/* npair: circles with an antipodal twin */
	double area[16];
	int faces;			/* bit per cube face accumulated */
	int dumped, npatch;		/* npatch: ground masks written */
	double last_c[3];		/* centre of the last ground mask */
};

static int w1_env = -1;		/* bit 0: dump, bit 1: debug tint */
static const char *w1_dir;

static int w1_enabled (void)
{
	if (w1_env < 0) {
		const char *d = getenv ("FE2_DUMP_PLANET"), *t = getenv ("FE2_DEBUG_SURFCLASS");
		w1_env = 0;
		/* both write files into the FE2_DUMP_PLANET directory only */
		if (d && *d) {
			w1_dir = d;
			w1_env = 1 | ((t && *t && *t != '0') ? 2 : 0);
		}
	}
	return w1_env;
}

static int in_mem (unsigned int a, unsigned int len)
{
	return a >= 0x400 && a < PS_MEM_SIZE && len <= PS_MEM_SIZE - a;
}
static int rw16 (unsigned int a) { return STMemory_ReadWord (a) & 0xffff; }

static int body_of_model (int m)
{
	switch (m) {
	case 0x7a: case 0x7c: case 0x80: return B_AIRLESS;
	case 0x7e: case 0x82: return B_ROCKY;
	case 0x84: return B_CORROSIVE;
	case 0x86: return B_METHANE;
	case 0x88: case 0x8a: case 0x90: return B_TERRAN;
	case 0x8c: return B_ICE;
	case 0x8e: return B_OCEAN;
	case 0x92: case 0x94: case 0x96: return B_GAS;
	}
	return m >= 0x98 && m <= 0xae ? B_STAR : B_UNKNOWN;
}

/* the feature list of the model at m, the way L3d3f0 skips to it */
static unsigned int features_of_model (unsigned int m)
{
	unsigned int p = m + 8;
	int flags, i;

	if (!in_mem (m, 8)) return 0;
	flags = STMemory_ReadByte (m) & 0xff;
	if (flags & 0x10) p += 58;
	else if (!(flags & 0x40)) p += 8;
	for (i = 0; i < 64; i++) {
		if (!in_mem (p, 2)) return 0;
		if (rw16 (p) == 0) return p + 2;
		p += i ? 4 : 6;
	}
	return 0;
}

/* The body, the model header and the code colours of a surface. */
static struct PInfo *pinfo (struct PSurf *s)
{
	struct PInfo *in;
	unsigned int m;
	int i, k, set = 0, c[8];

	if (s->info) return s->info;
	in = s->info = calloc (1, sizeof (*in));
	if (!in) return NULL;
	in->temp = -1;
	for (k = 0; k < 16; k++) in->col[k] = -1;

#if defined (__DL5eb6_a6_base) && defined (__DL44b02)
	/* the object drawn with this surface: the one with its seed */
	{
		unsigned int big = (unsigned int) STMemory_ReadLong (__DL5eb6_a6_base + PS_A6_SLOTS);
		for (i = 0; in_mem (big, 4) && i < PS_OBJ_SLOTS; i++) {
			unsigned int o = big + (short) rw16 (__DL44b02 + 2 * i);
			int mw;
			if (!in_mem (o, PS_OBJ_SIZE)) continue;
			if ((unsigned int) STMemory_ReadLong (o + PS_OBJ_SEED) != s->seed) continue;
			mw = rw16 (o + PS_OBJ_MODEL);
			if (mw < 0x7a || mw > 0xae) continue;
			in->obj = o;
			in->model_word = mw;
			in->body = body_of_model (mw);
			in->temp = rw16 (o + PS_OBJ_TEMP);
			break;
		}
	}
#endif

	/* the model header just before its feature list */
	for (m = s->feat_addr - 8; s->feat_addr > 8 && m + 400 > s->feat_addr && m >= 0x406; m -= 2)
		if (features_of_model (m) == s->feat_addr &&
		    (rw16 (m - 6) & 0x1f) == 0x1f) { in->model = m; break; }

	for (i = 0; i < s->npolys; i++) {
		const struct PSPoly *p = &s->polys[i];
		int j;
		k = (p->code >> 2) & 15;
		if (!p->circle) { in->npoly[k]++; continue; }
		in->ncirc[k]++;
		for (j = 0; j < s->npolys; j++)
			if (j != i && s->polys[j].circle && s->polys[j].code == p->code &&
			    dot3 (s->polys[j].cdir, p->cdir) < -0.999) { in->npair[k]++; break; }
	}
	if (!in->model) return in;

	m = in->model;
	in->flags = STMemory_ReadByte (m) & 0xff;
	for (i = 0; i < 4; i++) c[i] = rw16 (m + 2*i) & 0xfff;
	if (in->flags & 0x10) {
		/* l3d0be: a selector picks one of 8 colour sets */
		int d5 = (rw16 (m + 8) * 2) & 0x1fe;
		in->sel = rw16 (m + 8);
		if (d5 & 0x100) {
			if (d5 & 0x80) in->sel_src = 3;	/* a global: not known here */
			else {
				in->sel_src = 2;
				set = in->obj ? rw16 (in->obj + 114 + (d5 & 0x7f)) & 7 : -1;
			}
		} else {
			in->sel_src = 1;
			set = (d5 & 0x80) ? 0 : (d5 >> 1) & 7;
		}
		if (in->sel_src == 3 || set < 0) return in;
		if (set)
			for (i = 0; i < 4; i++) c[i] = rw16 (m + 2 + 8*set + 2*i) & 0xfff;
		for (i = 0; i < 4; i++) c[4+i] = c[i];
	} else if (!(in->flags & 0x40)) {
		for (i = 0; i < 4; i++) c[4+i] = rw16 (m + 8 + 2*i) & 0xfff;
	}
	for (k = 0; k < 16; k++)
		in->col[k] = c[(in->flags & 0x40) ? (k & 3) : (k & 7)];
	return in;
}

/*
 * THE RULE. One code in, one class out, from:
 *	the colour the model draws the code in (4 bits a channel),
 *	the kind of body and its temperature in kelvin,
 *	how the code's features are drawn: polygons, circles, and circles
 *	that come as an antipodal pair (the two polar caps).
 * 'other' whenever it cannot be sure.
 */
static int class_rule (int rgb, int body, int temp, int npoly, int ncirc, int npair)
{
	int r = (rgb >> 8) & 15, g = (rgb >> 4) & 15, b = rgb & 15;
	int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	int weather = body == B_TERRAN || body == B_OCEAN || body == B_ICE;
	int bare = body == B_AIRLESS || body == B_ROCKY;

	if (rgb < 0) return PS_OTHER;
	if (!weather && !bare && body != B_CORROSIVE && body != B_METHANE)
		return PS_OTHER;	/* gas giants, stars, unknown bodies */
	if (mx < 2) return PS_OTHER;	/* too dark to tell */

	/* blue clearly above red and green: open water where water is
	 * liquid, sea ice where it is not, unknown elsewhere */
	if (b >= r + 2 && b >= g + 1) {
		if (!weather) return PS_OTHER;
		if (temp < 0 || temp > 380) return PS_OTHER;
		return temp < 240 ? PS_ICE : PS_WATER;
	}
	/* grey or white: snow and ice on a world with weather; on a bare
	 * world rock, except polar caps (ice) and round spots (craters) */
	if (mx - mn <= 1) {
		if (weather) return PS_ICE;
		if (!bare) return PS_OTHER;
		if (ncirc && npair == ncirc) return PS_ICE;
		if (ncirc && ncirc >= npoly) return PS_CRATER;
		return PS_LAND;
	}
	if (bare && ncirc > npair && ncirc - npair >= npoly) return PS_CRATER;
	return PS_LAND;
}

int psurf_code_class (struct PSurf *s, int code)
{
	struct PInfo *in;
	int k = (code >> 2) & 15;

	if (!s || !(in = pinfo (s))) return PS_OTHER;
	return class_rule (in->col[k], in->body, in->temp,
			   in->npoly[k], in->ncirc[k], in->npair[k]);
}

unsigned int psurf_seed (struct PSurf *s)
{
	return s ? s->seed : 0;
}

int psurf_temperate (struct PSurf *s)
{
	struct PInfo *in = s ? pinfo (s) : NULL;
	return in && (in->body == B_TERRAN || in->body == B_OCEAN);
}

/* Two pass 3-4 chamfer from the coast: 3 units a texel, negative in
 * water, positive elsewhere, +-32767 when there is no coast. */
static void coast_distance (const unsigned char *cls, int n, short *d)
{
	static const int dx[4] = { -1, 0, -1, 1 }, dy[4] = { 0, -1, -1, -1 };
	static const int w[4] = { 3, 3, 4, 4 };
	int i, j, k;

	for (j = 0; j < n; j++)
		for (i = 0; i < n; i++) {
			int wet = cls[j*n+i] == PS_WATER, coast = 0;
			if (i > 0 && (cls[j*n+i-1] == PS_WATER) != wet) coast = 1;
			if (i < n-1 && (cls[j*n+i+1] == PS_WATER) != wet) coast = 1;
			if (j > 0 && (cls[(j-1)*n+i] == PS_WATER) != wet) coast = 1;
			if (j < n-1 && (cls[(j+1)*n+i] == PS_WATER) != wet) coast = 1;
			d[j*n+i] = coast ? 1 : 32767;
		}
	for (j = 0; j < n; j++)
		for (i = 0; i < n; i++)
			for (k = 0; k < 4; k++) {
				int x = i + dx[k], y = j + dy[k];
				if (x < 0 || x >= n || y < 0) continue;
				if (d[y*n+x] + w[k] < d[j*n+i]) d[j*n+i] = d[y*n+x] + w[k];
			}
	for (j = n-1; j >= 0; j--)
		for (i = n-1; i >= 0; i--)
			for (k = 0; k < 4; k++) {
				int x = i - dx[k], y = j - dy[k];
				if (x < 0 || x >= n || y >= n) continue;
				if (d[y*n+x] + w[k] < d[j*n+i]) d[j*n+i] = d[y*n+x] + w[k];
			}
	for (i = 0; i < n*n; i++)
		if (cls[i] == PS_WATER) d[i] = -d[i];
}

int psurf_classify (struct PSurf *s, const unsigned char *codes, int n,
		    unsigned char *cls, short *dist)
{
	unsigned char lut[16];
	int k, i;

	if (!s || n <= 0) return -1;
	for (k = 0; k < 16; k++) lut[k] = psurf_code_class (s, k << 2);
	for (i = 0; i < n*n; i++) cls[i] = lut[(codes[i] >> 2) & 15];
	if (dist) coast_distance (cls, n, dist);
	return 0;
}

static void w1_dump (struct PSurf *s, struct PInfo *in)
{
	char path[1024];
	double tot = 0.0;
	FILE *f;
	int k;

	snprintf (path, sizeof (path), "%s/planet-%06x-%08x.txt", w1_dir, s->feat_addr, s->seed);
	f = fopen (path, "r");
	if (f) { fclose (f); return; }
	f = fopen (path, "w");
	if (!f) return;
	for (k = 0; k < 16; k++) tot += in->area[k];
	fprintf (f, "feat 0x%06x seed 0x%08x model 0x%06x obj 0x%06x\n",
		 s->feat_addr, s->seed, in->model, in->obj);
	fprintf (f, "model_word 0x%02x body %s temp %d flags 0x%02x selector 0x%04x src %d\n",
		 in->model_word, body_name[in->body], in->temp, in->flags, in->sel, in->sel_src);
	fprintf (f, "code colour polys circles paired share class\n");
	for (k = 0; k < 16; k++) {
		if (!in->npoly[k] && !in->ncirc[k] && !(in->area[k] > 0.0)) continue;
		char col[8] = "?";
		if (in->col[k] >= 0) snprintf (col, sizeof (col), "0x%03x", in->col[k]);
		fprintf (f, "%2d %5s %3d %3d %3d %6.2f%% %s\n", k, col,
			 in->npoly[k], in->ncirc[k], in->npair[k], tot > 0.0 ? 100.0 * in->area[k] / tot : 0.0,
			 class_name[in->cls[k]]);
	}
	fclose (f);
}

/* the class tint over the model colour, half and half, every second texel */
static void w1_tga (struct PSurf *s, struct PInfo *in, const unsigned char *cls,
		    const unsigned char *codes, int n, const char *tag)
{
	static const unsigned char tint[5][3] = {
		{ 255, 0, 255 }, { 0, 64, 255 }, { 0, 200, 0 }, { 255, 255, 255 }, { 128, 128, 128 } };
	unsigned char hdr[18] = { 0 }, *row;
	char path[1024];
	int m = n / 2, i, j;
	FILE *f;

	snprintf (path, sizeof (path), "%s/surfclass-%06x-%08x-%s.tga", w1_dir, s->feat_addr, s->seed, tag);
	if (!(row = malloc (m * 3))) return;
	if (!(f = fopen (path, "wb"))) { free (row); return; }
	hdr[2] = 2; hdr[12] = m & 255; hdr[13] = m >> 8; hdr[14] = m & 255; hdr[15] = m >> 8;
	hdr[16] = 24; hdr[17] = 0x20;
	fwrite (hdr, 1, 18, f);
	for (j = 0; j < m; j++) {
		for (i = 0; i < m; i++) {
			int t = (2*j)*n + 2*i, c = in->col[(codes[t] >> 2) & 15], q;
			int rgb[3] = { ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17 };
			if (c < 0) rgb[0] = rgb[1] = rgb[2] = 0;
			for (q = 0; q < 3; q++)	/* TGA is BGR */
				row[3*i + 2 - q] = (rgb[q] + tint[cls[t]][q]) / 2;
		}
		fwrite (row, 1, m * 3, f);
	}
	fclose (f);
	free (row);
}

/* After each raster: the shares, the dump, and the debug masks. */
static void w1_after_raster (struct PSurf *s, const struct PPatch *pt, const unsigned char *codes)
{
	static unsigned char *cls;
	static short *dist;
	static int cap;
	struct PInfo *in = pinfo (s);
	int n = pt->n, face = -1, i, k;
	struct timespec t0, t1;
	char tag[16];
	int wrote_face = 0;

	if (!in) return;
	/* a cube face: half 1, centre on an axis */
	if (pt->half == 1.0)
		for (k = 0; k < 3; k++)
			if (fabs (pt->c[k]) == 1.0) face = 2*k + (pt->c[k] < 0.0);
	if (cap < n*n) {
		free (cls); free (dist);
		cls = malloc ((size_t) n*n);
		dist = malloc ((size_t) n*n * sizeof (*dist));
		cap = (cls && dist) ? n*n : 0;
		if (!cap) return;
	}
	clock_gettime (CLOCK_MONOTONIC, &t0);
	psurf_classify (s, codes, n, cls, dist);
	clock_gettime (CLOCK_MONOTONIC, &t1);
	fprintf (stderr, "surfclass: %s %dx%d classes+coast %.2f ms\n", face >= 0 ? "face" : "patch",
		 n, n, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	for (k = 0; k < 16; k++) in->cls[k] = psurf_code_class (s, k << 2);

	if (face >= 0 && !(in->faces & (1 << face))) {
		double h = 2.0 / n;
		in->faces |= 1 << face;
		wrote_face = 1;
		for (i = 0; i < n*n; i++) {
			double x = -1.0 + h * (i % n + 0.5), y = -1.0 + h * (i / n + 0.5);
			in->area[(codes[i] >> 2) & 15] += pow (1.0 + x*x + y*y, -1.5);
		}
		if (in->faces == 63 && (w1_env & 1) && !in->dumped) {
			in->dumped = 1;
			w1_dump (s, in);
		}
	}
	/* masks: each cube face once, then a few ground patches as they move */
	if (!(w1_env & 2)) return;
	if (face >= 0) {
		if (wrote_face) { snprintf (tag, sizeof (tag), "f%d", face); w1_tga (s, in, cls, codes, n, tag); }
	} else if (in->npatch < PS_MAX_DEBUG_PATCHES &&
		   (!in->npatch || dot3 (pt->c, in->last_c) < 0.99999 * sqrt (dot3 (pt->c, pt->c) * dot3 (in->last_c, in->last_c)))) {
		snprintf (tag, sizeof (tag), "p%02d", in->npatch++);
		memcpy (in->last_c, pt->c, sizeof (in->last_c));
		w1_tga (s, in, cls, codes, n, tag);
	}
}
