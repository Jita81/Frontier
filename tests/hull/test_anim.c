/*
 * Unit test for hull_anim_bind (src/hull.c): a face that moves against its
 * own model keeps its tile on the moving part. All geometry here is made
 * up (simple boxes); nothing comes from the game.
 *
 * Build and run: tests/hull/run.sh
 */
#include "../../src/hull.c"

#include <stdio.h>

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf ("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf (__VA_ARGS__); printf ("\n"); } } while (0)

static void quad (float w[4][3], float x, float ytop, float ybot, float z)
{
	float v[4][3] = { { x, ytop, z }, { -x, ytop, z }, { -x, ybot, z }, { x, ybot, z } };
	memcpy (w, v, sizeof v);
}

static float off (const float a[3], const float b[3])
{
	return hull_anim_dist (a, b);
}

/* a door whose bottom edge slides up to its top: the bottom corners keep
 * their tile position, the top ones move by the slide */
static void test_slide (void)
{
	float w[4][3], first[4][3];
	unsigned int f;
	int r = 0;

	hull_anim_reset ();
	for (f = 1; f <= 140; f++) {
		float ybot = -24000 + 350.0f * (f - 1);
		quad (w, 59000, 24500, ybot, 90000);
		r = hull_anim_bind (290, 0xcd2e3780u, 0xefbd2, f, 4, w, 1.0);
		if (f == 1) memcpy (first, w, sizeof w);
		CHECK (!(r & HULL_ANIM_FLAT), "slide frame %u drawn flat", f);
		CHECK (off (w[2], first[2]) < 1 && off (w[3], first[3]) < 1,
		       "slide frame %u: bottom edge left its tile (%g)", f, off (w[2], first[2]));
		CHECK (fabsf ((first[0][1] - w[0][1]) - (ybot + 24000)) < 1,
		       "slide frame %u: top edge not moved by the slide", f);
	}
	/* closed into a line; then the part's origin jumps in z under it */
	{
		float before[4][3], dz = -229000;
		quad (w, 59000, 24500, 24500, 90000);
		hull_anim_bind (290, 0xcd2e3780u, 0xefbd2, 141, 4, w, 1.0);
		memcpy (before, w, sizeof w);
		quad (w, 59000, 24500, 24500, 90000 + dz);
		r = hull_anim_bind (290, 0xcd2e3780u, 0xefbd2, 142, 4, w, 1.0);
		CHECK (!(r & HULL_ANIM_FLAT), "jump drawn flat");
		CHECK (off (w[0], before[0]) < 1 && off (w[2], before[2]) < 1,
		       "the origin's jump moved the tile (%g)", off (w[0], before[0]));
		/* and it stays put after */
		quad (w, 59000, 24500, 24500, 90000 + dz);
		hull_anim_bind (290, 0xcd2e3780u, 0xefbd2, 143, 4, w, 1.0);
		CHECK (off (w[0], before[0]) < 1, "tile moved the frame after the jump");
	}
}

/* still faces are not touched, and rounding noise does not move them */
static void test_still (void)
{
	float w[4][3], ref[4][3];
	unsigned int f;
	int i, k;

	hull_anim_reset ();
	for (f = 1; f <= 50; f++) {
		quad (ref, 34000, 8900, -8900, 34000);
		memcpy (w, ref, sizeof w);
		for (i = 0; i < 4; i++)
			for (k = 0; k < 3; k++) w[i][k] += (float) ((int) ((f * 7 + i * 3 + k) % 9) - 4);
		memcpy (ref, w, sizeof w);
		CHECK (hull_anim_bind (80, 0x459a3131u, 0x1000, f, 4, w, 1.0) == HULL_ANIM_KEPT,
		       "still face frame %u not plainly kept", f);
		for (i = 0; i < 4; i++)
			CHECK (off (w[i], ref[i]) == 0, "still face frame %u moved", f);
	}
}

/* a face whose corners go different ways is drawn flat */
static void test_deform (void)
{
	float w[4][3];
	unsigned int f;
	int r = 0;

	hull_anim_reset ();
	for (f = 1; f <= 20; f++) {
		quad (w, 50000, 20000, -20000, 0);
		w[0][1] += 900.0f * f;
		w[2][0] -= 1400.0f * f;
		r = hull_anim_bind (84, 0x1234u, 0x2000, f, 4, w, 1.0);
	}
	CHECK (r & HULL_ANIM_FLAT, "a face changing shape was not drawn flat");
}

/* two objects of one model draw the same face in different places: each
 * keeps its own position (no tile borrowed from the other) */
static void test_instances (void)
{
	float a[3][3] = { { -1260, 882, 0 }, { 1260, 378, 0 }, { 1260, 1890, 0 } };
	float b[3][3], w[3][3];
	unsigned int f;
	int i, k;

	hull_anim_reset ();
	for (i = 0; i < 3; i++) for (k = 0; k < 3; k++) b[i][k] = a[i][k] + (k == 0 ? 8800 : 0);
	for (f = 1; f <= 10; f++) {
		memcpy (w, f & 1 ? a : b, sizeof w);
		hull_anim_bind (84, 0x5610cc30u, 0x3000, f, 3, w, 1.0);
		CHECK (off (w[0], (f & 1 ? a : b)[0]) == 0, "instance moved, frame %u", f);
		memcpy (w, f & 1 ? b : a, sizeof w);
		hull_anim_bind (84, 0x5610cc30u, 0x3000, f, 3, w, 1.0);
		CHECK (off (w[0], (f & 1 ? b : a)[0]) == 0, "second instance moved, frame %u", f);
	}
}

/* a thin face that changes shape a lot every frame (a flickering beam):
 * flat every frame after the first, never plated by turns */
static void test_flicker (void)
{
	float w[3][3];
	unsigned int f;
	int r;

	hull_anim_reset ();
	for (f = 1; f <= 30; f++) {
		float z = 1000000 + (f % 3) * 60000.0f, y = (f % 2) * 3000.0f;
		float v[3][3] = { { 1300, 1700 - y, 22000 }, { 0, 0, z }, { 3200, 1000 + y, 21000 } };
		memcpy (w, v, sizeof w);
		r = hull_anim_bind (65608, 0x547ad297u, 0xee71e, f, 3, w, 1.0);
		if (f > 1) CHECK (r & HULL_ANIM_FLAT, "flickering face plated, frame %u", f);
	}
}

/* a blade spinning in its part's frame, an eighth of a turn a frame: no
 * move of the tile follows it, so it is flat, every frame after the first */
static void test_spin (void)
{
	float w[4][3];
	unsigned int f;
	int i, r;

	hull_anim_reset ();
	for (f = 1; f <= 24; f++) {
		float base[4][3] = { { 2742, -6620, -6142 }, { 2742, -6620, 0 },
				     { 31230, -25395, 18204 }, { 31230, -25395, 8989 } };
		double a = f * M_PI / 4;
		for (i = 0; i < 4; i++) {
			w[i][0] = (float) (base[i][0] * cos (a) - base[i][1] * sin (a));
			w[i][1] = (float) (base[i][0] * sin (a) + base[i][1] * cos (a));
			w[i][2] = base[i][2];
		}
		r = hull_anim_bind (65608, 0x2930642du, 0xeec6e, f, 4, w, 1.0);
		if (f > 1) CHECK (r & HULL_ANIM_FLAT, "spinning blade plated, frame %u", f);
	}
}

/* a copy of the face at half the size appears (a smaller sign on the
 * wall): it is a face of its own, not the first one changing shape */
static void test_scaled_copy (void)
{
	float a[4][3], b[4][3], w[4][3];
	unsigned int f;
	int r;

	hull_anim_reset ();
	quad (a, 2560, 1791, 767, 0);
	quad (b, 1280, 895, 383, 0);
	/* drawn after the first copy, then before it */
	for (f = 1; f <= 20; f++) {
		int j;
		for (j = 0; j < 2; j++) {
			int big = (j == 0) == (f < 12);
			if (!big && f < 4) continue;
			memcpy (w, big ? a : b, sizeof w);
			r = hull_anim_bind (288, 0x7eb450bfu, 0xef822, f, 4, w, 1.0);
			CHECK (r == HULL_ANIM_KEPT && off (w[0], (big ? a : b)[0]) == 0,
			       "%s copy touched, frame %u", big ? "full-size" : "half-size", f);
		}
	}
}

/* a whole part sliding (a stripe on the door) keeps its tile */
static void test_rigid (void)
{
	float w[4][3], first[4][3];
	unsigned int f;
	int i;

	hull_anim_reset ();
	for (f = 1; f <= 60; f++) {
		quad (w, 3000, 2000 + 370.0f * f, -2000 + 370.0f * f, 90000);
		hull_anim_bind (290, 0xacb2abc4u, 0xefbd2, f, 4, w, 1.0);
		if (f == 1) memcpy (first, w, sizeof w);
		for (i = 0; i < 4; i++) CHECK (off (w[i], first[i]) < 1, "rigid part frame %u drifted", f);
	}
}

int main (void)
{
	test_slide ();
	test_still ();
	test_deform ();
	test_instances ();
	test_rigid ();
	test_scaled_copy ();
	test_flicker ();
	test_spin ();
	if (fails) { printf ("hull tests: %d failed\n", fails); return 1; }
	printf ("hull tests: all passed\n");
	return 0;
}
