#!/bin/sh
# Fast deploy to a mounted gbOS86 stick: copies only the files that differ from usb/ (small scripts and patches), removes files the project no longer ships, verifies every copied file after a remount, ejects.
# Takes seconds. It never touches the big/rarely changing items (super.img, firmware/, bootdisk.img; modules/ only when it is another kernel build's), the kernel
# (EFI partition) or the stick's own gbos.cfg; use build.sh / the image for those.
# Usage: tools/deploy_stick.sh [--no-eject] [--over-set] [VOLUME]      (VOLUME defaults to /Volumes/GBOS)
# A stick with gbos/system.manifest holds a published update set (docs/system-update.md): copying files over it makes it no longer that
# set, so the updater can no longer apply deltas to it (2026-11-22: a hand-deployed APK broke the lab update test). The script then
# lists what it would change and stops; ship the change as an update (tools/publish_system_update.sh), or pass --over-set.
set -e
EJECT=1; OVERSET=0
while [ $# -gt 0 ]; do case "$1" in --no-eject) EJECT=0; shift;; --over-set) OVERSET=1; shift;; *) break;; esac; done
VOL=${1:-/Volumes/GBOS}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
U=$ROOT/usb
G=$VOL/gbos
[ -d "$G" ] || { echo "no gbos folder on $VOL: plug the stick in (it mounts as /Volumes/GBOS)"; exit 1; }
CHANGED=$(mktemp); trap 'rm -f "$CHANGED"' EXIT

put() {  # put SRC DST(relative to gbos/): copy when different (DRY=1: only list it)
  [ -f "$1" ] || return 0
  if ! cmp -s "$1" "$G/$2" 2>/dev/null; then
    [ "${DRY:-0}" = 1 ] && { echo "$2" >> "$CHANGED"; return 0; }
    mkdir -p "$(dirname "$G/$2")"
    printf '  %s\n' "$2"
    dd if="$1" of="$G/$2.part" bs=1m 2>/dev/null && mv -f "$G/$2.part" "$G/$2"
    echo "$2" >> "$CHANGED"
  fi
}

put_all() {
for f in hook.sh optimize.sh stick_fat.sh install.sh aidiag.sh native_zygote.sh early_patch.sh sensor_vendor_overlay.sh sensor_trial_diag.sh tablet_mode.sh sensordiag.sh hwdetect.sh mask_vendor_modules.sh apex_patch.sh crossystem libkm_compat.so system_ext_manifest.xml soft_gk gbos_fp sysupdate.sh sysupdate.pub gbos86-updater.apk; do put "$U/$f" "$f"; done
for d in patched audio installer kmod ksu a11y surface thermal camera display; do
  [ -d "$U/$d" ] || continue
  (cd "$U" && find "$d" -type f ! -name '.DS_Store') | while read -r f; do put "$U/$f" "$f"; done
done
}
if [ -f "$G/system.manifest" ] && [ $OVERSET = 0 ]; then
  DRY=1 put_all; DRY=0
  R=$(ls "$U/modules/lib/modules" 2>/dev/null | head -1)
  [ -n "$R" ] && ! cmp -s "$U/modules/lib/modules/$R/gbos-build-id" "$G/modules/lib/modules/$R/gbos-build-id" 2>/dev/null &&
    echo "modules/ (kernel $R)" >> "$CHANGED"
  if [ -s "$CHANGED" ]; then
    echo "refusing: $VOL holds the published update set seq $(sed -n 's/^seq //p' "$G/system.manifest" | head -n 1) (gbos/system.manifest), and these differ from usb/:"
    sed 's/^/  /' "$CHANGED"
    echo "Copying them would make the stick no longer that set (the updater could not apply deltas to it). Ship them as an update"
    echo "(tools/publish_system_update.sh, then copy the bundle into gbos/update.inbox), or run again with --over-set."
    exit 2
  fi
fi
echo "deploying to $VOL"
put_all
if [ -f "$G/gbos.cfg" ]; then
  if grep -q '^SUSPEND_MODE=s2idle$' "$G/gbos.cfg" 2>/dev/null; then
    printf '  gbos.cfg (SUSPEND_MODE=s2idle -> guard)\n'
    sed 's/^SUSPEND_MODE=s2idle$/SUSPEND_MODE=guard/' "$G/gbos.cfg" > "$G/gbos.cfg.part" && mv -f "$G/gbos.cfg.part" "$G/gbos.cfg"
    echo "CFG_GUARD" >> "$CHANGED"
  fi
  if ! grep -q '^#\{0,1\}LID_SHUTDOWN_MIN=' "$G/gbos.cfg" 2>/dev/null; then
    printf '  gbos.cfg (+LID_SHUTDOWN_MIN=60)\n'
    { cat "$G/gbos.cfg"
      printf '\n# LID_SHUTDOWN_MIN=60: cleanly shut down after N minutes lid-closed on battery (0 = disable).\nLID_SHUTDOWN_MIN=60\n'
    } > "$G/gbos.cfg.part" && mv -f "$G/gbos.cfg.part" "$G/gbos.cfg"
    echo "CFG_LIDSHUT" >> "$CHANGED"
  fi
fi
# The module tree belongs to one kernel build (directory = the kernel's release, signed with that build's key): an old tree on the stick
# gives "no driver for anything" (i915, Wi-Fi, audio) on a newer kernel, so replace it when the build id differs. ~100 MB, only then.
MODS=$U/modules/lib/modules
if [ -d "$MODS" ]; then
  R=$(ls "$MODS" | head -1)
  if [ -n "$R" ] && ! cmp -s "$MODS/$R/gbos-build-id" "$G/modules/lib/modules/$R/gbos-build-id" 2>/dev/null; then
    printf '  modules/ (kernel %s: the stick has %s)\n' "$R" "$(ls "$G/modules/lib/modules" 2>/dev/null | tr '\n' ' ')"
    rm -rf "$G/modules" && cp -R "$U/modules" "$G/modules" && echo "modules/lib/modules/$R/gbos-build-id" >> "$CHANGED"
  fi
fi
# Detect macOS 15+ FSKit MSDOS mounts up front: ANY diskutil call (info/mount/unmount) on a disk with an active
# com.apple.fskit.msdos mount can deadlock inside diskarbitrationd/fskitd, so derive DEV/WHOLE from mount(8) instead.
IS_FSKIT=0
DEV=$(mount 2>/dev/null | awk -v v="$VOL" '$3 == v { sub("^/dev/", "", $1); print $1; exit }')
mount 2>/dev/null | grep " on $VOL (" | grep -q 'fskit' && IS_FSKIT=1
WHOLE=$(printf '%s' "$DEV" | sed 's/s[0-9][0-9]*$//')
if [ -z "$DEV" ] && [ $IS_FSKIT = 0 ]; then
  DEV=$(diskutil info "$VOL" 2>/dev/null | awk -F: '/Device Identifier/{gsub(/ /,"",$2); print $2}')
  WHOLE=$(diskutil info "$VOL" 2>/dev/null | awk -F: '/Part of Whole/{gsub(/ /,"",$2); print $2}')
fi

# Kernel image on GBOS/EFI/BOOT/ (and /Volumes/EFI/EFI/BOOT/ if mounted; never call diskutil mount under FSKit)
ESP_DONE=0
if [ -f "$U/out/BOOTX64.EFI" ]; then
  if [ ! -d /Volumes/EFI/EFI/BOOT ] && [ -n "$WHOLE" ] && [ $IS_FSKIT = 0 ]; then
    diskutil info "${WHOLE}s1" 2>/dev/null | grep -qE 'EFI|FAT' && diskutil mount "${WHOLE}s1" >/dev/null 2>&1 || true
  fi
  if [ -d "$VOL/EFI/BOOT" ]; then
    if ! cmp -s "$U/out/BOOTX64.EFI" "$VOL/EFI/BOOT/BOOTX64.EFI" 2>/dev/null; then
      printf '  EFI/BOOT/BOOTX64.EFI (on %s)\n' "$VOL"
      dd if="$U/out/BOOTX64.EFI" of="$VOL/EFI/BOOT/BOOTX64.EFI.part" bs=1m 2>/dev/null && mv -f "$VOL/EFI/BOOT/BOOTX64.EFI.part" "$VOL/EFI/BOOT/BOOTX64.EFI"
      echo "EFI_UPDATED" >> "$CHANGED"
    fi
  fi
  if [ -d /Volumes/EFI/EFI/BOOT ]; then
    if ! cmp -s "$U/out/BOOTX64.EFI" /Volumes/EFI/EFI/BOOT/BOOTX64.EFI 2>/dev/null; then
      printf '  /Volumes/EFI/EFI/BOOT/BOOTX64.EFI\n'
      dd if="$U/out/BOOTX64.EFI" of=/Volumes/EFI/EFI/BOOT/BOOTX64.EFI.part bs=1m 2>/dev/null && mv -f /Volumes/EFI/EFI/BOOT/BOOTX64.EFI.part /Volumes/EFI/EFI/BOOT/BOOTX64.EFI
      echo "ESP_UPDATED" >> "$CHANGED"
    fi
    ESP_DONE=1
  fi
fi
# the stick's files that this project no longer ships (the removed AI stand-in; the unsigned rotation overlay, now display/)
[ -d "$G/localai" ] && { printf '  removing localai/\n'; rm -rf "$G/localai"; }
# gbos/system.manifest names the stick's exact set (docs/system-update.md); after a change it no longer does, and menu 1 would copy a
# wrong one to the laptop. Remake it with tools/make_system_update.sh manifest if you want the laptop to know its set.
if [ -s "$CHANGED" ] && [ -f "$G/system.manifest" ]; then printf '  removing system.manifest (it described the stick before this deploy)\n'; rm -f "$G/system.manifest"; fi
[ -f "$G/sensors/gbos-latitude7320-rotation-policy.apk" ] &&
  { printf '  removing sensors/gbos-latitude7320-rotation-policy.apk (unsigned; display/ has the signed one)\n'
    rm -f "$G/sensors/gbos-latitude7320-rotation-policy.apk"; }
[ -s "$CHANGED" ] || echo "  nothing to copy: the stick already matches usb/"
echo "syncing writes to disk..."
sync

# verify copied files (skip diskutil unmount/remount on macOS 15+ FSKit MSDOS mounts)
if [ -n "$DEV" ] && [ -s "$CHANGED" ] && [ $EJECT = 1 ] && [ $IS_FSKIT = 0 ]; then
  echo "remounting $VOL ($DEV) to verify..."
  (diskutil unmount force "$VOL" >/dev/null && diskutil mount "$DEV" >/dev/null) || true
fi
BAD=0
while read -r f; do
  case "$f" in
    EFI_UPDATED) cmp -s "$U/out/BOOTX64.EFI" "$VOL/EFI/BOOT/BOOTX64.EFI" || { echo "VERIFY FAILED: $VOL/EFI/BOOT/BOOTX64.EFI"; BAD=1; };;
    ESP_UPDATED) cmp -s "$U/out/BOOTX64.EFI" /Volumes/EFI/EFI/BOOT/BOOTX64.EFI || { echo "VERIFY FAILED: /Volumes/EFI/EFI/BOOT/BOOTX64.EFI"; BAD=1; };;
    CFG_GUARD) grep -q '^SUSPEND_MODE=guard$' "$G/gbos.cfg" 2>/dev/null || { echo "VERIFY FAILED: gbos.cfg SUSPEND_MODE=guard"; BAD=1; };;
    CFG_LIDSHUT) grep -q '^LID_SHUTDOWN_MIN=' "$G/gbos.cfg" 2>/dev/null || { echo "VERIFY FAILED: gbos.cfg LID_SHUTDOWN_MIN"; BAD=1; };;
    *) cmp -s "$U/$f" "$G/$f" || { echo "VERIFY FAILED: $f"; BAD=1; };;
  esac
done < "$CHANGED"
[ $BAD = 0 ] || { echo "some files did not verify; the stick is NOT ejected"; exit 1; }
echo "verified $(wc -l < "$CHANGED" | tr -d ' ') file(s)"
if [ $EJECT = 1 ]; then
  if [ $IS_FSKIT = 1 ]; then
    sync
    _um="$VOL"; [ -d /Volumes/EFI ] && _um="$VOL /Volumes/EFI"
    if [ $ESP_DONE = 0 ] && [ -n "$WHOLE" ] && [ -f "$U/out/BOOTX64.EFI" ] && grep -q '^EFI_UPDATED$' "$CHANGED" 2>/dev/null; then
      mkdir -p "$ROOT/build"
      if [ ! -f "$ROOT/build/efi_esp.fat" ] || [ "$U/out/BOOTX64.EFI" -nt "$ROOT/build/efi_esp.fat" ]; then
        echo "building 256M EFI partition image (build/efi_esp.fat) to bypass macOS FSKit..."
        docker run --rm --platform linux/amd64 -v "$U/out":/in:ro -v "$ROOT/build":/out alpine:latest sh -c '
          set -e; apk add -q mtools dosfstools >/dev/null
          rm -f /out/efi_esp.fat.tmp; truncate -s 256M /out/efi_esp.fat.tmp
          mkfs.fat -F 32 -n EFI /out/efi_esp.fat.tmp >/dev/null
          mmd -i /out/efi_esp.fat.tmp ::/EFI ::/EFI/BOOT
          mcopy -i /out/efi_esp.fat.tmp /in/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
          mv -f /out/efi_esp.fat.tmp /out/efi_esp.fat'
      fi
      echo "macOS 15.8 FSKit detected: unmount GBOS and flash kernel to EFI partition (/dev/r${WHOLE}s1) with:"
      echo "  sudo umount -f $_um && sudo dd if=build/efi_esp.fat of=/dev/r${WHOLE}s1 bs=1m && sync"
    else
      echo "macOS FSKit FAT32 detected: run 'sudo umount -f $_um' before unplugging (avoids macOS 15.8 diskutil hang)."
    fi
  elif [ -n "$WHOLE" ]; then
    echo "ejecting /dev/$WHOLE..."
    sync; diskutil unmount force "$VOL" >/dev/null 2>&1 || true
    diskutil eject "/dev/$WHOLE" | tail -1
  fi
fi
