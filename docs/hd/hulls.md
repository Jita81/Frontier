# Hulls: a material per class of ship

The GL renderer paints the flat faces of the ships and stations the game
draws (and the station's docking hangar) with a plating material. Every
other model (the intro's titles, banners and scenes, text, planets,
buildings on the ground, missiles, debris) is drawn flat, exactly as with
`--flat-hulls`. The
68k game is unchanged: it still decides every face, its colour and its
light. The host only multiplies that colour by a texture and lights the
texture's relief from the game's own light direction. A face never gets
brighter or darker on average, and the colour stays the game's.

`--hull-style 5` is the default. It gives each class of ship its own grey
plating tile and its own material numbers, so a police Viper looks freshly
painted, a Python looks welded and greasy, an Asp looks armoured, and the
hangar looks like an industrial hall with a plated deck.

The code is in `src/hull.c` and `src/hull.h` (plain C, no GL, no 68k
state) and in the hull region of `src/gl.c` (the shaders, the draw calls
and the reading of the model's frame). The art is in `art/hull/`.
`tools/art/hull_tile.py` makes the tiles that start as Grok pictures, and
`tools/art/plate_tile.py` draws the station's tile in code.

## What it does, face by face

1. **Which model.** The engine's state names the object being drawn: the
   word at +90 of its record (via -212(a6)) is the model's offset into the
   model table that the game data pointer (`__DL973c_game_data`) names
   while it draws (twice the model number). The intro (its title, its
   scenes and the menu's backdrop) swaps that pointer to a table of its own
   (`__DL685d0_intro_gamedata`) whose words name other models: its 40 to
   52 are whole intro scenes (the city among them), where the game's are
   ships. So the key is the word, the table (`HULL_TAB_INTRO` for the
   intro's, `HULL_TAB_OTHER` for any other, nothing for the game's own) and
   bit 31. The renderer reads all of it without writing.
2. **Which class, or flat.** `hull_models[]` in `hull.c` maps the word and
   table to a class and a panel scale. Only ship and station hulls (and the
   station's hangar) are listed. A model that is not listed is drawn flat
   in every style (`hull_paint` in `gl.c`), as with `--flat-hulls`.
   Parts drawn by a model, such as a hangar door, the hangar walls or
   landing gear, carry that model's word and so its class.
3. **Where the tile lies.** The tile is laid on the frame of the model, or
   of the part, that draws the face, not on the face. Panels then keep
   their size as the ship turns or comes closer, run on across
   neighbouring faces of the same part, and move with the part.
   - **Scale.** The model's turn and origin are read at -36(a6) and
     -16(a6), and its scale from -208(a6) against +10 of the model header.
     `hull_frame_words` and `hull_model_frame` in `gl.c` do this. A face's
     corners are taken back into model units, so one tile is always
     `panel x scale` model units.
   - **Projection (tipped face plane).** Each face is projected along its
     own normal onto a plane tipped from the model-axis plane it faces
     most:
     - Tu = e_cu - n[cu] n
     - Tv = n x Tu
     - h = d . (n[cu], n[cv])

     This is isometric on the face, so plates are never stretched. Neighbours
     facing the same way share the same coordinates. `hull_frame_obj` handles
     triangles and quads, and `hull_plane_obj` handles complex shapes.
   - **Parts.** A part (a door, a lift, a turning dish) takes its own turn
     and origin, with the object's scale. Its tile is bound to it and moves
     with it, so a sliding bay door carries its plates as it opens instead
     of sliding under them. Panels do not run on from a part across the
     object's faces around it. (Before, a part square to the object took
     the object's frame, so the plates stood still while the door moved.)
   - **Faces moved by their vertices.** Some doors are not parts: the
     hangar's bay doors (models 290, 294 and the bay's own 84) are one face
     whose corners the model moves, the leading edge sliding while the rest
     stands. `hull_anim_bind` in `hull.c` keeps, per face (model word, seed
     and the face's address in the model, a6), its corners from the last
     frame. Each frame it finds the corners that moved since the face was
     first seen, and if they all moved by one step, it shifts the tile by
     that step, so the plates ride on the moving edge. When every corner
     moves together (the part's origin jumping, as when the closed door's
     frame is moved), the tile follows the whole face and does not pop.
     A face whose corners move apart (a spinning blade, a flickering beam
     in the intro's station) has no one step and is drawn flat; it stays
     flat for 16 frames after it last changed shape, so it does not flicker
     between flat and plated. `wok` bit 2 in the context carries that flag
     to `hull_paint`. Faces that do not move are not touched. The table
     holds 8192 faces; a face unseen for 600 frames starts afresh.
4. **Replace or keep flat.** `hull_paint5` decides, for each face:
   - **Lamps, windows and lit signs stay flat.** These are bright faces of
     nearly pure colour (the brightest channel above 191 and the darkest
     below a fifth of it). In the hangar, near-white faces (all channels
     at 208 or more, the light strips) also stay flat.
   - **Markings stay flat.** On the station, its bay and its hangar a
     yellow face (red and green up, blue all but off, by their ratios so a
     stripe in shade is still one) is a marking: the hazard stripes, the
     floor markings and the yellow signs. They are paint, not hull, and
     are drawn flat like the other signs (`hull_marking`).
   - **Large pure-colour faces in the hangar are plated.** These are the
     doors (the red bay doors) and other big painted plates. The test is
     a face whose longest side is at least 0.4 of a tile. The lamps and
     small plaques are under a quarter of a tile, so they stay flat.
   - **The hangar's floor and ceiling use the `deck` class.** These are the
     hangar faces that look along the model's y axis.
   - **Faces that host decals** (faces the engine follows with a complex
     shape, such as a registration or a logo):
     - On a known ship they get calm plating: less bump, no chips and half
       the grime. The lettering then reads as paint on the panels.
     - On the station they get the full plating. The lettering there is
       big, and the weathering stops a face of many tiles looking like
       wallpaper.
     - In the hangar they stay flat. These are signs.
   - **The station's outside drawn by the docking bay's model.** Model 84
     draws the bay seen through the slot, and from the slot out to launch
     range it also draws the station's outside around it: the corner
     chamfers, the slot's surround and the docking arms. A face of model 84
     with a corner outside the bay's box in the model's frame
     (`hull_bays[]` in `hull.c`: |x| 60000, |y| 25600, z -41500 to 195000,
     the measured tunnel with a margin) is the station's outside and takes
     the `station` class with its graze fade (below). The rule is on the
     model's geometry, not the face's colour, so it does not change with the
     light. Before it, those faces took the hangar's tile and no graze, and
     a slanting chamfer smeared the hangar's tile into wooden boards.
   - **A ship's parts from another model** (the missiles on its pylons,
     which the engine draws from the missile's own model under the
     ship's key) are not its hull: they are drawn flat, like a missile in
     flight. A building's parts (the hangar's doors and walls) keep its
     plating.
   - **Complex shapes** take the plane of the face they lie on:
     - The hangar's own shapes (grey signs and lamps) and markings stay
       flat. Its dark grey ones (64, the alcove doors; its grey lettering
       is 96) are plated with the hangar's tile, calm, as they carry the
       yellow window markings.
     - A coloured one, which is the station's outside around the slot,
       takes the station plating. (These still go by colour, not by the
       bay box: a grey complex shape out of the box stays flat.)
5. **Shading** (`hull_fs5`, GLSL 1.20):
   - The game colour is multiplied by the tile's paint tone and by the
     ratio of bump-lit to flat-lit diffuse. Without light (the dark side)
     the ratio is 1, so the dark side keeps the game's own shade.
   - The tile also adds:
     - edge occlusion along the face's own edges
     - grime and chipped paint, held to the tile's mean so the face stays
       as bright
     - a specular sheen whose strength and exponent are the class's
   - A second, coarser sample of the same maps, at an unrelated scale,
     breaks up the repeat at a distance.
   - **Footprint fade:** `fd = 1 - smoothstep(0.015, 0.06, max |dUV|)`
     fades the fine detail (bump, grime, paint tone) to flat. The fade
     starts when a pixel covers about 1/60 of a tile and is complete by
     1/16. Far and grazing faces then do
     not shimmer or moire. Mipmaps (`GL_LINEAR_MIPMAP_LINEAR`) and 8x
     anisotropic filtering, where available, do the rest.
   - **Graze fade** (classes with a `graze`; only the station has one,
     2.3): `fa = 1 - smoothstep(graze, 1.5 graze, e / det)`, where `e / det`
     is the pixel footprint's long side over its short side (r + 1/r). For
     the station that fades the plating to the face's own colour between
     about 56 and 70 degrees off square. Without it, anisotropic filtering
     keeps the seams that run down a slanting face sharp and averages the
     ones across it away, so plates read as long boards. A floor seen from
     just above is edge on too, which is why the hangar and deck keep a
     graze of 0.

## Caching and cost

- Each class's tile is loaded and baked once, the first time a face of
  that class is drawn. The two RGBA maps per class are kept:
  - t0: normal x, normal y, albedo and cavity
  - t1: hazard, gloss, grime and chips
- Nothing is loaded or baked per frame.
- Bakes take 12 to 84 ms each on the test Mac mini (generic 18, station
  72, hangar 78, deck 71) and happen once per session per class. So the
  first face of a class drawn in a session costs a stall of up to about
  80 ms (one dropped frame or a few); later frames do not.
- Drawing the 3D view takes about 1.4 to 2.5 ms a frame, and up to about
  6 ms with a ship filling a 1600x1200 window.
- `FE2_HULL_LOG=1` prints:
  - each bake time
  - each model the first time it is seen, with its table when it is not
    the game's, and its class or a note that it is not listed and is
    drawn flat
  - the 3D view's draw time averaged over 300 frames
  - one line per model, face and reason, the first time only (M is the
    model's word, with `(intro's table)` when it is the intro's):
    - `hull: model M face S: moves against its model, the tile bound to
      it` and `... changes shape against its model, drawn flat while it
      does` for a face moved by its vertices
    - `hull: model M face S rgb R,G,B: <why>, drawn flat` for a face of a
      listed model kept flat: `sign/marking (yellow paint)`,
      `sign/marking (hosts lettering)`, `lamp, window or lit sign (pure
      colour)`, `lamp or light strip (near white)`, or `no frame for the
      tile (a sliver or a face edge-on)`
    - for a complex shape (S is its point count and colour, as it has no
      face number): `complex shape: lamp or lit sign (pure colour)`,
      `complex shape: sign/marking (yellow paint)`, `complex shape:
      hangar sign/marking or lamp (white)` or `complex shape: hangar
      sign/marking (grey)`
    - `hull: model M face ff0000PP: a part from another model (a missile
      on a pylon), not hull, drawn flat` (PP the part's word)

## Classes

| class | tile | look | panel (model units per tile) |
|---|---|---|---|
| generic | `generic.png` | grid of steel panels, light scuffs (no listed model uses it now; unlisted models are drawn flat) | 8192 |
| police | `police.png` | clean glossy paint, thin panel lines, no wear | 10240 |
| civilian | `civilian.png` | worn paint, chipped edges, rivets, patches | 10240 |
| freighter | `freighter.png` | thick welded plates, patch plates, oil and grime | 12288 |
| armour | `armour.png` | thick bevelled armour plates, deep gaps, bolts | 8192 |
| fighter | `fighter.png` | sleek composite skin, fine lines, high sheen | 6144 |
| station | `station.png` | large rectangular steel plates, straight seams, two hatches and two vents a tile (drawn in code) | 131072 |
| hangar | `hangar.png` | industrial bulkheads, beams, conduits | 49152 |
| deck | `deck.png` | tread and plain deck plates, grating (hangar floor and ceiling) | 32768 |

Each class also sets:
- contrast: how much of the tile's light and dark becomes paint
- seam: groove depth
- wear: grime
- chips: chipped paint
- bump: normal strength
- gloss and shine: specular strength and exponent
- ao: edge occlusion
- tint: how warm the grime is
- graze: where a face seen edge on fades to its own colour (0: never)

The station's row is quieter than the ships' (contrast 0.10, wear 0.25,
tint 0.10): its faces are tan and large, and warm grime in the grooves of
a tan face read as grout between paving stones.

All of these are on one line per class in `hull_classes[]` in `hull.c`.

Models (`hull_models[]`, by the whole type word and its table;
`FE2_HULL_LOG=1` names the unlisted ones a session meets):

| word | model | class |
|---|---|---|
| 20 | Shuttle | civilian |
| 40 | Krait | armour, panels x2 |
| 42 | Gecko | armour, panels x2 |
| 44 | Adder | civilian, panels x1.75 |
| 46 | Viper | police, panels x3 |
| 50 | Moray | civilian, panels x2 |
| 52 | Cobra Mk III | civilian, panels x3 (not measured) |
| 56 | Asp | armour, panels x5 |
| 58 | Eagle | armour, panels x1.5 (not measured) |
| 66 | Python | freighter, panels x6 |
| 70 | Transporter | freighter, panels x6 (not measured) |
| intro's 72 | the orbital station behind the menu | station |
| 80 | orbital station outside, close up | station |
| 82 | launch structure | station |
| 84 | docking bay seen through the slot | hangar; its faces out of the bay's box (the station's outside) station |
| 288, 290, 294 | hangar interior and its parts | hangar |

A ship's panel scale puts one to one and a half tiles across its biggest
face, measured in the shipyard's view (the Viper's biggest face is some
44000 units, the Python's 110000). At x1 a tile repeated four to nine
times across a face and read as crazy paving. The fighters use the armour
tile: the fighter tile's fine diagonal weave, once big enough to see,
read as wood grain on an orange Krait.

- **Adding a model:** add one line.
- **Adding a class:**
  1. Make a tile (below).
  2. Add an enum entry in `hull.h` and a row in `hull_classes[]`.
  3. List the PNG in `.githooks/allowed-images`.

Not listed, so drawn flat:
- game table 32 (landing pad), 96 (starport building) and 198 (city):
  ground structures, not hulls;
- game table 72: its unit (2^11) is a ship's, not a station's; it was not
  seen in a capture and is not guessed at;
- game table 176 (unit 2^0), seen beside the bay; not identified;
- the intro's 4 (the title), 12, 34 (the banner), 36, 38, 40 to 54 (its
  scenes, the city among them) and every other intro word but 72 and the
  scene rows below.

### The intro's scenes

The intro's records 34 to 70 are its scenes (4 is its title, 12 its logo,
72 the credits and the start menu, which keep their look). A scene draws its
ships, clouds and fireballs as parts that carry the scene's record, so in
those records `hull_model_key` keys a part by its own model: the word, in
the intro's table, of the header at -4(a6) (`hull_part_word`). The scene's
own model keeps the scene's word. `FE2_HULL_LOG=1` prints `a part of N`
after the table for a part of record N.

| intro word | what | HD path |
|---|---|---|
| 62 (a scene's own model) and 192 | the ring station and its hub | station plating |
| 86 | the green ship, in several scenes | civilian, panels x2 |
| 130 | the ship of modules | freighter, panels x6 |
| 212 | the large ship of the first space scene | armour, panels x2 |
| 214 | the hypercloud's discs | the game's hypercloud glow (`hyper_tag`, `HYPERCLOUD_INTRO`) |
| 114, 116 | the fireball's circles | the explosion fireball (`fx.c`, `I_FIRE`, as model 188) |

- **Classes are by role.** Comparing the first 40 header bytes of every
  intro model with every game model finds twins only for 90, 192, 194,
  214, 220, 222 and 224; none of the intro's ships is a game ship, so each
  takes the class of the game's ships of its size and role.
- **Flat:** the ground, the city, its buildings and the parts not
  identified (76, 206, 124 to 128, 190, 218 to 222, 220 and 222 being
  twins of game models that are not listed either).
- **Planets and stars:** the intro's planet is a circle (90, a twin of a
  game circle), and its sun a set of circles, not the game's planet or
  star objects, so the HD planet and the star corona do not draw them.
- **Lasers and plumes:** the scenes' beams and engine plumes are not
  matched to a model and keep their look; so does the earlier, orange
  fireball, whose model is not identified (only the later one is 114/116).
- **Off:** `--classic-ui`, `--flat-hulls`, `--flat-fx`, `--pixel-*`
  (`hull_intro_base`: a scene's key is then 0, so its models are flat, as
  in the base) and `--old-renderer` give the base intro.
- **No carry-over:** a fireball is drawn from its circle at once; nothing
  is added to fx's list of live effects, so nothing reaches the menu or a
  game started from it.

## Switches

| switch | effect |
|---|---|
| `--hull-style 5` | the per-class material (default) |
| `--hull-style 0..4` | the earlier looks, unchanged pixel for pixel: 0 fixed-function plating (`art/hull/plating.lum`), 1 big seeded texture, 2 panels, 3 bump and weathering, 4 a seeded profile per model (its own table, by the low byte of the word, as it always was) |
| `--flat-hulls` | no plating at all: the game's flat faces exactly |
| Ctrl+Shift+H | steps 5, off, 0, 1, 2, 3, 4, 5 |
| `FE2_HULL_TILE=path` | every class takes this raw square grey tile (64 to 2048 a side, a power of two, 128 = x1.0), to try a tile without a rebuild; set but empty, every class bakes its seeded plating |
| `FE2_HULL_FIXED=1` | style 5 without its shader (the fixed-function path below) |
| `FE2_HULL_TEX=2048` | texture size for styles 1 to 4 |
| `FE2_HULL_LOG=1` | the log above |
| `FE2_HULL_DBG=1` | style 5 paints every face with its fade instead of its plating: red the graze fade (1 plated, 0 flat), green the class's graze / 3 (the station about 195 of 255), blue the footprint ratio / 8. Shows which class and fade a face gets. |
| `FE2_ART_DIR=dir` | where `hull/*.png` are looked for (default `art`); the macOS launcher points it into the app |

Fallbacks:

- **No GLSL 1.20, or `FE2_HULL_FIXED=1`.** Style 5 draws each class's
  tile in fixed function, as the face colour times the baked paint and
  groove tone. There is no bump, no sheen, no footprint or graze fade, and
  no calm variant (a decal's host gets the full tone). The class textures
  there are mipmapped with no anisotropic filtering, so a far or grazing
  face blurs to the tile's mean, which is the face's own colour, instead of
  being smeared into stripes.
- **No tile file for a class, or no image loader.** The class bakes the
  seeded plating of its profile from `hull_profiles[]`, which is drawn in
  code. This applies on non-Apple builds, where `art_load_rgba` has no
  loader. Both fallbacks say so once on stderr.
- **Off switches.** `--flat-hulls` and `--hull-style 0..4` give exactly the
  pictures they gave before style 5. This was checked by pixel comparison of
  captures.

The macOS launcher no longer sets `FE2_HULL_TILE` to `plating.lum`: that
would give every class the one tile. `FE2_ART_DIR` already lets the game
find both `plating.lum` (style 0) and the class tiles.

## Making a tile

The station's tile is drawn in code, from a seed:

```
python3 -I tools/art/plate_tile.py SCRATCH/tiles/station --seed 11
```

It packs a 16 x 16 grid with plates of mixed sizes, mostly square, so no
seam runs the length of the tile and no row repeats the next. It adds thin
straight seams, inset panel lines and corner bolts on some plates, two
access hatches and two louvred vents, and a faint mottle that is the same
in every direction. It writes `NAME.png` (1024, mean 128) and
`NAME-2x2.png`.

The other tiles start as a Grok picture:

```
withkey XAI_API_KEY -- python3 -I tools/art/grok_image.py --model grok-imagine-image-2.0 \
    --n 2 --out SCRATCH/grok/NAME "PROMPT + SUFFIX"
python3 -I tools/art/hull_tile.py SCRATCH/grok/NAME-1.jpg SCRATCH/tiles/NAME --size 1024 [--std 0.16] [--cut]
```

`hull_tile.py` makes the picture:
- grey
- free of light falloff (a high-pass)
- seamless: a half-shift blend, or with `--cut` a quilt with minimum-error
  seams
- mean 128 with a set spread

It writes `NAME.png`, plus `NAME-2x2.png` for checking the repeat. Copy only
`NAME.png` into `art/hull/`. Raw Grok output stays out of the repository.

Size: a tile repeats, so its size sets texel density, not screen size.
- 512 suits ship classes: a tile spans a few metres of hull.
- 1024 suits the station, hangar and deck: one tile spans a whole wall
  section, seen across a 1600x1200 window.

The nine tiles total 1.6 MB.

## Provenance of every art file

Every Grok image was made from a text prompt only. No game bitmap,
screenshot or capture went into any prompt or any processing step. Model:
`grok-imagine-image-2.0`. Each call made 2 variants at $0.06 each, so $0.12
per call. Ledger: 13 calls, **$1.56**.

Every prompt ends with the same suffix:

> Seamless tileable square texture, seen straight on, perfectly flat
> orthographic view, even flat diffuse lighting with no shadows, no
> highlights and no vignette, no perspective, the pattern fills the whole
> frame edge to edge, neutral grey metal, photorealistic, highly detailed,
> no text, no letters, no numbers, no logos.

| file | prompt (before the suffix) | variant | `hull_tile.py` |
|---|---|---|---|
| `police.png` | Clean freshly painted spacecraft hull: smooth large painted metal panels with crisp thin panel lines, a few flush fasteners and small access hatches, glossy paint, almost no wear. | 2nd | `--std 0.16` (512) |
| `fighter.png` | Sleek fighter craft skin: smooth composite panels with very fine thin panel lines, small flush screws, a subtle composite weave, clean and aerodynamic. | 2nd | `--std 0.16` (512) |
| `freighter.png` | Heavy cargo freighter hull: thick steel plates joined by raised weld beads, overlapping patch plates welded on, heavy rivets, oil stains and grime, rough and industrial. | 2nd | `--std 0.16` (512) |
| `civilian.png` | Old civilian spacecraft hull: worn painted metal panels with paint chipped at the edges, scuffs and scratches, rows of rivets, small patched repairs, faint grime streaks. | 2nd | `--std 0.16` (512) |
| `generic.png` | Spaceship hull plating: a grid of rectangular steel panels of varied sizes with thin recessed seams, small flush rivets along the seams, light scuffs and faint grime. | 2nd | `--std 0.16` (512) |
| `armour.png` | Heavy armoured warship hull: large rectangular and trapezoidal armour plates of different sizes laid in staggered rows, each plate thick with bevelled edges and a deep dark gap around it, rows of countersunk bolts along the plate edges, one or two small recessed access hatches, a lightly battered surface, military. No hexagons, no honeycomb. | 2nd | `--size 1024` |
| `hangar.png` | Inside wall of a huge industrial hangar bay in a space station: large flat steel wall panels, each about a third of the picture wide, separated by deep recessed seams, one heavy horizontal steel beam across the wall, a vertical support column, a few conduit pipes and a small vent, bolts along the beam and the column, light wear and faint grime streaks running down, heavy industrial. | 2nd | `--size 1024` |
| `deck.png` | Floor of an industrial spaceship hangar seen from straight above: large square heavy steel deck plates, some with a diamond anti-slip tread pattern and some plain, recessed seams between the plates, bolt heads at the plate corners, a strip of drainage grating, light scuffs and tyre marks, heavy industrial. | 2nd | `--size 1024` |
| `plating.lum` | Weathered old cargo spaceship hull: large rectangular welded steel plates in staggered rows with clear dark seams between them, shallow dents, light scorch marks, faint grime streaks, a few patch repairs. (Earlier hull stream, commit 96683e4; its own suffix; Grok Imagine.) | picked from six | 512 raw `.lum`, used by style 0 |

`station.png` is drawn in code: `tools/art/plate_tile.py OUT --seed 11`
with its defaults (1024, 16 cells, `--std 0.09`, 2 hatches, 2 vents). No
picture goes into it.

Each shipped file reproduces byte for byte from its raw image (or, for
`station.png`, its seed) with the options shown. Four more calls were made
and not shipped:
- a first armour prompt, which gave hex plates (hence "no hexagons" in the
  second)
- two station prompts: a repeating grid of big plates with ribs and
  conduits, then broad armour plates in an irregular layout (shipped
  before, `--size 1024 --cut`, 2nd variant). On the station's tan faces its
  irregular cells read as flagstones, and slanting faces turned it to
  boards.
- a third station prompt for a strict grid of rectangular plates. Grok
  gave a grid of equal square tiles: its seams run the whole length of the
  tile, which a slanting face draws out into long lines again, and the
  grid does not repeat on the tile's edge. The tile is now drawn in code
  instead, with plates of mixed sizes so that no seam runs far
- a first hangar prompt: ribbed bulkheads with vents and grilles, replaced
  by the larger wall panels, beam and column above

All of these were superseded by the prompts above or by the drawn tile.

Drawn in code (no file): the seeded plating that styles 1 to 4 use, and
that style 5 uses when a tile is missing. This is `hull_bake` in `hull.c`:
a kd-tree panel layout on a torus, from a seed.

## Checked

- **Build and replay:** `make -f Makefile-C` builds. The oracle replay of
  `tools/oracle/fixtures/route.fe2rep` ends on the same line as before.
  Every read of the 68k's state is bounds-checked, and nothing is written
  to its memory.
- **Docked hangar:**
  - walls plated and floor in deck plate
  - hazard stripes plated; the doors carry their own plates and move
    with them
  - lamps and signs flat
- **Ships at several angles in the shipyard:** Shuttle, Adder, Krait (two
  liveries), Asp, Gecko and Cobra.
- **Cobra in flight:**
  - near and far, with no shimmer at range thanks to the fade
  - lit side, and the dark underside, which keeps the game's own shade
- **Station:** close and far; the Lave approach, the docking face at a
  few hundred metres, and the launch out through the slot. The chamfers
  and the slot's surround take the station's plates (`FE2_HULL_DBG=1`
  shows their graze, 2.29); the steepest of them fade to the flat tan, and
  no face shows boards or flagstones.
- **Off switch:** `--flat-hulls` draws the earlier build's flat faces.
  Styles 0 to 4 plate only the listed models too (they used to plate every
  face).
- **Intro and menu:** the title, the logo, the city and the credits are
  flat; the station behind the menu and the scenes' ships and stations
  (above) are plated.

Known limits:
- Bright, saturated liveries (an orange or red Krait) show little plating,
  and calm faces less, while grey ships are plated fully. The paint mask
  keeps their colour clean; the fleet is uneven for it.
- The lamp and pure-colour tests (lamps, signs and paint stay flat or are
  plated) read the face's colour as the game hands it over, already lit.
  The unlit colour is not available to the host (the 68k lights the face
  before the draw call), so a face near the threshold can change between
  flat and plated as the light changes. The bay-box rule above is
  geometric and does not.
- Signs are treated by colour and by their decals: a yellow sign is a
  marking and a sign that carries lettering hosts decals, and both stay
  flat. A large sign of another pure colour with no lettering of its own
  would be plated as paint.
- Only the station has a graze fade. A hangar wall seen edge on from the
  deck could still stripe.
- A part's plates are its own, so they do not line up with the wall
  around it. A face moved by its vertices keeps its plates by matching its
  corners frame to frame (above). The match is a heuristic: a new copy of
  a face (same model, seed and address) that appears next to one just
  drawn could borrow its offset for its first frame.
- The binding of moving faces was checked from logged corner values (the
  290 door's fixed edge stays at the same tile coordinate through the
  slide and through the closed door's origin jump), not yet by eye.
- Some of the intro's parts (76, 206 and others above) are not identified
  and are flat.
- The hyperspace and fade-in flashes were not captured with plating.
- The Viper, Python, Moray, Eagle and Transporter were not met in the test
  captures. They use their classes untested in-game.
- A change of video mode (`change_vidmode`) recreating the GL context was
  checked by reading the code only: the textures and the shader are made
  again on first use.
