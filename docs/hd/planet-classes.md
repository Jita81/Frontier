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
  built again only when the patch moves out of it or the planet changes.
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
