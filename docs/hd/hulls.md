# Hulls: a material per class of ship

The GL renderer paints the flat faces the game draws (ships, stations,
starport buildings and the docking hangar) with a plating material. The
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
and the reading of the model's frame). The art is in `art/hull/`, and
`tools/art/hull_tile.py` makes it.

## What it does, face by face

1. **Which model.** The engine's state names the object being drawn: the
   word at +90 of its record (via -212(a6)) is the model's offset into the
   game's model table (twice the model number). The renderer reads it
   without writing. The key is that word with bit 31 set.
2. **Which class.** `hull_models[]` in `hull.c` maps the whole word to a
   class and a panel scale. A model that is not listed gets `generic`.
   Parts drawn by a model, such as a hangar door, the hangar walls or
   landing gear, carry that model's word and so its class.
3. **Where the tile lies.** The tile is laid on the model's own frame, not
   on the face. Panels then keep their size as the ship turns or comes
   closer, and run on across neighbouring faces.
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
   - **Parts square to the object.** A part whose frame is the object's
     frame, or that frame by quarter turns and flips, takes the top frame
     whole. Its tile then continues the wall's: a painted stripe lines up
     with the wall's panels.
   - **Parts turned any other way.** These keep their own frame, so the
     tile moves with them.
4. **Replace or keep flat.** `hull_paint5` decides, for each face:
   - **Lamps, windows and lit signs stay flat.** These are bright faces of
     nearly pure colour (the brightest channel above 191 and the darkest
     below a fifth of it). In the hangar, near-white faces (all channels
     at 208 or more, the light strips) also stay flat.
   - **Large pure-colour faces in the hangar are plated.** These are paint:
     the band over the hall, the hazard stripes and the doors. The test is
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
     - In the hangar and on unlisted models they stay flat. These are signs.
   - **Complex shapes** take the plane of the face they lie on:
     - The hangar's own shapes (grey signs and lamps) stay flat.
     - A coloured one, which is the station's outside around the slot,
       takes the station plating.
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

## Caching and cost

- Each class's tile is loaded and baked once, the first time a face of
  that class is drawn. The two RGBA maps per class are kept:
  - t0: normal x, normal y, albedo and cavity
  - t1: hazard, gloss, grime and chips
- Nothing is loaded or baked per frame.
- Bakes take 12 to 84 ms each on the test Mac mini (generic 18, station
  72, hangar 78, deck 71) and happen once per session per class.
- Drawing the 3D view takes about 1.4 to 2.5 ms a frame, and up to about
  6 ms with a ship filling a 1600x1200 window.
- `FE2_HULL_LOG=1` prints:
  - each bake time
  - each model the first time it is seen, with its class, or a note that
    it is not listed
  - the 3D view's draw time averaged over 300 frames

## Classes

| class | tile | look | panel (model units per tile) |
|---|---|---|---|
| generic | `generic.png` | grid of steel panels, light scuffs (any unlisted model) | 8192 |
| police | `police.png` | clean glossy paint, thin panel lines, no wear | 10240 |
| civilian | `civilian.png` | worn paint, chipped edges, rivets, patches | 10240 |
| freighter | `freighter.png` | thick welded plates, patch plates, oil and grime | 12288 |
| armour | `armour.png` | thick bevelled armour plates, deep gaps, bolts | 8192 |
| fighter | `fighter.png` | sleek composite skin, fine lines, high sheen | 6144 |
| station | `station.png` | huge irregular armour plates, welded seams | 65536 |
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

All of these are on one line per class in `hull_classes[]` in `hull.c`.

Models (`hull_models[]`, by the whole type word; `FE2_HULL_LOG=1` names
the unlisted ones a session meets):

| word | model | class |
|---|---|---|
| 20 | Shuttle | civilian |
| 32 | landing pad | station |
| 40 | Krait | fighter |
| 42 | Gecko | fighter |
| 44 | Adder | civilian |
| 46 | Viper | police |
| 50 | Moray | civilian |
| 52 | Cobra Mk III | civilian |
| 56 | Asp | armour, panels x3 (a big hull) |
| 58 | Eagle | fighter |
| 66 | Python | freighter |
| 70 | Transporter | freighter |
| 72, 80 | orbital station outside, far and close | station |
| 82 | launch structure | station |
| 84 | docking bay seen through the slot | hangar |
| 96 | starport building | station |
| 198 | city | station |
| 288, 290, 294 | hangar interior and its parts | hangar |

- **Adding a model:** add one line.
- **Adding a class:**
  1. Make a tile (below).
  2. Add an enum entry in `hull.h` and a row in `hull_classes[]`.
  3. List the PNG in `.githooks/allowed-images`.

Words seen in test runs but not listed are 4 and 176. They get `generic`.

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
| `FE2_ART_DIR=dir` | where `hull/*.png` are looked for (default `art`); the macOS launcher points it into the app |

Fallbacks:

- **No GLSL 1.20, or `FE2_HULL_FIXED=1`.** Style 5 draws each class's
  tile in fixed function, as the face colour times the baked paint and
  groove tone. There is no bump, no sheen and no footprint fade; mipmaps
  and anisotropy still apply.
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

The nine tiles total 2.1 MB.

## Provenance of every art file

Every Grok image was made from a text prompt only. No game bitmap,
screenshot or capture went into any prompt or any processing step. Model:
`grok-imagine-image-2.0`. Each call made 2 variants at $0.06 each, so $0.12
per call. Ledger: 12 calls, **$1.44**.

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
| `station.png` | Exterior hull of a gigantic orbital space station: broad heavy armour plates of many different sizes in an irregular non-repeating layout, deep welded seams between the plates, sparse detail: an occasional small vent grille, a few rows of bolts, one rare service hatch, mostly large flat plates with subtle dents and surface variation. | 2nd | `--size 1024 --cut` |
| `plating.lum` | Weathered old cargo spaceship hull: large rectangular welded steel plates in staggered rows with clear dark seams between them, shallow dents, light scorch marks, faint grime streaks, a few patch repairs. (Earlier hull stream, commit 96683e4; its own suffix; Grok Imagine.) | picked from six | 512 raw `.lum`, used by style 0 |

Each shipped file reproduces byte for byte from its raw image with the
options shown. Three more calls were made and not shipped:
- a first armour prompt, which gave hex plates (hence "no hexagons" in the
  second)
- a first station prompt: a repeating grid of big plates with ribs and
  conduits, replaced by the sparser, irregular layout above
- a first hangar prompt: ribbed bulkheads with vents and grilles, replaced
  by the larger wall panels, beam and column above

All three were superseded by the prompts above.

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
  - hazard stripes and doors plated in line with the wall panels
  - lamps and signs flat
- **Ships at several angles in the shipyard:** Shuttle, Adder, Krait (two
  liveries), Asp, Gecko and Cobra.
- **Cobra in flight:**
  - near and far, with no shimmer at range thanks to the fade
  - lit side, and the dark underside, which keeps the game's own shade
- **Station:** close and far.
- **Off switches:** `--flat-hulls` and styles 0 to 4 match the earlier
  build pixel for pixel.

Known limits:
- Bright, saturated liveries (an orange or red Krait) show little plating.
  The paint mask keeps their colour clean.
- The station's grazing faces foreshorten the tile's long grooves into
  plank-like lines. The mapping is isotropic, so this is true
  foreshortening, not stretching.
- A moving part can switch between its own frame and the top frame at the
  moment it passes a quarter turn.
- The Viper, Python, Moray, Eagle and Transporter were not met in the test
  captures. They use their classes untested in-game.
