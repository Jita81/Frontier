# Procedural detail: more quality, little code

The question: where can procedural generation make the port look better
without building something big?  The answer: a few small shader layers.
Each sits on a primitive the game already draws, and they all share one
noise library.  Three are built:

| Layer | Owner | What it draws | Off |
|---|---|---|---|
| 1, sky band | procfx sky | star clouds and dust lanes in the Milky Way polygons | `--flat-sky` |
| 2, planets (#38) | procfx planet + gl.c | soft terminator, limb darkening, close detail, the lit limb rim (`uRim`) and the atmosphere halo (`draw_planet_halo`) | `--flat-planets` |
| 3, star corona | procfx corona | soft light and streamers round a star's disc, by class, flight view only | `--flat-stars` |

Down in the atmosphere, layer 2 keys its lit rim and its sun fall-off off
altitude: their weight (`planet_aloft` in gl.c) ramps from 0 on the ground
to 1 at 2% of the radius out, and stays 1 beyond.  Seen from space nothing
changes.  Near the surface every distant patch of ground is seen edge on,
like the limb, so the rim would wash it pale; there the game's own light
zones shade the ground and the sky gradient is the halo's job.

The game-over backdrop (#34) uses the same noise but is not a layer on a
game primitive.  A separate limb glow was prototyped on gl/procgen-glow
and dropped: layer 2 already owns the planet's rim and halo.

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
  (`star_light`, `is_star`); they moved there from `hdback.c` for the
  corona, and hdback still picks its backdrops with them.  `hdback.c`
  keeps the body class (`body_class`).  This gives the hierarchy every
  layer needs: star, then body, then detail.

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
| Object reader `pf_record`, `pf_frame`: record at a6-212, model at +90, seed at +118, rotation at a6-36 | ~30 | sky, corona, rings |
| Glow square on `billboard_begin` (gl.c `draw_corona`) | 19 | corona |
| `star_light` and `is_star`, moved out of hdback.c | 0 new | corona, backdrops |

## Candidates, ranked by quality per line

| # | Layer | Quality gain | Lines | Shared helpers | Risk |
|---|---|---|---|---|---|
| 1 | **Planets (built, with 6)** | Every textured planet gets a soft terminator, a darker limb, a lit rim and a soft halo from its own atmosphere layers. | 85 + 210 in gl.c | noise, program | Low |
| 2 | **Star corona by class (built, layer 3)** | The star becomes a glowing sun with soft streamers, not a flat disc. | ~95 + ~45 hooks | noise, program, glow square, object reader, star class | Low |
| 3 | **Sky band (built)** | The flat blue cut-outs become soft star clouds with dust lanes, on every space screen. | 150 + 33 hooks | noise, program, object reader | Low |
| 4 | Planet rings | Probably also a fix: rings look to be black discs today. They would become banded and see-through. | ~90 | noise, program, object reader | Medium |
| 5 | Backdrop variation | Ports of the same class stop looking identical. | ~30 | identity tables | Low |
| 6 | **Close planet detail (built, in 1)** | The ground keeps its grain when flying low. | in 1 | noise, program | Medium |

All six only read memory and draw GL, so the risk to the core and the
oracle is nil.  The risk column is about visuals and code.

- **Limb glow.**  Built as part of the planets layer: see "Prototype notes: the planets" below.
  - Hook: `Nu_DrawPlanet`, after `draw_planet_sphere`, drawn additive.
  - State: the outer `atm_col`, taken before `planet_atmosphere_band` zeroes `natm`; the sun vector; the body class.
  - Airless bodies get none.
  - Fade out while the near-atmosphere cones are drawn.
  - Verify: Lave from orbit.
  - Switch: `--flat-planets`, which covers the whole planets layer.
- **Corona.**  Built (layer 3); see its prototype notes.
  - Hooks: a model check in `Nu_PutTwinklyCircle` and `Nu_PutCircle` (`put_star`); `Nu_DrawTwinklyCircle` and `Nu_DrawCircle` draw it (`draw_corona`).
  - Streamers: `pf_fbm` over the direction away from the star on the sky, seeded by the star.
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
- **Close detail.**  Built as part of the planets layer: see "Prototype notes: the planets" below.
  - Hook: `draw_planet_sphere`.
  - 3D `pf_fbm` on the sphere position, so there are no cube seams.
  - Overlaps the parked planet-fidelity work, so do it last.
  - Switch: `--flat-planets`, as for the limb.

**Recommendation.**  The sky band, the planets (with their limb and
close detail) and the corona are built.  Do rings once a ringed save
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

- **Size.**  `src/procfx.c` is 243 lines: 93 shared, 150 for the layer.
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

## Prototype notes: the corona

- **Size.**  About 95 lines of the layer in `procfx.c` (shader, tones,
  put, begin and end), plus 26 lines of `star_light` and `is_star` moved
  from `hdback.c` and about 30 of shared object reader (`pf_record`,
  `pf_frame`, which the sky band now uses too).  Hooks: gl.c +37
  (`put_star`, `draw_corona` with its square, four calls, one line in
  `Nu_3DViewInit`), main.c +3, screen.h +1, soft.c +3/-2, procfx.h +26.
- **Put.**  `Nu_PutTwinklyCircle` and `Nu_PutCircle` add the star after the
  disc's data: the model word of the object being drawn, and its seed
  (the long at +118).  The model check keeps it to stars (`$98`-`$ae`),
  and only while the player's 3D view fills the screen (a6+42, as
  hdtip.c).  Every other disc gets model 0 and draws nothing.
- **Draw.**  `draw_corona` runs before the disc: a square of half side
  `3r - 0.016z`; the second term keeps a far star's glow at a fixed size
  on screen, as far as the game's twinkle reaches.  The game's disc and
  twinkle are drawn over it, so the disc stays readable.
  - Gate, as the sky band: only in a 3D view where the background object
    was put (`procfx_sky_seen`, set by `procfx_sky_put`, cleared in
    `Nu_3DViewInit`).  The system map, the orrery and the galaxy map put
    no sky, so their stars and markers stay the game's (and #37's).
  - Colour by `star_light`: red, orange, yellow, white or blue.  The tone
    is squared near the disc and paler further out, so a K star reads
    orange and an M star red; Sol stays near white-yellow.
  - Streamers: `pf_fbm` over the direction away from the star on the sky
    (the eye ray less its part along the ray to the star's centre), turned
    into the sky's frame and offset by the star's seed, so the same star
    always gets the same streamers.
  - Screen blend (`GL_ONE, GL_ONE_MINUS_SRC_COLOR`): light over the bright
    disc tints it rather than burning it to white.  Enable, colour and
    current state pushed and popped.
  - `--flat-sky` keeps the streamers: the sky put records the sky's frame
    and only then honours its switch.  `--flat-stars` is the corona's only
    switch: no put, no program bound.
  - The square leaves the depth mask alone; depth writes are not on in
    the far-to-near znode pass.
- **Checked** (this build, on the integration base, against the base
  binary): see the stream's report for captures.  Captures from the first
  build (ed7343a: additive blend, old tones; Sirius, Leesti, Lave) are not
  evidence for this build and are not cited.
- **Not checked.**  Blue stars, giants and white dwarfs; a planet in
  front of a star (the game's draw order should cover it); frame time; a
  star near enough to take the sphere path (it would get no corona).

## Lights

The game marks stations, docking bays and landing pads with blinking
lights: a small flat disc and a thin cross (`Nu_PutTwinklyCircle` /
`Nu_DrawTwinklyCircle`, the same primitive the stars use).  The HD layer
adds a soft additive halo round each one.

- **What gets one.**  Every twinkly whose object is not a star.  The put
  stores one more long after the corona's: 1 when `hull_model_key()`
  names a model and `is_star()` is false for its word, else 0.  Seen
  with a census over the dock, landed, orbit and traffic tours: models
  `$20` (a pad's lights), `$2c` (the bay), `$48` (intro table), `$50` and
  `$54` (a station in flight) get a halo; `$9e` (a star) does not, and
  keeps its corona.
- **Size.**  Disc radius `r` is the game's (`0.002 * d2 * z`, a fixed
  size on screen).  The halo reaches `r + 2r` (3x the disc), held between
  a floor of `1.5 * px_scale()` pixels (about 4 px at 1600x1200, so a
  light whose disc the game has shrunk to nothing still glints) and a
  ceiling of 2% of the viewport height.  Pixels per eye unit come from
  the projection matrix and the viewport, so the wide frustum is right.
- **Intensity.**  Full to 2 km, then `sqrt(2 km / d)`: 0.6 at 5.5 km,
  0.2 at 58 km.  A far station reads as a sparkle of points, not a fog
  over the hull; a near one is bright enough to see against it.  Two
  rings in `disc_strips`: the colour out to the edge (alpha 0.6), and a
  paler core, half white, to 0.45 of it (alpha 0.8); additive blend.
- **Occlusion.**  No depth test, as everything in the 3D view: the view
  is painted far to near, the halo is drawn in the disc's own place in
  that order, so a hull painted later hides the halo as it hides the
  disc.  A light in its off phase is not drawn by the game, so it has no
  halo.
- **The cross.**  Unchanged (same lines, same width, no smoothing).
- **Switch.**  `--flat-lights` writes 0 for every light, so the draw
  makes exactly the base's GL calls.  `--old-renderer` never puts a
  twinkly through this path and is unaffected.
- **Not checked by capture.**  A pad's lights at night (the landed tour
  is by day and shows none), and a star in view (left to the put flag).

## Prototype notes: the limb glow (dropped)

Dropped, superseded by #38.  The planets layer draws the lit limb rim
(`uRim` in the planet shader) and the soft atmosphere halo
(`draw_planet_halo`), both under `--flat-planets`.  The separate glow and
its `--flat-limb` switch were removed, so planet drawing is the planets
layer's code path unchanged.

## Engine exhaust

Host-side only: `Nu_PutTeardrop` / `Nu_DrawTeardrop` in `src/gl.c`, no
shader, no new file.

- **What is drawn.**  A ship's plume is two translucent fills of the
  game's own Bezier teardrop (same control points, same 40 steps):
  - Halo: the game's colour and outline, alpha 0.8 from a point on the
    axis out to 85% of the way to the rim, then 0 at the rim (normal
    blend), so the body keeps the game's width and only the edge is soft.
  - Core: the outline scaled about the engine end, half as wide and 80% as
    long, in the game's colour mixed 70% toward white, alpha 0.9,
    additive.  It lies inside the halo, so nothing is drawn outside the
    game's silhouette.
  - Flicker: the core's length moves by up to +-8%, from a byte hashed
    from the object's record and the game clock (A6+524 >> 12).  The same
    frame draws the same in every run; it stands still while paused.  No
    game RNG, no wall clock.
  - Colour, blend, current and lighting state are pushed and popped.
- **Which plumes.**  Decided at put time and carried as one extra znode
  long: `hull_model_key()` must name the game's own table and a model word
  from `$12` to `$4e` (above the missiles, below stations and ports).
  Chimney flares and the intro's plumes keep the game's flat fill.  The
  range is inferred from the model tables; only the player's own hull has
  been seen to take it.
- **Small plumes.**  The engine draws a plume whose projected radius is 8
  or less as a 3D line (fe2.s, the branch before the `$70` hostcall), so it
  never reaches the teardrop and keeps the game's look.
- **Switches.**  `--flat-exhaust` puts a 0 and the draw takes the game's
  path: frames byte-identical to the base build.  `--old-renderer` never
  puts a teardrop, so it is unaffected.
- **Not checked.**  Another ship's plume; an in-game chimney flare (the
  intro's flares were checked); missiles.  The plume code does not test
  `--flat-fx`, `--classic-ui` or `--pixel-art`; whether they should also
  turn it off is open.
