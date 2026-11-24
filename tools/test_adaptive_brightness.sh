#!/bin/sh
# Host test for Latitude 7320 adaptive brightness: the committed overlay APK (v2 signature, manifest, value, equal to a
# fresh build of its sources), the panel display config's curve, and hook.sh's staging, each with negative controls.
# Needs python3 with the `cryptography` package. Prints ADAPTIVE_BRIGHTNESS_TEST_PASS.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
APK=$ROOT/usb/display/gbos-latitude7320-brightness.apk
DDC=$ROOT/usb/display/latitude7320_display_port_0.xml
VALUES=$ROOT/sensors/trogdor/brightness-policy/res/values/config.xml
RRO=$ROOT/tools/build_static_rro.py

# ---- overlay APK --------------------------------------------------------------------------------------------------
grep -Fq '<bool name="config_automatic_brightness_available">true</bool>' "$VALUES" || fail "overlay source does not enable the flag"
sh "$ROOT/tools/build_brightness_overlay.sh" --check > "$T/check" || { cat "$T/check"; fail "committed overlay APK does not verify"; }
grep -q '^RRO OK ' "$T/check" || fail "no RRO OK line"
# The manifest/resources are what aapt2 writes (target android, static, priority 101) and carry the value.
python3 -I - "$RRO" "$APK" <<'EOF' || fail "overlay APK content"
import sys, zipfile, importlib.util
spec = importlib.util.spec_from_file_location('rro', sys.argv[1]); rro = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rro)
z = zipfile.ZipFile(sys.argv[2])
m, r = z.read('AndroidManifest.xml'), z.read('resources.arsc')
assert m == rro.build_manifest('android.overlay.gbos.latitude7320.brightness', 'android', 101)
for s in ('android.overlay.gbos.latitude7320.brightness', 'targetPackage', 'isStatic', 'priority'):
    assert s.encode('utf-16-le') in m, s
assert b'config_automatic_brightness_available' in r
# the one entry: bool, Res_value type INT_BOOLEAN (0x12), data 0xffffffff (true)
assert r.endswith(bytes.fromhex('0800 0000 0000 0000 0800 0012 ffffffff'.replace(' ', ''))), r[-16:].hex()
EOF
# Negative controls: a modified, an unsigned and a wrong-valued APK must all be refused.
python3 -I - "$APK" "$T/tampered.apk" <<'EOF'
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
i = b.index(b'config_automatic_brightness_available')
b[i] ^= 0x20
open(sys.argv[2], 'wb').write(b)
EOF
! sh "$ROOT/tools/build_brightness_overlay.sh" --check "$T/tampered.apk" > "$T/out" || fail "tampered APK verified"
grep -q 'content digest mismatch' "$T/out" || fail "tampered APK: $(cat "$T/out")"
python3 -I - "$RRO" "$VALUES" "$T/unsigned.apk" <<'EOF'
import sys, importlib.util
spec = importlib.util.spec_from_file_location('rro', sys.argv[1]); rro = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rro)
entries = [('AndroidManifest.xml', rro.build_manifest('android.overlay.gbos.latitude7320.brightness', 'android', 101)),
           ('resources.arsc', rro.build_arsc('android.overlay.gbos.latitude7320.brightness', rro.load_values(sys.argv[2])))]
a, cd, eocd = rro.build_zip(entries)
open(sys.argv[3], 'wb').write(a + cd + eocd)
EOF
! sh "$ROOT/tools/build_brightness_overlay.sh" --check "$T/unsigned.apk" > "$T/out" || fail "unsigned APK verified"
grep -q 'unsigned APK' "$T/out" || fail "unsigned APK: $(cat "$T/out")"
sed 's/>true</>false</' "$VALUES" > "$T/false.xml"
python3 -I "$RRO" build --package android.overlay.gbos.latitude7320.brightness --priority 101 --values "$T/false.xml" \
  "$T/false.apk" > /dev/null
python3 -I "$RRO" verify "$T/false.apk" > /dev/null || fail "a validly signed APK did not verify"
! sh "$ROOT/tools/build_brightness_overlay.sh" --check "$T/false.apk" > "$T/out" || fail "APK with the flag false accepted"
grep -q 'resources.arsc differs' "$T/out" || fail "false APK: $(cat "$T/out")"

# ---- panel display config ------------------------------------------------------------------------------------------
cat > "$T/ddc.py" <<'EOF'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
assert root.tag == 'displayConfiguration', root.tag
ab = root.find('autoBrightness')
assert ab is not None and ab.get('enabled') == 'true', 'autoBrightness enabled="true" missing'
maps = ab.findall('luxToBrightnessMapping')
assert len(maps) == 1, 'one luxToBrightnessMapping expected'
assert maps[0].findtext('mode') == 'default' and maps[0].findtext('setting') == 'normal', 'mode/setting'
pts = [(float(p.findtext('first')), float(p.findtext('second'))) for p in maps[0].findall('map/point')]
assert len(pts) >= 2, 'too few points'
assert pts[0][0] == 0, 'first lux must be 0 (DisplayBrightnessMappingConfig requires it)'
assert all(a[0] < b[0] for a, b in zip(pts, pts[1:])), 'lux not strictly increasing'
assert all(a[1] <= b[1] for a, b in zip(pts, pts[1:])), 'brightness decreases'
assert all(0 < v <= 1 for _, v in pts), 'brightness outside (0, 1]'
assert pts[-1][1] == 1.0, 'curve never reaches full backlight'
# Indoors (250-300 lux) near the framework default 102/255, dark under 0.1.
indoor = [v for lux, v in pts if lux == 250]
assert indoor and abs(indoor[0] - 102 / 255) < 0.05, 'indoor point not near the default brightness'
assert pts[0][1] < 0.1, 'dark level too bright'
for tag in ('brighteningLightDebounceMillis', 'darkeningLightDebounceMillis'):
    assert int(ab.findtext(tag)) > 0, tag
assert root.findtext('lightSensor/type') == 'android.sensor.light', 'light sensor type'
assert root.find('screenBrightnessMap') is None, 'a nits map would switch the curve to nits'
EOF
python3 -I "$T/ddc.py" "$DDC" || fail "panel display config"
for bad in 's#<first>0</first>#<first>5</first>#' 's#<second>0.48</second>#<second>0.30</second>#' \
           's#enabled="true"#enabled="false"#' 's#<second>1.0</second>#<second>1.5</second>#'; do
  sed "$bad" "$DDC" > "$T/bad.xml"
  cmp -s "$DDC" "$T/bad.xml" && fail "negative control did not change the file: $bad"
  if python3 -I "$T/ddc.py" "$T/bad.xml" 2> /dev/null; then fail "broken display config accepted: $bad"; fi
done

# ---- hook.sh staging -----------------------------------------------------------------------------------------------
awk '/^gbos_enable_latitude7320_brightness\(\) \{/ { copy=1 } copy { print } copy && /^}/ { exit }' \
  "$ROOT/usb/hook.sh" > "$T/function.sh"
[ -s "$T/function.sh" ] || fail "hook.sh has no gbos_enable_latitude7320_brightness"
. "$T/function.sh"
sensors_line=$(grep -n '^gbos_enable_latitude7320_sensors$' "$ROOT/usb/hook.sh" | cut -d: -f1)
bright_line=$(grep -n '^gbos_enable_latitude7320_brightness$' "$ROOT/usb/hook.sh" | cut -d: -f1)
[ -n "$sensors_line" ] && [ -n "$bright_line" ] && [ "$bright_line" -gt "$sensors_line" ] ||
  fail "hook.sh must call the brightness step after the sensor HAL step"
grep -q 'for d in [^;]* display; do' "$ROOT/tools/deploy_stick.sh" || fail "deploy_stick.sh does not copy display/"
grep -q 'for d in [^;]* display; do cp -R' "$ROOT/tools/make_gbos86_image.sh" || fail "make_gbos86_image.sh does not copy display/"
grep -q '^for d in sensors camera thermal display; do' "$ROOT/usb/install.sh" || fail "install.sh does not copy display/"
grep -q '^ADAPTIVE_BRIGHTNESS=1$' "$ROOT/usb/gbos.cfg" || fail "gbos.cfg does not document ADAPTIVE_BRIGHTNESS"

log() { printf '%s\n' "$*" >> "$T/log"; }
setup() {
  rm -rf "$T/root" "$T/gbos" "$T/log"
  mkdir -p "$T/root/vendor/overlay" "$T/root/vendor/etc/displayconfig" "$T/gbos/display"
  printf 'google vendor overlay\n' > "$T/root/vendor/overlay/framework-res__lapis__auto_generated_rro_vendor.apk"
  printf 'google port 1\n' > "$T/root/vendor/etc/displayconfig/display_port_1.xml"
  cp "$APK" "$DDC" "$T/gbos/display/"
  NEWROOT=$T/root G=$T/gbos GBOS_SENSOR_HAL_SELECTED=1
  unset ADAPTIVE_BRIGHTNESS
}
OVL=$T/root/vendor/overlay/gbos-latitude7320-brightness.apk
PORT0=$T/root/vendor/etc/displayconfig/display_port_0.xml

setup
gbos_enable_latitude7320_brightness
[ "$GBOS_BRIGHTNESS_SELECTED" = 1 ] || fail "not selected"
cmp "$APK" "$OVL" && cmp "$DDC" "$PORT0" || fail "files not staged"
[ "$(ls -l "$OVL" | cut -c1-10)" = -rw-r--r-- ] && [ "$(ls -l "$PORT0" | cut -c1-10)" = -rw-r--r-- ] || fail "mode not 0644"
[ -z "$(find "$T/root" -name '*.part')" ] || fail "partial files left"
grep -q 'adaptive brightness enabled (overlay' "$T/log" || fail "no enabled log line"
gbos_enable_latitude7320_brightness   # second run (hook re-run): same result
[ "$GBOS_BRIGHTNESS_SELECTED" = 1 ] && cmp "$APK" "$OVL" && cmp "$DDC" "$PORT0" || fail "second run"

# negative controls: switched off, no sensor HAL (other laptops), missing payload
setup; ADAPTIVE_BRIGHTNESS=0
gbos_enable_latitude7320_brightness
[ "$GBOS_BRIGHTNESS_SELECTED" = 0 ] && [ ! -e "$OVL" ] && [ ! -e "$PORT0" ] || fail "ADAPTIVE_BRIGHTNESS=0 staged files"
grep -q 'disabled (ADAPTIVE_BRIGHTNESS=0)' "$T/log" || fail "no disabled log line"
setup; GBOS_SENSOR_HAL_SELECTED=0
gbos_enable_latitude7320_brightness
[ "$GBOS_BRIGHTNESS_SELECTED" = 0 ] && [ ! -e "$OVL" ] && [ ! -e "$PORT0" ] || fail "staged without the sensor HAL"
setup; rm "$T/gbos/display/gbos-latitude7320-brightness.apk"
gbos_enable_latitude7320_brightness
[ "$GBOS_BRIGHTNESS_SELECTED" = 0 ] && [ ! -e "$PORT0" ] || fail "staged without the APK"
grep -q 'adaptive brightness unavailable (missing' "$T/log" || fail "no missing-payload log line"

# Google's own display_port_0.xml is never replaced; the overlay still goes in.
setup; printf 'google port 0\n' > "$PORT0"
gbos_enable_latitude7320_brightness
[ "$GBOS_BRIGHTNESS_SELECTED" = 1 ] && cmp "$APK" "$OVL" || fail "overlay missing next to Google's port 0 config"
[ "$(cat "$PORT0")" = 'google port 0' ] || fail "Google's display_port_0.xml replaced"
grep -q "Google's own display_port_0.xml kept" "$T/log" || fail "no kept log line"

# Read-only vendor (no overlay): fails, leaves nothing behind.
setup; rm -rf "$T/root/vendor/overlay"; printf 'not a directory\n' > "$T/root/vendor/overlay"
if gbos_enable_latitude7320_brightness 2> /dev/null; then fail "accepted an unwritable /vendor/overlay"; fi
[ "$GBOS_BRIGHTNESS_SELECTED" = 0 ] && [ ! -e "$PORT0" ] || fail "partial staging"
grep -q 'adaptive brightness NOT enabled' "$T/log" || fail "no failure log line"

echo "ADAPTIVE_BRIGHTNESS_TEST_PASS"
