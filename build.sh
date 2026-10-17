#!/bin/sh
# gbOS86: one-command build.
#   ./build.sh                     -> interactive menu (standard or looper, custom userdata size)
#   ./build.sh --standard          -> dist/gbOS86-YYYYMMDD.img (live USB installer, MODE=ask)
#   ./build.sh --looper            -> dist/gbOS86-looper-YYYYMMDD.img (persistent /data, MODE=boot)
#   ./build.sh --looper --data-size 32
#   ./build.sh -y                  don't ask before downloading (uses standard if not specified)
#   ./build.sh --dry-run           show what would be downloaded and built, change nothing
#
# What it does (each step is skipped when its result already exists, so re-running is cheap and resumable):
#   1. Google's OS: downloads Google's public Googlebook recovery image (7.7 GB) and extracts only the six partitions
#      gbOS86 needs (about 8 GB, streamed, so the 19 GB disk image inside is never written to disk).
#   2. busybox (Debian busybox-static, ~1 MB) via docker.
#   3. Kernel modules (14 MB) and firmware (53 MB) built from this repo's sources, from the GitHub release.
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
    printf '\n================ gbOS86 Image Target ================\n'
    echo "  1) Standard image (live USB + installer menu, MODE=ask)"
    echo "  2) Loopback persistent image (MODE=boot, persistent /data and /metadata)"
    printf 'Choose target [1/2, default: 1]: '
    read -r ans
    case "$ans" in
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

REL=https://github.com/linuxkid473/gbos-x86/releases/download/v36
MODS=gbos86-modules-6.18.21-gbos-whl-r2.tar.xz;            MODS_SHA=8d05757c6dcd531d34fd7b86ee1c5fc4c957abee491fc8185b8fb133a68a4370
REL_FW=https://github.com/linuxkid473/gbos-x86/releases/download/v35
FW=gbos86-firmware-12dfe670-sof2026.09.2-r4.tar.xz;        FW_SHA=8ad4ce620194e6555fe747bff95eb75b74c3c8f198ff7e3d5280c02f37bb1463
GOOGLE=https://dl.google.com/device/recovery/lapis-user/16452207/recovery.zip
PARTS="system_a vendor_a product_a system_ext_a system_dlkm_a vendor_dlkm_a"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
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
[ -f usb/modules/lib/modules/6.18.21-gbos-whl+/kernel/sound/soc/intel/boards/snd-soc-skl_hda_dsp.ko ] || NEED="$NEED\n  - $MODS (14 MB, from this project's GitHub release)"
[ -f usb/firmware/rtl_nic/rtl8168h-2.fw ] || NEED="$NEED\n  - $FW (70 MB, from this project's GitHub release)"
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

if [ ! -d usb/modules/lib/modules ]; then
  say "3/4 kernel modules and firmware"
  fetch "$REL/$MODS" "build/dl/$MODS" "$MODS_SHA"; tar -xJf "build/dl/$MODS" -C usb
else say "3/4 kernel modules: present"; fi
if [ ! -f usb/firmware/rtl_nic/rtl8168h-2.fw ]; then
  fetch "$REL_FW/$FW" "build/dl/$FW" "$FW_SHA"; tar -xJf "build/dl/$FW" -C usb
else say "    firmware: present"; fi

say "4/4 building the image ($BUILD_TYPE)"
sh tools/make_gbos86_image.sh --type "$BUILD_TYPE" --data-size "$DATA_GB"
echo
echo "Done. Flash the .img in dist/ to a USB stick or target drive (balenaEtcher, Rufus, or dd), then boot it."
