# Notes for agents working on gbos-x86

Read `README.md` (what/how) and `HANDOFF.md` (chronological log of every bug and fix) first.

## Hard rules
- **Never erase the user's NVMe without them typing the serial on the laptop.** Don't add bypasses.
  `gbos_install_confirm=` on the kernel cmdline exists for QEMU tests only.
- **The repo is public (owner's decision, 2026-10-16).** `usb/patched/` and `usb/libkm_compat.so` are modified Google binaries and stay in
  it knowingly. Never commit Google's OS images (`lapis/parts`, `*.img`), keys, tokens, server addresses or personal data.
- Ask before downloading anything (firmware, images) and say what, from where, and how big.
- Releasing an update from main (main repository only, never the lab): `tools/release.sh`, `check`, then `publish` (skill `.claude/skills/gbos-release`). Never pass
  `--skip-tests`/`--allow-stick-only`/`--yes` unless the maintainer asks; they type the key passphrase themselves.
- After changing a file on the stick, make the same change in `usb/` here, or copy it from here (they must
  match), and commit.

## Workflow with the user
- The laptop has no shell/adb. All evidence comes from logs on the stick. Ask the user to boot the stick
  and choose **1 (Update)**, which copies the NVMe install's last 3 boots to
  `gbos/logs/boot-N/nvme-logs/` on the stick. Then plug the stick into the Mac.
- Deploy = `tools/deploy_stick.sh` (copies only changed files, verifies after a remount, ejects the right disk: seconds), then the user boots the stick and picks **1**.
  The updater skips the unchanged big items (system image, firmware, modules; `UPDATE_FULL=1` in gbos.cfg forces everything), so an Update is about a minute, and it saves the previous boots' logs first: one stick boot both reads the last test and applies the next change (no separate "save logs" boot). Kernel changes go to
  **both** `/Volumes/EFI/EFI/BOOT/BOOTX64.EFI` (what the firmware boots) and `GBOS/EFI/BOOT/`.
- On a stick that holds a published update set (`gbos/system.manifest`, e.g. the persistent lab stick) `deploy_stick.sh` refuses and lists
  what differs: ship the change as an update (`tools/publish_system_update.sh`, bundle into `gbos/update.inbox`) or pass `--over-set`.
- The stick mounts on macOS as `/Volumes/EFI` and `/Volumes/GBOS`; the disk number changes. Eject with
  `diskutil eject` after `sync`.

## Traps that already cost hours
- **Absence of an error is not success.** Check the log actually covers the time of the event (logs were
  truncated by FAT corruption, and by booting an old kernel from the wrong EFI copy).
- **Confirm the running kernel:** `Linux version ... #N` in `kmsg.log`.
- **FAT logs:** reading past a file's real cluster chain gives zeros or EINVAL; copy with
  `dd conv=noerror,sync` and strip NULs. Never let Android (vold) mount the stick.
- **init `exec`/`service` need an explicit `seclabel`.** Init refuses an exec with no domain transition
  even in permissive mode.
- **`$` in an init `.rc` line is a property reference.** Don't use shell variables there.
- **The firmware loader path:** modules probed in stage0 (before switch_root) load firmware from `$G/firmware`;
  after switch_root the stick is at `/postinstall/gbos/firmware`.
- **Google's audio HAL (`dras`)** matches `<soundcard>` against the ALSA card **id** (`PCH`), and rejects a
  config section that is missing any field the sof-soundwire config has.
- **Hash-pinned patches:** `hook.sh`/`apex_patch.sh` only bind a patched library when the original's
  sha256 in `usb/patched/MANIFEST` matches. A new Google build disables them; look for `NOT patched` / `PATCH-STATUS` in
  `stage0-dmesg.txt`. Never edit the MANIFEST or the patched files by hand: `tools/port_build.sh --write` regenerates them (see `porting.md`).
- **drm_hwcomposer is written for Google's xe driver.** On i915 it silently falls back (e.g. `LOG_GAMMA_LUT` → `GAMMA_LUT`,
  logged every frame by `drmhwc`). Check `drmhwc` lines in logcat before blaming the panel or the colour pipeline.
- **Trusty-backed services** (Weaver, Gatekeeper, Secretkeeper, KeyMint, GSC) never answer here. A hang
  that waits on one ("Waited one second for ...") means hide or replace it in `hook.sh`.

## Where things run
- Kernel builds: docker volume `gbos-ksrc` (`/build/src`, `/build/out`), image `gbos-kbuild`,
  `kernel/isa_emu_patch/build.sh`. Modules for kernel #29 must come from the same tree.
- Emulator test: `kernel/isa_emu_test/run_qemu.py <bzImage>` must print `ISA_EMU_TEST_PASS`.
- Shell syntax checks: `sh -n usb/hook.sh usb/install.sh usb/stage0_init.sh usb/apex_patch.sh`.

## Commits
End messages with `Co-Authored-By: Example Model <noreply@example.com>` (replace example names with your model's name and provider url, example: "Big Pickle Free <noreply@opencode.ai>"). Add a dated line to
`HANDOFF.md` for anything learned on hardware.
