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

From the ground and in low flight the same clouds are drawn overhead, as
the underside of the deck.  `draw_cloud_deck` (`gl.c`) draws a dome of
`DECK_RINGS` x `DECK_SEGS` quads over every direction the horizon cap
leaves (from straight up to pi - asin(R/d)), each vertex carrying its
direction in the model frame.  The deck shader (`deck_glsl`, `procfx.c`)
follows that direction from the eye to the deck sphere and reads
`cl_cover (q, 5)` there, with the same seed, turn and drift uniforms as
the orbit view, so the cover overhead is the cover the planet shows from
space at that point.  Where there is no cover the fragment is discarded
before anything else is done.  It is drawn only below `DECK_TOP` = 1.2R,
on a world with clouds, and never with `--flat-clouds`, `--classic-ui`,
`--pixel-art` or the old renderer, so all of those, space, station
interiors, airless bodies and the view from above 1.2R draw exactly as
before.  The orbit view's shader is not changed: the night side from
orbit is as it was.

**Detail.**  Overhead the deck is close, so a finer noise breaks the cover
into separate clouds: `pf_fbm` at `CL_FINE_K` = 1500 cells across the unit
sphere, one octave, gone by the time a cell is under a pixel (`fwidth`).  It drifts with the clouds' change of shape
at `CL_FINE_DRIFT` = 25 times its rate (it was 250, which raced under time
acceleration).  It moves only cover that is neither clear nor full, so a
full overcast stays one unbroken deck; thicker cloud has a darker base,
and the thin cloud towards the sun brightens.  From below the deck thins
into the colour of the air with distance; at night it is a dim neutral
(8% of its day colour) that hides the stars.  From above, only the lit
deck near the horizon shows, with the cap's rim colour, fading out from
1.05R to 1.2R; looking down, the orbit view's clouds on the ground are
what shows, and near a port the game's own map of the surface is drawn
over those (as in base), so on the way down the cover appears overhead
as the ship passes under the deck.  Within `CLOUD_DECK_T` of the
deck's height it fades, so flying through it has no step.

**Height.**  The ST's camera matrix is not quite orthonormal (lengths off
by up to a percent), which near the ground is as large as the deck's
height.  So the deck takes the eye's direction and the sun from the
matrix, normalised, and the eye's height from d/R alone.

**Nose up.**  When the nose comes up so far that the horizon leaves the
picture, the game stops sending the planet, and with it the deck would go
in one frame, the whole sky at once.  So the last deck drawn is kept and
drawn on by `deck_hook`, called from `Nu_DrawScreen` before the tree is
drawn.  The player's record (`__DL5eb6_a6_base` + 674) holds the camera's
matrix P and its position in the planet's frame; the planet's matrix is
A = V P with V fixed by the view mode (+706), so the hook turns the cached
deck by A' = A inv(P) P' and moves the eye by the change in position,
scaled as the cached frame scales it, keeping its length as the moved
eye's (the same normalising as above).  The deck so follows the view as it
turns, and the eye's height carries on from the last planet frame without
a step (measured on `p2-ascent`: the drawn height continues the planet's
to within its own frame-to-frame step, and meets it again when the planet
comes back).  The hook draws only in a flight view, with the same camera
record, frame and view mode as when the deck was cached, and only below
`DECK_TOP`.  The sun stays where it was when the deck was cached, so
after `DECK_HOLD` (1/48 day, half an hour of game time) the deck fades
out over the second half of that time rather than be lit by a sun that
has moved.  A fade alone was the fallback: it was not needed, because
the camera record gives the orientation directly.

**Painter's order.**  There is no depth buffer: the tree is drawn from
the far end to the near.  When the planet is sent, the deck is drawn
inside the planet's own node, straight after the ground, so ships,
buildings and the game's own low clouds nearer than the planet are drawn
over it.  The hook has no planet node to draw in, so it adds its own node
to the top tree at the depth the planet last had.  Anything the game puts
in a sub-tree (a model's own tree) is ordered only within that sub-tree,
so the held deck is right against what the top tree holds and can be
wrong against an object that sits in another tree at a depth near the
planet's.  Nose up with the horizon gone, nothing but sky is behind it.

**Cost.**  One dome of 512 quads in one draw (it is centred on the eye,
so the direction each pixel reads is exact however coarse it is), and a
shader that discards first
what cannot show (the sky above the deck seen from above, and everything
within a hair of the deck's height), then reads the two-octave base and
the cover's three detail octaves only where the base is within their
reach (0.027) of the threshold: under a full overcast or a clear sky the
smoothstep is 0 or 1 whatever they add, so the cover is the same and
three noise reads are saved.  Where there is no cover it stops there; the
fine noise is one octave, and none where it is below a pixel.

Measured on `p2-ascent` at 1600x1200 (Apple M4, a scratch build that
skipped the deck on every other frame, so the two halves share the load):
the deck adds 0.8 to 1.3 ms to the median frame on the pad and 1.5 to
1.7 ms on the climb, over a target of 0.5 ms.  Of that, about 0.3 ms is
the cost of a full-screen pass at all (it stays when every fragment is
discarded), about 0.55 ms the two base reads that make the deck the same
cover as the planet's, and about 0.27 ms the fine octave.  Going lower
needs a different design (the base read per vertex with an exact read
only near the threshold, or the deck drawn at a lower resolution), not a
further cut to this one.

## The game's own clouds and the intro

The intro is not part of this slice.

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

**Lit puffs: the next slice, not this one.**  The triangles and quads
already carry their object-frame corners to Draw, but they are a few per
frame.  The bulk of the clouds are the complex shapes, and those carry no
object frame for an unlisted model (the complex context's frame is written
only for listed models), and are drawn from their outline.  A lit puff
with one radial measure per cloud needs, per shape, the frame (twelve
values) plus the cloud's centre and radius in its own space, written at Put
time by the complex path, which is outside the model-198 branch.  The
plan for that slice: admit model 198 to the complex context, take the
radius from the model, and in the 198 branch shade each fragment from
`cl_cover` over the object-space position, feathered over the outer 15%,
lit from the context's light and seeded from the object seed, with no
clock.
