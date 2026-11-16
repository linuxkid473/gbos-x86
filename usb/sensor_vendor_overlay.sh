#!/bin/sh

gbos_prepare_sensor_vendor_overlay() {
  _sensor_root=${NEWROOT:-/new}
  _sensor_sysfs=${SYSFS:-/sys}
  _sensor_vendor=$_sensor_root/vendor
  _sensor_hal_name=android.hardware.sensors@2.1-trogdor-subhal.so
  _sensor_source=$G/sensors/$_sensor_hal_name
  _sensor_hals=$_sensor_vendor/etc/sensors/hals.conf
  # /dev is tmpfs already, and stage0 moves it into the new root before switch_root.
  _sensor_tmp=${SENSOR_HAL_TMP:-/dev/.gbos-sensor-hal}

  if [ "${SENSOR_HAL:-0}" != 1 ] ||
     [ "$(cat "$_sensor_sysfs/class/dmi/id/sys_vendor" 2>/dev/null)" != "Dell Inc." ] ||
     [ "$(cat "$_sensor_sysfs/class/dmi/id/product_name" 2>/dev/null)" != "Latitude 7320 Detachable" ] ||
     [ "$(cat "$_sensor_sysfs/class/dmi/id/board_name" 2>/dev/null)" != "0TNW0H" ] ||
     [ ! -s "$_sensor_source" ] || [ ! -d "$_sensor_vendor/lib64" ] ||
     [ ! -f "$_sensor_hals" ] ||
     ! grep -Fq '/vendor/lib64/android.hardware.sensors@2.1-Desktop-IIO-Subhal.so' "$_sensor_hals"; then
    return 0
  fi

  if [ "${GBOS_SENSOR_OVERLAY_FAILED:-0}" = 1 ]; then
    log "hook: experimental Latitude 7320 Sensors HAL not selected (earlier overlay preparation failed)"
    return 1
  fi
  [ -f "$_sensor_tmp/ready" ] && return 0
  _sensor_storage_mounted=0
  _sensor_overlay_mounted=0
  if gbos_sensor_overlay_command mkdir -p "$_sensor_tmp" &&
     gbos_sensor_overlay_command mount -t tmpfs -o size=16m,mode=0755 tmpfs "$_sensor_tmp"; then
    _sensor_storage_mounted=1
    if gbos_sensor_overlay_command mkdir -p "$_sensor_tmp/upper" "$_sensor_tmp/work" "$_sensor_tmp/merged" &&
       gbos_sensor_overlay_command mount -t overlay overlay \
         -o "lowerdir=$_sensor_vendor,upperdir=$_sensor_tmp/upper,workdir=$_sensor_tmp/work" \
         "$_sensor_tmp/merged"; then
      _sensor_overlay_mounted=1
      if gbos_sensor_overlay_command touch "$_sensor_tmp/ready" &&
         gbos_sensor_overlay_command mount --bind "$_sensor_tmp/merged" "$_sensor_vendor"; then
        log "hook: experimental Latitude 7320 Sensors HAL vendor overlay prepared at $_sensor_tmp"
        return 0
      fi
    fi
  fi
  GBOS_SENSOR_OVERLAY_FAILED=1
  [ "$_sensor_overlay_mounted" = 0 ] || gbos_sensor_overlay_command umount "$_sensor_tmp/merged" || :
  [ "$_sensor_storage_mounted" = 0 ] || gbos_sensor_overlay_command umount "$_sensor_tmp" || :
  log "hook: experimental Latitude 7320 Sensors HAL not selected (overlay preparation failed)"
  return 1
}

gbos_sensor_overlay_command() {
  if _sensor_error=$("$@" 2>&1); then
    return 0
  else
    _sensor_rc=$?
    log "hook: sensor overlay command failed (rc=$_sensor_rc): $*; $_sensor_error"
    return "$_sensor_rc"
  fi
}
