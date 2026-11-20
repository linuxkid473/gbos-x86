#!/usr/bin/env python3
"""Convert a YUYV (BT.601 full range, as the camera bridge writes) picture to PNG.

    yuyv_to_png.py IN.yuyv WIDTH HEIGHT OUT.png
"""
import struct
import sys
import zlib


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v + 0.5)


def main():
    src, width, height, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    data = open(src, "rb").read()
    if len(data) < width * height * 2:
        sys.exit(f"{src}: {len(data)} bytes, expected {width * height * 2}")
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        base = y * width * 2
        for x in range(0, width, 2):
            y0, u, y1, v = data[base + x * 2: base + x * 2 + 4]
            u -= 128
            v -= 128
            for yy in (y0, y1):
                rows += bytes((clamp(yy + 1.402 * v), clamp(yy - 0.344 * u - 0.714 * v), clamp(yy + 1.772 * u)))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main()
