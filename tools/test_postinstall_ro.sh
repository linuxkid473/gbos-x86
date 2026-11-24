#!/bin/sh
# Test of usb/hook.sh gbos_postinstall_lockdown: Android's /postinstall (the gbos FAT, mounted umask=000 by stage0) is read-only for Android,
# and only this boot's log directory is writable, behind a root-only (0700) tmpfs. HANDOFF.md 2026-11-19.
#   1. host: the function cut out of the shipped hook.sh, run with a fake mount: the mount sequence, POSTINSTALL_RW=1, every fallback, the
#      run.sh gate, and a static check that everything Android writes on the volume is under logs/boot-N and written by a root service.
#   2. kernel (privileged docker, image gbos-kbuild, stage0's busybox): the same function with the real busybox mount on a loop vfat mounted as
#      stage0 mounts it, then writes and execs as root, as uid 1000 (system: gbos_fp, soft_gk), and as an app uid (10123). Also checks that a
#      root process gets write access in a mount namespace of its own without it reaching anyone else (toybox-style unshare, shared mounts),
#      and that stick_fat.sh's superblock read-only remount at shutdown still works with these mounts. gbos_fp (uid system) appends
#      logs/boot-N/fingerprint.txt through /dev/gbos-system-log (gbos_system_log), after stage0's mount --move of /dev.
# Run: sh tools/test_postinstall_ro.sh      (BUSYBOX=path, default kernel/out/busybox of this or the main checkout; SKIP_KERNEL=1 = host only)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
H=$ROOT/usb/hook.sh
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }
fn() { awk -v f="^$1\\\\(\\\\)" '$0 ~ f {p=1} p {print} p && /^}/ {exit}' "$2"; }

# ---------------------------------------------------------------- 2. kernel part (inside the container)
if [ "${1:-}" = --inner ]; then
  BB=/opt/gbos-bb/busybox; W=/w; mkdir -p $W
  export PATH=/usr/sbin:/usr/bin:/sbin:/bin
  as() { _u=$1; shift; setpriv --reuid="$_u" --regid="$_u" --clear-groups --inh-caps=-all -- /bin/sh -c "$*" >/dev/null 2>&1; }
  truncate -s 64M $W/fat.img && $BB mkdosfs -n GBOS $W/fat.img >/dev/null || { bad "mkdosfs"; exit 1; }
  DEV=$(losetup -f --show $W/fat.img) || { bad "losetup (privileged container?)"; exit 1; }
  trap 'cd /; umount -R /new 2>/dev/null; umount /gbos 2>/dev/null; losetup -d $DEV 2>/dev/null' EXIT
  mkdir -p /gbos /new/postinstall
  $BB mount -t vfat -o rw,flush,umask=000 $DEV /gbos || { bad "mount vfat as stage0 does"; exit 1; }
  G=/gbos/gbos; N=7; L=$G/logs/boot-$N
  mkdir -p $L $G/logs/boot-6 && echo 'MODE=ask' > $G/gbos.cfg && printf '#!/bin/sh\necho fp-ok\n' > $G/gbos_fp && echo old > $G/logs/boot-6/logcat.txt
  as 10123 "echo x > $G/app-on-gbos" && ok "precondition: an app uid can write stage0's umask=000 mount" || bad "precondition: umask=000 not world-writable?"
  rm -f $G/app-on-gbos
  # what hook.sh does
  mount() { $BB mount "$@"; }; umount() { $BB umount "$@"; }; log() { echo "$*" >> $W/hook.log; }
  eval "$(fn gbos_postinstall_lockdown /repo/usb/hook.sh)"; eval "$(fn gbos_root_rw /repo/usb/hook.sh)"
  mkdir -p /stage0dev /new/dev && $BB mount -t tmpfs -o mode=0755 tmpfs /stage0dev
  export GBOS_DEV=/stage0dev
  mount --bind /gbos /new/postinstall && gbos_postinstall_lockdown /new/postinstall "$L" "$N"; rc=$?
  # gbos_fp (uid system) gets logs/boot-7/fingerprint.txt through stage0's /dev, which then moves into Android as stage0 moves it
  eval "$(fn gbos_system_log /repo/usb/hook.sh)"
  FPLOG=$(gbos_system_log fingerprint.txt)
  $BB mount --move /stage0dev /new/dev
  unset -f mount umount
  mount --make-rshared / 2>/dev/null   # then switch_root, and Android init makes / rshared: a flag change must still not travel to peers
  P=/new/postinstall/gbos
  [ $rc = 0 ] && [ "$GBOS_LOGS_PRIVATE" = 1 ] && ok "lockdown with the real busybox mount: rc 0, logs private" || bad "lockdown rc=$rc private=$GBOS_LOGS_PRIVATE: $(cat $W/hook.log)"
  grep -q 'POSTINSTALL: /postinstall read-only' $W/hook.log && ! grep -q POSTINSTALL-RW $W/hook.log && ok "logged: read-only, logs root-only" || bad "log: $(cat $W/hook.log)"
  o=$(awk '$5 == "/new/postinstall" {print $6}' /proc/self/mountinfo)
  case ,$o, in *,ro,*) case ,$o, in *,nosuid,*nodev,*|*,nodev,*nosuid,*) ok "/postinstall mount options $o";; *) bad "/postinstall not nosuid,nodev: $o";; esac;; *) bad "/postinstall not ro: $o";; esac
  o=$(awk '$5 == "/new/postinstall/gbos/logs/boot-7" {print $6}' /proc/self/mountinfo)
  case ,$o, in *,rw,*) case ,$o, in *,noexec,*) ok "logs/boot-7 mount options $o";; *) bad "logs/boot-7 not noexec: $o";; esac;; *) bad "logs/boot-7 not rw: $o";; esac
  # an app (10123) and system (1000: gbos_fp, soft_gk, system_server)
  for u in 10123 1000; do
    as $u "echo pwn > $P/gbos.cfg" && bad "uid $u can overwrite gbos.cfg" || ok "uid $u cannot overwrite gbos.cfg"
    as $u "echo pwn > $P/hook.sh" && bad "uid $u can create hook.sh" || ok "uid $u cannot create hook.sh"
    as $u "echo pwn > $P/run.sh" && bad "uid $u can create run.sh" || ok "uid $u cannot create run.sh"
    as $u "mkdir $P/x" && bad "uid $u can mkdir in gbos/" || ok "uid $u cannot mkdir in gbos/"
    as $u "rm -f $P/gbos_fp; test ! -e $P/gbos_fp" && bad "uid $u can delete gbos_fp" || ok "uid $u cannot delete gbos_fp"
    as $u "ls $P/logs" && bad "uid $u can list logs/" || ok "uid $u cannot list logs/"
    as $u "echo pwn > $P/logs/boot-7/run.sh" && bad "uid $u can write logs/boot-7" || ok "uid $u cannot write logs/boot-7"
    as $u "cat $P/logs/boot-6/logcat.txt" && bad "uid $u can read an older boot's logcat" || ok "uid $u cannot read older boots' logs"
    as $u "$P/gbos_fp | grep -q fp-ok" && ok "uid $u can still exec gbos/gbos_fp (gbos_fp, soft_gk run as system)" || bad "uid $u cannot exec gbos_fp"
  done
  [ "$FPLOG" = /dev/gbos-system-log/fingerprint.txt ] && ok "gbos_fp gets --log $FPLOG" || bad "gbos_system_log printed [$FPLOG]: $(cat $W/hook.log)"
  F=/new$FPLOG
  as 1000 "echo fp-line >> $F" && [ "$(cat $L/fingerprint.txt)" = fp-line ] && ok "uid 1000 appends fingerprint.txt after the /dev move; it lands in logs/boot-7" || bad "uid 1000 cannot append fingerprint.txt"
  # gbos_fp.c's own open: O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW, 0644, as uid 1000 gid 1000. (No sed -i here: replacing the
  # bound file leaves its old inode unlinked but mounted, and then the shutdown superblock remount read-only fails with EBUSY.)
  FPWANT=fp-line
  if command -v python3 >/dev/null; then
    as 1000 "python3 -c 'import os; fd = os.open(\"$F\", os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_CLOEXEC | os.O_NOFOLLOW, 0o644); os.write(fd, b\"fp-open\\n\")'" \
      && [ "$(tail -n 1 $L/fingerprint.txt)" = fp-open ] && FPWANT="fp-line fp-open" \
      && ok "uid 1000 opens it with gbos_fp's flags (O_CREAT|O_APPEND|O_NOFOLLOW)" || bad "gbos_fp's open() flags fail on /dev/gbos-system-log/fingerprint.txt"
  fi
  as 1000 "echo x > /new/dev/gbos-system-log/other" && bad "uid 1000 can create other files in /dev/gbos-system-log" || ok "uid 1000 cannot create other files there (mode 0500)"
  as 1000 "rm -f $F; test ! -e $F" && bad "uid 1000 can remove the log file" || ok "uid 1000 cannot remove it"
  as 10123 "echo pwn >> $F" && bad "an app can write fingerprint.txt" || ok "an app cannot write fingerprint.txt"
  as 10123 "ls /new/dev/gbos-system-log" && bad "an app can list /dev/gbos-system-log" || ok "an app cannot list /dev/gbos-system-log"
  [ "$(cat $L/fingerprint.txt | tr '\n' ' ')" = "$FPWANT " ] && ok "fingerprint.txt holds only the HAL's lines" || bad "fingerprint.txt: $(cat $L/fingerprint.txt)"
  grep -q '^MODE=ask$' $G/gbos.cfg && [ ! -e $G/hook.sh ] && [ ! -e $G/run.sh ] && [ ! -e $L/run.sh ] && ok "nothing changed on the volume" || bad "the volume changed"
  # root services
  echo line > $P/logs/boot-7/logcat.txt && [ "$(cat $L/logcat.txt)" = line ] && ok "root writes logs/boot-7, it lands on the volume" || bad "root cannot write logs/boot-7"
  cp $P/logs/boot-7/logcat.txt /w/copied && ok "root (init copy) reads logs/boot-7" || bad "root cannot read logs/boot-7"
  mkdir -p $P/logs/boot-7 && ok "mkdir -p of the existing log dir succeeds (native_zygote.sh, camera bridge)" || bad "mkdir -p logs/boot-7 fails"
  ( echo pwn > $P/hook.sh ) 2>/dev/null && bad "root writes gbos/ through /postinstall" || ok "root gets EROFS outside logs/boot-7 too"
  printf '#!/bin/sh\necho x\n' > $P/logs/boot-7/t.sh; $P/logs/boot-7/t.sh >/dev/null 2>&1 && bad "a file in logs/boot-7 can be exec'd" || ok "logs/boot-7 is noexec"
  [ "$(sh $P/logs/boot-7/t.sh)" = x ] && ok "sh logs/boot-7/run.sh still works (the RUN_SH_BACKCHANNEL path)" || bad "sh of a script in logs fails"
  rm -f $P/logs/boot-7/t.sh
  echo s0 > $G/stage0-side && ok "stage0's /gbos stays writable (kmsg copier, FAT watcher)" || bad "/gbos became read-only"
  # root's own writable view (toybox-style unshare: no propagation change)
  unshare --propagation unchanged -m /bin/sh -c "$BB mount -o remount,bind,rw /new/postinstall && echo root > $P/root-ns" \
    && [ "$(cat $G/root-ns 2>/dev/null)" = root ] && ok "root writes gbos/ in its own mount namespace" || bad "root cannot get write access in its own namespace"
  ( echo pwn > $P/after-ns ) 2>/dev/null && bad "the namespace remount leaked: /postinstall writable" || ok "after it, /postinstall is still read-only here"
  as 10123 "echo pwn > $P/after-ns" && bad "the namespace remount leaked to apps" || ok "and still read-only for apps"
  # the system updater (usb/sysupdate.sh), run exactly as init runs it: /postinstall/busybox sh .../sysupdate.sh, empty environment
  R=/new/dev/gbos-rw
  grep -q 'POSTINSTALL: the system updater writes the volume through /dev/gbos-rw/stick' $W/hook.log && ok "logged: updater writes through /dev/gbos-rw/stick" || bad "gbos_root_rw log: $(cat $W/hook.log)"
  [ "$(stat -c '%a %u' $R)" = "700 0" ] && ok "/dev/gbos-rw is 0700 root after the /dev move" || bad "/dev/gbos-rw is $(stat -c '%a %u' $R 2>&1)"
  o=$(awk -v m=$R/stick '$5 == m {print $6}' /proc/self/mountinfo)
  case ,$o, in *,rw,*nosuid,*nodev,*noexec,*|*,rw,*noexec,*) ok "/dev/gbos-rw/stick mount options $o";; *) bad "/dev/gbos-rw/stick options: [$o]";; esac
  for u in 10123 1000; do
    as $u "ls $R/stick" && bad "uid $u can list /dev/gbos-rw/stick" || ok "uid $u cannot reach /dev/gbos-rw/stick"
    as $u "echo pwn > $R/stick/gbos/hook.sh" && bad "uid $u can write through /dev/gbos-rw" || ok "uid $u cannot write through /dev/gbos-rw"
  done
  cp $BB /gbos/busybox && cp /repo/usb/sysupdate.sh $G/sysupdate.sh
  SUE="env -i PATH=/product/bin:/system/bin SU_PI=/new/postinstall SU_RW_DIR=$R"
  mkdir -p $G/update.inbox/gbos86-update-202611220000-aaaaaaaaaaaa
  $SUE /new/postinstall/busybox sh $P/sysupdate.sh cancel > $W/su1 2>&1 && grep -q 'removed 1 waiting' $W/su1 && [ ! -e $G/update.inbox/gbos86-update-202611220000-aaaaaaaaaaaa ] \
    && ok "sysupdate cancel (as init runs it) writes through /dev/gbos-rw/stick" || bad "sysupdate cancel: $(cat $W/su1)"
  grep -q 'cancel: writing through' $L/sysupdate.txt && grep -q 'GBOS_STICK_RW= /new/postinstall: .* /new/postinstall ro,' $L/sysupdate.txt \
    && ok "it logged its choice and the mount lines to logs/boot-7/sysupdate.txt" || bad "sysupdate.txt: $(cat $L/sysupdate.txt 2>&1)"
  mkdir -p $G/update && printf 'set=abc\nphase=trying\n' > $G/update/state
  $SUE SYSUPDATE_TEST=1 /new/postinstall/busybox sh $P/sysupdate.sh boot-ok > $W/su2 2>&1 && [ ! -e $G/update/state ] && grep -q 'committed' $G/update/status.txt \
    && ok "sysupdate boot-ok commits the tried set through /dev/gbos-rw/stick (one restart)" || bad "boot-ok: $(cat $W/su2)"
  rm -f $G/update/boot-ok $G/update/state
  # an older hook.sh (no /dev/gbos-rw): its own mount namespace, with the busybox unshare stage0 ships
  $BB umount $R/stick && mkdir -p $G/update.inbox/gbos86-update-202611220000-bbbbbbbbbbbb
  $SUE /new/postinstall/busybox sh $P/sysupdate.sh cancel > $W/su3 2>&1 && grep -q 'removed 1 waiting' $W/su3 && grep -q 're-running in a mount namespace' $W/su3 \
    && ok "without /dev/gbos-rw: cancel re-runs in its own namespace and writes" || bad "namespace fallback: $(cat $W/su3)"
  ( echo pwn > $P/after-su ) 2>/dev/null && bad "the updater's namespace leaked: /postinstall writable" || ok "after it, /postinstall is still read-only here"
  as 10123 "echo pwn > $P/after-su" && bad "the updater's namespace leaked to apps" || ok "and for apps"
  # the FAT itself read-only (the kernel does this after a FAT error): a clear refusal, nothing half-done
  mkdir -p $G/update.inbox/gbos86-update-202611220000-cccccccccccc
  $BB mount -t vfat -o remount,ro $DEV /gbos
  $SUE /new/postinstall/busybox sh $P/sysupdate.sh cancel > $W/su4 2>&1; r4=$?
  [ $r4 != 0 ] && grep -q 'FAT is read-only for everyone' $W/su4 && ok "FAT read-only: cancel refuses and says why" || bad "FAT read-only: rc $r4 $(cat $W/su4)"
  $BB mount -t vfat -o remount,rw $DEV /gbos && rm -rf $G/update.inbox $L/sysupdate.txt $G/sysupdate.sh /gbos/busybox
  # stick_fat.sh's superblock remount at shutdown (no writer open)
  $BB mount -t vfat -o remount,ro $DEV /gbos && [ "$(awk '$5 == "/gbos" { for (i = 7; $i != "-"; i++); split($(i + 3), o, ","); print o[1] }' /proc/self/mountinfo)" = ro ] \
    && ok "stick_fat.sh superblock remount read-only still works with these mounts" || bad "superblock remount ro fails"
  echo "$n checks, $fail failed (kernel $(uname -r))"
  exit $fail
fi

# ---------------------------------------------------------------- 1. host part
W=$(mktemp -d "${TMPDIR:-/tmp}/pi-ro.XXXXXX"); trap 'rm -rf "$W"' EXIT
sh -n "$H" "$ROOT/usb/audio/audio-profile.sh" && ok "sh -n hook.sh audio-profile.sh" || bad "syntax"
fn gbos_postinstall_lockdown "$H" > "$W/fn.sh"
[ -s "$W/fn.sh" ] && ok "gbos_postinstall_lockdown extracted ($(wc -l < "$W/fn.sh") lines)" || { bad "gbos_postinstall_lockdown not in hook.sh"; exit 1; }
fn gbos_root_rw "$H" | grep -q 'mount --bind "${GBOS_VOL:-/gbos}" "$_rd/stick"' && fn gbos_root_rw "$H" | grep -q 'chmod 0700 "$_rd"' \
  && ok "gbos_root_rw binds the volume at gbos-rw/stick, below the 0700 directory (never on it)" || bad "gbos_root_rw: not the expected bind below a 0700 directory"

# FAILON = a substring of the one call that fails ("remount,bind,ro", "tmpfs", "mkdir", "bind LOGDIR PI/gbos/logs/boot-7", ...)
run() { # FAILON [VAR=value]
  : > "$W/calls"; : > "$W/log"
  ( FAILON=$1; shift; [ $# -gt 0 ] && export "$@"
    mount() { echo "mount $*" >> "$W/calls"; case "mount $*" in *"$FAILON"*) [ -n "$FAILON" ] && return 1;; esac; return 0; }
    umount() { echo "umount $*" >> "$W/calls"; }
    mkdir() { echo "mkdir $*" >> "$W/calls"; case "mkdir $*" in *"$FAILON"*) [ -n "$FAILON" ] && return 1;; esac; return 0; }
    log() { echo "$*" >> "$W/log"; }
    gbos_root_rw() { echo "gbos_root_rw" >> "$W/calls"; }   # real mounts: kernel part
    . "$W/fn.sh"
    gbos_postinstall_lockdown PI LOGDIR 7; echo "rc=$? private=$GBOS_LOGS_PRIVATE" > "$W/result" )
}
exp() { printf '%s\n' "$@" > "$W/exp"; cmp -s "$W/exp" "$W/calls"; }

run ""
exp "mount -o remount,bind,ro,nosuid,nodev PI" "gbos_root_rw" "mount -t tmpfs -o mode=0700,size=64k,nosuid,nodev,noexec gbos-logs PI/gbos/logs" \
    "mkdir -m 0700 PI/gbos/logs/boot-7" "mount --bind LOGDIR PI/gbos/logs/boot-7" "mount -o remount,bind,nosuid,nodev,noexec PI/gbos/logs/boot-7" \
  && ok "default: ro remount, root's /dev/gbos-rw, 0700 tmpfs over logs/, rw bind of this boot's dir (noexec)" || bad "default call sequence: $(cat "$W/calls")"
grep -qx 'rc=0 private=1' "$W/result" && ! grep -q POSTINSTALL-RW "$W/log" && ok "default: rc 0, logs private, no POSTINSTALL-RW" || bad "default: $(cat "$W/result") $(cat "$W/log")"

run "" POSTINSTALL_RW=1
[ ! -s "$W/calls" ] && grep -qx 'rc=0 private=0' "$W/result" && grep -q 'POSTINSTALL-RW.*POSTINSTALL_RW=1' "$W/log" \
  && ok "POSTINSTALL_RW=1: nothing remounted, logged as POSTINSTALL-RW, logs not private" || bad "POSTINSTALL_RW=1: $(cat "$W/calls" "$W/result")"
for v in 0 yes " 1"; do
  run "" "POSTINSTALL_RW=$v"; grep -qx 'rc=0 private=1' "$W/result" && ok "POSTINSTALL_RW=[$v] is not 1: locked down" || bad "POSTINSTALL_RW=[$v]: $(cat "$W/result")"
done

run "remount,bind,ro"
grep -qx 'rc=1 private=0' "$W/result" && [ "$(wc -l < "$W/calls")" = 1 ] && grep -q 'POSTINSTALL-RW.*FAILED' "$W/log" \
  && ok "ro remount fails: rc 1, logged POSTINSTALL-RW, nothing else mounted" || bad "ro remount failure: $(cat "$W/calls" "$W/result" "$W/log")"
for f in "tmpfs" "mkdir -m 0700" "--bind LOGDIR PI/gbos/logs/boot-7"; do
  run "$f"
  case $f in --bind*) want=1;; *) want=0;; esac
  if [ $want = 1 ]; then  # the gateway bind and the fallback bind are the same call: both fail, logs stay read-only
    grep -qx 'rc=1 private=0' "$W/result" && grep -q 'bind FAILED: no logcat' "$W/log" && grep -qx 'umount PI/gbos/logs' "$W/calls" \
      && ok "log dir bind fails: gateway removed, rc 1, logged (no logs, nothing writable)" || bad "bind failure: $(cat "$W/calls" "$W/result" "$W/log")"
  else
    grep -qx 'rc=0 private=0' "$W/result" && grep -q 'POSTINSTALL-RW.*gateway FAILED' "$W/log" && tail -2 "$W/calls" | head -1 | grep -qx 'mount --bind LOGDIR PI/gbos/logs/boot-7' \
      && ok "[$f] fails: falls back to a direct bind of logs/boot-7, logs not private, logged POSTINSTALL-RW" || bad "[$f] failure: $(cat "$W/calls" "$W/result" "$W/log")"
    { [ "$f" = tmpfs ] || grep -qx 'umount PI/gbos/logs' "$W/calls"; } && ok "[$f] fails: the tmpfs is not left over the log dirs" || bad "[$f]: tmpfs left mounted"
  fi
done
run "remount,bind,nosuid,nodev,noexec"
grep -qx 'rc=0 private=1' "$W/result" && grep -q 'stays exec' "$W/log" && ok "noexec remount fails: still private, logged" || bad "noexec failure: $(cat "$W/result" "$W/log")"

# call site: right after the bind, before anything reads GBOS_LOGS_PRIVATE or writes gbos.rc
b=$(grep -n '^if mount --bind /gbos /new/postinstall; then$' "$H" | cut -d: -f1)
c=$(grep -n '^  gbos_postinstall_lockdown /new/postinstall "\$L" "\$N"$' "$H" | cut -d: -f1)
g=$(grep -n '^  GBOS_RUN_SH=$' "$H" | head -1 | cut -d: -f1)
[ -n "$b" ] && [ "$c" = $((b + 1)) ] && [ -n "$g" ] && [ "$g" -gt "$c" ] && ok "called right after the /postinstall bind (line $c), before the run.sh gate" || bad "call site: bind=$b call=$c gate=$g"
[ "$(grep -c 'mount --bind /gbos /new/postinstall' "$H")" = 1 ] && ok "the only /postinstall bind is the locked-down one" || bad "another /postinstall bind in hook.sh"

# run.sh gate: only with private logs, and the path is under logs/boot-N
awk '/^  # Debug backchannel/{p=1} /^  cat > \/tmp\/sysinit\/gbos.rc <<RC$/{exit} p{print}' "$H" > "$W/gate.sh"
gate() { ( G=$W/g L=$W/g/logs/boot-7 N=7; export "$@"; log() { echo "$*" >> "$W/glog"; }; : > "$W/glog"; . "$W/gate.sh"; printf '%s' "$GBOS_RUN_SH" > "$W/grun" ); }
mkdir -p "$W/g/logs/boot-7"
gate RUN_SH_BACKCHANNEL=1 GBOS_LOGS_PRIVATE=0; [ ! -s "$W/grun" ] && grep -q 'ignored: logs/boot-7 is not root-only' "$W/glog" && ok "run.sh: off when logs are not private" || bad "run.sh with non-private logs: $(cat "$W/grun")"
echo 'echo hi' > "$W/g/run.sh"
gate RUN_SH_BACKCHANNEL=1 GBOS_LOGS_PRIVATE=1
grep -q '/postinstall/gbos/logs/boot-7/run.sh' "$W/grun" && ! grep -q '/postinstall/gbos/run\.' "$W/grun" && ok "run.sh: on, path logs/boot-7/run.sh" || bad "run.sh path: $(cat "$W/grun")"
[ ! -e "$W/g/run.sh" ] && [ -f "$W/g/logs/boot-7/run.sh" ] && ok "run.sh: gbos/run.sh moved into logs/boot-7 at hook time" || bad "run.sh not moved"

# everything the generated gbos.rc writes on the volume is under logs/boot-$N (shell redirections and logcat -f)
grep -oE '(>>?|-f) */postinstall/[^ ";]*' "$H" | sed -E 's/^(>>?|-f) *//' | sort -u > "$W/writes"
grep -v '^/postinstall/gbos/logs/boot-\$N/' "$W/writes" > "$W/badw"
[ -s "$W/writes" ] && [ ! -s "$W/badw" ] && ok "hook.sh's redirections and logcat -f on the volume ($(wc -l < "$W/writes") paths) are all under logs/boot-\$N" || bad "writes outside logs/boot-\$N: $(cat "$W/badw")"
# output files passed as arguments (sensordiag, tablet_mode, camera capture...) too: every logs path hook.sh hands Android is this boot's
grep -v '^[[:space:]]*#' "$H" | grep -oE '/postinstall/gbos/logs[^ ";]*' | grep -v '^/postinstall/gbos/logs/boot-\$N/' > "$W/bada"
[ ! -s "$W/bada" ] && ok "every /postinstall/gbos/logs path in hook.sh is under logs/boot-\$N (the only one Android can reach)" || bad "other log paths: $(sort -u "$W/bada")"
# ... and every service that writes there runs as root (the 0700 gateway keeps everyone else out)
awk '/^service /{s=$0; w=0; u=""} /^service .*\/postinstall\/gbos\/logs/{w=1} /^    user /{u=$2} /^$/{if (w && u != "root") print s; w=0}' "$H" > "$W/nonroot"
[ ! -s "$W/nonroot" ] && ok "every gbos.rc service writing logs runs as user root" || bad "non-root service writes logs: $(cat "$W/nonroot")"
# the one non-root writer, gbos_fp (user system), gets its log file through gbos_system_log (/dev/gbos-system-log)
grep -q '^        FPLOG=$(gbos_system_log fingerprint.txt)$' "$H" && grep -q '^service gbos_fp /postinstall/gbos/gbos_fp$FPARGS --log $FPLOG$' "$H" \
  && ok "gbos_fp's --log comes from gbos_system_log" || bad "gbos_fp --log is not from gbos_system_log"
# the Android-side scripts' log variables point into logs/boot-N
for d in 'ksu/install.sh:^LOG=\$G/logs/boot-\$N/' 'surface/iptsd-run.sh:^L=\$G/logs/boot-\$N$' 'audio/mixdiag.sh:^L=\${GBOS_AUDIO_DIAG_LOG_ROOT:-\$G/logs/boot-\$N}$' \
         'aidiag.sh:^L=/postinstall/gbos/logs/boot-\$N$' 'native_zygote.sh:^L=\${GBOS_NZ_LOGDIR:-/postinstall/gbos/logs/boot-\$N}$' \
         'installer/bin/ipu6-camera-bridge.sh:^LOGDIR=/postinstall/gbos/logs/boot-\$BOOT_ID$'; do
  grep -qE "${d#*:}" "$ROOT/usb/${d%%:*}" && ok "${d%%:*} logs into logs/boot-N" || bad "${d%%:*}: log path changed (${d#*:})"
done
# audio-profile.sh is the one root tool that writes the stick outside the logs: it must use a private namespace
A=$ROOT/usb/audio/audio-profile.sh
grep -q 'unshare -m /system/bin/sh -c .mount -o remount,bind,rw /postinstall' "$A" && ok "audio-profile.sh writes the stick in its own mount namespace" || bad "audio-profile.sh has no namespace wrapper"
grep -q '^POSTINSTALL_RW=' "$ROOT/usb/gbos.cfg" && bad "usb/gbos.cfg sets POSTINSTALL_RW" || ok "usb/gbos.cfg leaves POSTINSTALL_RW off"

# ---------------------------------------------------------------- 2. kernel part (launch)
BB=${BUSYBOX:-$ROOT/kernel/out/busybox}
[ -x "$BB" ] || BB=$(cd "$ROOT" && cd "$(git rev-parse --git-common-dir 2>/dev/null)/.." 2>/dev/null && pwd)/kernel/out/busybox
if [ "${SKIP_KERNEL:-0}" = 1 ]; then echo "skip kernel part (SKIP_KERNEL=1)"
elif ! [ -x "$BB" ] || ! docker image inspect gbos-kbuild >/dev/null 2>&1; then echo "skip kernel part (no stage0 busybox at $BB, or no docker image gbos-kbuild)"
else
  docker run --rm --privileged -v "$ROOT:/repo:ro" -v "$BB:/opt/gbos-bb/busybox:ro" gbos-kbuild sh /repo/tools/test_postinstall_ro.sh --inner > "$W/kernel.txt" 2>&1; krc=$?
  cat "$W/kernel.txt"
  kn=$(sed -n 's/^\([0-9]*\) checks, \([0-9]*\) failed.*/\1 \2/p' "$W/kernel.txt")
  if [ $krc = 0 ] && [ -n "$kn" ]; then n=$((n + ${kn% *})); else n=$((n+1)); fail=$((fail + ${kn#* } + 1)); echo "FAIL kernel part (rc $krc)"; fi
fi

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo POSTINSTALL_RO_TEST_PASS
exit $fail
