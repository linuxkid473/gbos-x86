#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOOK=$ROOT/usb/hook.sh
CFG=$ROOT/usb/gbos.cfg
BUILDER=$ROOT/tools/make_gbos86_image.sh
TMP=$(mktemp -d)
trap 'rm -f "$TMP/functions.sh" "$TMP/fstab" "$TMP/expected" "$TMP/actual" "$TMP/image.cfg" "$TMP/builder-function.sh"; rmdir "$TMP"' EXIT HUP INT TERM

sed -n '/^gbos_configure_boot_debug() {/,/^}/p' "$HOOK" > "$TMP/functions.sh"
sed -n '/^gbos_configure_kvm_trace() {/,/^}/p' "$HOOK" >> "$TMP/functions.sh"
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
/dev/block/sda1 /mnt/media_rw/usb vfat nosuid,nodev wait,voldmanaged=usb:auto
EOF
cat > "$TMP/expected" <<'EOF'
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
LABEL=gbos-data-mounted-by-stage0 /data ext4 noatime,nosuid,nodev wait,check,encryptable=userdata
/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait,first_stage_mount
EOF
gbos_stage0_fstab_overlay "$TMP/fstab" > "$TMP/actual"
diff -u "$TMP/expected" "$TMP/actual"
awk 'BEGIN { exit !(length("gbos-data-mounted-by-stage0") > 16) }'
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

echo "BOOT_TUNING_TEST_PASS"
