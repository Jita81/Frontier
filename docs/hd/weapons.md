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
| 190, laser type `$88` | the start ship's pulse laser | halo in the game's colour (D6) round a near-white core, muzzle bloom, a flare where it hits |
| 10 | the homing missile | flame plume off the end of its body where its smoke starts; no smoke, no plume |
| 188 | explosion circles | a noise fireball; while hot (yellow to white) a flash round it |
| 194 | smoke blobs | noise puffs that spread and thin with age |

## Base look kept (fallback)

- Other laser types, including `$a1`, which police ships were seen firing.
- Missiles 12, 14 and 16 (nuke), mines (6/8), shield (192), ECM and bomb
  (200): none was seen in flight to tune on.
- An explosion's lines, blobs and debris triangles; smoke circles (sparks);
  the missile's own body, lines and circles (drawn as they were).
- `PutTwinklyCircle` and `PutPoint` have no hooks: probes showed only models
  `$48`, `$50`, `$54` and `$9e` use them, none of them a weapon.

## Evidence

EVIDENCE_PLACEHOLDER
