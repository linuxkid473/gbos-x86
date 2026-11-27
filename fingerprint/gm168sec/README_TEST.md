# README_TEST.md — installing and testing the 533c HAL

Target: Dell XPS 13 9310, gbOS86 (Android 17, lapis), Goodix 27c6:533c.
Binary: `gbos_fp` in this directory (Milan matcher, 374,712 bytes).
`gbos_fp_stub` is the same HAL with the matcher stubbed out — it probes
and registers but enroll/identify refuse with `GM168SEC_ERR_NO_MATCHER`;
use it only to prove the transport/AIDL plumbing without the matcher.

## Gate 0 — do this FIRST, before installing anything (Ubuntu live boot)

The sensor works in Ubuntu with the proprietary Dell `.deb`, which
proves the hardware is good. It does **not** prove the open-source
driver works on your firmware. Boot an Ubuntu live USB, build the
open driver from `../goodix53x5-533c` (branch `pr334`), and run its
detect/probe. Expected: sensor found, firmware
`GF5288_GM168SEC_APP_13016`, chip ID `0x220ca1`, PSK check passes.

- If the PSK check fails (`sensor PSK hash is not the held PSK's
  hash` or equivalent): **stop**. The sensor holds a PSK the driver
  does not know, and the HAL will fail the same way. There is no
  recovery from the HAL side — only re-flashing sensor firmware
  clears it. Report the exact log and do not install the HAL.
- If the open driver works: the HAL has a real chance. Proceed.

## Install

The HAL runs from the GBOS volume at `/postinstall/gbos/gbos_fp`
(inside Android; the volume is the FAT partition hook.sh reads at
boot). The volume is mounted read-only in Android; root can remount
it read-write in a private mount namespace (the same trick
`audio/audio-profile.sh` uses).

From a root shell on the device (adb root, or the `run.sh`
backchannel):

```sh
# 1. Back up the HAL that is running now (upstream 6384 build).
unshare -m sh -c 'mount -o remount,bind,rw /postinstall &&
  cp /postinstall/gbos/gbos_fp /postinstall/gbos/gbos_fp.orig &&
  echo backed-up'

# 2. Copy the new binary in. Get it onto the device first (adb push
#    to /data/local/tmp, or onto the volume from another machine).
unshare -m sh -c 'mount -o remount,bind,rw /postinstall &&
  cp /data/local/tmp/gbos_fp /postinstall/gbos/gbos_fp &&
  chmod 755 /postinstall/gbos/gbos_fp'

# 3. Apply the hook patch to your gbos-x86 checkout, then rebuild /
#    re-stage the volume the way you normally do:
#      cd <gbos-x86> && patch -p1 < gbos_fp-533c/hook-533c.patch
#    (or edit the volume's copies of usb/hook.sh and usb/gbos.cfg
#    directly with the same changes; the patch was verified to apply
#    cleanly to upstream at HEAD 284769a and hook.sh passes sh -n.)
#    No gbos.cfg change is needed to enable it: FINGERPRINT=1 is the
#    default, and the patch extends the sensor check to 27c6:533c.

# 4. Reboot.
reboot
```

Notes:
- The SELinux label `u:r:hal_fingerprint_default:s0` is set by the
  init service definition hook.sh generates, not by file attributes —
  there is nothing to `chcon`.
- The service definition, data directory
  (`/data/vendor/crosfp/gbos_fp`, `0770 system system`), user/group
  (`system` / `system usb`), and `--log` path are all unchanged from
  the upstream 6384 HAL.
- The `cdc_acm` unbind is done by hook.sh as root before the service
  starts. The HAL also tries (`gm_unbind_cdc_acm`) but runs as user
  `system` and usually cannot write the sysfs node — that warning in
  the log is expected and harmless.

## Milestone 1 — boot / probe

After reboot, check the HAL started and probed the sensor:

```sh
adb shell ls -l /postinstall/gbos/gbos_fp
adb shell logcat -d -s gbos_fp | head -50
# and/or read logs/boot-N/fingerprint.txt on the GBOS volume
```

Expect (exact strings):

```
sensor ... firmware GF5288_GM168SEC_APP_13016, install ...
533c: firmware GF5288_GM168SEC_APP_13016
533c: chip id 0x00220ca1
533c: tcode 0x.. dac_h 0x.. dac_l 0x.. (otp: ...)
```

And in Settings > Security, "Fingerprint" appears (or
`adb shell dumpsys fingerprint` shows the HAL). If instead you see:

- `533c: firmware '...' is not GM168/GM168SEC` — the firmware on
  your sensor is not what the port was written against. Stop and
  report the firmware string.
- `533c: sensor PSK hash is not the held PSK's hash` — see Gate 0.
- `533c: claim interface 0 failed: ... (cdc_acm may still own it)` —
  the `cdc_acm` unbind did not happen. Check the hook patch applied
  and `dmesg` for the sensor's USB node.
- `Goodix 27c6:6384 not present` (old message) — the hook patch is
  not applied; the service never started.

If Milestone 1 fails, **do not attempt enroll**. Capture
`logs/boot-N/fingerprint.txt` and the logcat excerpt and report them.

## Milestone 2 — enroll

Settings > Security > Fingerprint > Add fingerprint. 12 touches
(the HAL logs `user 0: enroll 0 starting as ...`, one progress event
per accepted sample, then `user 0: enrolled 0`). A bad image logs a
retry — lift and touch again; that is normal.

After enroll, a template file should exist:

```sh
adb shell ls -l /data/vendor/crosfp/gbos_fp/
# tpl-GBOS-<instance>-u0-0.bin
```

Duplicate check: enrolling the same finger again should refuse with
"finger already stored" (`enroll failed: finger already stored on
the sensor`). Enrolling a different finger should succeed.

## Milestone 3 — lock-screen authenticate

Lock the device, touch the sensor. Expect `user 0: authenticate
starting (1 enrollments)` then `user 0: accepted enrollment 0`, and
the device unlocks.

**Watch for the 6384 boot-13 pattern**: one touch producing several
rapid `onAuthenticationFailed` lines (~290 ms apart) followed by a
30 s lockout. The 533c code waits for finger-up before returning
specifically to prevent this, but this path has never run against a
real sensor. If you see rapid repeated failures from one touch,
capture the full `fingerprint.txt` for that boot and stop testing
authenticate — enroll data is still valid, the bug is in the
identify return path.

## Logs to capture (every milestone)

- `logs/boot-N/fingerprint.txt` on the GBOS volume (the HAL's own
  log, one line per event; this is the single most useful file).
- `adb shell logcat -d -s gbos_fp` (same lines, via logcat).
- `adb shell dmesg | grep -i -E "usb|goodix|27c6"` (USB claim /
  `cdc_acm` issues).
- `adb shell ls -l /data/vendor/crosfp/gbos_fp/` after enroll.
- The exact `gbos_fp` binary used (Milan vs stub) and the boot
  number.

## Revert

Either of these, then reboot:

```sh
# Restore the original HAL binary:
unshare -m sh -c 'mount -o remount,bind,rw /postinstall &&
  cp /postinstall/gbos/gbos_fp.orig /postinstall/gbos/gbos_fp'

# ...or disable the HAL entirely (keeps the 533c binary in place):
# set FINGERPRINT=0 in usb/gbos.cfg on the volume, then reboot.
```

Reverting the binary restores the upstream 6384 HAL (which will log
`Goodix 27c6:6384 not present` on the 9310 and do nothing, exactly as
before). Template files in `/data/vendor/crosfp/gbos_fp/` are inert
once the HAL is reverted; delete them if you want a clean slate
(`tpl-GBOS-*.bin`, plus `milan-state.bin` if present).
