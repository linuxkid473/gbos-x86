#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BRIDGE=$ROOT/usb/installer/bin/ipu6-camera-bridge.sh

sh -n "$BRIDGE"
grep -Fq 'LOOPBACK_LABEL=GBOS_IPU6_Camera' "$BRIDGE"
grep -Fq '"card_label=$LOOPBACK_LABEL"' "$BRIDGE"
grep -Fq 'video_nr=64 "card_label=$LOOPBACK_LABEL" exclusive_caps=1' "$BRIDGE"
grep -Fq 'if [ "$ACTUAL_LABEL" != "$LOOPBACK_LABEL" ]; then' "$BRIDGE"
grep -Fq 'ERROR: loopback label mismatch:' "$BRIDGE"
grep -Fq 'ERROR: /dev/video64 was not created after loading v4l2loopback' "$BRIDGE"
if grep -Fq 'card_label="GBOS IPU6 Camera"' "$BRIDGE"; then
    echo "loopback card label contains spaces and will be split by module option parsing" >&2
    exit 1
fi

LOOPBACK_LABEL=GBOS_IPU6_Camera
set -- video_nr=64 "card_label=$LOOPBACK_LABEL" exclusive_caps=1
[ "$#" -eq 3 ]
[ "$2" = card_label=GBOS_IPU6_Camera ]

# Rear camera: both loopback devices from one module load, labels without spaces.
grep -Fq 'REAR_LABEL=GBOS_IPU6_Rear_Camera' "$BRIDGE"
grep -Fq 'video_nr=64,65 "card_label=$LOOPBACK_LABEL,$REAR_LABEL" exclusive_caps=1,1' "$BRIDGE"
REAR_LABEL=GBOS_IPU6_Rear_Camera
set -- video_nr=64,65 "card_label=$LOOPBACK_LABEL,$REAR_LABEL" exclusive_caps=1,1
[ "$#" -eq 3 ]
[ "$2" = card_label=GBOS_IPU6_Camera,GBOS_IPU6_Rear_Camera ]
# The rear bridge runs the OV8856 profile on video65; the front command line is unchanged.
grep -Fq '"$BRIDGE" --sensor ov8856 --rotate "$REAR_ROTATION" /dev/media0 /dev/video65' "$BRIDGE"
grep -Fq '"$BRIDGE" /dev/media0 /dev/video64 ${CAL:+"$CAL"}' "$BRIDGE"
grep -Fq 'REAR_LOG=$LOGDIR/camera-bridge-rear-runtime.txt' "$BRIDGE"
# Order: the rear starts only after the front streams, and the HAL restart comes after both.
front_ready=$(grep -n 'wait_active "$LOGDIR/camera-bridge-runtime.txt" "$BRIDGE_PID"' "$BRIDGE" | cut -d: -f1)
rear_start=$(grep -n '"$BRIDGE" --sensor ov8856' "$BRIDGE" | head -n 1 | cut -d: -f1)
rear_ready=$(grep -n 'wait_active "$REAR_LOG" "$REAR_PID_RUN"' "$BRIDGE" | cut -d: -f1)
hal_restart=$(grep -n 'killall -9 android.hardware.camera.provider-usb-service' "$BRIDGE" | cut -d: -f1)
[ -n "$front_ready" ] && [ -n "$rear_start" ] && [ -n "$rear_ready" ] && [ -n "$hal_restart" ]
[ "$front_ready" -lt "$rear_start" ] && [ "$rear_start" -lt "$rear_ready" ] && [ "$rear_ready" -lt "$hal_restart" ]
# Supervision: a dead bridge is restarted (bounded), one bridge's exit never ends the other,
# and the script only ends once neither runs.
grep -Fq 'BRIDGE_RESTARTS=3' "$BRIDGE"
grep -Fq 'while [ -n "$BRIDGE_PID$REAR_PID_RUN" ]; do' "$BRIDGE"
[ "$(grep -c '>> "$LOGDIR/camera-bridge-runtime.txt" 2>&1 &\|>> "$REAR_LOG" 2>&1 &' "$BRIDGE")" = 2 ]
if sed -n '/^while \[ -n "$BRIDGE_PID$REAR_PID_RUN" \]; do/,/^done/p' "$BRIDGE" | grep -Eq '(^|[^a-z])exit( |$)|killall|kill [^-]'; then
    echo "bridge supervision must not exit or kill" >&2
    exit 1
fi
# A rear failure is a warning, never an exit: the front keeps running.
grep -Fq 'WARNING: rear bridge did not reach streaming state; front camera only' "$BRIDGE"
if sed -n "${rear_start},${hal_restart}p" "$BRIDGE" | grep -q 'exit'; then
    echo "rear camera setup must not exit the launcher" >&2
    exit 1
fi
grep -Fq '[ "$(cfg CAMERA_REAR)" = 0 ] && REAR=0' "$BRIDGE"
# The front keeps its USB identity (Google lists 0408:4052 as FACING_FRONT).
grep -Fq 'expose_uvc 64 /postinstall/gbos/surface/latitude_fake_uvc_desc.bin "$FAKE_DIR" 0408 4052' "$BRIDGE"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Pull the launcher's own helper functions out and run them here.
for fn in strip_zeros uvc_uevent cfg add_rear_facing; do
    # A function ends at the first "}" line outside a here-document (the facing entry has some).
    awk -v fn="$fn" '$0 ~ "^" fn "\\(\\) \\{" { on = 1 }
        on { print }
        on && /<<.EOF.$/ { doc = 1; next }
        doc && /^EOF$/ { doc = 0; next }
        on && !doc && /^}/ { exit }' "$BRIDGE"
done > "$tmp/functions.sh"
[ "$(grep -c '^[a-z_]*() {' "$tmp/functions.sh")" -eq 4 ]
. "$tmp/functions.sh"

# Front uevent: byte-identical to the one the launcher wrote before the rear camera existed.
cat > "$tmp/front.expected" <<'EOF'
DRIVER=uvcvideo
PRODUCT=408/4052/100
TYPE=239/2/1
INTERFACE=14/1/0
MODALIAS=usb:v0408p4052d0100dcEFdsc02dp01ic0Eisc01ip00in00
EOF
uvc_uevent 0408 4052 > "$tmp/front.uevent"
cmp -s "$tmp/front.expected" "$tmp/front.uevent"
eval "$(grep -E '^REAR_USB_(VID|PID)=' "$BRIDGE")"
[ "$REAR_USB_VID:$REAR_USB_PID" = 1209:0001 ]
uvc_uevent "$REAR_USB_VID" "$REAR_USB_PID" > "$tmp/rear.uevent"
grep -qx 'PRODUCT=1209/1/100' "$tmp/rear.uevent"
grep -qx 'MODALIAS=usb:v1209p0001d0100dcEFdsc02dp01ic0Eisc01ip00in00' "$tmp/rear.uevent"
[ "$(strip_zeros 0000)" = 0 ] && [ "$(strip_zeros 0abc)" = abc ]

# Rear descriptor: the front's with only idVendor/idProduct replaced by the rear id.
FRONT_DESC=$ROOT/usb/surface/latitude_fake_uvc_desc.bin
REAR_DESC=$ROOT/usb/surface/latitude_rear_fake_uvc_desc.bin
[ "$(od -An -tx1 -j8 -N4 "$FRONT_DESC" | tr -d ' \n')" = 08045240 ]
[ "$(od -An -tx1 -j8 -N4 "$REAR_DESC" | tr -d ' \n')" = 09120100 ]
[ "$(cmp -l "$FRONT_DESC" "$REAR_DESC" | awk '$1 < 9 || $1 > 12' | wc -l)" -eq 0 ]
[ "$(wc -c < "$FRONT_DESC")" -eq "$(wc -c < "$REAR_DESC")" ]

# Facing: a copy of the HAL's camera table gets one FACING_BACK entry for the rear id.
cat > "$tmp/info.txtpb" <<'EOF'
use_hardware_jpeg_decoder: true
camera_modules {
  key: "0408:4052" # synthetic front entry
  value {
    facing: FACING_FRONT
  }
}
EOF
cp "$tmp/info.txtpb" "$tmp/info.orig"
CAMERA_INFO=$tmp/info.txtpb
REAR_FAKE=$tmp/rear
mkdir -p "$REAR_FAKE"
mount() { return 1; }	# no bind mounts here: add_rear_facing reports and returns 1
chcon() { return 0; }
grep() { if [ "$1" = -q ] && [ "$3" = /proc/mounts ]; then return 1; fi; command grep "$@"; }
add_rear_facing > "$tmp/facing.log" 2>&1 && exit 1
unset -f grep
merged=$REAR_FAKE/internal_usb_camera_info.txtpb
head -n "$(wc -l < "$tmp/info.orig")" "$merged" | cmp -s - "$tmp/info.orig"
[ "$(grep -c '^camera_modules {' "$merged")" -eq 2 ]
grep -Fq "key: \"$REAR_USB_VID:$REAR_USB_PID\"" "$merged"
[ "$(grep -c 'facing: FACING_BACK' "$merged")" -eq 1 ]
[ "$(grep -c 'facing: FACING_FRONT' "$merged")" -eq 1 ]
grep -Fq 'available_focal_lengths: 2.83' "$merged"
opens=$(tr -cd '{' < "$merged" | wc -c)
closes=$(tr -cd '}' < "$merged" | wc -c)
[ "$opens" -eq "$closes" ]
grep -q 'could not bind-mount' "$tmp/facing.log"
# A table that already lists the rear id is left alone (no duplicate key for the HAL to reject).
cp "$merged" "$tmp/info.txtpb"
rm -f "$merged"
add_rear_facing > "$tmp/facing2.log" 2>&1
grep -q 'already lists 1209:0001' "$tmp/facing2.log"
[ ! -e "$merged" ]

# gbos.cfg keys are read one at a time; the last value wins and CRs are dropped.
printf 'CAMERA_REAR=1\r\nCAMERA_REAR=0\r\nCAMERA_REAR_ROTATION=180\n' > "$tmp/gbos.cfg"
sed() { if [ "$#" -eq 3 ] && [ "$3" = /postinstall/gbos/gbos.cfg ]; then command sed "$1" "$2" "$tmp/gbos.cfg"; else command sed "$@"; fi; }
[ "$(cfg CAMERA_REAR)" = 0 ]
[ "$(cfg CAMERA_REAR_ROTATION)" = 180 ]
[ -z "$(cfg CAMERA_MISSING)" ]
unset -f sed

echo IPU6_CAMERA_BRIDGE_TEST_PASS
