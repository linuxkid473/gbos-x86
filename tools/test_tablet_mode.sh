#!/bin/sh
# Host tests for the Latitude 7320 tablet mode pieces: usb/tablet_mode.sh (switch parsing, rotation re-apply, on-screen
# keyboard), the intel_hid option and input report in usb/hwdetect.sh, and the service wiring in usb/hook.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- usb/tablet_mode.sh ---------------------------------------------------------------------------------------------
cat > "$T/devices" <<'EOF'
I: Bus=0019 Vendor=0000 Product=0005 Version=0000
N: Name="Lid Switch"
P: Phys=PNP0C0D/button/input0
H: Handlers=event0
B: PROP=0
B: EV=21
B: SW=1

I: Bus=0011 Vendor=0001 Product=0001 Version=ab41
N: Name="AT Translated Set 2 keyboard"
H: Handlers=sysrq kbd leds event3
B: EV=120013
B: KEY=402000000 3803078f800d001 feffffdfffefffff fffffffffffffffe

I: Bus=0019 Vendor=0000 Product=0000 Version=0000
N: Name="Intel HID switches"
H: Handlers=event21
B: PROP=0
B: EV=21
B: SW=2
EOF

GBOS_TABLET_MODE_LIB=1
GBOS_INPUT_DEVICES=$T/devices
GBOS_TABLET_STATE_DIR=$T/state
GBOS_TABLET_RUN_DIR=$T/run
GBOS_SYSFS_ROOT=$T/sys
mkdir -p "$T/state" "$T/run" "$T/sys/bus/serio/devices/serio0" "$T/sys/bus/serio/drivers/atkbd"
. "$ROOT/usb/tablet_mode.sh"
TM_OUT=$T/tablet-mode.txt TM_ROTATION=1 TM_KEYBOARD=ime

tm_switch_devices > "$T/switches"
cat > "$T/expected" <<'EOF'
/dev/input/event0 lid "Lid Switch"
/dev/input/event21 tablet "Intel HID switches"
EOF
diff -u "$T/expected" "$T/switches"
[ "$(tm_tablet_device)" = /dev/input/event21 ] || fail "tablet device not found"

# getevent -qS stub: "<device>: <bits>"
GETEVENT_BITS=0002
getevent() {
  [ "$1" = -qS ] || return 1
  [ -n "$GETEVENT_BITS" ] && printf '%s: %s\n' "$2" "$GETEVENT_BITS"
  return 0
}
[ "$(tm_tablet_state /dev/input/event21)" = 1 ] || fail "SW_TABLET_MODE=1 not parsed"
GETEVENT_BITS=0003
[ "$(tm_tablet_state /dev/input/event21)" = 1 ] || fail "lid+tablet bits not parsed"
GETEVENT_BITS=0001
[ "$(tm_tablet_state /dev/input/event21)" = 0 ] || fail "lid-only bits read as tablet"
GETEVENT_BITS=
[ "$(tm_tablet_state /dev/input/event21)" = unknown ] || fail "empty getevent output not unknown"
[ "$(tm_tablet_state '')" = unknown ] || fail "missing device not unknown"
GBOS_INPUT_DEVICES=$T/none TM_INPUT_DEVICES=$T/none
[ -z "$(tm_tablet_device)" ] || fail "device found without /proc/bus/input/devices"
TM_INPUT_DEVICES=$T/devices GBOS_INPUT_DEVICES=$T/devices

# Rotation: re-applied only when LaptopController has reset it, and logged.
echo default > "$T/rotation"
cmd() {
  [ "$1 $2" = "window fixed-to-user-rotation" ] || { [ "$1 $2" = "window user-rotation" ] && echo lock 0; return 0; }
  if [ $# -eq 3 ]; then echo "$3" > "$T/rotation"; echo "set $3" >> "$T/cmd.log"; else cat "$T/rotation"; fi
}
dumpsys() { echo "  mCommittedState=DeviceState{identifier=3, name='TABLET'}"; }
timeout() { shift; "$@"; }
: > "$T/tablet-mode.txt"
tm_rotation_check boot
[ "$(cat "$T/rotation")" = disabled ] || fail "rotation policy not re-applied"
grep -q "rotation: fixed-to-user-rotation was 'default' (boot); re-applied disabled, now 'disabled'" "$T/tablet-mode.txt" ||
  fail "re-apply not logged: $(cat "$T/tablet-mode.txt")"
: > "$T/cmd.log"; : > "$T/tablet-mode.txt"
tm_rotation_check poll
[ ! -s "$T/cmd.log" ] && [ ! -s "$T/tablet-mode.txt" ] || fail "rotation re-applied although already disabled"
TM_ROTATION=0
echo default > "$T/rotation"
tm_rotation_check poll
[ "$(cat "$T/rotation")" = default ] || fail "rotation changed with ROTATION=0"
TM_ROTATION=1

# Keyboard (ime): saved on detach, restored on attach; only on a state change.
settings() {
  [ "$1 $2" = "--user current" ] || { echo "settings without --user current: $*" >> "$T/errors"; return 1; }
  shift 2
  case "$1" in
    get) cat "$T/ime" 2>/dev/null || echo null ;;
    put) echo "$4" > "$T/ime" ;;
    delete) rm -f "$T/ime"; echo "Deleted 1 rows" ;;
  esac
}
echo 0 > "$T/ime"
TM_DEV=/dev/input/event21
GETEVENT_BITS=0002
tm_apply_mode test || fail "detach not applied"
[ "$(cat "$T/ime")" = 1 ] && [ "$(cat "$T/state/ime_saved")" = 0 ] || fail "tablet did not enable show_ime_with_hard_keyboard"
grep -q 'SW_TABLET_MODE=1 (was unset, test).*TABLET' "$T/tablet-mode.txt" || fail "switch change not logged"
if tm_apply_mode again; then fail "unchanged state applied twice"; fi
GETEVENT_BITS=0000
tm_apply_mode attach || fail "attach not applied"
[ "$(cat "$T/ime")" = 0 ] && [ ! -e "$T/state/ime_saved" ] || fail "laptop did not restore show_ime_with_hard_keyboard"
# The setting was never set (null): restore by deleting it.
rm -f "$T/ime" "$T/run/mode"
GETEVENT_BITS=0002; tm_apply_mode detach
[ "$(cat "$T/state/ime_saved")" = null ] || fail "null setting not saved"
GETEVENT_BITS=0000; tm_apply_mode attach
[ ! -e "$T/ime" ] || fail "null setting not deleted on attach"
# A reboot in tablet mode: the next start in laptop state restores the saved value.
echo 1 > "$T/ime"; echo 0 > "$T/state/ime_saved"; rm -f "$T/run/mode"
tm_apply_mode start
[ "$(cat "$T/ime")" = 0 ] || fail "stale tablet setting not restored at start"
[ ! -e "$T/errors" ] || fail "$(cat "$T/errors")"

# Keyboard (unbind): atkbd detached with "none" (no newline: serio uses strncmp with the write length), rebound on attach.
TM_KEYBOARD=unbind
ln -s ../../drivers/atkbd "$T/sys/bus/serio/devices/serio0/driver"
echo 'i8042 KBD port' > "$T/sys/bus/serio/devices/serio0/description"
: > "$T/sys/bus/serio/devices/serio0/drvctl"; : > "$T/sys/bus/serio/devices/serio0/bind_mode"
rm -f "$T/run/mode"
GETEVENT_BITS=0002; tm_apply_mode detach
[ "$(cat "$T/sys/bus/serio/devices/serio0/drvctl")" = none ] || fail "atkbd not detached"
[ "$(wc -c < "$T/sys/bus/serio/devices/serio0/drvctl")" -eq 4 ] || fail "drvctl write has a trailing newline"
[ "$(cat "$T/sys/bus/serio/devices/serio0/bind_mode")" = manual ] || fail "bind_mode not manual"
[ "$(cat "$T/state/atkbd_ports")" = serio0 ] || fail "detached port not recorded"
rm "$T/sys/bus/serio/devices/serio0/driver"
GETEVENT_BITS=0000; tm_apply_mode attach
[ "$(cat "$T/sys/bus/serio/devices/serio0/drvctl")" = atkbd ] || fail "atkbd not rebound"
[ "$(cat "$T/sys/bus/serio/devices/serio0/bind_mode")" = auto ] || fail "bind_mode not restored"
[ ! -e "$T/state/atkbd_ports" ] || fail "port record not cleared"
TM_KEYBOARD=ime
unset -f getevent cmd dumpsys settings timeout

# ---- usb/hwdetect.sh ------------------------------------------------------------------------------------------------
log() { printf '%s\n' "$*" >> "$T/hwlog"; }
. "$ROOT/usb/hwdetect.sh"
mkdir -p "$T/sys/class/dmi/id"
printf 'Dell Inc.\n' > "$T/sys/class/dmi/id/sys_vendor"
printf 'Latitude 7320 Detachable\n' > "$T/sys/class/dmi/id/product_name"
printf '32\n' > "$T/sys/class/dmi/id/chassis_type"
MODOPTS=
[ "$(gbos_tablet_switch_opts 2>/dev/null)" = 'options intel_hid enable_sw_tablet_mode=2' ] || fail "intel_hid option not written"
TABLET_SWITCH=1
[ "$(gbos_tablet_switch_opts 2>/dev/null)" = 'options intel_hid enable_sw_tablet_mode=1' ] || fail "TABLET_SWITCH=1 ignored"
TABLET_SWITCH=0
[ -z "$(gbos_tablet_switch_opts 2>/dev/null)" ] || fail "TABLET_SWITCH=0 still wrote an option"
TABLET_SWITCH=bogus
[ "$(gbos_tablet_switch_opts 2>/dev/null)" = 'options intel_hid enable_sw_tablet_mode=2' ] || fail "unknown TABLET_SWITCH not defaulted"
unset TABLET_SWITCH
MODOPTS="xe.enable_psr=0 intel_hid.enable_sw_tablet_mode=1"
[ -z "$(gbos_tablet_switch_opts 2>/dev/null)" ] || fail "MODOPTS override not respected"
MODOPTS=
printf 'Latitude 7320\n' > "$T/sys/class/dmi/id/product_name"
[ -z "$(gbos_tablet_switch_opts 2>/dev/null)" ] || fail "option written on another laptop"
printf 'Latitude 7320 Detachable\n' > "$T/sys/class/dmi/id/product_name"
# gbos_modopts writes it before its MODOPTS early return.
sed -n '/^gbos_modopts() {/,/^}/p' "$ROOT/usb/hwdetect.sh" > "$T/modopts.sh"
awk '/gbos_tablet_switch_opts >>/ { t = NR } /\[ -n "\$MODOPTS" \] \|\|/ { m = NR } END { exit !(t && m && t < m) }' "$T/modopts.sh" ||
  fail "gbos_modopts does not write the tablet switch option before MODOPTS"

mkdir -p "$T/sys/class/input/input21/capabilities" "$T/sys/module/intel_hid/parameters" "$T/sys/bus/acpi/devices/INTC1051:00"
echo 'Intel HID switches' > "$T/sys/class/input/input21/name"
echo 21 > "$T/sys/class/input/input21/capabilities/ev"
echo 2 > "$T/sys/class/input/input21/capabilities/sw"
echo 2 > "$T/sys/module/intel_hid/parameters/enable_sw_tablet_mode"
GBOS_PROC_MODULES=$T/modules; : > "$T/modules"
dmesg() { echo '[   15.3] intel-hid INTC1051:00: platform supports switches'; }
gbos_report_input > "$T/input-report" 2>&1
grep -q 'chassis_type=32 intel_hid enable_sw_tablet_mode=2 intel_vbtn=not loaded' "$T/input-report" || fail "tablet switch summary missing"
grep -q '^input21 Intel HID switches: ev=21 sw=2$' "$T/input-report" || fail "input capabilities missing"
grep -q '^serio0 i8042 KBD port driver= bind_mode=auto$' "$T/input-report" || fail "serio report missing"
grep -q 'ACPI: INTC1051:00' "$T/input-report" || fail "ACPI ids missing"
grep -q 'platform supports switches' "$T/input-report" || fail "intel_hid dmesg missing"
grep -q 'N: Name="Intel HID switches"' "$T/input-report" || fail "/proc/bus/input/devices missing"
grep -q '^    gbos_report_input$' "$ROOT/usb/hwdetect.sh" || fail "gbos_hw_report does not call gbos_report_input"
unset -f dmesg

# ---- usb/hook.sh service wiring -------------------------------------------------------------------------------------
sed -n '/^gbos_tablet_mode_config() {/,/^}/p' "$ROOT/usb/hook.sh" > "$T/hook-tablet.sh"
[ -s "$T/hook-tablet.sh" ] || fail "gbos_tablet_mode_config missing"
. "$T/hook-tablet.sh"
SYSFS=$T/sys G=$T/gbos N=7
mkdir -p "$G"; : > "$G/tablet_mode.sh"
GBOS_SENSOR_ROTATION_POLICY_SELECTED=1
gbos_tablet_mode_config
[ "$GBOS_TABLET_MODE_START" = '    start gbos_tablet_mode' ] || fail "helper not started"
case "$GBOS_TABLET_MODE_SERVICE" in
  'service gbos_tablet_mode /system/bin/sh /postinstall/gbos/tablet_mode.sh /postinstall/gbos/logs/boot-7/tablet-mode.txt 1 ime'*'seclabel u:r:shell:s0'*oneshot*disabled) ;;
  *) fail "unexpected service: $GBOS_TABLET_MODE_SERVICE" ;;
esac
[ -z "$GBOS_ROTATION_OVERRIDE_PROP" ] || fail "override prop set by default"
ROTATION_OVERRIDE=1 TABLET_KEYBOARD=unbind
gbos_tablet_mode_config
[ "$GBOS_ROTATION_OVERRIDE_PROP" = '    setprop config.override_forced_orient true' ] || fail "ROTATION_OVERRIDE=1 ignored"
case "$GBOS_TABLET_MODE_SERVICE" in *'tablet-mode.txt 1 unbind'*) ;; *) fail "TABLET_KEYBOARD=unbind ignored" ;; esac
GBOS_SENSOR_ROTATION_POLICY_SELECTED=0
gbos_tablet_mode_config
case "$GBOS_TABLET_MODE_SERVICE" in *'tablet-mode.txt 0 unbind'*) ;; *) fail "rotation enforced without the rotation trial" ;; esac
[ -z "$GBOS_ROTATION_OVERRIDE_PROP" ] || fail "override prop set without the rotation trial"
unset ROTATION_OVERRIDE TABLET_KEYBOARD
TABLET_MODE=0
gbos_tablet_mode_config
[ -z "$GBOS_TABLET_MODE_START$GBOS_TABLET_MODE_SERVICE" ] || fail "TABLET_MODE=0 still starts the helper"
unset TABLET_MODE
printf 'Latitude 7320\n' > "$T/sys/class/dmi/id/product_name"
gbos_tablet_mode_config
[ -z "$GBOS_TABLET_MODE_START$GBOS_TABLET_MODE_SERVICE" ] || fail "helper started on another laptop"

grep -q '^\$GBOS_TABLET_MODE_SERVICE$' "$ROOT/usb/hook.sh" || fail "service not in gbos.rc"
grep -q '^\$GBOS_TABLET_MODE_START$' "$ROOT/usb/hook.sh" || fail "start not in gbos.rc"
grep -q '^\$GBOS_ROTATION_OVERRIDE_PROP$' "$ROOT/usb/hook.sh" || fail "override prop not in gbos.rc"
grep -q '^  gbos_tablet_mode_config$' "$ROOT/usb/hook.sh" || fail "gbos_tablet_mode_config not called"
for f in usb/install.sh tools/deploy_stick.sh tools/make_gbos86_image.sh; do
  grep -q 'tablet_mode\.sh' "$ROOT/$f" || fail "$f does not ship tablet_mode.sh"
done
sh -n "$ROOT/usb/tablet_mode.sh"

echo "tablet mode tests passed"
