#!/usr/bin/env python3
"""make_super.py OUT_super.img LAYOUT.txt NAME=IMG... : minimal lpmake (liblp metadata v10.0).

Writes an Android dynamic-partition `super` image holding each IMG as a read-only
logical partition NAME (e.g. system_a=system_a.img), 1 MiB aligned, 2 metadata slots.
LAYOUT.txt gets one line per partition: "name start_sector num_sectors" (512-byte sectors).
"""
import hashlib, os, struct, sys

SECTOR, ALIGN, BLOCK = 512, 1 << 20, 4096
RESERVED, GEOM_SIZE = 4096, 4096
MAX_META, SLOTS = 65536, 2
GEOM_MAGIC, HDR_MAGIC = 0x616c4467, 0x414C5030
GROUP = 'google_dynamic_partitions_a'


def up(n, a):
    return (n + a - 1) // a * a


def name36(s):
    b = s.encode()
    assert len(b) < 36
    return b.ljust(36, b'\0')


def plan(parts):
    """parts: [(name, size)] in order -> (placed [(name, size, offset, aligned size)], total, geometry bytes, metadata bytes).
    The image is fully determined by the names and sizes: tools/super_parts.py and the updater app (SuperImage.java) rely on that."""
    meta_end = RESERVED + (GEOM_SIZE + MAX_META * SLOTS) * 2
    first_logical = up(meta_end, ALIGN)
    cur = first_logical
    placed = []
    for name, size in parts:
        size_al = up(size, BLOCK)
        placed.append((name, size, cur, size_al))
        cur = up(cur + size_al, ALIGN)
    total = up(cur + ALIGN, ALIGN)

    # tables
    ptab = b''.join(struct.pack('<36sIIII', name36(n), 1, i, 1, 1) for i, (n, *_r) in enumerate(placed))
    etab = b''.join(struct.pack('<QIQI', sz // SECTOR, 0, off // SECTOR, 0) for (_n, _s, off, sz) in placed)
    gtab = struct.pack('<36sIQ', name36('default'), 0, 0) + struct.pack('<36sIQ', name36(GROUP), 0, 0)
    btab = struct.pack('<QIIQ36sI', first_logical // SECTOR, ALIGN, 0, total, name36('super'), 0)
    tables = ptab + etab + gtab + btab
    assert len(ptab) == 52 * len(placed) and len(etab) == 24 * len(placed) and len(gtab) == 96 and len(btab) == 64

    def header():
        o = 0
        descs = []
        for t, esz in ((ptab, 52), (etab, 24), (gtab, 48), (btab, 64)):
            descs += [o, len(t) // esz, esz]
            o += len(t)
        body = lambda ck: struct.pack('<IHHI32sI32s12I', HDR_MAGIC, 10, 0, 128, ck, len(tables),
                                      hashlib.sha256(tables).digest(), *descs)
        h = body(b'\0' * 32)
        assert len(h) == 128
        return body(hashlib.sha256(h).digest())
    meta = header() + tables
    assert len(meta) <= MAX_META

    gbody = lambda ck: struct.pack('<II32sIII', GEOM_MAGIC, 52, ck, MAX_META, SLOTS, BLOCK)
    geom = gbody(hashlib.sha256(gbody(b'\0' * 32)).digest())
    return placed, total, geom, meta


def write_metadata(f, geom, meta):
    for g in (RESERVED, RESERVED + GEOM_SIZE):
        f.seek(g); f.write(geom)
    base = RESERVED + GEOM_SIZE * 2
    for i in range(SLOTS * 2):  # primary slots then backup slots
        f.seek(base + i * MAX_META); f.write(meta)


def main():
    out, layout_path, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
    imgs = {}
    parts = []
    for s in specs:
        name, img = s.split('=', 1)
        imgs[name] = img
        parts.append((name, os.path.getsize(img)))
    placed, total, geom, meta = plan(parts)

    with open(out, 'wb') as f:
        f.truncate(total)
        write_metadata(f, geom, meta)
        for name, size, off, _sz in placed:
            f.seek(off)
            with open(imgs[name], 'rb') as src:
                while True:
                    buf = src.read(8 << 20)
                    if not buf:
                        break
                    f.write(buf)
            print('%-16s %10d bytes at sector %d' % (name, size, off // SECTOR), flush=True)
    with open(layout_path, 'w') as lf:
        for name, _size, off, sz in placed:
            lf.write('%s %d %d\n' % (name, off // SECTOR, sz // SECTOR))
    print('super: %d bytes' % total)


if __name__ == '__main__':
    main()
