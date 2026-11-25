#!/bin/sh
# Test of usb/surface/iptsd-run.sh against a fake /sys, fake /dev and fake iptsd binaries (nothing touches the machine):
#   - the Surface touchpad's hidraw node (HID_ID ...045E:00000C9F) is found and iptsd is started on it, another HID device is ignored
#   - the Surface Pro 5 touchscreen (HID_ID ...1B96:0000001F) is checked as a touchscreen and gets iptsd-surface-pro-5.conf
#   - a rejecting iptsd-check-device means no start
#   - no such touchpad: gives up (exit 0) after WAIT; without the binaries it exits at once and writes no log
#   - the service in hook.sh is oneshot (the script exits by itself on other laptops, init must not restart it)
# Run: sh tools/test_iptsd_run.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
R=$ROOT/usb/surface/iptsd-run.sh
W=$(mktemp -d "${TMPDIR:-/tmp}/iptsdtest.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }

mkdev() { # mkdev SYS NAME HID_ID HID_NAME
  mkdir -p "$1/class/hidraw/$2/device"
  printf 'HID_ID=%s\nHID_NAME=%s\n' "$3" "$4" > "$1/class/hidraw/$2/device/uevent"
}
mkstick() { # mkstick DIR CHECK_RC : fake gbos dir with stub binaries
  mkdir -p "$1/surface" "$1/logs/boot-1"
  printf '#!/bin/sh\necho "$* config=${IPTSD_CONFIG_FILE:-}" >> "%s/checked"\n[ %s = 0 ] || echo "[12:00:00.000] [error] apps: InvalidScreenSize"\nexit %s\n' "$1" "$2" "$2" > "$1/surface/iptsd-check-device"
  printf '#!/bin/sh\necho "iptsd on $1 config $IPTSD_CONFIG_FILE" > "%s/ran"\nexit 0\n' "$1" > "$1/surface/iptsd"
  chmod +x "$1/surface/iptsd" "$1/surface/iptsd-check-device"
}
run() { # run GBOSDIR SYSDIR [extra env...]
  g=$1; s=$2; shift 2
  env GBOS_G="$g" GBOS_SYS="$s" GBOS_DEV=/dev GBOS_SLEEP=0 "$@" sh "$R" 1
}

# 1. touchpad present, one other HID device
S=$W/sys1; G=$W/g1
mkdev $S hidraw0 0003:000004F2:0000B6B7 "USB Camera"
mkdev $S hidraw1 0018:0000045E:00000C9F "MSHW0264:00 045E:0C9F"
mkstick $G 0
run $G $S GBOS_ONCE=1; rc=$?
[ $rc = 0 ] && ok "touchpad present: exit 0" || bad "touchpad present: exit $rc"
grep -q 'iptsd on /dev/hidraw1' $G/ran 2>/dev/null && ok "iptsd started on /dev/hidraw1 (not on the camera's hidraw0)" || bad "iptsd not started on hidraw1: $(cat $G/ran 2>&1)"
grep -q 'iptsd exited rc=0' $G/logs/boot-1/iptsd.txt && ok "exit status of iptsd logged correctly" || bad "rc not logged: $(cat $G/logs/boot-1/iptsd.txt)"
grep -q -- '--type touchpad /dev/hidraw1' $G/checked 2>/dev/null && ok "SL7 touchpad checked as --type touchpad" || bad "SL7 check args: $(cat $G/checked 2>&1)"
grep -q 'config .*/surface/iptsd.conf$' $G/ran 2>/dev/null && ok "SL7 touchpad runs with iptsd.conf" || bad "SL7 config: $(cat $G/ran 2>&1)"

# 1b. Surface Pro 5: IPTS touchscreen 045E:001F
S=$W/sys1b; G=$W/g1b
mkdev $S hidraw0 0003:0000045E:000007E8 "Microsoft Surface Type Cover"
mkdev $S hidraw2 0000:0000045E:0000001F "IPTS 045E:001F"   # as the ipts driver registers it on a real SP5 (2026-11-24 log)
mkstick $G 0
run $G $S GBOS_ONCE=1; rc=$?
[ $rc = 0 ] && grep -q 'iptsd on /dev/hidraw2 ' $G/ran 2>/dev/null && ok "SP5 touchscreen: iptsd started on /dev/hidraw2 (not on the Type Cover)" \
  || bad "SP5: rc=$rc ran=$(cat $G/ran 2>&1)"
grep -q -- '--type touchscreen /dev/hidraw2' $G/checked 2>/dev/null && ok "SP5 checked as --type touchscreen" || bad "SP5 check args: $(cat $G/checked 2>&1)"
# SP5 1807 on 2026-11-25: its IPTS has no metadata report, so check-device needs the config for the screen size (InvalidScreenSize)
grep -q -- '/dev/hidraw2 config=.*/surface/iptsd-surface-pro-5.conf$' $G/checked 2>/dev/null && ok "SP5 check runs with IPTSD_CONFIG_FILE (screen size)" \
  || bad "SP5 check without config: $(cat $G/checked 2>&1)"
grep -q 'config .*/surface/iptsd-surface-pro-5.conf$' $G/ran 2>/dev/null && ok "SP5 runs with iptsd-surface-pro-5.conf" || bad "SP5 config: $(cat $G/ran 2>&1)"
[ -f $ROOT/usb/surface/iptsd-surface-pro-5.conf ] && ok "usb/surface/iptsd-surface-pro-5.conf exists" || bad "SP5 config file missing"
# the older N-trig id of iptsd's preset (lower-case hex as some kernels print it) still matches
S=$W/sys1d; G=$W/g1d; mkdev $S hidraw0 0044:00001b96:0000001f "IPTS 1B96:001F"; mkstick $G 0
run $G $S GBOS_ONCE=1
grep -q 'config .*/surface/iptsd-surface-pro-5.conf$' $G/ran 2>/dev/null && ok "1B96:001F also gets the SP5 config" || bad "1B96: $(cat $G/ran 2>&1)"
# a product id that only starts like 001F must not match
S=$W/sys1c; G=$W/g1c; mkdev $S hidraw0 0044:00001B96:0000001F0 "not IPTS"; mkstick $G 0
run $G $S GBOS_WAIT=0; rc=$?
[ ! -e $G/ran ] && ok "HID_ID match is anchored (0000001F0 is not 0000001F)" || bad "loose HID_ID match: $(cat $G/ran 2>&1)"

# 2. check-device rejects the node (the SP5 touchscreen of 1b)
S=$W/sys1b; G=$W/g2; mkstick $G 1
run $G $S GBOS_ONCE=1; rc=$?
[ $rc = 1 ] && [ ! -e $G/ran ] && ok "rejected by iptsd-check-device: iptsd not started" || bad "rejected case: rc=$rc ran=$(cat $G/ran 2>&1)"
grep -q 'InvalidScreenSize' $G/logs/boot-1/iptsd.txt && ok "the refusal's reason is logged" || bad "reason not logged: $(cat $G/logs/boot-1/iptsd.txt)"
G=$W/g2b; mkstick $G 1
run $G $S GBOS_WAIT=6 GBOS_SLEEP=3; rc=$?
[ "$(grep -c InvalidScreenSize $G/logs/boot-1/iptsd.txt)" = 1 ] && ok "reason logged once, not on every retry" || bad "reason count: $(grep -c InvalidScreenSize $G/logs/boot-1/iptsd.txt)"
grep -q 'giving up: iptsd-check-device refused /dev/hidraw' $G/logs/boot-1/iptsd.txt && [ $rc = 0 ] && ok "giving up names the refused node" \
  || bad "giving-up line: rc=$rc $(tail -1 $G/logs/boot-1/iptsd.txt)"

# 3. no such touchpad on this machine
S=$W/sys3; mkdev $S hidraw0 0003:000004F2:0000B6B7 "USB Camera"
G=$W/g3; mkstick $G 0
run $G $S GBOS_WAIT=0; rc=$?
[ $rc = 0 ] && grep -q 'giving up' $G/logs/boot-1/iptsd.txt && [ ! -e $G/ran ] && ok "other laptop: gives up with exit 0, iptsd never started" || bad "other laptop: rc=$rc"

# 4. binaries missing
G=$W/g4; mkdir -p $G/logs/boot-1
run $G $S GBOS_WAIT=0; rc=$?
[ $rc = 0 ] && [ ! -e $G/logs/boot-1/iptsd.txt ] && ok "no surface/iptsd on the stick: exit 0, no log" || bad "no binaries: rc=$rc"

# 5. hook.sh / syntax
H=$ROOT/usb/hook.sh
awk '/^service gbos_iptsd /{f=1} f&&/oneshot/{print "yes"; exit} f&&/^$/{exit}' "$H" | grep -q yes && ok "hook: gbos_iptsd is oneshot" || bad "hook: gbos_iptsd service not oneshot"
grep -qF 'start gbos_iptsd' "$H" && ok "hook: gbos_iptsd is started" || bad "hook: gbos_iptsd never started"
sh -n "$R" && sh -n "$H" && ok "sh -n iptsd-run.sh hook.sh" || bad "syntax"

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo IPTSD_RUN_TEST_PASS
exit $fail
