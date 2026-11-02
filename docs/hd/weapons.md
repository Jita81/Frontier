# HD weapon effects (`src/fx.c`)

With the GL renderer, some weapon effects are drawn as soft light and smoke
made from two procedural 64x64 textures: a glow and a noise puff. There are
no image files, shaders or FBOs. `--flat-fx` gives the base HD look,
`--flat-explosions` gives the base explosions with the other effects kept,
`--flat-lasers` gives every laser the one steady ribbon it had before bolts
and hot cores (below), `--flat-missiles` gives a missile its old plume, drawn
only near its smoke (below), and `--old-renderer` never runs fx.

## When

`fx_tag_for` and `fx_laser_quad` read (never write) the object being put:
the record at A6-212 and its model word at +90. They act only in the flight
view. That means a game is on (bit 5 of 598(a6) clear, so not the intro or
the start menu) and a 3D view fills the screen (42(a6) negative, so not a
map or a 2D page). There are two exceptions, tested in this order. In the
intro's scenes (bit 5 set and a part of the intro's own table, 114 or 116)
the fireball circles take the explosion look (see hulls.md, the intro's
scenes); the tag carries an intro mark, and since the record there is the
scene's (no clock at +122, no size at +88) the fireball draws young and hot,
with no shock ring. In the death camera (state `$58` at 704(a6)), a 3D view too, every
explosion (188) and smoke (194) record is tagged, not only the player's own
(the one at 674(a6)); a death leaves several. The game-over screen after it
is not touched. A recognised model puts an `NU_FX` op before its primitive,
and the draw side replaces that primitive. Tagged polygons are not
hull-plated. The oracle is unchanged.

Effects that outlive their frame (a beam, its muzzle bloom, a hit flare) are
aged on game time, which stops while the time rate, 10428(a6), is 0. Pause
holds them. Any change of screen or view (front, rear or external) clears
them. No depth test is used, so a glow can show through a hull in front of
it.

| Model | What | HD look |
|---|---|---|
| 190 | laser beams, every type | per class (bits 3-6 of the type byte, see `looks[]`): a halo in the class's hue round a near-white core, muzzle bloom, a flare where it hits; pulse red/orange, mining amber, beams green to violet, plasma magenta |
| 10, 12, 14, 16 | homing, smart, naval and nuclear missiles | a flame off the tail every frame the body is drawn: near-white nozzle, a flame in the type's hue, flicker; longer, wider and hotter from homing to nuclear (see Missiles) |
| 188 | explosion circles | one layered fireball per blob, by age and size (see Explosions) |
| 194 | smoke blobs | noise puffs that spread and thin with age |

## Lasers

For class 1 the game draws one quad for each shot, for one frame (measured:
one every 640 ms; other classes not yet measured). fx keeps the beam for
90 ms of game time, always on the segment the game gave, from the muzzle to
the far end, so a hit flare is still where the game put the hit. Its class
(bits 3-6 of the type byte) picks its look and shape from `looks[]`:

| Class | Laser | Hue | Shape |
|---|---|---|---|
| 0 (not seen), 1 (`$88`, the start ship's), 2 | pulse | red, orange | bolt |
| 3 (`$9a`, mining) | pulse | amber, throbs at 8 Hz | bolt |
| 4 (`$a1`, the police's) to 7 | beam | green, cyan, blue, violet | beam |
| 8, 9 (and 10-15) | plasma | magenta, pink, crackles at 12 Hz | plasma |

- Bolt: a short bright bolt flies from the muzzle and reaches the far end
  at 70% of the shot's 90 ms, then its trail sinks into the hit. The trail is
  30% of the beam's on-screen length, widest at the rounded head and tapering
  to nothing behind it. "Along the beam" is measured on screen (1/z runs
  evenly), so a beam that runs 2000:1 in depth does not reach its far end in
  the first frame. Its pieces are shaded as the trapezoids they are on screen,
  so a strip that tapers to a point stays even about its line.
- Beam: the steady ribbon, plus two hot spots running out along it at one
  beam length in 400 ms, half a length apart: each a swell of the beam's hue
  round a near-white core, tapering to nothing at both ends.
- Plasma: a beam, with its crackle.

Every timing is game time from fx's clock and the beam's start: never the
game's random numbers. Muzzle blooms and hit flares are as before.
`--flat-lasers` draws the plain ribbon for every class.

Laser classes 0 and 10-15 were never seen; they take the nearest entry (1
and 9). Which shop laser is which class past 1 (`$88`), 3 (`$9a`, mining) and
4 (`$a1`, police) is inferred from the order of the game's damage table, not
seen in flight.

## Missiles

`fx_tri` keeps the body triangles of a missile (models 10, 12, 14, 16), and
`fx_frame_end` draws its flame after the 3D view, every frame the body is
drawn. The flame keys off:
- the type, `looks[16..19]`: hue (homing blue, smart green, naval orange,
  nuclear magenta), core and halo widths as a share of the body's length,
  and flicker rate. Heat is `(halo - 0.2) * 5`, 0.1 for homing up to 1 for
  nuclear; it lengthens the white core and grows the nozzle glow;
- the body's long axis (the two vertices farthest apart): the flame runs
  back from the tail about `10 * halo` body lengths (2.2 for homing, 4 for
  nuclear), as three ribbons (hue, a paler middle, the near-white nozzle)
  tapering to a point, and a glow at the nozzle;
- flicker: value noise on game time (`fx_ms`) seeded by the missile's
  record, so each missile flickers on its own and a pause freezes it;
- width: the halo share of the body's length on screen, at least 3 pixels
  and capped at `160 * halo`.

Which end is the tail, in order:
1. The missile's own rotation. When the object is put, `fx_tag_for` reads
   the nine 1.15 words at A6-36 (as `procfx_sky_put` does; read only). The
   model's nose is on its +z axis (measured: the body's long axis is the
   model z axis within about 7 degrees, and +z leads the flight). The tail is the
   end on -z. This holds through camera turns and view changes while the
   rotation is read. On a fallback frame (2 or 3 below) the tail can flip
   for that one frame.
2. With no rotation for it (more than four missiles put in a frame, or the
   frame unreadable), the end nearer its smoke.
3. With no smoke near it either, the end farther from the eye.
A missile is never skipped. Motion against the previous frame is not used:
it flips when the camera turns.

`--flat-missiles` restores the base plume: drawn only where the missile's
smoke starts, off the end nearer the smoke, nothing when it coasts.

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
- A body: a ragged, near-opaque disc of noise (alpha 0.95 for the first
  third of life, easing to 0.55 at the end), whose solid part is at least
  the game's own outer circle, so it covers base's footprint. Its colour goes
  from yellow-orange to orange to red, then to soot grey as it spreads.
- A capped additive light in the fire colour, gone once the blob is smoke.
- A yellow-white flash the size of the core, only in the first 3% of life
  (k = 0), up to 6% at k = 1 (+88 = +1, not seen); 4% for a missile's.

`GLOW_CAP` (0.6) is the most light one layer adds. The fire and near-white
shares were measured on the two death replays against base (same frames,
debug frame dumps): the fire share is at or above base's on every frame
checked and near-white pixels stay at 0%, as on base. A fighter turns to
smoke late in its life; a heavy ship burns on. The fireball lasts as long as
the game draws the explosion (the game decides that, as on base). Blue
spark circles stay small capped glows.

There is one shock ring per explosion event: an explosion that starts
within eight blob radii of another seen in the last two frames (the several
records of one death) gets none of its own. The ring is a soft pale annulus
round the first blob, fainter than the fire (alpha at most 0.3), out fast and
gone by a quarter of its life. It fades out before it reaches the view's
edge. It is drawn with the explosion, so in the painter's 3D view (no depth
test) a nearer hull drawn after it covers it.

Smoke (model 194) is an alpha-blended grey noise puff, opaque enough (0.8
to 1) and large enough (about twice the game's circle) to read like base's
grey clouds after the fire.

An explosion that starts within a tenth of its depth of where a missile was
drawn the frame before is taken as the missile's. This is a heuristic, not a
field the game keeps: a ship dying next to a missile can be misread. A
missile's gets a tighter, hotter palette: smaller blobs, a yellow-white fire that burns out
sooner, and a smaller ring.

The death camera draws its explosions and smoke the same way as flight: fx
tags every 188 and 194 record while the death state is set (`704(a6)` =
`$58`). Lasers and missiles are not tagged there. The game-over screen is not
touched.

The explosion and smoke sprites and the shock ring switch GL lighting off
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
fired. Their flames were checked by drawing a fired homing missile with each
type's look (a scratch build only), not in play.

Missile flames, checked on a fired homing missile in the external view: the
long axis stayed on the model z axis over 35 frames while the camera turned,
and the tail held; paused, the flame stays and holds still; with
`--flat-missiles` the same paused frame has no plume (no smoke near it).
