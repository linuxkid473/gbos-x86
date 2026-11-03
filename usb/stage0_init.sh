#!/bin/busybox sh
# gbOS86 stage0: find the gbOS86 volume (USB stick or NVMe/eMMC install), show the boot menu, log
# everything to it, mount the lapis erofs images, create dm devices, switch to Android init.
export PATH=/bin:/sbin
/bin/busybox --install -s /bin
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs -o mode=0755 tmpfs /dev
echo "1 1 1 1" > /proc/sys/kernel/printk 2>/dev/null || true
mknod -m 600 /dev/console c 5 1; mknod -m 600 /dev/kmsg c 1 11; mknod -m 666 /dev/null c 1 3
mknod -m 620 /dev/tty0 c 4 0; mknod -m 620 /dev/tty1 c 4 1
exec </dev/console >/dev/console 2>&1
log() { echo "<7>gbos-stage0: $*" > /dev/kmsg; }
shell() {
  log "DROPPING TO SHELL ($1). type 'exit' to continue boot."
  setsid cttyhack sh </dev/tty1 >/dev/tty1 2>&1
}
fail() { log "FATAL: $*"; shell "failure"; }

log "=== gbOS86 boot, kernel $(uname -r) ==="
mdev -s
mknod -m 660 /dev/loop-control c 10 237 2>/dev/null
for i in $(seq 0 15); do mknod -m 660 /dev/loop$i b 7 $i 2>/dev/null; done
mkdir -p -m 0755 /dev/socket /dev/block /dev/pts /dev/dm-user /dev/mapper
mknod -m 600 /dev/mapper/control c 10 236; mknod -m 600 /dev/device-mapper c 10 236

# eMMC is the internal disk of many small laptops: its controller driver is a module (the NVMe driver is built in), so load it before
# looking for the volume. Silent if there is no such controller; coldplug (hwdetect) would otherwise only load it after boot.
modprobe -q sdhci-pci 2>/dev/null && log "sdhci-pci loaded (eMMC/SD controller)"

# Find the gbos volume: the USB stick (label GBOS) or an NVMe/eMMC install (label GBOSSYS, made by
# gbos/install.sh). The stick wins when present, unless the NVMe install exists and the stick's
# gbos.cfg doesn't ask for it (MODE=install or BOOT_FROM=stick). NVMe shows up long before USB,
# so give the stick a few seconds after GBOSSYS appears.
log "waiting for LABEL=GBOS (USB) or LABEL=GBOSSYS (NVMe) ..."
STICK=; SYS=; n=0
for i in $(seq 1 60); do
  mdev -s
  STICK=$(findfs LABEL=GBOS 2>/dev/null)
  SYS=$(findfs LABEL=GBOSSYS 2>/dev/null)
  [ -n "$STICK" ] && break
  [ -n "$SYS" ] && { n=$((n+1)); [ $n -ge 6 ] && break; }
  sleep 1
done
[ -n "$STICK$SYS" ] || fail "no GBOS (USB) or GBOSSYS (NVMe) volume after 60s (block devices: $(ls /sys/class/block | tr '\n' ' '))"
mkdir -p /gbos
DEV=$STICK; FROM=stick; SELMODE=
# gbOS86 boot menu, shown when booting from the stick. The stick's gbos.cfg can skip it:
#   MODE=install -> installer/updater, MODE=live (or BOOT_FROM=stick) -> live from the stick,
#   MODE=boot -> the NVMe install if there is one, else live. Unset/MODE=ask -> menu.
boot_menu() {
  def=1; [ -n "$SYS" ] && def=3
  chvt 1 2>/dev/null; clear > /dev/tty1 2>/dev/null
  sleep 1
  # Discard any residual keystrokes (e.g. Enter pressed in GRUB)
  while read -t 1 -r _junk < /dev/tty1 2>/dev/null; do :; done
  {
    echo; echo "   gbOS86"; echo "   ------"; echo
    echo "   1) Run gbOS86 live from this USB (nothing is saved)"
    echo "   2) Install or update gbOS86 on this laptop's internal SSD"
    [ -n "$SYS" ] && echo "   3) Boot the installed gbOS86"
    echo; printf "   Choose 1-%s [default %s in 15 s]: " "$([ -n "$SYS" ] && echo 3 || echo 2)" $def
  } > /dev/tty1
  c=; read -t 15 -r c < /dev/tty1; echo > /dev/tty1
  case "$c" in 1|2) ;; 3) [ -n "$SYS" ] || c=$def;; *) c=$def;; esac
  log "boot menu: choice $c (default $def)"
  CHOICE=$c
}
if [ -n "$STICK" ]; then
  CFGMODE=
  mount -t vfat -o ro $STICK /gbos 2>/dev/null && {
    CFGMODE=$(sed -n 's/^MODE=//p' /gbos/gbos/gbos.cfg 2>/dev/null | tail -1)
    grep -q '^BOOT_FROM=stick' /gbos/gbos/gbos.cfg 2>/dev/null && CFGMODE=live
    umount /gbos
  }
  case "$CFGMODE" in
    install) CHOICE=2;;
    live) CHOICE=1;;
    boot) if [ -n "$SYS" ]; then CHOICE=3; else CHOICE=1; fi;;
    *) boot_menu;;
  esac
  case "$CHOICE" in
    1) SELMODE=boot;;
    2) SELMODE=install;;
    3) DEV=$SYS; FROM=nvme;;
  esac
elif [ -n "$SYS" ]; then
  DEV=$SYS; FROM=nvme
fi
log "found gbos volume on $FROM at $DEV (stick=${STICK:-none} nvme=${SYS:-none})"
mount -t vfat -o rw,flush,umask=000 $DEV /gbos || fail "mount $DEV failed"
G=/gbos/gbos
cp /bin/busybox /gbos/busybox 2>/dev/null

# Boot-numbered log directory on the stick
N=1; while [ -e $G/logs/boot-$N ]; do N=$((N+1)); done
L=$G/logs/boot-$N; mkdir -p $L
log "logging to USB: gbos/logs/boot-$N/kmsg.log"
# Logger keeps running after switch_root: open kmsg fd now, use busybox from the stick.
# Loops end (instead of spinning) if the stick becomes unreachable, e.g. SELinux denial.
( exec 3</dev/kmsg; while /gbos/busybox cat <&3 >> $L/kmsg.log || /gbos/busybox sleep 1; do :; done ) &
( while /gbos/busybox sleep 5; do /gbos/busybox sync; done ) &

# Config file on the stick (edit from any computer)
SHELL_AT=none; LOAD_I915=auto; LOAD_WIFI=0; EXTRA_SLEEP=0; MODE=boot
[ -f $G/gbos.cfg ] && . $G/gbos.cfg && log "loaded gbos.cfg: MODE=$MODE SHELL_AT=$SHELL_AT LOAD_I915=$LOAD_I915 LOAD_WIFI=$LOAD_WIFI"
# The boot menu's choice wins over gbos.cfg's MODE (ask/live are menu-level settings).
[ -n "$SELMODE" ] && MODE=$SELMODE
# Reset temporary one-shot MODE=live back to MODE=ask on USB stick so future boots prompt the user
[ "$FROM" = stick ] && [ -f $G/gbos.cfg ] && grep -q '^MODE=live' $G/gbos.cfg 2>/dev/null && sed -i 's/^MODE=live/MODE=ask/' $G/gbos.cfg 2>/dev/null || true
case "$MODE" in install) ;; *) MODE=boot;; esac
[ $FROM = nvme ] && MODE=boot

# Hardware inventory
{
  echo "== cmdline"; cat /proc/cmdline
  echo "== cpu"; grep -m1 'model name' /proc/cpuinfo; grep -m1 '^flags' /proc/cpuinfo
  echo "== mem"; head -3 /proc/meminfo
  echo "== pci (slot class vendor:device driver)"
  for d in /sys/bus/pci/devices/*; do
    drv=$(readlink $d/driver 2>/dev/null); echo "${d##*/} $(cat $d/class) $(cat $d/vendor):$(cat $d/device) ${drv##*/}"
  done
  echo "== block"; ls -l /sys/class/block/
  echo "== fb/drm"; ls /sys/class/graphics /sys/class/drm 2>&1
} > $L/hw.txt 2>&1
mount -t pstore pstore /sys/fs/pstore 2>/dev/null && cp -r /sys/fs/pstore $L/pstore-prev 2>/dev/null
log "hardware inventory written (hw.txt)"

[ "$SHELL_AT" = early ] && shell "SHELL_AT=early"

# Universal dynamic hardware bring-up via modalias coldplugging
if [ "$HWDETECT" != 0 ] && [ -f $G/hwdetect.sh ] && [ -d $G/modules/lib/modules ]; then
  . $G/hwdetect.sh
  gbos_hwdetect 5
elif [ "$LOAD_I915" = 1 ]; then
  # Fallback for minimal sticks without modules tree
  log "modprobe i915"
  modprobe i915 && log "i915 loaded" || log "i915 load FAILED rc=$?"
  sleep 2; mdev -s
fi

# Install to NVMe instead of booting (gbos.cfg MODE=install on the stick). install.sh never returns.
if [ "$MODE" = install ]; then
  [ -f $G/install.sh ] || fail "MODE=install but gbos/install.sh is missing"
  log "MODE=install: running gbos/install.sh"; . $G/install.sh
  fail "install.sh returned"
fi

# Android partitions. Preferred: gbos/super.img (liblp dynamic partitions, layout in
# super.layout: "name start_sector num_sectors"). Fallback: separate gbos/<name>.img files.
mkdir -p /new /dev/block/by-name
dmctl() { LD_LIBRARY_PATH=/system/lib64/bootstrap:/system/lib64 chroot /new /system/bin/bootstrap/linker64 /system/bin/dmctl "$@"; }
target() { case $1 in system_a) echo /new;; *) echo /new/${1%_a};; esac; }
if [ -f $G/super.img ] && [ -f $G/super.layout ]; then
  log "using super.img (dynamic partitions)"
  losetup -r /dev/loop7 $G/super.img || fail "losetup super.img failed"
  mknod -m 600 /dev/block/loop7 b 7 7 2>/dev/null; ln -sf /dev/block/loop7 /dev/block/by-name/super
  i=0
  while read n start len; do  # system_a is first, so / is mounted before the others
    t=$(target $n); mkdir -p $t 2>/dev/null
    losetup -r -o $((start * 512)) /dev/loop$i $G/super.img && mount -t erofs -o ro /dev/loop$i $t \
      && log "mounted $n (super @ sector $start) -> $t" || { log "mount $n FAILED"; [ $n = system_a ] && fail "system mount failed"; }
    i=$((i+1))
  done < $G/super.layout
  mount --bind /dev /new/dev; mount --bind /proc /new/proc; mount --bind /sys /new/sys
  while read n start len; do
    dmctl create $n -ro linear 0 $len /dev/block/loop7 $start >/dev/null 2>&1
    log "dmctl $n = super[$start +$len] rc=$?"
  done < $G/super.layout
else
  log "no super.img, using separate partition images"
  i=0
  for n in system_a vendor_a product_a system_ext_a system_dlkm_a vendor_dlkm_a; do
    t=$(target $n); mkdir -p $t 2>/dev/null
    losetup -r /dev/loop$i $G/$n.img && mount -t erofs -o ro /dev/loop$i $t \
      && log "mounted $n -> $t" || { log "mount $n FAILED"; [ $n = system_a ] && fail "system mount failed"; }
    i=$((i+1))
  done
  mount --bind /dev /new/dev; mount --bind /proc /new/proc; mount --bind /sys /new/sys
  i=8
  for n in system_a vendor_a product_a system_ext_a; do
    losetup -r /dev/loop$i $G/$n.img; sz=$(cat /sys/block/loop$i/size)
    dmctl create $n -ro linear 0 $sz /dev/loop$i 0 >/dev/null 2>&1
    log "dmctl $n on loop$i ($sz sectors) rc=$?"
    i=$((i+1))
  done
fi
umount /new/sys; umount /new/proc; umount /new/dev
mdev -s

[ -f $G/hook.sh ] && { log "running gbos/hook.sh"; . $G/hook.sh; }

[ "$SHELL_AT" = pre_android ] && shell "SHELL_AT=pre_android (Android root at /new)"

# /data and /metadata: the NVMe install's ext4 partitions (persistent), else tmpfs (fresh each boot).
# ext4 is made without the encrypt feature, so vold behaves as it did on tmpfs (no fscrypt).
DATA=; META=
if [ $FROM = nvme ]; then
  DATA=$(findfs LABEL=GBOS_DATA 2>/dev/null); META=$(findfs LABEL=GBOS_META 2>/dev/null)
  # A dual-boot install (installer mode 1) has no GBOS_META partition: /metadata is the 64 MB ext4 image gbos/metadata.img inside GBOSSYS.
  # loop12 is free in both layouts (0-5 and 7 are super.img, 8-11 the separate images, 6 the fake boot disk).
  if [ -n "$DATA" ] && [ -z "$META" ] && [ -f $G/metadata.img ]; then
    if losetup /dev/loop12 $G/metadata.img 2>/dev/null; then META=/dev/loop12; log "metadata: image $G/metadata.img on loop12"
    else log "metadata: losetup of $G/metadata.img FAILED"; fi
  fi
fi
e2fsck_p() { [ -x $G/installer/bin/e2fsck ] && $G/installer/lib/ld-musl-x86_64.so.1 --library-path $G/installer/lib $G/installer/bin/e2fsck -p "$1" >> $L/fsck.txt 2>&1; log "e2fsck -p $1 rc=$?"; }
if [ -n "$DATA" ] && [ -n "$META" ]; then
  e2fsck_p $META; e2fsck_p $DATA
  mount -t ext4 -o noatime,nosuid,nodev $META /new/metadata && log "metadata: ext4 $META" \
    || { log "metadata ext4 mount FAILED, using tmpfs"; mount -t tmpfs tmpfs /new/metadata; }
  if mount -t ext4 -o noatime,nosuid,nodev $DATA /new/data; then
    log "data: ext4 $DATA ($(df -h /new/data | awk 'NR==2{print $4}') free)"
    [ -e /new/data/.gbos_init ] || { chown 1000:1000 /new/data; chmod 0771 /new/data; : > /new/data/.gbos_init; }
  else
    log "data ext4 mount FAILED, using tmpfs"; mount -t tmpfs -o mode=0771 tmpfs /new/data
  fi
else
  mount -t tmpfs -o mode=0771 tmpfs /new/data
  mount -t tmpfs tmpfs /new/metadata
  log "data/metadata: tmpfs (not persistent)"
fi
mount -t devpts devpts /dev/pts -o mode=0620,ptmxmode=0666 2>/dev/null
mknod -m 666 /dev/ptmx c 5 2 2>/dev/null; mknod -m 666 /dev/zero c 1 5 2>/dev/null
mknod -m 666 /dev/random c 1 8 2>/dev/null; mknod -m 666 /dev/urandom c 1 9 2>/dev/null

# Ensure all DRM character devices exist in /dev/dri with permissions 0666
mkdir -p -m 0755 /dev/dri
for _c in /sys/class/drm/card[0-9]* /sys/class/drm/renderD[0-9]*; do
  [ -f "$_c/dev" ] || continue
  _name=${_c##*/}
  _dev=$(cat "$_c/dev")
  [ -c "/dev/dri/$_name" ] || mknod -m 0666 "/dev/dri/$_name" c ${_dev%:*} ${_dev#*:}
done
# Point /dev/dri/card0 at active display GPU (e.g. card1 if simpledrm took card0)
_prim=
for _s in /sys/class/drm/card*-*/status; do
  [ -f "$_s" ] || continue
  if grep -q '^connected' "$_s" 2>/dev/null; then
    _prim=${_s%/status}; _prim=${_prim##*/}; _prim=${_prim%%-*}; break
  fi
done
if [ -z "$_prim" ]; then
  for _c in /sys/class/drm/card[0-9]*; do
    case ${_c##*/} in card*-*) continue;; esac
    _drv=$(readlink "$_c/device/driver" 2>/dev/null); _drv=${_drv##*/}
    case "$_drv" in i915|xe|amdgpu|nouveau|radeon) _prim=${_c##*/}; break;; esac
  done
fi
if [ -n "$_prim" ] && [ "$_prim" != "card0" ] && [ -f "/sys/class/drm/$_prim/dev" ]; then
  _pdev=$(cat "/sys/class/drm/$_prim/dev")
  rm -f /dev/dri/card0
  mknod -m 0666 /dev/dri/card0 c ${_pdev%:*} ${_pdev#*:}
  log "drm: primary display on $_prim ($_pdev), mapped to /dev/dri/card0"
fi
chmod 0666 /dev/dri/* 2>/dev/null || true

dmesg > $L/stage0-dmesg.txt; sync
sleep $EXTRA_SLEEP
mount --move /dev /new/dev; mount --move /proc /new/proc; mount --move /sys /new/sys
mount -t selinuxfs selinuxfs /new/sys/fs/selinux
for d in mnt debug_ramdisk second_stage_resources; do mount -t tmpfs -o mode=0755 tmpfs /new/$d; done
echo "gbos-stage0: switching root -> /system/bin/init selinux_setup" > /new/dev/kmsg
exec switch_root /new /system/bin/init selinux_setup
