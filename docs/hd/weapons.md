# HD weapon effects (`src/fx.c`)

With the GL renderer, some weapon effects are drawn as soft light and smoke
made from two procedural 64x64 textures: a glow and a noise puff. There are
no image files, shaders or FBOs. `--flat-fx` gives the base HD look,
`--flat-explosions` gives the base explosions with the other effects kept,
and `--old-renderer` never runs fx.

## When

`fx_tag_for` and `fx_laser_quad` read (never write) the object being put: the
record at A6-212 and its model word at +90. They act only in the flight view.
That means a game is on (bit 5 of 598(a6) clear, so not the intro or the
start menu) and a 3D view fills the screen (42(a6) negative, so not a map or
a 2D page). One exception: in the death camera (state `$58` at 704(a6)) the
player's own explosion, the record the game keeps at 674(a6), is tagged too;
the game-over screen after it is not touched. A recognised model puts an `NU_FX` op before its primitive, and
the draw side replaces that primitive. Tagged polygons are not hull-plated.
The oracle is unchanged.

Effects that outlive their frame (a beam, its muzzle bloom, a hit flare) are
aged on game time, which stops while the time rate, 10428(a6), is 0. Pause
holds them. Any change of screen or view (front, rear or external) clears
them. No depth test is used, so a glow can show through a hull in front of
it.

| Model | What | HD look |
|---|---|---|
| 190 | laser beams, every type | per class (bits 3-6 of the type byte, see `looks[]`): a halo in the class's hue round a near-white core, muzzle bloom, a flare where it hits; pulse red/orange, mining amber, beams green to violet, plasma magenta |
| 10, 12, 14, 16 | homing, smart, naval and nuclear missiles | flame plume per type (hue, width, flicker) off the end of the body where its smoke starts; no smoke, no plume |
| 188 | explosion circles | one layered fireball per blob, by age and size (see Explosions) |
| 194 | smoke blobs | noise puffs that spread and thin with age |

Laser classes 0 and 10-15 were never seen; they take the nearest entry (1
and 9). Which shop laser is which class past 1 (`$88`), 3 (`$9a`, mining) and
4 (`$a1`, police) is inferred from the order of the game's damage table, not
seen in flight.

## Explosions

An explosion (model 188) is a copy of the record of the ship that died
(fe2.s:27095) or of the missile that went off (L75852). fx reads these
fields:

| Field | What the game keeps there | What fx does with it |
|---|---|---|
| word +88 | size step: the ship's, one up from 10t and one more from 400t; the renderer already scales the circles by it | pace, 0 (fighter) to 4+ (heavy): how long the fire burns before it turns to smoke, how long the white flash lasts, how big the flash is, how slowly it turns |
| long +122 | the clock, set to `$ffffffff` and run down each frame (L74d8e) | age 0 to 1; everything is keyed off it, so a replay draws the same |
| byte +93, word +126 | per-object bytes | the seed for each blob's rotation |

The game draws each blob as rings of circles, outer first. fx draws the
outer ring as one fireball and drops the inner rings, so the light no longer
stacks into a white blow-out. Each fireball has three layers:
- An alpha-blended noise body that goes from fire to soot, spreading and
  thinning with age.
- A capped additive light in the fire colour. It goes from yellow to orange
  to red, and is gone once the blob is smoke.
- A white flash, only in the first 3% of life (6% for a heavy ship).

`GLOW_CAP` (0.4) is the most light one layer adds, so a hull behind still
reads. A fighter is smoke by about half its life; a heavy ship burns on. Blue
spark circles stay small capped glows. Each explosion also gets one shock
ring, a soft pale annulus round its blobs. It grows fast and is gone by a
fifth of its life. It is drawn after the 3D view with no depth test, like
the beams.

An explosion that starts within a tenth of its depth of where a missile was
drawn the frame before is taken as the missile's. It gets a tighter, hotter
palette: smaller blobs, a yellow-white fire that burns out sooner, and a
smaller ring.

The player's own explosion in the death camera is drawn the same way. fx
treats it as in flight only while the death state is set (`704(a6)` = `$58`)
and the record being put is the one at `674(a6)`. The game-over screen is
not touched.

The fx sprites switch GL lighting off while they draw and put it back after.
With lighting left on by the hull renderer, GL ignores the sprite colour,
and an earlier build drew the death-camera fireball a flat blue.

`--flat-explosions` stops the tagging of model 188, so the base circles are
drawn and the lasers, plumes and smoke stay HD. `--flat-fx` still turns
everything off.

## Base look kept (fallback)

- Mines (6/8), shield (192), ECM and bomb (200).
- An explosion's lines, blobs and debris triangles; smoke circles (sparks);
  the missile's own body, lines and circles (drawn as they were).
- `PutTwinklyCircle` and `PutPoint` have no hooks: probes showed only models
  `$48`, `$50`, `$54` and `$9e` use them, none of them a weapon.

## Evidence

Checked on this branch at 1600x1200 with the GL renderer, against a base
build of the parent commit:
- Oracle replay of the route tape: exact, plain and with `--flat-explosions`
  and with `--flat-fx`. Neither flag changes the game.
- Police beam lasers (`$a1`, class 4) firing at the player: green beams
  with a pale core.
- The player firing in external view: no beam from the player's ship on
  base or on this branch, so there was nothing to restore. That the laser
  actually fired was not checked on its own.
- Death camera: one frame was caught before the lighting fix. It showed
  the textured fireball path drawing, but in a flat blue. That led to the
  lighting fix.

Not yet seen on screen:
- The fireball's colours after the lighting fix.
- A close kill, and a small death next to a large one.
- The missile palette and the shock ring.
- The `GLOW_CAP` value, which is still the design value rather than a
  measured one.

The death camera cannot be paused by the driver, and police fire at the
player is random, so most driven runs did not catch a death. A replay tape
that ends in a death would make these checks repeatable.

Earlier notes still stand. The red pulse beam (`$88`) and the smart, naval
and nuclear missiles were not fired. `--flat-fx` and `--flat-explosions` are
shown to keep the game the same (oracle), but not matched frame for frame on
screen.
