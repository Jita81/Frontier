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
 * and the ground patch agree. Off: --flat-biomes.
 * See docs/hd/planet-classes.md.
 */
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

/* the most a channel moves, as a share of the texel's brightest channel */
static const float shallow_k[3] = { -0.08f, 0.25f, 0.12f };
static const float deep_k[3] = { -0.10f, -0.12f, -0.08f };
static const float sand[3] = { 1.0f, 0.88f, 0.62f };

/* Per codes buffer, one byte a texel: 0 untouched, 1..128 water from the
 * shallows (1) to open sea (128), 129..255 beach weight. */
static struct WMap {
	const unsigned char *codes;
	unsigned char *w;	/* NULL: nothing to shade */
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

/* value noise, 0..1, on the unit direction d scaled by f */
static float vnoise (const double d[3], double f, unsigned int seed)
{
	double p[3], w[3];
	int i[3], k;
	float v = 0.0f;

	for (k = 0; k < 3; k++) {
		p[k] = d[k] * f;
		i[k] = (int) floor (p[k]);
		w[k] = p[k] - i[k];
		w[k] = w[k] * w[k] * (3.0 - 2.0 * w[k]);
	}
	for (k = 0; k < 8; k++)
		v += (k & 1 ? w[0] : 1 - w[0]) * (k & 2 ? w[1] : 1 - w[1]) * (k & 4 ? w[2] : 1 - w[2])
			* hash3 (i[0] + (k & 1), i[1] + !!(k & 2), i[2] + !!(k & 4), seed);
	return v;
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
				if (t > 0.0) w[y*n + x] = 128 + (int) (127.0 * t + 0.5);
			}
		}
	return w;
}

/* A ground patch too fine for a margin to reach the bands (low down and
 * landed) reads them from a probe: a 512² raster around it, at least as
 * fine as a cube face, reaching the bands beyond its edge. The probe is kept while
 * the patch stays inside it, so a patch rasterized again on every frame
 * (as when landed) does not pay for the coast distance again. */
static struct { struct PSurf *s; unsigned int seed; struct PPatch p; unsigned char *w; } probe;

static unsigned char *make_fine_map (const unsigned char *codes, const struct PPatch *pt, struct PSurf *s)
{
	static unsigned char pcodes[W_PROBE * W_PROBE];
	double ext = atan (pt->half) + 1.5 * W_SHALLOW, h;
	unsigned char lut[16], *w;
	int n = pt->n, x, y, k, wet = 0, beach;

	for (k = 0; k < 16; k++) {
		lut[k] = psurf_code_class (s, k << 2);
		wet |= lut[k] == PS_WATER;
	}
	if (!wet) return NULL;
	beach = psurf_temperate (s);
	if (!probe.w || probe.s != s || probe.seed != psurf_seed (s) ||
	    acos (fmin (1.0, wdot (pt->c, probe.p.c))) + ext > atan (probe.p.half)) {
		free (probe.w);
		probe.p = *pt;
		probe.p.n = W_PROBE;
		probe.p.half = tan (ext + 0.01);
		probe.s = s;
		probe.seed = psurf_seed (s);
		probe.w = psurf_raster (s, &probe.p, pcodes) ? NULL : make_map (pcodes, &probe.p, s);
		if (!probe.w) return NULL;
	}
	if (!(w = calloc (1, (size_t) n * n))) return NULL;
	h = 0.5 * W_PROBE / probe.p.half;
	for (y = 0; y < n; y++)
		for (x = 0; x < n; x++) {
			int c = lut[(codes[y*n + x] >> 2) & 15], px, py, v;
			double a = (2.0 * x + 1.0 - n) * pt->half / n, b = (2.0 * y + 1.0 - n) * pt->half / n;
			double d[3], dc;

			if (c != PS_WATER && !(c == PS_LAND && beach)) continue;
			for (k = 0; k < 3; k++) d[k] = pt->c[k] + a * pt->u[k] + b * pt->v[k];
			dc = wdot (d, probe.p.c);
			px = (int) (wdot (d, probe.p.u) / dc * h + 0.5 * W_PROBE);
			py = (int) (wdot (d, probe.p.v) / dc * h + 0.5 * W_PROBE);
			px = px < 0 ? 0 : px >= W_PROBE ? W_PROBE - 1 : px;
			py = py < 0 ? 0 : py >= W_PROBE ? W_PROBE - 1 : py;
			v = probe.w[py * W_PROBE + px];
			/* the probe is coarser: where it disagrees, this is the coast */
			if (c == PS_WATER) w[y*n + x] = v >= 1 && v <= 128 ? v : 1;
			else w[y*n + x] = v > 128 ? v : v ? 255 : 0;
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
	/* a ground patch within 0.3 radians: the probe is finer than a margin */
	e->w = pt->n > W_PROBE && atan (pt->half) < 0.3 ? make_fine_map (codes, pt, s)
		: make_map (codes, pt, s);
	e->lru = ++clock_;
	return e;
}

void worlds_tint (unsigned char *rgb, const unsigned char *codes,
		  const struct PPatch *pt, struct PSurf *s)
{
	const unsigned char *w;
	int i, k, n = pt->n;

	if (flat_biomes || !s || !(w = find_map (codes, pt, s)->w)) return;
	for (i = 0; i < n*n; i++, rgb += 3) {
		int l = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
		if (!w[i] || !(l = l > rgb[2] ? l : rgb[2])) continue;
		for (k = 0; k < 3; k++) {
			float sh, v;
			if (w[i] <= 128) {
				float t = (w[i] - 1) / 127.0f;
				sh = shallow_k[k] + (deep_k[k] - shallow_k[k]) * t;
			} else {
				/* toward sand of the same brightness, a quarter at most */
				sh = (sand[k] * l - rgb[k]) / (float) l;
				sh = (sh > 0.25f ? 0.25f : sh < -0.25f ? -0.25f : sh) * (w[i] - 128) / 127.0f;
			}
			v = rgb[k] + sh * l + 0.5f;
			rgb[k] = v < 0.0f ? 0 : v > 255.0f ? 255 : (unsigned char) v;
		}
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
	}
}
