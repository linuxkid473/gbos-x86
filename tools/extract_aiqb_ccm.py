#!/usr/bin/env python3
"""Extract per-illuminant colour-correction matrices from an Intel camera tuning file (.aiqb).

The tuning file ships with the Windows camera driver (for the Latitude 7320 front camera:
DriverStore\\FileRepository\\ov5678.inf_amd64_*\\OV5678_0BF501T3_TGL.aiqb). It is proprietary,
so neither it nor the extracted table belongs in this public repository: run this locally and
copy the output to the stick as gbos/camera/ov5678_ccm.txt (the camera bridge loads it from there).

Layout found by inspection (no public format description was used): a colour-matrix table is a
run of 920-byte records, one per calibrated illuminant:
    u32 illuminant id
    f32 R/G, B/G    raw sensor white point under that illuminant (IR already removed)
    f32 x, y        CIE 1931 chromaticity of the illuminant
    25 x 9 f32      3x3 matrices, one per 15-degree hue sector (rows sum to 1)
The first such table in the file is used; each illuminant's matrix is the mean of its sectors.

Output, one illuminant per line:  id r_over_g b_over_g x y m00 m01 m02 m10 m11 m12 m20 m21 m22
"""
import struct
import sys

RECORD = 4 + 4 * 4 + 25 * 36
SECTORS = 25


def plausible_matrix(m):
    rows = [sum(m[i * 3:i * 3 + 3]) for i in range(3)]
    return (all(x == x for x in m) and all(abs(r - 1.0) < 0.05 for r in rows)
            and all(0.5 < m[i] < 4.0 for i in (0, 4, 8))
            and all(-3.0 < m[i] < 1.5 for i in (1, 2, 3, 5, 6, 7)))


def record_at(data, off):
    if off < 0 or off + RECORD > len(data):
        return None
    ident = struct.unpack_from("<I", data, off)[0]
    rg, bg, x, y = struct.unpack_from("<4f", data, off + 4)
    if not (0 < ident < 256 and 0.05 < rg < 3 and 0.05 < bg < 3 and 0.2 < x < 0.6 and 0.2 < y < 0.6):
        return None
    mats = [struct.unpack_from("<9f", data, off + 20 + 36 * i) for i in range(SECTORS)]
    good = [m for m in mats if plausible_matrix(m)]
    if len(good) < SECTORS - 2:
        return None
    mean = [sum(m[k] for m in good) / len(good) for k in range(9)]
    return ident, rg, bg, x, y, mean


def first_table(data):
    for off in range(0, len(data) - RECORD, 4):
        first = record_at(data, off)
        if not first:
            continue
        table, pos = [], off
        while True:
            rec = record_at(data, pos)
            if not rec:
                break
            table.append(rec)
            pos += RECORD
        if len(table) >= 2:
            return table
    return []


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: extract_aiqb_ccm.py TUNING.aiqb > ov5678_ccm.txt")
    data = open(sys.argv[1], "rb").read()
    table = first_table(data)
    if not table:
        sys.exit("no colour-matrix table found")
    print("# id r/g b/g x y ccm(3x3, row-major); extracted from %s" % sys.argv[1].replace("\\", "/").split("/")[-1])
    for ident, rg, bg, x, y, m in table:
        print("%d %.5f %.5f %.4f %.4f %s" % (ident, rg, bg, x, y, " ".join("%.5f" % v for v in m)))


if __name__ == "__main__":
    main()
