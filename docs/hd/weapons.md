# HD weapon effects (`src/fx.c`)

With the GL renderer, some weapon effects are drawn as soft light and smoke
made from two procedural 64x64 textures: a glow and a noise puff. There are
no image files, shaders or FBOs. `--flat-fx` gives the base HD look, and
`--old-renderer` never runs fx.

## When

`fx_tag_for` and `fx_laser_quad` read (never write) the object being put: the
record at A6-212 and its model word at +90. They act only in the flight view.
That means a game is on (bit 5 of 598(a6) clear, so not the intro or the
start menu) and a 3D view fills the screen (42(a6) negative, so not a map or
a 2D page). A recognised model puts an `NU_FX` op before its primitive, and
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
| 188 | explosion circles | a noise fireball in the circle's colour, size kept to its fraction; while yellow to white (young) a hot flash round it |
| 194 | smoke blobs | noise puffs that spread and thin with age |

Laser classes 0 and 10-15 were never seen; they take the nearest entry (1
and 9). Which shop laser is which class past 1 (`$88`), 3 (`$9a`, mining) and
4 (`$a1`, police) is inferred from the order of the game's damage table, not
seen in flight.

## Base look kept (fallback)

- Mines (6/8), shield (192), ECM and bomb (200).
- An explosion's lines, blobs and debris triangles; smoke circles (sparks);
  the missile's own body, lines and circles (drawn as they were).
- `PutTwinklyCircle` and `PutPoint` have no hooks: probes showed only models
  `$48`, `$50`, `$54` and `$9e` use them, none of them a weapon.

## Evidence

Checked on this branch at 1600x1200 with the GL renderer:
- Oracle replay of the route tape: exact after the fresh build.
- Police beam lasers (`$a1`, class 4) firing at the player: green beams
  with a pale core.
- Hull plating: by code reading, tagged primitives skip every plating
  path, including the class-material path from #20.

Not yet seen on screen in this build: the red pulse beam (`$88`), any
missile plume, and the explosion fireball. Only homing missiles were in
reach; smart, naval and nuclear missiles were not fired. A ship death
showed flat yellow discs, which are probably debris rather than the 188
fireball. `--flat-fx` was run but was not matched frame for frame.
