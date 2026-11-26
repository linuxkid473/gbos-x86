#!/bin/sh
# Shell unit test of HDA_PINS (gbos_hda_pins in usb/hwdetect.sh) on a fake /sys/class/sound tree: no root, no sysfs.
#   sh tools/test_hda_pins.sh
# Pulls the block out of hwdetect.sh between its comment and gbos_hwdetect, so what is tested is what boots.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
HW=$HERE/../usb/hwdetect.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/hdapins.XXXXXX"); trap 'rm -rf "$T"' EXIT
sed -n '/^# HDA_PINS in gbos.cfg/,/^# Whole thing;/p' "$HW" > "$T/block.sh"
grep -q '^gbos_hda_pins()' "$T/block.sh" || { echo "FAIL: gbos_hda_pins not found in hwdetect.sh"; exit 1; }
fails=0; n=0
log() { echo "$*" >> "$T/log"; }
sleep() { :; }
. "$T/block.sh"
# The fake sysfs records each write instead of storing it: "<codec> <attr> <value>" in $T/writes.
# REJECT=<codec>/<attr> makes that write fail, like the kernel's -EINVAL / -EBUSY.
gbos_hda_write() {
  _w=$(basename "$(dirname "$1")")/$(basename "$1")
  [ "$_w" = "${REJECT:-}" ] && return 1
  echo "${_w%/*} ${_w#*/} $2" >> "$T/writes"
  [ "${1##*/}" = user_pin_configs ] && echo "$2" >> "$1.data"
  return 0
}

mk() { # $1 = "hwC0D0:0x10ec0269 hwC0D2:0x8086280b" ; $2 = no -> codecs without user_pin_configs
  rm -rf "$T/sys" "$T/state" "$T/log" "$T/writes"; mkdir -p "$T/sys" "$T/state"; : > "$T/writes"; : > "$T/log"
  for c in $1; do
    mkdir -p "$T/sys/${c%%:*}"; echo "${c#*:}" > "$T/sys/${c%%:*}/vendor_id"
    [ "${2:-}" = no ] || : > "$T/sys/${c%%:*}/user_pin_configs"
  done
  GBOS_HDA_SYS=$T/sys; GBOS_HDA_STATE=$T/state
}
check() { # name expected-writes(| separated, "" = none)
  n=$((n+1)); got=$(tr '\n' '|' < "$T/writes"); got=${got%|}
  if [ "$got" = "$2" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     want: $2"; echo "     got:  $got"; sed 's/^/     log: /' "$T/log"; fails=$((fails+1)); fi
}
check_log() { # name pattern
  n=$((n+1)); if grep -q -- "$2" "$T/log"; then echo "ok   $1"; else echo "FAIL $1: no log line matching '$2'"; sed 's/^/     log: /' "$T/log"; fails=$((fails+1)); fi
}
RT=hwC0D0:0x10ec0269; HDMI=hwC0D2:0x8086280b

mk "$RT $HDMI"; HDA_PINS=; gbos_hda_pins
check "unset: nothing written" ""

mk "$RT $HDMI"; HDA_PINS="0x14=0x411111f0 0x1b=0x90170110"; gbos_hda_pins
check "two pins on the Realtek codec, HDMI codec skipped, then reconfig" \
  "hwC0D0 user_pin_configs 0x14 0x411111f0|hwC0D0 user_pin_configs 0x1b 0x90170110|hwC0D0 reconfig 1"
check_log "reconfig logged" "hwC0D0 (0x10ec0269) reconfigured with 2 pin(s)"

gbos_hda_pins
check "second hwdetect run (hook.sh) does not reconfigure again" \
  "hwC0D0 user_pin_configs 0x14 0x411111f0|hwC0D0 user_pin_configs 0x1b 0x90170110|hwC0D0 reconfig 1"

mk "$RT $HDMI"; HDA_PINS="hwC0D2:0x05=0x18560010"; gbos_hda_pins
check "hwCxDy: prefix picks the HDMI codec explicitly" "hwC0D2 user_pin_configs 0x05 0x18560010|hwC0D2 reconfig 1"

mk "$RT hwC1D0:0x10ec0257"; HDA_PINS="hwC1D0:0x1b=0x90170110"; gbos_hda_pins
check "prefix for one of two analog codecs" "hwC1D0 user_pin_configs 0x1b 0x90170110|hwC1D0 reconfig 1"

mk "$RT hwC1D0:0x100200e8"; HDA_PINS="0x21=0x0321101f"; gbos_hda_pins
check "AMD HDMI vendor id skipped without prefix" "hwC0D0 user_pin_configs 0x21 0x0321101f|hwC0D0 reconfig 1"

mk "$RT hwC1D0:0x10de0084"; HDA_PINS="0x21=0x0321101f"; gbos_hda_pins
check "NVIDIA HDMI vendor id skipped without prefix" "hwC0D0 user_pin_configs 0x21 0x0321101f|hwC0D0 reconfig 1"

for bad in 0x1b 0x1b= =0x90170110 0x1b=90170110 0x1b=0x9017011g 0x1b=0x901701100 zz=0x90170110 "0x1b;reboot=0x1"; do
  mk "$RT"; HDA_PINS="$bad"; gbos_hda_pins
  check "malformed '$bad' ignored, no reconfig" ""
  check_log "malformed '$bad' logged" "ignored"
done

mk "$RT"; HDA_PINS="0x1b=0x90170110 junk 0x14=0x411111f0"; gbos_hda_pins
check "one bad entry among good ones" \
  "hwC0D0 user_pin_configs 0x1b 0x90170110|hwC0D0 user_pin_configs 0x14 0x411111f0|hwC0D0 reconfig 1"

mk "$RT"; HDA_PINS="27=0x90170110"; gbos_hda_pins
check "decimal NID passed through (kernel parses %i)" "hwC0D0 user_pin_configs 27 0x90170110|hwC0D0 reconfig 1"

mk "$RT"; HDA_PINS="0x1b=0x90170110"; REJECT=hwC0D0/reconfig; gbos_hda_pins; REJECT=
check "reconfig refused (codec busy): pins written" "hwC0D0 user_pin_configs 0x1b 0x90170110"
check_log "reconfig failure logged" "reconfig FAILED"
gbos_hda_pins
check "retried on the next run after a failed reconfig" \
  "hwC0D0 user_pin_configs 0x1b 0x90170110|hwC0D0 user_pin_configs 0x1b 0x90170110|hwC0D0 reconfig 1"

mk "$RT"; HDA_PINS="0x1b=0x90170110"; REJECT=hwC0D0/user_pin_configs; gbos_hda_pins; REJECT=
check "pin rejected by the kernel: no reconfig" ""
check_log "rejected pin logged" "rejected pin 0x1b 0x90170110"

mk "$RT" no; HDA_PINS="0x1b=0x90170110"; gbos_hda_pins
check "kernel without SND_HDA_RECONFIG: nothing written" ""
check_log "missing reconfig support logged" "no user_pin_configs"

mk ""; HDA_PINS="0x1b=0x90170110"; gbos_hda_pins
check "no codec at all" ""
check_log "missing codec logged" "no HD-audio codec"

mk "$HDMI"; HDA_PINS="0x1b=0x90170110"; gbos_hda_pins
check "only an HDMI codec and no prefix: nothing written" ""

echo "$((n-fails))/$n passed"
[ $fails = 0 ]
