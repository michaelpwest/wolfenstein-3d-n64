#!/usr/bin/env python3
"""Turn the PPMs tools/sim wrote into one PNG contact sheet.

    python tools/sheet.py build/sim [columns [scale]]

scale 2 (the default) keeps the frames full size, 1 halves them.
"""

import os
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(
            ">I", zlib.crc32(body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6))
                + chunk(b"IEND", b""))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "build/sim"
    cols = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 2   # 1 = half size

    names = sorted(n for n in os.listdir(src) if n.endswith(".ppm"))
    if not names:
        raise SystemExit("no .ppm files in " + src)

    tiles = [read_ppm(os.path.join(src, n)) for n in names]
    tw, th = tiles[0][0] // scale, tiles[0][1] // scale
    pad = 4
    rows = (len(tiles) + cols - 1) // cols
    W = cols * (tw + pad) + pad
    H = rows * (th + pad) + pad
    sheet = bytearray(W * H * 3)

    for i, (w, h, px) in enumerate(tiles):
        ox = pad + (i % cols) * (tw + pad)
        oy = pad + (i // cols) * (th + pad)
        for y in range(th):
            sy = y * scale
            row = oy + y
            for x in range(tw):
                s = (sy * w + x * scale) * 3
                d = (row * W + ox + x) * 3
                sheet[d:d + 3] = px[s:s + 3]

    out = os.path.join(src, "sheet.png")
    write_png(out, W, H, bytes(sheet))
    print("wrote %s  (%d frames: %s)" % (out, len(tiles), ", ".join(
        n[:-4] for n in names)))


if __name__ == "__main__":
    main()
