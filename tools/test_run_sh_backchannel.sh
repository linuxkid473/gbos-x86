#!/bin/sh
# Test of the run.sh root backchannel in usb/hook.sh's gbos_sync service: it must be OFF unless gbos.cfg sets RUN_SH_BACKCHANNEL=1.
# The gate block and the gbos_sync service line are cut out of hook.sh and run as hook.sh runs them (heredoc expansion at hook time),
# so what is tested is the shipped code: the generated .rc line, and the generated loop body run once against a fake /postinstall.
# Since 2026-11-19 run.sh lives in logs/boot-N (root-only, tools/test_postinstall_ro.sh) and runs only while GBOS_LOGS_PRIVATE=1.
# Run: sh tools/test_run_sh_backchannel.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
H=$ROOT/usb/hook.sh
W=$(mktemp -d "${TMPDIR:-/tmp}/runsh.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }

sh -n "$H" && ok "sh -n hook.sh" || bad "syntax"

# the gate: from the '# Debug backchannel' comment up to the gbos.rc heredoc; the service line: the one 'service gbos_sync' line
awk '/^  # Debug backchannel/{p=1} /^  cat > \/tmp\/sysinit\/gbos.rc <<RC$/{exit} p{print}' "$H" > "$W/gate.sh"
grep '^service gbos_sync ' "$H" > "$W/svc.line"
[ -s "$W/gate.sh" ] && grep -q RUN_SH_BACKCHANNEL "$W/gate.sh" && ok "gate block extracted ($(wc -l < "$W/gate.sh") lines)" || { bad "gate block not found in hook.sh"; exit 1; }
[ "$(wc -l < "$W/svc.line")" = 1 ] && ok "exactly one gbos_sync service line" || { bad "gbos_sync service line missing or repeated"; exit 1; }
{ echo 'log() { echo "$*" >> "$W/hook.log"; }'; cat "$W/gate.sh"; echo 'cat > "$W/gbos.rc" <<RC'; cat "$W/svc.line"; echo 'RC'; } > "$W/gen.sh"

# every mention of gbos/run.sh in hook.sh must be inside the gate (nothing else runs it)
gs=$(grep -n '^  # Debug backchannel' "$H" | cut -d: -f1); ge=$((gs + $(wc -l < "$W/gate.sh") - 1))
out=$(grep -n 'gbos/run\.sh' "$H" | cut -d: -f1 | while read -r l; do [ "$l" -ge "$gs" ] && [ "$l" -le "$ge" ] || echo "$l"; done)
[ -z "$out" ] && ok "gbos/run.sh appears only inside the gate (lines $gs-$ge)" || bad "gbos/run.sh referenced outside the gate: line(s) $out"

gen() { # VALUE|unset [G] [GBOS_LOGS_PRIVATE]
  rm -f "$W/gbos.rc" "$W/hook.log"; : > "$W/hook.log"
  _g=${2:-$W/g}; mkdir -p "$_g/logs/boot-7"
  if [ "$1" = unset ]; then (unset RUN_SH_BACKCHANNEL; W=$W G=$_g N=7 L=$_g/logs/boot-7 GBOS_LOGS_PRIVATE=${3:-1} . "$W/gen.sh")
  else (RUN_SH_BACKCHANNEL=$1 W=$W G=$_g N=7 L=$_g/logs/boot-7 GBOS_LOGS_PRIVATE=${3:-1} . "$W/gen.sh"); fi
}
# run the generated loop body once, with /postinstall pointed at $W/pi and the loop/sleep removed
run_once() {
  rm -rf "$W/pi"; mkdir -p "$W/pi/gbos/logs/boot-7"; printf 'echo pwned > %s/pi/gbos/proof\necho ran\n' "$W" > "$W/pi/gbos/logs/boot-7/run.sh"
  cp "$W/pi/gbos/logs/boot-7/run.sh" "$W/pi/gbos/run.sh"
  body=$(sed -n 's#^service gbos_sync /system/bin/sh -c "\(.*\)"$#\1#p' "$W/gbos.rc" | sed -e 's/^while true; do sleep 5; //' -e 's/; done$//' -e "s#/postinstall#$W/pi#g" -e 's/sync$/:/')
  [ -n "$body" ] || return 2
  sh -c "$body"
}
OFF='service gbos_sync /system/bin/sh -c "while true; do sleep 5; sync; done"'

for v in unset "" 0 yes true 2 " 1"; do
  gen "$v"
  [ "$(cat "$W/gbos.rc")" = "$OFF" ] && ok "RUN_SH_BACKCHANNEL=[$v]: gbos_sync only syncs" || bad "RUN_SH_BACKCHANNEL=[$v]: gbos_sync line is: $(cat "$W/gbos.rc")"
  run_once; [ ! -e "$W/pi/gbos/proof" ] && [ -e "$W/pi/gbos/logs/boot-7/run.sh" ] && ok "RUN_SH_BACKCHANNEL=[$v]: a dropped run.sh is not executed" || bad "RUN_SH_BACKCHANNEL=[$v]: run.sh was executed"
done

gen 1
grep -q 'gbos/logs/boot-7/run\.sh' "$W/gbos.rc" && ok "RUN_SH_BACKCHANNEL=1: gbos_sync runs logs/boot-7/run.sh" || bad "RUN_SH_BACKCHANNEL=1: run.sh not in gbos_sync"
grep -q 'sync; done"$' "$W/gbos.rc" && ok "RUN_SH_BACKCHANNEL=1: still syncs every loop" || bad "RUN_SH_BACKCHANNEL=1: sync missing"
grep -q 'RUN_SH_BACKCHANNEL=1' "$W/hook.log" && ok "RUN_SH_BACKCHANNEL=1 is logged" || bad "RUN_SH_BACKCHANNEL=1 not logged"
run_once; [ "$(cat "$W/pi/gbos/proof" 2>/dev/null)" = pwned ] && [ ! -e "$W/pi/gbos/logs/boot-7/run.sh" ] && [ "$(cat "$W/pi/gbos/logs/boot-7/run.log" 2>/dev/null)" = ran ] \
  && ok "RUN_SH_BACKCHANNEL=1: logs/boot-7/run.sh runs once, output in logs/boot-7/run.log, then deleted" || bad "RUN_SH_BACKCHANNEL=1: run.sh not run as expected"
[ -e "$W/pi/gbos/run.sh" ] && ok "RUN_SH_BACKCHANNEL=1: a run.sh at the volume's top level (app-reachable before) is not run" || bad "RUN_SH_BACKCHANNEL=1: top-level run.sh was consumed"
# with logs reachable by anyone but root (POSTINSTALL_RW=1, or the root-only gateway failed) it stays off
gen 1 "$W/g" 0
[ "$(cat "$W/gbos.rc")" = "$OFF" ] && grep -q 'RUN_SH_BACKCHANNEL=1 ignored' "$W/hook.log" && ok "RUN_SH_BACKCHANNEL=1 + logs not private: off, logged" || bad "RUN_SH_BACKCHANNEL=1 + logs not private: $(cat "$W/gbos.rc")"

# AGENTS.md trap: '$' in an init .rc line is a property reference. The generated line must have none, on or off.
for v in unset 1; do gen "$v"; grep -q '\$' "$W/gbos.rc" && bad "RUN_SH_BACKCHANNEL=[$v]: generated .rc line has a \$" || ok "RUN_SH_BACKCHANNEL=[$v]: no \$ in the generated .rc line"; done

# a leftover run.sh on the volume is reported (not run) when the flag is off
mkdir -p "$W/g"; : > "$W/g/run.sh"; gen unset "$W/g"
grep -q 'run.sh present but ignored' "$W/hook.log" && ok "flag off + run.sh present: logged as ignored" || bad "flag off + run.sh present: not logged"

# the service keeps its explicit seclabel (init refuses a service without a domain transition)
awk '/^service gbos_sync /{p=1; next} p && /^$/{exit} p' "$H" | grep -q '^    seclabel u:r:shell:s0$' && ok "gbos_sync keeps seclabel u:r:shell:s0" || bad "gbos_sync lost its seclabel"
grep -q '^    start gbos_sync$' "$H" && ok "gbos_sync is still started" || bad "gbos_sync never started"

# the shipped gbos.cfg leaves it off
grep -q '^RUN_SH_BACKCHANNEL=' "$ROOT/usb/gbos.cfg" && bad "usb/gbos.cfg enables RUN_SH_BACKCHANNEL" || ok "usb/gbos.cfg leaves RUN_SH_BACKCHANNEL off"

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo RUN_SH_BACKCHANNEL_TEST_PASS
exit $fail
