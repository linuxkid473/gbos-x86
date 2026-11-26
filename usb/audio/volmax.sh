#!/bin/sh
# volmax.sh CARD_DIR MIXER_XML: set the output volumes in MIXER_XML to this codec's own maxima (called by hook.sh and
# gen_config.sh; tested by tools/test_audio_config.sh).
#   CARD_DIR   /proc/asound/cardN (its codec#M files are read)
#   MIXER_XML  a mixer_paths.xml made from gbos/audio/gbos-hda-pch (rewritten in place)
# Prints one log line. Exit 0 when MIXER_XML was changed, 1 when it was left as it is.
#
# Why: the template sets "Master/Speaker/Headphone Playback Volume" to 87, the usual Realtek maximum (0x57 steps = 0 dB). Codecs
# with another step count get the wrong level: the ALC298 (Samsung Galaxy Book2 Pro and Book2 Pro 360, issues 31 and 32) has
# 0x7f steps, so 87 leaves it 40 steps below its maximum ("I cannot hear a thing unless I put the volume on max"). On a codec
# with fewer than 87 steps (the QEMU codec: 0x4a) 87 is out of range: amixer clamps it, tinyalsa's mixer_ctl_set_value refuses it,
# and the control keeps its start value 0. Its own maximum is in range either way.
# The maximum of an HDA amp volume control is the amp's step count minus the control's offset (hda_codec.c get_amp_max_value).
# The codec file lists each control under the node that owns it:
#   Node 0x02 [Audio Output] wcaps 0x41d: Stereo Amp-Out
#     Control: name="Speaker Playback Volume", index=0, device=0
#       ControlAmp: chs=3, dir=Out, idx=0, ofs=0
#     Amp-Out caps: ofs=0x7f, nsteps=0x7f, stepsize=0x01, mute=0
# "Master Playback Volume" is the parser's virtual master and belongs to no node. Its range is that of a follower (an output
# volume: hda_generic.c follower_pfxs), so it gets the smallest maximum of the output volumes; inputs mixed into the output
# (Beep, Mic, Line, CD ... Playback Volume, usually 0x1f steps) are not followers and are left out.
# Only ctl lines that still hold the template value 87 are changed, so machine-specific files are never touched.
C=$1; X=$2
[ -f "$X" ] || { echo "volmax: no $X"; exit 1; }
MAX=$(cat "$C"/codec#* 2>/dev/null | awk '
  function hex(s,   i, c, v) { s = tolower(s); v = 0
    for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)); if (!c) return -1; v = v * 16 + c - 1 }
    return v }
  function nsteps(line) { if (!match(line, /nsteps=0x[0-9a-fA-F]+/)) return -1; return hex(substr(line, RSTART + 9, RLENGTH - 9)) }
  function flush(   i, steps, v) {
    for (i = 1; i <= nc; i++) {
      steps = (cdir[i] == "In") ? insteps : outsteps
      if (steps + 0 <= 0) continue
      v = (cofs[i] < steps) ? steps - cofs[i] : steps
      if (!(cname[i] in vmax) || v < vmax[cname[i]]) vmax[cname[i]] = v
    }
    nc = 0; insteps = -1; outsteps = -1 }
  /^Codec:/ || /^Node 0x/ { flush() }
  /^  Control: name="[^"]* Playback Volume", index=0,/ { n = $0; sub(/^  Control: name="/, "", n); sub(/".*/, "", n)
    nc++; cname[nc] = n; cdir[nc] = "Out"; cofs[nc] = 0; next }
  /^    ControlAmp:/ && nc { if ($0 ~ /dir=In/) cdir[nc] = "In"; o = $0; if (sub(/.*ofs=/, "", o)) cofs[nc] = o + 0; next }
  /^  Amp-Out caps:/ { outsteps = nsteps($0); next }
  /^  Amp-In caps:/  { insteps = nsteps($0); next }
  END { flush(); low = -1
    for (k in vmax) {
      if (vmax[k] < 1 || vmax[k] > 255) continue
      print k "=" vmax[k]
      if (k ~ /^(Front|Surround|Center|LFE|Side|Headphone|Speaker|Mono|Line Out|CLFE|Bass Speaker|PCM)( |\+)/ && (low < 0 || vmax[k] < low)) low = vmax[k]
    }
    if (low > 0) print "Master Playback Volume=" low }')
[ -n "$MAX" ] || { echo "volmax: no playback volume controls in $C/codec*; volumes left at 87"; exit 1; }
cp "$X" "$X.vm"
echo "$MAX" | while IFS== read -r name v; do
  # <ctl name="NAME" value="87" /> or value="87 87": every number becomes v
  awk -v n="$name" -v v="$v" '
    index($0, "<ctl name=\"" n "\" value=\"") && match($0, /value="87( 87)*"/) {
      k = split(substr($0, RSTART + 7, RLENGTH - 8), a, " "); s = v; for (i = 2; i <= k; i++) s = s " " v
      $0 = substr($0, 1, RSTART + 6) s substr($0, RSTART + RLENGTH - 1) }
    { print }' "$X.vm" > "$X.vm2" && mv "$X.vm2" "$X.vm"
done
LIST=$(echo "$MAX" | sort | sed 's/ Playback Volume//' | tr '\n' ' ' | sed 's/ $//')
if cmp -s "$X" "$X.vm"; then rm -f "$X.vm"; echo "volmax: volume maxima $LIST; nothing to change"; exit 1; fi
mv "$X.vm" "$X"; echo "volmax: volume maxima $LIST; set in ${X##*/}"; exit 0
