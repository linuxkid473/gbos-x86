#!/system/bin/sh
# Speaker sound profile for Surface Laptop 7 (dras DSP: EQ + compressor).
# Usage (root):  sh /postinstall/gbos/surface/audio-profile.sh flat|balanced|bass|vocal|apply|status
#   flat      no processing (stock sound)
#   balanced  warmer lows, clearer vocals, more air, mild loudness  (default)
#   bass      stronger low end
#   vocal     speech / podcasts / video calls
G=/postinstall/gbos
S=$G/surface
T=$G/audio/gbos-hda-pch            # template dir: copied into the generated card config at every boot
L=/vendor/etc/audio/gbos-gen       # live config used by the audio HAL
P=$S/dsp
CUR=$S/speaker_profile
want=${1:-apply}
[ "$want" = status ] && { echo "profile: $(cat $CUR 2>/dev/null || echo bass)"; ls $L/dsp.ini 2>&1; exit 0; }
[ "$want" = apply ] && want=$(cat $CUR 2>/dev/null || echo bass)
case "$want" in flat|balanced|bass|vocal) ;; *) echo "unknown profile: $want"; exit 1;; esac
mkdir -p $S 2>/dev/null
echo "$want" > $CUR
if [ "$want" = flat ]; then
  rm -f $T/dsp.ini $L/dsp.ini
else
  [ -f "$P/dsp_$want.ini" ] || { echo "missing $P/dsp_$want.ini"; exit 1; }
  cp -f "$P/dsp_$want.ini" $T/dsp.ini
  [ -d $L ] && { cp -f "$P/dsp_$want.ini" $L/dsp.ini; chmod 0644 $L/dsp.ini; }
fi
# Restart only the audio HAL (never touches the sound driver); skip when nothing is live yet
[ -d $L ] && [ "$2" != norestart ] && setprop ctl.restart vendor.audio-hal-aidl
echo "speaker profile: $want"
