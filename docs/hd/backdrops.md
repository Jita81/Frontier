# Backdrop art: prompts, model and cost

Every picture in `art/backdrops/` was made by xAI Grok from the text prompt
below and nothing else: no image went in, and nothing was traced, sampled or
derived from the game's bitmaps or screens.  The prompts are built by
`tools/art/backdrop_prompts.py` (`prompt NAME` prints any of them again, word
for word); this page is written by its `record` command from the ledger
`tools/art/grok_image.py` keeps.  What the pictures are for, and how one is
chosen, is in [backdrop.md](backdrop.md).

- Calls: 58, spend $2.90 (cap $5.00).
- Shipped: 37 pictures, 4.13 MB.
- Processing: `backdrop_prompts.py process` crops each to 4:3 at Grok's own
  1152x864 and writes a progressive JPEG with cjpeg at quality 75.

## Shipped pictures

| File | Model | Calls | Cost | KB |
|---|---|---|---|---|
| `orbital-terran-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 126 |
| `orbital-terran-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 112 |
| `orbital-ice-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 131 |
| `orbital-ice-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 111 |
| `orbital-rocky-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 135 |
| `orbital-rocky-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 107 |
| `orbital-airless-yellow-a.jpg` | grok-imagine-image-quality | 5 | $0.25 | 111 |
| `orbital-airless-red-a.jpg` | grok-imagine-image-quality | 2 | $0.10 | 111 |
| `orbital-corrosive-yellow-a.jpg` | grok-imagine-image-quality | 2 | $0.10 | 114 |
| `orbital-corrosive-red-a.jpg` | grok-imagine-image-quality | 2 | $0.10 | 91 |
| `orbital-methane-yellow-a.jpg` | grok-imagine-image-quality | 2 | $0.10 | 118 |
| `orbital-methane-red-a.jpg` | grok-imagine-image-quality | 2 | $0.10 | 106 |
| `orbital-ocean-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 112 |
| `orbital-ocean-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 116 |
| `orbital-gasgiant-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 131 |
| `orbital-gasgiant-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 119 |
| `surface-terran-yellow-a.jpg` | grok-imagine-image-quality | 3 | $0.15 | 118 |
| `surface-terran-red-a.jpg` | grok-imagine-image-quality | 3 | $0.15 | 105 |
| `surface-ice-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 94 |
| `surface-ice-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 83 |
| `surface-rocky-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 92 |
| `surface-rocky-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 77 |
| `surface-airless-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 117 |
| `surface-airless-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 106 |
| `surface-corrosive-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 107 |
| `surface-corrosive-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 89 |
| `surface-methane-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 93 |
| `surface-methane-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 96 |
| `surface-ocean-yellow-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 97 |
| `surface-ocean-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 83 |
| `orbital-terran-orange-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 139 |
| `surface-terran-orange-a.jpg` | grok-imagine-image-quality | 3 | $0.15 | 127 |
| `orbital-terran-yellow-b.jpg` | grok-imagine-image-quality | 2 | $0.10 | 124 |
| `orbital-terran-red-b.jpg` | grok-imagine-image-quality | 1 | $0.05 | 101 |
| `surface-terran-yellow-b.jpg` | grok-imagine-image-quality | 3 | $0.15 | 127 |
| `surface-terran-red-b.jpg` | grok-imagine-image-quality | 3 | $0.15 | 105 |
| `surface-ice-moon-red-a.jpg` | grok-imagine-image-quality | 1 | $0.05 | 87 |

## The prompt of each shipped picture

### orbital-terran-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a living blue ocean world with green and brown continents and swirls of white cloud is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-terran-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a living blue ocean world with green and brown continents and swirls of white cloud is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-ice-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a frozen world wrapped in white ice sheets and pale blue glaciers, with a thin bright rim of atmosphere is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-ice-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a frozen world wrapped in white ice sheets and pale blue glaciers, with a thin bright rim of atmosphere is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-rocky-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a dry ochre and rust desert world with a thin dusty haze, canyons and dark craters is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-rocky-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a dry ochre and rust desert world with a thin dusty haze, canyons and dark craters is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-airless-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame. The bay has a closed ceiling and one single opening: a tall wide mouth filling most of the far wall from deck to ceiling, with the planet seen through it. The deck ends at its rim; nothing juts out into open space. No lens flare, light streaks or beams across the planet.

### orbital-airless-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame. Nothing crosses in front of the planet: no cables, wires, beams, rings or lines across the opening.

### orbital-corrosive-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a world hidden under a thick unbroken shroud of pale yellow sulphurous cloud, a smooth featureless ball like Venus with no bands, no stripes and no storms is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-corrosive-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a world hidden under a thick unbroken shroud of pale yellow sulphurous cloud, a smooth featureless ball like Venus with no bands, no stripes and no storms is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-methane-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a cold world wrapped in dense orange hydrocarbon smog, a smooth hazy orange ball like Saturn's moon Titan with no bands, no stripes, no storms and no rings is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-methane-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a cold world wrapped in dense orange hydrocarbon smog, a smooth hazy orange ball like Saturn's moon Titan with no bands, no stripes, no storms and no rings is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-ocean-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a world covered by one dark teal ocean with no land, under a thin greenish-yellow toxic haze is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-ocean-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a world covered by one dark teal ocean with no land, under a thin greenish-yellow toxic haze is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-gasgiant-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where an enormous banded gas giant with cream, tan and rust coloured cloud belts and swirling storms, filling much of the view is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-gasgiant-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where an enormous banded gas giant with cream, tan and rust coloured cloud belts and swirling storms, filling much of the view is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a temperate living world, at a working far-future starport built of weathered gunmetal steel: armoured hangars, tall cargo gantries, blast walls and heavy concrete pads, with green hills and scattered trees beyond the port and an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a temperate living world, at a working far-future starport built of weathered gunmetal steel: armoured hangars, tall cargo gantries, blast walls and heavy concrete pads, with green hills and scattered trees beyond the port and an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-ice-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a frozen world: flat plains of ice and wind-packed snow, frost on every surface, low pressurised buildings half buried in drifts, a pale cold sky, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-ice-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a frozen world: flat plains of ice and wind-packed snow, frost on every surface, low pressurised buildings half buried in drifts, a pale cold sky, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-rocky-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a dry desert world: ochre dust plains, wind-carved rock mesas on the horizon, a hazy ochre sky full of fine dust, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-rocky-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a dry desert world: ochre dust plains, wind-carved rock mesas on the horizon, a hazy ochre sky full of fine dust, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-airless-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of an airless rocky world: grey dusty ground strewn with rocks, sharp black shadows, pressurised domes and sealed hangars around the pad, a pure black sky full of stars, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-airless-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of an airless rocky world: grey dusty ground strewn with rocks, sharp black shadows, pressurised domes and sealed hangars around the pad, a pure black sky full of stars, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-corrosive-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a hostile world under a thick corrosive atmosphere: everything sealed and armoured, a dense yellow-brown murk overhead with no blue at all, dim diffuse light, acid-stained ground, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-corrosive-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a hostile world under a thick corrosive atmosphere: everything sealed and armoured, a dense yellow-brown murk overhead with no blue at all, dim diffuse light, acid-stained ground, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-methane-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a frigid world under thick orange methane smog: dim amber twilight, frost-crusted ground, dark pools of liquid hydrocarbons, sealed insulated buildings, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-methane-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a frigid world under thick orange methane smog: dim amber twilight, frost-crusted ground, dark pools of liquid hydrocarbons, sealed insulated buildings, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-ocean-yellow-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a water world with no land: the port stands on huge stilts above a dark rolling sea, under a hazy yellow-green poisonous sky, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-ocean-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a water world with no land: the port stands on huge stilts above a dark rolling sea, under a hazy yellow-green poisonous sky, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-terran-orange-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a living blue ocean world with green and brown continents and swirls of white cloud is seen off-centre in the upper half of the frame against the stars. The sunlight comes from an orange star: warm golden-amber light on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-orange-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a temperate living world, at a working far-future starport built of weathered gunmetal steel: armoured hangars, tall cargo gantries, blast walls and heavy concrete pads, with green hills and scattered trees beyond the port and an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from an orange star: warm golden-amber light. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-terran-yellow-b.jpg

grok-imagine-image-quality, $0.05

> Wide view across a long rectangular hangar deck inside an orbital space station, seen from a ship parked on a landing pad: tall ribbed metal walls, overhead gantry cranes, rows of landing pads with small guide lights, parked cargo shuttles at the sides. The huge open hangar doors at the far end frame open space, where a living blue ocean world with green and brown continents and swirls of white cloud is seen to one side, partly filling the view, against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the deck is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame. Nothing hangs or crosses in front of the planet: no hooks, cables or crane parts in the opening. The parked craft are sleek angular far-future shuttles, not present-day space shuttles.

### orbital-terran-red-b.jpg

grok-imagine-image-quality, $0.05

> Wide view across a long rectangular hangar deck inside an orbital space station, seen from a ship parked on a landing pad: tall ribbed metal walls, overhead gantry cranes, rows of landing pads with small guide lights, parked cargo shuttles at the sides. The huge open hangar doors at the far end frame open space, where a living blue ocean world with green and brown continents and swirls of white cloud is seen to one side, partly filling the view, against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the deck is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-yellow-b.jpg

grok-imagine-image-quality, $0.05

> Wide view out of a large starport hangar on the surface of a temperate living world, at a working far-future starport built of weathered gunmetal steel: armoured hangars, tall cargo gantries, blast walls and heavy concrete pads, with green hills and scattered trees beyond the port and an open sky with drifting white clouds, seen from a ship parked inside: steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the landing field, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-red-b.jpg

grok-imagine-image-quality, $0.05

> Wide view out of a large starport hangar on the surface of a temperate living world, at a working far-future starport built of weathered gunmetal steel: armoured hangars, tall cargo gantries, blast walls and heavy concrete pads, with green hills and scattered trees beyond the port and an open sky with drifting white clouds, seen from a ship parked inside: steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the landing field, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-ice-moon-red-a.jpg

grok-imagine-image-quality, $0.05

> Wide establishing view of a starport landing pad on the surface of a frozen world: flat plains of ice and wind-packed snow, frost on every surface, low pressurised buildings half buried in drifts, a pale cold sky, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. A huge banded gas giant hangs low in the sky above the horizon, its cloud belts clearly visible. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

## Attempts not shipped

### orbital-airless-yellow-a (dropped)

grok-imagine-image-quality, $0.05: a flat slab of deck jutted out of the bay mouth into open space.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-airless-yellow-a (dropped)

grok-imagine-image-quality, $0.05: the bay mouth came out as two openings joined like a figure eight.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame. The deck ends at the rim of the bay mouth; nothing juts out from the station into open space.

### orbital-airless-yellow-a (dropped)

grok-imagine-image-quality, $0.05: the bay had an open roof as well as a mouth, and the planet seen through the mouth did not line up with the planet above.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame. The bay has one single wide rounded-rectangle mouth; the deck ends at its rim and nothing juts out from the station into open space.

### orbital-airless-yellow-a (dropped)

grok-imagine-image-quality, $0.05: a lens-flare streak crossed the planet.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame. The bay has a closed ceiling and one single opening: a tall wide mouth filling most of the far wall from deck to ceiling, with the planet seen through it. The deck ends at its rim; nothing juts out into open space.

### orbital-airless-red-a (dropped)

grok-imagine-image-quality, $0.05: a thin straight line crossed the sky and the planet.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-corrosive-yellow-a (dropped)

grok-imagine-image-quality, $0.05: remade with the rewritten corrosive look.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a world hidden under a thick unbroken shroud of pale yellow sulphurous cloud is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-corrosive-red-a (dropped)

grok-imagine-image-quality, $0.05: the planet came out banded like a gas giant; the corrosive look was rewritten as a smooth featureless ball.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a world hidden under a thick unbroken shroud of pale yellow sulphurous cloud is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-methane-yellow-a (dropped)

grok-imagine-image-quality, $0.05: the planet came out banded like a gas giant; the methane look was rewritten as a smooth hazy ball.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a cold world wrapped in dense orange-brown hydrocarbon smog with no surface visible is seen off-centre in the upper half of the frame against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-methane-red-a (dropped)

grok-imagine-image-quality, $0.05: the planet came out banded like a gas giant; the methane look was rewritten as a smooth hazy ball.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out into space, where a cold world wrapped in dense orange-brown hydrocarbon smog with no surface visible is seen off-centre in the upper half of the frame against the stars. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows on the planet; the bay interior is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-yellow-a (dropped)

grok-imagine-image-quality, $0.05: the port looked like a present-day airport; the terran look was rewritten as a far-future starport.

> Wide establishing view of a starport landing pad on the surface of a temperate living world: green hills and scattered trees beyond the port, a few low white buildings, an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-yellow-a (dropped)

grok-imagine-image-quality, $0.05: the far-future port came out as sleek white towers, unlike the industrial steel of every other picture; the terran look was rewritten as a working gunmetal starport.

> Wide establishing view of a starport landing pad on the surface of a temperate living world, at a far-future starport of sleek curved white towers and domed terminals: green hills and scattered trees beyond the port, an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-red-a (dropped)

grok-imagine-image-quality, $0.05: the port looked like a present-day airport; the terran look was rewritten as a far-future starport.

> Wide establishing view of a starport landing pad on the surface of a temperate living world: green hills and scattered trees beyond the port, a few low white buildings, an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-red-a (dropped)

grok-imagine-image-quality, $0.05: the far-future port came out as sleek white towers, unlike the industrial steel of every other picture; the terran look was rewritten as a working gunmetal starport.

> Wide establishing view of a starport landing pad on the surface of a temperate living world, at a far-future starport of sleek curved white towers and domed terminals: green hills and scattered trees beyond the port, an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-orange-a (dropped)

grok-imagine-image-quality, $0.05: the port looked like a present-day airport; the terran look was rewritten as a far-future starport.

> Wide establishing view of a starport landing pad on the surface of a temperate living world: green hills and scattered trees beyond the port, a few low white buildings, an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from an orange star: warm golden-amber light. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-orange-a (dropped)

grok-imagine-image-quality, $0.05: the far-future port came out as sleek white towers, unlike the industrial steel of every other picture; the terran look was rewritten as a working gunmetal starport.

> Wide establishing view of a starport landing pad on the surface of a temperate living world, at a far-future starport of sleek curved white towers and domed terminals: green hills and scattered trees beyond the port, an open sky with drifting white clouds, seen from a ship parked on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at the sides, the port's buildings and towers around the edges of the frame, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from an orange star: warm golden-amber light. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### orbital-terran-yellow-b (dropped)

grok-imagine-image-quality, $0.05: a crane hook hung in front of the planet, and the parked craft looked like present-day space shuttles.

> Wide view across a long rectangular hangar deck inside an orbital space station, seen from a ship parked on a landing pad: tall ribbed metal walls, overhead gantry cranes, rows of landing pads with small guide lights, parked cargo shuttles at the sides. The huge open hangar doors at the far end frame open space, where a living blue ocean world with green and brown continents and swirls of white cloud is seen to one side, partly filling the view, against the stars. The sunlight comes from a yellow-white star like our Sun: natural daylight colours on the planet; the deck is lit by its own floodlights. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-yellow-b (dropped)

grok-imagine-image-quality, $0.05: remade with the first rewritten terran look.

> Wide view out of a large starport hangar on the surface of a temperate living world: green hills and scattered trees beyond the port, a few low white buildings, an open sky with drifting white clouds, seen from a ship parked inside: steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the landing field, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-yellow-b (dropped)

grok-imagine-image-quality, $0.05: the far-future port came out as sleek white towers, unlike the industrial steel of every other picture; the terran look was rewritten as a working gunmetal starport.

> Wide view out of a large starport hangar on the surface of a temperate living world, at a far-future starport of sleek curved white towers and domed terminals: green hills and scattered trees beyond the port, an open sky with drifting white clouds, seen from a ship parked inside: steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the landing field, the landscape and sky beyond. No other planets or moons in the sky. The sunlight comes from a yellow-white star like our Sun: natural daylight colours. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-red-b (dropped)

grok-imagine-image-quality, $0.05: remade with the first rewritten terran look.

> Wide view out of a large starport hangar on the surface of a temperate living world: green hills and scattered trees beyond the port, a few low white buildings, an open sky with drifting white clouds, seen from a ship parked inside: steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the landing field, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### surface-terran-red-b (dropped)

grok-imagine-image-quality, $0.05: the far-future port came out as sleek white towers, unlike the industrial steel of every other picture; the terran look was rewritten as a working gunmetal starport.

> Wide view out of a large starport hangar on the surface of a temperate living world, at a far-future starport of sleek curved white towers and domed terminals: green hills and scattered trees beyond the port, an open sky with drifting white clouds, seen from a ship parked inside: steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the landing field, the landscape and sky beyond. No other planets or moons in the sky. The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows. Composition for a background behind on-screen menus: the middle of the frame is open and calm (an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.

### station (trial before the prompt script)

grok-imagine-image-quality, $0.05: a first look at the idea, not shipped.

> Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from a docked ship: curved grey hull walls lined with lit service gantries, landing pads with amber guide lights, a cargo freighter parked on a pad, the open bay mouth at the far end showing a blue-green planet and stars. Photoreal, cinematic soft lighting, 4:3 landscape, rich detail, no text, no logos, no numbers, no people.
