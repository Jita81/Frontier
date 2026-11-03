/*
 * W2 biome map cache (src/worlds.c biome_map), under AddressSanitizer:
 * fill all its slots with varying face maps, then bake a landed patch
 * (one biome byte, no map) into a slot a face map held. It must come out
 * as it does from an empty cache, read no freed map, and free nothing
 * twice when everything is forgotten (a video mode change).
 * Stubs stand in for planet.c: every code is land of a 288 K terran world.
 */
#include <stdio.h>
#include <string.h>
#include "../../src/planet.h"
int flat_biomes = 0;
static unsigned int seed = 1;
unsigned int psurf_seed (struct PSurf *s) { return seed; }
int psurf_temperate (struct PSurf *s) { return 1; }
int psurf_terran_temp (struct PSurf *s) { return 288; }
int psurf_code_class (struct PSurf *s, int code) { return PS_LAND; }
int psurf_raster (struct PSurf *s, const struct PPatch *p, unsigned char *codes) { return 1; }
int psurf_classify (struct PSurf *s, const unsigned char *codes, int n, unsigned char *cls,
		    short *dist) { return -1; }
#include "../../src/worlds.c"

static unsigned char codes[1024 * 1024], ref[1024 * 1024 * 3], rgb[1024 * 1024 * 3], pal[16][3];

static void bake (int axis, double half, int n)
{
	struct PPatch pt;

	memset (&pt, 0, sizeof (pt));
	pt.c[axis % 3] = axis < 3 ? 1.0 : -1.0;
	pt.u[(axis + 1) % 3] = pt.v[(axis + 2) % 3] = 1.0;
	pt.half = half; pt.n = n;
	worlds_forget (codes);
	worlds_bake (rgb, codes, &pt, (struct PSurf *) pal, (const unsigned char (*)[3]) pal);
}

int main (void)
{
	int k, f;

	for (k = 0; k < 16; k++) pal[k][0] = 64, pal[k][1] = 128, pal[k][2] = 64;
	bake (1, 1e-6, 1024);			/* landed, from an empty cache */
	memcpy (ref, rgb, sizeof (rgb));
	for (seed = 100; seed < 100 + W_SLOTS / 6 + 1; seed++)
		for (f = 0; f < 6; f++) bake (f, 1.0, 512);
	seed = 1;
	bake (1, 1e-6, 1024);			/* the same, into a face's slot */
	if (memcmp (ref, rgb, sizeof (rgb))) {
		printf ("biomes: a landed patch baked after slot reuse differs from a fresh bake\n");
		return 1;
	}
	worlds_forget (NULL);
	printf ("biomes: %d face maps then a landed patch: same as fresh, no stale map\n",
		6 * (W_SLOTS / 6 + 1));
	return 0;
}
