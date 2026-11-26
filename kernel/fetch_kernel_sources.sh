#!/bin/sh
# Fetch the kernel build inputs into the docker volume "gbos-ksrc":
#
#   /build/src                     AOSP kernel/common (android17-6.18) at the pinned commit
#   /build/inputs/clang-r584948b  AOSP clang prebuilt (same toolchain the shipped kernel used)
#   /build/inputs/KernelSU        KernelSU sources (tiann/KernelSU, tag v3.3.1)
#
# Everything is skipped when it is already there, so re-running is cheap.
# Needs: docker (running), git, curl, tar. ~2.5 GB of downloads, ~7 GB in the volume.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VOL=gbos-ksrc
KSRC_COMMIT=3938c8d97142d5cf31543aa96f864a2cf832dda0   # AOSP kernel/common android17-6.18
KSRC_URL=https://android.googlesource.com/kernel/common
CLANG_TAG=android-17.0.0_r1        # AOSP prebuilts/clang/host/linux-x86
CLANG_DIR=clang-r584948b           # the toolchain the shipped kernel #37 was built with
KSU_TAG=v3.3.1

DL=$ROOT/build/dl
mkdir -p "$DL"
say() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

command -v docker >/dev/null || die "docker is not installed"
docker info >/dev/null 2>&1 || die "docker is installed but not running"
docker volume create "$VOL" >/dev/null

# --- 1. kernel source --------------------------------------------------------------
if docker run --rm -v "$VOL":/build debian:trixie test -f /build/src/Makefile; then
  say "kernel source: present in $VOL"
else
  say "kernel source: android.googlesource.com kernel/common @ $KSRC_COMMIT"
  # A shallow git fetch of the pinned commit, exported with git archive (a plain tree, no .git, as the tarball was). Git checks
  # every object against its hash, so the commit id is the integrity check. The +archive/<commit>.tar.gz download used before is
  # generated on request: its bytes (and the sha256 this script used to pin) changed while the tree did not.
  G=$DL/kernel-common.git
  [ -d "$G" ] || git init -q --bare "$G"
  git -C "$G" cat-file -e "$KSRC_COMMIT^{commit}" 2>/dev/null || git -C "$G" fetch -q --depth 1 "$KSRC_URL" "$KSRC_COMMIT"
  [ "$(git -C "$G" rev-parse "$KSRC_COMMIT^{commit}")" = "$KSRC_COMMIT" ] || die "could not fetch $KSRC_COMMIT from $KSRC_URL"
  git -C "$G" archive --format=tar "$KSRC_COMMIT" | docker run --rm -i -v "$VOL":/build debian:trixie \
    sh -c "set -e; mkdir -p /build/src; tar -xf - -C /build/src"
fi

# --- 2. AOSP clang -----------------------------------------------------------------
if docker run --rm -v "$VOL":/build debian:trixie test -x /build/inputs/$CLANG_DIR/bin/clang; then
  say "clang: present in $VOL"
else
  say "clang: android.googlesource.com prebuilts/clang/host/linux-x86 ($CLANG_TAG)"
  rm -rf "$DL/clang-prebuilts"
  git clone --filter=blob:none --sparse --depth 1 --branch "$CLANG_TAG" \
    https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86 "$DL/clang-prebuilts"
  git -C "$DL/clang-prebuilts" sparse-checkout set "$CLANG_DIR"
  # Only the toolchain is needed; the NDK runtimes and the C++ library are not.
  rm -rf "$DL/clang-prebuilts/$CLANG_DIR"/{android_libc++,musl,python3,runtimes_ndk_cxx}
  docker run --rm -v "$VOL":/build -v "$DL":/dl:ro debian:trixie \
    bash -c "set -e; mkdir -p /build/inputs; cp -a /dl/clang-prebuilts/$CLANG_DIR /build/inputs/"
fi

# --- 3. KernelSU -------------------------------------------------------------------
if docker run --rm -v "$VOL":/build debian:trixie test -d /build/inputs/KernelSU/kernel; then
  say "KernelSU: present in $VOL"
else
  say "KernelSU: github.com/tiann/KernelSU ($KSU_TAG)"
  rm -rf "$DL/KernelSU"
  git clone https://github.com/tiann/KernelSU "$DL/KernelSU"
  git -C "$DL/KernelSU" checkout "$KSU_TAG"
  # The kernel side derives its version from the commit count; a shallow clone would
  # report a wrong version, so keep the full history.
  docker run --rm -v "$VOL":/build -v "$DL":/dl:ro debian:trixie \
    bash -c "set -e; rm -rf /build/inputs/KernelSU; cp -a /dl/KernelSU /build/inputs/KernelSU; chown -R root:root /build/inputs/KernelSU"
fi

say "done: $VOL now holds /build/src, /build/inputs/$CLANG_DIR and /build/inputs/KernelSU"
echo "Next: kernel/kernelsu/apply.sh (in the build container), then kernel/isa_emu_patch/build.sh"
