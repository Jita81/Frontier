# Planetary rings (HD path)

Banded rings on ringed gas giants, opaque and in the game's own colours.
The game's own ring shapes are kept: the HD finish only changes how their
pixels are shaded.
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
| Forget the last planet at the start of every frame | `skies_frame_begin`, called from `Nu_3DViewInit` in gl.c |
| Remember the planet: centre, radius, ring-plane normal (object rotation at a6-36, words 3..5), the object record at a6-212 and the frame (a6) that put it | `skies_planet_put`, called from `Nu_PutPlanet` in gl.c |
| Decide a complex shape is a ring: a planet was put this frame, the shape has the same object record, and the shape's frame is a LINK child of the frame that put the planet (the long at its a6 is that a6); write an `NU_RINGS` opcode and payload in the shape's own data, so an aborted shape forgets it | `skies_ring_put`, called from `Nu_ComplexStart`; `put_complex_start_4real` writes it |
| Bind the ring shader for the shape | `Nu_DrawRings` sets `ring_pending`; `Nu_DrawComplexStart` calls `skies_ring_begin` |
| Per contour: ray-plane radius of each window vertex gives the band's inner and outer radius; the shader is on only while the range lies 1.02..8 body radii out, and a contour that breaks it turns the shader off again for the shape | `skies_ring_contour`, called from `flush_contour` |
| Unbind | `skies_ring_end`, called from `Nu_DrawComplexEnd` |

The fragment shader (`ring_glsl` in skies.c) casts the pixel's ray onto the
ring plane and works in planet radii:

- ringlets: `pf_fbm` along the radius, brightness 0.88 to 1.18 of the
  game's colour;
- gaps in fixed radius cells (0.1 R, odds 0.4), so all four quarter
  sectors agree; a gap darkens to half;
- a lift of 1.03 pays back the gaps' dark, so a band's mean stays the
  game's colour (measured within 3%);
- the band is opaque, as the game's; only the edges are anti-aliased over
  a pixel (`fwidth`).  The grey shadow sector stays grey and opaque: no
  sun-side or other tint is added;
- the detail fades as the view goes edge-on, back to the game's colour;
- the ray-plane divide is guarded, so a ray along the plane cannot divide
  by zero.

Everything is seeded from the body's object record (+118), never from the
game RNG or the clock.  Nothing writes ST memory.

## Limits

- At the system map's default zoom the body is drawn as a circle, not
  through `Nu_PutPlanet`, so no planet is put that frame and the rings
  keep the game's flat look.  The detail starts once the planet is drawn
  as a planet (one zoom step in).
- The live intro is paced by the wall clock, so two intro runs are not
  byte-comparable even with `--flat-rings` on both.  The intro proof is a
  counter: no shape passes the ring test there, which is the same code
  path as `--flat-rings`.

## Proof (scratch captures, not committed)

- `--flat-rings` == base, byte for byte, on five map angles and the
  external flight view.
- `--old-renderer` unchanged on the same shots.
- Intro: 0 shapes pass the ring test in 130 s (the first version passed
  8823, of which 1233 were shaded); start menu shots are byte-identical.
- System map: band means within 3% of the game's colour; the shadow
  sector measures the game's grey.
- Oracle end line unchanged.
