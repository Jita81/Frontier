/*
 * skies: the HD finish of the planetary rings, see docs/hd/rings.md.
 *
 * A ringed body's rings are its model's own complex shapes, flat bands
 * put right after the body (Nu_PutPlanet).  They keep the game's shape,
 * place and colour; a fragment shader gives each band soft ringlets, a
 * gap or two seeded from the body, anti-aliased edges, a little more
 * light on the sun side, and lets the planet show through (alpha < 1).
 * Read only: every number comes from the hcall's registers and ST memory.
 * Off: --flat-rings.
 */
#ifndef _SKIES_H
#define _SKIES_H

extern int flat_rings;

/* what one ring shape needs, in GL eye coordinates: the body's centre in
 * units of its radius, the ring plane's normal, the sun's direction */
struct skies_ring { float c[3], n[3], sun[3]; unsigned int seed; };

/* Put side.  In Nu_PutPlanet: the body just put (view coordinates, as the
 * hcall reads them).  In Nu_ComplexStart: 1, with r filled in, when the
 * shape belongs to that body's object. */
void skies_planet_put (const double centre[3], double R, const double light[3]);
int skies_ring_put (struct skies_ring *r);

/* Draw side, around one such shape: proj and vp as it was projected with;
 * 0, nothing changed, when the shader is not available.  Each contour
 * goes to skies_ring_contour in window coordinates. */
int skies_ring_begin (const struct skies_ring *r, const double proj[16], const int vp[4]);
void skies_ring_contour (const double (*v)[3], int n);
void skies_ring_end (void);

#endif
