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

The detail octaves move the cover very little.  The same replica, 40
seeds, with the three detail octaves off and on (mean, then the range
over seeds, then the largest change of any one seed):

| Class, threshold | Detail off | Detail on | Largest change |
|---|---|---|---|
| terran 0.582 | 28.25% (24.2-33.4) | 28.27% (24.1-33.4) | 0.11 points |
| terran 0.559 | 34.57% (30.1-38.7) | 34.59% (30.2-38.6) | 0.17 points |
| ice 0.685 | 9.44% (6.9-11.6) | 9.46% (6.9-11.7) | 0.12 points |
| ice 0.653 | 14.19% (11.0-18.0) | 14.21% (11.0-18.0) | 0.11 points |
| rocky 0.729 | 4.99% (3.1-6.6) | 5.01% (3.2-6.7) | 0.08 points |
| rocky 0.6845 | 10.01% (7.5-12.6) | 10.03% (7.6-12.7) | 0.11 points |

The detail changes the shape of the cloud edges, not how much sky is
covered.

**Cost.**  Per planet fragment the cloud program reads the noise eight
times: five octaves for the cover (two of `pf_fbm`, three of detail) and
three for the shadow.  If a slow machine needs it, cut the shadow to two
octaves first, then the cover to four.

## Sky deck

Below `PLANET_CAP_RATIO` (1.25R) the horizon cap only covers the
directions that hit the ground.  `draw_cloud_deck` in `gl.c` draws a dome
over all the other directions, from straight up to the horizon
(pi - asin(R/d) from up), 32 rings by 64 segments, packed towards the
horizon like the cap.  Each vertex carries its direction in model space.
The deck shader (`deck_glsl`, `procfx_deck_begin`/`_end`) follows that
direction from the eye to the deck sphere and reads `cl_cover` there.
These are the clouds the orbit view draws: the same sphere, seed, drift
and clock, and the same uniforms (`cloud_stage`).  The dome is drawn
after the ground, with alpha blending, in the planet's own znode.  So the
game's sun, moons and stars, drawn before it, are covered by cloud, and
anything the game draws later (ships, the port) is not.

**From below.**  The ray leaves the deck sphere overhead.  Thick cloud
has a darker base (0.95 of the lit colour where the cover is thin, 0.55
where it is full).  Toward the horizon the deck thins into the air's
colour (the innermost atmosphere layer, as the cap's rim uses).  It is
half mixed and fully transparent by a path length of 0.06R.  So the deck
reads as cloud overhead and haze at the horizon, never as a hard ceiling.
At night the clouds go to the same dim neutral as from orbit (`CL_NIGHT`),
and they cover the stars.

**From above** (the eye over R(1 + H + T)).  Only the band between the
horizon and the deck's own edge is drawn: the deck seen edge-on, with the
cap's rim colour at grazing angles.  The ground below already shows the
clouds through `PF_CLOUDS`.  The band fades out from 1.05R to 1.2R, so
at the 1.25R hand-over to the sphere there is no deck left to pop.

**Inside a cloud.**  `cl_inside` is the cover at the eye, times a tent
that is 1 at the deck height and 0 at `CLOUD_DECK_T` (H/4) above and
below it.  Both the deck and the `PF_CLOUDS` ground mix towards the lit
cloud colour by it, so flying up through a cloud whitens the view and
flying up through a gap does not.

**The eye.**  The ST's object matrix is not quite orthonormal: the length
of a unit vector taken into model space runs from 0.992 to 0.994 with the
ship's attitude (measured on the landed route and while steering).  From
orbit that is nothing.  At the deck it is a third of the deck's height,
so `cloud_stage` normalises the eye and sun directions and takes the
height from d/R alone.  d itself still carries the same scale error: d/R
moves by up to 0.0007 as the ship turns in place.

**Limits.**  The deck is drawn only on frames where the game draws the
planet.  Looking straight up from low altitude, the game sends no planet
(the ground is out of view), so there is no deck in that view.  Every
view with ground or horizon in it has the deck.  `--flat-clouds` turns
the deck off; `--flat-planets` keeps it (it uses nothing of the ground's
shader).  The cloud cells are sized for the orbit view: one cell is
hundreds of kilometres across, wider than the sky a pilot on the ground
can see.  So from a base the sky is mostly all cloud or all clear, and
broken sky shows only near a cell's edge or once the climb opens the
view.

**Cost.**  One fragment of the dome reads the noise five times for the
cover, and five more inside a cloud.

## The game's own clouds and the intro

Not built yet.
