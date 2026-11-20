#!/bin/sh
# Build the gbOS libcamera pipeline-handler plugins (kernel/libcamera-gbos/*.cpp) for Google's
# Android camera HAL (camera.libcamera_hal_base.so, libcamera 0.2.5 from AOSP external/libcamera,
# loaded by the vendor's ICameraProvider/legacy/0 service).
#
# Needs, all local and never committed:
#  - the AOSP libcamera source (android17-release) in build/src/libcamera (git clone of
#    https://android.googlesource.com/platform/external/libcamera);
#  - the Android NDK (r27);
#  - Google's vendor image in lapis/parts (the plugin links against its libc++.so and
#    camera.libcamera_hal_base.so; copied to build/libcamera-vendor).
# The plugin must use the platform libc++ ABI (std::__1, like the vendor libraries), not the
# NDK's std::__ndk1, so the NDK's libc++ headers are used with an overriding __config_site.
# Defines as Android.bp's libcamera_src_base_defaults (what libcamera's own sources see).
# --no-undefined: every symbol the plugin needs must exist in those vendor libraries.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAIN=$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir | sed 's#/\.git$##')
SRC=${LIBCAMERA_SRC:-$MAIN/build/src/libcamera}
GEN=${LIBCAMERA_GEN:-$MAIN/build/src/libcamera-gen}
VLIB=$MAIN/build/libcamera-vendor
NDK=${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.3.13750724}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
# GBOS_LIBCAMERA_OUT: build somewhere else (the test compares against the committed files).
OUT=${GBOS_LIBCAMERA_OUT:-$ROOT/usb/installer/lib/libcamera-pipeline}
[ -f "$SRC/include/libcamera/internal/pipeline_handler.h" ] || { echo "missing libcamera source at $SRC" >&2; exit 1; }

# Generated headers, as Android.bp's genrules make them.
if [ ! -f "$GEN/include/libcamera/control_ids.h" ]; then
	mkdir -p "$GEN/include/libcamera/internal"
	(cd "$SRC" &&
	 python3 utils/codegen/gen-controls.py -o "$GEN/include/libcamera/control_ids.h" --mode controls \
		-t include/libcamera/control_ids.h.in -r src/libcamera/control_ranges.yaml \
		src/libcamera/control_ids_core.yaml src/libcamera/control_ids_draft.yaml &&
	 python3 utils/codegen/gen-controls.py -o "$GEN/include/libcamera/property_ids.h" --mode properties \
		-t include/libcamera/control_ids.h.in -r src/libcamera/control_ranges.yaml \
		src/libcamera/property_ids_core.yaml src/libcamera/property_ids_draft.yaml &&
	 python3 utils/codegen/gen-formats.py -o "$GEN/include/libcamera/formats.h" \
		src/libcamera/formats.yaml include/libcamera/formats.h.in include/linux/drm_fourcc.h &&
	 python3 utils/codegen/gen-tp-header.py "$GEN/include/libcamera/internal/tracepoints.h" \
		include/libcamera/internal/tracepoints.h.in include/libcamera/internal/tracepoints/pipeline.perfetto \
		include/libcamera/internal/tracepoints/request.perfetto)
fi

# Vendor libraries to link against (from Google's image; local only).
if [ ! -f "$VLIB/camera.libcamera_hal_base.so" ]; then
	mkdir -p "$VLIB"
	docker run --rm --network none -v "$MAIN/lapis/parts:/parts:ro" -v "$VLIB:/out" gbos-erofs sh -c '
		mkdir -p /tmp/v && fsck.erofs --extract=/tmp/v /parts/vendor_a.img >/dev/null 2>&1
		cp /tmp/v/lib64/libc++.so /tmp/v/lib64/hw/camera.libcamera_hal_base.so /out/ && chmod a+r /out/*'
fi

# libc++ headers with the platform ABI namespace.
CFG=$MAIN/build/libcamera-libcxx-config
mkdir -p "$CFG"
sed 's/^#define _LIBCPP_ABI_NAMESPACE __ndk1$/#define _LIBCPP_ABI_NAMESPACE __1/' \
	"$TC/sysroot/usr/include/c++/v1/__config_site" > "$CFG/__config_site"
grep -q '_LIBCPP_ABI_NAMESPACE __1$' "$CFG/__config_site"

# Reproducible and free of local paths (__FILE__ is embedded by libcamera's LOG and ASSERT
# macros): compile the relative source path from $ROOT, and map the absolute include paths.
mkdir -p "$OUT"
cd "$ROOT"
for src in kernel/libcamera-gbos/*.cpp; do
	name=$(basename "$src" .cpp)
	"$TC/bin/clang++" --target=x86_64-linux-android30 -std=c++20 -O2 -fPIC -shared \
		-ffile-prefix-map="$MAIN/build/src/=" -ffile-prefix-map="$MAIN/build/=" \
		-ffile-prefix-map="$NDK/=ndk/" -ffile-prefix-map="$ROOT/=" \
		-fno-exceptions -frtti -Wall -Wextra -Werror -Wno-unused-parameter \
		-DLIBCAMERA_BASE_PRIVATE -DHAVE_LOCALE_T=1 -DHAVE_ANDROID_OS \
		-isystem "$CFG" -I "$GEN/include" -I "$SRC/include" -I "$SRC/src/libcamera" \
		-nostdlib++ -Wl,--no-undefined -Wl,-soname,"$name.so" \
		-o "$OUT/$name.so" "$src" "$VLIB/libc++.so" "$VLIB/camera.libcamera_hal_base.so" -llog
	"$TC/bin/llvm-strip" --strip-unneeded "$OUT/$name.so"
	if strings -n 6 "$OUT/$name.so" | grep -q -e "$HOME" -e "$MAIN"; then
		echo "$name.so embeds a local path" >&2
		exit 1
	fi
	echo "$OUT/$name.so"
done
