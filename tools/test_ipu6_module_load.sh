#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

GBOS_PROC_MODULES=$TMP/proc_modules
: > "$GBOS_PROC_MODULES"
LOG=$TMP/log
REQUESTS=$TMP/requests
: > "$LOG"
: > "$REQUESTS"

log() {
    printf '%s\n' "$*" >> "$LOG"
}

modprobe() {
    printf '%s\n' "$*" >> "$REQUESTS"
    [ "${FAIL_MODULE:-}" != "$2" ] || return 1
    return 0
}

. "$ROOT/usb/hwdetect.sh"

gbos_load_ipu6_isys
test ! -s "$REQUESTS"

printf 'intel_ipu6 123 0 - Live 0\n' > "$GBOS_PROC_MODULES"
gbos_load_ipu6_isys
grep -q '^-q intel_ipu6_isys$' "$REQUESTS"
grep -q 'loaded intel_ipu6_isys' "$LOG"

printf 'intel_ipu6 123 0 - Live 0\nintel_ipu6_isys 456 1 - Live 0\n' > "$GBOS_PROC_MODULES"
gbos_load_ipu6_isys
test "$(wc -l < "$REQUESTS")" -eq 1

GBOS_SYSFS_ROOT=$TMP/sys
mkdir -p "$GBOS_SYSFS_ROOT/bus/media/devices/media0" \
    "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00" \
    "$GBOS_SYSFS_ROOT/bus/platform/devices/INT3472:00" \
    "$GBOS_SYSFS_ROOT/bus/platform/devices/tps68470-gpio" \
    "$GBOS_SYSFS_ROOT/class/dmi/id"
printf 'Dell Inc.\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/sys_vendor"
printf 'Other model\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/product_name"
gbos_load_tps68470_children
test "$(wc -l < "$REQUESTS")" -eq 1
mkdir -p "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-INT3472:07"
printf 'Latitude 7320 Detachable\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/product_name"
gbos_load_tps68470_children
tail -n 3 "$REQUESTS" > "$TMP/pmic-requests"
printf '%s\n' '-q clk-tps68470' '-q tps68470-regulator' '-q gpio-tps68470' > "$TMP/pmic-expected"
cmp "$TMP/pmic-expected" "$TMP/pmic-requests"
grep -q 'requested gpio-tps68470' "$LOG"
gbos_load_tps68470_children
test "$(wc -l < "$REQUESTS")" -eq 4
GBOS_TPS_REQ=0
FAIL_MODULE=tps68470-regulator
if gbos_load_tps68470_children; then
    echo "PMIC preload accepted a failed regulator load" >&2
    exit 1
fi
grep -q 'modprobe tps68470-regulator FAILED' "$LOG"
test "$(tail -n 1 "$REQUESTS")" = '-q tps68470-regulator'
test "$GBOS_TPS_REQ" = 0
FAIL_MODULE=
gbos_load_tps68470_children
test "$GBOS_TPS_REQ" = 1
printf 'Intel IPU6\n' > "$GBOS_SYSFS_ROOT/bus/media/devices/media0/model"
printf 'OVTI8856:00\n' > "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/name"
ln -s /drivers/int3472-discrete "$GBOS_SYSFS_ROOT/bus/platform/devices/INT3472:00/driver"
ln -s /drivers/tps68470-gpio "$GBOS_SYSFS_ROOT/bus/platform/devices/tps68470-gpio/driver"
gbos_report_camera_bus_devices > "$TMP/report"
grep -q '^media0 model=Intel IPU6 ' "$TMP/report"
grep -q '^i2c-OVTI8856:00 name=OVTI8856:00 ' "$TMP/report"
grep -q '^INT3472:00 .*driver=int3472-discrete$' "$TMP/report"
grep -q '^tps68470-gpio .*driver=tps68470-gpio$' "$TMP/report"

mkdir -p "$TMP/bin" "$GBOS_SYSFS_ROOT/class/dmi/id"
printf 'Dell Inc.\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/sys_vendor"
printf 'Latitude 7320 Detachable\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/product_name"
: > "$TMP/media0"
cat > "$TMP/bin/media-graph" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$GRAPH_CALLS"
printf 'media model=ipu6 driver=intel-ipu6\n'
[ "${GRAPH_FAIL:-0}" = 0 ] || exit 23
EOF
chmod +x "$TMP/bin/media-graph"
GBOS_CAMERA_MEDIA_GRAPH_BIN=$TMP/bin/media-graph
GBOS_CAMERA_MEDIA_NODE=$TMP/media0
GRAPH_CALLS=$TMP/graph-calls
export GBOS_CAMERA_MEDIA_GRAPH_BIN GBOS_CAMERA_MEDIA_NODE GRAPH_CALLS
gbos_report_camera_media_graph > "$TMP/graph-report"
grep -q '^media model=ipu6 driver=intel-ipu6$' "$TMP/graph-report"
test "$(cat "$GRAPH_CALLS")" = "$TMP/media0"

GBOS_CAMERA_MEDIA_GRAPH_BIN=$TMP/bin/missing
if gbos_report_camera_media_graph > "$TMP/graph-report"; then
    echo "missing media graph helper was accepted" >&2
    exit 1
fi
grep -q '^ERROR: media topology helper is missing or not executable:' "$TMP/graph-report"

GBOS_CAMERA_MEDIA_GRAPH_BIN=$TMP/bin/media-graph
GBOS_CAMERA_MEDIA_NODE=$TMP/missing-media
if gbos_report_camera_media_graph > "$TMP/graph-report"; then
    echo "missing media device was accepted" >&2
    exit 1
fi
grep -q '^ERROR: media device is missing:' "$TMP/graph-report"

GBOS_CAMERA_MEDIA_NODE=$TMP/media0
GRAPH_FAIL=1
export GRAPH_FAIL
if gbos_report_camera_media_graph > "$TMP/graph-report"; then
    echo "media graph helper failure was accepted" >&2
    exit 1
fi
grep -q '^ERROR: media topology helper failed with status 23$' "$TMP/graph-report"
GRAPH_FAIL=0
export GRAPH_FAIL

printf 'Other vendor\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/sys_vendor"
calls_before=$(wc -l < "$GRAPH_CALLS")
gbos_report_camera_media_graph > "$TMP/graph-report"
grep -q '^== media topology capture (not Latitude 7320 Detachable; skipped)$' "$TMP/graph-report"
test "$(wc -l < "$GRAPH_CALLS")" -eq "$calls_before"
printf 'Dell Inc.\n' > "$GBOS_SYSFS_ROOT/class/dmi/id/sys_vendor"

mkdir -p "$GBOS_SYSFS_ROOT/kernel/debug" "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/power" "$GBOS_SYSFS_ROOT/bus/i2c/drivers/ov8856"
mkdir -p "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/power"
printf '1\n' > "$GBOS_SYSFS_ROOT/bus/i2c/drivers_autoprobe"
printf 'acpi:OVTI8856:\n' > "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/modalias"
gbos_report_camera_probe_state > "$TMP/probe-report"
! grep -q '^driver_override=' "$TMP/probe-report"
! grep -q '^firmware_node_' "$TMP/probe-report"
printf 'blocked-driver\n' > "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/driver_override"
printf 'suspended\n' > "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/power/runtime_status"
printf '0x0f\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/status"
printf '\\_SB.PCI0.I2C0.CAM1\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/path"
printf 'D0\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/power_state"
printf 'active\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/power/runtime_status"
printf 'auto\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/power/control"
printf 'enabled\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/status"
printf 'acpi:OVTI8856:\n' > "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/modalias"
printf 'i2c-OVTI8856:00: supplier not ready\nunrelated: test\n' > "$GBOS_SYSFS_ROOT/kernel/debug/devices_deferred"
ln -s "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00" "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/firmware_node"
ln -s /drivers/acpi-test "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/driver"
ln -s /devices/OVTI8856:00 "$GBOS_SYSFS_ROOT/bus/acpi/devices/OVTI8856:00/physical_node"
ln -s /suppliers/test "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/supplier:camera"
gbos_report_camera_probe_state > "$TMP/probe-report"
grep -q '^i2c-OVTI8856:00 driver=$' "$TMP/probe-report"
grep -q '^acpi:OVTI8856:$' "$TMP/probe-report"
grep -q '^suspended$' "$TMP/probe-report"
grep -q '^driver_override=blocked-driver$' "$TMP/probe-report"
grep -q '^firmware_node_status=enabled$' "$TMP/probe-report"
grep -q '^firmware_node_modalias=acpi:OVTI8856:$' "$TMP/probe-report"
grep -q '^firmware_node_driver=acpi-test$' "$TMP/probe-report"
grep -q '^supplier:camera=/suppliers/test$' "$TMP/probe-report"
rm "$GBOS_SYSFS_ROOT/bus/i2c/devices/i2c-OVTI8856:00/supplier:camera"
gbos_report_camera_probe_state > "$TMP/probe-report"
grep -q '^supplier_links=(none)$' "$TMP/probe-report"
grep -q '^acpi_physical_node=/devices/OVTI8856:00$' "$TMP/probe-report"
grep -q '^acpi_supplier_links=(none)$' "$TMP/probe-report"
grep -q '^i2c_sysfs_path=' "$TMP/probe-report"
grep -q '^i2c_parent_path=' "$TMP/probe-report"
grep -q '^== ACPI device power state OVTI8856:00$' "$TMP/probe-report"
grep -q '^acpi_status=enabled$' "$TMP/probe-report"
grep -q '^acpi_path=\\_SB.PCI0.I2C0.CAM1$' "$TMP/probe-report"
grep -q '^acpi_power_state=D0$' "$TMP/probe-report"
grep -q '^acpi_runtime_status=active$' "$TMP/probe-report"
grep -q '^acpi_power_control=auto$' "$TMP/probe-report"
grep -q '^acpi_supplier_links=(none)$' "$TMP/probe-report"
grep -q '^i2c drivers_autoprobe=1$' "$TMP/probe-report"
grep -q '^registered i2c driver=ov8856$' "$TMP/probe-report"
grep -q '^i2c-OVTI8856:00: supplier not ready$' "$TMP/probe-report"
! grep -q 'unrelated: test' "$TMP/probe-report"
printf 'unrelated: test\n' > "$GBOS_SYSFS_ROOT/kernel/debug/devices_deferred"
gbos_report_camera_probe_state > "$TMP/probe-report"
grep -q '^(none reported)$' "$TMP/probe-report"
rm "$GBOS_SYSFS_ROOT/kernel/debug/devices_deferred"
gbos_report_camera_probe_state > "$TMP/probe-report"
grep -q '^== deferred probes unavailable ' "$TMP/probe-report"
GBOS_SYSFS_ROOT=$TMP/empty-sys gbos_report_camera_probe_state > "$TMP/probe-report"
grep -q '^(no matching sensor I2C devices)$' "$TMP/probe-report"
GBOS_CAMERA_TMPDIR=$TMP
mount() {
    printf '%s\n' "$*" >> "$TMP/mount-calls"
    [ "${FAIL_MOUNT:-0}" = 0 ] || return 1
    for arg do last=$arg; done
    printf 'i2c-OVTI8856:00: mock deferred\n' > "$last/devices_deferred"
}
umount() {
    printf '%s\n' "$*" >> "$TMP/unmount-calls"
    rm "$1/devices_deferred"
}
gbos_report_camera_debug > "$TMP/probe-report"
grep -q '^i2c-OVTI8856:00: mock deferred$' "$TMP/probe-report"
grep -q '^-t debugfs -o ro,nosuid,nodev,noexec debugfs ' "$TMP/mount-calls"
test "$(wc -l < "$TMP/unmount-calls")" -eq 1
test -z "$(find "$TMP" -maxdepth 1 -name 'gbos-camera-debug.*' -print)"
FAIL_MOUNT=1
if gbos_report_camera_debug > "$TMP/probe-report"; then
    echo "camera debugfs mount failure was not reported" >&2
    exit 1
fi
grep -q '^ERROR: could not mount temporary read-only debugfs$' "$TMP/probe-report"
grep -q '^== deferred probes unavailable ' "$TMP/probe-report"
test -z "$(find "$TMP" -maxdepth 1 -name 'gbos-camera-debug.*' -print)"
FAIL_MOUNT=0
printf 'i2c-OVTI5678:00: existing mount\n' > "$GBOS_SYSFS_ROOT/kernel/debug/devices_deferred"
gbos_report_camera_debug > "$TMP/probe-report"
grep -q '^i2c-OVTI5678:00: existing mount$' "$TMP/probe-report"
test "$(wc -l < "$TMP/mount-calls")" -eq 2
test "$(wc -l < "$TMP/unmount-calls")" -eq 1
grep -q '^service gbos_cameradiag .*gbos_report_camera_debug.*camera.txt' "$ROOT/usb/hook.sh"
grep -q '^service gbos_cameradiag .*gbos_report_camera_media_graph.*camera.txt' "$ROOT/usb/hook.sh"
! grep '^service gbos_cameradiag ' "$ROOT/usb/hook.sh" | grep -q 'sleep 90'
grep -q '^    start gbos_cameradiag$' "$ROOT/usb/hook.sh"

# Simulate a non-Surface machine with /dev/media0 present. No bridge side effects
# should run; the first echo after the guard fails this isolated shell.
bash -c '
    grep() { return 1; }
    function [ {
        case "$*" in "! -e /dev/media0 ]") return 1;; *) exit 98;; esac
    }
    echo() { exit 99; }
    . "$1"
' sh "$ROOT/usb/surface/surface-camera.sh"

echo IPU6_MODULE_LOAD_TEST_PASS
