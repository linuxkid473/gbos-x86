# gbOS86 stage0 screen reader (Speakup + espeakup). Sourced by stage0_init.sh when gbos/a11y/ is on the stick; stage0 has copied
# this folder to /a11y (RAM) and unmounted the stick, so everything runs from there. Never aborts the boot: every step may fail quietly.
# The kernel has Speakup built in with the "soft" synthesizer (speakup.synth=soft on the command line); espeakup connects it to
# espeak-ng, which plays through ALSA. Needs a sound card the kernel brings up by itself (legacy Intel HDA: most laptops).
# SOF-only machines have none at this point, so there is no speech there (logged as "a11y: no sound card").
# gbos.cfg: A11Y=0 turns it off; A11Y_MENU_TIMEOUT=N seconds for the boot menu (default 60 with speech).
A=/a11y
asr() { $A/lib/ld-musl-x86_64.so.1 --library-path $A/lib "$@"; }
a11y_start() {
  if [ ! -d /sys/accessibility/speakup ]; then log "a11y: this kernel has no Speakup, no speech"; return 1; fi
  mdev -s
  if [ ! -c /dev/softsynth ]; then
    _mm=$(cat /sys/class/misc/softsynth/dev 2>/dev/null)
    [ -n "$_mm" ] && mknod -m 660 /dev/softsynth c ${_mm%:*} ${_mm#*:} 2>/dev/null
  fi
  [ -c /dev/softsynth ] || { log "a11y: no /dev/softsynth, no speech"; return 1; }
  [ "$(cat /sys/accessibility/speakup/synth 2>/dev/null)" = soft ] || echo soft > /sys/accessibility/speakup/synth 2>/dev/null

  # The sound card: the first one that is not an HDMI/DisplayPort output (those come first on some machines).
  _c=; _n=0
  while [ $_n -lt 8 ] && [ -z "$_c" ]; do
    for _d in /proc/asound/card[0-9]*; do
      [ -d "$_d" ] || continue
      _id=$(cat $_d/id 2>/dev/null)
      case "$_id" in *HDMI*|*hdmi*|*NVidia*|*DP*) continue;; esac
      _c=${_d##*card}; break
    done
    [ -n "$_c" ] || { _n=$((_n+1)); sleep 1; }
  done
  [ -n "$_c" ] || { log "a11y: no sound card, no speech"; return 1; }
  mdev -s
  mkdir -p /etc
  # Straight to the card (no dmix: it needs SysV IPC and /tmp in this early environment); plug converts rate and format.
  printf 'pcm.!default { type plug; slave.pcm "hw:%s,0" }\nctl.!default { type hw; card %s }\n' $_c $_c > /etc/asound.conf
  export ALSA_CONFIG_PATH=$A/share/alsa/alsa.conf ESPEAK_DATA_PATH=$A/share
  # Laptops boot with the speakers muted or very quiet: unmute and raise every playback control (not the microphone ones).
  asr $A/bin/amixer -c $_c scontrols 2>/dev/null | sed -n "s/^Simple mixer control '\(.*\)',[0-9]*$/\1/p" | while read -r _k; do
    case "$_k" in *Capture*|*Mic*|*Beep*|*Boost*|*Auto-Mute*) continue;; esac
    asr $A/bin/amixer -q -c $_c sset "$_k" 85% unmute > /dev/null 2>&1
  done
  # Kernel messages must not be read aloud: the command line has ignore_loglevel, which beats the console level, so switch it off.
  # The log on the stick is unaffected (it reads /dev/kmsg).
  echo 0 > /sys/module/printk/parameters/ignore_loglevel 2>/dev/null
  dmesg -n 1 2>/dev/null
  # This espeakup has no working -P: it daemonizes and writes /var/run/espeakup.pid.
  mkdir -p /var/run
  asr $A/bin/espeakup -V en > $A/espeakup.log 2>&1
  _t=0; while [ $_t -lt 10 ] && [ ! -s /var/run/espeakup.pid ]; do sleep 1; _t=$((_t+1)); done
  if [ -s /var/run/espeakup.pid ]; then
    : > $A/active
    log "a11y: espeakup running (pid $(cat /var/run/espeakup.pid)) on sound card $_c ($(cat /proc/asound/card$_c/id 2>/dev/null))"
    return 0
  fi
  log "a11y: espeakup did not start: $(head -c 200 $A/espeakup.log 2>/dev/null)"
  return 1
}
a11y_start || true
