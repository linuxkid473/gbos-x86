#!/bin/sh
# Build (or, with --check, verify) the Latitude 7320 adaptive-brightness overlay usb/display/gbos-latitude7320-brightness.apk
# from sensors/trogdor/brightness-policy/res/values/config.xml with tools/build_static_rro.py (no aapt2/apksigner needed;
# needs python3 with the `cryptography` package). The APK is committed; rebuild it only when the values change.
#   tools/build_brightness_overlay.sh [OUT.apk]      build (signed with a throwaway key; no key is kept)
#   tools/build_brightness_overlay.sh --check [APK]  verify the signature and that the APK matches the sources
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG=android.overlay.gbos.latitude7320.brightness
PRIORITY=101
VALUES=$ROOT/sensors/trogdor/brightness-policy/res/values/config.xml
APK=$ROOT/usb/display/gbos-latitude7320-brightness.apk
if [ "${1:-}" = --check ]; then
  exec python3 -I "$ROOT/tools/build_static_rro.py" verify --package "$PKG" --priority "$PRIORITY" --values "$VALUES" "${2:-$APK}"
fi
exec python3 -I "$ROOT/tools/build_static_rro.py" build --package "$PKG" --priority "$PRIORITY" --values "$VALUES" "${1:-$APK}"
