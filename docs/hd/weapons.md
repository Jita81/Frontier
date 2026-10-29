# HD weapon effects (`src/fx.c`)

With the GL renderer, some weapon effects are drawn as soft light and smoke
made from two procedural 64x64 textures: a glow and a noise puff. There are
no image files, shaders or FBOs. `--flat-fx` gives the base HD look,
`--flat-explosions` gives the base explosions with the other effects kept,
and `--old-renderer` never runs fx.

## When

`fx_tag_for` and `fx_laser_quad` read (never write) the object being put:
the record at A6-212 and its model word at +90. They act only in the flight
view. That means a game is on (bit 5 of 598(a6) clear, so not the intro or
the start menu) and a 3D view fills the screen (42(a6) negative, so not a
map or a 2D page). One exception: the death camera (state `$58` at 704(a6))
is a 3D view too, so there every explosion (188) and smoke (194) record is
tagged, not only the player's own (the one at 674(a6)); a death leaves
several. The game-over screen after it is not touched. A recognised model
puts an `NU_FX` op before its primitive, and the draw side replaces that
primitive. Tagged polygons are not hull-plated. The oracle is unchanged.

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
| word +88 | a size step, -1 to +1 (only -1 and 0 seen); the renderer already scales the circles by it | k = (s+1)/2, clamped to 0..1: how long the fire burns before it turns to smoke, how long the white flash lasts, how big the flash is, how slowly it turns |
| long +122 | the clock, set to `$ffffffff` and run down by a fixed step each frame (L74d8e) | age 0 to 1; everything is keyed off it, so a replay draws the same |
| byte +93, word +126 | per-object bytes | with the blob's place in its record, the seed for that blob's rotation |

The clock runs at the same rate for every explosion, so every explosion lasts
the same game time, whatever its size. k changes how it burns, not how long.

The game draws each blob as rings of circles, outer first. fx draws the
outer ring as one fireball and drops the inner rings, so the light no longer
stacks into a white blow-out. Each fireball has three layers:
- An alpha-blended noise body that goes from fire to soot, spreading and
  thinning with age.
- A capped additive light in the fire colour. It goes from yellow to orange
  to red, and is gone once the blob is smoke.
- A white flash, only in the first 3% of life (k = 0), up to 6% at k = 1
  (+88 = +1, not seen).

`GLOW_CAP` (0.4) is the most light one layer adds, so a hull behind still
reads. Measured on the death replay at 0.25, 0.4 and 0.6: none of them blows
out (near-white pixels stay at about 0%), and the higher caps mostly make
the shock rings stronger, so 0.4 stays. A fighter is smoke by about half its
life; a heavy ship burns on. Blue spark circles stay small capped glows.
Each explosion also gets one shock ring, a soft pale annulus round its
blobs. It grows fast and is gone by a fifth of its life. It is drawn after
the 3D view with no depth test, like the beams.

An explosion that starts within a tenth of its depth of where a missile was
drawn the frame before is taken as the missile's. This is a heuristic, not a
field the game keeps: a ship dying next to a missile can be misread. A
missile's gets a tighter, hotter palette: smaller blobs, a yellow-white fire that burns out
sooner, and a smaller ring.

The death camera draws its explosions and smoke the same way as flight: fx
tags every 188 and 194 record while the death state is set (`704(a6)` =
`$58`). Lasers and missiles are not tagged there. The game-over screen is not
touched.

The explosion and smoke sprites and the shock rings switch GL lighting off
while they draw and then restore whatever it was. With lighting left on by
the hull renderer, GL ignores the sprite colour (an earlier build drew the
death-camera fireball a flat blue). Beams and plumes leave GL state as base
did. A frame with no flight view and no explosion is not touched at all.

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
- Two replay tapes that end in the player's death, one with a single
  explosion record and one with four (+88 of -1 and 0). Every replay, base
  or branch and with either flag, gives the same end line, so the frames
  compare.
- Death camera: base draws flat concentric discs; this branch draws the
  layered fireballs, smoke puffs and shock rings at the same places.
- `--flat-fx`: byte-identical to base at every death-camera and game-over
  frame checked (31 frames on the two tapes).
- `--flat-explosions`: identical to base in the death camera except for the
  smoke, which stays HD.
- Flight frames before the death: the branch differs from base only where
  beams are, and base differs from itself there too (beams age on wall-clock
  milliseconds), so the lighting change has no effect in flight.
- Police beam lasers (`$a1`, class 4) firing at the player: green beams
  with a pale core.

Not yet seen on screen:
- An explosion in flight close to the camera; the tapes have none before the
  death.
- A heavy ship's explosion (+88 = +1) and the missile palette.

The red pulse beam (`$88`) and the smart, naval and nuclear missiles were not
fired.
