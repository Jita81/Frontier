# Hulls: a material per class of ship

The GL renderer paints the flat faces of the ships and stations the game
draws (and the station's docking hangar) with a plating material. Every
other model (the intro's titles, banners and scenes, text, planets,
landing pads, missiles, debris) is drawn flat, exactly as with
`--flat-hulls`. A surface port's buildings get storeys and windows
(below); its ground, roads, roofs and sign boards stay flat. The
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
| building | none: always the seeded `building` profile | one cladding, four storeys a tile, sky-tinted glass ribbons on a pane grid (a surface port's walls) | 65536 |

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
| 22 | small twin-pod utility hauler | civilian, panels x1.5 |
| 48 | small flat wedge trader | civilian, panels x3 |
| 54 | faceted armed wedge, toothed nose | armour, panels x8 |
| 60 | swept-wing passenger transport | civilian, panels x6 |
| 62 | long faceted hexagonal hauler | freighter, panels x6 |
| 72 | oval disc freighter, four drives | freighter, panels x8 |
| intro's 72 | the orbital station behind the menu | station |
| 80 | orbital station outside, close up | station |
| 82 | launch structure | station |
| 84 | docking bay seen through the slot | hangar; its faces out of the bay's box (the station's outside) station |
| 88 | large cross-armed orbital station, its slot tunnel too | station |
| 288, 290, 294 | hangar interior and its parts | hangar |
| 96 | a surface port: its buildings, apron, roads and signs | building (walls only, see below) |
| 18 | escape capsule | capsule (see Small objects) |
| 184 | jettisoned cargo canister | cargo (see Small objects) |

A ship's panel scale puts one to one and a half tiles across its biggest
face, measured in the shipyard's view (the Viper's biggest face is some
44000 units, the Python's 110000). At x1 a tile repeated four to nine
times across a face and read as crazy paving. The fighters use the armour
tile: the fighter tile's fine diagonal weave, once big enough to see,
read as wood grain on an orange Krait.

### Census (B2, slice 1)

`FE2_HULL_LOG=1` names each model once a session, so the count is the
number of census sessions (of ten: three traffic routes,
the docked start, and six shipyard visits at two surface
ports) that met the word, not frames. Each ship was then opened in the
shipyard's view and its scale set by eye, base against branch, to one to
one and a half tiles across its biggest face.

| word | shape and role | unit | sessions | where | class | panels |
|---|---|---|---|---|---|---|
| 22 | small twin-pod utility hauler (cargo boxes slung between twin cones) | 2^7 | 1 | shipyard | civilian | x1.5 |
| 48 | small flat tan wedge trader | 2^9 | 1 | shipyard | civilian | x3 |
| 54 | tan faceted armed wedge, toothed nose | 2^11 | 1 | shipyard | armour | x8 |
| 60 | white swept-wing passenger transport | 2^10 | 1 | shipyard | civilian | x6 |
| 62 | long faceted hexagonal hauler | 2^10 | 1 | shipyard | freighter | x6 |
| 72 | grey oval disc freighter, four drive glows | 2^11 | 1 | shipyard | freighter | x8 |
| 76 | giant blocky grey bulk freighter | 2^10 | 1 | shipyard | not listed (flat faces stayed flat) | - |
| 10, 12 | tiny stubs met at a surface launch | 2^7 | 2, 1 | launch | not listed | - |

Traffic met in the three routes was the listed ships only; none of the
new words was met in flight, so the proof is shipyard close-ups. The
other orbital station types (86, 88) were not met here; B5 met 88.

**B5, the large orbital station (88).** Route:
`tools/drive/scripts/p2-stations.drv`. From start 2 (landed at a surface
port of the fourth planet) it buys a ship with an autopilot, lifts off,
targets the orbital station beside that planet on the system map and
lets the autopilot dock at 100x (the game slows itself near the
station). `FE2_HULL_LOG=1` names it `model 88 -> class station`, unit
2^16. 88 is a long central shaft with broad arms swept out to either
side and lattice trusses at its ends; the slot and its tunnel are faces
of 88 itself, and docking goes on to the hangar words 288, 290 and 294
already listed, so 88 needs no bay row (84 has one because it is a
separate model). Scale x1, set by eye: at 800 m and in the slot the
shaft and the tunnel walls show several panels across each face, read as
plating rather than paving; the broad arm faces still read mostly flat
in the approach frames (seen at a grazing angle; not checked further).
The small station's route (start docked, launch, turn back) names the
same models with the same classes as base.

- **Adding a model:** add one line.
- **Adding a class:**
  1. Make a tile (below).
  2. Add an enum entry in `hull.h` and a row in `hull_classes[]`.
  3. List the PNG in `.githooks/allowed-images`.

Not listed, so drawn flat:
- game table 32 (the landing pad and the landed ship's markings) and 198
  (the port's clouds, not a city as this page used to say);
- game table 76 (unit 2^10), a giant blocky bulk freighter: plated, its
  biggest faces stayed flat at x6 and x12 (only its pods took the tile);
  its record is a 34-byte stub that draws other models, so the census
  left it out;
- game table 12 and 10 (unit 2^7, 34-byte stubs), met only during a
  lift-off; not identified;
- game table 86 (unit 2^13): by the backdrop code's orbital range
  (`hdback.c`, port models 84 to 88) the remaining orbital station type,
  thought to be the one at a station orbiting a moon (not seen). B5's
  route could not target such a station (the system map would not
  recentre on the inner planet to pick its moon's station), so it is not
  listed; a route would first fly to that planet under autopilot, then
  target the station from close in;
- game table 176 (unit 2^0), seen beside the bay, and 148 (ground,
  around a surface port); not identified (98, met beside it, is the
  airless port's buildings: below);
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
- **Effects (B15):** the intro's clouds, fireballs and beams are parts
  too, named by a small table of their own (`hull.c`, `intro_fx`, read by
  `hull_intro_fx`), apart from `hull_models[]`. Its key is
  `hull_intro_fx_part` (`gl.c`): the part word in the scenes and also in
  record 72 (the credits and the start menu), whose parts are the scenes'
  words while its plating keeps the record's key. Every word below was
  found by logging each line, teardrop, blob and circle put on the idle
  intro with its record, part word and colour (a scratch logger, not in
  the tree).

| intro word | what | HD path |
|---|---|---|
| 214 | the hypercloud's discs (blue) | the game's hypercloud glow (`hyper_tag`, `BLOB_HYPER`); in the scenes since #65, in record 72 new |
| 94 | an orange cloud a ship leaves | the hypercloud glow |
| 96 | a pale cloud a ship comes out of | the hypercloud glow |
| 114, 116 | the fireball's circles, in a scene and in the credits | the explosion fireball (`fx.c`, `put_model`, as model 188) |
| 80, 92, 132 | red beams (ground and space) | a red pulse laser, look 1: travelling bolts with a muzzle glow in the beam's colour (`fx_laser_line`) |
| 86's plumes | the green ship's engines | the HD plume (halo, white core, flicker; `exhaust_intro`) |

- **Plumes:** a part's plume is HD when its word has a `hull_models[]`
  row that is not a building's (86 is the only such ship with a plume) or
  an `EXHAUST` row of `intro_fx` (none yet). The flicker is seeded by the
  record, the word, the plume's place in the frame and the game's clock,
  so two ships of one scene do not flicker in step and a replay draws the
  same.
- **Left flat:** 144's orange teardrops (on a part not listed: flames,
  not a ship's engine), the red beams with no part word in records 56 and
  68 (part -1, the record's own model), 126's circles beside the fireball
  and the lines and circles not listed above.
- **Clock:** in the intro (bit 5 of 598(a6) set, not the death camera's
  state `$58` at 704(a6), and not `--flat-fx`) fx's clock is 20 ms per
  drawn 3D frame, not the wall clock, so a fireball's age and a bolt's
  travel are the same in every run.
- **Off:** `--classic-ui`, `--flat-hulls`, `--flat-fx`, `--pixel-*`
  (`hull_intro_base`: a scene's key is then its record's, as in the base,
  never marked `HULL_TAB_SCENE`, so the rows above are not reached and
  `hull_intro_part` is 0) and `--old-renderer` give the base intro.
- **Lights:** #57's halo tests the record's key (`hull_record_key`), as in
  the base, so a part whose word is in the stars' range keeps its halo.
- **Gate:** `fx.c`'s `put_model` tests the intro first (bit 5 of 598(a6)
  set and `hull_intro_fx` FIRE, or LASER in an intro frame, for
  `hull_intro_fx_part`); any other part falls through to the flight and
  death-camera gate, as in the base, so the intro does not depend on
  42(a6) and the death camera (bit 5 set too, but the game's table, so no
  intro part) keeps its blasts and smoke. The intro frame is latched in
  `fx_frame_begin` (bit 5 set and 704(a6) not `$58`), never re-read at
  draw time; the game data pointer cannot latch it, as it names the
  intro's table only while a model is drawn. The fireball takes the
  explosions' layered look (weapons.md).
- **No carry-over:** a fireball is drawn from its circle at once and a
  beam's bolt and glow live 90 ms of the intro's clock, and a change of
  scene empties fx's ring; on the newgame tape no pixel of the first
  flight frames moves by more than 24 of 255 against the base (run to run
  noise, no beam or glow).

## A surface port's buildings

Census (`FE2_HULL_LOG=1` with `FE2_HULL_DBG=1`, landed at the
atmospheric port of the second start position, then a lift-off to some
600 m; every word the port draws, by what it looks like):

| word | unit | what it draws | here |
|---|---|---|---|
| 96 | 2^18 | the whole port: one large trapezoid block (grey, a blue-violet sloped front of some 136000 units, a dark blue band), long low grey and white blocks (64000 to 490000), small white and grey boxes (5000 to 12000), sign boards (a pink frame round yellow, blue-violet or white panels of some 32500, each with lettering), the apron and taxi strips, roads, lakes and the hills (complex shapes); some faces move | listed: building |
| 198 | 2^2 | the white clouds | flat |
| 32 | 2^8 | the pad and the landed ship's yellow markings and cyan windows | flat (a pad) |
| 58 | 2^10 | the player's ship | as before |
| 176 | 2^0 | nothing visible changes when it is plated | flat |
| 12, 10 | 2^7 | met during the lift-off only | flat |

Word 96 is one model for all of it, so the class rules pick the walls:

- A face that looks along the model's y axis (`HULL_DECK_AXIS`: the
  apron, the pads, the strips painted on the ground) stays flat, unless
  it is a roof (below).
- A complex shape (more than four sides: the ground, roads, lakes, hills
  and the boards' frames) stays flat.
- A face whose longest side is under 0.6 of a tile (some 39000 units: the
  sign boards' lettered panels, kiosks, the small boxes) stays flat. Under
  a storey or two the windows do not read, and the boards keep their
  lettering clean.
- Yellow markings and bright pure colours stay flat, as for every
  building class.

Everything else is a wall and is plated. The class has no tile: it always
bakes the `building` profile in `hull.c` (`paint_storeys`), from the
profile's own seed, never from the game (B6, slice 2):

- four storeys a tile (16384 units each). Each storey is a floor line at
  its foot, a spandrel of cladding, then a ribbon of glass from 0.40 to
  0.88 of the storey behind thin frames;
- panes a sixteenth of a tile wide (32 texels at the 512 bake), with a
  column every four panes running through all the storeys. Every line
  lands on the grid the tile wraps on, so no pane is cut at the wrap;
- one cladding for the whole tile, concrete with a fine grain or plain
  metal (from the seed), so a block reads as one material and not as a
  patchwork of slabs;
- a pane's tone (blinds drawn, a lit room: about one in sixteen) is
  `hull_mix` of the seed, the storey and the pane, never the game's RNG
  or the clock, so it is the same every frame and every run. It rides in
  the tile's spare channel (HZ) with the glass mask, not in the albedo:
  the shader applies it only while a screen pixel covers under about a
  hundredth of a tile, so near glass keeps its blinds and lit
  rooms and glass at middle distance and beyond reads as one even tone
  (no blotches, no soft dark patches on a big glazed front).
- The shader (`hull_fs5`, uniform `uB`) moves the glass's hue toward the
  sky's colour, `MainRGBPalette[fe2_bgcol]`, read on the host each frame
  and keeping the glass's own luminance, so a pane reads as glass in
  daylight and follows the sky as it changes. The tint scales with the
  sky's luminance (none under a black or very dark sky, full from about
  0.12) and each channel is clamped at 1, so a saturated sky cannot push
  a channel over; nothing is added, and the luminance stays the game's
  colour times the tile.

`gl.c` (`hull_storey_fit`) lays each wall on the grid: its storeys start
at the wall's foot (the lowest corner along the port's y; the tile's v
runs up that axis), and its panes start at one end. Across, the wall is
stretched to a whole number of panes, by at most half a pane over its
width (a tall sliver narrower than one pane takes one: twice over at
half a pane, more on a thinner sliver). Up, a
wall over half a storey high is stretched to a whole number of storeys,
by at most half a storey, so its top storey is never cut. So a wall
never begins or ends mid-pane or mid-storey. The grounding is measured
from the port's ground (the y = 0 of the port's frame, not of the part
the wall belongs to: a part's own y = 0 can stand well over or under the
ground, which darkened a walled town's lower storeys in slice 3), not
from each wall's own foot: the
lowest 0.15 of a tile above the ground (some 9800 units, about half a
storey) darkens, by up to 0.3 at the ground, so a wall raised on or
stacked over another, or a fascia, has no dark band mid-facade. It fades
with distance like the tint. The class's graze (2.30, the station's) fades a wall seen edge on to
its own colour, and the footprint fade does the same far away (the glass
tint and the grounding fade with them), so the windows do not shimmer
from the air.

The airless port of the first start position draws its buildings with
another word, 98 (unit 2^16): dark towers with the game's own lit-window
polygons, low blocks with coloured roofs, sign boards and the domes'
lattice of lines (B6, slice 3). Census (`FE2_HULL_LOG=1`, landed and a
lift-off): the lettered boards (two colours) host lettering and stay
flat, one black face has no frame (edge on) and stays flat, a few faces
move against the model and keep their tile bound to them, and the roofs,
ground and pad (deck axis), the yellow markings and the domes' lines
(not faces) stay flat by the class rules; the walls are plated. It is
listed at scale 1, so a storey is the same 16384 units of the model's
frame as at the terran port, and with the whole-storey fit (above) the
towers' own lit windows sit one to each storey (3x crops beside the
landed ship). At scale 1 without that fit they fell between storeys and
read doubled; at scale 4 the low blocks took one storey too tall for
them.

Roofs and facades (B6, slice 4). A deck-axis face square to the port's
up (|N.y| at least 0.985 of |N|, so not a dome's or an arch's facet)
with every corner 0.15 of a tile or more over the port's ground (not the
apron, a pad or a road) is a roof. It takes no storey tile: its colour,
a quarter desaturated and 0.88 as bright, carries faint seams on a
quarter-tile grid, a seeded patching of a few percent per cell, and a
parapet at its edge (a light band, an inner shadow), all fading with
distance and footprint so it reads as one flat tone from the air. Each
building picks a facade from `hull_mix` of its model key and where the
port's models place it: the chain of sub-object commands that link its
part under the port (each frame keeps the address of its command at
-202), so one model placed twice is two buildings. A part placed by a
model of scale 2^10 or under is a piece of that model's building and
shares its facade. The facades are ribbon, punched or curtain wall; a
pane density of 1, 3/4 or 5/4; a tone within 4 percent either way.
No RNG, no clock and no view enters it, and no position either (the
part's origin in the port's frame is rebuilt each frame from the 68k's
view-space integers and drifts with distance), so a building keeps its
facade every frame, on the ground and in flight: over both lift-offs
not one part changed facade, where keying on two-tile cells of that
origin changed 3 of 73 parts on one port and 21 of 216 on the other.
Everything is the shader's: no image and no geometry are added, and
`--flat-buildings` still draws the game's own colours.

The ground (B7b). On words 96 and 98 a complex shape is the port's
ground when its colour is grey (its channels within 24 of each other),
its plane is within 3 degrees of level (|N.y| at least 0.9986 of |N|,
Newell's normal of its contour in the model's frame) and every point of
its contour lies within 0.02 of a tile of the model's foot: the apron,
the roads and the taxi strips. Signs and their lettering stand upright,
roofs and the low blocks are well above the foot, and the hills slope
(|N.y| 0.82 to 0.97, climbing tiles), so none of them pass; the pad and
its painted numbers are another word (32) and never reach this test. A
green or blue shape at the foot (the terran port's grass and lakes, the
airless port's gardens under its domes) is level too, but it is not
paved, so it is left out by its colour: slab joints on grass or water
would be wrong, and a look of its own is a later step. The ground is
drawn by a small shader in the model's frame, in building tiles (one
tile some four storeys): a fine value-noise grain of a few percent,
slabs a third of a tile across with thin dark joints and a faint tone of
their own, and broad worn patches. Each scales the shape's own colour,
so its hue is kept and its mean stays about the same, and each fades as
its cells shrink toward a pixel, so the far apron is the game's grey. It
does not touch the decal chain (a host stays a host, its lettering is
drawn as before), adds no geometry, and depends on no clock, RNG or
view. `--flat-pads` draws the ground flat, exactly as before. On the
airless port little passes: the wide grey round its pad is not one of
word 98's complex shapes (a tint of every accepted shape left it
untouched), so it stays flat, and its level shapes are mostly gardens.

Seeded ground props round a port (trees, shrubs, ice spires; W11b, #81)
were tried and removed at the operator's call: props are not terrain and
make no sense in a base. The effort on a port goes into its buildings.

## Wear: each ship its own (B12, slices 1 and 2)

Two ships of one model used to carry the same plating to the texel. Style
5 now lays a broad, low-frequency wear over each object: patches of warm
grime and of paler, sun-faded plating, a few plates across, that differ
from ship to ship and stay put on the hull.

- **The key.** The object record's long at `+118` (the same per-object
  seed `procfx.c` and `planet.c` read as `OBJ_SEED`). At put time
  `hull_obj_seed` (`gl.c`) reads the record through `a6-212`, as the model
  key does, and puts the seed into the znode stream as one more long after
  the face's context (`hull_put_ctx`, `hull_put_cctx` for complex shapes).
  A part drawn for a ship carries the ship's record, so the whole ship
  shares one seed. Confirmed with `FE2_HULL_LOG=1`, which logs each new
  record and seed once (`hull: wear record ... model ... seed ...`): in the
  traffic route three ships of one model came from three records with
  three different seeds, and each logged once in about a minute of frames,
  so the seed holds for the object's life. The player's ship has its own.
  The station and its hangar and deck share one record's seed (one object),
  and one small object type was seen to set its seed once, as it was made.
  Reads are bounds-checked; an unreadable record gives seed 0.
- **The noise.** Two octaves of the procedural library's value noise
  (`pf_fbm`, the same "hash without sine" as the skies), in the model's
  own frame: the face's corners as the bays and decals use them
  (`w = M(v - o)`), divided by 0.9 of the model's tile (`panel` of the
  model's class times its scale), which is some 4 to 6 plates, plus an
  offset hashed from the seed. It is the model's tile, not the face's: a
  station's hangar faces (drawn as station or deck) and an alcove door
  (tile x 0.55) used to take their own, so the patches changed size from
  face to face of one object; now one object has one wear scale. It is
  passed as a fifth texture coordinate (`hull_wear_at`), so the wear rides
  with the hull, runs on across face edges with no seam and does not move
  as the ship or the view turns. No game RNG and no clock.
- **The bounds.** `ws = 2 smoothstep(0.30, 0.58, fbm) - 1` times the
  face's weathering (`uQ.z`: half on a decal's host, 40% on a painted
  face, and it fades with the plating far away). The smoothstep gives the
  patches firm edges (slice 1 had a linear ramp, `6 fbm - 2.6`, and a
  gain of 0.11, about +1% and hard to see). Grime (`ws < 0`) scales the
  colour by `1 + 0.16 ws` and takes a slight warm tint (1.04, 1, 0.92);
  fade (`ws > 0`) scales it by `1 + 0.2 ws`. Grime is held lower than fade
  so it reads as weathering, not dirt, and because one object's few
  patches can fall mostly on the dark side: with an even +-0.2 the
  player's ship in the traffic route came out 4.3% darker than base.
  The centre, 0.44 of a noise whose mean is 0.5, leans the mean paler.
- **Paint.** Fade loses up to 30% of its saturation, times the face's
  bare-metal weight (`paint`: 1 on grey metal, 0 on a saturated face), so
  bare metal looks sun-faded and a painted face keeps its colour instead
  of going paler and greyer.
- **Measured** (1600x1200, mean luminance of the plated pixels, base =
  slice 1, flat = `--flat-wear`): the player's ship in the traffic route's
  external view (runs line up; the pixels the wear moves) flat 81.0, base
  81.7, slice 2 83.1: +1.7% on base, +2.6% on flat, the 5th to 95th
  percentile of the per-pixel ratio to base 0.92 to 1.10. The port's
  building in the landed route's view 3: +1.3% on base. The station in the
  dock route's approach (runs do not line up, so the station's own pixels
  in each frame): base 119.9, slice 2 123.4, flat 119.0, +2.9% on base.
  The player's ship from behind in the dock route's autopilot shot: +5.0%
  on base, but base and flat differ by 5.9% there, so that one is mostly
  the route's run-to-run noise. None is darker than base. The 3D view's
  draw time in the traffic route (`FE2_HULL_LOG=1`, three runs each,
  one game at a time, alternating) has a median of 2.23 ms a frame for
  base and 2.04 ms for slice 2: no cost beyond the noise (single runs'
  medians range 1.8 to 2.5 ms).
- **Where.** Style 5 with its shader only: every plated class (ships,
  stations, hangar, deck, buildings). A face with no model frame gets
  none. Styles 0 to 4, and style 5 in fixed function, have no object frame
  to hold a seam-free pattern, so they get no wear.
- **The intro.** Its scene objects take wear like any other, but every
  scene record logged carried the same seed, so the intro's ships are worn
  the same way each time it plays, and two of one model there would look
  alike.
- **Off.** `--flat-wear` puts nothing extra in the stream (the seed long
  goes in only for style 5's context, and not under the switch), sends no
  fifth coordinate, sets no wear coordinate and compiles the shader from
  its original single source, so its frames are those of the build before
  the wear (slice 1 checked that). In the landed route the 3D view of all three shots was bit for
  bit that of slice 1's `--flat-wear`; view 3's whole frame matched only
  within base's own run-to-run variance (the console clock can read a
  second apart), not bit for bit. The traffic route does not repeat even
  for base, so there it is within that variance too. `--flat-hulls` draws
  no plating at all, so no wear either (in the landed route its 3D view
  was bit for bit slice 1's `--flat-hulls`).

## Small objects (B10, slice 1b)

The jettisoned cargo canister (word 184) and the escape capsule (word 18)
are listed in `hull_models[]` at x1, each with a class of its own.

- **What the game draws.** Close up, the canister's body is no polygon but
  a cylinder: `Nu_PutCylinder` and `Nu_DrawCylinder` in `gl.c`, two discs
  and a tube of 20 slices (`gluCylinder`), fixed-function lit. Face
  plating never reached it, so the hook is the cylinder itself.
- **The hook.** `Nu_PutCylinder` adds the model's context to the znode
  stream after the cylinder's own words: with style 5 (`hull_ctx` 2) the
  complex-shape context (`hull_put_cctx`: key, object seed, light, model
  frame); with styles 0 to 4 (`hull_ctx` 1) the model key and the light.
  `Nu_DrawCylinder` reads it back. Only when the key's class is `cargo`
  or `capsule`, in the GL renderer, is the side drawn by `cyl_side`
  instead: the same 20 slices as explicit quads through `hull_paint`, so
  they take the class's plating in the model's own frame
  (`w = M(v - o)`, as a complex shape's faces): the plates are fixed to
  the object and turn with it. The caps are drawn as before (`gluDisk`,
  not plated). Every other cylinder user reads the extra words and then
  makes exactly the GL calls it made before.
- **The light.** `cyl_side` lights each slice edge as GL's fixed function
  lit the tube (ambient 0.2 of the colour, diffuse 0.8 of the light,
  GL's 0.04 global ambient, the game's integer colours as GL maps them),
  so the side keeps base's shading and mean colour; the plating then
  modulates it as on any face. GL's light, normal, current colour and
  `hull_chain` are left as base's side left them.
- **The classes.** `cargo` (profile `HP_CARGO`): big plates, banded on most
  plates (0.75), no hatches. `capsule` (`HP_CAPSULE`): hatches on most
  plates big enough for one, deep grooves, heavy wear, less sheen. Neither
  has a tile; each bakes its profile in code, as the building class does.
  Both sit before `HC_STATION` in the enum (not buildings), and their rows
  in `hull_classes[]` sit in the same place (the array is positional).
- **The bake.** The two classes bake at 256x256, not the 512 of the other
  seeded classes: a texel per model unit at panel 256, about one to two
  screen pixels per texel at the closest view, and a quarter of the work.
  With style 5 they are made with the station class, inside that load
  hitch (`hull_begin5`), so a canister's first sight in flight bakes
  nothing (`FE2_HULL_LOG=1`, load about 15: station 102 ms, then cargo
  5 ms and capsule 4 ms in the same frame; at the jettison, no bake). A
  game that never makes the station class makes them lazily, as any class
  (cargo alone measured 8 ms at 256 under load, 13 to 23 ms at 512).
  The bands and the capsule's "darker" are tone only: plating keeps a
  face's mean brightness at the game's colour, as on every class, so no
  new colour (no yellow and black) is added.
- **The scale, measured.** With a cylinder log on the jettison route the
  canister's cylinder has radius about 51 and length about 202 model
  units. A panel of 256 units puts some 1.6 to 2.6 plates along it and
  two to three across its face. The capsule was not reached (below), so
  its 256 is not measured.
- **Near and far.** One key and one class at every level of detail: the
  faces the game draws for a canister further off go through `hull_paint`
  with the same key as the close cylinder's side, and the style-5 shader
  fades the plating to the face's own colour on both once a texel is
  smaller than a pixel, so the tone does not jump when the game swaps
  levels (by construction; no capture of the swap).
- **The capsule.** No drive route reaches one: none was found to launch
  from the start ship, and none was met in the routes. It is listed so that one met
  later is plated, but no capture shows it.
- **Other cylinder users.** Logged on the routes run (a cylinder log
  build, not committed): models 32, 52 and 58 (ships' parts), 184 (the
  canister) and the intro's 72. Only 18 and 184 change: the landed route,
  which draws 32, 58 and the intro's 72, gave base-identical frames.
- **Route.** `tools/drive/scripts/p2-jettison.drv` jettisons a canister,
  pauses, turns the external view on it and zooms one step: the canister
  side-on some 20 to 25 m from the eye, about 230 px tall at 1600x1200.
- **Off.** `--flat-hulls` and `--old-renderer` give `hull_ctx` 0: nothing
  extra is written or read and the side is `gluCylinder` as before.
  `--classic-ui` and the `--pixel-*` modes follow the ships: the far
  faces go through `hull_paint`, which does not test them, so the close
  side does not either, and near and far agree under every flag.

## Switches

| switch | effect |
|---|---|
| `--hull-style 5` | the per-class material (default) |
| `--hull-style 0..4` | the earlier looks, unchanged pixel for pixel: 0 fixed-function plating (`art/hull/plating.lum`), 1 big seeded texture, 2 panels, 3 bump and weathering, 4 a seeded profile per model (its own table, by the low byte of the word, as it always was) |
| `--flat-hulls` | no plating at all: the game's flat faces exactly (buildings too) |
| `--flat-wear` | every ship of a model plated alike: no per-object wear, the 3D view of the build before the wear exactly |
| `--flat-buildings` | a surface port's buildings flat, everything else as it is: the frames before the building class, exactly |
| `--flat-pads` | a port's apron, roads and taxi strips (words 96 and 98) flat grey, everything else as it is: the frames before the ground shader, exactly |
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
- **Surface port:** landed (the three views of the landed tour) and a
  lift-off at some 130 m and 250 m. Only the walls of word 96 change; the
  pad, the ship, the sign boards and their lettering, the ground and the
  hills are the same pixels as before (a difference mask). With
  `--flat-buildings` the landed frames are byte for byte the earlier
  build's. The docked tour (stations and ships) was run in verification:
  the docked frame is byte for byte the same, and the others differ only
  through flight timing; word 96 is never met there. Slice 3 (98 listed,
  whole storeys, grounding from the model's ground, dark-sky guard, pane
  tone evened with distance): `--flat-buildings` and `--flat-hulls` are
  byte for byte the earlier build's on both landed tours (every view,
  title bar aside), and the pre-launch frame of the airless lift-off
  too. Slice 4 (roofs, facades, port-frame grounding): the same with
  `--flat-buildings` and `--flat-hulls` on both landed tours; ships,
  stations and the hangar unchanged on the dock and orbit tours.

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
  and are flat. Of its effects, 144's flames, the part -1 beams of records
  56 and 68 and 126's circles keep the base look (above).
- A surface port's buildings are told from its ground and signs only by
  facing, shape and size (the port is one model): a roof that is not
  level, a wall under 0.6 of a tile, or a large sign board of its own
  would be treated wrongly. The two start positions' ports (96 with an
  atmosphere, 98 airless) are plated; other worlds' ports were not
  captured. Every building shares one
  tile, so blocks differ only by the game's colours; a dusk sky was not
  reached in the captures.
- The hyperspace and fade-in flashes were not captured with plating.
- The Viper, Python, Moray, Eagle and Transporter were not met in the test
  captures. They use their classes untested in-game.
- A change of video mode (`change_vidmode`) recreating the GL context was
  checked by reading the code only: the textures and the shader are made
  again on first use.
