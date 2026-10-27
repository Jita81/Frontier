# HD stills

`src/hdstill.c` is the GL renderer's layer for the game's fixed pictures. These are
the bitmaps the 68000 code draws with `Call_BlitBmp`:

- the title wordmark;
- the control panel along the bottom (ST rows 168-199) and the icons in its keys;
- the missiles on the pylons;
- the keys beside the lines of the main screen's lists.

Each one keeps its size, place, job and state. The layer swaps its pixels for new art,
drawn in code with CoreGraphics and CoreText at the window's own pixel size. The art
follows the shared HD design language:

- dark gunmetal and glass;
- soft inner shadows and a 1 px top highlight;
- clean line glyphs in the original colour roles;
- cyan `#3fd0ff` for the lit key and amber `#ffb347` for caution (time paused);
- a soft glow only on what is active.

The game itself does not change. The layer never writes ST memory and changes nothing
the 68000 sees: no timing, no input, no click regions. The oracle replay ends on the
same line with the layer on as with it off.

## What it replaces

| Group | Bitmaps | Drawn as |
|---|---|---|
| Title | the 192x30 wordmark | an original wordmark set in Avenir Next Heavy, with a cyan-violet gradient, a glass bevel and a planet limb behind it. It has the same box and place as the old one. |
| Control panel | left, right and middle sections (empty bay or scanner); the map's own time keys; the gauge slats; the clock and date displays | one glass console scene in ST coordinates (see below) |
| Time keys | six variants: paused, and 1x to 10000x lit | a row of keys with line glyphs. The lit key is cyan with a glow, and paused is amber. |
| Function-key icons | 16x11 icons, singly or in rows of four, for the docked and flight sets | thin line glyphs on a key with its number set in DIN Alternate, bottom right |
| Bay keys | the orrery key and the row of keys along the bay floor | keys in the same style |
| Missiles | pointing left and pointing right | a slim missile with an amber exhaust glow |
| List keys | the key beside each line of a list, and the arrow that pages on | a small glass key and a soft arrow, on the list's own background |

36 bitmaps are named in the `arts[]` table at the top of `src/hdstill.c`. An entry
gives:

- the bitmap's hash and size;
- its kind (panel, logo, missile, list key, more-arrow);
- which time key is lit;
- the icons in its keys, left to right;
- a short name.

### The control panel is one scene

Every panel bitmap is drawn as the same console seen through that bitmap's own
rectangle, in the state the bitmap shows: which time key is lit, which icons sit in
which keys, and whether the bay is empty or holds the scanner. So a small bitmap blitted
over a big one joins it without a seam. A newly lit time key or a changed icon are
examples.

## How it decides when to replace

1. **Record.** `hdstill_blit` is called from the `Call_BlitBmp` host call and notes the
   bitmap: its place, its size, its pixels as palette indices, the set of colours it
   uses, its commonest colour, and an FNV-1a hash of its size and planes. The hash
   names the bitmap and finds its art in `arts[]`. Up to 256 are remembered. The one
   blitted longest ago makes room, and one unseen for 3000 frames is forgotten.
2. **Look.** Nothing recorded is trusted. Every frame, `hdstill_frame` checks each
   recorded bitmap against the screen being shown, newest first, under a cover mask: a
   pixel a newer bitmap on the screen covers is not counted for an older one. A bitmap
   is on the screen when at least 85% of its uncovered pixels are its own, and at least
   75% of those that are not its background. A pixel another HD layer has taken out and
   put back (HD text drawn over it) counts as its own.
3. **Defer.** If more than 25% of its visible pixels were already taken out by a layer
   that runs first (the restyled screens), the bitmap is left to that layer.
4. **Check the palette.** The art is drawn for the palette the game shows when no fade
   or flash is on. There are reference palettes for the panel (scanner reds, or skin
   tones while a face is in the bay), the title and the lists. If the live palette
   equals the reference, the art is drawn as it is. A list key is checked on its own
   colours only, not its background: each screen has its own background colour, and the
   key is drawn on whichever it is. If it is the reference uniformly
   darkened (a fade to black, within one ST colour step), the art is drawn darkened by
   the same factor. Anything else, such as a flash or a different screen's palette,
   keeps the game's own pixels.
5. **Replace.** The bitmap's pixels are set to 255 (transparent) in the copy of the
   screen uploaded to GL. Its art texture is drawn in their place, premultiplied, and
   faded by the factor from step 4. For list keys, the rectangle is first filled with
   the live colour of the bitmap's background, so the key sits on the list's own colour.
6. **Keep the game on top.** Pixels the game drew over a bitmap (scanner contacts,
   gauge bars, text) were never the bitmap's. They stay in the screen texture, which is
   drawn again over the art wherever it is not transparent.

Art textures are built on demand for the framebuffer's size (up to 96 are kept) and
rebuilt when the window size changes.

## Switches

| Switch | Effect |
|---|---|
| `--pixel-art` | Off: the game's own bitmaps, exactly the old picture. The software renderer is always like this. |
| `FE2_HD_DEBUG=1` | Logs every bitmap seen on the screen, once each: its hash, size, place and whether it was replaced, kept (palette) or deferred, plus the running coverage as replaced of seen. |
| `FE2_HD_DEBUG=2` | Also logs the colours each bitmap is shown in. |
| `FE2_HD_DUMP=dir` | Writes each bitmap that was not replaced to `dir` as a PPM, to work out what it is. These are pictures of the game. **Never put them in the repository.** |

The art is drawn with CoreGraphics, so builds other than macOS have an empty table and
show the game's own bitmaps.

## Adding a bitmap

1. Run with `FE2_HD_DEBUG=1` and go to the screen that shows the bitmap.
2. Take its hash, size and place from the `unknown` line.
3. Add an `arts[]` entry with the hash and size and the kind it belongs to.
4. If it shows a state (a lit key, a set of icons), describe that state in the entry.
   If it is a new kind, add a drawing function and a case in `draw_art`.
5. Describe what the bitmap is for in words and draw it new. Never trace or sample the
   game's pixels.

## Coverage

Across the final debug runs, the layer saw 66 distinct bitmaps. The runs covered the
start menu, docked, both time-key states, the galaxy and system maps, launch, flight and
missiles, the services, equipment and market lists, the cargo screen, key 3's icon
cycle, and a date rollover.

- It replaced 36 of them. That is every entry in `arts[]`, so every bitmap this layer
  owns.
- The other 30 are faces: the parts of the station portrait in all their variants, and
  the comms face in the bay. They belong to the HD faces layer.
- The date display appears only when the date rolls over. It was checked by running
  time at 10000x while docked.

## Provenance of the art

| Art | Source |
|---|---|
| Every picture this layer shows | Drawn in code (CoreGraphics paths, gradients, shadows) in `src/hdstill.c`. No art files are shipped. The `art/hd/` section of `.githooks/allowed-images` for this layer is empty. |
| Wordmark type | Avenir Next Heavy, a macOS system font, read at run time. Not shipped. |
| Key numbers | DIN Alternate Bold, a macOS system font, read at run time. Not shipped. |
| Unused texture trial | xAI Grok `grok-imagine-image-quality`, n=2, US$0.10, from a text prompt only. Not used and not shipped. The prompt asked for a seamless tileable texture of dark gunmetal brushed steel plate: fine straight horizontal brushing, very subtle micro scratches, flat studio lighting, top-down, neutral dark grey with a faint cool blue tint, no text, logos, rivets or seams. |

No art was traced, sampled, upscaled or otherwise derived from the game's bitmaps or
screenshots. The layout was read only to keep every shape where it was, so that clicks
land as before.

## Known gaps

- Pixels the game draws live stay at ST resolution, on top of the new art: the gauge
  bars, scanner contacts and panel text that HD text does not cover.
- Faces (the station portrait and the comms face) belong to the HD faces layer.
- List keys were seen only in their normal state. A pressed or other variant would show
  as `unknown` under `FE2_HD_DEBUG=1` until it is added.
- macOS only (CoreGraphics).
