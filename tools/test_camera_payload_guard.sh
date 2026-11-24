#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

USB=$TMP/usb
KREL=6.18.21-gbos-whl
CONFIG=$TMP/config
MODDIR=$USB/modules/lib/modules/$KREL
cat > "$CONFIG" <<'EOF'
CONFIG_MEDIA_PCI_SUPPORT=y
CONFIG_IPU_BRIDGE=m
CONFIG_VIDEO_INTEL_IPU6=m
CONFIG_VIDEO_OV5675=m
CONFIG_VIDEO_OV8856=m
CONFIG_VIDEO_DW9714=m
CONFIG_INTEL_SKL_INT3472=m
CONFIG_GPIO_TPS68470=m
CONFIG_REGULATOR_TPS68470=m
CONFIG_COMMON_CLK_TPS68470=m
EOF

mkdir -p "$USB/firmware/intel/ipu"
cp "$ROOT/usb/firmware/intel/ipu/ipu6_fw.bin" "$USB/firmware/intel/ipu/ipu6_fw.bin"
mkdir -p "$MODDIR"
touch "$MODDIR/modules.dep"

if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/missing.log" 2>&1; then
    echo "camera payload check accepted a missing IPU6 module tree" >&2
    exit 1
fi
grep -q 'missing module' "$TMP/missing.log"

for module in \
    kernel/drivers/media/pci/intel/ipu-bridge.ko \
    kernel/drivers/media/pci/intel/ipu6/intel-ipu6.ko \
    kernel/drivers/media/pci/intel/ipu6/intel-ipu6-isys.ko \
    kernel/drivers/media/i2c/ov5675.ko \
    kernel/drivers/media/i2c/ov8856.ko \
    kernel/drivers/media/i2c/dw9714.ko \
    kernel/drivers/gpio/gpio-tps68470.ko \
    kernel/drivers/regulator/tps68470-regulator.ko \
    kernel/drivers/clk/clk-tps68470.ko \
    kernel/drivers/platform/x86/intel/int3472/intel_skl_int3472_common.ko \
    kernel/drivers/platform/x86/intel/int3472/intel_skl_int3472_discrete.ko \
    kernel/drivers/platform/x86/intel/int3472/intel_skl_int3472_tps68470.ko; do
    mkdir -p "$MODDIR/$(dirname "$module")"
    : > "$MODDIR/$module"
    printf '%s:\n' "$module" >> "$MODDIR/modules.dep"
done
printf 'alias pci:v00008086d00009A19sv*sd*bc*sc*i* intel_ipu6\nalias acpi*:OVTI5678:* ov5675\nalias i2c:dw9714 dw9714\n' > "$MODDIR/modules.alias"
sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" | grep -q '^CAMERA_PAYLOAD_OK '

grep -Fv 'kernel/drivers/media/i2c/dw9714.ko:' "$MODDIR/modules.dep" > "$TMP/modules.dep"
cp "$TMP/modules.dep" "$MODDIR/modules.dep"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/vcm-dep.log" 2>&1; then
    echo "camera payload check accepted a missing DW9714 modules.dep entry" >&2
    exit 1
fi
grep -q 'modules.dep does not include kernel/drivers/media/i2c/dw9714.ko' "$TMP/vcm-dep.log"
printf 'kernel/drivers/media/i2c/dw9714.ko:\n' >> "$MODDIR/modules.dep"

printf 'alias pci:v00008086d00009A19sv*sd*bc*sc*i* intel_ipu6\nalias acpi*:OVTI5678:* ov5675\n' > "$MODDIR/modules.alias"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/vcm-alias.log" 2>&1; then
    echo "camera payload check accepted missing DW9714 VCM alias" >&2
    exit 1
fi
grep -q 'do not include the DW9714 VCM driver' "$TMP/vcm-alias.log"
printf 'alias pci:v00008086d00009A19sv*sd*bc*sc*i* intel_ipu6\nalias acpi*:OVTI5678:* ov5675\nalias i2c:dw9714 dw9714\n' > "$MODDIR/modules.alias"
sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" | grep -q '^CAMERA_PAYLOAD_OK '

printf 'alias pci:v00008086d00009A19sv*sd*bc*sc*i* intel_ipu6\nalias acpi*:OVTI5678:* ov5675\nalias i2c:dw9714 wrong_module\n' > "$MODDIR/modules.alias"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/vcm-alias-target.log" 2>&1; then
    echo "camera payload check accepted DW9714 alias mapped to another module" >&2
    exit 1
fi
grep -q 'do not include the DW9714 VCM driver' "$TMP/vcm-alias-target.log"
printf 'alias pci:v00008086d00009A19sv*sd*bc*sc*i* intel_ipu6\nalias acpi*:OVTI5678:* ov5675\nalias i2c:dw9714 dw9714\n' > "$MODDIR/modules.alias"

rm "$MODDIR/kernel/drivers/platform/x86/intel/int3472/intel_skl_int3472_discrete.ko"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/int3472.log" 2>&1; then
    echo "camera payload check accepted a missing INT3472 controller module" >&2
    exit 1
fi
grep -q 'intel_skl_int3472_discrete.ko' "$TMP/int3472.log"
: > "$MODDIR/kernel/drivers/platform/x86/intel/int3472/intel_skl_int3472_discrete.ko"

printf '' > "$MODDIR/modules.alias"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/alias.log" 2>&1; then
    echo "camera payload check accepted a module tree without the IPU6 PCI alias" >&2
    exit 1
fi
grep -q 'do not include IPU6 PCI ID 8086:9a19' "$TMP/alias.log"
grep -q 'do not include front sensor ACPI ID OVTI5678' "$TMP/alias.log"
printf 'alias pci:v00008086d00009A19sv*sd*bc*sc*i* intel_ipu6\nalias acpi*:OVTI5678:* ov5675\nalias i2c:dw9714 dw9714\n' > "$MODDIR/modules.alias"

rm "$MODDIR/kernel/drivers/media/i2c/ov5675.ko"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/ov5675.log" 2>&1; then
    echo "camera payload check accepted a missing OV5675 front sensor driver" >&2
    exit 1
fi
grep -q 'kernel/drivers/media/i2c/ov5675.ko' "$TMP/ov5675.log"
: > "$MODDIR/kernel/drivers/media/i2c/ov5675.ko"

sed 's/^CONFIG_VIDEO_OV8856=m$/# CONFIG_VIDEO_OV8856 is not set/' "$CONFIG" > "$TMP/broken.config"
if sh "$ROOT/tools/check_camera_payload.sh" "$TMP/broken.config" "$KREL" "$USB" > "$TMP/config.log" 2>&1; then
    echo "camera payload check accepted a disabled OV8856 driver" >&2
    exit 1
fi
grep -q 'CONFIG_VIDEO_OV8856=m' "$TMP/config.log"

sed 's/^CONFIG_VIDEO_OV5675=m$/# CONFIG_VIDEO_OV5675 is not set/' "$CONFIG" > "$TMP/broken.config"
if sh "$ROOT/tools/check_camera_payload.sh" "$TMP/broken.config" "$KREL" "$USB" > "$TMP/ov5675-config.log" 2>&1; then
    echo "camera payload check accepted a disabled OV5675 driver" >&2
    exit 1
fi
grep -q 'CONFIG_VIDEO_OV5675=m' "$TMP/ov5675-config.log"

for pair in \
    'GPIO_TPS68470:kernel/drivers/gpio/gpio-tps68470.ko' \
    'REGULATOR_TPS68470:kernel/drivers/regulator/tps68470-regulator.ko' \
    'COMMON_CLK_TPS68470:kernel/drivers/clk/clk-tps68470.ko' \
    'VIDEO_DW9714:kernel/drivers/media/i2c/dw9714.ko'; do
    setting=${pair%%:*}
    module=${pair#*:}
    sed "s/^CONFIG_${setting}=m$/# CONFIG_${setting} is not set/" "$CONFIG" > "$TMP/broken.config"
    if sh "$ROOT/tools/check_camera_payload.sh" "$TMP/broken.config" "$KREL" "$USB" > "$TMP/child-config.log" 2>&1; then
        echo "camera payload check accepted disabled $setting" >&2
        exit 1
    fi
    grep -q "CONFIG_${setting}=m" "$TMP/child-config.log"
    rm "$MODDIR/$module"
    if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/child-module.log" 2>&1; then
        echo "camera payload check accepted missing $module" >&2
        exit 1
    fi
    grep -q "$module" "$TMP/child-module.log"
    : > "$MODDIR/$module"
done

printf bad > "$USB/firmware/intel/ipu/ipu6_fw.bin"
if sh "$ROOT/tools/check_camera_payload.sh" "$CONFIG" "$KREL" "$USB" > "$TMP/firmware.log" 2>&1; then
    echo "camera payload check accepted a mismatched IPU6 firmware blob" >&2
    exit 1
fi
grep -q 'does not match the pinned Intel firmware' "$TMP/firmware.log"

echo CAMERA_PAYLOAD_GUARD_TEST_PASS
