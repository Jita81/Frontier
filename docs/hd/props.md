## Scenery props

`src/props.c` (with hook lines in `src/gl.c`, `src/main.c` and
`Makefile-C`): a sparse, seeded ring of pines and shrubs (terran worlds)
or ice spires (ice worlds) on the flat ground round a surface port.

**Status: stopped at placement (step 4).**  On both landed routes the
port's own flat, non-water ground lies wholly inside the clear disc, so
the census places nothing and nothing draws.  The census, the kit, the
lighting, the fade, the nodes and the switches are in and measured; no
prop has been seen on screen.  See *The stop* below.

- **What it keys off.**  The flight state byte (A6+704) at the 3D view's
  init, `$60` or `$68` (landed) and the climb after it, and a port object
  in the frame: a starport model whose parent body is terran (`$88`,
  `$8a`, `$90`) or ice (`$8c`).  The key is the port's slot, model, seed
  and parent; a new key resets the census.
- **Census.**  For the first 8 drawn frames (`WINDOW`) every complex
  shape, triangle and quad the engine puts while the port's frame is open
  is taken into the port's model space
  (`ldexp(V/32768, h10 - s) * (v - o)`, as `hull_model_frame`; one storey
  is 16384 units, model +y up).  Shapes are split into flat (y-range under
  `FLAT`), sloped, solid (triangles and quads) and cloud (above the
  ground plane by more than `CLOUD`).  The census is the same from the
  front, rear and outside views and over time: it is the whole port, not
  the frustum.  `FE2_PROPS_LOG=1` prints it.
- **Freeze.**  At the 8th frame: `y_g` is the height carrying the most
  flat area; `E` is the solids' radius; `Rc` is 1.15x the largest
  distance from the port's centre of the solids, the companion pads and
  the small ground shapes; up to 32 boxes are built round water, hills
  and companion terrain.
- **Placement.**  A 3-storey lattice over the annulus 1.15..2.5 `Rc`;
  a cell is a candidate when murmur3-seeded value noise is above 0.45, it
  lies inside a flat, non-water ground shape the port draws, outside every
  box and out of every hill's radial shadow.  The 200 nearest are kept;
  each takes a variant (terran 70 % pine, 30 % shrub; ice one of two
  spires), a size, a yaw and a lean from the hash.  If no flat ground lies
  beyond the disc, the lattice is not walked and nothing is placed.
- **Look.**  Flat-shaded triangles from the kit, lit on the CPU by the
  same sun vector as the hulls: ambient `.18 + .17 * day`, plus the diffuse
  term.  Back faces are culled on the CPU and the rest sorted back to
  front.  Lighting, texture, blend and cull state are pushed and popped.
- **Fade.**  `k = k_eye * k_t * k_px`: `k_eye` is 1 within `F0` = 3 `Rc`
  of the port and 0 beyond `F1` = 6 `Rc` (smoothstep), so props are gone
  before the engine stops drawing the port; `k_px` is a smoothstep of the
  prop's projected height from 0.75 to 1.5 px; `k_t` ramps in by 1/16 a
  drawn frame.  `k` scales the prop from its root, so there is no pop.
- **Where it sorts.**  Two detached znodes (`NU_PROPS`): FAR, in the
  ground's sub-tree at the ground's z - 1 (after the ground, before the
  port's buildings), for props whose eye-to-root segment crosses the
  port's disc; NEAR, in the buildings' sub-tree at their nearest z - 1,
  for the rest.  When the ground and the buildings interleave in one
  sub-tree, FAR goes after the highest building z.
- **Cost.**  `props_write` (the census, freeze and per-prop tests) runs a
  frame; `props_draw` emits the triangles.  Measured `props_write`:
  9-25 us a frame on both routes, the freeze frame about 23 us.
- **Bounds.**  Reads ST memory, never writes it.  No game RNG, no wall
  clock in any decision (the clock only times the log).
- **Switches.**  Off by default; `--props` turns it on.  `--flat-props`
  always wins.  `--old-renderer`, `--classic-ui` and `--pixel-art` also
  turn it off.  Off, no node is added and the frames are the base build's.

### Census

| | terran route (start 2) | ice route (start 1) |
|---|---|---|
| port model, body | `$60`, `$88` terran | `$62`, `$8c` ice |
| h10, s | 18, 18 | 16, 16 |
| prims | 407 | 252 |
| flat complex | 39, radius 1900 storeys (444 apron r <= 127; 060 squares 179-776; 55d water to 1900) | 6, radius 389 storeys (444, 060) |
| sloped complex | 53, y -15.8..472.6, radius 2616 (hills 18-47 storeys tall at 1100-2600, snow caps; one 472-storey mountain) | 1, radius 31 |
| solid | 315, y -0.3..29.5, radius 834.55 | 237, y -0.2..4.6, radius 355.97 |
| companions in range | none | model `$94`, s 22, 8 prims: terrain x 159..594, z 449..975, y -37..575 storeys |
| `E`, `Rc` | 834.55, 959.73 storeys | 355.97, 447.89 storeys |
| flat non-water ground beyond 1.15 `Rc` | none (land ends at 776) | none (ends at 389) |
| placed | 0 | 0 |

### Constants

| name | value | what |
|---|---|---|
| `STOREY` | 16384 | model units a storey |
| `PROPS_CELL` | 3 storeys | placement lattice |
| `FLAT` | 1/8 storey | a ground shape's y-range under this is flat |
| `CLOUD` | 16 storeys | above `y_g`: cloud, not port |
| `WINDOW` | 8 frames | census before the freeze |
| `Rc` | 1.15 x max distance | clear disc radius |
| annulus | 1.15 .. 2.5 `Rc` | where props may stand |
| noise | value noise > 0.45, lattice / 3 | clusters and clearings |
| `NPROPS` | 200 | per port, nearest kept |
| boxes | 32 | water, hills, terrain |
| `F0`, `F1` | 3, 6 `Rc` | `k_eye` full .. gone |
| `k_px` | 0.75 .. 1.5 px | projected height |
| `RAMP` | 1/16 a frame | `k_t` |
| ambient | .18 + .17 day | lighting |
| variants | terran 70 / 30 pine / shrub; ice 50 / 50 | |

### Asset ledger

All Claude-authored in `src/props.c` (`kit[]`): 14 rows of frusta (sides,
bottom and top radius, bottom and top height in storeys, x offset, lean,
rgb444), built into triangles at first use, at most 32 a variant.

| variant | rows | triangles |
|---|---|---|
| pine A, pine B | trunk (4-sided), two 6-sided tiers | 28 each |
| shrub A, shrub B | 6-sided bicone | 12 each |
| spire A, spire B | 5-sided cone and a leaning 4-sided shard | 14 each |

No game data, no external asset.

### The stop

Props stand only on flat ground the port itself draws.  The ground past
the port (the green land and the hills seen from the pad) is drawn by
the planet block, which this slice does not read, and the port's own flat
ground ends inside the disc on both routes.  Ways on, for a decision:
(a) stand props on the planet block's ground at `y_g` beyond the disc,
which needs that block's height (out of scope here); (b) let them stand
on the port's own green squares between buildings, with building boxes;
(c) define `Rc` from a percentile of the solids rather than the farthest;
(d) use the ice route's companion terrain.

**Ground-height limit.**  Props stand at one height, `y_g`, the plane of
the port's largest flat area.  They are never placed on a hill, slope,
lake, water or the planet's own terrain, and do not follow the curve of
the world; beyond about 2.5 `Rc` the ground falls away below `y_g` and
props would float, which is why the annulus stops there.

- **Not done.**  Any prop drawn; captures of props, the fade and the
  climb; frame-time comparison against the base build.
