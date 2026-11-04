#!/usr/bin/env python3
"""run_a11y_qemu.py KERNEL [--keep DIR] -- test the stage0 screen reader (usb/a11y/, Speakup + espeakup) on an emulated Intel HDA card.

Boots KERNEL (with the stage0 initramfs inside) in QEMU with a stick that has MODE=ask and the real usb/a11y/ folder, an `intel-hda` +
`hda-duplex` sound card whose output is recorded to a WAV file, and checks:
  1. kmsg.log on the stick has "a11y: espeakup running" (Speakup in the kernel, /dev/softsynth, sound card found, mixer, espeakup started)
  2. the recorded audio is not silence (the boot menu was spoken)
  3. with A11Y=0 in gbos.cfg the reader is not started and the audio is silent (the opt-out works)
Prints ISA_EMU_A11Y_PASS on success. Needs qemu-system-x86_64, mkfs.fat and mtools (Homebrew). Does not need the Android images."""
import os, pathlib, shutil, subprocess, sys, tempfile, time

repo = pathlib.Path(__file__).resolve().parents[2]
usb = repo / 'usb'
args = sys.argv[1:]
keep = None
if '--keep' in args:
    i = args.index('--keep'); keep = pathlib.Path(args[i + 1]); del args[i:i + 2]
kernel = pathlib.Path(args[0]).resolve()
work = keep or pathlib.Path(tempfile.mkdtemp(prefix='gbos-a11y-'))
work.mkdir(parents=True, exist_ok=True)
FAIL = []
def check(ok, msg):
    print(('PASS ' if ok else 'FAIL ') + msg, flush=True)
    if not ok: FAIL.append(msg)
def run(cmd, **kw): return subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)
MIB = 1 << 20

def make_stick(path, cfg):
    t = work / 'tree'; shutil.rmtree(t, ignore_errors=True); (t / 'gbos').mkdir(parents=True)
    shutil.copytree(usb / 'a11y', t / 'gbos/a11y')
    (t / 'gbos/gbos.cfg').write_text(cfg)
    # busybox is copied from the initramfs by stage0 itself; nothing else is needed to reach the menu.
    path.write_bytes(b''); os.truncate(path, 96 * MIB)
    r = run(['mkfs.fat', '-F', '32', '-n', 'GBOS', str(path)]); assert r.returncode == 0, r.stderr
    r = run(['mcopy', '-s', '-i', str(path)] + [str(x) for x in sorted(t.iterdir())] + ['::/']); assert r.returncode == 0, r.stderr

def boot(stick, wav, secs):
    cmd = ['qemu-system-x86_64', '-machine', 'q35', '-cpu', 'Skylake-Client', '-m', '2048', '-smp', '2', '-display', 'none',
           '-serial', f'file:{work}/serial.log', '-monitor', 'stdio', '-no-reboot', '-nic', 'none',
           '-audiodev', f'wav,id=snd0,path={wav}', '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0',
           '-drive', f'if=none,id=stick,file={stick},format=raw', '-device', 'qemu-xhci', '-device', 'usb-storage,drive=stick',
           '-kernel', str(kernel), '-append', 'console=ttyS0 8250.nr_uarts=1 panic=-1 loglevel=7 rdinit=/init ignore_loglevel speakup.synth=soft']
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(secs)
    try: p.stdin.write(b'quit\n'); p.stdin.flush()
    except Exception: pass
    try: p.wait(timeout=20)
    except subprocess.TimeoutExpired: p.kill(); p.wait()

def kmsg(stick):
    r = run(['mtype', '-i', str(stick), '::gbos/logs/boot-1/kmsg.log'])
    return r.stdout

def loud_samples(wav):
    # QEMU only fills in the WAV header's sizes on a clean exit, so read the 16-bit stereo samples after the 44-byte header by hand.
    try: raw = pathlib.Path(wav).read_bytes()[44:]
    except OSError: return -1
    import array
    a = array.array('h'); a.frombytes(raw[:len(raw) // 2 * 2])
    return sum(1 for x in a if abs(x) > 500)

for label, cfg, want in (('on', 'MODE=ask\n', True), ('off', 'MODE=ask\nA11Y=0\n', False)):
    stick = work / f'stick-{label}.img'; wav = work / f'out-{label}.wav'
    make_stick(stick, cfg)
    print(f'-- A11Y {label}: booting ...', flush=True)
    boot(stick, wav, 90)
    log = kmsg(stick)
    n = loud_samples(wav)
    print(f'   audio samples above the noise floor: {n}')
    if want:
        check('a11y: espeakup running' in log, 'kmsg.log has "a11y: espeakup running"')
        check(n > 2000, f'the recorded audio is not silent ({n} loud samples)')
    else:
        check('a11y: starting the screen reader' not in log, 'A11Y=0: the screen reader was not started')
        check(n <= 0 or n < 200, f'A11Y=0: the audio is silent ({n} loud samples)')
    if not want or FAIL:
        print('--- a11y lines in kmsg.log:'); print('\n'.join(l for l in log.splitlines() if 'a11y' in l or 'speakup' in l.lower())[:2000])

print(f'work dir: {work}')
if FAIL:
    print('FAILED: ' + '; '.join(FAIL)); sys.exit(1)
print('ISA_EMU_A11Y_PASS')
if not keep: shutil.rmtree(work, ignore_errors=True)
