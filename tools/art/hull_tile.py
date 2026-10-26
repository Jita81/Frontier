#!/usr/bin/env python3
"""Turn a picture of hull plating into a hull detail tile for the GL renderer.

    python3 -I tools/art/hull_tile.py IN.jpg OUT [--std 0.09]

writes OUT.lum (512x512 bytes, the format FE2_HULL_TILE loads) and
OUT.png and OUT-2x2.png to look at.  The game multiplies each ship face's
flat colour by the tile (texel 128 = x1.0), so the tile must:

  - be grey: the face colour comes from the game, the tile only adds detail;
  - have no light falloff: a high-pass takes out anything broader than a
    plate, or a whole face would come out darker on one side;
  - tile without a seam: it repeats across a face;
  - average 128 with a set spread (--std), so faces keep their brightness
    and every tile is as strong as every other.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bmp  # noqa: E402

N = 512


def box_blur(v, n, r):
    """Box blur of radius r, wrapping, along rows then columns."""
    out = [0.0] * (n * n)
    k = 2 * r + 1
    for y in range(n):
        base = y * n
        s = sum(v[base + (x % n)] for x in range(-r, r + 1))
        for x in range(n):
            out[base + x] = s / k
            s += v[base + (x + r + 1) % n] - v[base + (x - r) % n]
    res = [0.0] * (n * n)
    for x in range(n):
        s = sum(out[(y % n) * n + x] for y in range(-r, r + 1))
        for y in range(n):
            res[y * n + x] = s / k
            s += out[((y + r + 1) % n) * n + x] - out[((y - r) % n) * n + x]
    return res


def seam_error(v, n):
    """Mean step across the wrap edges over the mean step inside."""
    edge = sum(abs(v[y * n] - v[y * n + n - 1]) + abs(v[y] - v[(n - 1) * n + y])
               for y in range(n)) / (2 * n)
    inner = sum(abs(v[y * n + x] - v[y * n + x + 1])
                for y in range(0, n, 4) for x in range(n - 1)) / ((n // 4) * (n - 1))
    return edge / inner if inner else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out", help="output path without extension")
    ap.add_argument("--std", type=float, default=0.09,
                    help="spread of the multiplier about 1.0 (the procedural tile is about 0.07)")
    ap.add_argument("--radius", type=int, default=40, help="high-pass radius in texels")
    a = ap.parse_args()

    w, h, rows = bmp.load(a.src, N)
    v = []
    for row in rows:
        for x in range(N):
            v.append(0.2126 * row[3 * x] + 0.7152 * row[3 * x + 1] + 0.0722 * row[3 * x + 2] + 1.0)
    before = seam_error(v, N)

    # seamless, one axis at a time: blend with the copy shifted half a tile
    # along x (its edges are the original's middle, so they wrap), the
    # original weighted in the middle where the copy has its join and the
    # copy at the edges; then the same along y.  The blend keeps the
    # spread of detail, which a plain average would flatten into bands.
    mu = sum(v) / len(v)
    wgt = [math.sin(math.pi * (i + 0.5) / N) ** 2 for i in range(N)]

    def blend(a, b, w):
        return mu + (w * (a - mu) + (1 - w) * (b - mu)) / math.sqrt(w * w + (1 - w) * (1 - w))

    t = [blend(v[y * N + x], v[y * N + (x + N // 2) % N], wgt[x])
         for y in range(N) for x in range(N)]
    t = [blend(t[y * N + x], t[((y + N // 2) % N) * N + x], wgt[y])
         for y in range(N) for x in range(N)]

    # high-pass as a ratio to the local mean, twice-blurred (near Gaussian);
    # the blur wraps, which is right now that the tile does
    m = box_blur(box_blur(t, N, a.radius // 2), N, a.radius // 2)
    d = [t[i] / m[i] - 1.0 for i in range(N * N)]
    mean = sum(d) / len(d)
    sd = math.sqrt(sum((x - mean) ** 2 for x in d) / len(d)) or 1.0
    lum = []
    for x in d:
        f = 1.0 + (x - mean) * a.std / sd
        lum.append(max(0, min(255, int(128 * f + 0.5))))
    after = seam_error([float(x) for x in lum], N)

    with open(a.out + ".lum", "wb") as f:
        f.write(bytes(lum))
    bmp.save_png(a.out + ".png", N, N, bmp.grey_rows(N, N, lum))
    big = [lum[(y % N) * N + (x % N)] for y in range(2 * N) for x in range(2 * N)]
    bmp.save_png(a.out + "-2x2.png", 2 * N, 2 * N, bmp.grey_rows(2 * N, 2 * N, big))
    print(f"{a.out}: seam step {before:.2f}x the inner step before, {after:.2f}x after; "
          f"mean {sum(lum) / len(lum):.1f}")


if __name__ == "__main__":
    main()
