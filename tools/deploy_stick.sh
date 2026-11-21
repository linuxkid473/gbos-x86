#!/bin/sh
# Fast deploy to a mounted gbOS86 stick: copies only the files that differ from usb/ (small scripts and patches), removes files the project no longer ships, verifies every copied file after a remount, ejects.
# Takes seconds. It never touches the big/rarely changing items (super.img, firmware/, bootdisk.img; modules/ only when it is another kernel build's), the kernel
# (EFI partition) or the stick's own gbos.cfg; use build.sh / the image for those.
# Usage: tools/deploy_stick.sh [--no-eject] [VOLUME]      (VOLUME defaults to /Volumes/GBOS)
set -e
EJECT=1; [ "$1" = "--no-eject" ] && { EJECT=0; shift; }
VOL=${1:-/Volumes/GBOS}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
U=$ROOT/usb
G=$VOL/gbos
[ -d "$G" ] || { echo "no gbos folder on $VOL: plug the stick in (it mounts as /Volumes/GBOS)"; exit 1; }
CHANGED=$(mktemp); trap 'rm -f "$CHANGED"' EXIT

put() {  # put SRC DST(relative to gbos/): copy when different
  [ -f "$1" ] || return 0
  if ! cmp -s "$1" "$G/$2" 2>/dev/null; then
    mkdir -p "$(dirname "$G/$2")"
    printf '  %s\n' "$2"
    dd if="$1" of="$G/$2.part" bs=1m 2>/dev/null && mv -f "$G/$2.part" "$G/$2"
    echo "$2" >> "$CHANGED"
  fi
}

echo "deploying to $VOL"
for f in hook.sh stick_fat.sh install.sh aidiag.sh native_zygote.sh early_patch.sh sensor_vendor_overlay.sh sensor_trial_diag.sh tablet_mode.sh sensordiag.sh hwdetect.sh mask_vendor_modules.sh apex_patch.sh crossystem libkm_compat.so system_ext_manifest.xml soft_gk gbos_fp sysupdate.sh sysupdate.pub gbos86-updater.apk; do put "$U/$f" "$f"; done
if [ -n "${GBOS_ROTATION_POLICY_OVERLAY:-}" ]; then
  [ -s "$GBOS_ROTATION_POLICY_OVERLAY" ] || { echo "missing or empty rotation policy overlay: $GBOS_ROTATION_POLICY_OVERLAY" >&2; exit 1; }
  put "$GBOS_ROTATION_POLICY_OVERLAY" "sensors/gbos-latitude7320-rotation-policy.apk"
fi
for d in patched audio installer kmod ksu a11y surface thermal camera display; do
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
  fi
fi
# the stick's files that this project no longer ships (the removed AI stand-in)
[ -d "$G/localai" ] && { printf '  removing localai/\n'; rm -rf "$G/localai"; }
# gbos/system.manifest names the stick's exact set (docs/system-update.md); after a change it no longer does, and menu 1 would copy a
# wrong one to the laptop. Remake it with tools/make_system_update.sh manifest if you want the laptop to know its set.
if [ -s "$CHANGED" ] && [ -f "$G/system.manifest" ]; then printf '  removing system.manifest (it described the stick before this deploy)\n'; rm -f "$G/system.manifest"; fi
[ -s "$CHANGED" ] || echo "  nothing to copy: the stick already matches usb/"
sync

# verify from the disk, not the cache: remount (if it is a real volume), then compare every file we copied
DEV=$(diskutil info "$VOL" 2>/dev/null | awk -F: '/Device Identifier/{gsub(/ /,"",$2); print $2}')
WHOLE=$(diskutil info "$VOL" 2>/dev/null | awk -F: '/Part of Whole/{gsub(/ /,"",$2); print $2}')
if [ -n "$DEV" ] && [ -s "$CHANGED" ]; then
  diskutil unmount "$VOL" >/dev/null && diskutil mount "$DEV" >/dev/null
fi
BAD=0
while read -r f; do
  if [ "$f" = "sensors/gbos-latitude7320-rotation-policy.apk" ] &&
     [ -n "${GBOS_ROTATION_POLICY_OVERLAY:-}" ]; then
    VERIFY_SOURCE=$GBOS_ROTATION_POLICY_OVERLAY
  else
    VERIFY_SOURCE=$U/$f
  fi
  cmp -s "$VERIFY_SOURCE" "$G/$f" || { echo "VERIFY FAILED: $f"; BAD=1; }
done < "$CHANGED"
[ $BAD = 0 ] || { echo "some files did not verify; the stick is NOT ejected"; exit 1; }
echo "verified $(wc -l < "$CHANGED" | tr -d ' ') file(s)"
if [ $EJECT = 1 ] && [ -n "$WHOLE" ]; then
  sync; diskutil unmount "$VOL" >/dev/null 2>&1 || true
  diskutil eject "/dev/$WHOLE" | tail -1
fi
