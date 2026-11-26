#!/bin/sh
# Generate the dras audio config for an ALSA card that is not "PCH" (called by hook.sh; tested by tools/test_audio_config.sh).
#   gen_config.sh TEMPLATE_DIR DEFAULT_XML OUT_ROOT NAME CID CNAME
#     TEMPLATE_DIR  gbos/audio/gbos-hda-pch (audio_config.xml, mixer_paths.xml, volume_curves.xml)
#     DEFAULT_XML   gbos/audio/audio_default.xml
#     OUT_ROOT      writes OUT_ROOT/NAME/*.xml and OUT_ROOT/audio_default.xml
#     CID, CNAME    ALSA card id (/proc/asound/cards "[id]") and short name (the text after " - "; also the prefix of the
#                   card's input devices: "<name> Headphone")
#   env ASOUND (default /proc/asound), INPUTS (default /proc/bus/input/devices): where to read the card from; the test points
#   them at fake trees.
#
# The template is written for LEGACY HDA (snd_hda_intel): PCM 0 analog playback+capture (headset mic and, on the Latitude 5300,
# the internal mic too), HDMI on PCM 3/7/8, one combo jack input device "<name> Headphone Mic". A SOF-HDA card (skl_hda_dsp_generic,
# sof-hda-generic*.tplg) numbers its PCMs by the topology and has separate jack input devices:
#   pcm0 HDA Analog (playback+capture), pcm31 HDA Analog Deep Buffer, pcm3/4/5 HDMI1..3, pcm6 DMIC (48 kHz 2ch), pcm7 DMIC16kHz;
#   inputs "<name> Headphone", "<name> Mic", "<name> HDMI/DP,pcm=3|4|5".
# Mapping: Speaker uses a PCM explicitly named "Speaker" when available, else the first non-HDMI playback PCM; Headphone uses the
# first non-HDMI playback PCM. Mic (headset jack) uses the first analog capture PCM; InternalMic uses the DMIC capture PCM when the
# card has one (SOF), else analog capture (legacy HDA: the codec's own mic pin). HDMI1..3 use HDMI/DP playback PCMs in device order
# (an HDMI section with no PCM is dropped; sections are optional). JackDev uses the real input device names found in INPUTS.
# Anything that cannot be determined keeps the template value (= the old behaviour).
set -e
T=$1; DEFX=$2; OUT=$3; AC=$4; CID=$5; CNAME=$6
ASOUND=${ASOUND:-/proc/asound}; INPUTS=${INPUTS:-/proc/bus/input/devices}
O=$OUT/$AC

sedi() { f=$1; shift; sed "$@" "$f" > "$f.tmp" && mv "$f.tmp" "$f"; }   # no sed -i: portable to the Mac the test runs on
esc() { printf '%s' "$1" | sed 's/[\\/&]/\\&/g'; }
CN_E=$(esc "$CNAME"); CID_E=$(esc "$CID")

rm -rf "$O"; mkdir -p "$OUT"; cp -r "$T" "$O"
for f in "$O"/*.xml; do sedi "$f" -e "s/HDA Intel PCH/$CN_E/g" -e "s/PCH/$CID_E/g"; done
sed "s/<soundcard>PCH/<soundcard>$CID_E/; s#gbos-hda-pch#$AC#" "$DEFX" > "$OUT/audio_default.xml"

# --- PCM layout of this card -------------------------------------------------------------------------------------------
CARDNO=$(sed -n "s/^ *\([0-9][0-9]*\) \[$CID_E *\].*/\1/p" "$ASOUND/cards" 2>/dev/null | head -n 1)
APB=; SPK=; ACP=; DMIC=; DMIC16=; HDMI=
if [ -n "$CARDNO" ] && [ -f "$ASOUND/pcm" ]; then
  eval "$(awk -v card="$CARDNO" '
    function lc(s) { return tolower(s) }
    /^[0-9]+-[0-9]+:/ {
      split($1, a, "-"); if (a[1] + 0 != card + 0) next
      dev = a[2] + 0
      line = $0; sub(/^[0-9]+-[0-9]+: */, "", line)
      n = split(line, f, " : "); low = lc(f[1] " " f[2])
      pb = (line ~ / : playback [1-9]/); cp = (line ~ / : capture [1-9]/)
      if (low ~ /deep ?buffer|echo|loopback|bluetooth|offload|reference|(^| )(sco|hfp|a2dp)( |$)/) next
      if (pb && low ~ /hdmi|displayport|idisp/) { hdmi = hdmi " " dev; next }
      if (cp && !pb && low ~ /dmic/) { if (low ~ /16 *khz|16k/) { if (d16 == "") d16 = dev } else if (dm == "") dm = dev; next }
      if (pb && sp == "" && low ~ /(^| )speaker/) { sp = dev; next }   # SoundWire: the speaker amp has its own PCM
      if (pb && apb == "") apb = dev
      if (cp && acp == "") acp = dev
    }
    END { printf "APB=%s; SPK=%s; ACP=%s; DMIC=%s; DMIC16=%s; HDMI=\"%s\"\n", apb, sp, acp, dm, d16, hdmi }
  ' "$ASOUND/pcm")"
fi
[ -n "$DMIC" ] || DMIC=$DMIC16
# AMD laptops: the built-in microphones are DMICs on the audio coprocessor (ACP), which registers its own ASoC card
# (acp-pdm-mach -> id "acppdmmach", ACP 6.3/7.x ps-mach "acp63", Rembrandt "acp6x", Renoir "acp") next to the HDA card that has
# the speaker and the jack. When this card has no DMIC, InternalMic uses the first DMIC capture PCM of a card without HDA codecs.
IMIC_CID=$CID
if [ -z "$DMIC" ] && [ -n "$CARDNO" ] && [ -f "$ASOUND/pcm" ]; then
  for _dm in $(awk -v card="$CARDNO" '
      /^[0-9]+-[0-9]+:/ { split($1, a, "-"); if (a[1] + 0 == card + 0) next
        line = tolower($0); if (line ~ /dmic/ && line ~ / : capture [1-9]/ && line !~ / : playback [1-9]/) print (a[1] + 0) "-" (a[2] + 0) }
    ' "$ASOUND/pcm"); do
    _dc=${_dm%-*}
    ls "$ASOUND/card$_dc"/codec#* >/dev/null 2>&1 && continue   # an HDA card: not the ACP
    _did=$(sed -n "s/^ *$_dc \[\([^] ]*\) *\].*/\1/p" "$ASOUND/cards")
    [ -n "$_did" ] || continue
    DMIC=${_dm#*-}; IMIC_CID=$_did; break
  done
fi
IMIC=${DMIC:-$ACP}
H1=; H2=; H3=; i=0
for d in $HDMI; do i=$((i+1)); [ $i = 1 ] && H1=$d; [ $i = 2 ] && H2=$d; [ $i = 3 ] && H3=$d; done

# --- jack input devices ------------------------------------------------------------------------------------------------
has() { [ -f "$INPUTS" ] && grep -qxF "N: Name=\"$1\"" "$INPUTS"; }
HPJ=; MICJ=; COMBO=
for n in "$CNAME Headphone Mic" "$CNAME Headset Jack" "$CNAME Headset"; do has "$n" && { COMBO=$n; break; }; done
if [ -n "$COMBO" ]; then HPJ=$COMBO; MICJ=$COMBO; else
  # "Front Headphone"/"Front Mic": the HDA parser names a jack after its pin location (Acer Aspire A515-46, ALC255: "HD-Audio Generic Front Headphone").
  for n in "$CNAME Headphone" "$CNAME Headphones" "$CNAME Front Headphone"; do has "$n" && { HPJ=$n; break; }; done
  for n in "$CNAME Headset Mic" "$CNAME Mic" "$CNAME Front Mic"; do has "$n" && { MICJ=$n; break; }; done
  # A jack with no mic input device (the headset mic pin is not wired) must still name a device that exists: the template's
  # "<name> Headphone Mic" does not, and the HAL cannot open it. Use the headphone jack's device for the mic section too.
  [ -z "$MICJ" ] && [ -n "$HPJ" ] && MICJ=$HPJ
fi

# --- rewrite audio_config.xml ------------------------------------------------------------------------------------------
if [ -n "$APB" ]; then
  awk -v cid="$CID" -v cn="$CNAME" -v apb="$APB" -v spk="$SPK" -v acp="$ACP" -v imic="$IMIC" -v imiccid="$IMIC_CID" -v hpj="$HPJ" -v micj="$MICJ" \
      -v h1="$H1" -v h2="$H2" -v h3="$H3" '
    function setpcm(dev) { setpcmc(cid, dev) }
    function setpcmc(c, dev) { sub(/hw:[^<,]*,[0-9]*/, "hw:" c "," dev) }
    function setjack(name) { sub(/>[^<]*</, ">" name "<") }
    /^[ \t]*<(Speaker|Headphone|InternalMic|Mic|HDMI1|HDMI2|HDMI3)>/ {
      sec = $0; sub(/^[ \t]*</, "", sec); sub(/>.*/, "", sec); buf = ""; drop = 0
      if (sec == "HDMI1" && h1 == "") drop = 1
      if (sec == "HDMI2" && h2 == "") drop = 1
      if (sec == "HDMI3" && h3 == "") drop = 1
    }
    sec != "" && /<PlaybackPCM>/ {
      if (sec == "Headphone") setpcm(apb)
      if (sec == "Speaker") setpcm(spk != "" ? spk : apb)
      if (sec == "HDMI1") setpcm(h1); if (sec == "HDMI2") setpcm(h2); if (sec == "HDMI3") setpcm(h3)
    }
    sec != "" && /<CapturePCM>/ {
      if (sec == "InternalMic" && imic != "") setpcmc(imiccid, imic)
      if (sec == "Mic" && acp != "") setpcm(acp)
    }
    sec != "" && /<JackDev>/ {
      if (sec == "Headphone" && hpj != "") setjack(hpj)
      if (sec == "Mic" && micj != "") setjack(micj)
      if (sec == "HDMI1") setjack(cn " HDMI/DP,pcm=" h1)
      if (sec == "HDMI2") setjack(cn " HDMI/DP,pcm=" h2)
      if (sec == "HDMI3") setjack(cn " HDMI/DP,pcm=" h3)
    }
    { if (!drop) print }
    /^[ \t]*<\/(Speaker|Headphone|InternalMic|Mic|HDMI1|HDMI2|HDMI3)>/ { sec = ""; drop = 0 }
  ' "$O/audio_config.xml" > "$O/audio_config.xml.new" && mv "$O/audio_config.xml.new" "$O/audio_config.xml"
fi

# --- mixer_paths.xml: output volumes, the second speaker pair and the jack mic -----------------------------------------------------------
# With a codec fixup that declares a second speaker pin (ENVY 13-aq0xxx) the HDA parser names the pair "Speaker" and "Bass Speaker";
# unmute both. Some codecs (ALC245, HP Spectre x360 14, issue 10) have no "Master Playback Volume", only per-output ones that start at
# 0, so those are set too (87 is the usual Realtek maximum; a control that is absent or has a lower maximum is skipped by audio_route). Mic: "Mic" / "Headset Mic" depending on the pin label. Absent controls are skipped by audio_route.
sedi "$O/mixer_paths.xml" \
  -e '/name="Master Playback Volume"/{' -e p -e 's/name="Master Playback Volume" value="[^"]*"/name="Speaker Playback Volume" value="87 87"/' -e p \
  -e 's/name="Speaker Playback Volume"/name="Headphone Playback Volume"/' -e '}' \
  -e '/name="Speaker Playback Switch"/{' -e p -e 's/name="Speaker Playback Switch"/name="Bass Speaker Playback Switch"/' -e '}' \
  -e '/name="Capture Switch"/{' -e p -e 's/name="Capture Switch"/name="Mic Capture Switch"/' -e p \
  -e 's/name="Mic Capture Switch"/name="Headset Mic Capture Switch"/' -e '}'
# The 87s above are the usual Realtek maximum; volmax.sh replaces them with this codec's own maxima (ALC298: 127, issues 31/32).
VMSG=
if [ -n "$CARDNO" ] && [ -f "${0%/*}/volmax.sh" ]; then VMSG=$(sh "${0%/*}/volmax.sh" "$ASOUND/card$CARDNO" "$O/mixer_paths.xml") || true; fi

echo "pcm map: card=${CARDNO:-?} playback-default=${APB:--} speaker-playback=${SPK:-${APB:--}} headphone-playback=${APB:--} analog-capture=${ACP:--} dmic=${DMIC:--} internal-mic=${IMIC:+$IMIC_CID,}${IMIC:--} hdmi=${HDMI:--}; jacks: headphone='${HPJ:--}' mic='${MICJ:--}'"
[ -z "$VMSG" ] || echo "$VMSG"
