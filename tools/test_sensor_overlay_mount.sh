#!/bin/sh
# Run inside an isolated privileged container; this test moves its /dev mount.
set -eu
[ "${GBOS_SENSOR_MOUNT_TEST_CONTAINER:-0}" = 1 ] || {
  echo "Run only in the dedicated privileged test container" >&2
  exit 1
}
ROOT=${1:-/src}
T=$(mktemp -d)
trap 'umount "$T/new/vendor" 2>/dev/null || :; umount "$T/new/vendor" 2>/dev/null || :; umount /dev/.gbos-sensor-hal/merged 2>/dev/null || :; umount /dev/.gbos-sensor-hal 2>/dev/null || :; umount "$T/new" 2>/dev/null || :; rm -rf "$T"' EXIT
mkdir -p "$T/new/vendor/lib64" "$T/new/vendor/etc/sensors" "$T/new/vendor/overlay" "$T/new/dev" \
  "$T/sys/class/dmi/id" "$T/gbos/sensors" "$T/gbos/display"
printf 'Dell Inc.\n' > "$T/sys/class/dmi/id/sys_vendor"
printf 'Latitude 7320 Detachable\n' > "$T/sys/class/dmi/id/product_name"
printf '0TNW0H\n' > "$T/sys/class/dmi/id/board_name"
printf '/vendor/lib64/android.hardware.sensors@2.1-Desktop-IIO-Subhal.so\n' > "$T/new/vendor/etc/sensors/hals.conf"
printf 'test library\n' > "$T/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so"
printf 'test display policy overlay\n' > "$T/gbos/display/gbos-latitude7320-rotation-policy.apk"
cp "$ROOT/usb/sensor_vendor_overlay.sh" "$T/gbos/sensor_vendor_overlay.sh"
awk '/^gbos_enable_latitude7320_sensors\(\) \{/ { copy=1 } copy { print } copy && /^}/ { exit }' \
  "$ROOT/usb/hook.sh" > "$T/function.sh"
mount --bind "$T/new" "$T/new"
mount --bind "$T/new/vendor" "$T/new/vendor"
mount -o remount,bind,ro "$T/new/vendor"
mount -o remount,bind,ro "$T/new"
if mkdir "$T/new/.gbos-sensor-hal" 2>/dev/null; then
  echo "FAIL: system root is not read-only" >&2
  exit 1
fi

NEWROOT=$T/new SYSFS=$T/sys G=$T/gbos SENSOR_HAL=1
export NEWROOT SYSFS G SENSOR_HAL
unset SENSOR_HAL_TMP
log() { printf '%s\n' "$*" >> "$T/log"; }
. "$T/gbos/sensor_vendor_overlay.sh"
. "$T/function.sh"

# stage0 first binds /dev into the Android root for dmctl, then unbinds it.
mount --bind /dev "$T/new/dev"
gbos_prepare_sensor_vendor_overlay
gbos_prepare_sensor_vendor_overlay
umount "$T/new/dev"
gbos_enable_latitude7320_sensors
gbos_enable_latitude7320_sensors
cmp "$T/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so" \
  "$T/new/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so"
cmp "$T/gbos/display/gbos-latitude7320-rotation-policy.apk" \
  "$T/new/vendor/overlay/gbos-latitude7320-rotation-policy.apk"
[ "$(grep -c 'trogdor-subhal.so' "$T/new/vendor/etc/sensors/hals.conf")" = 1 ]

# stage0 then moves /dev under /new immediately before switch_root.
mount --move /dev "$T/new/dev"
[ -f "$T/new/dev/.gbos-sensor-hal/ready" ]
[ -s "$T/new/vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so" ]
mount --move "$T/new/dev" /dev
echo "SENSOR_OVERLAY_MOUNT_TEST_PASS"
