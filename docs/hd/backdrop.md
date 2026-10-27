# Docked-location backdrops (`src/hdback.c`)

While the ship is docked at an orbital station or landed at a surface port,
and a flat 2D page is up (station services, shipyard, bulletin board,
stockmarket, the F3 status pages), the GL renderer draws a photographic
picture of that place behind the page where the flat page colour used to be:

- **Orbital station:** the inside of a docking bay, with the station's
  parent body seen through the open bay mouth.
- **Surface port:** a landing pad or hangar under that world's sky, for
  example a black starry sky on an airless world, blue sky and green hills
  on a living one, ochre haze on a desert, yellow murk under a corrosive
  atmosphere, or ice plains on a frozen one.

The light in each picture is the light of the system's primary star. A red
dwarf gives a dim ruddy light and a G star gives daylight.

A picture of the wrong planet is worse than none. Any place the code cannot
place with confidence keeps the old flat colour.

Nothing is drawn in flight, on the 3D views, on the galactic map or the
system view, or with `--pixel-art`. The game's own pixels always stay on
top: text, faces, buttons, the F3 ship box and the console in rows 168-199.

## Switches

| Switch | Effect |
|---|---|
| `--pixel-art` | No backdrop; the screen is exactly the old one. |
| `FE2_HD_BACKDROP=0` | No backdrop, with the other HD layers still on. |
| `FE2_HD_DEBUG=1` | Writes one line to stderr each time the decoded location, page state or picture changes (see below). |
| `FE2_ART_DIR=DIR` | Read `DIR/backdrops/` instead of `art/backdrops/` (`src/art.c`). |

## How it decides to draw

`hdback_draw` is called straight after the frame's `glClear` in
`Nu_DrawScreen` (`src/gl.c`), on the GL renderer only. It draws when all of
these hold:

1. **Docked or landed.** The flight-state byte at 704(a6) is $54 (docked),
   $60 (landed) or $68 (landed on rough ground). The state compares are at
   fe2.s lines 31128 and 31705. `a6` is the game's state block,
   `L5eb6_a6_base` (fe2.s line 302).
2. **At a starport.** The object at 678(a6) has the starport flag, bit 4 of
   byte 280. Its model says the kind: $54-$58 is orbital and $5a-$6c is
   surface. Docking stores the port's index at 672(a6) and its object pointer
   at 678(a6), from fe2.s lines 39863 and 39867.
3. **A flat page is up.** Two tests:
   - `fe2_bgcol` is $e. The page fill host call sets it (`src/hostcall.c`),
     and the frame's `glClear` then paints exactly the pixels the page fill
     left transparent.
   - Bit 0 of the byte at 536(a6) is clear. This is the equate
     `A6_do_clr_2_space_cols`, fe2.s line 86. The 3D views, the galactic map
     and the system view set the bit.
4. **A picture exists for the location's key** (below).

The page's flat colour is the `glClear` colour, so a picture drawn straight
after `glClear` shows exactly where that colour would. No pixel of the game's
screen is read or changed.

Every read uses `STMemory_Read*`. Nothing is written to ST memory, and
nothing changes game timing, input or click regions. The oracle replay hash
is unchanged.

## Where the ship is

The port's object gives the body. Byte 68 holds the parent's index. The
object slots are reached through the offset table `L44b02` (fe2.s line
26888; 115 word entries) and the base pointer at 14466(a6). That base is the
equate `A6_big_space`, fe2.s line 130. Each object is 286 bytes.

Fields read:

| Offset | Field |
|---|---|
| 68 | parent index |
| 90 | model |
| 93 | own index |
| 118 | seed |
| 182 | temperature in kelvin |
| 280 | flags |

Body class, by the model the game draws the body with:

| Model | Class | Look |
|---|---|---|
| $7a, $80 | airless | bare rock, no air |
| $7e | rocky | thin dusty air, desert |
| $84 | corrosive | thick unbroken cloud |
| $86 | methane | cold orange smog |
| $88, $8a | terran | oxygen, oceans, cloud |
| $8c | ice | oxygen but frozen |
| $8e | ocean | corrosive water world |
| $92-$96 | gasgiant | banded giant (orbital stations only) |

A surface port on a gas giant or a star gets no picture.

The code walks up the parents to the first star (models $98-$ae). If the
body's own parent is a gas giant, the body is a **moon**, and the giant is
in its sky.

Star light, by star model:

| Model | Light |
|---|---|
| $98, $9a, $9c (brown dwarf, M dwarfs, red giant) | red |
| $9e (K) | orange |
| $a0 (G) | yellow |
| $a2, $a4, $a8, $aa, $ae (F, A, bright giants, white dwarf) | white |
| $a6, $ac (B, blue supergiant) | blue |

## Key, picture and variant

The key is `KIND-CLASS-LIGHT`, for example `orbital-terran-orange`.
`art/backdrops/manifest.txt` lists every picture as `KIND CLASS LIGHT FILE`.
The picture is chosen in this order:

1. **Moon first.** A moon tries the `CLASS-moon` pictures first, which show
   the giant in the sky, then plain `CLASS`.
2. **Borrowed light.** A light with no picture of its own borrows another,
   as set by `borrow LIGHT OTHER`. The picture is then tinted with the
   manifest's `tint LIGHT r g b`. Orange, white and blue all borrow yellow
   and are tinted warm, crisp or cold.
3. **Variant.** When a key has several pictures (`-a`, `-b`), the port's
   seed and index pick one. The same port always shows the same picture.

The picture is drawn at 0.80 brightness. It is then covered by a #0b1016
scrim at 50% so the game's text stays readable. The fit covers the whole
window, keeps the picture's shape and centres it.

The texture is loaded once and kept until the key's picture changes.
`hdback_forget` drops it when the GL context goes (video mode change).
Nothing is loaded per frame.

The debug line looks like this (the numbers come from the game's state):

```
hdback: state=$54 mode=$00 bgcol=14 page=1 port=#112 model=$54 flags=$90 orbital
  body=#113 model=$8a 287K class=terran chain=#113:$8a<#114:$9e star=#114
  model=$9e light=orange -> key orbital-terran-orange orbital-terran-orange-a.jpg
```

## The pictures

37 pictures: 19 orbital and 18 surface, 4.06 MB in all.

- **Two lights for every class.** Yellow and red, the two ends the tint grades
  between, for every orbital class (8) and every surface class (7).
- **Orange for terran.** Orange versions of the terran pictures, for the
  common living worlds round K stars.
- **Second variants.** A `-b` variant of the terran pictures under yellow and
  red light.
- **Moon picture.** One frozen-moon picture, `surface-ice-moon-red-a`, showing
  a gas giant in a red dwarf's sky.

Each picture is a 1152x864 progressive JPEG at quality 75.

Each prompt is built from fixed parts in `tools/art/backdrop_prompts.py`:

- the scene: a docking bay (two framings) or a surface pad or hangar
- the camera, placed in a docked ship
- the body's look and the star's light
- a composition clause: the centre of the frame is calm, with detail at the
  edges, and there is no text, logos, numbers or people

## Provenance

Every file under `art/backdrops/` is new art:

- **The `.jpg` files** were made by xAI Grok (grok-imagine-image-quality) from
  the text prompts recorded word for word in [backdrops.md](backdrops.md),
  with their model and cost. They were then cropped and re-encoded by
  `backdrop_prompts.py process`. No image was given to the model, and
  nothing was traced, sampled, upscaled or derived from the game's bitmaps
  or screens.
- **`manifest.txt`** is written by `backdrop_prompts.py process`.

Each `.jpg` is listed in `.githooks/allowed-images` under
`# art/backdrops/`.

## Checked

Each of the three start positions was driven with `tools/drive/drive.py` and
`FE2_HD_DEBUG=1`. On each, the page with the backdrop was compared with the
game's own view of the same body. The captures are private and are not in
the repository.

| Start | Decoded | Key, picture | Game's own view | Match |
|---|---|---|---|---|
| key 1 | landed $60, surface port model $62. Body: model $8c, 269 K, a moon of a $94 gas giant. Star $9a. | `surface-ice-red` → moon picture `surface-ice-moon-red-a.jpg` | 3D view from the pad: pale plains, ruddy sky, a giant in the sky | yes |
| key 2 | landed $60, surface port model $60. Body: model $88, 288 K. Star $a0. | `surface-terran-yellow` → `surface-terran-yellow-a.jpg` | 3D view from the pad: blue sky, white cloud, green hills | yes |
| key 3 | docked $54, orbital station model $54. Body: model $8a, 287 K. Star $9e. | `orbital-terran-orange` → `orbital-terran-orange-a.jpg` | Galactic map: a K star. The system view, zoomed on the planet, fills with its flat green. | yes |

Pages also checked:

| Pages | Result |
|---|---|
| F4 services, shipyard (and its second page), bulletin board, stockmarket, F3 equipment, profile and cargo | page=1, backdrop drawn |
| F1 3D views, galactic map, system view, in flight after launch | page=0 or not docked, no backdrop |

`--pixel-art` with the backdrop on and off gives identical shots. The only
difference was the F3 page's turning ship model, which moves between frames.

## Known limits

- **White and blue stars** use the yellow pictures, tinted. They have no
  pictures of their own yet.
- **Classes outside the table** keep the flat colour. So do surface ports
  on unusual bodies.
- **Pages with a different fill** get no backdrop. These are pages filled
  with black, and the F3 page's 3D ship box, which the game draws black.
- **Only the three starts were checked in the game.** Other locations rely
  on the same decoding and on the class and light tables above.
