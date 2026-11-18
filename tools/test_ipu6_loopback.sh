#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
docker run --rm --network none -v "$ROOT:/repo" -w /repo gbos-kbuild \
	sh -c 'gcc -O2 -Wall -Wextra -Werror -o /tmp/test-ipulb tools/test_ipu6_loopback.c -lm && /tmp/test-ipulb && gcc -O2 -Wall -Wextra -Werror -o /tmp/test-sizes tools/test_v4l2loopback_framesizes.c && /tmp/test-sizes'
sh -n "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh" \
	"$ROOT/usb/surface/surface-camera.sh" "$ROOT/tools/build_ipu6_loopback.sh"
grep -q 'CAMERA_BRIDGE=0' "$ROOT/usb/gbos.cfg"
grep -q 'video_nr=64' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q 'min_width=640 max_width=640 min_height=480 max_height=480' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '"$BRIDGE" /dev/media0 /dev/video64' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q 'wait "$BRIDGE_PID"' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '/sys/module/ov5675/parameters/gbos_windows_isp' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q '^OV5678_WINDOWS_ISP=0' "$ROOT/usb/gbos.cfg"
grep -q 'latitude_fake_uvc_desc.bin' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
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
echo IPU6_LOOPBACK_INTEGRATION_TEST_PASS
