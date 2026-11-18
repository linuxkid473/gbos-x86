#!/system/bin/sh
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
echo "=== Latitude 7320 IPU6 camera bridge $(date) ==="

if ! grep -qx 'Latitude 7320 Detachable' /sys/class/dmi/id/product_name 2>/dev/null; then
    echo "Not Latitude 7320 Detachable; refusing to start"
    exit 0
fi
if [ ! -s /postinstall/gbos/surface/latitude_fake_uvc_desc.bin ]; then
    echo "ERROR: missing UVC descriptors"
    exit 1
fi

if [ ! -e /dev/video64 ]; then
    insmod /postinstall/gbos/kmod/v4l2loopback.ko \
        video_nr=64 "card_label=$LOOPBACK_LABEL" exclusive_caps=1 \
        min_width=640 max_width=640 min_height=480 max_height=480 || {
        status=$?
        echo "ERROR: v4l2loopback insmod failed (status $status)"
        exit "$status"
    }
fi
for setting in min_width:640 max_width:640 min_height:480 max_height:480; do
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

chmod 666 /dev/video64
chown cameraserver:camera /dev/video64 2>/dev/null || true
chcon u:object_r:video_device:s0 /dev/video64 2>/dev/null || true

FAKE_DIR=/data/local/tmp/ipu6-camera-fake
VIDEO_SYS=$LOOPBACK_SYS
mkdir -p "$FAKE_DIR/usb_dev/intf/video4linux/video64" || exit 1
chmod 755 /data/local/tmp "$FAKE_DIR" "$FAKE_DIR/usb_dev" \
    "$FAKE_DIR/usb_dev/intf" "$FAKE_DIR/usb_dev/intf/video4linux" \
    "$FAKE_DIR/usb_dev/intf/video4linux/video64"
cp /postinstall/gbos/surface/latitude_fake_uvc_desc.bin "$FAKE_DIR/descriptors" || exit 1
chmod 644 "$FAKE_DIR/descriptors"
ln -sf ../descriptors "$FAKE_DIR/usb_dev/descriptors"
ln -sf ../../descriptors "$FAKE_DIR/usb_dev/intf/descriptors"
cat > "$FAKE_DIR/usb_dev/intf/uevent" <<'EOF'
DRIVER=uvcvideo
PRODUCT=408/4052/100
TYPE=239/2/1
INTERFACE=14/1/0
MODALIAS=usb:v0408p4052d0100dcEFdsc02dp01ic0Eisc01ip00in00
EOF
chmod 644 "$FAKE_DIR/usb_dev/intf/uevent"
for attr in "$VIDEO_SYS"/*; do
    [ -f "$attr" ] && cp -f "$attr" "$FAKE_DIR/usb_dev/intf/video4linux/video64/"
done
rm -f "$FAKE_DIR/usb_dev/intf/video4linux/video64/device"
ln -sf "$FAKE_DIR/usb_dev/intf" \
    "$FAKE_DIR/usb_dev/intf/video4linux/video64/device"
if ! grep -q "$VIDEO_SYS" /proc/mounts; then
    mount -o bind "$FAKE_DIR/usb_dev/intf/video4linux/video64" "$VIDEO_SYS" ||
        echo "WARNING: could not expose the virtual UVC sysfs interface"
fi

# OV5678_WINDOWS_ISP=1 in gbos.cfg: the front sensor gets the Windows driver's
# 0x5000/0x3d8c/0x3d8d values when the bridge starts streaming (A/B test, see
# docs/ov5678-windows-vs-linux.md). The rebuilt ov5675.ko provides the parameter and logs
# the values in effect ("gbos: Windows ISP values") in the kernel log.
WINISP=/sys/module/ov5675/parameters/gbos_windows_isp
if grep -q '^OV5678_WINDOWS_ISP=1' /postinstall/gbos/gbos.cfg 2>/dev/null; then
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
if pgrep -f '/ipu6-loopback ' >/dev/null 2>&1; then
    echo "ERROR: bridge is already running outside this init service"
    exit 1
else
    # Optional colour matrices and lens shading extracted from the module's Windows tuning
    # file (tools/extract_aiqb_ccm.py). Proprietary data: only ever on the stick, never in git.
    CAL=/postinstall/gbos/camera
    if [ -d "$CAL" ]; then
        "$BRIDGE" /dev/media0 /dev/video64 "$CAL" \
            > "$LOGDIR/camera-bridge-runtime.txt" 2>&1 &
    else
        "$BRIDGE" /dev/media0 /dev/video64 \
            > "$LOGDIR/camera-bridge-runtime.txt" 2>&1 &
    fi
    BRIDGE_PID=$!
    echo "Started bridge pid=$BRIDGE_PID"
fi

ready=0
i=0
while [ "$i" -lt 15 ]; do
    if grep -q '^bridge active:' "$LOGDIR/camera-bridge-runtime.txt" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        break
    fi
    sleep 1
    i=$((i + 1))
done
if [ "$ready" != 1 ]; then
    echo "ERROR: IPU6 loopback bridge did not reach streaming state"
    kill "$BRIDGE_PID" 2>/dev/null || true
    wait "$BRIDGE_PID" 2>/dev/null || true
    exit 1
fi

# Rescan the new virtual UVC device after its node and sysfs identity exist.
killall -9 android.hardware.camera.provider-usb-service.android-desktop 2>/dev/null || true
killall -9 cameraserver 2>/dev/null || true
echo "Bridge ready; init service will supervise pid=$BRIDGE_PID"
wait "$BRIDGE_PID"
status=$?
echo "ERROR: IPU6 loopback bridge exited with status $status"
exit "$status"
