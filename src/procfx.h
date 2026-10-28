/*
 * procfx: procedural detail layers for the GL renderer, drawn on top of
 * what the game already draws.  See docs/hd/procedural.md.
 *
 * The rules every layer here keeps:
 *  - its numbers come from game state, read from ST memory and never
 *    written (the oracle hashes RAM and every hcall);
 *  - one off switch, which gives back the old look exactly;
 *  - the noise comes from the one shared library below, never a copy.
 *
 * Layer 1, the sky band: the Milky Way is a flat polygon of the background
 * object (fe2.s L44200_DrawBGStars, model word $b0).  The polygon stays the
 * game's: same shape, same place, same colour on average.  A fragment
 * shader fills it with star clouds and dust lanes, fixed to the sky by the
 * object's own rotation, so the pattern turns with the band, and a thin
 * strip outside its outline fades the hard edge out.
 * Off: --flat-sky.
 *
 * Layer 2, the limb glow: a lit rim of the planet's own air colour round
 * a planet seen from afar, where the game draws no air.  Off: --flat-limb.
 *
 * Layer 3, the corona: soft light and streamers round a star's disc, in
 * the colour of its class, the disc drawn over it.  Off: --flat-stars.
 */
#ifndef _PROCFX_H
#define _PROCFX_H

/* What a body is, by the model word the game draws it with (hdback.c picks
 * its backdrops by these too). */
enum light { L_NONE, L_RED, L_ORANGE, L_YELLOW, L_WHITE, L_BLUE, L_MAX };
int is_star (int model);
enum light star_light (int model);
/* the class any port falls back to: a body outside the table, or one
 * with no picture of its own (a star, a surface port on a giant) */
#define DEFAULT_CLASS "airless"
const char *body_class (int model);

/* --flat-sky: the game's flat Milky Way band */
extern int flat_sky;

/* Put side, inside the hcall that starts a complex polygon: 1 when the
 * polygon belongs to the background object, with its rotation (nine 1.15
 * words of the object's frame, as floats) in m.  Reads only. */
int procfx_sky_put (float m[9]);
/* the sky was put in this 3D view (cleared in Nu_3DViewInit) */
extern int procfx_sky_seen;

/* Draw side, around one such polygon.  proj and vp are the projection and
 * viewport the polygon's vertices were projected with.  procfx_sky_begin
 * returns 0, and changes nothing, when the shader is not available. */
int procfx_sky_begin (const float m[9], const double proj[16], const int vp[4]);
/* between the two: a soft edge outside one contour of the polygon, given
 * in window coordinates */
void procfx_sky_feather (const double (*v)[3], int n);
void procfx_sky_end (void);
/* Layer 2: the game-over backdrop, a dark nebula filling the current
 * viewport under the tombstone (see endcard_frame in hdcontrols.c), faded
 * up from black by fade 0..1.  Changes nothing if the shader fails. */
void procfx_endgame_backdrop (float fade);

/* --flat-limb, --flat-stars */
extern int flat_limb, flat_stars;

/* Put side of the limb glow, in Nu_PutPlanet before the game's air test:
 * col and scale are the outer air layer (0 for none), c and r the
 * planet's view position and radius.  limb gets the rim's colour times
 * its strength and its width as a share of the radius; all 0 for none. */
void procfx_limb_put (int col, double scale, const double c[3], double r, float limb[4]);
/* Draw side, after the planet's sphere: c, r and sun (unit) in view
 * coordinates.  Returns the half side of the square to fill about the
 * planet's centre with the program bound, 0 (nothing bound) for none. */
float procfx_limb_begin (const float limb[4], const double c[3], double r, const double sun[3]);

/* Put side of the corona, in the circle hcalls: which star the disc
 * belongs to (model 0: none, or not a star). */
struct procfx_star { int model; unsigned int seed; };
void procfx_star_put (struct procfx_star *s);
/* Draw side, before the disc of radius r at c (GL eye coordinates): as
 * procfx_limb_begin. */
float procfx_corona_begin (const struct procfx_star *s, const float c[3], float r);
/* after the square of either */
void procfx_glow_end (void);

/* change_vidmode, before the GL context goes */
void procfx_forget (void);

#endif
