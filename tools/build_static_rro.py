#!/usr/bin/env python3
"""Build, sign and verify a value-only static resource overlay (RRO) APK without aapt2/apksigner.

Android only registers a preinstalled overlay that is signed: with targetSdkVersion >= 30 PackageManager wants an
APK Signature Scheme v2 block (it skips the content-digest check for system partitions, not the signature itself).
The unsigned aapt2 output used for the rotation overlay was never listed by `cmd overlay list android`.

  build_static_rro.py build  --package PKG --priority N --values VALUES.xml [--target android] [--key KEY.pem] OUT.apk
  build_static_rro.py verify APK [--package PKG --priority N --values VALUES.xml [--target android]]
  build_static_rro.py dump   APK

VALUES.xml is an Android res/values file with <bool> and <integer> items (the only kinds a value-only overlay
of framework config needs here). The manifest and resources.arsc are laid out exactly as aapt2 2.19 does
(verified byte for byte against aapt2's build of sensors/trogdor/display-policy). Entries are stored, with
resources.arsc 4-byte aligned. Without --key a throwaway RSA-2048 key signs the APK and is discarded: no key
is ever written. `verify` checks the v2 signature (signature, certificate/public-key match, content digest),
the zip layout, and with --values that the manifest and resources equal a fresh build of those sources.
Needs the Python `cryptography` package for build and verify.
"""
import argparse, hashlib, struct, sys, zipfile, zlib
import xml.etree.ElementTree as ET

ANDROID_NS = 'http://schemas.android.com/apk/res/android'
SDK = 37                 # Android 17, as Google's own overlays in this build declare
SDK_CODENAME = '17'
ATTR_ID = {
    'hasCode': 0x0101000c, 'priority': 0x0101001c, 'targetPackage': 0x01010021, 'minSdkVersion': 0x0101020c,
    'targetSdkVersion': 0x01010270, 'isStatic': 0x0101055a, 'compileSdkVersion': 0x01010572,
    'compileSdkVersionCodename': 0x01010573,
}
T_STRING, T_INT_DEC, T_INT_BOOLEAN = 0x03, 0x10, 0x12
TYPE_ORDER = ['bool', 'integer']          # aapt2 assigns type ids in its ResourceType order
V2_BLOCK_ID = 0x7109871a
SIG_RSA_PKCS1_SHA256 = 0x0103
MAGIC = b'APK Sig Block 42'


def u16(v): return struct.pack('<H', v)
def u32(v): return struct.pack('<I', v & 0xffffffff)
def pad4(b): return b + b'\0' * (-len(b) % 4)


# ---- resource values -------------------------------------------------------------------------------------------------
def parse_int(text):
    text = text.strip()
    return int(text, 16) if text.lower().startswith(('0x', '-0x')) else int(text)


def load_values(path):
    root = ET.parse(path).getroot()
    if root.tag != 'resources':
        raise SystemExit('%s: root element must be <resources>' % path)
    values = {}
    for el in root:
        name = el.get('name')
        if el.tag not in TYPE_ORDER or not name:
            raise SystemExit('%s: unsupported item <%s name=%r>' % (path, el.tag, name))
        text = (el.text or '').strip()
        if el.tag == 'bool':
            if text not in ('true', 'false'):
                raise SystemExit('%s: bool %s must be true or false' % (path, name))
            val = (T_INT_BOOLEAN, 0xffffffff if text == 'true' else 0)
        else:
            val = (T_INT_DEC, parse_int(text) & 0xffffffff)
        if name in values.setdefault(el.tag, {}):
            raise SystemExit('%s: duplicate %s/%s' % (path, el.tag, name))
        values[el.tag][name] = val
    if not values:
        raise SystemExit('%s: no values' % path)
    return values


def string_pool(strings, utf8):
    data, offs = b'', []
    for s in strings:
        offs.append(len(data))
        if utf8:
            enc = s.encode('utf-8')
            if len(s) > 0x7f or len(enc) > 0x7f:
                raise SystemExit('string too long for this writer: %r' % s)
            data += bytes([len(s), len(enc)]) + enc + b'\0'
        else:
            enc = s.encode('utf-16-le')
            if len(enc) // 2 > 0x7fff:
                raise SystemExit('string too long for this writer: %r' % s)
            data += u16(len(enc) // 2) + enc + b'\0\0'
    data = pad4(data)
    start = 28 + 4 * len(strings)
    hdr = u16(0x0001) + u16(28) + u32(start + len(data)) + u32(len(strings)) + u32(0) + \
        u32(0x100 if utf8 else 0) + u32(start) + u32(0)
    return hdr + b''.join(u32(o) for o in offs) + data


def build_arsc(package, values):
    types = [t for t in TYPE_ORDER if t in values]
    keys = [n for t in types for n in sorted(values[t])]
    type_pool = string_pool(types, utf8=False)
    key_pool = string_pool(keys, utf8=True)
    body = b''
    for tid, t in enumerate(types, 1):
        names = sorted(values[t])
        body += u16(0x0202) + u16(16) + u32(16 + 4 * len(names)) + bytes([tid, 0]) + u16(1) + \
            u32(len(names)) + b''.join(u32(0) for _ in names)
        header_size = 20 + 64
        entries = b''
        offsets = []
        for n in names:
            dtype, data = values[t][n]
            offsets.append(len(entries))
            entries += u16(8) + u16(0) + u32(keys.index(n)) + u16(8) + b'\0' + bytes([dtype]) + u32(data)
        start = header_size + 4 * len(names)
        body += u16(0x0201) + u16(header_size) + u32(start + len(entries)) + bytes([tid, 0]) + u16(0) + \
            u32(len(names)) + u32(start) + u32(64) + b'\0' * 60 + b''.join(u32(o) for o in offsets) + entries
    name = package.encode('utf-16-le')
    if len(name) > 254:
        raise SystemExit('package name too long')
    pkg_hdr_size = 288
    pkg = u16(0x0200) + u16(pkg_hdr_size) + u32(pkg_hdr_size + len(type_pool) + len(key_pool) + len(body)) + \
        u32(0x7f) + name + b'\0' * (256 - len(name)) + u32(pkg_hdr_size) + u32(0) + \
        u32(pkg_hdr_size + len(type_pool)) + u32(0) + u32(0) + type_pool + key_pool + body
    glob = string_pool([], utf8=True)
    return u16(0x0002) + u16(12) + u32(12 + len(glob) + len(pkg)) + u32(1) + glob + pkg


# ---- binary AndroidManifest.xml --------------------------------------------------------------------------------------
def manifest_tree(package, target, priority):
    """(tag, line, [(name, android_ns, kind, value)]) in document order; kind: int, str, bool."""
    return [
        ('manifest', 1, [('compileSdkVersion', True, 'int', SDK), ('compileSdkVersionCodename', True, 'str', SDK_CODENAME),
                         ('package', False, 'str', package), ('platformBuildVersionCode', False, 'int', SDK),
                         ('platformBuildVersionName', False, 'int', int(SDK_CODENAME))]),
        ('uses-sdk', 3, [('minSdkVersion', True, 'int', SDK), ('targetSdkVersion', True, 'int', SDK)]),
        ('application', 4, [('hasCode', True, 'bool', False)]),
        ('overlay', 5, [('priority', True, 'int', priority), ('targetPackage', True, 'str', target),
                        ('isStatic', True, 'bool', True)]),
    ]


def build_manifest(package, target, priority):
    tree = manifest_tree(package, target, priority)
    mapped = sorted({a[0] for _, _, attrs in tree for a in attrs if a[1]}, key=lambda n: ATTR_ID[n])
    others = set()
    for tag, _, attrs in tree:
        others.add(tag)
        for name, ns, kind, val in attrs:
            if not ns:
                others.add(name)
            if kind == 'str':
                others.add(val)
    others |= {'android', ANDROID_NS}
    pool = mapped + sorted(others - set(mapped))
    idx = {s: i for i, s in enumerate(pool)}
    NONE = 0xffffffff

    def node(ctype, line, payload):
        return u16(ctype) + u16(16) + u32(16 + len(payload)) + u32(line) + u32(NONE) + payload

    def attr(name, ns, kind, val):
        if kind == 'str':
            raw, dtype, data = idx[val], T_STRING, idx[val]
        elif kind == 'int':
            raw, dtype, data = NONE, T_INT_DEC, val
        else:
            raw, dtype, data = NONE, T_INT_BOOLEAN, 0xffffffff if val else 0
        return u32(idx[ANDROID_NS] if ns else NONE) + u32(idx[name]) + u32(raw) + u16(8) + b'\0' + \
            bytes([dtype]) + u32(data)

    def order(a):  # aapt2: attributes with a resource id first, by id; then the others by name
        return (0, ATTR_ID[a[0]], '') if a[1] else (1, 0, a[0])

    xml = node(0x0100, 1, u32(idx['android']) + u32(idx[ANDROID_NS]))
    for tag, line, attrs in tree:
        attrs = sorted(attrs, key=order)
        ext = u32(NONE) + u32(idx[tag]) + u16(20) + u16(20) + u16(len(attrs)) + u16(0) + u16(0) + u16(0)
        xml += node(0x0102, line, ext + b''.join(attr(*a) for a in attrs))
        if tag != 'manifest':
            xml += node(0x0103, line, u32(NONE) + u32(idx[tag]))
    xml += node(0x0103, 1, u32(NONE) + u32(idx['manifest']))
    xml += node(0x0101, 1, u32(idx['android']) + u32(idx[ANDROID_NS]))
    sp = string_pool(pool, utf8=False)
    rmap = u16(0x0180) + u16(8) + u32(8 + 4 * len(mapped)) + b''.join(u32(ATTR_ID[n]) for n in mapped)
    body = sp + rmap + xml
    return u16(0x0003) + u16(8) + u32(8 + len(body)) + body


# ---- zip (stored, aligned) -------------------------------------------------------------------------------------------
def build_zip(entries):
    out, cd = b'', b''
    for name, data in entries:
        fname = name.encode()
        off = len(out)
        crc = zlib.crc32(data)
        extra = b''
        if name == 'resources.arsc':  # 4-byte aligned data, using the zipalign/apksigner alignment extra field
            base = off + 30 + len(fname) + 6
            extra = u16(0xd935) + u16(2 + (-base % 4)) + u16(4) + b'\0' * (-base % 4)
        common = u16(0) + u16(0) + u16(0) + u16(0x0021) + u32(crc) + u32(len(data)) + u32(len(data)) + u16(len(fname))
        out += u32(0x04034b50) + u16(10) + common + u16(len(extra)) + fname + extra + data
        cd += u32(0x02014b50) + u16(10) + u16(10) + common + u16(0) + u16(0) + u16(0) + u16(0) + u32(0) + \
            u32(off) + fname
    eocd = u32(0x06054b50) + u16(0) + u16(0) + u16(len(entries)) + u16(len(entries)) + u32(len(cd)) + \
        u32(len(out)) + u16(0)
    return out, cd, eocd


# ---- APK Signature Scheme v2 -----------------------------------------------------------------------------------------
def lp(b): return u32(len(b)) + b


def content_digest(sections):
    chunks = []
    for s in sections:
        for i in range(0, len(s), 1 << 20):
            c = s[i:i + (1 << 20)]
            chunks.append(hashlib.sha256(b'\xa5' + u32(len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + u32(len(chunks)) + b''.join(chunks)).digest()


def new_key_and_cert(key_path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    import datetime
    if key_path:
        with open(key_path, 'rb') as f:
            key = serialization.load_pem_private_key(f.read(), None)
    else:
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'gbOS86 static resource overlay')])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc))
            .not_valid_after(datetime.datetime(2099, 12, 31, tzinfo=datetime.timezone.utc))
            .sign(key, hashes.SHA256()))
    return key, cert.public_bytes(serialization.Encoding.DER)


def sign_v2(entries_bytes, cd, eocd, key_path=None):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    key, cert_der = new_key_and_cert(key_path)
    digest = content_digest([entries_bytes, cd, eocd])
    signed_data = lp(lp(u32(SIG_RSA_PKCS1_SHA256) + lp(digest))) + lp(lp(cert_der)) + lp(b'')
    sig = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    pub = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = lp(signed_data) + lp(lp(u32(SIG_RSA_PKCS1_SHA256) + lp(sig))) + lp(pub)
    value = lp(lp(signer))
    pair = struct.pack('<Q', 4 + len(value)) + u32(V2_BLOCK_ID) + value
    size = len(pair) + 8 + 16
    block = struct.pack('<Q', size) + pair + struct.pack('<Q', size) + MAGIC
    new_eocd = eocd[:16] + u32(len(entries_bytes) + len(block)) + eocd[20:]
    return entries_bytes + block + cd + new_eocd


def build(package, target, priority, values_path, key_path=None):
    entries = [('AndroidManifest.xml', build_manifest(package, target, priority)),
               ('resources.arsc', build_arsc(package, load_values(values_path)))]
    return sign_v2(*build_zip(entries), key_path=key_path), dict(entries)


# ---- verification ----------------------------------------------------------------------------------------------------
class Bad(Exception):
    pass


def take(b, o):
    n = struct.unpack_from('<I', b, o)[0]
    if o + 4 + n > len(b):
        raise Bad('length prefix out of range')
    return b[o + 4:o + 4 + n], o + 4 + n


def seq(b):
    out, o = [], 0
    while o < len(b):
        item, o = take(b, o)
        out.append(item)
    return out


def verify_signature(apk):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    e = apk.rfind(b'PK\x05\x06')
    if e < 0 or len(apk) - e != 22:
        raise Bad('no end of central directory (or a zip comment)')
    cd_off = struct.unpack_from('<I', apk, e + 16)[0]
    if apk[cd_off - 16:cd_off] != MAGIC:
        raise Bad('no APK Signing Block (unsigned APK)')
    size = struct.unpack_from('<Q', apk, cd_off - 24)[0]
    start = cd_off - size - 8
    if start < 0 or struct.unpack_from('<Q', apk, start)[0] != size:
        raise Bad('APK Signing Block sizes disagree')
    o, v2 = start + 8, None
    while o < cd_off - 24:
        n, pid = struct.unpack_from('<QI', apk, o)
        if pid == V2_BLOCK_ID:
            v2 = apk[o + 12:o + 8 + n]
        o += 8 + n
    if v2 is None:
        raise Bad('no APK Signature Scheme v2 block')
    signers = seq(take(v2, 0)[0])
    if len(signers) != 1:
        raise Bad('expected one signer, found %d' % len(signers))
    signed_data, o = take(signers[0], 0)
    sigs, o = take(signers[0], o)
    pub, o = take(signers[0], o)
    key = serialization.load_der_public_key(pub)
    algs = {}
    for s in seq(sigs):
        algs[struct.unpack_from('<I', s, 0)[0]] = take(s, 4)[0]
    if SIG_RSA_PKCS1_SHA256 not in algs:
        raise Bad('no RSA PKCS#1 v1.5 SHA-256 signature')
    key.verify(algs[SIG_RSA_PKCS1_SHA256], signed_data, padding.PKCS1v15(), hashes.SHA256())
    digests_b, o = take(signed_data, 0)
    certs_b, o = take(signed_data, o)
    digests = {struct.unpack_from('<I', d, 0)[0]: take(d, 4)[0] for d in seq(digests_b)}
    if set(digests) != set(algs):
        raise Bad('digest and signature algorithms differ')
    cert = x509.load_der_x509_certificate(seq(certs_b)[0])
    der = serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo
    if cert.public_key().public_bytes(*der) != pub:
        raise Bad('certificate public key differs from the signer public key')
    eocd = apk[e:e + 16] + u32(start) + apk[e + 20:]
    if content_digest([apk[:start], apk[cd_off:e], eocd]) != digests[SIG_RSA_PKCS1_SHA256]:
        raise Bad('content digest mismatch (APK modified after signing)')
    return hashlib.sha256(seq(certs_b)[0]).hexdigest()


def verify(path, package=None, target='android', priority=None, values=None):
    apk = open(path, 'rb').read()
    cert = verify_signature(apk)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        if names != ['AndroidManifest.xml', 'resources.arsc']:
            raise Bad('unexpected entries %s' % names)
        info = z.getinfo('resources.arsc')
        if info.compress_type != zipfile.ZIP_STORED:
            raise Bad('resources.arsc is compressed')
        data_off = info.header_offset + 30 + len(info.filename) + struct.unpack_from('<H', apk, info.header_offset + 28)[0]
        if data_off % 4:
            raise Bad('resources.arsc is not 4-byte aligned')
        got = {n: z.read(n) for n in names}
    if values:
        want = {'AndroidManifest.xml': build_manifest(package, target, priority),
                'resources.arsc': build_arsc(package, load_values(values))}
        for n in names:
            if got[n] != want[n]:
                raise Bad('%s differs from a fresh build of %s' % (n, values))
    return cert


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('build', 'verify'):
        p = sub.add_parser(name)
        p.add_argument('--package'); p.add_argument('--priority', type=int); p.add_argument('--values')
        p.add_argument('--target', default='android')
        if name == 'build':
            p.add_argument('--key', help='PEM private key (default: a throwaway key)')
            p.add_argument('out')
        else:
            p.add_argument('apk')
    sub.add_parser('dump').add_argument('apk')
    a = ap.parse_args()
    if a.cmd in ('build', 'verify') and (a.values or a.cmd == 'build'):
        if not (a.package and a.priority is not None and a.values):
            ap.error('--package, --priority and --values are required')
    if a.cmd == 'build':
        apk, _ = build(a.package, a.target, a.priority, a.values, a.key)
        tmp = a.out + '.part'
        with open(tmp, 'wb') as f:
            f.write(apk)
        verify(tmp, a.package, a.target, a.priority, a.values)
        import os
        os.replace(tmp, a.out)
        print('built %s (%d bytes)' % (a.out, len(apk)))
    elif a.cmd == 'verify':
        try:
            cert = verify(a.apk, a.package, a.target, a.priority, a.values)
        except Bad as e:
            print('RRO VERIFY FAILED: %s: %s' % (a.apk, e))
            return 1
        except Exception as e:  # cryptography raises InvalidSignature without a message
            print('RRO VERIFY FAILED: %s: %s %s' % (a.apk, type(e).__name__, e))
            return 1
        print('RRO OK %s (v2 signature valid, signer certificate sha256 %s)' % (a.apk, cert))
    else:
        apk = open(a.apk, 'rb').read()
        try:
            print('signature: v2 valid, certificate sha256 %s' % verify_signature(apk))
        except Exception as e:
            print('signature: NOT valid (%s %s)' % (type(e).__name__, e))
        with zipfile.ZipFile(a.apk) as z:
            for i in z.infolist():
                print('entry %s %d bytes method %d' % (i.filename, i.file_size, i.compress_type))
    return 0


if __name__ == '__main__':
    sys.exit(main())
