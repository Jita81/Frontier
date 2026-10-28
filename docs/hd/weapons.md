# HD weapon effects (`src/fx.c`)

With the GL renderer, weapons are drawn as soft additive light from three
procedural 64x64 textures (glow, noise puff, ring): no image files, shaders
or FBO. `--flat-fx` gives the base HD look; `--old-renderer` never runs fx.

## How an object is recognised

`fx_tag_for` reads (never writes) the record at A6-212 and its model word
(+90); a known model puts an `NU_FX` op before its primitives, and the `fx_*`
hooks replace them. Tagged polygons are not hull-plated. Oracle unchanged.

| Model | What | HD look |
|---|---|---|
| 190 | laser | bloom, halo and white-hot core ribbons, muzzle bloom, hit flare |
| 10/12/14 | missiles | plume in cyan / green / violet off the body end its smoke starts at |
| 16 | nuke | orange plume, wider white core, red armed blink |
| 188 | explosion | flash and shock ring when new, puffs that cool to smoke |
| 194 | smoke | noise puffs, embers while hot, spread with age |
| 192 | shield | glow toward 0x8cf and a turning rim |
| 6/8 | mines | slow red beacon blink |
| 200 | ECM / bomb | fast cyan ring / slow white ring plus a view flash |

## Laser classes (fe2.s, not guessed)

The firer's laser byte (record +123, copied there at fe2.s 26479-26484 and
26547-26553) holds its class in bits 3-6, decoded the same way at L45f1e
(about 28431); the power table is L45f02. Classes: 1-2 pulse, 3 mining
(compare at 28413; sparks off the rock), 4-7 beam 1/4/20/100MW (thinner,
short, steady, white core rises with power), 8-9 plasma (widest, longest,
shimmering, near-white). Start ship: `$88` (fe2.s 39848); `FE2_FX_LOG=1`
prints `fx: laser type $88` when it fires. Hue is the game's own (D6).

## Time

Effects run on `fx_ms`, host time that stops while 10428(a6), the time rate, is 0:
pause holds every beam and flare on screen; a 2D screen (over 250 ms with no
3D frame) clears them. Ribbons send s/q screen-affine so a beam spanning a
2000:1 depth range keeps its core instead of collapsing to a hairline.

## Unverified

- Seen on screen: pulse laser `$88` (beam, bloom, core), missile `$0a`
  plume and smoke embers (External View), pause hold, `--flat-fx` = base.
- A missile puts only triangles and quads (no line, no circle near), so its
  plume is placed from its body and its smoke (model 194): no smoke, no plume.
- Not seen: beam classes 4-7, plasma 8-9, mining sparks, hit flare, missiles
  12/14/16, far missile spark, mines, shield, ECM ring, bomb flash, an
  explosion's flash and ring up close (one at 10 km shows only as a glow).
  No depth test: a glow can show through a hull in front of it.
