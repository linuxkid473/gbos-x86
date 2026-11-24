#!/bin/sh
# Every gbos/ folder that the boot reads (hook.sh, stage0 after the install, the camera bridge) must be copied
# to the internal disk by usb/install.sh (install and Update). Evaluates install.sh's own DIRS lines against a
# fake stick that has every folder. Stick-only folders are listed in STICK_ONLY with the reason.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
G=$T/gbos
# a11y: stage0 screen reader, only used when booting the stick. logs: written per boot.
STICK_ONLY="a11y logs"
# update: the system updater's state on the installed system (usb/sysupdate.sh), made at boot, never copied.
RUNTIME="update"
NEED="firmware patched audio installer kmod modules surface ksu sensors camera thermal display"
for d in $NEED $STICK_ONLY; do mkdir -p "$G/$d"; done
# The DIRS assignments, as install.sh runs them.
sed -n '/^DIRS="/,/^for d in \$DIRS; do \[ -d/p' "$ROOT/usb/install.sh" | grep -v -e "^for d in \$DIRS" -e "\$FILES" > "$T/dirs.sh"
DIRS=; . "$T/dirs.sh"
fail=0
for d in $NEED; do
  case " $DIRS " in *" $d "*) ;; *) echo "FAIL: install.sh does not copy gbos/$d/"; fail=1;; esac
done
# Optional folders must not be required: a stick without camera/ (no calibration extracted) still installs.
rm -rf "$G/camera" "$G/sensors"; DIRS=; . "$T/dirs.sh"
case " $DIRS " in *" camera "*|*" sensors "*) echo "FAIL: camera/ or sensors/ listed although absent"; fail=1;; esac
# Every folder referenced as $G/<dir> or /postinstall/gbos/<dir>/ by the boot scripts is either copied or stick-only.
for d in $(grep -rhoE '(\$G|/postinstall/gbos)/[a-z0-9_]+/' "$ROOT"/usb/*.sh "$ROOT"/usb/installer/bin/*.sh | sed -E 's#.*/([a-z0-9_]+)/$#\1#' | sort -u); do
  case " $NEED $STICK_ONLY $RUNTIME " in *" $d "*) ;; *) echo "FAIL: boot scripts use gbos/$d/ but this test does not know it (add it to NEED, STICK_ONLY or RUNTIME)"; fail=1;; esac
done
[ $fail = 0 ] && echo "test_install_dirs: PASS"
exit $fail
