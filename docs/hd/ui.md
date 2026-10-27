# Restyled 2D screens (hdui + hdtext)

The GL renderer can redraw the game's flat 2D pages as a modern interface:
a dark base, frosted glass panels, large cards with line icons, tables
with small plus and minus buttons, pills, check boxes and radios, and all
text set again in bundled fonts. It is the same game: every page keeps
its information, its choices and their results. Only the picture on the
host changes.

Two files do the work:

- `src/hdtext.c` finds every string the game wrote on the 320x200 screen
  and sets it again in a role of the design system (below), in the
  bundled fonts, at the window's own pixel scale.
- `src/hdui.c` recognises the page, lays it out again in the design
  system's components, and moves clicks on a moved control back into that
  control's original click region.

`--classic-ui` turns the layer off and gives exactly the old picture.
`--pixel-text` turns off both layers (the original bitmap font, the
original pages).

## Design system

### Tokens

| Token | Value | Use |
|---|---|---|
| base, bottom | `#0b1016` | the page base, a vertical gradient |
| base, top | `#141c26` | |
| glass | `#101823`, 70-85% alpha | panels: a faint vertical gradient, a 1 px inner highlight at about 12% white, a soft shadow, an 8 px radius at 1600x1200 |
| card | `#142030` | cards, rows, pills |
| control | `#1b2838` | small buttons |
| accent | `#3fd0ff` | selection, focus, active elements (the only glow) |
| amber | `#ffb347` | caution |
| red | `#ff4d4d` | danger |
| text | `#e8eef5` | primary text |
| text 2 | `#8fa3b8` | secondary text, labels, column headings |

The layout works on the ST's 320x200 grid; every component is drawn at the
window's pixel scale, so at 1600x1200 one ST pixel is 5x6 framebuffer
pixels and the 8 px grid, radii and hairlines are framebuffer pixels.
No hard black outlines, no dithering, no pixel edges.

### Type

| Role | Face | Use |
|---|---|---|
| title | Orbitron Regular | page titles in the header bar |
| section | Oswald Regular, capitals, tracked | headings, column headings |
| label, value, panel, map | Oswald Regular | labels, numbers (tabular figures), panels |
| body, desc | Open Sans Regular | list rows, card descriptions, conversation |
| strong | Open Sans Bold | emphasis |

Every run has a soft shadow instead of the game's hard one. If a bundled
font cannot be loaded the system falls back to DIN Alternate Bold.

Setting rules that keep the pages tidy:

- A left-set run is placed by the side bearing of a straight capital
  stem, not by the ink of its first letter. Letters such as A, T and W
  hang a little into the margin, as in print, and a column's stems line
  up.
- A table's figures are right-aligned on their column heading's right
  edge. Where they sit does not depend on the widest figure on the page,
  so a column stays put between the pages of a list. A heading that is
  wider than its figures still claims them. A heading set narrower than
  the game's sits centred where the game's sat, so the room it leaves
  goes to the columns on both sides.
- Card blurbs break one sentence at a time into lines of nearly equal
  length, so no line ends on a single word.
- Text over a picture (the video link's own captions) keeps the game's
  colour.

The game's font also holds icons. The tick box, the four diagonal arrows
and the small hex letters are drawn as vectors at full resolution. The
two unit glyphs (per hour and per second) are set in the run they follow,
as a letter and a superscript minus one. The few icons left keep a
smoothed copy of the original glyph.

### Components (hdui.c, CoreGraphics into cached textures)

- **glass panel** (`E_GLASS`): rounded on chosen corners, optional
  header band.
- **card** (`E_CARD`): a frosted tile; `F_BIG` puts a large thin line
  icon over its label (the services and shipyard grids); a cyan glow
  outline while the pointer is on it or it is pressed.
- **row** (`E_ROW`): a table row or a list answer; hover-only rows light
  under the pointer.
- **button** (`E_BTN`): the small plus, minus and eject squares of a
  table.
- **check box / radio** (`E_CHECK`, `E_RADIO`): the preferences and
  the start menu.
- **pill** (`E_PILL`): next page, go back, refuel and similar foot actions.
- **pager**: the next-page pill of a long list (the market, upgrades,
  the bulletin board). Two round buttons, a down chevron and an up
  chevron, sit in front of its label. Both are always
  there, so the pill keeps its shape as the list moves. A direction the
  game does not offer right now is dimmed and does nothing.
- **frame** (`E_FRAME`): a hairline frame with accent corners round a 3D
  view or a portrait. Round a portrait, the gap between the frame and the
  picture is the same on all four sides at every window size. The
  portrait's glass card sits flush with the left edge of the window, and
  a faint band with a hairline fills the strip above the portrait.
- **rule**, **quad**: hairlines and accent underlines.
- **line icons**: launch, ship, document, market, shield, chip, wrench,
  swap, back, more, plus, minus, eye, chevron, check, drop, speech, eject,
  station. All are drawn as strokes in code.

Each component is drawn once per size and state into a texture, which is
kept until the window size changes.

## When it replaces something

Every frame, after `hdtext_frame` has found the strings:

1. `hdui_frame` finds the game's tick boxes in the raster (exact pixel
   pattern) and the page's next-page arrow.
2. `classify` names the page from hdtext's run hashes (FNV-1a of a run's
   letters and digits; no text is stored), the boxes and the clear areas
   the game asked for:
   - the start menu;
   - a station page: services, market, shipyard, upgrades, repairs, ships
     for sale, police, bulletin board, video link;
   - a ship status page (F3);
   - the preferences.

   Anything else is `P_NONE` and is left alone, apart from hdtext's fonts.
3. The page's layout function places the components and gives every run
   a role, a colour, an alignment, a place and a maximum width. A station
   page whose right column holds anything the layout does not know, such
   as a picture or a 3D view, is laid out in place, without moving
   anything.
4. Pixels are taken out of the uploaded copy of the picture (`out`, the
   texture that goes to GL), never out of ST memory:
   - only pixels that are still exactly what the game drew (`out[o] ==
     raster[o]`);
   - only the boxes and arrows the layout redraws, and the page colour;
   - never pixels drawn by `Call_BlitBmp` (pictures, faces), never the
     control panel's rows 168-199, never the 3D view (the game's clear
     areas are left open).
5. The backdrop is drawn by `hdui_backdrop`, called just after the
   frame's `glClear`:
   - the opaque dark base is one draw call (`draw_base`, one
     `GL_TRIANGLES` batch);
   - the glass panels follow as separate translucent draws.

   It runs only when the page is a station, F3 or preferences page and
   the palette's page colour is still the one the layout was made for.
   The cards and buttons are drawn under the picture
   (`hdui_draw_under`), and frames over it (`hdui_draw_over`).

The layout is rebuilt every frame, and the click map goes live only after
the layout has been the same for a frame. A page that changes under the
pointer never sends a click to a stale place.

The page's layout key holds the page, its components and its hot
regions, but not the palette's page colour. During a palette fade that
colour changes every frame while the layout stands still, and a key that
held it would keep the click map from ever going live. The backdrop
checks the colour itself (point 5 above). When the colour has changed,
`hdui_draw_under` draws the base and the glass under the picture
instead.

### Frame order (gl.c)

1. `hdtext_frame`, then `hdui_frame`, then `hdback_frame`, on the new
   picture.
2. `glClear`, then `hdback_draw` (the docked picture, over the whole
   window, letterbox bars included), then `hdui_backdrop`.
3. `hdui_draw_under`, then the screen texture.
4. `hdtext_draw`, then `hdui_draw_over`.

The layer draws in the game's 4:3 picture (`gl_picture_rect`), not the
whole window, so on a wider window its panels sit between the letterbox
bars on the same pixels as the game's screen and the HD text.

### The backdrop under the glass

When a docked picture is up (`hdback_active`, see `docs/hd/backdrops.md`)
and the layer owns the page (`hdui_owns_page`), the page is drawn as
frosted glass over that picture instead of over the dark base:

- `hdui_backdrop` lays a 0.30 wash (`WASH_A`) instead of the opaque base,
  so the picture shows through.
- Panels and cards drop to 0.30 to 0.22 and 0.32 to 0.24 alpha
  (`SEE_GLASS_A0/A1`, `SEE_CARD_A0/A1`).
- Behind each panel `hdback_frost_begin` draws a blurred copy of the
  picture. It is made once per picture, never per frame, in `make_frost`:
  boxed down by 4, Gaussian blur with sigma 24 pixels of a 1600-pixel
  window, its luma pulled to 0.33 and held within 0.14 of it
  (`FROST_MID`, `FROST_SPAN`, `FROST_FLAT` 0.75), colour kept
  (`FROST_SAT` 1.00). A dark hangar and a sunlit planet therefore give
  glass of about the same brightness, so the text tokens keep their
  contrast.
- The frost's texture coordinates map window pixels through the same
  picture area the letterbox uses, so the blur stays under the panel at
  any window shape.
- Secondary text on that glass uses `C_TEXT2_SEE` (#c2d0e0) in place of
  `C_TEXT2`, which keeps it at 3.7:1 or more over the brightest part of
  any picture measured. Primary text stays at about 5:1 or more.

## Clicks

A control may be larger than the game's, or in another place. The host's
mouse-to-ST conversion (`Call_GetMouseInput` in `src/input.c`) calls
`hdui_mouse`, which works like this:

- On a press inside a live hot region with a remap, it latches that
  region's target point and reports that point while the button is held.
- Every target lies inside the original control's click region: the
  game's tick box, at its centre.
- A press elsewhere in the station pages' right column goes to an inert
  point (1,1), because the game's own boxes there are no longer where
  they are drawn.
- A list's pager: a press on the down button or on the label goes to
  the game's down arrow, a press on the up button to its up arrow, at
  each arrow's centre. A button the game does not show right now, and the
  label when there is no next page, go to the inert point.
- The left panel, the control panel (rows 168 and below) and pages the
  layer does not know pass through unchanged.

Nothing else changes: the 68k reads the same mouse words it always did,
the same click regions, at the same time. Jettison stays in place, at
its original size, because it throws cargo away.

### Proof

`tools/drive/drive.py` played the same script twice, with each click at
the new place under the layer and at the original box under
`--classic-ui --pixel-text`. The two runs were then compared shot by
shot. Every pair showed the same result:

- the start-position card picked the same system;
- a click on the header did nothing;
- the services grid opened the market;
- buy and sell changed the hold and cash by one unit;
- a click on a row's text did nothing;
- next page showed page two;
- the shipyard grid opened upgrades;
- go back returned to the shipyard;
- the bulletin board grid opened the board;
- an advert card opened the video link;
- the last answer hung up;
- the cargo page's refuel pill matched its box;
- preference rows clicked on their labels toggled and restored the same
  settings;
- a shape-detail radio label selected the same level.

The captures are private: they show the game, so they never enter the
repository.

## Switches

| Switch | Effect |
|---|---|
| `--classic-ui` | the game's own flat pages, exactly the old picture (hdtext keeps its old per-glyph typesetting); no docked backdrop and no HD gauges either; the F12 Controls page still opens (drawn by the host, over either look) |
| `FE2_HD_BACKDROP=0` | the restyled pages over the dark base, without the docked picture or the frost |
| `--pixel-text` | the original bitmap font and pages; no hdtext, no hdui |
| `FE2_HDUI_LOG=1` | each new page's boxes (B), runs (R, hashes and places only), clear areas (C) and hot regions (H, with targets) on stderr |
| `FE2_HDUI_DUMP=dir` | each new picture's raster, palettes and uploaded copy, raw, for offline study (keep it out of the repository) |
| `FE2_HDUI_PHOTO=file` | an image in place of the dark base, to check that the glass is translucent |
| `FE2_HDTEXT_DEBUG=1` | each glyph hdtext could not take over, and why, on stderr |
| `FE2_HD_FONT=name` | the PostScript font for the old per-glyph typesetting |

The other HD streams have their own switches: `--pixel-art` (stills) and
`--pixel-faces` (portraits).

## Pages

| Page | Status |
|---|---|
| start menu: start-position picker | done: cards with vector radio rings. The text that did not look HD there was the tick boxes and arrows, a smoothed upscale of the font's icon glyphs; they are vectors now. The logo belongs to the stills stream |
| station services | done: header bar, flush-left portrait card with an evenly framed portrait, 2x2 grid of large cards |
| stock market (both pages) | done: glass table, plus and minus buttons, pager pill with down and up buttons |
| shipyard menu | done: 2x2 grid |
| shipyard: upgrades (fit and remove) | done: table, next page and go back pills |
| shipyard: repairs and servicing | done: table |
| shipyard: new and reconditioned ships | done: table |
| police | done |
| bulletin board | done: advert cards |
| bulletin board advert, video link | done: answers as rows |
| ship equipment status (F3) | done: glass sheet, framed 3D view, cargo panel glass. Values are set in the body face. In the panel the figures sit right-aligned under their headings, a count trailing a label is split off as a value, and the cash becomes a panel heading with its figure in the accent colour, as in the station pages' header |
| commander profile and ratings (F3) | done |
| cargo inventory (F3) | done: jettison in place, refuel pill, column headings |
| preferences (escape) | done: headed groups of check boxes and radios, whole rows clickable |
| missions / contracts list | not verified: needs a save holding a mission; falls back to the F3 layout or to the original page |
| save and load file pages | not restyled: original layout with HD text |
| galaxy map, system view (F2) | not restyled: the map's fill, grid and markers are drawn by the GL scene, not the raster; HD text only, and the large sector-name font and the tiniest labels keep their pixels |
| ship view, external views | 3D only; nothing to restyle. The speed and target readouts over the view are HD text |
| any page not listed | left alone (P_NONE): original layout, HD text |

## Art and provenance

No image ships with this layer. Every panel, card, icon, button and frame
is drawn in code with CoreGraphics. `.githooks/allowed-images` keeps an
empty section for `art/ui/`.

| File | What | Licence |
|---|---|---|
| `art/ui/fonts/Orbitron-Regular.ttf` | Orbitron, Matt McInerney, 2009 (Google Fonts) | SIL OFL 1.1, `Orbitron-OFL.txt` |
| `art/ui/fonts/Oswald-Regular.ttf` | Oswald 2.002, Vernon Adams, 2011-12 (Google Fonts) | SIL OFL 1.1, `Oswald-OFL.txt` |
| `art/ui/fonts/OpenSans-Regular.ttf` | Open Sans 1.10, Ascender / Google, 2010-11 | Apache 2.0, `OpenSans-LICENSE.txt` |
| `art/ui/fonts/OpenSans-Bold.ttf` | Open Sans Bold 1.10, Ascender / Google, 2010-11 | Apache 2.0, `OpenSans-LICENSE.txt` |

Two concept images were generated from a text prompt only (xAI
grok-imagine-image, quality tier, two images, USD 0.10). They set the
direction and stay out of the repository. Nothing here was traced,
sampled, upscaled or derived from the game's bitmaps or screenshots.
