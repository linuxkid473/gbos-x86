#!/usr/bin/env python3
"""run_isa_qemu.py [KERNEL] [CPU] -- boot KERNEL with the ISA emulator's own test program as /init.

This is the project's ISA emulator test (kernel/isa_emu_test/t.c, one known answer per check() call)
without the gbrunch helper module: the test program is put into a tiny initramfs and the kernel is
booted in QEMU (CPU model Skylake-Client unless given; Haswell also exercises ADCX/ADOX). The stock
kernel dies with SIGILL on the first emulated instruction, ours must pass every check in t.c.
"""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def cpio(files):
    out = bytearray()
    ino = [1000]

    def ent(name, mode, data=b""):
        raw = name.encode() + b"\0"
        ino[0] += 1
        f = [ino[0], mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(raw), 0]
        out.extend(b"070701" + b"".join(b"%08x" % n for n in f) + raw)
        out.extend(bytes(-len(out) % 4))
        out.extend(data)
        out.extend(bytes(-len(out) % 4))

    for name, mode, data in files:
        ent(name, mode, data)
    ent("TRAILER!!!", 0)
    return bytes(out)


def expected_checks() -> int:
    """Number of known answers in t.c: one check("...") call each (several can share a line)."""
    return (HERE / "t.c").read_text().count('check("')


def main() -> int:
    kernel = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "usb/out/BOOTX64.EFI")
    # QEMU CPU model. Skylake-Client lacks the AVX-512-era instructions the emulator covers, but it has ADX; Haswell does not,
    # so `run_isa_qemu.py KERNEL Haswell` also exercises the ADCX/ADOX emulation.
    cpu = sys.argv[2] if len(sys.argv) > 2 else "Skylake-Client"
    test = HERE / "t"
    if not test.exists():
        print("no test program (kernel/isa_emu_test/t): run kernel/isa_emu_test/build_test.sh in gbos-kbuild")
        return 2
    files = [
        ("dev", 0o40755, b""),
        ("dev/console", 0o20600, b""),
        ("init", 0o100755, test.read_bytes()),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        rd = pathlib.Path(tmp) / "rd.cpio"
        rd.write_bytes(cpio(files))
        cmd = ["qemu-system-x86_64", "-machine", "q35", "-cpu", cpu, "-m", "1024",
               "-smp", "2", "-nographic", "-no-reboot", "-nic", "none", "-kernel", kernel,
               "-initrd", str(rd), "-append",
               "console=ttyS0 8250.nr_uarts=1 panic=-1 loglevel=4 rdinit=/init"]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=300)
    out = r.stdout + r.stderr
    (HERE / "last_isa.log").write_text(out)
    passes = out.count("PASS ")
    fails = out.count("FAIL ")
    want = expected_checks()
    print("%d PASS, %d FAIL (of %d in t.c, -cpu %s)" % (passes, fails, want, cpu))
    for line in out.splitlines():
        if line.startswith(("PASS ", "FAIL ")):
            print(" ", line)
    if passes == want and fails == 0:
        print("ISA_EMU_TEST_PASS")
        return 0
    print("ISA_EMU_TEST_FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
