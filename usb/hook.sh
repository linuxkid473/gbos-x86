# gbos pre-Android hook: sourced by stage0 just before switch_root (Android root at /new,
# helpers log/fail/shell and $G=/gbos/gbos available). Edit freely, no kernel rebuild needed.

# Reset one-shot MODE=live back to MODE=ask so subsequent boots prompt the user
if [ -f "$G/gbos.cfg" ] && grep -q '^MODE=live' "$G/gbos.cfg" 2>/dev/null; then
  sed -i 's/^MODE=live/MODE=ask/' "$G/gbos.cfg" 2>/dev/null || true
  sync 2>/dev/null || true
fi

# Library patches. patched/MANIFEST (written by tools/port_build.py) has one line per patch: kind, the path in the Android root
# (/new/...), the sha256 of the exact original the patch was made from, and the patched file (relative to gbos/). A patch is
# bound only over a file with that hash, so a new Google build never gets a stale patch: the line says "NOT patched" and shows
# the hash it found, which is what tools/port_build.sh needs to regenerate the patch. kind "early" = also bound by early_patch.sh
# before the first dmctl (then already mounted here), "apex" lines belong to apex_patch.sh.
gbos_bind_patches() {
  gp_ok=0; gp_bad=0
  if [ ! -f $G/patched/MANIFEST ]; then
    log "hook: PATCH-STATUS patched/MANIFEST missing: no library patches (SHA-NI/AVX-VNNI code will be emulated, very slow)"
    return 0
  fi
  while read -r gp_kind gp_target gp_want gp_file gp_rest; do
    case $gp_kind in early|bind) ;; *) continue;; esac
    if grep -q " $gp_target " /proc/mounts; then
      log "hook: $gp_target already patched (early_patch.sh)"; gp_ok=$((gp_ok+1))
    elif [ ! -f $G/$gp_file ]; then
      log "hook: $gp_target NOT patched (missing $gp_file)"; gp_bad=$((gp_bad+1))
    elif [ ! -f $gp_target ]; then
      log "hook: $gp_target NOT patched (not in this Google build)"; gp_bad=$((gp_bad+1))
    else
      gp_have=$(sha256sum $gp_target | cut -d' ' -f1)
      if [ "$gp_have" != "$gp_want" ]; then
        log "hook: $gp_target NOT patched (original is $gp_have, patch was made for $gp_want: run tools/port_build.sh)"; gp_bad=$((gp_bad+1))
      elif mount --bind $G/$gp_file $gp_target; then
        log "hook: patched $gp_target ($gp_file) active"; gp_ok=$((gp_ok+1))
      else
        log "hook: $gp_target NOT patched (bind mount failed)"; gp_bad=$((gp_bad+1))
      fi
    fi
  done < $G/patched/MANIFEST
  log "hook: PATCH-STATUS $gp_ok applied, $gp_bad not applied"
}
# Google's userspace needs AVX2: the dynamic linker and bionic libc (__linker_init: vxorps/vmovups ymm, vpbroadcastb)
# use AVX2 instructions, and the kernel's emulator only covers the newer extensions (GFNI, VNNI, SHA-NI, ...),
# not baseline AVX/AVX2. A CPU without AVX2 (Celeron/Pentium/Atom, or 2nd/3rd Gen Sandy/Ivy Bridge) kills init
# with SIGILL (exitcode=0x00000004), leading to a kernel panic. Say so on screen instead.
# REQUIRE_AVX=0 in gbos.cfg boots anyway.
if [ "${REQUIRE_AVX:-1}" != 0 ]; then
  if ! grep -qw avx2 ${CPUINFO:-/proc/cpuinfo}; then
    log "CPU: $(grep -m1 'model name' ${CPUINFO:-/proc/cpuinfo})"
    if ! grep -qw avx ${CPUINFO:-/proc/cpuinfo}; then
      fail "this CPU has no AVX, so Google's init cannot run on it (not fixable by drivers). Power off, or type exit to try anyway (REQUIRE_AVX=0 in gbos.cfg skips this check)."
    else
      fail "this CPU lacks AVX2 instruction support (Sandy/Ivy Bridge 2nd/3rd Gen). Google's userspace executes AVX2 (vpbroadcastb) in init/linker64, requiring Intel 4th Gen Haswell / 6th Gen Skylake+ or AMD Zen. Power off, or type exit to try anyway (REQUIRE_AVX=0 in gbos.cfg skips this check)."
    fi
  fi
fi

# zram is a module here (=m in Google's config too); mmd_setup aborts without /sys/block/zram0.
modprobe zram && log "hook: zram loaded" || log "hook: modprobe zram FAILED"

# Generic hardware bring-up (gbOS86 for any Intel gen6+ laptop): usb/hwdetect.sh binds the full module tree from
# gbos/modules, coldplugs every device by modalias (Wi-Fi, Bluetooth, audio, input, hotkeys, backlight, ...),
# with firmware from gbos/firmware, and writes hwdetect.txt. Falls back to the original Latitude 5300 sequence
# (below) when a stick has no gbos/modules.
FWP=/sys/module/firmware_class/parameters/path
if [ "$HWDETECT" != 0 ] && [ -f $G/hwdetect.sh ] && [ -d $G/modules/lib/modules ]; then
  . $G/hwdetect.sh
  gbos_hwdetect 15
  # zram (Android's mmd_setup aborts without it, see "zram setup failed" in crash.txt) must be loadable: when it is not in the
  # initramfs the early modprobe above fails, and the module tree from the stick is mounted only now.
  grep -q '^zram ' /proc/modules || { modprobe zram && log "hook: zram loaded (after hwdetect)" || log "hook: modprobe zram FAILED (after hwdetect)"; }
  # Late firmware requests (after switch_root) see the stick as /postinstall.
  echo -n /postinstall/gbos/firmware > $FWP
else
  # Wi-Fi: CNVi 9560/9462 (8086:9df0) needs iwlwifi-9000-pu-b0-jf-b0-46.ucode (linux-firmware), which
  # Google's vendor image doesn't carry. Point the firmware loader at the stick, load the driver, and
  # wait for the firmware so it isn't requested after switch_root (where /gbos no longer exists).
  # Afterwards point it at the same folder as seen from Android (/postinstall = the stick).
  FWP=/sys/module/firmware_class/parameters/path
  if [ -f $G/firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode ]; then
    echo -n $G/firmware > $FWP
    modprobe iwlmvm && log "hook: iwlmvm loaded" || log "hook: modprobe iwlmvm FAILED"
    i=0; while [ $i -lt 15 ] && ! dmesg | grep -q 'iwlwifi.*loaded firmware version'; do sleep 1; i=$((i+1)); done
    log "hook: wifi after ${i}s: $(dmesg | grep -m1 -o 'iwlwifi.*loaded firmware version.*') ifaces: $(ls /sys/class/net | tr '\n' ' ')"
    echo -n /postinstall/gbos/firmware > $FWP
  fi

  # Bluetooth: the 9560's Bluetooth is a USB device (8087:0aaa) and needs btusb, which Google's kernel
  # (PCIe Bluetooth) doesn't build. gbos/kmod/ has btusb/btintel/btrtl from our tree; bluetooth and btbcm
  # are in the initramfs. Firmware: intel/ibt-17-16-1.sfi/.ddc in gbos/firmware.
  if [ -f $G/kmod/btusb.ko ]; then
    [ -f $G/firmware/intel/ibt-17-16-1.sfi ] || log "hook: Bluetooth firmware intel/ibt-17-16-1.sfi missing"
    # btintel loads the firmware during probe, i.e. right now: the loader must look on this volume
    # ($G), not at /postinstall, which only exists after switch_root.
    echo -n $G/firmware > $FWP
    modprobe bluetooth && modprobe btbcm && insmod $G/kmod/btintel.ko && insmod $G/kmod/btrtl.ko \
      && insmod $G/kmod/btusb.ko && log "hook: btusb loaded ($(ls /sys/class/bluetooth 2>/dev/null | tr '\n' ' '))" \
      || log "hook: btusb load FAILED: $(dmesg | grep -iE 'btusb|btintel|bluetooth' | tail -2 | tr '\n' ' ')"
    i=0; while [ $i -lt 20 ] && ! dmesg | grep -qE 'hci0: (Firmware loaded|Failed to load Intel firmware)'; do sleep 1; i=$((i+1)); done
    log "hook: bluetooth: $(dmesg | grep -oE 'hci0: (Firmware loaded in [0-9]+ usecs|Failed to load Intel firmware[^)]*\)|Found device firmware: [^ ]+)' | tr '\n' ' ')"
    echo -n /postinstall/gbos/firmware > $FWP
  fi
fi

# --- software rendering for GPUs Google's image has no driver for (AMD) -- gbos_gpu_* functions ---
# Google's image renders through ANGLE -> Vulkan, and its only Vulkan driver is Intel's ANV (/vendor/lib64/hw/vulkan.intel.so);
# buffers come from minigbm (gralloc, /vendor/lib64/libminigbm_gralloc.so), which has no amdgpu backend. On a laptop whose GPU
# is not Intel's, "software GPU" mode swaps in (a) SwiftShader, a CPU Vulkan driver, over vulkan.intel.so (ro.hardware.vulkan=intel
# keeps working, no prop change) and (b) a gralloc whose generic linear-buffer backend answers to DRM driver "amdgpu" and which
# lets the CPU driver lock any buffer (tools/patch_gralloc_amdgpu.py; likewise libgbm.so, tools/patch_libgbm_amdgpu.py).
# GPU_SOFTWARE in gbos.cfg: auto (default: only when no Intel GPU is present and another GPU driver is bound, or Intel Gen <= 8
# which lacks Vulkan support in Google's image), 1 (force, even on Intel), 0 (never). SYSFS= and NEWROOT= are only for tools/test_gpu_select.sh.
GBOS_GRALLOC_SHA=d48536951fd0e9643e71a5c8843e90833ce3e212e182782185470e2fbbfad673
GBOS_LIBGBM_SHA=64a0f2537a5917822cb3f538dc74229e7ccf4ca45a30265bb97f742678ba294e
GBOS_VKINTEL_SHA=5c687450523ea9ed9e8d4d9cc98ef4fef6c8d592d46db4bf425ad571dc6f7c73
# "cardN driver" per DRM card (connectors like card0-eDP-1 are skipped); driver "none" = no driver bound.
gbos_gpu_cards() {
  for c in ${SYSFS:-/sys}/class/drm/card*; do
    [ -e "$c" ] || continue
    case ${c##*/} in card*-*) continue;; esac
    d=$(readlink $c/device/driver 2>/dev/null); d=${d##*/}
    echo "${c##*/} ${d:-none}"
  done
}
# A driver that really drives a GPU, as opposed to the firmware framebuffer (simpledrm) or the virtual test driver (vkms).
gbos_gpu_real() { case "$1" in none|simpledrm|simple-framebuffer|efifb|vesafb|vkms) return 1;; esac; return 0; }
# PCI display controllers (class 0x03xxxx) by vendor: "yes" when one is Intel's.
gbos_gpu_intel_pci() {
  for d in ${SYSFS:-/sys}/bus/pci/devices/*; do
    [ -e "$d/class" ] || continue
    case "$(cat $d/class)" in 0x03*) [ "$(cat $d/vendor)" = 0x8086 ] && return 0;; esac
  done
  return 1
}
# Checks if an Intel GPU has native Vulkan hardware support in Google's image (Gen 9 Skylake+).
# Older Intel GPUs (Gen 8 Broadwell, Haswell, Valleyview, Cherryview, Ivy Bridge, Sandy Bridge)
# are rejected by Google's vulkan.intel.so (devinfo->ver < 9) and must fall back to SwiftShader.
gbos_gpu_intel_has_vulkan() {
  [ "${1:-}" = xe ] && return 0

  # Check kernel dmesg for integrated display version (< 9 is Gen 8 or older)
  _ver=$(dmesg 2>/dev/null | grep -m1 -o 'integrated display version [0-9]*' | awk '{print $4}')
  if [ -n "$_ver" ] && [ "$_ver" -lt 9 ] 2>/dev/null; then return 1; fi
  if dmesg 2>/dev/null | grep -qiE 'Found (broadwell|haswell|ivybridge|sandybridge|valleyview|cherryview)'; then return 1; fi

  # Check PCI device IDs of Intel display controllers (legacy Gen <= 8)
  for _d in ${SYSFS:-/sys}/bus/pci/devices/*; do
    [ -e "$_d/class" ] || continue
    case "$(cat "$_d/class" 2>/dev/null)" in 0x03*)
      [ "$(cat "$_d/vendor" 2>/dev/null)" = 0x8086 ] || continue
      _dev=$(cat "$_d/device" 2>/dev/null | tr '[A-Z]' '[a-z]')
      case "$_dev" in
        0x16*) return 1;; # Broadwell Gen 8
        0x04*|0x0a*|0x0c*|0x0d*) return 1;; # Haswell Gen 7.5
        0x0f*|0x22*) return 1;; # Valleyview / Cherryview
        0x015*|0x016*) return 1;; # Ivy Bridge Gen 7
        0x010*|0x011*|0x012*) return 1;; # Sandy Bridge Gen 6
      esac
      ;;
    esac
  done
  return 0
}
# Sets GPU_SW=1 (software rendering wanted) or 0 and logs why. $1 = seconds to wait for a non-Intel driver to bind.
gbos_gpu_decide() {
  GPU_SW=0
  case "${GPU_SOFTWARE:-auto}" in
    0) log "hook: software rendering: off (GPU_SOFTWARE=0)"; return 0;;
    1) GPU_SW=1; log "hook: software rendering: on (GPU_SOFTWARE=1, forced)"; return 0;;
  esac
  w=0; real=; intel=
  while :; do
    real=; intel=
    for e in $(gbos_gpu_cards | tr ' ' ','); do
      drv=${e#*,}
      case "$drv" in i915|xe) intel=$drv;; *) gbos_gpu_real "$drv" && real="$real ${e%%,*}:$drv";; esac
    done
    # Intel (bound, or at least present on the PCI bus): check if supported by image Vulkan driver (Gen 9+)
    if [ -n "$intel" ]; then
      if gbos_gpu_intel_has_vulkan "$intel"; then
        log "hook: software rendering: off (auto; Intel GPU driver $intel bound, Gen 9+ hardware Vulkan supported)"
        return 0
      else
        GPU_SW=1
        log "hook: software rendering: on (auto; Intel GPU driver $intel bound, but Gen <= 8 lacks Vulkan in image; using SwiftShader)"
        return 0
      fi
    fi
    if gbos_gpu_intel_pci; then
      if gbos_gpu_intel_has_vulkan ""; then
        log "hook: software rendering: off (auto; Intel display controller present)"
        return 0
      else
        GPU_SW=1
        log "hook: software rendering: on (auto; Intel display controller present, but Gen <= 8 lacks Vulkan in image; using SwiftShader)"
        return 0
      fi
    fi
    [ -n "$real" ] && break
    [ $w -ge ${1:-0} ] && break
    sleep 1; w=$((w+1))
  done
  if [ -n "$real" ]; then
    GPU_SW=1; log "hook: software rendering: on (auto; no Intel GPU, DRM:$real, after ${w}s)"
  else
    log "hook: software rendering: off (auto; no DRM GPU driver bound after ${w}s; cards: $(gbos_gpu_cards | tr '\n' ';'))"
  fi
}
# Bind patched/$2 over $3 when it exists and $3 is the exact original ($4 = its sha256). $1 = what it is, for the log.
gbos_gpu_bind() {
  if [ ! -f "$G/patched/$2" ]; then log "hook: software rendering: gbos/patched/$2 missing ($1 not replaced; stick too old?)"; return 1; fi
  if [ "$(sha256sum "$3" | cut -d' ' -f1)" != "$4" ]; then log "hook: software rendering: $1 NOT replaced (unexpected original, new Google build?)"; return 1; fi
  mount --bind "$G/patched/$2" "$3" && log "hook: software rendering: $1 replaced" || { log "hook: software rendering: $1 bind FAILED"; return 1; }
}
# radv or swiftshader: which Vulkan driver to use (GPU_VULKAN=radv|swiftshader forces one; auto = RADV only if the stick has it and amdgpu is bound).
gbos_gpu_vulkan() {
  case "${GPU_VULKAN:-auto}" in
    swiftshader) echo swiftshader; return 0;;
    radv) echo radv; return 0;;
  esac
  if [ -f "$G/patched/vulkan.radv.so" ] && gbos_gpu_cards | grep -q ' amdgpu$'; then echo radv; else echo swiftshader; fi
}
# (a) gralloc: minigbm gets a backend for DRM driver "amdgpu" (linear dumb buffers); libgbm (camera/codec HALs) likewise.
# (b) SwiftShader over the Intel Vulkan driver. Without (a) or (b) nothing can draw, so any failure there cancels the prop changes.
gbos_gpu_enable() {
  V=${NEWROOT:-/new}/vendor
  gbos_gpu_bind "gralloc (libminigbm_gralloc, amdgpu backend)" libminigbm_gralloc.amdgpu.so $V/lib64/libminigbm_gralloc.so $GBOS_GRALLOC_SHA || GPU_SW=0
  gbos_gpu_bind "libgbm (amdgpu backend)" libgbm.amdgpu.so $V/lib64/libgbm.so $GBOS_LIBGBM_SHA
  # RADV (Mesa's AMD Vulkan driver, tools/radv-android/build.sh) when it is on the stick and an amdgpu card is bound; otherwise (or when it
  # cannot be bound) SwiftShader. GPU_VULKAN in gbos.cfg: auto (default), radv, swiftshader.
  if [ "$(gbos_gpu_vulkan)" = radv ] && gbos_gpu_bind "Vulkan driver (RADV, AMD GPU)" vulkan.radv.so $V/lib64/hw/vulkan.intel.so $GBOS_VKINTEL_SHA; then :
  else gbos_gpu_bind "Vulkan driver (SwiftShader, CPU)" vulkan.swiftshader.so $V/lib64/hw/vulkan.intel.so $GBOS_VKINTEL_SHA || GPU_SW=0
  fi
}
# --- end software rendering ---

# Let the DRM driver (amdgpu probes for several seconds and loads ~20 firmware files) finish before deciding.
gbos_gpu_decide ${GPU_WAIT:-25}
[ "$GPU_SW" = 1 ] && gbos_gpu_enable

# Set up DRM device nodes cleanly before switch_root
gbos_drm_setup() {
  mkdir -p -m 0755 /dev/dri
  for _c in /sys/class/drm/card[0-9]* /sys/class/drm/renderD[0-9]*; do
    [ -f "$_c/dev" ] || continue
    _name=${_c##*/}
    _dev=$(cat "$_c/dev")
    [ -c "/dev/dri/$_name" ] || mknod -m 0666 "/dev/dri/$_name" c ${_dev%:*} ${_dev#*:}
  done
  _prim=
  for _s in /sys/class/drm/card*-*/status; do
    [ -f "$_s" ] || continue
    if grep -q '^connected' "$_s" 2>/dev/null; then
      _prim=${_s%/status}; _prim=${_prim##*/}; _prim=${_prim%%-*}; break
    fi
  done
  if [ -z "$_prim" ]; then
    for _c in /sys/class/drm/card[0-9]*; do
      case ${_c##*/} in card*-*) continue;; esac
      _drv=$(readlink "$_c/device/driver" 2>/dev/null); _drv=${_drv##*/}
      case "$_drv" in i915|xe|amdgpu|nouveau|radeon) _prim=${_c##*/}; break;; esac
    done
  fi
  if [ -n "$_prim" ] && [ "$_prim" != "card0" ] && [ -f "/sys/class/drm/$_prim/dev" ]; then
    _pdev=$(cat "/sys/class/drm/$_prim/dev")
    rm -f /dev/dri/card0
    mknod -m 0666 /dev/dri/card0 c ${_pdev%:*} ${_pdev#*:}
    log "hook: primary display on $_prim ($_pdev), mapped to /dev/dri/card0"
  fi
  chmod 0666 /dev/dri/* 2>/dev/null || true
}
gbos_drm_setup

# Maximize CPU governor early to prevent throttling during init and software rendering
for _g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$_g" 2>/dev/null || true
done
for _e in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
  echo performance > "$_e" 2>/dev/null || true
done

# drm.debug=0x04 (KMS only; not ATOMIC, which logs every frame): i915 dumps gamma/CSC mode and range per modeset while
# drm_hwcomposer first sets up the panel. gbos.rc turns it off at boot_completed. Washed-out colours.
echo 0x04 > /sys/module/drm/parameters/debug 2>/dev/null && log "hook: drm.debug=0x04 (KMS) until boot_completed"

# Fake ChromeOS boot disk for the boot HAL: misc + vbmeta_a/vbmeta_b with cgpt slot attributes.
if [ -f $G/bootdisk.img ]; then
  losetup -P /dev/loop6 $G/bootdisk.img; sleep 1; mdev -s
  mknod -m 600 /dev/block/loop6 b 7 6 2>/dev/null
  i=1
  for n in misc vbmeta_a vbmeta_b; do
    [ -e /dev/block/loop6p$i ] || mknod -m 600 /dev/block/loop6p$i b $(tr : ' ' < /sys/class/block/loop6p$i/dev)
    ln -sf /dev/block/loop6p$i /dev/block/by-name/$n
    i=$((i+1))
  done
  for b in boot_a boot_b vendor_boot_a vendor_boot_b init_boot_a init_boot_b; do
    ln -sf /dev/block/loop6p1 /dev/block/by-name/$b
  done
  log "hook: bootdisk on loop6: $(ls /sys/class/block | grep loop6p | tr '\n' ' ')"
fi

# Mask SetupWizard APK directories so Android skips initial setup and launches Desktop directly
mkdir -p /tmp/empty_suw
for suw_dir in /new/product/priv-app/*SetupWizard* /new/system_ext/priv-app/*SetupWizard* /new/product/priv-app/*setupwizard* /new/system_ext/priv-app/*setupwizard*; do
  if [ -d "$suw_dir" ]; then
    mount --bind /tmp/empty_suw "$suw_dir" && log "hook: masked $suw_dir with empty dir"
  fi
done

# Fake crossystem (no ChromeOS firmware on this laptop)
[ -f $G/crossystem ] && mount --bind $G/crossystem /new/vendor/bin/crossystem && log "hook: crossystem faked"

# Logcat to the stick: expose the USB volume inside Android at /postinstall, and overlay
# /system/etc/init with a copy plus gbos.rc (a logcat service writing to this boot's log dir).
# "vid:pid" (lower-case hex, as in Android's IDC file names) of every touchpad that is not on USB, from /proc/bus/input/devices.
# INPUTS= is only for tests.
gbos_touchpads() {
  awk -v RS= -F'\n' '{
    bus = ""; v = ""; p = ""; name = ""
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^I: /) { n = split($i, a, " "); for (j = 1; j <= n; j++) {
        if (a[j] ~ /^Bus=/) bus = substr(a[j], 5); if (a[j] ~ /^Vendor=/) v = substr(a[j], 8); if (a[j] ~ /^Product=/) p = substr(a[j], 9) } }
      if ($i ~ /^N: Name=/) name = tolower($i)
    }
    if (name ~ /touchpad|trackpad/ && bus != "0003" && v != "" && p != "") print tolower(v) ":" tolower(p)
  }' ${INPUTS:-/proc/bus/input/devices} 2>/dev/null | sort -u
}
if mount --bind /gbos /new/postinstall; then
  mkdir -p /tmp/sysinit && cp -a /new/system/etc/init/. /tmp/sysinit/
  rm -f /tmp/sysinit/gscd.rc   # gscd needs Google's security chip (/dev/tpm0); it only crash-loops here
  rm -f /tmp/sysinit/timberslide.rc # timberslide needs ChromeOS EC (/dev/cros_ec); crash-loops on generic PCs
  rm -f /tmp/sysinit/udsattestation.rc # udsattestation needs Titan C security chip; loops on generic PCs
  rm -f /tmp/sysinit/mmd.rc            # disable mmd & 24GB zram/8GB disk swap on pendrive
  # RESOLUTION in gbos.cfg: logical display size (e.g. 2400x1350) that SurfaceFlinger renders at and the
  # display hardware scales to the 1920x1080 panel, so everything looks smaller with stock density.
  case "$RESOLUTION" in [0-9]*x[0-9]*) WMSIZE=$RESOLUTION;; *) WMSIZE=reset;; esac
  # Touchpad flicks: Android's touchpad gesture engine holds motion back for Change_Timeout (200 ms) after touchdown, so a quick flick
  # (40-160 ms) never moves the cursor (Dell Inspiron 14 Plus 7440, Goodix 27C6:0D53, issue 17). Android reads a per-device IDC file
  # from /data/system/devices/idc/Vendor_<vid>_Product_<pid>.idc: one is written for every non-USB touchpad found now, at post-fs-data.
  # TOUCHPAD_FLICK=0 in gbos.cfg turns it off.
  # The Trusty security VM never answers here (software KeyMint/Gatekeeper replace it) but spins one CPU core at 100%: stopped once
  # booted. TRUSTY_VM=1 in gbos.cfg keeps it running (for debugging).
  if [ "${TRUSTY_VM:-0}" = 1 ]; then TRUSTYSTOP="# gbos: Trusty VM kept (TRUSTY_VM=1)"; else TRUSTYSTOP="stop trusty_security_protected_vm_launcher"; fi
  IDCRC=
  if [ "${TOUCHPAD_FLICK:-1}" != 0 ]; then
    for tp in $(gbos_touchpads); do
      IDCN=Vendor_${tp%:*}_Product_${tp#*:}.idc
      printf 'device.internal = 1\ntouch.deviceType = touchPad\ngestureProp.Drumroll_Suppression_Enable = 0\ngestureProp.Change_Timeout = 0.02\n' > $L/$IDCN \
        && IDCRC="$IDCRC
    copy /postinstall/gbos/logs/boot-$N/$IDCN /data/system/devices/idc/$IDCN
    chown system system /data/system/devices/idc/$IDCN
    chmod 0644 /data/system/devices/idc/$IDCN" && log "hook: touchpad $tp: IDC with Change_Timeout=0.02 queued"
    done
    [ -n "$IDCRC" ] && IDCRC="    mkdir /data/system 0775 system system
    mkdir /data/system/devices 0775 system system
    mkdir /data/system/devices/idc 0775 system system$IDCRC"
  fi
  cat > /tmp/sysinit/gbos.rc <<RC
# Rotate at 1 MB: on this stick, data written past ~4 MB of a file that stays open never reaches
# the FAT cluster chain before power-off (fsck: "size ... should at most be 4046848"). Rotating
# closes each file; logcat.txt is newest, then logcat.txt.1, .2, ...
# Rotate at 1 MB, keep last 10 files to reduce USB flash drive write wear and I/O latency
service gbos_logcat /system/bin/logcat -b all -v threadtime -r 1024 -n 10 -f /postinstall/gbos/logs/boot-$N/logcat.txt
    user root
    group root log
    seclabel u:r:shell:s0

# Crash buffer only (FATAL EXCEPTION / native crashes): small, so it survives a hard power-off.
service gbos_crashlog /system/bin/logcat -b crash -v threadtime -f /postinstall/gbos/logs/boot-$N/crash.txt
    user root
    group root log
    seclabel u:r:shell:s0

# The laptop is usually powered off by hand, so flush the logs to the stick every 5 s.
# Also executes /postinstall/gbos/run.sh as root backchannel if present.
service gbos_sync /system/bin/sh -c "while true; do sleep 5; if [ -f /postinstall/gbos/run.sh ]; then sh /postinstall/gbos/run.sh > /postinstall/gbos/run.log 2>&1; rm -f /postinstall/gbos/run.sh; fi; sync; done"
    user root
    group root
    seclabel u:r:shell:s0

# Sound diagnostics, 60 s after boot: ALSA cards/PCMs/codec (incl. control names) and audioserver state.
service gbos_audiodiag /system/bin/sh -c "sleep 60; cd /proc/asound; ( head -n 3000 cards pcm devices card*/id card*/codec*; ls -l /dev/snd; grep '^N: Name' /proc/bus/input/devices; dumpsys media.audio_flinger; dumpsys media.audio_policy ) > /postinstall/gbos/logs/boot-$N/audio.txt 2>&1; sh /postinstall/gbos/audio/mixdiag.sh $N"
    user root
    group root audio
    seclabel u:r:shell:s0
    oneshot
# DRM card0 is configured directly in stage0_init before Android init starts.
on early-init
    setprop sys.gbos_dri_fixed 1
on property:sys.gbos_dri_fixed=1
    restart vendor.drm-widevine-hal

on post-fs-data
$IDCRC
on property:sys.boot_completed=1
    # KMS debug was on during the HWC's first modesets (set early in hook.sh); turn it off again.
    write /sys/module/drm/parameters/debug 0
    # The KVM exit trace (kvmtrace.txt) only matters while the Trusty VM boots; left on it costs CPU and writes a lot.
    write /sys/kernel/tracing/tracing_on 0
    # Maximize CPU frequency governor permanently
    write /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor performance
    write /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor performance
    write /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor performance
    write /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor performance
    $TRUSTYSTOP
    start gbos_skip_setup
    start gbos_optimize
    start gbos_audiodiag
    start gbos_devname
    start gbos_iptsd
    start gbos_wmsize
    start gbos_displaydiag
    start gbos_aidiag
    start gbos_ksu

# update_engine runs normally to satisfy GMS binder without crashing

# Skip SetupWizard completely and launch desktop (disables blurs and optimizes animations)
service gbos_skip_setup /system/bin/sh -c "sleep 1; settings put --user 0 secure user_setup_complete 1; settings put --user 10 secure user_setup_complete 1; settings put secure user_setup_complete 1; settings put global device_provisioned 1; settings put global setup_wizard_has_run 1; settings put global force_resizable_activities 1; settings put global enable_freeform_support 1; settings put global ota_disable_automatic_update 1; settings put global auto_system_update 0; settings put global disable_window_blurs 1; cmd window disable-blur 1; settings put global window_animation_scale 0; settings put global transition_animation_scale 0; settings put global animator_duration_scale 0; setprop ro.setupwizard.mode DISABLED; am start --user 10 -a android.intent.action.MAIN -c android.intent.category.HOME 2>/dev/null; am start -n com.google.android.apps.nexuslauncher/.NexusLauncherActivity -a android.intent.action.MAIN -c android.intent.category.HOME 2>/dev/null"
    user root
    group root
    seclabel u:r:shell:s0
    oneshot

# GBOS System, Gaming and RAM Optimization Service
service gbos_optimize /system/bin/sh /postinstall/gbos/optimize.sh
    user root
    group root
    seclabel u:r:shell:s0
    oneshot

# AI diagnostics (Magic Pointer / AICore): usb/aidiag.sh writes logs/boot-N/ai-1.txt (90 s) and ai-2.txt (4 min).
# The gbos_aidns_* services run one ping per app uid so aidiag can compare DNS as AICore (10107) with
# as.oss (10118), the Private Compute Services app that does reach Google.
service gbos_aidiag /system/bin/sh /postinstall/gbos/aidiag.sh $N
    user root
    group root
    seclabel u:r:shell:s0
    oneshot

service gbos_aidns_10107 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_10107.txt 2>&1"
    user 10107
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

service gbos_aidns_10118 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_10118.txt 2>&1"
    user 10118
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

service gbos_aidns_10119 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_10119.txt 2>&1"
    user 10119
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

service gbos_aidns_2000 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_2000.txt 2>&1"
    user 2000
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

# KernelSU (the kernel part is built into the kernel, kernel/kernelsu/apply.sh): the userspace
# side is the daemon the kernel starts (/data/adb/ksud) and the manager app, which the kernel
# only accepts with the official signing certificate. Both come from the stick (gbos/ksu/).
# Started twice: at post-fs-data (ksud, before the kernel's init.rc runs it) and after
# boot_completed (the manager app; pm needs the framework).
service gbos_ksu /system/bin/sh /postinstall/gbos/ksu/install.sh $N ${KSU_MANAGER:-1}
    user root
    group root
    seclabel u:r:shell:s0
    oneshot
    disabled

# Logical display size (RESOLUTION in gbos.cfg); "reset" = native 1920x1080.
service gbos_wmsize /system/bin/wm size $WMSIZE
    user root
    group root
    seclabel u:r:shell:s0
    oneshot
    disabled

# Display diagnostics, 70 s after boot (washed-out colours): DRM atomic state (CTM, gamma, Broadcast RGB,
# colour pipeline), i915 pipe/dither/bpc info, panel EDID, and SurfaceFlinger/colour-mode settings.
service gbos_displaydiag /system/bin/sh -c "sleep 70; ( mount -t debugfs debugfs /sys/kernel/debug; head -n 20000 /sys/kernel/debug/dri/*/state /sys/kernel/debug/dri/*/i915_display_info /sys/kernel/debug/dri/*/i915_display_capabilities /sys/kernel/debug/dri/*/eDP-1/*; echo EDID; od -A x -t x1 /sys/class/drm/card*-eDP-1/edid; dumpsys SurfaceFlinger; dumpsys color_display; dumpsys display; settings list secure; settings list system ) > /postinstall/gbos/logs/boot-$N/display.txt 2>&1"
    user root
    group root graphics
    seclabel u:r:shell:s0
    oneshot

# Touchpad gestures on the Surface Laptop 7: iptsd (a fork) turns the touchpad's raw frames into a multitouch uinput device.
# The script exits at once without surface/iptsd on the stick and when the machine has no such touchpad; oneshot so init does not restart it.
service gbos_iptsd /system/bin/sh /postinstall/gbos/surface/iptsd-run.sh $N
    user root
    group root input
    seclabel u:r:shell:s0
    oneshot
    disabled

# "Device name" in Settings (Settings.Global.device_name): kept in step with DEVICE_MODEL from gbos.cfg.
service gbos_devname /system/bin/sh -c "sleep 15; [ -n '$DEVICE_MODEL' ] && settings put global device_name '$DEVICE_MODEL'"
    user root
    group root
    seclabel u:r:shell:s0
    oneshot
    disabled

# Software Gatekeeper (gatekeeper/soft_gk.c) in place of the Trusty one, which never answers.
service gbos_soft_gk /postinstall/gbos/soft_gk
    class hal
    user system
    group system
    seclabel u:r:hal_gatekeeper_default:s0
    interface aidl android.hardware.gatekeeper.IGatekeeper/default

# Trusty security VM (KeyMint/Gatekeeper TEE) as a plain KVM VM: no pKVM on this laptop.
# vendor/build.prop's protected_vm.supported=1 is hidden (see below), so the launcher falls back
# to a non-protected VM; servicemanager starts it on demand for the KeyMint accessors.
on early-init
    setprop ro.boot.hypervisor.vm.supported 1
    # libcutils only uses memfd for apps targeting SDK >= 37; older ones (SetupWizard) go to
    # /dev/ashmem, which needs the Rust ashmem driver. Force memfd (kernel/ashmem_shim answers
    # the ashmem ioctls on memfd).
    setprop sys.use_memfd true
on init && property:trusty.security_vm.enabled=1
    setprop desktop.security_vm.vm_image /system_ext/etc/vm/trusty_vm/desktop_trusty_ext_boot_signed.elf
    setprop trusty.security_vm.vm_cid 200
    setprop trusty.security_vm.nonsecure_vm_ready 1
    setprop ro.hardware.trusty_ipc_dev.gatekeeper VSOCK:200:0
    setprop storageproxyd_system.trusty_ipc_dev VSOCK:200:0

# atrace.rc resets ftrace at late-init; turn the KVM trace back on before the Trusty VM starts.
on early-fs
    write /sys/kernel/tracing/buffer_size_kb 8192
    write /sys/kernel/tracing/events/kvm/kvm_inj_exception/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_emulate_insn/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_userspace_exit/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_exit/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_pio/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_mmio/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_hypercall/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_fast_mmio/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_inj_virq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_pic_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_ioapic_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_msi_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_apic_accept_irq/enable 1
    # Boot the Trusty VM before any KeyMint/StrongBox client connects (vsock startup race).
    start trusty_security_protected_vm_launcher
    write /sys/kernel/tracing/tracing_on 1

on post-fs
    start gbos_logcat
    start gbos_sync
    start gbos_crashlog
on post-fs-data
    start gbos_logcat
    start gbos_sync
    start gbos_crashlog
    # KernelSU: the daemon has to be in /data/adb before the kernel's own init.rc runs it.
    start gbos_ksu
RC
  # Conscrypt (and identical) APEX libcrypto: the APEX is signed, so patch a copy and bind it over the
  # original once apexd has activated every APEX (init.rc's "wait_for_prop apexd.status activated" is in
  # on post-fs-data, before zygote-start). See apex_patch.sh.
  if [ -f $G/apex_patch.sh ] && [ "$(grep -c '^    wait_for_prop apexd.status activated$' /tmp/sysinit/hw/init.rc)" = 1 ]; then
    sed -i 's#^    wait_for_prop apexd.status activated$#&\n    exec u:r:shell:s0 root root -- /system/bin/sh /postinstall/gbos/apex_patch.sh#' /tmp/sysinit/hw/init.rc \
      && log "hook: init.rc runs apex_patch.sh after APEX activation"
  else
    log "hook: apex_patch NOT installed (script missing or init.rc anchor not unique)"
  fi
  mount -t tmpfs -o mode=0755 tmpfs /new/system/etc/init && cp -a /tmp/sysinit/. /new/system/etc/init/ \
    && log "hook: logcat -> gbos/logs/boot-$N/logcat.txt ($(ls /new/system/etc/init | wc -l) rc files)" \
    || log "hook: init overlay FAILED"
else
  log "hook: bind /gbos -> /postinstall FAILED"
fi

# vendor/build.prop overrides. Hide protected_vm.supported=1 (no pKVM here). DENSITY in gbos.cfg
# optionally replaces ro.sf.lcd_density (stock 260); unset keeps stock. To make things smaller without
# touching density, use RESOLUTION instead (gbos_wmsize in gbos.rc above).
# The Latitude's LG LP133WF6 is an sRGB panel, not wide-gamut/HDR like Google's: with these true,
# SurfaceFlinger offers DISPLAY_BT2020 and drm_hwcomposer tags eDP as BT.2020 (pale colours; the
# kernel side is fixed in i915_edp_colorspace.sh). Declare it sRGB-only.
# Auto-detect optimal UI density from connected screen if unset or auto
if [ "$DENSITY" = "auto" ] || [ -z "$DENSITY" ]; then
  DENSITY=
  for _s in /sys/class/drm/card*-*/status; do
    [ -f "$_s" ] || continue
    if grep -q '^connected$' "$_s" 2>/dev/null; then
      _d=$(dirname "$_s")
      if [ -f "$_d/modes" ] && [ -s "$_d/modes" ]; then
        _res=$(head -n1 "$_d/modes" 2>/dev/null | tr -d '\r\n')
        if [ -n "$_res" ]; then
          _w=$(echo "$_res" | cut -d'x' -f1)
          _h=$(echo "$_res" | cut -d'x' -f2)
          if [ "$_w" -le 1366 ]; then
            DENSITY=140
          elif [ "$_w" -le 1600 ]; then
            DENSITY=160
          elif [ "$_w" -le 1920 ]; then
            [ "$_h" -ge 1200 ] && DENSITY=180 || DENSITY=160
          elif [ "$_w" -le 2560 ]; then
            DENSITY=220
          elif [ "$_w" -le 2880 ]; then
            DENSITY=240
          else
            DENSITY=320
          fi
          log "hook: auto-detected resolution ${_res}, calculated density: $DENSITY"
          break
        fi
      fi
    fi
  done
fi
case "$DENSITY" in ''|*[!0-9]*) DENSITY=;; esac

# Auto-detect real laptop device model from DMI if set to auto
if [ "$DEVICE_MODEL" = "auto" ]; then
  _prod=$(cat /sys/class/dmi/id/product_name 2>/dev/null | tr -d '\000\r\n' | sed -e 's/[[:space:]]\+/ /g' -e 's/^ *//; s/ *$//')
  _vend=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null | tr -d '\000\r\n' | sed -e 's/[[:space:]]\+/ /g' -e 's/^ *//; s/ *$//')
  case "$_prod" in
    ''|'None'|'Default string'|'System Product Name'|'To be filled by O.E.M.'|'System Version') _prod="" ;;
  esac
  if [ -n "$_prod" ]; then
    case "$_prod" in
      *"$_vend"*) DEVICE_MODEL="$_prod" ;;
      *) DEVICE_MODEL="$_vend $_prod" ;;
    esac
    DEVICE_MODEL=$(echo "$DEVICE_MODEL" | sed 's/^ *//; s/ *$//')
    command -v log >/dev/null 2>&1 && log "hook: auto-detected device model: $DEVICE_MODEL"
  else
    DEVICE_MODEL=""
  fi
fi
DROP='^ro.boot.hypervisor.protected_vm.supported=|^ro.surface_flinger.has_wide_color_display=|^ro.surface_flinger.has_HDR_display='
[ "$GPU_SW" = 1 ] && DROP="$DROP|^ro.surface_flinger.supports_background_blur=|^persist.sys.sf.disable_blurs=|^ro.launcher.blur.appLaunch=|^log.tag.drmhwc=|^persist.log.tag.drmhwc=|^ro.vendor.hwc.use_overlay_planes=|^vendor.hwc.drm.use_overlay_planes=|^vendor.hwc.drm.scale_with_gpu=|^vendor.hwc.drm.enable_color_pipeline=|^ro.hwui.use_vulkan=|^ro.surface_flinger.default_composition_pixel_format=|^debug.renderengine.backend=|^debug.renderengine.graphite=|^debug.renderengine.graphite_desktop_optin=|^debug.renderengine.vulkan=|^ro.hwui.render_ahead=|^ro.surface_flinger.max_frame_buffer_acquired_buffers=|^debug.sf.enable_gl_backpressure="   # blur on the CPU is far too slow
[ -n "$DENSITY" ] && DROP="$DROP|^ro.sf.lcd_density="
[ -n "$DEVICE_MODEL" ] && DROP="$DROP|^ro.product.model=|^ro.product.vendor.model=|^ro.product.marketname=|^ro.product.vendor.marketname="
{ grep -v -E "$DROP" /new/vendor/build.prop
  echo "ro.surface_flinger.has_wide_color_display=false"
  echo "ro.surface_flinger.has_HDR_display=false"
  if [ "$GPU_SW" = 1 ]; then
    echo "ro.surface_flinger.supports_background_blur=0"
    echo "persist.sys.sf.disable_blurs=1"
    echo "ro.launcher.blur.appLaunch=0"
    echo "log.tag.drmhwc=WARN"
    echo "persist.log.tag.drmhwc=WARN"
    # Hardware cursor & overlay planes:
    # Intel KMS (i915/xe) has a dedicated hardware cursor plane (plane[44], format AR24).
    # DO NOT disable overlay/cursor planes on Intel: disabling them forces drm_hwcomposer to
    # composite the cursor in software via SwiftShader on CPU, pinning all cores at 100% and
    # causing 3-4s mouse lag and ANRs. Non-Intel software rendering (e.g. AMD without overlay caps)
    # stays on the primary plane.
    is_intel_drm=0
    if gbos_gpu_cards 2>/dev/null | grep -qE " (i915|xe)$"; then
      is_intel_drm=1
    fi
    if [ "$is_intel_drm" = 1 ]; then
      echo "ro.vendor.hwc.use_overlay_planes=true"
      echo "vendor.hwc.drm.use_overlay_planes=0"
    else
      echo "ro.vendor.hwc.use_overlay_planes=false"
      echo "vendor.hwc.drm.use_overlay_planes=0"
      echo "vendor.hwc.drm.scale_with_gpu=1"
    fi
    # Google's vendor build.prop has vendor.hwc.drm.enable_color_pipeline=true: drm_hwcomposer then sets the DRM COLOR_PIPELINE client
    # cap and rejects every plane without a COLOR_PIPELINE property ("Plane[N] does not support COLOR_PIPELINE. Rejecting", then
    # "Primary plane for CRTC not found", "No pipelines available. Creating null-display for headless mode": SurfaceFlinger draws into
    # nothing, black screen). amdgpu's planes (kernel 6.18) have none. Seen on a Ryzen 3 3350U, logcat boot-6.
    echo "vendor.hwc.drm.enable_color_pipeline=false"
    # Apps (HWUI) otherwise draw with Vulkan directly and abort at start-up on a driver with one queue per family:
    # "Assertion failed: queueProps[i].queueFamilyProperties.queueCount < kRequestedQueueCount" (SIGABRT in RenderThread of setupwizard,
    # systemui, launcher, 300+ times; SwiftShader reports queueCount=1, RADV too). OpenGL ES through ANGLE (-> the same Vulkan driver) is
    # what SurfaceFlinger already uses successfully here. Seen on the Ryzen 3 3350U, second logs.
    echo "ro.hwui.use_vulkan=false"
    # Default composition pixel format: PixelFormat::RGBX_8888 (2). Intel Gen <= 8 (Broadwell, Haswell, Ivybridge)
    # primary display planes do NOT support DRM_FORMAT_ABGR8888 (AB24 / HAL_PIXEL_FORMAT_RGBA_8888), which causes
    # drmModeAddFB2 to fail with -EINVAL (-22), producing "drmhwc: could not create drm fb -22" and "SurfaceFlinger:
    # present failed for display: BAD_DISPLAY (2)" on every frame. RGBX_8888 maps to DRM_FORMAT_XBGR8888 (XB24),
    # which is natively supported on all Intel display generations, AMD, and CPU rendering.
    echo "ro.surface_flinger.default_composition_pixel_format=2"
    # RenderEngine Skia backend:
    # Google defaults to Graphite over Vulkan on desktop (debug.renderengine.graphite_desktop_optin=true).
    # Graphite compiles Vulkan pipelines asynchronously at runtime, which saturates SwiftShader on CPU.
    # Fall back to Ganesh (SkiaGL) via ANGLE: much lighter and 3-5x faster on 2-core / 4-thread CPUs.
    echo "debug.renderengine.backend=skiaglthreaded"
    echo "debug.renderengine.graphite=false"
    echo "debug.renderengine.graphite_desktop_optin=false"
    echo "debug.renderengine.vulkan=false"
    # Reduce frame queue latency: render immediately without 2-frame buffering
    echo "ro.hwui.render_ahead=0"
    echo "ro.surface_flinger.max_frame_buffer_acquired_buffers=2"
    echo "debug.sf.enable_gl_backpressure=0"
    echo "debug.sf.rounded_corner_opaque_optimization=true"
  fi
  if [ -n "$DENSITY" ]; then echo "ro.sf.lcd_density=$DENSITY"; fi
  if [ -n "$DEVICE_MODEL" ]; then
    echo "ro.product.model=$DEVICE_MODEL"
    echo "ro.product.vendor.model=$DEVICE_MODEL"
    echo "ro.product.marketname=$DEVICE_MODEL"
    echo "ro.product.vendor.marketname=$DEVICE_MODEL"
  fi
} > /tmp/vendor.build.prop && mount --bind /tmp/vendor.build.prop /new/vendor/build.prop \
  && log "hook: vendor build.prop: protected_vm.supported hidden, sRGB-only display, lcd_density=${DENSITY:-stock}, software GPU props=${GPU_SW:-0}"
# Only one Trusty VM may own CID 200: drop the plain launcher's auto-start so the on-demand
# (accessor-registering) launcher runs it, falling back to non-protected by itself.
R=/new/system_ext/etc/init/desktop-trusty_security_vm_launcher.rc
sed 's/^    start trusty_security_vm_launcher$/    # gbos: plain launcher disabled/' $R > /tmp/vm_launcher.rc \
  && mount --bind /tmp/vm_launcher.rc $R \
  && log "hook: plain trusty launcher disabled ($(grep -c 'gbos: plain launcher disabled' $R) triggers)"
# Software KeyMint: Trusty's TEE KeyMint needs Google's security chip for its root of trust and
# hangs without it. Hide the Trusty KeyMint/RKP/SecureClock declarations so keystore2 uses
# km_compat, and use a libkm_compat patched to serve its software KeyMint for every security level.
# All library patches (this one, libz, libcrypto) are bound here from patched/MANIFEST: see gbos_bind_patches above.
gbos_bind_patches
[ -f $G/system_ext_manifest.xml ] && mount --bind $G/system_ext_manifest.xml /new/system_ext/etc/vintf/manifest.xml \
  && log "hook: Trusty KeyMint VINTF declarations hidden"
# StrongBox KeyMint (vendor APEX) needs the security chip too, and keystore2 aborts boot when a
# declared KeyMint won't accept module info. Overlay /vendor/apex without that one APEX.
mkdir -p /tmp/vapex && mount --bind /new/vendor/apex /tmp/vapex \
  && mount -t tmpfs -o mode=0755 tmpfs /new/vendor/apex && for f in /tmp/vapex/*; do
    n=${f##*/}; case $n in com.android.hardware.keymint.strongbox.desktop.apex) continue;; esac
    touch /new/vendor/apex/$n && mount --bind $f /new/vendor/apex/$n
  done && log "hook: vendor apex without strongbox: $(ls /new/vendor/apex | tr '\n' ' ')"
# Weaver is Trusty/GSC-backed and only starts with protected_vm.supported=1 (hidden above), but
# its VINTF declaration stays, so LockSettingsService waits on it forever and the Watchdog kills
# system_server ("System process not responding"). Undeclared, LSS goes without Weaver.
W=/new/vendor/etc/vintf/manifest/android.hardware.weaver-service.android-desktop.xml
[ -f $W ] && echo '<manifest version="9.0" type="device"/>' > /tmp/weaver.xml \
  && mount --bind /tmp/weaver.xml $W && log "hook: Weaver VINTF declaration hidden"
# Secretkeeper is Trusty-backed too (VSOCK:200) and never answers. virtualizationservice waits for it
# in allocateInstanceId, so the Linux Terminal's VM never starts. Undeclared, AVF goes without it.
SK=/new/vendor/etc/vintf/manifest/android.hardware.security.secretkeeper.trusty.xml
[ -f $SK ] && echo '<manifest version="9.0" type="device"/>' > /tmp/secretkeeper.xml \
  && mount --bind /tmp/secretkeeper.xml $SK && : > /tmp/empty.rc \
  && mount --bind /tmp/empty.rc /new/vendor/etc/init/android.hardware.security.secretkeeper.trusty.desktop.rc \
  && log "hook: Secretkeeper VINTF declaration hidden"
# Gatekeeper is Trusty-backed (VSOCK:200) and enroll never returns, so setting a lock-screen
# password hangs LockSettingsService (and every Settings screen waiting on it). Keep the VINTF
# declaration but don't start the Trusty service: gbos_soft_gk (gbos.rc above) registers
# IGatekeeper/default instead. Without soft_gk on the stick, hide the declaration so enroll fails fast.
GK=/new/vendor/etc/vintf/manifest/android.hardware.gatekeeper-service.trusty.xml
: > /tmp/empty.rc && mount --bind /tmp/empty.rc /new/vendor/etc/init/android.hardware.gatekeeper-service.trusty.rc
if [ -x $G/soft_gk ]; then
  log "hook: Trusty Gatekeeper replaced by software Gatekeeper (soft_gk)"
else
  echo '<manifest version="9.0" type="device"/>' > /tmp/gatekeeper.xml && mount --bind /tmp/gatekeeper.xml $GK \
    && log "hook: Trusty Gatekeeper hidden, no soft_gk (no lock-screen password)"
fi
# Keep vold off USB disks: its voldmanaged=usb rule picks up this stick, and its fsck_msdos
# "repairs" the FAT while stage0 has it mounted (LOST.DIR, lost/truncated logs, cross-linked
# clusters). The stick must only ever be mounted by stage0.
F=/new/vendor/etc/fstab.android-desktop
grep -v 'voldmanaged=usb' $F > /tmp/fstab.android-desktop \
  && mount --bind /tmp/fstab.android-desktop $F && log "hook: vold USB rule removed from fstab"
# Audio: the audio HAL (dras) needs a config whose <soundcard> equals the ALSA card id. Google ships only its SOF
# SoundWire card; gbos/audio/gbos-hda-pch is the legacy-HDA one (card id PCH, true for nearly every Intel PCH HDA
# laptop). For any other main card (SOF: sof-hda-dsp, sof-rt5682, ...) a copy is generated with that card's id and
# name and the PCM numbers / mic / jack devices read from /proc/asound and /proc/bus/input/devices (usb/audio/gen_config.sh,
# tools/test_audio_config.sh); the mixer controls are still a best-effort guess (they differ per codec).
# --- audio card selection (tested by tools/test_audio_config.sh) ---
# The main sound card = the first one with a codec that is not HDMI/DisplayPort audio. A laptop with a GPU registers the GPU's
# HDMI/DP audio as card 0 (AMD: "HD-Audio Generic" id Generic, codec "ATI R6xx HDMI"; NVIDIA: "HDA NVidia"), so the name is no
# guide. Cards without HDA codec files (SoundWire/ASoC) fall back to the first card whose name is not HDMI/GPU audio.
# ASOUND= is only for the test.
gbos_audio_card() {
  _as=${ASOUND:-/proc/asound}
  for _n in $(sed -n 's/^ *\([0-9][0-9]*\) \[.*/\1/p' $_as/cards 2>/dev/null); do
    if cat $_as/card$_n/codec#* 2>/dev/null | grep '^Codec:' | grep -q -v -i -E 'hdmi|displayport'; then
      grep "^ *$_n \[" $_as/cards; return 0
    fi
  done
  grep '\[' $_as/cards 2>/dev/null | grep -v -i -E 'hdmi|nvidia|HDA ATI|radeon' | head -n 1
}
# --- end audio card selection ---
A=/new/vendor/etc/audio
AC=gbos-hda-pch
if [ -f $G/audio/gbos-hda-pch/audio_config.xml ]; then
  CARD=$(gbos_audio_card)
  CID=$(echo "$CARD" | sed -n 's/^ *[0-9]* \[\([^] ]*\) *\].*/\1/p'); CNAME=$(echo "$CARD" | sed -n 's/.* - //p')
  # Free the first id by leaving the GPU HDMI audio unbound, then re-probe the internal card.
  case "$CID" in *[!A-Za-z0-9]*)
    CNO=$(echo "$CARD" | sed -n 's/^ *\([0-9][0-9]*\) \[.*/\1/p')
    BCNO=
    for _cn in $(sed -n 's/^ *\([0-9][0-9]*\) \[.*/\1/p' /proc/asound/cards); do
      [ "$_cn" = "$CNO" ] && continue
      if grep -h '^Codec:' /proc/asound/card$_cn/codec#* 2>/dev/null | grep -qiE 'HDMI|DisplayPort'; then BCNO=$_cn; break; fi
    done
    D=/sys/bus/pci/drivers/snd_hda_intel
    MDEV=$(readlink -f /sys/class/sound/card$CNO/device 2>/dev/null); MDEV=${MDEV##*/}
    BDEV=$(readlink -f /sys/class/sound/card$BCNO/device 2>/dev/null); BDEV=${BDEV##*/}
    if [ -n "$BCNO" ] && [ -d $D/$MDEV ] && [ -d $D/$BDEV ] \
       && echo "$BDEV" > $D/unbind && echo "$MDEV" > $D/unbind && echo "$MDEV" > $D/bind; then
      CARD=
      _try=0
      while [ -z "$CARD" ] && [ "$_try" -lt 20 ]; do
        CARD=$(gbos_audio_card)
        [ -n "$CARD" ] || sleep 0.1
        _try=$((_try + 1))
      done
      CID=$(echo "$CARD" | sed -n 's/^ *[0-9]* \[\([^] ]*\) *\].*/\1/p'); CNAME=$(echo "$CARD" | sed -n 's/.* - //p')
      if [ -n "$CID" ]; then
        # Stage0 has no hotplug mdev daemon, so rebuild /dev/snd from the post-reprobe sysfs devices.
        mkdir -p /dev/snd
        rm -f /dev/snd/*
        _snd_ok=1
        for _snd in /sys/class/sound/*; do
          [ -r "$_snd/dev" ] || continue
          _snd_name=${_snd##*/}
          case "$_snd_name" in card[0-9]*) continue;; esac
          _snd_dev=$(cat "$_snd/dev")
          _snd_major=${_snd_dev%:*}; _snd_minor=${_snd_dev#*:}
          case "$_snd_major:$_snd_minor" in *[!0-9:]*|:*) _snd_ok=0; continue;; esac
          mknod -m 0660 "/dev/snd/$_snd_name" c "$_snd_major" "$_snd_minor" || _snd_ok=0
        done
        [ "$_snd_ok" = 1 ] || log "hook: failed to recreate some /dev/snd nodes from sysfs"
        log "hook: HDMI audio left unbound; internal audio card is '$CARD'"
      else
        log "hook: internal audio card did not register after HDMI audio unbind and re-probe"
      fi
    else
      [ -n "$MDEV" ] && [ -d "$D/$MDEV" ] || { [ -n "$MDEV" ] && echo "$MDEV" > $D/bind; }
      log "hook: could not re-probe internal audio card; HDMI audio may be unavailable"
    fi;;
  esac
  mkdir -p /tmp/gbos-audio
  if [ -n "$CID" ] && [ "$CID" != PCH ]; then
    AC=gbos-gen
    # gen_config.sh also remaps PCM numbers, the internal mic (SOF: the DMIC PCM) and the jack input devices to what this card has
    if [ -f $G/audio/gen_config.sh ] && AMSG=$(sh $G/audio/gen_config.sh $G/audio/gbos-hda-pch $G/audio/audio_default.xml /tmp/gbos-audio $AC "$CID" "$CNAME" 2>&1); then
      log "hook: audio $AMSG"
      cat /tmp/gbos-audio/$AC/audio_config.xml > $L/audio_config.generated.xml 2>/dev/null
    else
      log "hook: audio gen_config.sh missing or failed (${AMSG:-no output}), plain card id/name substitution"
      rm -rf /tmp/gbos-audio/$AC; cp -r $G/audio/gbos-hda-pch /tmp/gbos-audio/$AC
      sed -i "s/HDA Intel PCH/$CNAME/g; s/PCH/$CID/g" /tmp/gbos-audio/$AC/*.xml
      sed "s/<soundcard>PCH/<soundcard>$CID/; s#gbos-hda-pch#$AC#" $G/audio/audio_default.xml > /tmp/gbos-audio/audio_default.xml
    fi
    # Machine-specific mixer routes: the HDA template's controls do not exist on SoundWire cards. Keyed by DMI as well as the card id,
    # because other SoundWire laptops also have the card id sofsoundwire.
    if [ "$CID" = sofsoundwire ] && [ -f $G/audio/mixer_paths_surface_laptop_7.xml ] \
       && grep -qi 'Surface Laptop.*7th Edition' /sys/class/dmi/id/product_name 2>/dev/null; then
      cp $G/audio/mixer_paths_surface_laptop_7.xml /tmp/gbos-audio/$AC/mixer_paths.xml \
        && log "hook: audio mixer_paths for the Surface Laptop 7 (RT1320/RT721) installed"
    fi
    # ASRC lives in /tmp of the initramfs, which switch_root deletes: a bind mount of it is empty in Android (dras: "Failed to read
    # /vendor/etc/audio/gbos-gen/audio_config.xml"), so the files are copied into the tmpfs over /vendor/etc/audio below.
    ASRC=/tmp/gbos-audio/$AC; ADEF=/tmp/gbos-audio/audio_default.xml
  else
    ASRC=$G/audio/gbos-hda-pch; ADEF=$G/audio/audio_default.xml
  fi
  mkdir -p /tmp/vaudio && mount --bind $A /tmp/vaudio && mount -t tmpfs -o mode=0755 tmpfs $A \
    && for f in /tmp/vaudio/*; do n=${f##*/}; { [ -d $f ] && mkdir -p $A/$n || touch $A/$n; } && mount --bind $f $A/$n; done \
    && mkdir -p $A/$AC && cp -r $ASRC/. $A/$AC/ \
    && mount --bind $ADEF /new/vendor/etc/data/hal_configs/audio_default.xml \
    && log "hook: audio config $AC (card id ${CID:-none}, name ${CNAME:-none}) active"
fi
# apgetty (UART shell) has no UART configured here and crash-loops
: > /tmp/empty.rc && mount --bind /tmp/empty.rc /new/system_ext/etc/init/apgetty.rc && log "hook: apgetty disabled"
mount --bind /tmp/empty.rc /new/vendor/etc/init/hal_desktop_charge_control_default.rc 2>/dev/null && log "hook: charge_control disabled"
mount --bind /tmp/empty.rc /new/vendor/etc/init/suspend_notification_service.rc 2>/dev/null && log "hook: suspend_notification disabled"
mount --bind /tmp/empty.rc /new/vendor/etc/init/suspend_diagnostics.rc 2>/dev/null && log "hook: suspend_diagnostics disabled"
log "hook: kvm: $(ls /sys/class/misc | grep -E 'kvm|vhost' | tr '\n' ' ')"

# KVM trace -> kvmtrace.txt: guest exceptions / failed emulation / triple faults, to see why the
# Trusty VM goes silent. Reader opens trace_pipe now so it survives switch_root.
T=/tmp/tracing; mkdir -p $T
if mount -t tracefs tracefs $T; then
  E=$T/events/kvm
  echo 8192 > $T/buffer_size_kb
  for ev in kvm_inj_exception kvm_emulate_insn kvm_userspace_exit kvm_exit kvm_pio kvm_mmio kvm_hypercall kvm_fast_mmio kvm_inj_virq kvm_pic_set_irq kvm_ioapic_set_irq kvm_msi_set_irq kvm_set_irq kvm_apic_accept_irq; do
    [ -d $E/$ev ] && echo 1 > $E/$ev/enable
  done
  ( exec 4<$T/trace_pipe; /gbos/busybox dd bs=4096 count=8000 iflag=fullblock <&4 >> $L/kvmtrace.txt 2>/dev/null ) &
  log "hook: kvm tracing (all exits) -> kvmtrace.txt"
else
  log "hook: tracefs mount FAILED"
fi
