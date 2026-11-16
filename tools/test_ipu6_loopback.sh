#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
docker run --rm --network none -v "$ROOT:/repo" -w /repo gbos-kbuild \
	sh -c 'gcc -O2 -Wall -Wextra -Werror -o /tmp/test-ipulb tools/test_ipu6_loopback.c && /tmp/test-ipulb'
sh -n "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh" \
	"$ROOT/usb/surface/surface-camera.sh" "$ROOT/tools/build_ipu6_loopback.sh"
grep -q 'CAMERA_BRIDGE=0' "$ROOT/usb/gbos.cfg"
grep -q 'video_nr=64' "$ROOT/usb/installer/bin/ipu6-camera-bridge.sh"
grep -q 'GBOS_IPU6_CAMERA_BRIDGE' "$ROOT/tools/make_gbos86_image.sh"
echo IPU6_LOOPBACK_INTEGRATION_TEST_PASS
