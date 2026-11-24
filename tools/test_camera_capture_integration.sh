#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

cc -std=gnu11 -O2 -Wall -Wextra -Werror "$ROOT/tools/test_camera_capture_probe.c" -o "$TMP/test"
"$TMP/test"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -static "$ROOT/tools/camera_capture_probe.c" -o "$TMP/helper"
cmp "$TMP/helper" "$ROOT/usb/installer/bin/camera-capture-probe"
if "$TMP/helper" "$TMP/no-media" ov5675 > "$TMP/output" 2>&1; then
    echo "missing media node was accepted" >&2; exit 1
fi
grep -q 'open media:' "$TMP/output"
if "$TMP/helper" "$TMP/no-media" invalid > "$TMP/output" 2>&1; then
    echo "invalid sensor was accepted" >&2; exit 1
fi
grep -q '^usage:' "$TMP/output"
sh -n "$ROOT/usb/hook.sh" "$ROOT/usb/installer/bin/camera-capture-diag.sh"
sed -n '/^  GBOS_CAMERA_CAPTURE_START=$/,/^  fi$/p' "$ROOT/usb/hook.sh" > "$TMP/gate"
test -s "$TMP/gate"
for setting in 0 1; do
    for from in stick nvme; do
        CAMERA_CAPTURE_PROBE=$setting FROM=$from
        . "$TMP/gate"
        if [ "$setting" = 1 ] && [ "$from" = stick ]; then
            test "$GBOS_CAMERA_CAPTURE_START" = '    start gbos_camera_capture'
        else
            test -z "$GBOS_CAMERA_CAPTURE_START"
        fi
    done
    CAMERA_CAPTURE_PROBE=1 FROM=stick CAMERA_CAPTURE_SENSOR=invalid
    . "$TMP/gate"
    test -z "$GBOS_CAMERA_CAPTURE_START"
    CAMERA_CAPTURE_SENSOR=ov8856
    . "$TMP/gate"
    test "$GBOS_CAMERA_CAPTURE_START" = '    start gbos_camera_capture'
done
grep -q 'for d in .*installer' "$ROOT/tools/make_gbos86_image.sh"
echo CAMERA_CAPTURE_INTEGRATION_TEST_PASS
