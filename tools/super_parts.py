#!/usr/bin/env python3
"""super_parts.py SUPER.img SUPER.layout [--parts DIR]

For the update feed (tools/publish_system_update.sh): can a laptop rebuild this super.img from Google's own download? Prints

  google_part NAME BYTES SHA256    one per partition, in super.img's order; BYTES is the partition padded with zeros to 4 KiB,
                                   exactly as it sits in super.img
  google_super BYTES SHA256

after checking that super.img is byte for byte what usb/make_super.py makes from those partitions (metadata, zero gaps, size):
then the updater app (SuperImage.java, a port of make_super.py) rebuilds the same file from the partitions it streams out of
Google's recovery.zip. Exit 3 when it is not (super.img was made another way: the app cannot rebuild it).
--parts DIR: also check that DIR/NAME.img (Google's partitions as tools/fetch_google_parts.py wrote them, e.g. lapis/parts) are
those partitions. Exit 4 when one differs (super.img holds modified partitions: Google's download would not give them).
"""
import hashlib, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'usb'))
import make_super  # noqa: E402

CHUNK = 8 << 20


def die(code, msg):
    print('super_parts: ' + msg, file=sys.stderr)
    sys.exit(code)


def main():
    args = sys.argv[1:]
    parts_dir = None
    if '--parts' in args:
        i = args.index('--parts')
        parts_dir = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    sup, layout = args
    lay = []
    with open(layout) as f:
        for line in f:
            n, start, num = line.split()
            lay.append((n, int(start) * 512, int(num) * 512))
    if not lay or len(lay) > 6:
        die(3, 'layout has %d partitions' % len(lay))
    placed, total, geom, meta = make_super.plan([(n, size) for n, _o, size in lay])
    for (n, _s, off, sz), (ln, lo, lsz) in zip(placed, lay):
        if (n, off, sz) != (ln, lo, lsz):
            die(3, 'super.layout is not make_super.py\'s layout for these sizes (%s)' % ln)
    if os.path.getsize(sup) != total:
        die(3, 'super.img is %d bytes, make_super.py makes %d for these partitions' % (os.path.getsize(sup), total))
    first = placed[0][2]
    head = bytearray(first)
    for g in (make_super.RESERVED, make_super.RESERVED + make_super.GEOM_SIZE):
        head[g:g + len(geom)] = geom
    base = make_super.RESERVED + make_super.GEOM_SIZE * 2
    for i in range(make_super.SLOTS * 2):
        o = base + i * make_super.MAX_META
        head[o:o + len(meta)] = meta

    whole = hashlib.sha256()
    out = []
    with open(sup, 'rb') as f:
        def read(n):
            b = f.read(n)
            if len(b) != n:
                die(3, 'super.img ended early')
            whole.update(b)
            return b

        def zeros(n, what):
            while n:
                b = read(min(n, CHUNK))
                if b.count(0) != len(b):
                    die(3, 'super.img has data in %s, where make_super.py writes zeros' % what)
                n -= len(b)
        if read(first) != bytes(head):
            die(3, 'super.img\'s metadata is not make_super.py\'s for these partitions')
        pos = first
        for idx, (n, _s, off, sz) in enumerate(placed):
            zeros(off - pos, 'the gap before ' + n)
            h = hashlib.sha256()
            left = sz
            while left:
                b = read(min(left, CHUNK))
                h.update(b)
                left -= len(b)
            out.append((n, sz, h.hexdigest()))
            pos = off + sz
        zeros(total - pos, 'the end')

    if parts_dir:
        for n, sz, digest in out:
            p = os.path.join(parts_dir, n + '.img')
            if not os.path.isfile(p):
                die(4, 'no %s' % p)
            size = os.path.getsize(p)
            if make_super.up(size, make_super.BLOCK) != sz:
                die(4, '%s is %d bytes; super.img has room for %d' % (p, size, sz))
            h = hashlib.sha256()
            with open(p, 'rb') as f:
                while True:
                    b = f.read(CHUNK)
                    if not b:
                        break
                    h.update(b)
            h.update(bytes(sz - size))
            if h.hexdigest() != digest:
                die(4, '%s differs from the %s in super.img (super.img does not hold Google\'s partitions as downloaded)' % (p, n))

    for n, sz, digest in out:
        print('google_part %s %d %s' % (n, sz, digest))
    print('google_super %d %s' % (total, whole.hexdigest()))


if __name__ == '__main__':
    main()
