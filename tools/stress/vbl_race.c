/*
 * vbl_race.c -- stress test of the exception (VBL) flag protocol, no game data.
 *
 * In the game FlagException (host.c) runs on SDL's timer thread and the
 * oracle's watchdog, while the 68000 thread checks ExceptionsPending at every
 * jump and takes exceptions with TakeException (host.h).  This links the real
 * host.c and includes the real host.h, runs PRODUCERS flagging threads against
 * one taking thread, and fails unless
 *   - the count is never negative after a take,
 *   - every exception flagged is taken exactly once (taken == flagged when
 *     the flaggers are done and the taker has drained), and
 *   - the pending bit and the count are both 0 at the end.
 * -DOLD_PROTOCOL swaps in the code from before the fix, copied verbatim
 * (FlagException from host.c, handle_exception as as68k/output_c.c emitted
 * it), which fails: the count goes negative, the bit sticks and the taker
 * runs away, as the game's clock did.
 *
 *   cc -O2 -pthread -I. tools/stress/vbl_race.c host.c -o vbl_race && ./vbl_race
 *   cc -O2 -pthread -I. -DOLD_PROTOCOL tools/stress/vbl_race.c host.c -o vbl_race_old && ./vbl_race_old
 *
 * Arguments: [flags per producer, default 50000000] [producers, default 2].
 * Build it outside the work tree or delete the binary: never commit it.
 */

#include <pthread.h>
#include "host.h"

#ifdef OLD_PROTOCOL
/* verbatim from host.c FlagException before the fix */
static void flag (int num)
{
	if (exception_handlers[num]) {
		exceptions_pending |= 1<<num;
		exceptions_pending_nums[num]++;
	}
}
/* the check at every jump and handle_exception's loop body, verbatim from
 * as68k/output_c.c before the fix (jdest/flag saving left out) */
#define pending()	(exceptions_pending)
static int take (int i)
{
	if (exceptions_pending & ((1<<i))) {
		if (!--exceptions_pending_nums[i]) {
			exceptions_pending ^= 1<<i;
		}
		return 1;
	}
	return 0;
}
#define PROTOCOL "old"
#else
#define flag(num)	FlagException (num)
#define pending()	ExceptionsPending ()
#define take(i)		TakeException (i)
#define PROTOCOL "new"
#endif

/* in the game the 68000 code between two checks is opaque calls into gen/,
 * so the compiler reloads the globals at every check: so does this */
#define opaque()	__asm__ __volatile__ ("" ::: "memory")

static long per_producer = 50000000;
static int producers_done;

static void *producer (void *arg)
{
	unsigned x = 2463534242u + (unsigned) (long) arg * 7919u;
	long k;
	int spin;
	for (k = 0; k < per_producer; k++) {
		flag (0);
		/* a short random pause keeps the count near 0, where the races are */
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		for (spin = x & 63; spin; spin--) opaque ();
	}
	__atomic_fetch_add (&producers_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

int main (int argc, char **argv)
{
	int nprod = 2, i, p;
	long taken = 0, last = 0, stale = 0, negative = 0, flagged;
	s32 n, min = 0;
	pthread_t th[16];

	if (argc > 1) per_producer = atol (argv[1]);
	if (argc > 2) nprod = atoi (argv[2]);
	if (nprod < 1 || nprod > 16 || per_producer < 1) {
		fprintf (stderr, "usage: vbl_race [flags per producer] [producers 1-16]\n");
		return 2;
	}
	flagged = per_producer * nprod;
	exception_handlers[0] = 0x1c;	/* FlagException flags only installed ones */

	for (p = 0; p < nprod; p++)
		if (pthread_create (&th[p], NULL, producer, (void *) (long) p)) return 2;

	/* the taker: one check per loop, as at every jump in the game; it keeps
	 * going until the producers are done and nothing is pending, or it has
	 * taken more than was ever flagged (a runaway) */
	for (;;) {
		int done = __atomic_load_n (&producers_done, __ATOMIC_ACQUIRE) == nprod;
		if (pending ()) {
			for (i = 0; i < 32 && !take (i); i++) ;
			if (i < 32) taken++;
			else stale++;		/* only a late bit: the fallback jump */
			n = __atomic_load_n (&exceptions_pending_nums[0], __ATOMIC_RELAXED);
			if (n < 0) negative++;
			if (n < min) min = n;
			if (i < 32 && n == 0) last++;
		} else if (done) {
			break;
		}
		if (taken > flagged) break;
		opaque ();
	}
	for (p = 0; p < nprod; p++) pthread_join (th[p], NULL);

	n = exceptions_pending_nums[0];
	printf ("%s protocol, %d producers: flagged %ld taken %ld (%+ld), "
		"takes that emptied the count %ld, late-bit fallbacks %ld\n",
		PROTOCOL, nprod, flagged, taken, taken - flagged, last, stale);
	printf ("  count after a take: negative %ld times, minimum %d; at the end "
		"count %d, pending 0x%x\n", negative, min, n, exceptions_pending);
	if (negative || min < 0 || taken != flagged || n != 0 || exceptions_pending) {
		printf ("FAIL\n");
		return 1;
	}
	printf ("PASS\n");
	return 0;
}
