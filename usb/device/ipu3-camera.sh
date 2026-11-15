#!/system/bin/sh
N=${1:-0}
G=/postinstall/gbos
MODDIR=${0%/*}
[ -d "$MODDIR" ] || MODDIR=$G/device
L=$G/logs/boot-$N
[ -d "$L" ] || L=/data/local/tmp
BIN=$MODDIR/ipu3-webcam
exec >> $L/camera.txt 2>&1
echo "=== ipu3-camera $(date)"

[ -f $G/gbos.cfg ] && eval "$(grep -E '^CAMERA[A-Za-z0-9_]*=[A-Za-z0-9_.-]*$' $G/gbos.cfg)"
[ "${CAMERA:-1}" = 0 ] && { echo "CAMERA=0 in gbos.cfg: not started"; exit 0; }
[ -x "$BIN" ] || { echo "no $BIN (tools/build_camera_tools.sh builds it)"; exit 0; }

i=0
while [ $i -lt 20 ]; do
  CAMS=$($BIN --probe 2>/dev/null)
  [ -n "$CAMS" ] && break
  sleep 1; i=$((i + 1))
done
REPROBED=
for d in /sys/bus/i2c/devices/i2c-INT3479:* /sys/bus/i2c/devices/i2c-INT3477:* /sys/bus/i2c/devices/i2c-INT347A:* \
         /sys/bus/i2c/devices/i2c-INT33BE:* /sys/bus/i2c/devices/i2c-INT347E:* /sys/bus/i2c/devices/i2c-OVTI*; do
  [ -e "$d" ] || continue
  case "${d##*/}" in *-VCM) continue;; esac
  if [ ! -e "$d/driver" ]; then
    case "${d##*/}" in
      i2c-INT3479:*) DRV=ov5670;; i2c-INT3477:*) DRV=ov8858;; i2c-INT347A:*) DRV=ov8865;;
      i2c-INT33BE:*) DRV=ov5693;; i2c-INT347E:*) DRV=ov7251;; *) DRV=;;
    esac
    [ -n "$DRV" ] && modprobe -q $DRV 2>/dev/null
    if [ -n "$DRV" ] && [ -e /sys/bus/i2c/drivers/$DRV/bind ]; then
      echo "${d##*/}" > /sys/bus/i2c/drivers/$DRV/bind 2>&1 && R=bound || R="bind failed (see kmsg: $DRV)"
    elif [ -e /sys/bus/i2c/drivers_probe ]; then
      echo "${d##*/}" > /sys/bus/i2c/drivers_probe 2>&1 && R=probed || R="probe failed"
    else R="no ${DRV:-matching} driver loaded"; fi
    echo "${d##*/}: had no driver -> $R"
    REPROBED=1
  fi
done
if [ -n "$REPROBED" ]; then
  i=0
  while [ $i -lt 10 ]; do
    sleep 1; i=$((i + 1))
    CAMS=$($BIN --probe 2>/dev/null)
    [ -n "$CAMS" ] && break
  done
fi
ls /dev/v4l-subdev* >/dev/null 2>&1 || echo "no /dev/v4l-subdev*: a sensor the IPU3 receiver waits for is not bound"
for d in /sys/bus/i2c/devices/i2c-INT347*:* /sys/bus/i2c/devices/i2c-INT33BE:*; do
  [ -e "$d" ] && echo "  ${d##*/} driver=$(readlink $d/driver 2>/dev/null | sed 's#.*/##')"
done
echo "sensors:"; echo "${CAMS:-  (none)}"
[ -n "$CAMS" ] || { echo "no IPU3 sensor with a complete media graph: see hwdetect.txt '== mipi cameras'"; exit 0; }
NCAM=$(echo "$CAMS" | wc -l)

facing() {
  case "$1" in
    *orientation=0*) echo front;;
    *orientation=1*) echo rear;;
    ov8858*|ov8865*|ov13858*) echo rear;;
    *) echo front;;
  esac
}
ORDERED=$(echo "$CAMS" | while read -r l; do [ "$(facing "$l")" = front ] && echo "$l"; done
          echo "$CAMS" | while read -r l; do [ "$(facing "$l")" = rear ] && echo "$l"; done)
LABELS= NRS=
k=0
for s in $(echo "$ORDERED" | awk '{print $1}'); do
  NRS="$NRS${NRS:+,}$((42 + k))"
  LABELS="$LABELS${LABELS:+,}Camera_$((k + 1))_$s"
  k=$((k + 1))
done
CAPS=$(i=0; s=; while [ $i -lt $NCAM ]; do s="$s${s:+,}1"; i=$((i + 1)); done; echo $s)
if grep -q '^v4l2loopback ' /proc/modules; then
  [ -e /dev/video$((41 + NCAM)) ] || rmmod v4l2loopback 2>/dev/null
fi
if ! grep -q '^v4l2loopback ' /proc/modules; then
  for M in $G/kmod/v4l2loopback.ko $MODDIR/v4l2loopback.ko $G/surface/v4l2loopback.ko; do
    [ -f "$M" ] || continue
    insmod "$M" devices=$NCAM video_nr=$NRS exclusive_caps=$CAPS card_label=$LABELS && { echo "v4l2loopback: $M devices=$NCAM video_nr=$NRS"; break; }
  done
fi
grep -q '^v4l2loopback ' /proc/modules || { echo "ERROR: v4l2loopback not loaded"; exit 0; }
sleep 1

for n in /dev/video* /dev/media* /dev/v4l-subdev*; do
  [ -e "$n" ] || continue
  chmod 0666 "$n"; chown cameraserver:camera "$n"; chcon u:object_r:video_device:s0 "$n"
done 2>/dev/null

FAKE=/data/local/tmp/fake_sys_ipu3
rm -rf $FAKE 2>/dev/null; mkdir -p $FAKE; chmod 0755 /data/local/tmp $FAKE
DESC=$MODDIR/fake_uvc_desc.bin
[ -f $DESC ] || DESC=$G/surface/fake_uvc_desc.bin
fake_uvc() {
  V=video$1; D=$FAKE/$V; U=$D/usb_dev; I=$U/intf
  mkdir -p $I/video4linux/$V $U/physical_location
  if [ -f "$DESC" ]; then
    { dd if=$DESC bs=1 count=10 2>/dev/null; printf "\\x${2#??}\\x${2%??}"; dd if=$DESC bs=1 skip=12 2>/dev/null; } > $U/descriptors
    ln -sf ../descriptors $I/descriptors
  fi
  printf 'DRIVER=uvcvideo\nPRODUCT=408/%s/100\nTYPE=239/2/1\nINTERFACE=14/1/0\nMODALIAS=usb:v0408p%sd0100dcEFdsc02dp01ic0Eisc01ip00in00\n' \
    "$(echo $2 | sed 's/^0*//')" "$(echo $2 | tr a-f A-F)" > $I/uevent
  echo 0408 > $U/idVendor; echo $2 > $U/idProduct; echo "$4" > $U/product; echo "Built-in" > $U/manufacturer
  echo fixed > $U/removable
  echo $3 > $U/physical_location/panel; echo no > $U/physical_location/dock; echo yes > $U/physical_location/lid
  echo center > $U/physical_location/vertical_position; echo center > $U/physical_location/horizontal_position
  R=/sys/devices/virtual/video4linux/$V
  [ -d $R ] || { echo "$R missing"; return 1; }
  for a in $R/*; do [ -f "$a" ] && cp -f "$a" $I/video4linux/$V/ 2>/dev/null; done
  rm -f $I/video4linux/$V/device; ln -sf $I $I/video4linux/$V/device
  chmod -R 0755 $D; chmod 0644 $U/descriptors $I/uevent 2>/dev/null
  grep -q " $R " /proc/mounts || mount -o bind $I/video4linux/$V $R
  echo "$V: fake UVC 0408:$2 panel=$3 '$4'"
}

if [ -z "$CAMERA_FLICKER" ]; then
  case "$(getprop persist.sys.timezone)" in
    America/Argentina*|America/Buenos_Aires|America/Santiago|America/Montevideo|America/Asuncion|America/La_Paz|America/Cayenne|\
America/Paramaribo|America/Guyana|America/Godthab|America/Nuuk) CAMERA_FLICKER=50;;
    America/*|US/*|Canada/*|Pacific/Honolulu|Asia/Seoul|Asia/Taipei|Asia/Manila|Asia/Riyadh) CAMERA_FLICKER=60;;
    *) CAMERA_FLICKER=50;;
  esac
fi
echo "anti-flicker ${CAMERA_FLICKER} Hz"

rm -f /data/local/tmp/ipu3-webcam.lock.want
k=0
NF=0 NR_=0
while read -r s rest; do
  [ -n "$s" ] || continue
  NR=$((42 + k))
  if [ "$(facing "$s $rest")" = rear ]; then PANEL=back; NAME=rear; [ $NR_ -gt 0 ] && NAME=rear$NR_; NR_=$((NR_ + 1))
  else PANEL=front; NAME=front; [ $NF -gt 0 ] && NAME=front$NF; NF=$((NF + 1)); fi
  fake_uvc $NR $(printf '%04x' $((0x4052 + k))) $PANEL "Built-in $NAME camera ($s)"
  eval ROT=\${CAMERA_ROTATE_$s:-auto}
  eval MIR=\${CAMERA_MIRROR_$s:-0}
  ARGS="--sensor $s --out /dev/video$NR --name $NAME --rotate $ROT --flicker $CAMERA_FLICKER --debug-dir $L"
  [ "$MIR" = 1 ] && ARGS="$ARGS --mirror"
  echo "bridge $NAME: $BIN $ARGS"
  ( while true; do
      $BIN $ARGS >> $L/camera-$NAME.txt 2>&1
      echo "bridge $NAME exited ($?), restarting in 3 s" >> $L/camera-$NAME.txt
      sleep 3
    done ) &
  k=$((k + 1))
done <<EOF
$ORDERED
EOF
sleep 2

for p in $(ps -A -o NAME 2>/dev/null | grep -E '^android\.hardware\.camera\.provider' | sort -u); do
  echo "restarting $p"; killall $p 2>/dev/null
done
killall cameraserver 2>/dev/null
echo "=== ipu3-camera done: $NCAM camera(s)"
wait
