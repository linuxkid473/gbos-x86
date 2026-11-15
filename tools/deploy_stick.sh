#!/bin/sh
# Fast deploy to a mounted gbOS86 stick: copies only the files that differ from usb/ (small scripts and patches), removes files the project no longer ships, verifies every copied file after a remount, ejects.
# Takes seconds. It never touches the big/rarely changing items (super.img, firmware/, bootdisk.img; modules/ only when it is another kernel build's), the kernel
# (EFI partition) or the stick's own gbos.cfg; use build.sh / the image for those.
set -e
EJECT=1 KERNEL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-eject) EJECT=0; shift;;
    --kernel) KERNEL=1; shift;;
    *) break;;
  esac
done
if [ -n "$1" ]; then VOL=$1
elif [ "$(uname)" = Darwin ]; then VOL=/Volumes/GBOS
else VOL=$(findmnt -rn -S LABEL=GBOS -o TARGET 2>/dev/null | head -1); VOL=${VOL:-/run/media/$USER/GBOS}
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
U=$ROOT/usb
G=$VOL/gbos
[ -d "$G" ] || { echo "no gbos folder on $VOL: plug the stick in (macOS: /Volumes/GBOS; Linux: mount the GBOS partition) or pass its mount point"; exit 1; }
CHANGED=$(mktemp); trap 'rm -f "$CHANGED"' EXIT

put() {  # put SRC DST(relative to gbos/): copy when different
  [ -f "$1" ] || return 0
  if ! cmp -s "$1" "$G/$2" 2>/dev/null; then
    mkdir -p "$(dirname "$G/$2")"
    printf '  %s\n' "$2"
    dd if="$1" of="$G/$2.part" bs=1048576 2>/dev/null && mv -f "$G/$2.part" "$G/$2"
    echo "$2" >> "$CHANGED"
  fi
}

echo "deploying to $VOL"
for f in hook.sh install.sh aidiag.sh native_zygote.sh early_patch.sh hwdetect.sh apex_patch.sh crossystem libkm_compat.so system_ext_manifest.xml soft_gk; do put "$U/$f" "$f"; done
for d in patched audio installer kmod ksu a11y surface device; do
  [ -d "$U/$d" ] || continue
  (cd "$U" && find "$d" -type f ! -name '.DS_Store') | while read -r f; do put "$U/$f" "$f"; done
done
# The module tree belongs to one kernel build (directory = the kernel's release, signed with that build's key): an old tree on the stick
# gives "no driver for anything" (i915, Wi-Fi, audio) on a newer kernel, so replace it when the build id differs. ~100 MB, only then.
MODS=$U/modules/lib/modules
if [ -d "$MODS" ]; then
  R=$(ls "$MODS" | head -1)
  if [ -n "$R" ] && ! cmp -s "$MODS/$R/gbos-build-id" "$G/modules/lib/modules/$R/gbos-build-id" 2>/dev/null; then
    printf '  modules/ (kernel %s: the stick has %s)\n' "$R" "$(ls "$G/modules/lib/modules" 2>/dev/null | tr '\n' ' ')"
    rm -rf "$G/modules" && cp -R "$U/modules" "$G/modules" && echo "modules/lib/modules/$R/gbos-build-id" >> "$CHANGED"
  elif [ -n "$R" ]; then
    (cd "$U" && find "modules/lib/modules/$R" -type f) | while read -r f; do put "$U/$f" "$f"; done
  fi
fi
KCHANGED=$(mktemp); trap 'rm -f "$CHANGED" "$KCHANGED"' EXIT
if [ $KERNEL = 1 ]; then
  K=$U/out/BOOTX64.EFI
  [ -f "$K" ] || { echo "no $K (kernel/build_kernel.sh builds it)"; exit 1; }
  if [ "$(uname)" = Darwin ]; then EFIVOL=/Volumes/EFI
  else
    EFIDEV=$(lsblk -nro PATH,LABEL "/dev/$(lsblk -no PKNAME "$(findmnt -rn -T "$VOL" -o SOURCE)" | head -1)" 2>/dev/null | awk '$2=="EFI" {print $1; exit}')
    EFIVOL=$([ -n "$EFIDEV" ] && findmnt -rn -S "$EFIDEV" -o TARGET | head -1)
    if [ -z "$EFIVOL" ] && [ -n "$EFIDEV" ]; then
      udisksctl mount -b "$EFIDEV" >/dev/null 2>&1 && EFIVOL=$(findmnt -rn -S "$EFIDEV" -o TARGET | head -1)
    fi
  fi
  [ -n "$EFIVOL" ] && [ -d "$EFIVOL/EFI/BOOT" ] || { echo "the stick's EFI partition is not mounted (${EFIVOL:-not found}): mount it and retry"; exit 1; }
  for dst in "$EFIVOL/EFI/BOOT/BOOTX64.EFI" "$VOL/EFI/BOOT/BOOTX64.EFI"; do
    [ -d "$(dirname "$dst")" ] || continue
    if ! cmp -s "$K" "$dst"; then
      printf '  kernel -> %s (%s)\n' "$dst" "$(grep -a -m1 -o '#[0-9]\+ SMP' "$K" | head -1)"
      dd if="$K" of="$dst.part" bs=1048576 2>/dev/null && mv -f "$dst.part" "$dst"
      echo "$dst" >> "$KCHANGED"
    fi
  done
fi
# the stick's files that this project no longer ships (the removed AI stand-in)
[ -d "$G/localai" ] && { printf '  removing localai/\n'; rm -rf "$G/localai"; }
[ -d "$G/camera" ] && { printf '  removing camera/ (now device/)\n'; rm -rf "$G/camera"; }
[ -s "$CHANGED" ] || [ -s "$KCHANGED" ] || echo "  nothing to copy: the stick already matches usb/"
sync

# verify from the disk, not the cache: remount (if it is a real volume), then compare every file we copied
DEV= WHOLE=
if [ "$(uname)" = Darwin ]; then
  DEV=$(diskutil info "$VOL" 2>/dev/null | awk -F: '/Device Identifier/{gsub(/ /,"",$2); print $2}')
  WHOLE=$(diskutil info "$VOL" 2>/dev/null | awk -F: '/Part of Whole/{gsub(/ /,"",$2); print $2}')
  if [ -n "$DEV" ] && [ -s "$CHANGED" ]; then
    diskutil unmount "$VOL" >/dev/null && diskutil mount "$DEV" >/dev/null
  fi
else
  DEV=$(findmnt -rn -T "$VOL" -o SOURCE 2>/dev/null)
  [ -n "$DEV" ] && WHOLE=$(lsblk -no PKNAME "$DEV" 2>/dev/null | head -1)
  if [ -s "$CHANGED" ]; then
    while read -r f; do dd if="$G/$f" iflag=nocache count=0 2>/dev/null; dd of="$G/$f" oflag=nocache conv=notrunc,fdatasync count=0 2>/dev/null; done < "$CHANGED"
  fi
fi
BAD=0
while read -r f; do
  cmp -s "$U/$f" "$G/$f" || { echo "VERIFY FAILED: $f"; BAD=1; }
done < "$CHANGED"
while read -r f; do
  [ "$(uname)" = Darwin ] || dd if="$f" iflag=nocache count=0 2>/dev/null
  cmp -s "$U/out/BOOTX64.EFI" "$f" || { echo "VERIFY FAILED: $f"; BAD=1; }
done < "$KCHANGED"
[ $BAD = 0 ] || { echo "some files did not verify; the stick is NOT ejected"; exit 1; }
echo "verified $(( $(wc -l < "$CHANGED") + $(wc -l < "$KCHANGED") )) file(s)"
if [ $EJECT = 1 ] && [ -n "$WHOLE" ]; then
  sync
  if [ "$(uname)" = Darwin ]; then
    diskutil unmount "$VOL" >/dev/null 2>&1 || true
    diskutil eject "/dev/$WHOLE" | tail -1
  else
    for p in $(lsblk -nro NAME,MOUNTPOINT "/dev/$WHOLE" | awk '$2!="" {print $1}'); do udisksctl unmount -b "/dev/$p" >/dev/null 2>&1 || umount "/dev/$p" 2>/dev/null || true; done
    udisksctl power-off -b "/dev/$WHOLE" >/dev/null 2>&1 && echo "ejected /dev/$WHOLE" || echo "unmounted; unplug the stick (/dev/$WHOLE)"
  fi
fi
