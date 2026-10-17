# gbos-x86 handoff

## Goal
Run the Googlebook `lapis` Android userspace on an i5-8365U (Whiskey Lake). That CPU lacks GFNI, AVX-VNNI,
SHA-NI, 256-bit VAES/VPCLMULQDQ, AVX-IFMA and AVX-512; the lapis binaries use them.

## Done
- `isa-emu/isa_emu_core.c`: software versions of those instructions. 13,373 known-answer checks pass.
- **Kernel emulation** (new): `kernel/isa_emu_patch/`. #UD handler in `arch/x86/kernel/traps.c` calls
  `isa_emu_handle_ud()` (`arch/x86/kernel/fpu/isa_emu.c`), which decodes the instruction, runs the core on the
  saved xsave image, and advances RIP. Covers GFNI (SSE+VEX), AVX-VNNI(-INT8), SHA-NI, VAES-256, VPCLMULQDQ-256,
  AVX-IFMA. Boot param `noisaemu` disables it. Source tree is in docker volume `gbos-ksrc`; `apply.sh` applies the
  patch, `build.sh` does the incremental build (image `gbos-kbuild`, clang r584948c).
- Kernel image: `kernel/out_isa/bzImage` (sha256 in `bzImage.sha256`; rebuilt since, so recompute). Original kernel
  is `kernel/out/bzImage`. Modules: `kernel/out/modules/6.18.21-gbos-whl+/` (110, incl. i915, iwlwifi).
- `kernel/isa_emu_test/`: freestanding test. 16/16 pass on QEMU Skylake-Client; the stock kernel SIGILLs.
- `lapis/patched/init`: init with SHA-NI routine redirected to the scalar one (optional speed-up; not needed with the
  kernel emulation). `tools/patch_init_sha.py`.
- QEMU boots of lapis userspace (`lapis/qemu/`): the real first-stage `init` runs unpatched on the emu kernel.
  `run_b.py` hand-does first stage (mounts real erofs images from `lapis/parts/`, fake dm devices) then runs
  `/system/bin/init selinux_setup`. Reached: SELinux policy loaded and enforcing, second-stage init, ueventd,
  apexd (APEXes mounted), logd, lmkd, servicemanager, BoringSSL self-tests, vold. ~15M instructions emulated,
  **zero illegal-instruction deaths**.

## Not done / unknown
- Never run on real hardware. i915/iwlwifi only checked for module load in QEMU. Firmware not packaged
  (i915 KBL DMC blob; Wi-Fi blob needs card model from `lspci -nn`).
- Boot never reached zygote/system_server. Last attempt had a stand-in `super` (extra disk copies as dm-linear
  devices) to stop vold's "could not find logical partition system_a" reboot; its full-boot result was not seen.
- No real boot chain: no bootloader/GRUB entry, boot/vendor_boot/vbmeta(AVB)/super for this laptop. Google's
  vendor modules won't load on our kernel (different build).
- AVX-512, AMX, AVX-NE-CONVERT, WAITPKG, PREFETCHI are not emulated (libjxl, openvino, cros_ml, camera,
  desktop_audio_effects). Likely gated by CPU checks, not confirmed.
- Perf: each trapped instruction is a kernel round trip. SHA-NI-heavy paths (init, apexd, prng_seeder) are slow;
  consider binary-patching them to scalar like `init`.
- `gbrunch/` has installer docs (INSTALLER.md, STANDALONE-INSTALLER.md, BUILD.md) not yet read this session.

## 2026-10-15: first real-hardware boot (Inspiron 5300), boot-15 log
- Reached system_server + SetupWizard. Graphics, keyboard and trackpad work.
- SetupWizard crash: `ashmem_create_region failed ... Unable to stat ashmem device`. In this kernel ashmem is
  Rust-only (`ASHMEM depends on RUST`) and `configure_whl.py` sets RUST=n. Fix: `kernel/ashmem_shim/`, a C port
  of `ashmem_memfd_ioctl()` (CONFIG_MEMFD_ASHMEM_SHIM) so libcutils uses memfd. Built as #17, on the stick.
  The previous EFI is saved as `usb/out/BOOTX64.EFI.prev-noashmem`.
- system_server Watchdog kill (66s in LockSettingsService waiting for IWeaver): hook.sh now hides the Weaver VINTF
  fragment.
- The kernel still has the always-on isa_emu trap profiler (prof_hit) from the QEMU perf work.
- The firmware boots the stick's separate EFI partition (disk7s1, "EFI"), NOT GBOS/EFI/BOOT. Put new kernels in
  /Volumes/EFI/EFI/BOOT/BOOTX64.EFI. Boots 16-19 ran the old #14 kernel; #17 (ashmem shim) is now on disk7s1.
- Android's vold took over the stick (fstab rule `/devices/*/usb* voldmanaged=usb`) and ran fsck_msdos on it
  while it was mounted, so logs were cut off, LOST.DIR appeared and standalone product_a.img got corrupted.
  hook.sh now removes that rule. logcat rotates at 1 MB; crash.txt holds the crash buffer.
- boot-19 crash loops: SetupWizard shared Bitmap (old kernel), Bluetooth HAL abort (no BT HAL),
  mmd_setup zram (hook now runs modprobe zram).
- boot-20 (#17 kernel): still "Failed to create shared Bitmap!". libcutils use_memfd() only picks memfd when
  sys.use_memfd=true, or memfd_class policycap + ro.vendor.api_level >= 202604 + app targetSdk >= 37.
  SetupWizard targets older SDK, so it goes to /dev/ashmem. hook.sh's gbos.rc now sets sys.use_memfd true.
  ANRs in gms / settings.intelligence in the same boot also showed ashmem failures (CursorWindow).
- boot-21: SetupWizard screen reached on the Inspiron 5300 (sys.use_memfd + ashmem shim).
- Panic at boot-21/22 ("Fatal exception in interrupt", pstore in boot-22/pstore-prev): isa_emu_handle_ud() ran
  with IRQs off but can sleep (mmap lock, kmalloc, d_path in the profiler, copy_from_user). After a sleep it
  returned to user with IF=1; an IPI between swapgs and iret ran with user GS and recursed into a stack overflow.
  Fix: handle_invalid_op enables IRQs around the emulator (kernel/isa_emu_patch/traps_irq.sed, in apply.sh).
  Built #18, 16/16 QEMU tests pass, on both EFI copies. #17 kept as usb/out/BOOTX64.EFI.prev-17.
- Speed: profiler shows libz.so adler32 (vpdpwssd, 50%), conscrypt APEX libcrypto SHA-256 (40%),
  system libcrypto SHA-256 (10%). libz: the je at adler32_z+0xb5 (0xa7d5) gates the SIMD path.
- Wi-Fi: card is CNVi 8086:9df0 (9560/9462). gbos/firmware/ has iwlwifi-9000-pu-b0-jf-b0-46.ucode (linux-firmware intel/iwlwifi/, sha256 9a0c5d93...) + regulatory.db; hook.sh loads iwlmvm (LOAD_WIFI stays 0).
- boot-24: signed into Google, Wi-Fi works. Setting a lock password hung: Trusty Gatekeeper enroll never returns (LSS stuck after 'Device does not support weaver'). hook.sh hides Trusty Gatekeeper (VINTF + rc) so Skip works; no lock-screen password until a software Gatekeeper exists. libz patch deployed to gbos/patched.
- boot-25: Setup lock step loops (Settings SetupChooseLockGeneric returns RESULT_CANCELED to Skip -> back to avatar).
- Software Gatekeeper: gatekeeper/soft_gk.c (no NDK; gatekeeper/build.sh in gbos-kbuild, links image libs in
  gatekeeper/libs). AIDL V1 wire format taken from android.hardware.gatekeeper-V1-ndk.so (codes 1-4, 0xffffff,
  0xfffffe). HMAC-SHA256 password handle (58-byte password_handle_t). Auth-token MAC uses its own key, so
  KeyMint won't accept its tokens (auth-bound keys won't work). Installed as gbos/soft_gk + gbos_soft_gk service
  (hal_gatekeeper_default); Trusty gatekeeper rc disabled. Not yet run on hardware.
- boot-26: reached the desktop with soft_gk; user reports it's smooth. Committed as 03a6a46 (new git repo, images/binaries gitignored).
- Laptop is a Dell Latitude 5300 (DMI), not an Inspiron. Kernel #20 adds ACPI battery/AC, Intel LPSS PCI
  (I2C touchpad was stuck on its PS/2 fallback), and legacy HDA audio (snd_hda_intel + ALC269 + HDMI,
  snd_intel_dspcfg.dsp_driver=1). Google's vendor modules for these never loaded (wrong kernel).
  usb/audio/: gbos-hda-pch config for the dras audio HAL (audio_default.xml -> card "HDA Intel PCH").
- NVMe installer: usb/install.sh (MODE=install in gbos.cfg), tools in usb/installer (Alpine sfdisk/mke2fs/
  mkfs.fat + musl). Layout: GBOS_EFI 512M, GBOSSYS 8G FAT32, GBOS_META 64M ext4, GBOS_DATA rest ext4.
  stage0 boots GBOSSYS when no stick (or stick lacks MODE=install/BOOT_FROM=stick) and mounts ext4 data.
  QEMU: cancel path tested (wrong serial -> nothing written). Full install and NVMe boot NOT tested (user
  chose to go straight to hardware; burner laptop). Stick has #20 + MODE=install.
- 2026-10-15: installed to the Latitude 5300's NVMe and boots from it. User reports everything works except
  sound (touchpad gestures, battery, Wi-Fi OK). Sound not investigated (user deprioritised): no logs taken.
  Next step if revisited: boot the stick with MODE=boot + BOOT_FROM=stick and dump /proc/asound, the codec
  info, tinymix and dras logcat. Suspects: dras card-name match ("HDA Intel PCH"), mixer control names,
  or the DSP still claiming the controller.
- The stick still has MODE=install; booting with it plugged in starts the installer (it asks for the serial).
- Bluetooth: our config had BT_HCIBTUSB unset (Google's chip is PCIe). Built btusb/btintel/btrtl as modules
  (usb/kmod, loaded by hook.sh) + linux-firmware intel/ibt-17-16-1.sfi/.ddc in gbos/firmware/intel. Untested.
- install.sh: if an install exists, it asks 1) update (keep data, save NVMe logs to stick) / 2) reinstall.
  A full reinstall first saves the old install's logs to the stick. ACTION=update|install in gbos.cfg skips
  the question. A stick left in MODE=install once reinstalled over the user's working system (they aborted
  it midway, NVMe corrupt -> full reinstall); Play-install-stuck-at-100% logs were lost with it.
- Sound: kernel side works (ALC3254 found). dras loaded our config but never matched "HDA Intel PCH":
  it compares <soundcard> to the ALSA card id (sof-soundwire -> id sofsoundwire). Now <soundcard>PCH</soundcard>.
- Play installs: not broken, slow. PP-Copy (SHA-256 via Conscrypt) took ~5 min for Spotify's 33 MB apk.
  With the libcrypto patches deployed, Spotify installs (user confirmed).
- Linux Terminal: Debian download+unpack took ~11 min. Then virtualizationservice.allocateInstanceId waited
  56 s for ISecretkeeper/default (Trusty). hook.sh now hides Secretkeeper (VINTF + rc).
- boot-3 (NVMe, collected in stick boot-34): dras now finds card PCH but rejected audio_config.xml:
  "missing field JackSwitch" (fields inside a section are required, sections are optional, cf. USB configs).
  Config now mirrors sof-soundwire field for field; jack is "HDA Intel PCH Headphone Mic" (combo).
  Codec has an internal mic on pin 0x12, so InternalMic is configured on PCM 0.
- Bluetooth: btintel loads firmware at probe (pre-switch_root) but the path was already /postinstall:
  "Failed to load Intel firmware file intel/ibt-17-16-1.sfi (-2)". hook now points at $G/firmware while
  btusb probes and waits for "Firmware loaded".
- audiodiag stdio_to_kmsg output was lost; now writes logs/boot-N/audio.txt.
- Display: Google sets ro.sf.lcd_density=260; hook now replaces it with DENSITY from gbos.cfg (default 180). Update path syncs gbos.cfg (minus MODE/ACTION) to the NVMe. A 'Display size' set in Settings is stored as a forced density and overrides this; reset it to Default.
- 2026-10-15: user confirms sound and Bluetooth work on the NVMe install (after 445b5ce). Density change (DENSITY=180) deployed to the stick.
- Display looks washed out (grey blacks even in dark UIs). Added gbos_displaydiag -> logs/boot-N/display.txt (DRM state, i915 info, EDID, SurfaceFlinger, colour settings). Suspect limited RGB range or a colour matrix.
- Pale display: panel is an LG LP133WF6 (8-bit, ~105% sRGB area per EDID), so not the hardware. SurfaceFlinger
  outputs sRGB with an identity matrix, but drm_hwcomposer set eDP-1 colorspace=BT2020_RGB, so i915 sent a VSC SDP
  with BT.2020 colorimetry and the panel gamut-mapped it (pale). kernel/isa_emu_patch/i915_edp_colorspace.sh:
  eDP always uses default colorimetry (property kept). Kernel #23 (16/16 QEMU), on the stick; #20 kept as
  usb/out/BOOTX64.EFI.prev-20.
- User wanted stock density back (believes it caused the paleness). DENSITY now optional (unset = stock 260); new RESOLUTION=2400x1350 -> 'wm size' at boot_completed (gbos_wmsize) scales the UI without changing density.
- User reverted to DENSITY=180, RESOLUTION empty (native).
- hook: vendor build.prop now sets ro.surface_flinger.has_wide_color_display=false and has_HDR_display=false (were true), so SF offers only sRGB and the HWC has no reason to tag eDP BT.2020. Complements kernel #23.
- User reports #23 + sRGB-only props did not fix the paleness. Kernel #25 (i915_edp_no_colorspace.sh): no Colorspace property on eDP at all. 16/16 QEMU. On the stick in TEST mode (MODE=boot, BOOT_FROM=stick: stick's own system, tmpfs data); NVMe still has #23. #23 kept as usb/out/BOOTX64.EFI.prev-23. Note: no logs yet confirm #23/sRGB were active on the boot the user judged.
- Test boot (stick, boot-42) on #25: user confirms colors are good. Stick switched back to MODE=install for the NVMe update.
- Correction: after updating the NVMe to #25, colours in the fully booted desktop look no different (good-looking was the setup screens only). Paleness unresolved but deprioritised by the user. #25 + sRGB-only props kept (harmless). Next idea if revisited: Material You theme/wallpaper (muted palette) vs panel; compare a display.txt from a full boot on #25.
- User: video/UI still has a 'white veil' (lifted blacks) and says the panel looks right under Windows/Linux/BSD. Kernel #27 (i915_edp_full_range.sh): eDP always full-range RGB. hook: drm.debug=0x04 until boot_completed, so kmsg.log has i915's per-modeset gamma/CSC/range dump. #25 kept as usb/out/BOOTX64.EFI.prev-25. On the stick (MODE=install -> pick 1).
- Timeline correction: the "still veil" verdict was on NVMe boot-12 = kernel #25; #27 was only installed at stick
  boot-45 and never tested. boot-12 diag: eDP colorspace Default, SF wide colour 0, SRGB, identity CTM, so the
  colour-space fixes worked and weren't the cause.
- REAL CAUSE (likely): logcat "drmhwc: Missing optional LOG_GAMMA_LUT property, falling back to GAMMA_LUT" x32523,
  i.e. the HWC writes its xe log-gamma curve into i915's linear GAMMA_LUT every commit, lifting the shadows.
  Kernel #29 (i915_ignore_gamma.sh): hw degamma/gamma/CTM always NULL (linear). Includes #25/#27 changes.
  On the stick (MODE=install -> 1). #27 kept as usb/out/BOOTX64.EFI.prev-27. Cost: Night Light won't work.
- 2026-10-15 CONFIRMED by the user on kernel #29: the white veil / pale colours are gone. Cause: drm_hwcomposer's
  LOG_GAMMA_LUT -> GAMMA_LUT fallback (an xe log-gamma curve in i915's linear LUT). Fix: i915_ignore_gamma.sh.
  The colour-space (#23/#25), full-range (#27) and sRGB-only SF props weren't the cause but are kept (harmless,
  more correct for this sRGB panel). Known cost: Night Light has no effect.
- gbOS86 branding + boot menu in stage0 (kernel #30, initramfs-only change): with the stick in, a 10 s menu offers
  1 live / 2 install-or-update / 3 boot the installed system (default 3 if installed, else 1). gbos.cfg MODE=ask|live|
  install|boot (old BOOT_FROM=stick = live). Menu timeout path tested in QEMU. tools/make_gbos86_image.sh builds
  dist/gbOS86-YYYYMMDD.img (GPT: 256M EFI + 6G GBOS FAT32, super.img rebuilt from lapis/parts). First image
  gbOS86-20261016.img built; not boot-tested (user declined).
- 2026-10-16: usb/early_patch.sh (sourced from gbos.cfg) wraps `chroot` so the patched libz/libcrypto are bound before
  stage0's first `dmctl` (avoids ~3.7M SHA-NI traps at boot); hook.sh skips re-binding ("already patched"). install.sh
  FILES and make_gbos86_image.sh now ship it, so Update copies it to the NVMe. Stick deployed with kernel #30.
  Not yet verified on hardware: look for "early: patched system libcrypto" before "running gbos/hook.sh" in stage0-dmesg.txt.
- 2026-10-16: **Generic Intel gen6+ support (kernel #32, not yet booted on hardware).** `configure_whl.py --generic` adds ~400
  modules (Wi-Fi: iwldvm/iwlmvm/rtw88/rtw89/mt76/ath10k/ath11k/brcmfmac; BT: btusb+vendors, btintel_pcie; HDA codecs + SOF/SoundWire;
  I2C-HID/ISH/RMI4/PS2; dell/hp/thinkpad/ideapad/asus/... platform drivers; card reader, UVC, ethernet). `usb/hwdetect.sh` (from
  hook.sh) bind-mounts `gbos/modules` over /lib/modules, coldplugs by modalias (up to 4 passes) with firmware from `gbos/firmware`,
  and writes `logs/boot-N/hwdetect.txt`. The old Wi-Fi/BT sequence stays as a fallback when `gbos/modules` is absent.
  `snd_intel_dspcfg.dsp_driver=1` was REMOVED from the cmdline (HDA vs SOF is now automatic; on the Dell, Dell vendor + no DMIC should
  still pick HDA. **Verify sound on the 5300**; rollback = the #30 BOOTX64.EFI from git history, `e2207d9`). Firmware is not committed:
  `tools/fetch_firmware.sh` (linux-firmware d5455717 + sof-bin 2026.09.2, ~290 MB); iwlwifi files live at the firmware root
  (this kernel doesn't use the intel/iwlwifi/ prefix). Audio HAL config is generated for non-PCH cards (best effort).
  QEMU: ISA_EMU_TEST_PASS; `kernel/isa_emu_test/run_hwdetect.py` loaded e1000e/i2c_i801/rapl/wmi/video by modalias.
- 2026-10-16: dist/gbOS86-20261016.img rebuilt with kernel #32, usb/modules (396 modules), usb/firmware (~290 MB), hwdetect.sh,
  early_patch.sh (sha256 f20823e0…; 6.7 GB, 2.2 GB free on its GBOS partition). Not boot-tested. The Dell stick still has kernel #30
  until it is re-synced (copy usb/ -> stick incl. modules/ and firmware/, BOOTX64.EFI to both EFI copies).
- 2026-10-16 AI features (Magic Pointer says "Gemini is not available at this moment"), from NVMe boot-19 (kernel #30, in stick boot-1/nvme-logs):
  gPointer asks AICore for feature 713 (`AiCoreException ... 606-FEATURE_NOT_FOUND: Feature 713 is not available`), at boot and on every launch.
  AICore (Play-updated, runs as uid 10107) never gets its feature manifest: `Failed to fetch manifest config ... Unable to resolve host
  ondevicesafety-pa.googleapis.com` x55, then "Required labels are missing from the protected download manifest. Download will not be
  triggered". The first tries (15:54:33) were before Wi-Fi came up (15:54:33.97), but 15:54:43/48/53 were 10-20 s after it validated, and
  as.oss (10118, same Google AI stack) was online then, so it is not just "no network yet". The host resolves fine on a Mac. Cause undetermined:
  AICore's uid (PCC-allowed package, `PccSandboxManagerInternal: Updated allowed PCC Packages ... com.google.android.aicore`) may be
  network-blocked, or lacks INTERNET in netd's map. Nothing else on the device failed to resolve a host (only android.googleapis.com, pre-Wi-Fi).
  Separate and unfixable here: Private Inference attestation (`AttestAndSign ... PERMISSION_DENIED`; keystore2 `HARDWARE_TYPE_UNAVAILABLE`
  for the RKP IRPC x361, because Trusty KeyMint/RKP are hidden) and as.oss `PcsAi__enable_genai_inference_service` (AICore forwarding off).
  Added `usb/aidiag.sh` + `gbos_aidiag`/`gbos_aidns_*` services in hook.sh: `logs/boot-N/ai-1.txt` (90 s) and `ai-2.txt` (4 min) with
  ping-as-uid (10107 AICore, 10118 as.oss, 10119 as, 2000 shell), package/permission/gids dumps, connectivity/netpolicy/PCC state. Not yet run
  on hardware. install.sh FILES and make_gbos86_image.sh ship aidiag.sh. The dev stick was re-made this day (GPT, EFI 200 MB + GBOS 30 GB;
  mount the EFI partition with `sudo diskutil mount disk8s1`; the ESP image was written with dd so it keeps the EFI System type).
- 2026-10-16 **Wi-Fi regression on kernel #32 (NVMe boot-20): root cause found and fixed.** hwdetect loaded iwlwifi by PCI alias and it took
  its firmware (`loaded firmware version 46... op_mode iwlmvm`), but nothing loaded `iwlmvm`: the kernel's request_module() for the op_mode is a
  silent no-op in the stage0 initramfs (the old hook.sh did an explicit `modprobe iwlmvm`). No iwlmvm/mac80211 in lsmod, no wlan0 (`wifi=` empty in
  hwdetect.txt), so ai-1/ai-2 were taken with no network at all (every ping "unknown host", also as root) and say nothing about DNS.
  Fix: `gbos_load_opmodes` in usb/hwdetect.sh modprobes whatever `op_mode iwl*` the dmesg announces, after every coldplug pass and every second of the
  device wait while there is no wl* interface. hook.sh now also honours HWDETECT=0 by using the old sequence (it used to skip Wi-Fi entirely). I had
  briefly staged a revert (#30 + HWDETECT=0), then undid it: #32 and HWDETECT=1 are back (usb/out/BOOTX64.EFI.32 is a copy of #32). Untested on hardware:
  look for `hwdetect: loaded iwlmvm` and `wifi=wlan0` in stage0-dmesg.txt / hwdetect.txt.
  aidiag.sh now waits (up to 5 min) for the network before dumping and pings www.google.com as each uid too, so ai-1/ai-2 become meaningful.
- 2026-10-16 AI, from boot-20 dumps: AICore (prod_aicore_20260917.00_RC01, uid 10107) does NOT request android.permission.INTERNET (gids=[3013] only;
  as.oss has INTERNET + 3003, gpointer has 3003). Its feature-manifest fetch runs a gRPC DnsNameResolver in the AICore process itself (stack ioh/ihi in
  pid of com.google.android.aicore), so "Unable to resolve host ondevicesafety-pa.googleapis.com" (55x in boot-19, with Wi-Fi up) fits an app with no
  INTERNET: netd blocks its DNS. AICore also binds as.oss AstreaGrpcService (act=grpc.io.action.BIND), and as.oss hardcodes that host and has
  PccSecurity__aicore_package_security_info / PcsAi__enable_genai_inference_service flags, so the intended route is probably via as.oss and depends on a
  flag/security policy delivered by Phenotype. NOT fixed: no safe way found to give AICore network (INTERNET is set by ConnectivityService from the manifest;
  ndc only sets NETWORK/SYSTEM). Needs a run with working Wi-Fi: compare ping www.google.com / ondevicesafety as uid 10107 vs 10118 in ai-1/ai-2.
- 2026-10-16 "Extract logs": boot menu **2** -> sub-menu now has **3) Only save its logs** (usb/install.sh, `ACTION=logs`, `LOG_BOOTS`, default 3): mounts GBOSSYS
  read-only, copies the last N boots to the stick's boot-N/nvme-logs, powers off on Enter; seconds instead of Update's copy+verify of ~4 GB. `save_logs` (cp, falling
  back to dd conv=noerror,sync for FAT-truncated files) is also used by Update. A top-level menu key 4 was written for stage0 but dropped: the menu lives in the kernel's
  initramfs, so it needs a kernel rebuild (usb/make_initramfs.py reproduces #32's cpio byte for byte, so it would be an incremental relink). install.sh is a stick script, no kernel change.
- 2026-10-16 **Wi-Fi fix CONFIRMED on hardware (NVMe boot-21, kernel #32):** `hwdetect: loaded iwlmvm (op_mode announced by iwlwifi)`, `devices after 0s: wifi=wlan0`.
- 2026-10-16 **AI root cause (boot-21, Wi-Fi up, Magic Pointer tried): Google's server rejects AICore's feature-manifest request because there is no key attestation.**
  Retire the earlier "AICore has no INTERNET" theory: AICore (user-0 pid) does reach ondevicesafety-pa.googleapis.com. After a first pre-network `Unable to resolve host`
  (17:24:30), every attempt from 17:25:01 on fails with `ien: INVALID_ARGUMENT: Request contains an invalid argument`, immediately after as.oss (the gateway, pid 2876)
  logs `AttestationClientImpl: Failed to generate key with ID attestation, retrying without it` (java.security.ProviderException: Failed to generate key pair). Cause:
  keystore2 `Failed to get rpc for sec level TRUSTED_ENVIRONMENT: HARDWARE_TYPE_UNAVAILABLE` (115x in this boot): remote key provisioning has no IRemotelyProvisionedComponent
  (Trusty KeyMint/RKP are hidden in hook.sh, software KeyMint serves keys but cannot attest). With no manifest, AICore registers no features: gPointer `Feature 713 is not
  available`, notification summary `Feature 253`, ModelDownloadManager `Feature 2007`. The device identity is fine (ASUS/lapis, "ASUS Googlebook 14"). Same blocker as BSA
  `AttestAndSign ... PERMISSION_DENIED`. Fixing it would need a Google-accepted hardware attestation chain; not possible on this laptop, and faking one is not something to build.
  aidiag ping-as-uid results (user-0 uids 10107/10118/10119 "unknown host", shell 2000 fine) are an artifact: the foreground user is 10; test user-10 uids (1010107...) if this is revisited.
  Cloud-only features (Gemini app chat, Search) do not depend on AICore. Not yet checked on hardware: whether the Gemini app itself answers.
- 2026-10-16 **Discord report: Acer Aspire Lite AL15-52 (i5-1235U), kernel #32, "no Wi-Fi", everything else works. Two causes, both fixed in the repo.**
  (1) Their image predates `3626366`: the published/built `dist/gbOS86-20261016.img` (13:17) has no `gbos_load_opmodes` in `hwdetect.sh`, so iwlwifi loads but
  `iwlmvm` never does (same as NVMe boot-20). Kernel is byte-identical, so a stick is fixed by copying `usb/hook.sh` + `usb/hwdetect.sh` to `gbos/` (or rebuild).
  (2) `tools/fetch_firmware.sh` regex for `rtw89/` only matched `rtw8922a`: no firmware for RTL8852A/B/BT/C or 8851B (RTL8852BE is the usual Wi-Fi 6 card in budget Acers),
  so the v32 firmware release had none. Regex fixed; those 12 files added; release **v33** carries `gbos86-firmware-d5455717-sof2026.09.2-r2.tar.xz`
  (sha256 877f6fe9…), and `build.sh` now fetches firmware from v33 and re-downloads it if `usb/firmware/rtw89/rtw8852b_fw.bin` is missing (so existing checkouts upgrade).
  The reporter's card model is unknown (their report had no lspci): ask for `gbos/logs/boot-N/hwdetect.txt` after the rebuild (`wifi=` line, "pci devices without a driver", "missing firmware").
  Not verified on hardware (no such laptop here); AX201/AX211 (so-a0-hr/gf firmware) and RTL8852BE (rtw89_8852be) are both now covered.
- 2026-10-16 **Acer Aspire Lite AL15-52 Bluetooth (Intel AX201, USB 8087:0026): firmware name was a WHENCE link we never shipped.** Reporter's hwdetect.txt: btusb bound and
  `hci0` exists, but `Bluetooth: hci0: Failed to load Intel firmware file intel/ibt-0040-1050.sfi (-2)`. linux-firmware's WHENCE has `Link: intel/ibt-0040-1050.sfi -> ibt-0040-0041.sfi`
  (and .ddc); Links only exist after linux-firmware's install script, and `tools/fetch_firmware.sh` reads `git ls-tree`, so every Link was skipped. Same for `ibt-0040-4150 -> 0040-0041`
  and `ibt-0040-2120 -> 0040-1020`. `fetch_firmware.sh` now copies the target for each WHENCE Link whose target we ship (FAT has no symlinks). Local `usb/firmware/intel` has the six
  files; stick fix for the reporter = `dist/gbos-bt-fix-ibt-0040-1050.zip` (merge into the stick's `gbos/`, then Update). Permanent fix 2026-10-17: release **v34** carries `gbos86-firmware-d5455717-sof2026.09.2-r3.tar.xz`
  (v33 + those six files, 38 MB, sha256 653a6a22…); `build.sh` fetches it and re-downloads when `usb/firmware/intel/ibt-0040-1050.sfi` is missing (existing checkouts upgrade). Other Links (not only Bluetooth) are now picked up on the next full fetch; not yet re-run (needs the 1.5 GB linux-firmware clone). Not verified on hardware.
- 2026-10-17 **Firmware for every driver we ship (release v35).** Instead of fixing one missing name per bug report: `tools/fw_wanted.py` reads the `firmware=` strings out of every `.ko` in
  `usb/modules` (MODULE_FIRMWARE) and resolves them against linux-firmware (exact file, WHENCE Link target, wildcard, iwlwifi "newest API <= the driver's max"); `fetch_firmware.sh` ships all of that plus the
  families whose names are built at run time (ath10k/ath12k WCN7850, rtw88/rtw89, rtl_bt, rtl_nic, qca, brcm, cypress, cirrus, xe, intel/ish). Before: amdgpu (575 files, no AMD GPU could initialise), r8169/r8152 `rtl_nic/*`
  (the reporter's RTL8168 had none), btmtk/mt7615/mt76x2, brcmfmac, most cirrus amp tuning, ath12k. 309 -> 484 MB on the stick, 70 MB download. linux-firmware 12dfe670 (was d5455717; files re-extracted over the old).
  Still absent because linux-firmware has no such file: pre-production iwlwifi steppings (bz-a0, ma-a0, sc-a0...), SDIO-only brcm/rtl8723*s boards, amdgpu `*_cap.bin`/ip_discovery, ca0132 ctefx-desktop/r3di
  (list: `unresolved.txt` in the fetch work dir). Release **v35** `gbos86-firmware-12dfe670-sof2026.09.2-r4.tar.xz` (sha256 8ad4ce62…); `build.sh` fetches it and re-downloads when `usb/firmware/rtl_nic/rtl8168h-2.fw`
  is missing. Not verified on hardware; the Update path copies firmware only when the stamp changes (it will, this once).
- 2026-10-17 **eMMC support (installer + boot), kernel #33, tested in QEMU only.** `usb/install.sh` now takes an internal NVMe *or eMMC* as the target: eMMC = `mmcblkN` whose `device/type` is `MMC`
  (SD cards/readers and the stick are never candidates; `mmcblkNbootX`/`rpmb` don't match), serial = MMC `serial` (CID if empty/zero), partitions `mmcblk0p1..4`, `INSTALL_DISK=` in gbos.cfg chooses among several
  disks. The host controller driver `sdhci-pci` is a module, so: install.sh binds the stick's module tree and `modprobe`s it if the kernel doesn't have it (works on #32), and **stage0 now loads it before the
  volume search** (needs the initramfs in the kernel: #33 = #32 + `sdhci`, `sdhci-uhs2`, `cqhci`, `sdhci-pci` in `usb/modroot`, depmod'ed in gbos-kbuild, relinked with `kernel/isa_emu_patch/build.sh`;
  `usb/out/BOOTX64.EFI` is now #33, `.32` kept). Not supported: ACPI-enumerated eMMC (`MMC_SDHCI_ACPI` is off; some AMD/older Atom). Test: `kernel/isa_emu_test/run_installer_emmc.py usb/out/BOOTX64.EFI --boot`
  prints `ISA_EMU_INSTALLER_EMMC_PASS` (22 checks: wrong-serial cancel writes nothing, install, GPT/FAT/ext4 verified on the host, update, boot from the eMMC alone, NVMe still works, SD card refused).
  Test traps: QEMU's emulated eMMC *erases by really writing 0xFF*, so mke2fs's discard of the 25 GB data partition filled a Mac's disk (the test now passes `-E nodiscard` when `gbos_install_confirm=` is set,
  and aborts if the image passes 6 GB); QEMU stores the eMMC boot partitions first in the image, so the user area (GPT) is not at offset 0. Never run on a real eMMC laptop.
- 2026-10-17 **Persistent USB / looper image target (build.sh & tools/make_gbos86_image.sh) CONFIRMED on hardware.** Added
  a second build target variant: `looper` (persistent GPT image), alongside the existing `standard` live/installer image.
  The looper layout mirrors the internal disk installation: p1 `GBOS_EFI` (512 MiB FAT32), p2 `GBOSSYS` (8 GiB FAT32, `MODE=boot`),
  p3 `GBOS_META` (64 MiB ext4), and p4 `GBOS_DATA` (user-selected sparse ext4, default 64 GB). Sparse allocation via `truncate`,
  `mke2fs -E nodiscard -m 0`, and `dd conv=notrunc,sparse` ensures the resulting image on the host disk physically takes only
  ~6.5 GB (system payload + ext4 metadata) and expands dynamically up to the defined size. `build.sh` now interactively asks
  which variant to build (1: Standard live/installer, 2: Looper persistent) and prompts for the userdata capacity in GB, or
  accepts flags (`--standard`, `--looper`, `--data-size <GB>`). Hardware test confirmed by user: flashed to a USB drive,
  boots directly to desktop with `MODE=boot`, `GBOS_META` and `GBOS_DATA` mount cleanly, and apps/data persist across reboots.
- 2026-10-17 **SATA (M.2 SATA / 2.5") install support, tested in QEMU only.** `usb/install.sh` takes a SATA disk as the target: `sdX` whose sysfs path goes through a libata `ataN/hostM` port (USB sticks, USB-SATA bridges, virtio disks are
  `sdX` too but never match). SATA is only a candidate when there is no NVMe/eMMC (an NVMe + SATA data disk keeps installing to the NVMe, as before); `INSTALL_DISK=sda` picks it explicitly. No kernel change (`SATA_AHCI`, `SCSI`, `BLK_DEV_SD` are `=y` in #33),
  and `stage0_init.sh` is unchanged (it is inside the kernel image). Serial: `device/vpd_pg80` is **empty** on libata disks in this kernel, so the serial (and the full model) come from `device/wwid`, which is `t10.ATA` + vendor + 40-char model + serial; the sysfs `model` is cut to 16 chars.
  Partition names `sda1..4` (no `p`): the partition listing/umount greps were `^${T}p` and are now `^${T}p\?[0-9]`. Test: `kernel/isa_emu_test/run_installer_sata.py usb/out/BOOTX64.EFI` prints `ISA_EMU_INSTALLER_SATA_PASS` (8 steps: cancel, install, update, boot from SATA alone, NVMe+SATA,
  INSTALL_DISK=sda, two SATA refused, USB disk refused). NOT tried on real hardware (the NUC8BEH is the obvious candidate; its BIOS must be in AHCI, not RST/RAID). Known gap: with the stick in and gbos already installed on SATA, libata may enumerate after the stick, so the boot
  menu could miss "Boot the installed gbOS86"; a short wait in `stage0_init.sh` would fix it but needs a kernel rebuild, not done. The eMMC test's "no internal ..." message strings were updated to the new wording.
- 2026-10-17 **No sound on SOF machines (Dell Latitude 7391 2-in-1, i7-10510U, issue #1) = missing machine driver, fixed with one module (no kernel rebuild).** Logs from 3 NVMe boots, all kernel #33. `snd_hda_intel` hands the PCH HDA controller (8086:02c8) to SOF
  (`Digital mics found on Skylake+ platform, using SOF driver`, NHLT lists 2 DMICs), and SOF gets as far as `hda codecs found, mask 5`, `using HDA machine driver skl_hda_dsp_generic now`, firmware `intel/sof/sof-cml.ri` booted (`Firmware info: version 2:2:0-57864`).
  Then nothing: no `Topology:` line, no ASoC card, `/proc/asound/cards` = `--- no soundcards ---`, `hwdetect: devices after 15s: ... sound= `, and `lsmod` has the SOF core but not `snd_soc_skl_hda_dsp`. Cause: `# CONFIG_SND_SOC_INTEL_SKL_HDA_DSP_GENERIC_MACH is not set` in
  the #31+ `--generic` config (`configure_whl.py` listed `SND_SOC_INTEL_HDA_DSP_COMMON` and the other SOF machines but not this one), so the platform device `skl_hda_dsp_generic` that SOF registers never had a driver. Since #32 dropped `snd_intel_dspcfg.dsp_driver=1`, this hits every laptop
  with digital mics and an HDA codec (most 8th-12th gen laptops), not just this one. dras then logs `AlsaCardManager::init succeeded but internal card not found; retrying` x100, `AudioPolicy not available on AudioService start!` and `AudioFlinger: createTrack() ... error -19`:
  no audio policy at all, which is why Bluetooth audio did not work either (hci0 itself is fine: `ibt-19-0-4.sfi` loaded). The ueventd `Failed to insmod /vendor/lib/modules/snd-*.ko: Exec format error` lines are Google's vendor modules for Google's kernel and are expected.
  Fix: `SND_SOC_INTEL_SKL_HDA_DSP_GENERIC_MACH=m` in `kernel/configure_whl.py` (the module only depends on `snd-soc-intel-sof-board-helpers` + `snd-soc-intel-hda-dsp-common`, both already shipped; `TRIM_UNUSED_KSYMS` is off). Built with `make modules` in a *copy* of the gbos-ksrc volume (bzImage byte-identical to #33,
  same signing key and vermagic as the shipped modules), `depmod -e -E Module.symvers` reports no unresolved symbols or CRC mismatches; only `modules.{dep,alias}{,.bin}` change. QEMU on the shipped `usb/out/BOOTX64.EFI` (#33): `modprobe platform:skl_hda_dsp_generic` loads it plus its two dependencies
  with no dmesg errors, and fails with the old tree (`not found in modules.dep`). `hook.sh`: for the SOF HDA card (id `sofhdadsp`) the generated HAL config now uses HDMI PCM 3/4/5 (legacy HDA is 3/7/8) and the first `DMIC` capture PCM from `/proc/asound/pcm` for the internal mic.
  NOT verified on hardware: that the card then registers (topology `sof-hda-generic-2ch.tplg` is in the firmware release but was never loaded yet), PCM numbers and jack names in the generated config, DMIC gain, Bluetooth audio. Delivery: stick fix `dist/gbos-audio-fix-sof-hda.zip` (merge into the stick, Update);
  permanent: a modules release asset `gbos86-modules-6.18.21-gbos-whl-r2.tar.xz` (sha256 8d05757c6dcd531d34fd7b86ee1c5fc4c957abee491fc8185b8fb133a68a4370) must be uploaded and `build.sh` `REL`/`MODS`/`MODS_SHA` pointed at it (release v36, build.sh updated). If the card still does not appear, the next log lines to read are `sof-audio-pci-intel-cnl` after `Firmware: ABI` in kmsg.log and `logs/boot-N/audio.txt`.
