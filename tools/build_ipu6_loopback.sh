#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/usb/installer/bin/ipu6-loopback
mkdir -p "$(dirname "$OUT")"
docker run --rm --network none -v "$ROOT:/repo" -w /repo gbos-kbuild \
	sh -c 'gcc -O2 -static -pthread -Wall -Wextra -o usb/installer/bin/ipu6-loopback kernel/ipu6-loopback.c -lm'
file "$OUT"
