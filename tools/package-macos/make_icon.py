#!/usr/bin/env python3
"""Draw the neutral app icon for Frontier.app and write it as a PNG.

The picture is generated from scratch by this script (a dark rounded tile, a
ringed planet and one small star). It is not derived from any game artwork.
Standard library only: no Pillow, no ImageMagick.

usage: make_icon.py OUT.png [SIZE]      (SIZE defaults to 1024)
"""
import math
import struct
import sys
import zlib


def smooth(edge0, edge1, x):
    if edge0 == edge1:
        return 1.0 if x >= edge1 else 0.0
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def coverage(dist, aa):
    """1 inside (dist < 0), 0 outside, one anti-aliased pixel wide edge."""
    return 1.0 - smooth(-aa, aa, dist)


def over(dst, rgb, a):
    """Alpha-composite colour rgb with coverage a onto dst (r, g, b, alpha)."""
    r, g, b, da = dst
    na = a + da * (1.0 - a)
    if na <= 0.0:
        return (0.0, 0.0, 0.0, 0.0)
    f = lambda s, d: (s * a + d * da * (1.0 - a)) / na
    return (f(rgb[0], r), f(rgb[1], g), f(rgb[2], b), na)


def render(size):
    px = 1.0 / size                       # one pixel, in unit coordinates
    aa = 1.2 * px                         # width of the anti-aliased edge

    # geometry, in unit coordinates (0..1, y down)
    tile_half, tile_r = 0.43, 0.19
    cx, cy = 0.5, 0.5
    planet_r = 0.19
    ring_a, ring_b = 0.36, 0.105          # ring radii (ellipse), tilted
    tilt = math.radians(-24.0)
    ring_w = 0.026
    ct, st = math.cos(tilt), math.sin(tilt)
    star = (0.74, 0.27, 0.030)
    light = (-0.55, -0.62)                # direction the light comes from
    ln = math.hypot(*light)
    light = (light[0] / ln, light[1] / ln)

    rows = []
    for j in range(size):
        y = (j + 0.5) * px
        row = bytearray([0])              # PNG filter type 0
        for i in range(size):
            x = (i + 0.5) * px
            c = (0.0, 0.0, 0.0, 0.0)

            # rounded square tile with a vertical gradient
            qx, qy = abs(x - 0.5) - (tile_half - tile_r), abs(y - 0.5) - (tile_half - tile_r)
            d_tile = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - tile_r
            g = (y - (0.5 - tile_half)) / (2 * tile_half)
            g = max(0.0, min(1.0, g))
            bg = (0.05 + 0.05 * g, 0.08 + 0.07 * g, 0.19 + 0.12 * g)
            c = over(c, bg, coverage(d_tile, aa))
            if c[3] <= 0.0:
                row += bytes((0, 0, 0, 0))
                continue

            # ring geometry in the ring's own frame
            dx, dy = x - cx, y - cy
            rx = dx * ct + dy * st
            ry = -dx * st + dy * ct
            e = math.sqrt((rx / ring_a) ** 2 + (ry / ring_b) ** 2)
            d_ring = (e - 1.0) * ring_b * 0.9        # approx. distance to the ellipse
            ring_cov = coverage(abs(d_ring) - ring_w * 0.5, aa)
            ring_col = (0.86, 0.80, 0.62)
            behind = ry < 0.0                        # upper half passes behind the planet

            if behind:
                c = over(c, ring_col, ring_cov * 0.9)

            # planet disc with simple diffuse shading
            d_pl = math.hypot(dx, dy) - planet_r
            pcov = coverage(d_pl, aa)
            if pcov > 0.0:
                nz2 = 1.0 - (dx * dx + dy * dy) / (planet_r * planet_r)
                nz = math.sqrt(max(nz2, 0.0))
                nx, ny = dx / planet_r, dy / planet_r
                lam = max(0.0, nx * light[0] + ny * light[1]) * 0.75 + nz * 0.25
                lam = max(0.0, min(1.0, lam))
                base = (0.20 + 0.62 * lam, 0.38 + 0.46 * lam, 0.62 + 0.30 * lam)
                c = over(c, base, pcov)

            if not behind:
                c = over(c, ring_col, ring_cov)

            # star with a faint halo
            sx, sy, sr = star
            ds = math.hypot(x - sx, y - sy)
            c = over(c, (1.0, 0.97, 0.85), coverage(ds - sr, aa))
            c = over(c, (1.0, 0.97, 0.85), 0.18 * (1.0 - smooth(sr, sr * 3.2, ds)))

            a = c[3]
            row += bytes((
                int(max(0.0, min(1.0, c[0])) * 255.0 + 0.5),
                int(max(0.0, min(1.0, c[1])) * 255.0 + 0.5),
                int(max(0.0, min(1.0, c[2])) * 255.0 + 0.5),
                int(max(0.0, min(1.0, a)) * 255.0 + 0.5),
            ))
        rows.append(bytes(row))
    return rows


def write_png(path, size, rows):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)    # 8-bit RGBA
    raw = b"".join(rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    write_png(out, size, render(size))


if __name__ == "__main__":
    main()
