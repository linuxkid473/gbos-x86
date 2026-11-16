#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

awk '
  /^service gbos_camera / { in_service=1; found=1; next }
  in_service && /^service / { exit }
  in_service && /^    group / { group=$0; groups++ }
  END {
    if (!found || groups != 1 || group != "    group root") exit 1
  }
' "$ROOT/usb/hook.sh"

echo "HOOK_CAMERA_SERVICE_TEST_PASS"
