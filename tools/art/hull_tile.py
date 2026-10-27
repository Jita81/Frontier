#!/usr/bin/env python3
"""Turn a picture of hull plating into a hull detail tile for the GL renderer.

    python3 -I tools/art/hull_tile.py IN.jpg OUT [--std 0.09] [--size 512] [--cut]

writes OUT.png (an 8-bit grey PNG: the per-class tiles of --hull-style 5
are these, art/hull/*.png), OUT-2x2.png to look at the repeat, and at
--size 512 also OUT.lum (512x512 raw bytes, the format FE2_HULL_TILE
loads).  The game multiplies each ship face's flat colour by the tile
(texel 128 = x1.0), so the tile must:

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
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bmp  # noqa: E402

N = 512


def save_png_grey(path, n, lum):
    """An 8-bit greyscale PNG (a third the size of the RGB one sips writes)."""
    raw = b"".join(b"\x00" + bytes(lum[y * n:(y + 1) * n]) for y in range(n))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


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


def cut_seamless(v, n, feather=3):
    """Seamless along x without ghosting: the copy shifted half a tile (whose
    edges wrap) is kept near the tile's left and right edges and the original
    in the middle, the two joined along the path of least difference down
    each side (as in image quilting), softened over a few texels."""
    s = [v[y * n + (x + n // 2) % n] for y in range(n) for x in range(n)]
    err = [(v[i] - s[i]) ** 2 for i in range(n * n)]

    def path(lo, hi):
        # cheapest top-to-bottom path inside columns lo..hi, a column a row
        cost = [err[x] for x in range(lo, hi)]
        back = []
        for y in range(1, n):
            row, nb = [], []
            for i in range(hi - lo):
                j = min((cost[k], k) for k in (i - 1, i, i + 1) if 0 <= k < hi - lo)[1]
                row.append(cost[j] + err[y * n + lo + i])
                nb.append(j)
            cost = row
            back.append(nb)
        i = min(range(hi - lo), key=lambda k: cost[k])
        xs = [0] * n
        xs[n - 1] = lo + i
        for y in range(n - 1, 0, -1):
            i = back[y - 1][i]
            xs[y - 1] = lo + i
        return xs

    left, right = path(n // 16, n // 4), path(3 * n // 4, 15 * n // 16)
    out = []
    for y in range(n):
        a, b = left[y], right[y]
        for x in range(n):
            # weight of the original: 0 outside the cuts, 1 inside
            w = min(1.0, max(0.0, (x - a) / (2 * feather) + 0.5),
                    max(0.0, (b - x) / (2 * feather) + 0.5))
            out.append(w * v[y * n + x] + (1 - w) * s[y * n + x])
    return out


def transpose(v, n):
    return [v[x * n + y] for y in range(n) for x in range(n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out", help="output path without extension")
    ap.add_argument("--std", type=float, default=0.09,
                    help="spread of the multiplier about 1.0 (the procedural tile is about 0.07)")
    ap.add_argument("--radius", type=int, default=0,
                    help="high-pass radius in texels (default 40 per 512 texels)")
    ap.add_argument("--size", type=int, default=512, choices=(256, 512, 1024, 2048),
                    help="tile side in texels")
    ap.add_argument("--cut", action="store_true",
                    help="make it seamless along a cut of least difference, not by "
                         "blending (no ghosted double image; for pictures far from seamless)")
    a = ap.parse_args()
    global N
    N = a.size
    if not a.radius:
        a.radius = 40 * N // 512

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

    if a.cut:
        t = transpose(cut_seamless(transpose(cut_seamless(v, N), N), N), N)
    else:
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

    if N == 512:
        with open(a.out + ".lum", "wb") as f:
            f.write(bytes(lum))
    save_png_grey(a.out + ".png", N, lum)
    big = [lum[(y % N) * N + (x % N)] for y in range(2 * N) for x in range(2 * N)]
    bmp.save_png(a.out + "-2x2.png", 2 * N, 2 * N, bmp.grey_rows(2 * N, 2 * N, big))
    print(f"{a.out}: seam step {before:.2f}x the inner step before, {after:.2f}x after; "
          f"mean {sum(lum) / len(lum):.1f}")


if __name__ == "__main__":
    main()
