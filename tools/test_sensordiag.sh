#!/bin/sh
# Smoke-test IIO event-path diagnostics against a small synthetic sysfs tree.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/sensordiag_test.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/tmp"
export TMPDIR="$W/tmp"
mkdir -p "$W/bin"
cat > "$W/bin/getevent" <<'EOF'
#!/bin/sh
[ -z "${GETEVENT_PID_FILE:-}" ] || echo "$$" > "$GETEVENT_PID_FILE"
[ "${FAIL_INPUT:-0}" = 0 ] || { echo 'getevent failed'; exit 2; }
echo '[ 122.000000] /dev/input/event1: EV_KEY KEY_A DOWN'
echo '[ 123.000000] /dev/input/event1: EV_KEY KEY_VOLUMEUP DOWN'
echo '[ 124.000000] /dev/input/event1: EV_KEY KEY_SPACE UP'
echo '[ 125.000000] /dev/input/event1: EV_ABS ABS_X 00000001'
exec sleep 5
EOF
chmod +x "$W/bin/getevent"
PATH="$W/bin:$PATH"
export PATH

D="$W/iio/iio:device2"
mkdir -p "$D/scan_elements" "$D/buffer" "$D/trigger" "$W/dev"
mkdir -p "$W/hid/001F:8087:0AC2.0008" "$W/hid/001F:8087:0AC3.000B" \
  "$W/hid/001F:8087:0AC3.000C"
printf '\005\040\011\163' > "$W/hid/001F:8087:0AC2.0008/report_descriptor"
dd if=/dev/zero of="$W/hid/001F:8087:0AC3.000B/report_descriptor" bs=4097 count=1 2>/dev/null
echo accel_3d > "$D/name"
echo 136 > "$D/in_accel_x_raw"
echo 0.000009806 > "$D/in_accel_scale"
echo 10 > "$D/in_accel_sampling_frequency"
echo '2 5 10 20' > "$D/in_accel_sampling_frequency_available"
echo 0 > "$D/buffer/enable"
echo 16 > "$D/buffer/length"
echo 0.04 > "$D/buffer/hwfifo_timeout"
echo 0 > "$D/buffer/hwfifo_enabled"
echo monotonic > "$D/current_timestamp_clock"
echo 1 > "$D/scan_elements/in_accel_x_en"
echo 0 > "$D/scan_elements/in_accel_x_index"
echo 'le:s16/16>>0' > "$D/scan_elements/in_accel_x_type"
echo test-trigger > "$D/trigger/current_trigger"
: > "$W/dev/iio:device2"

SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" SENSORDIAG_CFG="$W/missing.cfg" \
  sh "$ROOT/usb/sensordiag.sh" "$W/sensors.txt"

grep -q "iio:device2" "$W/sensors.txt"
grep -q "in_accel_x_raw: 136" "$W/sensors.txt"
grep -q "scan_elements/in_accel_x_en: 1" "$W/sensors.txt"
grep -q "scan_elements/in_accel_x_type: le:s16/16>>0" "$W/sensors.txt"
grep -q "buffer/enable: 0" "$W/sensors.txt"
grep -q "IIO controls permissions (buffered HAL writes)" "$W/sensors.txt"
grep -q "$D/scan_elements/in_accel_x_en" "$W/sensors.txt"
grep -q "buffer/length: 16" "$W/sensors.txt"
grep -q "current_timestamp_clock: monotonic" "$W/sensors.txt"
grep -q "05 20 09 73" "$W/sensors.txt"
awk '/--- .*0AC3.000B$/ { active=1; next } /^--- |^== / { active=0 } active { n+=NF } END { exit n != 4096 }' "$W/sensors.txt"
grep -q "report_descriptor unavailable or unreadable" "$W/sensors.txt"
grep -q "$W/dev/iio:device2" "$W/sensors.txt"
grep -q 'SKIP: configuration unavailable or unreadable:' "$W/sensors.txt"

rm "$W/dev/iio:device2"
ln -s /dev/zero "$W/dev/iio:device2"
echo SENSOR_EVENT_PROBE=0 > "$W/gbos.cfg"
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" sh "$ROOT/usb/sensordiag.sh" "$W/disabled.txt"
grep -q 'SKIP: SENSOR_EVENT_PROBE=1 not set' "$W/disabled.txt"
echo SENSOR_EVENT_PROBE=1 > "$W/gbos.cfg"
for axis in x y z; do
  echo 0 > "$D/scan_elements/in_accel_${axis}_en"
  echo 'le:s32/32>>0' > "$D/scan_elements/in_accel_${axis}_type"
done
echo 0 > "$D/scan_elements/in_accel_x_index"
echo 1 > "$D/scan_elements/in_accel_y_index"
echo 2 > "$D/scan_elements/in_accel_z_index"
echo 0 > "$D/scan_elements/in_timestamp_en"
echo 3 > "$D/scan_elements/in_timestamp_index"
echo 'le:s64/64>>0' > "$D/scan_elements/in_timestamp_type"
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" sh "$ROOT/usb/sensordiag.sh" "$W/probe.txt"
grep -q 'IIO controls restored' "$W/probe.txt"
grep -q 'After sample: realtime=' "$W/probe.txt"
grep -q '24 bytes' "$W/probe.txt"
grep -q '00 00 00' "$W/probe.txt"
for f in "$D/buffer/enable" "$D"/scan_elements/*_en; do
  [ "$(cat "$f")" = 0 ] || { echo "control not restored: $f" >&2; exit 1; }
done

echo SENSOR_MOTION_PROBE=1 > "$W/gbos.cfg"
GETEVENT_PID_FILE="$W/input-pid" SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" SENSORDIAG_MOTION_DELAY_SECONDS=0 SENSORDIAG_MOTION_PHASE_SECONDS=0 \
  sh "$ROOT/usb/sensordiag.sh" "$W/motion.txt"
grep -q 'Motion pose trace starts in 0s' "$W/motion.txt"
[ "$(grep -c '^MOTION_PHASE [1-6]/6 start ' "$W/motion.txt")" = 6 ]
[ "$(grep -c '^MOTION_PHASE [1-6]/6 samples: 400 scans, 9600 bytes' "$W/motion.txt")" = 6 ]
grep -q 'IIO controls restored' "$W/motion.txt"
grep -q 'MOTION_INPUT start realtime=' "$W/motion.txt"
grep -q 'MOTION_INPUT end realtime=' "$W/motion.txt"
grep -q 'KEY_VOLUMEUP DOWN' "$W/motion.txt"
grep -q 'KEY_SPACE UP' "$W/motion.txt"
[ ! -e "/proc/$(cat "$W/input-pid")" ] || { echo "FAIL: input capture process still running" >&2; exit 1; }
if grep -Eq 'KEY_A|ABS_X' "$W/motion.txt"; then
  echo "FAIL: unrelated input was persisted" >&2
  exit 1
fi
if grep -q 'WARNING: no volume-up or space keypress marker captured' "$W/motion.txt"; then
  echo "FAIL: motion key marker was not detected" >&2
  exit 1
fi
for f in "$D/buffer/enable" "$D"/scan_elements/*_en; do
  [ "$(cat "$f")" = 0 ] || { echo "motion-probe control not restored: $f" >&2; exit 1; }
done
rm -f "$W/input-pid"
printf 'SENSOR_MOTION_PROBE=1\nSENSOR_CONTINUOUS_PROBE=1\n' > "$W/gbos.cfg"
GETEVENT_PID_FILE="$W/input-pid" SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" SENSORDIAG_CONTINUOUS_SECONDS=1 SENSORDIAG_CONTINUOUS_MAX_SCANS=3 \
  sh "$ROOT/usb/sensordiag.sh" "$W/continuous.txt"
grep -q 'MOTION_CONTINUOUS start realtime=' "$W/continuous.txt"
grep -q 'MOTION_CONTINUOUS sampling settings before enabling scan channels:' "$W/continuous.txt"
grep -q 'MOTION_CONTINUOUS sampling settings with buffer enabled:' "$W/continuous.txt"
grep -q 'in_accel_sampling_frequency: 10' "$W/continuous.txt"
grep -q 'hwfifo_timeout: 0.04' "$W/continuous.txt"
grep -q 'MOTION_CONTINUOUS samples: 3 scans, 72 bytes, dd_status=0' "$W/continuous.txt"
grep -q 'MOTION_CONTINUOUS end realtime=' "$W/continuous.txt"
grep -q 'WARNING: continuous trace reached its scan limit' "$W/continuous.txt"
grep -q 'IIO controls restored' "$W/continuous.txt"
[ ! -e "$W/input-pid" ] || { echo "FAIL: continuous trace started input capture" >&2; exit 1; }
if grep -q '^MOTION_PHASE' "$W/continuous.txt"; then
  echo "FAIL: continuous trace ran labeled phases" >&2
  exit 1
fi
for f in "$D/buffer/enable" "$D"/scan_elements/*_en; do
  [ "$(cat "$f")" = 0 ] || { echo "continuous-probe control not restored: $f" >&2; exit 1; }
done
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" SENSORDIAG_CONTINUOUS_SECONDS=bad \
  sh "$ROOT/usb/sensordiag.sh" "$W/continuous-invalid.txt"
grep -q 'ERROR: invalid continuous trace limits' "$W/continuous-invalid.txt"
grep -q 'IIO controls restored' "$W/continuous-invalid.txt"
rm "$W/dev/iio:device2"
ln -s /dev/null "$W/dev/iio:device2"
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" SENSORDIAG_CONTINUOUS_SECONDS=1 SENSORDIAG_CONTINUOUS_MAX_SCANS=3 \
  sh "$ROOT/usb/sensordiag.sh" "$W/continuous-empty.txt"
grep -q 'ERROR: incomplete continuous IIO scans (0 bytes)' "$W/continuous-empty.txt"
grep -q 'IIO controls restored' "$W/continuous-empty.txt"
rm "$W/dev/iio:device2"
ln -s /dev/zero "$W/dev/iio:device2"
echo SENSOR_MOTION_PROBE=1 > "$W/gbos.cfg"
FAIL_INPUT=1 SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" SENSORDIAG_MOTION_DELAY_SECONDS=0 SENSORDIAG_MOTION_PHASE_SECONDS=0 \
  sh "$ROOT/usb/sensordiag.sh" "$W/input-failed.txt"
grep -q 'ERROR: getevent exited early (status 2)' "$W/input-failed.txt"
grep -q 'WARNING: no volume-up or space keypress marker captured' "$W/input-failed.txt"
grep -q 'IIO controls restored' "$W/input-failed.txt"
rm "$W/dev/iio:device2"
ln -s /dev/null "$W/dev/iio:device2"
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" SENSORDIAG_MOTION_DELAY_SECONDS=0 SENSORDIAG_MOTION_PHASE_SECONDS=0 \
  sh "$ROOT/usb/sensordiag.sh" "$W/motion-empty.txt"
grep -q 'ERROR: incomplete IIO scans in phase 1 (0 bytes)' "$W/motion-empty.txt"
grep -q 'IIO controls restored' "$W/motion-empty.txt"
for f in "$D/buffer/enable" "$D"/scan_elements/*_en; do
  [ "$(cat "$f")" = 0 ] || { echo "motion-probe control not restored after failure: $f" >&2; exit 1; }
done
rm "$W/dev/iio:device2"
ln -s /dev/zero "$W/dev/iio:device2"
echo SENSOR_EVENT_PROBE=1 > "$W/gbos.cfg"

echo 1 > "$D/buffer/enable"
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" sh "$ROOT/usb/sensordiag.sh" "$W/busy.txt"
grep -q 'SKIP: IIO buffer is active' "$W/busy.txt"
[ "$(cat "$D/buffer/enable")" = 1 ]
echo 0 > "$D/buffer/enable"
rm "$W/dev/iio:device2"
ln -s /dev/null "$W/dev/iio:device2"
SENSORDIAG_IIO_SYSFS="$W/iio" SENSORDIAG_DEV_DIR="$W/dev" SENSORDIAG_HID_SYSFS="$W/hid" \
  SENSORDIAG_CFG="$W/gbos.cfg" sh "$ROOT/usb/sensordiag.sh" "$W/empty.txt"
grep -q 'ERROR: incomplete IIO sample (0 of 24 bytes)' "$W/empty.txt"
grep -q 'IIO controls restored' "$W/empty.txt"
for f in "$D/buffer/enable" "$D"/scan_elements/*_en; do
  [ "$(cat "$f")" = 0 ] || { echo "control not restored after failure: $f" >&2; exit 1; }
done
echo "SENSORDIAG_TEST_PASS"
