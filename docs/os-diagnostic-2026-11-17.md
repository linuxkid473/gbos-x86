# gbOS86 OS diagnostic: Latitude 7320 Detachable, 2026-11-17

Source: `gbos/logs/boot-3` from a **live** stick boot (kernel #61, 5/5 patches applied,
`DEBUG_LOGS=1`, front camera bridge off, rear capture probe on). It ran for 255 s and ended with a
user-requested shutdown. Earlier boots the same day (boot-1, boot-2) are cited where they add something.
These findings come from logs only. Nothing was measured under sustained load, and an NVMe install may differ
from a live boot.

## Health summary

| Check | Result |
|---|---|
| Java crashes (`FATAL EXCEPTION`), native crashes (`Fatal signal`), tombstones | 0 |
| ANRs, system_server watchdog | 0 |
| Kernel `WARNING`/`BUG`/`Oops`, hung tasks | 0 |
| GPU hangs or resets (i915) | 0 (search hits were KMS debug lines containing "reset") |
| Kernel OOM kills, lmkd kills | 0 |
| Thermal throttling messages | 0 |
| Firmware load failures, Wi-Fi/Bluetooth errors | 0 |
| Composition | 3,741 frames, 79 missed (2.1%: 51 HWC, 49 GPU), 1 failed test-commit |
| Slow `startProcess` operations | 50, all 58–95 ms (ActivityManager warns above 50 ms) |

The system is stable in this boot. The items below are things to improve, not failures.

## Boot timeline

| Time (s) | Event |
|---|---|
| 0.85 | Kernel init done |
| 4.1 → 6.5 | Stage0 starts the screen reader |
| 6.5 → 9.5 | Screen reader waits for a sound card and gives up ("no sound card, no speech") |
| 9.5 → 14.0 | Boot menu (`MODE=ask`), choice 1 |
| 14 → 27 | hwdetect: 4 coldplug passes with 1 s sleeps, camera PMIC/IPU6, a second coldplug round |
| 28.9 | switch_root to Android init |
| 38.6 | zygote starts |
| 47.2 → 57.0 | Boot animation |
| 120.1 | `sys.boot_completed=1` (includes time at the lock screen; a fingerprint/PIN was enrolled) |

## Stability findings

**S1. The stick's FAT partition is dirty at every boot.**
`FAT-fs (sda2): Volume was not properly unmounted` appears at 14 s. Android's shutdown unmounts APEXes and
its own partitions but never the stick partition that stage0 mounted, and live logs keep writing to it until
power-off. Stage0's `sync` every 5 s limits the damage, but HANDOFF already records log truncation from FAT
corruption.
*Improve:* add a GBOS `on shutdown` action that stops `gbos_logcat`/`gbos_crashlog` and the kmsg copier,
runs `sync`, and remounts the stick read-only where possible. Separately, run a read-only
`fsck.fat -n` in stage0 and log the result, so corruption gets noticed. Never let vold repair it (HANDOFF).

**S2. The IPU6 firmware stream stop times out on both sensors.**
`stream stop time out` / `stream close time out` happens for the front OV5675 (2026-11-15) and the rear
OV8856 (boot-3). The bridge works around it by streaming until reboot. That is stable, but it costs power
and CPU (see P1), and only a reboot frees a sensor once it has stopped.
*Improve:* look into the firmware stop sequence in `intel_ipu6_isys` (an upstream issue on Tiger Lake). Until
then, keep streaming continuously and never stop or restart a stream in normal use.

**S3. SoundWire errors at boot.**
`soundwire_intel link.2: Bus clash` (11 times) and `Clock stop failed -110` appear around 25–28 s in every
boot. Audio works, but these errors usually mean the codec is enumerating while the link is still
unstable.
*Improve:* compare with a cold boot vs warm reboot, and check whether a later `snd_sof` load (after the
TPS68470/camera PMIC settles) removes them. Watch for audio dropouts after resume.

**S4. Missing lazy services cause polling loops.**
servicemanager asks init to start services that do not exist about once a second:
`storage_metrics_service` polls `com.google.android.snapshot.ISnapshot` 73 times, and the audio HAL polls
`com.android.desktop.video_chat.IVideoChatManager` 67 times. Each lookup wakes init and logs at error
level.
*Improve:* in `hook.sh`, hide those VINTF/interface declarations or disable `storage_metrics_service`,
following the same approach as the Trusty-backed services (AGENTS.md).

**S5. Live mode keeps `/data` on tmpfs.**
This is expected ("nothing is saved"), but it has two consequences. Enrolled fingerprints and settings are
gone after a live reboot. fs-verity is unavailable, which produces 1,780 `ResilientAtomicFile` and
`AccessPersistence` error stack traces. The NVMe install formats `/data` with `-O verity` and does not get
these errors.

**S6. Security posture.**
SELinux is permissive (`androidboot.selinux=permissive`), and GBOS HALs (`gbos_fp`, the camera bridge) run
from the stick's VFAT partition. Enforcing mode is blocked until those binaries move to a labelled filesystem
and get their own policy. That affects the fingerprint reader, which now handles authentication.

## Performance findings

**P1. The camera bridge converts frames even with no viewer** (front camera enabled).
`ipu6-loopback` converts and writes every 2592×1944 frame at 30 fps whether or not anything has
`/dev/video64` open. With the RGB-IR 4x4 demosaic it measures 11.6 ms per frame single-threaded on the same CPU
model (i7-1180G7): about 35% of one core, all the time, plus IPU6 and sensor power.
*Improve:* skip the conversion while no reader has the loopback device open. Keep dequeuing and requeuing
raw buffers, so the sensor keeps streaming (S2) and the CPU stays idle. Highest return for battery life.

**P2. The screen reader adds ~5.4 s to every boot on this laptop.**
The laptop's audio is SOF/SoundWire (PCI class `0x040100`), so no ALSA card can exist in stage0, and the
screen reader always ends with "no sound card".
*Improve:* when no HDA controller (class `0x0403xx`) is present, skip the sound-card wait at once (or skip
the screen reader with a logged reason). `A11Y=0` already does this by hand.

**P3. Fixed sleeps in hwdetect.**
Coldplug runs up to 4 passes with a fixed `sleep 1` each, then another round. 14 → 27 s is mostly waiting.
*Improve:* break as soon as a pass loads no new modules and no uevents are pending, and poll at 200 ms
instead of 1 s. Expect 3–6 s saved; measure with the existing `hwdetect: coldplug pass` log lines.

**P4. The kernel console prints every message.**
The built-in command line (`kernel/isa_emu_patch/generic_cmdline.txt`) uses `loglevel=8 ignore_loglevel`
with `console=tty0`, so every message, including `drm.debug=0x04` KMS output until boot completion (1,017
lines), is drawn on the framebuffer console during boot.
*Improve:* for non-debug builds, use `loglevel=4` without `ignore_loglevel`. `kmsg.log` still records
everything, because it reads `/dev/kmsg`, which ignores the console level. Needs a kernel rebuild (embedded
command line). Keep the current line in `debug_cmdline.txt`.

**P5. Log volume with `DEBUG_LOGS=1`.**
Logcat produced 81,351 lines in 131 s (~620 lines/s), rotating 10 × 1 MB files on the USB stick, plus the
5-second `sync`. The top sources:
- `drmhwc` V "Missing optional LOG_GAMMA_LUT property, falling back to GAMMA_LUT": 5,745 lines, once per
  frame. `hook.sh` sets `log.tag.drmhwc=WARN` only on the software-rendering path.
- Settings looking for the missing `com.google.android.setupwizard.partner` provider: 2,846 lines, plus
  `PartnerConfigHelper`.
- MediaProvider failing to resolve ringtone titles from the absent `com.google.android.soundpicker`.

*Improve:* use `DEBUG_LOGS=0` for everyday use. Set `log.tag.drmhwc=WARN` on the hardware-GPU path too (at
least when `DEBUG_LOGS=0`); warnings stay visible, which is what AGENTS.md relies on.

**P6. Composition misses.**
2.1% of frames were missed (51 by HWC), and `drmhwc` falls back from `LOG_GAMMA_LUT` to `GAMMA_LUT` on
every frame. Worth a look once the logging noise is gone: drm_hwcomposer targets xe, and this machine uses
i915 (AGENTS.md).

## Suggested order

1. P1: bridge idles with no reader. Small, self-contained, testable on the host, big battery win.
2. P5 and P2: drmhwc log level, `DEBUG_LOGS=0` for daily use, screen-reader skip on SOF-only machines.
   Script-only changes.
3. S1: clean stick shutdown and an fsck report. Protects the logs everything else depends on.
4. S4: stub or hide the polled services.
5. P3 and P4: faster coldplug, quiet console (P4 needs a kernel rebuild).
6. S2, S3, S6, P6: deeper investigations.

## Implemented (2026-11-17, not yet tested on hardware)

| Item | Change | Test |
|---|---|---|
| P1 | `ipu6-loopback` reads v4l2loopback's `readers_active` control each frame. With no consumer it only samples statistics (exposure stays right), skipping the ~12 ms conversion and the write. It logs every change between idle and converting. If the control is missing, it converts every frame as before. | `test_ipu6_loopback.c` |
| P5 | `hook.sh` sets `log.tag.drmhwc=WARN` on every GPU path, not just software rendering. `DRMHWC_VERBOSE=1` keeps the vendor level. | `test_boot_tuning.sh` |
| P2 | Stage0 skips the screen reader (copy to RAM plus card wait, ~5 s) when no PCI class `0x0403xx` HDA controller exists, and logs why. `A11Y=0` still works. | `test_boot_tuning.sh` (SOF `0x040100`, HDA `0x040300`, HDA+DSP `0x040380`) |
| S4 | `hook.sh` bind-mounts filtered copies of the system manifests without `ISnapshot/default` and `IVideoChatManager/default` (`IAqueduct` stays). It only acts when the file exists and still declares that exact fqname. `HIDE_UNIMPLEMENTED_HALS=0` turns it off. | `test_boot_tuning.sh` with the real manifest contents. **Reverted 2026-11-22:** system_server implements both (desktop-services.jar), and the hidden declarations made its registration fail; see HANDOFF. |
| P3 | `gbos_coldplug` sleeps 1 s only after a pass that loaded new modules (saves ~1.1 s in the hook's second coldplug). The 4-pass cap and the settle after productive passes are unchanged. | `test_boot_tuning.sh` (settling, empty, capped) |

Not implemented:
- **S1** (clean stick unmount): implemented 2026-11-18 in `usb/stick_fat.sh` (not with an `on shutdown` action, which
  runs before init stops the log writers: a watcher started by `hook.sh` remounts the volume read-only at init's
  `sync() before umount` step, and `hook.sh` logs a `FAT-CHECK` line plus `fat-check.txt` with a read-only
  `fsck.fat -n`). See HANDOFF.md 2026-11-18.
- **P4** (quiet kernel console): implemented 2026-11-19 in kernel #62 (`generic_cmdline.txt`: `loglevel=4`, no
  `ignore_loglevel`; the stage0 screen reader now restores the levels it found instead of forcing `ignore_loglevel` on).
- **S2, S3, S6, P6**: investigations, not quick changes.

Every suite in `tools/` passes except three, none caused by these changes:
- `test_camera_fw_trace_kernel.sh` and `test_sensor_overlay_mount.sh` need a privileged container.
- `test_ui_defaults.sh` fails identically on a clean checkout of `5380e2b`.

## Windows-assisted follow-up (same laptop)

| Area | Windows data | Status |
|---|---|---|
| Camera white balance | Calibrated illuminant white points (OV5678 tuning file) | Grey-world constrained to the locus (done) |
| Thermal HAL | DTT participants `TCPU TSKN TMEM CHRG AMBF NGFF` vs Lapis config `TSR0-4`, `TCPU` | Zone report added to `hwdetect.txt`; config after one boot |
| Light sensor, gyroscope | Six HID sensor collections; Linux has `als`, `gyro_3d` and more | Needs a sensor sub-HAL rebuild |
| Rear camera colour | `OV8856_0BA801T3_TGL.aiqb` colour matrices extract | For when the rear camera is exposed to Android |
