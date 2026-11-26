#!/bin/sh
# make_gbos86_image.sh: build a flashable gbOS86 USB image.
#
# Supported modes:
#   standard: ~6.3 GiB GPT image (live USB installer, MODE=ask)
#     p1 EFI       256 MiB FAT32  EFI/BOOT/BOOTX64.EFI
#     p2 GBOS        6 GiB FAT32  /gbos: super.img, installer, patches
#
#   looper: persistent GPT image matching install.sh layout (MODE=boot)
#     p1 GBOS_EFI  512 MiB FAT32  EFI/BOOT/BOOTX64.EFI
#     p2 GBOSSYS     8 GiB FAT32  /gbos: super.img, patches, MODE=boot
#     p3 GBOS_META  64 MiB ext4   Android /metadata
#     p4 GBOS_DATA   N GiB ext4   Android /data (sparse, expands up to N GiB)
#
# Usage:
#   sh tools/make_gbos86_image.sh [--type standard|looper] [--data-size N_GB] [--auto] [OUT_DIR]
#   GBOS_DEBUG_LOGS=1 enables full logcat and verbose startup diagnostics in the image.
#   GBOS_CFG_SET="CAMERA_LIBCAMERA=both DEBUG_LOGS=1" sets gbos.cfg keys in this image only (applied last).
#   GBOS_USB_SOURCE=/path/to/usb uses a separately staged USB payload tree.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/dist"
BUILD_TYPE="standard"
DATA_GB=64
AUTO_TAILOR=0

# Argument parsing
while [ $# -gt 0 ]; do
  case "$1" in
    --type=*) BUILD_TYPE="${1#*=}";;
    --type) shift; BUILD_TYPE="$1";;
    --data-size=*) DATA_GB="${1#*=}";;
    --data-size) shift; DATA_GB="$1";;
    -a|--auto|--host) AUTO_TAILOR=1;;
    standard|looper|loopback)
      case "$1" in loopback) BUILD_TYPE="looper";; *) BUILD_TYPE="$1";; esac
      ;;
    *)
      if [ -d "$1" ] || [ ! -e "$1" ]; then OUT="$1"; fi
      ;;
  esac
  shift
done

case "$BUILD_TYPE" in
  looper|loopback) BUILD_TYPE="looper";;
  *) BUILD_TYPE="standard";;
esac

U=${GBOS_USB_SOURCE:-$ROOT/usb}
P=$ROOT/lapis/parts
# The IPU6 camera payload is needed only for a Latitude 7320 camera bridge image (GBOS_IPU6_CAMERA_BRIDGE=1, as below); other images
# built from the release kernel, modules (r5) and firmware (r4) have no IPU6 parts and must not stop here (issue 65).
[ "${GBOS_IPU6_CAMERA_BRIDGE:-0}" != 1 ] || (
  CAMERA_CONFIG=$(mktemp)
  trap 'rm -f "$CAMERA_CONFIG"' EXIT HUP INT TERM
  KREL=$(grep -a -m1 -o '[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*-gbos-whl+\{0,1\} (' "$U/out/BOOTX64.EFI" 2>/dev/null | sed 's/ ($//')
  python3 "$ROOT/tools/extract_ikconfig.py" "$U/out/BOOTX64.EFI" > "$CAMERA_CONFIG" ||
    { echo "camera payload: cannot read the kernel config; rebuild with kernel/build_kernel.sh" >&2; exit 1; }
  sh "$ROOT/tools/check_camera_payload.sh" "$CAMERA_CONFIG" "$KREL" "$U"
)
if [ "$BUILD_TYPE" = "looper" ]; then
  NAME="gbOS86-looper-$(date +%Y%m%d)"
else
  NAME="gbOS86-$(date +%Y%m%d)"
fi
STAGE="$OUT/$NAME.stage"
mkdir -p "$OUT"; rm -rf "$STAGE"; mkdir -p "$STAGE/efi/EFI/BOOT" "$STAGE/gbos/EFI/BOOT" "$STAGE/gbos/gbos"

# 1. System: super.img from Google's partitions or pre-built build/super.img
if [ -f "$ROOT/build/super.img" ] && [ -f "$ROOT/build/super.layout" ]; then
  echo "== super.img (reusing pre-built image from build/)"
  cp "$ROOT/build/super.img" "$STAGE/gbos/gbos/super.img"
  cp "$ROOT/build/super.layout" "$STAGE/gbos/gbos/super.layout"
elif [ -f "$ROOT/dist/super.img" ] && [ -f "$ROOT/dist/super.layout" ]; then
  echo "== super.img (reusing pre-built image from dist/)"
  cp "$ROOT/dist/super.img" "$STAGE/gbos/gbos/super.img"
  cp "$ROOT/dist/super.layout" "$STAGE/gbos/gbos/super.layout"
else
  for n in system_a vendor_a product_a system_ext_a system_dlkm_a vendor_dlkm_a; do
    [ -f "$P/$n.img" ] || { echo "missing $P/$n.img"; exit 1; }
  done
  echo "== super.img"
  python3 "$U/make_super.py" "$STAGE/gbos/gbos/super.img" "$STAGE/gbos/gbos/super.layout" \
    system_a="$P/system_a.img" vendor_a="$P/vendor_a.img" product_a="$P/product_a.img" \
    system_ext_a="$P/system_ext_a.img" system_dlkm_a="$P/system_dlkm_a.img" vendor_dlkm_a="$P/vendor_dlkm_a.img"
fi

# 2. Everything else stage0/hook/install expect on the stick
echo "== assembling files ($BUILD_TYPE mode)"
cp "$U/out/BOOTX64.EFI" "$STAGE/efi/EFI/BOOT/BOOTX64.EFI"
cp "$U/out/BOOTX64.EFI" "$STAGE/gbos/EFI/BOOT/BOOTX64.EFI"
G="$STAGE/gbos/gbos"
for f in hook.sh stick_fat.sh aidiag.sh native_zygote.sh early_patch.sh sensor_vendor_overlay.sh sensor_trial_diag.sh tablet_mode.sh hwdetect.sh sensordiag.sh mask_vendor_modules.sh install.sh apex_patch.sh gbos.cfg crossystem bootdisk.img libkm_compat.so system_ext_manifest.xml soft_gk gbos_fp sysupdate.sh sysupdate.pub gbos86-updater.apk Add-gbOS-to-Windows-BootMenu.bat; do
  [ -f "$U/$f" ] && cp "$U/$f" "$G/$f"
done
if [ -n "${GBOS_SENSOR_PAYLOAD:-}" ]; then
  sh "$ROOT/sensors/trogdor/image_payload.sh" stage "$GBOS_SENSOR_PAYLOAD" "$G"
fi
[ -f "$U/Add-gbOS-to-Windows-BootMenu.bat" ] && cp "$U/Add-gbOS-to-Windows-BootMenu.bat" "$STAGE/gbos/Add-gbOS-to-Windows-BootMenu.bat"
for d in firmware patched audio installer kmod modules surface ksu thermal display; do cp -R "$U/$d" "$G/$d"; done
# Stage0 screen reader (Speakup + espeakup, usb/a11y/, filled by tools/fetch_a11y_tools.sh): only used when booting the stick.
[ -d "$U/a11y/bin" ] && cp -R "$U/a11y" "$G/a11y"
# Camera calibration (colour matrices and lens shading extracted from the Windows tuning files with
# tools/extract_aiqb_ccm.py): proprietary, so never in git. Put it in usb/camera/ (gitignored) to include it;
# without it both cameras work, with generic colour.
if [ -d "$U/camera" ]; then cp -R "$U/camera" "$G/camera"; echo "   camera calibration: included ($(ls "$U/camera" | wc -l | tr -d ' ') files)"
else echo "   camera calibration: not included (no usb/camera/; generic camera colour)"; fi
# Ensure both '+' and non-'+' versions exist on the FAT32 image filesystem
for _m in "$G/modules/lib/modules/"*-gbos-whl+; do
  if [ -d "$_m" ]; then
    _base="${_m%+}"
    [ ! -d "$_base" ] && cp -a "$_m" "$_base" 2>/dev/null || true
  fi
done
for _m in "$G/modules/lib/modules/"*-gbos-whl; do
  if [ -d "$_m" ]; then
    _plus="${_m}+"
    [ ! -d "$_plus" ] && cp -a "$_m" "$_plus" 2>/dev/null || true
  fi
done
mkdir -p "$G/logs"

# Configure boot mode
if [ "$BUILD_TYPE" = "looper" ]; then
  # looper mode sets MODE=boot so Android starts immediately with persistent storage
  sed -i.bak -e 's/^MODE=.*/MODE=boot/' "$G/gbos.cfg" && rm -f "$G/gbos.cfg.bak"
  grep -q '^MODE=boot$' "$G/gbos.cfg" || { echo "failed to set MODE=boot in $G/gbos.cfg"; exit 1; }
else
  # standard installer mode: defaults to MODE=install (direct to clean installer menu) or MODE=ask
  if ! grep -q '^MODE=\(ask\|install\)$' "$G/gbos.cfg"; then
    sed -i.bak -e 's/^MODE=.*/MODE=install/' "$G/gbos.cfg" && rm -f "$G/gbos.cfg.bak"
  fi
fi

gbos_enable_image_debug_logs() {
  _debug_config=$1
  [ "${GBOS_DEBUG_LOGS:-0}" = 1 ] || return 0
  if grep -q '^DEBUG_LOGS=' "$_debug_config"; then
    sed -i.bak -e 's/^DEBUG_LOGS=.*/DEBUG_LOGS=1/' "$_debug_config"
    rm -f "$_debug_config.bak"
  else
    printf '\nDEBUG_LOGS=1\n' >> "$_debug_config"
  fi
  grep -q '^DEBUG_LOGS=1$' "$_debug_config" || { echo "failed to enable DEBUG_LOGS in $_debug_config"; exit 1; }
}
# Full logcat and verbose diagnostics are opt-in for a specific image; keep the source config unchanged.
gbos_enable_image_debug_logs "$G/gbos.cfg"

# Build a deliberately opt-in Latitude 7320 camera/fingerprint diagnostic image without changing usb/gbos.cfg.
if [ "${GBOS_IPU6_CAMERA_BRIDGE:-0}" = 1 ]; then
  for payload in \
      "$G/installer/bin/ipu6-camera-bridge.sh" \
      "$G/installer/bin/ipu6-loopback" \
      "$G/kmod/v4l2loopback.ko" \
      "$G/surface/fake_uvc_desc.bin" \
      "$G/surface/latitude_fake_uvc_desc.bin" \
      "$G/surface/latitude_rear_fake_uvc_desc.bin"; do
    [ -s "$payload" ] || { echo "missing IPU6 bridge test payload: $payload" >&2; exit 1; }
  done
  for setting in CAMERA_BRIDGE FINGERPRINT; do
    case "$setting" in
      CAMERA_BRIDGE) value=1 ;;
      FINGERPRINT) value=1 ;;
    esac
    if grep -q "^${setting}=" "$G/gbos.cfg"; then
      sed -i.bak "s/^${setting}=.*/${setting}=${value}/" "$G/gbos.cfg"
      rm -f "$G/gbos.cfg.bak"
    else
      printf '\n%s=%s\n' "$setting" "$value" >> "$G/gbos.cfg"
    fi
    grep -q "^${setting}=${value}$" "$G/gbos.cfg" ||
      { echo "failed to set ${setting}=${value} for bridge test image" >&2; exit 1; }
  done
fi

# Add sensor sampling only to an explicitly requested test image; leave usb/gbos.cfg unchanged.
if [ "${GBOS_SENSOR_CONTINUOUS_PROBE:-0}" = 1 ]; then
  sh "$ROOT/sensors/trogdor/image_payload.sh" enable-continuous-probe "$G/gbos.cfg"
elif [ "${GBOS_SENSOR_MOTION_PROBE:-0}" = 1 ]; then
  sh "$ROOT/sensors/trogdor/image_payload.sh" enable-motion-probe "$G/gbos.cfg"
fi

# Apply hardware auto-tailoring if enabled
if [ "$AUTO_TAILOR" = 1 ]; then
  if [ -f "$ROOT/tools/detect_hardware.sh" ]; then
    . "$ROOT/tools/detect_hardware.sh"
    detect_host_hardware
    echo "== tailoring image configuration for host hardware: $DETECTED_DEVICE_MODEL"
    apply_hardware_config "$G/gbos.cfg"
  fi
fi

# GBOS_CFG_SET="KEY=value KEY2=value2": settings for this image only, applied last (usb/gbos.cfg is unchanged).
# Keys are upper-case gbos.cfg names; values may not contain spaces or shell/sed specials.
gbos_image_cfg_set() {
  _cfg=$1; shift
  for _kv in "$@"; do
    _k=${_kv%%=*}; _v=${_kv#*=}
    case "$_kv" in *=*) ;; *) echo "GBOS_CFG_SET: '$_kv' is not KEY=value" >&2; exit 1;; esac
    case "$_k" in ''|*[!A-Z0-9_]*) echo "GBOS_CFG_SET: bad key '$_k'" >&2; exit 1;; esac
    case "$_v" in *[!A-Za-z0-9._,:+-]*) echo "GBOS_CFG_SET: bad value for $_k: '$_v'" >&2; exit 1;; esac
    if grep -q "^${_k}=" "$_cfg"; then
      sed -i.bak "s/^${_k}=.*/${_k}=${_v}/" "$_cfg" && rm -f "$_cfg.bak"
    else
      printf '\n# Set for this image (GBOS_CFG_SET)\n%s=%s\n' "$_k" "$_v" >> "$_cfg"
    fi
    grep -qx "${_k}=${_v}" "$_cfg" || { echo "GBOS_CFG_SET: failed to set ${_k}=${_v}" >&2; exit 1; }
    echo "   gbos.cfg: ${_k}=${_v} (GBOS_CFG_SET)"
  done
}
# shellcheck disable=SC2086
[ -z "${GBOS_CFG_SET:-}" ] || gbos_image_cfg_set "$G/gbos.cfg" $GBOS_CFG_SET

cp "$ROOT/kernel/out/busybox" "$STAGE/gbos/busybox"

if [ "$BUILD_TYPE" = "looper" ]; then
  cat > "$STAGE/gbos/README.txt" <<EOF
gbOS86: Persistent image (looper mode).
Pre-configured with persistent ext4 /metadata and /data (${DATA_GB} GB sparse userdata partition).
Boots directly into Android (MODE=boot).
Settings: gbos/gbos.cfg. Logs: gbos/logs/boot-N/.
EOF
else
  cat > "$STAGE/gbos/README.txt" <<'EOF'
gbOS86: Googlebook OS on x86 laptops (Intel 6th gen+; boot-time hardware detection; tested: Dell Latitude 5300, i5-8365U).
Boot this USB (F12 at the Dell logo) for the gbOS86 menu:
  1) live from USB   2) install or update on the internal SSD   3) boot the installed system
Settings: gbos/gbos.cfg (MODE, DENSITY, RESOLUTION, HWDETECT ...). Logs: gbos/logs/boot-N/ (hwdetect.txt = what was detected).
EOF
fi
du -sh "$STAGE/gbos/gbos/super.img" "$STAGE/gbos"

# 3. Assemble the GPT image (partitions created as files, then placed at their GPT offsets)
echo "== image assembly in docker"
IMG="$OUT/$NAME.img"
Z=; if [ "$(getenforce 2>/dev/null)" = Enforcing ]; then case $OUT in "$ROOT"/*) Z=:z;; esac; fi

if [ "$BUILD_TYPE" = "looper" ]; then
  # Looper layout: matching install.sh
  docker run --rm --platform linux/amd64 -v "$OUT":/o$Z alpine:latest sh -c '
  set -e
  apk add -q mtools dosfstools sfdisk coreutils e2fsprogs >/dev/null
  cd /o; S='"$NAME"'.stage
  EFI_MB=512; SYS_MB=8192; META_MB=64; DATA_MB='"$(( DATA_GB * 1024 ))"'
  rm -f p1.fat p2.fat p3.ext4 p4.ext4

  # Build FAT32 partitions
  truncate -s ${EFI_MB}M p1.fat
  mkfs.fat -F 32 -n GBOS_EFI p1.fat >/dev/null
  mcopy -s -i p1.fat $S/efi/EFI ::/

  truncate -s ${SYS_MB}M p2.fat
  mkfs.fat -F 32 -n GBOSSYS p2.fat >/dev/null
  mcopy -s -i p2.fat $S/gbos/* ::/

  # Build ext4 partitions (-E nodiscard keeps sparse allocation)
  truncate -s ${META_MB}M p3.ext4
  mke2fs -F -q -t ext4 -E nodiscard -L GBOS_META p3.ext4

  truncate -s ${DATA_MB}M p4.ext4
  mke2fs -F -q -t ext4 -E nodiscard -m 0 -O verity -L GBOS_DATA p4.ext4

  # Total size with GPT header and secondary backup table at end
  TOTAL_MB=$(( 1 + EFI_MB + SYS_MB + META_MB + DATA_MB + 1 ))
  rm -f '"$NAME"'.img
  truncate -s ${TOTAL_MB}M '"$NAME"'.img

  P1_SEC=$(( EFI_MB * 2048 ))
  P2_SEC=$(( SYS_MB * 2048 ))
  P3_SEC=$(( META_MB * 2048 ))
  P4_SEC=$(( DATA_MB * 2048 ))

  printf "label: gpt\nstart=2048, size=$P1_SEC, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"GBOS_EFI\"\nsize=$P2_SEC, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"GBOSSYS\"\nsize=$P3_SEC, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"GBOS_META\"\nsize=$P4_SEC, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"GBOS_DATA\"\n" | sfdisk -q '"$NAME"'.img

  SEEK1=1
  SEEK2=$(( SEEK1 + EFI_MB ))
  SEEK3=$(( SEEK2 + SYS_MB ))
  SEEK4=$(( SEEK3 + META_MB ))

  dd if=p1.fat of='"$NAME"'.img bs=1M seek=$SEEK1 conv=notrunc,sparse status=none
  dd if=p2.fat of='"$NAME"'.img bs=1M seek=$SEEK2 conv=notrunc,sparse status=none
  dd if=p3.ext4 of='"$NAME"'.img bs=1M seek=$SEEK3 conv=notrunc,sparse status=none
  dd if=p4.ext4 of='"$NAME"'.img bs=1M seek=$SEEK4 conv=notrunc,sparse status=none

  rm -f p1.fat p2.fat p3.ext4 p4.ext4
  sfdisk -l '"$NAME"'.img | tail -6
  mdir -i '"$NAME"'.img@@$(( 1048576 * SEEK2 )) ::/gbos | head -25'
else
  # Standard live/installer layout
  docker run --rm --platform linux/amd64 -v "$OUT":/o$Z alpine:latest sh -c '
  set -e
  apk add -q mtools dosfstools sfdisk coreutils >/dev/null
  cd /o; S='"$NAME"'.stage
  EFI_MB=256; GBOS_MB=6144
  rm -f p1.fat p2.fat
  truncate -s ${EFI_MB}M p1.fat;  mkfs.fat -F 32 -n EFI  p1.fat >/dev/null; mcopy -s -i p1.fat $S/efi/EFI ::/
  truncate -s ${GBOS_MB}M p2.fat; mkfs.fat -F 32 -n GBOS p2.fat >/dev/null; mcopy -s -i p2.fat $S/gbos/* ::/
  rm -f '"$NAME"'.img; truncate -s $(( (EFI_MB + GBOS_MB + 2) * 1048576 )) '"$NAME"'.img
  printf "label: gpt\nstart=2048, size=$((EFI_MB*2048)), type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"gbOS86 EFI\"\nsize=$((GBOS_MB*2048)), type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"gbOS86\"\n" | sfdisk -q '"$NAME"'.img
  dd if=p1.fat of='"$NAME"'.img bs=1M seek=1 conv=notrunc,sparse status=none
  dd if=p2.fat of='"$NAME"'.img bs=1M seek=$((1 + EFI_MB)) conv=notrunc,sparse status=none
  rm -f p1.fat p2.fat
  sfdisk -l '"$NAME"'.img | tail -3
  mdir -i '"$NAME"'.img@@$((1048576*(1+EFI_MB))) ::/gbos | head -25'
fi

rm -rf "$STAGE"
(cd "$OUT" && shasum -a 256 "$NAME.img" > "$NAME.img.sha256" && cat "$NAME.img.sha256")
echo "Logical size:  $(ls -lh "$IMG" | awk '{print $5}')"
echo "Physical size: $(du -sh "$IMG" | awk '{print $1}') (sparse image)"
ls -la "$IMG"
echo "Write $IMG to target drive with balenaEtcher, Rufus, or: sudo dd if=$IMG of=/dev/rdiskN bs=4m"
