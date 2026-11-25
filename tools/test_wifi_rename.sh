#!/bin/sh
# Test of gbos_rename_wifi (usb/hwdetect.sh) against a fake /sys/class/net and a fake ip (nothing touches the machine):
#   - Marvell mwifiex's mlan0 (Surface Pro 5) is renamed to wlan0, and the rename is logged
#   - nothing happens when wlan0 already exists (iwlwifi, rtw89, ...) or when there is no mlan interface
#   - a failing rename is logged as FAILED
#   - gbos_wait_devices calls it, and it does not touch hook.sh's N (boot number)
# Run: sh tools/test_wifi_rename.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
H=$ROOT/usb/hwdetect.sh
W=$(mktemp -d "${TMPDIR:-/tmp}/wifitest.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }

# only the function itself, so sourcing runs nothing else of hwdetect.sh
sed -n '/^gbos_rename_wifi() {/,/^}/p' "$H" > $W/fn.sh
[ -s $W/fn.sh ] && ok "gbos_rename_wifi found in hwdetect.sh" || { bad "gbos_rename_wifi missing"; exit 1; }

case1() { # case1 IP_RC IFACES...: runs the function on a fake net dir, prints the log and the ip calls
  rc=$1; shift
  rm -rf $W/net $W/ip.log $W/log; mkdir -p $W/net
  for i in "$@"; do mkdir $W/net/$i; done
  ( N=7
    log() { echo "$*" >> $W/log; }
    ip() { echo "ip $*" >> $W/ip.log; case "$*" in *" name "*) return $rc;; esac; return 0; }
    GBOS_NET=$W/net; . $W/fn.sh; gbos_rename_wifi
    echo "N=$N" >> $W/log )
}

case1 0 lo mlan0
grep -q 'set dev mlan0 name wlan0' $W/ip.log 2>/dev/null && ok "mlan0 is renamed to wlan0" || bad "no rename: $(cat $W/ip.log 2>&1)"
grep -q 'set dev mlan0 down' $W/ip.log && ok "set down before the rename" || bad "not set down: $(cat $W/ip.log)"
grep -q 'mlan0 renamed to wlan0' $W/log && ok "rename logged" || bad "not logged: $(cat $W/log)"
grep -q '^N=7$' $W/log && ok "hook.sh's N is left alone" || bad "N changed: $(cat $W/log)"

case1 0 lo wlan0
[ ! -e $W/ip.log ] && ok "wlan0 already there: nothing renamed" || bad "renamed although wlan0 exists: $(cat $W/ip.log)"
case1 0 lo eth0
[ ! -e $W/ip.log ] && ok "no mlan interface: nothing renamed" || bad "renamed without mlan: $(cat $W/ip.log)"
case1 1 mlan0
grep -q 'renaming Wi-Fi interface mlan0 to wlan0 FAILED' $W/log && ok "failed rename logged as FAILED" || bad "failure not logged: $(cat $W/log)"

awk '/^gbos_wait_devices\(\) \{/{f=1} f&&/gbos_rename_wifi/{print "yes"; exit} f&&/^}/{exit}' "$H" | grep -q yes \
  && ok "gbos_wait_devices calls gbos_rename_wifi" || bad "gbos_wait_devices does not call it"
sh -n "$H" && ok "sh -n hwdetect.sh" || bad "syntax"

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo WIFI_RENAME_TEST_PASS
exit $fail
