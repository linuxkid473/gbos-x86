#!/usr/bin/env python3
"""port_build.py WORK [--write] [--isa] [--kernel-config FILE] [--baseline MANIFEST]

Port gbOS86 to a Google build: re-apply every binary substitution to the new build's files, regenerate what we derive
from them, and say what in the boot scripts no longer matches. Run it through tools/port_build.sh (extracts Google's
partitions and runs this inside the gbos-erofs container); porting.md explains the whole flow.

WORK is a directory holding the extracted build:  system/  vendor/  system_ext/  [product/]  and  apex/<name>/  (APEX payloads).

What it does
  1. Patches (the "substitutions"), each located by pattern or exported symbol, never by address:
       libz.so       tools/patch_libz_adler.py   adler32_z always takes its scalar path        (system, vendor)
       libcrypto.so  tools/sha_redirect.py       SHA-NI/ADX routines jump to Haswell code      (system, vendor, APEX copies)
       libkm_compat  tools/patch_km_compat.py    software KeyMint for every security level
       media/audio   tools/patch_vnni_avx2.py    AVX-VNNI & GFNI lowered to native Haswell AVX2 (system, vendor, swcodec APEX)
  2. system_ext_manifest.xml: Google's VINTF manifest minus the Trusty KeyMint/RKP/SecureClock declarations.
  3. usb/patched/MANIFEST: one line per patch with the sha256 of the original it was made from. hook.sh, early_patch.sh and
     apex_patch.sh read it (nothing is hard-coded in the scripts any more) and bind a patch only over a file with that hash.
  4. Anchor checks: every path/line/property the scripts depend on (files they hide, lines they sed, services they name).
  5. Watched files: files our overlays were copied from (audio config, VINTF manifest). The MANIFEST records their hashes, so
     the next port says exactly which of them Google changed.
  6. --isa: instructions the new build uses that the kernel emulator does not cover (AVX-512, AMX, ...).

Without --write nothing in the repo changes: the result is compared with what is shipped (identical / differs).
Exit status: 0 = ready, 1 = something needs a person (a FAIL line), 2 = usage.
"""
import filecmp, glob, hashlib, json, os, re, shutil, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
USB = os.path.join(ROOT, 'usb')
PY = sys.executable

# kind: early = bound by early_patch.sh (and again by hook.sh as a fail-safe), bind = hook.sh, apex = apex_patch.sh
# (tree, path in tree, path on the device as seen by hook.sh, patcher, patched file relative to the gbos folder)
TARGETS = [
    ('early', 'system', 'lib64/libz.so', '/new/system/lib64/libz.so', 'patch_libz_adler.py', 'patched/system.libz.so'),
    ('early', 'vendor', 'lib64/libz.so', '/new/vendor/lib64/libz.so', 'patch_libz_adler.py', 'patched/vendor.libz.so'),
    ('early', 'system', 'lib64/libcrypto.so', '/new/system/lib64/libcrypto.so', 'sha_redirect.py', 'patched/system.libcrypto.so'),
    ('early', 'vendor', 'lib64/libcrypto.so', '/new/vendor/lib64/libcrypto.so', 'sha_redirect.py', 'patched/vendor.libcrypto.so'),
    ('bind', 'system', 'lib64/libkm_compat.so', '/new/system/lib64/libkm_compat.so', 'patch_km_compat.py', 'libkm_compat.so'),
    ('bind', 'system', 'lib64/libaudioprocessing.so', '/new/system/lib64/libaudioprocessing.so', 'patch_vnni_avx2.py', 'patched/system.libaudioprocessing.so'),
    ('bind', 'system', 'lib64/libsfplugin_ccodec_utils.so', '/new/system/lib64/libsfplugin_ccodec_utils.so', 'patch_vnni_avx2.py', 'patched/system.libsfplugin_ccodec_utils.so'),
    ('bind', 'system', 'lib64/libyuv.so', '/new/system/lib64/libyuv.so', 'patch_vnni_avx2.py', 'patched/system.libyuv.so'),
    ('bind', 'system', 'lib64/libstagefright.so', '/new/system/lib64/libstagefright.so', 'patch_vnni_avx2.py', 'patched/system.libstagefright.so'),
    ('bind', 'vendor', 'lib64/libcodec2_soft_common.so', '/new/vendor/lib64/libcodec2_soft_common.so', 'patch_vnni_avx2.py', 'patched/vendor.libcodec2_soft_common.so'),
    ('bind', 'vendor', 'lib64/libsfplugin_ccodec_utils.so', '/new/vendor/lib64/libsfplugin_ccodec_utils.so', 'patch_vnni_avx2.py', 'patched/vendor.libsfplugin_ccodec_utils.so'),
    ('bind', 'vendor', 'lib64/libyuv.so', '/new/vendor/lib64/libyuv.so', 'patch_vnni_avx2.py', 'patched/vendor.libyuv.so'),
]
# The Conscrypt APEX's libcrypto is the reference; every APEX copy with the same hash is bound to the same patched file.
APEX_REF = ('com.google.android.conscrypt', 'lib64/libcrypto.so', 'sha_redirect.py', 'patched/conscrypt.libcrypto.so')
# Media software codecs & buffer-copy libraries inside the swcodec APEX (bound by apex_patch.sh before perform_apex_config / zygote):
APEX_TARGETS = [
    ('com.google.android.media.swcodec', 'lib64/libvpx.so', 'patch_vnni_avx2.py', 'patched/swcodec.libvpx.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_avcdec.so', 'patch_vnni_avx2.py', 'patched/swcodec.avcdec.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_av1dec_dav1d.so', 'patch_vnni_avx2.py', 'patched/swcodec.dav1d.so'),
    ('com.google.android.media.swcodec', 'lib64/libFraunhoferAAC.so', 'patch_vnni_avx2.py', 'patched/swcodec.aac.so'),
    ('com.google.android.media.swcodec', 'lib64/libopus.so', 'patch_vnni_avx2.py', 'patched/swcodec.opus.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_common.so', 'patch_vnni_avx2.py', 'patched/swcodec.common.so'),
    ('com.google.android.media.swcodec', 'lib64/libsfplugin_ccodec_utils.so', 'patch_vnni_avx2.py', 'patched/swcodec.ccodec_utils.so'),
    ('com.google.android.media.swcodec', 'lib64/libcom.android.media.swcodec.apexcodecs.so', 'patch_vnni_avx2.py', 'patched/swcodec.apexcodecs.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_mpeg4dec.so', 'patch_vnni_avx2.py', 'patched/swcodec.mpeg4dec.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_h263dec.so', 'patch_vnni_avx2.py', 'patched/swcodec.h263dec.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_amrnbdec.so', 'patch_vnni_avx2.py', 'patched/swcodec.amrnbdec.so'),
    ('com.google.android.media.swcodec', 'lib64/libcodec2_soft_amrwbdec.so', 'patch_vnni_avx2.py', 'patched/swcodec.amrwbdec.so'),
]
# Vendor libraries bound conditionally by hook.sh when GPU_SW=1 (AMD GPUs and Intel Gen <= 8):
EXTRA_TARGETS = [
    ('vendor', 'lib64/hw/libdesktop_camera_common.dylib.so', 'patch_camera_common_cpu.py', 'patched/libdesktop_camera_common.cpu.so'),
    ('vendor', 'lib64/libminigbm_gralloc.so', 'patch_gralloc_amdgpu.py', 'patched/libminigbm_gralloc.amdgpu.so'),
    ('vendor', 'lib64/libgbm.so', 'patch_libgbm_amdgpu.py', 'patched/libgbm.amdgpu.so'),
]

# <hal> blocks removed from system_ext's VINTF manifest: Trusty KeyMint/RKP/SecureClock (km_compat serves them instead).
HIDE_HALS = {('android.hardware.security.keymint', 'IKeyMintDevice/default'),
             ('android.hardware.security.keymint', 'IRemotelyProvisionedComponent/default'),
             ('android.hardware.security.secureclock', 'ISecureClock/default')}

# Files our overlays were derived from (relative to a tree root "tree:path"; a trailing / = every file in that folder).
WATCH = ['system_ext:etc/vintf/manifest.xml', 'vendor:etc/data/hal_configs/audio_default.xml',
         'vendor:etc/audio/sof-soundwire_cs42l43_cs35l56/', 'vendor:etc/fstab.android-desktop']

# Instruction families the kernel emulator (kernel/isa_emu_patch) handles; anything else the scan finds is a problem.
EMULATED = {'GFNI', 'SHA-NI', 'AVX-VNNI', 'AVX-VNNI-INT8/16', 'AVX-IFMA', 'VAES (256/512-bit)', 'VPCLMULQDQ (256/512-bit)'}
CPU_HAS = {'ADX', 'RDSEED', 'CLFLUSHOPT', 'XSAVEC/XSAVES'}   # i5-8365U

results = []   # (status, text)


def note(status, text):
    results.append((status, text))
    print('%-5s %s' % (status, text), flush=True)


def base(root, tree):
    """Directory of a tree's files. system_a.img is the whole Android root, so /system is one level down."""
    d = os.path.join(root, tree)
    nested = os.path.join(d, 'system')
    return nested if tree == 'system' and os.path.isdir(nested) else d


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def changed_regions(a, b):
    """-> list of (offset, length) where two equal-length files differ."""
    if len(a) != len(b):
        return None
    out, i, n = [], 0, len(a)
    while i < n:
        if a[i] != b[i]:
            j = i
            while j + 1 < n and a[j + 1] != b[j + 1]:
                j += 1
            out.append((i, j - i + 1))
            i = j + 1
        else:
            i += 1
    return out


def patch_one(tree_root, tree, rel, patcher, shipped, outdir, label):
    """Run one patcher on tree/rel. -> dict(entry) or None when it could not be patched."""
    src = os.path.join(base(tree_root, tree) if not tree.startswith('apex/') else os.path.join(tree_root, tree), rel)
    if not os.path.isfile(src):
        note('FAIL', '%s: %s/%s does not exist in this build' % (label, tree, rel))
        return None
    orig_sha = sha(src)
    dst = os.path.join(outdir, label.replace('/', '_'))
    rc, out = run([PY, '-I', os.path.join(HERE, patcher), src, dst])
    if rc != 0 or not os.path.isfile(dst):
        note('FAIL', '%s: %s found no match (exit %s): %s' % (label, patcher, rc, out.splitlines()[-1] if out else ''))
        return None
    a, b = open(src, 'rb').read(), open(dst, 'rb').read()
    regs = changed_regions(a, b)
    if regs is not None and not regs:
        # nothing changed: fine for patch_vnni_avx2.py when a build compiled this library without VNNI/GFNI
        lvl = 'INFO' if patcher == 'patch_vnni_avx2.py' else 'WARN'
        note(lvl, '%s: patcher changed nothing (no target instructions in this file). Not shipped' % label)
        return None
    desc = '%d byte(s) in %d place(s)' % (sum(n for _, n in regs), len(regs)) if regs else 'size +%d KB' % ((len(b) - len(a) + 1023) >> 10)
    ship = os.path.join(USB, shipped)
    if os.path.isfile(ship):
        same = filecmp.cmp(dst, ship, shallow=False)
        cmp_s = 'identical to the shipped file' if same else 'DIFFERS from the shipped file (new bytes)'
    else:
        cmp_s = 'no shipped file yet'
    note('OK', '%s: patched, %s, %s [orig %s]' % (label, desc, cmp_s, orig_sha[:12]))
    return {'orig_sha': orig_sha, 'patched': dst, 'shipped': shipped}


def read_tree_file(root, tree, rel):
    p = os.path.join(base(root, tree), rel)
    return open(p, 'rb').read() if os.path.isfile(p) else None


def find_apex_libs(root):
    """-> {apex name: path of lib64/libcrypto.so} for every extracted APEX payload."""
    out = {}
    for d in sorted(glob.glob(os.path.join(root, 'apex', '*'))):
        p = os.path.join(d, 'lib64', 'libcrypto.so')
        if os.path.isfile(p):
            out[os.path.basename(d)] = p
    return out


def gen_system_ext_manifest(root, outdir):
    d = read_tree_file(root, 'system_ext', 'etc/vintf/manifest.xml')
    if d is None:
        note('FAIL', 'system_ext_manifest.xml: system_ext/etc/vintf/manifest.xml missing')
        return None
    text = d.decode()
    removed = set()

    def drop(m):
        blk = m.group(0)
        n = re.search(r'<name>([^<]+)</name>', blk)
        f = re.search(r'<fqname>([^<]+)</fqname>', blk)
        key = (n.group(1) if n else '', f.group(1) if f else '')
        if key in HIDE_HALS:
            removed.add(key)
            return ''
        return blk
    # a <hal> block plus the whitespace line it occupies
    new = re.sub(r'[ \t]*<hal\b.*?</hal>[ \t]*\n', drop, text, flags=re.S)
    missing = HIDE_HALS - removed
    dst = os.path.join(outdir, 'system_ext_manifest.xml')
    open(dst, 'w').write(new)
    ship = os.path.join(USB, 'system_ext_manifest.xml')
    cmp_s = 'no shipped file yet'
    if os.path.isfile(ship):
        cmp_s = 'identical to the shipped file' if filecmp.cmp(dst, ship, shallow=False) else 'DIFFERS from the shipped file'
    if missing:
        note('WARN', 'system_ext_manifest.xml: HALs no longer declared (renamed or gone?): %s; %s' %
             (', '.join('%s %s' % k for k in sorted(missing)), cmp_s))
    else:
        note('OK', 'system_ext_manifest.xml: removed %d Trusty <hal> blocks; %s' % (len(removed), cmp_s))
    return dst


def watched_hashes(root):
    out = {}
    for w in WATCH:
        tree, rel = w.split(':', 1)
        bp = os.path.join(base(root, tree), rel)
        if rel.endswith('/'):
            for p in sorted(glob.glob(os.path.join(bp, '**', '*'), recursive=True)):
                if os.path.isfile(p):
                    out['%s:%s' % (tree, os.path.relpath(p, base(root, tree)))] = sha(p)
        elif os.path.isfile(bp):
            out[w] = sha(bp)
        else:
            out[w] = 'missing'
    return out


def read_manifest(path):
    watch, build = {}, ''
    if os.path.isfile(path):
        for line in open(path):
            p = line.split()
            if line.startswith('# build '):
                build = line[8:].strip()
            elif p and p[0] == 'watch' and len(p) == 3:
                watch[p[1]] = p[2]
    return build, watch


def has(root, tree, rel):
    return os.path.exists(os.path.join(base(root, tree), rel))


def grep_count(root, tree, rel, text, regex=False):
    d = read_tree_file(root, tree, rel)
    if d is None:
        return None
    s = d.decode('utf-8', 'replace')
    if regex:
        return len(re.findall(text, s, flags=re.M))
    return sum(1 for ln in s.splitlines() if ln == text)


def anchors(root):
    """Everything hook.sh, apex_patch.sh and friends assume about the build. Mirror of what the scripts do."""
    def need(ok, what, hint=''):
        note('OK' if ok else 'WARN', ('anchor: ' if ok else 'anchor GONE: ') + what + ('' if ok else (' -- ' + hint if hint else '')))

    def similar(tree, pattern):
        return ', '.join(sorted(os.path.relpath(p, base(root, tree)) for p in glob.glob(os.path.join(base(root, tree), pattern)))[:4]) or 'nothing similar'

    n = grep_count(root, 'system', 'etc/init/hw/init.rc', '    wait_for_prop apexd.status activated')
    need(n == 1, 'init.rc has exactly one "wait_for_prop apexd.status activated" (apex_patch.sh is hooked after it; found %s)' % n,
         'hook.sh logs "apex_patch NOT installed" and the Conscrypt libcrypto stays unpatched (slow Play installs)')
    # services/files the hook hides (VINTF declaration + rc), so a renamed one cannot hang the boot unseen
    for desc, tree, rel, pat in [
            ('Weaver VINTF', 'vendor', 'etc/vintf/manifest/android.hardware.weaver-service.android-desktop.xml', 'etc/vintf/manifest/*weaver*'),
            ('Secretkeeper VINTF', 'vendor', 'etc/vintf/manifest/android.hardware.security.secretkeeper.trusty.xml', 'etc/vintf/manifest/*secretkeeper*'),
            ('Secretkeeper rc', 'vendor', 'etc/init/android.hardware.security.secretkeeper.trusty.desktop.rc', 'etc/init/*secretkeeper*'),
            ('Trusty Gatekeeper VINTF', 'vendor', 'etc/vintf/manifest/android.hardware.gatekeeper-service.trusty.xml', 'etc/vintf/manifest/*gatekeeper*'),
            ('Trusty Gatekeeper rc', 'vendor', 'etc/init/android.hardware.gatekeeper-service.trusty.rc', 'etc/init/*gatekeeper*'),
            ('plain Trusty VM launcher rc', 'system_ext', 'etc/init/desktop-trusty_security_vm_launcher.rc', 'etc/init/*trusty*'),
            ('apgetty rc', 'system_ext', 'etc/init/apgetty.rc', 'etc/init/*getty*'),
            ('vold fstab', 'vendor', 'etc/fstab.android-desktop', 'etc/fstab*'),
            ('audio HAL config', 'vendor', 'etc/data/hal_configs/audio_default.xml', 'etc/data/hal_configs/audio*'),
            ('crossystem (bind target)', 'vendor', 'bin/crossystem', 'bin/crossystem*'),
            ('StrongBox KeyMint APEX', 'vendor', 'apex/com.android.hardware.keymint.strongbox.desktop.apex', 'apex/*strongbox*'),
            ('Trusty VM boot image', 'system_ext', 'etc/vm/trusty_vm/desktop_trusty_ext_boot_signed.elf', 'etc/vm/trusty_vm/*')]:
        need(has(root, tree, rel), '%s %s/%s' % (desc, tree, rel), 'hook.sh does not hide/replace it; now: ' + similar(tree, pat))
    note('OK' if has(root, 'system', 'etc/init/gscd.rc') else 'INFO', 'anchor: gscd.rc %s (hook.sh deletes it from the init overlay if present)' %
         ('present' if has(root, 'system', 'etc/init/gscd.rc') else 'absent'))
    n = grep_count(root, 'system_ext', 'etc/init/desktop-trusty_security_vm_launcher.rc', '    start trusty_security_vm_launcher')
    need(n is not None and n >= 1, 'VM launcher rc starts "trusty_security_vm_launcher" (hook.sh comments that line out)', 'the plain launcher may now race the on-demand one')
    n = grep_count(root, 'vendor', 'etc/fstab.android-desktop', 'voldmanaged=usb', regex=True)
    need(bool(n), 'fstab has a voldmanaged=usb rule (hook.sh removes it so vold leaves the stick alone; found %s)' % n,
         'if vold now manages USB differently the stick may get fsck_msdos again (lost/truncated logs)')
    for key in ('ro.boot.hypervisor.protected_vm.supported=1', 'ro.surface_flinger.has_wide_color_display=', 'ro.surface_flinger.has_HDR_display=', 'ro.sf.lcd_density=', 'vendor.hwc.drm.enable_color_pipeline=true', 'ro.hwui.use_vulkan=true'):
        n = grep_count(root, 'vendor', 'build.prop', '^' + re.escape(key), regex=True)
        need(bool(n), 'vendor build.prop sets %s' % key, 'hook.sh filters/overrides it')
    for svc in ('trusty_security_protected_vm_launcher',):
        hit = False
        for tree in ('system_ext', 'system', 'vendor'):
            for rc in glob.glob(os.path.join(base(root, tree), 'etc/init', '*.rc')) + glob.glob(os.path.join(base(root, tree), 'etc/init/hw', '*.rc')):
                if re.search(r'^service %s\b' % svc, open(rc, errors='replace').read(), re.M):
                    hit = True
        need(hit, 'service %s is defined (gbos.rc starts it in early-fs)' % svc, 'start of an undefined service is ignored: KeyMint may never get its VM')
    # sepolicy domains gbos.rc names in seclabel lines are checked on the device (init refuses unknown labels): list them for a human
    note('INFO', 'anchor: not checkable here: SELinux types used by gbos.rc (u:r:shell:s0, u:r:hal_gatekeeper_default:s0), the dm/loop setup in stage0 and '
         'first-stage init expectations. Boot once and read stage0-dmesg.txt')


def isa_scan(root, scandir):
    """Compare the instruction families this build uses with the baseline scan of the shipped build (lapis/scan/*_a.json).
    Emulated families: fine. A family the emulator lacks: FAIL when the shipped build did not use it at all, WARN when new
    files use it, INFO when it is the same known gap as before (AVX-512/AMX code that is gated by CPU checks at run time)."""
    base = {}
    for t in ('system', 'vendor', 'system_ext', 'product'):
        p = os.path.join(ROOT, 'lapis', 'scan', t + '_a.json')
        if os.path.isfile(p):
            base[t] = json.load(open(p))
    os.makedirs(scandir, exist_ok=True)
    for tree in ('system', 'vendor', 'system_ext', 'product'):
        if not os.path.isdir(os.path.join(root, tree)):
            continue
        out = os.path.join(scandir, 'isa-%s.json' % tree)
        if not os.path.isfile(out):
            print('      scanning %s (minutes) ...' % tree, flush=True)
            rc, txt = run([PY, '-I', os.path.join(HERE, 'isa_scan.py'), os.path.join(root, tree), tree + '_new', out])
            if rc != 0 or not os.path.isfile(out):
                note('WARN', 'isa scan of %s failed: %s' % (tree, txt[-200:]))
                continue
        cur = json.load(open(out))
        old = base.get(tree)
        if old is None:
            note('INFO', 'isa %s: no baseline in lapis/scan/, listing only' % tree)
        for feat, nfiles in sorted(cur['files_per_feature'].items()):
            if feat in CPU_HAS:
                continue
            was = (old or {}).get('files_per_feature', {}).get(feat)
            if feat in EMULATED:
                note('OK', 'isa %s: %s in %d file(s)%s: emulated by the kernel' % (tree, feat, nfiles, '' if was is None else ' (shipped build: %d)' % was))
                continue
            gone = set((old or {}).get('all_files', {}).get(feat, []))
            fresh = sorted(set(cur['all_files'].get(feat, [])) - gone)
            if old is not None and was is None:
                note('FAIL', 'isa %s: %s is NEW (%d file(s), e.g. %s) and the emulator does not cover it' % (tree, feat, nfiles, ', '.join(fresh[:3])))
            elif old is not None and fresh:
                note('WARN', 'isa %s: %s, not emulated, now also in %d new file(s): %s' % (tree, feat, len(fresh), ', '.join(fresh[:5])))
            else:
                note('INFO', 'isa %s: %s in %d file(s), not emulated; same known gap as the shipped build' % (tree, feat, nfiles))
        for feat in sorted(set((old or {}).get('files_per_feature', {})) - set(cur['files_per_feature'])):
            note('INFO', 'isa %s: %s no longer used (shipped build: %d files)' % (tree, feat, old['files_per_feature'][feat]))


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    flags = {a.split('=')[0]: (a.split('=', 1)[1] if '=' in a else True) for a in argv if a.startswith('--')}
    if len(args) != 1 or not os.path.isdir(args[0]):
        print(__doc__)
        return 2
    root = os.path.abspath(args[0])
    write = bool(flags.get('--write'))
    outdir = tempfile.mkdtemp(prefix='port_build.')
    bprop = read_tree_file(root, 'system', 'build.prop') or b''
    m = re.search(rb'^ro\.system\.build\.fingerprint=(.*)$', bprop, re.M)
    build = m.group(1).decode() if m else 'unknown'
    old_build, old_watch = read_manifest(flags.get('--baseline') or os.path.join(USB, 'patched', 'MANIFEST'))
    print('build in WORK : %s' % build)
    print('shipped for   : %s\n' % (old_build or 'unknown (no MANIFEST yet)'))

    # 1. patches
    entries = []   # (kind, device target, sha, file relative to gbos)
    copies = []    # (temp file, repo path)
    for kind, tree, rel, target, patcher, shipped in TARGETS:
        e = patch_one(root, tree, rel, patcher, shipped, outdir, '%s/%s' % (tree, rel))
        if e:
            entries.append((kind, target, e['orig_sha'], e['shipped']))
            copies.append((e['patched'], os.path.join(USB, e['shipped'])))
    # APEX copies of libcrypto
    libs = find_apex_libs(root)
    name, rel, patcher, shipped = APEX_REF
    if name in libs:
        e = patch_one(root, 'apex/' + name, rel, patcher, shipped, outdir, 'apex/%s/%s' % (name, rel))
        if e:
            entries.append(('apex', rel, e['orig_sha'], e['shipped']))
            copies.append((e['patched'], os.path.join(USB, e['shipped'])))
            same = sorted(n for n, p in libs.items() if sha(p) == e['orig_sha'])
            other = {}
            for n, p in libs.items():
                if sha(p) != e['orig_sha']:
                    other.setdefault(sha(p), []).append(n)
            note('INFO', 'APEX libcrypto copies bound to the Conscrypt patch: %d (%s)' % (len(same), ', '.join(same)))
            for h, names in sorted(other.items()):
                names = sorted(names)
                rc, txt = run([PY, '-I', os.path.join(HERE, 'sha_redirect.py'), '--scan', libs[names[0]]])
                if 'patches=0' in txt:
                    note('INFO', 'other APEX libcrypto %s (%s): %s -- nothing to patch' % (h[:12], ', '.join(names), txt.splitlines()[0] if txt else ''))
                    continue
                if any(n.startswith('com.google.android.widevine') for n in names):
                    sub_shipped = 'patched/widevine.libcrypto.so'
                elif 'com.android.hardware.drm.clearkey' in names:
                    sub_shipped = 'patched/clearkey.libcrypto.so'
                else:
                    sub_shipped = 'patched/apex-%s.libcrypto.so' % h[:12]
                e2 = patch_one(root, 'apex/' + names[0], rel, patcher, sub_shipped, outdir, 'apex/%s/%s' % (names[0], rel))
                if e2:
                    entries.append(('apex', rel, e2['orig_sha'], e2['shipped']))
                    copies.append((e2['patched'], os.path.join(USB, e2['shipped'])))
                    note('INFO', 'APEX libcrypto copies bound to %s: %d (%s)' % (sub_shipped, len(names), ', '.join(names)))
    else:
        note('FAIL', 'APEX %s has no lib64/libcrypto.so in WORK/apex (extraction failed or the APEX changed)' % name)

    # APEX media/audio codec libraries (swcodec)
    for apex_prefix, rel, patcher, shipped in APEX_TARGETS:
        matches = sorted(d for d in glob.glob(os.path.join(root, 'apex', apex_prefix + '*')) if os.path.isdir(d))
        if not matches:
            note('FAIL', 'APEX %s missing in WORK/apex' % apex_prefix)
            continue
        aname = os.path.basename(matches[0])
        e = patch_one(root, 'apex/' + aname, rel, patcher, shipped, outdir, 'apex/%s/%s' % (aname, rel))
        if e:
            entries.append(('apex', rel, e['orig_sha'], e['shipped']))
            copies.append((e['patched'], os.path.join(USB, e['shipped'])))

    # Conditional GPU_SW=1 vendor patches (bound by hook.sh via sha256 check, not unconditional MANIFEST entries)
    for tree, rel, patcher, shipped in EXTRA_TARGETS:
        e = patch_one(root, tree, rel, patcher, shipped, outdir, '%s/%s' % (tree, rel))
        if e:
            copies.append((e['patched'], os.path.join(USB, e['shipped'])))

    # 2. derived VINTF manifest
    mf = gen_system_ext_manifest(root, outdir)
    if mf:
        copies.append((mf, os.path.join(USB, 'system_ext_manifest.xml')))

    # 3. anchors + watched files
    print()
    anchors(root)
    print()
    new_watch = watched_hashes(root)
    if old_watch:
        for k in sorted(set(old_watch) | set(new_watch)):
            a, b = old_watch.get(k), new_watch.get(k)
            if a == b:
                continue
            note('WARN', 'watched file %s %s: our overlay was copied from the old one, review it' %
                 (k, 'is new' if a is None else 'disappeared' if b in (None, 'missing') else 'CHANGED'))
        if all(old_watch.get(k) == new_watch.get(k) for k in set(old_watch) | set(new_watch)):
            note('OK', 'watched files (audio config, VINTF manifest, fstab) unchanged since the shipped build')
    else:
        note('INFO', 'no baseline of watched files yet; this run records one')

    # 4. instruction scan
    if flags.get('--isa'):
        print()
        isa_scan(root, os.path.join(root, 'scan'))   # kept in WORK: PORT_KEEP=1 re-runs reuse it
    if flags.get('--kernel-config'):
        txt = open(flags['--kernel-config'], errors='replace').read(4000)
        mm = re.search(r'Linux/x86\S* (\S+)', txt)
        note('INFO', "Google's kernel in this build: %s (ours is built from android17-6.18 @ 3938c8d9; vendor modules are never used)" % (mm.group(1) if mm else '?'))

    fails = [r for r in results if r[0] == 'FAIL']
    warns = [r for r in results if r[0] == 'WARN']

    # 5. manifest text (always built; written only with --write)
    lines = ['# gbOS86 patch manifest: generated by tools/port_build.py, do not edit.',
             '# kind: early = bound before the first dmctl (early_patch.sh) and again by hook.sh; bind = hook.sh; apex = apex_patch.sh',
             '# early/bind:  <kind> <path on the device in stage0> <sha256 of the ORIGINAL> <patched file relative to gbos/>',
             '# apex:        apex <path inside every /apex/*> <sha256 of the ORIGINAL> <patched file relative to gbos/>',
             '# A patch is bound only over a file with exactly that sha256.',
             '# build ' + build]
    lines += ['%s %s %s %s' % e for e in entries]
    lines += ['watch %s %s' % (k, v) for k, v in sorted(new_watch.items())]
    text = '\n'.join(lines) + '\n'
    cur = os.path.join(USB, 'patched', 'MANIFEST')
    same_m = os.path.isfile(cur) and open(cur).read() == text
    print('\nMANIFEST %s' % ('identical to the shipped one' if same_m else 'differs from the shipped one'))
    if write:
        if fails:
            print('NOT writing: %d FAIL line(s) above. Fix those first (patchers may need a new pattern).' % len(fails))
        else:
            for src, dst in copies:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not (os.path.isfile(dst) and filecmp.cmp(src, dst, shallow=False)):
                    shutil.copyfile(src, dst)
                    shutil.copymode(src, dst)
                    print('wrote %s' % os.path.relpath(dst, ROOT))
            os.makedirs(os.path.dirname(cur), exist_ok=True)
            open(cur, 'w').write(text)
            print('wrote %s' % os.path.relpath(cur, ROOT))
            for k in os.listdir(os.path.join(USB, 'patched')):
                if k == 'MANIFEST' or k.startswith('vulkan.') or k.endswith('.amdgpu.so') or k == 'libdesktop_camera_common.cpu.so' or k == 'i965_drv_video.so':
                    continue
                if ('patched/' + k) not in {e[3] for e in entries}:
                    print('NOTE: usb/patched/%s is not in the manifest any more: delete it if the build dropped it' % k)
    else:
        print('(dry run: repo untouched. --write copies the patches, system_ext_manifest.xml and MANIFEST into usb/)')
    shutil.rmtree(outdir, ignore_errors=True)
    print('\n%d FAIL, %d WARN. %s' % (len(fails), len(warns), 'Ready to deploy and boot.' if not fails and not warns else
          'Resolve the FAIL lines (patch not possible) and review each WARN (something a boot script relies on moved).' if fails else
          'No patch failed; review each WARN above before booting.'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
