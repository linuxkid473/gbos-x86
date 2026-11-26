#!/bin/sh
# Build Mesa's Intel Gen 7 / 7.5 (Haswell) / 8 (Broadwell) Vulkan driver (`intel_hasvk`)
# as an Android vendor Vulkan HAL ("vulkan.hasvk.so", x86_64) with the NDK.
# Google's shipped vulkan.intel.so (`anv`) only supports Intel Gen 9+ (Skylake+);
# hook.sh binds vulkan.hasvk.so over /vendor/lib64/hw/vulkan.intel.so on Intel Gen 7/7.5/8 GPUs.
#
#   tools/hasvk-android/build.sh [OUT_DIR]
#
# Shares the gbos-swsr docker volume (Android NDK r30, AOSP headers, Mesa b888682c, libdrm 2.4.133)
# with tools/radv-android/build.sh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../../build/hasvk}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
docker build --platform linux/amd64 -q -t gbos-hasvkbuild - <<'DOCKERFILE' >/dev/null
FROM --platform=linux/amd64 debian:trixie-slim
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    curl ca-certificates unzip git ninja-build meson pkgconf bison flex \
    python3 python3-mako python3-yaml python3-packaging python3-ply glslang-tools \
    xz-utils make file binutils build-essential cmake zlib1g-dev libexpat1-dev libdrm-dev \
    llvm-19-dev clang-19 libclang-19-dev libclang-cpp19-dev libclc-19-dev libllvmspirvlib-19-dev spirv-tools >/dev/null && rm -rf /var/lib/apt/lists/*
WORKDIR /w
DOCKERFILE
docker volume create gbos-swsr >/dev/null
docker run --rm --platform linux/amd64 -e KEEP_BUILD="${KEEP_BUILD:-1}" -v gbos-swsr:/w -v "$HERE":/h:ro -v "$OUT":/out gbos-hasvkbuild sh -ec '
  cd /w
  NDK=/w/android-ndk-r30
  if [ ! -d $NDK ]; then
    curl -fL --retry 5 -o ndk.zip https://dl.google.com/android/repository/android-ndk-r30-linux.zip
    echo "5107f898313790e449e87eee2183d9a20602dee9  ndk.zip" | sha1sum -c -
    unzip -q ndk.zip && rm -f ndk.zip
  fi
  mkdir -p aosp && cd aosp
  get() { n=$1; u=$2; s=$3; shift 3
    [ -d $n ] || { git init -q $n; git -C $n remote add origin $u; git -C $n config core.sparseCheckout true
      for p in "$@"; do echo "/$p/" >> $n/.git/info/sparse-checkout; done
      git -C $n fetch -q --depth 1 --filter=blob:none origin $s; git -C $n checkout -q FETCH_HEAD; }; }
  B=https://android.googlesource.com
  get libhardware $B/platform/hardware/libhardware 7e08958a6cbe2b02c9d80a66fba68fd8a864d73d include include_all
  get core $B/platform/system/core a3b721a32242006b59cb12bd62c9133632af3a2d libcutils/include libsync/include libsystem/include libutils/include liblog/include libvndksupport/include
  get native $B/platform/frameworks/native 4f463a6b1de9198963dc6aff74154a504ba3f8f6 libs/nativewindow/include libs/arect/include vulkan/include
  cd /w
  MESA_TAG=b888682cba909c477f405e377cee881ca68e289d
  DRM_TAG=libdrm-2.4.133
  if [ -d mesa/.git ]; then
    if [ "${KEEP_BUILD:-0}" != 1 ] || [ ! -d /w/mesa-hasvk-b ]; then
      git -C mesa checkout -q -f $MESA_TAG
      git -C mesa clean -fdx
    else
      git -C mesa checkout -q -- src/intel/vulkan_hasvk src/vulkan/runtime/vk_android.c
    fi
  else
    rm -rf mesa
    git init -q mesa
    git -C mesa fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git $MESA_TAG
    git -C mesa checkout -q -f FETCH_HEAD
  fi
  if [ -d drm/.git ]; then
    git -C drm checkout -q -f $DRM_TAG
    git -C drm clean -fdx
  else
    rm -rf drm
    git clone -q --depth 1 --branch $DRM_TAG https://gitlab.freedesktop.org/mesa/drm.git drm
  fi
  # Build host mesa_clc + vtn_bindgen2 once (blorp_shaders.cl needs them at build time; vulkan.hasvk.so itself has no LLVM)
  if [ ! -x /w/clc-host-out/bin/mesa_clc ] || [ ! -x /w/clc-host-out/bin/vtn_bindgen2 ]; then
    rm -rf /w/clc-host-b /w/clc-host-out
    PATH=/usr/lib/llvm-19/bin:$PATH meson setup /w/clc-host-b /w/mesa --prefix=/w/clc-host-out --buildtype=release \
      -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dllvm=enabled \
      -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
      -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dglvnd=disabled -Dtools= >/w/clc-host-setup.log 2>&1 || { tail -50 /w/clc-host-setup.log; exit 1; }
    ninja -C /w/clc-host-b -j"$(nproc)" install >/w/clc-host-build.log 2>&1 || { tail -50 /w/clc-host-build.log; exit 1; }
  fi
  export PATH=/w/clc-host-out/bin:$PATH
  A=/w/aosp
  INC="$A/libhardware/include:$A/libhardware/include_all:$A/core/libcutils/include:$A/core/libsync/include:$A/core/libsystem/include:$A/core/libutils/include:$A/core/liblog/include:$A/core/libvndksupport/include:$A/native/libs/nativewindow/include:$A/native/libs/arect/include:$A/native/vulkan/include"
  export CPLUS_INCLUDE_PATH=$INC C_INCLUDE_PATH=$INC
  TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
  SDK=34
  cat > cross-hasvk.txt <<EOF
[binaries]
ar = "$TC/llvm-ar"
c = ["$TC/x86_64-linux-android$SDK-clang", "-march=ivybridge", "-mno-adx", "-mno-rdseed", "-mno-clflushopt", "-mno-clwb", "-fno-exceptions", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables"]
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
  tr "\042" "\047" < cross-hasvk.txt > cross-hasvk.tmp && mv cross-hasvk.tmp cross-hasvk.txt
  if [ "${KEEP_BUILD:-0}" != 1 ] || [ ! -f /w/sys-hasvk/lib/libdrm.a ]; then
    rm -rf /w/drm-hasvk-b /w/sys-hasvk; mkdir -p /w/sys-hasvk
    meson setup drm-hasvk-b drm --cross-file cross-hasvk.txt --prefix=/w/sys-hasvk --libdir=lib --default-library=static \
      -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled \
      -Dvc4=disabled -Detnaviv=disabled -Dexynos=disabled -Domap=disabled -Dtegra=disabled \
      -Dudev=false -Dcairo-tests=disabled -Dvalgrind=disabled -Dtests=false -Dman-pages=disabled -Dinstall-test-programs=false >/w/drm-hasvk-setup.log 2>&1 || { tail -40 /w/drm-hasvk-setup.log; exit 1; }
    ninja -C drm-hasvk-b install >/w/drm-hasvk-build.log 2>&1 || { tail -40 /w/drm-hasvk-build.log; exit 1; }
  fi
  export PKG_CONFIG_PATH=/w/sys-hasvk/lib/pkgconfig PKG_CONFIG_LIBDIR=/w/sys-hasvk/lib/pkgconfig
  cd mesa
  python3 /h/patch_mesa.py
  [ "${KEEP_BUILD:-0}" = 1 ] || rm -rf /w/mesa-hasvk-b
  [ -d /w/mesa-hasvk-b ] || meson setup /w/mesa-hasvk-b . --cross-file /w/cross-hasvk.txt --prefix=/w/mesa-hasvk-out --buildtype=release -Db_ndebug=true -Dstrip=true \
    -Dc_args="-I/w/sys-hasvk/include -I/w/sys-hasvk/include/libdrm" -Dcpp_args="-I/w/sys-hasvk/include -I/w/sys-hasvk/include/libdrm" \
    -Dplatforms=android -Dandroid-stub=true -Dplatform-sdk-version=$SDK -Dandroid-libbacktrace=disabled -Dcpp_rtti=false -Dmesa-clc=system -Dexpat:default_library=static \
    -Dvulkan-drivers=intel_hasvk -Dgallium-drivers= -Dllvm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dgbm=disabled -Dglx=disabled \
    -Dglvnd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= \
    -Dvideo-codecs= -Dgallium-va=disabled -Dxlib-lease=disabled -Dzstd=disabled -Dperfetto=false -Dmicrosoft-clc=disabled \
    -Dintel-rt=disabled -Dteflon=false -Dgallium-rusticl=false -Dtools= >/w/mesa-hasvk-setup.log 2>&1 || { tail -60 /w/mesa-hasvk-setup.log; exit 1; }
  ninja -C /w/mesa-hasvk-b -j"$(nproc)" >/w/mesa-hasvk-build.log 2>&1 || { tail -60 /w/mesa-hasvk-build.log; exit 1; }
  F=$(find /w/mesa-hasvk-b -name "libvulkan_intel_hasvk.so" | head -n 1)
  [ -n "$F" ] || { echo "libvulkan_intel_hasvk.so not found"; exit 1; }
  $TC/llvm-strip --strip-unneeded -o /out/vulkan.hasvk.so "$F"
  ls -l /out/vulkan.hasvk.so; sha256sum /out/vulkan.hasvk.so
'
