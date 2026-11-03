#!/bin/sh
# test_detect_hardware.sh: Unit tests for host hardware auto-detection & gbos.cfg tailoring
# Run: sh tools/test_detect_hardware.sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/hwdetect_test.XXXXXX"); trap 'rm -rf "$W"' EXIT

fails=0
n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fails=$((fails+1)); echo "FAIL $*"; }

# 1. Test DMI Model Detection with simulated DMI trees
test_dmi() {
  _vendor="$1"
  _family="$2"
  _ver="$3"
  _name="$4"
  _expected="$5"

  rm -rf "$W/dmi"
  mkdir -p "$W/dmi"
  echo "$_vendor" > "$W/dmi/sys_vendor"
  echo "$_family" > "$W/dmi/product_family"
  echo "$_ver" > "$W/dmi/product_version"
  echo "$_name" > "$W/dmi/product_name"

  # Override DMI_DIR inside subshell
  res=$(
    DMI_DIR="$W/dmi"
    . "$ROOT/tools/detect_hardware.sh"
    detect_host_hardware
    echo "$DETECTED_DEVICE_MODEL"
  )
  if [ "$res" = "$_expected" ]; then
    ok "dmi: '$_vendor' '$_family' -> '$_expected'"
  else
    bad "dmi: got '$res', expected '$_expected'"
  fi
}

test_dmi "LENOVO" "IdeaPad Slim 5 14IMH9" "IdeaPad Slim 5 14IMH9" "83DA" "Lenovo IdeaPad Slim 5 14IMH9"
test_dmi "Dell Inc." "Latitude" "None" "Latitude 5300" "Dell Latitude 5300"
test_dmi "HP" "HP ENVY Laptop" "System Version" "HP ENVY Laptop 13-aq0xxx" "HP ENVY Laptop 13-aq0xxx"
test_dmi "HP" "103C_5335KV G=N L=CON B=HP S=PAV X=Null " "Type1ProductConfigId" "HP Pavilion Notebook " "HP Pavilion Notebook"
test_dmi "Microsoft Corporation" "Surface Laptop" "1.0" "Surface Laptop 7" "Microsoft Surface Laptop 7"

# 2. Test Display Resolution & Density Calculation
test_density() {
  _res="$1"
  _expected_density="$2"

  rm -rf "$W/drm"
  mkdir -p "$W/drm/card0-eDP-1"
  echo "connected" > "$W/drm/card0-eDP-1/status"
  echo "$_res" > "$W/drm/card0-eDP-1/modes"

  (
    # Mock /sys/class/drm by pointing loop
    DETECTED_RESOLUTION=""
    DETECTED_DENSITY=180
    for _status in "$W/drm"/card*-*/status; do
      if grep -q '^connected$' "$_status"; then
        _dir=$(dirname "$_status")
        DETECTED_RESOLUTION=$(head -n1 "$_dir/modes")
      fi
    done
    _w=$(echo "$DETECTED_RESOLUTION" | cut -d'x' -f1)
    _h=$(echo "$DETECTED_RESOLUTION" | cut -d'x' -f2)
    if [ "$_w" -le 1366 ]; then DETECTED_DENSITY=140
    elif [ "$_w" -le 1600 ]; then DETECTED_DENSITY=160
    elif [ "$_w" -le 1920 ]; then DETECTED_DENSITY=180
    elif [ "$_w" -le 2560 ]; then DETECTED_DENSITY=220
    elif [ "$_w" -le 2880 ]; then DETECTED_DENSITY=240
    else DETECTED_DENSITY=320; fi

    [ "$DETECTED_DENSITY" = "$_expected_density" ]
  ) && ok "density: $_res -> ${_expected_density} DPI" || bad "density: $_res expected $_expected_density"
}

test_density "1366x768" 140
test_density "1600x900" 160
test_density "1920x1080" 180
test_density "1920x1200" 180
test_density "2560x1440" 220
test_density "2880x1800" 240
test_density "3840x2160" 320

# 3. Test apply_hardware_config modifying gbos.cfg
cp "$ROOT/usb/gbos.cfg" "$W/test.cfg"
(
  . "$ROOT/tools/detect_hardware.sh"
  DETECTED_DEVICE_MODEL="Test ThinkPad X1"
  DETECTED_DENSITY=220
  DETECTED_RESOLUTION="2560x1440"
  DETECTED_GPU_SW=0
  DETECTED_GPU_VK=auto
  DETECTED_LOAD_I915=1
  DETECTED_XE_PARAMS=""
  DETECTED_EXTRA_SLEEP=0
  apply_hardware_config "$W/test.cfg"

  grep -q '^DEVICE_MODEL="Test ThinkPad X1"$' "$W/test.cfg" || exit 1
  grep -q '^DENSITY=220$' "$W/test.cfg" || exit 2
  grep -q '^RESOLUTION="2560x1440"$' "$W/test.cfg" || exit 3
  grep -q '^LOAD_I915=1$' "$W/test.cfg" || exit 4
  grep -q '^GPU_SOFTWARE=0$' "$W/test.cfg" || exit 5
) && ok "apply_hardware_config: successfully updated gbos.cfg fields" || bad "apply_hardware_config failed"

# 4. Test AVX2 check in hook.sh
H="$ROOT/usb/hook.sh"
grep -q "grep -qw avx2" "$H" && ok "hook.sh: verifies avx2 instruction support" || bad "hook.sh: missing avx2 check"
grep -q "vpbroadcastb" "$H" && ok "hook.sh: explains AVX2 vpbroadcastb requirement" || bad "hook.sh: missing vpbroadcastb explanation"

echo
echo "$n checks, $fails failed"
[ "$fails" -eq 0 ] && echo "DETECT_HARDWARE_TEST_PASS"
exit "$fails"
