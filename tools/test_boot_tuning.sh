#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOOK=$ROOT/usb/hook.sh
CFG=$ROOT/usb/gbos.cfg
BUILDER=$ROOT/tools/make_gbos86_image.sh
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

sed -n '/^gbos_configure_boot_debug() {/,/^}/p' "$HOOK" > "$TMP/functions.sh"
sed -n '/^gbos_configure_kvm_trace() {/,/^}/p' "$HOOK" >> "$TMP/functions.sh"
sed -n '/^gbos_hide_boot_usb_from_vold() {/,/^}/p' "$HOOK" >> "$TMP/functions.sh"
sed -n '/^gbos_stage0_fstab_overlay() {/,/^}/p' "$HOOK" >> "$TMP/functions.sh"
. "$TMP/functions.sh"

DEBUG_LOGS=0
gbos_configure_boot_debug
[ -z "$GBOS_LOGCAT_START" ] && [ -z "$GBOS_DEBUG_DIAGNOSTIC_START" ] && [ -z "$GBOS_DRM_DEBUG_STOP" ]
DEBUG_LOGS=1
gbos_configure_boot_debug
[ "$GBOS_LOGCAT_START" = '    start gbos_logcat' ]
case "$GBOS_DEBUG_DIAGNOSTIC_START" in
  *'start gbos_audiodiag'*'start gbos_displaydiag'*'start gbos_aidiag'*) ;;
  *) echo "DEBUG_LOGS did not enable the expected diagnostics" >&2; exit 1;;
esac
[ "$GBOS_DRM_DEBUG_STOP" = '    write /sys/module/drm/parameters/debug 0' ]

KVM_TRACE=0
gbos_configure_kvm_trace
[ -z "$GBOS_KVM_TRACE_SETUP" ] && [ -z "$GBOS_KVM_TRACE_ON" ] && [ -z "$GBOS_KVM_TRACE_STOP" ]
KVM_TRACE=1
gbos_configure_kvm_trace
case "$GBOS_KVM_TRACE_SETUP" in
  *'events/kvm/kvm_exit/enable 1'*'events/kvm/kvm_mmio/enable 1'*) ;;
  *) echo "KVM_TRACE did not enable the expected events" >&2; exit 1;;
esac
[ "$GBOS_KVM_TRACE_ON" = '    write /sys/kernel/tracing/tracing_on 1' ]
[ "$GBOS_KVM_TRACE_STOP" = '    write /sys/kernel/tracing/tracing_on 0' ]

cat > "$TMP/fstab" <<'EOF'
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
/dev/block/by-name/userdata /data ext4 noatime,nosuid,nodev wait,check,encryptable=userdata
/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait,first_stage_mount
/devices/*/usb* auto auto defaults voldmanaged=usb:auto
EOF
cat > "$TMP/expected" <<'EOF'
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
LABEL=gbos-data-mounted-by-stage0 /data ext4 noatime,nosuid,nodev wait,check,encryptable=userdata
/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait,first_stage_mount
EOF
gbos_stage0_fstab_overlay "$TMP/fstab" 0 > "$TMP/actual"
diff -u "$TMP/expected" "$TMP/actual"
cat > "$TMP/expected-usb" <<'EOF'
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
LABEL=gbos-data-mounted-by-stage0 /data ext4 noatime,nosuid,nodev wait,check,encryptable=userdata
/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait,first_stage_mount
/devices/*/usb* auto auto defaults voldmanaged=usb:auto
EOF
gbos_stage0_fstab_overlay "$TMP/fstab" 1 > "$TMP/actual"
diff -u "$TMP/expected-usb" "$TMP/actual"

BOOT_PART_SYS=$TMP/sys/devices/pci0000:00/usb1/1-2/1-2:1.0/host0/target0:0:0/0:0:0:0/block/sda/sda2
EXTERNAL_PART_SYS=$TMP/sys/devices/pci0000:00/usb1/1-3/1-3:1.0/host1/target1:0:0/1:0:0:0/block/sdb/sdb1
mkdir -p "${BOOT_PART_SYS%/*}" "$BOOT_PART_SYS" "${EXTERNAL_PART_SYS%/*}" "$EXTERNAL_PART_SYS"
: > "$BOOT_PART_SYS/partition"
: > "$EXTERNAL_PART_SYS/partition"
readlink() {
  [ "$1" = -f ] || return 1
  case $2 in
    /sys/class/block/sda2) printf '%s\n' "$BOOT_PART_SYS";;
    /sys/class/block/sdb1) printf '%s\n' "$EXTERNAL_PART_SYS";;
    *) return 1;;
  esac
}
mkdir() {
  [ "$1" = -p ] && [ "$2" = /dev/gbos-vold-exclude ]
}
log() { :; }
mount() {
  [ "$1" = --bind ] && [ "$2" = /dev/gbos-vold-exclude ] || return 1
  printf '%s\n' "$3" > "$TMP/masked-device"
  [ "${FAIL_MASK:-0}" = 0 ]
}
VOLD_USB_OK=0
if gbos_hide_boot_usb_from_vold /dev/sda2; then VOLD_USB_OK=1; fi
[ "$VOLD_USB_OK" = 1 ]
[ "$(cat "$TMP/masked-device")" = "${BOOT_PART_SYS%/*}" ]
[ "${BOOT_PART_SYS%/*}" != "${EXTERNAL_PART_SYS%/*}" ]
gbos_stage0_fstab_overlay "$TMP/fstab" "$VOLD_USB_OK" > "$TMP/actual"
grep -Fqx '/devices/*/usb* auto auto defaults voldmanaged=usb:auto' "$TMP/actual"
[ -d "$EXTERNAL_PART_SYS" ]
VOLD_USB_OK=0
if FAIL_MASK=1 gbos_hide_boot_usb_from_vold /dev/sda2; then VOLD_USB_OK=1; fi
if [ "$VOLD_USB_OK" = 1 ]; then
  echo "FAIL: accepted failure to hide the GBOS boot disk" >&2
  exit 1
fi
if gbos_hide_boot_usb_from_vold /dev/unknown; then
  echo "FAIL: accepted an unresolved GBOS block device" >&2
  exit 1
fi
gbos_stage0_fstab_overlay "$TMP/fstab" "$VOLD_USB_OK" > "$TMP/actual"
if grep -q 'voldmanaged=usb' "$TMP/actual"; then
  echo "FAIL: retained the generic vold rule after boot-disk masking failed" >&2
  exit 1
fi
awk 'BEGIN { exit !(length("gbos-data-mounted-by-stage0") > 16) }'
grep -q 'gbos_hide_boot_usb_from_vold "${STICK:-}"' "$HOOK"
grep -q 'gscutil.rc' "$HOOK"
grep -q '^DEBUG_LOGS=0$' "$CFG"
grep -q '^KVM_TRACE=0$' "$CFG"
grep -q '^    start gbos_crashlog$' "$HOOK"
grep -q '^    start gbos_sensordiag$' "$HOOK"
grep -q '^    start gbos_cameradiag$' "$HOOK"

sed -n '/^gbos_enable_image_debug_logs() {/,/^}/p' "$BUILDER" > "$TMP/builder-function.sh"
. "$TMP/builder-function.sh"
cat > "$TMP/image.cfg" <<'EOF'
MODE=ask
DEBUG_LOGS=0
EOF
GBOS_DEBUG_LOGS=0
gbos_enable_image_debug_logs "$TMP/image.cfg"
grep -q '^DEBUG_LOGS=0$' "$TMP/image.cfg"
GBOS_DEBUG_LOGS=1
gbos_enable_image_debug_logs "$TMP/image.cfg"
grep -q '^DEBUG_LOGS=1$' "$TMP/image.cfg"

# drmhwc: the vendor log level is replaced by WARN on every GPU path unless DRMHWC_VERBOSE=1.
sed -n '/^DROP=/,/^{ grep -v -E "\$DROP"/p' "$HOOK" > "$TMP/drop.sh"
grep -q 'DRMHWC_VERBOSE:-0}" != 1 ] && DROP="$DROP|^log.tag.drmhwc=|^persist.log.tag.drmhwc="' "$TMP/drop.sh"
! grep -q 'GPU_SW" = 1 ] && DROP=.*log.tag.drmhwc' "$HOOK"
for v in 0 1; do
  ( DRMHWC_VERBOSE=$v; DROP=x; eval "$(grep '^\[ "\${DRMHWC_VERBOSE' "$TMP/drop.sh")" || true; echo "$DROP" ) > "$TMP/drop.$v"
done
grep -q 'log.tag.drmhwc' "$TMP/drop.0" && ! grep -q 'log.tag.drmhwc' "$TMP/drop.1"
[ "$(grep -c 'echo "log.tag.drmhwc=WARN"' "$HOOK")" = 1 ]
grep -q '^#DRMHWC_VERBOSE=1$' "$CFG"

# Stage0 screen reader: skipped only when no legacy HDA controller (PCI class 0x0403xx) exists.
sed -n '/^has_hda_controller() {/,/^}/p' "$ROOT/usb/stage0_init.sh" > "$TMP/hda.sh"
. "$TMP/hda.sh"
command mkdir -p "$TMP/sys/bus/pci/devices/a" "$TMP/sys/bus/pci/devices/b"
echo 0x040100 > "$TMP/sys/bus/pci/devices/a/class"
echo 0x030000 > "$TMP/sys/bus/pci/devices/b/class"
! GBOS_SYSFS_ROOT="$TMP/sys" has_hda_controller
for c in 0x040300 0x040380; do
  echo $c > "$TMP/sys/bus/pci/devices/b/class"
  GBOS_SYSFS_ROOT="$TMP/sys" has_hda_controller
done
grep -q '! has_hda_controller; then' "$ROOT/usb/stage0_init.sh"

# Quiet kernel console (P4): the generic command line is quiet, the debug one stays verbose.
GEN=$(cat "$ROOT/kernel/isa_emu_patch/generic_cmdline.txt")
case " $GEN " in *" loglevel=4 "*) ;; *) echo "generic cmdline lacks loglevel=4" >&2; exit 1;; esac
case " $GEN " in *" ignore_loglevel "*|*" loglevel=8 "*) echo "generic cmdline is not quiet" >&2; exit 1;; esac
grep -q ' loglevel=8 ignore_loglevel ' "$ROOT/kernel/isa_emu_patch/debug_cmdline.txt"
# The screen reader silences the console and stop.sh restores the command line's levels (quiet stays quiet, debug stays verbose).
command mkdir -p "$TMP/a11ysys/module/printk/parameters" "$TMP/a11yproc/sys/kernel"
for st in "N 4" "Y 8"; do
  set -- $st
  echo "$1" > "$TMP/a11ysys/module/printk/parameters/ignore_loglevel"
  printf '%s\t4\t1\t7\n' "$2" > "$TMP/a11yproc/sys/kernel/printk"
  ( GBOS_SYSFS_ROOT="$TMP/a11ysys" GBOS_PROC_ROOT="$TMP/a11yproc"; . "$ROOT/usb/a11y/printk.sh"
    a11y_console_quiet "$TMP/printk.saved"; a11y_console_quiet "$TMP/printk.saved"
    [ "$(cat "$_a11y_il")" = 0 ]
    [ "$(cat "$_a11y_pk")" = 1 ]
    a11y_console_restore "$TMP/printk.saved" )
  want=0; [ "$1" = Y ] && want=1
  [ "$(cat "$TMP/a11ysys/module/printk/parameters/ignore_loglevel")" = $want ]
  [ "$(cat "$TMP/a11yproc/sys/kernel/printk")" = "$2" ]
  [ ! -f "$TMP/printk.saved" ]
done
grep -q 'a11y_console_quiet \$A/printk.saved' "$ROOT/usb/a11y/start.sh"
grep -q 'a11y_console_restore /a11y/printk.saved' "$ROOT/usb/a11y/stop.sh"
! grep -q '^echo 1 > /sys/module/printk/parameters/ignore_loglevel' "$ROOT/usb/a11y/stop.sh"

# Unimplemented framework HALs: drop only ISnapshot and IVideoChatManager; IAqueduct stays.
sed -n '/^gbos_drop_vintf_hal() {/,/^}/p' "$HOOK" > "$TMP/hal.sh"
sed -n '/^gbos_hide_unimplemented_hals() {/,/^}/p' "$HOOK" >> "$TMP/hal.sh"
. "$TMP/hal.sh"
command mkdir -p "$TMP/vintf" "$TMP/haltmp"
cat > "$TMP/vintf/com.google.android.snapshot.xml" <<'XML'
<!--
    Input:
        vendor/google/desktop/desktop-services/snapshot/com.google.android.snapshot.xml
-->
<manifest version="9.0" type="framework">
    <hal format="aidl">
        <name>com.google.android.aqueduct</name>
        <fqname>IAqueduct/default</fqname>
    </hal>
    <hal format="aidl">
        <name>com.google.android.snapshot</name>
        <version>5</version>
        <fqname>ISnapshot/default</fqname>
    </hal>
</manifest>
XML
cat > "$TMP/vintf/com.android.desktop.video_chat.xml" <<'XML'
<manifest version="9.0" type="framework">
    <hal format="aidl">
        <name>com.android.desktop.video_chat</name>
        <version>2</version>
        <fqname>IVideoChatManager/default</fqname>
    </hal>
</manifest>
XML
: > "$TMP/mounts"
mount() { echo "$*" >> "$TMP/mounts"; }
log() { :; }
GBOS_HAL_TMP="$TMP/haltmp" gbos_hide_unimplemented_hals "$TMP/vintf"
grep -q 'IAqueduct/default' "$TMP/haltmp/com.google.android.snapshot.xml"
! grep -q 'ISnapshot' "$TMP/haltmp/com.google.android.snapshot.xml"
[ "$(grep -c '<hal ' "$TMP/haltmp/com.google.android.snapshot.xml")" = 1 ]
grep -q '^<manifest version="9.0" type="framework">$' "$TMP/haltmp/com.android.desktop.video_chat.xml"
grep -q '^</manifest>$' "$TMP/haltmp/com.android.desktop.video_chat.xml"
! grep -q '<hal' "$TMP/haltmp/com.android.desktop.video_chat.xml"
[ "$(wc -l < "$TMP/mounts")" = 2 ]
# A changed or missing file is left alone.
: > "$TMP/mounts"; rm "$TMP/vintf/com.android.desktop.video_chat.xml"
sed -i 's/ISnapshot/ISnapshotV2/' "$TMP/vintf/com.google.android.snapshot.xml"
GBOS_HAL_TMP="$TMP/haltmp" gbos_hide_unimplemented_hals "$TMP/vintf"
[ ! -s "$TMP/mounts" ]
unset -f mount
grep -q 'HIDE_UNIMPLEMENTED_HALS:-1}" = 1' "$HOOK"

# Coldplug: sleep 1 s only after a pass that loaded new modules.
sed -n '/^gbos_coldplug() {/,/^}/p' "$ROOT/usb/hwdetect.sh" > "$TMP/coldplug.sh"
. "$TMP/coldplug.sh"
GBOS_MODALIAS="$TMP/modules.alias"; : > "$GBOS_MODALIAS"
GBOS_PROC_MODULES="$TMP/proc_modules"
gbos_load_opmodes() { :; }
mdev() { :; }
log() { :; }
sleep() { echo s >> "$TMP/sleeps"; }
gbos_coldplug_pass() {
  cp_add=$(echo "$CP_PLAN" | cut -d' ' -f1); CP_PLAN=$(echo "$CP_PLAN" | cut -s -d' ' -f2-)
  i=0; while [ $i -lt "${cp_add:-0}" ]; do echo m >> "$GBOS_PROC_MODULES"; i=$((i+1)); done
  echo p >> "$TMP/passes"
}
cp_case() {
  : > "$GBOS_PROC_MODULES"; : > "$TMP/sleeps"; : > "$TMP/passes"; CP_PLAN=$1
  gbos_coldplug
  [ "$(wc -l < "$TMP/passes")" = "$2" ] && [ "$(wc -l < "$TMP/sleeps")" = "$3" ]
}
cp_case "5 3 0" 3 2      # settles: no sleep after the empty pass
cp_case "0" 1 0          # nothing to load (the hook's second run): no sleep at all
cp_case "1 1 1 1 1" 4 4  # still loading: capped at 4 passes, each followed by the settle
unset -f sleep gbos_coldplug_pass mdev

# Thermal report: zone types, temperatures and trip points from sysfs.
sed -n '/^gbos_report_thermal() {/,/^}/p' "$ROOT/usb/hwdetect.sh" > "$TMP/thermal.sh"
. "$TMP/thermal.sh"
T="$TMP/thermsys/class/thermal"
command mkdir -p "$T/thermal_zone0" "$T/thermal_zone1" "$T/cooling_device0"
echo TSKN > "$T/thermal_zone0/type"; echo 41000 > "$T/thermal_zone0/temp"; echo step_wise > "$T/thermal_zone0/policy"
echo passive > "$T/thermal_zone0/trip_point_0_type"; echo 50000 > "$T/thermal_zone0/trip_point_0_temp"
echo TCPU > "$T/thermal_zone1/type"; echo 55000 > "$T/thermal_zone1/temp"
echo Processor > "$T/cooling_device0/type"; echo 0 > "$T/cooling_device0/cur_state"; echo 3 > "$T/cooling_device0/max_state"
GBOS_SYSFS_ROOT="$TMP/thermsys" gbos_report_thermal > "$TMP/thermal.out"
grep -qx 'thermal_zone0 TSKN 41000 step_wise' "$TMP/thermal.out"
grep -qx '  trip_point_0: passive 50000' "$TMP/thermal.out"
grep -q '^thermal_zone1 TCPU 55000' "$TMP/thermal.out"
grep -qx 'cooling_device0 Processor 0/3' "$TMP/thermal.out"
grep -q '^    gbos_report_thermal$' "$ROOT/usb/hwdetect.sh"

# Lapis power limits: only the powercap/TCC writes go; permissions and other commands stay.
sed -n '/^gbos_filter_lapis_power_limits() {/,/^}/p' "$HOOK" > "$TMP/lapis.sh"
sed -n '/^gbos_latitude_thermal_config() {/,/^}/p' "$HOOK" >> "$TMP/lapis.sh"
. "$TMP/lapis.sh"
cat > "$TMP/init.lapis.rc" <<'RC'
on init
    write /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw 25000000
    write /sys/class/powercap/intel-rapl-mmio:0/constraint_1_power_limit_uw 55000000
    write /sys/class/powercap/intel-rapl:0/constraint_0_time_window_us 28000000
    write /sys/devices/pci0000:00/0000:00:04.0/tcc_offset_degree_celsius 5
    write /proc/acpi/wakeup "CREC"

on property:vendor.thermal.link_ready=1
    chown root system /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw
    chmod 0664 /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw
RC
gbos_filter_lapis_power_limits "$TMP/init.lapis.rc" "$TMP/init.lapis.out"
! grep -q 'powercap.*power_limit_uw 25000000\|55000000\|time_window_us 28000000\|tcc_offset' "$TMP/init.lapis.out"
grep -q '^    write /proc/acpi/wakeup "CREC"$' "$TMP/init.lapis.out"
[ "$(grep -c 'powercap' "$TMP/init.lapis.out")" = 2 ]
grep -q '^on init$' "$TMP/init.lapis.out" && grep -q '^on property:vendor.thermal.link_ready=1$' "$TMP/init.lapis.out"
grep -q 'KEEP_LAPIS_POWER_LIMITS:-0}" != 1' "$HOOK"

# Latitude thermal config: DMI-scoped, bind-mounted over the existing HAL config paths.
command mkdir -p "$TMP/troot/vendor/etc/thermal/lapis"
: > "$TMP/troot/vendor/etc/thermal/lapis/thermal_info_config.json"
: > "$TMP/troot/vendor/etc/thermal_info_config.json"
: > "$TMP/mounts"
mount() { echo "$*" >> "$TMP/mounts"; }
log() { :; }
CFGJ="$ROOT/usb/thermal/latitude7320_thermal_info_config.json"
! gbos_latitude_thermal_config "$TMP/troot" "$CFGJ" "Latitude 5300"
[ ! -s "$TMP/mounts" ]
gbos_latitude_thermal_config "$TMP/troot" "$CFGJ" "Latitude 7320 Detachable"
[ "$(wc -l < "$TMP/mounts")" = 2 ]
! gbos_latitude_thermal_config "$TMP/troot" "$TMP/missing.json" "Latitude 7320 Detachable"
unset -f mount
python3 -I -c 'import json,sys
d = json.load(open(sys.argv[1]))
names = {s["Name"] for s in d["Sensors"]}
assert {"TSKN","TCPU","TMEM","CHRG","AMBF","NGFF"} <= names
for s in d["Sensors"]:
    assert "BindedCdevInfo" not in s and "PIDInfo" not in s, s["Name"]
    t = s.get("HotThreshold")
    if t: assert len(t) == 7 and t[6] == "NaN", s["Name"]
assert "CoolingDevices" not in d' "$CFGJ"
grep -q 'for d in patched audio installer kmod ksu a11y surface thermal; do' "$ROOT/tools/deploy_stick.sh"
grep -q 'surface ksu thermal; do' "$ROOT/tools/make_gbos86_image.sh"

# RAPL report: firmware power limits recorded before Android.
P="$TMP/thermsys/class/powercap/intel-rapl:0"
command mkdir -p "$P"
echo package-0 > "$P/name"; echo long_term > "$P/constraint_0_name"; echo 15000000 > "$P/constraint_0_power_limit_uw"; echo 28000000 > "$P/constraint_0_time_window_us"
GBOS_SYSFS_ROOT="$TMP/thermsys" gbos_report_thermal > "$TMP/thermal2.out"
grep -qx 'intel-rapl:0 package-0 constraint_0: long_term 15000000 28000000' "$TMP/thermal2.out"

echo "BOOT_TUNING_TEST_PASS"
