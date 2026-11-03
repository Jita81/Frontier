/*
 * HD planet surfaces, W3 slice 1: sea depth and beaches.
 *
 * planet.c classes every feature code (water, land, ice, crater, other)
 * from the game's own planet data and measures each texel's distance to
 * the coast. Here that turns into colour: water is lighter and a little
 * turquoise near a coast and darker out at sea, and the land of a
 * temperate world gets a thin sand rim along the shore. The coastline
 * never moves. Every shift is at most a quarter of the texel's own
 * brightness, so the night and twilight zones stay as dark as the game
 * draws them. Bands are set in radians of the surface, so the cube faces
 * and the ground patch agree.
 *
 * W2 slice 1: the land of a terran world takes a biome tone, lush, dry or
 * tundra and rock, from latitude, the body's temperature and a seeded
 * moisture noise.
 *
 * W12 slice 1: an airless body takes a fine seeded regolith speckle and
 * soft bright rays from its own craters, as albedo on every light zone.
 *
 * W4 slice 1: an airless body takes relief in the same gain: each
 * crater a darker floor and a lighter rim just outside it, and the ground
 * between a seeded ridged noise. It is a tone, not lighting, the same on
 * every light zone. Off: --flat-relief.
 * Off, all of these: --flat-biomes.
 * See docs/hd/planet-classes.md.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "planet.h"
#include "worlds.h"

/* Host constants, not ST data: band widths in radians of arc. */
#define W_SHALLOW	0.045	/* shallows to open sea */
#define W_BEACH		0.008	/* about 2 face texels at a face centre */
#define W_SLOTS		48	/* 6 cached planets x 7 rasters, and spare */
#define W_PROBE		512	/* the probe under a fine ground patch */
#define W_BLOCK		32	/* ground patch texels per probe lookup, a side */
#define W_AIRLESS	-2	/* biome_map's temp for the regolith of an airless body */
#define W_RF1		64.0	/* regolith noise octaves, cells a radian */
#define W_RF2		21.0
#define W_RAMP		0.10	/* speckle: the most a texel's brightness moves, a side */
#define W_RDESAT	0.2	/* share of the way to grey */
#define W_RAYS		8	/* rays a crater, at most */
#define W_CRATERS	64
#define W_RBRIGHT	0.2	/* a ray's brightening at the rim */
#define W_GF0		9.0	/* ground relief: ridged octaves, cells a radian (the second is W_RF2) */
#define W_GAMP		0.12	/* ground relief: share of a texel's brightness */
#define W_RIDGE		0.305	/* the mean of |2 x noise - 1|, so the ridges are zero-mean */
#define W_RIM		0.16	/* a crater's rim, lighter */
#define W_FLOOR		0.12	/* its floor, darker */
#define W_GAIN		0.25	/* speckle, rays and relief together, at most a side */
#define W_TEX0		2e-5	/* the relief faded out at this patch texel (radians) and finer, */
#define W_TEX1		2e-3	/* whole at this one and coarser: log-linear between */

/* the most a channel moves, as a share of the texel's brightest channel */
static const float shallow_k[3] = { -0.08f, 0.25f, 0.12f };
static const float deep_k[3] = { -0.10f, -0.12f, -0.08f };
static const float sand[3] = { 1.0f, 0.88f, 0.62f };

/* Per codes buffer, one byte a texel: 0 untouched, 1..128 water from the
 * shallows (1) to open sea (128), 129..255 beach weight. */
static struct WMap {
	const unsigned char *codes;
	unsigned char *w;	/* NULL: nothing to shade per texel */
	unsigned char wn[16];	/* if by_code: the shade byte of each code */
	int by_code;
	unsigned int lru;
} maps[W_SLOTS];
static unsigned int clock_;

static float hash3 (int x, int y, int z, unsigned int seed)
{
	unsigned int h = seed ^ (unsigned) x * 0x8da6b343u ^ (unsigned) y * 0xd8163841u
		^ (unsigned) z * 0xcb1ab31fu;
	h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
	return (h & 0xffff) / 65535.0f;
}

/* value noise, 0..1, on the unit direction d scaled by f; k, if not NULL,
 * keeps the last lattice cell's corner hashes, for a low octave sampled
 * many times a cell (W4) */
struct VCell { int ok, i[3]; unsigned int seed; float h[8]; };

static float vnoise_k (const double d[3], double f, unsigned int seed, struct VCell *c)
{
	double p[3], w[3];
	int i[3], k;
	float v = 0.0f, h[8];

	for (k = 0; k < 3; k++) {
		p[k] = d[k] * f;
		i[k] = (int) floor (p[k]);
		w[k] = p[k] - i[k];
		w[k] = w[k] * w[k] * (3.0 - 2.0 * w[k]);
	}
	if (c && c->ok && c->seed == seed && !memcmp (c->i, i, sizeof (i)))
		memcpy (h, c->h, sizeof (h));
	else {
		for (k = 0; k < 8; k++) h[k] = hash3 (i[0] + (k & 1), i[1] + !!(k & 2), i[2] + !!(k & 4), seed);
		if (c) c->ok = 1, memcpy (c->i, i, sizeof (i)), c->seed = seed, memcpy (c->h, h, sizeof (h));
	}
	for (k = 0; k < 8; k++)
		v += (k & 1 ? w[0] : 1 - w[0]) * (k & 2 ? w[1] : 1 - w[1]) * (k & 4 ? w[2] : 1 - w[2]) * h[k];
	return v;
}

static float vnoise (const double d[3], double f, unsigned int seed)
{
	return vnoise_k (d, f, seed, NULL);
}

static double smooth (double e0, double e1, double x)
{
	double t = (x - e0) / (e1 - e0);
	t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
	return t * t * (3.0 - 2.0 * t);
}

/* texels beyond each edge for the bands to see every coast in reach */
static int need_pad (const struct PPatch *pt)
{
	double p = W_SHALLOW * 1.5 * pt->n / (2.0 * pt->half) * (1.0 + pt->half * pt->half);
	return p > 1e6 ? 1000000 : (int) p + 1;
}

static double wdot (const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The shade bytes of one raster, or NULL when it has no water. The
 * distances come from a raster padded by a margin, so a coast just over
 * a cube face edge still counts. */
static unsigned char *make_map (const unsigned char *codes, const struct PPatch *pt, struct PSurf *s)
{
	static unsigned char *pcodes, *pcls;
	static short *pdist;
	static int cap;
	unsigned char lut[16], *w;
	int n = pt->n, k, x, y, wet = 0, dry = 0, beach;
	double tex = 2.0 * pt->half / n;
	int pad = need_pad (pt), m;
	unsigned int seed = psurf_seed (s);
	struct PPatch pp = *pt;

	for (k = 0; k < 16; k++) {
		lut[k] = psurf_code_class (s, k << 2);
		wet |= lut[k] == PS_WATER;
		dry |= lut[k] == PS_LAND;
	}
	if (!wet) return NULL;
	beach = dry && psurf_temperate (s);
	if (pad > n / 16) pad = n / 16;
	m = n + 2 * pad;
	if (cap < m * m) {
		free (pcodes); free (pcls); free (pdist);
		pcodes = malloc ((size_t) m * m);
		pcls = malloc ((size_t) m * m);
		pdist = malloc ((size_t) m * m * sizeof (*pdist));
		cap = pcodes && pcls && pdist ? m * m : 0;
		if (!cap) return NULL;
	}
	if (!(w = calloc (1, (size_t) n * n))) return NULL;
	/* the same texel centres, pad texels further out on every side */
	pp.n = m;
	pp.half = pt->half * m / n;
	if (psurf_raster (s, &pp, pcodes) || psurf_classify (s, pcodes, m, pcls, pdist)) {
		free (w);
		return NULL;
	}
	for (y = 0; y < n; y++)
		for (x = 0; x < n; x++) {
			int c = lut[(codes[y*n + x] >> 2) & 15], d = pdist[(y + pad) * m + x + pad];
			double a = (2.0 * x + 1.0 - n) * pt->half / n, b = (2.0 * y + 1.0 - n) * pt->half / n;
			double r2 = 1.0 + a*a + b*b, dir[3], dist, t;

			if (c != PS_WATER && !(c == PS_LAND && beach)) continue;
			if (abs (d) == 32767) {		/* no coast in reach */
				if (c == PS_WATER) w[y*n + x] = 128;
				continue;
			}
			/* the two rasters may disagree by a texel at the coast */
			d = abs (d) < 1 || (d < 0) != (c == PS_WATER) ? 1 : abs (d);
			/* texel distance to radians: the gnomonic scale there */
			dist = d / 3.0 * tex * pow (r2, -0.75);
			if (dist > (c == PS_WATER ? 1.5 * W_SHALLOW : 1.5 * W_BEACH)) {
				if (c == PS_WATER) w[y*n + x] = 128;
				continue;
			}
			for (k = 0; k < 3; k++) dir[k] = (pt->c[k] + a * pt->u[k] + b * pt->v[k]) / sqrt (r2);
			if (c == PS_WATER) {
				dist += 0.6 * W_SHALLOW * (vnoise (dir, 1.0 / W_SHALLOW, seed) - 0.5);
				t = smooth (0.0, W_SHALLOW, dist);
				w[y*n + x] = 1 + (int) (127.0 * t + 0.5);
			} else {
				dist += 0.8 * W_BEACH * (vnoise (dir, 1.0 / W_BEACH, seed ^ 0x5a17u) - 0.5);
				t = 1.0 - smooth (0.5 * W_BEACH, W_BEACH, dist);
				if (t > 0.0) w[y*n + x] = 128 + (int) fmax (1.0, 127.0 * t + 0.5);
			}
		}
	return w;
}

/* A ground patch too fine for a margin to reach the bands (low down and
 * landed) reads them from a probe: a 512² raster around it, at least as
 * fine as a cube face, reaching the bands beyond its edge. The probe is kept while
 * the patch stays inside it or is the same patch again, so a patch rasterized
 * again on every frame (as when landed) does not pay for the coast distance
 * again. */
static struct { struct PSurf *s; unsigned int seed; struct PPatch p; unsigned char *w; } probe;

/* Where texel (x, y) of pt falls in the probe, before rounding; returns
 * the depth along the probe's centre (> 0 on the near side). */
static double probe_at (const struct PPatch *pt, double h, int x, int y, double q[2])
{
	int n = pt->n, k;
	double a = (2.0 * x + 1.0 - n) * pt->half / n, b = (2.0 * y + 1.0 - n) * pt->half / n;
	double d[3], dc;

	for (k = 0; k < 3; k++) d[k] = pt->c[k] + a * pt->u[k] + b * pt->v[k];
	dc = wdot (d, probe.p.c);
	q[0] = wdot (d, probe.p.u) / dc * h + 0.5 * W_PROBE;
	q[1] = wdot (d, probe.p.v) / dc * h + 0.5 * W_PROBE;
	return dc;
}

/* the probe texel index of q, as (int) and clamped to the probe */
static int probe_clamp (double q)
{
	return q < 0.0 ? 0 : q >= W_PROBE - 1 ? W_PROBE - 1 : (int) q;
}

/* The probe byte under every texel of the size² block at (bx, by), or -1
 * if they may differ. The map from texel to probe is a ratio of linear
 * functions, so the block's corners bound it; a millionth of a probe
 * texel covers the rounding. Away from a coast one lookup serves the whole
 * block, and a landed patch lies inside one probe texel. */
static int block_value (const struct PPatch *pt, double h, int bx, int by, int size)
{
	double q[2], lo[2] = { 1e9, 1e9 }, hi[2] = { -1e9, -1e9 };
	int i, k, x0, x1, y0, y1, x, y, v;

	for (i = 0; i < 4; i++) {
		int cx = bx + (i & 1 ? size - 1 : 0), cy = by + (i & 2 ? size - 1 : 0);
		if (probe_at (pt, h, cx < pt->n ? cx : pt->n - 1, cy < pt->n ? cy : pt->n - 1, q) <= 0.0)
			return -1;
		for (k = 0; k < 2; k++) { lo[k] = fmin (lo[k], q[k]); hi[k] = fmax (hi[k], q[k]); }
	}
	x0 = probe_clamp (lo[0] - 1e-6); x1 = probe_clamp (hi[0] + 1e-6);
	y0 = probe_clamp (lo[1] - 1e-6); y1 = probe_clamp (hi[1] + 1e-6);
	v = probe.w[y0 * W_PROBE + x0];
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			if (probe.w[y * W_PROBE + x] != v) return -1;
	return v;
}

/* the shade byte of a water or beach texel over probe byte v: the probe
 * is coarser, so where it disagrees this is the coast */
static int fine_w (int c, int v)
{
	if (c == PS_WATER) return v >= 1 && v <= 128 ? v : 1;
	return v > 128 ? v : v ? 255 : 0;
}

/* As make_map, from the probe. When the whole patch lies over one probe
 * byte (landed, or low over open land or sea) the shade follows from the
 * code alone: it goes in e->wn, and nothing is kept per texel. */
static unsigned char *make_fine_map (const unsigned char *codes, const struct PPatch *pt,
				     struct PSurf *s, struct WMap *e)
{
	static unsigned char pcodes[W_PROBE * W_PROBE];
	double ext = atan (pt->half) + 1.5 * W_SHALLOW, h;
	unsigned char lut[16], *w;
	int n = pt->n, x, y, k, wet = 0, beach, bx, by, v;

	for (k = 0; k < 16; k++) {
		lut[k] = psurf_code_class (s, k << 2);
		wet |= lut[k] == PS_WATER;
	}
	if (!wet) return NULL;
	beach = psurf_temperate (s);
	/* The patch is rasterized again in place on most frames low down
	 * (its centre is not quite a unit vector, so the test against the
	 * probe never passes): the same probe again is kept. */
	if (!probe.w || probe.s != s || probe.seed != psurf_seed (s) ||
	    (acos (fmin (1.0, wdot (pt->c, probe.p.c))) + ext > atan (probe.p.half) &&
	     (memcmp (pt->c, probe.p.c, sizeof (pt->c)) || memcmp (pt->u, probe.p.u, sizeof (pt->u)) ||
	      memcmp (pt->v, probe.p.v, sizeof (pt->v)) || probe.p.half != tan (ext + 0.01)))) {
		free (probe.w);
		probe.p = *pt;
		probe.p.n = W_PROBE;
		probe.p.half = tan (ext + 0.01);
		probe.s = s;
		probe.seed = psurf_seed (s);
		probe.w = psurf_raster (s, &probe.p, pcodes) ? NULL : make_map (pcodes, &probe.p, s);
		if (!probe.w) return NULL;
	}
	h = 0.5 * W_PROBE / probe.p.half;
	if ((v = block_value (pt, h, 0, 0, n)) >= 0) {
		for (k = 0; k < 16; k++)
			e->wn[k] = lut[k] == PS_WATER || (lut[k] == PS_LAND && beach) ? fine_w (lut[k], v) : 0;
		e->by_code = 1;
		return NULL;
	}
	if (!(w = calloc (1, (size_t) n * n))) return NULL;
	for (by = 0; by < n; by += W_BLOCK)
		for (bx = 0; bx < n; bx += W_BLOCK) {
			int bv = block_value (pt, h, bx, by, W_BLOCK);

			for (y = by; y < by + W_BLOCK && y < n; y++)
				for (x = bx; x < bx + W_BLOCK && x < n; x++) {
					int c = lut[(codes[y*n + x] >> 2) & 15];
					double q[2];

					if (c != PS_WATER && !(c == PS_LAND && beach)) continue;
					if ((v = bv) < 0) {
						probe_at (pt, h, x, y, q);
						v = probe.w[probe_clamp (q[1]) * W_PROBE + probe_clamp (q[0])];
					}
					w[y*n + x] = fine_w (c, v);
				}
		}
	return w;
}

static struct WMap *find_map (const unsigned char *codes, const struct PPatch *pt, struct PSurf *s)
{
	struct WMap *e = NULL;
	int i;

	for (i = 0; i < W_SLOTS; i++) {
		if (maps[i].codes == codes) { maps[i].lru = ++clock_; return &maps[i]; }
		if (!e || !maps[i].codes || (e->codes && maps[i].lru < e->lru)) e = &maps[i];
	}
	free (e->w);
	e->codes = codes;
	e->by_code = 0;
	/* a ground patch within 0.3 radians: the probe is finer than a margin */
	e->w = pt->n > W_PROBE && atan (pt->half) < 0.3 ? make_fine_map (codes, pt, s, e)
		: make_map (codes, pt, s);
	e->lru = ++clock_;
	return e;
}

/* colour in, shaded by shade byte w (see WMap), into out */
static void shade (const unsigned char *in, int w, unsigned char *out)
{
	int k, l = in[0] > in[1] ? in[0] : in[1];

	memcpy (out, in, 3);
	if (!w || !(l = l > in[2] ? l : in[2])) return;
	for (k = 0; k < 3; k++) {
		float sh, v;
		if (w <= 128) {
			float t = (w - 1) / 127.0f;
			sh = shallow_k[k] + (deep_k[k] - shallow_k[k]) * t;
		} else {
			/* toward sand of the same brightness, a quarter at most */
			sh = (sand[k] * l - in[k]) / (float) l;
			sh = (sh > 0.25f ? 0.25f : sh < -0.25f ? -0.25f : sh) * (w - 128) / 127.0f;
		}
		v = in[k] + sh * l + 0.5f;
		out[k] = v < 0.0f ? 0 : v > 255.0f ? 255 : (unsigned char) v;
	}
}

/* ---- W2 slice 1: biomes on the land of terran worlds ----------------
 * A biome byte per texel: 128 lush, down to 0 polar tundra and rock, up
 * to 255 dry. It follows latitude (the polar caps sit on the model's y
 * axis), shifted by the body's temperature, and a seeded moisture noise,
 * all on the unit direction: the same at every resolution. */
#define W_BSTEP	16	/* texels between biome samples, interpolated between */

static struct BMap {
	unsigned int seed;
	int temp;
	struct PPatch p;
	unsigned char *b;	/* NULL: every texel is 'one' */
	int one;
	unsigned int lru;
} bmaps[W_SLOTS];
/* the slot key compares c, u, v and half as one block: they must lead PPatch */
typedef char bmap_key_layout[offsetof (struct PPatch, n) == 10 * sizeof (double) ? 1 : -1];

static double biome_at (const double d[3], int temp, unsigned int seed)
{
	double heat = fmax (-1.0, fmin (1.0, (temp - 288) / 40.0));
	double m = 0.65 * vnoise (d, 2.5, seed ^ 0xb10e5u) + 0.35 * vnoise (d, 6.0, seed ^ 0x3c7au);
	double y = fabs (d[1]) + 0.2 * (vnoise (d, 4.0, seed ^ 0x9e37u) - 0.5);
	double polar = smooth (0.72 + 0.3 * heat, 0.9 + 0.3 * heat, y);
	double band = 1.0 - smooth (0.2 + 0.35 * heat, 0.45 + 0.35 * heat, y);
	double dry = fmax (0.0, fmin (1.0, band * (0.3 + 0.7 * fmax (heat, 0.0)) + 2.0 * (0.45 - m)));

	return 128.0 + 127.0 * (dry * (1.0 - polar) - polar);
}

/* W12: the crater circles in reach of a patch, each with a tangent frame
 * and a few seeded rays: direction, and length 2 to 4 crater radii. */
static struct Crater { double c[3], u[3], w[3], r, reach2, bowl2, rim, flo, ray[W_RAYS][3]; int nr; } cr[W_CRATERS];
static int ncr;

/* W4: how much of the relief a patch of texel tex (radians) takes: none
 * at W_TEX0 and finer, all at W_TEX1 and coarser, log-linear between */
static double relief (double tex)
{
	return flat_relief ? 0.0 : smooth (0.0, 1.0, log (tex / W_TEX0) / log (W_TEX1 / W_TEX0));
}

/* texels between the regolith's gain samples: as close as the speckle needs */
static int regolith_step (double tex)
{
	return (int) fmin (32.0, fmax (2.0, 0.75 / (W_RF1 * tex)));
}

static void gather_craters (struct PSurf *s, const struct PPatch *pt, unsigned int seed)
{
	double c4[W_CRATERS][4], l, tex = 2.0 * pt->half / pt->n, rel = relief (tex), gs = regolith_step (tex) * tex;
	int i, j, k, n = psurf_craters (s, c4, W_CRATERS);

	for (ncr = i = 0; i < n; i++) {
		struct Crater *c = &cr[ncr];
		if (acos (fmin (1.0, wdot (c4[i], pt->c))) > atan (pt->half * 1.4143) + 4.0 * c4[i][3]) continue;
		memcpy (c->c, c4[i], sizeof (c->c));
		c->r = c4[i][3];
		c->reach2 = 16.0 * c->r * c->r;
		/* W4: the bowl's floor and rim, each faded as the gain samples'
		 * spacing gs nears its width, so neither aliases */
		c->bowl2 = rel > 0.0 ? 1.45 * 1.45 * c->r * c->r : 0.0;
		c->rim = rel * W_RIM * (1.0 - smooth (0.35, 0.7, 2.5 * gs / c->r));
		c->flo = rel * W_FLOOR * (1.0 - smooth (0.35, 0.7, gs / c->r));
		k = fabs (c->c[0]) < 0.6 ? 0 : fabs (c->c[1]) < 0.6 ? 1 : 2;
		for (j = 0; j < 3; j++) c->u[j] = (j == k) - c->c[j] * c->c[k];
		l = sqrt (wdot (c->u, c->u));
		for (j = 0; j < 3; j++) c->u[j] /= l;
		for (j = 0; j < 3; j++) c->w[j] = c->c[(j+1)%3] * c->u[(j+2)%3] - c->c[(j+2)%3] * c->u[(j+1)%3];
		c->nr = 4 + (int) (3.99 * hash3 (i, 0, 0, seed));
		for (j = 0; j < c->nr; j++) {
			double a = 6.2832 * (j + 0.6 * hash3 (i, j, 1, seed)) / c->nr;
			c->ray[j][0] = cos (a); c->ray[j][1] = sin (a);
			c->ray[j][2] = c->r * (2.0 + 2.0 * hash3 (i, j, 2, seed));
		}
		ncr++;
	}
}

/* W12: the gain byte of an airless surface at d, 128 + 256 x (speckle +
 * rays): two octaves of noise, each faded out where the patch's texel tex
 * (radians) is too coarse for it, and the rays of the gathered craters,
 * tapering and fading with distance.
 * W4: plus relief: two zero-mean ridged octaves on the ground and each
 * crater's floor and rim, faded the same way, and all of it by relief (tex). */
static double regolith_at (const double d[3], double tex, unsigned int seed)
{
	static double ltex = -1.0, k1, k2, g0, g2, rl;
	static struct VCell lo;
	float n2 = vnoise (d, W_RF2, seed ^ 0x7b1du);
	double v;
	int i, j;

	if (tex != ltex) {	/* the fades are the patch's: work them out once */
		double rel = rl = relief (tex);
		ltex = tex;
		k1 = 1.0 - smooth (0.35, 0.7, W_RF1 * tex);
		k2 = 1.0 - smooth (0.35, 0.7, W_RF2 * tex);
		g0 = rel * W_GAMP * (1.0 - smooth (0.35, 0.7, W_GF0 * tex));
		g2 = rel * W_GAMP * 0.6 * k2;
	}
	v = W_RAMP * (k1 * (vnoise (d, W_RF1, seed ^ 0x2e60u) - 0.5) + 0.75 * k2 * (n2 - 0.5));
	if (g0 > 0.0) v += g0 * (W_RIDGE - fabs (2.0 * vnoise_k (d, W_GF0, seed ^ 0x51e7u, &lo) - 1.0));
	if (g2 > 0.0) v += g2 * (W_RIDGE - fabs (2.0 * n2 - 1.0));
	for (i = 0; i < ncr; i++) {
		const struct Crater *c = &cr[i];
		double p[3] = { d[0] - c->c[0], d[1] - c->c[1], d[2] - c->c[2] }, pu, pw, p2 = wdot (p, p);
		if (p2 > c->reach2) continue;
		if (p2 < c->bowl2) {
			/* a rim about 0.4 of the radius wide, a floor about the radius */
			double s = sqrt (p2) / c->r;
			v += c->rim * smooth (0.85, 1.05, s) * (1.0 - smooth (1.05, 1.45, s)) - c->flo * (1.0 - smooth (0.5, 0.95, s));
		}
		pu = wdot (p, c->u); pw = wdot (p, c->w);
		for (j = 0; j < c->nr; j++) {
			double s = (c->ray[j][0] * pu + c->ray[j][1] * pw - c->r) / (c->ray[j][2] - c->r);
			double lat = fabs (c->ray[j][0] * pw - c->ray[j][1] * pu) / (0.25 * c->r * (1.0 - 0.8 * s));
			if (s > 0.0 && s < 1.0 && lat < 1.0) v += W_RBRIGHT * (1.0 - lat * lat) * (1.0 - s) * (1.0 - s);
		}
	}
	if (rl > 0.0) v = fmax (-W_GAIN, fmin (W_GAIN, v));	/* landed (no relief): W12's own, unclamped */
	return fmax (0.0, fmin (255.0, 128.0 + 256.0 * v));
}

/* The biome bytes of a patch: NULL and *one when one byte serves it all
 * (a landed patch). Kept by the patch's geometry, seed and temperature.
 * temp W_AIRLESS: the regolith gain bytes instead, on a grid as fine as
 * the speckle needs. */
static const unsigned char *biome_map (const struct PPatch *pt, unsigned int seed, int temp, int *one)
{
	static float g[(1024 / 2 + 1) * (1024 / 2 + 1)];
	float lo = 1e9f, hi = -1e9f;
	struct BMap *e = NULL;
	double tex = 2.0 * pt->half / pt->n;
	int n = pt->n, gn = n / (temp != W_AIRLESS ? W_BSTEP : regolith_step (tex)) + 1, i, k, x, y;
	double sc = (n - 1.0) / (gn - 1.0);

	for (i = 0; i < W_SLOTS; i++) {
		struct BMap *m = &bmaps[i];
		if (m->lru && m->seed == seed && m->temp == temp && m->p.n == n &&
		    !memcmp (m->p.c, pt->c, offsetof (struct PPatch, n))) {
			m->lru = ++clock_;
			*one = m->one;
			return m->b;
		}
		if (!e || m->lru < e->lru) e = m;
	}
	if (gn < 2 || gn > 1024 / 2 + 1) return *one = -1, NULL;
	for (y = 0; y < gn; y++)
		for (x = 0; x < gn; x++) {
			double a = (2.0 * x * sc + 1.0 - n) * pt->half / n, b = (2.0 * y * sc + 1.0 - n) * pt->half / n;
			double d[3], r = sqrt (1.0 + a*a + b*b);
			for (k = 0; k < 3; k++) d[k] = (pt->c[k] + a * pt->u[k] + b * pt->v[k]) / r;
			g[y*gn + x] = temp != W_AIRLESS ? biome_at (d, temp, seed) : regolith_at (d, tex, seed);
			lo = fminf (lo, g[y*gn + x]); hi = fmaxf (hi, g[y*gn + x]);
		}
	free (e->b);
	e->b = NULL;	/* a uniform patch keeps no map */
	e->seed = seed; e->temp = temp; e->p = *pt; e->lru = ++clock_;
	e->one = (int) ((lo + hi) * 0.5f + 0.5f);
	/* two steps of a byte are a fraction of a colour level: one byte */
	if (hi - lo > 2.0f && n <= 1024 && (e->b = malloc ((size_t) n * n))) {
		static float fxs[1024];
		static int x0s[1024];
		for (x = 0; x < n; x++) {	/* each column's cell and weight, once (W4) */
			float fx = x / sc;
			x0s[x] = fx >= gn - 1 ? gn - 2 : (int) fx;
			fxs[x] = fx - x0s[x];
		}
		for (y = 0; y < n; y++) {
			float fy = y / sc;
			int y0 = fy >= gn - 1 ? gn - 2 : (int) fy;
			const float *r = g + y0 * gn;
			fy -= y0;
			for (x = 0; x < n; x++) {
				const float *q = r + x0s[x];
				float fx = fxs[x], v = (q[0] * (1 - fx) + q[1] * fx) * (1 - fy) + (q[gn] * (1 - fx) + q[gn+1] * fx) * fy;
				e->b[y*n + x] = (unsigned char) (v + 0.5f);
			}
		}
	}
	*one = e->one;
	return e->b;
}

/* c with its hue turned toward h by at most dh degrees, its saturation
 * times ks and its value times kv, into d */
static void hue_toward (const unsigned char *c, float h, float dh, float ks, float kv, float *d)
{
	float mx = fmaxf (c[0], fmaxf (c[1], c[2])), mn = fminf (c[0], fminf (c[1], c[2]));
	float s = (mx - mn) / mx, hh, f, t;
	int i;

	if (mx - mn <= 0.0f) hh = h;
	else if (mx == c[0]) hh = 60.0f * fmodf ((c[1] - c[2]) / (mx - mn) + 6.0f, 6.0f);
	else if (mx == c[1]) hh = 60.0f * ((c[2] - c[0]) / (mx - mn) + 2.0f);
	else hh = 60.0f * ((c[0] - c[1]) / (mx - mn) + 4.0f);
	t = fmodf (h - hh + 540.0f, 360.0f) - 180.0f;
	hh = fmodf (hh + fmaxf (-dh, fminf (dh, t)) + 360.0f, 360.0f) / 60.0f;
	s = fminf (1.0f, s * ks);
	mx *= kv;
	i = (int) hh; f = hh - i;
	{
		float p = mx * (1 - s), q = mx * (1 - s * f), u = mx * (1 - s * (1 - f));
		float r[6][3] = { {mx,u,p}, {q,mx,p}, {p,mx,u}, {p,q,mx}, {u,p,mx}, {mx,p,q} };
		memcpy (d, r[i % 6], sizeof (r[0]));
	}
}

/* The pull d of biome byte b on colour in: a hue shift of the texel's own
 * colour, at most a quarter of its brightest channel. Airless: the colour
 * a little toward grey, times the regolith gain. */
static void tone_d (const unsigned char *in, int b, int air, float *d)
{
	float x = (b - 128) / 127.0f, a[3], o[3], y = 0.299f * in[0] + 0.587f * in[1] + 0.114f * in[2];
	int k, l = in[0] > in[1] ? in[0] : in[1];

	for (k = 0; k < 3 && air; k++) d[k] = (in[k] + (y - in[k]) * W_RDESAT) * (1.0f + (b - 128) / 256.0f) - in[k];
	if (air) return;
	memset (d, 0, 3 * sizeof (*d));
	if (!(l = l > in[2] ? l : in[2])) return;
	hue_toward (in, 110.0f, 25.0f, 1.1f, 1.0f, a);			/* lush */
	if (x < 0.0f) x = -x, hue_toward (in, 0.0f, 0.0f, 0.35f, 1.0f, o);	/* tundra, rock */
	else hue_toward (in, 40.0f, 60.0f, 0.7f, 1.0f, o);			/* dry */
	for (k = 0; k < 3; k++) {
		d[k] = a[k] + (o[k] - a[k]) * x - in[k];
		d[k] = d[k] > 0.25f * l ? 0.25f * l : d[k] < -0.25f * l ? -0.25f * l : d[k];
	}
}

/* sh, a colour shaded at byte w, pulled by d: the pull gives way to the
 * beach sand across the rim, so the two together stay within a quarter */
static void tone (const unsigned char *sh, int w, const float *d, unsigned char *out)
{
	float t = w > 128 ? (255 - w) / 127.0f : 1.0f, v;
	int k;

	for (k = 0; k < 3; k++) {
		v = sh[k] + d[k] * t + 0.5f;
		out[k] = v < 0.0f ? 0 : v > 255.0f ? 255 : (unsigned char) v;
	}
}

/* W7: a gas giant's fine bands. Its band coordinate at d is the latitude
 * (the poles sit on the model's y axis) in bands, waved by two octaves of
 * seeded noise so the edges wander. */
#define W_GBANDS	12.0	/* bands a radian: about 38 pole to pole */
#define W_GSTEP		0.05f	/* a band's brightness step, at most, a side */
#define W_GPULL		0.3f	/* its pull of hue toward one of the giant's colours, at most */
#define W_GMAX		0.08f	/* the two together, at most, a side: shares of the brightest channel */
#define W_GEDGE		0.45f	/* share of a band its soft edge takes */
#define W_GSTRIDE	8	/* texels between band samples, interpolated between */
#define W_GLEV		32	/* steps a band, for its edge */

static double band_at (const double d[3], unsigned int seed)
{
	return W_GBANDS * (asin (fmax (-1.0, fmin (1.0, d[1]))) + 0.07 * (vnoise (d, 3.0, seed ^ 0x6a5u) - 0.5)
		+ 0.025 * (vnoise (d, 11.0, seed ^ 0x1f3du) - 0.5));
}

/* The bands over the colours p of a giant's codes: per band a seeded step,
 * belts and zones in turn, and a pull toward the hue (not the brightness)
 * of one of its own colours; blended across a soft edge, and faded out
 * where a band would be under about three texels. Each texel looks its
 * colour up by band, code and step of the edge. 0 if n does not suit. */
static int giant_bake (unsigned char *rgb, const unsigned char *codes, const struct PPatch *pt,
		       unsigned int seed, const unsigned char p[16][3])
{
	static float g[(1024 / W_GSTRIDE + 1) * (1024 / W_GSTRIDE + 1)], row[1024 / W_GSTRIDE + 1][2], dl[65][16][3];
	static unsigned char lut[64][16][W_GLEV][3];
	int n = pt->n, gn = n / W_GSTRIDE + 1, j, c, k, x, y, i;
	float fade = 1.0f - smooth (0.35, 0.7, W_GBANDS * 2.0 * pt->half / n);

	if (n % W_GSTRIDE || gn < 2 || gn > 1024 / W_GSTRIDE + 1) return 0;
	for (j = 0; j < 65; j++) {
		const unsigned char *o = p[(int) (15.99f * hash3 (j & 63, 2, 0, seed))];
		float st = ((j & 1) ? W_GSTEP : -W_GSTEP) * (0.4f + 0.6f * hash3 (j & 63, 0, 0, seed));
		float w = W_GPULL * hash3 (j & 63, 1, 0, seed), yo = 0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2];
		for (c = 0; c < 16; c++) {
			const unsigned char *a = p[c];
			float ya = 0.299f * a[0] + 0.587f * a[1] + 0.114f * a[2], l = fmaxf (a[0], fmaxf (a[1], a[2]));
			for (k = 0; k < 3; k++) {
				float v = st * a[k] + (yo >= 1.0f ? w * (o[k] * ya / yo - a[k]) : 0.0f);
				dl[j][c][k] = fade * fmaxf (-W_GMAX * l, fminf (W_GMAX * l, v));
			}
		}
	}
	for (j = 0; j < 64; j++)
		for (c = 0; c < 16; c++)
			for (i = 0; i < W_GLEV; i++) {
				float t = fmaxf (0.0f, ((i + 0.5f) / W_GLEV - 1.0f + W_GEDGE) / W_GEDGE);
				t = t * t * (3.0f - 2.0f * t);
				for (k = 0; k < 3; k++)
					lut[j][c][i][k] = (unsigned char) fmaxf (0.0f, fminf (255.0f,
						p[c][k] + dl[j][c][k] + (dl[j+1][c][k] - dl[j][c][k]) * t + 0.5f));
			}
	for (y = 0; y < gn; y++)
		for (x = 0; x < gn; x++) {
			double a = (2.0 * x * W_GSTRIDE + 1.0 - n) * pt->half / n, b = (2.0 * y * W_GSTRIDE + 1.0 - n) * pt->half / n;
			double d[3], r = sqrt (1.0 + a*a + b*b);
			for (k = 0; k < 3; k++) d[k] = (pt->c[k] + a * pt->u[k] + b * pt->v[k]) / r;
			g[y*gn + x] = W_GLEV * (64.0f + (float) band_at (d, seed));	/* kept positive */
		}
	for (y = 0; y < n; y++) {
		const float *q = g + y / W_GSTRIDE * gn, fy = (float) (y % W_GSTRIDE) / W_GSTRIDE;
		for (x = 0; x < gn; x++) row[x][0] = q[x] + (q[x + gn] - q[x]) * fy;
		for (x = 0; x < gn - 1; x++) row[x][1] = (row[x + 1][0] - row[x][0]) / W_GSTRIDE;
		for (x = 0; x < n; x++, rgb += 3) {
			unsigned int u = (unsigned int) (row[x / W_GSTRIDE][0] + row[x / W_GSTRIDE][1] * (x % W_GSTRIDE));
			memcpy (rgb, lut[u / W_GLEV % 64][(codes[y*n + x] >> 2) & 15][u % W_GLEV], 3);
		}
	}
	return 1;
}

void worlds_bake (unsigned char *rgb, const unsigned char *codes, const struct PPatch *pt,
		  struct PSurf *s, const unsigned char pal[16][3])
{
	/* a texel's colour follows from its code, shade byte and biome byte:
	 * each triple is worked out once a bake (beach rims, being few, as
	 * they come) */
	static unsigned char tab[16][256][3], done[16][256], btab[16][256][3], bdone[16][256];
	static float bd[16][256][3];
	unsigned char p[16][3];
	struct WMap *e = flat_biomes || !s ? NULL : find_map (codes, pt, s);
	const unsigned char *bm = NULL;
	int i, k, n = pt->n, land[16] = { 0 }, any = 0, one = -1, air = e && psurf_airless (s);
	int temp = air ? W_AIRLESS : e ? psurf_terran_temp (s) : -1;

	for (k = 0; k < 16 && temp != -1; k++) any |= land[k] = air || psurf_code_class (s, k << 2) == PS_LAND;
	if (air) gather_craters (s, pt, psurf_seed (s));
	if (any) bm = biome_map (pt, psurf_seed (s), temp, &one);
	if (!bm && one < 0) memset (land, 0, sizeof (land));
	for (k = 0; k < 16; k++) {
		int w = e && e->by_code ? e->wn[k] : 0;
		shade (pal[k], w, p[k]);
		if (land[k] && one >= 0) tone_d (pal[k], one, air, bd[k][0]), tone (p[k], w, bd[k][0], p[k]);
	}
	if (e && psurf_gas_giant (s) && giant_bake (rgb, codes, pt, psurf_seed (s), (const unsigned char (*)[3]) p))
		return;
	if ((!e || !e->w) && !bm) {
		for (i = 0; i < n*n; i++, rgb += 3) memcpy (rgb, p[(codes[i] >> 2) & 15], 3);
		return;
	}
	memset (done, 0, sizeof (done));
	memset (bdone, 0, sizeof (bdone));
	for (i = 0; i < n*n; i++, rgb += 3) {
		int c = (codes[i] >> 2) & 15, w = e->w ? e->w[i] : e->by_code ? e->wn[c] : 0, b;

		if (!done[c][w]) { shade (pal[c], w, tab[c][w]); done[c][w] = 1; }
		if (!land[c]) { memcpy (rgb, tab[c][w], 3); continue; }
		b = bm ? bm[i] : one;
		if (!bdone[c][b]) {
			tone_d (pal[c], b, air, bd[c][b]);
			if (!done[c][0]) { shade (pal[c], 0, tab[c][0]); done[c][0] = 1; }
			tone (tab[c][0], 0, bd[c][b], btab[c][b]);
			bdone[c][b] = 1;
		}
		if (w) tone (tab[c][w], w, bd[c][b], rgb);
		else memcpy (rgb, btab[c][b], 3);
	}
}

void worlds_forget (const unsigned char *codes)
{
	int i;

	for (i = 0; i < W_SLOTS; i++)
		if (maps[i].codes && (!codes || maps[i].codes == codes)) {
			free (maps[i].w);
			memset (&maps[i], 0, sizeof (maps[i]));
		}
	if (!codes) {
		free (probe.w);
		memset (&probe, 0, sizeof (probe));
		for (i = 0; i < W_SLOTS; i++) free (bmaps[i].b);
		memset (bmaps, 0, sizeof (bmaps));
	}
}
