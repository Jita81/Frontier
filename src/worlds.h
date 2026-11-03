/*
 * HD planet surfaces: sea depth and beaches (W3), land biomes (W2).
 * See docs/hd/planet-classes.md, "What is drawn".
 */
#ifndef _WORLDS_H
#define _WORLDS_H

struct PSurf;
struct PPatch;

/* --flat-biomes: the game's flat seas and land, no beaches or biomes (main.c) */
extern int flat_biomes;
/* --flat-relief: no W4 crater bowls or ground relief on airless bodies,
 * the rest of the W passes kept (main.c) */
extern int flat_relief;

/* Fill rgb[n*n*3] with the patch pt of feature codes, baked for one light
 * zone in the colours pal (by code >> 2 & 15), and shade it: water by its
 * distance from the coast, temperate shores with a sand rim. Distances
 * are kept per codes buffer, so the other light zones reuse them. The land
 * of a terran world takes a biome tone (lush, dry, tundra and rock). Plain
 * colours if s is NULL or with --flat-biomes. */
void worlds_bake (unsigned char *rgb, const unsigned char *codes, const struct PPatch *pt,
		  struct PSurf *s, const unsigned char pal[16][3]);

/* The codes buffer was rasterized again (or freed): drop what is kept
 * for it. NULL drops everything. */
void worlds_forget (const unsigned char *codes);

#endif
