/*
 * Game-boundary trace oracle (docs/07-game-intact-oracle.md).
 *
 * Host-side only.  Every entry point is a no-op unless an --oracle-* flag
 * (or --seed) was given, so a normal run is unchanged: oracle_install()
 * returns at once, hcalls[] is never touched and the SDL VBL timer starts
 * exactly as before.
 */
#ifndef ORACLE_H
#define ORACLE_H

enum oracle_mode {
	ORACLE_OFF = 0,
	ORACLE_REPLAY,
	ORACLE_RECORD
};

struct oracle_state {
	int active;            /* any oracle flag given */
	enum oracle_mode mode; /* replay or record (lockstep VBLs) */
};

extern struct oracle_state oracle;

/* Strip oracle flags from argv (before Main_ReadParameters).  Exits with
 * status 2 on a bad or conflicting flag, including --fullscreen together
 * with an oracle flag (07 section 3.2). */
void oracle_parse_args (int *argc, char *argv[]);

/* Install the hcall trampolines, open outputs, start the watchdog.  Called
 * once after Main_Init and the fullscreen toggle (07 section 3.8). */
void oracle_install (void);

/* True in replay and record: the SDL VBL timer must not be started. */
int oracle_lockstep (void);

#endif /* ORACLE_H */
