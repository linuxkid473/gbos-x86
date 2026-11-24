#!/usr/bin/env python3
"""Tests for tools/extract_aiqb_ccm.py using synthetic tuning data only (no proprietary files)."""
import importlib.util
import struct
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "extract_aiqb_ccm", Path(__file__).resolve().parent / "extract_aiqb_ccm.py")
x = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x)

GW, GH = 21, 15  # large enough for the extractor's 1000-byte record guard


def rec(fmt, rec_id, payload, pad=0):
    body = payload + b"\0" * pad
    return struct.pack("<IHH", 8 + len(body), fmt, rec_id) + body


def general(order):
    return rec(102, 2, struct.pack("<6H", 3280, 2468, 10, order, 10, 0) + b"\0" * 20)


def grid(corner, centre=2048):
    vals = []
    for y in range(GH):
        for xx in range(GW):
            edge = x_edge = (xx in (0, GW - 1)) + (y in (0, GH - 1))
            vals.append(int(centre * (1 + (corner - 1) * x_edge / 2)))
    return struct.pack("<%dH" % (GW * GH), *vals)


def shading(cfa, n_ch, corners_per_channel, illums, pad=0):
    body = bytes(cfa) + struct.pack("<4H", len(illums), n_ch, GW, GH)
    for ident, rg, bg in illums:
        body += struct.pack("<Iff", ident, rg, bg) + b"\0" * 8 + struct.pack("<H", 5)
        for c in range(n_ch):
            body += grid(corners_per_channel[c])
    return rec(100, 28, body, pad)


def ccm_table(idents):
    body = b""
    for i, ident in enumerate(idents):
        body += struct.pack("<I4f", ident, 0.9 - 0.1 * i, 0.45 + 0.05 * i, 0.44 - 0.03 * i, 0.41 - 0.02 * i)
        body += struct.pack("<9f", 1.8, -0.5, -0.3, -0.2, 1.6, -0.4, -0.1, -0.7, 1.8) * 25
    return rec(100, 25, b"\0" * 8 + body)


def main():
    bayer_cfa = [0, 1, 0xFF, 0xFF, 2, 3] + [0xFF] * 10
    rgbir_cfa = [0, 1, 0, 1, 2, 3, 4, 3, 0, 1, 0, 1, 4, 3, 2, 3]

    # Bayer GRBG with record padding: Gr R / B Gb -> R = ch1, B = ch2, G = mean(ch0, ch3).
    data = b"CPFF" + general(0) + shading(bayer_cfa, 4, [3.0, 5.0, 2.5, 3.0], [(1, 0.9, 0.45), (0, 0.6, 0.7)], pad=6)
    what, gw, gh, table = x.lens_shading(data)
    assert "GRBG" in what and (gw, gh) == (GW, GH) and len(table) == 2
    ident, rg, bg, r, g, b = table[1]
    assert ident == 0 and abs(rg - 0.6) < 1e-6
    centre = (GH // 2) * GW + GW // 2
    assert abs(r[centre] - 1) < 1e-9 and abs(g[centre] - 1) < 1e-9
    assert abs(r[0] - 5.0) < 0.01 and abs(b[0] - 2.5) < 0.01 and abs(g[0] - 3.0) < 0.01

    # RGB-IR: R = ch2, B = ch4, G = mean(ch0, ch3); the IR grid (ch1) is ignored.
    data = b"CPFF" + general(260) + shading(rgbir_cfa, 5, [3.0, 8.0, 4.0, 3.0, 3.5], [(10, 0.67, 0.5)])
    what, _, _, table = x.lens_shading(data)
    assert "RGB-IR" in what
    _, _, _, r, g, b = table[0]
    assert abs(r[0] - 4.0) < 0.01 and abs(g[0] - 3.0) < 0.01 and abs(b[0] - 3.5) < 0.01

    # Unknown Bayer colour order is refused rather than guessed.
    data = b"CPFF" + general(2) + shading(bayer_cfa, 4, [3, 5, 2.5, 3], [(1, 0.9, 0.45)])
    try:
        x.lens_shading(data)
        raise AssertionError("unknown colour order accepted")
    except SystemExit:
        pass

    # Greens that disagree mean the map is not understood.
    data = b"CPFF" + general(0) + shading(bayer_cfa, 4, [3.0, 5.0, 2.5, 4.5], [(1, 0.9, 0.45)])
    try:
        x.lens_shading(data)
        raise AssertionError("disagreeing greens accepted")
    except SystemExit:
        pass

    # Colour matrices: illuminant id 0 is valid (the OV8856 file has one).
    table = x.first_table(b"CPFF" + ccm_table([19, 0, 1]))
    assert [t[0] for t in table] == [19, 0, 1]
    assert abs(table[0][5][0] - 1.8) < 1e-6

    print("EXTRACT_AIQB_TEST_PASS")


if __name__ == "__main__":
    main()
