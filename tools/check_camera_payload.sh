#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 KERNEL_CONFIG KERNEL_RELEASE USB_ROOT" >&2
    exit 2
fi

CONFIG=$1
KREL=$2
USB=$3
MODDIR=$USB/modules/lib/modules/$KREL
IPU6_FW_SHA256=70cbe88d075dc03938a94a897912409c5fdd56dc987b3db2a9a2a867bb2f5f99

if [ ! -s "$CONFIG" ]; then
    echo "camera payload: embedded kernel config is missing" >&2
    exit 1
fi
if [ -z "$KREL" ] || [ ! -d "$MODDIR" ]; then
    echo "camera payload: no module tree for kernel release '${KREL:-unknown}'" >&2
    exit 1
fi
if [ ! -f "$MODDIR/modules.dep" ]; then
    echo "camera payload: missing $MODDIR/modules.dep" >&2
    exit 1
fi

missing=0
for setting in \
    CONFIG_MEDIA_PCI_SUPPORT=y \
    CONFIG_IPU_BRIDGE=m \
    CONFIG_VIDEO_INTEL_IPU6=m \
    CONFIG_VIDEO_OV5675=m \
    CONFIG_VIDEO_OV8856=m \
    CONFIG_VIDEO_DW9714=m \
    CONFIG_INTEL_SKL_INT3472=m \
    CONFIG_GPIO_TPS68470=m \
    CONFIG_REGULATOR_TPS68470=m \
    CONFIG_COMMON_CLK_TPS68470=m; do
    if ! grep -Fxq "$setting" "$CONFIG"; then
        echo "camera payload: kernel config is missing $setting" >&2
        missing=1
    fi
done

if ! grep -qi 'pci:v00008086d00009A19' "$MODDIR/modules.alias"; then
    echo "camera payload: module aliases do not include IPU6 PCI ID 8086:9a19" >&2
    missing=1
fi
if ! grep -qi 'acpi.*OVTI5678' "$MODDIR/modules.alias"; then
    echo "camera payload: module aliases do not include front sensor ACPI ID OVTI5678" >&2
    missing=1
fi
if ! awk '$1 == "alias" && $2 == "i2c:dw9714" && $3 == "dw9714" { found = 1 }
    END { exit !found }' "$MODDIR/modules.alias"; then
    echo "camera payload: module aliases do not include the DW9714 VCM driver" >&2
    missing=1
fi

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
    if [ ! -f "$MODDIR/$module" ] &&
       [ ! -f "$MODDIR/$module.xz" ] &&
       [ ! -f "$MODDIR/$module.zst" ]; then
        echo "camera payload: missing module $MODDIR/$module" >&2
        missing=1
    fi
    if ! grep -Fq "$module:" "$MODDIR/modules.dep" &&
       ! grep -Fq "$module.xz:" "$MODDIR/modules.dep" &&
       ! grep -Fq "$module.zst:" "$MODDIR/modules.dep"; then
        echo "camera payload: modules.dep does not include $module" >&2
        missing=1
    fi
done

FW=$USB/firmware/intel/ipu/ipu6_fw.bin
if [ ! -f "$FW" ]; then
    echo "camera payload: missing $FW (run tools/fetch_firmware.sh)" >&2
    missing=1
else
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$FW" | awk '{print $1}')
    elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$FW" | awk '{print $1}')
    else
        echo "camera payload: need sha256sum or shasum to verify $FW" >&2
        exit 1
    fi
    if [ "$actual" != "$IPU6_FW_SHA256" ]; then
        echo "camera payload: $FW does not match the pinned Intel firmware SHA-256" >&2
        missing=1
    fi
fi

if [ "$missing" -ne 0 ]; then
    echo "camera payload: rebuild the kernel with sh kernel/build_kernel.sh before creating an image" >&2
    exit 1
fi

echo "CAMERA_PAYLOAD_OK ($KREL)"
