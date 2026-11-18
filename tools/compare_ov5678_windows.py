#!/usr/bin/env python3
"""Compare the Windows OV5678 sensor settings with the Linux ov5675 driver's tables.

The Latitude 7320 Detachable's front camera is ACPI OVTI5678 (Windows: "Camera Sensor OV5678",
module 0BF501T3); Linux binds it with drivers/media/i2c/ov5675.c. Inputs are read locally and
nothing is written: the Windows files are proprietary and stay out of this repository.

  compare_ov5678_windows.py ov5678.sys ov5675.c [OV5678_0BF501T3_TGL.aiqb]

ov5678.sys (DriverStore\\FileRepository\\ov5678.inf_amd64_*) stores its register sequences as
16-byte entries (u32 type, u32 address, u32 value, u32 0): type 1 writes an 8-bit register,
type 0x20 is a delay, type 0xffff ends a sequence. The mode sequences start with a software
reset (0x0103 = 1) and contain the output size registers 0x3808..0x380b.

With the .aiqb, also prints the AE exposure-program rows of tuning record 261 (LAIQ section,
format 119): per AE mode a u32 minimum exposure (us), five u32 exposure times (us) and six
16.16 fixed-point gains. Our reading: analogue gain 1..15.5x and, in the first (automatic)
row, exposure up to 66.7 ms.
"""
import re
import struct
import sys


def windows_sequences(data):
    seqs, cur, off = [], [], 0
    while off + 16 <= len(data):
        kind, addr, val, pad = struct.unpack_from("<IIII", data, off)
        if kind == 1 and addr <= 0xffff and val <= 0xff and pad == 0:
            cur.append((addr, val))
            off += 16
            continue
        if kind == 0x20 and addr == 0 and pad == 0:
            off += 16
            continue
        if kind == 0xffff and cur:
            seqs.append(cur)
        cur = []
        off += 16 if kind in (0xffff,) else 4
    return [s for s in seqs if (0x0103, 1) in s and any(a == 0x3808 for a, _ in s)]


def linux_tables(source):
    tables = {}
    for name, body in re.findall(r"static const struct ov5675_reg (\w+)\[\] = \{(.*?)\};",
                                 source, re.S):
        tables[name] = [(int(a, 16), int(v, 16)) for a, v in
                        re.findall(r"\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\s*\}", body)]
    return tables


def final_values(seq):
    vals = {}
    for addr, val in seq:
        vals[addr] = val
    return vals


def size_of(vals):
    return ((vals.get(0x3808, 0) << 8 | vals.get(0x3809, 0)),
            (vals.get(0x380a, 0) << 8 | vals.get(0x380b, 0)))


def exposure_program(aiqb):
    """Rows of the LAIQ AE exposure-program record (id 261, format 119), fixed-point copy."""
    for off in range(0, len(aiqb) - 8, 4):
        size, fmt, rid = struct.unpack_from("<IHH", aiqb, off)
        if rid == 261 and fmt == 119 and 0x100 < size < 0x10000 and off + size <= len(aiqb):
            rec = aiqb[off:off + size]
            rows = []
            for row in range(0x48, size - 0x30, 0x30):
                vals = struct.unpack_from("<6I6I", rec, row)
                gains = [v / 65536 for v in vals[6:]]
                if not (0 < vals[0] <= 100000 and all(0 < t <= 10 ** 7 for t in vals[1:6])
                        and gains[0] == 1.0 and all(0.5 <= g <= 64 for g in gains)):
                    break
                rows.append((vals[:6], gains))
            return rows
    return []


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    sysdata = open(sys.argv[1], "rb").read()
    linux = linux_tables(open(sys.argv[2], encoding="utf-8", errors="replace").read())
    pll = linux.get("mipi_data_rate_900mbps", [])
    full = linux.get("mode_2592x1944_regs", [])
    if not pll or not full:
        sys.exit("ov5675.c: register tables not found")
    lin = final_values(pll + full)
    seqs = windows_sequences(sysdata)
    if not seqs:
        sys.exit("ov5678.sys: no mode sequences found")
    for i, seq in enumerate(seqs):
        win = final_values(seq)
        w, h = size_of(win)
        print(f"Windows mode {i}: {len(seq)} writes, output {w}x{h}")
    win = final_values(seqs[0])
    print("\nWindows mode 0 vs Linux 2592x1944 (PLL table + mode table), final values:")
    for addr in sorted(set(win) | set(lin)):
        a, b = win.get(addr), lin.get(addr)
        if a != b:
            fa = "-" if a is None else f"0x{a:02x}"
            fb = "-" if b is None else f"0x{b:02x}"
            print(f"  0x{addr:04x}: windows {fa:>5}  linux {fb:>5}")
    if len(sys.argv) == 4:
        rows = exposure_program(open(sys.argv[3], "rb").read())
        print("\nAE exposure program (record 261): min_us, 5 exposure times (us), 6 gains")
        for times, gains in rows:
            print("  " + " ".join(f"{t:7d}" for t in times) + "  |  " +
                  " ".join(f"{g:5.2f}" for g in gains))


if __name__ == "__main__":
    main()
