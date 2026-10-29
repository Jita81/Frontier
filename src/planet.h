/*
 * Planet surface maps for the GL renderer.
 *
 * See planet.c for how the ST renderer draws planet surfaces (fe2.s
 * L3cd9c_ProjectPlanet and friends) and how this reproduces it.
 */
#ifndef _PLANET_H
#define _PLANET_H

/* An opaque, parsed planet surface description (continents, seas, ice
 * caps...). Owned by the cache in planet.c. */
struct PSurf;

/* A square gnomonic patch of the planet surface, in model coordinates:
 * the texel at (s, t) in [0,1]^2 covers the direction
 *	c + ((2s-1)*half) * u + ((2t-1)*half) * v
 * c, u, v must be orthonormal. */
struct PPatch {
	double c[3], u[3], v[3];
	double half;
	int n;
};

/* Look up (or parse and cache) the surface features of a planet.
 * feat_addr:	68k address of the feature list (fe2.s planet_features),
 *		0 for a planet with no features
 * seed:	the planet object's random seed (long at 118(object))
 * radius_word:	high word of the planet model's radius long
 * detail:	L60d4_optdetail2
 * Returns NULL when out of memory. The surface belongs to the cache and
 * may be freed by any later psurf_get: do not keep the pointer. */
struct PSurf *psurf_get (unsigned int feat_addr, unsigned int seed,
			 int radius_word, int detail);

/* The 68k address just past the end of the feature list at feat_addr,
 * walked as psurf_get parses it: 0 if the list does not end before limit.
 * Reads only addresses in [feat_addr, limit). */
unsigned int psurf_features_end (unsigned int feat_addr, unsigned int limit);

/* Rasterize the XOR'ed feature codes (multiples of 4, 0..0x3c) of the
 * patch into codes[n*n], row major, row 0 at t = 0. s may be NULL.
 * Returns 0, or -1 with codes all 0 when the surface could not be
 * rasterized (no surface, out of memory, too many outline leaves). */
int psurf_raster (struct PSurf *s, const struct PPatch *p, unsigned char *codes);

/* The radius word normalized the way l3cdd8 does it: 0x4000..0x7fff,
 * for any input (S-10). */
int psurf_norm_radius (int radius_word);

/* Light band code the ST renderer XORs over the feature codes for a surface
 * direction whose cosine to the sun is cos_sun (see planet.c). flags is the
 * high byte of the first word of the planet model. */
int psurf_light_code (int flags, double cos_sun);

/* W1 surface classes (docs/hd/planet-classes.md). */
enum { PS_OTHER, PS_WATER, PS_LAND, PS_ICE, PS_CRATER };

/* The class of a feature code (as psurf_raster writes it), from the
 * colour the model draws it in, the kind of body and its temperature. */
int psurf_code_class (struct PSurf *s, int code);

/* Classes of a patch of codes[n*n] into cls[n*n], and, if dist is not
 * NULL, the signed distance to the coast in dist[n*n]: 3 units a texel,
 * negative in water, +-32767 where there is no coast. 0, or -1 if s is
 * NULL. */
int psurf_classify (struct PSurf *s, const unsigned char *codes, int n,
		    unsigned char *cls, short *dist);

#endif /* _PLANET_H */
