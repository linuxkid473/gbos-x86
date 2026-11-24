#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
script=$ROOT/usb/installer/bin/camera-fw-trace.sh
sh -n "$script" "$ROOT/usb/installer/bin/camera-capture-diag.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir "$TMP/bin"
printf '#!/bin/sh\necho other-kernel\n' > "$TMP/bin/uname"
chmod +x "$TMP/bin/uname"
if PATH="$TMP/bin:$PATH" sh "$script" /bin/false ov5675 > "$TMP/out" 2>&1; then
    echo "failed capture reported success" >&2; exit 1
fi
grep -q 'TRACE UNAVAILABLE: response offsets verified only for kernel #61' "$TMP/out"
printf '#!/bin/sh\nexit 0\n' > "$TMP/helper"
chmod +x "$TMP/helper"
PATH="$TMP/bin:$PATH" sh "$script" "$TMP/helper" ov5675 > "$TMP/out" 2>&1
grep -q 'TRACE UNAVAILABLE:' "$TMP/out"
echo CAMERA_FW_TRACE_TEST_PASS
