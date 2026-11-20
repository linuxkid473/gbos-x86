#!/bin/sh
# Android-native (bionic) build of the camera bridge with the Vulkan compute path (-DIPU6_GPU):
# usb/installer/bin/vulkan/ipu6-loopback. The static glibc build (tools/build_ipu6_loopback.sh)
# stays as the CPU fallback; the launcher picks this one only when `--probe-gpu` succeeds.
# Needs the Android NDK (r27: ANDROID_NDK, default ~/Android/Sdk/ndk/27.3.13750724).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.3.13750724}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
API=30
OUT=$ROOT/usb/installer/bin/vulkan/ipu6-loopback
mkdir -p "$(dirname "$OUT")"
"$BIN/x86_64-linux-android$API-clang" -O2 -pthread -Wall -Wextra -Werror -DIPU6_GPU \
	-o "$OUT" "$ROOT/kernel/ipu6-loopback.c" -lm -ldl
"$BIN/llvm-strip" "$OUT"
file "$OUT"
