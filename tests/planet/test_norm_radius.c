/*
 * S-10: the radius normalization terminates, and agrees with the ST's
 * l3cdd8, for every one of the 65,536 radius words.
 *
 * The ST doubles the word until bit 15 is set and then halves it once.
 * For 0x8000 the first doubling gives 0 and it never stops; 0 it skips.
 * psurf_norm_radius gives 0x4000 for both (planet.c).
 */
#include <stdio.h>
#include "fakemem.h"
#include "../../src/planet.h"

/* the ST loop, with a cap standing in for "never terminates" */
static int st_model (int w)
{
	unsigned int x = w & 0xffff;
	int i;

	if (x == 0) return -1;
	for (i = 0; i < 32; i++) {
		x = (x << 1) & 0xffff;
		if (x & 0x8000) return x >> 1;
	}
	return -1;
}

int main (void)
{
	int w, bad = 0, hangs = 0;

	for (w = 0; w < 0x10000; w++) {
		int m = psurf_norm_radius (w), st = st_model (w);
		if (m < 0x4000 || m > 0x7fff) {
			if (bad++ < 10) printf ("out of range: %04x -> %04x\n", w, m);
		}
		if (st < 0) {
			hangs++;
			if (m != 0x4000) {
				if (bad++ < 10) printf ("ST hang/skip %04x gave %04x, want 4000\n", w, m);
			}
		} else if (m != st) {
			if (bad++ < 10) printf ("mismatch: %04x -> %04x, ST %04x\n", w, m, st);
		}
		/* the sign-extended form the callers may pass gives the same */
		if (psurf_norm_radius ((short) w) != m) {
			if (bad++ < 10) printf ("sign extension changes %04x\n", w);
		}
	}
	printf ("S-10: 65536 radius words, %d where the ST does not finish, %d bad\n", hangs, bad);
	return bad != 0;
}
