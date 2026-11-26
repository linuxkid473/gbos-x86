# gbOS86 hardware detection: sourced by stage0_init.sh (once the gbos volume is mounted at /gbos) and by hook.sh.
# Needs: $G (gbos dir), log(), $L (boot log dir, optional). Pure busybox ash.
#
# What it does, in order (every step is non-fatal; a laptop just ends up with fewer drivers):
#  1. gbos_modules_mount: bind the stick's full module tree (gbos/modules/lib/modules/<uname -r>, depmod'ed,
#     ~1000 drivers) over /lib/modules, so modprobe can resolve any device's modalias.
#  2. gbos_coldplug: the usual udev trick without udev: read every device's `modalias` from sysfs and
#     `modprobe -a` them, repeated because a loaded driver (HDA controller, USB hub, ...) creates new devices.
#     This finds the right Wi-Fi, Bluetooth, audio (HDA or SOF), touchpad/keyboard (I2C-HID, ISH, PS/2),
#     laptop hotkey/backlight (dell/hp/thinkpad/ideapad/asus/...), card-reader and webcam drivers on any machine.
#     MODOPTS="xe.enable_psr=0 ..." in gbos.cfg (the same module.param=value spelling as on a Linux kernel command line)
#     loads those modules first with the given parameters (gbos_modopts), so a screen/driver quirk fix needs no kernel rebuild.
#  3. firmware: the kernel's firmware loader looks in gbos/firmware (a copy of linux-firmware, trimmed).
#  4. gbos_hw_report: writes hwdetect.txt next to the other boot logs: what got a driver, what didn't,
#     which firmware files were missing.
GBOS_FWPATH=/sys/module/firmware_class/parameters/path

gbos_modules_mount() {
  KVER=$(uname -r)
  grep -q " /lib/modules/$KVER " /proc/mounts 2>/dev/null && return 0
  MDIR=""
  if [ -f "$G/modules/lib/modules/$KVER/modules.dep" ]; then
    MDIR="$G/modules/lib/modules/$KVER"
  elif [ -f "$G/modules/lib/modules/${KVER}+/modules.dep" ]; then
    MDIR="$G/modules/lib/modules/${KVER}+"
  fi
  if [ -n "$MDIR" ]; then
    mkdir -p "/lib/modules/$KVER"
    mount --bind "$MDIR" "/lib/modules/$KVER" 2>/dev/null \
      && log "hwdetect: module tree $KVER ($(find /lib/modules/$KVER -name '*.ko' | wc -l) modules) bound from $MDIR" \
      || log "hwdetect: bind mount of $MDIR to /lib/modules/$KVER FAILED"
    return 0
  fi
  # A tree built for another kernel (e.g. "6.18.21-gbos-whl+" on a "6.18.21-gbos-whl" kernel) is not bound: its modules carry the other
  # release in their vermagic and the kernel refuses them. Say so, because the symptom is only "no driver for anything" (i915, Wi-Fi, audio).
  log "hwdetect: gbos/modules/lib/modules/$KVER missing (found: $(ls $G/modules/lib/modules 2>/dev/null | tr '\n' ' '))," \
      "only the initramfs modules are available: rebuild the stick (tools/deploy_stick.sh) with the modules of this kernel"
  return 1
}

# iwlwifi is only the bus half: once it has its firmware it asks for the op_mode driver named in the ucode
# ("... op_mode iwlmvm") with request_module(), which silently does nothing in this initramfs. Without
# iwlmvm there is no wlan0 (boot-20 on kernel #32: iwlwifi loaded, firmware loaded, no Wi-Fi). Load it ourselves.
gbos_load_opmodes() {
  for m in $(dmesg | sed -n 's/.*op_mode \(iwl[a-z]*\).*/\1/p' | sort -u); do
    grep -q "^$m " /proc/modules && continue
    modprobe -q $m && log "hwdetect: loaded $m (op_mode announced by iwlwifi)" || log "hwdetect: modprobe $m FAILED"
  done
}

# IPU6 creates its ISYS capture pipeline on the auxiliary bus after the PCI core probes.
# Explicitly request it once the core is loaded, rather than relying on auxiliary-bus
# modalias coldplug ordering in the Android init environment.
gbos_load_ipu6_isys() {
  PROC_MODULES=${GBOS_PROC_MODULES:-/proc/modules}
  grep -q '^intel_ipu6 ' "$PROC_MODULES" 2>/dev/null || return 0
  grep -q '^intel_ipu6_isys ' "$PROC_MODULES" 2>/dev/null && return 0
  if modprobe -q intel_ipu6_isys; then
    log "hwdetect: loaded intel_ipu6_isys (IPU6 ISYS capture pipeline)"
  else
    log "hwdetect: modprobe intel_ipu6_isys FAILED (IPU6 ISYS capture pipeline)"
  fi
}

# BusyBox modprobe ignores modules.softdep. Register the PMIC child drivers before
# INT3472 probes so its MFD cells are handled in clock, regulator, GPIO order.
gbos_load_tps68470_children() {
  [ "${GBOS_TPS_REQ:-0}" = 1 ] && return 0
  sys=${GBOS_SYSFS_ROOT:-/sys}
  [ "$(cat "$sys/class/dmi/id/sys_vendor" 2>/dev/null)" = "Dell Inc." ] || return 0
  [ "$(cat "$sys/class/dmi/id/product_name" 2>/dev/null)" = "Latitude 7320 Detachable" ] || return 0
  for m in clk-tps68470 tps68470-regulator gpio-tps68470; do
    if modprobe -q "$m"; then
      log "hwdetect: requested $m (INT3472 PMIC child)"
    else
      log "hwdetect: modprobe $m FAILED (INT3472 PMIC child)"
      return 1
    fi
  done
  GBOS_TPS_REQ=1
  return 0
}

# Some SoundWire codec drivers are not autoloaded from their device modalias. Load them before coldplug
# probes the HDA/SOF controller; otherwise the machine driver can parse its topology before codec DAIs exist.
gbos_load_soundwire_codecs() {
  [ -z "$GBOS_SDW_REQ" ] || return 0
  GBOS_SDW_REQ=1
  if modprobe -a -q snd-soc-rt1320-sdw snd-soc-rt721-sdca snd-soc-rt711-sdca snd-soc-rt1316-sdw snd-soc-rt715-sdca 2>/dev/null; then
    log "hwdetect: SoundWire codec modules requested ($1)"
  else
    log "hwdetect: SoundWire codec module request failed ($1)"
  fi
}

# Latitude 7320 Detachable: intel_hid (INTC1051; there is no INT33D6 for intel_vbtn) is the only SW_TABLET_MODE source.
# In auto mode it creates its switch only for chassis types 31/32 and only after the first 0xCC/0xCD notify, which
# boots 8-10 (2026-11-18) never logged ("switch event received"), so Android never reached tablet state.
# enable_sw_tablet_mode=2 reads VGBS at probe, so Android sees the initial state too. The parameter is read-only (0444):
# it must be in modprobe.d before coldplug loads intel_hid. TABLET_SWITCH=0 (gbos.cfg) keeps the kernel's auto mode,
# TABLET_SWITCH=1 selects at-first-event; MODOPTS="intel_hid.enable_sw_tablet_mode=N" wins over both.
# Prints the modprobe.d line (if any).
gbos_tablet_switch_opts() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  [ "$(cat "$sys/class/dmi/id/sys_vendor" 2>/dev/null)" = "Dell Inc." ] || return 0
  [ "$(cat "$sys/class/dmi/id/product_name" 2>/dev/null)" = "Latitude 7320 Detachable" ] || return 0
  case " $MODOPTS " in
    *" intel_hid.enable_sw_tablet_mode="*)
      log "hwdetect: intel_hid tablet switch from MODOPTS" >&2
      return 0 ;;
  esac
  case "${TABLET_SWITCH:-2}" in
    0) log "hwdetect: TABLET_SWITCH=0: intel_hid tablet switch left in auto mode" >&2; return 0 ;;
    1|2) ;;
    *) log "hwdetect: TABLET_SWITCH=$TABLET_SWITCH unknown, using 2" >&2; TABLET_SWITCH=2 ;;
  esac
  echo "options intel_hid enable_sw_tablet_mode=${TABLET_SWITCH:-2}"
  log "hwdetect: intel_hid enable_sw_tablet_mode=${TABLET_SWITCH:-2} (Latitude 7320 Detachable, chassis_type $(cat "$sys/class/dmi/id/chassis_type" 2>/dev/null))" >&2
}

# Latitude 7320 Detachable: the BIOS enables SoundWire link 2 and declares an RT1308 amp there that is not fitted (Windows
# enumerates only RT711 link 0, RT1316 link 1, RT714 link 3; the mono topology does not use link 2). Started empty, link 2 logs
# "Bus clash for control word" x11 and, 3 s later, "Clock stop failed -110" before the driver powers it down anyway.
# sdw_link_skip_mask (gbOS soundwire_intel parameter, kernel/isa_emu_patch/sdw_link_skip.py) leaves it off from the start; the
# link stays in the link mask, so the same machine driver and topology are selected. A soundwire-intel.ko without the parameter
# ignores it ("unknown parameter"). MODOPTS="soundwire_intel.sdw_link_skip_mask=0" restores the upstream behaviour.
# Prints the modprobe.d line (if any).
gbos_sdw_link_opts() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  [ "$(cat "$sys/class/dmi/id/sys_vendor" 2>/dev/null)" = "Dell Inc." ] || return 0
  [ "$(cat "$sys/class/dmi/id/product_name" 2>/dev/null)" = "Latitude 7320 Detachable" ] || return 0
  case " $MODOPTS " in
    *" soundwire_intel.sdw_link_skip_mask="*|*" soundwire-intel.sdw_link_skip_mask="*)
      log "hwdetect: SoundWire link skip mask from MODOPTS" >&2
      return 0 ;;
  esac
  echo "options soundwire_intel sdw_link_skip_mask=4"
  log "hwdetect: soundwire_intel sdw_link_skip_mask=4 (Latitude 7320 Detachable: link 2 has no codec)" >&2
}

# MODOPTS="module.param=value ..." (gbos.cfg): configure module parameters generically via /etc/modprobe.d/
# and /etc/modprobe.conf so that when modalias coldplug loads a driver for a detected device, the parameters
# are applied naturally without hardcoding drivers or loading modules for hardware not present.
gbos_modopts() {
  mkdir -p /etc/modprobe.d 2>/dev/null
  : > /etc/modprobe.d/gbos.conf
  # Before MODOPTS, so a MODOPTS option for the same module comes later and wins.
  gbos_tablet_switch_opts >> /etc/modprobe.d/gbos.conf
  gbos_sdw_link_opts >> /etc/modprobe.d/gbos.conf

  # Backward compatibility: map legacy XE_PARAMS to options xe ...
  if [ -n "$XE_PARAMS" ]; then
    echo "options xe $XE_PARAMS" >> /etc/modprobe.d/gbos.conf
    log "hwdetect: MODOPTS: mapped legacy XE_PARAMS to xe options"
  fi

  # Backward compatibility: LOAD_I915=0 blacklists i915
  if [ "$LOAD_I915" = "0" ]; then
    echo "blacklist i915" >> /etc/modprobe.d/gbos.conf
    log "hwdetect: MODOPTS: LOAD_I915=0 -> blacklisted i915"
  fi

  [ -n "$MODOPTS" ] || { cp /etc/modprobe.d/gbos.conf /etc/modprobe.conf 2>/dev/null; return 0; }

  # Parse module.param=value entries into "options <module> <param>=<val>"
  echo "$MODOPTS" | tr ' ' '\n' | awk '
    index($0,".")>1 && index($0,"=")>index($0,".") {
      i=index($0,"."); m=substr($0,1,i-1); o[m]=o[m] " " substr($0,i+1)
    }
    /^blacklist=/ { split($0, a, "="); b[a[2]]=1 }
    END {
      for(m in o) print "options " m o[m]
      for(m in b) print "blacklist " m
    }' >> /etc/modprobe.d/gbos.conf

  cp /etc/modprobe.d/gbos.conf /etc/modprobe.conf 2>/dev/null
  log "hwdetect: configured modprobe options from MODOPTS"
}

# One coldplug pass: all modaliases, de-duplicated, to a single modprobe (it reads modules.alias once).
gbos_coldplug_pass() {
  gbos_load_tps68470_children || return 1
  find /sys/devices -name modalias 2>/dev/null | xargs cat 2>/dev/null | sort -u \
    | xargs modprobe -a -q 2>/dev/null
  gbos_load_ipu6_isys
}

# After a pass that loaded drivers: wait until their new devices stop appearing (the modalias count is unchanged for two 0.2 s
# polls), at most 1 s. Was a fixed 1 s per pass (~3 s of the Latitude 7320's 4 passes); a device that shows up later is still
# caught by the next pass and by gbos_wait_devices.
gbos_settle_devices() {
  _sd_last=$(find ${GBOS_SYSFS_DEVICES:-/sys/devices} -name modalias 2>/dev/null | wc -l); _sd_same=0; _sd_t=0
  while [ $_sd_t -lt ${GBOS_SETTLE_POLLS:-5} ]; do
    sleep ${GBOS_SETTLE_STEP:-0.2}; _sd_t=$((_sd_t+1))
    _sd_n=$(find ${GBOS_SYSFS_DEVICES:-/sys/devices} -name modalias 2>/dev/null | wc -l)
    if [ "$_sd_n" = "$_sd_last" ]; then _sd_same=$((_sd_same+1)); [ $_sd_same -ge 2 ] && return 0; else _sd_same=0; _sd_last=$_sd_n; fi
  done
}

# r8169 needs its PHY driver (realtek.ko, always built with it) loaded before it probes. It asks for it with
# MODULE_SOFTDEP("pre: realtek"), which busybox modprobe ignores, and phylib's request_module() for the PHY id does nothing
# here (as for iwlmvm above). Without it: "no dedicated PHY driver found for PHY ID 0x001cc800, maybe realtek.ko needs to be
# added to initramfs?", probe error -49, no Ethernet (issue 61: Dell OptiPlex 3020, RTL8111/8168). Before the first coldplug pass.
gbos_preload_phy() {
  for d in /sys/bus/pci/devices/*; do
    [ "$(cat $d/vendor 2>/dev/null)" = 0x10ec ] || continue
    case "$(cat $d/class 2>/dev/null)" in
      0x0200*) grep -q '^realtek ' /proc/modules && return 0
               modprobe -q realtek && log "hwdetect: loaded realtek (PHY driver for r8169, Realtek Ethernet ${d##*/})" \
                 || log "hwdetect: modprobe realtek FAILED (r8169 will find no PHY driver)"
               return 0 ;;
    esac
  done
}

gbos_coldplug() {
  [ -f ${GBOS_MODALIAS:-/lib/modules/$(uname -r)/modules.alias} ] || { log "hwdetect: no modules.alias, skipping coldplug"; return 1; }
  gbos_preload_phy
  # Not loaded by alias: ACPI video (brightness keys), battery already built in.
  p=1; before=$(wc -l < ${GBOS_PROC_MODULES:-/proc/modules})
  while [ $p -le 4 ]; do
    gbos_coldplug_pass || return 1
    # The 1 s settle is only for devices created by drivers this pass loaded. A pass that loaded nothing new has
    # nothing to wait for: the hook's second coldplug spent its whole 1.1 s here ("122 (was 122)", Latitude 7320).
    if [ "$(wc -l < ${GBOS_PROC_MODULES:-/proc/modules})" -ne $before ]; then gbos_settle_devices; fi
    gbos_load_opmodes; mdev -s 2>/dev/null
    now=$(wc -l < ${GBOS_PROC_MODULES:-/proc/modules})
    log "hwdetect: coldplug pass $p: $now modules loaded (was $before)"
    [ $now -eq $before ] && break
    before=$now; p=$((p+1))
  done
}

# Marvell's mwifiex (88W8897: Surface Pro 4/5/6, Surface Book) names its station interface mlan0. Android's Wi-Fi stack only
# looks for wlan0 (wifi.interface), so on a Surface Pro 5 (1807, 2026-11-25) Wi-Fi switched itself off again right after being
# switched on, although the firmware was running ("WLAN FW is active"). Renamed while it is still down, before Android starts.
gbos_rename_wifi() {
  _net=${GBOS_NET:-/sys/class/net}   # not N: hook.sh's boot number
  [ -e $_net/wlan0 ] && return 0
  for _if in $(ls $_net 2>/dev/null | grep '^mlan'); do
    ip link set dev $_if down 2>/dev/null
    if ip link set dev $_if name wlan0; then log "hwdetect: Wi-Fi interface $_if renamed to wlan0 (Android looks for wlan0)"
    else log "hwdetect: renaming Wi-Fi interface $_if to wlan0 FAILED"; fi
    return 0
  done
}

# Give drivers time to finish probing and pull their firmware (Wi-Fi, Bluetooth, audio DSP).
gbos_wait_devices() {
  i=0
  while [ $i -lt ${1:-15} ]; do
    gbos_rename_wifi
    w=$(ls /sys/class/net 2>/dev/null | grep -c '^wl')
    b=$(ls /sys/class/bluetooth 2>/dev/null | wc -l)
    s=$(grep -c '\[' /proc/asound/cards 2>/dev/null)
    # Done when every class that exists on this machine has shown up, or nothing is loading any more.
    if [ "$w" -gt 0 ] && [ "$b" -gt 0 ] && [ "$s" -gt 0 ]; then break; fi
    [ "$w" -eq 0 ] && grep -q '^iwlwifi ' /proc/modules && gbos_load_opmodes
    # Peripherals created by the drivers loaded above (SoundWire codecs, Surface Aggregator children) appear after the first
    # coldplug: another pass every 2 s while waiting.
    [ $((i % 2)) -eq 1 ] && gbos_coldplug_pass
    sleep 1; i=$((i+1))
  done
  gbos_rename_wifi
  log "hwdetect: devices after ${i}s: wifi=$(ls /sys/class/net 2>/dev/null | grep '^wl' | tr '\n' ' ') bt=$(ls /sys/class/bluetooth 2>/dev/null | tr '\n' ' ') sound=$(cat /proc/asound/cards 2>/dev/null | grep '\[' | tr -s ' ' | tr '\n' ';') backlight=$(ls /sys/class/backlight 2>/dev/null | tr '\n' ' ')"
}

# Thermal zones and cooling devices, so a device thermal config can be written from real data.
# Android's Pixel thermal HAL looks sensors up by zone type; Google's Lapis profile expects TSR0-TSR4,
# while e.g. the Latitude 7320's ACPI participants are TCPU, TSKN, TMEM, CHRG, AMBF, NGFF (Windows DTT).
gbos_report_thermal() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  echo "== thermal zones (zone type temp_mC policy; trips: type temp_mC)"
  for z in "$sys"/class/thermal/thermal_zone*; do
    [ -d "$z" ] || continue
    echo "${z##*/} $(cat "$z/type" 2>/dev/null) $(cat "$z/temp" 2>/dev/null || echo ?) $(cat "$z/policy" 2>/dev/null)"
    for t in "$z"/trip_point_*_type; do
      [ -f "$t" ] || continue
      n=${t%_type}
      echo "  ${n##*/}: $(cat "$t" 2>/dev/null) $(cat "${n}_temp" 2>/dev/null)"
    done
  done
  echo "== RAPL power limits before Android (zone constraint name power_uW window_us)"
  for z in "$sys"/class/powercap/intel-rapl*; do
    [ -d "$z" ] || continue
    for n in "$z"/constraint_*_name; do
      [ -f "$n" ] || continue
      c=${n%_name}
      echo "${z##*/} $(cat "$z/name" 2>/dev/null) ${c##*/}: $(cat "$n" 2>/dev/null) $(cat "${c}_power_limit_uw" 2>/dev/null) $(cat "${c}_time_window_us" 2>/dev/null)"
    done
  done
  echo "== cooling devices (device type cur/max)"
  for c in "$sys"/class/thermal/cooling_device*; do
    [ -d "$c" ] || continue
    echo "${c##*/} $(cat "$c/type" 2>/dev/null) $(cat "$c/cur_state" 2>/dev/null)/$(cat "$c/max_state" 2>/dev/null)"
  done
}

gbos_report_camera_bus_devices() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  echo "== camera platform devices"
  for d in "$sys"/bus/platform/devices/INT3472* "$sys"/bus/platform/devices/INT3482* "$sys"/bus/platform/devices/tps68470-*; do
    [ -e "$d" ] && echo "${d##*/} modalias=$(cat "$d/modalias" 2>/dev/null) driver=$(readlink "$d/driver" 2>/dev/null | sed 's#.*/##')"
  done
  echo "== i2c devices"
  for d in "$sys"/bus/i2c/devices/[0-9]* "$sys"/bus/i2c/devices/i2c-*; do
    [ -e "$d" ] && echo "${d##*/} name=$(cat "$d/name" 2>/dev/null) modalias=$(cat "$d/modalias" 2>/dev/null) driver=$(readlink "$d/driver" 2>/dev/null | sed 's#.*/##')"
  done
  echo "== media devices"
  for m in "$sys"/bus/media/devices/media*; do
    [ -e "$m" ] && echo "${m##*/} model=$(cat "$m/model" 2>/dev/null) path=$(readlink -f "$m" 2>/dev/null)"
  done
  return 0
}

gbos_report_camera_media_graph() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  vendor=$(cat "$sys/class/dmi/id/sys_vendor" 2>/dev/null)
  product=$(cat "$sys/class/dmi/id/product_name" 2>/dev/null)
  if [ "$vendor" != "Dell Inc." ] || [ "$product" != "Latitude 7320 Detachable" ]; then
    echo "== media topology capture (not Latitude 7320 Detachable; skipped)"
    return 0
  fi
  helper=${GBOS_CAMERA_MEDIA_GRAPH_BIN:-/postinstall/gbos/installer/bin/camera-media-graph}
  node=${GBOS_CAMERA_MEDIA_NODE:-/dev/media0}
  echo "== read-only media topology $node"
  if [ ! -x "$helper" ]; then
    echo "ERROR: media topology helper is missing or not executable: $helper"
    return 1
  fi
  if [ ! -e "$node" ]; then
    echo "ERROR: media device is missing: $node"
    return 1
  fi
  if "$helper" "$node"; then
    return 0
  else
    rc=$?
    echo "ERROR: media topology helper failed with status $rc"
    return "$rc"
  fi
}

gbos_report_camera_acpi_state() {
  sys=$1
  id=$2
  d="$sys/bus/acpi/devices/$id"
  echo "== ACPI device power state $id"
  if [ ! -e "$d" ]; then
    echo "(ACPI device unavailable)"
    return 0
  fi
  for f in path status modalias power_state power/runtime_status power/control power/wakeup; do
    [ -r "$d/$f" ] || continue
    case "$f" in
      power/runtime_status) key=runtime_status ;;
      power/control) key=power_control ;;
      power/wakeup) key=power_wakeup ;;
      *) key=$f ;;
    esac
    echo "acpi_$key=$(cat "$d/$f" 2>/dev/null)"
  done
  if [ -L "$d/physical_node" ]; then
    echo "acpi_physical_node=$(readlink "$d/physical_node")"
  fi
  acpi_supplier_found=0
  for link in "$d"/supplier:*; do
    [ -L "$link" ] || continue
    acpi_supplier_found=1
    echo "acpi_${link##*/}=$(readlink "$link")"
  done
  [ "$acpi_supplier_found" = 1 ] || echo "acpi_supplier_links=(none)"
  return 0
}

gbos_report_camera_probe_state() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  echo "== camera probe state"
  found=0
  for d in "$sys"/bus/i2c/devices/i2c-OVTI5678:* "$sys"/bus/i2c/devices/i2c-OVTI8856:*; do
    [ -e "$d" ] || continue
    found=1
    echo "${d##*/} driver=$(readlink "$d/driver" 2>/dev/null | sed 's#.*/##')"
    for f in modalias uevent power/runtime_status power/control; do
      if [ -r "$d/$f" ]; then
        echo "-- $f"
        cat "$d/$f"
      fi
    done
    if [ -r "$d/driver_override" ]; then
      echo "driver_override=$(cat "$d/driver_override")"
    fi
    for f in status modalias; do
      if [ -r "$d/firmware_node/$f" ]; then
        echo "firmware_node_$f=$(cat "$d/firmware_node/$f")"
      fi
    done
    if [ -L "$d/firmware_node/driver" ]; then
      echo "firmware_node_driver=$(readlink "$d/firmware_node/driver" | sed 's#.*/##')"
    fi
    for f in firmware_node physical_node; do
      for link in "$d"/$f; do
        [ -L "$link" ] && echo "${link##*/}=$(readlink "$link")"
      done
    done
    supplier_found=0
    for link in "$d"/supplier:*; do
      [ -L "$link" ] || continue
      supplier_found=1
      echo "${link##*/}=$(readlink "$link")"
    done
    [ "$supplier_found" = 1 ] || echo "supplier_links=(none)"
    case "${d##*/}" in
      i2c-OVTI8856:*)
        device_path=$(readlink -f "$d" 2>/dev/null)
        if [ -n "$device_path" ]; then
          echo "i2c_sysfs_path=$device_path"
          echo "i2c_parent_path=${device_path%/*}"
        fi
        gbos_report_camera_acpi_state "$sys" "${d##*/i2c-}"
        ;;
    esac
  done
  [ "$found" = 1 ] || echo "(no matching sensor I2C devices)"
  if [ -r "$sys/bus/i2c/drivers_autoprobe" ]; then
    echo "i2c drivers_autoprobe=$(cat "$sys/bus/i2c/drivers_autoprobe")"
  fi
  for d in "$sys"/bus/i2c/drivers/ov5675 "$sys"/bus/i2c/drivers/ov8856; do
    [ -d "$d" ] && echo "registered i2c driver=${d##*/}"
  done
  deferred="${GBOS_CAMERA_DEBUGFS:-$sys/kernel/debug}/devices_deferred"
  if [ -r "$deferred" ]; then
    echo "== camera-related deferred probes"
    if grep -iE 'OVTI5678|OVTI8856|INT3472|tps68470|ipu6' "$deferred"; then
      :
    else
      case $? in
        1) echo "(none reported)" ;;
        *) echo "ERROR: could not read deferred probes" ;;
      esac
    fi
  else
    echo "== deferred probes unavailable ($deferred)"
  fi
  return 0
}

gbos_report_camera_debug() (
  sys=${GBOS_SYSFS_ROOT:-/sys}
  if [ -r "$sys/kernel/debug/devices_deferred" ]; then
    gbos_report_camera_probe_state
    return
  fi
  tmp=$(mktemp -d "${GBOS_CAMERA_TMPDIR:-/dev}/gbos-camera-debug.XXXXXX") || {
    echo "ERROR: could not create temporary debugfs mount point"
    gbos_report_camera_probe_state
    return 1
  }
  mounted=0
  cleanup_camera_debug() {
    if [ "$mounted" = 1 ]; then
      umount "$tmp" || { echo "ERROR: could not unmount temporary camera debugfs at $tmp"; return 1; }
      mounted=0
    fi
    rmdir "$tmp" || { echo "ERROR: could not remove temporary camera debugfs directory $tmp"; return 1; }
  }
  trap 'cleanup_camera_debug' EXIT
  trap 'exit 1' HUP INT TERM
  if mount -t debugfs -o ro,nosuid,nodev,noexec debugfs "$tmp"; then
    mounted=1
    GBOS_CAMERA_DEBUGFS=$tmp gbos_report_camera_probe_state
  else
    echo "ERROR: could not mount temporary read-only debugfs"
    gbos_report_camera_probe_state
    exit 1
  fi
  cleanup_camera_debug || exit 1
  trap - EXIT
)

# Input devices and switches (tablet mode: which device reports SW_LID / SW_TABLET_MODE, the serio port atkbd is on,
# and what intel_hid resolved its tablet-switch mode to).
gbos_report_input() {
  sys=${GBOS_SYSFS_ROOT:-/sys}
  echo "== tablet switch: chassis_type=$(cat "$sys/class/dmi/id/chassis_type" 2>/dev/null)" \
       "intel_hid enable_sw_tablet_mode=$(cat "$sys/module/intel_hid/parameters/enable_sw_tablet_mode" 2>/dev/null || echo '(intel_hid not loaded)')" \
       "intel_vbtn=$(grep -q '^intel_vbtn ' "${GBOS_PROC_MODULES:-/proc/modules}" 2>/dev/null && echo loaded || echo 'not loaded')"
  echo "modprobe.d: $(grep -h intel_hid /etc/modprobe.d/*.conf 2>/dev/null | tr '\n' ';')"
  echo "ACPI: $(ls "$sys/bus/acpi/devices" 2>/dev/null | grep -E '^(INTC1051|INTC1070|INTC1078|INT33D6|INT33D5|PNP0C0D|PNP0C60)' | tr '\n' ' ')"
  echo "== input capabilities (input name: ev sw)"
  for i in "$sys"/class/input/input*; do
    [ -e "$i/name" ] || continue
    echo "${i##*/} $(cat "$i/name" 2>/dev/null): ev=$(cat "$i/capabilities/ev" 2>/dev/null) sw=$(cat "$i/capabilities/sw" 2>/dev/null)"
  done
  echo "== serio ports (port description driver bind_mode)"
  for p in "$sys"/bus/serio/devices/serio*; do
    [ -e "$p" ] || continue
    echo "${p##*/} $(cat "$p/description" 2>/dev/null) driver=$(readlink "$p/driver" 2>/dev/null | sed 's#.*/##') bind_mode=$(cat "$p/bind_mode" 2>/dev/null)"
  done
  echo "== intel_hid / intel_vbtn dmesg"
  dmesg 2>/dev/null | grep -iE 'intel[-_]hid|intel[-_]vbtn|INTC10[57][018]|INT33D6|switch event|5 button|platform supports|Intel HID' | tail -n 20
  echo "== /proc/bus/input/devices"
  cat "${GBOS_INPUT_DEVICES:-/proc/bus/input/devices}" 2>&1
}

gbos_hw_report() {
  [ -n "$L" ] || return 0
  {
    echo "== kernel"; uname -a
    echo "== cpu"; grep -m1 'model name' /proc/cpuinfo; for f in avx avx2 fma bmi2 sha_ni gfni avx_vnni avx512f; do grep -m1 -qw $f /proc/cpuinfo && echo "$f yes" || echo "$f NO"; done
    echo "== dmi"; for f in sys_vendor product_name product_version bios_version board_name; do echo "$f=$(cat /sys/class/dmi/id/$f 2>/dev/null)"; done
    echo "== pci devices without a driver (slot class vendor:device modalias)"
    for d in /sys/bus/pci/devices/*; do
      [ -e "$d/driver" ] || echo "${d##*/} $(cat "$d/class") $(cat "$d/vendor"):$(cat "$d/device") modalias=$(cat "$d/modalias" 2>/dev/null)"
    done
    echo "== usb devices (vid:pid product)"
    for d in /sys/bus/usb/devices/[0-9]*-[0-9.]*; do
      [ -f $d/idVendor ] && echo "$(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null) drv=$(for i in $d:*; do readlink $i/driver 2>/dev/null; done | sed 's#.*/##' | sort -u | tr '\n' ',')"
    done
    echo "== input"; grep -E '^(N: Name|H: Handlers)' /proc/bus/input/devices
    echo "== iio devices"
    for d in /sys/bus/iio/devices/iio:device*; do
      [ -e "$d" ] || continue
      echo "${d##*/} name=$(cat "$d/name" 2>/dev/null) device=$(readlink -f "$d/device" 2>/dev/null) driver=$(readlink "$d/device/driver" 2>/dev/null | sed 's#.*/##')"
      for a in "$d"/in_accel_*_raw "$d"/in_accel_*_scale; do [ -e "$a" ] && echo "  ${a##*/}"; done
    done
    echo "== HID sensor platform devices"
    for d in /sys/bus/platform/devices/HID-SENSOR-*; do
      [ -e "$d" ] && echo "${d##*/} modalias=$(cat "$d/modalias" 2>/dev/null) driver=$(readlink "$d/driver" 2>/dev/null | sed 's#.*/##')"
    done
    gbos_report_camera_bus_devices
    gbos_report_camera_probe_state
    echo "== video devices (cameras)"
    for v in /sys/class/video4linux/*; do
      [ -e "$v" ] && echo "${v##*/} $(cat "$v/name" 2>/dev/null) driver=$(readlink "$v/device/driver" 2>/dev/null | sed 's#.*/##')"
    done
    echo "== net";   ls /sys/class/net
    echo "== bluetooth"; ls /sys/class/bluetooth 2>&1
    echo "== backlight"; for b in /sys/class/backlight/*; do [ -e $b ] && echo "$b max=$(cat $b/max_brightness) cur=$(cat $b/brightness)"; done
    gbos_report_thermal
    # DRM cards and the driver bound to each, then the driver's own dmesg (amdgpu: ip blocks, firmware, ring tests, "[drm] Display Core").
    echo "== gpu (drm card: driver)"; for c in /sys/class/drm/card[0-9]*; do case ${c##*/} in card*-*) continue;; esac; [ -e $c ] && echo "${c##*/}: $(readlink $c/device/driver 2>/dev/null | sed 's#.*/##') $(cat $c/device/vendor $c/device/device 2>/dev/null | tr '\n' ' ')"; done
    echo "== gpu dmesg"; dmesg | grep -iE 'amdgpu|\[drm|drm:|i915|simpledrm|xe 0000|\*ERROR\*' | head -n 100
    echo "== sound"; cat /proc/asound/cards 2>&1
    # Codec id + PCI subsystem id are what an HDA speaker/mic quirk is keyed on; the dmesg lines say why a card is missing
    # (no machine driver, DSP firmware or topology not found, codec probe errors).
    echo "== sound codecs"
    for c in /proc/asound/card*/codec#*; do [ -f "$c" ] && { echo "--- $c"; grep -E '^(Codec|Address|Vendor Id|Subsystem Id|Revision Id|Default PCM|Node 0x[0-9a-f]+ \[Pin Complex\]|  Pin Default)' "$c" | head -n 40; }; done
    # "Pin Default" above is the BIOS value read from the codec; overrides from HDA_PINS and kernel quirks are only here.
    echo "== HDA pin overrides (HDA_PINS=${HDA_PINS:-unset})"
    for c in /sys/class/sound/hwC*D*; do [ -e $c/user_pin_configs ] && echo "${c##*/} user: $(tr '\n' ' ' < $c/user_pin_configs) driver: $(tr '\n' ' ' < $c/driver_pin_configs)"; done
    # PCM numbers + jack input devices are what usb/audio/gen_config.sh maps the audio HAL config from; the speaker/DAC nodes show
    # whether a codec fixup connected the second speaker pin (0x14) to the same DAC as 0x17.
    echo "== sound pcm"; cat /proc/asound/pcm 2>&1
    echo "== sound inputs"; grep '^N: Name' /proc/bus/input/devices 2>&1 | grep -iE 'sof|hda|pch|headphone|mic|hdmi'
    echo "== sound speaker nodes"
    for c in /proc/asound/card*/codec#*; do [ -f "$c" ] && { echo "--- $c"; awk '/^Node 0x(02|03|14|17|21) /{print; p=1; next} /^Node /{p=0} p' "$c" | head -n 80; }; done
    echo "== sound dmesg"; dmesg | grep -iE 'sof|snd_|hdaudio|hda_|hdac|dmic|tplg|topology|skl_hda|dspcfg|audio|tas2[0-9]+|TAS2XXX' | head -n 80
    echo "== soundwire"; for d in /sys/bus/soundwire/devices/*; do [ -e $d ] && echo "${d##*/} modalias=$(cat $d/modalias 2>/dev/null) driver=$(readlink $d/driver 2>/dev/null | sed 's#.*/##') status=$(cat $d/status 2>/dev/null)"; done
    echo "soundwire_intel sdw_link_skip_mask=$(cat /sys/module/soundwire_intel/parameters/sdw_link_skip_mask 2>/dev/null || echo '(no parameter)')"
    echo "== soundwire dmesg"; dmesg | grep -iE 'sdw|soundwire|rt1320|rt721|sdca|asoc|tplg' | grep -v find_sdca_function | tail -n 60
    echo "== rfkill"; for r in /sys/class/rfkill/*; do [ -e $r ] && echo "$(cat $r/name) $(cat $r/type) soft=$(cat $r/soft) hard=$(cat $r/hard)"; done
    echo "== missing firmware"; dmesg | grep -iE 'firmware.*(failed|not found|-2\))|Direct firmware load.*failed|failed to load.*(ucode|\.bin|\.sfi)' | sort -u
    echo "== MODOPTS"; echo "${MODOPTS:-(none)}"
    echo "== display (drm connectors: status, enabled, first modes)"
    for c in /sys/class/drm/card*-*; do
      [ -e $c/status ] && { echo "${c##*/} status=$(cat $c/status) enabled=$(cat $c/enabled 2>/dev/null)"; head -6 $c/modes 2>/dev/null | sed 's/^/    /'; }
    done
    for m in xe i915; do
      [ -d /sys/module/$m/parameters ] && { echo "== $m parameters"; for p in /sys/module/$m/parameters/*; do echo "${p##*/}=$(cat $p 2>/dev/null)"; done; }
    done
    echo "== driver messages (wifi, graphics, camera, sensors, audio, bluetooth; first 250)"
    dmesg | grep -iE 'iwlwifi|iwlmvm|iwlmld|\bxe [0-9a-f:.]+|\[drm|ipu[0-9]|intel-ipu|ivsc|camera|hid[_-]sensor|intel_ish|ishtp|iio|sof-audio|snd_sof|sof-firmware|soundwire|cs42l43|Bluetooth: hci|btintel|No config found|unsupported' | head -250
    gbos_report_input
    echo "== lsmod"; lsmod
  } > $L/hwdetect.txt 2>&1
  log "hwdetect: report written (hwdetect.txt)"
}

# HDA_PINS in gbos.cfg: override HD-audio pin defaults, as hdajackretask or a kernel HDA_FIXUP_PINS quirk does on Linux (issue 56).
# Entries are "[hwCxDy:]NID=CONFIG", space separated, e.g. HDA_PINS="0x14=0x411111f0 0x1b=0x90170110" (0x14 unused, 0x1b internal
# speaker). Without a hwCxDy: prefix an entry applies to every codec that is not an HDMI/DP one (Intel, AMD, NVIDIA vendor ids).
# The pins go to the codec's user_pin_configs and the codec is re-probed ("reconfig"), which only works while nothing has the card
# open: stage0, before Android's audio HAL starts. Needs CONFIG_SND_HDA_RECONFIG + CONFIG_SND_HDA_HWDEP (kernel/configure_whl.py).
# Tests: GBOS_HDA_SYS replaces /sys/class/sound, GBOS_HDA_STATE the marker directory; gbos_hda_write is the only sysfs write.
gbos_hda_write() { echo "$2" > "$1"; }
gbos_hda_entry_ok() { # NID=CONFIG, both numbers (CONFIG as 0x + 1-8 hex digits)
  _hn=${1%%=*}; _hc=${1#*=}
  [ "$_hn" != "$1" ] && [ -n "$_hn" ] || return 1
  case "$_hn" in *[!0-9a-fA-Fx]*) return 1 ;; esac
  case "$_hc" in 0x?*) ;; *) return 1 ;; esac
  case "${_hc#0x}" in *[!0-9a-fA-F]*|?????????*) return 1 ;; esac
}
gbos_hda_pins() {
  [ -n "$HDA_PINS" ] || return 0
  _hs=${GBOS_HDA_SYS:-/sys/class/sound}; _hm=${GBOS_HDA_STATE:-/dev}
  # The card probes asynchronously (SOF loads its DSP firmware first): wait up to 10 s for a codec node.
  _i=0; while [ $_i -lt 10 ] && ! ls -d $_hs/hwC*D* >/dev/null 2>&1; do sleep 1; _i=$((_i+1)); done
  ls -d $_hs/hwC*D* >/dev/null 2>&1 || { log "hwdetect: HDA_PINS set, but no HD-audio codec (hwC*D*) appeared"; return 0; }
  for _c in $_hs/hwC*D*; do
    _cn=${_c##*/}; _v=$(cat $_c/vendor_id 2>/dev/null); _n=0
    [ -e $_c/user_pin_configs ] || { log "hwdetect: HDA_PINS: $_cn has no user_pin_configs (kernel without SND_HDA_RECONFIG)"; continue; }
    # hwdetect runs twice per boot (stage0, then hook.sh): once is enough.
    [ "$(cat $_hm/.gbos_hda_pins.$_cn 2>/dev/null)" = "$HDA_PINS" ] && continue
    for _e in $HDA_PINS; do
      case "$_e" in
        hw*:*) [ "${_e%%:*}" = "$_cn" ] || continue; _e=${_e#*:} ;;
        *) case "$_v" in 0x8086*|0x1002*|0x10de*) continue ;; esac ;;
      esac
      gbos_hda_entry_ok "$_e" || { log "hwdetect: HDA_PINS: ignored '$_e' (want NID=CONFIG, e.g. 0x1b=0x90170110)"; continue; }
      if gbos_hda_write $_c/user_pin_configs "${_e%%=*} ${_e#*=}"; then _n=$((_n+1))
      else log "hwdetect: HDA_PINS: $_cn rejected pin ${_e%%=*} ${_e#*=}"; fi
    done
    [ $_n -gt 0 ] || continue
    if gbos_hda_write $_c/reconfig 1; then
      log "hwdetect: HDA_PINS: $_cn ($_v) reconfigured with $_n pin(s): $(tr '\n' ' ' < $_c/user_pin_configs 2>/dev/null)"
      echo "$HDA_PINS" > $_hm/.gbos_hda_pins.$_cn 2>/dev/null
    else log "hwdetect: HDA_PINS: $_cn reconfig FAILED (codec in use?)"; fi
  done
}

# Whole thing; $1 = seconds to wait for devices (default 15).
gbos_hwdetect() {
  [ -f $G/gbos.cfg ] && [ "$HWDETECT" = 0 ] && { log "hwdetect: disabled (HWDETECT=0)"; return 0; }
  [ -d $G/firmware ] && echo -n $G/firmware > $GBOS_FWPATH
  if gbos_modules_mount && gbos_modopts; then
    gbos_load_soundwire_codecs "before coldplug"
    gbos_coldplug && gbos_wait_devices ${1:-15}
  fi
  # The brightness keys/backlight need acpi_video when the panel has no native backlight.
  [ -d /sys/class/backlight ] && [ -z "$(ls /sys/class/backlight 2>/dev/null)" ] && modprobe -q video 2>/dev/null
  gbos_hda_pins
  gbos_hw_report
}
