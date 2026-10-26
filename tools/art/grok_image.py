#!/usr/bin/env python3
"""Make images with xAI's Grok Imagine API, from a text prompt only.

The key comes from the environment, never the command line:

    withkey XAI_API_KEY -- python3 -I tools/art/grok_image.py \
        --out DIR/name "prompt"

Every image is new art made from words.  Never give it game bitmaps or
screenshots: the remakes must not be traced from the original's art.
Each call appends a line (model, prompt, cost) to DIR/ledger.jsonl so the
spend is always known.
"""
import argparse
import base64
import json
import os
import re
import sys
import urllib.error
import urllib.request

URL = "https://api.x.ai/v1/images/generations"
# US dollars per image, from /v1/image-generation-models (2026-10-26)
PRICE = {
    "grok-imagine-image": 0.02,
    "grok-imagine-image-quality": 0.05,
    "grok-imagine-image-2.0": 0.06,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--model", default="grok-imagine-image", choices=sorted(PRICE))
    ap.add_argument("--n", type=int, default=1, help="images for this prompt")
    ap.add_argument("--aspect", default="1:1", help="aspect ratio, '' to leave it out")
    ap.add_argument("--out", required=True, help="output path, without extension")
    ap.add_argument("prompt")
    a = ap.parse_args()

    key = os.environ.get("XAI_API_KEY")
    if not key:
        sys.exit("grok_image: no XAI_API_KEY in the environment (run it under withkey)")
    body = {"model": a.model, "prompt": a.prompt, "n": a.n, "response_format": "b64_json"}
    if a.aspect:
        body["aspect_ratio"] = a.aspect
    req = urllib.request.Request(
        URL, json.dumps(body).encode(),
        {"Authorization": "Bearer " + key, "Content-Type": "application/json",
         "User-Agent": "frontier-art/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            d = json.load(r)
    except urllib.error.HTTPError as e:
        # an error body can quote the key: never show one unredacted
        msg = re.sub(r"xai-[A-Za-z0-9_-]+", "xai-[key]", e.read().decode("utf-8", "replace"))
        sys.exit(f"grok_image: HTTP {e.code}: {msg[:500]}")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    images = d.get("data", [])
    for i, im in enumerate(images):
        raw = base64.b64decode(im["b64_json"])
        ext = ".png" if raw[:4] == b"\x89PNG" else ".jpg"
        path = (f"{a.out}-{i}" if len(images) > 1 else a.out) + ext
        with open(path, "wb") as f:
            f.write(raw)
        print(path, len(raw), "bytes")
        if im.get("revised_prompt"):
            print("  revised prompt:", im["revised_prompt"][:300])
    cost = PRICE[a.model] * len(images)
    with open(os.path.join(os.path.dirname(os.path.abspath(a.out)), "ledger.jsonl"), "a") as f:
        f.write(json.dumps({"out": a.out, "model": a.model, "n": len(images),
                            "usd": round(cost, 4), "prompt": a.prompt}) + "\n")
    print(f"grok_image: {len(images)} image(s), ${cost:.2f}")


if __name__ == "__main__":
    main()
