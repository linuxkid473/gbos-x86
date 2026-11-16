#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HOOK=$ROOT/usb/hook.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/ui-defaults.XXXXXX")
trap 'rm -rf "$T"' EXIT
LOG=$T/calls
export LOG

CMD=$(sed -n 's#^service gbos_skip_setup /system/bin/sh -c "\(.*\)"$#\1#p' "$HOOK")
[ -n "$CMD" ] || { echo "FAIL: gbos_skip_setup command not found"; exit 1; }

sleep() { :; }
settings() { printf 'settings %s\n' "$*" >> "$LOG"; }
cmd() { printf 'cmd %s\n' "$*" >> "$LOG"; }
setprop() { :; }
am() { :; }
eval "$CMD"

grep -Fx "settings put global disable_window_blurs 0" "$LOG" >/dev/null
grep -Fx "cmd window disable-blur 0" "$LOG" >/dev/null
grep -Fx "settings put global window_animation_scale 1" "$LOG" >/dev/null
grep -Fx "settings put global transition_animation_scale 1" "$LOG" >/dev/null
grep -Fx "settings put global animator_duration_scale 1" "$LOG" >/dev/null
! grep -Eq '^settings put global (disable_window_blurs 1|window_animation_scale 0|transition_animation_scale 0|animator_duration_scale 0)$|^cmd window disable-blur 1$' "$LOG"

echo "UI_DEFAULTS_TEST_PASS"
