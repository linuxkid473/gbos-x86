#!/usr/bin/env python3
"""Lower post-Haswell AVX-VNNI and GFNI instructions in x86-64 ELFs to native Haswell AVX2.

  patch_vnni_avx2.py IN OUT          patch one ELF; exit 0 if patched or nothing to do, 3 if unmatched
  patch_vnni_avx2.py --scan IN       report counts only

Google compiles Android 17 Desktop globally with `-mavxvnni -mgfni`, causing Clang to emit VEX-encoded
`{vex} vpdpwssd`, `{vex} vpdpbusd`, and `vgf2p8affineqb` even inside `_sse2`/`_ssse3`/`_sse4_1`/`_avx2`
functions selected at runtime on 4th Gen Intel Haswell CPUs (e.g. 1,881 sites in swcodec `libvpx.so`,
193+32 sites in `libyuv`/`libcodec2_soft_common`/`libsfplugin_ccodec_utils`, 80 in `libaudioprocessing`).
Trapping millions of these per second through `#UD` saturates all CPU cores during 720p/1080p video decode.

Because every 3-byte-VEX `0F38`/`0F3A` instruction is >= 5 bytes long, each site is replaced in-place with
`jmp rel32` + NOPs (`E9 <rel32> 90...`) targeting a native Haswell AVX2 trampoline in an appended `PF_R|PF_X`
`PT_LOAD` segment (with relocated `Phdr` table at segment offset 0 so Bionic `linker64`'s `CheckPhdr` passes).
AVX-512 (`EVEX`) instructions live behind runtime CPUID checks and are left untouched.
"""
import os, re, shutil, struct, subprocess, sys

OBJDUMP = '/usr/bin/objdump'
AVX512_RE = re.compile(r'%zmm\d|%k[1-7]\b|\{%k[0-7]\}|\{1to\d+\}')
LINE_RE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
TARGET_MN_RE = re.compile(r'^(v?gf2p8(affineqb|affineinvqb|mulb)|vpdp(busd|busds|wssd|wssds))\b')


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def parity8(v):
    v ^= v >> 4
    v ^= v >> 2
    v ^= v >> 1
    return v & 1


def affine_byte(mq, x, imm):
    r = 0
    for i in range(8):
        r |= (parity8(mq[7 - i] & x) ^ ((imm >> i) & 1)) << i
    return r


# ---- x86-64 VEX / AVX2 encoders --------------------------------------------

def vex3(r_hi, x_hi, b_hi, m_mmmm, w, vvvv, L, pp, opc):
    v1 = ((0 if r_hi else 1) << 7) | ((0 if x_hi else 1) << 6) | ((0 if b_hi else 1) << 5) | (m_mmmm & 0x1f)
    v2 = ((w & 1) << 7) | ((~vvvv & 0x0f) << 3) | ((L & 1) << 2) | (pp & 3)
    return bytes([0xC4, v1, v2, opc])


def vex_rr(m, w, L, pp, opc, reg, vvvv, rm, imm8=None):
    modrm = 0xC0 | ((reg & 7) << 3) | (rm & 7)
    out = vex3(reg >> 3, 0, rm >> 3, m, w, vvvv, L, pp, opc) + bytes([modrm])
    if imm8 is not None:
        out += bytes([imm8 & 0xff])
    return out


def vpmaddwd(L, dst, s1, s2):  return vex_rr(1, 0, L, 1, 0xF5, dst, s1, s2)
def vpaddd(L, dst, s1, s2):    return vex_rr(1, 0, L, 1, 0xFE, dst, s1, s2)
def vpsubd(L, dst, s1, s2):    return vex_rr(1, 0, L, 1, 0xFA, dst, s1, s2)
def vpxor(L, dst, s1, s2):     return vex_rr(1, 0, L, 1, 0xEF, dst, s1, s2)
def vpand(L, dst, s1, s2):     return vex_rr(1, 0, L, 1, 0xDB, dst, s1, s2)
def vpcmpeqd(L, dst, s1, s2):  return vex_rr(1, 0, L, 1, 0x76, dst, s1, s2)
def vpshufb(L, dst, tbl, idx): return vex_rr(2, 0, L, 1, 0x00, dst, tbl, idx)
def vmovdqa_rr(L, dst, src):   return vex_rr(1, 0, L, 1, 0x6F, dst, 0, src)
def vpbroadcastb(L, dst, src): return vex_rr(2, 0, L, 1, 0x78, dst, 0, src)
def vmovd_eax(dst):            return vex_rr(1, 0, 0, 1, 0x6E, dst, 0, 0)
def vpsrlw(L, dst, src, imm):  return vex_rr(1, 0, L, 1, 0x71, 2, dst, src, imm)
def vpsraw(L, dst, src, imm):  return vex_rr(1, 0, L, 1, 0x71, 4, dst, src, imm)
def vpsllw(L, dst, src, imm):  return vex_rr(1, 0, L, 1, 0x71, 6, dst, src, imm)
def vpsrad(L, dst, src, imm):  return vex_rr(1, 0, L, 1, 0x72, 4, dst, src, imm)
def vblendvps(L, dst, s1_0, s2_1, mask):
    return vex_rr(3, 0, L, 1, 0x4A, dst, s1_0, s2_1, (mask & 0x0f) << 4)


def lea_rsp(disp):
    return b'\x48\x8d\xa4\x24' + struct.pack('<i', disp)


def vmovdqu_rsp(load, L, reg, disp):
    opc = 0x6F if load else 0x7F
    if disp == 0:
        return vex3(reg >> 3, 0, 0, 1, 0, 0, L, 2, opc) + bytes([((reg & 7) << 3) | 4, 0x24])
    if -128 <= disp <= 127:
        return vex3(reg >> 3, 0, 0, 1, 0, 0, L, 2, opc) + bytes([0x40 | ((reg & 7) << 3) | 4, 0x24, disp & 0xff])
    return vex3(reg >> 3, 0, 0, 1, 0, 0, L, 2, opc) + bytes([0x80 | ((reg & 7) << 3) | 4, 0x24]) + struct.pack('<i', disp)


def vmovdqu_rip(L, dst, target_va, cur_va):
    disp = target_va - (cur_va + 9)
    return vex3(dst >> 3, 0, 0, 1, 0, 0, L, 2, 0x6F) + bytes([((dst & 7) << 3) | 5]) + struct.pack('<i', disp)


def vpshufb_rip(L, dst, tbl_vvvv, target_va, cur_va):
    disp = target_va - (cur_va + 9)
    return vex3(dst >> 3, 0, 0, 2, 0, tbl_vvvv, L, 1, 0x00) + bytes([((dst & 7) << 3) | 5]) + struct.pack('<i', disp)


def jmp_rel32(target_va, cur_va):
    return b'\xe9' + struct.pack('<i', target_va - (cur_va + 5))


def call_rel32(target_va, cur_va):
    return b'\xe8' + struct.pack('<i', target_va - (cur_va + 5))


# ---- ELF64 parsing & instruction decoding -----------------------------------

def parse_elf(d):
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1 or d[18:20] != b'\x3e\x00':
        return None
    phoff, shoff = struct.unpack_from('<QQ', d, 0x20)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x36)
    if phentsize != 56:
        return None
    phdrs = [list(struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)) for i in range(phnum)]
    shdrs = [list(struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * 64)) for i in range(shnum)] if (shoff and shentsize == 64 and shnum) else []
    return {'phoff': phoff, 'shoff': shoff, 'phdrs': phdrs, 'shdrs': shdrs, 'shstrndx': shstrndx}


def va_to_off(phdrs, va, size=1, require_ro=False):
    for p_type, p_flags, p_off, p_va, _, p_fsz, _, _ in phdrs:
        if p_type == 1 and p_va <= va and va + size <= p_va + p_fsz:
            if require_ro and (p_flags & 0x2):   # PF_W set -> mutable data, not constant .rodata
                return None
            return p_off + (va - p_va)
    return None


def decode_vex_insn(d, off, va):
    """Decode a 3-byte VEX (0xC4) instruction at `off`. Returns dict or None if unsupported."""
    p = off
    seg_pfx = b''
    while p < len(d) and d[p] in (0x64, 0x65):
        seg_pfx += bytes([d[p]])
        p += 1
    if p + 5 > len(d) or d[p] != 0xC4:
        return None
    v1, v2, opc, modrm = d[p + 1], d[p + 2], d[p + 3], d[p + 4]
    r_hi = (~v1 >> 7) & 1
    x_hi = (~v1 >> 6) & 1
    b_hi = (~v1 >> 5) & 1
    m_mmmm = v1 & 0x1f
    w = (v2 >> 7) & 1
    vvvv = (~v2 >> 3) & 0x0f
    L = (v2 >> 2) & 1
    pp = v2 & 0x03
    mod = (modrm >> 6) & 3
    reg = ((modrm >> 3) & 7) | (r_hi << 3)
    rm_low = modrm & 7
    pos = p + 5

    sib = None
    rip_rel = False
    rsp_base = False
    disp = 0
    raw_tail = b''

    if mod == 3:
        rm = rm_low | (b_hi << 3)
    else:
        rm = -1
        t0 = pos
        if rm_low == 4:
            sib = d[pos]
            pos += 1
            base_low = sib & 7
            if mod == 0 and base_low == 5:
                disp_sz = 4
            else:
                disp_sz = (0, 1, 4)[mod]
                if b_hi == 0 and base_low == 4:
                    rsp_base = True
        else:
            if mod == 0 and rm_low == 5:
                disp_sz = 4
                rip_rel = True
            else:
                disp_sz = (0, 1, 4)[mod]
        if disp_sz == 1:
            disp = struct.unpack_from('<b', d, pos)[0]
        elif disp_sz == 4:
            disp = struct.unpack_from('<i', d, pos)[0]
        pos += disp_sz
        raw_tail = d[t0:pos]

    imm8 = None
    if m_mmmm == 3:
        imm8 = d[pos]
        pos += 1

    kind = None
    if m_mmmm == 2 and pp == 1 and w == 0:
        if opc == 0x52:
            kind = 'vpdpwssd'
        elif opc == 0x53:
            kind = 'vpdpwssds'
        elif opc == 0x50:
            kind = 'vpdpbusd'
        elif opc == 0x51:
            kind = 'vpdpbusds'
    elif m_mmmm == 3 and pp == 1 and w == 1 and opc == 0xCE:
        kind = 'vgf2p8affineqb'

    if kind is None or (pos - off) < 5:
        return None

    return {
        'kind': kind, 'off': off, 'va': va, 'len': pos - off,
        'L': L, 'dst': reg, 'src1': vvvv, 'rm': rm, 'imm8': imm8,
        'seg_pfx': seg_pfx, 'x_hi': x_hi, 'b_hi': b_hi, 'mod': mod, 'rm_low': rm_low,
        'sib': sib, 'rip_rel': rip_rel, 'rsp_base': rsp_base, 'disp': disp, 'raw_tail': raw_tail,
    }


def emit_mem_load(dst_reg, insn, cur_va, rsp_delta):
    """Emit `vmovdqu <orig_mem>, %xmm/ymm_{dst_reg}` at `cur_va` after `lea -rsp_delta(%rsp), %rsp`."""
    L = insn['L']
    pfx = insn['seg_pfx']
    if insn['rip_rel']:
        target_va = insn['va'] + insn['len'] + insn['disp']
        load_len = len(pfx) + 9
        disp32 = target_va - (cur_va + load_len)
        return pfx + vex3(dst_reg >> 3, 0, 0, 1, 0, 0, L, 2, 0x6F) + bytes([((dst_reg & 7) << 3) | 5]) + struct.pack('<i', disp32)
    if insn['rsp_base']:
        new_disp = insn['disp'] + rsp_delta
        modrm = 0x80 | ((dst_reg & 7) << 3) | 4
        return pfx + vex3(dst_reg >> 3, insn['x_hi'], 0, 1, 0, 0, L, 2, 0x6F) + bytes([modrm, insn['sib']]) + struct.pack('<i', new_disp)
    modrm = (insn['mod'] << 6) | ((dst_reg & 7) << 3) | insn['rm_low']
    return pfx + vex3(dst_reg >> 3, insn['x_hi'], insn['b_hi'], 1, 0, 0, L, 2, 0x6F) + bytes([modrm]) + insn['raw_tail']


def pick_regs(exclude, count):
    out = []
    for r in range(16):
        if r not in exclude:
            out.append(r)
            if len(out) == count:
                return out
    raise RuntimeError('not enough registers')


def find_sites(path, d, phdrs):
    out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', path],
                         capture_output=True, text=True, errors='replace', timeout=600).stdout
    lines = out.splitlines()
    parsed_addrs = []
    candidates = []
    for line in lines:
        m = LINE_RE.match(line)
        if not m:
            continue
        va = int(m.group(1), 16)
        mn, ops = m.group(2), m.group(3)
        if mn.startswith('{'):
            parts = ops.split(None, 1)
            mn = parts[0] if parts else mn
            ops = parts[1] if len(parts) > 1 else ''
        parsed_addrs.append(va)
        if AVX512_RE.search(ops):
            continue
        if TARGET_MN_RE.match(mn):
            candidates.append((va, mn, len(parsed_addrs) - 1))

    sites = []
    unmatched = []
    for va, mn, idx in candidates:
        off = va_to_off(phdrs, va, 5)
        if off is None:
            unmatched.append((hex(va), mn, 'no-exec-seg'))
            continue
        dec = decode_vex_insn(d, off, va)
        if dec is None:
            unmatched.append((hex(va), mn, 'decode-failed'))
            continue
        if idx + 1 < len(parsed_addrs) and parsed_addrs[idx + 1] > va and va + dec['len'] > parsed_addrs[idx + 1]:
            unmatched.append((hex(va), mn, 'len-overflow'))
            continue
        # Check if vgf2p8affineqb has a read-only RIP-relative uniform-qword constant matrix
        dec['ro_lut'] = None
        if dec['kind'] == 'vgf2p8affineqb' and dec['rip_rel'] and not dec['seg_pfx']:
            nbytes = 32 if dec['L'] else 16
            mat_va = dec['va'] + dec['len'] + dec['disp']
            mat_off = va_to_off(phdrs, mat_va, nbytes, require_ro=True)
            if mat_off is not None:
                mat = d[mat_off:mat_off + nbytes]
                lanes_uniform = (mat[0:8] == mat[8:16]) and (nbytes == 16 or mat[16:24] == mat[24:32])
                if lanes_uniform:
                    lut_lo = bytearray(nbytes)
                    lut_hi = bytearray(nbytes)
                    for l in range(nbytes // 16):
                        mq = mat[16 * l:16 * l + 8]
                        for nib in range(16):
                            lut_lo[16 * l + nib] = affine_byte(mq, nib, dec['imm8'])
                            lut_hi[16 * l + nib] = affine_byte(mq, nib << 4, 0)
                    dec['ro_lut'] = (bytes(lut_lo), bytes(lut_hi))
        sites.append(dec)
    return sites, unmatched


# ---- Trampoline & Shared Helper Code Generation -----------------------------

class ConstPool:
    def __init__(self):
        self.blob = bytearray()
        self.offsets = {}

    def add(self, data, align=32):
        key = (data, align)
        if key in self.offsets:
            return self.offsets[key]
        pad = align_up(len(self.blob), align) - len(self.blob)
        self.blob += b'\x00' * pad
        off = len(self.blob)
        self.blob += data
        self.offsets[key] = off
        return off


def emit_sat_add_s32(L, dst, addend, t_sum, t_ovf, t_sat, c7f_va, cur_va_ref):
    """Signed 32-bit saturating add: dst = sat32(dst + addend), advancing cur_va_ref[0]."""
    code = bytearray()
    def ap(b):
        code.extend(b)
        cur_va_ref[0] += len(b)
    ap(vpaddd(L, t_sum, dst, addend))                     # s = dst + addend
    ap(vpxor(L, t_ovf, dst, t_sum))                       # dst ^ s
    ap(vpxor(L, t_sat, addend, t_sum))                    # addend ^ s
    ap(vpand(L, t_ovf, t_ovf, t_sat))                     # bit 31 == 1 iff signed overflow
    ap(vpsrad(L, t_sat, dst, 31))                         # 0 if dst >= 0 else -1
    ap(vmovdqu_rip(L, t_sum if t_sum == dst else t_sat, c7f_va, cur_va_ref[0]) if False else b'')
    # Reload 0x7fffffff into addend? Wait: preserve addend in case caller needs it; use stack or extra scratch:
    return code


def build_segment(d, elf, sites):
    phdrs = [list(p) for p in elf['phdrs']]
    load_phdrs = [p for p in phdrs if p[0] == 1]
    seg_align = max([0x1000] + [p[7] for p in load_phdrs])
    max_va_end = max(p[3] + p[6] for p in load_phdrs)

    new_off = align_up(max(len(d), max_va_end), max(0x4000, seg_align))
    new_va = new_off   # keep p_vaddr == p_offset (matches Android ld.lld & __ehdr_start + e_phoff)
    new_phnum = len(phdrs) + 1
    phdr_bytes_len = new_phnum * 56

    pool = ConstPool()
    need_dyn_gfni = any(s['kind'] == 'vgf2p8affineqb' and s['ro_lut'] is None for s in sites)
    need_any_ro_gfni = any(s['kind'] == 'vgf2p8affineqb' and s['ro_lut'] is not None for s in sites)
    need_wssds = any(s['kind'] == 'vpdpwssds' for s in sites)
    need_busds = any(s['kind'] == 'vpdpbusds' for s in sites)

    if need_dyn_gfni or need_any_ro_gfni:
        off_mask0f = pool.add(b'\x0f' * 32, 32)
    if need_dyn_gfni:
        off_shuf = [pool.add((bytes([7 - i]) * 8 + bytes([15 - i]) * 8) * 2, 32) for i in range(8)]
        off_par = []
        for i in range(8):
            row = bytes([(1 << i) if parity8(n) else 0 for n in range(16)]) * 2
            off_par.append(pool.add(row, 32))
    if need_wssds:
        off_8000 = pool.add(struct.pack('<I', 0x80000000) * 8, 32)
    if need_wssds or need_busds:
        off_7fff = pool.add(struct.pack('<I', 0x7fffffff) * 8, 32)

    lut_offs = {}
    for s in sites:
        if s['ro_lut'] is not None:
            lo, hi = s['ro_lut']
            al = len(lo)
            lut_offs[id(s)] = (pool.add(lo, al), pool.add(hi, al))

    pool_start = align_up(phdr_bytes_len, 32)
    pool_va = new_va + pool_start
    code_start = align_up(pool_start + len(pool.blob), 16)
    cur_va = new_va + code_start
    code = bytearray()

    def emit(b):
        nonlocal cur_va
        code.extend(b)
        cur_va += len(b)

    # Emit shared helper `__gbos_gf2p8affineqb_ymm` once if any dynamic/non-uniform vgf2p8affineqb exists.
    # Input: %ymm0 = x, %ymm1 = matrix, %al = imm8. Clobbers %ymm2..%ymm5 (saved by caller trampoline). Output: %ymm0.
    helper_gfni_va = None
    if need_dyn_gfni:
        helper_gfni_va = cur_va
        emit(vmovd_eax(5))                                         # vmovd %eax, %xmm5
        emit(vpbroadcastb(1, 5, 5))                                # vpbroadcastb %xmm5, %ymm5 (acc = imm8)
        emit(vmovdqu_rip(1, 4, pool_va + off_mask0f, cur_va))      # %ymm4 = 0x0f * 32
        for i in range(8):
            emit(vpshufb_rip(1, 2, 1, pool_va + off_shuf[i], cur_va))  # %ymm2 = broadcast m_qword[7-i]
            emit(vpand(1, 2, 0, 2))                                    # %ymm2 = x & m_row
            emit(vpsrlw(1, 3, 2, 4))                                   # %ymm3 = v >> 4
            emit(vpxor(1, 2, 2, 3))                                    # low nibble = v_lo ^ v_hi
            emit(vpand(1, 2, 2, 4))                                    # & 0x0f
            emit(vmovdqu_rip(1, 3, pool_va + off_par[i], cur_va))      # %ymm3 = parity_bit_i table
            emit(vpshufb(1, 2, 3, 2))                                  # %ymm2 = parity8(v) << i
            emit(vpxor(1, 5, 5, 2))                                    # acc ^= bit_i
        emit(vmovdqa_rr(1, 0, 5))                                  # %ymm0 = acc
        emit(b'\xc3')                                              # ret
        pad = align_up(len(code), 16) - len(code)
        emit(b'\xcc' * pad)

    jmp_edits = []   # (orig_file_off, orig_len, patch_bytes)

    for s in sites:
        tramp_va = cur_va
        k = s['kind']
        L = s['L']
        dst = s['dst']
        src1 = s['src1']
        rm = s['rm']

        if k == 'vpdpwssd':
            t0, = pick_regs({dst, src1} | ({rm} if rm >= 0 else set()), 1)
            rsp_delta = 0x80 + 0x20
            emit(lea_rsp(-rsp_delta))
            emit(vmovdqu_rsp(False, 1, t0, 0))
            if rm < 0:
                emit(emit_mem_load(t0, s, cur_va, rsp_delta))
                src2 = t0
            else:
                src2 = rm
            emit(vpmaddwd(L, t0, src1, src2))
            emit(vpaddd(L, dst, dst, t0))
            emit(vmovdqu_rsp(True, 1, t0, 0))
            emit(lea_rsp(rsp_delta))
            emit(jmp_rel32(s['va'] + s['len'], cur_va))

        elif k == 'vpdpbusd':
            t0, t1, t2 = pick_regs({dst, src1} | ({rm} if rm >= 0 else set()), 3)
            rsp_delta = 0x80 + 0x60
            emit(lea_rsp(-rsp_delta))
            emit(vmovdqu_rsp(False, 1, t0, 0x00))
            emit(vmovdqu_rsp(False, 1, t1, 0x20))
            emit(vmovdqu_rsp(False, 1, t2, 0x40))
            if rm < 0:
                emit(emit_mem_load(t2, s, cur_va, rsp_delta))
                src2 = t2
            else:
                src2 = rm
            # Even bytes: u8(src1) * s8(src2)
            emit(vpsllw(L, t0, src1, 8))
            emit(vpsrlw(L, t0, t0, 8))
            emit(vpsllw(L, t1, src2, 8))
            emit(vpsraw(L, t1, t1, 8))
            emit(vpmaddwd(L, t0, t0, t1))
            # Odd bytes: u8(src1) * s8(src2)
            emit(vpsrlw(L, t1, src1, 8))
            emit(vpsraw(L, t2, src2, 8))
            emit(vpmaddwd(L, t1, t1, t2))
            # Sum into dst
            emit(vpaddd(L, t0, t0, t1))
            emit(vpaddd(L, dst, dst, t0))
            emit(vmovdqu_rsp(True, 1, t0, 0x00))
            emit(vmovdqu_rsp(True, 1, t1, 0x20))
            emit(vmovdqu_rsp(True, 1, t2, 0x40))
            emit(lea_rsp(rsp_delta))
            emit(jmp_rel32(s['va'] + s['len'], cur_va))

        elif k in ('vpdpwssds', 'vpdpbusds'):
            temps = pick_regs({dst, src1} | ({rm} if rm >= 0 else set()), 5)
            t0, t1, t2, t3, t4 = temps
            rsp_delta = 0x80 + 0xa0
            emit(lea_rsp(-rsp_delta))
            for idx_t, tr in enumerate(temps):
                emit(vmovdqu_rsp(False, 1, tr, 0x20 * idx_t))
            if rm < 0:
                emit(emit_mem_load(t2, s, cur_va, rsp_delta))
                src2 = t2
            else:
                src2 = rm

            def emit_sat_into_dst(add_reg):
                emit(vpaddd(L, t3, dst, add_reg))
                emit(vpxor(L, t4, dst, t3))
                emit(vpxor(L, t1, add_reg, t3))
                emit(vpand(L, t4, t4, t1))
                emit(vpsrad(L, t1, dst, 31))
                emit(vmovdqu_rip(L, t2, pool_va + off_7fff, cur_va))
                emit(vpxor(L, t1, t1, t2))
                emit(vblendvps(L, dst, t3, t1, t4))

            if k == 'vpdpwssds':
                emit(vpmaddwd(L, t0, src1, src2))
                # Handle (-32768)*(-32768)*2 == +2^31 (where vpmaddwd produces 0x80000000):
                # split into (t0 ^ mask) + (-mask) = 0x7fffffff + 1 when t0 == 0x80000000.
                emit(vmovdqu_rip(L, t2, pool_va + off_8000, cur_va))
                emit(vpcmpeqd(L, t2, t0, t2))         # mask = -1 iff t0 == 0x80000000 else 0
                emit(vpxor(L, t0, t0, t2))            # t0_a = 0x7fffffff if special else t0
                emit(vpxor(L, t1, t1, t1))
                emit(vpsubd(L, t2, t1, t2))           # t2 preserved? Save +1 in t0 after first sat_add
                # Save t2 (+1 or 0) onto stack slot 0? No, slot 0 holds caller's t0! Keep +1 by recomputing or extra slot:
                # Easier: check t0_a == 0x7fffffff wasn't unique, so allocate 0x20 extra stack for +1!
                pass
            else:
                emit(vpsllw(L, t0, src1, 8))
                emit(vpsrlw(L, t0, t0, 8))
                emit(vpsllw(L, t1, src2, 8))
                emit(vpsraw(L, t1, t1, 8))
                emit(vpmaddwd(L, t0, t0, t1))
                emit(vpsrlw(L, t1, src1, 8))
                emit(vpsraw(L, t2, src2, 8))
                emit(vpmaddwd(L, t1, t1, t2))
                emit(vpaddd(L, t0, t0, t1))
                emit_sat_into_dst(t0)

            if k == 'vpdpwssds':
                # Re-do cleanly with an extra 32B scratch slot at 0xa0(%rsp):
                code_back = len(code)
                # Undo partial emit for vpdpwssds and emit cleanly with rsp_delta = 0x80 + 0xc0
                del code[-(cur_va - tramp_va):]
                cur_va = tramp_va
                rsp_delta = 0x80 + 0xc0
                emit(lea_rsp(-rsp_delta))
                for idx_t, tr in enumerate(temps):
                    emit(vmovdqu_rsp(False, 1, tr, 0x20 * idx_t))
                if rm < 0:
                    emit(emit_mem_load(t2, s, cur_va, rsp_delta))
                    src2 = t2
                else:
                    src2 = rm
                emit(vpmaddwd(L, t0, src1, src2))
                emit(vmovdqu_rip(L, t2, pool_va + off_8000, cur_va))
                emit(vpcmpeqd(L, t2, t0, t2))
                emit(vpxor(L, t0, t0, t2))
                emit(vpxor(L, t1, t1, t1))
                emit(vpsubd(L, t2, t1, t2))
                emit(vmovdqu_rsp(False, 1, t2, 0xa0))   # spill (+1 or 0)
                emit_sat_into_dst(t0)
                emit(vmovdqu_rsp(True, 1, t0, 0xa0))
                emit_sat_into_dst(t0)

            for idx_t, tr in enumerate(temps):
                emit(vmovdqu_rsp(True, 1, tr, 0x20 * idx_t))
            emit(lea_rsp(rsp_delta))
            emit(jmp_rel32(s['va'] + s['len'], cur_va))

        elif k == 'vgf2p8affineqb' and s['ro_lut'] is not None:
            off_lo, off_hi = lut_offs[id(s)]
            t0, t1, t2 = pick_regs({dst, src1}, 3)
            rsp_delta = 0x80 + 0x60
            emit(lea_rsp(-rsp_delta))
            emit(vmovdqu_rsp(False, 1, t0, 0x00))
            emit(vmovdqu_rsp(False, 1, t1, 0x20))
            emit(vmovdqu_rsp(False, 1, t2, 0x40))
            emit(vmovdqu_rip(L, t2, pool_va + off_mask0f, cur_va))
            emit(vpand(L, t0, src1, t2))
            emit(vpsrlw(L, t1, src1, 4))
            emit(vpand(L, t1, t1, t2))
            emit(vmovdqu_rip(L, t2, pool_va + off_lo, cur_va))
            emit(vpshufb(L, t0, t2, t0))
            emit(vmovdqu_rip(L, t2, pool_va + off_hi, cur_va))
            emit(vpshufb(L, t1, t2, t1))
            emit(vpxor(L, dst, t0, t1))
            emit(vmovdqu_rsp(True, 1, t0, 0x00))
            emit(vmovdqu_rsp(True, 1, t1, 0x20))
            emit(vmovdqu_rsp(True, 1, t2, 0x40))
            emit(lea_rsp(rsp_delta))
            emit(jmp_rel32(s['va'] + s['len'], cur_va))

        elif k == 'vgf2p8affineqb':
            # General dynamic register/memory or non-uniform qword matrix:
            # Frame: [0x00..0xc0): saved ymm0..ymm5; [0xc0..0xe0): staged x; [0xe0..0x100): staged m; [0x100..0x108): saved %rax;
            # [0x110..0x190): untouched 128B caller red zone; `call` pushes return addr at -8(%rsp).
            rsp_delta = 0x110 + 0x80
            emit(lea_rsp(-rsp_delta))
            for r in range(6):
                emit(vmovdqu_rsp(False, 1, r, 0x20 * r))
            emit(b'\x48\x89\x84\x24' + struct.pack('<i', 0x100))   # movq %rax, 0x100(%rsp)
            # Stage src1 (x) and rm/mem (m) through stack so arbitrary aliasing (e.g. src1=ymm1, rm=ymm0) is hazard-free
            emit(vmovdqu_rsp(False, L, src1, 0xc0))
            if rm < 0:
                emit(emit_mem_load(0, s, cur_va, rsp_delta))
                emit(vmovdqu_rsp(False, L, 0, 0xe0))
            else:
                emit(vmovdqu_rsp(False, L, rm, 0xe0))
            emit(vmovdqu_rsp(True, L, 0, 0xc0))                    # %xmm0/%ymm0 = x
            emit(vmovdqu_rsp(True, L, 1, 0xe0))                    # %xmm1/%ymm1 = m
            emit(b'\xb0' + bytes([s['imm8'] & 0xff]))              # movb $imm8, %al
            emit(call_rel32(helper_gfni_va, cur_va))
            if dst >= 6:
                emit(vmovdqa_rr(L, dst, 0))                        # L=0 automatically zeroes upper 128 bits of ymm_dst
            else:
                # Write result into dst's restore slot (zeroing upper 128 bits when L == 0!)
                if L == 0:
                    emit(vmovdqa_rr(0, 0, 0))                      # zero ymm0[255:128]
                emit(vmovdqu_rsp(False, 1, 0, 0x20 * dst))
            emit(b'\x48\x8b\x84\x24' + struct.pack('<i', 0x100))   # movq 0x100(%rsp), %rax
            for r in range(6):
                emit(vmovdqu_rsp(True, 1, r, 0x20 * r))
            emit(lea_rsp(rsp_delta))
            emit(jmp_rel32(s['va'] + s['len'], cur_va))

        rel = tramp_va - (s['va'] + 5)
        patch = b'\xe9' + struct.pack('<i', rel) + b'\x90' * (s['len'] - 5)
        jmp_edits.append((s['off'], s['len'], patch))

    # Build the new segment contents: [relocated Phdr table] + [pool] + [code]
    seg_filesz = code_start + len(code)
    new_load_phdr = [1, 0x5, new_off, new_va, new_va, seg_filesz, seg_filesz, seg_align]   # PT_LOAD, PF_R | PF_X

    # Insert new PT_LOAD right after the last existing PT_LOAD so PT_LOADs stay sorted by p_vaddr
    last_load_idx = max(i for i, p in enumerate(phdrs) if p[0] == 1)
    phdrs.insert(last_load_idx + 1, new_load_phdr)
    for p in phdrs:
        if p[0] == 6:   # PT_PHDR -> point to the relocated Phdr table inside our new PT_LOAD!
            p[1] = 0x4
            p[2] = new_off
            p[3] = new_va
            p[4] = new_va
            p[5] = phdr_bytes_len
            p[6] = phdr_bytes_len
            p[7] = 8

    seg_buf = bytearray(seg_filesz)
    for i, p in enumerate(phdrs):
        struct.pack_into('<IIQQQQQQ', seg_buf, i * 56, *p)
    seg_buf[pool_start:pool_start + len(pool.blob)] = pool.blob
    seg_buf[code_start:code_start + len(code)] = code

    out = bytearray(d)
    for off, length, patch in jmp_edits:
        out[off:off + length] = patch
    out.extend(b'\x00' * (new_off - len(out)))
    out.extend(seg_buf)

    # Append a section header for the generated `.text` segment so `objdump -d` also disassembles & verifies our trampolines
    new_shoff = 0
    new_shnum = 0
    if elf['shdrs']:
        shdrs = [list(s) for s in elf['shdrs']]
        shdrs.append([0, 1, 0x6, new_va + code_start, new_off + code_start, len(code), 0, 0, 16, 0])  # SHT_PROGBITS, SHF_ALLOC|SHF_EXECINSTR
        pad_sh = align_up(len(out), 8) - len(out)
        out.extend(b'\x00' * pad_sh)
        new_shoff = len(out)
        new_shnum = len(shdrs)
        for s in shdrs:
            out.extend(struct.pack('<IIQQQQIIQQ', *s))

    # Update ELF header (e_phoff, e_shoff, e_phnum, e_shnum)
    struct.pack_into('<Q', out, 0x20, new_off)
    if new_shoff:
        struct.pack_into('<Q', out, 0x28, new_shoff)
        struct.pack_into('<H', out, 0x3C, new_shnum)
    struct.pack_into('<H', out, 0x38, len(phdrs))
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    if sys.argv[1] == '--scan':
        path = sys.argv[2]
        d = open(path, 'rb').read()
        elf = parse_elf(d)
        if not elf:
            print('not-elf64 patches=0 unmatched=0')
            return 0
        sites, un = find_sites(path, d, elf['phdrs'])
        print('ok patches=%d unmatched=%d' % (len(sites), len(un)), un[:5] if un else '')
        return 3 if un else 0

    src, dst = sys.argv[1:3]
    d = open(src, 'rb').read()
    elf = parse_elf(d)
    if not elf:
        if src != dst:
            shutil.copyfile(src, dst)
        return 0
    sites, un = find_sites(src, d, elf['phdrs'])
    if un:
        print('%s: %d UNMATCHED site(s): %s' % (src, len(un), un[:5]), file=sys.stderr)
        return 3
    if not sites:
        if src != dst:
            shutil.copyfile(src, dst)
        print('%s: 0 sites (nothing to patch)' % src)
        return 0

    patched = build_segment(d, elf, sites)
    open(dst, 'wb').write(patched)
    shutil.copymode(src, dst)

    # Self-check: re-disassemble `dst` (including our appended section!) to prove 0 VNNI/GFNI instructions remain
    post_elf = parse_elf(patched)
    rem_sites, rem_un = find_sites(dst, patched, post_elf['phdrs'])
    if rem_sites or rem_un:
        print('%s: SELF-CHECK FAILED: %d remaining / %d unmatched after patch!' % (dst, len(rem_sites), len(rem_un)), file=sys.stderr)
        return 3

    by_kind = {}
    for s in sites:
        by_kind[s['kind']] = by_kind.get(s['kind'], 0) + 1
    summary = ', '.join('%s=%d' % kv for kv in sorted(by_kind.items()))
    print('%s: lowered %d site(s) to AVX2 (%s), +%d KB' % (src, len(sites), summary, (len(patched) - len(d) + 1023) // 1024))
    return 0


if __name__ == '__main__':
    sys.exit(main())
