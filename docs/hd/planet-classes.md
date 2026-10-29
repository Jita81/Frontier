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

It is computed in the same pass as the classes and kept for the next slice.
Nothing draws it yet.

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
