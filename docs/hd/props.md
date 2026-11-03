## Scenery props

`src/props.c` (with hook lines in `src/gl.c`, `src/main.c` and
`Makefile-C`): a sparse, seeded scatter of pines and shrubs (terran
worlds) or ice spires (ice worlds) on a surface port's own flat green
ground, between its buildings.  Off by default; `--props` turns it on.

**Status: drawing (W11b slice 1).**  Props are seen on both landed routes
(`p2-landed.drv`, `p2-landed-ice.drv`); the default build draws the same
pixels as the base.  Measurements below.

### The rule

- **Which port.**  A model the engine draws with its object at A6-212 is
  a port when its model word is an even number 90..108 and its flags byte
  (+280) has bit 4; its parent slot names the body it stands on: terran
  (`$88`, `$8a`, `$90`) or ice (`$8c`).  The key is the port's slot, model
  word and seed; a new key resets everything.
- **Census (8 drawn frames, `WINDOW`).**  Every complex shape, triangle
  and quad put while that port is being drawn is taken into the port's
  model space, `model = fsc * R (v - o)` with `fsc = ldexp (1, h10 - s)`
  (one storey is 16384 units, +y up).  A put whose y-range is under
  `FLAT` is a *flat shape* (its outline kept, 32 points, and whether its
  colour is green: G above both R and B in rgb444); anything else is a
  *box*: its xz bounds grown by `MARGIN`.  Anything whose lowest point is
  above `CLOUD` is a cloud and is left out.  A shape or box seen again
  (same bounds within 1/16 storey) is counted once.
- **Placement (at the freeze).**  A `CELL` lattice over each green
  shape's bounds, each point jittered by +-0.4 cell from a murmur3 hash of
  the port's seed, its parent's seed and the cell.  A point is a candidate
  when value noise (period 3 cells) is at least 0.45 (on ice worlds, also
  a 30 % hash keep), and it and its four probes at +-`MARGIN` in x and z
  all lie inside the same green shape, inside no non-green flat shape at
  or above that height (a road, the apron, a pad, a lake), and in no
  grown box (a building, a hill, a billboard).  A green square drawn twice
  is walked once.  The `NPROPS` nearest the port's centre are kept; each
  takes a variant (terran 70 % pine, 30 % shrub; ice one of two spires),
  a size of 0.7..1.3, a yaw and, on ice, a lean of +-8 degrees.
- **Where it sorts.**  The engine paints back to front by node z; a
  building's node z is 0.141 (`ZK`) times its view depth (measured on
  both ports).  Each prop is its own detached node in the port's solids
  tree at `max (1, ZK * depth of its centre)`, so it paints after the
  ground (whose nodes carry a huge z) and in depth order with the
  buildings.
- **Look.**  Flat-shaded triangles from the kit below, lit on the CPU by
  the port's sun vector: ambient `.18 + .17 * day`, plus the diffuse term
  scaled by `day`, times a per-prop tint of 0.92..1.08.  Back faces are
  culled on the CPU and the rest sorted back to front.
- **Fade and cull.**  `k = k_t * k_px` scales a prop from its root:
  `k_t` ramps in by 1/16 a drawn frame after the freeze, `k_px` is a
  smoothstep of the prop's projected height from 0.75 to 1.5 px.  A prop
  behind the eye, faded to 0 or wholly off the 3D view (its centre
  projected through the last frame's projection matrix, with a 1.2x
  height radius) gets no node.
- **Log.**  `FE2_PROPS_LOG=1` prints one line a freeze: the port, its
  scale, the flat shapes (green), the boxes, the candidates and the props
  placed.  Nothing else is logged.
- **Bounds.**  Reads ST memory only, never writes it.  No game RNG, no
  wall clock in any decision.
- **Switches.**  `--props` turns it on.  It is off whenever the 3D view is
  not the HD GL view: `--old-renderer`, `--classic-ui`, `--pixel-art`,
  `--flat-hulls` (also when the hull cycle reaches flat), and
  `--flat-props` always wins.

### Constants

| name | value | what |
|---|---|---|
| `STOREY` | 16384 | model units a storey of the port's buildings |
| `CELL` | 3 storeys | placement lattice |
| `MARGIN` | 2 storeys | clearance from roads, apron, pads, buildings |
| `FLAT` | 1/8 storey | a flat shape's y-range is under this |
| `CLOUD` | 16 storeys | lowest point above this: a cloud, not port |
| `ZK` | 0.141 | engine node z per unit of view depth |
| `WINDOW` | 8 | drawn frames measured before the freeze |
| `NPROPS` | 200 | props kept, nearest the port's centre |

### Measured (2026-11-03, 1600x1200)

| route | port | scale (h10, s) | flat shapes (green) | boxes | candidates | placed |
|---|---|---|---|---|---|---|
| `p2-landed.drv` | `$60` terran | 18, 18 | 179 (49) | 199 | 315 | 200 |
| `p2-landed-ice.drv` | `$62` ice | 16, 16 | 34 (11) | 206 | 437 | 200 |

- **Seen.**  In the external view (the third shot; the front and rear
  shots show the HD hangar backdrop, which hides the 3D view).  Terran:
  pines stand on the green beyond the pad, right of the billboard, clear
  of the apron and the roads.  Ice: sparse spires stand on the lawns
  between and in front of the buildings.  Neither route has a prop on the
  pad.
- **Default unchanged.**  Without `--props`, all three shots of both
  routes are pixel-identical to the base below the title bar (0 px of
  1 920 000 each); with `--props` added to `--old-renderer`,
  `--classic-ui`, `--pixel-art` or `--flat-hulls`, the land route's
  shots are identical to the same flag without it.
- **Oracle.**  The route tape ends on the reference line with and
  without `--props`.
- **Cost.**  Timed with a throwaway `clock_gettime` patch (not in the
  tree) over 256-frame windows on both routes: the props' CPU time (write
  and nodes, plus drawing) is 0.02-0.10 ms a frame with 24-136 nodes, the
  worst window 0.102 ms (write 7 us, draw 95 us, 52 nodes).  Before the
  off-screen cull and with GL state saved by `glPushAttrib`, drawing alone
  took up to 0.43 ms; the cull and four `glIsEnabled` reads replaced it.
  The 3D view is still drawn under the hangar backdrop, so the props cost
  the same in the front and rear views, where they cannot be seen.

The white puffs above the terran port's horizon are the port's clouds:
their lowest point is far above `CLOUD`, so the census leaves them out.
They are not snow caps on the hills, and props never test against them.

### Asset ledger

All six variants are Claude-authored rows in `src/props.c` (`kit[]`):
frusta of 4-6 sides with radii, heights and offsets in storeys, a lean
about z, and an rgb444 albedo per part.  No binary assets.  Author:
Claude.  Date: 2026-11-03.

| variant | parts | albedo (rgb444) | design prompt |
|---|---|---|---|
| 0 pine A | trunk, lower tier, upper tier | `642`, `262`, `373` | a stylised conifer in two stacked hexagonal cones on a short brown trunk, about 3.4 storeys tall |
| 1 pine B | trunk, lower tier, upper tier | `532`, `253`, `364` | a slimmer, bluer twin of pine A, about 3 storeys tall, so a stand does not look cloned |
| 2 shrub A | bicone halves | `463`, `473` | a low rounded bush, a hexagonal bicone 0.6 storey tall, lighter on top |
| 3 shrub B | bicone halves | `552`, `562` | an olive bush like shrub A for variety on the same lawns |
| 4 ice spire A | cone, leaning shard | `cdf`, `bcf` | a pale blue-white ice spike 2.2 storeys tall with a smaller shard leaning off its foot |
| 5 ice spire B | cone, leaning shard | `def`, `ace` | a shorter (1.6 storeys) ice spike with a shard leaning the other way |

### Not done

- No approach frame: the landed routes stay landed, and no drive script
  in this tree yet climbs from a surface pad.
- Placement is on green ground only; a port with no green flat ground
  gets no props (the freeze line says so).
