/*
 * Hit feedback for the HD flight view: when the player's ship is hit, a
 * brief glow at the edges of the view (a cyan shimmer when the shields
 * took it, a red pulse when the hull did), a slow red tint at the edges
 * while the hull is critical, and while it is damaged a small glass
 * readout of its shields and hull.
 *
 * Same contract as the other HD layers: everything is read from ST
 * memory, nothing is written (the oracle hashes that).  Only over a
 * flight view; off with --classic-ui, --pixel-art and the old renderer.
 */
#ifndef _HDDAMAGE_H
#define _HDDAMAGE_H

extern int flat_hud_hits;	/* --flat-hud-hits: one red flash, no tint (main.c) */

/* In gl.c's draw_control_panel, before hdtip_draw; fb_w x fb_h is the
 * framebuffer.  Leaves the GL state as it found it. */
void hddamage_draw (int fb_w, int fb_h);

/* The GL context is going away: drop this layer's texture. */
void hddamage_forget (void);

#endif /* _HDDAMAGE_H */
