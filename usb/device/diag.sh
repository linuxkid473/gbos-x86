#!/system/bin/sh
N=${1:-0}
L=/postinstall/gbos/logs/boot-$N
[ -d "$L" ] || exit 0
{
  echo "== dmi"; cat /sys/class/dmi/id/sys_vendor /sys/class/dmi/id/product_name 2>/dev/null
  echo "== video nodes"; ls -l /dev/video* /dev/media* /dev/v4l-subdev* 2>&1
  for v in /sys/class/video4linux/*; do [ -e "$v" ] && echo "${v##*/}: $(cat $v/name 2>/dev/null)"; done
  echo "== camera bridges"; ps -A -o PID,NAME,ARGS 2>/dev/null | grep -E 'ipu3-webcam|surface-webcam|camera' | grep -v grep
  echo "== ipu3-webcam --probe"; /postinstall/gbos/device/ipu3-webcam --probe 2>&1
  echo "== dumpsys media.camera"; dumpsys media.camera 2>&1 | head -n 250
  echo "== iio devices (rotation, light)"
  for d in /sys/bus/iio/devices/iio:device*; do [ -e "$d" ] && echo "${d##*/}: $(cat $d/name 2>/dev/null)"; done
  echo "== gbos-autorotate --read (gravity direction now, tablet mode)"; /postinstall/gbos/device/gbos-autorotate --read 2>&1
  echo "== dumpsys sensorservice"; dumpsys sensorservice 2>&1 | head -n 120
  echo "== input switches (SW_TABLET_MODE = keyboard detached/folded)"; getevent -lp 2>&1 | grep -E 'add device|name:|SW_' | grep -B2 'SW_'
  echo "== dumpsys input (tablet mode, devices)"; dumpsys input 2>&1 | grep -iE 'tablet|SW_|Device [0-9]+:|  Name:|Sources:' | head -n 120
  echo "== rotation settings"; settings get system accelerometer_rotation; settings get system user_rotation
  echo "== dumpsys window (rotation)"; dumpsys window displays 2>&1 | grep -iE 'rotation|orientation' | head -n 20
} > $L/tablet.txt 2>&1
