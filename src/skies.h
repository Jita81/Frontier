/*
 * skies: the HD finish of the planetary rings, see docs/hd/rings.md.
 *
 * A ringed body's rings are its model's own complex shapes, flat bands
 * put right after the body (Nu_PutPlanet).  They keep the game's shape,
 * place and mean colour; a fragment shader gives each band ringlets, a
 * gap or two seeded from the body and anti-aliased edges.
 * Read only: every number comes from the hcall's registers and ST memory.
 * Off: --flat-rings.
 */
#ifndef _SKIES_H
#define _SKIES_H

extern int flat_rings;

/* what one ring shape needs, in GL eye coordinates: the body's centre in
 * units of its radius and the ring plane's normal */
struct skies_ring { float c[3], n[3]; unsigned int seed; };

/* Put side.  In Nu_PutPlanet: the body just put (view coordinates, as the
 * hcall reads them).  In Nu_ComplexStart: 1, with r filled in, when the
 * shape belongs to that body's object. */
void skies_frame_begin (void);	/* Nu_3DViewInit: forget the last body */
void skies_planet_put (const double centre[3], double R);
int skies_ring_put (struct skies_ring *r);

/* Draw side, around one such shape: proj and vp as it was projected with;
 * 0, nothing changed, when the shader is not available.  Each contour
 * goes to skies_ring_contour in window coordinates. */
int skies_ring_begin (const struct skies_ring *r, const double proj[16], const int vp[4]);
void skies_ring_contour (const double (*v)[3], int n);
void skies_ring_end (void);
void skies_forget (void);	/* before the GL context goes */

/*
 * puffs.c: the game's own low clouds (model 198) lit as cumulus.  Put
 * side: NU_PUFF and a puff_tag before a low cloud's complex shape (kind
 * 0) or face (kind 1): the model's frame (a view point v is M (v - o) in
 * the model's space), towards the sun in GL eye space and the object's
 * seed.  Draw side: puff_next before every primitive outside a shape,
 * puff_shape just before a tagged shape is tessellated, puff_face and
 * puff_corner around a tagged face, puff_frame at the end of the frame,
 * puff_forget before the GL context goes.
 */
struct puff_tag { int kind; unsigned int seed; float M[9], L[3]; int o[3]; };
int puff_on (void);
void puff_draw_tag (void **data);
int puff_next (int kind);
void puff_end (void);
void puff_frame (void);
void puff_forget (void);
int puff_shape (const double (*e)[3], int n, const double mv[16], const double pj[16], const int vp[4]);
int puff_face (const double (*p)[3], int n);
void puff_corner (int i);

#endif
