#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

awk '
  /^gbos_enable_latitude7320_sensors\(\) \{/ { copy=1 }
  copy { print }
  copy && /^}/ { exit }
' "$ROOT/usb/hook.sh" > "$T/function.sh"
[ -s "$T/function.sh" ] || { echo "FAIL: sensor HAL function missing"; exit 1; }
. "$T/function.sh"
grep -Fq 'sensor_vendor_overlay.sh' "$ROOT/tools/make_gbos86_image.sh"
grep -Fq 'sensor_vendor_overlay.sh' "$ROOT/usb/install.sh"
grep -Fq '. "$G/sensor_vendor_overlay.sh"' "$ROOT/usb/early_patch.sh"
grep -Fq 'sensor_trial_diag.sh' "$ROOT/tools/make_gbos86_image.sh"
grep -Fq 'sensor_trial_diag.sh' "$ROOT/usb/install.sh"
grep -Fq 'tablet_mode.sh' "$ROOT/tools/make_gbos86_image.sh"
grep -Fq 'tablet_mode.sh' "$ROOT/usb/install.sh"
# The rotation RRO is committed signed under usb/display/ (shipped by deploy_stick/make_gbos86_image/install.sh with
# display/); the unsigned aapt2 build passed in GBOS_ROTATION_POLICY_OVERLAY was never registered by Android.
if grep -q 'GBOS_ROTATION_POLICY_OVERLAY' "$ROOT/tools/make_gbos86_image.sh" "$ROOT/tools/deploy_stick.sh"; then
  echo "FAIL: GBOS_ROTATION_POLICY_OVERLAY (unsigned overlay path) is back"; exit 1
fi
for f in "$ROOT/tools/make_gbos86_image.sh" "$ROOT/tools/deploy_stick.sh"; do
  grep -Eq '^for d in .* display; do' "$f" || { echo "FAIL: $f does not ship display/"; exit 1; }
done
grep -Fq 'for d in sensors camera thermal display;' "$ROOT/usb/install.sh"
grep -Fq '_sensor_policy_source=$G/display/$_sensor_policy_name' "$ROOT/usb/hook.sh"
sh "$ROOT/tools/build_rotation_policy_overlay.sh" --check > "$T/rro-check" ||
  { cat "$T/rro-check"; echo "FAIL: committed rotation overlay is unsigned or differs from its sources"; exit 1; }
grep -Fq 'fixed-to-user-rotation disabled' "$ROOT/usb/tablet_mode.sh"
grep -Fq '<bool name="config_supportAutoRotation">true</bool>' \
  "$ROOT/sensors/trogdor/display-policy/res/values/config.xml"
grep -Fq 'android:priority="100"' "$ROOT/sensors/trogdor/display-policy/AndroidManifest.xml"

log() { printf '%s\n' "$*" >> "$T/log"; }
mount() {
  printf 'mount %s\n' "$*" >> "$T/mounts"
  case "$1 $2" in
    "-t tmpfs"|-t\ overlay) ;;
    "--bind "* ) ;;
    *) return 1 ;;
  esac
  case "${FAIL_MOUNT:-}" in
    tmpfs) [ "$1 $2" != "-t tmpfs" ] || return 1 ;;
    overlay) [ "$2" != overlay ] || { echo "simulated overlay failure" >&2; return 1; } ;;
    bind) [ "$1" != --bind ] || return 1 ;;
  esac
}
umount() { printf 'umount %s\n' "$*" >> "$T/mounts"; }

setup() {
  rm -rf "$T/root" "$T/sys" "$T/gbos" "$T/sensor-overlay" "$T/log" "$T/mounts"
  mkdir -p "$T/root/vendor/lib64" "$T/root/vendor/etc/sensors" \
    "$T/sys/class/dmi/id" "$T/gbos/sensors" "$T/gbos/display"
  printf '%s' '/vendor/lib64/android.hardware.sensors@2.1-Desktop-IIO-Subhal.so' \
    > "$T/root/vendor/etc/sensors/hals.conf"
  cp "$T/root/vendor/etc/sensors/hals.conf" "$T/expected-hals.conf"
  printf 'library payload\n' > \
    "$T/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so"
  printf 'rotation policy payload\n' > \
    "$T/gbos/display/gbos-latitude7320-rotation-policy.apk"
  cp "$ROOT/usb/sensor_vendor_overlay.sh" "$T/gbos/sensor_vendor_overlay.sh"
  printf 'Dell Inc.\n' > "$T/sys/class/dmi/id/sys_vendor"
  printf 'Latitude 7320 Detachable\n' > "$T/sys/class/dmi/id/product_name"
  printf '0TNW0H\n' > "$T/sys/class/dmi/id/board_name"
  NEWROOT=$T/root
  SYSFS=$T/sys
  G=$T/gbos
  SENSOR_HAL=1
  SENSOR_HAL_TMP=$T/sensor-overlay
  export NEWROOT SYSFS G SENSOR_HAL SENSOR_HAL_TMP
  unset FAIL_MOUNT GBOS_SENSOR_OVERLAY_FAILED
}

setup
. "$T/gbos/sensor_vendor_overlay.sh"
gbos_prepare_sensor_vendor_overlay
gbos_prepare_sensor_vendor_overlay
gbos_enable_latitude7320_sensors
[ "$GBOS_SENSOR_HAL_SELECTED" = 1 ] && [ "$GBOS_SENSOR_ROTATION_POLICY_SELECTED" = 1 ]
grep -Fqx '/vendor/lib64/android.hardware.sensors@2.1-Desktop-IIO-Subhal.so' \
  "$T/root/vendor/etc/sensors/hals.conf"
grep -Fqx '/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so' \
  "$T/root/vendor/etc/sensors/hals.conf"
cmp "$T/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so" \
  "$T/root/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so"
cmp "$T/gbos/display/gbos-latitude7320-rotation-policy.apk" \
  "$T/root/vendor/overlay/gbos-latitude7320-rotation-policy.apk"
gbos_enable_latitude7320_sensors
[ "$GBOS_SENSOR_HAL_SELECTED" = 1 ] && [ "$GBOS_SENSOR_ROTATION_POLICY_SELECTED" = 1 ]
[ "$(grep -Fc '/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so' \
  "$T/root/vendor/etc/sensors/hals.conf")" = 1 ]
[ "$(grep -c '^mount -t overlay ' "$T/mounts")" = 1 ]

setup
printf 'Other Product\n' > "$T/sys/class/dmi/id/product_name"
gbos_enable_latitude7320_sensors
[ "$GBOS_SENSOR_HAL_SELECTED" = 0 ]
[ "$GBOS_SENSOR_ROTATION_POLICY_SELECTED" = 0 ]
[ ! -e "$T/root/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so" ]
[ ! -e "$T/root/vendor/overlay/gbos-latitude7320-rotation-policy.apk" ]
! grep -Fq 'trogdor-subhal.so' "$T/root/vendor/etc/sensors/hals.conf"

setup
SENSOR_HAL=0
export SENSOR_HAL
. "$T/gbos/sensor_vendor_overlay.sh"
gbos_prepare_sensor_vendor_overlay
gbos_enable_latitude7320_sensors
[ "$GBOS_SENSOR_HAL_SELECTED" = 0 ]
[ "$GBOS_SENSOR_ROTATION_POLICY_SELECTED" = 0 ]
[ ! -e "$T/mounts" ]
[ ! -e "$T/root/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so" ]
! grep -Fq 'trogdor-subhal.so' "$T/root/vendor/etc/sensors/hals.conf"

setup
rm "$T/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so"
. "$T/gbos/sensor_vendor_overlay.sh"
gbos_prepare_sensor_vendor_overlay
gbos_enable_latitude7320_sensors
[ "$GBOS_SENSOR_HAL_SELECTED" = 0 ]
[ "$GBOS_SENSOR_ROTATION_POLICY_SELECTED" = 0 ]
[ ! -e "$T/mounts" ]
cmp "$T/expected-hals.conf" "$T/root/vendor/etc/sensors/hals.conf"

setup
mv "$T/gbos/display/gbos-latitude7320-rotation-policy.apk" "$T/gbos/sensors/"   # only the old unsigned stick copy
gbos_enable_latitude7320_sensors
[ "$GBOS_SENSOR_HAL_SELECTED" = 0 ]
[ "$GBOS_SENSOR_ROTATION_POLICY_SELECTED" = 0 ]
[ ! -e "$T/mounts" ]
cmp "$T/expected-hals.conf" "$T/root/vendor/etc/sensors/hals.conf"
grep -Fq "missing display policy overlay $T/gbos/display/gbos-latitude7320-rotation-policy.apk" "$T/log"
[ ! -e "$T/root/vendor/overlay/gbos-latitude7320-rotation-policy.apk" ]

setup
FAIL_MOUNT=overlay
export FAIL_MOUNT
if gbos_enable_latitude7320_sensors; then
  echo "FAIL: accepted unavailable vendor overlay" >&2
  exit 1
fi
[ -f "$T/expected-hals.conf" ]
cmp "$T/expected-hals.conf" "$T/root/vendor/etc/sensors/hals.conf"
[ ! -e "$T/root/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so" ]
grep -Fq 'sensor overlay command failed (rc=1): mount -t overlay' "$T/log"
grep -Fq 'simulated overlay failure' "$T/log"
grep -Fq "umount $T/sensor-overlay" "$T/mounts"
unset FAIL_MOUNT
if gbos_enable_latitude7320_sensors; then
  echo "FAIL: retried failed overlay during the same boot" >&2
  exit 1
fi

setup
FAIL_MOUNT=bind
export FAIL_MOUNT
if gbos_enable_latitude7320_sensors; then
  echo "FAIL: accepted failed vendor bind" >&2
  exit 1
fi
grep -Fq "umount $T/sensor-overlay/merged" "$T/mounts"
grep -Fq "umount $T/sensor-overlay" "$T/mounts"
cmp "$T/expected-hals.conf" "$T/root/vendor/etc/sensors/hals.conf"

echo "SENSOR_HAL_ENABLE_TEST_PASS"
