#!/usr/bin/env python3
"""Draw a hull tile of hard-surface plating in code, from nothing but a seed.

    python3 -I tools/art/plate_tile.py OUT [--size 1024] [--cells 16] [--seed 7]
                                       [--std 0.09] [--hatches 2] [--vents 2]

writes OUT.png (an 8-bit grey PNG, mean 128, the format of art/hull/*.png:
see hull_tile.py) and OUT-2x2.png to look at the repeat.  No picture goes
in.  The tile is a grid of --cells x --cells cells packed with steel plates
of mixed sizes, mostly square (four cells to one a side, a few two by
three), so no seam runs the length of the tile and no row of plates repeats
the next: a face seen at a slant gets a shuffle of rectangles, never the
running courses of boards or bricks.  Plates may cross the tile's edge (the
packing wraps), so the tile repeats without a seam of its own.  The seams
are thin straight grooves; the plates differ a little in tone; a few carry
an inset panel line or corner bolts, and only --hatches plates an access
hatch and --vents a louvred vent (two of each a tile: sparse, so the face
reads as plating, not as a board of fittings); and a faint broad mottle
(no grain, no streaks: it is the same in every direction) keeps the steel
from looking printed.
"""
import argparse
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bmp  # noqa: E402
from hull_tile import save_png_grey  # noqa: E402

SEAM = -2.0      # the groove between plates
GROOVE = -1.1    # an inset panel line or a hatch's outline
SLOT = -1.6      # a vent's louvre, a hatch's handle
BOLT = -1.0


def value_noise(n, period, rnd):
    """Smooth noise that wraps at the tile's edge: period x period lattice."""
    lat = [rnd.random() for _ in range(period * period)]
    cell = n / period
    out = [0.0] * (n * n)
    for y in range(n):
        fy = y / cell
        iy = int(fy)
        ty = fy - iy
        ty = ty * ty * (3 - 2 * ty)
        r0 = (iy % period) * period
        r1 = ((iy + 1) % period) * period
        base = y * n
        for x in range(n):
            fx = x / cell
            ix = int(fx)
            tx = fx - ix
            tx = tx * tx * (3 - 2 * tx)
            i0 = ix % period
            i1 = (ix + 1) % period
            a = lat[r0 + i0] + (lat[r0 + i1] - lat[r0 + i0]) * tx
            b = lat[r1 + i0] + (lat[r1 + i1] - lat[r1 + i0]) * tx
            out[base + x] = a + (b - a) * ty - 0.5
    return out


def pack(g, rnd):
    """Plates as (cx, cy, w, h) in cells over a g x g grid that wraps."""
    used = [[False] * g for _ in range(g)]

    def free(cx, cy, w, h):
        return all(not used[(cy + j) % g][(cx + i) % g] for j in range(h) for i in range(w))

    def take(cx, cy, w, h):
        for j in range(h):
            for i in range(w):
                used[(cy + j) % g][(cx + i) % g] = True

    plates = []
    # big plates first, at random, then the gaps filled with what fits
    for sizes, tries in (([(4, 4), (4, 3), (3, 4), (3, 3)], 200),
                         ([(3, 2), (2, 3), (2, 2), (2, 2)], 400)):
        for _ in range(tries):
            w, h = rnd.choice(sizes)
            cx, cy = rnd.randrange(g), rnd.randrange(g)
            if free(cx, cy, w, h):
                take(cx, cy, w, h)
                plates.append((cx, cy, w, h))
    order = [(cx, cy) for cy in range(g) for cx in range(g)]
    rnd.shuffle(order)
    for cx, cy in order:
        if used[cy][cx]:
            continue
        thin = [(2, 1), (1, 2)] if rnd.random() < 0.6 else []
        rnd.shuffle(thin)
        for w, h in [(2, 2)] + thin + [(1, 1)]:
            if free(cx, cy, w, h):
                take(cx, cy, w, h)
                plates.append((cx, cy, w, h))
                break
    return plates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", help="output path without extension")
    ap.add_argument("--size", type=int, default=1024, choices=(512, 1024, 2048))
    ap.add_argument("--cells", type=int, default=16, help="grid cells a side")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--std", type=float, default=0.09,
                    help="spread of the multiplier about 1.0 (as hull_tile.py)")
    ap.add_argument("--hatches", type=int, default=2, help="access hatches a tile")
    ap.add_argument("--vents", type=int, default=2, help="louvred vents a tile")
    a = ap.parse_args()
    n, g = a.size, a.cells
    s = n / 1024.0
    c = n // g
    rnd = random.Random(a.seed)
    h = [SEAM] * (n * n)

    def put(x, y, v):
        h[(y % n) * n + (x % n)] = v

    def rect(x0, y0, w, hh, v):
        for y in range(y0, y0 + hh):
            for x in range(x0, x0 + w):
                put(x, y, v)

    def outline(x0, y0, w, hh, t, v):
        rect(x0, y0, w, t, v)
        rect(x0, y0 + hh - t, w, t, v)
        rect(x0, y0, t, hh, v)
        rect(x0 + w - t, y0, t, hh, v)

    plates = pack(g, rnd)
    # the seam: the outer pixels of each cell boundary, about 3 at 1024
    lo, hi = max(1, round(2 * s)), max(1, round(1 * s))
    big = []
    for cx, cy, w, hh in plates:
        tone = rnd.uniform(-0.12, 0.12)
        if rnd.random() < 0.08:
            tone += rnd.choice((-0.25, 0.25))
        x0, y0, pw, ph = cx * c + lo, cy * c + lo, w * c - lo - hi, hh * c - lo - hi
        rect(x0, y0, pw, ph, tone)
        if min(w, hh) >= 3 and rnd.random() < 0.5:
            k = round(14 * s)
            outline(x0 + k, y0 + k, pw - 2 * k, ph - 2 * k, max(1, round(2 * s)), tone + GROOVE)
        if min(w, hh) >= 2 and rnd.random() < 0.5:
            k, r = round(8 * s), max(1, round(2 * s))
            for bx, by in ((x0 + k, y0 + k), (x0 + pw - k, y0 + k),
                           (x0 + k, y0 + ph - k), (x0 + pw - k, y0 + ph - k)):
                for dy in range(-r, r + 1):
                    for dx in range(-r, r + 1):
                        if dx * dx + dy * dy <= r * r:
                            put(bx + dx, by + dy, tone + BOLT)
        if min(w, hh) >= 2:
            big.append((x0, y0, pw, ph, tone))

    # a few access hatches and louvred vents, each on a plate of its own
    rnd.shuffle(big)
    t = max(1, round(2 * s))
    nh, nv = a.hatches, a.vents
    for x0, y0, pw, ph, tone in big[:nh]:
        hw, hh = int(pw * rnd.uniform(0.35, 0.55)), int(ph * rnd.uniform(0.3, 0.5))
        hx, hy = x0 + (pw - hw) // 2, y0 + (ph - hh) // 2
        outline(hx, hy, hw, hh, t, tone + GROOVE)
        rect(hx + hw // 2 - round(8 * s), hy + round(8 * s), round(16 * s), max(2, round(4 * s)),
             tone + SLOT)
    for x0, y0, pw, ph, tone in big[nh:nh + nv]:
        vw, vh = int(pw * 0.4), int(ph * 0.4)
        vx, vy = x0 + (pw - vw) // 2, y0 + (ph - vh) // 2
        outline(vx, vy, vw, vh, t, tone + GROOVE)
        across = rnd.random() < 0.5
        step = round(8 * s)
        span = vh if across else vw
        for k in range(round(8 * s), span - round(6 * s), step):
            if across:
                rect(vx + round(6 * s), vy + k, vw - round(12 * s), round(3 * s), tone + SLOT)
            else:
                rect(vx + k, vy + round(6 * s), round(3 * s), vh - round(12 * s), tone + SLOT)

    # the faint mottle, the same every way
    for period, amp in ((4, 0.10), (8, 0.06), (32, 0.03)):
        m = value_noise(n, period, rnd)
        for i in range(n * n):
            h[i] += amp * m[i]

    mean = sum(h) / len(h)
    sd = math.sqrt(sum((v - mean) ** 2 for v in h) / len(h)) or 1.0
    lum = [max(0, min(255, int(128 * (1.0 + (v - mean) * a.std / sd) + 0.5))) for v in h]
    save_png_grey(a.out + ".png", n, lum)
    twice = [lum[(y % n) * n + (x % n)] for y in range(2 * n) for x in range(2 * n)]
    bmp.save_png(a.out + "-2x2.png", 2 * n, 2 * n, bmp.grey_rows(2 * n, 2 * n, twice))
    print(f"{a.out}: {len(plates)} plates, mean {sum(lum) / len(lum):.1f}, "
          f"min {min(lum)}, max {max(lum)}")


if __name__ == "__main__":
    main()
