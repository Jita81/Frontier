/*
 * Scenery round a surface port for the GL renderer: a sparse, seeded ring
 * of pines and shrubs (terran worlds) or ice spires (ice worlds) on the
 * flat ground the port's own model draws (see docs/hd/props.md).  Off by
 * default: --props turns it on, --flat-props always off.  Reads ST memory,
 * never writes it.
 */
#ifndef _PROPS_H
#define _PROPS_H

extern int flat_props;		/* --flat-props (main.c): none, always */
extern int props_opt_in;	/* --props (main.c): on */

/* the node data props_draw reads: the set */
#define PROPS_BYTES	sizeof (int)

/* At the engine's 3D view init; hd: the GL renderer with the HD look. */
void props_begin (int hd);

/* A complex shape opens, its colour rgb444. */
void props_shape (unsigned int rgb444);

/* n vertex records in the registers from first, for the open complex
 * shape (solid 0) or a triangle or quad (solid 1); z and tree are the
 * znode the engine is filling. */
void props_put (int first, int n, int solid, unsigned int z, void *tree);

/* After the frame's puts: a bitmask of the sets (0 far, 1 near) that have
 * something to draw. */
int props_write (void);

/* The tree and z for a set's node; 0 when it has none this frame. */
int props_node (int set, void **tree, unsigned int *z);

/* The draw function of the node; advances *data past it. */
void props_draw (void **data);

#endif
