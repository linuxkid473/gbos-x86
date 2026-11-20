#!/system/bin/sh
# sleep-guard.sh: Lid & Sleep Guard + Power Profile Daemon for Surface Laptop 7 (Intel Lunar Lake) on gbOS86
# Manages lid close / open events, prevents unhandled s2idle sleep freezes,
# and dynamically optimizes Intel P-state / EPP power profiles.

# Exit immediately on non-Surface hardware
if ! grep -qi 'Surface' /sys/class/dmi/id/product_name 2>/dev/null; then
    exit 0
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
