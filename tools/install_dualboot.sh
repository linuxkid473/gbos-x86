#!/bin/sh
# tools/install_dualboot.sh: Automated, Non-Destructive Persistent Dual-Boot Installer for gbOS86
# Installs gbOS86 on a target disk alongside existing Windows and Linux (Ubuntu) installations.
# Usage: sudo tools/install_dualboot.sh --target /dev/sdX [options]

set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
U=$ROOT/usb
TARGET_DISK=""
EFI_PART=""
SYS_SIZE="10G"
META_SIZE="64M"
DATA_SIZE="64G"
REFORMAT=0
DRY_RUN=0
SKIP_DOWNLOAD=0

ALLOW_INTERNAL=0
ALLOW_ROOT=0

say() { printf '\n== %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: sudo $0 --target <disk> [options]

Options:
  --target <disk>        Target disk device (e.g. /dev/sda, /dev/sdb). REQUIRED.
  --efi-part <part>      EFI partition to use (default: auto-detected, e.g. /dev/sda2).
  --sys-size <size>      Size of GBOSSYS partition (default: 10G).
  --meta-size <size>     Size of GBOS_META partition (default: 64M).
  --data-size <size>     Size of GBOS_DATA partition (default: 64G).
  --reformat             Re-format existing gbOS86 partitions.
  --allow-internal       Allow targeting internal NVMe drives (/dev/nvme*).
  --allow-root           Allow targeting the disk holding the active root filesystem.
  --dry-run              Simulate operations without writing changes.
  -h, --help             Show this help message.

Example:
  sudo $0 --target /dev/sda
EOF
  exit "${1:-1}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --target) TARGET_DISK="$2"; shift 2;;
    --efi-part) EFI_PART="$2"; shift 2;;
    --sys-size) SYS_SIZE="$2"; shift 2;;
    --meta-size) META_SIZE="$2"; shift 2;;
    --data-size) DATA_SIZE="$2"; shift 2;;
    --reformat) REFORMAT=1; shift;;
    --allow-internal) ALLOW_INTERNAL=1; shift;;
    --allow-root) ALLOW_ROOT=1; shift;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) usage 0;;
    *) die "Unknown argument: $1";;
  esac
done

[ -n "$TARGET_DISK" ] || usage 1
[ -b "$TARGET_DISK" ] || die "$TARGET_DISK is not a valid block device."

# Root privileges check
if [ "$(id -u)" -ne 0 ]; then
  die "This script requires root privileges. Please run with sudo."
fi

# Safety guard 1: Check if target disk holds the running root filesystem
ROOT_DEV=$(findmnt -n -o SOURCE / 2>/dev/null || true)
if [ -n "$ROOT_DEV" ] && echo "$ROOT_DEV" | grep -q "${TARGET_DISK##*/}"; then
  if [ $ALLOW_ROOT -eq 0 ]; then
    die "$TARGET_DISK holds your active host root filesystem ($ROOT_DEV). To prevent accidents, pass --allow-root if you intentionally wish to partition free space on this drive."
  fi
fi

# Safety guard 2: Check if target disk is internal NVMe (often Windows)
case "$TARGET_DISK" in
  *nvme*)
    if [ $ALLOW_INTERNAL -eq 0 ]; then
      die "$TARGET_DISK appears to be an internal NVMe SSD. To protect internal Windows/Linux drives, pass --allow-internal to confirm."
    fi
    ;;
esac

say "Target disk: $TARGET_DISK"

# Auto-detect EFI partition if not specified
if [ -z "$EFI_PART" ]; then
  for p in $(lsblk -rn -o NAME,PARTTYPE "$TARGET_DISK" 2>/dev/null | awk '$2 ~ /c12a7328-f81f-11d2-ba4b-00a0c93ec93b/i {print $1}'); do
    EFI_PART="/dev/$p"
    break
  done
  if [ -z "$EFI_PART" ]; then
    # Fallback to partition 1 or 2 if labeled EFI / SYSTEM
    for p in $(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2 ~ /^(EFI|SYSTEM|ESP|GBOS_EFI)$/i {print $1}'); do
      EFI_PART="/dev/$p"
      break
    done
  fi
fi

[ -n "$EFI_PART" ] && [ -b "$EFI_PART" ] || die "Could not identify EFI partition on $TARGET_DISK. Specify with --efi-part."
say "EFI partition: $EFI_PART"

# Check for existing GBOS partitions
SYS_PART=$(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2=="GBOSSYS"{print "/dev/"$1; exit}')
META_PART=$(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2=="GBOS_META"{print "/dev/"$1; exit}')
DATA_PART=$(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2=="GBOS_DATA"{print "/dev/"$1; exit}')

case "$TARGET_DISK" in
  *[0-9]) P_PREFIX="${TARGET_DISK}p";;
  *) P_PREFIX="${TARGET_DISK}";;
esac

if [ -n "$SYS_PART" ] && [ -n "$META_PART" ] && [ -n "$DATA_PART" ]; then
  say "Found existing gbOS86 partitions on $TARGET_DISK:"
  echo "  GBOSSYS:   $SYS_PART"
  echo "  GBOS_META: $META_PART"
  echo "  GBOS_DATA: $DATA_PART"
  ALREADY_PARTITIONED=1
else
  ALREADY_PARTITIONED=0
fi

if [ $DRY_RUN -eq 1 ]; then
  say "[DRY RUN] Target: $TARGET_DISK (partitions present: $(lsblk -no NAME "$TARGET_DISK" 2>/dev/null | tr '\n' ' '))"
  say "[DRY RUN] Would allocate $SYS_SIZE for GBOSSYS, $META_SIZE for GBOS_META, and $DATA_SIZE for GBOS_DATA into free space."
  exit 0
fi

# Partition if not already present
if [ $ALREADY_PARTITIONED -eq 0 ]; then
  command -v sgdisk >/dev/null 2>&1 || die "sgdisk is required to partition $TARGET_DISK non-destructively."
  say "Creating gbOS86 partitions non-destructively into unallocated free space..."
  # '0' tells sgdisk to pick the next unused partition number automatically
  sgdisk \
    -n 0:0:+"$SYS_SIZE"  -t 0:0700 -c 0:"GBOSSYS" \
    -n 0:0:+"$META_SIZE" -t 0:8300 -c 0:"GBOS_META" \
    -n 0:0:+"$DATA_SIZE" -t 0:8300 -c 0:"GBOS_DATA" \
    "$TARGET_DISK" || die "sgdisk failed to create partitions. Check if sufficient unallocated free space exists on $TARGET_DISK."
  partprobe "$TARGET_DISK" 2>/dev/null || sleep 2

  SYS_PART=$(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2=="GBOSSYS"{print "/dev/"$1; exit}')
  META_PART=$(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2=="GBOS_META"{print "/dev/"$1; exit}')
  DATA_PART=$(lsblk -rn -o NAME,LABEL "$TARGET_DISK" 2>/dev/null | awk '$2=="GBOS_DATA"{print "/dev/"$1; exit}')

  [ -n "$SYS_PART" ] && [ -n "$META_PART" ] && [ -n "$DATA_PART" ] || die "Could not resolve newly created partition device names on $TARGET_DISK"
fi

# Format partitions
if [ $ALREADY_PARTITIONED -eq 0 ] || [ $REFORMAT -eq 1 ]; then
  say "Formatting partitions ($SYS_PART, $META_PART, $DATA_PART)..."
  mkfs.vfat -F 32 -n GBOSSYS "$SYS_PART"
  mkfs.ext4 -F -L GBOS_META "$META_PART"
  mkfs.ext4 -F -O verity -L GBOS_DATA "$DATA_PART"
fi

# Populate GBOSSYS
MNT_SYS=$(mktemp -d)
trap 'umount "$MNT_SYS" 2>/dev/null; rmdir "$MNT_SYS" 2>/dev/null' EXIT INT TERM
mount "$SYS_PART" "$MNT_SYS"

# Create loopback metadata.img if physical GBOS_META partition is absent
if [ -z "$META_PART" ] && [ ! -f "$MNT_SYS/gbos/metadata.img" ]; then
  say "Creating loopback metadata.img (64 MB ext4) inside GBOSSYS..."
  mkdir -p "$MNT_SYS/gbos"
  dd if=/dev/zero of="$MNT_SYS/gbos/metadata.img" bs=1M count=64 status=none
  mkfs.ext4 -F -q -L GBOS_META "$MNT_SYS/gbos/metadata.img"
fi

say "Copying system files to $SYS_PART (GBOSSYS)..."
mkdir -p "$MNT_SYS/EFI/BOOT" "$MNT_SYS/gbos/logs"
[ -f "$U/out/BOOTX64.EFI" ] && cp "$U/out/BOOTX64.EFI" "$MNT_SYS/EFI/BOOT/BOOTX64.EFI"
[ -f "$ROOT/kernel/out/busybox" ] && cp "$ROOT/kernel/out/busybox" "$MNT_SYS/busybox"

for f in hook.sh optimize.sh aidiag.sh early_patch.sh hwdetect.sh install.sh apex_patch.sh gbos.cfg \
         crossystem bootdisk.img libkm_compat.so system_ext_manifest.xml soft_gk; do
  [ -f "$U/$f" ] && cp "$U/$f" "$MNT_SYS/gbos/$f"
done

for d in firmware patched audio installer kmod modules surface ksu; do
  [ -d "$U/$d" ] && { mkdir -p "$MNT_SYS/gbos/$d"; cp -R "$U/$d/." "$MNT_SYS/gbos/$d/"; }
done

# If super.img is in dist or build, copy it
for s in "$ROOT/dist" "$ROOT/build"; do
  if [ -f "$s/super.img" ]; then
    say "Copying $s/super.img..."
    cp "$s/super.img" "$MNT_SYS/gbos/super.img"
    [ -f "$s/super.layout" ] && cp "$s/super.layout" "$MNT_SYS/gbos/super.layout"
    break
  fi
done

# Ensure MODE=boot in gbos.cfg
sed -i 's/^MODE=.*/MODE=boot/' "$MNT_SYS/gbos/gbos.cfg" 2>/dev/null || true

# Copy kernel to EFI partition
MNT_EFI=$(mktemp -d)
mount "$EFI_PART" "$MNT_EFI"
mkdir -p "$MNT_EFI/EFI/gbos86"
if [ -f "$U/out/BOOTX64.EFI" ]; then
  cp "$U/out/BOOTX64.EFI" "$MNT_EFI/EFI/gbos86/BOOTX64.EFI"
  say "Installed gbOS86 UEFI kernel to $EFI_PART -> EFI/gbos86/BOOTX64.EFI"
fi
umount "$MNT_EFI" && rmdir "$MNT_EFI"

# Register in motherboard UEFI NVRAM if efibootmgr is available
if command -v efibootmgr >/dev/null 2>&1 && [ -d /sys/firmware/efi/efivars ]; then
  EFI_NUM=$(echo "$EFI_PART" | grep -o '[0-9]*$')
  EFI_DISK=$(echo "$EFI_PART" | sed -E 's/p?[0-9]+$//')
  if ! efibootmgr 2>/dev/null | grep -q "Googlebook OS"; then
    say "Registering 'Googlebook OS' in motherboard UEFI NVRAM..."
    efibootmgr -c -d "$EFI_DISK" -p "$EFI_NUM" -L "Googlebook OS" -l '\EFI\gbos86\BOOTX64.EFI' >/dev/null 2>&1 || true
  fi
fi

# Configure GRUB 40_custom if running on Linux with GRUB
if [ -d /etc/grub.d ] && command -v update-grub >/dev/null 2>&1; then
  SYS_UUID=$(blkid -s UUID -o value "$SYS_PART" 2>/dev/null || true)
  if [ -n "$SYS_UUID" ] && ! grep -q "Googlebook OS" /etc/grub.d/40_custom 2>/dev/null; then
    say "Adding gbOS86 dual-boot menu entry to /etc/grub.d/40_custom..."
    cat <<GRUB_EOF >> /etc/grub.d/40_custom

menuentry "Googlebook OS (gbOS86 persistent)" --class android --class gnu-linux --class os {
    insmod part_gpt
    insmod fat
    search --no-floppy --fs-uuid --set=root $SYS_UUID
    chainloader /EFI/BOOT/BOOTX64.EFI
}
GRUB_EOF
    update-grub || true
  fi
fi

sync
say "gbOS86 persistent dual-boot installation complete!"
echo "Reboot and select gbOS86 from GRUB or your UEFI boot menu (F12)."
