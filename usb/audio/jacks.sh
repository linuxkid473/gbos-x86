#!/bin/sh
# Point the Headphone and Mic sections of a dras audio_config.xml at the jack input devices the card really has
# (called by gen_config.sh, and by hook.sh for card id PCH; tested by tools/test_audio_config.sh).
#   jacks.sh AUDIO_CONFIG_XML CNAME
#     CNAME  the card's short name, the prefix of its input devices ("HDA Intel PCH Front Headphone")
#   env INPUTS (default /proc/bus/input/devices): where to read the input devices from; the test points it at a fake file.
# The HAL opens the <JackDev> device of a section. A name that does not exist leaves the card without any sound: the template's
# "HDA Intel PCH Headphone Mic" (the Latitude 5300's combo jack) on a desktop board with separate jacks (Dell OptiPlex 3020,
# ALC3220: "Front Headphone", "Line Out", "Front Mic", "Rear Mic"; issue 88). A jack that cannot be found keeps the file's value.
# Prints what it chose.
set -e
X=$1; CNAME=$2; INPUTS=${INPUTS:-/proc/bus/input/devices}

has() { [ -f "$INPUTS" ] && grep -qxF "N: Name=\"$1\"" "$INPUTS"; }
HPJ=; MICJ=; COMBO=
for n in "$CNAME Headphone Mic" "$CNAME Headset Jack" "$CNAME Headset"; do has "$n" && { COMBO=$n; break; }; done
if [ -n "$COMBO" ]; then HPJ=$COMBO; MICJ=$COMBO; else
  # "Front Headphone"/"Front Mic": the HDA parser names a jack after its pin location (Acer Aspire A515-46, ALC255: "HD-Audio Generic
  # Front Headphone"; OptiPlex 3020 desktop: "HDA Intel PCH Front Headphone").
  for n in "$CNAME Headphone" "$CNAME Headphones" "$CNAME Front Headphone"; do has "$n" && { HPJ=$n; break; }; done
  for n in "$CNAME Headset Mic" "$CNAME Mic" "$CNAME Front Mic"; do has "$n" && { MICJ=$n; break; }; done
  # A jack with no mic input device (the headset mic pin is not wired) must still name a device that exists: the template's
  # "<name> Headphone Mic" does not, and the HAL cannot open it. Use the headphone jack's device for the mic section too.
  [ -z "$MICJ" ] && [ -n "$HPJ" ] && MICJ=$HPJ
fi

if [ -n "$HPJ$MICJ" ]; then
  awk -v hpj="$HPJ" -v micj="$MICJ" '
    function setjack(name) { sub(/>[^<]*</, ">" name "<") }
    /^[ \t]*<(Headphone|Mic)>/ { sec = $0; sub(/^[ \t]*</, "", sec); sub(/>.*/, "", sec) }
    sec != "" && /<JackDev>/ {
      if (sec == "Headphone" && hpj != "") setjack(hpj)
      if (sec == "Mic" && micj != "") setjack(micj)
    }
    { print }
    /^[ \t]*<\/(Headphone|Mic)>/ { sec = "" }
  ' "$X" > "$X.tmp" && mv "$X.tmp" "$X"
fi
echo "jacks: headphone='${HPJ:--}' mic='${MICJ:--}'"
