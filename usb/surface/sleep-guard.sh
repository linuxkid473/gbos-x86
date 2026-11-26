#!/system/bin/sh
# sleep-guard.sh: Lid & Sleep Guard + Power Profile Daemon for gbOS86
# Prevents unhandled kernel s2idle / ACPI S3 freezes by holding a kernel wake_lock while letting Android
# sleep/Doze normally (DPMS Off + 0 backlight + min CPU freq / C-states) on lid close or idle timeout, and
# cleanly restores CPU frequencies, un-inhibits input devices, and wakes display + backlight on lid reopen.

if ! grep -qi 'Surface' /sys/class/dmi/id/product_name 2>/dev/null; then
  echo "gbos_nos2idle" > /sys/power/wake_lock 2>/dev/null
  settings put global lid_behavior 1 2>/dev/null

  # Recover Wi-Fi / Bluetooth if a previous session lost power while radios were paused during lid-closed sleep.
  RADIO_STATE_FILE="/data/local/tmp/.gbos_sleep_radios"
  if [ -f "$RADIO_STATE_FILE" ]; then
    grep -q '^wifi=1$' "$RADIO_STATE_FILE" 2>/dev/null && { settings put global wifi_on 1 2>/dev/null; svc wifi enable 2>/dev/null; }
    grep -q '^bt=1$' "$RADIO_STATE_FILE" 2>/dev/null && { settings put global bluetooth_on 1 2>/dev/null; svc bluetooth enable 2>/dev/null; }
    rm -f "$RADIO_STATE_FILE" 2>/dev/null
  fi

  LID_SHUTDOWN_MIN=60
  if [ -r /postinstall/gbos/gbos.cfg ]; then
    _cfg_min=$(sed -n 's/^LID_SHUTDOWN_MIN=\([0-9][0-9]*\).*/\1/p' /postinstall/gbos/gbos.cfg 2>/dev/null | tail -n 1)
    [ -n "$_cfg_min" ] && LID_SHUTDOWN_MIN=$_cfg_min
  fi
  LID_SHUTDOWN_SEC=$(( LID_SHUTDOWN_MIN * 60 ))
  RADIO_PAUSE_SEC=60

  BL_DIR=""
  for _b in /sys/class/backlight/intel_backlight /sys/class/backlight/*; do
    [ -d "$_b" ] && [ -f "$_b/brightness" ] && { BL_DIR="$_b"; break; }
  done
  MAX_BRT=937
  [ -n "$BL_DIR" ] && [ -r "$BL_DIR/max_brightness" ] && read -r MAX_BRT < "$BL_DIR/max_brightness" 2>/dev/null
  case "$MAX_BRT" in ''|*[!0-9]*|0) MAX_BRT=937 ;; esac
  MIN_SAVE=$(( MAX_BRT / 20 + 1 ))
  saved_brt=$(( MAX_BRT * 60 / 100 ))
  [ "$saved_brt" -lt "$MIN_SAVE" ] && saved_brt=$MIN_SAVE

  DPMS_FILE=""
  for _dp in /sys/class/drm/card*-eDP-*/dpms /sys/class/drm/card*-LVDS-*/dpms /sys/class/drm/card*-DSI-*/dpms; do
    [ -r "$_dp" ] && { DPMS_FILE="$_dp"; break; }
  done

  LID_FILES=""
  for _lf in /proc/acpi/button/lid/*/state; do
    [ -r "$_lf" ] && LID_FILES="$LID_FILES $_lf"
  done

  MIN_KHZ=""; MAX_KHZ=""; WAKE_GOV=""; WAKE_EPP=""; WAKE_MAX_PCT="100"; WAKE_SATA_LPM="max_performance"
  [ -r /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq ] && read -r MIN_KHZ < /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq 2>/dev/null
  [ -r /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq ] && read -r MAX_KHZ < /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null
  [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ] && read -r WAKE_GOV < /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
  [ -r /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference ] && read -r WAKE_EPP < /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null
  [ -r /sys/devices/system/cpu/intel_pstate/max_perf_pct ] && read -r WAKE_MAX_PCT < /sys/devices/system/cpu/intel_pstate/max_perf_pct 2>/dev/null
  for _sh in /sys/class/scsi_host/host*/link_power_management_policy; do
    [ -r "$_sh" ] && { read -r WAKE_SATA_LPM < "$_sh" 2>/dev/null; break; }
  done
  [ -z "$WAKE_GOV" ] && WAKE_GOV="performance"
  [ -z "$WAKE_EPP" ] && WAKE_EPP="performance"
  [ -z "$WAKE_MAX_PCT" ] && WAKE_MAX_PCT="100"
  [ -z "$WAKE_SATA_LPM" ] && WAKE_SATA_LPM="max_performance"

  is_on_battery() {
    _has_bat=0
    for _ps in /sys/class/power_supply/*; do
      [ -r "$_ps/type" ] || continue
      read -r _pst < "$_ps/type" 2>/dev/null || continue
      case "$_pst" in
        Battery)
          _has_bat=1
          if [ -r "$_ps/status" ]; then
            read -r _bst < "$_ps/status" 2>/dev/null || _bst=""
            case "$_bst" in Charging) return 1 ;; esac
          fi
          ;;
        Mains|UPS|USB*)
          if [ -r "$_ps/online" ]; then
            read -r _onl < "$_ps/online" 2>/dev/null || _onl="0"
            [ "$_onl" = "1" ] && return 1
          fi
          ;;
      esac
    done
    [ "$_has_bat" = 1 ] && return 0
    return 1
  }

  usb_hid_awake_restore() {
    for _u in /sys/bus/usb/devices/*; do
      [ -w "$_u/power/control" ] || continue
      case "${_u##*/}" in *:*) continue ;; esac
      _is_hid=0
      _is_sensor=0
      for _if in "$_u":*; do
        [ -e "$_if/driver" ] || continue
        case "$(readlink "$_if/driver" 2>/dev/null)" in */usbhid) _is_hid=1 ;; esac
        for _hd in "$_if"/0003:*; do
          [ -e "$_hd/driver" ] || continue
          case "$(readlink "$_hd/driver" 2>/dev/null)" in */hid-sensor-hub) _is_sensor=1 ;; esac
        done
      done
      if [ "$_is_hid" = 1 ] && [ "$_is_sensor" = 0 ]; then
        echo on > "$_u/power/control" 2>/dev/null
      else
        echo auto > "$_u/power/control" 2>/dev/null
      fi
    done
  }

  set_generic_sleep_cpu() {
    if [ "$1" = "sleep" ]; then
      for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo powersave > "$g" 2>/dev/null; done
      for e in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do [ -w "$e" ] && echo power > "$e" 2>/dev/null; done
      [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
      [ -w /sys/devices/system/cpu/intel_pstate/max_perf_pct ] && echo 25 > /sys/devices/system/cpu/intel_pstate/max_perf_pct 2>/dev/null
      if [ -n "$MIN_KHZ" ]; then
        for m in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do echo "$MIN_KHZ" > "$m" 2>/dev/null; done
      fi
      [ -w /sys/firmware/acpi/platform_profile ] && echo low-power > /sys/firmware/acpi/platform_profile 2>/dev/null
      [ -w /sys/module/snd_hda_intel/parameters/power_save ] && echo 1 > /sys/module/snd_hda_intel/parameters/power_save 2>/dev/null
      for _sh in /sys/class/scsi_host/host*/link_power_management_policy; do
        [ -w "$_sh" ] && echo med_power_with_dipm > "$_sh" 2>/dev/null
      done
    else
      [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
      [ -w /sys/devices/system/cpu/intel_pstate/max_perf_pct ] && echo "$WAKE_MAX_PCT" > /sys/devices/system/cpu/intel_pstate/max_perf_pct 2>/dev/null
      if [ -n "$MAX_KHZ" ]; then
        for m in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do echo "$MAX_KHZ" > "$m" 2>/dev/null; done
      fi
      for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo "$WAKE_GOV" > "$g" 2>/dev/null; done
      for e in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do [ -w "$e" ] && echo "$WAKE_EPP" > "$e" 2>/dev/null; done
      [ -w /sys/firmware/acpi/platform_profile ] && echo balanced > /sys/firmware/acpi/platform_profile 2>/dev/null
      [ -w /sys/module/snd_hda_intel/parameters/power_save ] && echo 0 > /sys/module/snd_hda_intel/parameters/power_save 2>/dev/null
      for _sh in /sys/class/scsi_host/host*/link_power_management_policy; do
        [ -w "$_sh" ] && echo "$WAKE_SATA_LPM" > "$_sh" 2>/dev/null
      done
    fi
  }

  wifi_paused=0
  bt_paused=0
  radios_checked=0
  paused_logcat_pid=""
  deep_lid_sleeping=0
  lid_closed_bat_since=""

  pause_radios_for_lid() {
    radios_checked=1
    _won=$(settings get global wifi_on 2>/dev/null | tr -d ' \r\n')
    _bon=$(settings get global bluetooth_on 2>/dev/null | tr -d ' \r\n')
    if [ "$_won" = "1" ] && [ "$wifi_paused" = 0 ]; then
      wifi_paused=1
    fi
    if [ "$_bon" = "1" ] && [ "$bt_paused" = 0 ]; then
      bt_paused=1
    fi
    if [ "$wifi_paused" = 1 ] || [ "$bt_paused" = 1 ]; then
      printf 'wifi=%s\nbt=%s\n' "$wifi_paused" "$bt_paused" > "$RADIO_STATE_FILE" 2>/dev/null
      [ "$wifi_paused" = 1 ] && svc wifi disable 2>/dev/null
      [ "$bt_paused" = 1 ] && svc bluetooth disable 2>/dev/null
    fi
  }

  restore_paused_radios() {
    if [ "$wifi_paused" = 1 ]; then
      settings put global wifi_on 1 2>/dev/null
      svc wifi enable 2>/dev/null
      wifi_paused=0
    fi
    if [ "$bt_paused" = 1 ]; then
      settings put global bluetooth_on 1 2>/dev/null
      svc bluetooth enable 2>/dev/null
      bt_paused=0
    fi
    radios_checked=0
    rm -f "$RADIO_STATE_FILE" 2>/dev/null
  }

  set_deep_lid_power() {
    if [ "$1" = "enter" ]; then
      [ "$deep_lid_sleeping" = 1 ] && return 0
      [ -n "$BL_DIR" ] && [ -w "$BL_DIR/bl_power" ] && echo 4 > "$BL_DIR/bl_power" 2>/dev/null
      # Allow all USB devices & root hubs to autosuspend while lid is closed (unblocks Intel Package C6/C7)
      for _uc in /sys/bus/usb/devices/*/power/control; do
        [ -w "$_uc" ] && echo auto > "$_uc" 2>/dev/null
      done
      # Enable PCI runtime PM on peripherals (skip storage 0x01*, GPU 0x03*, and bridges 0x06*)
      for _pd in /sys/bus/pci/devices/*; do
        [ -r "$_pd/class" ] && [ -w "$_pd/power/control" ] || continue
        read -r _pcls < "$_pd/class" 2>/dev/null || continue
        case "$_pcls" in
          0x01*|0x03*|0x06*) continue ;;
          *) echo auto > "$_pd/power/control" 2>/dev/null ;;
        esac
      done
      # Enter Android Deep Doze so background services/alarms stop waking the CPU
      dumpsys deviceidle force-idle 2>/dev/null || cmd deviceidle force-idle deep 2>/dev/null
      # Flush logs once, then pause periodic disk writers so SATA enters DIPM Slumber
      sync 2>/dev/null
      stop gbos_sync 2>/dev/null
      paused_logcat_pid=""
      for _p in /proc/[0-9]*/cmdline; do
        [ -r "$_p" ] || continue
        if tr '\000' ' ' < "$_p" 2>/dev/null | grep -q 'logcat .*-b all .*/postinstall/gbos/logs/'; then
          _pid=${_p#/proc/}; _pid=${_pid%/cmdline}
          kill -STOP "$_pid" 2>/dev/null && paused_logcat_pid="$_pid"
          break
        fi
      done
      deep_lid_sleeping=1
    else
      [ "$deep_lid_sleeping" = 0 ] && return 0
      usb_hid_awake_restore
      dumpsys deviceidle unforce 2>/dev/null
      cmd deviceidle unforce 2>/dev/null
      dumpsys deviceidle step 2>/dev/null
      restore_paused_radios
      if [ -n "$paused_logcat_pid" ]; then
        kill -CONT "$paused_logcat_pid" 2>/dev/null
        paused_logcat_pid=""
      fi
      start gbos_sync 2>/dev/null
      deep_lid_sleeping=0
      lid_closed_bat_since=""
    fi
  }

  last_lid="open"
  cpu_sleeping=0

  while true; do
    lid_state="open"
    for _lf in $LID_FILES; do
      [ -r "$_lf" ] || continue
      read -r _lsl < "$_lf" 2>/dev/null || continue
      case "$_lsl" in *closed*|*CLOSED*) lid_state="closed"; break ;; esac
    done

    if [ "$lid_state" = "closed" ]; then
      if [ "$last_lid" != "closed" ]; then
        dpms_state="On"
        [ -n "$DPMS_FILE" ] && [ -r "$DPMS_FILE" ] && read -r dpms_state < "$DPMS_FILE" 2>/dev/null
        [ "$dpms_state" != "Off" ] && { cmd power sleep 2>/dev/null || input keyevent 223 2>/dev/null; }
        last_lid="closed"
      fi
      if [ "$cpu_sleeping" = 0 ]; then
        set_generic_sleep_cpu sleep
        cpu_sleeping=1
      fi
      set_deep_lid_power enter

      if is_on_battery; then
        read -r _up_s _ < /proc/uptime 2>/dev/null || _up_s=0
        now_s=${_up_s%%.*}
        case "$now_s" in ''|*[!0-9]*) now_s=0 ;; esac
        [ -z "$lid_closed_bat_since" ] && lid_closed_bat_since=$now_s
        bat_elapsed=$(( now_s - lid_closed_bat_since ))
        if [ "$bat_elapsed" -ge "$RADIO_PAUSE_SEC" ] && [ "$radios_checked" = 0 ]; then
          pause_radios_for_lid
        fi
        if [ "$LID_SHUTDOWN_SEC" -gt 0 ] && [ "$bat_elapsed" -ge "$LID_SHUTDOWN_SEC" ]; then
          [ -n "$paused_logcat_pid" ] && { kill -CONT "$paused_logcat_pid" 2>/dev/null; paused_logcat_pid=""; }
          [ "$wifi_paused" = 1 ] && settings put global wifi_on 1 2>/dev/null
          [ "$bt_paused" = 1 ] && settings put global bluetooth_on 1 2>/dev/null
          rm -f "$RADIO_STATE_FILE" 2>/dev/null
          echo "<6>sleep-guard: lid closed on battery for ${LID_SHUTDOWN_MIN}m, shutting down cleanly" > /dev/kmsg 2>/dev/null
          sync 2>/dev/null
          svc power shutdown 2>/dev/null || reboot -p 2>/dev/null || poweroff 2>/dev/null
          sleep 25
          # Fallback only if lid is still closed and Android ShutdownThread hung
          for _lf in $LID_FILES; do
            read -r _lsl < "$_lf" 2>/dev/null || continue
            case "$_lsl" in *closed*|*CLOSED*) sync 2>/dev/null; reboot -p 2>/dev/null || poweroff -f 2>/dev/null ;; esac
          done
        fi
      else
        # Plugged into AC: cancel shutdown countdown and restore radios if they were paused on battery
        if [ -n "$lid_closed_bat_since" ]; then
          lid_closed_bat_since=""
          restore_paused_radios
        fi
      fi

      sleep 2
      continue
    fi

    dpms_state="On"
    [ -n "$DPMS_FILE" ] && [ -r "$DPMS_FILE" ] && read -r dpms_state < "$DPMS_FILE" 2>/dev/null

    if [ -n "$BL_DIR" ] && [ -r "$BL_DIR/brightness" ]; then
      read -r cur_brt < "$BL_DIR/brightness" 2>/dev/null || cur_brt=""
      case "$cur_brt" in
        ''|*[!0-9]*) ;;
        *)
          if [ "$cur_brt" -ge "$MIN_SAVE" ] && [ "$cur_brt" -le "$MAX_BRT" ]; then
            saved_brt=$cur_brt
          fi
          ;;
      esac
    fi

    if [ "$last_lid" = "closed" ]; then
      # Lid just reopened: restore CPU clocks, wake display + backlight immediately, then exit Doze & restore radios.
      set_generic_sleep_cpu wake
      cpu_sleeping=0
      for inh in /sys/class/input/input*/inhibited; do
        [ -w "$inh" ] && echo 0 > "$inh" 2>/dev/null
      done
      if [ -n "$BL_DIR" ]; then
        [ -w "$BL_DIR/bl_power" ] && echo 0 > "$BL_DIR/bl_power" 2>/dev/null
      fi
      cmd power wakeup 2>/dev/null || input keyevent 224 2>/dev/null
      if [ -n "$BL_DIR" ] && [ -r "$BL_DIR/brightness" ]; then
        read -r cur_brt < "$BL_DIR/brightness" 2>/dev/null || cur_brt="0"
        [ "${cur_brt:-0}" = "0" ] && echo "$saved_brt" > "$BL_DIR/brightness" 2>/dev/null
      fi
      set_deep_lid_power exit
      last_lid="open"
    elif [ "$dpms_state" = "Off" ]; then
      # Lid is open, screen timed out to idle sleep: throttle CPU/SATA/HDA while keeping wake inputs & Wi-Fi active.
      if [ "$cpu_sleeping" = 0 ]; then
        set_generic_sleep_cpu sleep
        cpu_sleeping=1
      fi
    else
      # Lid is open and display is On: ensure CPU clocks and backlight are restored after idle wakeup.
      if [ "$cpu_sleeping" = 1 ]; then
        set_generic_sleep_cpu wake
        cpu_sleeping=0
        for inh in /sys/class/input/input*/inhibited; do
          [ -w "$inh" ] && echo 0 > "$inh" 2>/dev/null
        done
      fi
      if [ -n "$BL_DIR" ]; then
        [ -w "$BL_DIR/bl_power" ] && echo 0 > "$BL_DIR/bl_power" 2>/dev/null
        read -r cur_brt < "$BL_DIR/brightness" 2>/dev/null || cur_brt="0"
        [ "${cur_brt:-0}" = "0" ] && echo "$saved_brt" > "$BL_DIR/brightness" 2>/dev/null
      fi
    fi

    sleep 1
  done
fi

PROFILE_FILE="/data/local/tmp/gbos_surface_power_profile"
KSU_PROFILE_FILE="/data/adb/modules/gbos_surface_touchpad_fix/active_profile"

if [ -f "$KSU_PROFILE_FILE" ]; then
    PROFILE_FILE="$KSU_PROFILE_FILE"
elif [ ! -f "$PROFILE_FILE" ]; then
    echo "balance_performance" > "$PROFILE_FILE" 2>/dev/null
fi
chmod 0666 "$PROFILE_FILE" 2>/dev/null

# Ensure sysfs nodes are readable by non-root Android apps
chmod 0644 /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference 2>/dev/null
chmod 0644 /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq 2>/dev/null
chmod 0644 /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq 2>/dev/null
chmod 0644 /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null
chmod 0644 /sys/devices/system/cpu/intel_pstate/* 2>/dev/null
chmod 0644 /sys/firmware/acpi/platform_profile 2>/dev/null
chmod 0644 /sys/class/power_supply/*/capacity /sys/class/power_supply/*/status /sys/class/power_supply/*/power_now /sys/class/power_supply/*/current_now /sys/class/power_supply/*/voltage_now 2>/dev/null

apply_cpu_profile() {
  mode="$1"
  case "$mode" in
    power)
      gov="powersave"
      epp="power"
      max_freq="1200000"
      no_turbo="1"
      max_pct="30"
      plat="low-power"
      ;;
    balance_power)
      gov="powersave"
      epp="balance_power"
      max_freq="2200000"
      no_turbo="1"
      max_pct="55"
      plat="balanced"
      ;;
    balance_performance)
      gov="powersave"
      epp="balance_performance"
      max_freq="3800000"
      no_turbo="0"
      max_pct="85"
      plat="balanced"
      ;;
    performance)
      gov="performance"
      epp="performance"
      max_freq="4900000"
      no_turbo="0"
      max_pct="100"
      plat="performance"
      ;;
    *)
      gov="powersave"
      epp="balance_performance"
      max_freq="3800000"
      no_turbo="0"
      max_pct="85"
      plat="balanced"
      ;;
  esac

  # 1. Set scaling_governor
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo "$gov" > "$g" 2>/dev/null
  done

  # 2. Set frequency cap and turbo state
  if [ "$no_turbo" = "0" ]; then
    echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
    echo "$max_pct" > /sys/devices/system/cpu/intel_pstate/max_perf_pct 2>/dev/null
  fi
  for m in /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq; do
    echo "$max_freq" > "$m" 2>/dev/null
  done
  if [ "$no_turbo" = "1" ]; then
    echo "$max_pct" > /sys/devices/system/cpu/intel_pstate/max_perf_pct 2>/dev/null
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
  fi

  # 3. Set energy_performance_preference
  for e in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
    echo "$epp" > "$e" 2>/dev/null
  done

  if [ -w /sys/firmware/acpi/platform_profile ]; then
    echo "$plat" > /sys/firmware/acpi/platform_profile 2>/dev/null
  fi
}

sync_app_desired_files() {
  target_prof="$1"
  for pkg_dir in /data/user/10/com.gbos.surfacepower /data/user/0/com.gbos.surfacepower; do
    [ -d "$pkg_dir" ] || continue
    app_dir="$pkg_dir/files"
    mkdir -p "$app_dir" 2>/dev/null
    owner=$(stat -c "%u:%g" "$pkg_dir" 2>/dev/null)
    req_file="$app_dir/desired_profile.txt"
    echo "$target_prof" > "$req_file" 2>/dev/null
    [ -n "$owner" ] && chown "$owner" "$app_dir" "$req_file" 2>/dev/null
    chcon --reference="$pkg_dir" "$app_dir" "$req_file" 2>/dev/null
    chmod 0777 "$app_dir" 2>/dev/null
    chmod 0666 "$req_file" 2>/dev/null
  done
}

echo "gbos_nos2idle" > /sys/power/wake_lock 2>/dev/null
settings put global lid_behavior 1 2>/dev/null

# Check if User 10 already requested a profile before daemon restart
if [ -f /data/user/10/com.gbos.surfacepower/files/desired_profile.txt ]; then
  u10_prof=$(cat /data/user/10/com.gbos.surfacepower/files/desired_profile.txt 2>/dev/null | tr -d ' \r\n')
  case "$u10_prof" in
    power|balance_power|balance_performance|performance)
      echo "$u10_prof" > "$PROFILE_FILE" 2>/dev/null
      ;;
  esac
fi

active_profile=$(cat "$PROFILE_FILE" 2>/dev/null | tr -d ' \r\n')
[ -z "$active_profile" ] && active_profile="balance_performance"
sync_app_desired_files "$active_profile"
apply_cpu_profile "$active_profile"
last_applied_profile="$active_profile"

last_lid="open"
saved_brt=26786

while true; do
  echo "gbos_nos2idle" > /sys/power/wake_lock 2>/dev/null

  # Check if companion app requested a profile change
  for pkg_dir in /data/user/10/com.gbos.surfacepower /data/user/0/com.gbos.surfacepower; do
    [ -d "$pkg_dir" ] || continue
    app_dir="$pkg_dir/files"
    if [ ! -d "$app_dir" ]; then
      mkdir -p "$app_dir" 2>/dev/null
      owner=$(stat -c "%u:%g" "$pkg_dir" 2>/dev/null)
      [ -n "$owner" ] && chown "$owner" "$app_dir" 2>/dev/null
      chcon --reference="$pkg_dir" "$app_dir" 2>/dev/null
      chmod 0777 "$app_dir" 2>/dev/null
    fi
    req_file="$app_dir/desired_profile.txt"
    if [ -f "$req_file" ]; then
      req=$(cat "$req_file" 2>/dev/null | tr -d ' \r\n')
      case "$req" in
        power|balance_power|balance_performance|performance)
          if [ "$req" != "$active_profile" ]; then
            active_profile="$req"
            echo "$active_profile" > "$PROFILE_FILE" 2>/dev/null
            sync_app_desired_files "$active_profile"
            break
          fi
          ;;
      esac
    else
      sync_app_desired_files "$active_profile"
    fi
  done

  lid_state="open"
  if grep -q "closed" /proc/acpi/button/lid/*/state 2>/dev/null; then
    lid_state="closed"
  fi
  dpms_state=$(cat /sys/class/drm/card0-eDP-1/dpms 2>/dev/null)
  cur_brt=$(cat /sys/class/backlight/intel_backlight/brightness 2>/dev/null)
  if [ -n "$cur_brt" ] && [ "$cur_brt" -gt 100 ] 2>/dev/null; then
    saved_brt=$cur_brt
  fi

  if [ "$lid_state" = "closed" ] || [ "$dpms_state" = "Off" ]; then
    if [ "$last_lid" != "closed" ]; then
      apply_cpu_profile "power"
      last_applied_profile="power"
      last_lid="closed"
    fi
  else
    if [ "$last_lid" = "closed" ]; then
      apply_cpu_profile "$active_profile"
      last_applied_profile="$active_profile"
      for inh in /sys/class/input/input*/inhibited; do
        [ -w "$inh" ] && echo 0 > "$inh" 2>/dev/null
      done
      cmd power wakeup 2>/dev/null
      echo 0 > /sys/class/backlight/intel_backlight/bl_power 2>/dev/null
      cur_brt=$(cat /sys/class/backlight/intel_backlight/brightness 2>/dev/null)
      if [ "$cur_brt" = "0" ]; then
        echo "$saved_brt" > /sys/class/backlight/intel_backlight/brightness 2>/dev/null
      fi
      last_lid="open"
    else
      cur_sys_epp=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null | tr -d ' \r\n')
      if [ "$active_profile" != "$last_applied_profile" ] || [ "$cur_sys_epp" != "$active_profile" ]; then
        apply_cpu_profile "$active_profile"
        last_applied_profile="$active_profile"
      fi
    fi
  fi

  # Live telemetry export
  cur_epp=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null | tr -d ' \r\n')
  cur_gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null | tr -d ' \r\n')
  cur_freq_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null | tr -d ' \r\n')
  cur_max_khz=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null | tr -d ' \r\n')
  cur_freq_mhz=$(( ${cur_freq_khz:-0} / 1000 ))
  cap_freq_mhz=$(( ${cur_max_khz:-0} / 1000 ))
  max_freq_mhz=0
  sum_freq_mhz=0
  cnt_cores=0
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
    fk=$(cat "$f" 2>/dev/null | tr -d ' \r\n')
    fm=$(( ${fk:-0} / 1000 ))
    if [ "$fm" -gt 0 ] 2>/dev/null; then
      sum_freq_mhz=$(( sum_freq_mhz + fm ))
      cnt_cores=$(( cnt_cores + 1 ))
      [ "$fm" -gt "$max_freq_mhz" ] 2>/dev/null && max_freq_mhz=$fm
    fi
  done
  if [ "$cnt_cores" -gt 0 ]; then
    avg_freq_mhz=$(( sum_freq_mhz / cnt_cores ))
  else
    avg_freq_mhz=$cur_freq_mhz
  fi
  bat_cap=$(cat /sys/class/power_supply/BAT*/capacity 2>/dev/null | head -n 1 | tr -d ' \r\n')
  bat_stat=$(cat /sys/class/power_supply/BAT*/status 2>/dev/null | head -n 1 | tr -d ' \r\n')

  for pkg_dir in /data/user/10/com.gbos.surfacepower /data/user/0/com.gbos.surfacepower; do
    app_dir="$pkg_dir/files"
    [ -d "$app_dir" ] || continue
    owner=$(stat -c "%u:%g" "$pkg_dir" 2>/dev/null)
    stat_tmp="$app_dir/status.prop.tmp"
    stat_file="$app_dir/status.prop"
    cat << EOF_STAT > "$stat_tmp" 2>/dev/null
active_profile=$active_profile
applied_epp=$cur_epp
governor=$cur_gov
cpu0_freq_mhz=$avg_freq_mhz
max_core_freq_mhz=$max_freq_mhz
scaling_max_freq_mhz=$cap_freq_mhz
lid_state=$lid_state
sleep_guard=active
battery_capacity=${bat_cap:-100}
battery_status=${bat_stat:-Unknown}
EOF_STAT
    [ -n "$owner" ] && chown "$owner" "$stat_tmp" 2>/dev/null
    chcon --reference="$pkg_dir" "$stat_tmp" 2>/dev/null
    chmod 0666 "$stat_tmp" 2>/dev/null
    mv -f "$stat_tmp" "$stat_file" 2>/dev/null
  done

  sleep 1
done
