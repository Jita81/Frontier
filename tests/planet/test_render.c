/*
 * S-9: draw 20 different planets (more than either the texture cache in
 * gl.c or the surface cache in planet.c holds) through the real planet
 * code of gl.c, under AddressSanitizer, including the case S-9 is about:
 * one planet kept on screen, close up and moving, while the others push
 * its parsed surface out of planet.c's cache. Textures are dropped half
 * way through as after a video mode change (#12) and at the end, and must
 * all be gone then.
 *
 * run.sh cuts the block between the PLANET BEGIN and PLANET END markers
 * out of src/gl.c into planet_block.c, which this includes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define GL_SILENCE_DEPRECATION
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "fakemem.h"
#include "../../src/planet.h"
#include "gl_stubs.h"
#include "split_rgb.c"
#include "planet_block.c"

#define NPLANETS 20

static void make_planet (struct PlanetPrim *p, int i, double dist, double angle)
{
	int k;

	memset (p, 0, sizeof (*p));
	p->R = 6000.0 + 37.0 * i;
	p->centre[0] = sin (angle) * dist * p->R;
	p->centre[1] = 0.1 * dist * p->R;
	p->centre[2] = cos (angle) * dist * p->R;
	p->light[0] = 0.6; p->light[1] = 0.0; p->light[2] = 0.8;
	for (k = 0; k < 3; k++) p->A[k][k] = 1.0;
	p->feat_addr = 0x1000 + i * 0x700;
	p->seed = 0x1234567u * (i + 1);
	p->radius_word = 0x4000 + 0x123 * i;
	p->detail = i % 4;
	p->flags = (i & 3) == 3 ? 0 : 0x80 | (i & 1 ? 0x40 : 0) | (i & 2 ? 0x20 : 0);
	for (k = 0; k < 16; k++) p->table[k] = (k * 0x111 + i) & 0xfff;
	p->natm = i % 3;
	for (k = 0; k < p->natm; k++) {
		p->atm_scale[k] = 1.0 + 0.02 * (k + 1);
		p->atm_col[k] = 0x48f;
	}
}

static void draw (int i, double dist, double angle)
{
	struct PlanetPrim p;
	void *data;

	make_planet (&p, i, dist, angle);
	data = &p;
	Nu_DrawPlanet (&data);
	data = &p;
	Nu_DrawAtmosphere (&data);
}

static int check_tint (void)
{
	static const struct { unsigned short col, tint, want; } t[] = {
		{ 0x000, 0x123, 0x123 },
		{ 0xfff, 0x111, 0xfff },	/* the ST's word add would wrap */
		{ 0x0f0, 0x020, 0x0f0 },	/* no carry into red */
		{ 0x888, (unsigned short) -0x111, 0x777 },
		{ 0x101, (unsigned short) -0x222, 0x000 },
	};
	int i, bad = 0;
	for (i = 0; i < (int) (sizeof (t) / sizeof (t[0])); i++) {
		unsigned short got = rgb444_add_sat (t[i].col, t[i].tint);
		if (got != t[i].want) {
			printf ("S-23: %03x + %04x gave %03x, want %03x\n", t[i].col, t[i].tint, got, t[i].want);
			bad++;
		}
	}
	return bad;
}

int main (void)
{
	int round, i, frame;

	for (i = 0; i < NPLANETS; i++) random_features (0x1000 + i * 0x700, 0x6ff);

	for (round = 0; round < 2; round++) {
		/* every planet, far and then close up (the patch path) */
		for (i = 0; i < NPLANETS; i++) {
			draw (i, 4.0, 0.3 * i);
			draw (i, 1.1, 0.3 * i);
		}
		/* planet 0 stays on screen close up while the camera moves
		 * over it and the other planets go by */
		for (frame = 0; frame < 3 * NPLANETS; frame++) {
			draw (0, 1.05, 0.25 * frame);
			draw (1 + frame % (NPLANETS - 1), 3.0, 0.1 * frame);
		}
		/* a video mode change (#12) */
		planet_forget_textures ();
		if (tex_count != 0) {
			printf ("%d textures left after planet_forget_textures\n", tex_count);
			return 1;
		}
	}
	if (check_tint ()) return 1;
	printf ("S-9: %d planets drawn in 2 rounds, %d texture uploads, %d GL misuses, "
		"0 textures left\n", NPLANETS, tex_uploads, gl_errors);
	return gl_errors != 0;
}
