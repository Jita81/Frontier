# Picture shape: round planets (`src/gl.c`, `--tall-pixels`)

## The measurement

Planets used to be drawn taller than wide. A limb-fit of the disc seen from
orbit (an axis-aligned ellipse through the edge of the planet against the
sky) gave height / width:

| Renderer | 1600x1200 | 1600x1000 | 1280x960 |
|---|---|---|---|
| HD (before) | 1.206 | 1.206 | 1.206 |
| `--old-renderer` (before) | 1.212 | 1.212 | 1.212 |

Counted in ST pixels instead of screen pixels the same discs are round to
about 1%. The game draws round spheres; the presentation stretched them.

## The cause

The game's screen is 320x200. It was shown as a 4:3 picture, so each ST
pixel was 5 screen pixels wide and 6 tall (at 1600x1200): 1.2 times taller
than wide. Both renderers draw the 3D view into that picture through the
same fixed projection (`gluPerspective(36.5, 1.9)`, the game's own aspect for
its 320x168 view), so the stretch reached everything, the planets included.

## The choice

ST pixels are now square by default: the picture is the largest 16:10
rectangle the window holds, centred, with bars on the other axis when the
window is another shape. The default window is 16:10 too (in 64x40 steps,
the largest that fits the desktop), so it has no bars. In a 16:10 picture
the 3D viewport is 320:168 = 1.90, the projection's own aspect, and spheres
are round in both renderers.

The ST's own pixel on a PAL television was a little taller than wide, about
1.09 (an estimate: it depended on the set). Neither 1.0 nor 1.2 is "the
original"; 1.0 is the one that makes the game's geometry right.

## `--tall-pixels`

Restores the old presentation: a 4:3 picture, the default window 4:3 (in
64x48 steps), and planets 1.2 times taller than wide. At an exactly 4:3
window (1280x960, 1600x1200) the frames are the same as before, pixel for
pixel.

## The rule for HD layers

Positions follow the ST layout per axis: an ST point (x, y) lands at
`pic_x + x * pic_w / 320, pic_y + y * pic_h / 200`, so everything the core
draws (brackets, labels, buttons) stays where it was relative to the game.

Sizes do not. A radius, a stroke, a corner, a font, a padding is one scale
for both axes, keyed on the picture's height:

- `hdui.c`: `uu = height / 1200`.
- `hdstill.c`: `U = sy / 6`, a device pixel of a picture 1200 high.
- `hdface.c`: the thumbnail corner radius is `THUMB_R` of a picture 1200
  high, converted to ST pixels per axis so it stays round.
- `hdcontrols.c`: the 1600x1200 design is drawn at one scale, `fit()`, the
  largest that fits, and centred; the entry pill keeps to the picture's top
  right.

A new layer that scales x by width and y by height draws squashed circles
and stretched text the moment the picture is not 4:3; use one scale.

`px_scale()` (points and lines of the old renderer) is keyed on the picture
width, and gives exactly the old value under `--tall-pixels`.
