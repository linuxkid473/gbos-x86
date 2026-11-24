#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/vendor_module_mask.XXXXXX")
trap 'rm -rf "$W"' EXIT

mkdir -p "$W/new/vendor/lib" "$W/new/vendor_dlkm/lib/modules"
ln -s /vendor_dlkm/lib/modules "$W/new/vendor/lib/modules"
echo incompatible > "$W/new/vendor_dlkm/lib/modules/intel-ipu6.ko"
: > "$W/mounts"
LOG=
log() { LOG="$LOG$*"; }
mount() {
  [ "$1" = -t ] && [ "$2" = tmpfs ] || return 1
  [ "$3" = -o ] && [ "$4" = mode=0755,nosuid,nodev ] || return 1
  [ "$5" = tmpfs ] || return 1
  [ "$6" = "$EXPECTED_MODULES_DIR" ] || return 1
  printf 'tmpfs %s tmpfs rw 0 0\n' "$6" >> "$MOUNTS_FILE"
}

. "$ROOT/usb/mask_vendor_modules.sh"
VENDOR_ROOT="$W/new"
EXPECTED_MODULES_DIR="$W/new/vendor_dlkm/lib/modules"
MOUNTS_FILE="$W/mounts"
gbos_mask_vendor_modules

grep -q "^tmpfs $W/new/vendor_dlkm/lib/modules tmpfs " "$MOUNTS_FILE"
case "$LOG" in
  *"$W/new/vendor_dlkm/lib/modules"*) ;;
  *) echo "FAIL: log did not identify masked vendor_dlkm path" >&2; exit 1 ;;
esac
echo "VENDOR_MODULE_MASK_TEST_PASS"
