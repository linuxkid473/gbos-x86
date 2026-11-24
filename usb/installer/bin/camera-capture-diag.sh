#!/system/bin/sh
# Opt-in live-USB diagnostic; records metadata only, never raw image data.
out=$1
sensor=$2
helper=/postinstall/gbos/installer/bin/camera-capture-probe
{
    date
    echo "== bounded IPU6 raw capture diagnostic"
    case "$sensor" in
        ov5675|ov8856) ;;
        *) echo "ERROR: invalid sensor selection"; exit 1 ;;
    esac
    if [ "$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null)" != "Dell Inc." ] ||
       [ "$(cat /sys/class/dmi/id/product_name 2>/dev/null)" != "Latitude 7320 Detachable" ]; then
        echo "ERROR: unsupported machine"
        exit 1
    fi
    if [ ! -x "$helper" ] || [ ! -e /dev/media0 ]; then
        echo "ERROR: capture helper or /dev/media0 missing"
        exit 1
    fi
    if ! command -v timeout >/dev/null 2>&1; then
        echo "ERROR: timeout is unavailable; capture not attempted"
        exit 1
    fi
    echo "== kernel messages before capture"
    dmesg | grep -iE 'ov5675|ov8856|ipu6|i2c|tps68470' | tail -100
    echo "== $sensor capture (45-second watchdog)"
    trace=/postinstall/gbos/installer/bin/camera-fw-trace.sh
    if [ "$sensor" = ov5675 ] && [ -f "$trace" ]; then
        /system/bin/sh "$trace" "$helper" "$sensor"
    else
        [ "$sensor" != ov5675 ] || echo "TRACE UNAVAILABLE: camera firmware tracer missing"
        timeout 45 "$helper" /dev/media0 "$sensor"
    fi
    rc=$?
    echo "$sensor capture helper exit=$rc"
    if [ "$rc" -eq 3 ] || [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        echo "ERROR: cleanup incomplete or watchdog terminated capture; graph state uncertain; reboot live USB"
    fi
    echo "== kernel messages after capture"
    dmesg | grep -iE 'ov5675|ov8856|ipu6|i2c|tps68470' | tail -200
    exit "$rc"
} > "$out" 2>&1
