#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/sensor_payload_test.XXXXXX")
trap 'rm -rf "$W"' EXIT

mkdir -p "$W/bin" "$W/gbos" "$W/inputs"
cat > "$W/bin/python3" <<'EOF'
#!/bin/sh
set -eu
[ "$2" = "--inputs" ] && [ "$3" = "test-inputs" ]
[ "$4" = "--clang" ] && [ "$5" = "test-clang" ]
[ "$6" = "--output" ]
[ "${FAIL_BUILD:-0}" = 0 ] || exit 1
printf 'test sensor library\n' > "$7"
EOF
chmod +x "$W/bin/python3"
cat > "$W/bin/docker" <<'EOF'
#!/bin/sh
set -eu
[ "$1" = run ]
[ "$2" = --rm ]
[ "$3" = --pull=never ]
while [ "$#" -gt 0 ]; do
  if [ "$1" = --output ]; then
    shift
    case "$1" in /out/*) name=${1#/out/};; *) exit 2;; esac
    [ "${FAIL_BUILD:-0}" = 0 ] || exit 1
    printf 'test sensor library\n' > "$MOCK_OUTPUT/$name"
    exit 0
  fi
  shift
done
exit 2
EOF
chmod +x "$W/bin/docker"
LIB="$W/out/test.so"
PATH="$W/bin:$PATH" sh "$ROOT/sensors/trogdor/image_payload.sh" build test-inputs test-clang "$LIB"
sh "$ROOT/sensors/trogdor/image_payload.sh" stage "$LIB" "$W/gbos"
cmp "$LIB" "$W/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so"
MOCK_OUTPUT="$W/out" PATH="$W/bin:$PATH" sh "$ROOT/sensors/trogdor/image_payload.sh" build-docker "$W/inputs" "$LIB" test-clang
cmp "$LIB" "$W/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so"
if FAIL_BUILD=1 MOCK_OUTPUT="$W/out" PATH="$W/bin:$PATH" sh "$ROOT/sensors/trogdor/image_payload.sh" build-docker "$W/inputs" "$LIB" test-clang; then
  echo "failed sensor build unexpectedly succeeded" >&2
  exit 1
fi
cmp "$LIB" "$W/gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so"
if sh "$ROOT/sensors/trogdor/image_payload.sh" stage "$W/missing" "$W/gbos" 2>/dev/null; then
  echo "missing sensor library unexpectedly staged" >&2
  exit 1
fi
printf 'MODE=ask\nSENSOR_EVENT_PROBE=0\nOTHER_SETTING=keep\n' > "$W/gbos.cfg"
sh "$ROOT/sensors/trogdor/image_payload.sh" enable-motion-probe "$W/gbos.cfg"
sh "$ROOT/sensors/trogdor/image_payload.sh" enable-motion-probe "$W/gbos.cfg"
[ "$(grep -c '^SENSOR_EVENT_PROBE=1$' "$W/gbos.cfg")" = 1 ]
[ "$(grep -c '^SENSOR_MOTION_PROBE=1$' "$W/gbos.cfg")" = 1 ]
grep -q '^OTHER_SETTING=keep$' "$W/gbos.cfg"
printf 'MODE=ask\nSENSOR_EVENT_PROBE=0\nOTHER_SETTING=keep\n' > "$W/gbos.cfg"
sh "$ROOT/sensors/trogdor/image_payload.sh" enable-continuous-probe "$W/gbos.cfg"
sh "$ROOT/sensors/trogdor/image_payload.sh" enable-continuous-probe "$W/gbos.cfg"
[ "$(grep -c '^SENSOR_EVENT_PROBE=1$' "$W/gbos.cfg")" = 1 ]
[ "$(grep -c '^SENSOR_CONTINUOUS_PROBE=1$' "$W/gbos.cfg")" = 1 ]
! grep -q '^SENSOR_MOTION_PROBE=1$' "$W/gbos.cfg"
grep -q '^OTHER_SETTING=keep$' "$W/gbos.cfg"
echo "SENSOR_IMAGE_PAYLOAD_TEST_PASS"
