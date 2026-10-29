/*
 * Hull material for the GL renderer (--hull-style 1..5).
 *
 * Everything here is plain C with no GL and no 68k state. Styles 1 to 4
 * make their textures at run time from nothing but a seed; style 5 (the
 * default) bakes its maps from a grey tile per class of ship (art/hull/,
 * new art made from a text prompt, see docs/hd/hulls.md) and falls back
 * to the seeded ones when the tile is not there. Nothing is taken from
 * the game's bitmaps. The per-polygon maths takes the polygon's own
 * vertices. gl.c owns the shaders and the draw calls; the comment above
 * hull_style there says what each style does.
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

/* Model key: bit 31 set when valid, the low 16 bits the model's type
 * word (object+90) and bits 16 and 17 the model table that word indexes:
 * none for the game's own, HULL_TAB_INTRO for the intro's (its title,
 * scenes and the menu's backdrop: its words name other models than the
 * game's), HULL_TAB_OTHER for any other. HULL_KEY_MODEL is the table and
 * word together, which is what names one model. Which profile style 4
 * gives a model (by the word's low byte, as that style always read it). */
#define HULL_KEY_OK(k)	((unsigned int) (k) >> 31)
#define HULL_TAB_INTRO	0x10000u
#define HULL_TAB_OTHER	0x20000u
#define HULL_KEY_MODEL(k)	((unsigned int) (k) & 0x3ffffu)
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
	float mu[4][2];		/* style 5: the second scale's coordinates */
};
/* p: n (3 or 4) vertices in view space; light: direction towards the
 * light in the same space; seed: this face's own number (stable as the ship
 * turns). 0 for a degenerate polygon. */
int hull_frame (const double p[][3], int n, const double light[3], unsigned int seed,
		struct HullFrame *f);

unsigned int hull_mix (unsigned int x);

/*
 * Style 5: a material per class of ship. Each class has its own grey tile
 * (a seamless photo-like plating, 128 = the face's own colour) and its own
 * numbers for how the tile is turned into the shader's maps and how the
 * shader lights it. hull_models[] in hull.c says which model is which
 * class; a model not listed is HC_GENERIC here, and the draw side leaves
 * it flat.
 */
enum { HC_GENERIC, HC_POLICE, HC_CIVIL, HC_FREIGHT, HC_ARMOUR, HC_FIGHTER,
       HC_STATION, HC_HANGAR, HC_DECK, HC_N };
/* HC_STATION and after are buildings and interiors: their decal hosts and
 * complex shapes (signs) stay flat. HC_DECK is no model's class: it is the
 * hangar's floor and ceiling (a hangar face that looks along the model's
 * HULL_DECK_AXIS), see hull_obj_axis. */
#define HULL_BUILDING(cls)	((cls) >= HC_STATION)
#define HULL_DECK_AXIS	1

struct HullClass {
	const char *name;
	const char *tile;	/* the grey tile, in the art folder */
	int prof;		/* hull_profiles row baked when the tile is missing */
	float panel;		/* object units one tile covers */
	/* baking the tile: how much of its light and dark becomes paint, how
	 * dark its grooves go, how much of it is grime and chipped paint */
	float contrast, seam, wear, chips;
	/* shader: normal map strength, specular strength and exponent, edge
	 * occlusion, warmth of grime */
	float bump, gloss, shine, ao, tint;
	/* shader: the footprint's long side over its short side (as e / det,
	 * r + 1/r) from which a face seen edge on fades to its own colour, flat
	 * by 1.5 times that; 0 keeps the plating at every angle */
	float graze;
};
extern const struct HullClass hull_classes[HC_N];

/* The class of a model key; *scale (times the class's panel) and off[2]
 * (where the model starts on the tile) are that model's own. */
int hull_class_of_key (unsigned int key, float *scale, float off[2]);
/* 1 if the model is listed in hull_models[] (a ship or station hull, the
 * only models plated), 0 if not */
int hull_key_listed (unsigned int key);
/* gl.c, at a put: in the intro's own scenes (not its title, logo, credits
 * or start menu) the model word of the part being put, its own (hull_models'
 * key less the table); 0 anywhere else */
unsigned int hull_intro_part (void);
/* 1 if the face (n corners in the model's frame) of a docking bay's model
 * lies partly out of the bay: it is the station's outside around the slot */
int hull_bay_outside (unsigned int key, const float w[][3], int n);

/* The maps of a class from its n x n grey tile lum (n a power of two), in
 * the layout of hull_bake. */
void hull_bake_tile (int cls, const unsigned char *lum, int n, unsigned char *t0,
		     unsigned char *t1, float *mean_grime);

/* The axis of the model's frame (0 x, 1 y, 2 z) a face of n vertices w
 * (model units) looks along most, a near tie going to the first; -1 if
 * the face is degenerate. */
int hull_obj_axis (const float w[][3], int n);
/* the longest distance between two of the face's corners, in model units */
double hull_obj_span (const float w[][3], int n);

/*
 * Style 5 on a face that moves against its own model (a docking bay's
 * door, whose edge slides while the rest stays): w (n corners in the
 * model's frame, as hull_frame_obj takes them) is moved so that the tile
 * stays on the face's moving part instead of the model, and a face whose
 * part's frame jumps under it keeps its tile. A face is followed from
 * frame to frame by key, seed (the corners it uses), a6 (the 68k's frame
 * that drew it) and where it was; frame counts the engine's frames and q
 * is how many model units one unit of the 68k's view space is. Returns
 * HULL_ANIM_KEPT (w is ready), with HULL_ANIM_FLAT when the face changes
 * shape in a way no move of the tile follows (draw it flat; w untouched),
 * and the TELL bits the first time a face is seen to move or deform; 0
 * when the face could not be followed (w untouched).
 */
#define HULL_ANIM_KEPT		1
#define HULL_ANIM_FLAT		2
#define HULL_ANIM_TELL_MOVE	4
#define HULL_ANIM_TELL_FLAT	8
int hull_anim_bind (unsigned int key, unsigned int seed, unsigned int a6, unsigned int frame,
		    int n, float w[][3], double q);
/* forget every face followed so far */
void hull_anim_reset (void);

/* Style 5 coordinates: as hull_frame, but the tile is laid on the model
 * itself, not on the face: w are the n vertices in the model's own frame
 * (object units), and the face takes the plane of the model's box it
 * faces most (x, y or z), panel units to a tile, starting at off. mu are
 * the second, larger scale. 0 if the face is degenerate. */
int hull_frame_obj (const double p[][3], const float w[][3], int n, const double light[3],
		    double panel, const float off[2], struct HullFrame *f);

/* Style 5 on a complex shape (more than four sides, or holes): the tile
 * laid as hull_frame_obj lays it, from the model's frame. c: n points of
 * the shape in view space; M, o: the model's frame (a vertex v, as the 68k
 * keeps it, is at M (v - o) in the model's units); u at a point e of the
 * shape is gu . e + u0, v likewise. T, B, N: the tangent frame in view
 * space, lt the light in it. 0 if the shape is degenerate. */
struct HullPlane {
	double gu[3], gv[3], u0, v0, base[2], mb[2], T[3], B[3], N[3];
	float lt[3];
};
int hull_plane_obj (const double c[][3], int n, const float M[9], const int o[3],
		    const double light[3], double panel, const float off[2], struct HullPlane *g);
/* the shader's coordinates at a point e of the shape (as HullFrame's uv,
 * mu and vt) */
void hull_plane_at (const struct HullPlane *g, const double e[3], float uv[2], float mu[2],
		    float vt[3]);

#endif
