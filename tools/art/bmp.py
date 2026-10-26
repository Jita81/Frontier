"""Read and write uncompressed BMP files, and convert to and from them with
macOS sips, so the art tools need nothing beyond the Python standard library.

An image is (w, h, rows), rows a list of h bytearrays of 3*w bytes (R, G, B),
top row first.
"""
import os
import struct
import subprocess
import tempfile


def read(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off, = struct.unpack_from("<I", d, 10)
    w, h, _, bpp, comp = struct.unpack_from("<iiHHI", d, 18)
    if bpp not in (24, 32) or comp not in (0, 3):
        raise ValueError(f"{path}: {bpp} bit, compression {comp} not handled")
    top_down = h < 0
    h = abs(h)
    stride = (bpp * w + 31) // 32 * 4
    n = bpp // 8
    rows = []
    for y in range(h):
        src = off + (y if top_down else h - 1 - y) * stride
        row = bytearray(3 * w)
        row[0::3] = d[src + 2:src + n * w:n]
        row[1::3] = d[src + 1:src + n * w:n]
        row[2::3] = d[src + 0:src + n * w:n]
        rows.append(row)
    return w, h, rows


def write(path, w, h, rows):
    stride = (24 * w + 31) // 32 * 4
    pad = bytes(stride - 3 * w)
    out = bytearray(b"BM" + struct.pack("<IHHI", 54 + stride * h, 0, 0, 54))
    out += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0)
    for row in reversed(rows):
        bgr = bytearray(3 * w)
        bgr[0::3] = row[2::3]
        bgr[1::3] = row[1::3]
        bgr[2::3] = row[0::3]
        out += bgr + pad
    with open(path, "wb") as f:
        f.write(out)


def load(path, size=None):
    """Any image sips reads, optionally resized to size x size."""
    with tempfile.TemporaryDirectory() as t:
        tmp = os.path.join(t, "a.bmp")
        cmd = ["sips", "-s", "format", "bmp"]
        if size:
            cmd += ["-z", str(size), str(size)]
        p = subprocess.run(cmd + [path, "--out", tmp], check=True, capture_output=True)
        # sips can exit 0 having written nothing (a sandboxed temp dir:
        # set TMPDIR somewhere writable)
        if not os.path.exists(tmp):
            raise RuntimeError(f"sips wrote nothing for {path}: {p.stderr.decode().strip()}")
        return read(tmp)


def save_png(path, w, h, rows):
    with tempfile.TemporaryDirectory() as t:
        tmp = os.path.join(t, "a.bmp")
        write(tmp, w, h, rows)
        subprocess.run(["sips", "-s", "format", "png", tmp, "--out", path],
                       check=True, capture_output=True)


def grey_rows(w, h, lum):
    """RGB rows from a flat list of 0..255 values."""
    rows = []
    for y in range(h):
        v = bytes(lum[y * w:(y + 1) * w])
        row = bytearray(3 * w)
        row[0::3] = v
        row[1::3] = v
        row[2::3] = v
        rows.append(row)
    return rows


def sheet(images, cols, gap=8, bg=40):
    """Images (w, h, rows) of one size in a grid."""
    w, h = images[0][0], images[0][1]
    nrow = (len(images) + cols - 1) // cols
    W, H = cols * w + (cols + 1) * gap, nrow * h + (nrow + 1) * gap
    rows = [bytearray([bg]) * (3 * W) for _ in range(H)]
    for i, (_, _, im) in enumerate(images):
        x0 = gap + (i % cols) * (w + gap)
        y0 = gap + (i // cols) * (h + gap)
        for y in range(h):
            rows[y0 + y][3 * x0:3 * (x0 + w)] = im[y]
    return W, H, rows
