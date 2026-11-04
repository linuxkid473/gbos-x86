#!/bin/sh
# Shell unit test of the software-GPU decision and bind logic in usb/hook.sh (fake /sys and fake /new trees, no root, no mounts).
#   sh tools/test_gpu_select.sh
# Pulls the gbos_gpu_* block out of hook.sh between its marker comments, so what is tested is what boots.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
HOOK=$HERE/../usb/hook.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/gpusel.XXXXXX"); trap 'rm -rf "$T"' EXIT
sed -n '/^# --- software rendering for GPUs/,/^# --- end software rendering ---/p' "$HOOK" > "$T/block.sh"
[ -s "$T/block.sh" ] || { echo "FAIL: marker block not found in hook.sh"; exit 1; }
fails=0; n=0
log() { echo "$*" >> "$T/log"; }
sleep() { :; }
mount() { echo "$*" >> "$T/mounts"; return ${MOUNT_RC:-0}; }
sha256sum() { case "$1" in */libminigbm_gralloc.so) echo "${FAKE_GRALLOC_SHA:-d48536951fd0e9643e71a5c8843e90833ce3e212e182782185470e2fbbfad673}  $1";; */libgbm.so) echo "${FAKE_LIBGBM_SHA:-64a0f2537a5917822cb3f538dc74229e7ccf4ca45a30265bb97f742678ba294e}  $1";; */vulkan.intel.so) echo "${FAKE_VK_SHA:-5c687450523ea9ed9e8d4d9cc98ef4fef6c8d592d46db4bf425ad571dc6f7c73}  $1";; *) command sha256sum "$@" 2>/dev/null || shasum -a 256 "$@";; esac; }
. "$T/block.sh"

# fake sysfs: $1 = list of "cardN:driver" ("none" = unbound), $2 = list of "vendor:class" PCI devices
mk() {
  rm -rf "$T/sys" "$T/log" "$T/mounts"; mkdir -p "$T/sys/class/drm" "$T/sys/bus/pci/devices" "$T/drivers"
  for mc in $1; do
    mn=${mc%%:*}; md=${mc#*:}; mkdir -p "$T/sys/class/drm/$mn/device" "$T/sys/class/drm/$mn-eDP-1"
    [ "$md" = none ] || { mkdir -p "$T/drivers/$md"; ln -s "$T/drivers/$md" "$T/sys/class/drm/$mn/device/driver"; }
  done
  mi=0
  for mp in $2; do
    mi=$((mi+1)); pdir="$T/sys/bus/pci/devices/0000:00:0$mi.0"; mkdir -p "$pdir"
    echo "$mp" | cut -d: -f1 > "$pdir/vendor"
    echo "$mp" | cut -d: -f2 > "$pdir/class"
    mdev=$(echo "$mp" | cut -d: -f3)
    [ -n "$mdev" ] && echo "$mdev" > "$pdir/device"
  done
}
check() { # name expected(GPU_SW) cards pci [GPU_SOFTWARE]
  n=$((n+1)); mk "$3" "$4"; SYSFS=$T/sys; GPU_SOFTWARE=${5:-auto}; GPU_SW=x
  gbos_gpu_decide 3
  if [ "$GPU_SW" = "$2" ]; then echo "ok   $1 -> $GPU_SW ($(tail -n1 $T/log | cut -c1-110))"; else echo "FAIL $1: wanted $2, got $GPU_SW"; cat "$T/log"; fails=$((fails+1)); fi
}
AMD=0x1002:0x030000; INTEL=0x8086:0x030000; NV=0x10de:0x030200
INTEL_BDW=0x8086:0x030000:0x1616; INTEL_HSW=0x8086:0x030000:0x0416
check "Intel i915 laptop"              0 "card0:i915"                "$INTEL"
check "Intel xe"                       0 "card0:xe"                  "$INTEL"
check "Intel Broadwell Gen 8 (legacy)" 1 "card0:i915"                "$INTEL_BDW"
check "Intel Haswell Gen 7.5 (legacy)" 1 "card0:i915"                "$INTEL_HSW"
check "Intel, i915 not bound yet"      0 "card0:simpledrm"           "$INTEL"
check "Intel Broadwell unbound"        1 "card0:simpledrm"           "$INTEL_BDW"
check "Intel + AMD dGPU (hybrid)"      0 "card0:i915 card1:amdgpu"   "$INTEL $AMD"
check "AMD amdgpu (reporter's laptop)" 1 "card0:amdgpu"              "$AMD"
check "AMD, simpledrm replaced"        1 "card1:amdgpu card0:simpledrm" "$AMD"
check "AMD but amdgpu did not bind"    0 "card0:simpledrm"           "$AMD"
check "AMD, no card at all"            0 ""                          "$AMD"
check "nouveau only"                   1 "card0:nouveau"             "$NV"
check "forced on Intel"                1 "card0:i915"                "$INTEL" 1
check "disabled on AMD"                0 "card0:amdgpu"              "$AMD" 0
check "vkms only"                      0 "card0:vkms"                ""

# enable: binds only over the exact originals, skips quietly when the stick has no files
# runenable NAME GRALLOC LIBGBM VULKAN SW_AFTER: expected 1 = bind attempted
runenable() {
  n=$((n+1)); rm -f "$T/log" "$T/mounts"; GPU_SW=1; gbos_gpu_enable
  m=$(cat "$T/mounts" 2>/dev/null)
  g=0; b=0; v=0; case "$m" in *libminigbm_gralloc.so*) g=1;; esac; case "$m" in *lib64/libgbm.so*) b=1;; esac; case "$m" in *vulkan.intel.so*) v=1;; esac
  if [ $g = $2 ] && [ $b = $3 ] && [ $v = $4 ] && [ "$GPU_SW" = $5 ]; then echo "ok   enable: $1 (gralloc=$g gbm=$b vk=$v sw=$GPU_SW)"
  else echo "FAIL enable: $1: gralloc=$g gbm=$b vk=$v sw=$GPU_SW"; cat "$T/log"; fails=$((fails+1)); fi
}
G=$T/gbos; NEWROOT=$T/new; mkdir -p $G/patched $NEWROOT/vendor/lib64/hw
: > $NEWROOT/vendor/lib64/libgbm.so; : > $NEWROOT/vendor/lib64/libminigbm_gralloc.so; : > $NEWROOT/vendor/lib64/hw/vulkan.intel.so
runenable "stick without the files (old stick)" 0 0 0 0
: > $G/patched/libgbm.amdgpu.so; : > $G/patched/vulkan.swiftshader.so; : > $G/patched/libminigbm_gralloc.amdgpu.so
runenable "all files, originals match" 1 1 1 1
FAKE_VK_SHA=deadbeef; runenable "unknown vulkan.intel.so (new Google build)" 1 1 0 0; unset FAKE_VK_SHA
FAKE_GRALLOC_SHA=deadbeef; runenable "unknown libminigbm_gralloc.so" 0 1 1 0; unset FAKE_GRALLOC_SHA
FAKE_LIBGBM_SHA=deadbeef; runenable "unknown libgbm.so (not essential)" 1 0 1 1; unset FAKE_LIBGBM_SHA
rm $G/patched/vulkan.swiftshader.so
runenable "no SwiftShader HAL on the stick" 1 1 0 0
: > $G/patched/vulkan.swiftshader.so; MOUNT_RC=1; runenable "bind fails" 1 1 1 0; unset MOUNT_RC

# RADV: used when it is on the stick and amdgpu is bound; GPU_VULKAN forces either; falls back to SwiftShader when RADV cannot be bound
radvmount() { grep -q "vulkan.radv.so" "$T/mounts" 2>/dev/null && echo radv || { grep -q "vulkan.swiftshader.so" "$T/mounts" 2>/dev/null && echo swiftshader || echo none; }; }
rv() { # name expected GPU_VULKAN
  n=$((n+1)); runenable_quiet; r=$(radvmount)
  if [ "$r" = "$2" ]; then echo "ok   vulkan: $1 -> $r"; else echo "FAIL vulkan: $1: wanted $2, got $r"; cat "$T/log"; fails=$((fails+1)); fi
}
runenable_quiet() { rm -f "$T/log" "$T/mounts"; GPU_SW=1; gbos_gpu_enable; }
mk "card0:amdgpu" "$AMD"; SYSFS=$T/sys; : > $G/patched/vulkan.swiftshader.so
rv "amdgpu, no RADV on the stick" swiftshader
: > $G/patched/vulkan.radv.so
rv "amdgpu + RADV on the stick" radv
GPU_VULKAN=swiftshader; rv "forced SwiftShader" swiftshader
GPU_VULKAN=radv; rv "forced RADV" radv; unset GPU_VULKAN
mk "card0:nouveau" "$NV"; SYSFS=$T/sys; rv "other GPU driver, RADV present" swiftshader
mk "card0:amdgpu" "$AMD"; SYSFS=$T/sys; FAKE_VK_SHA=deadbeef; rv "unknown vulkan.intel.so" none; unset FAKE_VK_SHA
# vendor build.prop rewrite: in software-GPU mode Google's enable_color_pipeline=true must become false (drm_hwcomposer rejects every amdgpu plane otherwise)
sed -n '/^DROP=/,/^} > \/tmp\/vendor.build.prop/p' "$HOOK" | sed "s#/new/vendor/build.prop#$T/vendor.build.prop#g; s#/tmp/vendor.build.prop#$T/out.prop#g; s#^} > .*#}#" > "$T/prop.sh"
bp() { # name GPU_SW expected(enable_color_pipeline in the result)
  n=$((n+1)); printf 'vendor.hwc.drm.enable_color_pipeline=true\nvendor.hwc.use_log_gamma_lut=true\nro.sf.lcd_density=260\n' > "$T/vendor.build.prop"; rm -f "$T/out.prop"
  ( GPU_SW=$2; DENSITY=; DEVICE_MODEL=; . "$T/prop.sh" > "$T/out.prop" ) 2>/dev/null
  r=$(grep '^vendor.hwc.drm.enable_color_pipeline=' "$T/out.prop" | tr '\n' ' ')
  if [ "$r" = "vendor.hwc.drm.enable_color_pipeline=$3 " ]; then echo "ok   build.prop: $1 -> color_pipeline=$3"; else echo "FAIL build.prop: $1: got '$r'"; cat "$T/out.prop"; fails=$((fails+1)); fi
}
bp "software GPU mode (AMD)" 1 false
n=$((n+1)); printf 'ro.hwui.use_vulkan=true\nvendor.hwc.drm.enable_color_pipeline=true\n' > "$T/vendor.build.prop"
( GPU_SW=1; DENSITY=; DEVICE_MODEL=; . "$T/prop.sh" > "$T/out.prop" ) 2>/dev/null
if [ "$(grep -c '^ro.hwui.use_vulkan=' "$T/out.prop")" = 1 ] && grep -q '^ro.hwui.use_vulkan=false$' "$T/out.prop"; then echo "ok   build.prop: software GPU mode -> ro.hwui.use_vulkan=false (once)"; else echo "FAIL build.prop hwui"; cat "$T/out.prop"; fails=$((fails+1)); fi
n=$((n+1)); printf 'ro.hwui.use_vulkan=true\n' > "$T/vendor.build.prop"; ( GPU_SW=0; DENSITY=; DEVICE_MODEL=; . "$T/prop.sh" > "$T/out.prop" ) 2>/dev/null
if grep -q '^ro.hwui.use_vulkan=true$' "$T/out.prop"; then echo "ok   build.prop: Intel keeps ro.hwui.use_vulkan=true"; else echo "FAIL build.prop hwui Intel"; fails=$((fails+1)); fi
n=$((n+1)); ( GPU_SW=1; DENSITY=; DEVICE_MODEL=; . "$T/prop.sh" > "$T/out.prop" ) 2>/dev/null
if grep -q '^ro.surface_flinger.default_composition_pixel_format=2$' "$T/out.prop" && grep -q '^vendor.hwc.drm.use_overlay_planes=0$' "$T/out.prop"; then
  echo "ok   build.prop: software GPU mode -> default_composition_pixel_format=2 and use_overlay_planes=0"
else
  echo "FAIL build.prop pixel_format / overlay_planes"; cat "$T/out.prop"; fails=$((fails+1))
fi
n=$((n+1)); ( GPU_SW=0; DENSITY=; DEVICE_MODEL=; . "$T/prop.sh" > "$T/out.prop" ) 2>/dev/null
if ! grep -q '^ro.surface_flinger.default_composition_pixel_format=' "$T/out.prop"; then
  echo "ok   build.prop: Intel stock -> no forced default_composition_pixel_format"
else
  echo "FAIL build.prop Intel has unexpected default_composition_pixel_format"; cat "$T/out.prop"; fails=$((fails+1))
fi
bp "Intel (stock, unchanged)" 0 true
[ $fails = 0 ] && echo "GPU_SELECT_TEST_PASS ($n checks)" || { echo "$fails FAILED"; exit 1; }
