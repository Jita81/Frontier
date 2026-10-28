# The docked concourse (`src/hdback.c`)

When the ship is docked at a station or landed at a surface port, and the
3D view looks out of the ship (front or rear view), the GL renderer shows a
bright arrivals concourse instead of the 3D bay. The picture fills the
whole view, cover-fitted, with hdui's station-home cards on top of it. The
cards keep their Frost style.

Two blank signs in the picture are lit at run time:

- the yellow sign shows the port's name, the same text the station card shows;
- the blue sign shows the game date.

On a launch, or a switch to the external view, the concourse cross-fades
(0.3 s) back to the 3D view. The game's launch and docking sequences are
not changed, and all game state is only read.

## Switches

No new switch. Each of these gives exactly the old 3D bay:

| Switch | Effect |
|---|---|
| `--classic-ui` | No concourse (hdui is off, so the gate below never opens). |
| `--pixel-art` | No concourse. |
| `--old-renderer` | No concourse (`gl.c` calls it only for `R_GL`). |
| `FE2_HD_BACKDROP=0` | No concourse and no page backdrop; the other HD layers stay on. |

`FE2_HD_DEBUG=1` adds `view=$xx` (the 3D view mode) to hdback's location line.

## When it shows

`hdback_concourse` runs once a frame in `gl.c`, after the 3D view and the
hangar grade, and before the control panel (hdui's cards and the game's
raster). It draws only when all of these hold:

1. hdback can place the location (`locate`: an orbital or surface port it knows).
2. `hdui_concourse` says yes. hdui reads the console's own label runs, the
   same way it lays out the dock card:
   - the status is **Docked** or **Landed**;
   - the view label is **Front View** or **Rear View**;
   - no game buttons are up;
   - the page is the plain 3D view or the station-home page;
   - the speed line names the port (`Relative to:`).
3. The 3D view mode byte (A6+706) is 0 (front) or 1 (rear).
4. The space mode byte (A6+536) has bit 0 set, so a 3D view fills the frame.

If any of these fails the concourse fades out. It covers the 3D view and
the bars beside it at 16:9, down to the top of the console.

The labels are matched by hash (FNV-1a 32 over lowercase letters and
digits). No game text is in the source.

## The pictures

There are four pictures, one per ceiling. The far wall, doors and signs are
the same in all four; only what shows through the glass ceiling changes.

| Picture | Ceiling | Used for |
|---|---|---|
| `concourse-orbital.jpg` | space with a planet | every orbital station; the ceiling is tinted to the parent world |
| `concourse-terran.jpg` | blue day sky, towers | living and ice worlds, and orange-star worlds (tinted) |
| `concourse-dusk.jpg` | red-amber dusk sky | red-star worlds, and corrosive, methane and rocky worlds (tinted) |
| `concourse-airless.jpg` | black starry sky, moonscape | airless worlds |

Each picture is 1280x720 (16:9), a JPEG under 350 KB.

### Manifest

`art/hangar/manifest.txt` says where things are in each picture and which
picture goes where. It has two kinds of line.

```
pic FILE CEIL  yx0 yy0 yx1 yy1 yx2 yy2 yx3 yy3  bx0 by0 bx1 by1 bx2 by2 bx3 by3
use KIND CLASS LIGHT FILE [R G B]
```

**`pic` lines**

- `CEIL` is where the ceiling opening ends, as a fraction of the height.
  The tint fades out over the next 0.08.
- The 16 numbers are the yellow sign's corners, then the blue sign's. Each
  sign is given TL, TR, BR, BL as x,y fractions of the picture, measured
  on the art.

**`use` lines**

- They match hdback's location key: kind `orbital`/`surface`, the world
  class, and the star's light.
- The first match wins, and `*` matches anything.
- `R G B` multiplies the ceiling opening, so a few pictures cover many
  worlds.

## The signs

The art has blank lightboxes; the text is drawn at run time. For each sign:

1. `hdtext_sign` renders the text with CoreText (Orbitron) into an alpha
   texture, together with a blurred copy for the glow.
   - The cap height is 46 % of the sign, shrunk to fit 84 % of its width.
   - Both textures are cached until the text or size changes.
2. `sign_quad` maps the texture onto the four measured corners in
   perspective. The texture coordinates are projective (`glTexCoord4f`),
   with q worked out from where the diagonals cross.
3. The glow is added first (the light bleeding round the letters), then
   the letters:
   - on the yellow sign, dark letters;
   - on the blue sign, white-hot letters.

Off Apple, `hdtext_sign` returns 0 and the signs are left blank.

## Life

A slow, faint band of light sweeps along the wall's light strips every 9 s
(additive, peak 10 %). Nothing in the picture moves, and there are no people.

## Making the art

The pictures were made with xAI Grok (`grok-imagine-image-2.0`), using
`tools/art/grok_image.py --aspect 16:9`, from text prompts only. No image
was ever passed to the model. Paul's reference picture was described in
words and never traced.

The spend was 8 images at $0.06, $0.48 in all (cap $1.50).

### Prompts

The first round (4 images) put the signs over the left and right doors.
It was rejected because the station cards cover the left of the view and
would hide the yellow sign. The second round, below, is the one in use.
Every prompt is the base text followed by one ceiling sentence.

Base:

> Wide cinematic eye-level view across a bright, spotless starport arrivals
> concourse, looking straight at the far wall. The far wall is tall
> brushed-steel panelling with fine seams. Along it stand three tall dark
> double doors with chamfered octagonal tops, evenly spaced, each framed by
> twin vertical warm-yellow light panels; slim yellow light strips run up
> the wall between the doors and warm downlights wash the steel. High on the
> wall hang two large flat rectangular illuminated sign panels facing the
> viewer squarely: a wide warm-yellow one above the middle door and a
> blue-violet one above the right-hand door, while the wall above the
> left-hand door is plain panelling; the two large illuminated sign panels
> are completely blank, plain glowing colour with no text. The floor is
> mirror-polished and reflects the lights. A curved balcony with a glass
> railing sweeps along the left side, a few potted plants stand by the
> walls, soft magenta neon accents glow on the right. The middle of the
> floor is open, empty and uncluttered. No people. Clean, bright, inviting,
> high-end game art quality. Photoreal, cinematic, wide 16:9 landscape. No
> text, no lettering, no logos, no numbers anywhere.

Ceilings:

- **orbital:** Above, a large glass ceiling opening looks out into black
  space full of stars, with a big blue-and-white planet hanging in view
  through the glass.
- **terran:** Above, a large glass ceiling opening looks up into a clear
  bright blue daytime sky with soft white clouds, the tops of gleaming city
  towers rising at the edges of the glass and a few small flying craft
  crossing the sky.
- **dusk:** Above, a large glass ceiling opening looks up into a hazy dusk
  sky glowing deep red and amber from a dim red sun low on the horizon,
  thin bands of cloud and the first stars showing.
- **airless:** Above, a large glass ceiling opening looks up into a
  pitch-black airless sky crowded with sharp stars, with a grey cratered
  moonscape ridge just visible along the edge of the glass under harsh
  white sunlight.

The outputs were scaled to 1280x720 with `sips`, and the sign corners and
ceiling line were measured by hand. Every output is also kept in
`~/Frontier-art-cache/hangar/`.
