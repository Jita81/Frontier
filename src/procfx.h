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
/* Layer 2: the game-over backdrop, a dark nebula filling the current
 * viewport under the tombstone (see endcard_frame in hdcontrols.c), faded
 * up from black by fade 0..1.  Changes nothing if the shader fails. */
void procfx_endgame_backdrop (float fade);

/* change_vidmode, before the GL context goes */
void procfx_forget (void);

#endif
