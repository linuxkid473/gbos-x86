#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/usb/installer/bin/ipu6-loopback
mkdir -p "$(dirname "$OUT")"
# The default size (1920x1080), the CAMERA_SIZE builds (installer/bin/1280x720/, 640x480/) and the
# libcamera rear's 4:3 build (1920x1440/, CAMERA_LIBCAMERA=1).
mkdir -p "$ROOT/usb/installer/bin/1280x720" "$ROOT/usb/installer/bin/640x480" "$ROOT/usb/installer/bin/1920x1440"
docker run --rm --network none -v "$ROOT:/repo" -w /repo gbos-kbuild \
	sh -c 'gcc -O2 -static -pthread -Wall -Wextra -o usb/installer/bin/ipu6-loopback kernel/ipu6-loopback.c -lm &&
	       gcc -O2 -static -pthread -Wall -Wextra -DOUT_WIDTH=1280 -DOUT_HEIGHT=720 \
		-o usb/installer/bin/1280x720/ipu6-loopback kernel/ipu6-loopback.c -lm &&
	       gcc -O2 -static -pthread -Wall -Wextra -DOUT_WIDTH=640 -DOUT_HEIGHT=480 \
		-o usb/installer/bin/640x480/ipu6-loopback kernel/ipu6-loopback.c -lm &&
	       gcc -O2 -static -pthread -Wall -Wextra -DOUT_WIDTH=1920 -DOUT_HEIGHT=1440 \
		-o usb/installer/bin/1920x1440/ipu6-loopback kernel/ipu6-loopback.c -lm'
file "$OUT" "$ROOT/usb/installer/bin/1280x720/ipu6-loopback" "$ROOT/usb/installer/bin/640x480/ipu6-loopback" \
	"$ROOT/usb/installer/bin/1920x1440/ipu6-loopback"
