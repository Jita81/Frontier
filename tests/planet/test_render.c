/*
 * S-9: draw 20 different planets (more than either the texture cache in
 * gl.c or the surface cache in planet.c holds) through the real planet
 * code of gl.c, under AddressSanitizer, including the case S-9 is about:
 * one planet kept on screen, close up and moving, while the others push
 * its parsed surface out of planet.c's cache. Textures are dropped half
 * way through as after a video mode change (#12) and at the end, and must
 * all be gone then.
 *
 * V-1/V-2: the atmosphere limb comes from the body's own model data. A
 * model with an atmosphere list gets its layers read and drawn; the same
 * body without one gets no layers and no limb geometry at all. And it is
 * drawn where the ST draws it (planet_atmosphere_band): not for a planet
 * small on the screen, nor for one that fills it, and the band reaches as
 * far as the ST's formula says.  Both with the smooth planets of procfx
 * layer 2 (its stub says the shader is there) and with --flat-planets.
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
	planet_atmosphere_band (&p);
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

/* V-1/V-2: lay out a planet model in fake ST memory, with or without an
 * atmosphere list between the header and the features (the layout
 * planet_read_atmosphere reads), put the planet at dist planet radii,
 * angle off the line of sight, and count the limb geometry drawn. */
static long limb (int with, double dist, double angle, struct PlanetPrim *p)
{
	const int model = 0xc000, a3 = 0xd000, a6 = 0xe000;
	void *data;
	long before;
	int k;

	for (k = 0; k < 0x40; k += 2) MemWriteWord (model + k, 0);
	for (k = -256; k < 256; k += 2) {
		MemWriteWord (a3 + k, 0);
		MemWriteWord (a6 + k, 0);
	}
	make_planet (p, 0, dist, angle);
	p->flags = 0x80 | 0x40;	/* no extra header words before the list */
	if (with) {
		MemWriteWord (model + 8, 0x123);	/* atmosphere colour */
		MemWriteWord (model + 10, 0x4400);	/* layer: scale, colour */
		MemWriteWord (model + 12, 0x48f);
		MemWriteWord (model + 14, 0x4200);
		MemWriteWord (model + 16, 0x36f);
		MemWriteWord (model + 18, 0);		/* end of layers */
		p->feat_addr = model + 20;
	} else {
		p->feat_addr = model + 10;	/* colour word 0: no atmosphere */
	}
	planet_read_atmosphere (p, model, a3, a6, 0);
	planet_atmosphere_band (p);
	before = vertex_count;
	data = p;
	Nu_DrawAtmosphere (&data);
	return vertex_count - before;
}

/* the ST's own formulas for the near and far limb (l3d274, l3d2d4), in
 * pixels on the line from the screen centre through the planet centre:
 * 256 (rho z -+ r T) / (z^2 - r^2), T = sqrt (x^2 + y^2 + z^2 - r^2) */
static double st_limb (const struct PlanetPrim *p, int sign)
{
	double x = p->centre[0], y = p->centre[1], z = p->centre[2], r = p->R;
	double rho = sqrt (x*x + y*y), T = sqrt (x*x + y*y + z*z - r*r);
	return 256.0 * (rho*z + sign*r*T) / (z*z - r*r);
}

static int check_limb (void)
{
	static const struct { int with; double dist, angle; int want; const char *what; } t[] = {
		/* the limb near the centre of the screen: the bands show */
		{ 1, 1.5, 0.75, 1, "V-1: body with an atmosphere, limb on screen" },
		{ 0, 1.5, 0.75, 0, "V-2: body without an atmosphere, limb on screen" },
		/* the planet fills the screen: the $ff4e test */
		{ 1, 1.5, 0.0, 0, "V-1: body with an atmosphere, filling the screen" },
		/* small on the screen: the ST draws a circle, no atmosphere */
		{ 1, 20.0, 0.0, 0, "V-1: body with an atmosphere, small on screen" },
		/* moderate range: far + near under $199, the small path too */
		{ 1, 3.0, 0.4, 0, "V-1: body with an atmosphere, moderate range" },
		/* closer, the limb 74 pixels off the centre: the bands show */
		{ 1, 2.0, 0.8, 1, "V-1: body with an atmosphere, limb off centre" },
		{ 0, 2.0, 0.8, 0, "V-2: body without an atmosphere, limb off centre" },
		/* off the screen: near limb at 178 pixels or more */
		{ 1, 3.0, 1.2, 0, "V-1: body with an atmosphere, off screen" },
	};
	int i, l, bad = 0;

	for (i = 0; i < (int) (sizeof (t) / sizeof (t[0])); i++) {
		struct PlanetPrim p;
		long n = limb (t[i].with, t[i].dist, t[i].angle, &p);

		printf ("%s: %d layers, %ld limb vertices\n", t[i].what, p.natm, n);
		if (t[i].want ? (p.natm != 2 || n == 0) : n != 0)
			bad++;
		/* the band against the ST's limb formulas: the near side of
		 * the layer's cone projects to near - (f-1) 6 far, the width
		 * measured on the ST's screen (see planet_atmosphere_band) */
		for (l = 0; l < p.natm; l++) {
			double x = p.centre[0], y = p.centre[1], z = p.centre[2];
			double gam = atan2 (sqrt (x*x + y*y), z);
			double edge = st_limb (&p, -1), near = floor (edge), far = floor (st_limb (&p, 1));
			double got, want;

			if (far + near >= 1536.0) far = 1536.0;
			want = edge - (p.atm_scale[l] - 1.0) * 6.0 * far;
			got = 256.0 * tan (gam - p.atm_th[l]);
			if (fabs (got - want) > 0.5) {
				printf ("V-1: layer %d edge at %.2f pixels, ST formula %.2f\n", l, got, want);
				bad++;
			}
		}
	}
	return bad;
}

/* V-1 against the screen: Lave from Lave Station, front view, in the
 * software renderer. Its near limb is 159.3 pixels off the centre, the
 * far one 569, and its four layers' band edges were sampled at 153.4,
 * 150.3, 145.2 and 138.1 pixels along the line to the planet centre. */
static int check_lave (void)
{
	static const double scale[4] = { 1.0018, 1.0027, 1.0042, 1.0063 };
	static const double seen[4] = { 153.4, 150.3, 145.2, 138.1 };
	double a = atan (159.3 / 256.0), b = atan (569.5 / 256.0);
	double gam = (a + b) / 2, thp = (b - a) / 2, d = 1.0e8 / sin (thp);
	struct PlanetPrim p;
	int l, bad = 0;

	memset (&p, 0, sizeof (p));
	p.R = 1.0e8;
	p.centre[0] = d * sin (gam);
	p.centre[2] = d * cos (gam);
	p.natm = 4;
	for (l = 0; l < 4; l++) p.atm_scale[l] = scale[l];
	planet_atmosphere_band (&p);
	if (p.natm != 4) {
		printf ("V-1: Lave from Lave Station, %d layers, want 4\n", p.natm);
		return 1;
	}
	for (l = 0; l < 4; l++) {
		double got = 256.0 * tan (gam - p.atm_th[l]);
		printf ("V-1: Lave layer %d edge at %.1f pixels, the ST drew it at %.1f\n", l, got, seen[l]);
		if (fabs (got - seen[l]) > 1.0) bad++;
	}
	return bad;
}

int main (void)
{
	int round, i, frame;

	for (i = 0; i < NPLANETS; i++) random_features (0x1000 + i * 0x700, 0x6ff);

	for (round = 0; round < 2; round++) {
		/* the smooth path (procfx layer 2) first, then --flat-planets */
		flat_planets = round;
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
	if (check_limb ()) return 1;
	flat_planets = 0;
	if (check_limb ()) return 1;
	if (check_lave ()) return 1;
	printf ("S-9: %d planets drawn in 2 rounds, %d texture uploads, %d GL misuses, "
		"0 textures left\n", NPLANETS, tex_uploads, gl_errors);
	return gl_errors != 0;
}
