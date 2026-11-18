#!/bin/sh
# Tests for usb/stick_fat.sh (clean FAT at shutdown, read-only fsck at boot) and its hooks in hook.sh / install.sh.
#   sh tools/test_stick_fat.sh                       host tests (no root): real FAT images, the stick's fsck.fat, a fake /proc, a FIFO as /dev/kmsg
#   BUSYBOX=/path/busybox sh tools/test_stick_fat.sh the same with every applet from that busybox (use the stick's /busybox for parity)
#   FAT_KERNEL_TEST=1 BUSYBOX=... sh tools/test_stick_fat.sh   also the real vfat driver in a privileged docker container (loop device):
#                                                    superblock remount from a root with no /proc and /dev, EBUSY with a writer,
#                                                    the dirty flag before/after, clearing it while mounted
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
U=$ROOT/usb
TMP=$(mktemp -d)
PIDS=
cleanup() { for p in $PIDS; do kill -KILL "$p" 2>/dev/null || :; done; rm -rf "$TMP"; }
trap cleanup EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok - $*"; }
LOADER="$U/installer/lib/ld-musl-x86_64.so.1 --library-path $U/installer/lib"
BB=${BUSYBOX:-}

# --- syntax
for f in stick_fat.sh hook.sh install.sh stage0_init.sh apex_patch.sh; do sh -n "$U/$f" || fail "sh -n usb/$f"; done
[ -z "$BB" ] || for f in stick_fat.sh hook.sh install.sh; do "$BB" sh -n "$U/$f" || fail "busybox sh -n usb/$f"; done
ok "syntax"

# --- wiring
grep -q '^if \[ -f \$G/stick_fat.sh \] && \. \$G/stick_fat.sh; then$' "$U/hook.sh" || fail "hook.sh does not source stick_fat.sh"
grep -q '^  gbos_fat_boot_check "\$DEV" /gbos "\$L"$' "$U/hook.sh" || fail "hook.sh: no boot check"
grep -q '^  gbos_fat_start_shutdown_watch "\$DEV" /gbos "\$L/kmsg.log"$' "$U/hook.sh" || fail "hook.sh: no shutdown watcher"
[ "$(grep -n 'gbos_fat_boot_check "\$DEV"' "$U/hook.sh" | cut -d: -f1)" -lt "$(grep -n '^gbos_bind_patches() {' "$U/hook.sh" | cut -d: -f1)" ] \
  || fail "the FAT check must run before hook.sh starts binding and writing"
! grep -nE '^[[:space:]]*(poweroff|reboot) -f' "$U/install.sh" || fail "install.sh powers off without fat_off"
[ "$(grep -cE '^[[:space:]]*fat_off (poweroff|reboot)$' "$U/install.sh")" -ge 6 ] || fail "install.sh: fat_off calls missing"
grep -q '^FILES="super.img super.layout hook.sh stick_fat.sh ' "$U/install.sh" || fail "install.sh FILES lacks stick_fat.sh"
grep -q '^\[ -f \$G/stick_fat.sh \] && \. \$G/stick_fat.sh && gbos_fat_boot_check "\$DEV" /gbos "\$L"$' "$U/install.sh" || fail "install.sh: no boot check"
for t in deploy_stick.sh make_gbos86_image.sh install_dualboot.sh; do grep -q 'for f in hook.sh stick_fat.sh ' "$ROOT/tools/$t" || fail "tools/$t does not ship stick_fat.sh"; done
grep -q 'mkfs.fat fsck.fat blkid' "$ROOT/tools/fetch_installer_tools.sh" || fail "fetch_installer_tools.sh does not fetch fsck.fat"
[ -x "$U/installer/bin/fsck.fat" ] || fail "usb/installer/bin/fsck.fat missing"
$LOADER "$U/installer/bin/fsck.fat" --help 2>&1 | grep -q -- '-n  *no-op' || fail "fsck.fat does not run under the stick's musl loader"
ok "wiring (hook.sh, install.sh, ship lists, fsck.fat runs under the stick's loader)"

# --- library under test, with host tools (or $BUSYBOX applets) instead of /gbos/busybox
LOG=$TMP/stage0.log; : > "$LOG"
log() { echo "$*" >> "$LOG"; }
if [ -n "$BB" ]; then
  mkdir -p "$TMP/bbin"; for a in od tr dd df sync timeout grep cut cat awk sleep printf; do ln -s "$BB" "$TMP/bbin/$a"; done
  PATH=$TMP/bbin:$PATH; export PATH
fi
GBOS_FAT_BB=; GBOS_PROC=$TMP/proc; mkdir -p "$GBOS_PROC"
. "$U/stick_fat.sh"
G=$TMP/g; mkdir -p "$G/installer"; ln -s "$U/installer/bin" "$G/installer/bin"; ln -s "$U/installer/lib" "$G/installer/lib"

mkimg() { # FILE 32|16
  rm -f "$1"; dd if=/dev/zero of="$1" bs=1048576 count=64 2>/dev/null
  $LOADER "$U/installer/bin/mkfs.fat" -F "$2" -n GBOS "$1" >/dev/null || fail "mkfs.fat -F $2"
}
setbyte() { printf "\\$(printf %o "$3")" | dd of="$1" bs=1 seek="$2" count=1 conv=notrunc 2>/dev/null; }
fsck_n() { $LOADER "$U/installer/bin/fsck.fat" -n "$1" > "$2" 2>&1; }
# a lost cluster: cluster 5 marked end-of-chain in the first FAT only
lose_cluster() { # FILE 32|16
  rs=$(od -A n -t u2 -j 14 -N 2 "$1" | tr -d ' ')
  if [ "$2" = 32 ]; then printf '\377\377\377\017' | dd of="$1" bs=1 seek=$((rs * 512 + 20)) conv=notrunc 2>/dev/null
  else printf '\377\377' | dd of="$1" bs=1 seek=$((rs * 512 + 10)) conv=notrunc 2>/dev/null; fi
}

# --- boot sector parsing and the dirty bit
mkimg "$TMP/f32" 32; mkimg "$TMP/f16" 16
[ "$(gbos_fat_state_offset "$TMP/f32")" = 65 ] || fail "FAT32 state offset"
[ "$(gbos_fat_state_offset "$TMP/f16")" = 37 ] || fail "FAT16 state offset"
dd if=/dev/zero of="$TMP/zero" bs=512 count=4 2>/dev/null
! gbos_fat_state_offset "$TMP/zero" >/dev/null || fail "an all-zero sector parsed as FAT"
cp "$TMP/f32" "$TMP/nosig"; setbyte "$TMP/nosig" 510 0
! gbos_fat_state_offset "$TMP/nosig" >/dev/null || fail "a sector without 55 AA parsed as FAT"
for t in 32 16; do
  o=$(gbos_fat_state_offset "$TMP/f$t")
  [ "$(gbos_fat_byte "$TMP/f$t" "$o")" = 0 ] || fail "FAT$t fresh state not 0"
  setbyte "$TMP/f$t" "$o" 3
  gbos_fat_clear_dirty "$TMP/f$t" || fail "FAT$t clear"
  [ "$(gbos_fat_byte "$TMP/f$t" "$o")" = 2 ] || fail "FAT$t clear changed more than bit 0"
  setbyte "$TMP/f$t" "$o" 0
done
cp "$TMP/f32" "$TMP/f32.ref"; gbos_fat_clear_dirty "$TMP/f32"; cmp -s "$TMP/f32" "$TMP/f32.ref" || fail "clearing a clean volume wrote something"
ok "state byte: FAT32 @65, FAT16 @37, only bit 0 cleared, non-FAT rejected"

# --- fsck.fat -n classification (dosfstools 4.2 output)
for t in 32 16; do
  o=$(gbos_fat_state_offset "$TMP/f$t")
  cp "$TMP/f$t" "$TMP/d$t"; setbyte "$TMP/d$t" "$o" 1
  fsck_n "$TMP/d$t" "$TMP/out" && fail "fsck.fat -n exited 0 on a dirty FAT$t"
  gbos_fsck_only_dirty "$TMP/out" "$o" || { cat "$TMP/out"; fail "FAT$t dirty-only not recognised"; }
  cp "$TMP/d$t" "$TMP/l$t"; lose_cluster "$TMP/l$t" "$t"
  fsck_n "$TMP/l$t" "$TMP/out" || :
  ! gbos_fsck_only_dirty "$TMP/out" "$o" || { cat "$TMP/out"; fail "FAT$t lost cluster taken as dirty-only"; }
done
cp "$TMP/d32" "$TMP/fi32"; printf '\012\000\000\000' | dd of="$TMP/fi32" bs=1 seek=1000 conv=notrunc 2>/dev/null   # FSInfo free count
fsck_n "$TMP/fi32" "$TMP/out" || :; grep -q 'Free cluster summary wrong' "$TMP/out" || fail "FSInfo fixture"
gbos_fsck_only_dirty "$TMP/out" 65 || fail "a stale FSInfo free count is only a hint"
printf 'fsck.fat 4.2 (2021-01-31)\nDirty bit is set. Fs was not properly unmounted and some data may be corrupt.\n Automatically removing dirty bit.\n' > "$TMP/out"
! gbos_fsck_only_dirty "$TMP/out" 65 || fail "an fsck cut short (no summary line) taken as dirty-only"
printf 'fsck.fat 4.2 (2021-01-31)\nThere are differences between boot sector and its backup.\nThis is mostly harmless. Differences: (offset:original/backup)\n  65:01/00\n  71:47/48\n  Not automatically fixing this.\nLeaving filesystem unchanged.\n/dev/sda2: 1 files, 1/9 clusters\n' > "$TMP/out"
! gbos_fsck_only_dirty "$TMP/out" 65 || fail "a backup boot sector differing elsewhere taken as dirty-only"
ok "fsck.fat -n: dirty-only (FAT32/16, FSInfo hint) vs lost cluster / truncated run / other boot-sector change"

# --- fake /proc with stage0's copier
sleep 300 & SUB=$!; sleep 300 & CAT=$!; PIDS="$PIDS $SUB $CAT"
mkp() { mkdir -p "$GBOS_PROC/$1"; printf "$2" > "$GBOS_PROC/$1/cmdline"; echo "$1 ($3) S $4 0 0 0" > "$GBOS_PROC/$1/stat"; }
mkp 2 '' kthreadd 0
mkp 231 '/gbos/busybox\000sleep\0005\000' 'busybox' 230
mkp "$CAT" '/gbos/busybox\000cat\000' 'bu) sy (box' "$SUB"
mkp 400 '/system/bin/logcat\000-b\000all\000' logcat 1
[ "$(gbos_fat_copier)" = "$SUB $CAT" ] || fail "copier: got '$(gbos_fat_copier)', want '$SUB $CAT'"
ok "kmsg copier found by its cmdline (subshell = its parent)"

# --- boot check, end to end on image files
pstate() { sed -n 's/^State:[[:space:]]*\([A-Z]\).*/\1/p' "/proc/$1/status"; }
DM=$TMP/dmesg; GBOS_DMESG="cat $DM"
bootcheck() { # IMAGE: runs the check with a fresh log dir, the image as the device
  rm -rf "$TMP/L"; mkdir -p "$TMP/L"; : > "$LOG"
  gbos_fat_boot_check "$1" "$TMP" "$TMP/L"
  [ "$(pstate "$CAT")" = S ] || fail "the copier was left stopped"
}
cp "$TMP/d32" "$TMP/sda2"; echo "[   25.7][ T226] FAT-fs (sda2): Volume was not properly unmounted. Some data may be corrupt. Please run fsck." > "$DM"
bootcheck "$TMP/sda2"
grep -q 'FAT-CHECK .*/sda2: previous shutdown NOT clean; fsck.fat -n: no errors besides the dirty flag; dirty flag cleared$' "$LOG" || { cat "$LOG"; fail "dirty-only: not cleared"; }
[ "$(gbos_fat_byte "$TMP/sda2" 65)" = 0 ] || fail "dirty bit still set on disk"
fsck_n "$TMP/sda2" "$TMP/out" || { cat "$TMP/out"; fail "volume not clean after clearing"; }
grep -q '^--- fsck.fat -n .*/sda2 (rc=1, uptime' "$TMP/L/fat-check.txt" && grep -q '^Dirty bit is set' "$TMP/L/fat-check.txt" || fail "fat-check.txt lacks the fsck output"
cp "$TMP/l32" "$TMP/sda2"; bootcheck "$TMP/sda2"
grep -q 'FAT-CHECK .*previous shutdown NOT clean; fsck.fat -n rc=1: PROBLEMS FOUND, nothing changed' "$LOG" || { cat "$LOG"; fail "problems: not reported"; }
cmp -s "$TMP/sda2" "$TMP/l32" || fail "a volume with problems was modified"
cp "$TMP/d32" "$TMP/sda2"; FAT_CLEAR_DIRTY=0; bootcheck "$TMP/sda2"; unset FAT_CLEAR_DIRTY
grep -q 'no errors besides the dirty flag; left set (FAT_CLEAR_DIRTY=0)$' "$LOG" || fail "FAT_CLEAR_DIRTY=0"
[ "$(gbos_fat_byte "$TMP/sda2" 65)" = 1 ] || fail "FAT_CLEAR_DIRTY=0 cleared it"
FAT_FSCK=0; bootcheck "$TMP/sda2"; unset FAT_FSCK; grep -q 'previous shutdown NOT clean; fsck skipped (FAT_FSCK=0)$' "$LOG" || fail "FAT_FSCK=0"
: > "$DM"   # the kernel said nothing at the rw mount: clean
bootcheck "$TMP/sda2"; grep -q 'FAT-CHECK .*previous shutdown clean; fsck not needed$' "$LOG" || { cat "$LOG"; fail "clean: fsck should be skipped"; }
[ ! -e "$TMP/L/fat-check.txt.tmp" ] || fail "temporary fsck output left behind"
FAT_FSCK=1; bootcheck "$TMP/sda2"; unset FAT_FSCK   # mounted volumes always carry the flag the kernel set at mount: reported, not cleared
grep -q 'previous shutdown clean; fsck.fat -n: no errors besides the dirty flag (set by this boot.s mount; cleared at shutdown)$' "$LOG" || { cat "$LOG"; fail "FAT_FSCK=1 on a clean boot"; }
[ "$(gbos_fat_byte "$TMP/sda2" 65)" = 1 ] || fail "cleared the flag of a volume that was clean at mount"
cp "$TMP/f32" "$TMP/sdb1"; echo "FAT-fs (sdb1): Volume was not properly unmounted." > "$DM"
mkdir -p "$TMP/g2/installer/bin"; G=$TMP/g2; bootcheck "$TMP/sdb1"; G=$TMP/g
grep -q 'fsck skipped (gbos/installer/bin/fsck.fat missing)$' "$LOG" || fail "missing fsck.fat"
ok "boot check: clears a dirty-only flag, reports problems untouched, FAT_FSCK/FAT_CLEAR_DIRTY, copier resumed"

# --- shutdown watcher: a FIFO as /dev/kmsg, a fake busybox recording mount
cat > "$TMP/fakebb" <<'EOF'
#!/bin/sh
a=$1; shift
case $a in
  mount) echo "mount $*" >> "$FAKE_MOUNTS"; [ ! -e "$FAKE_MOUNT_FAIL" ] || { echo "mount: mounting $5 on $6 failed: Device or resource busy" >&2; exit 1; };;
  sync) ;;
  *) exec ${REAL_BB:+"$REAL_BB"} "$a" "$@";;
esac
EOF
chmod +x "$TMP/fakebb"
export FAKE_MOUNTS="$TMP/mounts" FAKE_MOUNT_FAIL="$TMP/mountfail" REAL_BB="$BB"
waitfor() { _i=0; while ! eval "$1"; do _i=$((_i + 1)); [ $_i -lt 60 ] || return 1; sleep 0.1; done; }
alive() { [ -d "/proc/$1" ] && [ "$(pstate "$1")" != Z ]; }
watch_case() { # FAIL(0|1)
  rm -f "$FAKE_MOUNTS" "$FAKE_MOUNT_FAIL" "$TMP/kmsg" "$TMP/kmsg.out"; [ "$1" = 0 ] || : > "$FAKE_MOUNT_FAIL"
  mkfifo "$TMP/kmsg"; : > "$TMP/L/kmsg.log"
  sleep 300 & SUB=$!; sleep 300 & CAT=$!; PIDS="$PIDS $SUB $CAT"
  rm -rf "$GBOS_PROC"; mkp "$CAT" '/gbos/busybox\000cat\000' busybox "$SUB"
  : > "$LOG"
  GBOS_FAT_BB=$TMP/fakebb; GBOS_KMSG=$TMP/kmsg; GBOS_KMSG_OUT=$TMP/kmsg.out; GBOS_FAT_ERR=$TMP/watch.err; GBOS_FAT_RETRIES=3
  gbos_fat_start_shutdown_watch /dev/sda2 /gbos "$TMP/L/kmsg.log"
  GBOS_FAT_BB=; unset GBOS_KMSG GBOS_KMSG_OUT GBOS_FAT_ERR GBOS_FAT_RETRIES
  grep -q "shutdown watcher pid [0-9]* (kmsg copier: $SUB $CAT)" "$LOG" || { cat "$LOG"; fail "watcher start log"; }
  exec 7>"$TMP/kmsg"
  printf '6,10,1000,-,caller=T1;init: Reboot start, reason: shutdown,userrequested\n14,11,1001,-,caller=T5;gbos-x: not init: sync() before umount\n' >&7
  sleep 0.5; [ ! -s "$FAKE_MOUNTS" ] || fail "remounted before init's unmount step"
  alive "$CAT" || fail "copier stopped too early"
  exec 7>&-; sleep 1.5; exec 7>"$TMP/kmsg"   # cat sees EOF (like kmsg EPIPE): the watcher must read again
  printf '14,12,1002,-,caller=T1;init: sync() before umount...\n' >&7   # and nothing after it: must not wait for another record
  waitfor '[ -s "$FAKE_MOUNTS" ]' || fail "no remount after the marker"
  [ "$(head -1 "$FAKE_MOUNTS")" = "mount -t vfat -o remount,ro /dev/sda2 /gbos" ] || fail "mount args: $(head -1 "$FAKE_MOUNTS")"
  waitfor '! alive "$CAT" && ! alive "$SUB"' || fail "the kmsg copier is still running"
  grep -q 'kmsg copier stopped, remounting /gbos read-only (end of this log)$' "$TMP/L/kmsg.log" || fail "no end-of-log line"
  exec 7>&-
}
watch_case 0
waitfor 'grep -q "^<5>gbos-fat: /gbos is read-only, the FAT is clean$" "$TMP/kmsg.out"' || fail "no success message in kmsg"
[ "$(wc -l < "$FAKE_MOUNTS")" = 1 ] || fail "remounted more than once"
ok "watcher: waits for init's unmount step (not lookalikes), survives EOF, stops the copier, remounts read-only"
watch_case 1
waitfor 'grep -q "^<3>gbos-fat: remounting /gbos read-only FAILED" "$TMP/kmsg.out"' || fail "no failure message in kmsg"
[ "$(wc -l < "$FAKE_MOUNTS")" = 3 ] || fail "retries: $(wc -l < "$FAKE_MOUNTS")"
grep -q '^gbos-fat: remount of /gbos read-only FAILED: mount: mounting /dev/sda2 on /gbos failed: Device or resource busy$' "$TMP/L/kmsg.log" || fail "failure reason not logged"
ok "watcher: EBUSY retried, reason written to kmsg.log"

# --- install.sh's fat_off: release before reboot (stubbed reboot)
sleep 300 & SUB=$!; sleep 300 & CAT=$!; PIDS="$PIDS $SUB $CAT"
rm -rf "$GBOS_PROC"; mkp "$CAT" '/gbos/busybox\000cat\000' busybox "$SUB"
rm -f "$FAKE_MOUNTS" "$FAKE_MOUNT_FAIL"; : > "$LOG"
eval "$(sed -n '/^fat_off() {/p' "$U/install.sh")"
DEV=/dev/sdb1 L=$TMP/L G=$U GBOS_FAT_BB=$TMP/fakebb
myreboot() { echo "reboot $*" >> "$FAKE_MOUNTS"; }
fat_off myreboot
GBOS_FAT_BB=; G=$TMP/g
[ "$(cat "$FAKE_MOUNTS")" = "mount -t vfat -o remount,ro /dev/sdb1 /gbos
reboot -f" ] || fail "fat_off: $(cat "$FAKE_MOUNTS")"
waitfor '! alive "$CAT" && ! alive "$SUB"' || fail "fat_off left the copier running"
grep -q '^gbos-fat: /gbos remounted read-only (clean)$' "$LOG" || fail "fat_off log"
ok "install.sh fat_off: copier stopped, remount read-only, then reboot -f"

# --- the real vfat driver (privileged docker, loop device)
if [ "${FAT_KERNEL_TEST:-0}" = 1 ]; then
  [ -n "$BB" ] || fail "FAT_KERNEL_TEST needs BUSYBOX (a static busybox, e.g. the stick's /busybox)"
  cat > "$TMP/k.sh" <<'EOF'
set -eu
fail() { echo "FAIL (kernel): $*" >&2; exit 1; }
L="/u/installer/lib/ld-musl-x86_64.so.1 --library-path /u/installer/lib"
st() { od -A n -t u1 -j 65 -N 1 "$D" | tr -d ' '; }
cd /tmp; dd if=/dev/zero of=img bs=1M count=64 2>/dev/null; $L /u/installer/bin/mkfs.fat -F 32 img >/dev/null
D=$(losetup -f); losetup "$D" img
mkdir -p /r/gbos /p; mount -t vfat -o rw,flush,umask=000 "$D" /r/gbos; mount --bind /r/gbos /p   # stage0's mount and Android's /postinstall
cp /bb /r/gbos/busybox
[ "$(st)" = 1 ] || fail "rw mount did not set the dirty flag"
( exec 5>>/r/gbos/kmsg.log; echo x >&5; sleep 30 ) & W=$!; sleep 0.3
! chroot /r /gbos/busybox mount -t vfat -o remount,ro /dev/sda2 /gbos 2>/tmp/e || fail "remounted with a writer open"
grep -q 'Device or resource busy' /tmp/e || fail "not EBUSY: $(cat /tmp/e)"
kill $W; wait $W 2>/dev/null || :
chroot /r /gbos/busybox mount -t vfat -o remount,ro /dev/sda2 /gbos || fail "remount from a root without /proc and /dev"
[ "$(st)" = 0 ] || fail "remount ro left the dirty flag"
grep ' /p ' /proc/self/mountinfo | grep -q ' - vfat [^ ]* ro,' || fail "superblock not read-only"
! sh -c 'echo y >> /p/t' 2>/dev/null || fail "/postinstall still writable"
umount /p /r/gbos
printf '\001' | dd of="$D" bs=1 seek=65 conv=notrunc 2>/dev/null; mount -t vfat -o rw,flush "$D" /r/gbos
mount -o remount,ro /r/gbos; [ "$(st)" = 1 ] || fail "expected: a volume dirty at mount stays dirty"
mount -o remount,rw /r/gbos; echo z > /r/gbos/z; sync
. /s/stick_fat.sh; GBOS_FAT_BB=
gbos_fat_clear_dirty "$D" || fail "clear while mounted"
mount -o remount,ro /r/gbos; [ "$(st)" = 0 ] || fail "cleared flag came back"
umount /r/gbos; $L /u/installer/bin/fsck.fat -n "$D" >/tmp/o 2>&1 || { cat /tmp/o; fail "fsck after clearing while mounted"; }
mount -t vfat -o rw "$D" /r/gbos; [ "$(cat /r/gbos/z)" = z ] || fail "data lost"; mount -o remount,ro /r/gbos; [ "$(st)" = 0 ] || fail "next boot not clean"
umount /r/gbos; losetup -d "$D"
echo KERNEL-OK
EOF
  docker run --rm --privileged -v "$U":/u:ro -v "$BB":/bb:ro -v "$TMP/k.sh":/k.sh:ro -v "$U/stick_fat.sh":/s/stick_fat.sh:ro alpine:latest sh /k.sh | grep -q '^KERNEL-OK$' \
    || fail "kernel test (run it by hand for details)"
  ok "kernel: superblock remount ro from a bare root, EBUSY with a writer, dirty flag cleared, clearing while mounted is safe"
fi
echo "PASS test_stick_fat"
