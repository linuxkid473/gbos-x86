#!/system/bin/sh
# mixdiag.sh N: sound diagnostics of boot-N, run by the gbos_audiodiag service 60 s after boot. Writes logs/boot-N/mixer.txt:
# the ALSA mixer controls of card 0, the UCM / SoundWire files in the image, and the card tree. Uses the optional static
# audio/tinymix when present, otherwise the bundled a11y amixer and musl loader.
N=$1
G=${GBOS_AUDIO_DIAG_ROOT:-/postinstall/gbos}
L=${GBOS_AUDIO_DIAG_LOG_ROOT:-$G/logs/boot-$N}
T=$G/audio/tinymix
A=$G/a11y
{
  echo "== mixer controls (card 0)"
  if [ -x "$T" ]; then
    "$T" -D 0 contents
  elif [ -x "$A/lib/ld-musl-x86_64.so.1" ] && [ -x "$A/bin/amixer" ]; then
    echo "(using bundled amixer)"
    ALSA_CONFIG_DIR="$A/share/alsa" \
    ALSA_CONFIG_PATH="$A/share/alsa/alsa.conf" \
      "$A/lib/ld-musl-x86_64.so.1" --library-path "$A/lib" "$A/bin/amixer" -D hw:0 contents
  else
    echo "(no mixer utility available)"
  fi
  echo "== ucm / soundwire config files in the image"
  find /vendor /system /system_ext /product -maxdepth 7 \( -iname '*ucm*' -o -iname '*sof-soundwire*' \) 2>/dev/null | head -60
  echo "== /proc/asound/card0 tree"; ls -R /proc/asound/card0 2>/dev/null | head -80
} > $L/mixer.txt 2>&1
