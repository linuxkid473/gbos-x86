#!/usr/bin/env python3
"""extract_build.py PARTS WORK [--product]

Unpack the pieces of a Google build that tools/port_build.py looks at, from the partition images in PARTS
(system_a.img, vendor_a.img, system_ext_a.img, [product_a.img]; see tools/fetch_google_parts.py) into WORK:

  WORK/system  WORK/vendor  WORK/system_ext  [WORK/product]     the erofs filesystems, extracted
  WORK/apex/<name>/                                              every APEX payload (erofs or ext4) up to 64 MB

Runs inside the gbos-erofs container (needs fsck.erofs for erofs; ext4 payloads such as Widevine are unpacked in Python);
tools/port_build.sh does that. Payloads over 64 MB (art, runtime, i18n) are listed and skipped.
"""
import io, os, shutil, struct, subprocess, sys, zipfile

EROFS_MAGIC = bytes.fromhex('e2e1f5e0')     # little-endian 0xE0F5E1E2 at offset 1024
EXT4_MAGIC = 0xEF53                         # little-endian u16 at offset 1024 + 0x38
BIG = 64 << 20


def erofs(img, dst):
    os.makedirs(dst, exist_ok=True)
    r = subprocess.run(['fsck.erofs', '--extract=' + dst, '--force', img], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('fsck.erofs failed on %s:\n%s' % (img, (r.stdout + r.stderr)[-600:]))


def ext4(img, dst):
    """Unpack regular files and symlinks from an ext4 APEX payload (supports multi-level extent trees)."""
    data = open(img, 'rb').read()
    bs = 1024 << struct.unpack_from('<I', data, 1024 + 0x18)[0]
    ipg = struct.unpack_from('<I', data, 1024 + 0x28)[0]
    isz = struct.unpack_from('<H', data, 1024 + 0x58)[0]
    incompat = struct.unpack_from('<I', data, 1024 + 0x60)[0]
    is64 = bool(incompat & 0x80)
    dsz = max(32, struct.unpack_from('<H', data, 1024 + 0xFE)[0] if is64 else 32)
    gdt = bs if bs > 1024 else 2048

    def get_inode(ino):
        grp, idx = divmod(ino - 1, ipg)
        gd = gdt + grp * dsz
        it = struct.unpack_from('<I', data, gd + 8)[0] | ((struct.unpack_from('<I', data, gd + 40)[0] << 32) if (is64 and dsz >= 64) else 0)
        off = it * bs + idx * isz
        return data[off:off + isz]

    def read_extents(node, out):
        magic, entries, _, depth, _ = struct.unpack_from('<HHHHI', node, 0)
        if magic != 0xF30A:
            sys.exit('ext4: bad extent magic %#x in %s' % (magic, img))
        if depth == 0:
            for i in range(entries):
                lblk, elen, hi, lo = struct.unpack_from('<IHHI', node, 12 + i * 12)
                uninit = elen > 32768
                out.append((lblk, elen - 32768 if uninit else elen, lo | (hi << 32), uninit))
        else:
            for i in range(entries):
                _, lo, hi, _ = struct.unpack_from('<IIHH', node, 12 + i * 12)
                pblk = lo | (hi << 32)
                read_extents(data[pblk * bs:(pblk + 1) * bs], out)

    def read_inode(inode):
        mode = struct.unpack_from('<H', inode, 0)[0]
        size = struct.unpack_from('<I', inode, 4)[0] | ((struct.unpack_from('<I', inode, 0x6C)[0] << 32) if (mode & 0xF000) == 0x8000 else 0)
        flags = struct.unpack_from('<I', inode, 0x20)[0]
        if (mode & 0xF000) == 0xA000 and size < 60 and not (flags & 0x80000):
            return mode, inode[0x28:0x28 + size]
        if not (flags & 0x80000):
            return mode, b''
        exts = []
        read_extents(inode[0x28:0x28 + 60], exts)
        exts.sort(key=lambda x: x[0])
        buf = bytearray(size)
        for lblk, length, pblk, uninit in exts:
            start = lblk * bs
            if start < size and not uninit:
                n = min(length * bs, size - start)
                buf[start:start + n] = data[pblk * bs:pblk * bs + n]
        return mode, bytes(buf)

    def walk(ino, outdir):
        os.makedirs(outdir, exist_ok=True)
        _, dbytes = read_inode(get_inode(ino))
        pos = 0
        while pos + 8 <= len(dbytes):
            cino, rlen, nlen, _ = struct.unpack_from('<IHBB', dbytes, pos)
            if rlen == 0:
                break
            name = dbytes[pos + 8:pos + 8 + nlen].decode('utf-8', 'replace')
            pos += rlen
            if cino == 0 or name in ('.', '..', 'lost+found'):
                continue
            cinode = get_inode(cino)
            cmode = struct.unpack_from('<H', cinode, 0)[0]
            kind = cmode & 0xF000
            target = os.path.join(outdir, name)
            if kind == 0x4000:
                walk(cino, target)
            elif kind == 0x8000:
                _, fbytes = read_inode(cinode)
                open(target, 'wb').write(fbytes)
                os.chmod(target, cmode & 0o777)
            elif kind == 0xA000:
                _, lbytes = read_inode(cinode)
                if os.path.lexists(target):
                    os.unlink(target)
                os.symlink(lbytes.decode('utf-8', 'replace'), target)

    walk(2, dst)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    parts, work = args
    names = ['system', 'vendor', 'system_ext'] + (['product'] if '--product' in sys.argv else [])
    os.makedirs(work, exist_ok=True)
    for n in names:
        img = os.path.join(parts, n + '_a.img')
        if not os.path.isfile(img):
            sys.exit('missing %s' % img)
        print('extracting %s ...' % n, flush=True)
        erofs(img, os.path.join(work, n))
    skipped = []
    done = 0
    tmp = os.path.join(work, 'payload.img')
    for n in names:
        d = os.path.join(work, n, 'system', 'apex') if n == 'system' and os.path.isdir(os.path.join(work, n, 'system')) else os.path.join(work, n, 'apex')
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith(('.apex', '.capex')):
                continue
            name = f.rsplit('.', 1)[0]
            try:
                z = zipfile.ZipFile(os.path.join(d, f))
                if f.endswith('.capex'):                      # compressed APEX: the real one is inside
                    z = zipfile.ZipFile(io.BytesIO(z.read('original_apex')))
                info = z.getinfo('apex_payload.img')
            except (KeyError, zipfile.BadZipFile) as e:
                skipped.append('%s (%s)' % (name, e))
                continue
            if info.file_size > BIG:
                skipped.append('%s (%d MB, over the size limit)' % (name, info.file_size >> 20))
                continue
            with z.open(info) as src, open(tmp, 'wb') as dst:
                shutil.copyfileobj(src, dst)
            with open(tmp, 'rb') as fh:
                fh.seek(1024)
                hdr = fh.read(64)
            if hdr[:4] == EROFS_MAGIC:
                erofs(tmp, os.path.join(work, 'apex', name))
                done += 1
            elif len(hdr) >= 0x3A and struct.unpack_from('<H', hdr, 0x38)[0] == EXT4_MAGIC:
                ext4(tmp, os.path.join(work, 'apex', name))
                done += 1
            else:
                skipped.append('%s (unknown fs)' % name)
    if os.path.exists(tmp):
        os.remove(tmp)
    print('extracted %d APEX payloads' % done)
    if skipped:
        print('skipped: ' + '; '.join(skipped))
    open(os.path.join(work, '.done'), 'w').write('ok\n')


if __name__ == '__main__':
    main()
