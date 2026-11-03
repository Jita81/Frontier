/*
 * Scenery round a surface port for the GL renderer: sparse, seeded pines and
 * shrubs (terran worlds) or ice spires (ice worlds) on the port's own flat
 * green ground, clear of its buildings, roads and pads (docs/hd/props.md).
 * Off by default: --props on, --flat-props always off.  Reads ST memory only.
 */
#ifndef _PROPS_H
#define _PROPS_H
extern int flat_props, props_opt_in;	/* main.c */
#define PROPS_BYTES	sizeof (int)	/* a node's data: the prop */
void props_begin (int hd);		/* 3D view init; hd: GL with the HD look */
void props_shape (unsigned int rgb444);	/* a complex shape opens */
/* n vertex records in the registers from first: of the open complex shape
 * (solid 0) or a triangle or quad (solid 1); tree: the z tree being filled */
void props_put (int first, int n, int solid, void *tree);
int props_write (void);			/* after the puts: how many props to ask for */
int props_node (int i, void **tree, unsigned int *z);	/* prop i's node; 0: none */
void props_draw (void **data);		/* the node's draw; advances *data */
#endif
