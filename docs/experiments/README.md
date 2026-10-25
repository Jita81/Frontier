# Phase 2 experiments

Five experiments, run on branch `claude/phase2-unlock-455dca` while Phase 1 continues, to prove or disprove that the future features can be built. Each one adds a hook that is **off by default**; with it off, the game is byte-identical to e508341, shown at least once per experiment by screenshot md5s or traces. Screenshots containing Frontier Developments art are kept outside git.

| Experiment | Result | Hook side (prototype → production) | Effort | Recommended decision |
|---|---|---|---|---|
| [E1 UI](EXP-01-ui-host-calls.md) | **PROVED**: every screen is drawn through host calls; `FE2_TEXT=hires` redraws all UI text at window resolution, game memory unchanged | Host only → host only for text, bitmaps and GL 3D; `fe2.s` labels for span-drawn content (galaxy image, gauges) | M (L for a full redesign) | **Go with conditions**: host-side text and bitmap layer first, with the provenance check in CI; full redesign only after the span content is labelled |
| [E2 Sound](EXP-02-sound.md) | **PROVED**: every sound is a sample or track id; replacements play bit-exact | Host only → host only | S code, M with a licensed sound pack | **Go**, conditional on commissioning a licensed sound pack (33 effects, 8 tracks) |
| [E3 Ship redesign](EXP-03-ship-redesign.md) | **PROVED**: each 3D primitive is tied to its model; one ship redrawn with a new mesh from game data | Host only (stack inspection) → `ObjectBegin`/`ObjectEnd` host calls (4-line `fe2.s` patch) | M code, L with a full fleet of original art | **Go with conditions**: the host calls and depth pass first; original art only, never Elite Dangerous assets |
| [E4 NPC personalities](EXP-04-npc-llm.md) | **PROVED** against a local mock: NPC messages rewritten in four voices, fact-checked, cached, no frame cost; the live model was not called (no key set) | Host only (message parsing) → `Say` host call per message kind | M (L with two-way dialogue) | **Go with conditions**: one run against the live model first; ship a pre-generated cache; opt-in with a notice that text goes to the API |
| [E5 New star bases](EXP-05-new-starbase.md) | **PROVED**: Merlin High added in Ross 154; on the map beside its planet, docks, bulletin board with missions, market in line with the system | Host only (RAM overlay, hand-made systems) → `SysGenDone` host call at the end of sysgen (all systems) | M (L with building and improving stations in game) | **Go with conditions**: the host call and a station file first; derive the orbit from the parent body; in-game building is a separate design |

## Standing rules

- **Fidelity** (2026-10-25): modernised output is derived from game data and never contradicts it. Each EXP doc has a fidelity check under "Oracle implications".
- **Oracle**: run parity oracles with every experiment flag off. E1–E4 change only what is drawn or played; E5 changes the simulation of the one modded system, so a parity oracle may only compare systems without added stations.
- **Copyright**: `fe2.s`, its generated files, `sfx/`, `music/`, `savs/` and any Frontier Developments asset are never committed.

## Evidence

`evidence/E0n/` holds the capture scripts, identity checks and logs for each experiment. `tools/` holds helper scripts (the E2 audio generator).
