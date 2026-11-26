#!/bin/sh
# Build (or, with --check, verify) usb/display/gbos-updater-settings.apk: a static overlay of Google's Settings that shows the
# gbOS86 Updater as Settings > System > Software update > "gbOS86 updates" (updater-app/settings-overlay/res/values/config.xml)
# with tools/build_static_rro.py (no aapt2/apksigner needed; needs python3 with the `cryptography` package). The APK is
# committed; rebuild it only when the values change. hook.sh (gbos_settings_updater_entry) puts it in /vendor/overlay.
#   tools/build_settings_updater_overlay.sh [OUT.apk]      build (signed with a throwaway key; no key is kept)
#   tools/build_settings_updater_overlay.sh --check [APK]  verify the signature and that the APK matches the sources
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PKG=org.gbos86.overlay.settings.updater
PRIORITY=102
TARGET=com.android.settings
VALUES=$ROOT/updater-app/settings-overlay/res/values/config.xml
APK=$ROOT/usb/display/gbos-updater-settings.apk
if [ "${1:-}" = --check ]; then
  exec python3 -I "$ROOT/tools/build_static_rro.py" verify --package "$PKG" --priority "$PRIORITY" --target "$TARGET" --values "$VALUES" "${2:-$APK}"
fi
exec python3 -I "$ROOT/tools/build_static_rro.py" build --package "$PKG" --priority "$PRIORITY" --target "$TARGET" --values "$VALUES" "${1:-$APK}"
