#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

: > "$TMP/base.config"
python3 "$ROOT/kernel/configure_whl.py" --generic "$TMP/base.config" "$TMP/config"
grep -q '^CONFIG_MEDIA_PCI_SUPPORT=y$' "$TMP/config"
grep -q '^CONFIG_IPU_BRIDGE=m$' "$TMP/config"
grep -q '^CONFIG_VIDEO_INTEL_IPU6=m$' "$TMP/config"
grep -q '^CONFIG_VIDEO_OV5675=m$' "$TMP/config"
grep -q '^CONFIG_VIDEO_OV8856=m$' "$TMP/config"
grep -q '^CONFIG_VIDEO_DW9714=m$' "$TMP/config"
grep -q '^CONFIG_INTEL_SKL_INT3472=m$' "$TMP/config"
for setting in GPIO_TPS68470 REGULATOR_TPS68470 COMMON_CLK_TPS68470; do
    grep -q "^CONFIG_${setting}=m$" "$TMP/config"
done
python3 "$ROOT/kernel/configure_whl.py" --check "$TMP/config" > "$TMP/check.log" 2>&1 || {
    cat "$TMP/check.log" >&2
    exit 1
}

sed 's/^CONFIG_VIDEO_INTEL_IPU6=m$/# CONFIG_VIDEO_INTEL_IPU6 is not set/' \
    "$TMP/config" > "$TMP/broken.config"
if python3 "$ROOT/kernel/configure_whl.py" --check "$TMP/broken.config" > "$TMP/broken.log" 2>&1; then
    echo "camera config check accepted a disabled IPU6 driver" >&2
    exit 1
fi
grep -q 'VIDEO_INTEL_IPU6' "$TMP/broken.log"

sed 's/^CONFIG_VIDEO_DW9714=m$/# CONFIG_VIDEO_DW9714 is not set/' \
    "$TMP/config" > "$TMP/broken.config"
if python3 "$ROOT/kernel/configure_whl.py" --check "$TMP/broken.config" > "$TMP/broken.log" 2>&1; then
    echo "camera config check accepted a disabled DW9714 VCM driver" >&2
    exit 1
fi
grep -q 'VIDEO_DW9714' "$TMP/broken.log"

sed 's/^CONFIG_INTEL_SKL_INT3472=m$/# CONFIG_INTEL_SKL_INT3472 is not set/' \
    "$TMP/config" > "$TMP/broken.config"
if python3 "$ROOT/kernel/configure_whl.py" --check "$TMP/broken.config" > "$TMP/broken.log" 2>&1; then
    echo "camera config check accepted a disabled INT3472 controller" >&2
    exit 1
fi
grep -q 'INTEL_SKL_INT3472' "$TMP/broken.log"

for setting in GPIO_TPS68470 REGULATOR_TPS68470 COMMON_CLK_TPS68470; do
    sed "s/^CONFIG_${setting}=m$/# CONFIG_${setting} is not set/" "$TMP/config" > "$TMP/broken.config"
    if python3 "$ROOT/kernel/configure_whl.py" --check "$TMP/broken.config" > "$TMP/broken.log" 2>&1; then
        echo "camera config check accepted a disabled $setting driver" >&2
        exit 1
    fi
    grep -q "$setting" "$TMP/broken.log"
done

echo CAMERA_KERNEL_CONFIG_TEST_PASS
