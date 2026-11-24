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
grep -Fq '"$REAR_BRIDGE" --sensor ov8856 --rotate "$REAR_ROTATION" /dev/media0 "$REAR_OUT"' "$BRIDGE"
grep -Fq '"$BRIDGE" /dev/media0 "$FRONT_OUT" ${CAL:+"$CAL"}' "$BRIDGE"
grep -Fq 'REAR_LOG=$LOGDIR/camera-bridge-rear-runtime.txt' "$BRIDGE"
# Order: the rear starts only after the front streams, and the HAL restart comes after both.
front_ready=$(grep -n 'wait_active "$LOGDIR/camera-bridge-runtime.txt" "$BRIDGE_PID"' "$BRIDGE" | tail -n 1 | cut -d: -f1)
rear_start=$(grep -n '"$REAR_BRIDGE" --sensor ov8856' "$BRIDGE" | head -n 1 | cut -d: -f1)
rear_ready=$(grep -n 'wait_active "$REAR_LOG" "$REAR_PID_RUN"' "$BRIDGE" | cut -d: -f1)
hal_restart=$(grep -n 'killall -9 android.hardware.camera.provider-usb-service' "$BRIDGE" | cut -d: -f1)
[ -n "$front_ready" ] && [ -n "$rear_start" ] && [ -n "$rear_ready" ] && [ -n "$hal_restart" ]
[ "$front_ready" -lt "$rear_start" ] && [ "$rear_start" -lt "$rear_ready" ] && [ "$rear_ready" -lt "$hal_restart" ]
# Supervision: a dead bridge is restarted (bounded), one bridge's exit never ends the other,
# and the script only ends once neither runs.
grep -Fq 'BRIDGE_RESTARTS=3' "$BRIDGE"
grep -Fq 'while [ -n "$BRIDGE_PID$REAR_PID_RUN" ]; do' "$BRIDGE"
# (front and rear restarts, and the front's CPU fallback when the GPU bridge doesn't start)
[ "$(grep -c '>> "$LOGDIR/camera-bridge-runtime.txt" 2>&1 &\|>> "$REAR_LOG" 2>&1 &' "$BRIDGE")" = 3 ]
if sed -n '/^while \[ -n "$BRIDGE_PID$REAR_PID_RUN" \]; do/,/^done/p' "$BRIDGE" | grep -Eq '(^|[^a-z])exit( |$)|killall|kill [^-]'; then
    echo "bridge supervision must not exit or kill" >&2
    exit 1
fi
# Idle stop: the bridges stop their sensors themselves and keep running, so the supervisor
# never sees an idle stop; CAMERA_IDLE_STOP reaches them as IPU6_IDLE_STOP.
grep -Fq 'IDLE_STOP=$(cfg CAMERA_IDLE_STOP)' "$BRIDGE"
grep -Fq 'export IPU6_IDLE_STOP="$IDLE_STOP"' "$BRIDGE"
grep -Fq 'BRIDGE_HEALTHY_S=300' "$BRIDGE"
# A bridge that ran long before exiting gets its restarts back (one check per sensor).
[ "$(sed -n '/^while \[ -n "$BRIDGE_PID$REAR_PID_RUN" \]; do/,/^done/p' "$BRIDGE" |
    grep -c 'if ran_long "$\(FRONT\|REAR\)_SINCE"; then')" = 2 ]
[ "$(grep -c '_SINCE=$(date +%s)$' "$BRIDGE")" = 3 ]
for who in FRONT REAR; do
    grep -A1 "if ran_long \"\$${who}_SINCE\"; then" "$BRIDGE" | tail -n 1 | grep -Eq "^ +${who}_RESTARTS=0$"
done
# A rear failure is a warning, never an exit: the front keeps running.
grep -Fq 'WARNING: rear bridge did not reach streaming state; front camera only' "$BRIDGE"
if sed -n "${rear_start},${hal_restart}p" "$BRIDGE" | grep -q 'exit'; then
    echo "rear camera setup must not exit the launcher" >&2
    exit 1
fi
grep -Fq '[ "$(cfg CAMERA_REAR)" = 0 ] && REAR=0' "$BRIDGE"
# With the libcamera plugins on, the USB rear stays off (its public ID 1 clashed on boot-17).
rear_block=$(awk '/^REAR=1$/ { on = 1 } on { print } on && /^fi$/ { exit }' "$BRIDGE")
rear_with() {	# CAMERA_REAR, CAMERA_LIBCAMERA -> "REAR LC_REAR"
    (
        cfg() { case "$1" in CAMERA_REAR) printf '%s' "$c_rear" ;; CAMERA_LIBCAMERA) printf '%s' "$c_lc" ;; esac; }
        c_rear=$1 c_lc=$2
        eval "$rear_block" >/dev/null
        echo "$REAR $LC_REAR"
    )
}
# CAMERA_LIBCAMERA=1: the rear moves to the libcamera plugin; =test: the test camera replaces it.
[ "$(rear_with '' '')" = "1 0" ] && [ "$(rear_with '' 1)" = "0 1" ] && [ "$(rear_with 0 '')" = "0 0" ] &&
    [ "$(rear_with '' 0)" = "1 0" ] && [ "$(rear_with '' test)" = "0 0" ] && [ "$(rear_with 0 1)" = "0 0" ] &&
    [ "$(rear_with '' both)" = "0 1" ]

# CAMERA_LIBCAMERA=both: the front moves to the libcamera plugin too (frame channel camera0, the
# 1920x1440 builds); any other value keeps it on v4l2loopback.
front_block=$(awk '/^# --- libcamera front/ { on = 1; next } /^# --- end of libcamera front/ { exit } on { print }' "$BRIDGE")
front_with() {	# CAMERA_LIBCAMERA -> "LC_FRONT FRONT_OUT SIZE_DIR"
    (
        cfg() { [ "$1" = CAMERA_LIBCAMERA ] && printf '%s' "$c_lc"; }
        c_lc=$1 SIZE_DIR=/1280x720
        eval "$front_block" >/dev/null
        echo "$LC_FRONT $FRONT_OUT $SIZE_DIR"
    )
}
[ "$(front_with '')" = "0 /dev/video64 /1280x720" ] && [ "$(front_with 1)" = "0 /dev/video64 /1280x720" ] &&
    [ "$(front_with test)" = "0 /dev/video64 /1280x720" ] && [ "$(front_with both)" = "1 channel:camera0 /1920x1440" ]
# No loopback module and no front UVC identity with both on libcamera.
grep -q '^if \[ "$LC_FRONT" != 1 \]; then$' "$BRIDGE"
[ "$(grep -c '^if \[ "$LC_FRONT" != 1 \]; then$' "$BRIDGE")" = 2 ]
loop_line=$(grep -n 'insmod /postinstall/gbos/kmod/v4l2loopback.ko' "$BRIDGE" | cut -d: -f1)
guard_line=$(grep -n '^if \[ "$LC_FRONT" != 1 \]; then$' "$BRIDGE" | head -n 1 | cut -d: -f1)
[ "$guard_line" -lt "$loop_line" ]
# The USB fallback: after the libcamera cameras failed, CAMERA_LIBCAMERA reads as 0 and the script
# runs itself again; the bridges are stopped first.
cfg_fn=$(awk '/^cfg\(\) \{/ { on = 1 } on { print } on && /^}/ { exit }' "$BRIDGE")
[ "$(GBOS_CAMERA_USB_FALLBACK=1 sh -c "$cfg_fn"'; cfg CAMERA_LIBCAMERA')" = 0 ]
grep -Fq 'GBOS_CAMERA_USB_FALLBACK=1 exec /system/bin/sh "$0" "$BOOT_ID"' "$BRIDGE"
grep -Fq 'if ! add_libcamera_plugins && [ "$LC_FRONT" = 1 ]; then' "$BRIDGE"
fallback=$(awk '/^if ! add_libcamera_plugins/ { on = 1 } on { print } on && /^fi$/ { exit }' "$BRIDGE")
[ "$(echo "$fallback" | grep -n 'kill "$pid"' | cut -d: -f1)" -lt "$(echo "$fallback" | grep -n 'exec /system/bin/sh' | cut -d: -f1)" ]

# Rear bridge choice: the loopback rear uses the front's build; the libcamera rear the 1920x1440
# builds with the frame channel output, on the GPU when the front is, and not at all without them.
rear_choice=$(awk '/^# --- rear bridge choice/ { on = 1; next } /^# --- end of rear bridge choice/ { exit } on { print }' "$BRIDGE")
echo "$rear_choice" | grep -q '/postinstall/gbos/installer/bin/1920x1440/vulkan/ipu6-loopback'
rear_pick() {	# LC_REAR, front BRIDGE (cpu|gpu), 1920x1440 builds present (1|0) -> "REAR_BRIDGE REAR_OUT LC_REAR"
    (
        d=$(mktemp -d)
        mkdir -p "$d/1920x1440/vulkan"
        if [ "$3" = 1 ]; then
            printf '#!/bin/sh\n' > "$d/1920x1440/ipu6-loopback"
            cp "$d/1920x1440/ipu6-loopback" "$d/1920x1440/vulkan/ipu6-loopback"
            chmod +x "$d/1920x1440/ipu6-loopback" "$d/1920x1440/vulkan/ipu6-loopback"
        fi
        LC_REAR=$1 BRIDGE_CPU=/cpu BRIDGE_GPU=/gpu
        BRIDGE=/$2
        eval "$(echo "$rear_choice" | sed "s#/postinstall/gbos/installer/bin#$d#g")" >/dev/null
        echo "$REAR_BRIDGE $REAR_OUT $LC_REAR" | sed "s#$d#D#g"
        rm -rf "$d"
    )
}
[ "$(rear_pick 0 cpu 1)" = "/cpu /dev/video65 0" ]
[ "$(rear_pick 1 cpu 1)" = "D/1920x1440/ipu6-loopback channel:camera1 1" ]
[ "$(rear_pick 1 gpu 1)" = "D/1920x1440/vulkan/ipu6-loopback channel:camera1 1" ]
[ "$(rear_pick 1 gpu 0 | cut -d' ' -f3)" = 0 ]
# The front keeps its USB identity (Google lists 0408:4052 as FACING_FRONT).
grep -Fq 'expose_uvc 64 "$FRONT_DESC" "$FAKE_DIR" 0408 4052' "$BRIDGE"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Pull the launcher's own helper functions out and run them here.
for fn in strip_zeros uvc_uevent cfg add_rear_facing ran_long; do
    # A function ends at the first "}" line outside a here-document (the facing entry has some).
    awk -v fn="$fn" '$0 ~ "^" fn "\\(\\) \\{" { on = 1 }
        on { print }
        on && /<<.EOF.$/ { doc = 1; next }
        doc && /^EOF$/ { doc = 0; next }
        on && !doc && /^}/ { exit }' "$BRIDGE"
done > "$tmp/functions.sh"
[ "$(grep -c '^[a-z_]*() {' "$tmp/functions.sh")" -eq 5 ]
. "$tmp/functions.sh"

# ran_long: 300 s or more since the start counts as healthy; less does not.
eval "$(grep -E '^BRIDGE_HEALTHY_S=' "$BRIDGE")"
now=$(date +%s)
ran_long $((now - 301))
if ran_long $((now - 20)); then
    echo "a bridge that ran 20 s must not get its restarts back" >&2
    exit 1
fi

# The idle-stop setting: digits are exported; empty or anything else means 0 (off), because the first idle
# stop on boot-13 left the bridge unable to restart.
idle_block=$(awk '/^IDLE_STOP=\$\(cfg CAMERA_IDLE_STOP\)/ { on = 1 } on { print } on && /^export IPU6_IDLE_STOP=/ { exit }' "$BRIDGE")
for value in 0 30 '' 1x; do
    out=$(cfg() { printf '%s' "$value"; }; unset IPU6_IDLE_STOP; eval "$idle_block"; echo "[${IPU6_IDLE_STOP-unset}]")
    case "$value" in
        0|30) echo "$out" | grep -qx "\[$value\]" ;;
        *) echo "$out" | grep -qx '\[0\]' ;;
    esac
done

# Supervisor, run for real with a fake bridge: a failed idle stop (status 4) restarts that bridge at once
# with --idle-stop 0 without using up a restart; later exits (even 4 again) are ordinary restarts, so the
# loop still ends after BRIDGE_RESTARTS.
supervisor=$(awk '/^BRIDGE_RESTARTS=/ { on = 1 } on { print } on && /^exit "\$status"$/ { exit }' "$BRIDGE")
echo "$supervisor" | grep -q 'EXIT_IDLE_FAILED=4'
printf '#!/bin/sh\necho "$*" >> "$FAKE_ARGS"\nexit 4\n' > "$tmp/fake-bridge"
chmod +x "$tmp/fake-bridge"
sup_out=$(
    set +e  # as in the launcher (set -u only)
    sleep() { :; }
    FAKE_ARGS=$tmp/args; export FAKE_ARGS
    BRIDGE=$tmp/fake-bridge LOGDIR=$tmp REAR_LOG=$tmp/rear.log CAL= REAR_ROTATION=0 IDLE_STOP=15 FRONT_OUT=/dev/video64
    BRIDGE_CPU=$tmp/fake-bridge BRIDGE_GPU=$tmp/no-gpu-build
    sh -c 'exit 4' & BRIDGE_PID=$!
    REAR_PID_RUN=
    eval "$supervisor"
) || true
[ "$(echo "$sup_out" | grep -c 'front idle stop failed; restarted front bridge pid=[0-9]* with idle stop off for this boot')" = 1 ]
echo "$sup_out" | grep -q 'ERROR: front bridge out of restarts'
[ "$(wc -l < "$tmp/args")" = 4 ]
[ "$(grep -c '^--idle-stop 0 /dev/media0 /dev/video64' "$tmp/args")" = 4 ]
# A GPU bridge that exits is restarted with the CPU build.
printf '#!/bin/sh\necho "GPU $*" >> "$FAKE_ARGS"\nexit 1\n' > "$tmp/fake-gpu"
chmod +x "$tmp/fake-gpu"
: > "$tmp/args"
sup_out=$(
    set +e
    sleep() { :; }
    FAKE_ARGS=$tmp/args; export FAKE_ARGS
    LOGDIR=$tmp REAR_LOG=$tmp/rear.log CAL= REAR_ROTATION=0 IDLE_STOP=0 FRONT_OUT=/dev/video64
    BRIDGE_CPU=$tmp/fake-bridge BRIDGE_GPU=$tmp/fake-gpu BRIDGE=$tmp/fake-gpu
    sh -c 'exit 1' & BRIDGE_PID=$!
    REAR_PID_RUN=
    eval "$supervisor"
) || true
echo "$sup_out" | grep -q 'GPU bridge exited; restarts use the CPU converter'
! grep -q '^GPU' "$tmp/args"
[ "$(grep -c '^--idle-stop 0 /dev/media0 /dev/video64' "$tmp/args")" = 3 ]
# The rear gets its own switch; the front's setting is untouched by it.
grep -Fq '"$REAR_BRIDGE" --sensor ov8856 --rotate "$REAR_ROTATION" --idle-stop "$REAR_IDLE_STOP" /dev/media0 "$REAR_OUT"' "$BRIDGE"
# A rear GPU bridge that exits is restarted with its CPU build, and keeps its output (here the
# libcamera frame channel); the front's choice is not touched.
: > "$tmp/args"
sup_out=$(
    set +e
    sleep() { :; }
    FAKE_ARGS=$tmp/args; export FAKE_ARGS
    LOGDIR=$tmp REAR_LOG=$tmp/rear.log CAL= REAR_ROTATION=0 IDLE_STOP=0 FRONT_OUT=/dev/video64
    BRIDGE_CPU=$tmp/fake-bridge BRIDGE_GPU=$tmp/fake-gpu BRIDGE=$tmp/fake-gpu
    REAR_BRIDGE_CPU=$tmp/fake-bridge REAR_BRIDGE_GPU=$tmp/fake-gpu REAR_BRIDGE=$tmp/fake-gpu
    REAR_OUT=channel:camera1
    BRIDGE_PID=
    sh -c 'exit 1' & REAR_PID_RUN=$!
    eval "$supervisor"
) || true
echo "$sup_out" | grep -q 'rear GPU bridge exited; restarts use the CPU converter'
[ "$(echo "$sup_out" | grep -c 'GPU bridge exited')" = 1 ]
! grep -q '^GPU' "$tmp/args"
[ "$(grep -c '^--sensor ov8856 --rotate 0 --idle-stop 0 /dev/media0 channel:camera1$' "$tmp/args")" = 3 ]
grep -Fq 'REAR_IDLE_STOP=0' "$BRIDGE"

# Converter choice, run for real: the GPU build only when --probe-gpu succeeds and CAMERA_GPU is
# not 0; otherwise (missing, failing probe) the static CPU build.
choice=$(awk '/^# --- converter choice/ { on = 1; next } /^# --- end of converter choice/ { exit } on { print }' "$BRIDGE")
echo "$choice" | grep -q 'probe=$(timeout 10 "$BRIDGE_GPU" --probe-gpu 2>&1)'
printf '#!/bin/sh\necho "gpu: Vulkan compute on Fake GPU"\n' > "$tmp/gpu-ok"
printf '#!/bin/sh\necho "gpu: no libvulkan" >&2\nexit 1\n' > "$tmp/gpu-bad"
chmod +x "$tmp/gpu-ok" "$tmp/gpu-bad"
choose() {	# CAMERA_GPU value, GPU binary -> chosen binary
    (
        cfg() { [ "$1" = CAMERA_GPU ] && printf '%s' "$cfg_gpu"; }
        cfg_gpu=$1 BRIDGE_CPU=/cpu BRIDGE_GPU=$2
        eval "$choice" >/dev/null
        echo "$BRIDGE"
    )
}
[ "$(choose '' "$tmp/gpu-ok")" = "$tmp/gpu-ok" ]
[ "$(choose 1 "$tmp/gpu-ok")" = "$tmp/gpu-ok" ]
[ "$(choose 0 "$tmp/gpu-ok")" = /cpu ]
[ "$(choose '' "$tmp/gpu-bad")" = /cpu ]
[ "$(choose '' "$tmp/missing")" = /cpu ]
# A GPU bridge that fails to start, or exits later, is replaced by the CPU build.
grep -Fq 'WARNING: GPU bridge did not reach streaming state; using the CPU converter' "$BRIDGE"
[ "$(grep -c 'GPU bridge exited; restarts use the CPU converter' "$BRIDGE")" = 2 ]

# libcamera plugins: the vendor pipeline directory is copied with our plugins added, the copy
# bind-mounted over it and the legacy-wrapper service restarted; CAMERA_LIBCAMERA=0 or no
# plugins leave it alone. Run with stubs for mount/chcon/stop/start.
plugin_fn=$(awk '/^(add_libcamera_plugins|libcamera_service_stable)\(\) \{/ { on = 1 } on { print } on && /^}/ { on = 0 }' "$BRIDGE")
[ "$(echo "$plugin_fn" | grep -c '^[a-z_]*() {')" = 2 ]
echo "$plugin_fn" | grep -q 'mount -o bind "$LIBCAMERA_COPY" "$LIBCAMERA_PIPE"'
[ "$(grep -n '^if ! add_libcamera_plugins' "$BRIDGE" | cut -d: -f1)" -lt "$(grep -n 'killall -9 cameraserver' "$BRIDGE" | cut -d: -f1)" ]
mkdir -p "$tmp/pipe" "$tmp/plugins"
echo google > "$tmp/pipe/camera.libipu7_hal_fatcat.so"
echo ours > "$tmp/plugins/gbos_test.so"
echo ipu6 > "$tmp/plugins/gbos_ipu6.so"
run_plugins() {	# CAMERA_LIBCAMERA value, service behaviour (stable|crash|dead) -> calls made
    (
        cfg() { [ "$1" = CAMERA_LIBCAMERA ] && printf '%s' "$cfg_lc"; }
        mount() { echo "mount $*"; }
        umount() { echo "umount $*"; }
        chcon() { :; }
        stop() { echo "stop $*"; }
        start() { echo "start $*"; }
        sleep() { :; }
        pidof() {	# a crash loop gives a new pid at every look
            case "$behaviour" in
            stable) echo 4242 ;;
            crash) n=$(( $(cat "$tmp/pidn" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$tmp/pidn"; echo "50$n" ;;
            *) return 1 ;;
            esac
        }
        cfg_lc=$1 behaviour=${2:-stable}
        LIBCAMERA_PIPE=$tmp/pipe LIBCAMERA_PLUGINS=$tmp/plugins LIBCAMERA_COPY=$tmp/copy
        LIBCAMERA_SERVICE=vendor.camera-provider-2-7-legacy-wrapper
        LIBCAMERA_PROCESS=android.hardware.camera.provider@2.7-service-legacy-wrapper
        LIBCAMERA_SETTLE_S=3 LIBCAMERA_WATCH_S=8
        eval "$plugin_fn"
        if add_libcamera_plugins; then echo "status 0"; else echo "status 1"; fi
    )
}
# Off by default since boot-16's crash loop: only CAMERA_LIBCAMERA=1 adds the plugins.
calls=$(run_plugins '')
! echo "$calls" | grep -q '^mount\|^stop\|^start'
calls=$(run_plugins 1 stable)
echo "$calls" | grep -qx "mount -o bind $tmp/copy $tmp/pipe"
echo "$calls" | grep -qx 'stop vendor.camera-provider-2-7-legacy-wrapper'
echo "$calls" | grep -qx 'start vendor.camera-provider-2-7-legacy-wrapper'
echo "$calls" | grep -q 'stable (pid 4242)'
echo "$calls" | grep -qx 'status 0'
! echo "$calls" | grep -q '^umount'
# =1 adds only the IPU6 plugin, =test only the test camera (both are "camera1").
[ "$(cat "$tmp/copy/camera.libipu7_hal_fatcat.so")" = google ] && [ "$(cat "$tmp/copy/gbos_ipu6.so")" = ipu6 ] &&
    [ ! -e "$tmp/copy/gbos_test.so" ]
rm -rf "$tmp/copy"
rm -rf "$tmp/copy"
calls=$(run_plugins both stable)
echo "$calls" | grep -qx 'status 0'
[ "$(cat "$tmp/copy/gbos_ipu6.so")" = ipu6 ] && [ ! -e "$tmp/copy/gbos_test.so" ]
rm -rf "$tmp/copy"
calls=$(run_plugins test stable)
echo "$calls" | grep -qx 'status 0'
[ "$(cat "$tmp/copy/gbos_test.so")" = ours ] && [ ! -e "$tmp/copy/gbos_ipu6.so" ]
# A crash loop (new pid at every look) or a dead service: the overlay is removed and the
# service restarted as shipped (boot-16).
for behaviour in crash dead; do
    rm -rf "$tmp/copy" "$tmp/pidn"
    calls=$(run_plugins 1 "$behaviour")
    echo "$calls" | grep -qx "umount $tmp/pipe"
    [ "$(echo "$calls" | grep -c '^start vendor.camera-provider-2-7-legacy-wrapper')" = 2 ]
    echo "$calls" | grep -q 'WARNING: vendor.camera-provider-2-7-legacy-wrapper crashed'
    echo "$calls" | grep -qx 'status 1'
done
calls=$(run_plugins 0)
! echo "$calls" | grep -q '^mount\|^stop\|^start'
rm -f "$tmp/plugins/gbos_ipu6.so"
calls=$(run_plugins 1)
! echo "$calls" | grep -q '^mount\|^stop\|^start'
echo "$calls" | grep -qx 'status 1'	# a missing plugin is a failure (the both-cameras fallback needs it)

# Output size: CAMERA_SIZE picks bridge builds, loopback bounds and descriptors together; the
# files it names exist in usb/.
size_block=$(awk '/^# --- output size/ { on = 1; next } /^# --- end of output size/ { exit } on { print }' "$BRIDGE")
size_of() {	# CAMERA_SIZE value -> "W H SIZE_DIR FRONT_DESC REAR_DESC"
    (
        cfg() { [ "$1" = CAMERA_SIZE ] && printf '%s' "$cfg_size"; }
        cfg_size=$1
        eval "$size_block" >/dev/null
        echo "$OUT_W $OUT_H $SIZE_DIR $FRONT_DESC $REAR_DESC"
    )
}
S=/postinstall/gbos/surface
[ "$(size_of '')" = "1920 1080  $S/latitude_fake_uvc_desc.bin $S/latitude_rear_fake_uvc_desc.bin" ]
[ "$(size_of 1920x1080)" = "$(size_of '')" ]
[ "$(size_of 1280x720)" = "1280 720 /1280x720 $S/latitude_fake_uvc_desc-1280x720.bin $S/latitude_rear_fake_uvc_desc-1280x720.bin" ]
[ "$(size_of 640x480)" = "640 480 /640x480 $S/latitude_fake_uvc_desc-640x480.bin $S/latitude_rear_fake_uvc_desc-640x480.bin" ]
# 1280x960 (no CameraX video quality: the app crashed on boot-18) and anything else: the default.
[ "$(size_of 1280x960)" = "$(size_of '')" ]
[ "$(size_of 800x600)" = "$(size_of '')" ]
grep -Fq 'BRIDGE_CPU=/postinstall/gbos/installer/bin$SIZE_DIR/ipu6-loopback' "$BRIDGE"
grep -Fq 'BRIDGE_GPU=/postinstall/gbos/installer/bin$SIZE_DIR/vulkan/ipu6-loopback' "$BRIDGE"
for size in 1280x720 640x480; do
    for f in installer/bin/$size/ipu6-loopback installer/bin/$size/vulkan/ipu6-loopback; do
        [ -x "$ROOT/usb/$f" ]
    done
    [ -s "$ROOT/usb/surface/latitude_fake_uvc_desc-$size.bin" ] && [ -s "$ROOT/usb/surface/latitude_rear_fake_uvc_desc-$size.bin" ]
done

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
