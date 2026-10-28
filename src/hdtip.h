/*
 * Hover help for the HD control panel: rest the pointer on a part of the
 * panel (an icon, a time key, the clock, the gauges, the scanner, a
 * missile, the speed readouts over the 3D view) and after a moment a
 * small glass label says what it is and which key does the same.  When
 * the game would refuse it now, the label says why, read from the
 * game's own state.
 *
 * Same contract as the other HD layers: everything is read from ST
 * memory and the host's input state, nothing is written (the oracle
 * hashes that).  Off with --classic-ui, --pixel-art and the old renderer.
 *
 * FE2_HDTIP_LOG=1 prints, each time the pointer moves onto another
 * element, the state the label was worked out from.
 */
#ifndef _HDTIP_H
#define _HDTIP_H

/* Last thing each frame, after hdui_draw_over, in gl.c's ortho view.
 * Leaves the GL state as it found it. */
void hdtip_draw (void);

/* The GL context is going away: drop this layer's texture. */
void hdtip_forget (void);

#endif /* _HDTIP_H */
