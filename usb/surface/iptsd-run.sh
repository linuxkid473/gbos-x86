#!/system/bin/sh
# iptsd-run.sh N: touchpad gestures on the Surface Laptop 7 (Intel). Started by the gbos_iptsd service of hook.sh.
# The touchpad is an IPTS HID device (045E:0C9F, behind the quickspi bus). The kernel's hid-multitouch gets a plain pointer from it,
# but no multitouch, palm rejection or physical click: iptsd (static build of a fork, surface/iptsd) reads the raw frames from the
# hidraw node and creates a multitouch uinput device. Log: logs/boot-N/iptsd.txt.
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
[ -x $S/iptsd ] && [ -x $S/iptsd-check-device ] || exit 0
export IPTSD_CONFIG_FILE=$S/iptsd.conf
if [ -f $S/Vendor_045e_Product_0c9f.idc ] && [ ! -f /data/system/devices/idc/Vendor_045e_Product_0c9f.idc ]; then
  mkdir -p /data/system/devices/idc 2>/dev/null
  cp $S/Vendor_045e_Product_0c9f.idc /data/system/devices/idc/ 2>/dev/null
  chmod 0644 /data/system/devices/idc/Vendor_045e_Product_0c9f.idc 2>/dev/null
fi
echo "$(date +%T) iptsd-run started (config $IPTSD_CONFIG_FILE)" >> $LOG
( sleep 25; echo "== input devices (25 s after start)"; cat /proc/bus/input/devices ) >> $LOG 2>&1 &
seen=0; waited=0
while true; do
  dev=
  for d in $SYS/class/hidraw/hidraw*; do
    [ -e $d/device/uevent ] || continue
    grep -qE 'HID_ID=[0-9A-Fa-f]+:0000045[Ee]:00000[Cc]9[Ff]' $d/device/uevent || continue
    cand=$DEVDIR/${d##*/}
    echo "$(date +%T) candidate $cand ($(grep HID_NAME $d/device/uevent))" >> $LOG
    if $S/iptsd-check-device --quiet --type touchpad $cand >> $LOG 2>&1; then dev=$cand; break; fi
  done
  if [ -z "$dev" ]; then
    [ $waited = 0 ] && echo "$(date +%T) waiting for the IPTS touchpad hidraw" >> $LOG
    if [ $seen = 0 ] && [ $waited -ge $WAIT ]; then echo "$(date +%T) no IPTS touchpad on this machine, giving up" >> $LOG; exit 0; fi
    [ -n "$GBOS_ONCE" ] && exit 1
    sleep $STEP; waited=$((waited + STEP)); continue
  fi
  seen=1
  echo "$(date +%T) running iptsd on $dev" >> $LOG
  $S/iptsd $dev >> $LOG 2>&1
  rc=$?
  echo "$(date +%T) iptsd exited rc=$rc" >> $LOG
  [ -n "$GBOS_ONCE" ] && exit 0
  sleep $STEP
done
