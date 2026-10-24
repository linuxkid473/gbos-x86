#!/bin/sh
# gbOS86: one-command build.
#   ./build.sh                     -> interactive menu (standard, looper, or dualboot)
#   ./build.sh --standard          -> dist/gbOS86-YYYYMMDD.img (live USB installer, MODE=ask)
#   ./build.sh --looper            -> dist/gbOS86-looper-YYYYMMDD.img (persistent /data, MODE=boot)
#   ./build.sh --looper --data-size 32
#   ./build.sh --dualboot --target /dev/sda -> install directly to disk alongside existing Windows/Linux
#   ./build.sh -y                  don't ask before downloading (uses standard if not specified)
#   ./build.sh --dry-run           show what would be downloaded and built, change nothing
#
# What it does (each step is skipped when its result already exists, so re-running is cheap and resumable):
#   1. Google's OS: downloads Google's public Googlebook recovery image (7.7 GB) and extracts only the six partitions
#      gbOS86 needs (about 8 GB, streamed, so the 19 GB disk image inside is never written to disk).
#   2. busybox (Debian busybox-static, ~1 MB) via docker.
#   3. Kernel modules (14 MB), firmware (70 MB) and the SwiftShader software Vulkan driver (AMD GPUs; 6 MB) built from this
#      repo's sources, from the GitHub release.
#   4. Assembles the flashable image with tools/make_gbos86_image.sh (the kernel, patched libraries and installer
#      tools are already in this repository).
# Needs: macOS or Linux, docker (running), python3, curl, tar, xz, ~35 GB free disk.
set -e
cd "$(dirname "$0")"

YES=0
DRY=0
BUILD_TYPE=""
DATA_GB=64

# Parse command line options
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=1;;
    --dry-run) DRY=1;;
    --standard) BUILD_TYPE="standard";;
    --looper|--loopback) BUILD_TYPE="looper";;
    --dualboot|--dual-boot) BUILD_TYPE="dualboot";;
    --target) shift; TARGET_DISK="$1";;
    --target=*) TARGET_DISK="${1#*=}";;
    --allow-internal) EXTRA_FLAGS="$EXTRA_FLAGS --allow-internal";;
    --allow-root) EXTRA_FLAGS="$EXTRA_FLAGS --allow-root";;
    --reformat) EXTRA_FLAGS="$EXTRA_FLAGS --reformat";;
    --data-size) shift; DATA_GB="$1";;
    --data-size=*) DATA_GB="${1#*=}";;
    -h|--help) sed -n '2,16p' "$0"; exit 0;;
    *) echo "Unknown option: $1"; exit 1;;
  esac
  shift
done

# Interactive selection if build type is not pre-set via flags
if [ -z "$BUILD_TYPE" ]; then
  if [ $YES = 1 ]; then
    BUILD_TYPE="standard"
  else
    printf '\n================ gbOS86 Target ================\n'
    echo "  1) Standard image (live USB + installer menu, MODE=ask)"
    echo "  2) Loopback persistent image (MODE=boot, persistent /data and /metadata)"
    echo "  3) Dual-boot direct install (non-destructive direct install alongside Windows/Linux)"
    printf 'Choose target [1/2/3, default: 1]: '
    read -r ans
    case "$ans" in
      3|dualboot|dual-boot)
        BUILD_TYPE="dualboot"
        printf 'Enter target drive device (e.g. /dev/sda, /dev/sdb): '
        read -r TARGET_DISK
        ;;
      2|looper|loopback)
        BUILD_TYPE="looper"
        printf 'Enter userdata (/data) size in GB (sparse, expands as needed) [default: 64]: '
        read -r user_sz
        case "$user_sz" in
          ''|*[!0-9]*) [ -n "$user_sz" ] && echo "Invalid number; using default 64 GB." ;;
          *) [ "$user_sz" -gt 0 ] && DATA_GB="$user_sz" ;;
        esac
        ;;
      *)
        BUILD_TYPE="standard"
        ;;
    esac
  fi
fi

REL=https://github.com/linuxkid473/gbos-x86/releases/download/v40   # r5 = the module tree of kernel #42 (release v40: tag to be created, upload build/dl/$MODS)
MODS=gbos86-modules-6.18.21-gbos-whl-r5.tar.xz;            MODS_SHA=67b1b97f64efc2017df685357adc1299e96b047963e3a22a3c20bd87f002db46
REL_FW=https://github.com/linuxkid473/gbos-x86/releases/download/v35
FW=gbos86-firmware-12dfe670-sof2026.09.2-r4.tar.xz;        FW_SHA=8ad4ce620194e6555fe747bff95eb75b74c3c8f198ff7e3d5280c02f37bb1463
# KernelSU's userspace (the kernel part is built into the kernel, see kernel/kernelsu/): the manager app and the daemon.
# The kernel only accepts a manager APK with the official signing certificate, so the release APK is used, not a Play Store copy.
REL_KSU=https://github.com/tiann/KernelSU/releases/download/v3.3.1
KSU_APK=KernelSU_v3.3.1_32680-release.apk;                  KSU_APK_SHA=b4a059ace71de2fa65cc7f12b1c58d55fc23ab25e057ee4230ce779742abe450
KSUD=ksud-x86_64-linux-android;                             KSUD_SHA=5324a1e8f2f3c74dc69624c4cd973fb9bc1d5da3614a2f2e694583f73bd13cbd
# SwiftShader as an Android Vulkan HAL (tools/swiftshader-android/build.sh): hook.sh binds it over Google's Intel Vulkan driver on
# laptops without an Intel GPU (AMD). Release v38 (tag to be created); tar contains patched/vulkan.swiftshader.so.
REL_SWR=https://github.com/linuxkid473/gbos-x86/releases/download/v38
SWR=gbos86-swr-r1.tar.xz;                                  SWR_SHA=7faaa84e3453523ae80d3792983e13d88324e6ce66367a93260e53541147f66a
# The Google build comes from lapis/BUILD (one place to change when moving to a new build: see porting.md)
. lapis/BUILD; GOOGLE=$URL
PARTS="system_a vendor_a product_a system_ext_a system_dlkm_a vendor_dlkm_a"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
# The module tree has to be the one built with usb/out/BOOTX64.EFI: its directory is named after the kernel's release (usb/hwdetect.sh binds
# gbos/modules/lib/modules/$(uname -r)) and a tree from another build is rejected by the kernel (module signature, symbol CRCs).
KREL=$(grep -a -m1 -o '[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*-gbos-whl+\{0,1\} (' usb/out/BOOTX64.EFI 2>/dev/null | sed 's/ ($//')
have_mods() { [ -n "$KREL" ] && [ -f "usb/modules/lib/modules/$KREL/kernel/drivers/iio/accel/hid-sensor-accel-3d.ko" ]; }
have_swr() { [ -f usb/patched/vulkan.swiftshader.so ]; }
have_ksu() { [ -f usb/ksu/manager.apk ] && [ -f usb/ksu/ksud ]; }
have_parts() { for p in $PARTS; do [ -s lapis/parts/$p.img ] || return 1; done; }
fetch() { # url dest sha
  mkdir -p "$(dirname "$2")"
  curl -fL --retry 5 -C - -o "$2" "$1" || die "download failed: $1"
  [ "$(sha "$2")" = "$3" ] || { rm -f "$2"; die "checksum mismatch for $1 (file deleted, run again)"; }
}

for c in docker python3 curl tar xz; do command -v $c >/dev/null 2>&1 || die "missing '$c'"; done
docker info >/dev/null 2>&1 || die "docker is installed but not running"

NEED=""
have_parts || NEED="$NEED\n  - Google's recovery image: $GOOGLE (7.7 GB, from dl.google.com)"
[ -x kernel/out/busybox ] || NEED="$NEED\n  - busybox-static from Debian via docker (about 1 MB)"
have_mods || NEED="$NEED\n  - $MODS (14 MB, from this project's GitHub release)"
[ -f usb/firmware/rtl_nic/rtl8168h-2.fw ] || NEED="$NEED\n  - $FW (70 MB, from this project's GitHub release)"
have_swr || NEED="$NEED\n  - $SWR (software GPU driver for AMD graphics, 6 MB, from this project's GitHub release)"
have_ksu || NEED="$NEED\n  - $KSU_APK + $KSUD (19 MB, from the KernelSU release on GitHub)"
if [ -z "$NEED" ]; then say "everything is already downloaded"; else
  printf 'This will download:%b\n' "$NEED"
fi

FREE_KB=$(df -k . | awk 'NR==2{print $4}'); [ "$FREE_KB" -gt 36000000 ] || printf '\nWARNING: only %d GB free here; about 35 GB are needed.\n' $((FREE_KB / 1000000))
[ $DRY = 1 ] && { echo "(dry run: nothing changed)"; exit 0; }
if [ -n "$NEED" ] && [ $YES = 0 ]; then printf '\nContinue? [y/N] '; read -r a; case $a in y|Y|yes) ;; *) exit 1;; esac; fi

if ! have_parts; then
  say "1/4 Google's OS image"
  fetch "$GOOGLE" lapis/recovery.zip "$(cut -d' ' -f1 lapis/recovery.zip.sha256)"
  python3 tools/fetch_google_parts.py lapis/recovery.zip lapis/parts $PARTS
  [ "${KEEP_ZIP:-0}" = 1 ] || rm -f lapis/recovery.zip
else say "1/4 Google's OS image: already extracted"; fi

if [ ! -x kernel/out/busybox ]; then say "2/4 busybox"; sh tools/fetch_busybox.sh; else say "2/4 busybox: present"; fi

if ! have_mods; then
  say "3/4 kernel modules and firmware"
  fetch "$REL/$MODS" "build/dl/$MODS" "$MODS_SHA"; rm -rf usb/modules; tar -xJf "build/dl/$MODS" -C usb
  have_mods || die "the module tree in $MODS is not for this kernel (${KREL:-unknown}): usb/modules/lib/modules/ holds $(ls usb/modules/lib/modules 2>/dev/null | tr '\n' ' '). Build it with kernel/build_kernel.sh"
else say "3/4 kernel modules: present"; fi
if [ ! -f usb/firmware/rtl_nic/rtl8168h-2.fw ]; then
  fetch "$REL_FW/$FW" "build/dl/$FW" "$FW_SHA"; tar -xJf "build/dl/$FW" -C usb
else say "    firmware: present"; fi
if ! have_swr; then
  # Only AMD laptops need it: a failed download is a warning, not the end of the build (Intel images are complete without it).
  if ( fetch "$REL_SWR/$SWR" "build/dl/$SWR" "$SWR_SHA" ) && tar -xJf "build/dl/$SWR" -C usb; then :
  else printf '\nWARNING: no software GPU driver in this image; it will not render on AMD graphics (Intel laptops are unaffected).\n'; fi
else say "    software GPU driver (SwiftShader): present"; fi
if ! have_ksu; then
  # KernelSU's userspace: the manager app (the kernel only accepts the official signature) and the daemon.
  mkdir -p usb/ksu
  fetch "$REL_KSU/$KSU_APK" "usb/ksu/manager.apk" "$KSU_APK_SHA"
  fetch "$REL_KSU/$KSUD" "usb/ksu/ksud" "$KSUD_SHA"
  chmod 755 usb/ksu/ksud
else say "    KernelSU manager + ksud: present"; fi

if [ "$BUILD_TYPE" = "dualboot" ]; then
  [ -n "$TARGET_DISK" ] || die "Target disk required for dualboot. Pass --target /dev/sdX"
  say "4/4 installing gbOS86 in dual-boot mode onto $TARGET_DISK"
  [ $DRY -eq 1 ] && EXTRA_FLAGS="$EXTRA_FLAGS --dry-run"
  sudo sh tools/install_dualboot.sh --target "$TARGET_DISK" --data-size "$DATA_GB" $EXTRA_FLAGS
else
  say "4/4 building the image ($BUILD_TYPE)"
  sh tools/make_gbos86_image.sh --type "$BUILD_TYPE" --data-size "$DATA_GB"
  echo
  echo "Done. Flash the .img in dist/ to a USB stick or target drive (balenaEtcher, Rufus, or dd), then boot it."
fi
