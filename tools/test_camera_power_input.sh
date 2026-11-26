#!/bin/sh
# Unit tests for UVC camera registration, touchpad IDC discovery, and power/suspend setup in usb/hook.sh.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HOOK=$ROOT/usb/hook.sh
PASS=0; FAIL=0
W=$(mktemp -d "${TMPDIR:-/tmp}/gbos-cpi-test.XXXXXX")
trap 'rm -rf "$W"' EXIT

ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; }
eq()   { [ "$2" = "$3" ] && ok "$1" || fail "$1 (got '$2', want '$3')"; }

# Extract helper functions from hook.sh (ignoring column-0 braces inside heredocs)
awk '
  /^gbos_(touchpads|uvc_max_res|camera_setup|power_setup)\(\)/ { fn=1 }
  fn && /<<'\''?[A-Z_]+'\''?$/ { hd=$NF; gsub(/<<'\''?|'\''/, "", hd); print; next }
  fn && hd != "" { print; if ($0 == hd) hd=""; next }
  fn { print; if (/^}$/) fn=0 }
' "$HOOK" > "$W/funcs.sh"

log() { echo "$*" >> "$W/hook.log"; }
mount() {
  # Mock mount --bind SRC DST by copying SRC over DST and recording the bind
  if [ "$1" = "--bind" ]; then
    cp -f "$2" "$3"
    echo "bind $2 -> $3" >> "$W/mounts.log"
    return 0
  fi
  return 0
}
. "$W/funcs.sh"

# 1. Test gbos_touchpads: Elantech PS/2, Synaptics RMI4 (Bus=001d, no "TouchPad" in name), Goodix I2C, and USB mouse ignored
cat > "$W/inputs.txt" <<'EOF'
I: Bus=0011 Vendor=0001 Product=0001 Version=ab41
N: Name="AT Translated Set 2 keyboard"

I: Bus=0011 Vendor=0002 Product=000e Version=0000
N: Name="ETPS/2 Elantech Touchpad"

I: Bus=001d Vendor=06cb Product=0000 Version=0000
N: Name="Synaptics TM2334-001"

I: Bus=0018 Vendor=27C6 Product=0D53 Version=0100
N: Name="GXTP5100:00 27C6:0D53 Touchpad"

I: Bus=0003 Vendor=1b96 Product=0f04 Version=0110
N: Name="N-trig DuoSense Mouse"
EOF

TP_LIST=$(INPUTS="$W/inputs.txt" gbos_touchpads | tr '\n' ' ' | sed 's/ *$//')
eq "touchpads: Elantech PS/2 + Synaptics RMI4 + Goodix I2C matched, USB ignored" "$TP_LIST" "0002:000e 06cb:0000 27c6:0d53"

# 2. Test gbos_uvc_max_res on fake_uvc_desc.bin
RES=$(gbos_uvc_max_res "$ROOT/usb/surface/fake_uvc_desc.bin")
eq "uvc_max_res: parses 1920x1080 from UVC VideoStreaming frame descriptors" "$RES" "1920x1080"

# 3. Test gbos_camera_setup on VAIO Flip 13 dual-camera layout (0bda:56ff Rear + 5986:0548 Front)
NEWROOT="$W/new"; SYSFS="$W/sys"; DEVROOT="$W/dev"; GBOS_RUN="$W/run"
mkdir -p "$NEWROOT/vendor/etc/camera" "$NEWROOT/vendor/etc/permissions" "$SYSFS/bus/usb/devices" "$DEVROOT" "$GBOS_RUN"
: > "$DEVROOT/video0"; : > "$DEVROOT/video2"; : > "$DEVROOT/media0"
chmod 0600 "$DEVROOT/video0" "$DEVROOT/video2" "$DEVROOT/media0"
cat > "$NEWROOT/vendor/etc/ueventd.rc" <<'EOF'
/dev/media* 0660 system system
EOF
cat > "$NEWROOT/vendor/etc/camera/internal_usb_camera_info.txtpb" <<'EOF'
use_hardware_jpeg_decoder: true
face_detection_input_width: 320
camera_modules {
  key: "0408:4052"
  value {
    enable_face_detection: true
    facing: FACING_FRONT
  }
}
EOF
cat > "$NEWROOT/vendor/etc/camera/effect_config_usb.json" <<'EOF'
{"stream_manipulators": [{"name": "VcEffects"}]}
EOF
cat > "$NEWROOT/vendor/etc/permissions/features.xml" <<'EOF'
<permissions>
  <feature name="android.hardware.camera.any"/>
  <feature name="android.hardware.camera.front"/>
</permissions>
EOF

# USB 3-4: Rear Camera 0bda:56ff (no descriptors -> fallback 1920x1080)
mkdir -p "$SYSFS/bus/usb/devices/3-4" "$SYSFS/bus/usb/devices/3-4:1.0"
echo 0bda > "$SYSFS/bus/usb/devices/3-4/idVendor"
echo 56ff > "$SYSFS/bus/usb/devices/3-4/idProduct"
echo "Rear Camera" > "$SYSFS/bus/usb/devices/3-4/product"
echo 0e > "$SYSFS/bus/usb/devices/3-4:1.0/bInterfaceClass"

# USB 1-7: Front Camera 5986:0548 (with fake_uvc_desc.bin -> 1920x1080)
mkdir -p "$SYSFS/bus/usb/devices/1-7" "$SYSFS/bus/usb/devices/1-7:1.0"
echo 5986 > "$SYSFS/bus/usb/devices/1-7/idVendor"
echo 0548 > "$SYSFS/bus/usb/devices/1-7/idProduct"
echo "Front Camera" > "$SYSFS/bus/usb/devices/1-7/product"
cp "$ROOT/usb/surface/fake_uvc_desc.bin" "$SYSFS/bus/usb/devices/1-7/descriptors"
echo 0e > "$SYSFS/bus/usb/devices/1-7:1.0/bInterfaceClass"

gbos_camera_setup

grep -q '^use_hardware_jpeg_decoder: false$' "$NEWROOT/vendor/etc/camera/internal_usb_camera_info.txtpb" \
  && ok "camera: use_hardware_jpeg_decoder set to false" || fail "camera: use_hardware_jpeg_decoder still true"
! grep -q 'enable_face_detection: true' "$NEWROOT/vendor/etc/camera/internal_usb_camera_info.txtpb" \
  && ok "camera: enable_face_detection disabled on all entries" || fail "camera: enable_face_detection still true"
grep -A25 'key: "5986:0548"' "$NEWROOT/vendor/etc/camera/internal_usb_camera_info.txtpb" | grep -q 'facing: FACING_FRONT' \
  && ok "camera: 5986:0548 Front Camera registered as FACING_FRONT" || fail "camera: 5986:0548 missing or wrong facing"
grep -A25 'key: "0bda:56ff"' "$NEWROOT/vendor/etc/camera/internal_usb_camera_info.txtpb" | grep -q 'facing: FACING_BACK' \
  && ok "camera: 0bda:56ff Rear Camera registered as FACING_BACK" || fail "camera: 0bda:56ff missing or wrong facing"
grep -q '"android.hardware.camera"' "$NEWROOT/vendor/etc/permissions/features.xml" \
  && ok "camera: rear camera feature added to features.xml" || fail "camera: rear camera feature missing"
! grep -q 'VcEffects' "$NEWROOT/vendor/etc/camera/effect_config_usb.json" \
  && ok "camera: NPU VcEffects stripped when /dev/accel/accel0 absent" || fail "camera: VcEffects still present"
grep -q '^/dev/video\* 0666' "$NEWROOT/vendor/etc/ueventd.rc" \
  && ok "camera: /dev/video* 0666 added to vendor ueventd.rc" || fail "camera: ueventd.rc missing /dev/video*"

# 4. Test gbos_power_setup: Power Button un-excluded, powerhint cpusets clamped to 0-3, mem_sleep=s2idle, wakeup enabled
cat > "$NEWROOT/vendor/etc/excluded-input-devices.xml" <<'EOF'
<devices><device name="Power Button"/></devices>
EOF
cat > "$NEWROOT/vendor/etc/powerhint.json" <<'EOF'
{
    "Nodes": [
        {
            "Name": "TopAppCpuset",
            "Path": "/dev/cpuset/top-app/cpus",
            "Values": [
                "0-7",
                "4-7",
                "0-3"
            ],
            "DefaultIndex": 0
        },
        {
            "Name": "WorkQueueAffinity",
            "Path": "/sys/devices/virtual/workqueue/cpumask",
            "Values": ["ff", "f0"],
            "DefaultIndex": 0
        }
    ],
    "Actions": [
        {
            "PowerHint": "PROFILE_MEDIA_PLAYBACK",
            "Node": "TopAppCpuset",
            "Duration": 0,
            "Value": "4-7"
        },
        {
            "PowerHint": "PROFILE_MEDIA_PLAYBACK",
            "Node": "WorkQueueAffinity",
            "Duration": 0,
            "Value": "f0"
        }
    ]
}
EOF
cat > "$W/cpuinfo4" <<'EOF'
processor	: 0
processor	: 1
processor	: 2
processor	: 3
EOF
mkdir -p "$SYSFS/power" "$SYSFS/bus/acpi/devices/PNP0C0C:00/power" "$SYSFS/bus/acpi/devices/PNP0C0D:00/power" \
         "$SYSFS/devices/platform/i8042/serio1/power" "$SYSFS/devices/platform/sony-laptop"
echo "s2idle [deep]" > "$SYSFS/power/mem_sleep"
: > "$SYSFS/power/wake_lock"
for p in "$SYSFS/bus/acpi/devices/PNP0C0C:00/power/wakeup" "$SYSFS/bus/acpi/devices/PNP0C0D:00/power/wakeup" "$SYSFS/devices/platform/i8042/serio1/power/wakeup"; do
  echo disabled > "$p"
done
echo 0 > "$SYSFS/devices/platform/sony-laptop/lid_resume_S3"

CPUINFO="$W/cpuinfo4" SUSPEND_MODE=guard gbos_power_setup

! grep -q 'Power Button' "$NEWROOT/vendor/etc/excluded-input-devices.xml" \
  && ok "power: ACPI Power Button un-excluded on non-Chromebook PC" || fail "power: Power Button still excluded"
! grep -qE '"(0-7|4-7|ff|f0)"' "$NEWROOT/vendor/etc/powerhint.json" && grep -q '"0-3"' "$NEWROOT/vendor/etc/powerhint.json" \
  && ok "power: powerhint.json cpusets and masks clamped to 0-3 / f on 4-thread CPU" || fail "power: powerhint.json not clamped"
python3 - "$NEWROOT/vendor/etc/powerhint.json" <<'PYEOF' && ok "power: powerhint.json valid JSON with unique Values per Node (libperfmgr HintManager)" || fail "power: powerhint.json failed libperfmgr schema check"
import json, sys
d = json.load(open(sys.argv[1]))
nodes = {}
for n in d["Nodes"]:
    vals = n["Values"]
    assert len(vals) > 0 and len(vals) == len(set(vals)), f"duplicate/empty Values in {n['Name']}: {vals}"
    assert 0 <= n.get("DefaultIndex", 0) < len(vals)
    nodes[n["Name"]] = vals
for a in d["Actions"]:
    if a.get("Type") != "EndHint":
        assert a["Value"] in nodes[a["Node"]], f"Action value {a['Value']} missing from {nodes[a['Node']]}"
PYEOF
eq "power: mem_sleep set to s2idle" "$(cat "$SYSFS/power/mem_sleep")" "s2idle"
eq "power: SUSPEND_MODE=guard holds gbos_nos2idle wake_lock" "$(cat "$SYSFS/power/wake_lock")" "gbos_nos2idle"
eq "power: Power Button wakeup enabled" "$(cat "$SYSFS/bus/acpi/devices/PNP0C0C:00/power/wakeup")" "enabled"
eq "power: Lid Switch wakeup enabled" "$(cat "$SYSFS/bus/acpi/devices/PNP0C0D:00/power/wakeup")" "enabled"
eq "power: i8042 serio1 wakeup enabled" "$(cat "$SYSFS/devices/platform/i8042/serio1/power/wakeup")" "enabled"
eq "power: sony-laptop lid_resume_S3 enabled" "$(cat "$SYSFS/devices/platform/sony-laptop/lid_resume_S3")" "1"

# --- 4. sleep-guard.sh & optimize.sh selective USB autosuspend + on-battery lid shutdown helpers ---
grep -q '^LID_SHUTDOWN_MIN=60$' "$ROOT/usb/gbos.cfg" \
  && ok "gbos.cfg: LID_SHUTDOWN_MIN=60 default configured" || fail "gbos.cfg: LID_SHUTDOWN_MIN=60 missing"

# Extract is_on_battery and usb_hid_awake_restore from sleep-guard.sh and verify against fake sysfs
sed -n '/^  is_on_battery() {/,/^  }/p; /^  usb_hid_awake_restore() {/,/^  }/p' "$ROOT/usb/surface/sleep-guard.sh" \
  | sed "s#/sys/#$SYSFS/#g" > "$W/sg_fns.sh"
. "$W/sg_fns.sh"

# Case A: Desktop/VM with no battery -> NOT on battery
rm -rf "$SYSFS/class/power_supply"; mkdir -p "$SYSFS/class/power_supply"
! is_on_battery && ok "sleep-guard: desktop without BAT* is not treated as on-battery" || fail "sleep-guard: no-BAT treated as on-battery"

# Case B: Laptop on AC (Mains online=1, BAT0 Discharging/Full) -> NOT on battery
mkdir -p "$SYSFS/class/power_supply/AC" "$SYSFS/class/power_supply/BAT0"
echo Mains > "$SYSFS/class/power_supply/AC/type"; echo 1 > "$SYSFS/class/power_supply/AC/online"
echo Battery > "$SYSFS/class/power_supply/BAT0/type"; echo Full > "$SYSFS/class/power_supply/BAT0/status"
! is_on_battery && ok "sleep-guard: AC online=1 cancels on-battery shutdown" || fail "sleep-guard: AC online=1 treated as on-battery"

# Case C: Laptop unplugged (Mains online=0, BAT0 Discharging) -> ON battery
echo 0 > "$SYSFS/class/power_supply/AC/online"
echo Discharging > "$SYSFS/class/power_supply/BAT0/status"
is_on_battery && ok "sleep-guard: AC online=0 + BAT0 Discharging detected as on-battery" || fail "sleep-guard: unplugged BAT0 not detected"

# Case D: Selective USB autosuspend keeps mouse/touchscreen 'on', lets root hub, webcam & sensor hub 'auto'
rm -rf "$SYSFS/bus/usb/devices"; mkdir -p "$SYSFS/bus/usb/devices/usb1/power" \
  "$SYSFS/bus/usb/devices/1-2/power" "$SYSFS/bus/usb/devices/1-2:1.0" \
  "$SYSFS/bus/usb/devices/1-4/power" "$SYSFS/bus/usb/devices/1-4:1.0" \
  "$SYSFS/bus/usb/devices/1-5/power" "$SYSFS/bus/usb/devices/1-5:1.0/0003:0483:91D1.0001" \
  "$W/fake_drivers/usbhid" "$W/fake_drivers/uvcvideo" "$W/fake_drivers/hid-sensor-hub" "$W/fake_drivers/hid-ntrig"
ln -sf "$W/fake_drivers/uvcvideo" "$SYSFS/bus/usb/devices/1-2:1.0/driver"
ln -sf "$W/fake_drivers/usbhid" "$SYSFS/bus/usb/devices/1-4:1.0/driver"
mkdir -p "$SYSFS/bus/usb/devices/1-4:1.0/0003:1B96:1B05.0001"
ln -sf "$W/fake_drivers/hid-ntrig" "$SYSFS/bus/usb/devices/1-4:1.0/0003:1B96:1B05.0001/driver"
ln -sf "$W/fake_drivers/usbhid" "$SYSFS/bus/usb/devices/1-5:1.0/driver"
ln -sf "$W/fake_drivers/hid-sensor-hub" "$SYSFS/bus/usb/devices/1-5:1.0/0003:0483:91D1.0001/driver"
for p in usb1 1-2 1-4 1-5; do echo on > "$SYSFS/bus/usb/devices/$p/power/control"; done
usb_hid_awake_restore
eq "usb-pm: root hub usb1 set to auto" "$(cat "$SYSFS/bus/usb/devices/usb1/power/control")" "auto"
eq "usb-pm: UVC webcam 1-2 set to auto" "$(cat "$SYSFS/bus/usb/devices/1-2/power/control")" "auto"
eq "usb-pm: interactive HID 1-4 kept on while awake" "$(cat "$SYSFS/bus/usb/devices/1-4/power/control")" "on"
eq "usb-pm: hid-sensor-hub 1-5 set to auto" "$(cat "$SYSFS/bus/usb/devices/1-5/power/control")" "auto"

[ "$FAIL" = 0 ] || exit 1
echo "CAMERA_POWER_INPUT_TEST_PASS ($PASS checks)"
