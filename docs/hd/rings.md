# Planetary rings (HD path)

Banded, see-through rings on ringed gas giants.  The game's own ring shapes
are kept: the HD finish only changes how their pixels are shaded.
`--flat-rings` turns it off, and the output is then byte-identical to the
build before this layer.

## Where the rings come from

- They do **not** use the oval opcode.  Each frame the ringed model emits
  `Nu_PutPlanet` (fe2.s 17169), then its rings as ordinary complex shapes:
  2 bands, each cut into 4 quarter sectors, so 8 `Nu_ComplexStart` shapes
  (complex hcalls from about fe2.s 14135).
- `L3d648_PutPlanetRings` (17221) is misnamed: it inserts the planet circle
  through `L3d6d0_PutPlanetCircle` (17270).
- `Nu_PutOval` (hcall at 9465, in `L37fb2_ProjectOvalXYZ`, 9464, reached
  from `L3b9aa_FilledOvalThingy`, 14525) is used only by the intro logo.
  This layer does not touch it, so the logo keeps its look.
- GL already drew the rings in the right place, shape, tilt and colour,
  matching `--old-renderer`.  So this is a finish, not a fix.

## How it works

| Piece | Where |
|---|---|
| Remember the planet: centre, radius, light, ring-plane normal (object rotation at a6-36, words 3..5) and the object record at a6-212 | `skies_planet_put`, called from `Nu_PutPlanet` in gl.c |
| Decide a complex shape is a ring: same object record as the last planet; write an `NU_RINGS` opcode and payload in the shape's own data, so an aborted shape forgets it | `skies_ring_put`, called from `Nu_ComplexStart`; `put_complex_start_4real` writes it |
| Bind the ring shader for the shape | `Nu_DrawRings` sets `ring_pending`; `Nu_DrawComplexStart` calls `skies_ring_begin` |
| Per contour: ray-plane radius of each window vertex gives the band's inner and outer radius; contours that are not a ring band stay plain | `skies_ring_contour`, called from `flush_contour` |
| Unbind | `skies_ring_end`, called from `Nu_DrawComplexEnd` |

The fragment shader (`ring_glsl` in skies.c) casts the pixel's ray onto the
ring plane and works in planet radii:

- soft radial bands from `pf_fbm` along the radius;
- gaps in fixed radius cells (0.1 R), so all four quarter sectors agree;
- alpha 0.72 to 0.94 inside a band, falling to zero at the band edges over
  1.5 pixels (`fwidth`), so the edges are anti-aliased;
- brightness 0.95 to 1.15 of the game's colour, up to 8% more on the sun
  side;
- the detail fades as the view goes edge-on, back to the game's colour at
  alpha 0.8.

Everything is seeded from the body's object record (+118), never from the
game RNG or the clock.  Nothing writes ST memory.

## Proof (scratch captures, not committed)

- `--flat-rings` == base, byte for byte, on five map angles and the
  external flight view.
- `--old-renderer` unchanged on the same shots.
- Intro logo: no ring shapes are restyled in 30 s of intro; start menu
  shots are byte-identical.
- Oracle end line unchanged.
