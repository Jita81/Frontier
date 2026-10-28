# The docked concourse (`src/hdback.c`)

When the ship is docked at a station or landed at a surface port, and the
3D view looks out of the ship (front or rear view), the GL renderer shows a
bright arrivals concourse instead of the 3D bay. The picture fills the
whole view, cover-fitted, with hdui's station-home cards on top of it. The
cards keep their Frost style.

Two blank signs in the picture are lit at run time:

- the yellow sign shows the port's own name, read from the port's object
  in game memory: the same name the station services page carries in its
  title (at a surface port too, where the speed line names the world);
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

## When it shows

`hdback_concourse` runs once a frame in `gl.c`'s `draw_control_panel`
(GL only), after the HD frame hooks have edited the screen and before it
is uploaded, so it lies over the 3D view and under the console and cards.
It draws only when all of these hold:

1. hdback can place the location (`locate`: an orbital or surface port it
   knows; not a rough landing).
2. `hdui_concourse` returns 2. hdui decides it from this frame's own label
   runs, the way it finds the dock card:
   - the page is the plain 3D view or the station-home page (no game
     buttons, no page text over the view, no comms face on it);
   - the console's status is **Docked** or **Landed**.
3. The 3D view mode byte (A6+706) is 0 (front) or 1 (rear).
4. The space mode byte (A6+536) has bit 0 set, so a 3D view fills the frame.

When 2 fails because a page or a map has opened (hdui returns 0), the
concourse is gone in that same frame: it never shows behind another
screen. When the view is still a bare 3D view (a launch has started, or
the external view is up) it cross-fades out over 0.3 s.

The labels are matched by hash (FNV-1a 32 over lowercase letters and
digits). No game text is in the source.

While the concourse is mostly faded in, the game's gunsight is cleared
from the screen over the signs and the middle door.

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

### Where things are

A small static table in `hdback.c` (`cpics`) gives, for each picture, where
its ceiling opening ends (a fraction of the height; the tint fades out over
the next 0.08) and the two signs' corners (TL, TR, BR, BL as x,y fractions
of the picture, measured on the art). There is no manifest file for the
concourse: `conc_pick` chooses the picture from hdback's own location key
(orbital or surface, the world class, the star's light), and the tint of
the ceiling opening is the page backdrops' star-light grading, read from
`art/backdrops/manifest.txt` as before.

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

None: nothing in the picture moves, and there are no people.

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
