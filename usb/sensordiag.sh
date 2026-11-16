#!/system/bin/sh
# Capture the live Android sensor HAL configuration, IIO access/data, and framework enumeration.
OUT=${1:-/postinstall/gbos/logs/sensors.txt}
IIO_SYSFS=${SENSORDIAG_IIO_SYSFS:-/sys/bus/iio/devices}
IIO_DEV=${SENSORDIAG_DEV_DIR:-/dev}
HID_SYSFS=${SENSORDIAG_HID_SYSFS:-/sys/bus/hid/devices}
CFG=${SENSORDIAG_CFG:-/postinstall/gbos/gbos.cfg}

probe_accel() (
  d=$1
  dev="$IIO_DEV/${d##*/}"
  channels="in_accel_x in_accel_y in_accel_z in_timestamp"
  changed=0
  tmp=
  cleanup() {
    result=0
    if [ "$changed" = 1 ]; then
      failed=0
      echo 0 > "$d/buffer/enable" || failed=1
      for channel in $channels; do
        echo 0 > "$d/scan_elements/${channel}_en" || failed=1
      done
      if [ "$failed" = 1 ]; then
        echo "ERROR: failed to restore IIO controls for $d"
        result=1
      else
        echo "IIO controls restored"
      fi
    fi
    [ -z "$tmp" ] || rm -f "$tmp"
    return "$result"
  }
  trap 'cleanup || exit 1' EXIT
  trap 'exit 1' HUP INT TERM
  for command in timeout dd wc tr od date mktemp; do
    command -v "$command" >/dev/null 2>&1 || { echo "ERROR: missing $command"; exit 1; }
  done
  [ -c "$dev" ] || { echo "ERROR: $dev is not an IIO character device"; exit 1; }
  [ "$(cat "$d/buffer/enable" 2>/dev/null)" = 0 ] || {
    echo "SKIP: IIO buffer is active or unreadable"; exit 1;
  }
  for channel in $channels; do
    [ "$(cat "$d/scan_elements/${channel}_en" 2>/dev/null)" = 0 ] || {
      echo "SKIP: $channel is active or unreadable"; exit 1;
    }
  done
  for axis in x y z; do
    [ "$(cat "$d/scan_elements/in_accel_${axis}_type" 2>/dev/null)" = 'le:s32/32>>0' ] || {
      echo "ERROR: unsupported accelerometer scan type for $axis"; exit 1;
    }
  done
  [ "$(cat "$d/scan_elements/in_timestamp_type" 2>/dev/null)" = 'le:s64/64>>0' ] || {
    echo "ERROR: unsupported timestamp scan type"; exit 1;
  }
  i=0
  for channel in $channels; do
    [ "$(cat "$d/scan_elements/${channel}_index" 2>/dev/null)" = "$i" ] || {
      echo "ERROR: unexpected scan index for $channel"; exit 1;
    }
    i=$((i + 1))
  done
  tmp=$(mktemp "${TMPDIR:-/data/local/tmp}/gbos-iio-sample.XXXXXX") || {
    echo "ERROR: cannot create temporary sample file"; exit 1;
  }
  changed=1
  for channel in $channels; do
    echo 1 > "$d/scan_elements/${channel}_en" || { echo "ERROR: cannot enable $channel"; exit 1; }
  done
  echo 1 > "$d/buffer/enable" || { echo "ERROR: cannot enable IIO buffer"; exit 1; }
  echo "Before sample: realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime)"
  echo "One scan (24 bytes, little-endian x/y/z, padding, timestamp):"
  timeout 8 dd if="$dev" of="$tmp" bs=24 count=1 2>&1 || {
    echo "ERROR: IIO sample read failed or timed out"; exit 1;
  }
  sample_size=$(wc -c < "$tmp" | tr -d '[:space:]')
  [ "$sample_size" = 24 ] || {
    echo "ERROR: incomplete IIO sample ($sample_size of 24 bytes)"; exit 1;
  }
  od -An -v -tx1 -N 24 "$tmp" || { echo "ERROR: cannot format IIO sample"; exit 1; }
  echo "After sample: realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime)"
)

report_accel_sampling_settings() {
  for f in "$d"/in_accel_sampling_frequency "$d"/in_accel_sampling_frequency_available \
           "$d"/sampling_frequency "$d"/sampling_frequency_available \
           "$d"/buffer/hwfifo_timeout "$d"/buffer/hwfifo_enabled "$d"/buffer/length \
           "$d"/current_timestamp_clock; do
    if [ -f "$f" ]; then
      echo "$f: $(cat "$f" 2>&1)"
    else
      echo "$f: (missing)"
    fi
  done
}

probe_accel_motion() (
  d=$1
  mode=${2:-phases}
  dev="$IIO_DEV/${d##*/}"
  channels="in_accel_x in_accel_y in_accel_z in_timestamp"
  changed=0
  failed=0
  phase_tmp=
  input_tmp=
  input_err=
  input_dir=
  input_pid=
  filter_pid=
  cleanup() {
    result=0
    if [ -n "$input_pid" ]; then
      kill "$input_pid" 2>/dev/null || true
      wait "$input_pid" 2>/dev/null || true
    fi
    if [ -n "$filter_pid" ]; then
      kill "$filter_pid" 2>/dev/null || true
      wait "$filter_pid" 2>/dev/null || true
    fi
    if [ "$changed" = 1 ]; then
      failed_restore=0
      echo 0 > "$d/buffer/enable" || failed_restore=1
      for channel in $channels; do
        echo 0 > "$d/scan_elements/${channel}_en" || failed_restore=1
      done
      if [ "$failed_restore" = 1 ]; then
        echo "ERROR: failed to restore IIO controls for $d"
        result=1
      else
        echo "IIO controls restored"
      fi
    fi
    [ -z "$phase_tmp" ] || rm -f "$phase_tmp"
    [ -z "$input_tmp" ] || rm -f "$input_tmp"
    [ -z "$input_err" ] || rm -f "$input_err"
    if [ -n "$input_dir" ]; then
      rm -f "$input_dir/events"
      rmdir "$input_dir" || result=1
    fi
    return "$result"
  }
  trap 'cleanup || exit 1' EXIT
  trap 'exit 1' HUP INT TERM
  for command in timeout dd wc tr od date mktemp; do
    command -v "$command" >/dev/null 2>&1 || { echo "ERROR: missing $command"; exit 1; }
  done
  if [ "$mode" = phases ]; then
    for command in mkfifo awk sleep getevent; do
      command -v "$command" >/dev/null 2>&1 || { echo "ERROR: missing $command"; exit 1; }
    done
  fi
  [ -c "$dev" ] || { echo "ERROR: $dev is not an IIO character device"; exit 1; }
  [ "$(cat "$d/buffer/enable" 2>/dev/null)" = 0 ] || {
    echo "SKIP: IIO buffer is active or unreadable"; exit 1;
  }
  for channel in $channels; do
    [ "$(cat "$d/scan_elements/${channel}_en" 2>/dev/null)" = 0 ] || {
      echo "SKIP: $channel is active or unreadable"; exit 1;
    }
  done
  for axis in x y z; do
    [ "$(cat "$d/scan_elements/in_accel_${axis}_type" 2>/dev/null)" = 'le:s32/32>>0' ] || {
      echo "ERROR: unsupported accelerometer scan type for $axis"; exit 1;
    }
  done
  [ "$(cat "$d/scan_elements/in_timestamp_type" 2>/dev/null)" = 'le:s64/64>>0' ] || {
    echo "ERROR: unsupported timestamp scan type"; exit 1;
  }
  i=0
  for channel in $channels; do
    [ "$(cat "$d/scan_elements/${channel}_index" 2>/dev/null)" = "$i" ] || {
      echo "ERROR: unexpected scan index for $channel"; exit 1;
    }
    i=$((i + 1))
  done
  if [ "$mode" = continuous ]; then
    echo "MOTION_CONTINUOUS sampling settings before enabling scan channels:"
    report_accel_sampling_settings
  fi
  changed=1
  for channel in $channels; do
    echo 1 > "$d/scan_elements/${channel}_en" || { echo "ERROR: cannot enable $channel"; exit 1; }
  done
  echo 1 > "$d/buffer/enable" || { echo "ERROR: cannot enable IIO buffer"; exit 1; }
  if [ "$mode" = continuous ]; then
    echo "MOTION_CONTINUOUS sampling settings with buffer enabled:"
    report_accel_sampling_settings
    duration=${SENSORDIAG_CONTINUOUS_SECONDS:-180}
    count=${SENSORDIAG_CONTINUOUS_MAX_SCANS:-4000}
    case "$duration" in ''|*[!0-9]*) echo "ERROR: invalid continuous trace limits"; exit 1;; esac
    case "$count" in ''|*[!0-9]*) echo "ERROR: invalid continuous trace limits"; exit 1;; esac
    [ "$duration" -gt 0 ] && [ "$count" -gt 0 ] || {
      echo "ERROR: invalid continuous trace limits"; exit 1;
    }
    phase_tmp=$(mktemp "${TMPDIR:-/data/local/tmp}/gbos-iio-continuous.XXXXXX") || {
      echo "ERROR: cannot create temporary continuous trace"; exit 1;
    }
    echo "MOTION_CONTINUOUS start realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime) duration=${duration}s max_scans=$count"
    dd_status=0
    timeout "$duration" dd if="$dev" of="$phase_tmp" bs=24 count="$count" 2>&1 || dd_status=$?
    echo "MOTION_CONTINUOUS read end realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime)"
    sample_size=$(wc -c < "$phase_tmp" | tr -d '[:space:]')
    case "$sample_size" in ''|*[!0-9]*) echo "ERROR: invalid continuous sample size"; exit 1;; esac
    if [ "$sample_size" -eq 0 ] || [ $((sample_size % 24)) -ne 0 ]; then
      echo "ERROR: incomplete continuous IIO scans ($sample_size bytes)"; exit 1
    fi
    case "$dd_status" in 0|124) ;; *) echo "ERROR: continuous IIO read failed (status $dd_status)"; exit 1;; esac
    echo "MOTION_CONTINUOUS samples: $((sample_size / 24)) scans, $sample_size bytes, dd_status=$dd_status"
    [ "$dd_status" != 0 ] || echo "WARNING: continuous trace reached its scan limit before timeout"
    od -An -v -tx1 "$phase_tmp" || { echo "ERROR: cannot format continuous samples"; exit 1; }
    echo "MOTION_CONTINUOUS end realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime)"
    exit 0
  fi
  delay=${SENSORDIAG_MOTION_DELAY_SECONDS:-60}
  phase_seconds=${SENSORDIAG_MOTION_PHASE_SECONDS:-10}
  case "$delay" in ''|*[!0-9]*) echo "ERROR: invalid motion probe delay"; exit 1;; esac
  case "$phase_seconds" in ''|*[!0-9]*) echo "ERROR: invalid motion phase duration"; exit 1;; esac
  input_tmp=$(mktemp "${TMPDIR:-/data/local/tmp}/gbos-iio-input.XXXXXX") || {
    echo "ERROR: cannot create temporary input event file"; exit 1;
  }
  input_err=$(mktemp "${TMPDIR:-/data/local/tmp}/gbos-iio-input-error.XXXXXX") || {
    echo "ERROR: cannot create temporary input error file"; exit 1;
  }
  input_dir=$(mktemp -d "${TMPDIR:-/data/local/tmp}/gbos-iio-input-pipe.XXXXXX") || {
    echo "ERROR: cannot create temporary input pipe directory"; exit 1;
  }
  mkfifo "$input_dir/events" || {
    echo "ERROR: cannot create input event pipe"; exit 1;
  }
  echo "MOTION_INPUT start realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime)"
  getevent -qlt > "$input_dir/events" 2> "$input_err" &
  input_pid=$!
  (
    while IFS= read -r line; do
      case "$line" in
        *EV_KEY*KEY_VOLUMEUP*" DOWN"|*EV_KEY*KEY_VOLUMEUP*" UP"|\
        *EV_KEY*KEY_SPACE*" DOWN"|*EV_KEY*KEY_SPACE*" UP") printf '%s\n' "$line";;
      esac
    done < "$input_dir/events" > "$input_tmp"
  ) &
  filter_pid=$!
  echo "Motion pose trace starts in ${delay}s; six ${phase_seconds}-second phases follow"
  sleep "$delay"
  phase=0
  for pose in \
    'portrait, screen toward you, top edge up' \
    'landscape clockwise, screen toward you, top edge toward your right' \
    'portrait, screen toward you, top edge down' \
    'landscape counter-clockwise, screen toward you, top edge toward your left' \
    'flat, screen facing the ceiling' \
    'flat, screen facing the floor; hold securely over a soft surface and do not set it down'; do
    phase=$((phase + 1))
    phase_start=$(awk '{printf "%.0f\n", $1 * 1000}' /proc/uptime) || {
      echo "ERROR: cannot read uptime"; exit 1;
    }
    echo "MOTION_PHASE $phase/6 start realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime); pose=$pose"
    phase_tmp=$(mktemp "${TMPDIR:-/data/local/tmp}/gbos-iio-phase.XXXXXX") || {
      echo "ERROR: cannot create temporary phase file"; exit 1;
    }
    dd_status=0
    timeout 8 dd if="$dev" of="$phase_tmp" bs=24 count=400 2>&1 || dd_status=$?
    sample_size=$(wc -c < "$phase_tmp" | tr -d '[:space:]')
    case "$sample_size" in
      ''|*[!0-9]*) echo "ERROR: invalid motion sample size"; rm -f "$phase_tmp"; exit 1;;
    esac
    if [ "$sample_size" -eq 0 ] || [ $((sample_size % 24)) -ne 0 ]; then
      echo "ERROR: incomplete IIO scans in phase $phase ($sample_size bytes)"
      rm -f "$phase_tmp"
      exit 1
    fi
    echo "MOTION_PHASE $phase/6 samples: $((sample_size / 24)) scans, $sample_size bytes, dd_status=$dd_status"
    od -An -v -tx1 "$phase_tmp" || {
      echo "ERROR: cannot format motion samples"; rm -f "$phase_tmp"; exit 1;
    }
    rm -f "$phase_tmp"
    phase_tmp=
    phase_elapsed=$(awk -v start="$phase_start" '{printf "%.0f\n", $1 * 1000 - start}' /proc/uptime) || {
      echo "ERROR: cannot read phase duration"; exit 1;
    }
    phase_target=$((phase_seconds * 1000))
    if [ "$phase_elapsed" -lt "$phase_target" ]; then
      phase_remaining=$((phase_target - phase_elapsed))
      sleep "$(awk -v ms="$phase_remaining" 'BEGIN { printf "%.3f\n", ms / 1000 }')"
    fi
  done
  input_stopped=0
  if kill "$input_pid" 2>/dev/null; then input_stopped=1; fi
  input_status=0; wait "$input_pid" || input_status=$?
  input_pid=
  filter_status=0; wait "$filter_pid" || filter_status=$?
  filter_pid=
  if [ "$input_stopped" != 1 ]; then
    echo "ERROR: getevent exited early (status $input_status)"
    cat "$input_err"
  fi
  echo "MOTION_INPUT end realtime=$(date '+%s.%N') uptime=$(cat /proc/uptime) stopped=$input_stopped status=$input_status"
  if [ "$filter_status" != 0 ]; then
    echo "ERROR: input marker filter failed (status $filter_status)"
  fi
  echo "MOTION_INPUT markers (physical key events, not verified pose labels):"
  cat "$input_tmp"
  if ! grep -Eq 'KEY_(VOLUMEUP|SPACE)[[:space:]]+DOWN' "$input_tmp"; then
    echo "WARNING: no volume-up or space keypress marker captured"
  fi
)

{
  echo "== identity"
  id
  echo "== Opt-in accelerometer motion trace"
  if [ ! -r "$CFG" ]; then
    echo "SKIP: configuration unavailable or unreadable: $CFG"
  elif ! grep -q '^SENSOR_MOTION_PROBE=1$' "$CFG" && ! grep -q '^SENSOR_CONTINUOUS_PROBE=1$' "$CFG"; then
    echo "SKIP: SENSOR_MOTION_PROBE=1 not set"
  else
    count=0
    for d in "$IIO_SYSFS"/iio:device*; do
      [ -e "$d" ] || continue
      [ "$(cat "$d/name" 2>/dev/null)" = accel_3d ] || continue
      count=$((count + 1))
      accel=$d
    done
    if [ "$count" = 1 ]; then
      if grep -q '^SENSOR_CONTINUOUS_PROBE=1$' "$CFG"; then
        probe_accel_motion "$accel" continuous || echo "ERROR: accelerometer motion trace did not complete"
      else
        probe_accel_motion "$accel" || echo "ERROR: accelerometer motion trace did not complete"
      fi
    else
      echo "SKIP: expected exactly one accelerometer, found $count"
    fi
  fi
  echo "== sensor HAL configs"
  for f in /vendor/etc/sensors/hals.conf /odm/etc/sensors/hals.conf /product/etc/sensors/hals.conf; do
    if [ -f "$f" ]; then
      echo "--- $f"
      cat "$f"
    else
      echo "--- $f (missing)"
    fi
  done
  echo "== experimental sensor library"
  ls -lZ /vendor/lib64/android.hardware.sensors@2.1-trogdor-subhal.so 2>&1
  echo "== IIO character devices"
  ls -lZ "$IIO_DEV"/iio* 2>&1
  echo "== IIO devices, labels, permissions, and live values"
  for d in "$IIO_SYSFS"/iio:device*; do
    [ -e "$d" ] || continue
    echo "--- $d name=$(cat "$d/name" 2>/dev/null)"
    ls -ldZ "$d" "$d"/in_* "$d"/buffer 2>&1
    echo "-- IIO controls permissions (buffered HAL writes)"
    ls -lZ "$d/buffer/enable" "$d/buffer/length" "$d"/scan_elements/*_en \
      "$d"/scan_elements/in_timestamp_en "$d"/in_accel_sampling_frequency 2>&1
    for f in "$d"/in_accel_* "$d"/in_gyro_* "$d"/in_rot_* "$d"/in_anglvel_* \
             "$d"/in_magn_* "$d"/sampling_frequency "$d"/buffer/enable; do
      [ -f "$f" ] || continue
      echo "$f: $(cat "$f" 2>&1)"
    done
    echo "-- scan, buffer, trigger, and calibration attributes"
    for f in "$d"/scan_elements/* "$d"/buffer/enable "$d"/buffer/length \
             "$d"/buffer/watermark "$d"/buffer/hwfifo_timeout \
             "$d"/trigger/current_trigger "$d"/current_timestamp_clock "$d"/sampling_frequency \
             "$d"/sampling_frequency_available "$d"/in_accel_*_calibbias \
             "$d"/in_gyro_*_calibbias; do
      [ -f "$f" ] || continue
      echo "$f: $(cat "$f" 2>&1)"
    done
  done
  echo "== IIO trigger devices"
  for d in "$IIO_SYSFS"/trigger*; do
    [ -e "$d" ] || continue
    echo "--- $d"
    ls -ldZ "$d" 2>&1
    [ ! -f "$d/name" ] || echo "$d/name: $(cat "$d/name" 2>&1)"
  done
  echo "== Opt-in accelerometer buffered-event probe"
  if [ ! -r "$CFG" ]; then
    echo "SKIP: configuration unavailable or unreadable: $CFG"
  elif ! grep -q '^SENSOR_EVENT_PROBE=1$' "$CFG"; then
    echo "SKIP: SENSOR_EVENT_PROBE=1 not set"
  else
    count=0
    for d in "$IIO_SYSFS"/iio:device*; do
      [ -e "$d" ] || continue
      [ "$(cat "$d/name" 2>/dev/null)" = accel_3d ] || continue
      count=$((count + 1))
      accel=$d
    done
    if [ "$count" = 1 ]; then
      probe_accel "$accel" || echo "ERROR: accelerometer probe did not complete"
    else
      echo "SKIP: expected exactly one accelerometer, found $count"
    fi
  fi
  echo "== Intel ISH HID report descriptors (up to 4096 bytes each)"
  for d in "$HID_SYSFS"/001F:8087:0AC2.* "$HID_SYSFS"/001F:8087:0AC3.*; do
    [ -e "$d" ] || continue
    echo "--- $d"
    if [ -r "$d/report_descriptor" ]; then
      od -An -v -tx1 -N 4096 "$d/report_descriptor" 2>&1
    else
      echo "report_descriptor unavailable or unreadable"
    fi
  done
  echo "== IIO uevent rules"
  for f in /vendor/ueventd.rc /vendor/etc/ueventd.rc /odm/ueventd.rc /odm/etc/ueventd.rc; do
    [ -f "$f" ] && { echo "--- $f"; grep -iE 'iio|sensor' "$f"; }
  done
  echo "== sensor processes"
  ps -AZ 2>&1 | grep -iE 'sensor|multihal'
  echo "== sensor binder services"
  service list 2>&1 | grep -i sensor
  echo "== framework sensor enumeration"
  dumpsys sensorservice 2>&1
  echo "== sensor and IIO logcat"
  logcat -d -b all -v threadtime 2>&1 \
    | grep -iE 'sensor|iio|accelerometer|orientationeventlistener|avc:.*(sysfs|iio)' \
    | tail -n 500
} > "$OUT" 2>&1
