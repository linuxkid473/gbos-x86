#!/bin/sh
# test_port_scripts.sh: run the patch-binding code of usb/hook.sh, usb/early_patch.sh and usb/apex_patch.sh under the
# stick's own busybox (kernel/out/busybox, same applets and ash as stage0) in a throwaway container, with fake files and
# a recording `mount`. Checks every branch: hash matches, hash differs (log shows both hashes), patch file missing,
# target missing from the build, no MANIFEST, target already bound, and the PATCH-STATUS counts.
# Prints PORT_SCRIPTS_PASS. Needs docker and the gbos-erofs image (any amd64 Linux image works for this).
set -e
if [ "$1" != inner ]; then
  ROOT=$(cd "$(dirname "$0")/.." && pwd)
  [ -x "$ROOT/kernel/out/busybox" ] || { echo "kernel/out/busybox missing (sh tools/fetch_busybox.sh)"; exit 1; }
  Z=; [ "$(getenforce 2>/dev/null)" = Enforcing ] && Z=,z
  OUT=$(docker run --rm -v "$ROOT":/host:ro$Z gbos-erofs /host/kernel/out/busybox sh /host/tools/test_port_scripts.sh inner 2>&1) || true
  echo "$OUT" | grep -v "requested image's platform"
  echo "$OUT" | grep -q '^PORT_SCRIPTS_PASS$'
  exit $?
fi

H=/host/usb
T=/tmp/t; rm -rf $T; mkdir -p $T
FAIL=0
LOGF=$T/log; MNT=$T/mounts
log() { echo "$*" >> $LOGF; }
mount() { echo "$*" >> $MNT; return ${MOUNT_RC:-0}; }
reset() { : > $LOGF; : > $MNT; }
expect() { # description pattern(in log) [file]
  if grep -qF -- "$2" ${3:-$LOGF}; then echo "ok   $1"; else echo "FAIL $1 (wanted: $2)"; sed 's/^/     | /' ${3:-$LOGF}; FAIL=1; fi
}
reject() { if grep -qF -- "$2" ${3:-$LOGF}; then echo "FAIL $1 (unexpected: $2)"; FAIL=1; else echo "ok   $1"; fi; }

# --- extract the real function from hook.sh (so the test follows the shipped code)
sed -n '/^gbos_bind_patches() {/,/^}/p' $H/hook.sh > $T/fn.sh
[ -s $T/fn.sh ] || { echo "FAIL gbos_bind_patches not found in hook.sh"; exit 1; }
. $T/fn.sh

# --- fake Android root and gbos folder
G=$T/gbos; mkdir -p $G/patched $T/new/system/lib64 $T/new/vendor/lib64
echo orig-sys-libz > $T/new/system/lib64/libz.so
echo orig-ven-libz > $T/new/vendor/lib64/libz.so
echo orig-km > $T/new/system/lib64/libkm_compat.so
echo patched > $G/patched/system.libz.so; echo patched > $G/patched/vendor.libz.so; echo patched > $G/libkm_compat.so
S1=$(sha256sum $T/new/system/lib64/libz.so | cut -d' ' -f1)
S2=$(sha256sum $T/new/vendor/lib64/libz.so | cut -d' ' -f1)
S3=$(sha256sum $T/new/system/lib64/libkm_compat.so | cut -d' ' -f1)
WRONG=0000000000000000000000000000000000000000000000000000000000000000
mkman() { printf '# comment\nwatch vendor:x %s\napex lib64/libcrypto.so %s patched/c.so\n%s\n' "$WRONG" "$WRONG" "$*" > $G/patched/MANIFEST; }

echo "== gbos_bind_patches"
mkman "early $T/new/system/lib64/libz.so $S1 patched/system.libz.so
early $T/new/vendor/lib64/libz.so $S2 patched/vendor.libz.so
bind $T/new/system/lib64/libkm_compat.so $S3 libkm_compat.so"
reset; gbos_bind_patches
expect "all three bound" "PATCH-STATUS 3 applied, 0 not applied"
expect "mount --bind called for libz" "--bind $G/patched/system.libz.so $T/new/system/lib64/libz.so" $MNT
expect "mount --bind called for km_compat" "--bind $G/libkm_compat.so $T/new/system/lib64/libkm_compat.so" $MNT
reject "apex/watch lines ignored" "libcrypto" $MNT

mkman "early $T/new/system/lib64/libz.so $WRONG patched/system.libz.so"
reset; gbos_bind_patches
expect "changed original is NOT patched" "NOT patched (original is $S1, patch was made for $WRONG: run tools/port_build.sh)"
expect "status counts the failure" "PATCH-STATUS 0 applied, 1 not applied"
[ ! -s $MNT ] && echo "ok   nothing mounted over a changed original" || { echo "FAIL mounted over a changed original"; FAIL=1; }

mkman "early $T/new/system/lib64/libz.so $S1 patched/missing.so"
reset; gbos_bind_patches
expect "missing patch file" "NOT patched (missing patched/missing.so)"

mkman "early $T/new/system/lib64/gone.so $S1 patched/system.libz.so"
reset; gbos_bind_patches
expect "target not in this build" "NOT patched (not in this Google build)"

mkman "early /etc/hosts $S1 patched/system.libz.so"
reset; gbos_bind_patches
expect "already bound by early_patch.sh" "/etc/hosts already patched (early_patch.sh)"
expect "already bound counts as applied" "PATCH-STATUS 1 applied, 0 not applied"
[ ! -s $MNT ] && echo "ok   no second mount over an already bound target" || { echo "FAIL double mount"; FAIL=1; }

mkman "early $T/new/system/lib64/libz.so $S1 patched/system.libz.so"
reset; MOUNT_RC=1 gbos_bind_patches
expect "failed mount is reported" "NOT patched (bind mount failed)"

rm $G/patched/MANIFEST
reset; gbos_bind_patches
expect "no MANIFEST" "PATCH-STATUS patched/MANIFEST missing"

echo "== early_patch.sh"
mkman "early $T/new/system/lib64/libz.so $S1 patched/system.libz.so
early $T/new/vendor/lib64/libz.so $WRONG patched/vendor.libz.so
bind $T/new/system/lib64/libkm_compat.so $S3 libkm_compat.so"
# busybox ash runs the chroot applet for `command chroot`, so point that call at a recording stand-in
sed 's/command chroot/chroot_real/' $H/early_patch.sh > $T/early_patch.sh
chroot_real() { echo "chroot $*" >> $LOGF; }
. $T/early_patch.sh
reset; chroot /new /system/bin/dmctl a; chroot /new /system/bin/dmctl b
expect "early: patched the matching early line" "early: patched $T/new/system/lib64/libz.so"
reject "early: leaves a changed original alone" "early: patched $T/new/vendor/lib64/libz.so"
reject "early: ignores bind lines" "libkm_compat"
expect "chroot still runs" "chroot /new /system/bin/dmctl a"
[ "$(grep -c '^early: patched\|early: patched' $LOGF)" = 1 ] && echo "ok   bound once for two chroot calls" || { echo "FAIL early pass ran twice"; FAIL=1; }
[ "$(grep -c -- '--bind' $MNT)" = 1 ] && echo "ok   one mount" || { echo "FAIL expected one mount"; cat $MNT; FAIL=1; }
rm $G/patched/MANIFEST; GBOS_EARLY_DONE=; reset; chroot /new /system/bin/dmctl a
expect "no MANIFEST: boot continues" "chroot /new /system/bin/dmctl a"

echo "== apex_patch.sh"
A=$T/apex; mkdir -p $A/com.a/lib64 $A/com.b/lib64 $A/com.c/lib64 "$A/com.a@1/lib64"
echo apex-orig > $A/com.a/lib64/libcrypto.so; echo apex-orig > $A/com.b/lib64/libcrypto.so; echo other > $A/com.c/lib64/libcrypto.so
echo apex-orig > "$A/com.a@1/lib64/libcrypto.so"
AS=$(sha256sum $A/com.a/lib64/libcrypto.so | cut -d' ' -f1)
mkdir -p $T/pi/gbos/patched; echo patched > $T/pi/gbos/patched/conscrypt.libcrypto.so
printf 'apex lib64/libcrypto.so %s patched/conscrypt.libcrypto.so\nearly /x %s patched/x\n' $AS $WRONG > $T/pi/gbos/patched/MANIFEST
sed "s#/postinstall/gbos#$T/pi/gbos#g; s#/apex/\*#$A/*#g" $H/apex_patch.sh > $T/apex_patch.sh
reset
. $T/apex_patch.sh
expect "APEX copy with the pinned hash is bound" "apex_patch: patched $A/com.a/lib64/libcrypto.so"
expect "second identical copy is bound" "apex_patch: patched $A/com.b/lib64/libcrypto.so"
expect "different copy is left alone and logged" "apex_patch: $A/com.c/lib64/libcrypto.so left alone"
reject "versioned @ dirs skipped" "com.a@1"
[ "$(grep -c -- '-o bind' $MNT)" = 2 ] && echo "ok   exactly two APEX mounts" || { echo "FAIL expected two APEX mounts"; cat $MNT; FAIL=1; }

# Multiple apex lines for the same rel path (Conscrypt + Widevine/ClearKey libcrypto variants)
AS_C=$(sha256sum $A/com.c/lib64/libcrypto.so | cut -d' ' -f1)
echo patched-wv > $T/pi/gbos/patched/widevine.libcrypto.so
printf 'apex lib64/libcrypto.so %s patched/conscrypt.libcrypto.so\napex lib64/libcrypto.so %s patched/widevine.libcrypto.so\n' $AS $AS_C > $T/pi/gbos/patched/MANIFEST
reset
. $T/apex_patch.sh
expect "multi-rule: Conscrypt copy bound" "apex_patch: patched $A/com.a/lib64/libcrypto.so"
expect "multi-rule: Widevine copy bound" "apex_patch: patched $A/com.c/lib64/libcrypto.so"
reject "multi-rule: no spurious left-alone log" "left alone"
[ "$(grep -c -- '-o bind' $MNT)" = 3 ] && echo "ok   all three matching APEX copies bound" || { echo "FAIL expected three APEX mounts"; cat $MNT; FAIL=1; }

[ $FAIL = 0 ] && echo PORT_SCRIPTS_PASS || { echo PORT_SCRIPTS_FAIL; exit 1; }
