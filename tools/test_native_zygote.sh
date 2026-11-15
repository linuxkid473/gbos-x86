#!/bin/sh
# test_native_zygote.sh: runs usb/native_zygote.sh against stub device_config / setprop / pidof / getprop and a fake /proc
# (no Android, no root): the flag is forced to false and re-applied, a not-ready device_config is retried without log spam,
# every zygote_next pid is dumped once, and hook.sh wires the service ("on boot", oneshot, seclabel) and the deploy lists.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$HERE/..
S=$ROOT/usb/native_zygote.sh; H=$ROOT/usb/hook.sh
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

sh -n "$S" && sh -n "$H" && ok "sh -n native_zygote.sh hook.sh" || bad "syntax"

mkdir -p $T/bin $T/proc/4242/fd $T/proc/4242/fdinfo $T/proc/4242/attr $T/proc/1/fd $T/log
cat > $T/bin/device_config <<'EOF'
#!/bin/sh
# state in $T/dc ("null" when never set); $T/notready makes every call fail, as before system_server's service is up
[ -e "$FAKE_T/notready" ] && exit 1
case $1 in
  get) [ -f "$FAKE_T/dc" ] && cat "$FAKE_T/dc" || echo null;;
  put) echo "$4" > "$FAKE_T/dc"; echo "$2/$3 $4" >> "$FAKE_T/dc.puts";;
esac
EOF
printf '#!/bin/sh\necho "$@" >> "$FAKE_T/props"\n' > $T/bin/setprop
printf '#!/bin/sh\n[ -f "$FAKE_T/pids" ] && cat "$FAKE_T/pids"\n' > $T/bin/pidof
printf '#!/bin/sh\necho "[zygote.zygote_next.server_ready]: [false]"\n' > $T/bin/getprop
chmod +x $T/bin/*
echo "zygote_next --name zygote_next --species x" | tr ' ' '\0' > $T/proc/4242/cmdline
echo "u:r:zygote_next:s0" > $T/proc/4242/attr/current
printf 'Name:\tzygote_next\nPPid:\t1\nUid:\t0\t0\t0\t0\nFDSize:\t64\n' > $T/proc/4242/status
ln -s /dev/null $T/proc/4242/fd/3; printf 'pos:\t0\nflags:\t0100002\n' > $T/proc/4242/fdinfo/3
ln -s /dev/null $T/proc/1/fd/0
export FAKE_T=$T PATH=$T/bin:$PATH GBOS_NZ_LOGDIR=$T/log GBOS_NZ_PROC=$T/proc GBOS_NZ_SLEEP=0
LOG=$T/log/native-zygote.txt

# 1. device_config not ready for the first ticks: no crash, failure logged at most 5 times, flag still ends false after it is ready
touch $T/notready
GBOS_NZ_TICKS=20 sh $S 1 >/dev/null 2>&1
[ "$(grep -c 'failed' $LOG)" -le 5 ] && [ "$(grep -c 'failed' $LOG)" -ge 1 ] && ok "not-ready device_config: $(grep -c 'failed' $LOG) failure lines (max 5)" || bad "failure log count"
rm -f $T/notready; : > $LOG
GBOS_NZ_TICKS=2 sh $S 1 >/dev/null 2>&1
[ "$(cat $T/dc)" = false ] && ok "flag set to false once device_config answers" || bad "flag not false: $(cat $T/dc 2>&1)"
grep -q "android_os/native_framework_prototype false" $T/dc.puts && ok "namespace/flag name android_os native_framework_prototype" || bad "wrong namespace or flag: $(cat $T/dc.puts)"
grep -q "persist.zygote.zygote_next.start_on_boot false" $T/props && ok "start_on_boot property set false" || bad "start_on_boot not set"

# 2. a server that put the flag back to true is corrected on the next minute check (12 ticks)
echo true > $T/dc; rm -f $T/dc.puts; : > $LOG
GBOS_NZ_TICKS=13 sh $S 1 >/dev/null 2>&1
[ "$(cat $T/dc)" = false ] && grep -q "was 'true', set to false" $LOG && ok "flag put back to true is corrected" || bad "not corrected: $(cat $T/dc) / $(cat $LOG)"
[ "$(wc -l < $T/dc.puts | tr -d ' ')" = 1 ] && ok "already-false flag is not rewritten every tick" || bad "rewritten: $(cat $T/dc.puts)"

# 3. evidence: one dump per pid, with the fd table, fdinfo, context and init's fds; a restart (new pid) is dumped too
: > $LOG; echo 4242 > $T/pids
GBOS_NZ_TICKS=6 sh $S 1 >/dev/null 2>&1
[ "$(grep -c '== zygote_next pid 4242' $LOG)" = 1 ] && ok "pid 4242 dumped exactly once over 6 ticks" || bad "dump count: $(grep -c '== zygote_next pid 4242' $LOG)"
for want in "^-- ls -l .*4242/fd" "fd 3:" "flags:" "u:r:zygote_next:s0" "FDSize:" "ls -l /proc/1/fd" "zygote.zygote_next.server_ready"; do
  grep -q -e "$want" $LOG && ok "dump has: $want" || bad "dump lacks: $want"
done
mkdir -p $T/proc/4243/fd $T/proc/4243/fdinfo $T/proc/4243/attr; printf '4242\n4243\n' > $T/pids; : > $T/proc/4243/cmdline; : > $T/proc/4243/status
GBOS_NZ_TICKS=3 sh $S 1 >/dev/null 2>&1 </dev/null
grep -q '== zygote_next pid 4243' $LOG && ok "a restarted zygote_next (new pid) is dumped" || bad "new pid not dumped"

# 4. never more than 8 dumps a boot (a crash loop must not fill the stick)
: > $LOG; rm -f $T/pids
for p in 5001 5002 5003 5004 5005 5006 5007 5008 5009 5010 5011 5012; do mkdir -p $T/proc/$p/fd $T/proc/$p/fdinfo $T/proc/$p/attr; : > $T/proc/$p/cmdline; : > $T/proc/$p/status; done
echo 5001 5002 5003 5004 5005 5006 5007 5008 5009 5010 5011 5012 > $T/pids
GBOS_NZ_TICKS=3 sh $S 1 >/dev/null 2>&1
n=$(grep -c '== zygote_next pid' $LOG); [ "$n" -le 8 ] && ok "dump cap: $n dumps for 12 pids" || bad "dump cap broken: $n"

# 5. hook.sh wiring
grep -q '^service gbos_nativezygote /system/bin/sh /postinstall/gbos/native_zygote.sh \$N' $H && ok "hook.sh defines gbos_nativezygote with the boot number" || bad "service missing"
awk '/^service gbos_nativezygote/{p=1;next} p&&/^$/{exit} p' $H > $T/svc
for w in "seclabel u:r:shell:s0" "oneshot" "disabled" "user root"; do grep -q "$w" $T/svc && ok "service has: $w" || bad "service lacks: $w"; done
awk '/^on boot$/{p=1;next} /^on /{p=0} p' $H | grep -q "start gbos_nativezygote" && ok "started from 'on boot'" || bad "not started from on boot"
for f in tools/deploy_stick.sh tools/make_gbos86_image.sh tools/install_dualboot.sh usb/install.sh; do
  grep -q 'native_zygote\.sh' $ROOT/$f && ok "$f ships native_zygote.sh" || bad "$f does not ship native_zygote.sh"
done

[ $fail = 0 ] && echo NATIVE_ZYGOTE_PASS || { echo NATIVE_ZYGOTE_FAIL; exit 1; }
