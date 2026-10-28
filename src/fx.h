/* HD weapon effects for the GL renderer: see fx.c and docs/hd/weapons.md. */
#ifndef _FX_H
#define _FX_H

extern int flat_fx;	/* --flat-fx: none of this, the base HD look */
extern int flat_explosions;	/* --flat-explosions: the base explosions only */

/* Put side (ST memory is only read).  fx_tag_for: 1 when the object being
 * drawn has effects; tag (kind, aux, rec) then goes in front of its
 * primitive as an NU_FX op.  fx_laser_quad: 1 when the quad being put is a
 * laser beam, which fx has taken (the flat quad is not drawn). */
int fx_tag_for (unsigned int tag[3]);
int fx_laser_quad (void);
void fx_frame_begin (void);

/* Draw side.  fx_draw_tag reads the NU_FX op; fx_prim_done follows every
 * other primitive (1 after a complex start, 2 after its end).  The fx_X
 * hooks return 1 when they drew the tagged primitive themselves. */
void fx_draw_tag (void **data);
int fx_tagged (void);	/* a tagged primitive is next: no hull plating */
void fx_prim_done (int complex_mark);
int fx_tri (const float *a, const float *b, const float *c);
int fx_blob (const int *v, int rad, int r, int g, int b);
int fx_circle (const int *v, unsigned int d2, int r, int g, int b);
void fx_frame_end (void);	/* the afterglow ring, over the 3D view */
void fx_forget (void);		/* the GL context is going away */

#endif /* _FX_H */
