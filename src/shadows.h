/*
 * Contact shadows for the GL renderer: the player's ship, landed at a
 * surface port, sits on a soft dark blob on the ground (see
 * docs/hd/procedural.md, "Contact shadows").  Reads ST memory, never
 * writes it.  gl.c sorts the blob into the 3D view just behind the ship's
 * farthest face, so it lies over the pad and under the hull.
 */
#ifndef _SHADOWS_H
#define _SHADOWS_H

extern int flat_shadows;	/* --flat-shadows (main.c): none */

/* the node data shadows_write fills and shadows_draw reads */
#define SHADOW_BYTES	(10 * sizeof (float))

/* At the engine's 3D view init; hd: the GL renderer with the HD look. */
void shadows_begin (int hd);

/* A face, strut or wheel was put: its n vertex records in the registers
 * from first (REG_A0, REG_A2), r its radius (a wheel's), the drawing
 * frame in a6.  1 when it is the landed player ship's. */
int shadows_put (int first, int n, int r);

/* After the frame's faces: the blob into data (SHADOW_BYTES); 0 if none. */
int shadows_write (void *data);

/* The draw function of the node; advances *data past it. */
void shadows_draw (void **data);

#endif
