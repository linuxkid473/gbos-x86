# gbOS86: keep the gbos FAT volume clean (the stick's GBOS partition, or an NVMe install's GBOSSYS). Sourced by hook.sh and
# install.sh in stage0; nothing here runs on its own. HANDOFF.md 2026-11-18.
#
# Why: stage0 mounts the volume rw at /gbos (Android sees it as /postinstall) and nothing unmounted it. Android's init only unmounts
# /dev/block/* mounts, super.img is loop-mounted from the volume, and init's last steps before reboot() are sysrq "terminate all tasks"
# (SIGTERM to every process) and reboot(). So every boot logged "FAT-fs (sda2): Volume was not properly unmounted".
# Kernel facts this relies on (6.18 fs/fat/inode.c, fs/namespace.c, fs/super.c):
#   - A superblock remount read-only (no MS_BIND) makes every mount of the volume read-only and clears the dirty flag, but only when the
#     volume was clean when it was mounted: sbi->dirty is read once at mount, and while it is set fat_set_state() never touches the flag.
#     So a volume that was dirty at mount stays dirty until something clears the flag itself.
#   - The remount fails with EBUSY while any file of the volume is open for writing (sb_prepare_remount_readonly), and that includes the
#     backing file of a read-write loop device. hook.sh's fake boot disk (loop6 on gbos/bootdisk.img) was one, for the whole boot: it now
#     runs on a RAM copy, and any read-write loop left on the volume is detached here first.
#   - With stage0's "flush" mount option every close of a file opened for writing sleeps 100 ms (fat_file_release), so nothing on the
#     success path at shutdown writes to the volume.
#   - sysrq "u" (emergency remount) skips the writer check (SB_FORCE): the last resort, only when no read-write loop is left on the volume.
#
#   gbos_fat_boot_check DEV MP LOGDIR    (hook.sh, install.sh) the previous shutdown's state (from the kernel's mount message) and the
#                                        previous boot's fat-shutdown.txt; a read-only fsck.fat -n when it was not clean (gbos.cfg
#                                        FAT_FSCK=1: every boot, 0: never). fsck's output goes to RAM: written to the volume while fsck ran,
#                                        it showed up as a lost cluster of the volume itself ("Reclaimed 1 unused cluster", boots 9 and 10).
#                                        If fsck finds nothing but the dirty bit (and lost clusters, which only waste space), the flag is
#                                        cleared at shutdown, right after the read-only remount, when nothing can write any more
#                                        (FAT_CLEAR_DIRTY=0 leaves it). Report: LOGDIR/fat-check.txt and a "FAT-CHECK" line in
#                                        stage0-dmesg.txt. Never repairs anything else. fsck runs before any file of the previous
#                                        boot is opened, and those files are read only when the volume is known intact: opening a
#                                        damaged one is a FAT error, and errors=remount-ro then makes the volume read-only for the
#                                        whole boot (boot-4 of the persistent stick, 2026-11-22). If that happened anyway, the report
#                                        goes to kmsg instead.
#   gbos_fat_start_shutdown_watch DEV MP LOGDIR
#                                        (hook.sh) a background child of stage0 (like the kmsg copier, so init's service shutdown leaves it
#                                        alone; it ignores sysrq's SIGTERM) that waits for init's "sync() before umount" line in /dev/kmsg.
#                                        By then init has stopped every service. It stops the kmsg copier, detaches read-write loops backed
#                                        by the volume, remounts it read-only (retried while EBUSY, then sysrq u), checks the result in
#                                        mountinfo and clears the dirty flag if the boot check said so. Failures (with the processes and
#                                        loops still writing) are appended to LOGDIR/fat-shutdown.txt while the volume is still writable;
#                                        success writes nothing, and the next boot's fat-check.txt says how it ended. FAT_SHUTDOWN_RO=0 off.
#   gbos_fat_release DEV MP LOGDIR       (install.sh) the same, right before its poweroff -f / reboot -f.
#
# After switch_root, stage0's processes only reach the old initramfs root: /gbos and /gbos/busybox, no /proc, /sys or /dev. So the
# watcher runs every command through $GBOS_FAT_BB, gets its /dev/kmsg fds before switch_root, and keeps what it needs in a small tmpfs at
# $GBOS_FAT_RT that switch_root leaves alone (it only deletes rootfs files): proc and sysfs mounts, the volume's block device node and a
# null device. Tests: tools/test_stick_fat.sh.

GBOS_FAT_BB=${GBOS_FAT_BB-/gbos/busybox}   # empty in the host tests: plain commands
GBOS_PROC=${GBOS_PROC:-/proc}
GBOS_SYS=${GBOS_SYS:-/sys}
GBOS_FAT_RT=${GBOS_FAT_RT:-/.gbos-fat}
GBOS_FAT_NULL=${GBOS_FAT_NULL:-/dev/null}  # the watcher sets $GBOS_FAT_RT/null: a redirection to a missing file makes ash skip the command
# init's shutdown (system/core/init/reboot.cpp): StopServices (all, then logd/vold/tombstoned), sync(), "Ready to unmount apexes", apexd
# --unmount-all, then the sysrq kill. Boot-8 (2026-11-18): 316 ms from "sync() before umount" to "sysrq: Terminate All Tasks".
# A /dev/kmsg record is "prio,seq,usec,flags[,caller=T1];message": anchored at the ";" so a message quoting these words does not match.
GBOS_FAT_MARKER=${GBOS_FAT_MARKER:-';init: (sync[(][)] before umount|Ready to unmount apexes)'}

# Offset of the FAT "state" byte (bit 0 = dirty, as the Linux driver and fsck.fat use it): 65 on FAT32, 37 on FAT12/16.
# Fails (prints nothing) unless DEV starts with a FAT boot sector that has an extended BPB.
gbos_fat_state_offset() {
  _fo=$($GBOS_FAT_BB od -A n -t u1 -v -N 512 "$1" 2>"$GBOS_FAT_NULL") || return 1
  set -- $_fo
  [ $# = 512 ] && [ "${511}" = 85 ] && [ "${512}" = 170 ] || return 1
  case $(( ${12} + 256 * ${13} )) in 512|1024|2048|4096) ;; *) return 1;; esac
  if [ "${23}" = 0 ] && [ "${24}" = 0 ]; then _fo=65; else _fo=37; fi   # 16-bit FAT size 0: FAT32
  eval "_fs=\${$((_fo + 2))}"                                          # extended boot signature 0x28/0x29 right after the state byte
  case $_fs in 40|41) echo $_fo;; *) return 1;; esac
}

gbos_fat_byte() { $GBOS_FAT_BB od -A n -t u1 -j "$2" -N 1 "$1" 2>"$GBOS_FAT_NULL" | $GBOS_FAT_BB tr -d ' \n'; }

# Clear bit 0 of the state byte and nothing else; check by reading it back. Writes one byte through the block device's page cache,
# which the FAT driver's own boot-sector buffer shares, so it is coherent with the mounted filesystem.
gbos_fat_clear_dirty() {
  _co=$(gbos_fat_state_offset "$1") || return 1
  _cv=$(gbos_fat_byte "$1" $_co); [ -n "$_cv" ] || return 1
  [ $((_cv & 1)) = 0 ] && return 0
  printf "\\$(printf %o $((_cv & 254)))" | $GBOS_FAT_BB dd of="$1" bs=1 seek=$_co count=1 conv=notrunc,fsync 2>"$GBOS_FAT_NULL" || return 1
  [ "$(gbos_fat_byte "$1" $_co)" = $((_cv & 254)) ]
}

# Was DEV dirty when stage0 mounted it rw? The kernel says so at that mount (stage0's earlier read-only mount prints nothing).
gbos_fat_was_dirty() { ${GBOS_DMESG:-dmesg} 2>/dev/null | grep -q "FAT-fs (${1##*/}): Volume was not properly unmounted"; }

# fsck.fat -n output (dosfstools 4.2) that shows nothing wrong but the dirty bit: the bit itself, the FAT32 backup boot sector differing
# in that one byte, the free-cluster count in FSInfo (a hint the FAT spec says not to trust; Linux recounts it), and lost clusters
# (allocated in the FAT but in no file: left as they are they only waste space, nothing reads them). Anything else (bad or cross-linked
# chains, FAT copies differing, directory errors, ...) is a real problem and the volume is left exactly as it is.
gbos_fsck_only_dirty() { # OUTPUT_FILE STATE_OFFSET
  grep -q ': [0-9]* files, [0-9]*/[0-9]* clusters$' "$1" || return 1   # fsck got to the end
  ! grep -v -E -e '^fsck\.fat [0-9]' -e '^$' \
      -e '^There are differences between boot sector and its backup\.$' \
      -e '^This is mostly harmless\. Differences: \(offset:original/backup\)$' \
      -e "^  $2:[0-9a-f][0-9a-f]/[0-9a-f][0-9a-f]\$" -e '^  Not automatically fixing this\.$' \
      -e '^Dirty bit is set\. Fs was not properly unmounted and some data may be corrupt\.$' -e '^ Automatically removing dirty bit\.$' \
      -e '^Free cluster summary wrong \([0-9]+ vs\. really [0-9]+\)$' -e '^  Auto-correcting\.$' \
      -e '^Reclaimed [0-9]+ unused clusters? \([0-9]+ bytes\)\.$' \
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

# From here on everything also runs after switch_root: only $GBOS_FAT_BB applets and shell builtins, and stderr only to $GBOS_FAT_NULL.
gbos_fat_stop_copier() { # [SUBSHELL_PID CAT_PID]: stop the subshell first so it cannot start a new cat
  [ -n "$1" ] && kill -STOP "$1"
  [ -n "$2" ] && kill -KILL "$2"
  [ -n "$1" ] && kill -KILL "$1"
  return 0
}

# "ro" or "rw": the superblock state of the volume mounted at MP, from mountinfo's super options (the field after "- fstype source").
gbos_fat_sb_state() { # MP PROC
  [ -r "$2/self/mountinfo" ] || return 1
  $GBOS_FAT_BB awk -v mp="$1" '$5 == mp { for (i = 7; i <= NF && $i != "-"; i++); split($(i + 3), o, ","); print o[1]; exit }' \
    "$2/self/mountinfo" 2>"$GBOS_FAT_NULL"
}

# Read-write loop devices whose backing file is on the volume: "loopN PATH" lines. Their open backing file makes the remount EBUSY.
gbos_fat_rw_loops() { # MP SYS
  for _ll in "$2"/block/loop*; do
    [ -r "$_ll/loop/backing_file" ] || continue
    read -r _lro < "$_ll/ro" || continue; [ "$_lro" = 0 ] || continue
    read -r _lbf < "$_ll/loop/backing_file" || continue
    case $_lbf in "$1"/*) echo "${_ll##*/} $_lbf";; esac
  done
}

# Detach them (nothing uses the fake boot disk any more at init's unmount step). A loop that is still in use (the dual-boot install's
# metadata.img under /metadata) only gets autoclear and stays. Prints one line per loop.
gbos_fat_detach_loops() { # MP SYS NODEDIR
  gbos_fat_rw_loops "$1" "$2" | while read -r _dn _df; do
    read -r _dm < "$2/block/$_dn/dev" || continue
    [ -b "$3/$_dn" ] || $GBOS_FAT_BB mknod "$3/$_dn" b "${_dm%:*}" "${_dm#*:}" 2>"$GBOS_FAT_NULL"
    if $GBOS_FAT_BB losetup -d "$3/$_dn" 2>"$GBOS_FAT_NULL"; then echo "detached read-write $_dn ($_df)"
    else echo "detaching read-write $_dn ($_df) FAILED"; fi
  done
}

# What still keeps the volume writable: processes with a file of it open for writing (/proc/PID/fd + fdinfo flags; same st_dev as MP)
# and read-write loops on it.
gbos_fat_writers() { # MP PROC SYS
  _wv=$($GBOS_FAT_BB stat -c %d "$1" 2>"$GBOS_FAT_NULL")
  for _wf in "$2"/[0-9]*/fd/*; do
    [ -n "$_wv" ] && [ "$($GBOS_FAT_BB stat -L -c %d "$_wf" 2>"$GBOS_FAT_NULL")" = "$_wv" ] || continue
    _wp=${_wf%/fd/*}; _wn=${_wf##*/}; _wl=
    while read -r _wk _wx; do [ "$_wk" = flags: ] && { _wl=$_wx; break; }; done < "$_wp/fdinfo/$_wn"
    [ -n "$_wl" ] && [ $(( 0$_wl & 3 )) != 0 ] || continue                       # octal open flags: O_WRONLY 1, O_RDWR 2
    _wc=?; read -r _wc < "$_wp/comm"
    echo "  open for writing: pid ${_wp##*/} ($_wc) fd $_wn -> $($GBOS_FAT_BB readlink "$_wf" 2>"$GBOS_FAT_NULL")"
  done
  gbos_fat_rw_loops "$1" "$3" | while read -r _dn _df; do echo "  read-write loop device: $_dn -> $_df"; done
}

# Superblock remount (no MS_BIND: every mount of the volume, /gbos and Android's /postinstall, becomes read-only). The kernel ignores the
# source and the options on a vfat remount, and busybox doesn't look the source up for one, so it works without /dev and /proc. Retried
# while EBUSY (a writer that was just killed still closes its files, 100 ms each with "flush"). The last error goes to $_fe.
gbos_fat_remount_ro() { # DEV MP
  _rn=0
  while ! _fe=$($GBOS_FAT_BB mount -t vfat -o remount,ro "$1" "$2" 2>&1); do
    _rn=$((_rn + 1))
    [ $_rn -lt ${GBOS_FAT_RETRIES:-30} ] || return 1
    $GBOS_FAT_BB sleep 0.05
  done
}

# Make the volume read-only and clean. DEV is only passed to mount; BLK is the block device to clear the flag through (DEV in stage0,
# $GBOS_FAT_RT/blk after switch_root). On failure, the reason and what was still writing go to REPORT (the volume is still writable then).
gbos_fat_finish() { # DEV MP REPORT PROC SYS BLK NODEDIR CLEAR(0|1)
  _ff=$(gbos_fat_detach_loops "$2" "$5" "$7")
  [ -n "$_ff" ] && echo "<5>gbos-fat: $_ff" >&4
  _fz=
  if gbos_fat_remount_ro "$1" "$2"; then
    _fz="remount"
  else
    _fy=$(gbos_fat_writers "$2" "$4" "$5")
    if [ "${FAT_SHUTDOWN_SYSRQ:-1}" = 0 ]; then _fg="no emergency remount (FAT_SHUTDOWN_SYSRQ=0)"
    elif [ ! -w "$4/sysrq-trigger" ] || [ -z "$(gbos_fat_sb_state "$2" "$4")" ]; then _fg="no emergency remount (no /proc)"
    elif [ -n "$(gbos_fat_rw_loops "$2" "$5")" ]; then _fg="no emergency remount (a read-write loop device still uses the volume)"
    else _fg="trying an emergency remount (sysrq s, u)"; fi
    { echo "remount of $2 read-only FAILED after ${GBOS_FAT_RETRIES:-30} tries: $_fe"; [ -z "$_ff" ] || echo "$_ff"; [ -z "$_fy" ] || echo "$_fy"
      echo "$_fg"; } >> "$3"
    echo "<3>gbos-fat: remount of $2 read-only FAILED: $_fe; $_fg" >&4
    case $_fg in trying*)
      echo s >> "$4/sysrq-trigger"; echo u >> "$4/sysrq-trigger"   # both run in a kworker: wait for the result
      _fn=0; while [ "$(gbos_fat_sb_state "$2" "$4")" != ro ] && [ $_fn -lt 60 ]; do _fn=$((_fn + 1)); $GBOS_FAT_BB sleep 0.05; done
      [ "$(gbos_fat_sb_state "$2" "$4")" = ro ] && _fz="emergency remount"
      [ -n "$_fz" ] || echo "emergency remount did not make $2 read-only" >> "$3";;
    esac
    [ -n "$_fz" ] || return 1
  fi
  _fq=$(gbos_fat_sb_state "$2" "$4")
  if [ "$_fq" = rw ]; then   # mount said yes but the superblock is still writable (a bind remount?): report it, nothing else to try
    echo "$_fz of $2 returned success but mountinfo still says rw" >> "$3"; echo "<3>gbos-fat: $2 still rw after $_fz" >&4; return 1
  fi
  if [ "$8" = 1 ]; then
    if gbos_fat_clear_dirty "$6"; then _fz="$_fz, dirty flag cleared"
    else   # the volume is read-only now: open it for one line, then close it again
      _fz="$_fz, clearing the dirty flag through $6 FAILED"
      $GBOS_FAT_BB mount -t vfat -o remount,rw "$1" "$2" 2>"$GBOS_FAT_NULL" && { echo "$_fz" >> "$3"; gbos_fat_remount_ro "$1" "$2"; }
    fi
  fi
  echo "<5>gbos-fat: $2 is read-only (${_fz}${_fq:+, mountinfo: $_fq})" >&4
  case $_fz in *FAILED*) return 1;; esac
}

# The tmpfs at $GBOS_FAT_RT (before switch_root): proc and sysfs, the volume's block device node "blk", "null". Prints what is missing.
gbos_fat_rt_setup() { # DEV
  _tr=$GBOS_FAT_RT; _tm=
  if [ "${GBOS_FAT_RT_MOUNT:-1}" = 1 ]; then
    mkdir -p "$_tr" 2>/dev/null && { mountpoint -q "$_tr" 2>/dev/null || mount -t tmpfs -o mode=0700,size=1m gbos-fat "$_tr" 2>/dev/null; } \
      || _tm="$_tm tmpfs"
    mkdir -p "$_tr/proc" "$_tr/sys" 2>/dev/null
    [ -r "$_tr/proc/self/mountinfo" ] || mount -t proc proc "$_tr/proc" 2>/dev/null || _tm="$_tm proc"
    [ -d "$_tr/sys/block" ] || mount -t sysfs sysfs "$_tr/sys" 2>/dev/null || _tm="$_tm sysfs"
    _tn=$(cat "$GBOS_SYS/class/block/${1##*/}/dev" 2>/dev/null)
    [ -b "$_tr/blk" ] || { [ -n "$_tn" ] && mknod "$_tr/blk" b "${_tn%:*}" "${_tn#*:}" 2>/dev/null; } || _tm="$_tm blk"
    [ -c "$_tr/null" ] || mknod -m 666 "$_tr/null" c 1 3 2>/dev/null || _tm="$_tm null"
  fi
  echo "${_tm# }"
}

# Paths (volume-relative, lower case) that fsck.fat -n names in its output: the files and directories it found something wrong with.
gbos_fsck_named_paths() { grep '^/' "$1" 2>/dev/null | grep -v ': [0-9]* files, [0-9]*/[0-9]* clusters$' | tr 'A-Z' 'a-z'; }

# A forced power-off leaves the files that were open for appending (kmsg.log, logcat.txt) longer in their directory entry than their
# cluster chain: fsck says "File size is X bytes, cluster chain length is Y bytes." (boots 4-7 of the persistent stick, 2026-11-22/23).
# gbos_fsck_log_truncations prints "Y PATH" for each such file under /gbos/logs/ and returns 0 only when, apart from those, fsck found
# nothing but what gbos_fsck_only_dirty accepts. Any other problem, or a damaged file anywhere else, returns 1: nothing is repaired.
gbos_fsck_log_truncations() { # OUTPUT_FILE STATE_OFFSET
  _lt=${1}.rest
  awk -v rest="$_lt" '
    function flush() { if (p != "") print p > rest; p = "" }
    /^\// && !/: [0-9]+ files, [0-9]+\/[0-9]+ clusters$/ { flush(); p = $0; next }
    p != "" && match($0, /^  File size is [0-9]+ bytes, cluster chain length is [0-9]+ bytes\.$/) {
      split($0, w, " "); q = tolower(p)
      if (q ~ /^\/gbos\/logs\/boot-[0-9]+\/[a-z0-9._-]+$/ && w[10] + 0 < w[4] + 0) { print w[10], p; p = ""; skip = 1; next }
      flush(); print $0 > rest; next }
    skip && /^  Truncating file to [0-9]+ bytes\.$/ { skip = 0; next }
    { skip = 0; flush(); print $0 > rest }
    END { flush() }' "$1" > "$1.trunc" || { rm -f "$_lt" "$1.trunc"; return 1; }
  touch "$_lt"
  if [ -s "$1.trunc" ] && gbos_fsck_only_dirty "$_lt" "$2"; then cat "$1.trunc"; rm -f "$_lt" "$1.trunc"; return 0; fi
  rm -f "$_lt" "$1.trunc"; return 1
}
# Cut each listed log file to its cluster chain through the mounted volume. truncate(2) to a size the chain holds frees nothing and
# never reads data past the chain, so it cannot trip the FAT error that reading the file would. FAT_REPAIR_LOGS=0 turns it off.
gbos_fat_repair_logs() { # MP LIST_FILE ("BYTES /PATH" lines) -> prints how many were cut
  _rn=0
  while read -r _ry _rp; do
    case "$_rp" in */../*|*/./*) continue;; esac
    $GBOS_FAT_BB truncate -s "$_ry" "$1$_rp" 2>/dev/null && _rn=$((_rn + 1))
  done < "$2"
  $GBOS_FAT_BB sync
  echo $_rn
}

# The kernel's FAT messages in a previous boot's kmsg.log (whole file, first events): "MOUNTED_DIRTY_S|READ_ONLY_S|FIRST_ERROR", empty
# fields when absent. READ_ONLY_S is "Filesystem has been set read-only": a FAT error with errors=remount-ro (stage0's mount, the
# default) made the superblock read-only. From then on no file can be created (EROFS) while files that were already open (kmsg.log)
# keep growing, and "remount,ro" has nothing left to do and returns 0 (real vfat driver, 2026-11-22). Boot-4 of the persistent stick.
gbos_fat_kmsg_fat_events() { # KMSG_LOG
  tr -d '\0' < "$1" 2>/dev/null | awk -F, '
    /;FAT-fs \([^)]*\): Volume was not properly unmounted/ { if (d == "") d = sprintf("%.3f", $3 / 1000000) }
    /;FAT-fs \([^)]*\): error, / { if (e == "") { e = $0; sub(/^[^;]*;/, "", e) } }
    /;FAT-fs \([^)]*\): Filesystem has been set read-only/ { r = sprintf("%.3f", $3 / 1000000); exit }
    END { print d "|" r "|" e }'
}

# The watcher's "gbos-fat: shutdown:" line near the end of a kmsg.log: "LINE|RECORDS_AFTER_IT|LAST_RECORD_S". The copier keeps copying
# for ~50 ms after that line (boot-4: 72 apexd records, 13 KB), so it is not in the last 8 KB: search the last 256 KB.
gbos_fat_kmsg_shutdown_line() { # KMSG_LOG
  tail -c 262144 "$1" 2>/dev/null | tr -d '\0' | awk -F, '
    /;gbos-fat: shutdown: / { s = $0; sub(/^[^;]*;/, "", s); n = 0; t = ""; next }
    s != "" && /^[0-9]+,[0-9]+,[0-9]+,/ { n++; t = $3 }
    END { if (s != "") printf "%s|%d|%s\n", s, n, (t == "" ? "" : sprintf("%.3f", t / 1000000)) }'
}

# The previous boot's shutdown, as far as the volume can tell: its fat-shutdown.txt (armed at boot; failures appended at shutdown), the
# watcher's line near the end of its kmsg.log (copied there just before the copier stops), the kernel's FAT messages in that log, and
# whether this mount found the volume clean. WHY_NOT_READ (from the boot check, when the volume was not clean and fsck did not show it
# intact): the previous boot's files are not opened at all. Reading a file whose directory entry claims more than its cluster chain holds
# is a FAT error, and the kernel then makes the whole volume read-only for the rest of the boot: boot-4 of the persistent stick lost every
# file it would have created after 25.8 s (fat-check.txt, fat-shutdown.txt, logcat) to the old check's tail of boot-3's damaged kmsg.log.
gbos_fat_prev_shutdown() { # LOGDIR WAS_DIRTY(0|1) [WHY_NOT_READ]
  _pn=${1##*/boot-}; case $_pn in ''|*[!0-9]*) return 0;; esac
  _pd=${1%/boot-*}/boot-$((_pn - 1)); [ -d "$_pd" ] || return 0
  if [ -n "${3:-}" ]; then
    echo "previous boot (${_pd##*/}): its files were not read: $3"
    if [ "$2" = 0 ]; then echo "=> it left the volume read-only and clean"; else echo "=> unknown: that boot's logs were not read (see above)"; fi
    return 0
  fi
  _pk=; _pe='||'
  if [ -f "$_pd/kmsg.log" ]; then
    _pk=$(gbos_fat_kmsg_shutdown_line "$_pd/kmsg.log")
    [ "$2" = 1 ] && _pe=$(gbos_fat_kmsg_fat_events "$_pd/kmsg.log")
  fi
  _pkl=${_pk%%|*}; _pkr=${_pk#*|}; _pkn=${_pkr%%|*}; _pkt=${_pkr#*|}
  _pmd=${_pe%%|*}; _per=${_pe#*|}; _pro=${_per%%|*}; _perr=${_per#*|}
  _pat=; [ -n "$_pkl" ] && _pat=$(echo "$_pkl" | sed -n 's/.*unmount step at \([0-9.]*\) s.*/\1/p')
  _pcl=   # was that shutdown step to clear the dirty flag? its kmsg line says so; for older lines, the armed line
  case $_pkl in *'clearing the dirty flag'*) _pcl=yes;; *remounting*) _pcl=no;; esac
  if [ -z "$_pcl" ] && [ -f "$_pd/fat-shutdown.txt" ]; then
    _pcl=$(tr -d '\0' < "$_pd/fat-shutdown.txt" | sed -n 's/^armed at boot: .*clear the dirty flag then: \([a-z]*\).*/\1/p' | head -1)
  fi
  _pmdirty=0; { [ -n "$_pmd" ] || grep -q 'NOT clean' "$_pd/fat-check.txt" 2>/dev/null; } && _pmdirty=1
  if [ -f "$_pd/fat-shutdown.txt" ]; then
    echo "previous boot (${_pd##*/}) fat-shutdown.txt:"; tr -d '\0' < "$_pd/fat-shutdown.txt" | sed 's/^/  /'
  elif [ -n "$_pro" ]; then
    echo "previous boot (${_pd##*/}): no fat-shutdown.txt: it could not be created, the volume was read-only from $_pro s (below)"
  else
    echo "previous boot (${_pd##*/}): no fat-shutdown.txt (FAT_SHUTDOWN_RO=0, an older stick_fat.sh, or it never got that far)"
  fi
  if [ -n "$_pkl" ]; then
    echo "  end of its kmsg.log: $_pkl"
    [ "${_pkn:-0}" -gt 0 ] && echo "  ($_pkn more kernel records were copied after it, the last at $_pkt s: the copier stops 50 ms after that line)"
  fi
  [ -n "$_pmd" ] && echo "  its kmsg.log: the kernel found the volume dirty when that boot mounted it ($_pmd s)"
  if [ -n "$_pro" ]; then
    echo "  its kmsg.log: a FAT error made the volume READ-ONLY at $_pro s: ${_perr:-no error line}"
    echo "  (from then on no file could be created on it: no fat-check.txt, fat-shutdown.txt or logcat; files already open, like kmsg.log, kept growing)"
  fi
  if [ "$2" = 0 ]; then echo "=> it left the volume read-only and clean"
  elif [ -f "$_pd/fat-shutdown.txt" ] && grep -q 'FAILED' "$_pd/fat-shutdown.txt"; then echo "=> its shutdown step failed (lines above)"
  elif [ "$_pcl" = no ] && [ $_pmdirty = 1 ]; then
    echo "=> that boot already mounted it dirty and was not allowed to clear the flag (its fsck found problems or did not run, or FAT_CLEAR_DIRTY=0): it stays dirty whatever the remount did${_pat:+ (its shutdown step started at $_pat s)}"
  elif [ -n "$_pat" ]; then
    echo "=> its shutdown step started at $_pat s but did not finish: killed or cut off by reboot() before the remount or the flag clear, or the flag clear failed once the volume was read-only"
  elif [ -n "$_pk" ]; then echo "=> its shutdown step started but did not finish: killed or cut off by reboot() before the remount or the flag clear"
  else echo "=> its shutdown step never started: forced power-off, crash, or the watcher was not running"; fi
}

# A description when the volume at MP is read-only now although stage0 mounted it rw: a FAT error this boot (errors=remount-ro).
gbos_fat_ro_now() { # DEV MP
  [ "$(gbos_fat_sb_state "$2" "$GBOS_PROC")" = ro ] || return 1
  _rq=$(${GBOS_DMESG:-dmesg} 2>/dev/null | grep "FAT-fs (${1##*/}): error" | head -1 | sed 's/^.*\(FAT-fs (\)/\1/')
  echo "${_rq:-no FAT-fs error in dmesg}"
}

gbos_fat_boot_check() { # DEV MP LOGDIR
  _bd=$1; _bm=$2; _bld=$3; _bo=$3/fat-check.txt
  _bf=$G/installer/bin/fsck.fat
  _bx=${GBOS_FAT_TMP:-/tmp}; mkdir -p "$_bx" 2>/dev/null
  _br=$_bx/gbos-fat-check.$$   # the report and fsck's output stay in RAM until fsck is done (see the top of this file)
  GBOS_FAT_CLEAR=0
  _bo2=$(gbos_fat_state_offset "$_bd") || _bo2=
  case $_bo2 in 65) _bt=FAT32;; 37) _bt=FAT12/16;; *) _bt="no FAT boot sector found";; esac
  if gbos_fat_was_dirty "$_bd"; then _bp=1; _bs="previous shutdown NOT clean"; else _bp=0; _bs="previous shutdown clean"; fi
  case ${FAT_FSCK:-auto} in
    0) _bk=0; _bv="fsck skipped (FAT_FSCK=0)";;
    1) _bk=1;;
    *) _bk=$_bp; _bv="fsck not needed";;
  esac
  [ $_bk = 1 ] && [ -z "$_bo2" ] && { _bk=0; _bv="fsck skipped (no FAT boot sector)"; }
  [ $_bk = 1 ] && [ ! -f "$_bf" ] && { _bk=0; _bv="fsck skipped (gbos/installer/bin/fsck.fat missing)"; }
  # fsck first: it reads the block device, never a file, so it cannot trip a FAT error. The previous boot's files are opened only when
  # the volume is known to be intact (clean at mount, or fsck found nothing but the dirty flag and lost clusters).
  _bn="the volume was not cleanly unmounted and fsck did not run ($_bv); opening a damaged file would make the kernel set the whole volume read-only"
  : > "$_br.fsck"
  if [ $_bk = 1 ]; then
    _bc=$(gbos_fat_copier); set -- $_bc
    [ -n "$2" ] && kill -STOP "$2" 2>/dev/null       # no kmsg.log appends while fsck reads the directories
    $GBOS_FAT_BB df "$_bm" >/dev/null 2>&1           # statfs: the driver recounts free clusters and rewrites a stale FSInfo hint
    $GBOS_FAT_BB sync
    _bt0=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    _brc=0; _brep=
    $GBOS_FAT_BB timeout 120 $G/installer/lib/ld-musl-x86_64.so.1 --library-path $G/installer/lib "$_bf" -n "$_bd" > "$_br.fsck" 2>&1 || _brc=$?
    # Only damage a forced power-off leaves in our own logs: cut those files to their chains, then check again (one pass).
    if [ $_brc != 0 ] && [ "${FAT_REPAIR_LOGS:-1}" = 1 ] && gbos_fsck_log_truncations "$_br.fsck" "$_bo2" > "$_br.trunc"; then
      _bru=$(gbos_fat_repair_logs "$_bm" "$_br.trunc")
      _brep="repaired $_bru of $(wc -l < "$_br.trunc") log file(s) a forced power-off left longer than their data: $(awk '{ printf "%s%s (%s bytes)", s, $2, $1; s = ", " }' "$_br.trunc")"
      cat "$_br.fsck" > "$_br.fsck0"; _brc0=$_brc; _brc=0
      $GBOS_FAT_BB timeout 120 $G/installer/lib/ld-musl-x86_64.so.1 --library-path $G/installer/lib "$_bf" -n "$_bd" > "$_br.fsck" 2>&1 || _brc=$?
    fi
    rm -f "$_br.trunc"
    _bt1=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    [ -n "$2" ] && kill -CONT "$2" 2>/dev/null
    _bl=$(sed -n 's/^Reclaimed \([0-9]*\) unused cluster.*/\1/p' "$_br.fsck" | head -1)
    if [ $_brc = 0 ]; then
      _bv="fsck.fat -n: no errors"; _bn=
    elif gbos_fsck_only_dirty "$_br.fsck" "$_bo2"; then
      _bv="fsck.fat -n: no errors besides the dirty flag"; _bn=
      [ -n "$_bl" ] && _bv="$_bv and $_bl lost cluster(s) (left allocated: harmless; chkdsk or fsck.fat -a frees them)"
      if [ $_bp = 0 ]; then
        _bv="$_bv (set by this boot's mount; the read-only remount at shutdown clears it)"
      elif [ "${FAT_CLEAR_DIRTY:-1}" = 0 ]; then
        _bv="$_bv; left set (FAT_CLEAR_DIRTY=0)"
      else
        GBOS_FAT_CLEAR=1
        _bv="$_bv; the flag is cleared at shutdown, after the read-only remount"
      fi
    else
      _bv="fsck.fat -n rc=$_brc: PROBLEMS FOUND, nothing changed (see fat-check.txt; repair on another computer: Windows chkdsk X: /f, macOS Disk Utility First Aid)"
      _bw=$(gbos_fsck_named_paths "$_br.fsck" | tr '\n' ' ')
      _bn="fsck found problems${_bw:+ (in ${_bw% })}; opening a damaged file would make the kernel set the whole volume read-only"
    fi
  fi
  [ -n "${_brep:-}" ] && _bv="$_brep; then $_bv"
  [ $_bp = 0 ] && _bn=
  { echo "$_bd at $_bm, $_bt: $_bs"
    [ $_bp = 1 ] && echo "(the kernel found the dirty flag set when stage0 mounted it: \"Volume was not properly unmounted\")"
    gbos_fat_prev_shutdown "$_bld" $_bp "$_bn"
    [ -n "${_brep:-}" ] && { echo "--- fsck.fat -n $_bd before the repair (rc=$_brc0)"; cat "$_br.fsck0"; }
    [ $_bk = 1 ] && { echo "--- fsck.fat -n $_bd (rc=$_brc, uptime $_bt0 -> $_bt1 s)"; cat "$_br.fsck"; echo "---"; }
    echo "$_bv"; } > "$_br" 2>/dev/null
  rm -f "$_br.fsck" "$_br.fsck0"
  # Safety net: the volume should still be read-write here. If a FAT error made it read-only, nothing can create a file on it for the
  # rest of the boot (this report, fat-shutdown.txt, logcat): say so in kmsg, which the already-open kmsg.log still records.
  _bro=; _bro=$(gbos_fat_ro_now "$_bd" "$_bm") && echo "$_bm is READ-ONLY now (FAT error: $_bro): no file can be created on it this boot" >> "$_br"
  if ! { cat "$_br" > "$_bo"; } 2>/dev/null; then
    while IFS= read -r _bz; do log "hook: fat-check.txt (not writable): $_bz"; done < "$_br"
  fi
  rm -f "$_br"
  log "hook: FAT-CHECK $_bd: $_bs; $_bv${_bro:+; $_bm is READ-ONLY now (FAT error: $_bro)}"
}

# fd 3 = /dev/kmsg for reading, fd 4 = /dev/kmsg for writing (both opened before switch_root). The shutdown runs inside the pipeline as soon
# as awk sees the marker (not after the pipeline, which would wait for cat's next kmsg record). cat ending (kmsg EPIPE) restarts the read.
gbos_fat_watch() { # DEV MP LOGDIR CLEAR [COPIER_SUBSHELL COPIER_CAT]
  while :; do
    $GBOS_FAT_BB cat <&3 | {
      _wt=$($GBOS_FAT_BB awk -v m="$GBOS_FAT_MARKER" -F, '$0 ~ m { printf "%.3f", $3 / 1000000; f = 1; exit } END { exit !f }') || exit 1
      gbos_fat_shutdown "$_wt" "$@"
      exit 0
    } && return 0
    $GBOS_FAT_BB sleep 1 || return 1
  done
}

# Kernel lines that say the stick stopped taking writes (FAT error, I/O error, USB reset or disconnect, SCSI offline).
GBOS_FAT_ERRPAT=${GBOS_FAT_ERRPAT:-'FAT-fs \(|I/O error|USB disconnect|reset (high|super|Super)|rejecting I/O|offline device|Remounting filesystem read-only'}

# One line now: uptime, kmsg.log's size as Linux sees it, whether the copier's cat still runs, the superblock state.
gbos_fat_status() { # MP LOGDIR COPIER_CAT PROC
  _su=?; [ -r "$4/uptime" ] && read -r _su _sv < "$4/uptime"
  _sk=$($GBOS_FAT_BB stat -c %s "$2/kmsg.log" 2>"$GBOS_FAT_NULL") || _sk=
  _sc=dead; [ -z "$3" ] && _sc="not found"; [ -n "$3" ] && kill -0 "$3" 2>"$GBOS_FAT_NULL" && _sc=alive
  echo "uptime ${_su} s: kmsg.log ${_sk:-?} bytes, kmsg copier cat ${_sc}, superblock $(gbos_fat_sb_state "$1" "$4" || echo ?)"
}

gbos_fat_shutdown() { # KMSG_SECONDS DEV MP LOGDIR CLEAR [COPIER_SUBSHELL COPIER_CAT]
  # This line reaches kmsg.log through the copier, which still runs: the last line of that log. Besides the status line below, nothing
  # else is written to the volume unless something fails. It also says whether the volume is read-only already (a FAT error this boot, see gbos_fat_kmsg_fat_events):
  # then the remount has nothing to do, and nothing was written to the volume since that error but files that were already open.
  _sx=; [ "$(gbos_fat_sb_state "$3" "$GBOS_FAT_RT/proc")" = ro ] && _sx="; $3 is ALREADY read-only (a FAT error earlier this boot?)"
  echo "<5>gbos-fat: shutdown: init reached its unmount step at $1 s; stopping the kmsg copier, remounting $3 read-only$([ "$5" = 1 ] && echo ' and clearing the dirty flag' || echo ', leaving the dirty flag as it is')$_sx (end of this log)" >&4
  [ -n "${GBOS_FAT_SYNC_PID:-}" ] && kill -KILL "$GBOS_FAT_SYNC_PID" 2>/dev/null   # KILL: it inherits the watcher's ignored TERM
  # Written while the volume is still writable, so the next boot sees how this boot ended: whether kmsg.log still grew, the copier ran,
  # and what held the volume open. No line after it but a FAILED one means the remount worked.
  { echo "shutdown step at $1 s: $(gbos_fat_status "$3" "$4" "$7" "$GBOS_FAT_RT/proc")"; gbos_fat_writers "$3" "$GBOS_FAT_RT/proc" "$GBOS_FAT_RT/sys"
  } >> "$4/fat-shutdown.txt" 2>"$GBOS_FAT_NULL"
  $GBOS_FAT_BB sleep 0.05
  gbos_fat_stop_copier "$6" "$7"
  gbos_fat_finish "$2" "$3" "$4/fat-shutdown.txt" "$GBOS_FAT_RT/proc" "$GBOS_FAT_RT/sys" "$GBOS_FAT_RT/blk" "$GBOS_FAT_RT" "$5"
}

# Every SECONDS s, syncfs(2) the volume: the directory entries and FAT sectors of files written since the last sync. Without it they
# stayed in RAM for the whole session (the "flush" option writes a file's data at close, not its directory entry or FAT chain), so a
# forced power-off cost every logcat file rotated during the boot, not only the open one: boot 6 left logcat.txt.01-.05 at 0 bytes, and
# chkdsk deleted all six of boot 11's (HANDOFF.md 2026-11-24). A background child of stage0 like the watcher, so it outlives switch_root
# (busybox at /gbos/busybox, the mount point is still /gbos). gbos.cfg FAT_SYNC_S=seconds, 0 = off.
#
# Before each sync it appends one gbos_fat_status line to LOGDIR/fat-heartbeat.txt (FAT_HEARTBEAT=1, the default; at most
# FAT_HEARTBEAT_MAX lines), then any new kernel line matching GBOS_FAT_ERRPAT: boots 10/11 (2026-11-23) stopped writing every log
# about a minute into the boot although the user shut down cleanly, and the last heartbeat that reached the stick dates that stop.
gbos_fat_syncer() { # MP SECONDS [LOGDIR [COPIER_SUBSHELL COPIER_CAT]]
  _hn=0; _he=0; _hp=$GBOS_FAT_RT/proc; [ -r "$_hp/self/mountinfo" ] || _hp=$GBOS_PROC
  while $GBOS_FAT_BB sleep "$2"; do
    if [ -n "${3:-}" ] && [ "${FAT_HEARTBEAT:-1}" = 1 ] && [ $_hn -lt ${FAT_HEARTBEAT_MAX:-480} ]; then
      _hn=$((_hn + 1))
      _hd=$($GBOS_FAT_BB dmesg 2>"$GBOS_FAT_NULL" | $GBOS_FAT_BB grep -E "$GBOS_FAT_ERRPAT" 2>"$GBOS_FAT_NULL") || _hd=
      _hx=0; [ -n "$_hd" ] && _hx=$(echo "$_hd" | $GBOS_FAT_BB wc -l)
      { echo "$(gbos_fat_status "$1" "$3" "${5:-}" "$_hp"), kernel error lines $_hx"
        [ "$_hx" -gt "$_he" ] && echo "$_hd" | $GBOS_FAT_BB tail -n $((_hx - _he)) | $GBOS_FAT_BB sed 's/^/  /'
      } >> "$3/fat-heartbeat.txt" 2>"$GBOS_FAT_NULL"
      _he=$_hx   # fewer than last time: the ring buffer wrapped, count from what it holds now
    fi
    $GBOS_FAT_BB sync -f "$1" || return 1
  done
}

gbos_fat_sync_interval() { # prints the interval in seconds, nothing when off or not a number
  case "${FAT_SYNC_S:-30}" in ''|*[!0-9]*) return 0;; esac
  [ "${FAT_SYNC_S:-30}" -gt 0 ] && echo "${FAT_SYNC_S:-30}"
  return 0
}

gbos_fat_start_shutdown_watch() { # DEV MP LOGDIR
  _wr=$3/fat-shutdown.txt
  _ws=$(gbos_fat_sync_interval)
  if [ "${FAT_SHUTDOWN_RO:-1}" = 0 ]; then
    echo "not armed: FAT_SHUTDOWN_RO=0" > "$_wr" 2>/dev/null; log "hook: fat: no read-only remount at shutdown (FAT_SHUTDOWN_RO=0)"
    if [ -n "$_ws" ]; then ( gbos_fat_syncer "$2" "$_ws" "$3" ) & log "hook: fat: syncing $2 every $_ws s (pid $!)"; fi
    return 0
  fi
  if [ -n "$GBOS_FAT_BB" ] && [ ! -x "$GBOS_FAT_BB" ]; then
    echo "not armed: $GBOS_FAT_BB missing" > "$_wr" 2>/dev/null; log "hook: fat: $GBOS_FAT_BB missing, no read-only remount at shutdown"; return 1
  fi
  _wc=$(gbos_fat_copier)
  _wm=$(gbos_fat_rt_setup "$1")
  _wn=$GBOS_FAT_RT/null; [ -c "$_wn" ] || [ -L "$_wn" ] || _wn=$GBOS_FAT_RT/proc/self/fd/2
  _wl=$(gbos_fat_rw_loops "$2" "$GBOS_SYS" | tr '\n' ' ')
  _wa="armed at boot: remount $2 ($1) read-only at init's unmount step; clear the dirty flag then: $([ "${GBOS_FAT_CLEAR:-0}" = 1 ] && echo yes || echo no)"
  _wa="$_wa (kmsg copier: ${_wc:-not found}${_wm:+; missing in $GBOS_FAT_RT: $_wm}${_wl:+; read-write loops on it now: $_wl})"
  # Not writable (boot-4: a FAT error had made the volume read-only): the line goes to kmsg, which the open kmsg.log still records.
  { echo "$_wa" > "$_wr"; } 2>/dev/null || log "hook: fat: fat-shutdown.txt NOT writable ($(gbos_fat_ro_now "$1" "$2" || echo 'reason unknown')); it would say: $_wa"
  ( trap '' TERM HUP   # init's last step is sysrq "terminate all tasks": SIGTERM to everything, possibly before the remount is done
    exec 3<"${GBOS_KMSG:-/dev/kmsg}" 4>"${GBOS_KMSG_OUT:-/dev/kmsg}" 2>"${GBOS_FAT_ERR:-/dev/null}"
    GBOS_FAT_NULL=$_wn
    GBOS_FAT_SYNC_PID=
    if [ -n "$_ws" ]; then gbos_fat_syncer "$2" "$_ws" "$3" $_wc & GBOS_FAT_SYNC_PID=$!; fi
    gbos_fat_watch "$1" "$2" "$3" "${GBOS_FAT_CLEAR:-0}" $_wc ) &
  log "hook: fat: shutdown watcher pid $! (kmsg copier: ${_wc:-not found}; clear the flag at shutdown: ${GBOS_FAT_CLEAR:-0}${_wm:+; missing in $GBOS_FAT_RT: $_wm}); remounts $2 read-only at init's unmount step; syncs $2 every ${_ws:-(off)} s"
}

gbos_fat_release() { # DEV MP LOGDIR: before poweroff -f / reboot -f in stage0 (install.sh); nothing can write to MP afterwards
  echo "install.sh: releasing $2 before the power-off/reboot; clear the dirty flag: $([ "${GBOS_FAT_CLEAR:-0}" = 1 ] && echo yes || echo no)" >> "$3/fat-shutdown.txt" 2>/dev/null
  gbos_fat_stop_copier $(gbos_fat_copier) 2>/dev/null
  if gbos_fat_finish "$1" "$2" "$3/fat-shutdown.txt" "$GBOS_PROC" "$GBOS_SYS" "$1" "${GBOS_FAT_TMP:-/tmp}" "${GBOS_FAT_CLEAR:-0}" 4>/dev/null; then
    log "gbos-fat: $2 remounted read-only (clean)"
  else
    log "gbos-fat: releasing $2 FAILED (gbos/logs/boot-N/fat-shutdown.txt)"
  fi
}
