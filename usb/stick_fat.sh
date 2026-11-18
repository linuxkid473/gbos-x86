# gbOS86: keep the gbos FAT volume clean (the stick's GBOS partition, or an NVMe install's GBOSSYS). Sourced by hook.sh and
# install.sh in stage0; nothing here runs on its own. HANDOFF.md 2026-11-18.
#
# Why: stage0 mounts the volume rw at /gbos (Android sees it as /postinstall) and nothing unmounted it. Android's init only unmounts
# /dev/block/* mounts, super.img is loop-mounted from the volume, and init's last step before reboot() is sysrq "terminate all tasks",
# which also kills stage0's kmsg copier. So every boot logged "FAT-fs (sda2): Volume was not properly unmounted". Note that Linux never
# clears the on-disk dirty flag of a volume that was already dirty when it was mounted (fs/fat/inode.c fat_set_state, sbi->dirty);
# remounting read-only only cleans a volume that was clean at mount, so a dirty one stays dirty until something checks it.
#
#   gbos_fat_boot_check DEV MP LOGDIR    (hook.sh) the previous shutdown's state (from the kernel's mount message); a read-only
#                                        fsck.fat -n when it was not clean (gbos.cfg FAT_FSCK=1: every boot, 0: never); if that finds
#                                        nothing but the dirty bit, clears that one bit (FAT_CLEAR_DIRTY=0 leaves it). Report:
#                                        LOGDIR/fat-check.txt and a "FAT-CHECK" line in stage0-dmesg.txt. Never repairs anything else.
#   gbos_fat_start_shutdown_watch DEV MP KMSGLOG
#                                        (hook.sh) a background process (a child of stage0, like the kmsg copier, so init's service
#                                        shutdown leaves it alone) that waits for init's "sync() before umount" line in /dev/kmsg. By then
#                                        init has stopped every service (no more log writers); the watcher stops the kmsg copier, syncs and
#                                        remounts the volume read-only, which clears the dirty flag. FAT_SHUTDOWN_RO=0 turns it off.
#   gbos_fat_release DEV MP LOGFILE      (install.sh) the same, right before its poweroff -f / reboot -f.
#
# After switch_root, stage0's processes only reach the old initramfs root: /gbos and /gbos/busybox, no /proc, no /dev. So the watcher
# runs every command through $GBOS_FAT_BB and gets its /dev/kmsg fds before switch_root. Tests: tools/test_stick_fat.sh.

GBOS_FAT_BB=${GBOS_FAT_BB-/gbos/busybox}   # empty in the host tests: plain commands
GBOS_PROC=${GBOS_PROC:-/proc}
# init's shutdown (system/core/init/reboot.cpp): StopServices (all, then logd/vold/tombstoned), sync(), "Ready to unmount apexes", apexd
# --unmount-all, then the sysrq kill. Boot-8 (2026-11-18): 316 ms from "sync() before umount" to "sysrq: Terminate All Tasks".
# A /dev/kmsg record is "prio,seq,usec,flags[,caller=T1];message": anchored at the ";" so a message quoting these words does not match.
GBOS_FAT_MARKER=${GBOS_FAT_MARKER:-';init: (sync[(][)] before umount|Ready to unmount apexes)'}

# Offset of the FAT "state" byte (bit 0 = dirty, as the Linux driver and fsck.fat use it): 65 on FAT32, 37 on FAT12/16.
# Fails (prints nothing) unless DEV starts with a FAT boot sector that has an extended BPB.
gbos_fat_state_offset() {
  _fo=$($GBOS_FAT_BB od -A n -t u1 -v -N 512 "$1" 2>/dev/null) || return 1
  set -- $_fo
  [ $# = 512 ] && [ "${511}" = 85 ] && [ "${512}" = 170 ] || return 1
  case $(( ${12} + 256 * ${13} )) in 512|1024|2048|4096) ;; *) return 1;; esac
  if [ "${23}" = 0 ] && [ "${24}" = 0 ]; then _fo=65; else _fo=37; fi   # 16-bit FAT size 0: FAT32
  eval "_fs=\${$((_fo + 2))}"                                          # extended boot signature 0x28/0x29 right after the state byte
  case $_fs in 40|41) echo $_fo;; *) return 1;; esac
}

gbos_fat_byte() { $GBOS_FAT_BB od -A n -t u1 -j "$2" -N 1 "$1" 2>/dev/null | $GBOS_FAT_BB tr -d ' \n'; }

# Clear bit 0 of the state byte and nothing else; check by reading it back. Writes one byte through the block device's page cache,
# which the FAT driver's own boot-sector buffer shares, so it is coherent with the mounted filesystem.
gbos_fat_clear_dirty() {
  _co=$(gbos_fat_state_offset "$1") || return 1
  _cv=$(gbos_fat_byte "$1" $_co); [ -n "$_cv" ] || return 1
  [ $((_cv & 1)) = 0 ] && return 0
  printf "\\$(printf %o $((_cv & 254)))" | $GBOS_FAT_BB dd of="$1" bs=1 seek=$_co count=1 conv=notrunc,fsync 2>/dev/null || return 1
  [ "$(gbos_fat_byte "$1" $_co)" = $((_cv & 254)) ]
}

# Was DEV dirty when stage0 mounted it rw? The kernel says so at that mount (stage0's earlier read-only mount prints nothing).
gbos_fat_was_dirty() { ${GBOS_DMESG:-dmesg} 2>/dev/null | grep -q "FAT-fs (${1##*/}): Volume was not properly unmounted"; }

# fsck.fat -n output (dosfstools 4.2) that shows nothing wrong but the dirty bit: the bit itself, the FAT32 backup boot sector differing
# in that one byte, and the free-cluster count in FSInfo (a hint the FAT spec says not to trust; Linux recounts it). Anything else
# (lost clusters, bad chains, FAT copies differing, ...) is a real problem and the volume is left exactly as it is.
gbos_fsck_only_dirty() { # OUTPUT_FILE STATE_OFFSET
  grep -q ': [0-9]* files, [0-9]*/[0-9]* clusters$' "$1" || return 1   # fsck got to the end
  ! grep -v -E -e '^fsck\.fat [0-9]' -e '^$' \
      -e '^There are differences between boot sector and its backup\.$' \
      -e '^This is mostly harmless\. Differences: \(offset:original/backup\)$' \
      -e "^  $2:[0-9a-f][0-9a-f]/[0-9a-f][0-9a-f]\$" -e '^  Not automatically fixing this\.$' \
      -e '^Dirty bit is set\. Fs was not properly unmounted and some data may be corrupt\.$' -e '^ Automatically removing dirty bit\.$' \
      -e '^Free cluster summary wrong \([0-9]+ vs\. really [0-9]+\)$' -e '^  Auto-correcting\.$' \
      -e '^Leaving filesystem unchanged\.$' -e ': [0-9]+ files, [0-9]+/[0-9]+ clusters$' "$1" | grep -q .
}

# stage0's kmsg copier: "( exec 3</dev/kmsg; while /gbos/busybox cat <&3 >> $L/kmsg.log || ...; do :; done ) &". Prints
# "SUBSHELL_PID CAT_PID" (the cat holds kmsg.log open for writing; the subshell would restart it). Needs /proc: call before switch_root.
gbos_fat_copier() {
  for _cc in $(grep -l '^/gbos/busybox' $GBOS_PROC/[0-9]*/cmdline 2>/dev/null); do
    [ "$(tr '\0' ' ' < "$_cc" 2>/dev/null)" = "/gbos/busybox cat " ] || continue
    _cd=${_cc%/cmdline}; _cs=$(cat "$_cd/stat" 2>/dev/null) || continue
    _cs=${_cs##*) }; set -- $_cs                                       # after "pid (comm) ": state ppid ...
    echo "$2 ${_cd##*/}"; return 0
  done
  return 1
}

# No "2>/dev/null" in gbos_fat_stop_copier, gbos_fat_remount_ro, gbos_fat_watch and gbos_fat_shutdown: they run after switch_root, where
# /dev/null does not exist and a failed redirection makes the shell skip the command. The watcher's stderr goes to /dev/null once, at start.
gbos_fat_stop_copier() { # [SUBSHELL_PID CAT_PID]: stop the subshell first so it cannot start a new cat
  [ -n "$1" ] && kill -STOP "$1"
  [ -n "$2" ] && kill -KILL "$2"
  [ -n "$1" ] && kill -KILL "$1"
  return 0
}

# Superblock remount (no MS_BIND: every mount of the volume, /gbos and Android's /postinstall, becomes read-only). The kernel ignores the
# source and the options on a vfat remount, and busybox doesn't look the source up for one, so it works without /dev and /proc. EBUSY means
# a file is still open for writing: retried for ~0.2 s. A failure is written to LOGFILE (the volume is still writable then).
gbos_fat_remount_ro() { # DEV MP LOGFILE
  $GBOS_FAT_BB sync
  _rn=0
  while ! _re=$($GBOS_FAT_BB mount -t vfat -o remount,ro "$1" "$2" 2>&1); do
    _rn=$((_rn + 1))
    if [ $_rn -ge ${GBOS_FAT_RETRIES:-5} ]; then
      echo "gbos-fat: remount of $2 read-only FAILED: $_re" >> "$3"; $GBOS_FAT_BB sync
      return 1
    fi
    $GBOS_FAT_BB sleep 0.05
  done
}

gbos_fat_boot_check() { # DEV MP LOGDIR
  _bd=$1; _bm=$2; _bo=$3/fat-check.txt
  _bf=$G/installer/bin/fsck.fat
  _bo2=$(gbos_fat_state_offset "$_bd") || _bo2=
  case $_bo2 in 65) _bt=FAT32;; 37) _bt=FAT12/16;; *) _bt="no FAT boot sector found";; esac
  if gbos_fat_was_dirty "$_bd"; then _bp=1; _bs="previous shutdown NOT clean"; else _bp=0; _bs="previous shutdown clean"; fi
  echo "$_bd at $_bm, $_bt: $_bs" > "$_bo" 2>/dev/null
  [ $_bp = 1 ] && echo "(the kernel found the dirty flag set when stage0 mounted it: \"Volume was not properly unmounted\")" >> "$_bo"
  case ${FAT_FSCK:-auto} in
    0) _br=0; _bv="fsck skipped (FAT_FSCK=0)";;
    1) _br=1;;
    *) _br=$_bp; _bv="fsck not needed";;
  esac
  [ $_br = 1 ] && [ -z "$_bo2" ] && { _br=0; _bv="fsck skipped (no FAT boot sector)"; }
  [ $_br = 1 ] && [ ! -f "$_bf" ] && { _br=0; _bv="fsck skipped (gbos/installer/bin/fsck.fat missing)"; }
  if [ $_br = 1 ]; then
    _bc=$(gbos_fat_copier); set -- $_bc
    [ -n "$2" ] && kill -STOP "$2" 2>/dev/null       # no kmsg.log appends while fsck reads the directories
    $GBOS_FAT_BB df "$_bm" >/dev/null 2>&1           # statfs: the driver recounts free clusters and rewrites a stale FSInfo hint
    $GBOS_FAT_BB sync
    _bt0=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    _brc=0
    $GBOS_FAT_BB timeout 120 $G/installer/lib/ld-musl-x86_64.so.1 --library-path $G/installer/lib "$_bf" -n "$_bd" > "$_bo.tmp" 2>&1 || _brc=$?
    _bt1=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    if [ $_brc = 0 ]; then
      _bv="fsck.fat -n: no errors"
    elif gbos_fsck_only_dirty "$_bo.tmp" "$_bo2"; then
      _bv="fsck.fat -n: no errors besides the dirty flag"
      if [ $_bp = 0 ]; then
        _bv="$_bv (set by this boot's mount; cleared at shutdown)"
      elif [ "${FAT_CLEAR_DIRTY:-1}" = 0 ]; then
        _bv="$_bv; left set (FAT_CLEAR_DIRTY=0)"
      elif gbos_fat_clear_dirty "$_bd"; then
        _bv="$_bv; dirty flag cleared"
      else
        _bv="$_bv; clearing the dirty flag FAILED"
      fi
    else
      _bv="fsck.fat -n rc=$_brc: PROBLEMS FOUND, nothing changed (see fat-check.txt; repair on another computer: Windows chkdsk X: /f, macOS Disk Utility First Aid)"
    fi
    [ -n "$2" ] && kill -CONT "$2" 2>/dev/null
    { echo "--- fsck.fat -n $_bd (rc=$_brc, uptime $_bt0 -> $_bt1 s)"; cat "$_bo.tmp"; echo "---"; } >> "$_bo"; rm -f "$_bo.tmp"
  fi
  echo "$_bv" >> "$_bo"
  log "hook: FAT-CHECK $_bd: $_bs; $_bv"
}

# fd 3 = /dev/kmsg for reading, fd 4 = /dev/kmsg for writing (both opened before switch_root). The shutdown runs inside the pipeline as soon
# as awk sees the marker (not after the pipeline, which would wait for cat's next kmsg record). cat ending (kmsg EPIPE) restarts the read.
gbos_fat_watch() { # DEV MP KMSGLOG [COPIER_SUBSHELL COPIER_CAT]
  while :; do
    $GBOS_FAT_BB cat <&3 | {
      $GBOS_FAT_BB awk -v m="$GBOS_FAT_MARKER" '$0 ~ m { f = 1; exit } END { exit !f }' || exit 1
      gbos_fat_shutdown "$@"
      exit 0
    } && return 0
    $GBOS_FAT_BB sleep 1 || return 1
  done
}

gbos_fat_shutdown() { # DEV MP KMSGLOG [COPIER_SUBSHELL COPIER_CAT]
  echo "<5>gbos-fat: init reached its unmount step: stopping the kmsg copier, remounting $2 read-only" >&4
  gbos_fat_stop_copier "$4" "$5"
  echo "gbos-fat: shutdown: init reached its unmount step; kmsg copier stopped, remounting $2 read-only (end of this log)" >> "$3"
  if gbos_fat_remount_ro "$1" "$2" "$3"; then
    echo "<5>gbos-fat: $2 is read-only, the FAT is clean" >&4
  else
    echo "<3>gbos-fat: remounting $2 read-only FAILED (reason at the end of kmsg.log)" >&4; return 1
  fi
}

gbos_fat_start_shutdown_watch() { # DEV MP KMSGLOG
  if [ "${FAT_SHUTDOWN_RO:-1}" = 0 ]; then log "hook: fat: no read-only remount at shutdown (FAT_SHUTDOWN_RO=0)"; return 0; fi
  if [ -n "$GBOS_FAT_BB" ] && [ ! -x "$GBOS_FAT_BB" ]; then log "hook: fat: $GBOS_FAT_BB missing, no read-only remount at shutdown"; return 1; fi
  _wc=$(gbos_fat_copier)
  ( exec 3<"${GBOS_KMSG:-/dev/kmsg}" 4>"${GBOS_KMSG_OUT:-/dev/kmsg}" 2>"${GBOS_FAT_ERR:-/dev/null}"; gbos_fat_watch "$1" "$2" "$3" $_wc ) &
  log "hook: fat: shutdown watcher pid $! (kmsg copier: ${_wc:-not found}); remounts $2 read-only at init's unmount step"
}

gbos_fat_release() { # DEV MP LOGFILE: before poweroff -f / reboot -f in stage0 (install.sh); nothing can write to MP afterwards
  gbos_fat_stop_copier $(gbos_fat_copier) 2>/dev/null
  if gbos_fat_remount_ro "$1" "$2" "$3"; then log "gbos-fat: $2 remounted read-only (clean)"; else log "gbos-fat: remounting $2 read-only FAILED"; fi
}
