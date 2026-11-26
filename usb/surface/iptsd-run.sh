#!/system/bin/sh
# iptsd-run.sh N: IPTS touch input on Surfaces. Started by the gbos_iptsd service of hook.sh.
#   Surface Laptop 7 (Intel): the touchpad is an IPTS HID device (045E:0C9F, behind the quickspi bus). The kernel's hid-multitouch
#     gets a plain pointer from it, but no multitouch, palm rejection or physical click.
#   Surface Pro 5 (2017): the touchscreen and pen are an IPTS device of the ipts driver (kernel/surface/apply.sh). Without iptsd the
#     screen does not react to touch at all. This driver registers it as 045E:001F (hidraw HID_ID=0000:0000045E:0000001F, seen on an
#     SP5 on 2026-11-24); iptsd's own preset still names the older N-trig id 1B96:001F, which is kept as a second entry.
# iptsd (static build of a fork, surface/iptsd) reads the raw frames from the hidraw node and creates multitouch (and pen) uinput
# devices. Each known device has its own iptsd config in surface/ (DEVICES below). Log: logs/boot-N/iptsd.txt.
# Exits at once without surface/iptsd on the stick, and after WAIT seconds (default 120) when no such hidraw node ever appears (every
# other laptop), so the service costs nothing there. Once the device was seen, iptsd is restarted when it dies.
# GBOS_G, GBOS_SYS, GBOS_DEV, GBOS_WAIT, GBOS_SLEEP, GBOS_ONCE exist for tools/test_iptsd_run.sh.
N=$1
G=${GBOS_G:-/postinstall/gbos}
S=$G/surface
SYS=${GBOS_SYS:-/sys}
DEVDIR=${GBOS_DEV:-/dev}
WAIT=${GBOS_WAIT:-120}
STEP=${GBOS_SLEEP:-3}
L=$G/logs/boot-$N
LOG=$L/iptsd.txt
# vendor:product as in the uevent's HID_ID, iptsd-check-device --type, iptsd config file in surface/
DEVICES="0000045E:00000C9F,touchpad,iptsd.conf 0000045E:0000001F,touchscreen,iptsd-surface-pro-5.conf
         00001B96:0000001F,touchscreen,iptsd-surface-pro-5.conf"
[ -x $S/iptsd ] && [ -x $S/iptsd-check-device ] || exit 0
if [ -f $S/Vendor_045e_Product_0c9f.idc ] && [ ! -f /data/system/devices/idc/Vendor_045e_Product_0c9f.idc ]; then
  mkdir -p /data/system/devices/idc 2>/dev/null
  cp $S/Vendor_045e_Product_0c9f.idc /data/system/devices/idc/ 2>/dev/null
  chmod 0644 /data/system/devices/idc/Vendor_045e_Product_0c9f.idc 2>/dev/null
fi
# match UEVENT: prints "type,config" of the first DEVICES entry whose vendor:product the uevent's HID_ID has
match() {
  for e in $DEVICES; do
    grep -qiE "HID_ID=[0-9A-Fa-f]+:${e%%,*}\$" $1 && { echo "${e#*,}"; return 0; }
  done
  return 1
}
echo "$(date +%T) iptsd-run started" >> $LOG
( sleep 25; echo "== input devices (25 s after start)"; cat /proc/bus/input/devices ) >> $LOG 2>&1 &
seen=0; waited=0; refused=
while true; do
  dev=
  for d in $SYS/class/hidraw/hidraw*; do
    [ -e $d/device/uevent ] || continue
    m=$(match $d/device/uevent) || continue
    type=${m%%,*}; conf=$S/${m#*,}
    cand=$DEVDIR/${d##*/}
    echo "$(date +%T) candidate $cand ($(grep HID_NAME $d/device/uevent), $type)" >> $LOG
    # The check needs the config too: iptsd-check-device builds the same application as iptsd, and that throws InvalidScreenSize
    # when neither the device's metadata report nor a config gives the screen size. The SP5's IPTS (EDS v1) has no metadata, so
    # without IPTSD_CONFIG_FILE every check failed and iptsd never started (SP5 1807, 2026-11-25: 100 s of "candidate", then
    # "giving up"). Its output (why it refused) is logged once per node.
    if out=$(IPTSD_CONFIG_FILE=$conf $S/iptsd-check-device --type $type $cand 2>&1); then dev=$cand; break; fi
    case " $refused " in *" $cand "*) ;; *) echo "$out" >> $LOG; refused="$refused $cand";; esac
  done
  if [ -z "$dev" ]; then
    [ $waited = 0 ] && echo "$(date +%T) waiting for an IPTS hidraw" >> $LOG
    if [ $seen = 0 ] && [ $waited -ge $WAIT ]; then
      if [ -n "$refused" ]; then echo "$(date +%T) giving up: iptsd-check-device refused$refused" >> $LOG
      else echo "$(date +%T) no IPTS device on this machine, giving up" >> $LOG; fi
      exit 0
    fi
    [ -n "$GBOS_ONCE" ] && exit 1
    sleep $STEP; waited=$((waited + STEP)); continue
  fi
  seen=1
  export IPTSD_CONFIG_FILE=$conf
  echo "$(date +%T) running iptsd on $dev ($type, config $IPTSD_CONFIG_FILE)" >> $LOG
  $S/iptsd $dev >> $LOG 2>&1
  rc=$?
  echo "$(date +%T) iptsd exited rc=$rc" >> $LOG
  [ -n "$GBOS_ONCE" ] && exit 0
  sleep $STEP
done
