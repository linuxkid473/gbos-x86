#!/bin/sh
# gbOS libcamera plugins against Google's camera HAL: they rebuild (linking with --no-undefined
# against the vendor libc++.so and camera.libcamera_hal_base.so), the committed .so files match
# the rebuild, and the classes they derive from have the same vtable size as in the vendor
# library (same header revision as Google's build). Exit 77 when the NDK, the AOSP libcamera
# source or the vendor image (all local only) are missing.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAIN=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir | sed 's#/\.git$##')
# The NV12 scaler (kernel/libcamera-gbos/gbos_convert.h) runs on the build machine.
conv=$(mktemp -d)
gcc -O2 -Wall -Wextra -Werror -o "$conv/convert" "$ROOT/tools/test_libcamera_gbos_convert.c"
"$conv/convert" | grep -qx LIBCAMERA_GBOS_CONVERT_TEST_PASS || { rm -rf "$conv"; exit 1; }
rm -rf "$conv"
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.3.13750724}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
if [ ! -x "$TC/bin/clang++" ] || [ ! -d "$MAIN/build/src/libcamera/include" ] ||
   [ ! -f "$MAIN/lapis/parts/vendor_a.img" ]; then
	echo "LIBCAMERA_GBOS_TEST_SKIP (needs the NDK, build/src/libcamera and lapis/parts)"
	exit 77
fi
OUT=$ROOT/usb/installer/lib/libcamera-pipeline
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Rebuild into a temporary directory (never over the tracked files) and compare.
GBOS_LIBCAMERA_OUT=$tmp/rebuilt sh "$ROOT/tools/build_libcamera_gbos.sh" >/dev/null
for f in "$OUT"/*.so; do
	cmp -s "$f" "$tmp/rebuilt/$(basename "$f")" || {
		echo "$(basename "$f") differs from a rebuild: run tools/build_libcamera_gbos.sh" >&2
		exit 1
	}
	if strings -n 6 "$f" | grep -q -e /home/ -e "$MAIN"; then
		echo "$(basename "$f") embeds a local path" >&2
		exit 1
	fi
done
# vtable sizes: the derived classes against the vendor's base classes.
VLIB=$MAIN/build/libcamera-vendor/camera.libcamera_hal_base.so
size() { "$TC/bin/llvm-readelf" -sW "$1" | awk -v s="$2" '$8 == s { print $3; exit }'; }
"$TC/bin/clang++" --target=x86_64-linux-android30 -std=c++20 -O2 -fPIC -shared -fno-exceptions -frtti \
	-DLIBCAMERA_BASE_PRIVATE -DHAVE_LOCALE_T=1 -DHAVE_ANDROID_OS \
	-isystem "$MAIN/build/libcamera-libcxx-config" -I "$MAIN/build/src/libcamera-gen/include" \
	-I "$MAIN/build/src/libcamera/include" -I "$MAIN/build/src/libcamera/src/libcamera" -nostdlib++ \
	-o "$tmp/test.so" "$ROOT/kernel/libcamera-gbos/gbos_test.cpp" "$ROOT/kernel/libcamera-gbos/gbos_ipu6.cpp" \
	"$MAIN/build/libcamera-vendor/libc++.so" "$VLIB" -llog
for pair in _ZTVN9libcamera15PipelineHandlerE:_ZTVN9libcamera23PipelineHandlerGbosTestE \
	    _ZTVN9libcamera6Camera7PrivateE:_ZTVN9libcamera18GbosTestCameraDataE \
	    _ZTVN9libcamera19CameraConfigurationE:_ZTVN9libcamera27GbosTestCameraConfigurationE \
	    _ZTVN9libcamera15PipelineHandlerE:_ZTVN9libcamera23PipelineHandlerGbosIpu6E \
	    _ZTVN9libcamera6Camera7PrivateE:_ZTVN9libcamera18GbosIpu6CameraDataE \
	    _ZTVN9libcamera19CameraConfigurationE:_ZTVN9libcamera27GbosIpu6CameraConfigurationE; do
	base=$(size "$VLIB" "${pair%%:*}"); ours=$(size "$tmp/test.so" "${pair#*:}")
	[ -n "$base" ] && [ "$base" = "$ours" ] || {
		echo "vtable size mismatch: ${pair%%:*} $base vs ${pair#*:} $ours" >&2
		exit 1
	}
done
# Only libraries the vendor HAL process can load.
for so in gbos_test.so gbos_ipu6.so; do
	"$TC/bin/llvm-readelf" -d "$OUT/$so" | awk '/NEEDED/ { print $5 }' | tr -d '[]' | sort |
		tr '\n' ' ' | grep -qx 'camera.libcamera_hal_base.so libc++.so libc.so libdl.so liblog.so libm.so ' || {
		echo "$so needs a library the camera HAL process may not have" >&2
		exit 1
	}
done
echo "LIBCAMERA_GBOS_TEST_PASS"
