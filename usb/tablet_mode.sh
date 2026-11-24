#!/system/bin/sh
# Latitude 7320 Detachable: tablet mode helper. hook.sh starts it (service gbos_tablet_mode) after boot_completed,
# only on that DMI identity.
#   tablet_mode.sh LOG ROTATION KEYBOARD
#     ROTATION=1: keep "cmd window fixed-to-user-rotation disabled" applied. Google's
#       DisplayRotation$LaptopController.refreshConfig (services.jar) resets it to "default" (= fixed on a PC) on every
#       device-state callback outside tablet state: at boot (a race with a one-shot command, lost on boot-9 2026-11-18)
#       and after every lid or tablet-switch change. Each re-apply is logged.
#     KEYBOARD=ime: in tablet mode set Settings.Secure show_ime_with_hard_keyboard=1 for the current user, and restore the
#       previous value when the keyboard cover is attached again. The EC's i8042 "AT Translated Set 2 keyboard" exists
#       with the cover off too, so Android always sees a hardware keyboard and hides the on-screen one.
#     KEYBOARD=unbind: detach atkbd from its serio port in tablet mode instead, rebind on attach (that device then
#       disappears; whatever else the EC reports through it, e.g. hardware buttons, goes with it).
#     KEYBOARD=0: leave the keyboard alone.
# Tablet state comes from SW_TABLET_MODE (intel_hid "Intel HID switches", enable_sw_tablet_mode=2 from hwdetect.sh).
# Change detection: one "getevent -c 1" per switch device as a wake-up (getevent's stdout is block-buffered in a pipe,
# so its event lines cannot be streamed), then the authoritative state from "getevent -qS"; a slow loop reconciles
# anything missed between wake-ups. Without a switch device only the rotation part runs.
# GBOS_TABLET_MODE_LIB=1 only defines the functions (tools/test_tablet_mode.sh).

TM_OUT=${1:-/postinstall/gbos/logs/tablet-mode.txt}
TM_ROTATION=${2:-0}
TM_KEYBOARD=${3:-ime}
TM_STATE=${GBOS_TABLET_STATE_DIR:-/data/local/tmp/gbos-tablet}
TM_RUN=${GBOS_TABLET_RUN_DIR:-/dev/gbos-tablet}
TM_INPUT_DEVICES=${GBOS_INPUT_DEVICES:-/proc/bus/input/devices}
TM_SYS=${GBOS_SYSFS_ROOT:-/sys}
TM_POLL=${GBOS_TABLET_POLL:-30}
TM_FAST_POLLS=${GBOS_TABLET_FAST_POLLS:-30}

tm_log() { echo "$(date '+%H:%M:%S') $*" >> "$TM_OUT"; }

# Switch-capable input devices: "/dev/input/eventN kinds name", kinds = lid,tablet / lid / tablet / other.
# B: SW= is a hex bitmap; SW_LID = bit 0, SW_TABLET_MODE = bit 1, so the last hex digit is enough.
tm_switch_devices() {
  awk '
    function flush() {
      if (h != "" && sw != "") {
        c = substr(sw, length(sw), 1); k = ""
        if (index("13579bdfBDF", c)) k = "lid"
        if (index("2367abefABEF", c)) k = (k == "" ? "tablet" : k ",tablet")
        if (k == "") k = "other"
        print "/dev/input/" h, k, name
      }
      h = ""; sw = ""; name = ""
    }
    /^I:/ { flush() }
    /^N: Name=/ { name = substr($0, 9) }
    /^H: Handlers=/ { s = $0; sub(/^H: Handlers=/, "", s); n = split(s, f, " "); for (i = 1; i <= n; i++) if (f[i] ~ /^event[0-9]+$/) h = f[i] }
    /^B: SW=/ { sw = substr($0, 7); sub(/[ \t]+$/, "", sw); m = split(sw, w, " "); sw = w[m] }
    END { flush() }
  ' "$TM_INPUT_DEVICES" 2>/dev/null
}

tm_tablet_device() { tm_switch_devices | awk '$2 ~ /tablet/ { print $1; exit }'; }

# 1 (tablet), 0 (cover attached) or "unknown". getevent -qS prints "<device>: <hex switch bits>".
tm_tablet_state() {
  _dev=${1:-$(tm_tablet_device)}
  [ -n "$_dev" ] || { echo unknown; return 0; }
  _bits=$(getevent -qS "$_dev" 2>/dev/null | sed -n 's/^.*:[[:space:]]*\([0-9a-fA-F]\{1,8\}\)[[:space:]]*$/\1/p' | tail -n 1)
  [ -n "$_bits" ] || { echo unknown; return 0; }
  case $(printf '%s' "$_bits" | tail -c 1) in
    2|3|6|7|a|b|e|f|A|B|E|F) echo 1 ;;
    *) echo 0 ;;
  esac
}

tm_device_state() {
  timeout 10 dumpsys device_state 2>&1 | grep -E 'mCommittedState|mBaseState|mPendingState' | sed 's/^[[:space:]]*//' | tr '\n' ' '
}

# ROTATION=1: re-apply the display-0 policy whenever it is not "disabled". $1 = why we looked.
tm_rotation_check() {
  [ "$TM_ROTATION" = 1 ] || return 0
  _mode=$(echo $(cmd window fixed-to-user-rotation 2>&1))
  [ "$_mode" = disabled ] && return 0
  if cmd window fixed-to-user-rotation disabled >/dev/null 2>&1; then
    tm_log "rotation: fixed-to-user-rotation was '$_mode' ($1); re-applied disabled, now '$(echo $(cmd window fixed-to-user-rotation 2>&1))'; user-rotation: $(echo $(cmd window user-rotation 2>&1))"
  else
    tm_log "rotation: ERROR: fixed-to-user-rotation was '$_mode' ($1) and cmd window fixed-to-user-rotation disabled failed"
    return 1
  fi
}

tm_ime_get() { settings --user current get secure show_ime_with_hard_keyboard 2>/dev/null; }

tm_keyboard_tablet() {
  case "$TM_KEYBOARD" in
    ime)
      if [ ! -f "$TM_STATE/ime_saved" ]; then
        tm_ime_get > "$TM_STATE/ime_saved" || tm_log "keyboard: ERROR: cannot save show_ime_with_hard_keyboard"
      fi
      if settings --user current put secure show_ime_with_hard_keyboard 1; then
        tm_log "keyboard: tablet: show_ime_with_hard_keyboard=1 (saved '$(cat "$TM_STATE/ime_saved" 2>/dev/null)')"
      else
        tm_log "keyboard: ERROR: settings put show_ime_with_hard_keyboard 1 failed"
      fi
      ;;
    unbind)
      for _p in "$TM_SYS"/bus/serio/devices/serio*; do
        [ -e "$_p/drvctl" ] || continue
        case $(readlink "$_p/driver" 2>/dev/null) in */atkbd) ;; *) continue ;; esac
        # serio compares with strncmp(buf, "none", count): a trailing newline makes the write fail (EINVAL).
        printf manual > "$_p/bind_mode" 2>/dev/null
        if printf none > "$_p/drvctl" 2>/dev/null; then
          grep -qx "${_p##*/}" "$TM_STATE/atkbd_ports" 2>/dev/null || echo "${_p##*/}" >> "$TM_STATE/atkbd_ports"
          tm_log "keyboard: tablet: atkbd detached from ${_p##*/} ($(cat "$_p/description" 2>/dev/null))"
        else
          tm_log "keyboard: ERROR: cannot detach atkbd from ${_p##*/}"
        fi
      done
      ;;
  esac
}

# Also runs at start-up in laptop state, so a reboot while in tablet mode cannot leave the setting changed.
tm_keyboard_laptop() {
  if [ -f "$TM_STATE/ime_saved" ]; then
    _v=$(cat "$TM_STATE/ime_saved" 2>/dev/null)
    case "$_v" in
      0|1) settings --user current put secure show_ime_with_hard_keyboard "$_v" ;;
      *) settings --user current delete secure show_ime_with_hard_keyboard >/dev/null ;;
    esac && rm -f "$TM_STATE/ime_saved" &&
      tm_log "keyboard: laptop: show_ime_with_hard_keyboard restored to '${_v:-null}'"
  fi
  if [ -f "$TM_STATE/atkbd_ports" ]; then
    _left=
    for _n in $(cat "$TM_STATE/atkbd_ports"); do
      _p=$TM_SYS/bus/serio/devices/$_n
      if [ -e "$_p/driver" ] || printf atkbd > "$_p/drvctl" 2>/dev/null; then
        printf auto > "$_p/bind_mode" 2>/dev/null
        tm_log "keyboard: laptop: atkbd bound to $_n"
      else
        _left="$_left $_n"
        tm_log "keyboard: ERROR: cannot bind atkbd to $_n"
      fi
    done
    if [ -n "$_left" ]; then echo $_left | tr ' ' '\n' > "$TM_STATE/atkbd_ports"; else rm -f "$TM_STATE/atkbd_ports"; fi
  fi
  return 0
}

# Act only on a change of the switch state (the last applied one lives in $TM_RUN, a per-boot tmpfs). $1 = why.
tm_apply_mode() {
  _st=$(tm_tablet_state "$TM_DEV")
  [ "$_st" = unknown ] && return 1
  _last=$(cat "$TM_RUN/mode" 2>/dev/null) || _last=
  [ "$_st" = "$_last" ] && return 1
  echo "$_st" > "$TM_RUN/mode"
  tm_log "switch: SW_TABLET_MODE=$_st (was ${_last:-unset}, $1); device_state: $(tm_device_state)"
  if [ "$_st" = 1 ]; then tm_keyboard_tablet; else tm_keyboard_laptop; fi
  return 0
}

# After a switch change Android commits the new device state and LaptopController runs about a second later.
tm_after_event() {
  tm_apply_mode "$1"
  for _d in 1 2 3; do
    sleep "$_d"
    tm_rotation_check "$1 +${_d}s"
  done
}

tm_report() {
  echo "Latitude 7320 tablet mode helper: $(date '+%Y-%m-%d %H:%M:%S %z') rotation=$TM_ROTATION keyboard=$TM_KEYBOARD"
  echo "== chassis_type=$(cat "$TM_SYS/class/dmi/id/chassis_type" 2>/dev/null) intel_hid enable_sw_tablet_mode=$(cat "$TM_SYS/module/intel_hid/parameters/enable_sw_tablet_mode" 2>/dev/null || echo '(intel_hid not loaded)')"
  echo "== switch devices (device kinds name)"; tm_switch_devices
  echo "== getevent -S (device: switch bits; SW_LID=1, SW_TABLET_MODE=2)"; getevent -qS 2>&1
  echo "== dumpsys device_state"; timeout 10 dumpsys device_state 2>&1 | head -n 60
  echo "== settings"
  echo "show_ime_with_hard_keyboard=$(tm_ime_get)"
  echo "fixed-to-user-rotation=$(echo $(cmd window fixed-to-user-rotation 2>&1)) user-rotation=$(echo $(cmd window user-rotation 2>&1))"
  echo "config.override_forced_orient=$(getprop config.override_forced_orient)"
  echo "== events"
}

tm_watch() {
  while true; do
    getevent -qc 1 "$1" > /dev/null 2>&1 || { tm_log "watch: getevent on $1 failed; stopped watching it"; return 1; }
    tm_after_event "event on $1"
  done
}

tm_main() {
  mkdir -p "${TM_OUT%/*}" "$TM_STATE" "$TM_RUN" || exit 1
  tm_report >> "$TM_OUT" 2>&1
  TM_DEV=$(tm_tablet_device)
  if [ -n "$TM_DEV" ]; then
    tm_apply_mode "start" || tm_log "switch: start state $(tm_tablet_state "$TM_DEV") on $TM_DEV"
  else
    tm_log "switch: no SW_TABLET_MODE device (intel_hid switches missing: see hwdetect.txt); keyboard handling off"
    tm_keyboard_laptop
  fi
  tm_rotation_check "start"
  for _w in $(tm_switch_devices | awk '{ print $1 }'); do
    tm_watch "$_w" &
  done
  # Fast at first (LaptopController's boot-time reset arrived 1.4 s after boot_completed on boot-9), then slow.
  _i=0
  while true; do
    if [ "$_i" -lt "$TM_FAST_POLLS" ]; then sleep 2; _i=$((_i + 1)); else sleep "$TM_POLL"; fi
    [ -n "$TM_DEV" ] && tm_apply_mode "poll"
    tm_rotation_check "poll"
  done
}

[ "${GBOS_TABLET_MODE_LIB:-0}" = 1 ] || tm_main
