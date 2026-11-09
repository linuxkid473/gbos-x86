# gbos-x86 (gbOS86)

<p align="center"><img src="docs/demo.gif" alt="gbOS86 running on a Dell Latitude 5300" height="420">&nbsp;&nbsp;<img src="docs/screenshot-dell-latitude-5300.jpg" alt="Googlebook OS on a Dell Latitude 5300" height="420"></p>
<p align="center"><b>Googlebook OS on a Dell Latitude 5300.</b> Full 43 s demo: <a href="https://gbos86.com/static/demo.mp4">gbos86.com/static/demo.mp4</a></p>

Google's **Googlebook OS** (the `lapis` Android desktop build) running on an ordinary laptop: a
**Dell Latitude 5300** (Intel i5-8365U "Whiskey Lake", UHD 620, 16 GB). It boots from a USB stick or
from the internal NVMe/eMMC, reaches the desktop, and runs Play Store apps.

**Website and community:** https://gbos86.com (guides, hardware list, forum) | Discord: https://discord.gg/h9AD9fSnDT

> `usb/patched/` and `usb/libkm_compat.so` are modified copies of Google's proprietary libraries, included
> because the build needs them; Google's terms apply to them. The OS images themselves are not in the repo (they are
> GBs and Google's); you rebuild them from Google's official recovery download. gbOS86 is an independent project, not
> affiliated with or endorsed by Google.

## Try it: build and flash in 3 steps

You need macOS or Linux with **docker** (running), **python3**, curl and about **35 GB** of free disk. Nothing from Google is
stored in this repository: the script downloads Google's public recovery image itself.

```sh
git clone https://github.com/linuxkid473/gbos-x86 && cd gbos-x86
./build.sh
```

When you run `./build.sh`, it prompts you to select the image type:
1. **Host-tailored auto image**: Auto-probes the host hardware (CPU, AVX2, GPU, display resolution and optimal UI density) and tailors `gbos.cfg` automatically.
2. **Standard image (`dist/gbOS86-YYYYMMDD.img`)**: Live USB installer. Boots into the gbOS86 boot menu (`MODE=ask`), allowing you to run live in RAM or install/update on the internal SSD.
3. **Loopback persistent image (`dist/gbOS86-looper-YYYYMMDD.img`)**: Portable standalone OS. Boots directly into Android (`MODE=boot`) with persistent ext4 `/metadata` and `/data`. All installed apps, accounts, and settings persist directly on the USB stick.

You can also pass CLI arguments directly:
```sh
./build.sh --auto                      # Auto-detect host hardware and build a tailored image
./build.sh --standard                  # Standard installer image
./build.sh --looper --data-size 64     # 64 GB sparse userdata (only ~6.5 GB on host disk)
```

Then flash the generated `.img` file from `dist/`:
```sh
# Flash with balenaEtcher, Rufus, or dd:
sudo dd if=dist/gbOS86-*.img of=/dev/rdiskN bs=4m status=progress
```
Boot the laptop from the stick (**F12** at the logo, Secure Boot off).

`./build.sh --dry-run` shows what it would download (Google's 7.7 GB image, ~1 MB busybox, 14 MB of kernel modules and 70 MB of firmware
from this repo's [release](https://github.com/linuxkid473/gbos-x86/releases)). It streams only the six partitions it needs out of
Google's zip, so the 19 GB disk image inside is never unpacked. Allow 30 to 60 minutes, mostly the download. Twenty-one machines have been
tried (see [Tested laptops](#tested-laptops)); if you try another, please post its `gbos/logs/boot-N/hwdetect.txt` in an issue.
Fedora/RHEL with SELinux enforcing: the scripts add `:z` to the docker bind mounts of this repo's own folders automatically; don't disable SELinux.

## Status

| Works | Notes |
|---|---|
| Boot to desktop, setup, Google sign-in | From USB or internal NVMe/eMMC |
| Persistent data | NVMe/eMMC install or USB looper image: `/data` and `/metadata` are ext4 partitions |
| Wi-Fi | Intel 9560 (CNVi `8086:9df0`) + linux-firmware |
| Bluetooth | Intel 9560 USB (`8087:0aaa`) via our `btusb` |
| Sound | Legacy HDA + Realtek ALC3254, speakers, headset jack, internal mic, HDMI |
| Touchpad with scrolling/gestures, keyboard | Intel LPSS I2C HID |
| Battery / AC | ACPI battery |
| Play Store installs | With the patched libcrypto (SHA-256 no longer emulated) |
| Screen-lock password | Software Gatekeeper (`gatekeeper/soft_gk.c`) |
| Root for apps (KernelSU) | Built into the kernel; the manager app is installed on first boot, root is granted per app in it |
| Correct display colours / black levels | Kernel #29: i915 ignores the HWC's gamma tables (see below) |

| Not done / limited | |
|---|---|
| Speed | Some instructions are still emulated in the kernel (see "Why it's slow") |
| Night Light | Gamma tables from userspace are ignored (that's the colour fix), so it has no effect |
| Linux Terminal (AVF) | Secretkeeper hang fixed; VM boot not yet confirmed |
| Auth-bound keystore keys | `soft_gk`'s auth tokens aren't trusted by the software KeyMint |
| Google's on-device AI (Magic Pointer, AICore) | Google's servers refuse it without hardware key attestation, which no non-Google laptop can provide |
| OS updates | Built-in OTA can't work; moving to a new Google build is semi-automated: `tools/port_build.sh`, see [`porting.md`](porting.md) |
| Other laptops | Eight of twenty-one tried machines work fully, ten partly, one pending, two not; see [Tested laptops](#tested-laptops). The CPU-emulation kernel should help any pre-2021 Intel CPU |

## How it works

```
UEFI firmware
 └─ EFI/BOOT/BOOTX64.EFI      our kernel (android17-6.18 + patches + KernelSU) with stage0 built in as initramfs
     └─ stage0 (usb/stage0_init.sh, busybox)
         ├─ finds the gbos volume: USB stick (FAT label GBOS) or system volume (GBOSSYS)
         ├─ loads i915, logs to gbos/logs/boot-N/
         ├─ MODE=install → gbos/install.sh (NVMe/eMMC installer/updater), never boots Android
         ├─ mounts super.img's erofs partitions (system, vendor, product, ...) via loop + dm-linear
         ├─ runs gbos/hook.sh (all Android-side workarounds, editable without a kernel rebuild)
         ├─ mounts /data, /metadata (ext4 partitions if present, else tmpfs)
         └─ switch_root → /system/bin/init selinux_setup   (Google's Android init, unmodified)
```

**The core problem:** lapis binaries use CPU instructions this CPU lacks: GFNI, AVX-VNNI, SHA-NI,
256-bit VAES/VPCLMULQDQ, AVX-IFMA. Two layers deal with that:

1. **Kernel emulation** (`kernel/isa_emu_patch/`). The `#UD` (invalid opcode) handler decodes the
   instruction, emulates it on the saved FPU state (`isa_emu_core.c`, 13k known-answer tests), and
   continues. It is correct but slow, about a microsecond per instruction.
2. **Binary patches** for the hottest code (`usb/patched/`, bind-mounted by `hook.sh`, and only
   applied when the original's sha256 matches):
   - `libz.so`: `adler32_z` always takes its scalar path (was ~50% of all emulation).
   - `libcrypto.so` ×3 (system, vendor, Conscrypt APEX): SHA-NI SHA-256 → scalar SHA-256 (was ~50%).

   **When they are bound.** `usb/early_patch.sh` (sourced from `gbos.cfg`) wraps `chroot` in stage0 and binds the
   system/vendor `libz` + `libcrypto` just before the first `dmctl` call. Without it, every `dmctl` process
   (one per partition) loads libcrypto and runs BoringSSL's load-time module hash through SHA-NI, about 3.7M
   emulated instructions, ~2 s of boot, before `hook.sh` could bind anything. `hook.sh` still does the same binds
   later as a fail-safe and logs `already patched (early_patch.sh)` when the early pass got there first. Both
   are gated on the sha256 of the exact original, so a new Google build silently disables them
   (`NOT patched` in `stage0-dmesg.txt`; the hashes live in `usb/patched/MANIFEST`, regenerated by `tools/port_build.sh`, see [`porting.md`](porting.md)).
   *To verify on hardware:* `stage0-dmesg.txt` must show `early: patched /new/system/lib64/libcrypto.so` before
   `running gbos/hook.sh`.
   *Shipping:* `early_patch.sh` is in `install.sh`'s `FILES` (so **1 Update** copies it to the NVMe) and in the
   image builder. The `gbos.cfg` source line is guarded by `[ -f ... ]`, so a stick without the file still boots.

**Google hardware the laptop doesn't have**, mostly the GSC security chip and the pKVM Trusty VM:
`hook.sh` hides or replaces each service that would otherwise hang the boot:

| Symptom | Cause | Fix (in `usb/hook.sh` unless noted) |
|---|---|---|
| "System process not responding" | LockSettings waits forever for Weaver (Trusty) | hide Weaver's VINTF declaration |
| Setup crashes ("Failed to create shared Bitmap") | no `/dev/ashmem` (Rust ashmem driver, `RUST=n`) | `kernel/ashmem_shim/` (C memfd ashmem ioctls) + `sys.use_memfd=true` |
| Setting a password hangs Settings | Trusty Gatekeeper never answers | `gatekeeper/soft_gk.c` software Gatekeeper |
| keystore2 aborts / KeyMint hangs | Trusty/StrongBox KeyMint need the GSC | patched `libkm_compat.so` (software KeyMint), StrongBox APEX hidden |
| Linux Terminal never starts its VM | Trusty Secretkeeper never answers | hide Secretkeeper's VINTF declaration |
| Logs truncated, stick corrupted | vold `fsck_msdos`'d the stick while mounted | remove vold's `voldmanaged=usb` fstab rule |
| No sound | Google uses SOF; our kernel has legacy HDA | `usb/audio/` config for the `dras` audio HAL (card id `PCH`) |
| White veil, lifted blacks (even video) | drm_hwcomposer loads its xe `LOG_GAMMA_LUT` curve into i915's linear `GAMMA_LUT` every frame | `kernel/isa_emu_patch/i915_ignore_gamma.sh` (kernel #29) |
| Everything too big | `ro.sf.lcd_density=260` for Google's panel | `RESOLUTION` in `gbos.cfg` (default 2400x1350, stock density); `DENSITY` optional |

Laptop-specific drivers that Google's vendor modules can't provide (they're built for Google's kernel):
battery/AC, Intel LPSS I2C (touchpad), AMD pinctrl/GPIO (I2C touchpad interrupts), legacy HDA audio and
`btusb` are enabled in our kernel (`kernel/configure_whl.py`). Wi-Fi and Bluetooth firmware are in
`usb/firmware/`.

**Root for apps (KernelSU).** [KernelSU](https://github.com/tiann/KernelSU) is built into our kernel
(`kernel/kernelsu/apply.sh` puts the sources in `drivers/kernelsu`, `CONFIG_KSU=y`), so apps can be
given root the usual Android way: the manager app is installed on the first boot (from the stick,
`usb/ksu/`), and it decides which app may use `su`. Two things are worth knowing:

* The kernel only accepts a manager APK whose signing certificate is the official one
  (827 bytes, sha256 `c371061b…`), so a Play Store copy of the app is **not** accepted: the APK comes
  from the KernelSU release (`build.sh` puts it in `usb/ksu/manager.apk`). The daemon the kernel
  starts (`/data/adb/ksud`) is in `usb/ksu/ksud` (the x86_64 Android build).
* Since Linux 6.9 x86 dispatches syscalls through a switch statement instead of the syscall table,
  so patching the table (what KernelSU's hooks do) has no effect. KernelSU's own answer to that is
  `CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y`, which patches the `x64_sys_call` dispatcher at runtime;
  it is on in `kernel/configure_whl.py`. Nothing else in the kernel is changed, and SELinux stays
  permissive (KernelSU is built in, not loaded late, so it does not switch SELinux to enforcing).

`HANDOFF.md` is the chronological engineering log: every bug, its evidence and its fix. Read it before
changing anything.

## Tested laptops

Reports from people who booted the stick (2026-10-18). A row says what was reported, not what the newest build can do. The same list is on
[gbos86.com/hardware](https://gbos86.com/hardware); the source of truth is [`website/laptops.py`](website/laptops.py).

| Laptop | CPU | Result | Works | Does not work / notes |
|---|---|---|---|---|
| Dell Latitude 5300 | Core i5-8365U, 16 GB | **Works** | Everything tested: Wi-Fi, Bluetooth, sound, touchpad, brightness, battery, install | Suspend. The maintainer's machine |
| Acer Aspire Lite AL15-52 | Core i5-1235U, 8 GB | **Works** | Wi-Fi, display, touchpad, keyboard, sound, brightness, camera, Bluetooth, SD card | Needed two firmware fixes (Realtek Wi-Fi; Intel AX201 Bluetooth name `ibt-0040-1050.sfi`), both in firmware v34+. Older sticks need a rebuild or an Update from a fresh stick |
| Lenovo ThinkPad T470 | Intel 7th gen | **Works** | Reported as working with no setup | Short report |
| Intel NUC8BEH (mini PC) | Core i3-8109U, 8 GB | **Works** (live) | Wi-Fi, Bluetooth, display and brightness (external monitor), keyboard (external), USB ports | Sound not tested. SATA M.2 SSD: install support for SATA disks added afterwards (QEMU-tested only, not yet tried on this NUC) |
| HP Envy 13-aq0xxx | Core i5-8265U, 8 GB | **Works** | Reported as working great, live and installed to the NVMe (kernel #32) | Per-device detail is in the forum thread |
| Dell Latitude 5300 2-in-1 | Core i7-8665U, 16 GB | **Works** | Wi-Fi, Bluetooth, sound, touchpad, backlight, display, touchscreen, hinge, webcam, pen, suspend | Auto-rotate (sensor driver added to the next kernel), mic-mute LED |
| Dell Latitude 5290 2-in-1 | Core i5 (model not given) | **Works** | Sound, once the speaker volume is raised | Short report: speakers were very quiet, `Master Playback Volume` 87 in `gbos-hda-pch/mixer_paths.xml` fixed it (shipped from 2026-10-26) |
| Lenovo ThinkPad T480 | Core i5-8350U, 16 GB | **Works** | Wi-Fi, Bluetooth, sound, touchpad, touchscreen, keyboard, brightness, display, webcam (installed) | Slow to wake; Wi-Fi needs a toggle after sleep |
| Dell Latitude 3380 | Core i5-7200U, 8 GB | Partly | Wi-Fi, sound, touchpad, keyboard, brightness, display, webcam (installed) | Bluetooth (Qualcomm 0cf3:e007: firmware loads, `hci0` up, cause unknown; needs `logcat.txt`). Kernel #42 |
| HP OmniBook 7 | Core Ultra 5 225U, 16 GB | Partly | Touchpad, keyboard, brightness, display | Wi-Fi (Intel BE201). Sound, webcam, Bluetooth not tested |
| HP Pavilion Power 15 | Core i7-7700HQ + GTX 1050, 16 GB | Partly | Wi-Fi, Bluetooth, touchpad, brightness, display, webcam | Speakers (fixed in the next update: audio config picked the NVIDIA HDMI card); dock/DisplayLink and the NVIDIA GPU have no driver |
| Lenovo ThinkPad T14 Gen 2 | Core i5-1135G7, 32 GB | Partly | Wi-Fi, Bluetooth, touchpad, keyboard, brightness, display (installed) | Sound, microphone, webcam. Needs a full `hwdetect.txt` |
| Microsoft Surface Pro 4 | Core i5-6300U | Partly | Screen, keyboard, touchpad, brightness, microphones | Wi-Fi (Marvell 88W8897: driver added to the next kernel), Bluetooth, touchscreen, cameras |
| Dell Inspiron 7391 2-in-1 | Core i7-10510U, 8 GB | Partly | Wi-Fi, Bluetooth, touchpad, keyboard, brightness, display, webcam, touchscreen (installed to NVMe) | Sound. The reporter notes this laptop uses SOF audio firmware on regular Linux |
| Realme Book Slim (2021) | Core i5-1135G7, 8 GB | Partly | Wi-Fi, Bluetooth, touchpad, keyboard, brightness, display, webcam (live USB) | Sound. No `hwdetect.txt` yet |
| Fujitsu Arrows Tab V727 (tablet) | Core m3-7Y30, 8 GB | Partly | Wi-Fi (Intel 8265), Bluetooth, sound card detected (HDA Intel PCH); live USB on kernel #33, USB 2.0 | Tagged "mostly works"; what fails is not listed in the report |
| Dell XPS 16 (2026) | Intel Core Ultra | Partly | Boots and runs | Screen refresh rate, Wi-Fi, camera. A kernel option reportedly fixes the screen: try it with `MODOPTS` in `gbos.cfg` |
| Framework Laptop 13 Pro | Core Ultra 5 325 (Panther Lake), 16 GB | Fix pending | Boots | Black screen on sticks without the Panther Lake `xe` firmware (GuC); firmware v35 has it. Awaiting confirmation |
| Microsoft Surface Laptop 7 (Intel) | Core Ultra 7 266V (Lunar Lake), 16 GB | Partly | Wi-Fi, keyboard, battery, speakers, headset jack, touchpad, Bluetooth | Microphone, HDMI audio, camera, touchscreen, suspend. Needed extra kernel and boot-script work (section below) |
| ASUS X441NA | Celeron N3350 | Does not work | | No AVX on this CPU: Google's init dies on its first instruction (kernel panic, exit code 4). Not fixable; newer sticks stop with a message |
| HP 14-ne0013dx | Intel | Does not work | | No logs yet |

Pattern: 2017 to 2022 business and mainstream laptops work with little setup (Wi-Fi, Bluetooth, touchpad, display, webcam). The common gap on 10th/11th-gen machines is **sound** (Inspiron 7391, Realme Book Slim), which on those uses the SOF driver; newer Core Ultra machines boot but hit missing firmware (`xe`)
or Wi-Fi 7 cards without a working driver and firmware pair. Missing firmware is listed in `hwdetect.txt` under "missing firmware".
To try a driver option on your own laptop, put e.g. `MODOPTS="xe.enable_panel_replay=0"` in `gbos/gbos.cfg` (modules only; see the file's comments).

## Other laptops (generic Intel gen 6+ and AMD touchpad GPIO support)

The kernel (#31) builds drivers for most Intel-era laptops as **modules**, and boot detects what is present:

1. `usb/hwdetect.sh` (called by `hook.sh`) bind-mounts the stick's full module tree (`gbos/modules/`, depmod'ed for
   the running kernel) over `/lib/modules`, then does a udev-style *coldplug*: it reads every device's sysfs `modalias`
   and `modprobe -a`'s them, repeating until no new module loads (a loaded driver creates new devices).
2. The kernel's firmware loader is pointed at `gbos/firmware` (a trimmed linux-firmware + sof-bin, ~300 MB).
3. `logs/boot-N/hwdetect.txt` records DMI vendor/model, PCI/USB devices **without a driver**, input devices, network and
   Bluetooth interfaces, backlight, sound cards, rfkill, missing-firmware messages and `lsmod`. Send it back when
   something doesn't work.

| Area | Covered |
|---|---|
| CPU | Anything the ISA-emulation kernel handles: 4th gen (Haswell) and newer **with AVX2** (CPUs lacking AVX2, such as Celeron/Pentium/Atom or Sandy/Ivy Bridge, halt at boot with a clear message, as AOSP's dynamic linker executes AVX2 instructions); the Googlebook OS needs instructions older CPUs lack, so it is emulated (slow on pre-2021 CPUs) |
| Graphics / display / brightness | `i915` (Gen9 to Raptor Lake) + ACPI video/backlight; `xe` for newer parts (needs its own firmware, in firmware release v35; the colour/gamma fixes are i915-only). **AMD Radeon (`amdgpu`): experimental, software rendering only** (CPU-drawn, slow; see below; **untested on hardware**) |
| Wi-Fi | Intel (`iwldvm`: 7260/7265, `iwlmvm`: 8265 to AX211/BE200), Realtek rtw88/rtw89, MediaTek mt7921/7922/7925, Qualcomm ath10k/ath11k, Broadcom brcmfmac |
| Bluetooth | `btusb` with Intel/Realtek/MediaTek/Qualcomm/Broadcom, Intel PCIe BT |
| Sound | Legacy HDA with every common codec, **or** SOF (Intel DSP, DMIC, SoundWire); the kernel picks per machine (`snd_intel_dspcfg`). The audio HAL config is generated for the card that appears (`hook.sh`) |
| Touchpad / keyboard | Intel LPSS I2C-HID, ISH, PS/2 (Synaptics, ALPS, Elan, ...), Synaptics RMI4 |
| Hotkeys, fans, rfkill | Dell, HP, ThinkPad, IdeaPad, Asus, Acer, Samsung, MSI, Sony, Fujitsu, LG, Intel HID/VBTN |
| Other | SD card readers (Realtek, SDHCI), USB webcams (UVC), Ethernet (r8169, e1000e, igc, USB r8152 and friends) |

**Honest limits.** Twenty-one machines have been booted (below); anything not on that list is built and wired but untested on
hardware. Sound on SOF machines needs the codec's mixer controls to match the generated config (best effort);
suspend/resume, fingerprint readers, IPU6 cameras and NVIDIA/AMD discrete GPUs are not covered. Build the payload with
`tools/fetch_firmware.sh` (firmware, not committed) and `kernel/isa_emu_patch/build_generic*.sh` (modules, not committed).

## Microsoft Surface Laptop 7 (Intel, Core Ultra 7 266V / Lunar Lake)

Tested on a Surface Laptop for Business 7th Edition with Intel: boots from the stick to the desktop. What works and what it needed
(the causes and the order they were found in are in HANDOFF.md, 2026-10-18):

| Works | Needs |
|---|---|
| Wi-Fi (BE201) | `CONFIG_IWLMLD` |
| Internal keyboard, battery/charger, fan, platform profile | Surface Aggregator: `kernel/isa_emu_patch/surface_sl7_sam.sh`, `SERIAL_8250_RUNTIME_UARTS=4`, `PINCTRL_INTEL_PLATFORM` (the last two in `configure_whl.py --generic`) |
| Speakers, headset jack, internal mic | RT1320 amp + RT721 codec on SoundWire: `python3 kernel/isa_emu_patch/sdw_sl7.py`, firmware `realtek/rt1320/` (`tools/fetch_firmware.sh`), `usb/audio/mixer_paths_surface_laptop_7.xml`, `audio_config_surface_laptop_7.xml` |
| Touchpad with gestures, physical click and precision IDC | `tools/build_surface_tools.sh` (iptsd), `usb/surface/` |
| Camera (OV02C10 on IPU7) | `surface-webcam` bridge (`kernel/surface-webcam.c`, `tools/build_surface_tools.sh`), `usb/surface/`, `v4l2loopback` |
| Sleep, lid close/open guard & power profiles | `usb/surface/sleep-guard.sh` |
| Bluetooth | nothing extra |
| Touchscreen, suspend/resume | nothing beyond the rows above (tested by hand on the laptop) |
| Stable picture (no ghost images or freezes) | `XE_PARAMS="enable_psr=0 enable_fbc=0"` in `gbos.cfg` |

Not working: HDMI audio. `DEVICE_MODEL="Surface Laptop 7"` in `gbos.cfg` shows
that name as Model / Device name in Settings. Tests: `tools/test_audio_config.sh`, `tools/test_iptsd_run.sh`, `tools/test_device_model.sh`.
Credit: nierdal's write-up and patches in linux-surface/linux-surface issue #1710 (SAM entry, SoundWire analysis, touchpad calibration and
`LiftGraceMs`), and alex-lentz's iptsd fork.

### AMD graphics (experimental, software rendering)

Google's image only has an Intel graphics driver (the Vulkan driver `vulkan.intel.so`, and a gralloc (minigbm) with no `amdgpu` backend), so on a Ryzen
laptop nothing could draw. When boot finds no Intel GPU and a non-Intel DRM driver bound (the kernel and firmware for `amdgpu` are already
shipped), `hook.sh` switches to **software rendering**, logged as `hook: software rendering: ...`:

- `usb/patched/vulkan.swiftshader.so` (SwiftShader, a CPU Vulkan driver built for Android by `tools/swiftshader-android/build.sh`; fetched by
  `build.sh` from the release, 24 MB) is bind-mounted over `/vendor/lib64/hw/vulkan.intel.so`, so ANGLE, Skia and SurfaceFlinger render on the CPU.
- `usb/patched/libminigbm_gralloc.amdgpu.so` (`tools/patch_gralloc_amdgpu.py`) is bind-mounted over `/vendor/lib64/libminigbm_gralloc.so`
  (Google's gralloc, minigbm built in): one of its identical generic linear dumb-buffer backends (the ARM-only "komeda" one) now answers to DRM
  driver `amdgpu`, so gralloc can allocate scanout/render buffers; and Google's "software renderer" flag (lets a CPU Vulkan driver lock buffers that
  were allocated for the GPU only) is switched on for `ro.hardware.vulkan=intel`. `libgbm.amdgpu.so` does the same for `/vendor/lib64/libgbm.so` (only the
  camera and video-codec HALs use that one). Each is bound only over the exact original (sha256).
- Background blur and overlay planes are turned off (`ro.surface_flinger.supports_background_blur=0`, `ro.vendor.hwc.use_overlay_planes=false`):
  blur is far too slow on the CPU, and the composer's non-Intel backend is untested.

**RADV (real GPU rendering, experimental).** `tools/radv-android/build.sh` cross-builds Mesa main (b888682c)'s AMD Vulkan driver (RADV, no LLVM) for Android with the
NDK r30 in Docker (about 10 minutes after the downloads) and writes `build/radv/vulkan.radv.so` (18 MB; an Android Vulkan HAL: exports `HMI`, needs only
`libcutils/liblog/libnativewindow/libsync/libz` + bionic). Copy it to `usb/patched/`; with `GPU_VULKAN=auto` (default) `hook.sh` then binds it over
`vulkan.intel.so` instead of SwiftShader when `amdgpu` is bound (`GPU_VULKAN=swiftshader` goes back). It reaches the desktop on an
MSI Modern 15 B7M (Ryzen 5 7530U, Barcelo, RADV RENOIR), and video previews in Play Store and Photos play. Known problems on that machine: Photos cannot open
HEVC clips (the software HEVC decoder fails), the pointer renders as a red square, and the built-in touchpad is absent; AMD pinctrl/GPIO is now enabled in the generic kernel profile, but that touchpad change still needs hardware verification. See `HANDOFF.md`.
`tools/test_gpu_select.sh` tests only the driver-selection logic.

`GPU_SOFTWARE` in `gbos.cfg`: `auto` (default; never changes anything while an Intel GPU is present), `1` (force, for testing on Intel), `0` (never).
Expect a working desktop; use a lower `RESOLUTION` if it crawls. The AMD CPU side (Ryzen) has been tested on one MSI Modern 15 B7M (Ryzen 5 7530U).
`tools/test_gpu_select.sh` tests the decision logic and `tools/swiftshader-android/qemu/run.py` smoke-tests the
driver inside Google's userspace under QEMU.

## Repository map

| Path | What |
|---|---|
| `usb/stage0_init.sh` | stage0: volume detection, logging, partition mounts, switch_root |
| `usb/hook.sh` | Android workarounds, run by stage0 before switch_root; writes `gbos.rc` (init services) |
| `usb/early_patch.sh` | Sourced from `gbos.cfg`; binds the patched libz/libcrypto before stage0's first `dmctl` |
| `usb/install.sh` | NVMe/eMMC/SATA installer and updater (`MODE=install`) |
| `usb/apex_patch.sh` | binds the patched libcrypto over the APEX copies (run from init.rc) |
| `usb/gbos.cfg` | user settings: `MODE` (ask/live/install/boot), `DENSITY`, `RESOLUTION`, `LOAD_I915`, ... |
| `tools/make_gbos86_image.sh` | builds the flashable gbOS86 USB image into `dist/` |
| `usb/audio/` | audio HAL config for the HDA card |
| `usb/ksu/` | KernelSU's manager app and daemon (installed on the stick by `build.sh`) |
| `usb/out/BOOTX64.EFI` | **current kernel** (build #30: #29 + gbOS86 boot menu), boots directly from UEFI |
| `usb/patched/`, `usb/libkm_compat.so` | patched Google libraries; `usb/patched/MANIFEST` (generated) lists each patch with the sha256 of its original |
| `tools/swiftshader-android/` | builds SwiftShader as Android's Vulkan HAL (docker + NDK) for AMD GPUs |
| `usb/kmod/` | `btusb`, `btintel`, `btrtl` built for kernel #29 |
| `usb/installer/` | Alpine `sfdisk`/`mke2fs`/`e2fsck`/`mkfs.fat`/`blkid` + musl (`tools/fetch_installer_tools.sh`) |
| `usb/a11y/` | stage0 screen reader: `start.sh`/`stop.sh` + `espeakup`, `espeak-ng` (English), `amixer` (`tools/fetch_a11y_tools.sh`) |
| `usb/firmware/` | Intel Wi-Fi + Bluetooth firmware, `regulatory.db` |
| `usb/soft_gk`, `gatekeeper/` | software Gatekeeper binary and source |
| `usb/make_super.py`, `make_bootdisk.py`, `make_initramfs.py` | build `super.img`, the fake ChromeOS boot disk, the stage0 initramfs |
| `kernel/configure_whl.py` | kernel config for this laptop (from Google's recovery kernel config) |
| `kernel/fetch_kernel_sources.sh` | fetches the kernel tree, the AOSP clang and KernelSU into the `gbos-ksrc` volume |
| `kernel/kernelsu/apply.sh` | builds KernelSU into the kernel (`drivers/kernelsu`, `CONFIG_KSU=y`) |
| `kernel/isa_emu_patch/` | instruction emulator + `apply.sh`/`build.sh`, `debug_cmdline.txt` |
| `kernel/build_kernel.sh` | builds the whole kernel (patches, config, modules, initramfs) in the `gbos-kbuild` container |
| `kernel/stage0_modules.txt` | the modules the stage0 initramfs carries (see `kernel/make_stage0_initramfs.sh`) |
| `kernel/ashmem_shim/` | C memfd ashmem shim |
| `kernel/isa_emu_test/` | QEMU tests: the emulator (16 instructions), `run_isa_qemu.py` and `run_ksu_qemu.py` (KernelSU in the kernel), hwdetect, `run_installer_emmc.py` (installer on an emulated eMMC) and `run_installer_sata.py` (on emulated SATA disks) |
| `isa-emu/` | emulator core + host known-answer tests |
| `tools/` | image extraction (`lp_extract.py`), ISA scanner (`isa_scan.py`), binary patchers, and the new-build porting tools (`port_build.sh`, `check_new_build.sh`, see [`porting.md`](porting.md)) |
| `lapis/BUILD` | the Google build this repo is ported to (id, version, download URL) |
| `lapis/` | download script, ISA scan results, QEMU userspace harness |

## Using it

### gbOS86 USB image variants

#### 1. Standard Live & Installer Image (`gbOS86-YYYYMMDD.img`)
- **p1 (`EFI`)**: 256 MiB FAT32 ESP.
- **p2 (`GBOS`)**: 6 GiB FAT32 (`super.img`, installer, patches).
Booting shows the **gbOS86 boot menu** with a 10-second timeout:
- **1) Live from USB** (runs purely in RAM/tmpfs; nothing saved).
- **2) Install or update on internal SSD** (erases the internal disk and installs gbOS86).
- **3) Boot the installed system** (offered and default when an internal install exists).

#### 2. Looper Persistent Image (`gbOS86-looper-YYYYMMDD.img`)
Creates a full portable install directly inside the flash drive image, matching the internal disk layout:
- **p1 (`GBOS_EFI`)**: 512 MiB FAT32 ESP.
- **p2 (`GBOSSYS`)**: 8 GiB FAT32 (`super.img`, firmware, modules, `MODE=boot`).
- **p3 (`GBOS_META`)**: 64 MiB ext4 (`/metadata`).
- **p4 (`GBOS_DATA`)**: User-configured capacity (e.g. 64 GiB) ext4 (`/data`).

> **Sparse capacity on host vs physical drive size:**
> On the host file system, the image is created as a sparse file: a 64 GB userdata partition only occupies **~150 MB** of ext4 metadata on disk, so the whole `.img` file is only **~6.5 GB** physically.
> However, when writing directly to a physical USB drive (`dd`), the drive must be large enough to hold all partitions. Choose `--data-size` to match your stick:
> - **16 GB stick**: `--data-size 6`
> - **32 GB stick**: `--data-size 20`
> - **64 GB+ stick**: `--data-size 64` (default)

### Install to the internal disk (NVMe, eMMC or SATA)
Boot the stick and select option 2 ("Install or update gbOS86 on this laptop's internal SSD"), or set `MODE=install` in `gbos/gbos.cfg`.

The target is the internal NVMe or **eMMC** disk (an eMMC is recognised by its MMC device type: SD cards, card readers and the stick are never targets; the eMMC's boot/RPMB areas are left alone). With more than one internal disk the installer refuses until `INSTALL_DISK=nvme0n1` (or `mmcblk0`) is set in `gbos.cfg`.
A **SATA disk** (M.2 SATA or 2.5") is a target when the machine has no NVMe/eMMC: it is recognised by being owned by the SATA driver (libata), so a USB stick, a USB-SATA adapter or a card reader is never a target. A laptop with an NVMe *and* a SATA disk keeps installing to the NVMe; to use the SATA disk set `INSTALL_DISK=sda` in `gbos.cfg`.
The BIOS must show the disk in **AHCI** mode (not Intel RST/RAID or VMD), otherwise Linux cannot see it. (On a Dell the setting is *SATA/NVMe Operation*: change `RAID On` to `AHCI/NVMe`. `CONFIG_VMD` is not in this kernel.)
Minimum disk size 24 GB. eMMC controllers on the PCI bus work (`sdhci-pci`); ACPI-only ones (some AMD and older Atom parts) are not supported.

#### Installation Modes
When installing onto a drive with existing operating systems (Windows or Linux), the installer offers an interactive choice:

- **1) Dual-Boot (Recommended — alongside Windows or Linux):**
  > [!IMPORTANT]
  > **Unallocated Free Space Required:** Dual-boot requires unallocated (unpartitioned) space on the target disk (at least 20–24 GB recommended; 8 GB for the `GBOSSYS` system partition + remaining space for `GBOS_DATA` userdata).
  >
  > - **Windows:** Open *Disk Management* (`diskmgmt.msc`), right-click your `C:` partition, select **Shrink Volume**, and specify at least `24000` MB (24 GB) to leave as unallocated space.
  > - **Linux:** Use GParted or `parted` to shrink an existing partition and leave unallocated space at the end of the disk.
  >
  > The installer preserves all existing OS partitions and the shared EFI system partition. It creates `GBOSSYS` (8 GB FAT32) and `GBOS_DATA` (ext4) within the unallocated space.
  > - **Linux / GRUB:** Automatically adds `Googlebook OS (gbOS86)` to your GRUB menu (via `custom.cfg`) without menu duplicates.
  > - **Windows Boot Manager:** The installer copies `Add-gbOS-to-Windows-BootMenu.bat` into the EFI partition (`\EFI\gbos86\`); simply right-click and run it as Administrator in Windows to add gbOS86 to the Windows boot menu.

- **2) Full clean install (Erases entire disk):**
  Lays out the entire disk cleanly: `GBOS_EFI` 512M (ESP), `GBOSSYS` 8G FAT32 (system), `GBOS_META` 64M ext4, and `GBOS_DATA` (remaining space for user apps/data). To prevent accidental data loss, it requires confirming by typing the drive's serial number.

- **3) Start Live Desktop:**
  Configures a one-time live session in RAM without writing to the disk.

- **If gbOS is already installed:**
  The installer offers **1) Update** (keeps your apps and data, updates system files and kernel), **2) Reinstall** (clean wipe), or **3) Only save logs**.

During installation, live progress bars indicate system image and driver/firmware copying, followed by fast sha256 verification.

### Accessibility (screen reader)

- **Boot menu and installer are spoken** (kernel #46 and newer). The kernel has Speakup built in; `usb/a11y/` on the stick holds `espeakup` +
  `espeak-ng` (English) + `amixer` (about 10 MB, from Alpine, filled by `tools/fetch_a11y_tools.sh`). Stage0 copies it to RAM, unmutes the
  laptop's sound card, starts the reader and reads the boot menu and every installer message aloud; typed characters are echoed. The boot
  menu waits 60 s instead of 15 (`A11Y_MENU_TIMEOUT`), progress is announced every 10 percent, and the drive serial you must type before an
  erase is also spelled out character by character. The confirmation itself is unchanged. `A11Y=0` in `gbos.cfg` turns it off.
  Works on laptops whose sound card the kernel brings up by itself (legacy Intel HDA: most pre-2021 laptops), and on newer Intel ones whose HDA
  controller waits for the GPU driver (Raptor Lake: `start.sh` loads i915/xe itself first). SOF-only machines have no sound
  at that stage, so no speech there (logged as `a11y: no sound card`). Nothing is spoken before the stick's kernel starts (the firmware's own
  boot-device menu is outside gbOS86). Test: `kernel/isa_emu_test/run_a11y_qemu.py usb/out/BOOTX64.EFI` (emulated Intel HDA, records the audio).
- **Android**: set `ACCESSIBILITY=talkback` in `gbos.cfg` and Google's TalkBack (already in the image, with Google TTS) is switched on at the
  first boot.

### Debugging workflow
The system writes logs to its own `GBOSSYS` (or `GBOS`) partition. To read them, boot the stick, choose
menu **2**, then **3 (Only save its logs)** (or 1 to update too), and read `gbos/logs/boot-N/nvme-logs/` on the stick from a computer. Each boot dir has
`kmsg.log`, `stage0-dmesg.txt`, rotated `logcat.txt*`, `crash.txt`, `hw.txt`, `audio.txt`, and `pstore-prev/`.
FAT has no journal: always shut down from Android's power menu, not the physical power button.

### Display size
Density stays Google's stock 260. `RESOLUTION=2400x1350` in `gbos.cfg` makes Android render at a larger
logical size that the display scales to the 1920x1080 panel: bigger = smaller UI (empty = native).
`DENSITY=` can still override the density but is unset by default. If you changed Settings → Display →
Display size, reset it to Default.

## Building from scratch

Needs macOS or Linux with docker, python3, qemu (for tests) and ~30 GB free. The build images used:
`gbos-kbuild` (`kernel/docker/Dockerfile`) and `gbos-erofs` (`tools/erofs-docker/Dockerfile`).

1. **Google's OS image:** `lapis/dl.sh` downloads `lapis-user/16658739/recovery.zip` from
   `dl.google.com`. Extract the recovery disk image from it, then pull the logical partitions (system,
   vendor, product, system_ext, system_dlkm, vendor_dlkm; slot `_a`) out of `super` with
   `tools/lp_extract.py DISK.raw lapis/parts NAME...` (run it with just `DISK.raw` to list the names).
2. **`super.img`:** `usb/make_super.py OUT/super.img OUT/super.layout system_a=... vendor_a=...`
   (system_a first). It must stay under 4 GB (FAT32).
3. **Kernel source:** AOSP `kernel/common`, branch `android17-6.18`, commit
   `3938c8d97142d5cf31543aa96f864a2cf832dda0`, and the AOSP clang prebuilt. `kernel/fetch_kernel_sources.sh`
   fetches all of it into the docker volume `gbos-ksrc` (`/build/{src,inputs,out}`): the kernel tree,
   `clang-r584948b` (LLVM 22.0.1, the toolchain the shipped kernel was built with; the AOSP prebuilts
   repo publishes the `b` build of that clang, not the `c` one) and the KernelSU sources
   (`/build/inputs/KernelSU`, tag v3.3.1).
4. **Config:** `kernel/configure_whl.py RECOVERY_KERNEL.config /build/out/.config`, then `make olddefconfig`;
   set `CONFIG_CMDLINE` from `kernel/isa_emu_patch/debug_cmdline.txt` (`debug_config.sh`) and
   `CONFIG_INITRAMFS_SOURCE=/build/out/gbos_stage0.cpio`.
5. **Patches:** in the volume, `kernel/isa_emu_patch/apply.sh`, `kernel/ashmem_shim/apply.sh`, and the i915
   scripts `kernel/isa_emu_patch/i915_color_pipeline.sh` (dummy plane COLOR_PIPELINE for the HWC) and
   `i915_edp_colorspace.sh` (eDP always gets default colorimetry; BT.2020 made the panel pale) and
   `i915_edp_no_colorspace.sh` (no Colorspace property on eDP at all, from kernel #25) and
   `i915_edp_full_range.sh` (eDP always full-range RGB, from kernel #27) and `i915_ignore_gamma.sh`
   (ignore userspace gamma/degamma/CTM: the HWC loads an xe LOG_GAMMA_LUT curve into GAMMA_LUT, kernel #29), and
   `kernel/isa_emu_patch/debug_selinux.sh` (the `gbos_permissive` parameter of the kernel command line: without it the
   kernel is not permissive, Android's init enforces SELinux and the boot stops right after "init: Loading SELinux policy").
   For the Surface Laptop 7 (Intel) also `kernel/isa_emu_patch/surface_sl7_sam.sh` and `python3 kernel/isa_emu_patch/sdw_sl7.py`
   (see HANDOFF.md); both only touch modules. And `kernel/kernelsu/apply.sh`, which copies KernelSU's
   kernel sources into `drivers/kernelsu` and hooks them into `drivers/Makefile` and `drivers/Kconfig`
   (`CONFIG_KSU=y`, `CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER=y` come from `kernel/configure_whl.py`).
6. **Build:** `kernel/isa_emu_patch/build.sh` (in `gbos-kbuild`), then `make modules_install` into
   `usb/modroot`, then `usb/make_initramfs.py usb/modroot usb/stage0.cpio`, copy it to
   `/build/out/gbos_stage0.cpio`, and build again so it's embedded. `arch/x86/boot/bzImage` is
   `BOOTX64.EFI`. `btusb.ko`/`btintel.ko`/`btrtl.ko` from `/build/out/drivers/bluetooth/` go in `usb/kmod/`.
7. **Test the emulator:** `kernel/isa_emu_test/run_isa_qemu.py usb/out/BOOTX64.EFI` (expects 16/16 PASS),
   and `kernel/isa_emu_test/run_ksu_qemu.py usb/out/BOOTX64.EFI` (expects KSU_QEMU_TEST_PASS: KernelSU
   came up, its version is one the manager app accepts, and the x86_64 syscall dispatcher was patched).
8. **Userspace pieces:** `tools/fetch_installer_tools.sh`; `gatekeeper/build.sh` (needs `gatekeeper/libs/`
   from the image: `libc`, `libdl` from the runtime APEX, `libbinder_ndk`, `libcrypto`, `liblog`);
   patched libraries with `tools/patch_libz_adler.py` and `tools/sha_redirect.py`. The hashes in
   `hook.sh`/`apex_patch.sh` pin the exact originals they apply to. KernelSU's manager app and daemon
   come from the KernelSU release (`build.sh` puts them in `usb/ksu/`).
9. **Firmware:** from linux-firmware (`intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-46.ucode`,
   `intel/ibt-17-16-1.{sfi,ddc}`); `regulatory.db*` from the vendor image.

## Updating Googlebook OS
Android's built-in updater can't work here (it expects Google's A/B partitions and kernel). Moving to a newer lapis build is
mostly automated; [`porting.md`](porting.md) is the full procedure:
1. `tools/check_new_build.sh` says whether a newer build exists; put it in `lapis/BUILD` and run `./build.sh` to fetch it;
2. `tools/port_build.sh --isa` re-applies every binary substitution (libz, libcrypto, libkm_compat, the VINTF manifest) to the new
   files, writes `usb/patched/MANIFEST` with the sha256 of each original, checks every path/line/property the boot scripts rely on,
   and scans for instructions the emulator doesn't cover (add `--write` once it reports 0 FAIL);
3. `sh tools/test_port_scripts.sh` runs the scripts' patch-binding logic under the stick's busybox;
4. copy to the stick and run **1 (Update)**; `PATCH-STATUS N applied, 0 not applied` in `stage0-dmesg.txt` confirms the patches
   bound. Data carries over; going back to an older build requires a data wipe.

## Safety
- Full clean install erases the **entire** internal disk (NVMe, eMMC or SATA); typing the serial number is required. Dual-boot mode preserves existing OS partitions and only writes into unallocated free space.
- SELinux runs **permissive**, and the security services are software stand-ins. Don't treat this as a
  secure device.
- The ISA emulator runs in the kernel's `#UD` handler; a bug there is a kernel bug.
