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
 * Layer 2, planets: the textured planets of gl.c keep the game's light
 * zones, colours and features; a fragment shader blends the zones across
 * the terminator and adds the sun's fall-off, limb darkening, close-range
 * detail and an atmosphere rim.  gl.c also trades the flat atmosphere
 * discs for one soft halo.  Off: --flat-planets.
 *
 * Layer 3, the star corona: soft light and streamers round a star's disc
 * in the colour of its class, the disc drawn over it; flight view only.
 * Off: --flat-stars.
 */
#ifndef _PROCFX_H
#define _PROCFX_H

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
/* --flat-planets: the planets' exact light zones and flat atmosphere discs */
extern int flat_planets;

/* Layer 3: around one textured planet.  lo, hi: each light zone's cos(sun) range
 * (9, 9 for none); soft: half width of the blend at a boundary; rim: the
 * innermost atmosphere colour, alpha its weight (0 when airless or down
 * on the ground, 1 seen from space); look: the sun's fall-off (1 lit and
 * seen from space, 0 unlit or on the ground), limb darkening, detail
 * amplitude; seed: the body's.
 * 0, changing nothing, when off or when the shader is not available. */
int procfx_planet_begin (const float lo[4], const float hi[4], float soft,
			 const float rim[4], const float look[3], unsigned int seed);
/* between the two: light zone unit's texture */
void procfx_planet_bind (int unit, unsigned int tex);
/* before each vertex: cos(sun), view cosine, model direction there */
void procfx_planet_attr (double c, double mu, const double m[3]);
void procfx_planet_end (void);

/* The game-over backdrop, a dark nebula filling the current
 * viewport under the tombstone (see endcard_frame in hdcontrols.c), faded
 * up from black by fade 0..1.  Changes nothing if the shader fails. */
void procfx_endgame_backdrop (float fade);

/* --flat-stars: the game's own star discs */
extern int flat_stars;

/* What a star is, by the model word the game draws it with (hdback.c
 * picks its backdrops by these too). */
enum light { L_NONE, L_RED, L_ORANGE, L_YELLOW, L_WHITE, L_BLUE, L_MAX };
int is_star (int model);
enum light star_light (int model);

/* Put side, in the circle hcalls: which star the disc belongs to (model
 * 0: none, not a star, or not the flight view). */
struct procfx_star { int model; unsigned int seed; };
void procfx_star_put (struct procfx_star *s);
/* Draw side, before the disc of radius r at c (GL eye coordinates): the
 * half side of the square to fill about c with the program bound, then
 * procfx_corona_end; 0, nothing bound, for none. */
float procfx_corona_begin (const struct procfx_star *s, const float c[3], float r);
void procfx_corona_end (void);

/* a layer's program: the shared noise library plus body, its main();
 * 0, after a note on stderr, when it does not compile (skies.c too) */
unsigned int procfx_program (const char *name, const char *body);

/* change_vidmode, before the GL context goes */
void procfx_forget (void);

#endif
