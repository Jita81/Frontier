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
 */
#ifndef _PROCFX_H
#define _PROCFX_H

/* --flat-sky: the game's flat Milky Way band */
extern int flat_sky;

/* Put side, inside the hcall that starts a complex polygon: 1 when the
 * polygon belongs to the background object, with its rotation (nine 1.15
 * words of the object's frame, as floats) in m.  Reads only. */
int procfx_sky_put (float m[9]);

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

/* Around one textured planet.  lo, hi: each light zone's cos(sun) range
 * (9, 9 for none); soft: half width of the blend at a boundary; rim: the
 * innermost atmosphere colour, alpha 1 with an atmosphere; look: sun
 * shading on (1 lit), limb darkening, detail amplitude; seed: the body's.
 * 0, changing nothing, when off or when the shader is not available. */
int procfx_planet_begin (const float lo[4], const float hi[4], float soft,
			 const float rim[4], const float look[3], unsigned int seed);
/* between the two: light zone unit's texture */
void procfx_planet_bind (int unit, unsigned int tex);
void procfx_planet_end (void);
/* change_vidmode, before the GL context goes */
void procfx_forget (void);

#endif
