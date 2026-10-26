/*
 * S-11 / S-21: feed the feature parser and the rasterizer 10,000 random
 * feature lists (structured and raw, some running off the end of memory)
 * and random patches, including degenerate ones (NaN, huge, tiny).
 * Built with AddressSanitizer and UBSan by run.sh: any report fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fakemem.h"
#include "../../src/planet.h"

static void random_patch (struct PPatch *pt)
{
	double a[3], b[3], l;
	int k, kind = rnd () % 16;

	do {
		for (k = 0; k < 3; k++) a[k] = rndf (-1, 1);
		l = sqrt (a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
	} while (l < 1e-3);
	for (k = 0; k < 3; k++) pt->c[k] = a[k] / l;
	b[0] = -pt->c[1]; b[1] = pt->c[0]; b[2] = 0;
	if (fabs (pt->c[2]) > 0.9) { b[0] = 0; b[1] = -pt->c[2]; b[2] = pt->c[1]; }
	l = sqrt (b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
	for (k = 0; k < 3; k++) pt->u[k] = b[k] / l;
	pt->v[0] = pt->c[1]*pt->u[2] - pt->c[2]*pt->u[1];
	pt->v[1] = pt->c[2]*pt->u[0] - pt->c[0]*pt->u[2];
	pt->v[2] = pt->c[0]*pt->u[1] - pt->c[1]*pt->u[0];
	pt->half = rndf (0.001, 2.5);
	pt->n = 1 + rnd () % 48;
	switch (kind) {
	case 0: pt->half = NAN; break;
	case 1: pt->half = 1e300; break;
	case 2: pt->half = 1e-300; break;
	case 3: pt->c[0] = NAN; break;
	case 4: pt->n = 0; break;
	case 5: pt->n = -5; break;
	}
}

int main (int argc, char **argv)
{
	int iters = argc > 1 ? atoi (argv[1]) : 10000, i, j, nulls = 0, fails = 0, ends = 0;
	static unsigned char codes[48 * 48];
	unsigned int count[16] = { 0 };

	for (i = 0; i < iters; i++) {
		unsigned int addr = rnd () & 0xffff;
		struct PSurf *s;
		struct PPatch pt;

		/* sometimes start right at the end of memory, to wrap */
		if ((i & 7) == 0) addr = 0x10000 - 1 - (rnd () & 63);
		random_features (addr, 64 + rnd () % 2048);
		/* distinct seeds: every call parses afresh */
		s = psurf_get (addr, 0x9e3779b9u * (i + 1), rnd () & 0xffff, (int) (rnd () % 13) - 4);
		if (!s) nulls++;
		/* P-2: the end of the list is where it ends, and only there */
		{
			unsigned int limit = addr + 1 + rnd () % 4096;
			unsigned int e = psurf_features_end (addr, limit);
			if (e && (e <= addr || e > limit || psurf_features_end (addr, e) != e ||
				  psurf_features_end (addr, e - 1) != 0)) {
				printf ("features end %x for list at %x, limit %x\n", e, addr, limit);
				return 1;
			}
			if (e) ends++;
		}
		for (j = 0; j < 2; j++) {
			int n;
			random_patch (&pt);
			n = pt.n > 0 ? pt.n : 1;
			memset (codes, 0xaa, sizeof (codes));
			if (psurf_raster (s, &pt, codes) < 0) {
				fails++;
				if (pt.n > 0 && memchr (codes, 0xaa, n * n) != NULL) {
					printf ("codes not cleared on failure\n");
					return 1;
				}
			} else {
				int k;
				for (k = 0; k < n * n; k++) {
					if (codes[k] & 3) {
						printf ("code %02x is not a multiple of 4\n", codes[k]);
						return 1;
					}
					count[(codes[k] >> 2) & 15]++;
				}
			}
		}
		/* now and then a NULL surface, as gl.c passes when out of memory */
		if ((i & 255) == 0) {
			random_patch (&pt);
			if (pt.n > 0 && psurf_raster (NULL, &pt, codes) != -1) {
				printf ("NULL surface was not refused\n");
				return 1;
			}
		}
	}
	printf ("S-11: %d feature lists, %d patches failed cleanly, %d NULL surfaces, "
		"%u texels coded 0, %u non-zero\n", iters, fails, nulls, count[0],
		count[1] + count[2] + count[3] + count[4] + count[5] + count[6] + count[7] +
		count[8] + count[9] + count[10] + count[11] + count[12] + count[13] +
		count[14] + count[15]);
	printf ("P-2: %d of %d feature lists ended before their limit\n", ends, iters);
	return 0;
}
