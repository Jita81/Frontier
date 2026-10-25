# EXP-01: Can the 2D UI be rebuilt from host calls alone?

## Question

Can the 2D user interface (text, panels, maps, menus) be restyled on the host side from the host-call stream, without changing `fe2.s`? For each main screen, is it fully reproducible from host calls, or does it depend on pixels that `fe2.s` writes itself?

## Method

1. **Harness** (`src/exp.c`, `src/exp.h`, about 20 lines of call sites in `main.c`, `hostcall.c`, `gl.c` and `soft.c`). Every part is off unless an `FE2_*` environment variable is set. When one is set, `Exp_Init` swaps each `hcalls[]` entry for a wrapper. The wrapper records the call, then calls the original.
   - `FE2_TRACE_HCALLS=1` (or `2` to include `Idle`) with `FE2_TRACE_FILE=path` writes one line per host call: frame number, hcall index and name, d0–d5, a0, a1, the top three stack longs, and the decoded string for `DrawStr`, `DrawStrShadowed` and `QueueDrawStr`.
   - `FE2_SCRIPT=file` with `FE2_SHOT_DIR=dir` drives the game with no OS input injection, so no Accessibility permission is needed. Script commands: `key`, `mouse`, `click`, `wait <frames|text>`, `shot`, `trace on|off`, `mark`, `poke`, `quit`. Input is fed into the game's own input state from `Call_HostUpdate`.
   - `shot name` saves two in-process dumps: the frame about to be shown (`glReadPixels`, or the SDL surface in the soft build) and the game's own 320×200 screen buffer, as `.st`.
   - `FE2_PROVENANCE=1` keeps a shadow tag for every byte of every screen buffer: the four screen pointers plus `VideoBase`. Bytes changed inside a host call are tagged with that call. Bytes that change between host calls can only have been written by 68k code in `fe2.s`, so the harness tags them `fe2s-direct` and reports them with the frame and rows. `Memcpy` copies the tags along with the bytes.
   - At each shot the harness writes a colour map (`.prov.png`) and a table (`.prov.txt`) of which writer produced every visible pixel.
2. **Positive control.** The script command `poke 100 60 15` writes 16 pixels into the visible buffer between host calls, the same way an `fe2.s` rasteriser would. Provenance must catch it.
3. **Capture.** One scripted run (`evidence/E01/capture-script.txt`, 959 frames) visits seven screens and shoots each one with tracing on for that screen only:
   - intro
   - cockpit (front view after launch from Sirocco Station, Ross 154)
   - galactic map (F2)
   - galaxy overview (F10)
   - system map (F2, then F6)
   - stock market (F4 → Stockmarket)
   - shipyard upgrades (F4 → Shipyard → Upgrades)
   - bulletin board (F4 → BBS)

   The run was made in the GL build (the default) and repeated in the software renderer (`WITH_GL=n`).
4. **Visible change.** `FE2_TEXT=hires` (GL build only, in the new file `src/exptext.c`) draws every `DrawStr` string at window resolution with a stroke font I wrote for this experiment, and hides the bitmap glyphs under it. `fe2.s` memory is never changed:
   - `DrawStr` records, for each glyph, which buffer bytes it changed, with the old and new values.
   - `Memcpy` copies those records along with the bytes, and `Memset` drops them.
   - Before the 320×200 buffer is uploaded as the UI texture, a copy of it is peeled. Each glyph whose bytes still hold what it wrote is restored to the background in the copy and redrawn as smoothed GL lines.
   - A glyph that something else has overdrawn stays a bitmap, so the screen can lose resolution but cannot show wrong pixels.
5. **Identity checks.**
   - Same script with `FE2_TEXT` on and off: compare the game's own screen buffers.
   - Same script on the final binary (all hooks compiled in, only the script driver on) and on the build that had no display hooks: compare the shown frames.

## Result

**PROVED.** All seven screens are drawn entirely through host calls: in 959 frames across both renderers, `fe2.s` wrote zero screen bytes outside a host call (only the 16-byte positive control was flagged). A default-off host-side flag (`FE2_TEXT=hires`) replaces all UI text with window-resolution vector text and leaves the game's memory byte-identical.

Restyling has a limit. Text, bitmaps (`BlitBmp`) and, in the GL build, all 3D and map geometry (`Nu_*` ZNode primitives) arrive as named calls with semantic arguments. A second group of pixels reaches the host only as horizontal spans whose geometry `fe2.s` computed: `OldHLine`, `HLine`, `FillLine`, `PutPix`, `BackHLine`. That group covers:
- the galaxy density image;
- the dark panel behind the station portraits;
- the scanner and gauge bars;
- the whole 3D view in the soft renderer.

The host can redraw span pixels, for example at a higher resolution or in a different palette. It cannot change what the shapes are unless someone reads their meaning out of `fe2.s`.

## Evidence

### Per-screen table (GL build, visible pixels of the 320×200 UI layer at the shot frame)

Sources: `evidence/E01/gl/<screen>.prov.txt`, `evidence/E01/hcall-histograms.txt`. In the GL build, 3D and map geometry is not in this layer at all; it arrives as `Nu_*` ZNode calls (counts are in the histogram file).

| Screen | UI pixels | Text (`DrawStr`) | Bitmaps (`BlitBmp`) | Spans (fe2.s geometry) | Written by fe2.s directly | Verdict |
|---|---:|---:|---:|---:|---:|---|
| Intro | 0 | – | – | – | 0 | Host-side. 100% `Nu_*` 3D (`ComplexBezier` 292, `ComplexSNext` 356, `PutQuad` 39, …) |
| Cockpit | 11,180 | 11.7% | 88.1% | 0.2% | 0 | Host-side. 3D view via `Nu_*`; panel bitmaps; spans = 2 gauge bars |
| Galactic map | 11,331 | 12.3% + 8.1% queued names | 79.4% | 0.2% | 0 | Host-side. Stars and grid via `Nu_PutLine`/`PutCircle`/`PutBlob`, names via `QueueDrawStr` |
| Galaxy overview | 64,000 | 4.4% | 14.1% | **81.5%** | 0 | Reproducible from calls. The density image needs fe2.s knowledge to restyle |
| System map | 10,860 | 8.1% | 91.6% | 0.3% | 0 | Host-side. Orbits via `Nu_ComplexBezier`/`PutCircle`/`PutBlob` |
| Stock market | 35,332 | 23.3% | 52.0% | 24.8% | 0 | Host-side, except the portrait backdrop (spans) |
| Shipyard upgrades | 36,914 | 27.4% | 48.8% | 23.8% | 0 | Host-side, except the portrait backdrop (spans) |
| Bulletin board | 31,523 | 25.2% | 49.1% | 25.7% | 0 | Host-side, except the portrait backdrop (spans) |
| Stock market + `poke` control | 35,332 | 23.3% | 52.0% | 24.7% | **16** | Control detected: 16 bytes, rows 60–60, frame 679 |

Run-wide counter, GL build: `FE2_PROVENANCE: 16 bytes written by fe2.s outside host calls (all tracked buffers, whole run)`. The 16 bytes are the control.

**Software renderer** (`evidence/E01/soft/`): the same seven screens give the same answer. The only fe2s-direct bytes in the whole run are the 16 control bytes, logged as `F679 buffer 0xf0000: 16 bytes by fe2.s, rows 60-60`. Here the whole 3D scene is drawn by `fe2.s` as host spans, so 79.5% of cockpit pixels and 81.4% of galaxy pixels are spans. The 2.2% tagged `initial` were written before tracking attached to that buffer; at the intro this is the bottom 32 rows of black.

Static screens (stock market, shipyard, bulletin board, galaxy overview) are drawn once into a back buffer. After that, each frame is only a `Memcpy` to the shown buffer plus the cursor, the control-panel gauges and the clock. That is why provenance has to follow `Memcpy`.

### Visible change: `FE2_TEXT=hires`

- `evidence/E01/stock-text-before.png` and `stock-text-after.png`: stock market list at 1280×960, flag off and on.
- `evidence/E01/bbs-text-before.png` and `bbs-text-after.png`: bulletin board adverts.
- `evidence/E01/panel-text-before.png` and `panel-text-after.png`: control-panel text over the galactic map, including the `QueueDrawStr` star names.
- Command:
  ```
  FE2_TEXT=hires FE2_SCRIPT=capture-script.txt FE2_SHOT_DIR=out ./frontier --size 1280 960
  ```
- Coverage at each shot, from `evidence/E01/fe2_text-coverage.txt`, last frame:

  | Screen | Glyphs drawn hi-res |
  |---|---:|
  | Cockpit | 78 |
  | Galactic map | 206 |
  | Galaxy overview | 194 |
  | System map | 69 |
  | Stock market | 447 |
  | Shipyard | 501 |
  | Bulletin board | 426 |

  The "fresh glyphs left as bitmap" counter is non-zero on the static screens. Those are superseded duplicate records left behind when the back buffer is redrawn. On full-window shots no bitmap-font glyph is left on any of the seven screens, except special glyphs ≥ 0x7f, which have no stroke definition and stay bitmaps by design.
- Full-window before/after shots of all seven screens are kept out of the repo, because they show Frontier Developments artwork (portraits, cockpit, ships). They are in the parent session's notes folder under `phase2-evidence-local/E01/`.
- Text the 3D renderer draws itself is not a `DrawStr` string, so it is unchanged. Two examples: the "CORE" label and "YOU ARE HERE" on the galactic map.

### Identity (`evidence/E01/identity.txt`)

- **`FE2_TEXT` on vs off**, same script, GL build. The game's own 320×200 buffer at all eight shots is **byte-identical**. The flag changes only what GL draws on top; it never writes to game memory.
- **All hooks compiled in, every flag off except the script driver**, vs the build with no display hooks:
  - intro, cockpit and galactic map: window frames are pixel-identical (md5 of decoded RGB is equal);
  - the other screens differ only in a 6×14-pixel box, the clock seconds digit (and the time-point readout on the system map), because the game clock runs from wall time.
- **No `FE2_*` variable set:**
  - `Exp_Init` returns before installing any wrapper, so `hcalls[]` is the e508341 table.
  - `Exp_HostUpdate` and `Exp_EndFrame` return at `if (!exp_any)`.
  - `DrawStr` and `draw_control_panel` test `exp_text_on` (0) and take the original path.
  - No game or host state is written.

  This is a code-path argument. It is not a screenshot of the unmodified binary, because without the harness there is nothing in-process to take one, and OS screen capture would need a Screen Recording permission, which is out of bounds for this work.

### Reproduce

```
make -f Makefile-C -j8
FE2_PROVENANCE=1 FE2_TRACE_HCALLS=1 FE2_TRACE_FILE=out/t.trace \
  FE2_SCRIPT=docs/experiments/evidence/E01/capture-script.txt FE2_SHOT_DIR=out ./frontier
```

## Hook needed

**Host-side only.** No `fe2.s` patch.
- Text: `DrawStr` in `src/gl.c` (two calls in the glyph loop) plus one call each in `draw_control_panel` and in the `Memcpy`/`Memset` wrappers.
- Bitmaps: the `BlitBmp` arguments already name the bitmap (a0/d0), so they can be swapped host-side the same way.
- 3D and maps in GL: already semantic (`Nu_*`).
- What would need `fe2.s` work is giving meaning to span-drawn content: the galaxy density image, the portrait backdrop and gauge bars, and soft-renderer 3D. The fe2.s routines behind `OldHLine` (0x06), `HLine` (0x16) and `FillLine` (0x0b) would need to pass "what this is" (for example a panel id) to the host. Their call sites can be found by tracing the pc after each span call; the harness already logs the next pc for fe2s-direct writes.

## Oracle implications

- The provenance tracker is an oracle in its own right: "fe2.s writes zero screen bytes outside host calls" is a cheap invariant to assert in CI on any scripted run. If a Phase 1 change to the CPU core or host calls breaks it, the run fails loudly with the frame and rows.
- Golden screenshots are not deterministic, because the in-game clock (and `srand(time)` for the 3D scenes) follows wall time. Golden images either need the clock area masked (ST x 25–27, y 169–175, plus the system-map time readout) or a fixed-time hook. The game's `.st` buffer dump plus the host-call trace is the better oracle: it does not depend on the renderer, and on static screens it matches exactly apart from the clock.
- A host-side UI layer like `FE2_TEXT` can be checked with the same oracle: with the flag on, the game buffer must stay byte-identical to the flag-off run.

## Legal flags

- The stroke font in `src/exptext.c` (polylines on a 5×8 grid) was written for this experiment. It is not traced from any font file.
- The existing 8-pixel bitmap font `font_bmp[]` in `src/gl.c` is very likely Frontier Developments' original font data. It was already in the repository before this work. Flag it for the asset review, and do not copy it anywhere new.
- Screenshots that show Frontier Developments artwork are not committed. The repository is public. Only text-region crops and synthetic provenance maps are committed.
- Traces contain game text (BBS adverts, station names) generated by `fe2.s`. The committed histogram file redacts `str=` values, and full traces stay local.

## Effort to productionise

**M**, for a host-side UI layer covering text, bitmaps and the GL 3D and map primitives:
- The record-and-peel technique already works across all seven screens in about 350 lines.
- Production work:
  - a proper vector or SDF font with real glyph metrics;
  - the ≥ 0x7f special glyphs;
  - DPI-aware sizing;
  - replacement bitmaps keyed by `BlitBmp` source address;
  - an ingame toggle;
  - a CI provenance check.

**L**, if the goal is a fully redesigned UI (new layouts, not just sharper rendering). That needs the semantics of the span-drawn content and the screen layouts, which means `fe2.s` study and labelled hooks. In practice it is the "UI kit plus `fe2.s` patch" route.

### Phase 1 issues met (not fixed, as instructed)

- `Nu_QueueDrawStr` only `assert`s on `MAX_QUEUED_STRINGS`, so it would overflow in a release build.
- The game clock follows wall time, so runs are not reproducible (see Oracle implications).
- The linker warns `reducing alignment of section __DATA,__common from 0x8000 to 0x4000`. `nm -m` shows that none of the experiment objects (`exp.o`, `exptext.o`) has a common symbol aligned above 2^3, so the warning comes from the existing objects.
- The harness found and fixed one bug of its own: before initialisation, `fe2.s` screen-pointer variables hold low addresses such as 0x46, which the first provenance run tracked as a screen. The harness now ignores bases below 0x10000 or not 256-byte aligned.
