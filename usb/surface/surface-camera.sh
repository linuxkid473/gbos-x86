#!/system/bin/sh
# Surface Laptop 7 (Intel Lunar Lake) Camera Bridge Service
# Sourced/started by the gbos_camera service in hook.sh.
# Bridges the IPU7 raw CSI-2 MIPI sensor (/dev/media0, /dev/video0)
# to Android's USB Camera HAL via v4l2loopback (/dev/video42).
# Hardware sensor streaming is gated on-demand (LED only lights when an app uses the camera).

# Exit immediately on non-Surface / non-IPU7 hardware so service costs nothing
if ! grep -qi 'Surface' /sys/class/dmi/id/product_name 2>/dev/null && [ ! -e /dev/media0 ]; then
    exit 0
fi

MODDIR="${0%/*}"
[ -d "$MODDIR" ] || MODDIR="/postinstall/gbos/surface"

echo "=== Starting Surface Laptop 7 Camera Service ($(date)) ==="

# 1. Clean up any stale virtual sysfs mounts
if grep -q " /sys/class/video4linux " /proc/mounts; then
    umount -l /sys/class/video4linux 2>/dev/null
fi

# 2. Ensure v4l2loopback is loaded (video_nr=42, exclusive_caps=1 for Android HAL compatibility)
if [ ! -e /dev/video42 ]; then
    for M in \
        "$MODDIR/v4l2loopback.ko" \
        "$MODDIR/modules/v4l2loopback.ko" \
        "/postinstall/gbos/kmod/v4l2loopback.ko" \
        "/postinstall/gbos/modules/lib/modules/$(uname -r)/kernel/drivers/media/test-drivers/v4l2loopback/v4l2loopback.ko" \
        "/postinstall/gbos/modules/v4l2loopback.ko" \
        "/data/adb/modules/gbos_surface_touchpad_fix/modules/v4l2loopback.ko"; do
        if [ -f "$M" ]; then
            echo "Loading v4l2loopback from $M"
            insmod "$M" video_nr=42 card_label="Surface Webcam" exclusive_caps=1 2>/dev/null
            [ -e /dev/video42 ] && break
        fi
    done
fi

if [ ! -e /dev/video42 ]; then
    echo "ERROR: /dev/video42 not found after insmod search"
fi

# Set proper permissions and SELinux contexts for camera devices
chmod 666 /dev/video* /dev/media* /dev/v4l-subdev* 2>/dev/null
chown cameraserver:camera /dev/video* /dev/media* /dev/v4l-subdev* 2>/dev/null
chcon u:object_r:video_device:s0 /dev/video* /dev/media* /dev/v4l-subdev* 2>/dev/null

# 3. Present fake UVC sysfs hierarchy for Android USB camera HAL discovery
FAKE_DIR=/data/local/tmp/fake_sys
mkdir -p "$FAKE_DIR/usb_dev/intf/video4linux/video42" 2>/dev/null
chmod 755 /data/local/tmp "$FAKE_DIR" "$FAKE_DIR/usb_dev" "$FAKE_DIR/usb_dev/intf" "$FAKE_DIR/usb_dev/intf/video4linux" "$FAKE_DIR/usb_dev/intf/video4linux/video42" 2>/dev/null

# Copy binary UVC descriptors
for D in \
    "$MODDIR/fake_uvc_desc.bin" \
    "$MODDIR/etc/fake_uvc_desc.bin" \
    "/postinstall/gbos/surface/fake_uvc_desc.bin" \
    "/postinstall/gbos/fake_uvc_desc.bin" \
    "/data/adb/modules/gbos_surface_touchpad_fix/etc/fake_uvc_desc.bin"; do
    if [ -f "$D" ]; then
        cp -f "$D" "$FAKE_DIR/descriptors" 2>/dev/null
        chmod 644 "$FAKE_DIR/descriptors" 2>/dev/null
        ln -sf ../descriptors "$FAKE_DIR/usb_dev/descriptors" 2>/dev/null
        ln -sf ../../descriptors "$FAKE_DIR/usb_dev/intf/descriptors" 2>/dev/null
        echo "Installed descriptors from $D"
        break
    fi
done

# Create fake UVC uevent
cat << 'EOF_UEVENT' > "$FAKE_DIR/usb_dev/intf/uevent"
DRIVER=uvcvideo
PRODUCT=408/4052/100
TYPE=239/2/1
INTERFACE=14/1/0
MODALIAS=usb:v0408p4052d0100dcEFdsc02dp01ic0Eisc01ip00in00
EOF_UEVENT
chmod 644 "$FAKE_DIR/usb_dev/intf/uevent" 2>/dev/null

# Populate fake video42 node and bind-mount
V42_REAL="/sys/devices/virtual/video4linux/video42"
if [ -d "$V42_REAL" ]; then
    for attr in "$V42_REAL"/*; do
        [ -f "$attr" ] && cp -f "$attr" "$FAKE_DIR/usb_dev/intf/video4linux/video42/" 2>/dev/null
    done
    rm -f "$FAKE_DIR/usb_dev/intf/video4linux/video42/device" 2>/dev/null
    ln -sf "$FAKE_DIR/usb_dev/intf" "$FAKE_DIR/usb_dev/intf/video4linux/video42/device" 2>/dev/null
    chmod -R 755 "$FAKE_DIR" 2>/dev/null
    chmod 644 "$FAKE_DIR/descriptors" "$FAKE_DIR/usb_dev/intf/uevent" 2>/dev/null

    if ! grep -q "$V42_REAL" /proc/mounts; then
        mount -o bind "$FAKE_DIR/usb_dev/intf/video4linux/video42" "$V42_REAL" 2>/dev/null
        echo "Bind mounted fake sysfs over $V42_REAL"
    fi
fi

# 4. Locate and run surface-webcam daemon
CAM_BIN=
for B in \
    "$MODDIR/surface-webcam" \
    "$MODDIR/bin/surface-webcam" \
    "/postinstall/gbos/surface/surface-webcam" \
    "/postinstall/gbos/surface-webcam" \
    "/data/adb/modules/gbos_surface_touchpad_fix/bin/surface-webcam"; do
    if [ -x "$B" ]; then
        CAM_BIN="$B"
        break
    fi
done

if [ -n "$CAM_BIN" ] && [ -e /dev/media0 ] && [ -e /dev/video0 ] && [ -e /dev/video42 ]; then
    if ! pgrep -f surface-webcam >/dev/null 2>&1; then
        echo "Starting surface-webcam daemon ($CAM_BIN)..."
        nohup "$CAM_BIN" /dev/media0 /dev/video0 /dev/video42 > /data/local/tmp/surface-webcam.log 2>&1 &
        sleep 1
    else
        echo "surface-webcam already running (PID $(pgrep -f surface-webcam))"
    fi
fi

# 5. Restart camera HAL and cameraserver to detect the newly registered UVC webcam
echo "Restarting camera HAL provider..."
killall -9 android.hardware.camera.provider-usb-service.android-desktop 2>/dev/null
killall -9 cameraserver 2>/dev/null

echo "=== Camera service initialized successfully ==="
