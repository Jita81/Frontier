#!/usr/bin/env python3
"""Text prompts and processing for the docked-location backdrops (src/hdback.c).

    python3 -I tools/art/backdrop_prompts.py plan [--only NAME,...]
    python3 -I tools/art/backdrop_prompts.py prompt NAME
    python3 -I tools/art/backdrop_prompts.py process RAW_DIR [--quality 75]
    python3 -I tools/art/backdrop_prompts.py record LEDGER

`plan` prints one line per picture: NAME, a tab, its prompt.  Feed each to
grok_image.py (4:3, grok-imagine-image-quality), with the output named NAME:

    withkey XAI_API_KEY -- python3 -I tools/art/grok_image.py \
        --model grok-imagine-image-quality --aspect 4:3 \
        --out RAW_DIR/NAME "PROMPT"

`process` turns RAW_DIR/NAME.jpg (or .png) into art/backdrops/NAME.jpg -
centre crop to 4:3, 1152x864, progressive JPEG via cjpeg - and
writes art/backdrops/manifest.txt for the pictures it found.

`record` turns grok_image.py's ledger (one JSON line per call) into
docs/hd/backdrops.md: every prompt sent, its model and cost, the shipped
picture each became and why any earlier attempt was dropped.

A NAME is KIND-CLASS-LIGHT-VARIANT, which is also the key hdback.c builds
from the game's state:

  KIND     orbital (a station's docking bay, the body seen through the bay
           mouth) or surface (a pad or hangar on the body itself)
  CLASS    the body's look; CLASS-moon is the same body seen as a moon of
           a gas giant, with the giant in its sky (tried first for moons)
  LIGHT    the colour of the system's primary star
  VARIANT  a, b, ...: a starport's identity picks one, always the same

Every picture is made from words only.  Nothing here may describe the
original game's screens or bitmaps, or quote its text: the scenes are
written from what such places would look like, not from the game.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "art", "backdrops")
W, H = 1152, 864  # Grok's own 4:3 size: upscaling would add bytes, not detail

# --- the body seen through a bay mouth, from orbit -------------------------
ORBIT_LOOK = {
    "terran": "a living blue ocean world with green and brown continents and swirls of white cloud",
    "ice": "a frozen world wrapped in white ice sheets and pale blue glaciers, with a thin bright rim of atmosphere",
    "rocky": "a dry ochre and rust desert world with a thin dusty haze, canyons and dark craters",
    "airless": "a grey cratered airless world with no atmosphere at all and a razor-sharp line between day and night",
    "corrosive": "a world hidden under a thick unbroken shroud of pale yellow sulphurous cloud, a smooth "
                 "featureless ball like Venus with no bands, no stripes and no storms",
    "methane": "a cold world wrapped in dense orange hydrocarbon smog, a smooth hazy orange ball like "
               "Saturn's moon Titan with no bands, no stripes, no storms and no rings",
    "ocean": "a world covered by one dark teal ocean with no land, under a thin greenish-yellow toxic haze",
    "gasgiant": "an enormous banded gas giant with cream, tan and rust coloured cloud belts and swirling storms, filling much of the view",
}

# --- the place itself, on the surface --------------------------------------
GROUND_LOOK = {
    "terran": "a temperate living world, at a working far-future starport built of weathered gunmetal steel: "
              "armoured hangars, tall cargo gantries, blast walls and heavy concrete pads, with green hills and "
              "scattered trees beyond the port and an open sky with drifting white clouds",
    "ice": "a frozen world: flat plains of ice and wind-packed snow, frost on every surface, low pressurised buildings "
           "half buried in drifts, a pale cold sky",
    "rocky": "a dry desert world: ochre dust plains, wind-carved rock mesas on the horizon, "
             "a hazy ochre sky full of fine dust",
    "airless": "an airless rocky world: grey dusty ground strewn with rocks, sharp black shadows, pressurised domes and "
               "sealed hangars around the pad, a pure black sky full of stars",
    "corrosive": "a hostile world under a thick corrosive atmosphere: everything sealed and armoured, a dense "
                 "yellow-brown murk overhead with no blue at all, dim diffuse light, acid-stained ground",
    "methane": "a frigid world under thick orange methane smog: dim amber twilight, frost-crusted ground, "
               "dark pools of liquid hydrocarbons, sealed insulated buildings",
    "ocean": "a water world with no land: the port stands on huge stilts above a dark rolling sea, "
             "under a hazy yellow-green poisonous sky",
}

# the giant a moon's sky shows
MOON_SKY = ("A huge banded gas giant hangs low in the sky above the horizon, "
            "its cloud belts clearly visible.")

# --- the star -------------------------------------------------------------
LIGHT = {
    "red": "The only sunlight comes from a dim red dwarf star: deep ruddy red-orange light, dusky tones, long soft shadows",
    "orange": "The sunlight comes from an orange star: warm golden-amber light",
    "yellow": "The sunlight comes from a yellow-white star like our Sun: natural daylight colours",
    "white": "The sunlight comes from a brilliant white star: crisp, cool, bright white light with hard shadows",
    "blue": "The sunlight comes from a fierce blue-white star: cold, harsh bluish light",
}

# --- the scene ------------------------------------------------------------
ORBITAL_SCENE = {
    "a": ("Wide establishing view inside the docking bay of a huge rotating orbital space station, seen from "
          "a ship parked on a landing pad: curved grey hull walls lined with lit service gantries, cranes and "
          "cargo pods, landing pads with small amber guide lights. At the far end the open bay mouth looks out "
          "into space, where {look} is seen off-centre in the upper half of the frame against the stars. "
          "{light} on the planet; the bay interior is lit by its own floodlights."),
    "b": ("Wide view across a long rectangular hangar deck inside an orbital space station, seen from a ship "
          "parked on a landing pad: tall ribbed metal walls, overhead gantry cranes, rows of landing pads with "
          "small guide lights, parked cargo shuttles at the sides. The huge open hangar doors at the far end "
          "frame open space, where {look} is seen to one side, partly filling the view, against the stars. "
          "{light} on the planet; the deck is lit by its own floodlights."),
}

SURFACE_SCENE = {
    "a": ("Wide establishing view of a starport landing pad on the surface of {look}, seen from a ship parked "
          "on the pad: the pad's edge markings and small guide lights, service vehicles and cargo containers at "
          "the sides, the port's buildings and towers around the edges of the frame, the landscape and sky "
          "beyond. {sky}{light}."),
    "b": ("Wide view out of a large starport hangar on the surface of {look}, seen from a ship parked inside: "
          "steel hangar walls and gantries at the sides of the frame, the huge hangar doors open onto the "
          "landing field, the landscape and sky beyond. {sky}{light}."),
}

COMMON = ("Composition for a background behind on-screen menus: the middle of the frame is open and calm "
          "(an empty deck, open ground or open sky), with the detail and interest towards the edges and corners; "
          "no bright light source in the centre. Photoreal, cinematic, wide-angle 4:3 landscape, rich detail. "
          "No text, no lettering, no logos, no numbers, no signs, no people, no figures, no cockpit frame.")

ORBITAL_CLASSES = ["terran", "ice", "rocky", "airless", "corrosive", "methane", "ocean", "gasgiant"]
SURFACE_CLASSES = ["terran", "ice", "rocky", "airless", "corrosive", "methane", "ocean"]

# what to make: (kind, class, light, variant).  Yellow and red for every
# class (the two ends of the lights the code tints between); orange for the
# living worlds of the commonest stars; a second variant for the commonest
# keys; a moon version of the frozen moon one of the starts is on.
def plan():
    out = []
    for cls in ORBITAL_CLASSES:
        for light in ("yellow", "red"):
            out.append(("orbital", cls, light, "a"))
    for cls in SURFACE_CLASSES:
        for light in ("yellow", "red"):
            out.append(("surface", cls, light, "a"))
    out += [("orbital", "terran", "orange", "a"), ("surface", "terran", "orange", "a")]
    for kind in ("orbital", "surface"):
        for light in ("yellow", "red"):
            out.append((kind, "terran", light, "b"))
    out.append(("surface", "ice-moon", "red", "a"))
    return out


# a light without its own picture borrows another's, graded by a GL tint
BORROW = {"orange": "yellow", "white": "yellow", "blue": "yellow"}
TINT = {"orange": (1.00, 0.85, 0.68), "white": (0.93, 0.97, 1.00), "blue": (0.80, 0.89, 1.00)}


def name_of(kind, cls, light, variant):
    return f"{kind}-{cls}-{light}-{variant}"


def parse(name):
    kind, rest = name.split("-", 1)
    rest, variant = rest.rsplit("-", 1)
    cls, light = rest.rsplit("-", 1)
    return kind, cls, light, variant


# a clause one picture's prompt gets after a try came out with a fault the
# scene words alone did not prevent (see REMADE)
EXTRA = {
    "orbital-airless-red-a": "Nothing crosses in front of the planet: no cables, wires, beams, "
                             "rings or lines across the opening.",
    "orbital-airless-yellow-a": "The bay has a closed ceiling and one single opening: a tall wide mouth "
                                "filling most of the far wall from deck to ceiling, with the planet seen "
                                "through it. The deck ends at its rim; nothing juts out into open space. No lens "
                                "flare, light streaks or beams across the planet.",
    "orbital-terran-yellow-b": "Nothing hangs or crosses in front of the planet: no hooks, cables or "
                               "crane parts in the opening. The parked craft are sleek angular far-future "
                               "shuttles, not present-day space shuttles.",
}


def prompt(kind, cls, light, variant):
    base = cls[:-5] if cls.endswith("-moon") else cls
    if kind == "orbital":
        text = ORBITAL_SCENE[variant].format(look=ORBIT_LOOK[base], light=LIGHT[light])
    else:
        sky = MOON_SKY + " " if cls.endswith("-moon") else "No other planets or moons in the sky. "
        text = SURFACE_SCENE[variant].format(look=GROUND_LOOK[base], sky=sky, light=LIGHT[light])
    extra = EXTRA.get(name_of(kind, cls, light, variant))
    return text + " " + COMMON + (" " + extra if extra else "")


def raw_file(raw, name):
    for ext in (".jpg", ".png"):
        p = os.path.join(raw, name + ext)
        if os.path.exists(p):
            return p
    return None


def process(raw, quality):
    cjpeg = shutil.which("cjpeg") or "/opt/homebrew/bin/cjpeg"
    os.makedirs(OUT, exist_ok=True)
    lines = ["# hdback.c: KIND CLASS LIGHT FILE, one per picture (see docs/hd/backdrop.md)",
             "# a light with no picture of its own borrows another and is tinted (r g b)"]
    for light, other in BORROW.items():
        lines.append(f"borrow {light} {other}")
    for light, (r, g, b) in TINT.items():
        lines.append(f"tint {light} {r:.2f} {g:.2f} {b:.2f}")
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for kind, cls, light, variant in plan():
            name = name_of(kind, cls, light, variant)
            src = raw_file(raw, name)
            if not src:
                print("missing", name, file=sys.stderr)
                continue
            ppm = os.path.join(tmp, name + ".ppm")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vf",
                            f"crop='min(iw,ih*4/3)':'min(ih,iw*3/4)',scale={W}:{H}:flags=lanczos",
                            "-f", "image2", "-c:v", "ppm", ppm], check=True)
            dst = os.path.join(OUT, name + ".jpg")
            with open(dst, "wb") as f:
                subprocess.run([cjpeg, "-quality", str(quality), "-optimize", "-progressive", ppm],
                               stdout=f, check=True)
            size = os.path.getsize(dst)
            total += size
            print(f"{name}.jpg {size // 1024} KB")
            lines.append(f"{kind} {cls} {light} {name}.jpg")
    with open(os.path.join(OUT, "manifest.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"total {total / 1e6:.2f} MB")


# why a picture was made more than once: one reason per dropped attempt, in
# the order they were made (the last attempt is the one shipped)
TERRAN_AIRPORT = ("the port looked like a present-day airport; the terran look was rewritten as a "
                  "far-future starport")
TERRAN_TOWERS = ("the far-future port came out as sleek white towers, unlike the industrial steel of "
                 "every other picture; the terran look was rewritten as a working gunmetal starport")
REMADE = {
    "orbital-corrosive-red-a": ["the planet came out banded like a gas giant; the corrosive look was "
                                "rewritten as a smooth featureless ball"],
    "orbital-corrosive-yellow-a": ["remade with the rewritten corrosive look"],
    "orbital-methane-red-a": ["the planet came out banded like a gas giant; the methane look was "
                              "rewritten as a smooth hazy ball"],
    "orbital-methane-yellow-a": ["the planet came out banded like a gas giant; the methane look was "
                                 "rewritten as a smooth hazy ball"],
    "orbital-airless-red-a": ["a thin straight line crossed the sky and the planet"],
    "orbital-airless-yellow-a": ["a flat slab of deck jutted out of the bay mouth into open space",
                                 "the bay mouth came out as two openings joined like a figure eight",
                                 "the bay had an open roof as well as a mouth, and the planet seen through "
                                 "the mouth did not line up with the planet above",
                                 "a lens-flare streak crossed the planet"],
    "orbital-terran-yellow-b": ["a crane hook hung in front of the planet, and the parked craft looked "
                                "like present-day space shuttles"],
    "surface-terran-yellow-a": [TERRAN_AIRPORT, TERRAN_TOWERS],
    "surface-terran-red-a": [TERRAN_AIRPORT, TERRAN_TOWERS],
    "surface-terran-orange-a": [TERRAN_AIRPORT, TERRAN_TOWERS],
    "surface-terran-yellow-b": ["remade with the first rewritten terran look", TERRAN_TOWERS],
    "surface-terran-red-b": ["remade with the first rewritten terran look", TERRAN_TOWERS],
}


def why_remade(name, i):
    reasons = REMADE.get(name, [])
    return reasons[i] if i < len(reasons) else "superseded"


def record(ledger):
    calls = [json.loads(line) for line in open(ledger) if line.strip()]
    by_name = {}
    for c in calls:
        by_name.setdefault(os.path.basename(c["out"]), []).append(c)
    planned = [name_of(*k) for k in plan()]
    total = sum(c["usd"] for c in calls)
    shipped = [n for n in planned if os.path.exists(os.path.join(OUT, n + ".jpg"))]
    size = sum(os.path.getsize(os.path.join(OUT, n + ".jpg")) for n in shipped)
    o = ["# Backdrop art: prompts, model and cost",
         "",
         "Every picture in `art/backdrops/` was made by xAI Grok from the text prompt",
         "below and nothing else: no image went in, and nothing was traced, sampled or",
         "derived from the game's bitmaps or screens.  The prompts are built by",
         "`tools/art/backdrop_prompts.py` (`prompt NAME` prints any of them again, word",
         "for word); this page is written by its `record` command from the ledger",
         "`tools/art/grok_image.py` keeps.  What the pictures are for, and how one is",
         "chosen, is in [backdrop.md](backdrop.md).",
         "",
         f"- Calls: {len(calls)}, spend ${total:.2f} (cap $5.00).",
         f"- Shipped: {len(shipped)} pictures, {size / 1e6:.2f} MB.",
         "- Processing: `backdrop_prompts.py process` crops each to 4:3 at Grok's own",
         "  1152x864 and writes a progressive JPEG with cjpeg at quality 75.",
         "",
         "## Shipped pictures",
         "",
         "| File | Model | Calls | Cost | KB |",
         "|---|---|---|---|---|"]
    for n in shipped:
        cs = by_name.get(n, [])
        model = cs[-1]["model"] if cs else "?"
        kb = os.path.getsize(os.path.join(OUT, n + ".jpg")) // 1024
        o.append(f"| `{n}.jpg` | {model} | {len(cs)} | ${sum(c['usd'] for c in cs):.2f} | {kb} |")
    o += ["", "## The prompt of each shipped picture", ""]
    for n in shipped:
        cs = by_name.get(n, [])
        if not cs:
            o += [f"### {n}.jpg", "", "(no ledger entry)", ""]
            continue
        last = cs[-1]
        same = last["prompt"] == prompt(*parse(n))
        o += [f"### {n}.jpg", "",
              f"{last['model']}, ${last['usd']:.2f}" + ("" if same else
              " (this prompt differs from what the script now builds)"), "",
              "> " + last["prompt"], ""]
    o += ["## Attempts not shipped", ""]
    for n in planned:
        for i, c in enumerate(by_name.get(n, [])[:-1]):
            o += [f"### {n} (dropped)", "",
                  f"{c['model']}, ${c['usd']:.2f}: {why_remade(n, i)}.", "",
                  "> " + c["prompt"], ""]
    for n, cs in by_name.items():
        if n in planned:
            continue
        for c in cs:
            o += [f"### {n} (trial before the prompt script)", "",
                  f"{c['model']}, ${c['usd']:.2f}: a first look at the idea, not shipped.", "",
                  "> " + c["prompt"], ""]
    path = os.path.join(ROOT, "docs", "hd", "backdrops.md")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(o).rstrip() + "\n")
    print(f"{path}: {len(calls)} calls, ${total:.2f}, {len(shipped)} shipped")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--only", default="", help="comma-separated names")
    p = sub.add_parser("prompt")
    p.add_argument("name")
    p = sub.add_parser("process")
    p.add_argument("raw")
    p.add_argument("--quality", type=int, default=75)
    p = sub.add_parser("record")
    p.add_argument("ledger")
    a = ap.parse_args()
    if a.cmd == "plan":
        only = set(filter(None, a.only.split(",")))
        for key in plan():
            name = name_of(*key)
            if not only or name in only:
                print(name + "\t" + prompt(*key))
    elif a.cmd == "prompt":
        print(prompt(*parse(a.name)))
    elif a.cmd == "record":
        record(a.ledger)
    else:
        process(a.raw, a.quality)


if __name__ == "__main__":
    main()
