#!/usr/bin/env python3
"""updater_core_fixtures.py OUTDIR : test data for tools/test_updater_core.sh (no Google download needed).

Writes a small Google-like recovery zip: a raw disk image with a GPT whose "super" partition holds liblp metadata v10 with
awkward extents (a partition split in two, one part placed before the other on disk, a zero-fill extent, sizes that are not
4 KiB multiples, a partition the app must ignore), inside a zip64 deflated entry that is not the first entry. Also a stored
(uncompressed) copy, and a tiny Range-capable HTTP server (serve) that can drop a connection once to test resuming.
  updater_core_fixtures.py OUTDIR            make OUTDIR/google.zip, OUTDIR/google-stored.zip
  updater_core_fixtures.py serve DIR PORTFILE   serve DIR on 127.0.0.1 (port written to PORTFILE); /drop/NAME drops once
"""
import hashlib, os, random, struct, sys, zipfile

SECTOR = 512


def name36(s):
    return s.encode().ljust(36, b'\0')


def liblp_metadata(parts, extents):
    """parts: [(name, first_extent, num_extents)], extents: [(num_sectors, type, data, source)]"""
    ptab = b''.join(struct.pack('<36sIIII', name36(n), 1, f, k, 1) for n, f, k in parts)
    etab = b''.join(struct.pack('<QIQI', *e) for e in extents)
    gtab = struct.pack('<36sIQ', name36('default'), 0, 0) + struct.pack('<36sIQ', name36('google_dynamic_partitions_a'), 0, 0)
    btab = struct.pack('<QIIQ36sI', 2048, 1 << 20, 0, 64 << 20, name36('super'), 0)
    tables = ptab + etab + gtab + btab
    descs, o = [], 0
    for t, esz in ((ptab, 52), (etab, 24), (gtab, 48), (btab, 64)):
        descs += [o, len(t) // esz, esz]
        o += len(t)
    hdr = struct.pack('<IHHI32sI32s12I', 0x414C5030, 10, 2, 128, b'\0' * 32, len(tables), hashlib.sha256(tables).digest(), *descs)
    return hdr + tables


def make(outdir):
    super_start = 1 << 20                     # byte offset of super on the disk
    super_size = 24 << 20
    # random filler everywhere (so nothing is accidentally zero), the same every run
    disk = bytearray(random.Random(7).randbytes(super_start + super_size + (2 << 20)))
    # protective MBR area and GPT
    disk[0:512] = bytes(512)
    entries = bytearray(128 * 4)
    def gpt_entry(i, name, first, last):
        e = bytearray(128)
        e[0:16] = b'\x11' * 16                 # type guid (anything non-zero)
        e[16:32] = bytes([i + 1]) * 16
        struct.pack_into('<QQ', e, 32, first, last)
        nm = name.encode('utf-16-le')
        e[56:56 + len(nm)] = nm
        entries[i * 128:(i + 1) * 128] = e
    gpt_entry(0, 'STATE', 64, 1023)
    gpt_entry(1, 'super', super_start // SECTOR, (super_start + super_size) // SECTOR - 1)
    hdr = bytearray(92)
    hdr[0:8] = b'EFI PART'
    struct.pack_into('<QII', hdr, 72, 2, 4, 128)
    disk[512:512 + 92] = hdr
    disk[1024:1024 + len(entries)] = entries
    # liblp: geometry at super+4096, metadata slot 0 at super+3*4096
    geo = struct.pack('<II32sIII', 0x616C4467, 52, b'\0' * 32, 65536, 2, 4096)
    disk[super_start + 4096:super_start + 4096 + 4096] = geo.ljust(4096, b'\0')
    # partitions (sector numbers relative to super):
    #   system_a = [linear 3000 sectors at 9000] [zero 8 sectors] [linear 1999 sectors at 4096]   (second piece earlier on disk)
    #   vendor_a = [linear 17 sectors at 20000]                                                     (8704 bytes, not 4 KiB aligned)
    #   product_b = [linear 100 sectors at 30000]                                                   (not wanted)
    extents = [(3000, 0, 9000, 0), (8, 1, 0, 0), (1999, 0, 4096, 0), (17, 0, 20000, 0), (100, 0, 30000, 0)]
    parts = [('system_a', 0, 3), ('vendor_a', 3, 1), ('product_b', 4, 1)]
    meta = liblp_metadata(parts, extents)
    m0 = super_start + 3 * 4096
    disk[m0:m0 + 65536] = meta.ljust(65536, b'\0')
    os.makedirs(outdir, exist_ok=True)
    with zipfile.ZipFile(os.path.join(outdir, 'google.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('README.txt', 'not the disk image\n' * 50)
        with z.open('chromeos_test_recovery.bin', 'w', force_zip64=True) as f:
            f.write(bytes(disk))
    with zipfile.ZipFile(os.path.join(outdir, 'google-stored.zip'), 'w', zipfile.ZIP_STORED) as z:
        z.writestr('README.txt', 'small\n')
        z.writestr('disk.bin', bytes(disk))


def serve(root, portfile):
    import http.server, socketserver
    dropped = set()

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            path = self.path.split('?')[0]
            drop = path.startswith('/drop/')
            if drop:
                path = path[5:]
            fp = os.path.join(root, path.lstrip('/'))
            if '..' in path or not os.path.isfile(fp):
                self.send_error(404)
                return
            size = os.path.getsize(fp)
            etag = '"%s"' % hashlib.sha256(('%s %d %d' % (fp, size, os.path.getmtime(fp))).encode()).hexdigest()[:16]
            start, code = 0, 200
            rng = self.headers.get('Range')
            if rng and rng.startswith('bytes=') and rng.endswith('-'):
                ifr = self.headers.get('If-Range')
                if ifr is None or ifr == etag:
                    start, code = int(rng[6:-1]), 206
            if start >= size and code == 206:
                self.send_error(416)
                return
            self.send_response(code)
            self.send_header('Content-Length', str(size - start))
            self.send_header('ETag', etag)
            self.send_header('Accept-Ranges', 'bytes')
            if code == 206:
                self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, size - 1, size))
            self.end_headers()
            with open(fp, 'rb') as f:
                f.seek(start)
                sent = 0
                while True:
                    b = f.read(65536)
                    if not b:
                        break
                    if drop and fp not in dropped and sent > 300000:
                        dropped.add(fp)
                        self.close_connection = True
                        return
                    try:
                        self.wfile.write(b)
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    sent += len(b)

    class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
    s = S(('127.0.0.1', 0), H)
    with open(portfile + '.part', 'w') as f:
        f.write(str(s.server_address[1]))
    os.rename(portfile + '.part', portfile)
    s.serve_forever()


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'serve':
        serve(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2:
        make(sys.argv[1])
    else:
        sys.exit(__doc__)
