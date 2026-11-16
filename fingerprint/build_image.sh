#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PARTS=${1:-$ROOT/lapis/parts}
PARTS=$(CDPATH= cd -- "$PARTS" && pwd)
BUILD_IMAGE=${GBOS_FP_BUILD_IMAGE:-gbos-kbuild}
BUILD_VOLUME=${GBOS_FP_BUILD_VOLUME:-gbos-ksrc}
CLANG_DIR=${GBOS_FP_CLANG_DIR:-/build/inputs/clang-r584948b}

[ -s "$PARTS/system_a.img" ] || { echo "missing $PARTS/system_a.img" >&2; exit 1; }
docker image inspect gbos-erofs >/dev/null 2>&1 || {
  echo "missing local gbos-erofs image; build it with tools/erofs-docker before building the fingerprint HAL" >&2
  exit 1
}
docker image inspect "$BUILD_IMAGE" >/dev/null 2>&1 || {
  echo "missing local $BUILD_IMAGE image; prepare the Android clang toolchain first" >&2
  exit 1
}
docker volume inspect "$BUILD_VOLUME" >/dev/null 2>&1 || {
  echo "missing local $BUILD_VOLUME volume; prepare the Android clang toolchain first" >&2
  exit 1
}

mkdir -p "$ROOT/build"
WORK=$(mktemp -d "$ROOT/build/gbos-fp.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
mkdir -p "$WORK/libs"
PARTS_UID=$(id -u):$(id -g)

docker run --rm --pull=never --user "$PARTS_UID" \
  -v "$PARTS:/parts:ro" -v "$WORK:/work" gbos-erofs \
  /bin/sh -ec '
    mkdir -p /work/system /work/runtime-apex /work/runtime
    fsck.erofs --extract=/work/system --force /parts/system_a.img >/dev/null
    python3 -m zipfile -e /work/system/system/apex/com.android.runtime.apex /work/runtime-apex
    fsck.erofs --extract=/work/runtime --force /work/runtime-apex/apex_payload.img >/dev/null
    cp /work/system/system/lib64/libbinder_ndk.so /work/libs/
    cp /work/system/system/lib64/liblog.so /work/libs/
    cp /work/system/system/lib64/libcrypto.so /work/libs/
    cp /work/runtime/lib64/bionic/libc.so /work/libs/
    cp /work/runtime/lib64/bionic/libdl.so /work/libs/
  '

docker run --rm --pull=never --user "$PARTS_UID" \
  -v "$ROOT:/src" -v "$WORK/libs:/libs:ro" -v "$BUILD_VOLUME:/build:ro" \
  -w /src/fingerprint \
  -e CLANG_DIR="$CLANG_DIR" -e LIB_DIR=/libs -e OUTPUT=/src/usb/gbos_fp \
  "$BUILD_IMAGE" bash ./build.sh

[ -s "$ROOT/usb/gbos_fp" ] || { echo "fingerprint HAL build produced no binary" >&2; exit 1; }
