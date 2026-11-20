#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/usb/installer/bin/ipu6-loopback
mkdir -p "$(dirname "$OUT")"
# Also the CAMERA_SIZE=640x480 fallback build (installer/bin/640x480/).
mkdir -p "$ROOT/usb/installer/bin/640x480"
docker run --rm --network none -v "$ROOT:/repo" -w /repo gbos-kbuild \
	sh -c 'gcc -O2 -static -pthread -Wall -Wextra -o usb/installer/bin/ipu6-loopback kernel/ipu6-loopback.c -lm &&
	       gcc -O2 -static -pthread -Wall -Wextra -DOUT_WIDTH=640 -DOUT_HEIGHT=480 \
		-o usb/installer/bin/640x480/ipu6-loopback kernel/ipu6-loopback.c -lm'
file "$OUT" "$ROOT/usb/installer/bin/640x480/ipu6-loopback"
