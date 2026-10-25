/*
 * E5 new star bases experiment (docs/experiments/EXP-05-new-starbase.md).
 * Both renderers, default OFF.
 *
 *   FE2_NEW_STARPORT=1   add one orbital station, "Merlin High", around the
 *                        planet Merlin in the Ross 154 system
 *
 * How fe2.s resolves a system's starports: the sysgen module (entry 7,
 * L4655a) writes a fresh list of 64-byte body records into a buffer on the
 * stack every time a system is needed (arrival, system map, system data),
 * so there is no lasting list to edit. Procedural systems are rolled from
 * the system seed (starports by L47164). Hand-made systems (Sol, Alpha
 * Centauri, Ross 154, ...) have d0 = 0 and take the path at l473ec, which
 * looks the system id up in a table of (id, offset) pairs at L47390 and
 * then expands the 64-byte source records found at L47390 + offset, up to
 * a record whose first long is 0 (the terminator, which also carries two
 * system fields). Source record layout:
 *    +0 long   orbit size (packed by the expander), 0 = end of list
 *    +4 words  seed, seed, parent (1-based index), body type, model
 *   +14 long   orbit, +18 word, +28/+32/+56 longs, +62 word: copied
 *   +33 byte   name flag, +34 name (NUL terminated)
 * Type 0x46 records (surface starports) are copied verbatim; types 0x40,
 * 0x42 and 0x44 are orbital stations.
 *
 * The overlay: at start-up, before the first 68k instruction, copy Ross
 * 154's source records to unused RAM above the program image, append one
 * station record and its terminator there, and point Ross 154's entry in
 * L47390 at the copy. Nothing else in RAM is changed, so every other
 * system's records, and the expander itself, are untouched. The new record
 * is the game's own orbital station record for Mayflower High (Wolf 630,
 * which orbits Landfall, a planet of the same body type as Merlin) with
 * only the parent and the name changed; " High" is the game's own suffix
 * for an orbital station named after the body it orbits (Mars High,
 * Mayflower High). Economy, allegiance and population are system-level
 * values that sysgen does not take from these records, so the station
 * inherits Ross 154's.
 *
 * Off: ExpBase_Init returns on the getenv test and RAM is never touched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../m68000.h"
#include "../host.h"
#include "exp.h"

int expbase_on;

#define IMAGE_END	0x83ac9		/* fe2.s.bin length; RAM above is free up to the stack */
#define OVERLAY_ADR	0x84000		/* zero in every dump taken (see the EXP-05 evidence) */
#define REC		64
#define MAX_RECS	48

#define ID_ROSS154	0xa925c5c1u
#define ID_WOLF630	0xa91dc583u

static const unsigned char l47390_sig[8] = { 0xa9, 0x25, 0xc6, 0x00, 0x00, 0x00, 0x03, 0xd2 };

static u32 find_table (void)
{
	u32 a;

	for (a = 0; a + sizeof (l47390_sig) < IMAGE_END; a += 2)
		if (!memcmp (m68kram + a, l47390_sig, sizeof (l47390_sig)))
			return a;
	return 0;
}

/* address of the (id, offset) pair for this system, or 0 */
static u32 find_entry (u32 table, u32 id)
{
	u32 a;

	for (a = table; a < table + 8 * 32; a += 8) {
		u32 k = (u32) rdlong (a);
		if (!k) break;
		if (k == id) return a;
	}
	return 0;
}

static int count_recs (u32 recs)
{
	int n = 0;

	while (n < MAX_RECS && rdlong (recs + n * REC)) n++;
	return n < MAX_RECS ? n : -1;
}

static u32 find_named (u32 recs, int n, const char *name)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp ((char *) m68kram + recs + i * REC + 34, name))
			return recs + i * REC;
	return 0;
}

void ExpBase_Init (void)
{
	const char *s = getenv ("FE2_NEW_STARPORT");
	u32 table, ross, wolf, rrecs, wrecs, donor, merlin, rec, a;
	int n, wn;

	if (!s || !*s || !strcmp (s, "0")) return;

	if (!(table = find_table ()) ||
	    !(ross = find_entry (table, ID_ROSS154)) ||
	    !(wolf = find_entry (table, ID_WOLF630))) {
		fprintf (stderr, "FE2_NEW_STARPORT: system table not found, off\n");
		return;
	}
	rrecs = table + (u32) rdlong (ross + 4);
	wrecs = table + (u32) rdlong (wolf + 4);
	n = count_recs (rrecs);
	wn = count_recs (wrecs);
	merlin = n > 0 ? find_named (rrecs, n, "Merlin") : 0;
	donor = wn > 0 ? find_named (wrecs, wn, "Mayflower High") : 0;
	if (!merlin || !donor || rdword (donor + 10) != 0x42 ||
	    strcmp ((char *) m68kram + rrecs + 34, "Ross 154")) {
		fprintf (stderr, "FE2_NEW_STARPORT: records not as expected, off\n");
		return;
	}
	for (a = OVERLAY_ADR; a < OVERLAY_ADR + (n + 2) * REC; a++)
		if (m68kram[a]) {
			fprintf (stderr, "FE2_NEW_STARPORT: overlay RAM in use, off\n");
			return;
		}

	/* the system's own records, the new one, then the original terminator */
	memcpy (m68kram + OVERLAY_ADR, m68kram + rrecs, n * REC);
	rec = OVERLAY_ADR + n * REC;
	memcpy (m68kram + rec, m68kram + donor, REC);
	wrword (rec + 8, (merlin - rrecs) / REC + 1);	/* parent: 1-based record index */
	memset (m68kram + rec + 34, 0, REC - 34);
	strcpy ((char *) m68kram + rec + 34, "Merlin High");
	memcpy (m68kram + rec + REC, m68kram + rrecs + n * REC, REC);

	wrlong (ross + 4, OVERLAY_ADR - table);
	expbase_on = 1;
	fprintf (stderr, "FE2_NEW_STARPORT: Ross 154 entry at 0x%x now -> 0x%x (%d+1 records, parent %d)\n",
		ross, OVERLAY_ADR, n, rdword (rec + 8));
}
