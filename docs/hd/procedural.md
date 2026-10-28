# Procedural detail: more quality, little code

The question: where can procedural generation make the port look better
without building something big?  The answer: a few small shader layers.
Each sits on a primitive the game already draws, and they all share one
noise library.  Three are built: the Milky Way (`--flat-sky`), the planet
limb glow (`--flat-limb`) and the star corona (`--flat-stars`).

## What is already procedural

- **The core owns identity.**  Systems, stars, bodies and seeds come from
  the game.  A body's seed is word 118 of its object.
- **Planet surfaces.**  `planet.c` rebuilds the game's own surface: the
  features, the XOR fill, and the coastlines seeded by the planet
  (L3cd9c_ProjectPlanet).  Exact, not invented.
- **The sky.**  The background object (model `$b0`, L44200_DrawBGStars)
  draws point stars and the Milky Way.  The Milky Way is a ring of flat
  polygons with Bezier outlines, within about 13 degrees of one great circle.
- **Identity tables.**  `procfx.c` maps models to a star class
  (`star_light`) and a body class (`body_class`).  They moved there from
  `hdback.c`, which still picks its backdrops with them.  This gives the
  hierarchy every layer needs: star, then body, then detail.

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
| GLSL eye ray `pf_ray`, from the viewport and projection | 7 | sky, limb glow |
| `procfx_program`: compile, link, fail closed | ~30 | every shader layer |
| Object reader `pf_object`, `pf_rot`, `pf_frame`: record at a6-212, model at +90, seed at +118, rotation at a6-36 | ~40 | sky, corona, limb glow, rings |
| Glow core `glow_begin`, `procfx_glow_end`: additive blend, state pushed and popped | 41 | corona, limb glow |
| Glow quad `glow_quad` on `billboard_begin` (gl.c) | 17 | corona, limb glow |
| `star_light` and `body_class`, moved out of hdback.c | 0 new | corona, limb, backdrops |

## Candidates, ranked by quality per line

| # | Layer | Quality gain | Lines | Shared helpers | Risk |
|---|---|---|---|---|---|
| 1 | **Planet limb glow (built)** | A planet with air seen from afar gets a lit, coloured rim instead of a flat disc. | 64 + 35 hooks | eye ray, program, glow core and quad, object reader, body class | Low |
| 2 | **Star corona by class (built)** | The star becomes a glowing sun with soft streamers, not a flat disc. | 60 + 34 hooks | noise, program, glow core and quad, object reader, star class | Low |
| 3 | **Sky band (built)** | The flat blue cut-outs become soft star clouds with dust lanes, on every space screen. | 116 + 33 hooks | noise, eye ray, program, object reader | Low |
| 4 | Planet rings | Probably also a fix: rings look to be black discs today. They would become banded and see-through. | ~90 | noise, program, object reader | Medium |
| 5 | Backdrop variation | Ports of the same class stop looking identical. | ~30 | identity tables | Low |
| 6 | Close planet detail | The ground keeps its grain when flying low. | ~60 + seams | noise, program | Medium |

All six only read memory and draw GL, so the risk to the core and the
oracle is nil.  The risk column is about visuals and code.

- **Limb glow.**  Built; see its prototype notes.
  - Hook: `Nu_DrawPlanet`, after `draw_planet_sphere`, drawn additive.
  - State: the outer `atm_col`, taken before `planet_atmosphere_band` zeroes `natm`; the sun vector; the body class.
  - Airless bodies get none.
  - Fade out while the near-atmosphere cones are drawn.
  - Verify: Lave from orbit.
  - Switch: `--flat-limb`.
- **Corona.**  Built; see its prototype notes.
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

**Recommendation.**  Sky band, limb glow and corona are built: 483 lines
in `procfx.c`, 78 in `procfx.h`, and the hooks.  Do rings once a ringed
save exists.

The limb glow and corona came in above their estimate in total: about 275
new lines against a target of 200.  The glow core, the eye ray and the
object reader's split are shared and count once.  Each layer is inside the
150 + 40 budget.

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

- **Size.**  When it landed, `src/procfx.c` was 243 lines: 93 shared, 150
  for the layer.  The glows later moved its object reading and eye ray
  into the shared part, so the layer itself is now 116 lines.
  `src/procfx.h` is 41 lines.  Hooks: gl.c +27 -2, main.c +3, soft.c
  +2 -1, screen.h +1, and one word in Makefile-C.  That is 33 hook lines.
- **Put.**  `Nu_ComplexStart` checks the object's model.  For `$b0`, the
  shape's data gets an `NU_SKYBAND` znode carrying the object's rotation.
  If the shape aborts, the znode goes with it.
- **Draw.**  The shader rebuilds each pixel's eye ray from the polygon's
  projection and turns it into the sky's own frame, so the pattern stays
  fixed to the sky.  This was checked: band vertices keep their sky
  direction while the view turns.
  - The band blends with `GL_MAX`: the brighter of band and sky wins, so
    overlapping patches and seams never add up.
  - `procfx_sky_begin` saves the enable and colour-buffer state and sets
    the blend equation itself.  `procfx_sky_end` restores both.
- **Soft edge.**
  - A fade by distance from the band's plane straightened the game's
    winding ribbon, so it was dropped.
  - The noise sets coverage, `smoothstep(0.10, 0.40, cloud)`.  Thin cloud
    fades out, so the visible edge follows the clouds, not the polygon's
    straight Bezier chords.
  - `procfx_sky_feather` draws a strip outside each outline, `FEATHER`
    (6%) of the 3D view's height wide: about 60 px at 1600x1200, where the
    view is 1008 px high.  Its alpha runs from 1 at the outline to 0 and
    scales the coverage.
  - Fans round the strip's corners.  Repeated vertices are skipped, so a
    patch's end cap is round, not a straight cut.
  - The outline is now only an upper bound for the fill.  The visible
    extent is set by the noise threshold.  Measured at blue >= 40 in the
    same frames against `--flat-sky`, the band is 50 to 97 px wider.  The
    old 3% strip made it 12 to 60 px wider.  Narrow tails widen most.
  - The strip's side comes from the contour's signed area, not the ODD
    rule, so a hole wound like its outline would keep a hard edge.  In the
    frames dumped here, no contour lay inside another.
  - Dust lanes are value noise and can run straight for a short way.  They
    lie inside the band, not on its edge.
- **Colour.**  Mean blue over the flat band's pixels, same frames, as flat
  / new / old:
  - front: 96.5 / 97.2 / 74.7
  - front, ship in view: 110.1 / 108.8 / 103.5
  - rear: 96.1 / 122.8 / 95.0, brighter where the cloud is dense
  - Up to 2% of the flat band's pixels fall back to the sky.
- **GPU.**  The review timed the first version on an M4 at 3400x1360
  (3D view 1813x1143).
  - 9 noise octaves per band pixel: 42 fps, against 51-53 with
    `--flat-sky`.  The band took 1.95 ms a frame, against 0.77 ms flat.
  - 4 + 3 octaves: 50-51 fps, 1.4 ms.
  - The layer now uses 4 + 3.  A pixel with no coverage is discarded
    before the lane noise.
- **Oracle.**  End line unchanged, `end 3100 g=14a8e1f0b71dbab4
  m=0ec88beb674ab6e9 c=dcad3c8a8bf9c46d s=cbf29ce484222325
  r=cbf29ce484222325 f=e2cf48f9f4d07294 p=d0b676b2ffc4d3cb`.
- **Switch.**  `--flat-sky` writes no znode, binds no shader and draws no
  strip.  Checked: with it, the band is the game's flat (0, 23, 96), as
  before this layer.
- **Not yet checked.**
  - Other systems' band colours.
  - This build's frame rate on an idle machine.  The machine here was
    loaded, so the 4 + 3 numbers above are the review's.
  - A real hole contour.
  - The wireframe renderer is untouched.

## Prototype notes: the game-over backdrop

- **What.**  The tombstone sequence used to sit on flat black.  It now
  sits on a dark nebula with a vignette, drawn from `pf_fbm` as a
  full-view pass (`procfx_endgame_backdrop`, about 20 lines of GLSL).
  It fades in over half a second.  The frosted title card that goes with
  it is in `hdcontrols.c` (`endcard_*`).
- **Detection.**  The pass is host-side and reads only.  It runs while
  the core's tombstone record is live and animating: the flag in the
  loop is set, the model is the stone's, its name matches the
  commander's, and its matrix or z changed within the last two frames.
  It does not fire in the intro, menus, flight, station pages or the
  death camera.
- **Switches.**  `--old-renderer`, `--classic-ui`, `--pixel-art` and
  `--pixel-text` keep the black stone with no card.

## Prototype notes: the limb glow

- **Size.**  64 lines in `procfx.c`.  Hooks: gl.c +29 (`PlanetPrim` 2,
  `Nu_PutPlanet` 6, `Nu_DrawPlanetGlow` 20, the draw table 1), main.c +3,
  screen.h +1, soft.c +2.  That is 35 hook lines.
- **Put.**  `Nu_PutPlanet` calls `procfx_limb_put` with the outer air
  layer (`atm_col`, `atm_scale`), before `planet_atmosphere_band` zeroes
  `natm`.  It stores colour times strength and a width in the planet's
  znode.  If the band keeps `natm`, so the game draws its own air, the
  width is set to 0.
  - Airless and star bodies get nothing (`body_class`).  Bodies with no
    air layer never call it.
  - Strength fades in as the game's own air rings fade out.  With near
    and far the limbs' distances from the screen centre in ST pixels:
    `1 - smooth(245, 409, near + far) * (1 - smooth(178, 230, near))`.
  - It also fades out close in: `smooth(1.3, 2.0, distance / radius)`.
  - Width: `(scale - 1) * 12` of the planet's angular radius, kept to
    0.04-0.12.  Thin dusty air ("rocky") gets 1.0 of the colour, the
    rest 1.6.
- **Draw.**  `Nu_DrawPlanetGlow` takes `Nu_DrawPlanet`'s place in the draw
  table.  It draws the planet, then a square about the planet's centre
  that holds the glow.
  - The shader works from the eye ray (`pf_ray`): a thin bright edge just
    inside the disc, a falloff out to 4 widths outside.
  - Lit by the sun: bright on the sun's side, a little past the
    terminator, dark on the night side.  Looking towards the sun adds some
    forward scatter.
  - Additive (`GL_ONE, GL_ONE`), cull face off.  `glow_begin` pushes the
    enable, colour-buffer and current state.  `procfx_glow_end` unbinds the
    program and pops it.
- **Checked.**
  - Mars and Lave from orbit: a rim in the game's own outer air colour
    (violet for both), lit on the sun's side, none on the night side.
    `--flat-limb` removes it and nothing else changes.
  - The Moon, airless: no glow.
  - Near Lave, where the game draws its own air rings: no glow.  With Lave
    centred, so the game draws no rings: the glow is there.
  - On the pad at London, inside the air: no glow.
  - A temporary draw counter (since removed): 25-35 limb glows a second in
    the Lave view, none with `--flat-limb`.  None on the galactic map, the
    system map, the other F2 pages, F3 or a station's services screen.
  - A shader broken on purpose: one log line, `limb glow shader does not
    compile, left flat`.  The planet looked as before and the game went on.
  - Oracle: end line unchanged.
- **Not checked.**
  - A nearer body in front of a glowing planet.  No such view was reached.
    The glow is drawn in its own planet's draw call, in the game's far to
    near order, so a nearer body drawn later covers it as it covers the
    planet.
  - Gas giants, ice, ocean and corrosive worlds, and ringed planets.
  - Frame time.
  - The old renderer has only the switch stub.

## Prototype notes: the corona

- **Size.**  60 lines in `procfx.c`.  Hooks: gl.c +28 (`put_star`,
  `draw_corona` and four calls), main.c +3, screen.h +1, soft.c +2.  That is
  34 hook lines.
- **Put.**  `Nu_PutTwinklyCircle` and `Nu_PutCircle` add the star after the
  disc's data: the model word of the object being drawn, and its seed
  (word 118).  The model check keeps it to stars (`$98`-`$ae`).  Every
  other disc gets model 0 and draws nothing.
- **Draw.**  `draw_corona` runs before the disc: a square of half side
  `3r - 0.016z`: the second term keeps a far star's glow at a fixed
  size on screen, as far as the game's twinkle reaches.  The
  game's disc and twinkle are drawn over it, so the disc stays readable.
  - Colour by `star_light`: red, orange, yellow, white or blue.
  - Streamers: `pf_fbm` over the direction from the disc, turned into the
    sky band's frame and offset by the star's seed.  They should stay
    fixed to the sky when the ship rolls (not checked).  The same star
    always gets the same streamers.
  - Same additive state and restore as the limb glow.
- **Checked.**
  - Sol (G, `$a0`): a bloom with streamers round a readable disc.  Over the
    violet band the yellow reads near white, because it adds to the band.
  - Sirius (A, `$a4`): a small blue-white corona.
  - Leesti (M dwarf, `$9a`): a small orange-red corona.
  - `--flat-stars`: the game's disc and twinkle only.
  - Draw counter: 32-34 coronas a second in the Sol view, none with
    `--flat-stars`.  None on the maps or the station screen.
  - On the pad at London, one run of three drew a corona for about 4 s.
    That is the 3D view with the Sun in its sky.  Where it was on screen
    was not found.
  - A shader broken on purpose: one log line, `corona shader does not
    compile, left flat`.  The disc looked as before.
  - Oracle: end line unchanged.
- **Not checked.**
  - Blue stars, giants and white dwarfs.
  - A planet in front of a star.  As for the limb glow, the game's draw
    order should cover it.
  - Streamers staying put while the ship rolls.
  - Frame time.
