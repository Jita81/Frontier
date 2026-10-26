/*
 * Hull material for the GL renderer (--hull-style 1..4).
 *
 * Everything here is plain C with no GL and no 68k state: the textures
 * are made at run time from nothing but a seed (no image files, nothing
 * taken from the game's bitmaps), and the per-polygon maths takes the
 * polygon's own vertices. gl.c owns the shaders and the draw calls; the
 * comment above hull_style there says what each style does.
 */
#ifndef _HULL_H
#define _HULL_H

/* Material profiles. HP_GENERIC is what styles 1 to 3 give every model;
 * style 4 picks one of the four classes per model from hull_models[]. */
enum { HP_GENERIC, HP_SMALL, HP_TRADER, HP_MILITARY, HP_STATION, HP_N };

struct HullProfile {
	const char *name;
	unsigned int seed;
	/* texture layout: panel sizes (fraction of the tile) and the chance
	 * that a panel carries each feature */
	float pmin, pmax;
	float recess;			/* how far panels sit below/above the seams */
	float hatch, vent, greeble, hazard, rivet;
	float wear;			/* grime, streaks, scratches */
	float contrast;			/* plate to plate tone */
	/* shader: normal map strength, specular strength and exponent,
	 * edge occlusion strength, warmth of worn paint */
	float bump, gloss, shine, ao, tint;
};
extern const struct HullProfile hull_profiles[HP_N];

/* Model key: bit 31 set when valid, low byte the model's type word
 * (object+90). Which profile style 4 gives a model. */
#define HULL_KEY_OK(k)	((unsigned int) (k) >> 31)
int hull_profile_of_key (unsigned int key);

/* Texture sets. Level 1 is plain plating (style 1), level 2 the full
 * panel layout (styles 2 to 4). Both textures are n x n RGBA8 and tile:
 *	t0: normal x, normal y (slope, 128 = flat), albedo, cavity
 *	t1: hazard stripes, gloss, grime, chips and scratches
 * mean_grime is returned for the shader to keep the average brightness. */
void hull_bake (int prof, int level, int n, unsigned char *t0, unsigned char *t1,
		float *mean_grime);

/* One polygon's frame on the plating: texture coordinates, the distance
 * to each of its edges (for edge occlusion), the light and the eye in the
 * polygon's own tangent frame. */
struct HullFrame {
	float uv[4][2];
	float ed[4][4];
	float vt[4][3];
	float lt[3];
	float inr;		/* radius of the biggest circle that fits, in tiles */
	float seed[2];		/* offset for the second texture scale */
};
/* p: n (3 or 4) vertices in view space; light: direction towards the
 * light in the same space; seed: this face's own number (stable as the ship
 * turns). 0 for a degenerate polygon. */
int hull_frame (const double p[][3], int n, const double light[3], unsigned int seed,
		struct HullFrame *f);

unsigned int hull_mix (unsigned int x);

#endif
