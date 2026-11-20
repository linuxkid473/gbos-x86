#!/bin/sh
# GPU converter vs CPU converter on the host, with Mesa's lavapipe (CPU Vulkan). Needs the
# Android NDK (Vulkan headers, glslc) and the host Vulkan loader + lavapipe. Exit 77 = skipped.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.3.13750724}
INC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include
ICD=${LVP_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
if [ ! -d "$INC/vulkan" ] || [ ! -f "$ICD" ]; then
	echo "IPU6_GPU_TEST_SKIP (needs the NDK's Vulkan headers and lavapipe)"
	exit 77
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# The committed SPIR-V must match the shader sources.
if [ -x "$NDK/shader-tools/linux-x86_64/glslc" ]; then
	sh "$ROOT/tools/build_ipu6_gpu_shaders.sh" "$tmp/spirv.h" >/dev/null
	cmp -s "$tmp/spirv.h" "$ROOT/kernel/ipu6-gpu/spirv.h" || {
		echo "kernel/ipu6-gpu/spirv.h is stale: run tools/build_ipu6_gpu_shaders.sh" >&2
		exit 1
	}
fi
# Only the Vulkan headers from the NDK (its other headers are bionic's).
mkdir -p "$tmp/inc"
ln -s "$INC/vulkan" "$tmp/inc/vulkan"
ln -s "$INC/vk_video" "$tmp/inc/vk_video"
gcc -O2 -Wall -Wextra -Werror -Wno-unused-function -pthread -I "$tmp/inc" -o "$tmp/test-gpu" "$ROOT/tools/test_ipu6_gpu.c" -lm -ldl
VK_ICD_FILENAMES=$ICD "$tmp/test-gpu"
