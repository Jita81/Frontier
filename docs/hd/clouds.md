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
opacity.  `CL_K` = 6 and not a coarser 2.5: at 2.5 a few big cells decide
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
space.  The terminator of the clouds sits sqrt(2H) later than the ground's
(the deck sees the sun a little longer).  Past it, the clouds take the
ground's own colour, so the night side shows what the game drew there.
Lit clouds go from 0.6 to 0.95 of their tint, from the terminator to high
sun, and get the same limb darkening as the ground.  On the lit ground,
each cloud casts a soft shadow (35%, three octaves), read where the sun
line from the ground meets the deck.

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

**Cost.**  Per planet fragment the cloud program reads the noise eight
times: five octaves for the cover (two of `pf_fbm`, three of detail) and
three for the shadow.  If a slow machine needs it, cut the shadow to two
octaves first, then the cover to four.

## Sky deck

Not built yet.

## The game's own clouds and the intro

Not built yet.
