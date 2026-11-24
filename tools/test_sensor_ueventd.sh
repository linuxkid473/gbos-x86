#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

awk '
  /^gbos_sensor_ueventd\(\) \{/ { copy=1 }
  copy { print }
  copy && /^}/ { exit }
' "$ROOT/usb/hook.sh" > "$T/function.sh"
[ -s "$T/function.sh" ] || { echo "FAIL: sensor ueventd function missing"; exit 1; }
. "$T/function.sh"

log() { :; }
mount() {
  [ "$1" = -t ] && [ "$2" = tmpfs ] || return 1
  [ "${FAIL_MOUNT:-0}" = 0 ]
}
umount() {
  printf '%s\n' "$1" > "$T/umount-called"
}
cp() {
  if [ "${FAIL_POPULATE:-0}" = 1 ] && [ "$2" = "$T/stage/." ]; then return 1; fi
  command cp "$@"
}

mkdir -p "$T/root/vendor/etc/ueventd"
printf 'import /vendor/etc/ueventd\n' > "$T/root/vendor/etc/ueventd.rc"
printf 'existing vendor rule\n' > "$T/root/vendor/etc/ueventd/ueventd.existing.rc"
NEWROOT=$T/root UEVENTD_TMP=$T/stage
export NEWROOT UEVENTD_TMP

gbos_sensor_ueventd
grep -Fqx 'import /vendor/etc/ueventd' "$T/root/vendor/etc/ueventd.rc"
grep -Fqx 'existing vendor rule' "$T/root/vendor/etc/ueventd/ueventd.existing.rc"
grep -Fqx '/sys/bus/iio/devices/iio:device* in_accel_sampling_frequency 0660 system system' \
  "$T/root/vendor/etc/ueventd/ueventd.gbos-sensors.rc"
grep -Fqx '/sys/bus/iio/devices/iio:device* in_anglvel_sampling_frequency 0660 system system' \
  "$T/root/vendor/etc/ueventd/ueventd.gbos-sensors.rc"

gbos_sensor_ueventd
[ "$(grep -Fc 'in_accel_sampling_frequency 0660 system system' "$T/root/vendor/etc/ueventd/ueventd.gbos-sensors.rc")" = 1 ]
[ "$(grep -Fc 'in_anglvel_sampling_frequency 0660 system system' "$T/root/vendor/etc/ueventd/ueventd.gbos-sensors.rc")" = 1 ]
if FAIL_MOUNT=1 gbos_sensor_ueventd; then
  echo "FAIL: accepted mount failure" >&2
  exit 1
fi
if FAIL_POPULATE=1 gbos_sensor_ueventd; then
  echo "FAIL: accepted overlay copy failure" >&2
  exit 1
fi
grep -Fqx "$T/root/vendor/etc/ueventd" "$T/umount-called"
printf '# no directory import\n' > "$T/root/vendor/etc/ueventd.rc"
if gbos_sensor_ueventd; then
  echo "FAIL: accepted missing ueventd directory import" >&2
  exit 1
fi

echo "SENSOR_UEVENTD_TEST_PASS"
