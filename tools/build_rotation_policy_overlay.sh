#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$#" -ne 3 ]; then
  echo "usage: $0 AAPT2 FRAMEWORK_RES_APK OUTPUT_APK" >&2
  exit 1
fi

AAPT2=$1
FRAMEWORK_RES=$2
OUTPUT=$3

[ -x "$AAPT2" ] || { echo "AAPT2 is not executable: $AAPT2" >&2; exit 1; }
[ -s "$FRAMEWORK_RES" ] || { echo "framework-res.apk is missing or empty: $FRAMEWORK_RES" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/gbos-rotation-overlay.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$AAPT2" compile --dir "$ROOT/sensors/trogdor/display-policy/res" -o "$TMP/resources.zip"
"$AAPT2" link \
  --no-resource-removal \
  --auto-add-overlay \
  --manifest "$ROOT/sensors/trogdor/display-policy/AndroidManifest.xml" \
  -I "$FRAMEWORK_RES" \
  --min-sdk-version 37 \
  --target-sdk-version 37 \
  --compile-sdk-version-code 37 \
  --compile-sdk-version-name 17 \
  -o "$TMP/gbos-latitude7320-rotation-policy.apk" \
  "$TMP/resources.zip"

"$AAPT2" dump resources "$TMP/gbos-latitude7320-rotation-policy.apk" > "$TMP/resources.txt"
grep -A1 -F 'bool/config_supportAutoRotation' "$TMP/resources.txt" |
  grep -Eq '^[[:space:]]*\(\) true$' || {
    echo "rotation overlay does not set config_supportAutoRotation=true" >&2
    exit 1
  }
"$AAPT2" dump xmltree "$TMP/gbos-latitude7320-rotation-policy.apk" --file AndroidManifest.xml \
  > "$TMP/manifest.txt"
grep -Fq 'targetPackage(0x01010021)="android"' "$TMP/manifest.txt" || {
  echo "rotation overlay does not target android" >&2
  exit 1
}
grep -Fq 'priority(0x0101001c)=100' "$TMP/manifest.txt" || {
  echo "rotation overlay priority is not 100" >&2
  exit 1
}
grep -Fq 'isStatic(0x0101055a)=true' "$TMP/manifest.txt" || {
  echo "rotation overlay is not static" >&2
  exit 1
}

mkdir -p "$(dirname "$OUTPUT")"
mv -f "$TMP/gbos-latitude7320-rotation-policy.apk" "$OUTPUT"
