/*
 * HD backdrop for the GL renderer: while the player is docked or landed
 * and a flat 2D page is up (station services, market, shipyard, bulletin
 * board, ...), a photographic picture of where the ship is sits behind
 * the page instead of the flat page colour.
 *
 * Where the ship is comes from the game's own state, read from ST memory
 * and never written (the oracle hashes that): the starport docked with,
 * whether it is an orbital station or a surface port, the body it orbits
 * or stands on and the star that lights that body.  These give a key -
 * kind x body class x light - that art/backdrops/manifest.txt maps to a
 * picture.  A key without a picture gets the old flat colour: a picture
 * of the wrong planet is worse than none.  See docs/hd/backdrop.md.
 *
 * The flat page colour needs nothing taken out of the screen: the GL
 * renderer already fills it with the transparent index and paints it with
 * glClear, so a picture drawn straight after glClear shows exactly where
 * that colour would.  Text, faces, bitmaps and the console stay on top.
 */
#ifndef _HDBACK_H
#define _HDBACK_H

/* --pixel-art: no backdrop */
extern int pixel_art;

/* Straight after the frame's glClear (GL renderer only).  fb_w x fb_h is
 * the window's framebuffer.  Draws the backdrop, cover fit and dimmed,
 * when one is due; leaves the viewport, matrices and GL state as it found
 * them, the depth buffer untouched. */
void hdback_draw (int fb_w, int fb_h);

/* The GL context is going away: drop the backdrop texture. */
void hdback_forget (void);

#endif /* _HDBACK_H */
