/*
 * HD backdrop for the GL renderer: while the player is docked or landed
 * and a flat 2D page is up (station services, market, shipyard, bulletin
 * board, ...), a photographic picture of where the ship is sits behind
 * the page instead of the flat page colour.
 *
 * Where the ship is comes from the game's own state, read from ST memory
 * and never written (the oracle hashes that): the starport docked with,
 * whether it is an orbiting station or a surface port, the body it orbits
 * or stands on and the star that lights that body.  These give a key -
 * kind x body class x light - that art/backdrops/manifest.txt maps to a
 * picture.  A key without a picture gets the old flat colour: a picture
 * of the wrong planet is worse than none.  See docs/hd/backdrop.md.
 *
 * The page fill itself needs nothing taken out of the screen: the GL
 * renderer already fills it with the transparent index and paints it with
 * glClear, so a picture drawn straight after glClear shows exactly where
 * that colour would.  But the game also draws the page colour itself, as
 * the margin round each button and arrow: on the flat page that margin is
 * invisible, over a picture it is a purple block.  hdback_frame takes those
 * pixels out of the upload copy (never ST memory) when they touch the
 * page fill, and leaves page colour that something encloses (a button's
 * face, an arrow) as it is.  Text, faces, bitmaps and the console stay on
 * top.
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

/* The frame hook, after the other HD layers' (draw_control_panel).  raster
 * is the game's 320x200 screen, read only; out is the copy uploaded to GL,
 * where 255 is transparent.  Only when hdback_draw put a picture down for
 * this frame and the screen is still a flat page: the page-colour pixels
 * joined to the page fill become 255 in out, rows 0-167 only. */
void hdback_frame (const unsigned char *raster, unsigned char *out);

/* Nonzero when hdback_draw put a picture down for the frame now being
 * built; valid from that call until the next one (hdback_frame does not
 * reset it).  hdui asks, so that its base lets the picture show through. */
int hdback_active (void);

/* Frosted glass: the picture hdback_draw put down for this frame, blurred
 * and graded once when it was loaded (its luma held near a fixed value
 * whatever the picture, see make_frost), for hdui to fill a glass shape
 * with before it lays the glass's tint over it.  begin returns 0 and
 * changes nothing unless a picture is up this frame (hdback_active) and
 * its frost was made.  Otherwise it binds the frost texture and turns on
 * object-linear texture coordinates such that a 2D vertex (z 0), under the
 * projection, modelview and viewport current at the call, gets the texture
 * coordinates the picture has at that window pixel: the frost registers
 * with the sharp picture.  rgb gets the tint of the light the picture was
 * drawn under, for the caller's glColor (texture modulate).  end puts back
 * every state begin changed. */
int hdback_frost_begin (float *rgb);
void hdback_frost_end (void);

/* After the HD frame hooks (GL): docked or landed, the front or rear view
 * bare, the port's concourse over the 3D view (docs/hd/hangar.md).  out:
 * the screen about to be uploaded; the gunsight is taken out of it. */
void hdback_concourse (int fb_w, int fb_h, unsigned char *out);

/* Nonzero when the last hdback_concourse took the gunsight out (the
 * concourse is up): hdinst draws no HD sight then.  Valid until the next
 * hdback_concourse, which runs every GL frame and resets it. */
int hdback_concourse_up (void);

/* The GL context is going away: drop the backdrop texture. */
void hdback_forget (void);

#endif /* _HDBACK_H */
