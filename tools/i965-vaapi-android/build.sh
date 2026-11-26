#!/bin/sh
# Build Intel's Gen 7 / 7.5 (Haswell) / 8 (Broadwell) VA-API driver (`i965_drv_video.so`)
# for Android 17 Codec2 (`c2.cros-codecs.vaapi.*`, x86_64) with the Android NDK.
# Google's vendor image only ships `iHD_drv_video.so` (Gen 9+ Skylake+);
# hook.sh binds `i965_drv_video.so` over `/vendor/lib64/iHD_drv_video.so` on Intel Gen 7/7.5/8 GPUs.
#
#   tools/i965-vaapi-android/build.sh [OUT_DIR]
#
# Shares the gbos-swsr docker volume (Android NDK r30, libdrm 2.4.133, intel-vaapi-driver fd727a4e)
# with tools/hasvk-android/build.sh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../build/i965-vaapi}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
docker build --platform linux/amd64 -q -t gbos-i965build - <<'DOCKERFILE' >/dev/null
FROM --platform=linux/amd64 debian:trixie-slim
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    curl ca-certificates unzip git ninja-build meson pkgconf \
    python3 xz-utils make file binutils build-essential libva-dev >/dev/null && rm -rf /var/lib/apt/lists/*
WORKDIR /w
DOCKERFILE
docker volume create gbos-swsr >/dev/null
docker run --rm --platform linux/amd64 -e KEEP_BUILD="${KEEP_BUILD:-1}" -v gbos-swsr:/w -v "$HERE":/h:ro -v "$OUT":/out gbos-i965build sh -ec '
  cd /w
  NDK=/w/android-ndk-r30
  if [ ! -d $NDK ]; then
    curl -fL --retry 5 -o ndk.zip https://dl.google.com/android/repository/android-ndk-r30-linux.zip
    echo "5107f898313790e449e87eee2183d9a20602dee9  ndk.zip" | sha1sum -c -
    unzip -q ndk.zip && rm -f ndk.zip
  fi
  DRM_TAG=libdrm-2.4.133
  I965_COMMIT=fd727a4e9cb8b2878a1e93d4dddc8dd1c1a4e0ea
  if [ -d drm/.git ]; then
    git -C drm checkout -q -f $DRM_TAG
    git -C drm clean -fdx
  else
    rm -rf drm
    git clone -q --depth 1 --branch $DRM_TAG https://gitlab.freedesktop.org/mesa/drm.git drm
  fi
  if [ -d intel-vaapi-driver/.git ]; then
    git -C intel-vaapi-driver checkout -q -f $I965_COMMIT
    git -C intel-vaapi-driver clean -fdx
  else
    rm -rf intel-vaapi-driver
    git init -q intel-vaapi-driver
    git -C intel-vaapi-driver fetch -q --depth 1 https://github.com/intel/intel-vaapi-driver.git $I965_COMMIT
    git -C intel-vaapi-driver checkout -q -f FETCH_HEAD
  fi

  python3 /h/patch_i965.py /w/drm /w/intel-vaapi-driver

  TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
  SDK=34
  cat > /w/cross-i965.txt <<EOF
[binaries]
ar = "$TC/llvm-ar"
c = ["$TC/x86_64-linux-android$SDK-clang", "-march=ivybridge", "-mno-adx", "-mno-rdseed", "-mno-clflushopt", "-mno-clwb", "-fno-exceptions", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-Wno-deprecated-non-prototype", "-Wno-unused-but-set-variable"]
cpp = ["$TC/x86_64-linux-android$SDK-clang++", "-march=ivybridge", "-mno-adx", "-mno-rdseed", "-mno-clflushopt", "-mno-clwb", "-fno-exceptions", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-static-libstdc++"]
c_ld = "lld"
cpp_ld = "lld"
strip = "$TC/llvm-strip"
pkg-config = "pkgconf"
[host_machine]
system = "android"
cpu_family = "x86_64"
cpu = "x86_64"
endian = "little"
[properties]
needs_exe_wrapper = true
EOF
  tr "\042" "\047" < /w/cross-i965.txt > /w/cross-i965.tmp && mv /w/cross-i965.tmp /w/cross-i965.txt

  # 1. Build static libdrm.a + libdrm_intel.a (without libpciaccess)
  rm -rf /w/drm-i965-b /w/sys-i965; mkdir -p /w/sys-i965/lib/pkgconfig /w/sys-i965/include
  meson setup /w/drm-i965-b /w/drm --cross-file /w/cross-i965.txt --prefix=/w/sys-i965 --libdir=lib --default-library=static \
    -Dintel=enabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled \
    -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled -Dtegra=disabled \
    -Dudev=false -Dcairo-tests=disabled -Dvalgrind=disabled -Dtests=false -Dman-pages=disabled -Dinstall-test-programs=false >/w/drm-i965-setup.log 2>&1 || { tail -50 /w/drm-i965-setup.log; exit 1; }
  ninja -C /w/drm-i965-b -j"$(nproc)" install >/w/drm-i965-build.log 2>&1 || { tail -50 /w/drm-i965-build.log; exit 1; }

  # 2. Copy VA-API 1.22 headers (matching Android 17 /vendor/lib64/libva.so 1.22.0) + header-only pkg-config
  cp -a /usr/include/va /w/sys-i965/include/va
  cat > /w/sys-i965/lib/pkgconfig/libva.pc <<EOF
prefix=/w/sys-i965
includedir=\${prefix}/include
driverdir=/vendor/lib64

Name: libva
Description: Userspace Video Acceleration (VA) core interface (headers)
Version: 1.22.0
Cflags: -I\${includedir}
Libs:
EOF

  export PKG_CONFIG_PATH=/w/sys-i965/lib/pkgconfig PKG_CONFIG_LIBDIR=/w/sys-i965/lib/pkgconfig

  # 3. Build i965_drv_video.so
  rm -rf /w/i965-b
  meson setup /w/i965-b /w/intel-vaapi-driver --cross-file /w/cross-i965.txt --prefix=/w/sys-i965 --buildtype=release \
    -Dwith_x11=no -Dwith_wayland=no -Denable_hybrid_codec=false -Denable_tests=false -Ddriverdir=/vendor/lib64 >/w/i965-setup.log 2>&1 || { tail -60 /w/i965-setup.log; exit 1; }
  ninja -C /w/i965-b -j"$(nproc)" >/w/i965-build.log 2>&1 || { tail -80 /w/i965-build.log; exit 1; }

  F=$(find /w/i965-b -name "i965_drv_video.so" | head -n 1)
  [ -n "$F" ] || { echo "i965_drv_video.so not found"; exit 1; }
  $TC/llvm-strip --strip-unneeded -o /out/i965_drv_video.so "$F"
  ls -lh /out/i965_drv_video.so; sha256sum /out/i965_drv_video.so
  readelf -d /out/i965_drv_video.so | grep -E "NEEDED|SONAME"
  nm -D /out/i965_drv_video.so | grep vaDriverInit
'
