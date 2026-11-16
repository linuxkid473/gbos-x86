#!/system/bin/sh
# native_zygote.sh N: keep Android 17's Native Framework Prototype off, and record what zygote_next holds open.
# Run by the gbos_nativezygote service from gbos.rc ("on boot"), as root, once per boot. Writes logs/boot-N/native-zygote.txt.
#
# Why: Android 17 can start isolated app processes (Chrome's sandboxed/GPU processes, WebView renderers) through a second zygote,
# /system/bin/zygote_next, instead of app_zygote. On gbOS86 every SpawnSubspecies child of zygote_next dies in its fd-registry
# reset ("zygote::file_descriptors: Resetting FileDescriptorRegistry for subspecies" then "Reinitialization", EBADF), so each
# isolated process fails to start: apps open as blank windows, Chrome's GPU channel times out, and system_server logs 12k-50k
# "ZygoteProcess: Got error connecting to zygote" lines per boot (Dell Vostro 3400 on R5, HP ENVY x360). Cause (HANDOFF.md
# 2026-11-16, found by itakosu55): /dev/null was a regular file, created by a redirect in stage0 before its mknod, so the offsets
# of fds 0-2 moved and zygote_next's per-child fd audit failed. hook.sh now recreates it as the character device. This script stays
# as a second line of defence (flag off) and as the record: it logs the type of /dev/null and zygote_next's fd table every boot.
#
# Mitigation (needs no knowledge of the cause):
#  1. persist.zygote.zygote_next.start_on_boot=false (zygote_next64.rc starts it on boot only when this is true; it is
#     "disabled" otherwise, but the framework can still start it on demand, which is why this alone is not enough).
#  2. DeviceConfig android_os/native_framework_prototype=false, so the framework stops routing app processes to it. Re-applied
#     every minute: Google Play services syncs DeviceConfig and can put the server value back.
#  NOT covered: an app whose manifest names a native-only service explicitly (Chrome 157's NativeOnlySandboxedProcessService)
#  can still reach zygote_next with the flag off. The capture below is for finding out why the child fails.
#
# Evidence: for every zygote_next process (a new pid is a restart) it dumps /proc/PID/fd, fdinfo, status and the SELinux
# context, plus init's own fd table, so the fd the registry rejects can be named from one boot's log.
#
# Test hooks (tools/test_native_zygote.sh): GBOS_NZ_LOGDIR, GBOS_NZ_PROC, GBOS_NZ_TICKS (stop after that many 5 s ticks), GBOS_NZ_SLEEP.
N=$1
L=${GBOS_NZ_LOGDIR:-/postinstall/gbos/logs/boot-$N}
P=${GBOS_NZ_PROC:-/proc}
SLEEP=${GBOS_NZ_SLEEP:-5}
O=$L/native-zygote.txt
NS=android_os
FLAG=native_framework_prototype

mkdir -p "$L" 2>/dev/null
log() { echo "$(date +%T 2>/dev/null) $*" >> "$O"; }

# 1. the boot-start property (also set at "on boot" in the rc; harmless twice)
setprop persist.zygote.zygote_next.start_on_boot false 2>/dev/null

# 2. the DeviceConfig flag. "device_config get" prints the value, or "null" when the flag has never been set.
apply_flag() {
  cur=$(device_config get $NS $FLAG 2>/dev/null)
  [ "$cur" = false ] && return 0
  if device_config put $NS $FLAG false >/dev/null 2>&1; then
    log "device_config $NS/$FLAG was '${cur:-<no answer>}', set to false"
  else
    fails=$((fails + 1))
    [ $fails -le 5 ] && log "device_config put $NS/$FLAG failed (was '${cur:-<no answer>}'): system_server not ready yet?"
    return 1
  fi
}

# 3. one dump per zygote_next pid
dumped=
dump_zygote() {
  pid=$1
  {
    echo "== zygote_next pid $pid ($(date +%T 2>/dev/null))"
    echo "-- cmdline"; tr '\0' ' ' < $P/$pid/cmdline 2>&1; echo
    echo "-- SELinux context"; cat $P/$pid/attr/current 2>&1; echo
    echo "-- status (Name, PPid, Uid, FDSize)"; grep -E '^(Name|PPid|Uid|FDSize):' $P/$pid/status 2>&1
    echo "-- ls -l /proc/$pid/fd"; ls -l $P/$pid/fd 2>&1
    echo "-- fdinfo"
    for f in $P/$pid/fdinfo/*; do
      [ -e "$f" ] || continue
      echo "fd ${f##*/}: $(readlink $P/$pid/fd/${f##*/} 2>/dev/null)"
      sed 's/^/    /' "$f" 2>&1
    done
    echo "-- ls -l /proc/1/fd (init: anything open here that is not CLOEXEC is inherited by every service)"; ls -l $P/1/fd 2>&1
    echo "-- zygote properties"; getprop 2>/dev/null | grep -i zygote
  } >> "$O" 2>&1
  dumped="$dumped $pid"
}

log "native_zygote.sh start: boot $N"
# must be "crw-rw-rw- ... 1, 3": a regular file here (leading "-") breaks every zygote_next subspecies child
log "/dev/null: $(ls -l /dev/null 2>&1)"
tick=0
while :; do
  # the flag every 12 ticks (a minute), and straight away until it has been set once
  if [ $((tick % 12)) -eq 0 ] || [ -z "$flag_ok" ]; then apply_flag && flag_ok=1; fi
  # evidence: at most 8 dumps a boot
  for pid in $(pidof zygote_next 2>/dev/null); do
    [ $(echo $dumped | wc -w) -ge 8 ] && break
    case " $dumped " in *" $pid "*) ;; *) dump_zygote $pid;; esac
  done
  tick=$((tick + 1))
  [ -n "$GBOS_NZ_TICKS" ] && [ $tick -ge "$GBOS_NZ_TICKS" ] && break
  sleep $SLEEP
done
log "native_zygote.sh end"
