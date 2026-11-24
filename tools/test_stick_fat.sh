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
export FAT_SYNC_S=0   # no periodic syncer except in its own cases below

# --- syntax
for f in stick_fat.sh hook.sh install.sh stage0_init.sh apex_patch.sh; do sh -n "$U/$f" || fail "sh -n usb/$f"; done
[ -z "$BB" ] || for f in stick_fat.sh hook.sh install.sh; do "$BB" sh -n "$U/$f" || fail "busybox sh -n usb/$f"; done
ok "syntax"

# --- wiring
grep -q '^if \[ -f \$G/stick_fat.sh \] && \. \$G/stick_fat.sh; then$' "$U/hook.sh" || fail "hook.sh does not source stick_fat.sh"
grep -q '^  gbos_fat_boot_check "\$DEV" /gbos "\$L"$' "$U/hook.sh" || fail "hook.sh: no boot check"
grep -q '^  gbos_fat_start_shutdown_watch "\$DEV" /gbos "\$L"$' "$U/hook.sh" || fail "hook.sh: no shutdown watcher"
# the fake boot disk must not be a read-write loop on the volume (its open backing file made every read-only remount EBUSY)
grep -q '^  \[ "\${BOOTDISK_RAM:-1}" != 0 \] && mkdir -p /tmp && cp \$G/bootdisk.img /tmp/bootdisk.img 2>/dev/null && _bdi=/tmp/bootdisk.img$' "$U/hook.sh" \
  && grep -q '^  losetup -P /dev/loop6 \$_bdi; ' "$U/hook.sh" && ! grep -q 'losetup -P /dev/loop6 \$G/' "$U/hook.sh" || fail "hook.sh: bootdisk not looped from RAM"
grep -q 'gbos_fat_release "\$DEV" /gbos "\$L"; "\$1" -f; }$' "$U/install.sh" || fail "install.sh: fat_off does not pass the log dir"
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

# --- the log-file repair's parser, on fsck.fat 4.2's real output from the persistent stick (boot-8, 2026-11-23)
fk() { printf '%s\n' 'fsck.fat 4.2 (2021-01-31)' 'There are differences between boot sector and its backup.' \
  'This is mostly harmless. Differences: (offset:original/backup)' '  65:01/00' '  Not automatically fixing this.' "$@" \
  'Dirty bit is set. Fs was not properly unmounted and some data may be corrupt.' ' Automatically removing dirty bit.' '' \
  'Leaving filesystem unchanged.' '/dev/sda2: 7506 files, 1131544/2093057 clusters' > "$TMP/fk"; }
fk '/GBOS/LOGS/boot-7/kmsg.log' '  File size is 1298432 bytes, cluster chain length is 1261568 bytes.' '  Truncating file to 1261568 bytes.' \
   '/GBOS/LOGS/boot-5/logcat.txt' '  File size is 73728 bytes, cluster chain length is 65536 bytes.' '  Truncating file to 65536 bytes.'
[ "$(gbos_fsck_log_truncations "$TMP/fk" 65)" = "$(printf '1261568 /GBOS/LOGS/boot-7/kmsg.log\n65536 /GBOS/LOGS/boot-5/logcat.txt')" ] \
  || { gbos_fsck_log_truncations "$TMP/fk" 65; fail "log truncations: the stick's boot-8 output not recognised"; }
fk '/GBOS/HOOK.SH' '  File size is 9000 bytes, cluster chain length is 4096 bytes.' '  Truncating file to 4096 bytes.'
! gbos_fsck_log_truncations "$TMP/fk" 65 >/dev/null || fail "log truncations: a damaged file outside logs/ must not be repaired"
fk '/GBOS/LOGS/boot-7/kmsg.log' '  File size is 1298432 bytes, cluster chain length is 1261568 bytes.' '  Truncating file to 1261568 bytes.' \
   '/GBOS/LOGS/boot-7' '  Directory has no . entry.'
! gbos_fsck_log_truncations "$TMP/fk" 65 >/dev/null || fail "log truncations: any other problem means no repair"
fk '/GBOS/LOGS/boot-7/kmsg.log' '  File size is 4096 bytes, cluster chain length is 8192 bytes.' '  Truncating file to 8192 bytes.'
! gbos_fsck_log_truncations "$TMP/fk" 65 >/dev/null || fail "log truncations: a chain longer than the size is not this damage"
fk '/GBOS/LOGS/../HOOK.SH' '  File size is 9000 bytes, cluster chain length is 4096 bytes.' '  Truncating file to 4096 bytes.'
! gbos_fsck_log_truncations "$TMP/fk" 65 >/dev/null || fail "log truncations: a path with .. is never repaired"
fk
! gbos_fsck_log_truncations "$TMP/fk" 65 >/dev/null || fail "log truncations: nothing to repair returns 1 (the dirty-only path handles it)"
ok "log truncations: only log files under gbos/logs/boot-N whose size exceeds the chain, and nothing else wrong"
# a lost cluster: cluster 5 marked end-of-chain in the first FAT only (the FATs differ: a real problem), or in both (only a lost cluster)
lose_cluster() { # FILE 32|16 [both]
  rs=$(od -A n -t u2 -j 14 -N 2 "$1" | tr -d ' ')
  if [ "$2" = 32 ]; then fs=$(od -A n -t u4 -j 36 -N 4 "$1" | tr -d ' '); else fs=$(od -A n -t u2 -j 22 -N 2 "$1" | tr -d ' '); fi
  for c in 0 ${3:+1}; do
    if [ "$2" = 32 ]; then printf '\377\377\377\017' | dd of="$1" bs=1 seek=$(((rs + c * fs) * 512 + 20)) conv=notrunc 2>/dev/null
    else printf '\377\377' | dd of="$1" bs=1 seek=$(((rs + c * fs) * 512 + 10)) conv=notrunc 2>/dev/null; fi
  done
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
  ! gbos_fsck_only_dirty "$TMP/out" "$o" || { cat "$TMP/out"; fail "FAT$t lost cluster in one FAT copy taken as dirty-only"; }
  cp "$TMP/d$t" "$TMP/b$t"; lose_cluster "$TMP/b$t" "$t" both
  fsck_n "$TMP/b$t" "$TMP/out" || :; grep -q '^Reclaimed 1 unused cluster' "$TMP/out" || { cat "$TMP/out"; fail "FAT$t lost-cluster fixture"; }
  gbos_fsck_only_dirty "$TMP/out" "$o" || { cat "$TMP/out"; fail "FAT$t a lost cluster (both FATs agree) is harmless"; }
done
cp "$TMP/d32" "$TMP/fi32"; printf '\012\000\000\000' | dd of="$TMP/fi32" bs=1 seek=1000 conv=notrunc 2>/dev/null   # FSInfo free count
fsck_n "$TMP/fi32" "$TMP/out" || :; grep -q 'Free cluster summary wrong' "$TMP/out" || fail "FSInfo fixture"
gbos_fsck_only_dirty "$TMP/out" 65 || fail "a stale FSInfo free count is only a hint"
printf 'fsck.fat 4.2 (2021-01-31)\nDirty bit is set. Fs was not properly unmounted and some data may be corrupt.\n Automatically removing dirty bit.\n' > "$TMP/out"
! gbos_fsck_only_dirty "$TMP/out" 65 || fail "an fsck cut short (no summary line) taken as dirty-only"
printf 'fsck.fat 4.2 (2021-01-31)\nThere are differences between boot sector and its backup.\nThis is mostly harmless. Differences: (offset:original/backup)\n  65:01/00\n  71:47/48\n  Not automatically fixing this.\nLeaving filesystem unchanged.\n/dev/sda2: 1 files, 1/9 clusters\n' > "$TMP/out"
! gbos_fsck_only_dirty "$TMP/out" 65 || fail "a backup boot sector differing elsewhere taken as dirty-only"
ok "fsck.fat -n: dirty-only (FAT32/16, FSInfo hint, lost clusters) vs FATs differing / truncated run / other boot-sector change"

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
DM=$TMP/dmesg; GBOS_DMESG="cat $DM"; GBOS_FAT_TMP=$TMP/ram; mkdir -p "$GBOS_FAT_TMP"
LD=$TMP/logs/boot-7
bootcheck() { # IMAGE: runs the check with a fresh log dir, the image as the device
  rm -rf "$LD"; mkdir -p "$LD"; : > "$LOG"
  gbos_fat_boot_check "$1" "$TMP" "$LD"
  [ "$(pstate "$CAT")" = S ] || fail "the copier was left stopped"
  [ -z "$(ls "$GBOS_FAT_TMP")" ] || fail "RAM files left behind: $(ls "$GBOS_FAT_TMP")"
}
cp "$TMP/d32" "$TMP/sda2"; echo "[   25.7][ T226] FAT-fs (sda2): Volume was not properly unmounted. Some data may be corrupt. Please run fsck." > "$DM"
bootcheck "$TMP/sda2"
grep -q 'FAT-CHECK .*/sda2: previous shutdown NOT clean; fsck.fat -n: no errors besides the dirty flag; the flag is cleared at shutdown, after the read-only remount$' "$LOG" \
  || { cat "$LOG"; fail "dirty-only: not scheduled for clearing"; }
[ "$GBOS_FAT_CLEAR" = 1 ] || fail "GBOS_FAT_CLEAR not set for the watcher"
cmp -s "$TMP/sda2" "$TMP/d32" || fail "the boot check wrote to the volume (the flag is cleared at shutdown, not while mounted rw)"
grep -q '^--- fsck.fat -n .*/sda2 (rc=1, uptime' "$LD/fat-check.txt" && grep -q '^Dirty bit is set' "$LD/fat-check.txt" || fail "fat-check.txt lacks the fsck output"
[ ! -e "$LD/fat-check.txt.tmp" ] || fail "fsck output written to the volume"
cp "$TMP/b32" "$TMP/sda2"; bootcheck "$TMP/sda2"
grep -q 'no errors besides the dirty flag and 1 lost cluster(s) (left allocated: harmless; chkdsk or fsck.fat -a frees them); the flag is cleared at shutdown' "$LOG" \
  || { cat "$LOG"; fail "lost cluster: not harmless"; }
cp "$TMP/l32" "$TMP/sda2"; bootcheck "$TMP/sda2"
grep -q 'FAT-CHECK .*previous shutdown NOT clean; fsck.fat -n rc=1: PROBLEMS FOUND, nothing changed' "$LOG" || { cat "$LOG"; fail "problems: not reported"; }
[ "$GBOS_FAT_CLEAR" = 0 ] || fail "a volume with problems is scheduled for clearing"
cmp -s "$TMP/sda2" "$TMP/l32" || fail "a volume with problems was modified"
cp "$TMP/d32" "$TMP/sda2"; FAT_CLEAR_DIRTY=0; bootcheck "$TMP/sda2"; unset FAT_CLEAR_DIRTY
grep -q 'no errors besides the dirty flag; left set (FAT_CLEAR_DIRTY=0)$' "$LOG" && [ "$GBOS_FAT_CLEAR" = 0 ] || fail "FAT_CLEAR_DIRTY=0"
FAT_FSCK=0; bootcheck "$TMP/sda2"; unset FAT_FSCK; grep -q 'previous shutdown NOT clean; fsck skipped (FAT_FSCK=0)$' "$LOG" || fail "FAT_FSCK=0"
: > "$DM"   # the kernel said nothing at the rw mount: clean
bootcheck "$TMP/sda2"; grep -q 'FAT-CHECK .*previous shutdown clean; fsck not needed$' "$LOG" || { cat "$LOG"; fail "clean: fsck should be skipped"; }
FAT_FSCK=1; bootcheck "$TMP/sda2"; unset FAT_FSCK   # mounted volumes always carry the flag the kernel set at mount: reported, not cleared
grep -q "previous shutdown clean; fsck.fat -n: no errors besides the dirty flag (set by this boot's mount; the read-only remount at shutdown clears it)$" "$LOG" \
  || { cat "$LOG"; fail "FAT_FSCK=1 on a clean boot"; }
[ "$GBOS_FAT_CLEAR" = 0 ] || fail "scheduled clearing the flag of a volume that was clean at mount (the kernel does it)"
cp "$TMP/f32" "$TMP/sdb1"; echo "FAT-fs (sdb1): Volume was not properly unmounted." > "$DM"
mkdir -p "$TMP/g2/installer/bin"; G=$TMP/g2; bootcheck "$TMP/sdb1"; G=$TMP/g
grep -q 'fsck skipped (gbos/installer/bin/fsck.fat missing)$' "$LOG" || fail "missing fsck.fat"
ok "boot check: fsck output in RAM, dirty-only and lost clusters scheduled for clearing at shutdown, problems untouched, FAT_FSCK/FAT_CLEAR_DIRTY"

# --- the previous boot's shutdown, in fat-check.txt
P6=$TMP/logs/boot-6; prevcase() { # WAS_DIRTY EXPECTED_CONCLUSION
  rm -rf "$LD"; mkdir -p "$LD"; gbos_fat_prev_shutdown "$LD" "$1" > "$TMP/prev"
  grep -q "^=> $2" "$TMP/prev" || { cat "$TMP/prev"; fail "previous shutdown: want '$2'"; }
}
rm -rf "$P6"; mkdir -p "$P6"
prevcase 1 'its shutdown step never started'
grep -q 'previous boot (boot-6): no fat-shutdown.txt' "$TMP/prev" || fail "no fat-shutdown.txt not reported"
echo 'armed at boot: remount /gbos (/dev/sda2) read-only at init'"'"'s unmount step; clear the dirty flag then: yes (kmsg copier: 1 2)' > "$P6/fat-shutdown.txt"
printf '14,1,1,-,caller=T1;init: sync() before umount...\n5,2,2,-,caller=T9;gbos-fat: shutdown: init reached its unmount step at 313.731 s; stopping\0\0\0' > "$P6/kmsg.log"
prevcase 0 'it left the volume read-only and clean'
grep -q '^  armed at boot: ' "$TMP/prev" && grep -q '^  end of its kmsg.log: gbos-fat: shutdown: init reached its unmount step at 313.731 s; stopping$' "$TMP/prev" \
  || { cat "$TMP/prev"; fail "previous shutdown details"; }
prevcase 1 'its shutdown step started at 313.731 s but did not finish'
echo 'remount of /gbos read-only FAILED after 30 tries: mount: ... busy' >> "$P6/fat-shutdown.txt"; prevcase 1 'its shutdown step failed'
echo 'armed at boot: ...; clear the dirty flag then: no (kmsg copier: 1 2)' > "$P6/fat-shutdown.txt"; echo '/dev/sda2 at /gbos, FAT32: previous shutdown NOT clean' > "$P6/fat-check.txt"
prevcase 1 'that boot already mounted it dirty'
ok "fat-check.txt: the previous boot's fat-shutdown.txt and kmsg.log end, and how its shutdown ended"

# --- boot-4 of the persistent stick (2026-11-22): the volume mounted dirty, a FAT error at 25.766 s made it read-only, so neither
# fat-check.txt nor fat-shutdown.txt exists; the watcher's line is followed by 72 apexd records (13 KB, more than the old 8 KB tail).
boot4_kmsg() { # CLEAR_WORDS (the watcher's words about the flag)
  { printf '4,886,9617835,-,caller=T196;FAT-fs (sda2): Volume was not properly unmounted. Some data may be corrupt. Please run fsck.\n'
    printf ' SUBSYSTEM=block\n'
    printf '3,1072,25765873,-,caller=T1860;FAT-fs (sda2): error, fat_bmap_cluster: request beyond EOF (i_pos 143899267)\n'
    printf '3,1073,25765878,-,caller=T1860;FAT-fs (sda2): Filesystem has been set read-only\n'
    printf '3,1074,25765881,-,caller=T1860;FAT-fs (sda2): error, fat_bmap_cluster: request beyond EOF (i_pos 143899267)\n'
    i=0; while [ $i -lt 400 ]; do printf '14,%d,%d,-,caller=T1;init: service %d stopped\n' $((2000 + i)) $((30000000 + i * 1000000)) $i; i=$((i + 1)); done
    printf '14,15354,446612553,-,caller=T1;init: sync() before umount...\n'
    printf '13,15355,446613938,-,caller=T1907;gbos-fat: shutdown: init reached its unmount step at 446.613 s; stopping the kmsg copier, remounting /gbos read-only%s (end of this log)\n' "$1"
    i=0; while [ $i -lt 72 ]; do
      printf '14,%d,%d,-,caller=T11462;apexd-unmount-all: Unmounting /system/apex/com.google.android.example%02d.apex mounted on /apex/com.android.example%02d@371899999\n' \
        $((15356 + i)) $((446614000 + i * 700)) $i $i; i=$((i + 1)); done
    printf '6,15428,446664598,-,caller=T11462;EXT4-fs (loop44): unmounting filesystem.\n'; } > "$P6/kmsg.log"
}
rm -rf "$P6"; mkdir -p "$P6"; boot4_kmsg ''
! tail -c 8192 "$P6/kmsg.log" | grep -q 'gbos-fat: shutdown:' || fail "fixture: the watcher line must be beyond the last 8 KB"
prevcase 1 'that boot already mounted it dirty and was not allowed to clear the flag .*: it stays dirty whatever the remount did (its shutdown step started at 446.613 s)$'
grep -q '^previous boot (boot-6): no fat-shutdown.txt: it could not be created, the volume was read-only from 25.766 s (below)$' "$TMP/prev" \
  && grep -q '^  end of its kmsg.log: gbos-fat: shutdown: init reached its unmount step at 446.613 s; stopping the kmsg copier, remounting /gbos read-only (end of this log)$' "$TMP/prev" \
  && grep -q '^  (73 more kernel records were copied after it, the last at 446.665 s: ' "$TMP/prev" \
  && grep -q '^  its kmsg.log: the kernel found the volume dirty when that boot mounted it (9.618 s)$' "$TMP/prev" \
  && grep -q '^  its kmsg.log: a FAT error made the volume READ-ONLY at 25.766 s: FAT-fs (sda2): error, fat_bmap_cluster: request beyond EOF (i_pos 143899267)$' "$TMP/prev" \
  || { cat "$TMP/prev"; fail "boot-4 replica: details"; }
! grep -q 'never started' "$TMP/prev" || fail "boot-4 replica: the step did start"
boot4_kmsg ' and clearing the dirty flag'; prevcase 1 'its shutdown step started at 446.613 s but did not finish'
grep -q 'READ-ONLY at 25.766 s' "$TMP/prev" || fail "boot-4 replica (clear): the read-only event"
boot4_kmsg ', leaving the dirty flag as it is; /gbos is ALREADY read-only (a FAT error earlier this boot?)'
prevcase 1 'that boot already mounted it dirty and was not allowed to clear the flag'
prevcase 0 'it left the volume read-only and clean'
! grep -q 'READ-ONLY' "$TMP/prev" || fail "a clean volume: the whole previous kmsg.log is only scanned when this mount found it dirty"
# negative controls: no watcher line (forced power-off after a FAT error), and a log with no FAT error at all
grep -v 'gbos-fat: shutdown:' "$P6/kmsg.log" > "$P6/k" && mv "$P6/k" "$P6/kmsg.log"; prevcase 1 'its shutdown step never started'
grep -q 'READ-ONLY at 25.766 s' "$TMP/prev" || fail "no watcher line: the read-only event still reported"
boot4_kmsg ' and clearing the dirty flag'; grep -v 'FAT-fs' "$P6/kmsg.log" > "$P6/k" && mv "$P6/k" "$P6/kmsg.log"
echo 'armed at boot: ...; clear the dirty flag then: yes (kmsg copier: 1 2)' > "$P6/fat-shutdown.txt"
prevcase 1 'its shutdown step started at 446.613 s but did not finish'; ! grep -q 'READ-ONLY\|found the volume dirty' "$TMP/prev" || fail "FAT events invented"
# the boot check reads the previous boot's files only once the volume is known intact
rm -rf "$P6"; mkdir -p "$P6"; boot4_kmsg ''
echo "[    9.6][ T196] FAT-fs (sda2): Volume was not properly unmounted. Some data may be corrupt. Please run fsck." > "$DM"
cp "$TMP/l32" "$TMP/sda2"; bootcheck "$TMP/sda2"
grep -q '^previous boot (boot-6): its files were not read: fsck found problems; opening a damaged file would make the kernel set the whole volume read-only$' "$LD/fat-check.txt" \
  && grep -q '^=> unknown: ' "$LD/fat-check.txt" && ! grep -q 'end of its kmsg.log\|READ-ONLY' "$LD/fat-check.txt" || { cat "$LD/fat-check.txt"; fail "problems: previous boot's files read"; }
grep -q '^--- fsck.fat -n ' "$LD/fat-check.txt" || fail "problems: fsck output missing"
FAT_FSCK=0; bootcheck "$TMP/sda2"; unset FAT_FSCK
grep -q '^previous boot (boot-6): its files were not read: the volume was not cleanly unmounted and fsck did not run (fsck skipped (FAT_FSCK=0))' "$LD/fat-check.txt" \
  || { cat "$LD/fat-check.txt"; fail "FAT_FSCK=0 on a dirty volume: previous boot's files read"; }
cp "$TMP/d32" "$TMP/sda2"; bootcheck "$TMP/sda2"
grep -q '^  end of its kmsg.log: gbos-fat: shutdown: init reached its unmount step at 446.613 s' "$LD/fat-check.txt" && grep -q 'READ-ONLY at 25.766 s' "$LD/fat-check.txt" \
  || { cat "$LD/fat-check.txt"; fail "dirty-only: the previous boot's files must be read"; }
: > "$DM"; bootcheck "$TMP/sda2"; grep -q '^  end of its kmsg.log: ' "$LD/fat-check.txt" || { cat "$LD/fat-check.txt"; fail "clean: previous files must be read"; }
printf 'fsck.fat 4.2 (2021-01-31)\n/gbos/logs/boot-3/kmsg.log\n  File size is 120871 bytes, cluster chain length is 55808 bytes.\n  Truncating file to 55808 bytes.\n/GBOS/LOGS/BOOT-3/HW.TXT\n  Contains a free cluster (5). Assuming EOF.\nLeaving filesystem unchanged.\n/dev/sda2: 4 files, 113/129022 clusters\n' > "$TMP/out"
[ "$(gbos_fsck_named_paths "$TMP/out" | tr '\n' ' ')" = "/gbos/logs/boot-3/kmsg.log /gbos/logs/boot-3/hw.txt " ] || fail "fsck-named paths: $(gbos_fsck_named_paths "$TMP/out")"
ok "boot-4 replica: the watcher line beyond 8 KB, the FAT error that made the volume read-only, no 'never started'; previous files read only when intact"

# --- the volume made read-only by a FAT error during the boot: the report goes to kmsg (stage0's log), not to an uncreatable file
mkdir -p "$GBOS_PROC/self"; echo "36 25 8:2 / $TMP rw,relatime shared:5 - vfat /dev/sda2 ro,flush,umask=000,errors=remount-ro" > "$GBOS_PROC/self/mountinfo"
printf '[    9.6][ T196] FAT-fs (sda2): Volume was not properly unmounted. Some data may be corrupt. Please run fsck.\n[   25.7][T1860] FAT-fs (sda2): error, fat_bmap_cluster: request beyond EOF (i_pos 143899267)\n[   25.7][T1860] FAT-fs (sda2): Filesystem has been set read-only\n' > "$DM"
rm -rf "$LD"; mkdir -p "$LD"; chmod 555 "$LD"; : > "$LOG"
gbos_fat_boot_check "$TMP/sda2" "$TMP" "$LD"
grep -q 'FAT-CHECK .*/sda2: previous shutdown NOT clean; fsck.fat -n: .*; '"$TMP"' is READ-ONLY now (FAT error: FAT-fs (sda2): error, fat_bmap_cluster: request beyond EOF (i_pos 143899267))$' "$LOG" \
  && grep -q '^hook: fat-check.txt (not writable): .*/sda2 at .*, FAT32: previous shutdown NOT clean$' "$LOG" \
  && grep -q '^hook: fat-check.txt (not writable): Dirty bit is set' "$LOG" || { cat "$LOG"; fail "read-only volume: report not in kmsg"; }
# arming: fat-shutdown.txt cannot be created either; the armed line goes to kmsg
: > "$TMP/kmsg.empty"; : > "$LOG"
GBOS_FAT_RT=$TMP/rt0 GBOS_FAT_RT_MOUNT=0 GBOS_KMSG=$TMP/kmsg.empty GBOS_KMSG_OUT=/dev/null GBOS_SYS=$TMP/nosys
gbos_fat_start_shutdown_watch /dev/sda2 "$TMP" "$LD"
GBOS_FAT_RT=/nonexistent; GBOS_SYS=/nonexistent; unset GBOS_KMSG GBOS_KMSG_OUT
WP=$(sed -n 's/.*shutdown watcher pid \([0-9]*\) .*/\1/p' "$LOG"); [ -n "$WP" ] && PIDS="$PIDS $WP"
grep -q "^hook: fat: fat-shutdown.txt NOT writable (FAT-fs (sda2): error, fat_bmap_cluster: request beyond EOF (i_pos 143899267)); it would say: armed at boot: remount $TMP (/dev/sda2) read-only" "$LOG" \
  || { cat "$LOG"; fail "arming on a read-only volume: not reported"; }
[ -n "$WP" ] && kill -KILL "$WP" 2>/dev/null; chmod 755 "$LD"
# negative control: the same volume read-write: no READ-ONLY claim, the report is a file
echo "36 25 8:2 / $TMP rw,relatime shared:5 - vfat /dev/sda2 rw,flush,umask=000,errors=remount-ro" > "$GBOS_PROC/self/mountinfo"
bootcheck "$TMP/sda2"; ! grep -q 'READ-ONLY now\|not writable' "$LOG" "$LD/fat-check.txt" && [ -s "$LD/fat-check.txt" ] || { cat "$LOG"; fail "read-write volume reported read-only"; }
rm -rf "$GBOS_PROC/self"; : > "$DM"
ok "a FAT error made the volume read-only this boot: FAT-CHECK says so, fat-check.txt and the armed line go to kmsg; read-write volume: not claimed"

# --- shutdown watcher: a FIFO as /dev/kmsg, a fake busybox (mount, losetup, mknod), fake proc/sys in the watcher's runtime dir
cat > "$TMP/fakebb" <<'EOF'
#!/bin/sh
a=$1; shift
case $a in
  mount) echo "mount $*" >> "$FAKE_MOUNTS"
         case $4 in remount,ro) [ ! -e "$FAKE_MOUNT_FAIL" ] || { echo "mount: mounting $5 on $6 failed: Device or resource busy" >&2; exit 1; }
                                sed -i 's/ - vfat \([^ ]*\) rw,/ - vfat \1 ro,/' "$FAKE_MI";;
                    remount,rw) sed -i 's/ - vfat \([^ ]*\) ro,/ - vfat \1 rw,/' "$FAKE_MI";; esac;;
  losetup) echo "losetup $*" >> "$FAKE_MOUNTS"; rm -rf "$FAKE_SYS/block/${2##*/}/loop";;
  mknod) ;;
  sync) echo "sync $*" >> "${FAKE_SYNCS:-/dev/null}";;
  dmesg) cat "${FAKE_DMESG:-/dev/null}";;
  *) exec ${REAL_BB:+"$REAL_BB"} "$a" "$@";;
esac
EOF
chmod +x "$TMP/fakebb"
RT=$TMP/rt; MP=$TMP/vol; mkdir -p "$MP/gbos/logs"
export FAKE_MOUNTS="$TMP/mounts" FAKE_MOUNT_FAIL="$TMP/mountfail" REAL_BB="$BB" FAKE_MI="$RT/proc/self/mountinfo" FAKE_SYS="$RT/sys"
waitfor() { _i=0; while ! eval "$1"; do _i=$((_i + 1)); [ $_i -lt 80 ] || return 1; sleep 0.1; done; }
alive() { [ -d "/proc/$1" ] && [ "$(pstate "$1")" != Z ]; }
mkrt() { # the runtime dir as gbos_fat_rt_setup leaves it; loop6 a read-write loop on the volume, pid 4242 a writer
  rm -rf "$RT"; mkdir -p "$RT/proc/self" "$RT/sys/block/loop6/loop" "$RT/sys/block/loop0/loop" "$RT/proc/4242/fd" "$RT/proc/4242/fdinfo"
  echo "36 25 8:2 / $MP rw,relatime shared:5 - vfat /dev/sda2 rw,flush,umask=000" > "$FAKE_MI"
  echo 0 > "$RT/sys/block/loop6/ro"; echo 7:6 > "$RT/sys/block/loop6/dev"; echo "$MP/gbos/bootdisk.img" > "$RT/sys/block/loop6/loop/backing_file"
  echo 1 > "$RT/sys/block/loop0/ro"; echo 7:0 > "$RT/sys/block/loop0/dev"; echo "$MP/gbos/super.img" > "$RT/sys/block/loop0/loop/backing_file"
  : > "$MP/gbos/logs/logcat.txt"; : > "$MP/gbos/super.img"; echo logcat > "$RT/proc/4242/comm"
  ln -s "$MP/gbos/logs/logcat.txt" "$RT/proc/4242/fd/5"; printf 'pos:\t0\nflags:\t0102001\n' > "$RT/proc/4242/fdinfo/5"
  ln -s "$MP/gbos/super.img" "$RT/proc/4242/fd/6"; printf 'pos:\t0\nflags:\t0100000\n' > "$RT/proc/4242/fdinfo/6"
  : > "$RT/proc/sysrq-trigger"; ln -s /dev/null "$RT/null"; cp "$TMP/d32" "$TMP/blk"; ln -s "$TMP/blk" "$RT/blk"
}
watch_case() { # FAIL(0|1) CLEAR(0|1) [ro: a FAT error made the volume read-only before the shutdown]
  rm -f "$FAKE_MOUNTS" "$FAKE_MOUNT_FAIL" "$TMP/kmsg" "$TMP/kmsg.out"; [ "$1" = 0 ] || : > "$FAKE_MOUNT_FAIL"
  mkfifo "$TMP/kmsg"; rm -rf "$LD"; mkdir -p "$LD"; mkrt
  sleep 300 & SUB=$!; sleep 300 & CAT=$!; PIDS="$PIDS $SUB $CAT"
  rm -rf "$GBOS_PROC"; mkp "$CAT" '/gbos/busybox\000cat\000' busybox "$SUB"
  : > "$LOG"
  GBOS_FAT_BB=$TMP/fakebb GBOS_KMSG=$TMP/kmsg GBOS_KMSG_OUT=$TMP/kmsg.out GBOS_FAT_ERR=$TMP/watch.err GBOS_FAT_RETRIES=3 GBOS_FAT_RT=$RT \
    GBOS_FAT_RT_MOUNT=0 GBOS_FAT_CLEAR=$2 GBOS_SYS=$RT/sys
  gbos_fat_start_shutdown_watch /dev/sda2 "$MP" "$LD"
  GBOS_FAT_BB=; GBOS_FAT_RT=/nonexistent; GBOS_SYS=/nonexistent; unset GBOS_KMSG GBOS_KMSG_OUT GBOS_FAT_ERR GBOS_FAT_RETRIES GBOS_FAT_CLEAR
  WP=$(sed -n 's/.*shutdown watcher pid \([0-9]*\) (kmsg copier: '"$SUB $CAT"'; clear the flag at shutdown: '"$2"'); .*/\1/p' "$LOG")
  [ -n "$WP" ] || { cat "$LOG"; fail "watcher start log"; }
  grep -q "^armed at boot: remount $MP (/dev/sda2) read-only at init's unmount step; clear the dirty flag then: $([ "$2" = 1 ] && echo yes || echo no) (kmsg copier: $SUB $CAT; read-write loops on it now: loop6 $MP/gbos/bootdisk.img )$" "$LD/fat-shutdown.txt" \
    || { cat "$LD/fat-shutdown.txt"; fail "fat-shutdown.txt not armed"; }
  kill -TERM "$WP"; sleep 0.3; alive "$WP" || fail "the watcher died of SIGTERM (init's sysrq 'terminate all tasks')"
  [ "${3:-}" != ro ] || sed -i 's/ - vfat \([^ ]*\) rw,/ - vfat \1 ro,/' "$FAKE_MI"
  exec 7>"$TMP/kmsg"
  printf '6,10,1000,-,caller=T1;init: Reboot start, reason: shutdown,userrequested\n14,11,1001,-,caller=T5;gbos-x: not init: sync() before umount\n' >&7
  sleep 0.5; [ ! -s "$FAKE_MOUNTS" ] || fail "remounted before init's unmount step"
  alive "$CAT" || fail "copier stopped too early"
  exec 7>&-; sleep 1.5; exec 7>"$TMP/kmsg"   # cat sees EOF (like kmsg EPIPE): the watcher must read again
  printf '14,12,313731191,-,caller=T1;init: sync() before umount...\n' >&7   # and nothing after it: must not wait for another record
  waitfor '[ -s "$FAKE_MOUNTS" ] && grep -q "^<[35]>gbos-fat: .*\(is read-only (\|FAILED\)" "$TMP/kmsg.out"' || { cat "$TMP/kmsg.out" "$TMP/watch.err"; fail "no remount after the marker"; }
  grep -q "^<5>gbos-fat: shutdown: init reached its unmount step at 313.731 s; stopping the kmsg copier, remounting $MP read-only.* (end of this log)$" "$TMP/kmsg.out" \
    || { cat "$TMP/kmsg.out"; fail "no end-of-log line for the copier"; }
  [ "$(head -2 "$FAKE_MOUNTS")" = "losetup -d $RT/loop6
mount -t vfat -o remount,ro /dev/sda2 $MP" ] || fail "loop detach, then remount: $(cat "$FAKE_MOUNTS")"
  waitfor '! alive "$CAT" && ! alive "$SUB"' || fail "the kmsg copier is still running"
  exec 7>&-
}
watch_case 0 1
[ "$(wc -l < "$FAKE_MOUNTS")" = 2 ] || fail "remounted more than once: $(cat "$FAKE_MOUNTS")"
grep -q "^<5>gbos-fat: $MP is read-only (remount, dirty flag cleared, mountinfo: ro)$" "$TMP/kmsg.out" || { cat "$TMP/kmsg.out"; fail "no success message"; }
[ "$(gbos_fat_byte "$TMP/blk" 65)" = 0 ] || fail "dirty flag not cleared after the remount"
# besides the armed line only the shutdown step's status (uptime, kmsg.log, copier, superblock, what holds the volume): no FAILED line
grep -q "^shutdown step at 313.731 s: uptime ? s: kmsg.log ? bytes, kmsg copier cat alive, superblock rw$" "$LD/fat-shutdown.txt" \
  || { cat "$LD/fat-shutdown.txt" "$TMP/watch.err"; fail "no shutdown status line"; }
grep -q "^  open for writing: pid 4242 (logcat) fd 5 -> $MP/gbos/logs/logcat.txt$" "$LD/fat-shutdown.txt" || { cat "$LD/fat-shutdown.txt"; fail "writer not in the status"; }
[ "$(grep -v -e '^armed at boot: ' -e '^shutdown step at ' -e '^  open for writing: ' -e '^  read-write loop device: ' "$LD/fat-shutdown.txt")" = "" ] \
  || fail "success wrote more to the volume: $(cat "$LD/fat-shutdown.txt")"
ok "watcher: ignores SIGTERM, waits for init's unmount step (not lookalikes), survives EOF, stops the copier, detaches rw loops, remounts, clears the flag"
grep -q "^<5>gbos-fat: shutdown: .*, remounting $MP read-only and clearing the dirty flag (end of this log)$" "$TMP/kmsg.out" || { cat "$TMP/kmsg.out"; fail "start line: clear"; }
watch_case 0 0
[ "$(gbos_fat_byte "$TMP/blk" 65)" = 1 ] || fail "cleared the flag although the boot check did not ask (the kernel cleans a volume that was clean at mount)"
grep -q "^<5>gbos-fat: shutdown: .*, remounting $MP read-only, leaving the dirty flag as it is (end of this log)$" "$TMP/kmsg.out" || { cat "$TMP/kmsg.out"; fail "start line: no clear"; }
ok "watcher: leaves the flag to the kernel unless the boot check asked"
watch_case 0 0 ro
grep -q "^<5>gbos-fat: shutdown: .*, leaving the dirty flag as it is; $MP is ALREADY read-only (a FAT error earlier this boot?) (end of this log)$" "$TMP/kmsg.out" \
  || { cat "$TMP/kmsg.out"; fail "start line: already read-only not said"; }
cp "$TMP/kmsg.out" "$TMP/kmsg.ro"; mkdir -p "$TMP/logs/boot-8"; { printf '13,1,446613938,-,caller=T1907;'; sed -n 's/^<5>\(gbos-fat: shutdown: .*\)/\1/p' "$TMP/kmsg.ro"; } > "$TMP/logs/boot-8/kmsg.log"
gbos_fat_prev_shutdown "$TMP/logs/boot-9" 1 > "$TMP/prev"; grep -q '^=> its shutdown step started at 313.731 s but did not finish' "$TMP/prev" || { cat "$TMP/prev"; fail "the watcher's own line, read back"; }
rm -rf "$TMP/logs/boot-8"
ok "watcher: its kmsg line says whether it clears the flag and whether the volume was already read-only, and the next boot parses it"
watch_case 1 1
waitfor 'grep -q "^remount of $MP read-only FAILED after 3 tries: mount: mounting /dev/sda2 on $MP failed: Device or resource busy$" "$LD/fat-shutdown.txt"' \
  || { cat "$LD/fat-shutdown.txt"; fail "failure reason not in fat-shutdown.txt"; }
waitfor 'grep -q "^emergency remount did not make" "$LD/fat-shutdown.txt"' || { cat "$LD/fat-shutdown.txt"; fail "no emergency remount result"; }
[ "$(grep -c '^mount -t vfat -o remount,ro' "$FAKE_MOUNTS")" = 3 ] || fail "retries: $(cat "$FAKE_MOUNTS")"
grep -q "^  open for writing: pid 4242 (logcat) fd 5 -> $MP/gbos/logs/logcat.txt$" "$LD/fat-shutdown.txt" || { cat "$LD/fat-shutdown.txt"; fail "writer not listed"; }
! grep -q 'fd 6' "$LD/fat-shutdown.txt" || fail "a read-only fd listed as a writer"
grep -q "^detached read-write loop6 ($MP/gbos/bootdisk.img)$" "$LD/fat-shutdown.txt" && grep -q '^trying an emergency remount (sysrq s, u)$' "$LD/fat-shutdown.txt" \
  || { cat "$LD/fat-shutdown.txt"; fail "loop detach / sysrq lines"; }
[ "$(tr -d '\n' < "$RT/proc/sysrq-trigger")" = su ] || fail "sysrq-trigger: $(cat "$RT/proc/sysrq-trigger")"
[ "$(gbos_fat_byte "$TMP/blk" 65)" = 1 ] || fail "cleared the flag of a volume that is still writable"
ok "watcher: EBUSY retried, writers and loops written to fat-shutdown.txt, then sysrq s+u"
# periodic syncfs of the volume during the session (FAT_SYNC_S), stopped at init's unmount step
for v in abc 0 -5 1x; do [ -z "$(FAT_SYNC_S=$v gbos_fat_sync_interval)" ] || fail "FAT_SYNC_S='$v' must be off"; done
[ "$(FAT_SYNC_S=30 gbos_fat_sync_interval)" = 30 ] && [ "$(FAT_SYNC_S= gbos_fat_sync_interval)" = 30 ] && [ "$( (unset FAT_SYNC_S; gbos_fat_sync_interval) )" = 30 ] || fail "FAT_SYNC_S default/30"
export FAKE_SYNCS=$TMP/syncs FAKE_DMESG=$TMP/dmesg; rm -f "$FAKE_SYNCS"; FAT_SYNC_S=1
printf '[   12.5] usb 2-2.3: reset SuperSpeed USB device number 3 using xhci_hcd\n[   12.6] i915 0000:00:02.0: fine\n' > "$FAKE_DMESG"
watch_case 0 1
FAT_SYNC_S=0
grep -q "syncs $MP every 1 s" "$LOG" || { cat "$LOG"; fail "syncer not in the watcher's start line"; }
_n=$(grep -c "^sync -f $MP\$" "$FAKE_SYNCS" 2>/dev/null || :)
[ "${_n:-0}" -ge 2 ] || fail "syncer: ${_n:-0} syncs of $MP in ~2 s: $(cat "$FAKE_SYNCS" 2>/dev/null)"
sleep 2.5; [ "$(grep -c . "$FAKE_SYNCS")" = "$_n" ] || fail "syncer still running after the shutdown step: $(grep -c . "$FAKE_SYNCS") > $_n"
[ "$(grep -v -e '^armed at boot: ' -e '^shutdown step at ' -e '^  open for writing: ' -e '^  read-write loop device: ' "$LD/fat-shutdown.txt")" = "" ] \
  || fail "syncer wrote to fat-shutdown.txt: $(cat "$LD/fat-shutdown.txt")"
# heartbeat: one status line per sync, the matching kernel line once (not again on the next beat), nothing after the shutdown step
HB=$LD/fat-heartbeat.txt
[ "$(grep -c '^uptime ? s: kmsg.log ? bytes, kmsg copier cat alive, superblock rw, kernel error lines 1$' "$HB")" -ge 2 ] \
  || { cat "$HB" 2>/dev/null; fail "heartbeat lines"; }
[ "$(grep -c '^  ' "$HB")" = 1 ] && grep -q '^  \[   12.5\] usb 2-2.3: reset SuperSpeed USB device' "$HB" && ! grep -q i915 "$HB" \
  || { cat "$HB"; fail "heartbeat: kernel error line once"; }
[ "$(grep -c '^uptime' "$HB")" -le "$_n" ] || fail "heartbeat after the shutdown step"
_hb=$(grep -c . "$HB"); unset FAKE_DMESG
# FAT_SHUTDOWN_RO=0: no watcher, the syncer still runs
rm -f "$FAKE_SYNCS"; rm -rf "$LD"; mkdir -p "$LD"; : > "$LOG"
GBOS_FAT_BB=$TMP/fakebb FAT_SHUTDOWN_RO=0 FAT_SYNC_S=1; gbos_fat_start_shutdown_watch /dev/sda2 "$MP" "$LD"; GBOS_FAT_BB=; FAT_SHUTDOWN_RO=1; FAT_SYNC_S=0
SP=$(sed -n 's/.*syncing .* every 1 s (pid \([0-9]*\)).*/\1/p' "$LOG"); [ -n "$SP" ] || { cat "$LOG"; fail "FAT_SHUTDOWN_RO=0: no syncer"; }
PIDS="$PIDS $SP"; waitfor 'grep -q "^sync -f $MP\$" "$FAKE_SYNCS" 2>/dev/null' || fail "FAT_SHUTDOWN_RO=0: syncer never synced"
kill -KILL "$SP" 2>/dev/null || :; unset FAKE_SYNCS
grep -q '^uptime ? s: kmsg.log ? bytes, kmsg copier cat not found, superblock ?, kernel error lines 0$' "$LD/fat-heartbeat.txt" \
  || { cat "$LD/fat-heartbeat.txt"; fail "FAT_SHUTDOWN_RO=0: heartbeat"; }
rm -rf "$LD"; mkdir -p "$LD"; GBOS_FAT_BB=$TMP/fakebb FAT_SYNC_S=1 FAT_HEARTBEAT=0; ( gbos_fat_syncer "$MP" 1 "$LD" ) & SP=$!; PIDS="$PIDS $SP"
sleep 1.5; kill -KILL "$SP" 2>/dev/null || :; GBOS_FAT_BB=; FAT_SYNC_S=0; FAT_HEARTBEAT=1
[ ! -e "$LD/fat-heartbeat.txt" ] || fail "FAT_HEARTBEAT=0 still wrote fat-heartbeat.txt"
ok "syncer: syncfs of the volume every FAT_SYNC_S s, stopped at the unmount step, off for 0/garbage, also without the watcher"
ok "heartbeat: status line before each sync (kmsg.log size, copier, superblock), new kernel error lines once, FAT_HEARTBEAT=0 off"
# the emergency remount working (a kworker makes it read-only a little later), and FAT_SHUTDOWN_SYSRQ=0
rm -f "$FAKE_MOUNT_FAIL" "$FAKE_MOUNTS"; mkrt; : > "$FAKE_MOUNT_FAIL"; rm -rf "$LD"; mkdir -p "$LD"
( while ! grep -q u "$RT/proc/sysrq-trigger"; do sleep 0.05; done; sleep 0.2; sed -i 's/ - vfat \([^ ]*\) rw,/ - vfat \1 ro,/' "$FAKE_MI" ) &
GBOS_FAT_BB=$TMP/fakebb GBOS_FAT_RETRIES=2; gbos_fat_finish /dev/sda2 "$MP" "$LD/fat-shutdown.txt" "$RT/proc" "$RT/sys" "$RT/blk" "$RT" 1 4>"$TMP/kmsg.out" || fail "emergency remount path"
GBOS_FAT_BB=; unset GBOS_FAT_RETRIES
grep -q "^<5>gbos-fat: $MP is read-only (emergency remount, dirty flag cleared, mountinfo: ro)$" "$TMP/kmsg.out" || { cat "$TMP/kmsg.out"; fail "emergency remount result"; }
mkrt; rm -rf "$RT/sys/block/loop6"; : > "$FAKE_MOUNT_FAIL"; rm -rf "$LD"; mkdir -p "$LD"
GBOS_FAT_BB=$TMP/fakebb GBOS_FAT_RETRIES=2 FAT_SHUTDOWN_SYSRQ=0
! gbos_fat_finish /dev/sda2 "$MP" "$LD/fat-shutdown.txt" "$RT/proc" "$RT/sys" "$RT/blk" "$RT" 1 4>/dev/null || fail "FAT_SHUTDOWN_SYSRQ=0: reported success"
GBOS_FAT_BB=; unset GBOS_FAT_RETRIES FAT_SHUTDOWN_SYSRQ
[ ! -s "$RT/proc/sysrq-trigger" ] && grep -q '^no emergency remount (FAT_SHUTDOWN_SYSRQ=0)$' "$LD/fat-shutdown.txt" || fail "FAT_SHUTDOWN_SYSRQ=0"
ok "emergency remount: waits for the kworker; FAT_SHUTDOWN_SYSRQ=0 skips it"

# --- install.sh's fat_off: release before reboot (stubbed reboot)
sleep 300 & SUB=$!; sleep 300 & CAT=$!; PIDS="$PIDS $SUB $CAT"
rm -rf "$GBOS_PROC"; mkp "$CAT" '/gbos/busybox\000cat\000' busybox "$SUB"
rm -f "$FAKE_MOUNTS" "$FAKE_MOUNT_FAIL"; : > "$LOG"; rm -rf "$LD"; mkdir -p "$LD"
eval "$(sed -n '/^fat_off() {/p' "$U/install.sh")"
DEV=/dev/sdb1 L=$LD G=$U GBOS_FAT_BB=$TMP/fakebb GBOS_SYS=$TMP/nosys GBOS_FAT_CLEAR=0
myreboot() { echo "reboot $*" >> "$FAKE_MOUNTS"; }
fat_off myreboot
GBOS_FAT_BB=; G=$TMP/g
[ "$(cat "$FAKE_MOUNTS")" = "mount -t vfat -o remount,ro /dev/sdb1 /gbos
reboot -f" ] || fail "fat_off: $(cat "$FAKE_MOUNTS")"
waitfor '! alive "$CAT" && ! alive "$SUB"' || fail "fat_off left the copier running"
grep -q '^gbos-fat: /gbos remounted read-only (clean)$' "$LOG" || { cat "$LOG"; fail "fat_off log"; }
grep -q '^install.sh: releasing /gbos before the power-off/reboot; clear the dirty flag: no$' "$LD/fat-shutdown.txt" || fail "fat_off: fat-shutdown.txt"
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
cp /bb/busybox /r/gbos/busybox
[ "$(st)" = 1 ] || fail "rw mount did not set the dirty flag"
( exec 5>>/r/gbos/kmsg.log; echo x >&5; sleep 30 ) & W=$!; sleep 0.3
! chroot /r /gbos/busybox mount -t vfat -o remount,ro /dev/sda2 /gbos 2>/tmp/e || fail "remounted with a writer open"
grep -q 'Device or resource busy' /tmp/e || fail "not EBUSY: $(cat /tmp/e)"
kill $W; wait $W 2>/dev/null || :
# a read-write loop on a file of the volume (hook.sh's old fake boot disk) is a writer too, until it is detached
dd if=/dev/zero of=/r/gbos/bootdisk.img bs=1M count=1 2>/dev/null; LB=$(losetup -f); /bb/busybox losetup "$LB" /r/gbos/bootdisk.img
! chroot /r /gbos/busybox mount -t vfat -o remount,ro /dev/sda2 /gbos 2>/dev/null || fail "remounted with a read-write loop on the volume"
/bb/busybox losetup -d "$LB"; sleep 0.3
chroot /r /gbos/busybox mount -t vfat -o remount,ro /dev/sda2 /gbos || fail "remount from a root without /proc and /dev"
[ "$(st)" = 0 ] || fail "remount ro left the dirty flag"
grep ' /p ' /proc/self/mountinfo | grep -q ' - vfat [^ ]* ro,' || fail "superblock not read-only"
! sh -c 'echo y >> /p/t' 2>/dev/null || fail "/postinstall still writable"
umount /p /r/gbos
printf '\001' | dd of="$D" bs=1 seek=65 conv=notrunc 2>/dev/null; mount -t vfat -o rw,flush "$D" /r/gbos
mount -o remount,ro /r/gbos; [ "$(st)" = 1 ] || fail "expected: a volume dirty at mount stays dirty"
mount -o remount,rw /r/gbos; echo z > /r/gbos/z; sync
# boots 9 and 10: fsck.fat's output written to the volume it checks is a lost cluster of that volume ("Reclaimed 1 unused cluster")
$L /u/installer/bin/fsck.fat -n "$D" > /r/gbos/fsck.out 2>&1 || :; grep -q '^Reclaimed 1 unused cluster' /r/gbos/fsck.out || fail "expected the self-made lost cluster"
rm /r/gbos/fsck.out; sync
umount /r/gbos; losetup -d "$D"
echo KERNEL-OK
EOF
  # End to end: stage0 before switch_root (root /r with /bin, /proc, /sys, /dev), the boot check and the watcher from hook.sh, then
  # "switch_root" (/bin, /proc, /sys and /dev gone from /r), a read-write loop on the volume, SIGTERM to everything, init's marker.
  cat > "$TMP/k2.sh" <<'EOF'
set -eu
fail() { echo "FAIL (kernel e2e): $*" >&2; [ -f /r/gbos/gbos/logs/boot-2/fat-shutdown.txt ] && cat /r/gbos/gbos/logs/boot-2/fat-shutdown.txt >&2; cat /r/kmsg.out /r/err >&2 2>/dev/null; exit 1; }
L="/u/installer/lib/ld-musl-x86_64.so.1 --library-path /u/installer/lib"
st() { od -A n -t u1 -j 65 -N 1 "$D" | tr -d ' '; }
cd /tmp; dd if=/dev/zero of=img bs=1M count=64 2>/dev/null; $L /u/installer/bin/mkfs.fat -F 32 img >/dev/null
printf '\001' | dd of=img bs=1 seek=65 conv=notrunc 2>/dev/null             # the previous shutdown left it dirty
D=$(losetup -f); losetup "$D" img
mkdir -p /r; mount -t tmpfs tmpfs /r; mkdir -p /r/gbos /r/bin /r/proc /r/sys /r/dev /r/tmp
mount -t vfat -o rw,flush,umask=000 "$D" /r/gbos                              # sbi->dirty = 1
G=/r/gbos/gbos; mkdir -p $G/logs/boot-2 $G/installer/bin $G/installer/lib
cp /bb/busybox /r/gbos/busybox; cp /bb/busybox /r/bin/busybox; chroot /r /bin/busybox --install -s /bin
cp /s/stick_fat.sh /r/gbos/gbos/; cp /u/installer/bin/fsck.fat $G/installer/bin/; cp /u/installer/lib/ld-musl-x86_64.so.1 $G/installer/lib/
dd if=/dev/zero of=$G/bootdisk.img bs=1M count=1 2>/dev/null; sync
mount --bind /proc /r/proc; mount --bind /sys /r/sys; mount --bind /dev /r/dev; mkfifo /r/kmsg
LB=$(losetup -f); chroot /r /bin/losetup "$LB" /gbos/gbos/bootdisk.img     # read-write, like hook.sh's old loop6
cat > /r/hook.sh <<'EOS'
G=/gbos/gbos; L=$G/logs/boot-2; log() { echo "$*" >> /log; }
GBOS_DMESG="echo FAT-fs (${DEV##*/}): Volume was not properly unmounted. Some data may be corrupt."
. $G/stick_fat.sh
gbos_fat_boot_check "$DEV" /gbos "$L"
GBOS_KMSG=/kmsg GBOS_KMSG_OUT=/kmsg.out GBOS_FAT_ERR=/err gbos_fat_start_shutdown_watch "$DEV" /gbos "$L"
EOS
chroot /r /bin/sh -c "DEV=$D; . /hook.sh" || fail "hook part"
grep -q 'FAT-CHECK .*previous shutdown NOT clean; fsck.fat -n: no errors besides the dirty flag; the flag is cleared at shutdown' /r/log || { cat /r/log $G/logs/boot-2/fat-check.txt; fail "boot check"; }
! grep -q Reclaimed $G/logs/boot-2/fat-check.txt || fail "boot check: fsck still sees its own output as a lost cluster"
grep -q 'clear the dirty flag then: yes' $G/logs/boot-2/fat-shutdown.txt || fail "not armed"
[ "$(st)" = 1 ] || fail "flag cleared while mounted rw"
umount -l /r/proc /r/sys /r/dev; rm -rf /r/bin                                 # switch_root
[ ! -e /r/dev/null ] && [ ! -e /r/proc/self ] || fail "switch_root emulation"
echo late > $G/logs/boot-2/late.txt                                            # Android wrote more files
exec 7>/r/kmsg; sleep 0.5                                                       # the watcher's kmsg reader
WP=$(sed -n 's/.*shutdown watcher pid \([0-9]*\) .*/\1/p' /r/log); [ -n "$WP" ] || fail "no watcher pid"
pkill -TERM -P "$WP" || :; kill -TERM "$WP"; sleep 0.5                          # sysrq "terminate all tasks", early
kill -0 "$WP" || fail "the watcher died of SIGTERM"
printf '14,9,557278000,-,caller=T1;init: sync() before umount...\n' >&7
i=0; until grep -q 'gbos-fat: /gbos is read-only\|FAILED' /r/kmsg.out 2>/dev/null; do i=$((i + 1)); [ $i -lt 100 ] || fail "watcher did nothing"; sleep 0.1; done
grep -q '^<5>gbos-fat: /gbos is read-only (remount, dirty flag cleared, mountinfo: ro)$' /r/kmsg.out || fail "watcher result"
grep -q "^<5>gbos-fat: detached read-write ${LB##*/} (/gbos/gbos/bootdisk.img)$" /r/kmsg.out || fail "rw loop not detached"
[ "$(st)" = 0 ] || fail "flag still set"
[ "$(wc -l < $G/logs/boot-2/fat-shutdown.txt)" = 1 ] || fail "fat-shutdown.txt has more than the armed line"
grep ' /r/gbos ' /proc/self/mountinfo | grep -q ' - vfat [^ ]* ro,' || fail "not read-only"
umount -l /r/.gbos-fat/proc /r/.gbos-fat/sys 2>/dev/null || :; umount -l /r/.gbos-fat 2>/dev/null || :
umount /r/gbos || umount -l /r/gbos
$L /u/installer/bin/fsck.fat -n "$D" > /tmp/o 2>&1 || { cat /tmp/o; fail "fsck after the shutdown"; }
mount -t vfat -o ro "$D" /mnt; [ "$(cat /mnt/gbos/logs/boot-2/late.txt)" = late ] || fail "data lost"; umount /mnt
echo KERNEL-E2E-OK
EOF
  for k in k k2; do
    docker run --rm --privileged -v "$U":/u:ro -v "$BB":/bb/busybox:ro -v "$TMP/$k.sh":/k.sh:ro -v "$U/stick_fat.sh":/s/stick_fat.sh:ro alpine:3.24 sh /k.sh > "$TMP/$k.out" 2>&1 \
      && grep -q '^KERNEL\(-E2E\)\?-OK$' "$TMP/$k.out" || { cat "$TMP/$k.out"; fail "kernel test $k"; }
  done
  ok "kernel: remount ro from a bare root, EBUSY with a writer or a rw loop, dirty-at-mount stays dirty, fsck's own output is a lost cluster"
  ok "kernel end to end: boot check + watcher, switch_root, SIGTERM, rw loop detached, read-only, flag cleared, fsck clean"
  # Boot-4 of the persistent stick (2026-11-22) on the real driver: the previous boot's kmsg.log claims more bytes than its cluster
  # chain holds. Opening it past the chain is a FAT error, errors=remount-ro makes the volume read-only, and fat-check.txt and
  # fat-shutdown.txt can no longer be created. The new boot check must not open it; the old one (negative control) must trip.
  command -v python3 >/dev/null || fail "FAT_KERNEL_TEST needs python3 (to damage a directory entry)"
  mkdir -p "$TMP/k3"; chmod 777 "$TMP/k3"
  cat > "$TMP/k3a.sh" <<'EOF'
set -eu
L="/u/installer/lib/ld-musl-x86_64.so.1 --library-path /u/installer/lib"
cd /t; dd if=/dev/zero of=img bs=1M count=64 2>/dev/null; $L /u/installer/bin/mkfs.fat -F 32 img >/dev/null
D=$(losetup -f); losetup "$D" img; mkdir -p /m; mount -t vfat -o rw,flush,umask=000 "$D" /m
mkdir -p /m/gbos/logs/boot-3 /m/gbos/installer/bin /m/gbos/installer/lib
cp /u/installer/bin/fsck.fat /m/gbos/installer/bin/; cp /u/installer/lib/ld-musl-x86_64.so.1 /m/gbos/installer/lib/
i=0; while [ $i -lt 600 ]; do echo "14,$i,$((i * 1000)),-,caller=T1;init: line $i"; i=$((i + 1)); done > /m/gbos/logs/boot-3/kmsg.log
echo 'armed at boot: remount /gbos (/dev/sda2) read-only at init'"'"'s unmount step; clear the dirty flag then: yes (kmsg copier: 1 2)' > /m/gbos/logs/boot-3/fat-shutdown.txt
umount /m; losetup -d "$D"; chmod 666 img
EOF
  cat > "$TMP/k3b.sh" <<'EOF'
# no set -u: stage0 runs without it
cd /t; cp img run.img; D=$(losetup -f); losetup "$D" run.img; mkdir -p /m; mount -t vfat -o rw,flush,umask=000 "$D" /m
G=/m/gbos; L=$G/logs/boot-4; mkdir -p "$L"
log() { echo "$*" >> /t/stage0.log; }
e0=$(dmesg | grep -c "FAT-fs (${D##*/}): error")
GBOS_FAT_BB=/bb/busybox GBOS_FAT_TMP=/tmp/ram GBOS_FAT_RT=/tmp/rt GBOS_FAT_RT_MOUNT=0 GBOS_KMSG=/dev/null GBOS_KMSG_OUT=/dev/null
GBOS_DMESG="echo FAT-fs (${D##*/}): Volume was not properly unmounted. Some data may be corrupt."
FAT_REPAIR_LOGS=$(cat /t/repair 2>/dev/null || echo 1)
. /s/stick_fat.sh
gbos_fat_boot_check "$D" /m "$L"
gbos_fat_start_shutdown_watch "$D" /m "$L"
WP=$(sed -n 's/.*shutdown watcher pid \([0-9]*\) .*/\1/p' /t/stage0.log); [ -n "$WP" ] && kill -KILL "$WP" 2>/dev/null
e1=$(dmesg | grep -c "FAT-fs (${D##*/}): error")
st=$(awk '$5 == "/m" { for (i = 7; i <= NF && $i != "-"; i++); split($(i + 3), o, ","); print o[1] }' /proc/self/mountinfo)
echo "RESULT sb=$st errors=$((e1 - e0)) check=$([ -f $L/fat-check.txt ] && echo yes || echo no) shutdown=$([ -f $L/fat-shutdown.txt ] && echo yes || echo no) clear=${GBOS_FAT_CLEAR:-}"
echo "SIZE $(stat -c %s $G/logs/boot-3/kmsg.log)"
[ -f $L/fat-check.txt ] && cat $L/fat-check.txt; cat /t/stage0.log
umount /m; losetup -d "$D"
/u/installer/lib/ld-musl-x86_64.so.1 --library-path /u/installer/lib /u/installer/bin/fsck.fat -n run.img > /t/after.fsck 2>&1; echo "AFTER rc=$?"; sed 's/^/AFTER /' /t/after.fsck
rm -f run.img /t/stage0.log /t/after.fsck
EOF
  docker run --rm --privileged -v "$U":/u:ro -v "$TMP/k3":/t -v "$TMP/k3a.sh":/k.sh:ro alpine:3.24 sh /k.sh > "$TMP/k3a.out" 2>&1 || { cat "$TMP/k3a.out"; fail "kernel test k3: image"; }
  python3 -I - "$TMP/k3/img" <<'EOF' || fail "kernel test k3: damaging the directory entry"
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
i = b.find(b'KMSG    LOG'); assert i > 0 and b.find(b'KMSG    LOG', i + 1) < 0
size = struct.unpack_from('<I', b, i + 28)[0]; struct.pack_into('<I', b, i + 28, size + 65536)   # 64 KB more than its chain
b[65] |= 1                                                                                      # and the volume dirty
open(p, 'wb').write(b)
EOF
  $LOADER "$U/installer/bin/fsck.fat" -n "$TMP/k3/img" > "$TMP/k3.fsck" 2>&1 || :
  grep -q '^/gbos/logs/boot-3/kmsg.log$' "$TMP/k3.fsck" && grep -q 'File size is .* cluster chain length is' "$TMP/k3.fsck" || { cat "$TMP/k3.fsck"; fail "k3 fixture: fsck does not see the damage"; }
  git -C "$ROOT" show 23208d6:usb/stick_fat.sh > "$TMP/old_stick_fat.sh" 2>/dev/null || : > "$TMP/old_stick_fat.sh"
  for v in new norepair old; do
    f=$U/stick_fat.sh; [ $v = old ] && f=$TMP/old_stick_fat.sh
    [ -s "$f" ] || { echo "skip: no old stick_fat.sh (git history) for the negative control"; continue; }
    if [ $v = norepair ]; then echo 0 > "$TMP/k3/repair"; else rm -f "$TMP/k3/repair"; fi
    docker run --rm --privileged -v "$U":/u:ro -v "$BB":/bb/busybox:ro -v "$TMP/k3":/t -v "$TMP/k3b.sh":/k.sh:ro -v "$f":/s/stick_fat.sh:ro alpine:3.24 sh /k.sh \
      > "$TMP/k3$v.out" 2>&1 || { cat "$TMP/k3$v.out"; fail "kernel test k3 ($v)"; }
  done
  # default: the damaged log is cut to its chain through the mounted volume, fsck then finds only the dirty flag, the flag will be cleared
  # at shutdown, the previous boot's files are read, no FAT error, and fsck afterwards finds no damaged file
  grep -q '^RESULT sb=rw errors=0 check=yes shutdown=yes clear=1$' "$TMP/k3new.out" \
    && grep -q 'FAT-CHECK .*repaired 1 of 1 log file(s) a forced power-off left longer than their data: /gbos/logs/boot-3/kmsg.log ([0-9]* bytes); then fsck.fat -n: no errors besides the dirty flag' "$TMP/k3new.out" \
    && grep -q '^--- fsck.fat -n .* before the repair (rc=1)$' "$TMP/k3new.out" \
    && grep -q '^previous boot (boot-3) fat-shutdown.txt:$' "$TMP/k3new.out" && ! grep -q 'its files were not read' "$TMP/k3new.out" \
    && ! grep -q '^AFTER .*File size is' "$TMP/k3new.out" && ! grep -q 'READ-ONLY' "$TMP/k3new.out" \
    || { cat "$TMP/k3new.out"; fail "kernel k3: the damaged log was not repaired safely"; }
  ok "kernel: a log file a forced power-off left longer than its chain is cut to the chain; fsck then clean; no FAT error"
  # FAT_REPAIR_LOGS=0: the previous behaviour (nothing changed, the file not opened, flag left set)
  grep -q '^RESULT sb=rw errors=0 check=yes shutdown=yes clear=0$' "$TMP/k3norepair.out" \
    && grep -q '^previous boot (boot-3): its files were not read: fsck found problems (in /gbos/logs/boot-3/kmsg.log); ' "$TMP/k3norepair.out" \
    && grep -q 'FAT-CHECK .*PROBLEMS FOUND' "$TMP/k3norepair.out" && grep -q '^AFTER .*File size is' "$TMP/k3norepair.out" \
    && ! grep -q 'READ-ONLY' "$TMP/k3norepair.out" || { cat "$TMP/k3norepair.out"; fail "kernel k3: FAT_REPAIR_LOGS=0 changed something"; }
  if [ -s "$TMP/old_stick_fat.sh" ]; then
    grep -q '^RESULT sb=ro errors=[1-9][0-9]* check=no shutdown=no ' "$TMP/k3old.out" || { cat "$TMP/k3old.out"; fail "kernel k3 negative control: the old boot check did not trip (fixture?)"; }
    ok "kernel boot-4 replica: the old check makes the volume read-only (no fat-check.txt, no fat-shutdown.txt); the new one leaves it read-write"
  else
    ok "kernel boot-4 replica: the new check leaves the volume read-write (negative control skipped: no git history)"
  fi
fi
echo "PASS test_stick_fat"
