#!/bin/sh
# Build (or, with --check, verify) the Latitude 7320 rotation-policy overlay usb/display/gbos-latitude7320-rotation-policy.apk
# (static overlay of android, priority 100, only config_supportAutoRotation=true) from sensors/trogdor/display-policy/
# with tools/build_static_rro.py (no aapt2/apksigner or framework-res needed; needs python3 with the `cryptography`
# package). The package name and priority are read from display-policy/AndroidManifest.xml. The APK is committed;
# rebuild it only when the sources change. The former aapt2 build had no signature and Android never registered it.
#   tools/build_rotation_policy_overlay.sh [OUT.apk]      build (signed with a throwaway key; no key is kept)
#   tools/build_rotation_policy_overlay.sh --check [APK]  verify the signature and that the APK matches the sources
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC=$ROOT/sensors/trogdor/display-policy
MANIFEST=$SRC/AndroidManifest.xml
VALUES=$SRC/res/values/config.xml
APK=$ROOT/usb/display/gbos-latitude7320-rotation-policy.apk
PKG=$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$MANIFEST")
PRIORITY=$(sed -n 's/.*android:priority="\([0-9]*\)".*/\1/p' "$MANIFEST")
[ -n "$PKG" ] && [ -n "$PRIORITY" ] || { echo "cannot read package/priority from $MANIFEST" >&2; exit 1; }
grep -Fq 'android:targetPackage="android"' "$MANIFEST" && grep -Fq 'android:isStatic="true"' "$MANIFEST" || {
  echo "$MANIFEST is not a static overlay of android (build_static_rro.py only writes those)" >&2; exit 1; }
if [ "${1:-}" = --check ]; then
  exec python3 -I "$ROOT/tools/build_static_rro.py" verify --package "$PKG" --priority "$PRIORITY" --values "$VALUES" "${2:-$APK}"
fi
exec python3 -I "$ROOT/tools/build_static_rro.py" build --package "$PKG" --priority "$PRIORITY" --values "$VALUES" "${1:-$APK}"
