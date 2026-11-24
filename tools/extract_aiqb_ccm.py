#!/usr/bin/env python3
"""Extract colour calibration from an Intel camera tuning file (.aiqb): per-illuminant
colour-correction matrices and, with --lsc, lens-shading gain grids.

The tuning file ships with the Windows camera driver (for the Latitude 7320 front camera:
DriverStore\\FileRepository\\ov5678.inf_amd64_*\\OV5678_0BF501T3_TGL.aiqb). It is proprietary,
so neither it nor the extracted tables belong in this public repository: run this locally and
copy the output to the stick as gbos/camera/ov5678_ccm.txt and gbos/camera/ov5678_lsc.txt (the
camera bridge loads gbos/camera/ from there).

Layout found by inspection (no public format description was used): a colour-matrix table is a
run of 920-byte records, one per calibrated illuminant:
    u32 illuminant id
    f32 R/G, B/G    raw sensor white point under that illuminant (IR already removed)
    f32 x, y        CIE 1931 chromaticity of the illuminant
    25 x 9 f32      3x3 matrices, one per 15-degree hue sector (rows sum to 1)
The first such table in the file is used; each illuminant's matrix is the mean of its sectors.

Output, one illuminant per line:  id r_over_g b_over_g x y m00 m01 m02 m10 m11 m12 m20 m21 m22

Lens shading (--lsc) is the record whose header is (u32 size, u16 100, u16 28): a 16-byte 4x4
colour-filter map, u16 illuminants, u16 channels, u16 grid width, u16 grid height, then per
illuminant: u32 id, f32 R/G, f32 B/G, 8 bytes, a u16 (5 in both files seen; not the channel count),
and one u16 gain grid per channel (row-major, 2048 = 1.0 at the centre). The front OV5678 is RGB-IR
(map 0 G, 1 IR, 2 R, 3 G, 4 B, matching the measured mosaic; its IR grid is a clamped placeholder and
not used); the rear OV8856 is Bayer GRBG (map 0 1 / 2 3 = Gr R / B Gb; its two green grids agree to
~1-2% and the red one shows the strongest corner fall-off). Grids are written normalised to 1.0 at
the centre (auto exposure sets the overall level); green is the mean of both green phases.
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
    if not (0 <= ident < 256 and 0.05 < rg < 3 and 0.05 < bg < 3 and 0.2 < x < 0.6 and 0.2 < y < 0.6):
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


def record(data, rec_id, fmt):
    """First top-level record (u32 size, u16 format, u16 id) with this id and format."""
    for off in range(0, len(data) - 8, 4):
        size, f, r = struct.unpack_from("<IHH", data, off)
        if r == rec_id and f == fmt and 8 <= size and off + size <= len(data):
            return off, size
    return None


def lens_shading(data):
    """Returns (description, gw, gh, [(id, rg, bg, R, G, B)]) or None.

    5 channels: the RGB-IR map (0 G, 1 IR, 2 R, 3 G, 4 B), as on the Latitude's OV5678.
    4 channels: a 2x2 Bayer map (unused 4x4 entries 0xff) whose colours follow the sensor's
    colour order in the general record (id 2); only order 0 = GRBG (the OV8856, matching its
    Linux bus code) is known. The per-illuminant u16 after the 8 zero bytes is not the channel
    count (it reads 5 in both files); the record header's count is used, and a record may end
    with a few bytes of padding.
    """
    for off in range(0, len(data) - 32, 4):
        size, fmt, rec = struct.unpack_from("<IHH", data, off)
        if rec != 28 or fmt != 100 or size < 1000 or off + size > len(data):
            continue
        h = off + 8
        cfa = list(data[h:h + 16])
        n_ill, n_ch, gw, gh = struct.unpack_from("<4H", data, h + 16)
        if not (0 < n_ill < 32 and n_ch in (4, 5) and 4 < gw <= 128 and 4 < gh <= 128):
            continue
        stride = 22 + n_ch * gw * gh * 2
        if not 0 <= size - (8 + 24 + n_ill * stride) < 8:
            continue
        if n_ch == 5:
            if sorted(set(cfa)) != [0, 1, 2, 3, 4]:
                continue
            red, greens, blue = 2, (0, 3), 4
            what = "RGB-IR 4x4 cfa %s" % cfa
        else:
            if [cfa[0], cfa[1], cfa[4], cfa[5]] != [0, 1, 2, 3] or \
               any(v != 0xFF for i, v in enumerate(cfa) if i not in (0, 1, 4, 5)):
                continue
            general = record(data, 2, 102)
            order = struct.unpack_from("<H", data, general[0] + 14)[0] if general else None
            if order != 0:
                sys.exit("unsupported Bayer colour order %r (only 0 = GRBG is verified)" % order)
            red, greens, blue = 1, (0, 3), 2
            what = "Bayer GRBG (Gr R / B Gb)"
        out, o = [], h + 24
        for _ in range(n_ill):
            ident, rg, bg = struct.unpack_from("<Iff", data, o)
            grids = [struct.unpack_from("<%dH" % (gw * gh), data, o + 22 + c * gw * gh * 2)
                     for c in range(n_ch)]
            centre = (gh // 2) * gw + gw // 2

            def norm(g):
                return [v / g[centre] for v in g]
            g0, g1 = norm(grids[greens[0]]), norm(grids[greens[1]])
            if sum(abs(a - b) for a, b in zip(g0, g1)) / len(g0) > 0.05:
                sys.exit("green grids of illuminant %d disagree: channel map not understood" % ident)
            out.append((ident, rg, bg, norm(grids[red]), [(a + b) / 2 for a, b in zip(g0, g1)],
                        norm(grids[blue])))
            o += stride
        return what, gw, gh, out
    return None


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--lsc":
        found = lens_shading(open(sys.argv[2], "rb").read())
        if not found:
            sys.exit("no lens-shading record found")
        what, gw, gh, table = found
        print("# lens shading, gains normalised to 1.0 at the centre; %s; channels R G B" % what)
        for ident, rg, bg, r, g, b in table:
            print("lsc %d %.5f %.5f %d %d" % (ident, rg, bg, gw, gh))
            for grid in (r, g, b):
                print(" ".join("%.4f" % v for v in grid))
        return
    if len(sys.argv) != 2:
        sys.exit("usage: extract_aiqb_ccm.py TUNING.aiqb > ov5678_ccm.txt\n"
                 "       extract_aiqb_ccm.py --lsc TUNING.aiqb > ov5678_lsc.txt")
    data = open(sys.argv[1], "rb").read()
    table = first_table(data)
    if not table:
        sys.exit("no colour-matrix table found")
    print("# id r/g b/g x y ccm(3x3, row-major); extracted from %s" % sys.argv[1].replace("\\", "/").split("/")[-1])
    for ident, rg, bg, x, y, m in table:
        print("%d %.5f %.5f %.4f %.4f %s" % (ident, rg, bg, x, y, " ".join("%.5f" % v for v in m)))


if __name__ == "__main__":
    main()
