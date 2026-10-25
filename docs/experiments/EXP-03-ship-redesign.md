# EXP-03: Can a ship's model be redrawn by the host with a new mesh?

## Question

Can the host tell which game model each 3D primitive belongs to, along with that model's position and rotation? Can it then draw a new mesh for one ship in place of the game's own polygons, behind a default-off flag, while the game draws byte-identically with the flag off? And what would a legal pipeline for real replacement art need?

## Method

1. **Find the model table and the draw path.**
   - Every 3D object goes through `L39426_PutGameData3DObj`. The caller passes a draw request in a6:
     - bytes 0..17: a 3×3 rotation matrix (1.15 words);
     - bytes 20..31: the view-space position;
     - word 90: the model's byte offset into the model table at `L973c_game_data`. The model index is that offset divided by 2. Game objects carry the same word at offset 90.
   - The routine looks up the model header, copies the request into a fresh frame (`link a6,#-220`), and then:
     - `L39646_3DObjClipOffscreen` clips it, using the header's size word 14 shifted by its scale word 10;
     - `L39546_CalcRotViewNLight` takes the light vector into model space;
     - `L3e14e_SetupNProjectObj` stores the header at −4(a6) and runs the model's bytecode (`L3e0a4_Project3DObj`).
   - Sub-objects such as the undercarriage come from bytecode opcodes 0xd and 0x1d. These link another frame on top and copy the request pointer.
2. **Find how models reach GL.**
   - While the bytecode runs, it emits the `Nu_*` host calls 0x60–0x79: `PutTriangle`, `PutQuad`, `ComplexStart`/`SNext`/`End`, `InsertZNode`, and so on.
   - Each call passes view-space vertex pointers in a0–a3, a colour in d6, and a depth in d4 for `InsertZNode`.
   - `src/gl.c` records them into a z-sorted tree of nodes and replays it painter-style (far to near) at `DrawScreen`. There is no depth buffer.
   - Nothing in the host-call stream says which model a primitive belongs to. That information exists only in the 68k stack frame.
3. **The hook: host-side frame inspection, with no `fe2.s` change.**
   - The experiment harness already wraps every host call (`src/exp.c`, `trace_call`). With `FE2_MESH_LOG` or `FE2_SHIP_MESH` set, it passes each 3D primitive to `ExpMesh_Prim` (`src/expmesh.c`).
   - That function walks the 68k a6 chain to the top-level `PutGameData3DObj` frame. This is the frame whose saved a6 equals its own −212 request pointer, which holds only there because of `movea.l a6,a4; link a6`.
   - From that frame it reads:
     - the model index (request word 90 ÷ 2);
     - the rotation (−36);
     - the position (−16);
     - the header's scale and size;
     - the light vector (−198).
   - It accumulates, per instance, the primitive count, the colours, and the box of every emitted vertex in model axes.
   - Primitives with no such frame are never touched: 2D, the HUD, and the stars and planets drawn by other paths.
4. **The placeholder mesh.** `FE2_SHIP_MESH=<index>` swaps that model:
   - Its primitives are not recorded.
   - At its first `InsertZNode`, one new node type, `NU_EXTMESH`, goes into the same z-sorted tree (`Nu_ExtMeshNode`, `src/gl.c`). That depth is the one the game chose.
   - When that node is drawn, `ExpMesh_Draw` draws a generated hull: 6 rings × 8 segments plus nose and tail caps, 96 triangles, computed at start-up from a 6-point profile. It is:
     - stretched to the box of the game's own vertices for that instance;
     - placed with the game's position and rotation;
     - coloured with the game's most-used face colour;
     - flat-shaded with the game's light vector, using the game's rule (lit when n·L < 0).
   - Faces are culled against the camera with normals oriented away from the hull centre, because the view transform flips handedness.
   - The swap happens only when the camera is outside the game's own clipping radius (size << scale). Otherwise, for example in the Rear View from inside the player's hull, the original polygons are kept.
5. **Find a ship.** `FE2_MESH_LOG=<file>` logs every projected instance per frame. A scripted run ([`capture-script.txt`](evidence/E03/capture-script.txt)):
   - starts at Ross 154;
   - launches from the Merlin surface port;
   - uses F1 to cycle External, Front, Rear and External views (shots v1–v4).

   The log and screenshots identified model 16 as the player's own ship ([`model-census.txt`](evidence/E03/model-census.txt)).
6. **Identity and effect.** The same script was run five times:

   | Run | Binary | Flags |
   |---|---|---|
   | A | pre-E3 (404ccae) | none |
   | B | E3 | none |
   | C | E3 | `FE2_MESH_LOG` only |
   | D | E3 | `FE2_SHIP_MESH=16` plus log |
   | E | E3 | `FE2_SHIP_MESH=16` plus log, repeating D |

   For every run I compared the shown frames, the game's 320×200 buffers and the mesh logs.

## Result

**PROVED.** A default-off host-side hook identifies every 3D primitive's model index, position and rotation from the 68k frame, with no `fe2.s` change. With `FE2_SHIP_MESH=16`, it replaced the player's ship with a generated 96-triangle hull, placed, sized, coloured and lit from the game's own data. With the flag off, all 8 shots are md5-identical to the pre-E3 binary.

## Evidence

All in `docs/experiments/evidence/E03/`. The summary file is [`identity.txt`](evidence/E03/identity.txt).

- **Flag off.** A = B = C on all eight shots: v1–v4 as shown, and their `.st` buffers. Neither the build nor the observer alone changes a pixel.
- **Flag on.** D differs from A only in v1 and v4, the External View frames with the ship in view.
  - v2 (Front) and v3 (Rear, camera inside the hull) are unchanged.
  - All four `.st` buffers are unchanged, so the game's state and 2D output are untouched.
  - The mesh logs of C and D are identical apart from the `REPLACED` tag: the game projects exactly the same objects either way. 129 instances were replaced.
- **Where the picture changed.** 21,178 pixels differ in v1, all within x 461–844, y 305–442, which is the ship's footprint. [`v1-changed-pixels-mask.png`](evidence/E03/v1-changed-pixels-mask.png) shows them as a mask, with no game art.
- **Reproducible.** D = E for every shot, and the two mesh logs are byte-identical.
- **The model hook works for every object.** In one run it logged 9 distinct models and 6,346 instances:
  - the player's ship (16);
  - the starport complex (49);
  - starport buildings (99, 17 instances in one frame);
  - particles (97);
  - distant bodies (70, 74);
  - intro models (2, 36, 88).

  Each has the header scale and size from the game's table.
- **Screenshots.** Full-window before and after shots, and a before/after crop of the ship, are kept outside git in `phase2-evidence-local/E03/` in the shared notes folder, because they contain Frontier Developments art.
  - The original is a grey delta wing with cyan panels and two landing legs.
  - The replacement is a grey lozenge hull of the same span, length and height at the same place, lit from above.
  - The legs disappear too, because they are sub-objects of model 16.

## Hook needed

**What exists now** is host side only:
- `src/expmesh.c` (about 390 lines);
- a node type and its draw function in `src/gl.c` (about 20 lines);
- three guarded lines in `src/exp.c`.

When off, `expmesh_on` is 0 and nothing is called. It depends on the `PutGameData3DObj` frame layout. That layout is fixed, because `fe2.s` is a fixed binary, but it is undocumented, so the reads are bounds-checked and validated by the frame-chain rule.

**A production version should change four things:**
1. **An explicit hook instead of frame inspection.** Add two host calls, `ObjectBegin` (model index, request pointer, header) and `ObjectEnd`, at the start and end of `PutGameData3DObj` and the gamedata2 variant. That is a 4-line `fe2.s` patch, and it removes the dependence on stack layout. Frame inspection stays a good zero-patch option for experiments.
2. **A view-independent box.** Today the box comes from the vertices the game emitted that frame, so it depends on view and level of detail. Model 16 gives one box in External View and another from inside the hull. Production should decode the model's own vertex table (header word 2) once per model. Together with the header scale, that gives a box and anchor points that never change, including where sub-objects such as the gear, turrets and exhaust attach.
3. **Sub-objects and state.** The undercarriage, turrets and engine glow are separate bytecode sub-objects, controlled by game flags such as gear down and thrust. A replacement must either draw them from the same flags or swap them individually, keyed by sub-model index.
4. **Depth.** The 3D view has no depth buffer, so a swapped mesh is one node sorted at one depth. That is fine for a lone ship, but it is wrong where a ship overlaps a building or another ship. Replacement meshes need a depth-buffered pass, or must be split per face into the z tree.

## Oracle implications

- **No effect on game state.** The swap changes only what GL draws. The `.st` buffers and the projected-object log are identical, so a parity oracle can keep comparing the game's own output (host-call stream, `.st` buffers, memory) and ignore replaced pixels.
- **A new oracle input.** The mesh log is a per-frame list of (model, position, rotation, scale, size, box, colour). A renderer oracle can check that a modern renderer draws the same objects in the same places, independent of art.
- **Painter order.** The original 3D view is painter-sorted with no depth buffer. A modern renderer with a depth buffer will differ wherever the original sorted wrongly. The oracle needs a tolerance, or a per-object silhouette test, rather than pixel equality.
- **Fidelity check** (standing rule, 2026-10-25):
  - **Classification equals the game's record.** The mesh is selected only by the game's model index (request word 90 ÷ 2), never by guessing from pixels. Scale and size come from the model header (words 10 and 14). The box comes from the game's own vertices. The colour is the game's most-used face colour. Model 16 is replaced as model 16 in every view the game draws it from outside, and nothing else is touched.
  - **Two arrivals give identical derived parameters.** Runs D and E are separate launches. Each gives model 16 the same derived parameters in every External View frame:

    | Parameter | Value |
    |---|---|
    | scale | 8 |
    | size | 125 |
    | box | ±8442 × −1514..1512 × ±6304 |
    | colour | 666 |

    The swapped shots in D and E are md5-identical.
  - **Known gap.** From inside the hull, the game emits a different primitive set for the same model, which gives a different box and colour. The swap is skipped there. Production item 2 (decoding the vertex table) makes the derived parameters view-independent.
  - The new art may restyle a ship, but it must keep the game's class (model index), size, proportions and colour scheme, and must not add or hide what the game draws, such as gear, weapons or damage.

## Legal flags

- No Elite Dangerous material was downloaded, viewed or used. The placeholder hull is generated from a 6-point profile in `src/expmesh.c`.
- `fe2.s` and its model data are Frontier Developments copyright. The evidence holds only numbers derived at runtime: model indices, header words, boxes and colours. No model data is reproduced. Screenshots with game art stay out of git.
- **What a legal asset pipeline needs:**
  1. **Original or clearly licensed meshes and textures.** Use new work by contributors, or CC0 or CC-BY assets with attribution. Never meshes, textures or reference renders from Elite Dangerous or any other Frontier Developments product, whose EULA and copyright forbid extraction. Fan rips are out.
  2. **A provenance manifest per asset:** author, licence, source-file hash, the tool chain, and the model index it replaces. Add a CI check that refuses any asset without a manifest entry.
  3. **Contributor sign-off.** Use a DCO or CLA stating the asset is the contributor's own and is not traced from Elite Dangerous art. Add a review step that rejects designs substantially similar to Elite Dangerous ships.
  4. **Design inputs.** The game's own runtime numbers (box, proportions, colour, class) are facts, and are fine to design to. The original low-poly silhouettes are part of Frontier Developments' expression, so artists should work from the derived numbers, not trace the original polygons.
  5. **Separate distribution.** Ship packs go out apart from any FD material and are loaded at runtime, as `FE2_AUDIO_DIR` does for sound.
  6. **Names and logos.** Ship names such as "Eagle" and "Cobra" and the Elite and Frontier names are Frontier Developments trademarks. Packs should be keyed by model index, and ship names should be shown only as the game itself shows them.

## Effort to productionise

**M** for the engineering:
- the `ObjectBegin`/`ObjectEnd` host calls;
- a vertex-table decoder for view-independent boxes and attach points;
- a mesh loader for glTF or OBJ plus a manifest;
- a depth-buffered pass for replaced meshes;
- sub-object and flag handling (gear, turrets, thrust).

**L** overall once a full original fleet of licensed art is counted (about 30 ship models plus stations and buildings). That is content work, not engineering.

Phase 1 issues seen and not fixed:
1. The link step warns `reducing alignment of section __DATA,__common from 0x8000 to 0x4000`. It is harmless here.
2. The GL 3D view has no depth buffer, so intersecting objects sort by node depth only. This is original behaviour, recorded under oracle implications.
