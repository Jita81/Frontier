# Planet surface classes (`src/planet.c`)

A planet surface is drawn from feature codes. Each texel of the cube-face
and ground-patch rasters holds a code index (0-15), and the model gives each
index a colour. This file describes how `src/planet.c` works out what each
code *is*: water, land, ice, crater, or other. It also adds a signed
distance to the coast. Later HD work uses both to light water and land
differently and to place shorelines.

Nothing in this file changes what is drawn. With no environment variables
set, the rasters, the textures and the frames are identical to the build
before it. The classifier only reads 68000 memory and writes none.

## Inputs

For one planet surface, keyed by its feature list and seed:

- **Code colour.** This is the colour the model header gives each code
  index, decoded the same way the 68000 decodes it:
  - the four base colours;
  - the second set of four, or the colour set picked by the model's selector
    word, when the selector reads a fixed value or a field of the planet
    object;
  - the light tint is not added, so the colour is the base colour.

  If the selector reads a global that the host cannot see, the colours are
  unknown, and every code of that surface is 'other'.
- **Body kind.** This comes from the model word of the object drawn with that
  seed, using the same model ranges the docked backdrops use. The kinds are:
  - airless;
  - rocky;
  - corrosive;
  - methane;
  - terran;
  - ice;
  - ocean;
  - gas giant;
  - star.
- **Temperature.** This is the object's surface temperature in kelvin.
- **Feature shape.** This is how often each code is drawn as a polygon, as a
  circle, or as a circle with an antipodal twin of the same code (a pair of
  polar caps).

No table is kept per world. Everything above is read at run time from game
data, so worlds the tour never visited are classed by the same rule.

## The rule

One code in, one class out. Brightness is the brightest of the three
channels, on a 4-bit-per-channel scale.

1. **Unknown colour, gas giant, star or unknown body:** other.
2. **Brightness below 2** (too dark to judge): other.
3. **Clearly blue** (blue at least 2 above red and at least 1 above green):
   - On a world with weather (terran, ocean, ice):
     - under 240 K, it is ice;
     - from 240 K up to 380 K, it is water;
     - hotter than 380 K, it is other.
   - On any other kind of world, it is other.
4. **Grey or white** (channels within 1 of each other):
   - On a world with weather, it is ice: snow and caps.
   - On a bare world (airless, rocky):
     - a code drawn only as antipodal circle pairs is ice (polar caps);
     - a code drawn at least as often as circles as polygons is crater;
     - otherwise it is land.
   - Otherwise it is other.
5. **Any other hue:**
   - On a bare world, a code with more unpaired circles than polygons is crater.
   - Otherwise it is land.

Hue alone is not enough. A blue code can be:
- sea on a temperate world;
- sea ice on a cold one;
- nothing known on a gas giant.

A grey code can be:
- snow on a world with weather;
- rock or crater on a bare one.

So the body kind and the temperature decide those cases.

**'other' means the rule cannot be sure.** It is never a guess. Any later
work must draw 'other' exactly as it draws today.

## Coast distance

`psurf_classify` fills a class per texel from a 16-entry lookup. It then
runs a two-pass 3-4 chamfer from the coast over the same raster. The coast
is any texel whose 4-neighbour differs in being water.

- Units are 3 per texel.
- The distance is negative in water and positive elsewhere.
- It is ±32767 when the raster has no coast.

It is computed in the same pass as the classes. `src/worlds.c` draws from it:
see [What is drawn](#what-is-drawn-sea-depth-and-beaches).

## Switches

| Variable | Effect |
| --- | --- |
| `FE2_DUMP_PLANET=<dir>` | Writes `<dir>/planet-<features>-<seed>.txt` once per surface, after all six cube faces have been rasterised. The file holds: the model address and word, the body kind, the temperature, the header flags and selector, and one row per code with its colour, polygon, circle and paired-circle counts, its area share of the sphere, and its class. Each raster's classify-plus-chamfer time goes to stderr. |
| `FE2_DEBUG_SURFCLASS=1` | Needs `FE2_DUMP_PLANET`. Writes the class mask as TGA files next to the dump: each cube face once, and up to 12 ground patches per surface as they move. The mask is the model colour mixed half and half with the class tint: water blue, land green, ice white, crater grey, other magenta. |

These variables write files only. `src/gl.c` does not read them, so no frame
changes. The `--old-renderer`, `--classic-ui`, `--flat-hulls` and `--pixel-*`
looks are untouched.

## Cost

The work runs only when one of the variables above is set, or when a caller
asks for `psurf_classify`. Measured on an M-series Mac:

- a 512² cube face: about 1.4 ms;
- a 1024² ground patch: about 5 ms.

## What is drawn: sea depth and beaches

`src/worlds.c` (W3, slice 1) shades the planet textures from the classes
and the coast distance. `planet_bake` in `src/gl.c` calls `worlds_bake`
to turn the codes into colours for one light zone, shaded.

- **Sea depth.** Water near a coast is lighter and a little turquoise
  (the shallows). It deepens to a slightly darker blue over 0.045 radians
  of arc from the coast. Water with no coast in reach is open sea. A small
  value noise, fixed to the body and seeded by the planet's own seed, moves
  the band edge, so it is not a perfect contour line.
- **Beaches.** On a terran or ocean world whose codes include both water
  and land, the land along the shore takes a thin sand rim, about 0.008
  radians wide (two face texels at a face centre). Ice worlds, bare worlds
  and the rest get none.
- **Untouched.** Ice, crater and 'other' codes are drawn as before, and so
  is every world with no water.

The rules every term keeps:

- The coastline never moves. Only colours change, and only from the game's
  own planet data.
- Each channel moves by at most a quarter of the texel's brightest channel.
  The night and twilight zones therefore stay as dark as the game draws
  them.
- The bands are set in radians of the surface, not in texels, and a texel
  distance is scaled by the gnomonic scale where it lies. So the 512-pixel
  cube faces and the 1024-pixel ground patch draw the same band widths.
- The distances come from a raster padded beyond each edge, by up to a
  sixteenth of its size. So a coast just over a cube face edge still makes
  shallows, and there is no seam at the edge.
- Classes and distances are worked out once per raster, at its first
  bake. The other light zones reuse them.
- A fine ground patch (1024², under about 0.3 radians across) is too small
  for its own padding to reach a coast 0.045 radians away. It reads its
  distances instead from a coarser probe raster (512², centred on the
  patch, wide enough to cover the patch plus the shallows band). The probe
  is kept while the patch stays inside it, and also whenever the patch is
  the same patch again (the same centre, axes and size, bit for bit). The
  second rule is the one that holds low down: the patch centre the game
  gives is not quite a unit vector, so the inside test never passes, yet
  the game rasters the very same patch again on most frames. The probe is
  built again when the patch changes or the planet changes.
  So the shallows seen from orbit are the shallows near the ground. The
  probe is sampled at its nearest texel, so close to the ground the band
  edge is blocky at the probe's scale.
- The patch reads the probe in 32×32 blocks. A block whose four corners
  (and so every texel: the map from patch to probe is projective) fall in
  one probe texel takes that texel's value once. When the whole patch lies
  in one probe texel, as it does landed, the shade follows from the code
  alone: `worlds_bake` builds a 16-entry shaded palette and expands the
  codes with it, at the cost of the plain expansion. Otherwise it keeps a
  table of each (code, shade value) colour it has worked out. The texels
  drawn are the same as shading each one in turn.

`--flat-biomes` turns all of this off: `worlds_bake` expands the codes
with the plain palette and reads nothing else, so the textures are the ones the build before
it uploads.

### Cost of the shading

Measured on an M-series Mac with a timing build, at orbit around a sea
world, with other game processes running on the same machine:

| Step | Time |
| --- | --- |
| First bake of a 512² cube face: padded raster (576²) | 0.1 to 0.35 ms |
| the same: classes and coast distance of the padded raster | 1.8 to 3.1 ms |
| the same: shade map in all, with the noise | 4.4 to 6.3 ms |
| Each later light zone of the same face (map reused) | 0.4 to 0.9 ms |
| A world with no water | one class lookup per code, then nothing |

The work runs once per raster: when a face is first drawn and each time
the ground patch is rastered again.

The tint on top of the plain palette expansion, per ground-patch bake (the
timing build, about 80 s of play on a sea world with the `q1t` drive; the
build before this change in brackets):

| Phase | Tint per patch bake, mean |
| --- | --- |
| Landed | 0.001 ms (5.5 ms) |
| Launch | 0.014 ms (5.2 ms) |
| Low flight | 0.44 ms, p95 2.0 ms (3.2 ms) |
| Cruise and landing | 0.008 ms (2.5 ms) |
| Landed again | 0.002 ms (2.7 ms) |

The game rasters the ground patch again on nearly every low frame (about
50 bakes a second). Before, each of those built the shade map, and often
the probe, again: about 5 ms a frame landed. Now the probe is kept for the
same patch and the landed patch is one palette expansion. The output is
byte for byte the same (checked on every bake of four full drives, about
5,000 bakes each, and on 2,160 stress bakes).

Memory: one byte a texel per raster kept (256 KB a face, 1 MB a ground
patch), in up to 48 slots, plus the padded scratch buffers.

## What is drawn: land biomes

`src/worlds.c` (W2, slice 1) gives the land of a terran world three tones:
lush, dry, and tundra and rock. Only `PS_LAND` texels of a body drawn with
a terran model (`psurf_terran_temp`, the body kind from the model word)
take them. Water, beaches, ice, craters, 'other' codes and every
non-terran world are drawn as before, byte for byte.

What a texel's biome follows, all on its unit direction on the body (so
the cube faces, the ground patch and the landed patch agree):

- **Latitude.** The game's polar caps sit on the model's y axis, so the
  latitude is |y|, wobbled by a little seeded noise. Near the poles the
  land turns to tundra and grey rock; round the equator it leans dry.
- **Temperature.** The body's surface temperature in kelvin (the object
  word at +182, the one the planet dump prints) moves both bands: a hot
  world's dry band widens toward the poles and its polar band thins out;
  a cold world's tundra reaches toward the equator. 288 K is the middle.
- **Moisture.** A low-frequency value noise, seeded by the planet's own
  seed (the same `psurf_seed` the coasts use), makes dry and lush regions
  across every latitude.

Measured share of the sphere (byte under 96 tundra and rock, over 160
dry): 240 K, 51% tundra and rock, 6% dry; 288 K, 23% and 20%; 325 K,
none and 68%.

The bounds:

- Each tone is a hue shift of the texel's own palette colour (toward
  green for lush, toward tan for dry, toward grey for tundra and rock), at
  the palette colour's own brightness (so where two biomes meet across a
  narrow lush band no darker crease shows), and
  each channel moves by at most a quarter of the palette colour's
  brightest channel (checked over every 12-bit colour, every biome byte
  and every beach weight, rim bytes 129 to 255: at most a quarter plus
  0.75 of an 8-bit level of rounding). A land texel never takes byte 128,
  which reads as open sea: before W12 slice 1 the outermost rim texels
  (0 < t < 1/254) rounded to it and lost their sand and biome (336, 285
  and 13 texels over the six faces and two ground patches of three test
  worlds); now they start at 129. The night and twilight zones stay as dark as the game draws
  them, and a world's land keeps its own hue on average (a green world's
  mean land hue moves by 5° to 13°).
- Across the beach rim the biome gives way to the sand, so the two
  together stay within the same quarter.

Cost: the biome is sampled every 16 texels of a raster and interpolated,
and kept per patch (the same centre, axes, size, seed and temperature), in
48 slots like the coast maps. A patch that falls within two steps of one
biome byte (the landed patch) keeps one byte and no map, and is one
palette expansion. Colours are worked out once per (code, biome byte) a
bake.

Memory: a varying patch keeps one byte a texel (256 KB a face, 1 MB a
ground patch), a uniform one none; with the 48 slots full of varying
ground patches that is at most 48 MB beside the coast maps, freed on a
video mode change. Rasters under 16 texels a side get no biome.

`--flat-biomes` turns the biomes off with the sea depth and the beaches.

## What is drawn: airless regolith and crater rays

`src/worlds.c` (W12, slice 1) gives an airless body, seen from orbit, a
fine grey regolith speckle, and soft bright rays from its own craters.

- **What keys it.** The body kind from the model word: `$7a`, `$7c` or
  `$80` (`psurf_airless` in `src/planet.c`). Rocky, corrosive, terran and
  every other body are drawn as before, byte for byte. Every code of the
  body takes it, water and ice included (an airless body has none).
- **The speckle.** Two octaves of the same value noise the coasts use, on
  the texel's unit direction, seeded by the planet's own seed
  (`psurf_seed`): cells of 1/64 and 1/21 radian, zero mean, moving the
  texel's brightness by at most ±5% and ±3.75% (±8.75% together). Each
  octave fades out (smoothstep from 0.35 to 0.7 cells a texel) where the
  raster's texel is too coarse to hold it, so the 512² faces and the 1024²
  ground patch draw the same speckle and a coarse raster never aliases it.
- **Grey.** The colour moves a fifth of the way to its own luminance, then
  takes the gain: one multiply a colour, applied to every light zone's
  palette, so it is albedo and the zones light it as they light the rest.
- **The rays.** From each of the model's crater circles (`psurf_craters`:
  the parsed circles of class `PS_CRATER`, no second parse), 4 to 7 rays,
  seeded by the planet seed and the crater's index in the body's list (so
  every face agrees). A ray leaves the rim at a seeded angle and reaches 2
  to 4 crater radii; it is a quarter of a radius wide at the rim and
  narrows to a fifth of that at its tip, and brightens by up to 0.2 at the
  rim, fading with the square of the distance along it. Only craters
  within reach of a raster are looked at.

Measured on the harness's three airless test bodies (a disc lit at 0.5,
600 px): mean disc luminance 119.8, 105.5, 100.8 before; 120.6, 106.0,
101.2 after (+0.7%, +0.5%, +0.4%). A face and the ground patch over the
same ground (the same texel size, different grids) differ by 0.19 of an
8-bit level on average.

Cost: the gain is sampled on a grid of about three texels (two at the
least, 32 at the most: 0.75 of a fine cell) and interpolated, in the
biome cache. First bake of an airless face, raster included, went from
about 0.7 ms to about 7 ms (5 to 12 ms on a busy host); a 1024² ground
patch 2 radians across from about 1.5 ms to about 27 ms, under the 61 ms
a terran ground patch already takes for its coasts. Later light zones
reuse the map. The grid array is static (1 MB) instead of on the stack.

`--flat-biomes` turns all of this off: `worlds_bake` expands the codes
with the plain palette, as for the biomes.

## What is drawn: gas giant fine bands

`src/worlds.c` (W7, slice 1) gives a gas giant, seen from orbit, many
fine latitude bands with soft, wavy edges, over the model's own coarse
bands.

- **What keys it.** The body kind from the model word: `$92`, `$94` or
  `$96` (`psurf_gas_giant` in `src/planet.c`). Every other body is drawn
  as before, byte for byte. The codes stay the base layer: each texel
  keeps its code's colour and only moves by its band's step.
- **The band rule.** A texel's band coordinate is its latitude (the poles
  sit on the model's y axis) times 12 bands a radian, about 38 pole to
  pole, waved by two octaves of the coasts' value noise (cells of 1/3 and
  1/11 radian, ±0.42 and ±0.15 of a band), seeded by the planet's own
  seed (`psurf_seed`): the same giant always draws the same bands, two
  giants differ. Each band gets a seeded step of its brightness, darker
  and lighter in turn (2% to 5% a side), and a seeded pull of up to 30%
  toward the hue, at the same brightness, of one of the giant's own
  sixteen code colours: no new hues. The two together are clamped to ±8%
  of the colour's brightest channel, so the change is zero mean and a
  bounded share of every light zone's colour (night stays dark). The last
  45% of each band blends into the next with a smoothstep.
- **No seams, no swimming.** The bands are a function of the texel's unit
  direction on the body, so faces and the ground patch agree at their
  edges and the bands turn with the body. They fade out (smoothstep from
  0.35 to 0.7 bands a texel) where a raster is too coarse to hold them.

Measured on the harness's three giant test bodies (a 600 px disc with
the day palette, then the night one): mean day-disc luminance 155.56, 65.15,
177.27 before; 155.64, 64.99, 177.12 after (+0.05%, −0.24%, −0.09%).
Night: 38.61, 15.89, 43.83 before; 38.59, 15.83, 43.78 after (−0.04%,
−0.40%, −0.10%).

Cost: the band coordinate is sampled on a grid of 8 texels and
interpolated, and each texel looks its colour up in a table by band,
code and step of the edge (64 × 16 × 32). First bake of a giant face,
raster included, went from about 0.13 ms (a plain palette copy) to about
0.45 ms, under any other class's face (an airless face takes about 7 ms).

`--flat-biomes` turns it off: `worlds_bake` expands the codes with the
plain palette, as for the biomes.
