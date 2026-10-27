# Procedural detail: more quality, little code

The question: where can procedural generation make the port look better
without building something big?  The answer: a few small shader layers.
Each sits on a primitive the game already draws, and they all share one
noise library.  The first is built: the Milky Way, switched off with
`--flat-sky`.

## What is already procedural

- **The core owns identity.**  Systems, stars, bodies and seeds come from
  the game.  A body's seed is word 118 of its object.
- **Planet surfaces.**  `planet.c` rebuilds the game's own surface: the
  features, the XOR fill, and the coastlines seeded by the planet
  (L3cd9c_ProjectPlanet).  Exact, not invented.
- **The sky.**  The background object (model `$b0`, L44200_DrawBGStars)
  draws point stars and the Milky Way.  The Milky Way is a ring of flat
  polygons with Bezier outlines, within about 13 degrees of one great circle.
- **Identity tables.**  `hdback.c` maps models to a star class
  (`star_light`) and a body class (`body_class`).  This gives the hierarchy
  every layer needs: star, then body, then detail.

## How to add quality without bloat

1. **One noise library, not one per layer.**  `procfx.c` holds `pf_hash`,
   `pf_noise` and `pf_fbm` once, in GLSL.  A layer's shader is that text
   plus its own `main()`.
2. **One program helper.**  `procfx_program` compiles every layer.  If it
   fails, the layer is off and the old look stays.  Fold the ship work's
   `fx_compile` into it later.
3. **Detail in the shader, not in files.**  Add an asset file only if it
   replaces a lot of code.
4. **Game state, at run time, read only.**  Seeds, model words, rotations,
   light and colours come from ST memory.  Nothing is written, so the
   oracle cannot change.  The star class picks the palette, the body class
   the effect, the seed the detail.
5. **One off switch per layer.**  `--flat-<thing>` skips the hook, so the
   old draw path runs unchanged.
6. **A line budget.**  At most 150 lines per layer in `procfx.c`, and 40
   lines of hooks elsewhere.  Shared blocks do not count.  If a layer goes
   over, split it or drop it.
7. **Keep the game's shape.**  Draw in and around the game's primitive, and
   keep its colour on average.
8. **Prove it.**  The oracle end line is unchanged, and there are before
   and after `drive.py` shots using the switch.

## Shared building blocks

| Block | Lines | Used by |
|---|---|---|
| GLSL noise `pf_hash`, `pf_noise`, `pf_fbm` | ~25 | sky, corona, rings, close detail |
| `procfx_program`: compile, link, fail closed | ~30 | every shader layer |
| Object reader: record at a6-212, model at +90, rotation at a6-36 | ~20 | sky, rings, corona |
| Glow quad on `billboard_begin` (gl.c) | ~25 new | corona, limb glow |
| `star_light` and `body_class`, moved out of hdback.c | 0 new | corona, limb, backdrops |

## Candidates, ranked by quality per line

| # | Layer | Quality gain | Lines | Shared helpers | Risk |
|---|---|---|---|---|---|
| 1 | Planet limb glow | Every planet seen from space gets a lit, coloured rim instead of a flat disc. | ~80 | noise, program, glow quad, body class | Low |
| 2 | Star corona by class | The star becomes a glowing sun with soft streamers, not a flat disc. | ~60 | noise, program, glow quad, object reader, star class | Low |
| 3 | **Sky band (built)** | The flat blue cut-outs become soft star clouds with dust lanes, on every space screen. | 137 + 45 hooks | noise, program, object reader | Low |
| 4 | Planet rings | Probably also a fix: rings look to be black discs today. They would become banded and see-through. | ~90 | noise, program, object reader | Medium |
| 5 | Backdrop variation | Ports of the same class stop looking identical. | ~30 | identity tables | Low |
| 6 | Close planet detail | The ground keeps its grain when flying low. | ~60 + seams | noise, program | Medium |

All six only read memory and draw GL, so the risk to the core and the
oracle is nil.  The risk column is about visuals and code.

- **Limb glow.**
  - Hook: `Nu_DrawPlanet`, after `draw_planet_sphere`, drawn additive.
  - State: the outer `atm_col`, taken before `planet_atmosphere_band` zeroes `natm`; the sun vector; the body class.
  - Airless bodies get none.
  - Fade out while the near-atmosphere cones are drawn.
  - Verify: Lave from orbit.
  - Switch: `--flat-limb`.
- **Corona.**
  - Hook: `Nu_DrawTwinklyCircle`, with a model check in `Nu_PutTwinklyCircle`.
  - Streamers: `pf_fbm` over angle, seeded by the star.
  - Verify: red, yellow and white stars.
  - Switch: `--flat-stars`.
- **Rings.**
  - Path: L3d648_PutPlanetRings (fe2.s 17221) → L3b9aa_FilledOvalThingy (14525) → L37fb2_ProjectOvalXYZ (9464) → `Nu_PutOval`.
  - `Nu_DrawOval` draws black, and its rotation code is commented out.
  - Orient with the object reader, and band with `pf_fbm` seeded by the planet.
  - First check the oval opcode's other users.
  - Verify: needs a save in a ringed system.
  - Switch: `--flat-rings`.
- **Backdrops.**
  - Hook: `pick` in hdback.c.
  - Flip, hue and crop from a seed of the port.
  - Switch: `--same-backdrops`.
- **Close detail.**
  - Hook: `draw_planet_sphere`.
  - 3D `pf_fbm` on the sphere position, so there are no cube seams.
  - Overlaps the parked planet-fidelity work, so do it last.
  - Switch: `--flat-detail`.

**Recommendation.**  Ship the sky band, then limb glow, then corona.  With
the shared blocks that is about 450 lines.  Do rings once a ringed save
exists.

## Left out, and why

- **Voxel terrain and quadtree LOD landing.**  Thousands of lines, and they
  would replace a surface `planet.c` already reproduces exactly.
- **Galaxy simulation, or new stars and bodies.**  The core owns the
  galaxy.  Anything new disagrees with the map, the missions and the oracle.
- **Ray-marched nebulae.**  Big, costly on the GPU, and anchored to no game
  state.  The sky band gives most of that look cheaply.
- **Asteroid instancing.**  The game has no state for them.  Objects you
  cannot hit or scan contradict the simulation.
- **Noise on hulls.**  That belongs to gl/hull-ships and gl/ship-remaster.
- **Bloom and HDR.**  They need framebuffer objects and a pipeline rework.
- **AI textures.**  They cost money and add asset files.

## Prototype notes: the sky band

- **Size.**  `src/procfx.c` is 230 lines: 93 shared, 137 for the layer.
  `src/procfx.h` is 39 lines.  Hooks: gl.c +39, main.c +3, soft.c +1,
  screen.h +1, and one word in Makefile-C.
- **Put.**  `Nu_ComplexStart` checks the object's model.  For `$b0`, the
  shape's data gets an `NU_SKYBAND` znode carrying the object's rotation.
  If the shape aborts, the znode goes with it.
- **Draw.**  The shader rebuilds each pixel's eye ray from the polygon's
  projection and turns it into the sky's own frame, so the pattern stays
  fixed to the sky.  This was checked: band vertices keep their sky
  direction while the view turns.
- **Soft edge.**  A fade by distance from the band's plane straightened
  the game's winding ribbon, so it was dropped.  Instead, `flush_contour`
  draws a strip outside each outline, 3% of the screen high, fading to
  nothing.  Where two patches meet, the strip lies on the same colour and
  does not show.
- **Oracle.**  End line unchanged, `end 3100 g=14a8e1f0b71dbab4
  m=0ec88beb674ab6e9 c=dcad3c8a8bf9c46d s=cbf29ce484222325
  r=cbf29ce484222325 f=e2cf48f9f4d07294 p=d0b676b2ffc4d3cb`.
- **Switch.**  `--flat-sky` writes no znode and binds no shader.
- **Not yet checked.**  Other systems' band colours, and slow GPUs (9 noise
  octaves per band pixel).  The wireframe renderer is untouched.
