# HD weapon effects (`src/fx.c`)

With the GL renderer, lasers, missiles, explosions and shield, ECM and bomb
rings are drawn as soft glowing light instead of flat polygons. Everything
is procedural: three 64x64 greyscale textures (glow, puff, ring) made at
start-up, blended additively. There are no image files and no shaders.

`--flat-fx` turns all of it off and gives the base HD look. `--old-renderer`
is unaffected, since fx runs only under the GL renderer.

## How an object is recognised

At put time (`Nu_Put*` in `gl.c`) `fx_tag_for` reads the current object's
record (A6-212) and its model number. When the model is one fx knows, an
`NU_FX` op holding (kind, aux, record) goes in front of the primitive. The
draw side then lets the `fx_*` hooks replace that primitive. ST memory is
only read; the 68000 core is untouched and the oracle replay is unchanged.

| Model | What | HD look |
|---|---|---|
| 190 | laser beam | tapered additive ribbon (halo plus white-hot core), muzzle bloom, hit flare, short afterglow |
| 10-16 | missiles | engine plume ribbon, nozzle glow, nuke warning blink |
| 188 | explosion | flash and shock ring when new, then glowing puffs and embers |
| 194 | smoke | soft grey puffs that fade with age |
| 192 | shield | its polygons drawn additive |
| 200 | ECM / bomb | expanding ring (bomb larger and slower) |

## Each laser type looks different

The laser type byte (record +123) is hashed into the beam's halo width,
core width, afterglow length and flicker, so every type has its own style
without a table. The 1MW pulse laser (`$88`) is a crisp, steady red bolt.
The mining laser (`$9a`) is thin and flickers. The colour is always the
game's own beam colour.

## Afterglow

A beam lives for one game frame. fx keeps it in a 16-slot ring and fades it
over 50-95 ms of host time, so a pulse reads as a bolt rather than a flash
that one frame shows and the next does not. Flares and rings use the same
ring.

`FE2_FX_LOG=1` prints, to stderr, each model and laser type the first time
fx sees it.

## Limits

- No depth test: a beam or glow can show through a hull in front of it.
- Mines, mining sparks and a full-screen bomb flash are not done.
