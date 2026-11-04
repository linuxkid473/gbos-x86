# gbOS86 stage0 screen reader (Speakup + espeakup). Sourced by stage0_init.sh when gbos/a11y/ is on the stick; stage0 has copied
# this folder to /a11y (RAM) and unmounted the stick, so everything runs from there. Never aborts the boot: every step may fail quietly.
# The kernel has Speakup built in with the "soft" synthesizer (speakup.synth=soft on the command line); espeakup connects it to
# espeak-ng, which plays through ALSA. Needs a sound card the kernel brings up by itself (legacy Intel HDA: most laptops).
# SOF-only machines have none at this point, so there is no speech there (logged as "a11y: no sound card").
# gbos.cfg: A11Y=0 turns it off; A11Y_MENU_TIMEOUT=N seconds for the boot menu (default 60 with speech).
A=/a11y
asr() { $A/lib/ld-musl-x86_64.so.1 --library-path $A/lib "$@"; }
# Sets _c to the first sound card that is not an HDMI/DisplayPort one, waiting up to $1 seconds for it.
a11y_find_card() {
  _c=; _n=0
  while [ -z "$_c" ]; do
    for _d in /proc/asound/card[0-9]*; do
      [ -d "$_d" ] || continue
      case "$(cat $_d/id 2>/dev/null)" in *HDMI*|*hdmi*|*NVidia*|*DP*) continue;; esac
      _c=${_d##*card}; return 0
    done
    _n=$((_n+1)); [ $_n -gt $1 ] && return 1
    sleep 1
  done
}
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
  a11y_find_card 3
  if [ -z "$_c" ]; then
    # Newer Intel laptops (Raptor Lake, ...): the HDA controller (class 0403) cannot bind until the GPU driver is loaded, because the
    # display codecs share it ("couldn't bind with audio component"). Stage0 loads the GPU driver only after the boot menu, so load it
    # now, exactly as hwdetect does later (by the GPU's modalias, so xe or i915 as the hardware needs). The firmware comes from the stick.
    _def=; _gpu=
    for _d in /sys/bus/pci/devices/*; do
      case "$(cat $_d/class 2>/dev/null)" in
        0x0403*) [ -e $_d/driver ] || _def=1;;
        0x03*) _gpu="$_gpu $(cat $_d/modalias 2>/dev/null)";;
      esac
    done
    if [ -n "$_def" ] && [ -n "$_gpu" ] && [ -n "$STICK" ] && mount -t vfat -o ro $STICK /gbos 2>/dev/null; then
      if grep -q '^LOAD_I915=0' /gbos/gbos/gbos.cfg 2>/dev/null; then
        log "a11y: LOAD_I915=0, not loading the GPU driver for the sound card"
      else
        log "a11y: sound controller waits for the GPU driver, loading it now ($_gpu)"
        echo -n /gbos/gbos/firmware > /sys/module/firmware_class/parameters/path 2>/dev/null
        modprobe -a $_gpu > /dev/null 2>&1
        a11y_find_card 25
        echo -n > /sys/module/firmware_class/parameters/path 2>/dev/null
      fi
      umount /gbos 2>/dev/null || umount -l /gbos 2>/dev/null
    fi
  fi
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
