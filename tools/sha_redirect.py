#!/usr/bin/env python3
"""Redirect post-Haswell BoringSSL assembly routines (SHA-NI and ADX) to Haswell-compatible code.

  sha_redirect.py IN OUT          patch one ELF; exit 0 if patched or nothing to do, 3 if unmatched
  sha_redirect.py --scan IN       report only

1. SHA-NI (`sha256_block_data_order_shaext`, `sha1_block_data_order_shaext`):
   5-byte `jmp` to their scalar siblings (`_nohw`, same ABI: rdi=state, rsi=data, rdx=nblocks).
2. BoringSSL Montgomery BIGNUM ADX (`x86_64-mont.S` / `x86_64-mont5.S`):
   Google compiles BoringSSL with `__BMI2__` + `__ADX__`, folding `bn_mulx_adx_capable()` to 1 so
   RSA/DH/BIGNUM unconditionally enters `bn_mulx4x_mont`, `bn_sqr8x_mont` (`__bn_sqrx8x_internal`),
   `bn_mulx4x_mont_gather5`, and `bn_powerx5` (100M+ #UD traps during keystore2 RSA keygen on Haswell).
   All four non-ADX siblings (`bn_mul_mont_nohw`, `__bn_sqr4x_mont`, `bn_mul_mont_gather5_nohw`,
   `bn_power5_nohw`) are still present in `libcrypto.so` with identical ABIs: redirect to them.
3. BoringSSL P-256 ADX (`p256-x86_64-asm.S`):
   `ecp_nistz256_mul_mont` and `ecp_nistz256_sqr_mont` have no `_nohw` sibling compiled in when
   `__ADX__` is enabled; replace `mul_mont` in-place (508 B <= 688 B) with constant-time pure-GPR
   BMI2 (`mulxq` + `adcq`) code and redirect `sqr_mont` (`mov %rsi,%rdx; jmp mul_mont`).
"""
import re, shutil, struct, subprocess, sys

OBJDUMP = '/usr/bin/objdump'
SHA_RE = re.compile(r'\bsha(1rnds4|1nexte|1msg1|1msg2|256rnds2|256msg1|256msg2)\b')
ADX_RE = re.compile(r'\b(adcx|adox)[lq]?\b')

FAMILIES = {
    # name: (shaext prologue, scalar prologue)
    '256': (re.compile(rb'\x48\x8d\x0d....\xf3\x0f\x6f\x0f\xf3\x0f\x6f\x57\x10', re.S),
            bytes.fromhex('4889e0' '53' '55' '4154' '4155' '4156' '4157' '48c1e204' '4883ec60' '488d1496' '4883e4c0')),
    '1': (re.compile(rb'\xf3\x0f\x6f\x07\x66\x0f\x6e\x4f\x10', re.S),
          bytes.fromhex('4889e0' '53' '55' '4154' '4155' '4156' '4989f8' '4883ec48' '4989f1' '4883e4c0' '4989d2')),
}

# x86_64-mont / x86_64-mont5: (name, adx_prologue, nohw_prologue)
MONT_REDIRECTS = [
    ('bn_mulx4x_mont',
     bytes.fromhex('4889e05355415441554156415741c1e1034d31d24d29ca4d8b004a8d6c14b848'),
     bytes.fromhex('4589c94889e05355415441554156415749f7d94989e34e8d54ccf049f7d94981')),
    ('bn_mulx4x_mont_gather5',
     bytes.fromhex('4889e05355415441554156415741c1e1034f8d144949f7d94d8b004e8d9c4cc0feffff4889e54929fb4981e3ff0f00004d39da720d4c29dd4a8dac4dc0feffff'),
     bytes.fromhex('4589c94889e0660f6e6c24085355415441554156415749f7d94989e34e8d94cc')),
    ('bn_powerx5',
     bytes.fromhex('4889e05355415441554156415741c1e1034f8d144949f7d94d8b004e8d9c4cc0feffff4889e54929fb4981e3ff0f00004d39da722b4c29dd4a8dac4dc0feffff'),
     bytes.fromhex('4889e05355415441554156415741c1e103478d144949f7d94d8b004e8d9c4cc0')),
]

# In bn_sqr8x_mont: `testq %rdx,%rdx; je +0x27 (__bn_sqr4x_mont); callq __bn_sqrx8x_internal`
# Change `74 27` (je) to `eb 27` (jmp) so bn_sqr8x_mont always uses non-ADX __bn_sqr4x_mont.
SQR8X_GATE_RE = re.compile(rb'\x48\x85\xd2\x74\x27\xe8....\x49\x8d\x1c\x08\x49\x89', re.S)

# ecp_nistz256_mul_mont (688 B) and ecp_nistz256_sqr_mont (634 B) prologues in p256-x86_64-asm.S
P256_MUL_PROLOGUE = bytes.fromhex('554889e54889d0488b1684c0c462f3f60048895c2480c4e2b3f658084c897424')
P256_SQR_PROLOGUE = bytes.fromhex('554889e5488b16c462fbf65618c4e2a3f6cac462bbf64e0848895c24804831db')
P256_SQR_END_TAIL = bytes.fromhex('4c8b7424984c8b7c24a05dc3')

# Constant-time pure-GPR Haswell BMI2 (`mulxq` + `adcq`, zero ADX/SSE/relocs) implementation of
# void ecp_nistz256_mul_mont(uint64_t res[4], const uint64_t a[4], const uint64_t b[4]), 508 bytes.
P256_MUL_NOHW_CODE = bytes.fromhex(
    '5541574156415541545348897c24c8488b0648894424d04c8b76084c8b7e104c8b6618488b02488b4a08'
    '488b7210488b521848894424d848894c24e048897424e848895424f04531ed4531d24531db4531c94531'
    'c031c9660f1f8400000000004a8b54ecd8c4e2fbf66c24d0c4c2cbf6fe4c01de4883d7004c01d04811ee'
    '4883d700c4c2e3f6ef4c01cb4883d5004801fb4883d5004989c349c1eb204989c2c4c2b3f6fc49c1e220'
    '4901f24911db4889c248b801000000ffffffffc4e2ebf6c04883d2004883d0004d01c14883d7004901e9'
    '4883d700488d34394901d14883d00031d24989c04901f00f92c24801cf4889d14883d10049ffc54983fd'
    '040f855dffffff31d24c89d04883c00141bd000000004983d5ff49c1fd3f4c89db4c01eb4983d50048bf'
    '01000000ffffffff4801fb4983d5ff49c1fd3f4d89ee4d01ce4983d50049c1fd3f4d89c74d01ef4983d5'
    '0041bcffffffff4d01e74983d5ff49c1ed3f4889ce4c29ee490f42c2490f42db4d0f42f14d0f42f8480f'
    '42f2480f43ca4809f14989c04983c0014883d2ff48c1fa3f4989d94901d14883d2004901f94883d2ff48'
    'c1fa3f4889d64c01f64883d20048c1fa3f4c89ff4801d74883d2004c01e74883d2ff48c1ea3f4839d14c'
    '0f42c04c0f42cb490f42f6490f42ff488b4424c84c89004c89480848897010488978185b415c415d415e'
    '415f5dc3'
)


def load_segments(d):
    phoff, = struct.unpack_from('<Q', d, 0x20)
    es, n = struct.unpack_from('<HH', d, 0x36)
    segs = []
    for i in range(n):
        t, fl, off, va, _, fs, ms, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * es)
        if t == 1:
            segs.append((off, va, fs, fl))
    return segs


def insn_addrs(path):
    out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', path], capture_output=True, text=True, errors='replace').stdout
    sha, adx = [], []
    for line in out.splitlines():
        if SHA_RE.search(line):
            m = re.match(r'\s*([0-9a-f]+):', line)
            if m:
                sha.append((int(m.group(1), 16), '1' if 'sha1' in line else '256'))
        elif ADX_RE.search(line):
            m = re.match(r'\s*([0-9a-f]+):', line)
            if m:
                adx.append(int(m.group(1), 16))
    return sha, adx


def plan(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        return d, [], [], [], 'not-elf64'
    segs = load_segments(d)
    sha, adx = insn_addrs(path)
    if not sha and not adx:
        return d, [], [], [], 'none'

    def v2o(va):
        for off, sva, fs, fl in segs:
            if sva <= va < sva + fs:
                return off + va - sva
        return None

    entries = {}
    unmatched = []
    for a, fam in sha:
        o = v2o(a)
        best = None
        for m in FAMILIES[fam][0].finditer(d, max(0, o - 0x1000), o):
            best = m.start()
        if best is None:
            unmatched.append(a)
        else:
            entries[best] = fam
    jmp_patches = []
    for eo in sorted(entries):
        lo = max(0, eo - 0x8000)
        so = d.rfind(FAMILIES[entries[eo]][1], lo, eo)
        if so < 0:
            unmatched.append(eo)
            continue
        jmp_patches.append((eo, so))

    raw_patches = []   # (offset, replacement_bytes)
    covered_adx_ranges = []

    if adx:
        # 1. Montgomery ADX redirects
        for name, adx_pro, nohw_pro in MONT_REDIRECTS:
            if d.count(adx_pro) != 1 or d.count(nohw_pro) != 1:
                unmatched.append(name)
                continue
            eo = d.index(adx_pro)
            so = d.index(nohw_pro)
            jmp_patches.append((eo, so))
            # bn_mulx4x_mont covers ~0x580 B; gather5 + powerx5 cover up to bn_scatter5 (+0x1400 B)
            span = 0x580 if name == 'bn_mulx4x_mont' else 0x1400
            covered_adx_ranges.append((eo, eo + span))

        gates = list(SQR8X_GATE_RE.finditer(d))
        if len(gates) != 1:
            unmatched.append('bn_sqr8x_gate')
        else:
            go = gates[0].start()
            raw_patches.append((go + 3, b'\xeb'))   # je +0x27 -> jmp +0x27

        # 2. P-256 Montgomery mul & sqr in-place replacement
        if d.count(P256_MUL_PROLOGUE) != 1 or d.count(P256_SQR_PROLOGUE) != 1:
            unmatched.append('ecp_nistz256')
        else:
            mo = d.index(P256_MUL_PROLOGUE)
            qo = d.index(P256_SQR_PROLOGUE)
            end_idx = d.find(P256_SQR_END_TAIL, qo, qo + 0x400)
            if qo <= mo or (qo - mo) < len(P256_MUL_NOHW_CODE) or end_idx < 0:
                unmatched.append('ecp_nistz256_size')
            else:
                mul_slot = qo - mo
                sqr_slot = (end_idx + len(P256_SQR_END_TAIL)) - qo
                mul_repl = P256_MUL_NOHW_CODE + b'\xcc' * (mul_slot - len(P256_MUL_NOHW_CODE))
                # sqr(res, a) -> movq %rsi, %rdx (48 89 f2); jmp mul(res, a, a)
                rel = mo - (qo + 8)
                sqr_code = b'\x48\x89\xf2\xe9' + struct.pack('<i', rel)
                sqr_repl = sqr_code + b'\xcc' * (sqr_slot - len(sqr_code))
                raw_patches.append((mo, mul_repl))
                raw_patches.append((qo, sqr_repl))
                covered_adx_ranges.append((mo, qo + sqr_slot))

        for a in adx:
            o = v2o(a)
            if o is None or not any(lo <= o < hi for lo, hi in covered_adx_ranges):
                unmatched.append(a)

    return d, jmp_patches, raw_patches, unmatched, 'ok'


def main():
    if sys.argv[1] == '--scan':
        _, jmps, raws, un, st = plan(sys.argv[2])
        print(st, 'jmps=%d raws=%d unmatched=%d' % (len(jmps), len(raws), len(un)), un if un else '')
        return 0
    src, dst = sys.argv[1:3]
    d, jmps, raws, un, st = plan(src)
    if st != 'ok':
        if src != dst:
            shutil.copyfile(src, dst)
        return 0
    d = bytearray(d)
    for eo, so in jmps:
        d[eo:eo + 5] = b'\xe9' + struct.pack('<i', so - (eo + 5))
    for off, repl in raws:
        d[off:off + len(repl)] = repl
    open(dst, 'wb').write(d)
    shutil.copymode(src, dst)
    print('%s: redirected %d function(s), %d inline patch(es)%s' %
          (src, len(jmps), len(raws), ', %d UNMATCHED: %s' % (len(un), un) if un else ''))
    return 3 if un else 0


sys.exit(main())
