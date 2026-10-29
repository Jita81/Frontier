/*
 * HD planet surfaces, W3 slice 1: sea depth and beaches.
 * See docs/hd/planet-classes.md, "What is drawn".
 */
#ifndef _WORLDS_H
#define _WORLDS_H

struct PSurf;
struct PPatch;

/* --flat-biomes: the game's flat seas, no beaches (main.c) */
extern int flat_biomes;

/* Shade rgb[n*n*3], the patch pt of feature codes baked for one light
 * zone: water by its distance from the coast, temperate shores with a
 * sand rim. Distances are kept per codes buffer, so the other light zones
 * reuse them. Changes nothing if s is NULL or with --flat-biomes. */
void worlds_tint (unsigned char *rgb, const unsigned char *codes,
		  const struct PPatch *pt, struct PSurf *s);

/* The codes buffer was rasterized again (or freed): drop what is kept
 * for it. NULL drops everything. */
void worlds_forget (const unsigned char *codes);

#endif
