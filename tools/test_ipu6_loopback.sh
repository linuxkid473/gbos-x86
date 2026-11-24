#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
docker run --rm --network none -v "$ROOT:/repo" -w /repo gbos-kbuild \
	sh -c 'gcc -O2 -Wall -Wextra -Werror -pthread -o /tmp/test-ipulb tools/test_ipu6_loopback.c -lm && /tmp/test-ipulb &&
	       gcc -O2 -Wall -Wextra -Werror -pthread -DOUT_WIDTH=1280 -DOUT_HEIGHT=720 -o /tmp/test-ipulb-720 tools/test_ipu6_loopback.c -lm && /tmp/test-ipulb-720 &&
	       gcc -O2 -Wall -Wextra -Werror -pthread -DOUT_WIDTH=640 -DOUT_HEIGHT=480 -o /tmp/test-ipulb-640 tools/test_ipu6_loopback.c -lm && /tmp/test-ipulb-640 &&
	       gcc -O2 -Wall -Wextra -Werror -pthread -DOUT_WIDTH=1920 -DOUT_HEIGHT=1440 -o /tmp/test-ipulb-1440 tools/test_ipu6_loopback.c -lm && /tmp/test-ipulb-1440 && gcc -O2 -Wall -Wextra -Werror -o /tmp/test-sizes tools/test_v4l2loopback_framesizes.c && /tmp/test-sizes'
sh -n "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh" \
	"$ROOT/usb/surface/surface-camera.sh" "$ROOT/tools/build_ipu6_loopback.sh"
grep -q 'CAMERA_BRIDGE=0' "$ROOT/usb/gbos.cfg"
grep -q 'video_nr=64' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -Fq 'min_width=$OUT_W max_width=$OUT_W min_height=$OUT_H max_height=$OUT_H' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
# The bridge's output size is what the launcher's bounds and the descriptors say.
grep -q '^#define OUT_WIDTH 1920$' "$ROOT/kernel/ipu6-loopback.c"
grep -q '^#define OUT_HEIGHT 1080$' "$ROOT/kernel/ipu6-loopback.c"
grep -Fq '"$BRIDGE" /dev/media0 "$FRONT_OUT"' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '^FRONT_OUT=/dev/video64$' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q 'wait "$BRIDGE_PID"' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '/sys/module/ov5675/parameters/gbos_windows_isp' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '^OV5678_WINDOWS_ISP=0' "$ROOT/usb/gbos.cfg"
grep -Fq 'latitude_fake_uvc_desc$DESC_SUFFIX.bin' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
if grep -q 'nohup.*ipu6-loopback' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"; then
	echo "IPU6 bridge must stay in the init service process group" >&2
	exit 1
fi
grep -q 'GBOS_IPU6_CAMERA_BRIDGE' "$ROOT/tools/make_gbos86_image.sh"
grep -q 'latitude_fake_uvc_desc.bin' "$ROOT/tools/make_gbos86_image.sh"
tmp_desc=$(mktemp)
trap 'rm -f "$tmp_desc"' EXIT HUP INT TERM
python3 "$ROOT/tools/make_latitude_uvc_desc.py" \
	"$ROOT/usb/surface/fake_uvc_desc.bin" "$tmp_desc"
cmp -s "$tmp_desc" "$ROOT/usb/surface/latitude_fake_uvc_desc.bin"
# Rear camera: same 1920x1080 descriptor, its own USB id (the launcher's REAR_USB_VID/PID).
python3 "$ROOT/tools/make_latitude_uvc_desc.py" --usb-id 1209:0001 \
	"$ROOT/usb/surface/fake_uvc_desc.bin" "$tmp_desc"
cmp -s "$tmp_desc" "$ROOT/usb/surface/latitude_rear_fake_uvc_desc.bin"
# CAMERA_SIZE builds: the same descriptors with a single frame of that size.
for size in 1280x720 640x480; do
	python3 "$ROOT/tools/make_latitude_uvc_desc.py" --size $size "$ROOT/usb/surface/fake_uvc_desc.bin" "$tmp_desc"
	cmp -s "$tmp_desc" "$ROOT/usb/surface/latitude_fake_uvc_desc-$size.bin"
	python3 "$ROOT/tools/make_latitude_uvc_desc.py" --size $size --usb-id 1209:0001 \
		"$ROOT/usb/surface/fake_uvc_desc.bin" "$tmp_desc"
	cmp -s "$tmp_desc" "$ROOT/usb/surface/latitude_rear_fake_uvc_desc-$size.bin"
done
grep -q 'latitude_rear_fake_uvc_desc.bin' "$ROOT/tools/make_gbos86_image.sh"
grep -q '^REAR_USB_VID=1209$' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '^REAR_USB_PID=0001$' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
if python3 "$ROOT/tools/make_latitude_uvc_desc.py" --usb-id 1209 \
	"$ROOT/usb/surface/fake_uvc_desc.bin" "$tmp_desc" 2>/dev/null; then
	echo "malformed --usb-id accepted" >&2
	exit 1
fi
echo IPU6_LOOPBACK_INTEGRATION_TEST_PASS
