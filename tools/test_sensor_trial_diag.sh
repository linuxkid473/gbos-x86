#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
cat > "$T/bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$T/bin/timeout" <<'EOF'
#!/bin/sh
shift
exec "$@"
EOF
cat > "$T/bin/dumpsys" <<'EOF'
#!/bin/sh
case "$1" in
  sensorservice) echo "Sensor Event: test" ;;
  window) echo "rotation=1" ;;
  display) printf '%s\n' "  DisplayDeviceConfig{mLoadedFrom=/vendor/etc/displayconfig/display_port_0.xml" \
    "  unrelated line" "  mAutoBrightnessEnabled=true" ;;
  *) exit 1 ;;
esac
EOF
cat > "$T/bin/cmd" <<'EOF'
#!/bin/sh
case "$1 $2" in
  "window fixed-to-user-rotation") echo "disabled" ;;
  "overlay list") echo "[x] android.overlay.gbos.latitude7320.rotation" ;;
  "overlay lookup") [ "$*" = "overlay lookup --verbose android android:bool/config_supportAutoRotation" ] &&
    echo "Overlaid: android.overlay.gbos.latitude7320.rotation" ;;
  *) exit 1 ;;
esac
EOF
cat > "$T/bin/settings" <<'EOF'
#!/bin/sh
[ "$*" = "get system screen_brightness_mode" ] && echo 1
EOF
chmod +x "$T/bin/"*
PATH="$T/bin:$PATH" sh "$ROOT/usb/sensor_trial_diag.sh" "$T/out"
[ "$(grep -c '^== snapshot ' "$T/out")" = 11 ]
[ "$(grep -c '^Sensor Event: test' "$T/out")" = 11 ]
[ "$(grep -c '^rotation=1' "$T/out")" = 11 ]
[ "$(grep -c '^disabled$' "$T/out")" = 11 ]
[ "$(grep -c '^\[x\] android.overlay.gbos.latitude7320.rotation$' "$T/out")" = 11 ]
[ "$(grep -c '^-- config_supportAutoRotation resolution$' "$T/out")" = 11 ]
[ "$(grep -c '^Overlaid: android.overlay.gbos.latitude7320.rotation$' "$T/out")" = 11 ]
[ "$(grep -c '^-- adaptive brightness (screen_brightness_mode 1)$' "$T/out")" = 11 ]
[ "$(grep -c 'mLoadedFrom=/vendor/etc/displayconfig/display_port_0.xml' "$T/out")" = 11 ]
[ "$(grep -c 'mAutoBrightnessEnabled=true' "$T/out")" = 11 ]
! grep -q 'unrelated line' "$T/out"
echo "SENSOR_TRIAL_DIAG_TEST_PASS"
