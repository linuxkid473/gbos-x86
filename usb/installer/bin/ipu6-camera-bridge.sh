#!/system/bin/sh
# Latitude 7320 Detachable IPU6 camera bridges: front OV5678 (ov5675 driver) on /dev/video64 and
# rear OV8856 on /dev/video65. Each sensor has its own ipu6-loopback process that converts raw
# frames to 1280x960 YUYV on a v4l2loopback device, exposed to Android's USB camera HAL through a
# fake UVC sysfs identity. The front is required; the rear is optional (CAMERA_REAR=0 in gbos.cfg
# turns it off) and starts only once the front streams, so a rear failure never costs the front.
set -u

BOOT_ID=${1:-unknown}
case "$BOOT_ID" in
    ''|*[!0-9]*) BOOT_ID=unknown ;;
esac
LOGDIR=/postinstall/gbos/logs/boot-$BOOT_ID
mkdir -p "$LOGDIR" || exit 1
LOG=$LOGDIR/camera-bridge-setup.txt
exec >> "$LOG" 2>&1
LOOPBACK_LABEL=GBOS_IPU6_Camera
REAR_LABEL=GBOS_IPU6_Rear_Camera
# Rear USB identity (pid.codes test id, never a real camera). The front keeps Quanta 0408:4052,
# which Google's internal_usb_camera_info.txtpb lists as FACING_FRONT; the rear entry is added below.
REAR_USB_VID=1209
REAR_USB_PID=0001
REAR_DESC=/postinstall/gbos/surface/latitude_rear_fake_uvc_desc.bin
CAMERA_INFO=/vendor/etc/camera/internal_usb_camera_info.txtpb
echo "=== Latitude 7320 IPU6 camera bridge $(date) ==="

if ! grep -qx 'Latitude 7320 Detachable' /sys/class/dmi/id/product_name 2>/dev/null; then
    echo "Not Latitude 7320 Detachable; refusing to start"
    exit 0
fi
if [ ! -s /postinstall/gbos/surface/latitude_fake_uvc_desc.bin ]; then
    echo "ERROR: missing UVC descriptors"
    exit 1
fi

# gbos.cfg is read for a few keys only (sourcing it would let its names clash with this script's).
cfg() {
    sed -n "s/^$1=//p" /postinstall/gbos/gbos.cfg 2>/dev/null | tail -n 1 | tr -d '\r'
}
REAR=1
[ "$(cfg CAMERA_REAR)" = 0 ] && REAR=0
REAR_ROTATION=$(cfg CAMERA_REAR_ROTATION)
case "$REAR_ROTATION" in
    ''|0) REAR_ROTATION=0 ;;
    180) ;;
    *) echo "WARNING: CAMERA_REAR_ROTATION=$REAR_ROTATION is not 0 or 180; using 0"; REAR_ROTATION=0 ;;
esac
if [ "$REAR" = 1 ] && [ ! -s "$REAR_DESC" ]; then
    echo "WARNING: missing rear UVC descriptor $REAR_DESC; rear camera off"
    REAR=0
fi
echo "rear camera: $([ "$REAR" = 1 ] && echo "on, rotation $REAR_ROTATION" || echo off)"
# Idle stop: each bridge stops its sensor stream after this many seconds without a consumer
# (the ISYS refuses system suspend while a stream is open) and restarts it when an app streams.
# The bridge stays running meanwhile; only a failed stop or restart exits (status 4, see the supervisor).
# CAMERA_IDLE_STOP=0 keeps both sensors streaming as before; CAMERA_IDLE_STOP=15 is the bridge's own default.
# Off unless set: on boot-13 (2026-11-19) the first idle stop hit "stream stop/close time out" in the ISYS and
# the bridge never streamed again, so no app could open a camera. Until a restart works on hardware, empty or
# invalid values mean 0 (never stop; system suspend stays blocked while the cameras stream).
IDLE_STOP=$(cfg CAMERA_IDLE_STOP)
case "$IDLE_STOP" in
    '') IDLE_STOP=0; echo "camera idle stop: off (set CAMERA_IDLE_STOP=15 in gbos.cfg to test it)" ;;
    *[!0-9]*) echo "WARNING: CAMERA_IDLE_STOP=$IDLE_STOP is not a number of seconds; idle stop off"; IDLE_STOP=0 ;;
    *) echo "camera idle stop: $IDLE_STOP s (0 = never)" ;;
esac
export IPU6_IDLE_STOP="$IDLE_STOP"

if [ ! -e /dev/video64 ]; then
    # One module load creates both devices; the bounds are module-wide. Min = max = 1280x960 (the
    # bridge's OUT_WIDTH x OUT_HEIGHT): the HAL takes the largest size v4l2loopback lists, so it
    # must list only the one the bridge produces (the module's list has 1280x960 since 2026-11-20;
    # with an older module nothing would match).
    if [ "$REAR" = 1 ]; then
        set -- video_nr=64,65 "card_label=$LOOPBACK_LABEL,$REAR_LABEL" exclusive_caps=1,1
    else
        set -- video_nr=64 "card_label=$LOOPBACK_LABEL" exclusive_caps=1
    fi
    insmod /postinstall/gbos/kmod/v4l2loopback.ko "$@" \
        min_width=1280 max_width=1280 min_height=960 max_height=960 || {
        status=$?
        echo "ERROR: v4l2loopback insmod failed (status $status)"
        exit "$status"
    }
fi
for setting in min_width:1280 max_width:1280 min_height:960 max_height:960; do
    parameter=${setting%:*}
    expected=${setting#*:}
    actual=$(cat "/sys/module/v4l2loopback/parameters/$parameter" 2>/dev/null) || {
        echo "ERROR: cannot verify v4l2loopback $parameter"
        exit 1
    }
    if [ "$actual" != "$expected" ]; then
        echo "ERROR: v4l2loopback $parameter is $actual, expected $expected"
        exit 1
    fi
done
i=0
while [ ! -e /dev/video64 ] && [ "$i" -lt 10 ]; do
    sleep 1
    i=$((i + 1))
done
if [ ! -e /dev/video64 ]; then
    echo "ERROR: /dev/video64 was not created after loading v4l2loopback"
    grep '^v4l2loopback ' /proc/modules || echo "v4l2loopback is not listed in /proc/modules"
    ls /sys/class/video4linux 2>&1
    exit 1
fi
LOOPBACK_SYS=/sys/devices/virtual/video4linux/video64
if [ ! -r "$LOOPBACK_SYS/name" ]; then
    echo "ERROR: /dev/video64 exists but $LOOPBACK_SYS/name is unavailable"
    exit 1
fi
ACTUAL_LABEL=$(cat "$LOOPBACK_SYS/name")
if [ "$ACTUAL_LABEL" != "$LOOPBACK_LABEL" ]; then
    echo "ERROR: loopback label mismatch: got '$ACTUAL_LABEL', expected '$LOOPBACK_LABEL'"
    exit 1
fi
REAR_SYS=/sys/devices/virtual/video4linux/video65
if [ "$REAR" = 1 ]; then
    # The module may predate this boot's service start (loaded with one device): then no rear.
    if [ ! -e /dev/video65 ] || [ ! -r "$REAR_SYS/name" ]; then
        echo "WARNING: /dev/video65 missing (v4l2loopback loaded without it); rear camera off"
        REAR=0
    elif [ "$(cat "$REAR_SYS/name")" != "$REAR_LABEL" ]; then
        echo "WARNING: /dev/video65 is '$(cat "$REAR_SYS/name")', not '$REAR_LABEL'; rear camera off"
        REAR=0
    fi
fi

prepare_node() {  # prepare_node NR
    chmod 666 "/dev/video$1"
    chown cameraserver:camera "/dev/video$1" 2>/dev/null || true
    chcon u:object_r:video_device:s0 "/dev/video$1" 2>/dev/null || true
}

# uvc_uevent VID PID (4 hex digits each): the fake UVC interface's uevent. PRODUCT is hex
# without leading zeros (vid/pid/bcdDevice), as the kernel writes it; MODALIAS keeps all digits.
strip_zeros() {
    v=$1
    while [ "${#v}" -gt 1 ] && [ "${v#0}" != "$v" ]; do v=${v#0}; done
    echo "$v"
}
uvc_uevent() {
    echo DRIVER=uvcvideo
    echo "PRODUCT=$(strip_zeros "$1")/$(strip_zeros "$2")/100"
    echo TYPE=239/2/1
    echo INTERFACE=14/1/0
    echo "MODALIAS=usb:v$(echo "$1" | tr a-f A-F)p$(echo "$2" | tr a-f A-F)d0100dcEFdsc02dp01ic0Eisc01ip00in00"
}

# expose_uvc NR DESCRIPTOR FAKE_DIR VID PID: give /sys/devices/virtual/video4linux/videoNR a
# fake USB parent (descriptors + uevent) so the USB camera HAL treats it as a UVC camera.
# Returns 2 if the fake tree cannot be built, 1 if only the sysfs bind mount failed.
expose_uvc() {
    nr=$1 desc=$2 fake=$3 vid=$4 pid=$5
    video_sys=/sys/devices/virtual/video4linux/video$nr
    node_dir=$fake/usb_dev/intf/video4linux/video$nr
    mkdir -p "$node_dir" || return 2
    chmod 755 /data/local/tmp "$fake" "$fake/usb_dev" \
        "$fake/usb_dev/intf" "$fake/usb_dev/intf/video4linux" "$node_dir"
    cp "$desc" "$fake/descriptors" || return 2
    chmod 644 "$fake/descriptors"
    ln -sf ../descriptors "$fake/usb_dev/descriptors"
    ln -sf ../../descriptors "$fake/usb_dev/intf/descriptors"
    uvc_uevent "$vid" "$pid" > "$fake/usb_dev/intf/uevent" || return 2
    chmod 644 "$fake/usb_dev/intf/uevent"
    for attr in "$video_sys"/*; do
        [ -f "$attr" ] && cp -f "$attr" "$node_dir/"
    done
    rm -f "$node_dir/device"
    ln -sf "$fake/usb_dev/intf" "$node_dir/device"
    if ! grep -q "$video_sys" /proc/mounts; then
        mount -o bind "$node_dir" "$video_sys" || {
            echo "WARNING: could not expose the virtual UVC sysfs interface for video$nr"
            return 1
        }
    fi
    return 0
}

# Facing: the HAL takes lens data and facing for built-in cameras from $CAMERA_INFO by USB id
# (one camera per facing). Google's copy lists front cameras only, so bind-mount a copy with
# the rear entry added. Focal length and pixel size: OV8856_0BA801T3 tuning file record 13
# (283 = 2.83 mm, 1.12 um pixels); the aperture is not known and 2.0 is assumed.
add_rear_facing() {
    if grep -q " $CAMERA_INFO " /proc/mounts; then
        echo "rear facing: $CAMERA_INFO already replaced"
        return 0
    fi
    if [ ! -r "$CAMERA_INFO" ]; then
        echo "WARNING: $CAMERA_INFO missing; the rear camera will be listed as external"
        return 1
    fi
    if grep -q "\"$REAR_USB_VID:$REAR_USB_PID\"" "$CAMERA_INFO"; then
        echo "rear facing: $CAMERA_INFO already lists $REAR_USB_VID:$REAR_USB_PID"
        return 0
    fi
    info=$REAR_FAKE/internal_usb_camera_info.txtpb
    cp "$CAMERA_INFO" "$info" || return 1
    cat >> "$info" <<'EOF'
camera_modules {
  key: "1209:0001" # gbOS: Latitude 7320 Detachable rear OV8856 (0BA801T3) via ipu6-loopback
  value {
    lens_info {
      available_focal_lengths: 2.83
      minimum_focus_distance: 0.30
      optimal_focus_distance: 1.5
      available_apertures: 2.0
    }
    sensor_info {
      physical_size {
        width: 3.6736
        height: 2.7597
      }
      pixel_array_size {
        width: 3280
        height: 2464
      }
      active_array {
        left: 0
        top: 0
        right: 3280
        bottom: 2464
      }
    }
    facing: FACING_BACK
  }
}
EOF
    chmod 644 "$info"
    chcon u:object_r:vendor_configs_file:s0 "$info" 2>/dev/null || true
    mount -o bind "$info" "$CAMERA_INFO" || {
        echo "WARNING: could not bind-mount $CAMERA_INFO; the rear camera will be listed as external"
        return 1
    }
    echo "rear facing: $REAR_USB_VID:$REAR_USB_PID added to $CAMERA_INFO as FACING_BACK"
}

# wait_active LOGFILE PID: 0 once the bridge logs that it streams, 1 if it exits or times out.
wait_active() {
    i=0
    while [ "$i" -lt 15 ]; do
        if grep -q '^bridge active:' "$1" 2>/dev/null; then
            return 0
        fi
        kill -0 "$2" 2>/dev/null || return 1
        sleep 1
        i=$((i + 1))
    done
    return 1
}

prepare_node 64
FAKE_DIR=/data/local/tmp/ipu6-camera-fake
expose_uvc 64 /postinstall/gbos/surface/latitude_fake_uvc_desc.bin "$FAKE_DIR" 0408 4052
[ "$?" = 2 ] && exit 1

# OV5678_WINDOWS_ISP=1 in gbos.cfg: the front sensor gets the Windows driver's
# 0x5000/0x3d8c/0x3d8d values when its bridge starts streaming (A/B test, see
# docs/ov5678-windows-vs-linux.md). The rebuilt ov5675.ko provides the parameter and logs
# the values in effect ("gbos: Windows ISP values") in the kernel log.
WINISP=/sys/module/ov5675/parameters/gbos_windows_isp
if [ "$(cfg OV5678_WINDOWS_ISP)" = 1 ]; then
    if [ -w "$WINISP" ] && echo Y > "$WINISP"; then
        echo "OV5678 Windows ISP values: on"
    else
        echo "WARNING: OV5678_WINDOWS_ISP=1 but $WINISP is not writable (old ov5675.ko?)"
    fi
elif [ -e "$WINISP" ]; then
    echo N > "$WINISP" 2>/dev/null || true
    echo "OV5678 Windows ISP values: off"
fi

BRIDGE=/postinstall/gbos/installer/bin/ipu6-loopback
if [ ! -x "$BRIDGE" ]; then
    echo "ERROR: missing IPU6 loopback bridge binary"
    exit 1
fi
# Optional colour matrices and lens shading extracted from each module's Windows tuning file
# (tools/extract_aiqb_ccm.py): ov5678_* (front) and ov8856_* (rear). Proprietary data: only ever
# on the stick, never in git.
CAL=/postinstall/gbos/camera
[ -d "$CAL" ] || CAL=
if pgrep -f '/ipu6-loopback ' >/dev/null 2>&1; then
    echo "ERROR: bridge is already running outside this init service"
    exit 1
else
    "$BRIDGE" /dev/media0 /dev/video64 ${CAL:+"$CAL"} \
        > "$LOGDIR/camera-bridge-runtime.txt" 2>&1 &
    BRIDGE_PID=$!
    echo "Started bridge pid=$BRIDGE_PID"
fi

if ! wait_active "$LOGDIR/camera-bridge-runtime.txt" "$BRIDGE_PID"; then
    echo "ERROR: IPU6 loopback bridge did not reach streaming state"
    kill "$BRIDGE_PID" 2>/dev/null || true
    wait "$BRIDGE_PID" 2>/dev/null || true
    exit 1
fi

# Rear: second sensor on its own CSI2 port and ISYS capture node, streamed concurrently with the
# front. Its sysfs identity and facing entry are only added once it streams, so a failed rear
# never shows up in Android as a camera without frames.
REAR_PID_RUN=
REAR_FAKE=/data/local/tmp/ipu6-camera-fake-rear
REAR_LOG=$LOGDIR/camera-bridge-rear-runtime.txt
if [ "$REAR" = 1 ]; then
    prepare_node 65
    "$BRIDGE" --sensor ov8856 --rotate "$REAR_ROTATION" /dev/media0 /dev/video65 ${CAL:+"$CAL"} \
        > "$REAR_LOG" 2>&1 &
    REAR_PID_RUN=$!
    echo "Started rear bridge pid=$REAR_PID_RUN"
    if wait_active "$REAR_LOG" "$REAR_PID_RUN" &&
       expose_uvc 65 "$REAR_DESC" "$REAR_FAKE" "$REAR_USB_VID" "$REAR_USB_PID"; then
        add_rear_facing || true
        echo "Rear bridge ready"
    else
        echo "WARNING: rear bridge did not reach streaming state; front camera only"
        tail -n 5 "$REAR_LOG" 2>/dev/null
        kill "$REAR_PID_RUN" 2>/dev/null || true
        wait "$REAR_PID_RUN" 2>/dev/null || true
        REAR_PID_RUN=
    fi
fi

# Rescan the new virtual UVC device after its node and sysfs identity exist.
killall -9 android.hardware.camera.provider-usb-service.android-desktop 2>/dev/null || true
killall -9 cameraserver 2>/dev/null || true
echo "Bridge ready; supervising pid=$BRIDGE_PID${REAR_PID_RUN:+ and rear pid=$REAR_PID_RUN}"

# Supervise both bridges. A bridge exits when it gets no good IPU6 frame for 10 s (or on a
# hard error, or when a restart after an idle stop fails); it is then restarted, at most
# BRIDGE_RESTARTS times per sensor, 5 s after its exit, appending to its runtime log. A bridge
# that ran for BRIDGE_HEALTHY_S before exiting gets its restarts back: with idle stops a bridge
# runs for days and starts its sensor many times, so only failures in a row count. Idle stops
# themselves never exit. A bridge whose idle stop or restart failed (a step blocked for 12 s, the
# restart failed, or no frame came 5 s after it) exits with status 4: it is restarted at once
# (without using up a restart) with --idle-stop 0, so that camera streams without idle stops
# for the rest of the boot. The loopback nodes and their sysfs identities stay, and one bridge
# dying never stops the other. The script ends (for init) only when the front bridge has used
# up its restarts and the rear is gone too.
BRIDGE_RESTARTS=3
BRIDGE_HEALTHY_S=300
FRONT_RESTARTS=0
REAR_RESTARTS=0
FRONT_SINCE=$(date +%s)
REAR_SINCE=$FRONT_SINCE
EXIT_IDLE_FAILED=4
FRONT_IDLE_STOP=$IDLE_STOP
REAR_IDLE_STOP=$IDLE_STOP
start_front() {
    "$BRIDGE" --idle-stop "$FRONT_IDLE_STOP" /dev/media0 /dev/video64 ${CAL:+"$CAL"} \
        >> "$LOGDIR/camera-bridge-runtime.txt" 2>&1 &
    BRIDGE_PID=$!
    FRONT_SINCE=$(date +%s)
}
start_rear() {
    "$BRIDGE" --sensor ov8856 --rotate "$REAR_ROTATION" --idle-stop "$REAR_IDLE_STOP" /dev/media0 /dev/video65 ${CAL:+"$CAL"} \
        >> "$REAR_LOG" 2>&1 &
    REAR_PID_RUN=$!
    REAR_SINCE=$(date +%s)
}
# ran_long SINCE: true when the bridge started at SINCE has run for BRIDGE_HEALTHY_S or more.
ran_long() {
    [ $(( $(date +%s) - $1 )) -ge "$BRIDGE_HEALTHY_S" ]
}
status=0
while [ -n "$BRIDGE_PID$REAR_PID_RUN" ]; do
    sleep 2
    if [ -n "$BRIDGE_PID" ] && ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        wait "$BRIDGE_PID"
        status=$?
        echo "$(date +%T) front bridge pid=$BRIDGE_PID exited with status $status"
        BRIDGE_PID=
        if ran_long "$FRONT_SINCE"; then
            FRONT_RESTARTS=0
        fi
        if [ "$status" = "$EXIT_IDLE_FAILED" ] && [ "$FRONT_IDLE_STOP" != 0 ]; then
            FRONT_IDLE_STOP=0
            start_front
            echo "$(date +%T) front idle stop failed; restarted front bridge pid=$BRIDGE_PID with idle stop off for this boot"
        elif [ "$FRONT_RESTARTS" -lt "$BRIDGE_RESTARTS" ]; then
            FRONT_RESTARTS=$((FRONT_RESTARTS + 1))
            sleep 5
            start_front
            echo "$(date +%T) restarted front bridge pid=$BRIDGE_PID ($FRONT_RESTARTS of $BRIDGE_RESTARTS)"
        else
            echo "ERROR: front bridge out of restarts"
        fi
    fi
    if [ -n "$REAR_PID_RUN" ] && ! kill -0 "$REAR_PID_RUN" 2>/dev/null; then
        wait "$REAR_PID_RUN"
        rear_status=$?
        echo "$(date +%T) rear bridge pid=$REAR_PID_RUN exited with status $rear_status"
        REAR_PID_RUN=
        if ran_long "$REAR_SINCE"; then
            REAR_RESTARTS=0
        fi
        if [ "$rear_status" = "$EXIT_IDLE_FAILED" ] && [ "$REAR_IDLE_STOP" != 0 ]; then
            REAR_IDLE_STOP=0
            start_rear
            echo "$(date +%T) rear idle stop failed; restarted rear bridge pid=$REAR_PID_RUN with idle stop off for this boot"
        elif [ "$REAR_RESTARTS" -lt "$BRIDGE_RESTARTS" ]; then
            REAR_RESTARTS=$((REAR_RESTARTS + 1))
            sleep 5
            start_rear
            echo "$(date +%T) restarted rear bridge pid=$REAR_PID_RUN ($REAR_RESTARTS of $BRIDGE_RESTARTS)"
        else
            echo "WARNING: rear bridge out of restarts"
        fi
    fi
done
echo "ERROR: no IPU6 loopback bridge left running (front status $status)"
exit "$status"
