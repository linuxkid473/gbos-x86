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
cd "$ROOT"	# relative source path: no local paths in the binary
# The default size (1920x1080), the CAMERA_SIZE builds (installer/bin/<size>/vulkan/) and the
# libcamera rear's 4:3 build (1920x1440, CAMERA_LIBCAMERA=1).
OUT720=$ROOT/usb/installer/bin/1280x720/vulkan/ipu6-loopback
OUT640=$ROOT/usb/installer/bin/640x480/vulkan/ipu6-loopback
OUT1440=$ROOT/usb/installer/bin/1920x1440/vulkan/ipu6-loopback
mkdir -p "$(dirname "$OUT720")" "$(dirname "$OUT640")" "$(dirname "$OUT1440")"
for target in "$OUT:" "$OUT720:-DOUT_WIDTH=1280 -DOUT_HEIGHT=720" "$OUT640:-DOUT_WIDTH=640 -DOUT_HEIGHT=480" \
	"$OUT1440:-DOUT_WIDTH=1920 -DOUT_HEIGHT=1440"; do
	out=${target%%:*} size=${target#*:}
	# shellcheck disable=SC2086
	"$BIN/x86_64-linux-android$API-clang" -O2 -pthread -Wall -Wextra -Werror -DIPU6_GPU $size \
		-ffile-prefix-map="$ROOT/=" -ffile-prefix-map="$NDK/=ndk/" \
		-o "$out" kernel/ipu6-loopback.c -lm -ldl
	"$BIN/llvm-strip" "$out"
	if strings -n 6 "$out" | grep -q -e "$HOME" -e /home/; then
		echo "$out embeds a local path" >&2
		exit 1
	fi
done
file "$OUT720" "$OUT640" "$OUT1440"
file "$OUT"
