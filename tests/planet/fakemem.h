/*
 * A fake 68000 memory for the planet tests: 64 KiB that wraps, so a
 * feature list can run off the end of it. Stands in for the emulator's
 * MemRead* (m68000.h maps STMemory_Read* onto them).
 */
#ifndef FAKEMEM_H
#define FAKEMEM_H

#include "../../m68000.h"

static unsigned char fakemem[1 << 16];

char MemReadByte (p68K pos) { return (char) fakemem[pos & 0xffff]; }
short MemReadWord (p68K pos)
{
	return (short) ((fakemem[pos & 0xffff] << 8) | fakemem[(pos + 1) & 0xffff]);
}
int MemReadLong (p68K pos)
{
	return (int) (((unsigned) (unsigned short) MemReadWord (pos) << 16) |
		      (unsigned short) MemReadWord (pos + 2));
}
void MemWriteByte (p68K pos, int val) { fakemem[pos & 0xffff] = val; }
void MemWriteWord (p68K pos, int val) { MemWriteByte (pos, val >> 8); MemWriteByte (pos + 1, val); }
void MemWriteLong (p68K pos, int val) { MemWriteWord (pos, val >> 16); MemWriteWord (pos + 2, val); }
int GetReg (int reg) { (void) reg; return 0; }

/* xorshift32: the tests are reproducible from their seed */
static unsigned int rnd_state = 2463534242u;
static unsigned int rnd (void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}
static double rndf (double lo, double hi) { return lo + (hi - lo) * (rnd () / 4294967296.0); }

/* A feature list in the planet model format (see planet.c), with random
 * contents; about one in four comes out as raw random bytes instead. */
static void random_features (unsigned int addr, int maxbytes)
{
	unsigned int p = addr, end = addr + maxbytes - 1;
	int i, n;

	if ((rnd () & 3) == 0) {
		for (i = 0; i < maxbytes; i++) MemWriteByte (addr + i, rnd ());
		return;
	}
	while (p + 8 < end && (rnd () & 15)) {
		if (rnd () & 1) {
			/* circle */
			MemWriteByte (p++, 0x80 | (rnd () & 0x7c));
			for (i = 0; i < 4; i++) MemWriteByte (p++, rnd ());
		} else {
			/* polygon: code frac then vertices, 0 terminated */
			MemWriteByte (p++, (rnd () & 0x3c) | 4);
			MemWriteByte (p++, rnd () & 7);
			n = 2 + rnd () % 12;
			for (i = 0; i < n && p + 4 < end; i++) {
				int x = (signed char) rnd ();
				MemWriteByte (p++, x ? x : 1);
				MemWriteByte (p++, rnd ());
				MemWriteByte (p++, rnd ());
			}
			MemWriteByte (p++, 0);
		}
	}
	MemWriteByte (p, 0);
}

#endif
