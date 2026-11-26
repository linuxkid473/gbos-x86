#!/bin/sh
# Test of usb/audio/gen_config.sh against fake /proc/asound + /proc/bus/input/devices trees:
#   - legacy HDA card id PCH (what the Latitude 5300 has): output must be IDENTICAL to the shipped template (PCM 0 / 3,7,8)
#   - SOF-HDA card "sofhdadsp" (HP ENVY 13-aq0xxx, reporter's hwdetect.txt): speaker/headphone pcm0, internal mic = DMIC pcm6,
#     headset mic pcm0, HDMI 3/4/5, jack devices "sof-hda-dsp Headphone" / "sof-hda-dsp Mic" / "sof-hda-dsp HDMI/DP,pcm=N"
#   - a legacy HDA card with another id ("Generic"), and a SOF card with a combo jack and only one HDMI PCM
# Also checks the XML is well-formed and that the PCH path in hook.sh is untouched (no generation for card id PCH).
# Run: sh tools/test_audio_config.sh   (needs python3 for the XML check)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GEN=$ROOT/usb/audio/gen_config.sh; TPL=$ROOT/usb/audio/gbos-hda-pch; DEFX=$ROOT/usb/audio/audio_default.xml
W=$(mktemp -d "${TMPDIR:-/tmp}/audiotest.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }
chk() { # chk "description" file pattern  (fixed string must appear)
  if grep -qF -- "$3" "$2"; then ok "$1"; else bad "$1: '$3' not in $2"; fi; }
nochk() { if grep -qF -- "$3" "$2"; then bad "$1: '$3' present in $2"; else ok "$1"; fi; }

wellformed() { python3 -c 'import sys,xml.etree.ElementTree as E; [E.parse(f) for f in sys.argv[1:]]' "$@" 2>"$W/xmlerr" \
  && ok "well-formed XML: $(echo "$@" | sed "s#$W/##g")" || { bad "XML not well-formed: $(cat "$W/xmlerr")"; }; }

mk() { # mk NAME : fake tree dir
  mkdir -p "$W/$1"; echo "$W/$1"; }

# ---- fake systems ----------------------------------------------------------------------------------------------------------
# SOF-HDA, HP ENVY 13-aq0xxx (cards line from hwdetect.txt; PCMs from sof-hda-generic-2ch.tplg: ids 0,31,3,4,5,6,7)
D=$(mk sof)
cat > $D/cards <<'EOF'
 0 [sofhdadsp      ]: sof-hda-dsp - sof-hda-dsp
                      HP-HPENVYLaptop13_aq0xxx-Type1ProductConfigId-85E2
EOF
cat > $D/pcm <<'EOF'
00-00: HDA Analog (*) :  : playback 1 : capture 1
00-03: HDMI1 (*) :  : playback 1
00-04: HDMI2 (*) :  : playback 1
00-05: HDMI3 (*) :  : playback 1
00-06: DMIC (*) :  : capture 1
00-07: DMIC16kHz (*) :  : capture 1
00-31: HDA Analog Deep Buffer (*) :  : playback 1
EOF
cat > $D/input <<'EOF'
I: Bus=0019 Vendor=0000 Product=0005 Version=0000
N: Name="Lid Switch"
I: Bus=0000 Vendor=0000 Product=0000 Version=0000
N: Name="sof-hda-dsp Mic"
I: Bus=0000 Vendor=0000 Product=0000 Version=0000
N: Name="sof-hda-dsp Headphone"
N: Name="sof-hda-dsp HDMI/DP,pcm=3"
N: Name="sof-hda-dsp HDMI/DP,pcm=4"
N: Name="sof-hda-dsp HDMI/DP,pcm=5"
EOF

# legacy HDA, Latitude 5300 (ALC3254): card id PCH, HDMI on 3/7/8, combo jack
D=$(mk pch)
cat > $D/cards <<'EOF'
 0 [PCH            ]: HDA-Intel - HDA Intel PCH
                      HDA Intel PCH at 0xe1228000 irq 142
EOF
cat > $D/pcm <<'EOF'
00-00: ALC3254 Analog : ALC3254 Analog : playback 1 : capture 1
00-03: HDMI 0 : HDMI 0 : playback 1
00-07: HDMI 1 : HDMI 1 : playback 1
00-08: HDMI 2 : HDMI 2 : playback 1
EOF
cat > $D/input <<'EOF'
N: Name="HDA Intel PCH Headphone Mic"
N: Name="HDA Intel PCH HDMI/DP,pcm=3"
N: Name="HDA Intel PCH HDMI/DP,pcm=7"
N: Name="HDA Intel PCH HDMI/DP,pcm=8"
EOF

# legacy HDA with another card id (AMD/other board): same layout, different id
D=$(mk generic)
cat > $D/cards <<'EOF'
 1 [Generic        ]: HDA-Intel - HD-Audio Generic
                      HD-Audio Generic at 0xf0000000 irq 33
 0 [NVidia         ]: HDA-Intel - HDA NVidia
EOF
cat > $D/pcm <<'EOF'
01-00: ALC256 Analog : ALC256 Analog : playback 1 : capture 1
01-01: ALC256 Digital : ALC256 Digital : playback 1
00-03: HDMI 0 : HDMI 0 : playback 1
EOF
cat > $D/input <<'EOF'
N: Name="HD-Audio Generic Headphone"
N: Name="HD-Audio Generic Mic"
EOF

# SOF card, combo jack, only one HDMI PCM, DMIC only 16 kHz variant
D=$(mk sofcombo)
cat > $D/cards <<'EOF'
 0 [sofrt5682      ]: sof-rt5682 - sof-rt5682
                      Google-Foo
EOF
cat > $D/pcm <<'EOF'
00-00: Headset (*) :  : playback 1 : capture 1
00-05: HDMI1 (*) :  : playback 1
00-07: DMIC16kHz (*) :  : capture 1
EOF
cat > $D/input <<'EOF'
N: Name="sof-rt5682 Headset Jack"
N: Name="sof-rt5682 HDMI/DP,pcm=5"
EOF

# card whose layout cannot be read at all (no pcm file): must fall back to the template numbers
D=$(mk nopcm)
cat > $D/cards <<'EOF'
 0 [Weird          ]: Weird - Weird card
EOF
: > $D/input

gen() { # gen TREE NAME CID CNAME -> $W/out-NAME
  ASOUND=$W/$1 INPUTS=$W/$1/input; export ASOUND INPUTS
  sh "$GEN" "$TPL" "$DEFX" "$W/out-$1" "$2" "$3" "$4" > "$W/msg-$1" 2>&1 || { bad "gen_config.sh failed on $1: $(cat $W/msg-$1)"; return 1; }
  unset ASOUND INPUTS; cat "$W/msg-$1"; }

# ---- 1. legacy PCH: identical to the template -------------------------------------------------------------------------------
gen pch gbos-gen PCH "HDA Intel PCH" >/dev/null
O=$W/out-pch/gbos-gen
cmp -s "$O/audio_config.xml" "$TPL/audio_config.xml" && ok "PCH layout: audio_config.xml identical to the template" || bad "PCH: audio_config.xml differs from the template"
cmp -s "$O/volume_curves.xml" "$TPL/volume_curves.xml" && ok "PCH: volume_curves.xml identical" || bad "PCH: volume_curves.xml differs"

# ---- 2. SOF-HDA ENVY 13-aq -------------------------------------------------------------------------------------------------
gen sof gbos-gen sofhdadsp sof-hda-dsp
O=$W/out-sof/gbos-gen; C=$O/audio_config.xml
wellformed "$C" "$O/mixer_paths.xml" "$O/volume_curves.xml" "$W/out-sof/audio_default.xml"
chk "sof: soundcard id"            "$W/out-sof/audio_default.xml" "<soundcard>sofhdadsp</soundcard>"
chk "sof: config dir"              "$W/out-sof/audio_default.xml" "<audio-config-dir>gbos-gen</audio-config-dir>"
chk "sof: SoundCardName"           "$C" "<SoundCardName>hw:sofhdadsp</SoundCardName>"
python3 - "$C" > "$W/sof.py.out" 2>&1 <<'EOF'
import sys, xml.etree.ElementTree as E
r = E.parse(sys.argv[1]).getroot(); h = r.find('HiFi')
def g(sec, tag):
    e = h.find(sec); return None if e is None else (e.findtext(tag))
exp = {
 ('Speaker','PlaybackPCM'): 'hw:sofhdadsp,0',
 ('Headphone','PlaybackPCM'): 'hw:sofhdadsp,0',
 ('Headphone','JackDev'): 'sof-hda-dsp Headphone',
 ('Headphone','JackSwitch'): '2',
 ('InternalMic','CapturePCM'): 'hw:sofhdadsp,6',      # DMIC, not the headset jack
 ('Mic','CapturePCM'): 'hw:sofhdadsp,0',
 ('Mic','JackDev'): 'sof-hda-dsp Mic',
 ('Mic','CaptureMixerElem'): 'Capture',
 ('HDMI1','PlaybackPCM'): 'hw:sofhdadsp,3', ('HDMI1','JackDev'): 'sof-hda-dsp HDMI/DP,pcm=3',
 ('HDMI2','PlaybackPCM'): 'hw:sofhdadsp,4', ('HDMI2','JackDev'): 'sof-hda-dsp HDMI/DP,pcm=4',
 ('HDMI3','PlaybackPCM'): 'hw:sofhdadsp,5', ('HDMI3','JackDev'): 'sof-hda-dsp HDMI/DP,pcm=5',
}
bad = 0
for (s, t), v in exp.items():
    if g(s, t) != v: print('FAIL', s, t, 'got', g(s, t), 'want', v); bad += 1
# no section may keep a PCH / legacy name or point at a DMIC/deep-buffer PCM for playback
txt = open(sys.argv[1]).read()
for s in ('PCH', 'HDA Intel', ',7<', ',8<', ',31<'):
    if s in txt: print('FAIL leftover', s); bad += 1
# every required field of Google's sections is still there (dras rejects the file on a missing field)
for sec, tags in {'Speaker':['PlaybackPCM'], 'Headphone':['PlaybackPCM','JackDev','JackSwitch'],
                  'InternalMic':['CapturePCM','IntrinsicSensitivity','RampUpMs'], 'Mic':['CapturePCM','CaptureMixerElem','JackDev'],
                  'HDMI1':['PlaybackPCM','JackDev','PlaybackChannels']}.items():
    for t in tags:
        if g(sec, t) is None: print('FAIL missing', sec, t); bad += 1
print('PYOK' if not bad else 'PYBAD')
sys.exit(1 if bad else 0)
EOF
if [ $? = 0 ]; then ok "sof: all device numbers / jack names / required fields ($(grep -c . $W/sof.py.out) line)"; else bad "sof mapping: $(cat $W/sof.py.out)"; fi
chk "sof: mixer unmutes Bass Speaker"   "$O/mixer_paths.xml" 'name="Bass Speaker Playback Switch"'
chk "sof: mixer sets Speaker Playback Volume (no Master volume on ALC245)" "$O/mixer_paths.xml" 'name="Speaker Playback Volume" value="87 87"'
chk "sof: mixer sets Headphone Playback Volume" "$O/mixer_paths.xml" 'name="Headphone Playback Volume" value="87 87"'
chk "sof: mixer keeps Speaker switch"   "$O/mixer_paths.xml" 'name="Speaker Playback Switch"'
chk "sof: mixer jack mic switch"        "$O/mixer_paths.xml" 'name="Mic Capture Switch"'

# ---- 3. legacy card with another id -----------------------------------------------------------------------------------------
gen generic gbos-gen Generic "HD-Audio Generic" >/dev/null
C=$W/out-generic/gbos-gen/audio_config.xml
wellformed "$C"
chk "generic: playback pcm 0"  "$C" "<PlaybackPCM>hw:Generic,0</PlaybackPCM>"
chk "generic: internal mic = analog capture 0 (no DMIC)" "$C" "<CapturePCM>hw:Generic,0</CapturePCM>"
chk "generic: card number taken from /proc/asound/cards (card 1; the NVidia card 0 PCM 3 is not used)" "$C" "<CapturePCM>hw:Generic,0</CapturePCM>"
nochk "generic: no HDMI sections (this card has no HDMI PCM)" "$C" "<HDMI1>"
chk "generic: jack names"      "$C" "<JackDev>HD-Audio Generic Headphone</JackDev>"
chk "generic: mic jack"        "$C" "<JackDev>HD-Audio Generic Mic</JackDev>"

# ---- 3b. Acer Aspire A515-46 (Ryzen 3 3350U, ALC255), from a user's logs: AMD HDMI card 0 + ALC255 card 1, both "HD-Audio Generic"; the
# jack input device is named after its pin location ("Front Headphone") and there is no mic jack input device at all
D=$(mk acer)
cat > $D/cards <<'EOF'
 0 [Generic        ]: HDA-Intel - HD-Audio Generic
                      HD-Audio Generic at 0xc07c8000 irq 48
 1 [Generic_1      ]: HDA-Intel - HD-Audio Generic
                      HD-Audio Generic at 0xc07c0000 irq 49
EOF
cat > $D/pcm <<'EOF'
00-03: HDMI 0 : HDMI 0 : playback 1
01-00: ALC255 Analog : ALC255 Analog : playback 1 : capture 1
EOF
cat > $D/input <<'EOF'
N: Name="HD-Audio Generic HDMI/DP,pcm=3"
N: Name="HD-Audio Generic Front Headphone"
EOF
gen acer gbos-gen Generic_1 "HD-Audio Generic" >/dev/null
C=$W/out-acer/gbos-gen/audio_config.xml
wellformed "$C"
chk "acer: playback on card Generic_1" "$C" "<PlaybackPCM>hw:Generic_1,0</PlaybackPCM>"
chk "acer: headphone jack = Front Headphone" "$C" "<JackDev>HD-Audio Generic Front Headphone</JackDev>"
nochk "acer: no jack device that does not exist" "$C" "Headphone Mic</JackDev>"
chk "acer: volumes set" "$W/out-acer/gbos-gen/mixer_paths.xml" 'name="Speaker Playback Volume" value="87 87"'

# ---- 3c. Lenovo Yoga Pro 7 14AKP10 (Ryzen AI 7 350, issue 39): Radeon HDMI card 0, ALC287 card 1 (speaker, jack), and the built-in
# DMICs on the ACP as their own ASoC card (acp-pdm-mach, id acppdmmach, no HDA codec). Internal mic must use the ACP card, the jack
# mic stays on the ALC287. A 4th card (HDA, with a codec) that happens to have a "DMIC" capture PCM must not be picked.
D=$(mk acp)
cat > $D/cards <<'EOF'
 0 [Generic        ]: HDA-Intel - HD-Audio Generic
                      HD-Audio Generic at 0x902c8000 irq 47
 1 [Generic_1      ]: HDA-Intel - HD-Audio Generic
                      HD-Audio Generic at 0x902c0000 irq 48
 2 [Other          ]: HDA-Intel - Other HDA
 3 [acppdmmach     ]: acp-pdm-mach - acp-pdm-mach
                      LENOVO-83KG-YogaPro714AKP10-LNVNB161216
EOF
cat > $D/pcm <<'EOF'
00-03: HDMI 0 : HDMI 0 : playback 1
01-00: ALC287 Analog : ALC287 Analog : playback 1 : capture 1
02-00: DMIC Fake : DMIC Fake : capture 1
03-00: DMIC capture dmic-hifi-0 : DMIC capture dmic-hifi-0 : capture 1
EOF
for c in 0 1 2; do mkdir -p $D/card$c; echo "Codec: x" > "$D/card$c/codec#0"; done; mkdir -p $D/card3
cat > $D/input <<'EOF'
N: Name="HD-Audio Generic Mic"
N: Name="HD-Audio Generic Headphone"
EOF
M=$(gen acp gbos-gen Generic_1 "HD-Audio Generic")
C=$W/out-acp/gbos-gen/audio_config.xml
wellformed "$C"
python3 - "$C" > "$W/acp.py.out" 2>&1 <<'EOF'
import sys, xml.etree.ElementTree as E
h = E.parse(sys.argv[1]).getroot().find('HiFi')
exp = {('Speaker','PlaybackPCM'): 'hw:Generic_1,0', ('InternalMic','CapturePCM'): 'hw:acppdmmach,0',
       ('Mic','CapturePCM'): 'hw:Generic_1,0', ('Mic','JackDev'): 'HD-Audio Generic Mic'}
bad = [f'{s}/{t}={h.find(s).findtext(t)!r} want {v!r}' for (s, t), v in exp.items() if h.find(s).findtext(t) != v]
print('\n'.join(bad) or 'PYOK'); sys.exit(1 if bad else 0)
EOF
if [ $? = 0 ]; then ok "acp: internal mic = ACP DMIC card, speaker and jack mic on the ALC287"; else bad "acp mapping: $(cat $W/acp.py.out)"; fi
case "$M" in *"internal-mic=acppdmmach,0"*) ok "acp: log line names the DMIC card";; *) bad "acp: log line: $M";; esac
# without the ACP card the internal mic falls back to the ALC287's analog capture (as before)
grep -v acppdmmach $D/cards > $D/cards.n && mv $D/cards.n $D/cards; grep -v '^03-' $D/pcm > $D/pcm.n && mv $D/pcm.n $D/pcm
gen acp gbos-gen Generic_1 "HD-Audio Generic" >/dev/null
chk "acp: no ACP card -> internal mic on analog capture" "$C" "<CapturePCM>hw:Generic_1,0</CapturePCM>"
nochk "acp: no ACP card -> the HDA card's DMIC PCM is not used" "$C" "hw:Other"

# ---- 4. SOF, combo jack, one HDMI, only DMIC16kHz ---------------------------------------------------------------------------
gen sofcombo gbos-gen sofrt5682 sof-rt5682 >/dev/null
C=$W/out-sofcombo/gbos-gen/audio_config.xml
wellformed "$C"
chk "combo: headphone jack"    "$C" "<JackDev>sof-rt5682 Headset Jack</JackDev>"
chk "combo: internal mic = DMIC16kHz pcm 7" "$C" "<CapturePCM>hw:sofrt5682,7</CapturePCM>"
chk "combo: HDMI1 on pcm 5"    "$C" "<JackDev>sof-rt5682 HDMI/DP,pcm=5</JackDev>"
nochk "combo: HDMI2 dropped"   "$C" "<HDMI2>"
nochk "combo: HDMI3 dropped"   "$C" "<HDMI3>"

# ---- 5. unreadable layout: template numbers, only card id/name substituted -------------------------------------------------
gen nopcm gbos-gen Weird "Weird card" >/dev/null
C=$W/out-nopcm/gbos-gen/audio_config.xml
wellformed "$C"
chk "nopcm: falls back to template pcm 3" "$C" "<PlaybackPCM>hw:Weird,3</PlaybackPCM>"
chk "nopcm: name substituted"             "$C" "<JackDev>Weird card Headphone Mic</JackDev>"

# ---- 5b. SoundWire card (Surface Laptop 7 Intel: RT721 jack codec + RT1320 amp, SDCA function topology). The speaker amp has its
#          own PCM "Speaker" (2), the jack is "Jack Out" (0) / "Jack In" (1); the "Deepbuffer ..." PCMs (31, 35) must never be picked ---
D=$(mk sdw)
cat > $D/cards <<'EOF'
 0 [sofsoundwire   ]: sof-soundwire - sof-soundwire
                      MicrosoftCorporation-SurfaceLaptopforBusiness7thEditionwithIntel-124I00128T00
EOF
cat > $D/pcm <<'EOF'
00-00: Jack Out (*) :  : playback 1
00-01: Jack In (*) :  : capture 1
00-02: Speaker (*) :  : playback 1
00-05: HDMI1 (*) :  : playback 1
00-06: HDMI2 (*) :  : playback 1
00-07: HDMI3 (*) :  : playback 1
00-31: Deepbuffer Jack Out (*) :  : playback 1
00-35: Deepbuffer Speaker (*) :  : playback 1
EOF
cat > $D/input <<'EOF'
N: Name="sof-soundwire Headset Jack"
N: Name="sof-soundwire HDMI/DP,pcm=5"
EOF
gen sdw gbos-gen sofsoundwire sof-soundwire >/dev/null
C=$W/out-sdw/gbos-gen/audio_config.xml
wellformed "$C"
python3 - "$C" > "$W/sdw.py.out" 2>&1 <<'EOF'
import sys, xml.etree.ElementTree as E
h = E.parse(sys.argv[1]).getroot().find('HiFi')
def g(sec, tag):
    e = h.find(sec); return None if e is None else e.findtext(tag)
exp = {('Speaker','PlaybackPCM'): 'hw:sofsoundwire,2',        # not the jack's PCM 0, not Deepbuffer 35
       ('Headphone','PlaybackPCM'): 'hw:sofsoundwire,0',      # not Deepbuffer 31
       ('Headphone','JackDev'): 'sof-soundwire Headset Jack',
       ('Mic','CapturePCM'): 'hw:sofsoundwire,1',
       ('HDMI1','PlaybackPCM'): 'hw:sofsoundwire,5'}
bad = 0
for (s, t), v in exp.items():
    if g(s, t) != v: print('FAIL', s, t, 'got', g(s, t), 'want', v); bad += 1
txt = open(sys.argv[1]).read()
for s in (',31<', ',35<', 'PCH'):
    if s in txt: print('FAIL leftover', s); bad += 1
print('PYOK' if not bad else 'PYBAD'); sys.exit(1 if bad else 0)
EOF
if [ $? = 0 ]; then ok "sdw: Speaker pcm 2, Headphone pcm 0, jack mic pcm 1, no Deepbuffer PCM, jack names"; else bad "sdw mapping: $(cat $W/sdw.py.out)"; fi

# ---- 5c. Dell Latitude 7320 Detachable: dedicated Speaker PCM 2 vs jack playback PCM 0 -------------------------------
D=$(mk latitude7320)
cat > $D/cards <<'EOF'
 0 [sofsoundwire   ]: sof-soundwire - sof-soundwire
                      DellInc-Latitude7320Detachable-0TNW0H
EOF
cat > $D/pcm <<'EOF'
00-00: Jack Out (*) :  : playback 1
00-01: Jack In (*) :  : capture 1
00-02: Speaker (*) :  : playback 1
00-05: HDMI1 (*) :  : playback 1
00-06: HDMI2 (*) :  : playback 1
00-07: HDMI3 (*) :  : playback 1
00-31: Deepbuffer Jack Out (*) :  : playback 1
00-35: Deepbuffer Speaker (*) :  : playback 1
EOF
cat > $D/input <<'EOF'
EOF
gen latitude7320 gbos-gen sofsoundwire sof-soundwire >/dev/null
C=$W/out-latitude7320/gbos-gen/audio_config.xml
wellformed "$C"
python3 - "$C" > "$W/latitude7320.py.out" 2>&1 <<'EOF'
import sys, xml.etree.ElementTree as E
h = E.parse(sys.argv[1]).getroot().find('HiFi')
got = {name: h.findtext(f'{name}/PlaybackPCM')
       for name in ('Speaker', 'Headphone', 'HDMI1', 'HDMI2', 'HDMI3')}
want = {'Speaker': 'hw:sofsoundwire,2', 'Headphone': 'hw:sofsoundwire,0',
        'HDMI1': 'hw:sofsoundwire,5', 'HDMI2': 'hw:sofsoundwire,6',
        'HDMI3': 'hw:sofsoundwire,7'}
if got != want:
    print(f'FAIL got {got}, want {want}')
    sys.exit(1)
print('PYOK')
EOF
if [ $? = 0 ]; then ok "7320: Speaker PCM 2 and headphone/jack PCM 0"; else bad "7320 PCM mapping: $(cat "$W/latitude7320.py.out")"; fi
nochk "7320: generated template comment does not misidentify Latitude 5300" "$C" "Latitude 5300"
chk "7320: PCM diagnostic distinguishes speaker and jack playback" "$W/msg-latitude7320" "speaker-playback=2 headphone-playback=0"
nochk "7320: generated default profile comment does not misidentify Latitude 5300" "$W/out-latitude7320/audio_default.xml" "Latitude 5300"

# ---- 5c. Meteor Lake SOF-HDA (Lenovo ThinkBook Plus G5 Hybrid 21N9, Core Ultra 7 155H; TAS2781 I2C amps behind the Realtek codec; HDMI
#          codec at address 2). NOT a real /proc/asound capture: the card does not exist yet (probe fails on the missing topology).
#          Card id/name are what skl_hda_dsp_generic registers (same as the ENVY); the PCM list is the one in
#          intel/sof-ace-tplg/sof-hda-generic-2ch.tplg (= sof-ipc4-tplg/..., parsed from the file: HDA Analog 0, Deepbuffer 31,
#          "DMIC Raw" 6, HDMI1/2/3 = 3/4/5; no DMIC16kHz PCM in the IPC4 topology). The HDA names of the PCMs are the machine driver's ---
D=$(mk sofmtl)
cat > $D/cards <<'EOF'
 0 [sofhdadsp      ]: sof-hda-dsp - sof-hda-dsp
                      LENOVO-21N9-ThinkBookPlusG5Station-LNVNB161216
EOF
cat > $D/pcm <<'EOF'
00-00: HDA Analog (*) :  : playback 1 : capture 1
00-03: HDMI1 (*) :  : playback 1
00-04: HDMI2 (*) :  : playback 1
00-05: HDMI3 (*) :  : playback 1
00-06: DMIC Raw (*) :  : capture 1
00-31: Deepbuffer HDA Analog (*) :  : playback 1
EOF
cat > $D/input <<'EOF'
N: Name="sof-hda-dsp Mic"
N: Name="sof-hda-dsp Headphone"
N: Name="sof-hda-dsp HDMI/DP,pcm=3"
N: Name="sof-hda-dsp HDMI/DP,pcm=4"
N: Name="sof-hda-dsp HDMI/DP,pcm=5"
EOF
gen sofmtl gbos-gen sofhdadsp sof-hda-dsp >/dev/null
C=$W/out-sofmtl/gbos-gen/audio_config.xml
wellformed "$C"
chk "mtl: speaker on the analog PCM 0 (the TAS2781 amps hang off the codec's speaker pin)" "$C" "<PlaybackPCM>hw:sofhdadsp,0</PlaybackPCM>"
chk "mtl: internal mic = 'DMIC Raw' pcm 6" "$C" "<CapturePCM>hw:sofhdadsp,6</CapturePCM>"
chk "mtl: headset mic = analog capture pcm 0" "$C" "<CapturePCM>hw:sofhdadsp,0</CapturePCM>"
chk "mtl: HDMI1 pcm 3" "$C" "<PlaybackPCM>hw:sofhdadsp,3</PlaybackPCM>"
chk "mtl: HDMI3 jack" "$C" "<JackDev>sof-hda-dsp HDMI/DP,pcm=5</JackDev>"
nochk "mtl: no deep-buffer PCM used" "$C" ",31<"
nochk "mtl: no leftover PCH" "$C" "PCH"

# ---- 5e. output volumes at the codec's own maxima (usb/audio/volmax.sh; issues 31/32) ------------------------------------------
# Codec files as /proc/asound/cardN/codec#M prints them (hda_proc.c): each control under the node that owns it.
codec() { # codec FILE NAME STEPS : a Realtek-style codec, Speaker/Headphone on DACs 0x02/0x03 with STEPS steps, Beep (0x1f) on a mixer
  cat > "$1" <<EOF
Codec: Realtek $2
Address: 0
Default Amp-Out caps: N/A
Node 0x02 [Audio Output] wcaps 0x41d: Stereo Amp-Out
  Control: name="Speaker Playback Volume", index=0, device=0
    ControlAmp: chs=3, dir=Out, idx=0, ofs=0
  Device: name="$2 Analog", type="Audio", device=0
  Amp-Out caps: ofs=$3, nsteps=$3, stepsize=0x01, mute=0
  Amp-Out vals:  [0x00 0x00]
Node 0x03 [Audio Output] wcaps 0x41d: Stereo Amp-Out
  Control: name="Headphone Playback Volume", index=0, device=0
    ControlAmp: chs=3, dir=Out, idx=0, ofs=0
  Amp-Out caps: ofs=$3, nsteps=$3, stepsize=0x01, mute=0
Node 0x0b [Audio Mixer] wcaps 0x20010b: Stereo Amp-In
  Control: name="Beep Playback Volume", index=0, device=0
    ControlAmp: chs=3, dir=In, idx=4, ofs=0
  Amp-In caps: ofs=0x17, nsteps=0x1f, stepsize=0x05, mute=1
EOF
}
# Galaxy Book2 Pro 360 (ALC298, 0x7f steps) on SOF: the ENVY tree plus codec files
cp -R $W/sof $W/gb; mkdir -p $W/gb/card0
codec "$W/gb/card0/codec#0" ALC298 0x7f
printf 'Codec: Intel Alderlake HDMI\nNode 0x03 [Audio Output] wcaps 0x6611: 8-Channels Digital\n' > "$W/gb/card0/codec#2"
msg=$(gen gb gbos-gen sofhdadsp sof-hda-dsp); M=$W/out-gb/gbos-gen/mixer_paths.xml
wellformed "$M"
chk "volmax ALC298: Master 127"            "$M" '<ctl name="Master Playback Volume" value="127" />'
chk "volmax ALC298: Speaker 127 127"       "$M" '<ctl name="Speaker Playback Volume" value="127 127" />'
chk "volmax ALC298: Headphone 127 127"     "$M" '<ctl name="Headphone Playback Volume" value="127 127" />'
nochk "volmax ALC298: no 87 left"          "$M" 'value="87'
case "$msg" in *"volmax: volume maxima Beep=31 Headphone=127 Master=127 Speaker=127; set in mixer_paths.xml"*)
  ok "volmax ALC298: logged, Beep (an input) does not lower Master";; *) bad "volmax ALC298 message: $msg";; esac
# A codec with fewer steps than 87 (0x4a, the QEMU hda-duplex codec): 87 is out of range there
cp -R $W/sof $W/low; mkdir -p $W/low/card0; codec "$W/low/card0/codec#0" ALCX 0x4a
gen low gbos-gen sofhdadsp sof-hda-dsp >/dev/null; M=$W/out-low/gbos-gen/mixer_paths.xml
chk "volmax 0x4a: Master 74"               "$M" '<ctl name="Master Playback Volume" value="74" />'
chk "volmax 0x4a: Speaker 74 74"           "$M" '<ctl name="Speaker Playback Volume" value="74 74" />'
# A control offset (ControlAmp ofs) lowers the maximum: hda_codec.c get_amp_max_value
cp -R $W/sof $W/ofs; mkdir -p $W/ofs/card0; codec "$W/ofs/card0/codec#0" ALCO 0x57
sed 's/^\(    ControlAmp: chs=3, dir=Out, idx=0, \)ofs=0$/\1ofs=7/' "$W/ofs/card0/codec#0" > "$W/ofs/c" && mv "$W/ofs/c" "$W/ofs/card0/codec#0"
gen ofs gbos-gen sofhdadsp sof-hda-dsp >/dev/null
chk "volmax ofs=7 on 0x57: Speaker 80 80"  "$W/out-ofs/gbos-gen/mixer_paths.xml" '<ctl name="Speaker Playback Volume" value="80 80" />'
# The usual Realtek 0x57: unchanged, and so is a card without codec files (the ENVY tree above)
cp -R $W/sof $W/r87; mkdir -p $W/r87/card0; codec "$W/r87/card0/codec#0" ALC255 0x57
msg=$(gen r87 gbos-gen sofhdadsp sof-hda-dsp)
cmp -s "$W/out-r87/gbos-gen/mixer_paths.xml" "$W/out-sof/gbos-gen/mixer_paths.xml" \
  && ok "volmax 0x57: mixer_paths.xml identical to the one without codec files" || bad "volmax 0x57 changed mixer_paths.xml"
case "$msg" in *"nothing to change"*) ok "volmax 0x57: logged as nothing to change";; *) bad "volmax 0x57 message: $msg";; esac
chk "volmax: card without codec files keeps 87" "$W/out-sof/gbos-gen/mixer_paths.xml" '<ctl name="Speaker Playback Volume" value="87 87" />'
# The PCH path in hook.sh: volmax.sh on a copy of the shipped template (Master only)
mkdir -p $W/pchvm/card0 $W/pchcp; codec "$W/pchvm/card0/codec#0" ALC298 0x7f; cp "$TPL/mixer_paths.xml" $W/pchcp/
sh "$ROOT/usb/audio/volmax.sh" $W/pchvm/card0 $W/pchcp/mixer_paths.xml >/dev/null && ok "volmax PCH copy: exit 0 when changed" || bad "volmax PCH copy: exit status"
chk "volmax PCH copy: Master 127"          "$W/pchcp/mixer_paths.xml" '<ctl name="Master Playback Volume" value="127" />'
[ "$(grep -c . $W/pchcp/mixer_paths.xml)" = "$(grep -c . $TPL/mixer_paths.xml)" ] && ok "volmax PCH copy: no lines added or lost" || bad "volmax PCH copy: line count"
cp "$TPL/mixer_paths.xml" $W/pchcp/
sh "$ROOT/usb/audio/volmax.sh" $W/nonexistent $W/pchcp/mixer_paths.xml >/dev/null && bad "volmax: exit 0 without codec files" || ok "volmax: exit 1 without codec files"
cmp -s "$W/pchcp/mixer_paths.xml" "$TPL/mixer_paths.xml" && ok "volmax: file untouched without codec files" || bad "volmax: file changed without codec files"
H=$ROOT/usb/hook.sh
chk "hook: PCH path runs volmax.sh on a copy" "$H" 'VMSG=$(sh $G/audio/volmax.sh /proc/asound/card$CNO /tmp/gbos-audio/$AC/mixer_paths.xml) && ASRC=/tmp/gbos-audio/$AC'

# ---- 6. hook.sh: card id PCH still takes the static files, and generation is guarded -------------------------------------
H=$ROOT/usb/hook.sh
chk "hook: PCH uses the shipped config"   "$H" 'ASRC=$G/audio/gbos-hda-pch; ADEF=$G/audio/audio_default.xml'
chk "hook: generation only for non-PCH"   "$H" 'if [ -n "$CID" ] && [ "$CID" != PCH ]; then'
chk "hook: no-card path leaves vendor audio configuration untouched" "$H" 'hook: no ALSA card; leaving vendor audio configuration untouched'
chk "hook: calls the generator"           "$H" 'sh $G/audio/gen_config.sh'
chk "hook: clears stale sound nodes after HDA re-probe" "$H" 'rm -f /dev/snd/*'
chk "hook: rebuilds sound nodes from sysfs after HDA re-probe" "$H" 'for _snd in /sys/class/sound/*; do'
chk "hook: creates sound nodes with sysfs device numbers" "$H" 'mknod -m 0660 "/dev/snd/$_snd_name" c "$_snd_major" "$_snd_minor"'
nochk "hook: no temporary early audio diagnostic service" "$H" 'gbos_audiodiag_early'
nochk "hook: no temporary early audio diagnostic trigger" "$H" 'start gbos_audiodiag_early'
chk "hook: Surface mixer_paths only on the Surface (DMI), not on every sofsoundwire card" "$H" "grep -qi 'Surface Laptop.*7th Edition' /sys/class/dmi/id/product_name"
chk "hook: Latitude 7320 mixer path requires matching card, model, and board" "$H" "grep -qi '0TNW0H' /sys/class/dmi/id/board_name"
chk "hook: Latitude 7320 mixer path is installed only for its SoundWire card" "$H" 'cp $G/audio/mixer_paths_latitude_7320.xml /tmp/gbos-audio/$AC/mixer_paths.xml'
chk "hook: Latitude 7320 audio config is installed with its mixer path" "$H" 'cp $G/audio/audio_config_latitude_7320.xml /tmp/gbos-audio/$AC/audio_config.xml'
chk "hook: generated config is copied into the tmpfs" "$H" 'cp -r $ASRC/. $A/$AC/'
nochk "hook: no bind mount of the initramfs /tmp config" "$H" 'mount --bind $ASRC $A/$AC'
sh -n "$H" && sh -n "$GEN" && ok "sh -n hook.sh gen_config.sh" || bad "syntax"

# ---- 6a. Dell Latitude 7320 Detachable: RT1316 DAC and RT711/RT714 capture routes ---------------------------------
M7320=$ROOT/usb/audio/mixer_paths_latitude_7320.xml
AC7320=$ROOT/usb/audio/audio_config_latitude_7320.xml
wellformed "$M7320" "$AC7320"
python3 - "$M7320" > "$W/latitude7320-mixer.py.out" 2>&1 <<'EOF'
import sys, xml.etree.ElementTree as E
r = E.parse(sys.argv[1]).getroot()
speaker = r.find("./path[@name='Speaker']")
controls = {c.get('name'): c.get('value') for c in speaker.findall('ctl')}
want = {'rt1316-1 RX Channel Select': 'L,R', 'rt1316-1 DAC Switch': '1 1'}
if controls != want:
    print(f'FAIL controls {controls}, want {want}')
    sys.exit(1)
for path, name in (('Mic', 'rt711 FU0F Capture Switch'),
                   ('InternalMic', 'rt714 FU02 Capture Switch')):
    entries = r.findall(f"./path[@name='{path}']/ctl")
    if [(c.get('name'), c.get('value')) for c in entries] != [(name, '1')]:
        print(f'FAIL {path} controls {[(c.get("name"), c.get("value")) for c in entries]}')
        sys.exit(1)
print('PYOK')
EOF
if [ $? = 0 ]; then ok "7320 mixer: speaker DAC and RT711/RT714 capture switches are route-scoped"; else bad "7320 mixer path: $(cat "$W/latitude7320-mixer.py.out")"; fi
python3 - "$AC7320" > "$W/latitude7320-config.py.out" 2>&1 <<'EOF'
import sys, xml.etree.ElementTree as E
h = E.parse(sys.argv[1]).getroot().find('HiFi')
got = {(name, field): h.findtext(f'{name}/{field}')
       for name, field in (('Speaker', 'PlaybackPCM'), ('Headphone', 'PlaybackPCM'),
                           ('Mic', 'CapturePCM'), ('Mic', 'CaptureMixerElem'),
                           ('Mic', 'JackDev'), ('InternalMic', 'CapturePCM'))}
want = {('Speaker', 'PlaybackPCM'): 'hw:sofsoundwire,2',
        ('Headphone', 'PlaybackPCM'): 'hw:sofsoundwire,0',
        ('Mic', 'CapturePCM'): 'hw:sofsoundwire,1',
        ('Mic', 'CaptureMixerElem'): 'rt711 FU0F',
        ('Mic', 'JackDev'): 'sof-soundwire Headset Jack',
        ('InternalMic', 'CapturePCM'): 'hw:sofsoundwire,4'}
if got != want:
    print(f'FAIL got {got}, want {want}')
    sys.exit(1)
print('PYOK')
EOF
if [ $? = 0 ]; then ok "7320 config: InternalMic PCM 4, headset Mic PCM 1 / RT711"; else bad "7320 config: $(cat "$W/latitude7320-config.py.out")"; fi

# ---- 6b. mixer diagnostics: use bundled amixer when optional tinymix is absent --------------------------------------------
M=$ROOT/usb/audio/mixdiag.sh
sh -n "$M" && ok "sh -n audio/mixdiag.sh" || bad "mixdiag syntax"
D=$W/mixdiag
mkdir -p "$D/a11y/lib" "$D/a11y/bin" "$D/a11y/share/alsa" "$D/logs/boot-test"
cat > "$D/a11y/lib/ld-musl-x86_64.so.1" <<'EOF'
#!/bin/sh
[ "$1" = "--library-path" ] || exit 1
shift 2
exec "$@"
EOF
cat > "$D/a11y/bin/amixer" <<'EOF'
#!/bin/sh
printf 'fake amixer: %s (config=%s, config_dir=%s)\n' "$*" "$ALSA_CONFIG_PATH" "$ALSA_CONFIG_DIR"
EOF
chmod +x "$D/a11y/lib/ld-musl-x86_64.so.1" "$D/a11y/bin/amixer"
GBOS_AUDIO_DIAG_ROOT=$D sh "$M" test
grep -q '(using bundled amixer)' "$D/logs/boot-test/mixer.txt" \
  && grep -q 'fake amixer: -D hw:0 contents' "$D/logs/boot-test/mixer.txt" \
  && grep -q "$D/a11y/share/alsa/alsa.conf" "$D/logs/boot-test/mixer.txt" \
  && grep -q "config_dir=$D/a11y/share/alsa" "$D/logs/boot-test/mixer.txt" \
  && ok "mixdiag: bundled amixer uses explicit card and bundled ALSA config" \
  || bad "mixdiag: bundled amixer fallback did not run"

# ---- 7. card selection: skip the GPU's HDMI audio, whatever it is called ----------------------------------------------------
sed -n '/^# --- audio card selection/,/^# --- end audio card selection ---/p' "$H" > $W/cardsel.sh
. $W/cardsel.sh
mkcards() { # dir; then "id|name|codec,codec" per card, in order
  d=$1; shift; rm -rf $d; mkdir -p $d; i=0; : > $d/cards
  for c in "$@"; do
    id=${c%%|*}; r=${c#*|}; nm=${r%%|*}; cs=${r#*|}
    printf ' %s [%-15s]: HDA-Intel - %s\n                      %s at 0x1 irq 1\n' $i "$id" "$nm" "$nm" >> $d/cards
    mkdir -p $d/card$i; j=0
    for cd in $(echo "$cs" | tr ',' ' '); do [ "$cd" = - ] && continue; printf 'Codec: %s\nAddress: %s\n' "$(echo $cd | tr '_' ' ')" $j > "$d/card$i/codec#$j"; j=$((j+1)); done
    i=$((i+1))
  done
}
selcheck() { # name expected_id dir
  got=$(ASOUND=$3 gbos_audio_card | sed -n 's/^ *[0-9]* \[\([^] ]*\) *\].*/\1/p')
  [ "$got" = "$2" ] && ok "card: $1 -> $got" || bad "card: $1: wanted $2, got '$got'"
}
mkcards $W/a1 "Generic|HD-Audio Generic|ATI_R6xx_HDMI" "Generic_1|HD-Audio Generic|Realtek_ALC255"
selcheck "AMD Ryzen laptop (both cards named HD-Audio Generic)" Generic_1 $W/a1
mkcards $W/a2 "NVidia|HDA NVidia|Nvidia_GPU_8c_HDMI/DP" "PCH|HDA Intel PCH|Realtek_ALC295,Intel_Kabylake_HDMI"
selcheck "NVIDIA dGPU is card 0" PCH $W/a2
mkcards $W/a3 "sofhdadsp|sof-hda-dsp|Realtek_ALC285,Intel_Kabylake_HDMI"
selcheck "SOF HDA with the codec's own HDMI" sofhdadsp $W/a3
mkcards $W/a3b "sofhdadsp|sof-hda-dsp|Realtek_ALC287,Intel_MeteorLake_HDMI"
selcheck "Meteor Lake SOF HDA (Realtek codec + Intel HDMI codec, codec mask 5)" sofhdadsp $W/a3b
mkcards $W/a4 "PCH|HDA Intel PCH|Realtek_ALC3254,Intel_Skylake_HDMI"
selcheck "plain Intel laptop" PCH $W/a4
mkcards $W/a5 "sofsoundwire|sof-soundwire|-" "HDMI|HDA Intel HDMI|Intel_HDMI"
selcheck "SoundWire card (no HDA codec files) falls back to the name rule" sofsoundwire $W/a5
mkcards $W/a6 "NVidia|HDA NVidia|-" "PCH|HDA Intel PCH|-"
selcheck "no codec files at all: name rule" PCH $W/a6

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo AUDIO_CONFIG_TEST_PASS
exit $fail
