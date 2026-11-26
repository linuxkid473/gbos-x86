#!/usr/bin/env python3
"""Diskless QEMU smoke test for the Whiskey Lake replacement kernel.

  test_kernel_qemu.py OUT_DIR

OUT_DIR holds bzImage, kernel.release, busybox and modules/<release>/ (see
kernel/out). Boots the kernel under QEMU with a Skylake-Client CPU, loads the
i915 and Intel Wi-Fi module closures and reports each result. QEMU emulates no
Intel GPU or Wi-Fi card, so this checks kernel boot and module admission only.
Adapted from GBRUNCH's tools/test_amd_kernel_qemu.py.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOTS = ['i915', 'iwlmvm']


def cpio(files):
    out = bytearray()
    for ino, (name, data, mode, major, minor) in enumerate(files + [('TRAILER!!!', b'', 0, 0, 0)], 1):
        raw = name.encode() + b'\0'
        fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(raw), 0]
        out += b'070701' + b''.join(b'%08x' % n for n in fields) + raw
        out += bytes(-len(out) % 4)
        out += data
        out += bytes(-len(out) % 4)
    return bytes(out)


def main():
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'gbrunch/tools'))
    from test_amd_kernel_qemu import closure, modinfo  # noqa: E402
    root = pathlib.Path(sys.argv[1]).resolve()
    release = (root / 'kernel.release').read_text().strip()
    moddir = root / 'modules' / release
    modules = {}
    for f in moddir.rglob('*.ko'):
        modules[f.stem.replace('-', '_')] = f
    order = closure(modules, ROOTS)
    for m in order:
        if modinfo(m)['vermagic'][0].split()[0] != release:
            raise SystemExit('vermagic mismatch: ' + m.name)

    files = [(d, b'', 0o40755, 0, 0) for d in ('dev', 'proc', 'sys', 'bin', 'modules')]
    files.append(('dev/console', b'', 0o20600, 5, 1))
    files.append(('bin/busybox', (root / 'busybox').read_bytes(), 0o100755, 0, 0))
    script = ['#!/bin/busybox sh', 'export PATH=/bin', '/bin/busybox --install -s /bin',
              'mount -t proc proc /proc', 'mount -t sysfs sysfs /sys', 'mount -t devtmpfs devtmpfs /dev',
              'exec >/dev/console 2>&1', 'uname -r', 'failed=0']
    for m in order:
        files.append(('modules/' + m.name, m.read_bytes(), 0o100644, 0, 0))
        script += ['insmod /modules/%s' % m.name, 'rc=$?', 'echo MODULE_RESULT:%s:$rc' % m.name,
                   '[ "$rc" -eq 0 ] || failed=1']
    script += ['for n in i915 iwlwifi iwlmvm; do [ -d /sys/module/$n ] || failed=1; done',
               'if [ "$failed" -eq 0 ]; then echo KERNEL_MODULE_SMOKE_PASS; else echo KERNEL_MODULE_SMOKE_FAIL; fi',
               'dmesg | grep -i -E "i915|iwlwifi|taint|unknown symbol|disagrees" | tail -15',
               'poweroff -f']
    files.append(('init', ('\n'.join(script) + '\n').encode(), 0o100755, 0, 0))

    with tempfile.TemporaryDirectory() as tmp:
        initrd = pathlib.Path(tmp) / 'initrd.cpio'
        initrd.write_bytes(cpio(files))
        cmd = ['qemu-system-x86_64', '-machine', 'q35', '-cpu', 'Skylake-Client', '-m', '2048',
               '-smp', '2', '-nographic', '-no-reboot', '-nic', 'none',
               '-kernel', str(root / 'bzImage'), '-initrd', str(initrd),
               '-append', 'console=ttyS0 8250.nr_uarts=1 panic=-1 loglevel=4']
        print('modules in closure:', ', '.join(m.stem for m in order), flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600)
    log = r.stdout + r.stderr
    (root / 'qemu-smoke.log').write_text(log)
    for line in log.splitlines():
        if line.startswith(('MODULE_RESULT', 'KERNEL_MODULE_SMOKE', '6.18')) or 'i915' in line or 'iwl' in line \
                or 'Unknown symbol' in line or 'Kernel panic' in line:
            print(line)
    sys.exit(0 if 'KERNEL_MODULE_SMOKE_PASS' in log else 1)


if __name__ == '__main__':
    main()
