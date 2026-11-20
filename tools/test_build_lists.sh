#!/bin/sh
# Every file and folder usb/install.sh puts on an internal disk must also be put there, or on the stick, by the
# other three copies: tools/make_gbos86_image.sh (build.sh's USB image), tools/install_dualboot.sh (build.sh
# --dualboot) and tools/deploy_stick.sh (quick stick updates). A file added to one list only was how camera/ and
# thermal/ went missing on installs (2026-11-19).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
# install.sh's lists, as it builds them (FILES, gbos_fp, DIRS and the optional folders), on a fake stick with everything.
G=$T/gbos; mkdir -p "$G"
for d in sensors camera thermal display; do mkdir -p "$G/$d"; done
: > "$G/gbos_fp"
sed -n '/^FILES="/,/^for d in \$DIRS; do \[ -d/p' "$ROOT/usb/install.sh" | grep -v -e '^for d in \$DIRS' -e '^for f in \$FILES' > "$T/lists.sh"
FILES=; DIRS=; . "$T/lists.sh"
[ -n "$FILES" ] && [ -n "$DIRS" ] || { echo "FAIL: could not read FILES/DIRS from usb/install.sh"; exit 1; }
# Items each tool gets elsewhere: super.img/super.layout are built separately by make_gbos86_image.sh and
# install_dualboot.sh; deploy_stick.sh never copies the big items (super.img, bootdisk.img, firmware/) and replaces
# modules/ by build id; sensors/ comes from the experimental sensor payload (image_payload.sh stage).
skip() { case " $2 " in *" $1 "*) return 0;; esac; return 1; }
fail=0
check() { # tool, items it gets elsewhere
  tool=$1; elsewhere=$2
  for i in $FILES $DIRS; do
    skip "$i" "$elsewhere" && continue
    grep -Eq "(^|[ \"])$i([ \";]|$)" "$ROOT/$tool" || { echo "FAIL: $tool does not copy $i (usb/install.sh does)"; fail=1; }
  done
}
check tools/make_gbos86_image.sh "super.img super.layout sensors"
check tools/install_dualboot.sh "super.img super.layout"
check tools/deploy_stick.sh "super.img super.layout bootdisk.img firmware modules sensors"
# build.sh must build what the stick runs on the Latitude 7320: cameras on and the sensor sub-HAL from the prepared
# inputs when present (usb/gbos.cfg keeps CAMERA_BRIDGE=0 on purpose), for the USB image and the dual-boot install.
grep -q 'GBOS_IPU6_CAMERA_BRIDGE=${GBOS_IPU6_CAMERA_BRIDGE:-1}' "$ROOT/build.sh" || { echo "FAIL: build.sh does not turn the cameras on by default"; fail=1; }
grep -q 'build/sensor-hal-inputs/INPUTS' "$ROOT/build.sh" || { echo "FAIL: build.sh does not use the prepared sensor inputs"; fail=1; }
grep -q 'GBOS_IPU6_CAMERA_BRIDGE="$GBOS_IPU6_CAMERA_BRIDGE" GBOS_SENSOR_PAYLOAD="$SENSOR_PAYLOAD" sh tools/install_dualboot.sh' "$ROOT/build.sh" || { echo "FAIL: build.sh --dualboot drops the camera/sensor settings"; fail=1; }
grep -q 'image_payload.sh" stage "$GBOS_SENSOR_PAYLOAD"' "$ROOT/tools/install_dualboot.sh" || { echo "FAIL: install_dualboot.sh does not stage the sensor sub-HAL"; fail=1; }
grep -q 'CAMERA_BRIDGE=1' "$ROOT/tools/install_dualboot.sh" || { echo "FAIL: install_dualboot.sh cannot turn the cameras on"; fail=1; }
grep -q '"$U/camera"' "$ROOT/tools/make_gbos86_image.sh" || { echo "FAIL: make_gbos86_image.sh does not include usb/camera/"; fail=1; }
[ $fail = 0 ] && echo "test_build_lists: PASS"
exit $fail
