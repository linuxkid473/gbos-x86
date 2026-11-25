#!/bin/sh
# Infinix INBOOK X3 Slim (PCI SSID 2782:0250, Realtek ALC269VB): the BIOS declares the speaker on the wrong pin, so the
# internal speakers are silent. Backport of upstream "ALSA: hda/realtek: Add quirk for Infinix INBOOK X3 Slim" (v2 by
# Gunal Seenivasagan, applied by Takashi Iwai to the sound tree, 2026-07); our android17-6.18 tree does not have it yet.
# Pin 0x14 not connected, pin 0x1b internal speaker. Only 2782:0250 is affected. Issue 56 asked how to do this by hand;
# the generic way is HDA_PINS in gbos.cfg (usb/hwdetect.sh), this makes that laptop work without it.
# Built in (CONFIG_SND_HDA_CODEC_ALC269=y): kernel relink. Idempotent; drop it once the tree has the upstream commit.
set -e
cd /build/src/sound/hda/codecs/realtek
grep -q 'SND_PCI_QUIRK(0x2782, 0x0250,' alc269.c && { echo already; exit 0; }
python3 - alc269.c <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()

def sub(old, new):
    global s
    assert s.count(old) == 1, "anchor mismatch: " + old
    s = s.replace(old, new)

# 1. enum
sub("\tALC269VC_FIXUP_INFINIX_Y4_MAX,\n\tALC269VB_FIXUP_CHUWI_COREBOOK_XPRO,\n",
    "\tALC269VC_FIXUP_INFINIX_Y4_MAX,\n\tALC269VB_FIXUP_INFINIX_INBOOK_X3_SLIM,\n\tALC269VB_FIXUP_CHUWI_COREBOOK_XPRO,\n")

# 2. fixup entry (before the CHUWI one, after Infinix Y4 Max)
sub("""	[ALC269VB_FIXUP_CHUWI_COREBOOK_XPRO] = {
""", """	/* gbos: Infinix INBOOK X3 Slim, kernel/isa_emu_patch/alc269_infinix_inbook_x3.sh (upstream backport) */
	[ALC269VB_FIXUP_INFINIX_INBOOK_X3_SLIM] = {
		.type = HDA_FIXUP_PINS,
		.v.pins = (const struct hda_pintbl[]) {
			{ 0x14, 0x411111f0 }, /* disable, not connected */
			{ 0x1b, 0x90170110 }, /* use as internal speaker */
			{ }
		},
	},
	[ALC269VB_FIXUP_CHUWI_COREBOOK_XPRO] = {
""")

# 3. quirk, sorted by SSID (after 0x0232)
sub("""	SND_PCI_QUIRK(0x2782, 0x0232, "CHUWI CoreBook XPro", ALC269VB_FIXUP_CHUWI_COREBOOK_XPRO),
""", """	SND_PCI_QUIRK(0x2782, 0x0232, "CHUWI CoreBook XPro", ALC269VB_FIXUP_CHUWI_COREBOOK_XPRO),
	SND_PCI_QUIRK(0x2782, 0x0250, "Infinix INBOOK X3 Slim", ALC269VB_FIXUP_INFINIX_INBOOK_X3_SLIM),
""")
open(p, 'w').write(s)
PYEOF
grep -n 'INBOOK_X3_SLIM' alc269.c
