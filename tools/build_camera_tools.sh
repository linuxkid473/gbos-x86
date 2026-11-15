#!/bin/sh
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$REPO/usb/device/ipu3-webcam
SRC=$REPO/kernel/ipu3-webcam.c
CFLAGS="-O3 -march=x86-64-v3 -Wall -static -pthread"
mkdir -p "$REPO/usb/device"
if [ "${CAMERA_BUILD:-}" != host ] && command -v docker >/dev/null && docker info >/dev/null 2>&1; then
  docker run --rm -i --platform linux/amd64 -v "$REPO:/repo" -e HOSTUID=$(id -u) -e HOSTGID=$(id -g) debian:trixie sh -s <<INNER
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev >/dev/null
gcc $CFLAGS -o /repo/usb/device/ipu3-webcam /repo/kernel/ipu3-webcam.c -lm
gcc $CFLAGS -o /repo/usb/device/gbos-autorotate /repo/kernel/gbos-autorotate.c -lm
strip /repo/usb/device/ipu3-webcam /repo/usb/device/gbos-autorotate
chown \$HOSTUID:\$HOSTGID /repo/usb/device/ipu3-webcam /repo/usb/device/gbos-autorotate
INNER
else
  command -v gcc >/dev/null || { echo "no docker and no gcc: cannot build $OUT" >&2; exit 1; }
  gcc $CFLAGS -o "$OUT" "$SRC" -lm
  gcc $CFLAGS -o "$REPO/usb/device/gbos-autorotate" "$REPO/kernel/gbos-autorotate.c" -lm
  strip "$OUT" "$REPO/usb/device/gbos-autorotate"
fi
file "$OUT" 2>/dev/null | grep -q 'statically linked' || { echo "$OUT is not statically linked" >&2; exit 1; }
"$OUT" --selftest
ls -l "$OUT"
