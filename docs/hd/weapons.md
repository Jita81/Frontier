# HD weapon effects (`src/fx.c`)

With the GL renderer, some weapon effects are drawn as soft light and smoke
made from two procedural 64x64 textures: a glow and a noise puff. There are
no image files, shaders or FBOs. `--flat-fx` gives the base HD look,
`--flat-explosions` gives the base explosions with the other effects kept,
`--flat-lasers` gives every laser the one steady ribbon it had before bolts
and hot cores (below), `--flat-missiles` gives a missile its old plume, drawn
only near its smoke (below), `--flat-shields` gives the flat shield-hit
flash (below), and `--old-renderer` never runs fx.

## When

`fx_tag_for` and `fx_laser_quad` read (never write) the object being put:
the record at A6-212 and its model word at +90. They act only in the flight
view. That means a game is on (bit 5 of 598(a6) clear, so not the intro or
the start menu) and a 3D view fills the screen (42(a6) negative, so not a
map or a 2D page). There are two exceptions, tested in this order. In the
intro (bit 5 set and a part of the intro's own table, in its scenes and in
its credits and menu, record 72) the parts that `hull_intro_fx` names take
the HD look (see hulls.md, the intro's scenes): the fireball circles (114,
116) the explosion's, with an intro mark on the tag, and since the record
there is the scene's (no clock at +122, no size at +88) the fireball draws
young and hot, with no shock ring; the red beams (80, 92, 132), which the
intro draws as lines (`Nu_PutLine`, `fx_laser_line`), a pulse laser's,
look 1, with no hit flare (the scene's record has no hit word). The intro's
effects age on a clock of 20 ms per drawn 3D frame, not the wall clock, so
its replays draw the same; `--flat-fx` keeps the base intro. In the death camera (state `$58` at 704(a6)), a 3D view too, every
explosion (188) and smoke (194) record is tagged, not only the player's own
(the one at 674(a6)); a death leaves several. The game-over screen after it
is not touched. A recognised model puts an `NU_FX` op before its primitive,
and the draw side replaces that primitive. Tagged polygons are not
hull-plated. The oracle is unchanged.

Effects that outlive their frame (a beam, its muzzle bloom, a hit flare) are
aged on the game's clock (see Combat FX lab), so pause holds them and a
replay draws them the same. Any change of screen or view (front, rear or external) clears
them. No depth test is used, so a glow can show through a hull in front of
it.

| Model | What | HD look |
|---|---|---|
| 190 | laser beams, every type | per class (bits 3-6 of the type byte, see `looks[]`): a halo in the class's hue round a near-white core, muzzle bloom, a flare where it hits; pulse red/orange, mining amber, beams green to violet, plasma magenta |
| 10, 12, 14, 16 | homing, smart, naval and nuclear missiles | a flame off the tail every frame the body is drawn: near-white nozzle, a flame in the type's hue, flicker; longer, wider and hotter from homing to nuclear (see Missiles) |
| 188 | explosion circles | one layered fireball per blob, by age and size (see Explosions) |
| 194 | smoke blobs | noise puffs that spread and thin with age |
| 192 | shield hit | a translucent bubble round the hit ship in the circle's colour (pale at full shields, red when drained), rim-lit, with a ripple, on the game's flash frames only (see Shield hits) |
| 200 | ECM pulse, energy bomb | one translucent shell round the ship that grows and fades over the countdown: ECM blue-white, the bomb a hotter white-orange with a second, brighter rim (see ECM and energy bomb) |

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

Every explosion event (the same test as the ring) throws embers: n =
round(8 + 16k) seeded sparks, 5 for a missile's. Ember i flies out along a
seeded direction (flattened by half in depth) as
pos = start + dir * reach * (1 - (1 - u)^2), where u is its own age (the
explosion's age over its lifetime, which is 0.6 to 1 of the ember lifetime
L) and reach is (2.5 + 3k) blob radii scaled by a seeded half to whole
(1.25 to 2.5 radii at k = 0, up to 5.5 at k = 1; 0.75 to 1.5 for a
missile's). L is 0.5 + 0.2k of the explosion's life (0.35
for a missile's), so every ember is gone by 50% of life for a fighter, 70%
for a heavy ship. Each ember is a short additive streak (from u - 0.07 to u)
with a dot at its head, under a quarter of the flash across on screen
(1.5 px plus 3% of the blob's pixel radius wide, at most 0.3 of it), its
colour cooling from yellow-white through orange to deep red, its alpha
GLOW_CAP * (1 - u^2). The seed is word +126 and byte +93 of the record, so a
replay throws the same embers; nothing reads the game RNG or the clock.

The player's own death (`704(a6)` = `$58` and byte +236 = `$ff`, the owner
byte of the player's explosion) gets more. The death timer `722(a6)` is set
to `$3cae6` when the death starts and run down by the frame step
`14446(a6)` every frame (L63b9c); when it would go below zero the game goes
to the game-over screen without writing it. fx reads t = 1 - timer/$3cae6,
0 to 1 across the death camera:
- Two or three seeded secondary bursts (a fireball and embers, no ring),
  1 to 2 blob radii off the first blob, 0.7 of its size, starting at
  t = 0.1, 0.28, 0.46 (plus a seeded 0 to 0.1) and each lasting 0.3 of t.
- A slow white bloom: one additive warm-white glow sprite in screen space
  over the 3D view (2.5 view-heights across), centred on the blast, alpha
  0.35 * sin(pi (t - 0.2) / 0.6) from t = 0.2 to 0.8, so at most 0.35 and
  zero well before the game-over screen. It is drawn after the hulls and
  before the control panel; lighting is switched off and restored.

The embers, bursts and bloom are part of the HD explosion: `--flat-explosions`
and `--flat-fx` turn them off with the rest, and there is no new flag.

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

## Combat FX lab

**The clock.** `fx_frame_begin` reads the game's day, 528(a6), and its time
of day, 524(a6) (a whole day is 2^32), as milliseconds, and the effects ring
ages by that clock's step each frame, at most 50 ms. Probed on replay tapes:
it steps 20 ms a game frame at 1x (994 time-of-day units), the same as base's
wall step at 50 frames a second, so every effect lasts as long as before; it
does not move while paused; it runs 10x and up under time acceleration; and
a replay gives the same value on the same frame. The 50 ms cap is base's:
at 10x a frame is 200 ms of game time, more than a 90 ms beam lives, so an
uncapped beam would be gone before it was drawn twice. Under acceleration an
effect therefore ages 2.5 times faster per frame than at 1x; smoke and the
fireball age on their own game counters and follow the rate fully. A load
(the clock steps back) or a jump counts as one 50 ms step. The intro keeps
its own frame clock. Two side effects: the clock stops for about 20 s in the
player's death camera (the game holds its time there), so the smoke spin
stops while the puffs still fade; and below about 11 frames a second a 90 ms
beam shows for one frame only.

**The route.** `tools/drive/scripts/p2-fx-lab.drv` starts a new game,
launches, turns to face the station, stops about 1.5 km off and fires the
front laser at it in short pulses; its ships come out and are destroyed
close ahead (by eye about 1 to 1.5 km; not measured), with the front view
held on the fight. Record it with
the game's `--pace` (unpaced, the waits run ahead of the game and the route
misses the station) and a timeout of 300 s or more:

    SDL_AUDIODRIVER=dummy python3 tools/drive/drive.py --lib LIB --bin ./frontier \
      --script tools/drive/scripts/p2-fx-lab.drv --out DIR --timeout 400 --quiet \
      "--args=--size 1600 1200 --pace --oracle-record TAPE --oracle-out TAPE.rec.txt"

The recorded tape is game data: it lives outside git, at
`~/Frontier-ops/oracle/tapes/fxlab-1.fe2rep` (its line in the README there
gives the end line). Replaying it with `--oracle-replay` gives the same game
on every build, since the host never writes ST memory. At 50 frames a
second, frame N is N/50 s into it: laser shots at frames 6388, 7040 and
7540 (about 128, 141 and 151 s); fireballs at 6240, 6690, 7140 and 7640;
explosions with debris, hit flares and beams at 6740, 7190 and 7690; smoke
after each, at 6340, 6790, 7240 to 7340 and 7740.

**Frames.** Frame dumps need a scratch debug build with the dbgshot hook,
never committed: `~/Frontier-ops/cicd/p2fxcheck/tools/setup.sh` builds one
(`hook.py` pastes `dbgshot.c.txt` into `src/gl.c`), then

    FE2_SHOT_DIR=OUT FE2_SHOT_RANGE=6240-7790:50 SDL_AUDIODRIVER=dummy \
      ./frontier --size 1600 1200 --oracle-replay fxlab-1.fe2rep --oracle-out OUT.orc

dumps `OUT/f%06d.bmp` for every 50th frame from 6240 to 7790 (`rp.sh` in the
same folder wraps it). Checked this way at 1600x1200: two replays give
byte-identical frames (32 of 32, HD and `--flat-fx`), where two base
replays differ in 29 of 32; `--flat-fx` is byte-identical to base; a laser
bolt shows on the same three frames as base at 1x; paused frames 2.6 s
apart are identical; the ECM slice's build gives the same 32 frames as the
build before it. This route does not run at 10x: from 10x up a bolt is
drawn on three frames, where base drew it on two (see 10x and up below).

**10x and up.** At 10x a frame is 200 ms of game time and fx's clock steps
its 50 ms cap, so a 90 ms bolt is drawn at 0 and 50 ms and gone on the third
frame; a pulse laser fires every fourth frame there (one shot each 200 ms of
fx's clock, measured), and the first frame's bolt is still at the muzzle,
mostly below the view, so base showed a bolt on about one frame in four.
Now, while the last running frame stepped 100 ms of game time or more, a
bolt is kept for three frames and run in even steps (35% and 70% of its
life on the second and third), so it shows on two frames in four and still
reaches the hit. Its frames are counted only on frames where the game's
clock moves, and the rate is the one the game ran at before a pause, so a
paused bolt holds still. At 1x nothing changes (20 ms a frame: the bolt's
five frames as before). Beams, plasma, blooms and flares are unchanged;
`--flat-lasers` and `--flat-fx` turn it off.

## ECM and energy bomb

The game draws an ECM pulse or an energy bomb as model 200 (fe2.s L44480): a
copy of the firing ship's record put twice a frame, each copy randomly turned
and drawn as three complex-polygon patches on a sphere of about 49000 units
round the ship. The copy carries the word at +284 of the firing ship's record
at +116, so the kind is in +117's flags. fx reads (never writes) the record
being put:

| Offset | What | Measured |
|---|---|---|
| +116 | the high byte of the same copied word | `$00` on the ECM pulse (not the kind) |
| +117 | flags | `$20` (bit 5) on the ECM pulse, measured; bit 6 (`$40`) the bomb, read from the game's code (no bomb was fired) |
| +118 | a long that differs per object | fx's key for the shell |
| +283 | countdown | the ECM's was seen at 2, then 1, stepping once a second of game time; the bomb's starts at 7 (game code) |

`fx_tag_for` tags model 200 when it sees the first patch: the shell's centre
is the object's origin in eye space (the longs at -16(a6), -12(a6) and
-8(a6)), and its radius the distance to a patch corner, measured once per
object (the patches' corners differ by a few per cent and the game turns
them while paused). One line in `src/gl.c` (`Nu_DrawComplexSNext`, through
`fx_shell`) drops every vertex of a tagged patch, so neither copy's patches
are drawn, and fx draws one shell per object per frame instead: an additive,
camera-facing disc with a procedural texture, a rim round a faint inside.

The shell follows a phase that runs from 0 to 1 over the countdown on the
game's clock: it starts mid-way through the first second (the countdown is
only seen once it is running) and re-aims at each step of the countdown.
Its radius grows from 10% to 100% of the sphere, fast at first; its
brightness falls with the phase's remainder to the power 1.5, so it is gone
at the end. Pause holds it; under time acceleration it runs on the
game's clock (at 1x the ECM pulse is drawn on 62 frames, at 10x on six). No random
numbers and no wall clock.

It must not hide ships. The inside is faint (at most 12% of the rim); the
brightness is capped as a glow's is; the on-screen radius is clamped softly
to about 45% of the view's height; and it fades as its edge nears the
camera: full while its radius is under 70% of the camera's distance, gone
when it reaches the camera. The ECM is blue-white; the
bomb is white-orange with a second, near-white rim turned the other way,
brighter and thicker. `--flat-fx` turns it off and the game's patches are
drawn as before.
With the camera inside the shell (a ship's own ECM seen from its cockpit)
the same rim, on the same phase and cap, is drawn over the 3D view, centred,
running out from just inside the view's edges to past its corners; it fades
in from nothing as the shell's edge passes the camera, so there is no pop.

The route: `tools/drive/scripts/p2-ecm.drv` starts a new landed game, sells
the beam laser for the cash, fits the ECM system, takes the external view
and fires three pulses: one at 1x, one paused part-way for 3 s, one at 10x.
Its second shot, `02-ecm-paused`, is taken in the pause, since a shot can
take longer to capture than a 1x pulse lasts.
Record it with `--pace` and a timeout of 400 s as the Combat FX lab route;
the tape is game data and lives outside git.

## Shield hits

When a shield takes a hit the game sets bit 0 of +285 in the hit ship's
record (fe2.s 27044). On the next drawn frame `L4452a` (fe2.s 26412) clears
it and puts the ship as model 192 instead of its hull: one flat circle
(`Nu_PutCircle`) round the ship's centre, its radius from the model's field
10, so about twice the ship's size. That is one drawn frame a hit; a beam
held on the ship gives a run of frames (probe runs of 15, 7, 5, 4, 3 and 2).
The hull is not drawn on those frames, in base or here.

`fx_tag_for` tags model 192 and `fx_circle` (`shield_hit`) skips the flat
circle, keeping its centre, radius and colour. The colour is the circle's
own colour argument, which the game picks by the shields left: pale beige
(192,192,128) with plenty left, through (192,160,64), (192,128,0) and
(192,64,0) to red (192,0,0) and dark red (128,0,0) when nearly gone (seen
at 62%, 46%, 21% and 1%: pale, orange, red, dark red). The bubble is
keyed by the ship's record, at most four at once; a run starts when the
record was not seen on the last frame, and its start is on the game's
clock. `fx_frame_end` draws
one bubble per hit ship, on the game's flash frames only, so it is in the
same place at the same time as base and never lasts longer:

- camera-facing, drawn at the circle's depth and never eased or capped, so
  the rim sits at 0.9 of the circle's radius at any range (`sprite`'s
  half-view cap is not used: it would shrink a near bubble);
- a mottled fill in the circle's colour (`T_FILL`, 60%), densest toward the
  rim and thin in the middle, the view behind showing through; the game
  draws no ship on these frames;
- a rim in the same colour, added on top and lifted toward white by the
  colour's brightness: a full shield's pale rim reads near white, a drained
  shield's dark red rim is dim and barely lifted, so it reads as failing;
- a ripple running out from the centre every 240 ms, its phase from the
  record and the slot, no random numbers and no wall clock;
- brightest on a run's first frame, settling to half in about 150 ms, the
  rim's brightness capped as a glow's is;
- with the eye inside the bubble or within 1.2 of its radius, the same
  layers drawn over the whole view (`screen_quad`, as the ECM shell does): a
  tint in the shield's colour with its rim round the view's edge, never a
  whiteout.

Paused, the game keeps putting model 192 on every frame while a hit is
pending (base holds its disc through the pause too); the bubble's clock
does not move, so it holds still. No flash is drawn from the cockpit (the
game puts none); in the external view the flash is round the player's own
ship. `--flat-shields` and `--flat-fx` give the base flash.

The route: `tools/drive/scripts/p2-shields.drv` starts a new landed game,
swaps the beam laser for a pulse laser, fits a shield generator, launches,
takes the external view and fires three bursts at the port, whose patrols
answer; it then pauses and resumes the game thirty times so that hits land
in a pause. Record it with `--pace` and a timeout of 400 s; the tape is game
data and lives outside git.

## Base look kept (fallback)

- Mines (6/8).
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

- Embers and the player's death (on the two death tapes, base | branch at
  the same frames): yellow-white streaks out of the core in the first tenth
  of the death camera, cooling and gone by mid-life; two or three secondary
  bursts; the bloom rises and falls smoothly with no seam. The fire share is
  at or above base on every frame checked; near-white rises only in the
  bloom's frames (at most 2.5% of pixels) and is 0% as on base elsewhere.
  The last death-camera frame differs from base only in the smoke trail,
  which differs base against base too (smoke spins on wall-clock
  milliseconds); the game-over screen's fade-in is wall-clock timed and
  differs base against base, and the frames before and after it are
  identical to base. `--flat-fx`: byte-identical to base on both tapes
  except the fade-in frame. With the fx clock fixed (a scratch build), two
  runs of a death tape give byte-identical frames. The 3D view plus fx
  takes the same time as base at p50 (within 0.2 ms) with an explosion and
  the bloom in view.

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

ECM and energy bomb, on a tape of the `p2-ecm.drv` route (scratch dbgshot
build, 1600x1200, base | branch | `--flat-fx`): base draws large flat cyan
patches round the ship; the branch draws one blue-white ring that grows
from small to past the ship's width and fades, the ship
visible through it, and the building behind it. It is bright and small at
the start, faint at about 70% of the phase and gone before the end. Over 150
frames sampled through the three pulses (every fifth): two replays give
byte-identical frames; `--flat-fx` is byte-identical to base; the branch
differs from base on 56, all inside the 3D view. Paused for 150 frames (3 s),
the shell's phase holds at 0.53 and the 30 sampled frames are one image,
where base's randomly turned patches give a new image almost every frame. At
10x the pulse is drawn on six frames (phase 0 to 0.83 in even steps).
`--old-renderer` is byte-identical to base (150 of 150); with `--classic-ui`
and with `--pixel-art --pixel-faces --pixel-text` the frames that differ
differ only inside the 3D view, the panels are identical. The bomb was not
fired (too dear for a new game): its look was checked with the kind forced
in a scratch build only. The oracle is unchanged.

10x bolts, on a scratch tape of the Combat FX lab start that fires the front
laser at 1x and then at 10x: at 1x each bolt is drawn on its five frames as
on base; at 10x on three running frames (0, 35% and 70% of its life) where
base drew two (0 and 56%), and the bolt is on screen on two frames of each
four where base showed it on one (its first frame is at the muzzle, below
the view, on both). With only two frames it would still show on one, so the
floor is three. With `--flat-lasers` or `--flat-fx` the frames are
byte-identical to base run with the same flag (21 of 21).

Shield hits, on two scratch tapes (a hit scene from the route's start, and
the `p2-shields.drv` route), dbgshot builds at 1600x1200, base | branch |
`--flat-shields`: base draws a flat red disc about twice the ship's size;
the branch draws a blue-cyan ring with a ripple and a faint haze in the same
place on the same frames, the star and beams behind it visible. On the hit
scene (frames sampled through a 15-frame run): `--flat-shields` is
byte-identical to base (21 of 21); the branch differs from base only on the
15 flash frames; two replays are byte-identical. In a pause held on a hit,
base and branch change in exactly the same pixels round the bubble (the
star's flare, which base redraws while paused), so the bubble holds still;
paused frames differ base against base at the view's edges and on the
beams, so they do not compare across runs. The Combat FX lab tape: identical
to base (no shield hits on it). The ECM route: the ECM frames are identical
to base; the only other differences are 2D station frames that differ base
against base. `--old-renderer` is byte-identical to base; with `--classic-ui`
and the `--pixel-*` flags the branch differs only on the flash frames, inside
the 3D view. Time acceleration did not engage under attack (the game held
1x), so 10x was not seen. The oracle is unchanged.

Shield colour, size and fill (the follow-up to the first slice), on the same
hit-scene tape at 1600x1200, base | first slice | branch | `--flat-shields`,
frames with 62%, 21% and 1% shields left: the bubble takes the circle's
pale, red and dark red; the rim peak is at 0.89-0.91 of base's disc radius
(the disc's edge row by row, the projection being taller than wide) and the
haze ends at the disc's edge, where the first slice's ring sat well inside
it. Through the fill the view behind keeps 0.71-0.72 of its contrast from
0.3 to 0.7 of the radius and 0.60 near the rim. Mean brightness of the 3D
view: 154 against base's 161 at 62% (7% of pixels near white, on the rim),
121 against 90 at 21%, 102 against 79 at 1%. Replays of this tape are not
frame-exact even base against base (some 370 frames in eleven spans jitter
by a frame); on every frame stable across three base replays,
`--flat-shields` and `--flat-fx` are byte-identical to base, and the branch
differs from the first slice only on the flash frames, which two replays
give identically. A paced recording of `p2-shields.drv` gave hits at 46%
and 18% (orange, red) at the same range, identical across two replays on
its stable frames. The Combat FX lab and ECM tapes are identical to the
first slice on every stable frame; `--old-renderer` is identical,
`--classic-ui` and `--pixel-*` differ only on and next to the flash frames.
No tape reached a hit seen from inside or near the bubble: zoomed close,
the game drew no circle on three paced recordings, so the near tint was
seen only in a scratch build that forced it. No recorded pause landed on a
hit, so holding still in a pause rests on the bubble using only the game's
clock.
