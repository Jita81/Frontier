# Clouds

One cloud model, `CLOUD_GLSL` in `src/procfx.c`, drawn on every world
with air.  Its one function, `cl_cover (q, oct)`, gives the cloud cover
(0-1) at a direction `q` from the planet's centre, in the planet's own
model frame.  Every view of the clouds reads that function, so they are
the same clouds wherever they are seen from.  Off: `--flat-clouds`.

## Orbit

Seen from space, the planet shader of layer 2 (`docs/hd/procedural.md`)
is compiled a second time with `PF_CLOUDS` defined.  `gl.c` picks that
program for a world with clouds and the plain one for every other world.
Airless worlds, gas giants, stars and worlds of no known class go through
the plain program with the same uniforms, so they draw as they did before.

**The deck.**  The clouds lie on a sphere at R (1 + `CLOUD_DECK_H`), with
`CLOUD_DECK_H` = 0.002 (`procfx.h`).  For each ground fragment the shader
follows the line of sight from the eye to the deck and reads the cover
there, so the clouds sit a little above the ground and slide over it
towards the limb.  When the eye comes down to the deck, the clouds fade to
nothing across the band `CLOUD_DECK_T` = H/4 above it.  The ground shadows
stay.  The game's horizon cap, inside `PLANET_CAP_RATIO`, uses the same
program and the same uniforms as the sphere, so nothing pops when the
view crosses from one to the other.

**The model.**  The cover is a two-octave `pf_fbm` at `CL_K` = 6 cells
across the unit sphere, plus up to three octaves of fine detail.  The
detail fades out by screen size (`fwidth`) the same way the ground detail
does, and changes the cover by less than 0.003.  The cover goes through a
soft threshold (edge 0.04), is raised to a haze floor, and is scaled by an
opacity.  The second octave is read a little east or west of its place,
by up to 0.75 of its own cell, in proportion to the first octave (no extra
read).  The spread of the cover does not change, but where the first octave
changes fastest, at the cloud margins, the cells shear into streaks and
wisps along the latitudes, as cloud does in a wind, instead of round blobs.
`CL_K` = 6 and not a coarser 2.5: at 2.5 a few big cells decide
the whole planet, and the cover ran from 22% to 48% between seeds.  At 6
it runs from 24% to 39%.

**Seed, drift and the clock.**  Each body's seed (word 118) is hashed
into a 3D offset of the noise.  The clouds turn about the planet's spin
axis, the model's y axis (measured: the frame-to-frame rotation of the
model is about y), once every 16 game days.  They also change shape
slowly, through a second offset that goes round once every 3 days.  Both
are read from the game clock (`__DL5eb6_a6_base` + 524, the time of day,
and + 528, the day), never the wall clock.  So pausing stops them, time
acceleration speeds them up, and the same date gives the same clouds.

**Light.**  The cloud tops take the sun from `planet_sun`, put into model
space.  The object matrix scales it by about 0.993, so the shader
normalizes it.  The terminator of the clouds sits sqrt(2H) later than the
ground's (the deck sees the sun a little longer).  Past it, the clouds
take the ground's own colour, so the night side shows what the game drew
there and no cloud turns into a dark patch over the night ground.  On the
lit side and through the terminator each cloud is lit as a mass
(`cl_light`, in `CLOUD_GLSL` so every view of the clouds can call it): the
two-octave base is read at the deck point and 0.02 away along each of the
three model axes; the differences, projected onto the deck, are its
gradient, and the gradient tilts the cloud's normal.  There is no tangent
basis to switch, so no seam line anywhere on the sphere.  A flank that
faces the sun goes to 0.97 of the tint; one turned away goes to a cool
grey (0.5, 0.55, 0.65 of it).  With the sun high nearly every flank faces
it, so the relief is strongest towards the terminator, as with real
cloud.  Thick cloud (the base up to 0.12 above the threshold) is white;
thin margins take a little of a cool sky tint (0.84, 0.9, 1.0) and, through
the soft threshold, stay translucent.  The tilt fades out when the second
octave's cells shrink under a few pixels, so a far planet does not
sparkle.  Clouds get the same limb darkening as the ground.  On the lit
ground, each cloud casts a soft shadow (35%, three octaves), read where
the sun line from the ground meets the deck.

**Which worlds.**  The class comes from the model word (the same ranges
`hdback.c` and `planet.c` use).  A world gets clouds only if its model has
air, meaning its innermost atmosphere layer has a colour.  `natm`, the
number of layers drawn at the current size, is not the test: it is 0 from
most orbits.

| Class (model words) | Threshold (seed picks a value in the range) | Global cover (replica, 40 seeds) | Look |
|---|---|---|---|
| terran, ocean ($88 $8a $8e $90) | 0.582-0.559 | 28-35% (24-39%) | white |
| ice ($8c) | 0.685-0.653 | 9-14% (7-18%) | white |
| rocky ($7e $82) | 0.729-0.685 | 5-9% (3-12%), half opacity | wisps, 60% the air's colour |
| corrosive, methane ($84 $86) | 0.5, floor 0.8 | full haze | 50% the air's colour |
| airless, gas giant, star, unknown | none | 0 | plain program |

The global cover was measured with a C copy of the shader in float32, on
3 seeds of 100k points and 40 seeds of 20k points.  From the orbit
captures (`tools/p2shots --routes orbit`), the cover of the lit disc of
the first terran world, measured against a capture with full cover, was
41% at shot 02, 32% at 03 and 31% at 05.

**Cost.**  Per planet fragment the cloud program reads the noise sixteen
times: five octaves for the cover (two of the base, three of detail),
three for the shadow and eight for the relief (the two-octave base at four
points; the driver may share the centre one with the cover's).  If a slow
machine needs it, cut the shadow to two octaves first, then the cover to
four.

The global cover above was re-measured on the warped base with the same
float32 replica (detail octaves at full weight, the mean of the soft
threshold before the haze floor and opacity).  Old and new:

| Class | 40 seeds x 20k, old | new |
|---|---|---|
| terran, ocean | 24.1-37.6% (mean 30.6) | 24.1-37.8% (mean 30.6) |
| ice | 7.3-17.1% (mean 11.4) | 7.1-17.2% (mean 11.4) |
| rocky | 3.7-11.3% (mean 6.8) | 3.6-11.5% (mean 6.8) |

The detail octaves fade by the pixel size of the unwarped base, as
before the warp, so the warp does not make them fade early.

## Sky deck

Not built yet (parked).  When it comes back it calls `cl_cover` for its
cover and `cl_light` for its relief, so it shows the same clouds lit the
same way.

## Haze on the ground

Low over a world with air, the far ground fades into the sky's colour at
the horizon.  The planet shader (plain and cloud programs, sphere and
horizon cap) mixes each ground fragment towards `uHz.rgb` by

    a = w * smoothstep(-0.1, 0.1, c) * (1 - exp(-D * I * R / L))

with `D` the distance from the eye to the fragment (in R), `c` the
sun's cosine there (no haze on the night side, so no glow), and `w` = 1 -
ramp(1.02, `PLANET_SKY_RATIO`, d/R): full in the air, gone by 2R.  The
colour is the halo's innermost layer, the game's sun-tinted rim (0xeee
on a terran world by day), kept when the ST draws no halo, so it follows
the sun and looking down cannot pop it.  The reach L is the 6th column
of `cloud_look` times R: terran 0.009, ice 0.0135, rocky 0.027, corrosive
0.0045 (thick air hazes more, thin less; none without air), the reach at
sea level.

`I` is what makes it read as air.  Air thins with height, so what fades
the ground is the air along the path, not the path's length: `I` is the
path's mean of e^(-height / H), with `HAZE_SCALE_H` H = 0.0003 R and the
height taken from the sphere's curve along the path, in a closed form
good to about 6% above 0.003 R and to about 20% on the lowest grazing
paths (the planet shader in procfx.c).  A path that looks steeply down leaves the air quickly and keeps the ground's own
colour; only one that grazes the ground stays low, so the fade gathers
in a band just under the horizon at every height, with no cap to harden
its edge.

The port's own ground fades with it, by the same `pf_haze` (procfx.h):
same colour and air, along the path to each fragment, cut off where the
ray would meet the sphere (the port is flat, the world is not, and a far
hill stands past the ground it is drawn on).  That is the port model's
flat green and blue faces (hills, lakes; `hull_hazed`), its grey and blue
lines (roads and rivers far off), its apron, pad and near roads (one
shape, `gnd_fs`), and the dark patches of key 198, which go through the
clouds' program and are told from them by lying under `GROUND_Y` in the
model.  `planet_haze` hands these the view's values in `hull_hz`.  A
cloud stays as it was: the program takes a zero `uHz` for it.

Left as drawn: buildings, signs, lamps, grey and white hills (grey faces
cannot be told from a building, and a lit part must not pale), ships,
stations and every model view (no planet there).  `hull_hz_up` takes it
all away in about a second (50 views) once the game says the ship is
down, so a port seen from the ground is as the base draws it.

Off: `--flat-descent` (byte-identical to base); `--classic-ui`,
`--flat-hulls` and `--pixel-*` keep the port as drawn (`hull_intro_base`).
Nothing changes above 2R, under `--old-renderer`, or in the cockpit view
on a terran pad (the eye sits on the sphere there).  An ice world's pad
sits a little above the sphere: its external view changes slightly, and
its cockpit view by at most 3 levels of 255, in the far-ground strip the
base already hazes.

Measured at x = 1400 on a 1600x1200 terran climb, the flat reach this
replaced to the path's air: in the view it paled most, the ground just
under the horizon (201,209,199) to (171,191,165) and the ground below
it (123,159,112) to (98,142,83); in the hover, low over the port,
(144,173,134) to (149,176,139), a touch stronger.
The oracle end line is exact.

## The game's own clouds and the intro

The intro's clouds take the same look: see "The intro's clouds" under Low
clouds.

The game draws its own low clouds as model 198.  A census (a scratch build
that logged every model-198 primitive with `FE2_HULL_LOG=1`; nothing of it
is committed) on the landed route and on a climb from the pad to space:

| Route, phase | Complex shapes (start / inner / Bezier / point) | Quads | Triangles | Colours |
|---|---|---|---|---|
| landed, on the pad | 100 / 100 / 418 / 13 per frame (up to 110 / 461) | 4 (up to 11) | 6 (up to 13) | white shapes and quads, grey triangles |
| climb, under the deck | 150 / 150 / 660 at most, falling to 22 / 95 | 3 to 36, then 79 and 123 | 4 to 38, then 93 and 128 | the same |
| climb, at and above the deck | none | 14 | 16 | the same |

So model 198 is the low-flight clouds too.  As the ship climbs into them
the outlines give way to quads and triangles, so far away a cloud looks to
be a flat white outline of Bezier curves and close by a few white quads and
grey triangles (read from the counts, not traced per cloud).  The object
frame resolved on every logged primitive.

## Low clouds

`src/puffs.c` lights model 198 (and the intro's clouds, below) as cumulus: sunlit tops, shaded sides, a
darker flat base and soft rims, on the game's own outlines.  Off
(`puff_on`): the old renderer, `--flat-clouds`, `--classic-ui` and every
`--pixel-*` flag.  With it off nothing is tagged and the frames are the
same bytes as before.

**Nothing new is drawn.**  The put side (`gl.c`) writes `NU_PUFF` and a
`puff_tag` (the model's frame, the sun and the object's seed, all read
from the hcall's registers and ST memory, never written) before each
model-198 complex shape, triangle and quad.  The draw side keeps the
game's GLU fill and its flat faces exactly as they were (the same
tessellation, the same vertices, so the same pixels covered) and binds
one fragment program around them: once for a run of a record's shapes
and faces, released by the first primitive that is not one.

**Classification, in the model's space.**  Each shape's points go back to
the model's space through the tag's frame.  A shape whose y extent is
under 2% of its larger x or z extent is a *sheet* (the flat base); any
other shape is a *lobe*.

**Lobes.**  A lobe is lit as the front half of an ellipsoid filling its
bound on screen and facing the eye: pseudo-normal (q, sqrt (1 - |q|^2)),
q the pixel's place in the bound.  Wrap light from the game's own light
vector, lambda = (N.L + `WRAP`) / (1 + `WRAP`), `WRAP` 0.6.  The shade
factor is f = 1 + day (`K_SUN` (lambda - mean) + `K_TOP` (q.up)), where
mean is lambda averaged over the bound, so the lit side gains what the
shaded side loses and the mean stays at the game's colour; up is the
model's y on screen.  `K_SUN` 0.30, `K_TOP` 0.12.

**Sheets.**  The game's colour, a little darker towards the middle:
f = 1 - `K_MID` (0.5 - |q|^2), `K_MID` 0.12.

**Mottle.**  Every pixel takes f *= 1 + `K_MOT` (n - 0.5), n one two-octave
`pf_fbm` (two noise reads), `K_MOT` 0.16, so within +-8%.  Shapes read it
at their q; faces at their place in the model's space (radius `FACE_R` 20
to a lobe).  The colour is gl_Color.rgb * clamp (f, 0.6, 1 + 0.3 day): one
multiply by the game's colour, never brighter than that by night.

**Day.**  day = smoothstep of (L.y + 0.1) / 0.3, y the model's up: by night
the sun and top terms go and only the mottle stays.

**Soft rims.**  The outline's radius in q is taken in 32 directions from
its edges (`uR`).  Alpha falls from 1 to 0.15 over the last `RIM_PX` 3
pixels inside it (by `fwidth`), never outside: the coverage is the
game's.  No rim on a shape under 8 pixels across, a full one from 24.

**Shared edges stay hard.**  The lobes of one cloud are faces of one
closed solid, so two that meet share their edge's points exactly; where
the two lie on either side of that edge on screen it is inside the cloud,
and a soft edge there shows the sky through as a light seam.  A table
keyed by (seed, the point in the model's space at half-unit steps) keeps,
per point, the inward normal on screen of the first shape to use it this
frame; a second shape whose normal there points the other way (more than
a quarter turn off) marks it inside.  A rim direction whose outline runs
along edges with both ends inside last frame stays hard.  A point not
seen last frame is hard, so a cloud just come into view has hard edges
for one frame.  The table's clock is the frames drawn (`puff_frame`),
never time.

**Near faces.**  The quads and triangles keep the game's colour and take
only the mottle, from their place in the model's space.  The far-to-near
switch is per part of a cloud, so in one record outlines and faces mix;
the faces' mean is the game's colour, as the outlines' is, and there is
no step at the switch (frames 8116-8118 and 8126 of the climb).

**The intro's clouds.**  `puff_key` (`gl.c`) is a list of three models, by
the key `hull_model_key` gives them: 198, and two intro parts, which carry
`HULL_TAB_INTRO | HULL_TAB_SCENE` off `hull_intro_base`, so the base intro
(`--classic-ui`, `--flat-hulls`, `--flat-fx`, `--pixel-*`) never matches.

- 186, low puffs in the city and battle scenes: 198's twin, the same
  proportions and primitive mix (lobes, sheets, faces near), at unit 2^10.
- 206, the bands over the limb: one dark blue, outlines only, no sheet and
  no face.  They shade by themselves (below, "The bands").

The tag's M is put in 198's unit (`ldexp` by 2 minus the model's unit, an
exact no-op for 198), since half a unit (shared edges), 4 units (seed) and
`FACE_R` are sizes in the model's space: without it shared points were not
found (specks of sky inside a cloud) and the mottle was too fine.  All
intro objects read one seed, so two clouds differ only by their shapes'
centres.  Up is the model's y, as for 198, and that is the scene's up; when
the camera pitches the sun, which the intro fixes to the eye, moves round
it and day follows.  The old renderer draws the same shapes flat, so this
is a restyle, not a regression fixed.

**The bands.**  Lit as lobes they took the look only gently, and the dark
colour was not why.  Their bounds run to many screens, so the screen only
shows a slice of each lobe, and blue over the base colour 96 ran from 71
to 114.  In the two shots with the sun below the model's horizon half to
85% of the pixels did not move at all: by night a lobe only goes darker,
so the upper half of a +-8% mottle clamps away, and shapes past the guard
were flat.  The seed made steps of 1-3 levels, up to 8 at an edge: the
centre of a shape drifts across a multiple of 4 as the game changes its
detail, on 14 frames from 2131 to 4673 of the idle intro.

So a band takes neither light nor seed.  The camera sits within about 2
units of the model's origin and the bands on a ring of radius about 210
around it: it only turns.  `puff_band` hangs the mottle on the ray to the
pixel in the model's frame, 4 octaves and 6 times the swing (the gain, in
`puff_tag`, 0 for any other model), about 1, so the mean stays the band's
colour.  One rule for every shot, no seam between shapes, and a shape past
the guard shades as the rest.

How the intro is proved: an idle replay of the opening, 9000 frames at
1600x1200, frame hashes against the base (neither tape nor frames kept).

- Bands, 79 frames of six shots: 95-98% of their pixels now move (32-45%
  in the two night shots before).  Blue p5/p50/p95 about 80/97/111 in
  five shots, 77/86/110 in the smallest (a sliver, frames 7178-7296).
- Steps: at the 14 frames the seed flipped, the change from the frame
  before is no bigger than its neighbours'.  The mottle follows the pan:
  the best shift between frames leaves about 0.1 of a level (up to 1 at
  the fastest pans, 25-40 pixels a frame).  A held frame stays held.
- Changed frames fall in the cloud footprints: 1441-2401, 4130-4705 and
  6914-7297, every other frame the same bytes bar noise, so 186 too.  One
  frame in 96, against 206 drawn magenta and `--flat-clouds`: 4523218
  changed pixels inside the footprint, 1 outside, by 1 level.
- `--flat-clouds`, `--old-renderer`, `--classic-ui`, `--flat-hulls` and
  `--pixel-art`: the same bytes as the base but for 0 to 2 single frames a
  run, a different one each time, as between two runs of the base.
- Two runs of the branch: 0 to 2 single frames, as for the flags.
- Climb and landed tapes: the same frames differ as between two runs of
  the base (404-418 and three stretches on the climb), no others.
- Cost, ABBAABBA: band shots +0.04 to +0.12 ms median, the whole replay
  +0.02.

**Guard.**  A shape with a point at or behind the near plane (so one
round the camera), one whose bound is more than 4 screens or under one
pixel takes the game's flat fill.  A band shades instead, without the rim.

**Seed.**  The object's seed mixed with the shape's centre in the model's
space, quantised to 4 units: the mottle stays on the cloud and two clouds
differ.  Never view space, time, a frame count or `rand`.  A band has
none: the ray alone places its mottle.

**Measured** (1600x1200, scratch builds with frame-hash and frame-time
hooks, `FE2_HD_BACKDROP=0`; a pad-to-space climb tape and a landed tape,
neither committed):

- Step 0 (the bound identity program): every frame the same bytes as
  the base, 9270 climb frames and 1804 landed.
- Coverage (each puff fragment drawn magenta, and again only where alpha
  is above 0): on the pad and seven climb frames, from 9770 to 143941
  covered pixels, no pixel changed outside the game's footprint and no
  hole inside it.  Mean RGB inside it moves by at most 2.4 of 255 on the
  climb (the worst coverage, frame 7272: 131.4 to 133.8) and 5.8 on the
  pad front view (196.1 to 190.3).
- Off, against the base: climb and landed with `--flat-clouds`,
  `--old-renderer`, `--classic-ui` and `--pixel-art`, the same bytes but
  for single frames that differ between two runs of the base too (one to
  three a run, a different frame each run).
- Two runs of the same tape, lit: no frame differed, 9270 climb and 1804
  landed.
- Cost, the 3D view's draw time (`glFinish` at both ends).  Whole replays
  in ABBAABBA order could not resolve it with other work on the machine
  (5-minute load 9 to 18; one view's pairs swung by up to 3 ms): the
  median pair difference was -0.27 ms on the climb, +0.91 in its worst
  coverage and +0.90 on the pad.  So one process compared itself: every
  fourth frame unlit, each set against the lit frame before it, the
  control being the two lit frames before that.

  | View | Pairs | Lit minus unlit | Control |
  |---|---|---|---|
  | climb, under the deck | 5678 | +0.32 ms | -0.03 ms |
  | whole climb | 9268 | +0.18 ms | 0.00 ms |
  | climb, worst coverage | 64 | +0.24 ms | +0.26 ms (not resolved) |
  | climb, the switch to faces | 86 | -0.03 ms | +0.11 ms |
  | pad, front view | 170 | +0.33 ms | -0.15 ms |

  `puff_shape` itself takes 0.21 ms a frame on the landed tape (92 shapes,
  6522 points): no trigonometry per point, which halved it from 0.48 ms.
