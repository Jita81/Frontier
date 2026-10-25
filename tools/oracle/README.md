# The game-intact oracle

This directory holds the tooling for the game-boundary trace oracle described in
[docs/07-game-intact-oracle.md](../../docs/07-game-intact-oracle.md) (07 below).

The oracle answers one question for a host-side change: **does the 68000 game
still do exactly what it did before?** It never looks inside the game. It
records what crosses the boundary between the translated 68000 code and the
host. That means every hcall entry and exit, the masked RAM at each VBL, a few
checkpoints, save/load bytes, and the 8-bit frame. It then hashes all of it into
streams that two builds can be compared on.

Everything here is **off by default**. A build with no `--oracle-*` flag runs the
game exactly as before:

- `oracle_install()` returns at once, so `hcalls[]` is not touched;
- the 20 ms VBL timer is added as before;
- the generated code (`fe2.s.c`, `gen/*.c`, `fixups.h`, `fe2.s.bin`) is
  byte-identical (see "Behaviour-neutral" below).

## Files

| File | What it is |
|---|---|
| `src/oracle.c`, `src/oracle.h` | Flags, trampolines, golden streams, tape record/replay, lockstep VBL, watchdog |
| `src/oracle_tramp.c` | 128 generated trampolines `tNN() { oracle_call(0xNN); }` |
| `tools/oracle/run.py` | Driver: `lint`, `filter`, `rundir`, `replay`, `compare`, `verdict`, `diverge` |
| `tools/oracle/test_run.py` | Unit tests for `run.py`, using synthetic streams only |
| `tools/oracle/fixtures/route.script` | Scripted live input used to record the route tape |
| `tools/oracle/fixtures/route.fe2rep` | A 3100-VBL (62 s) tape: intro skip, Mars, F-keys, mouse, quit |

## Game data: what may and may not be committed

`fe2.s`, `fe2.s.c`, `fe2.s.bin`, `fixups.h`, `gen/`, `sfx/`, `savs/`, the
`frontier` binary and screenshots of the game are **never** committed. Stage files
by name and never run `git add -A`.

What the oracle itself writes is safe to commit:

- **Tapes** (`*.fe2rep`) hold only ST key codes, mouse deltas and VBL numbers.
- **Stream files** hold only tags, decimal VBL numbers and hex hashes.
  `run.py filter FILE` checks this line by line. Run it before committing any
  stream file.

The run directory is a temporary directory that holds a **symlink** to
`fe2.s.bin`, never a copy.

## The game binary's flags

These flags are stripped from argv before the game's own parser sees them.
They are listed by `--oracle-help`; the main `--help` output is unchanged.

| Flag | Meaning |
|---|---|
| `--oracle-replay TAPE` | Replay a tape. VBLs are driven by the game's own Idle loop (lockstep, §3.3), not by the 20 ms timer |
| `--oracle-record TAPE` | Record live input to a tape, also in lockstep |
| `--oracle-out FILE` | **Required.** The golden stream file |
| `--oracle-script FILE` | Record mode only: SDL events pushed before HostUpdate `v` (keys, motion, buttons, quit) |
| `--oracle-detail A:B` | Also write a `G` line for every hcall entry and exit in VBLs A..B |
| `--oracle-sample N` | Write the `g`/`m`/`f`/`p` lines every N VBLs; the cumulative hash still covers every VBL |
| `--oracle-timeout S` | Wall-clock limit; when it is exceeded the trace is marked `invalid reason=timeout` |
| `--oracle-stall S` | No HostUpdate progress for S seconds gives `invalid reason=no-vbl-progress` (default 10) |
| `--oracle-test-hang V[:novbl]` | Test hook: hang inside HostUpdate at VBL V, or with `:novbl` stop the Idle-epoch rule at VBL V |
| `--seed N` | `srand` seed (replay default: the tape's seed; record default: 0) |
| `--pace` | Sleep to 50 Hz in Idle (record at human speed); the hashes do not change |

Other pins in oracle modes:

- `--fullscreen` is refused (exit 2);
- sound is forced off;
- an hcall that runs longer than 10 s gives `invalid reason=hung-hcall`;
- a SIGSEGV exits 139 with no `end` line, which `compare` treats as INVALID.

## Streams (07 §4.1)

| Tag | Content | Class |
|---|---|---|
| `g v h` | Hash of every hcall entry and exit in VBL v: regs, CCR, rdest, exception state, top 32 bytes of stack | game |
| `m v h` | RAM hash at HostUpdate, with the screen buffers masked | game |
| `c v t l548 l552 w14482` | Checkpoint words read through A6 (their meaning is **UNVERIFIED**) | game |
| `s v len h_data h_name` | `Fwrite` (save) length and hashes | game |
| `r v len h` | `Fread` (load) length and hash | game |
| `w v hc=..` | A watchdog VBL injection | game |
| `f v h` | The 8-bit frame, base and palettes at `Nu_DrawScreen` entry | frame |
| `p v h` | The presented raster after `Nu_DrawScreen` | frame |
| `stat ...` / `end v ...` | Counters and cumulative per-stream hashes | game |
| `invalid reason=...` | The trace cannot be trusted | → INVALID |

## run.py

```sh
R=tools/oracle/run.py
python3 $R lint tools/oracle/fixtures/route.fe2rep
python3 $R replay --bin ./frontier --tape tools/oracle/fixtures/route.fe2rep --out /tmp/a.out
python3 $R replay --bin ./frontier --tape tools/oracle/fixtures/route.fe2rep --out /tmp/b.out
python3 $R compare /tmp/a.out /tmp/b.out           # IDENTICAL
python3 $R verdict --baseline GOLD.out /tmp/a.out /tmp/b.out   # PASS / FAIL-* / ERROR-INFRA
python3 $R diverge --tape T --work DIR --bin-a OLD/frontier --bin-b NEW/frontier
python3 $R diverge --tape T --work DIR --bin-a ./frontier --bin-b ./frontier --args-b=--old-renderer
python3 tools/oracle/test_run.py
```

How the commands run:

- `replay` makes a fresh run directory. It holds a symlinked `fe2.s.bin`, an empty or
  copied `savs/`, and no `sfx/` or `music/`.
- `replay` sets `SDL_VIDEO_WINDOW_POS` (default `1500,800`), so several agents can
  share a desktop.
- `replay` enforces a subprocess timeout and kills only its own child.
- Run replays **one at a time**. Two GL replays at the same window position once
  produced `invalid reason=hung-hcall hc=68`. The watchdog marked it INVALID, which
  is correct, but it costs a re-run.

**Pairwise verdicts** (`compare`):

- IDENTICAL;
- DIVERGENT-GAME (any of `g m c s r w`, a counter, or the end VBL);
- DIVERGENT-FRAME (only `f`/`p`);
- INVALID (an `invalid` line, or no `end`).

**Scenario verdicts** (`verdict`, 07 §7.1):

| Verdict | When |
|---|---|
| PASS | All checks pass |
| FAIL-NONDET | The double run disagrees with itself |
| FAIL-GAME | A game stream or counter differs from the baseline, the key ring overflowed, `n_flagged` is not within one of `n_hostupdate`, or A6 changed |
| FAIL-FRAME | Only the frame streams differ |
| ERROR-INFRA | INVALID, or the artefact filter failed |

**Divergence report** (`diverge`, 07 §7.2) prints:

- the first divergent VBL and its stream;
- every divergent stream with its first line on each side;
- checkpoint deltas as numbers;
- the first divergent hcall boundary, with its name parsed from `src/hostcall.c`.

To find that boundary it re-runs both sides with `--oracle-detail v-5:v+1`.

## Reference results (route.fe2rep, GL build, macOS arm64)

```
end 3100 g=04240640be4bb4ad m=734f0b7cb04961c4 c=dcad3c8a8bf9c46d s=cbf29ce484222325 r=cbf29ce484222325 f=e2cf48f9f4d07294 p=d0b676b2ffc4d3cb
stat flagged=3100 hostupdate=3100 wd=0 wdv=- keyovf=0 a6=0000015e a6chg=0 draws=3106 screens=3 idle=036f70:1000,03709e:3600,0729f4:600,037082:999
```

The record run and two replays produce byte-identical stream files (excluding
the `#` header line, which names the oracle mode).

Replaying under `--old-renderer` gives **DIVERGENT-GAME**, by design (07 §3.7):

- **First divergence.** VBL 0, hcall #12, the exit of `0x73 Nu_IsGLRenderer`. The
  game asks which renderer it has and takes the other branch.
- **Divergent streams.** From that point `g` and `m` differ (`m` from VBL 1), and so
  do `f` and `p`.
- **Streams that agree.** `c`, `s`, `r` and every counter match over all 3100 VBLs.
- **The `Call_Memset`/`Call_MemsetBlue` fill difference.** These calls fill with
  0/0x0e under R_OLD and 0xff under GL (`src/hostcall.c` 74-109). They write only
  screen memory, which `m` masks, and their exit `g` hash agrees, so the difference
  shows up only in `f`.

Tapes are therefore per renderer mode: the tape header names the mode, and
replaying in another mode prints a warning.

## Reviewer sign-off

A reviewer signing off a host-side change against this oracle checks the
following, in order.

1. **Behaviour-neutral when off** (07 §14).
   - `shasum -a 256 fe2.s.c fe2.s.bin fixups.h` and the concatenated `gen/*.c`
     are unchanged by the PR.
   - The only edits outside oracle files are the listed ones:
     - `hcalls_len` and its `_Static_assert` in `src/hostcall.c`;
     - three gated lines in `src/main.c`: `oracle_parse_args`, `oracle_install`,
       and `if (!oracle_lockstep())` around `SDL_AddTimer`;
     - the two objects in `Makefile-C`.
   - A normal start, with no flags, reaches the intro and title as before.
2. **The oracle is deterministic.**
   - Two replays of each tape give `compare` → IDENTICAL.
   - `verdict` → PASS against the committed baseline.
   - FAIL-NONDET is never acceptable and is never blessed.
3. **Game streams are never blessed.**
   - A FAIL-GAME blocks the PR. The only exception is a declared
     `game-input-change` (07 §7.4) or a G4 68000 change, signed by the operator.
   - FAIL-FRAME is acceptable only with the `oracle:fb-bless` label, the named
     streams, and an image comparison (07 §7.3).
4. **Validity.** No stream file in the evidence carries an `invalid` line, and the
   watchdog counters (`wd`, `wdv`) match the baseline.
5. **No game data in the PR.** `git status --porcelain` shows only source, tapes and
   filtered stream files, and `run.py filter` passes on every committed stream.
6. **Record the sign-off.** In the PR, record:
   - the commands run, the verdict lines and the `end` line;
   - the commit SHA of both builds.

   A `signoff.json` (07 T-0.16) is not implemented yet.

## Not done yet (see 07 §12)

- **Streams.** The `d` (display list) stream is not written, and observe mode is
  not implemented.
- **Other build files.** Only `Makefile-C` links the oracle. `Makefile-mingw` and
  `Makefile-i386` do not list `oracle.o oracle_tramp.o` yet, so those builds will not
  link until they do.
- **CI and baselines.** There is no `baseline.lock`, mutation suite, `bless`,
  `bisect` or CI workflow yet. The stub harness (T-0.9) is also missing.
- **Watchdog.** The no-hcall spin injection path is written but has not been
  exercised by a scenario yet.
