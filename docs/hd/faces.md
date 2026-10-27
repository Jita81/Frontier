# HD faces

The game builds its people out of small bitmap parts in a black box on the
left of the station screens. The GL renderer now draws a photographic
portrait in that box instead, at the same place and the same size. The
same person always gets the same portrait, on every screen, every frame
and every run.

Code: `src/hdface.c` and `src/hdface.h`. Art: `art/faces/` (the portraits
and `manifest.txt`). The renderer calls into it from `src/gl.c`.

## Where it shows

The layer works on every face the game puts together in its face box. The
box's black interior is ST pixels x 1..118 and y 22..135 (118 x 114), which
is 590 x 684 screen pixels in a 1600 x 1200 window. That covers:

- the station services screen (F4),
- the bulletin board and every advert's contact,
- mission givers,
- the shipyard and its upgrade, repair and new-ship screens,
- the police contact,
- any other screen that draws a face in that box.

A face drawn anywhere else is left alone. None has been seen.

## How it works

Nothing here writes ST memory or changes what the 68000 code sees. The
layer only edits the copy of the 320 x 200 screen that is uploaded to GL,
then draws over it. Game timing, input and click regions are unchanged,
and the oracle replay ends on the same hashes.

1. **Collecting the parts.** Each opaque bitmap blit that lands wholly
   inside the face box goes into a *group*. The layer paints the group, in
   order, onto a black copy of the box. That copy is the *composite*:
   exactly what the box shows once the face is finished. A blit that hits
   the place and size of a part already in the group starts a new group,
   because the face is being redrawn.
2. **Reading the person.** Every face ends with the same seven parts:
   shoulders, the two ears, chin, mouth, nose, and the eyes last. What
   comes around those seven tells the head:
   - hair parts drawn before them mean a civilian;
   - a part over the top of the head drawn after them means a cap;
   - a single part drawn before them in skin colours only means a shaved
     head.

   The other attributes:
   - **Sex:** the eye strip's x position, the ears' height and the nose's
     row each cast a vote.
   - **Glasses:** the eye strip is taller (21 rows or more).
   - **Sunglasses:** at least 75% of the eye pixels, below the brows, are
     black or hair-coloured.
   - **Long hair:** a hair part reaches past the sides of the head (x < 16
     or x + w > 100).
   - **Colour:** colours 1-4 of the palette hold one of the game's eight
     hair ramps (brown, auburn, blonde, grey, black, red, green, blue). The
     game paints hair, clothes, caps, brows and lipstick in that ramp, so
     on a cap or a shaved head it is the colour of the uniform or clothes.
3. **Choosing a portrait.** Each portrait in the manifest is scored
   against the person:

   | match | points |
   |---|---|
   | sex | 1000 |
   | cap | 100 |
   | shaved head | 100 |
   | eyewear (none, glasses or sunglasses) | 50 |
   | hair ramp: exact | 4 |
   | hair ramp: same family (warm browns, blonde, greys, green, blue) | 2 |
   | hair length (women only) | 1 |

   - Under a cap or on a shaved head, only the colour family counts (4
     points).
   - The game gives all its men short hair, so for men a portrait with hair
     to the collar matches as well as a short one.

   The person's id is an FNV-1a hash of every part's position, size and
   pixel data, plus the ramp. The id is mixed with a 64-bit finaliser and
   picks one portrait among all the top-scoring ones. The pick is
   therefore stable across frames, screens and runs, and different people
   with the same attributes spread over the matching portraits.
4. **Replacing, and only then.** In each frame, the box is taken out of
   the uploaded copy and the portrait drawn only when both of these hold:
   - the screen being shown has exactly the composite in the box, pixel
     for pixel;
   - colours 5-14 are the game's fixed skin ramp, or that ramp uniformly
     darkened (a fade). During a fade the portrait is darkened by the same
     factor.

   At any other time the game's own pixels show. That includes a half-drawn
   face, the box's black or video-link placeholder before the face
   appears, another screen, or a palette being cycled. The portrait
   therefore appears on the same frame the game's face would have.
5. **Memory.** A portrait is decoded only when it is first shown. At most
   eight stay uploaded, as 1024 x 1024 RGBA textures of 4 MB each; the one
   drawn longest ago is dropped. If the manifest is missing, the layer does
   nothing. If a file cannot be read, it is skipped and the next best
   portrait is used.

## Switches

| switch | effect |
|---|---|
| `--pixel-faces` | Keep the game's own faces. The layer returns before doing anything, so the picture is exactly the one without it. |
| `FE2_HDFACE_LOG=file` | Write one line per person to `file`: frame, id, the attributes read (sex, ramp, hair/cap/shaved, eyewear, eye darkness, hair length) and the portrait chosen. |
| `FE2_ART_DIR=dir` | Where `art/` is, shared with the other HD layers (default `art` in the current directory). |

## The portraits

`art/faces/manifest.txt` lists one portrait per line, with these columns:

- file,
- sex (`m`/`f`),
- the ramp it stands for,
- head (`0` hair, `1` cap, `2` shaved),
- eyes (`none`, `glasses` or `shades`),
- hair (`short` or `long`).

A new portrait needs only a new file and a new line.

Every portrait is new art of a fictional person, made by xAI Grok from a
text prompt alone. No game bitmap, screen or capture was used as input,
reference or mask, and nothing was traced, sampled, upscaled or derived
from the game. Every prompt reads:

> Photorealistic head-and-shoulders portrait of a fictional *{person}*.
> Looking straight into the camera at eye level, face centred, head and
> top of the shoulders filling the frame, calm neutral expression. Soft
> neutral studio lighting, plain near-black backdrop. Sharp focus, natural
> skin texture, film still. No text, no lettering, no logos, no watermark.

Each *{person}* is listed in the table below.

How the files were made:

- **Generation:** `tools/art/grok_image.py --model
  grok-imagine-image-quality --aspect 3:4`, which gives 864 x 1152 pixels at
  $0.05 a render.
- **Crop:** the centre 864 x 1010 rows (`djpeg -crop 864x1010+0+71`). That
  is the face box's shape on screen.
- **Encoding:** once, with `cjpeg -quality 75 -optimize -progressive`.
- **Review:** every portrait was checked by eye for stray text, logos,
  badges with lettering, and the wrong headgear or eyewear. Cap badges are
  plain, made-up shapes.

## Provenance

Every portrait was made with `grok-imagine-image-quality` at $0.05 a
render. "Renders" counts every render made for that file, rejected ones
included.

| file | person | model | renders | cost |
|---|---|---|---|---|
| `f_brown_long.jpg` | woman in her thirties of Mediterranean descent, long wavy brown hair falling past her shoulders, small gold hoop earrings, wearing a brown leather spacer's jacket over a dark top | grok-imagine-image-quality | 1 | $0.05 |
| `f_brown_short.jpg` | West African woman in her twenties, short cropped dark brown natural hair, wearing a brown canvas trader's work jacket with a high collar | grok-imagine-image-quality | 1 | $0.05 |
| `f_auburn_long.jpg` | Irish woman in her forties, long straight dark auburn hair, wearing a maroon high-collared pilot's flight jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_auburn_short.jpg` | South Asian woman in her thirties, dark auburn hair in a short bob, wearing a deep maroon trader's tunic | grok-imagine-image-quality | 1 | $0.05 |
| `f_blonde_long.jpg` | Scandinavian woman in her twenties, long dark-blonde hair, small drop earrings, wearing an olive-khaki spacer's jumpsuit | grok-imagine-image-quality | 1 | $0.05 |
| `f_blonde_short.jpg` | Eastern European woman in her thirties, short dark-blonde pixie cut, wearing an olive-green pilot's flight suit | grok-imagine-image-quality | 1 | $0.05 |
| `f_blonde_glasses.jpg` | Northern European woman in her forties, dark-blonde hair pinned up, round wire-rimmed glasses, wearing an olive-khaki technician's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_grey_long.jpg` | woman in her sixties of Latin American descent, long curly silver-grey hair, small earrings, wearing a charcoal-grey trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `f_grey_short.jpg` | Japanese woman in her late fifties, short neat silver-grey hair, wearing a grey high-collared station administrator's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_black_long.jpg` | Korean woman in her twenties, long straight black hair, dark lipstick, wearing a black spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_black_bun.jpg` | Black British woman in her thirties, black hair pulled back in a high bun, small earrings, dark lipstick, wearing a black high-collared pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_red_long.jpg` | Scottish woman in her thirties, long thick copper-red curly hair, freckles, wearing a rust-red spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_red_short.jpg` | Middle Eastern woman in her twenties, copper-red hair cut in a short asymmetric bob, wearing a rust-coloured trader's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_green.jpg` | Brazilian woman in her twenties, shoulder-length hair dyed deep forest green, wearing a dark green spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_blue.jpg` | Vietnamese woman in her twenties, shoulder-length hair dyed deep navy blue, wearing a plain navy-blue pilot's jacket with no patches, flags or insignia | grok-imagine-image-quality | 2 | $0.10 |
| `m_brown_short.jpg` | Turkish man in his thirties, short brown hair, clean-shaven, wearing a brown leather spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_brown_long.jpg` | Native American man in his forties, shoulder-length straight brown hair, clean-shaven, wearing a brown canvas trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `m_auburn_short.jpg` | Welsh man in his twenties, short dark auburn hair, clean-shaven, wearing a maroon flight jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_auburn_long.jpg` | Persian man in his thirties, collar-length dark auburn hair swept back, clean-shaven, wearing a maroon trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `m_auburn_shades.jpg` | Mexican man in his thirties, short dark auburn hair, dark aviator sunglasses, clean-shaven, wearing a maroon pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_blonde_short.jpg` | Dutch man in his twenties, short dark-blonde hair, clean-shaven, wearing an olive-khaki spacer's jumpsuit | grok-imagine-image-quality | 1 | $0.05 |
| `m_blonde_long.jpg` | Australian man in his thirties, collar-length swept-back dark-blonde hair, clean-shaven, wearing an olive-green flight jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_grey_short.jpg` | Nigerian man in his sixties, short grey hair, clean-shaven, wearing a charcoal-grey trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `m_grey_long.jpg` | Russian man in his late fifties, swept-back collar-length silver-grey hair, clean-shaven, wearing a grey high-collared captain's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_black_short.jpg` | Chinese man in his twenties, short spiky black hair, clean-shaven, wearing a black spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_black_long.jpg` | Indian man in his thirties, black hair swept back to the collar, clean-shaven, wearing a black high-collared pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_red_short.jpg` | Irish man in his twenties, short copper-red hair, clean-shaven, wearing a rust-red work jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_red_shades.jpg` | Polish man in his thirties, short copper-red hair, dark wraparound sunglasses, clean-shaven, wearing a rust-red spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_green.jpg` | Samoan man in his twenties, short hair dyed deep green, clean-shaven, wearing a dark green spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_blue.jpg` | Filipino man in his twenties, short hair dyed deep navy blue, clean-shaven, wearing a navy-blue pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_1.jpg` | Black woman in her forties, a starport police officer, wearing a dark charcoal-grey peaked uniform cap with a small plain silver badge and a dark grey high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_2.jpg` | white woman in her fifties, a station police official, short grey hair under a dark grey peaked uniform cap with a small plain silver badge, grey high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_3.jpg` | South Asian woman in her thirties, a customs official, wearing a black peaked uniform cap with a small plain silver badge and a black high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_grey.jpg` | middle-aged East Asian man, a starport police officer, wearing a dark charcoal-grey peaked uniform cap with a small plain silver badge and a dark grey high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_2.jpg` | Arab man in his forties, a starport police officer, wearing a black peaked uniform cap with a small plain silver badge and a black high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_3.jpg` | Black man in his fifties, a station security chief, wearing a dark grey peaked uniform cap with a small plain silver badge and a grey high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_shades.jpg` | Latino man in his thirties, a shipyard official, wearing a maroon-brown peaked uniform cap with a small plain silver badge, dark aviator sunglasses and a maroon high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_maroon_1.jpg` | Chinese woman in her thirties, a starport traffic officer, wearing a maroon peaked uniform cap with a small plain silver badge and a maroon high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_maroon_2.jpg` | Nigerian woman in her fifties, a station harbour master, wearing a dark brown peaked uniform cap with a small plain brass badge and a dark brown high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_maroon_1.jpg` | Korean man in his forties, a starport customs officer, wearing a maroon peaked uniform cap with a small plain silver badge and a maroon high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_maroon_2.jpg` | Scottish man in his sixties with grey sideburns, a station harbour master, wearing a dark rust-brown peaked uniform cap with a small plain brass badge and a rust-brown high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_khaki.jpg` | Brazilian woman in her twenties, a shipyard inspector, wearing an olive-khaki peaked uniform cap with a small plain brass badge and an olive-khaki high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_khaki_1.jpg` | Egyptian man in his thirties, a shipyard foreman, wearing an olive-khaki peaked uniform cap with a small plain brass badge and an olive-khaki high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_khaki_2.jpg` | Kenyan man in his fifties, a starport security officer, wearing a sand-khaki peaked uniform cap with a small plain brass badge and a sand-khaki high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_green.jpg` | Indonesian woman in her forties, a station patrol officer, wearing a dark green peaked uniform cap with a small plain silver badge and a dark green high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_green.jpg` | Ukrainian man in his twenties, a station patrol officer, wearing a dark green peaked uniform cap with a small plain silver badge and a dark green high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_navy_1.jpg` | Ethiopian woman in her thirties, a starport police officer, wearing a navy-blue peaked uniform cap with a small plain silver badge and a navy-blue high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_navy_glasses.jpg` | English woman in her fifties, a station police inspector, wearing thin rectangular glasses, a navy-blue peaked uniform cap with a small plain silver badge and a navy-blue high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_navy_1.jpg` | Japanese man in his forties, a starport police officer, wearing a navy-blue peaked uniform cap with a small plain silver badge and a navy-blue high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_navy_2.jpg` | Black American man in his thirties, a station police sergeant, wearing a navy-blue peaked uniform cap with a small plain silver badge and a navy-blue high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_glasses.jpg` | Indian man in his fifties, a station customs inspector, wearing thin metal-framed glasses, a charcoal-grey peaked uniform cap with a small plain silver badge and a charcoal-grey high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_shades.jpg` | Latina woman in her thirties, a starport security officer, wearing dark aviator sunglasses, a black peaked uniform cap with a small plain silver badge and a black high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_bald_1.jpg` | Ghanaian man in his forties with a smooth shaved head, clean-shaven, wearing a plain maroon high-collared spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_bald_2.jpg` | German man in his sixties, completely bald, clean-shaven, wearing a plain charcoal-grey trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `m_bald_3.jpg` | Thai man in his thirties with a shaved head, clean-shaven, wearing a plain olive-green flight jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_bald_shades.jpg` | Moroccan man in his thirties with a shaved head, dark wraparound sunglasses, clean-shaven, wearing a plain black spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_bald_1.jpg` | Somali woman in her twenties with a closely shaved head, small earrings, wearing a plain navy-blue pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_bald_2.jpg` | Icelandic woman in her thirties with a shaved head, wearing a plain dark green technician's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_brown_short_2.jpg` | Brazilian man in his forties, short dark brown hair greying slightly at the temples, clean-shaven, wearing a plain brown canvas work jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_auburn_short_2.jpg` | Lebanese man in his fifties, short dark auburn hair, clean-shaven, wearing a plain maroon trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `m_blonde_short_2.jpg` | Swedish man in his forties, short dark-blonde hair, clean-shaven, wearing a plain olive-khaki technician's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_grey_short_2.jpg` | Canadian man in his seventies, short white-grey hair, clean-shaven, wearing a plain grey high-collared trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `m_black_short_2.jpg` | Vietnamese man in his thirties, short neat black hair, clean-shaven, wearing a plain black high-collared pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_red_short_2.jpg` | Dutch man in his forties, short ginger-red hair, clean-shaven, wearing a plain rust-red work jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_green_2.jpg` | Nigerian man in his twenties, short hair dyed deep green, clean-shaven, wearing a plain dark green flight jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_blue_2.jpg` | Norwegian man in his thirties, short hair dyed deep navy blue, clean-shaven, wearing a plain navy-blue spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_green_short.jpg` | Japanese woman in her twenties, short pixie cut dyed deep green, wearing a plain dark green technician's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_blue_short.jpg` | Ghanaian woman in her thirties, short natural hair dyed deep navy blue, wearing a plain navy-blue spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_brown_glasses.jpg` | Peruvian woman in her forties, brown hair tied back, round tortoiseshell glasses, wearing a plain brown leather spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_black_shades.jpg` | Thai woman in her thirties, short black hair, dark wraparound sunglasses, wearing a plain black pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_brown_glasses.jpg` | Israeli man in his fifties, short brown hair, thin metal-framed glasses, clean-shaven, wearing a plain brown trader's coat | grok-imagine-image-quality | 1 | $0.05 |
| `f_brown_long_2.jpg` | Filipina woman in her forties, long straight dark brown hair falling past her shoulders, wearing a tan-brown trader's coat with a high collar | grok-imagine-image-quality | 1 | $0.05 |
| `f_brown_short_2.jpg` | Persian woman in her fifties, short dark brown hair with a few grey strands, wearing a brown corduroy station worker's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_auburn_long_2.jpg` | young woman in her twenties with pale freckled skin, long dark auburn hair in loose waves past her shoulders, wearing a plain unmarked maroon spacer's jacket with no patches, flags or emblems | grok-imagine-image-quality | 2 | $0.10 |
| `f_auburn_short_2.jpg` | Ghanaian woman in her forties, short cropped natural hair tinted dark auburn, wearing a deep maroon high-collared tunic | grok-imagine-image-quality | 1 | $0.05 |
| `f_blonde_long_2.jpg` | Australian woman in her forties, long straight dark-blonde hair past her shoulders, wearing an olive-khaki trader's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_blonde_short_2.jpg` | Dutch woman in her sixties, short dark-blonde hair going ash, wearing an olive-khaki pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_grey_long_2.jpg` | Indian woman in her sixties, long silver-grey hair in a loose braid over one shoulder, wearing a charcoal-grey shawl-collared coat | grok-imagine-image-quality | 1 | $0.05 |
| `f_grey_short_2.jpg` | Black American woman in her seventies, short natural silver-grey hair, wearing a grey high-collared jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_black_long_2.jpg` | Latina woman in her thirties, long straight black hair past her shoulders, wearing a plain unmarked black leather jacket with no patches, flags or emblems | grok-imagine-image-quality | 2 | $0.10 |
| `f_black_short_2.jpg` | Chinese woman in her fifties, short black bob, wearing a black high-collared administrator's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_red_long_2.jpg` | Welsh woman in her fifties, long copper-red hair streaked with grey falling past her shoulders, wearing a rust-red wool coat | grok-imagine-image-quality | 1 | $0.05 |
| `f_red_short_2.jpg` | Moroccan woman in her thirties, short copper-red hennaed hair, wearing a rust-coloured spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_green_long_2.jpg` | Nigerian woman in her thirties, long braids dyed deep forest green falling past her shoulders, wearing a dark green pilot's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_green_short_2.jpg` | Finnish woman in her forties, short spiky hair dyed deep forest green, wearing a dark green work jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_blue_long_2.jpg` | Turkish woman in her twenties, long straight hair dyed deep navy blue falling past her shoulders, wearing a navy-blue spacer's jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_blue_short_2.jpg` | Thai woman in her thirties, short pixie cut dyed deep navy blue, wearing a navy-blue high-collared tunic | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_khaki_2.jpg` | Peruvian woman in her forties, a shipyard engineer, wearing an olive-khaki peaked uniform cap with a small plain brass badge and an olive-khaki high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_green_2.jpg` | Scottish woman in her thirties, a station patrol officer, wearing a dark green peaked uniform cap with a small plain silver badge and a dark green high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `f_cap_navy_2.jpg` | Korean woman in her fifties, a starport police officer, wearing a navy-blue peaked uniform cap with a small plain silver badge and a navy-blue high-collared uniform jacket | grok-imagine-image-quality | 1 | $0.05 |
| `m_cap_green_2.jpg` | Pakistani man in his fifties, a station patrol officer, wearing a dark green peaked uniform cap with a small plain silver badge and a dark green high-collared uniform jacket, clean-shaven | grok-imagine-image-quality | 1 | $0.05 |

**Total:** 91 portraits from 94 renders, $4.70.

Three renders were rejected because the clothes carried a flag-like patch
or emblem: the first renders of `f_blue`, `f_auburn_long_2` and
`f_black_long_2`. Each was made again with "plain" or "no patches, flags
or emblems" added to the description, as the table shows. Rejected renders
were never processed and are not in the repository.

## Known gaps

- **Skin tone is not matched.** The game draws every skin from one fixed
  ramp, so the portraits cover a range of skin tones and the pick among
  them depends only on the person's id.
- **Other details are not matched.** Age, face shape, earrings, lipstick,
  beards and expression are not read. The portrait matches only the
  attributes listed above.
- **Small groups share portraits.** Some combinations of attributes have
  only one or two portraits, such as a woman with sunglasses and green
  hair. Two different people in such a group can get the same portrait.
- **The attributes are read by rules.** Sunglasses and long hair are judged
  from the parts' pixels and positions. An unusual face can be read wrong;
  it then gets a portrait of the nearest kind.
- **Resolution.** The files are 864 x 1010 pixels, about 1.5 times the
  box's size in a 1600 x 1200 window, not 2 times. The files total 5.6 MB.
- **Only the face box is covered.** A face drawn anywhere else on the
  screen keeps the game's pixels. None has been seen.
