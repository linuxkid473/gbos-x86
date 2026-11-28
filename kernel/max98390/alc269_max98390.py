#!/usr/bin/env python3
# The alc269.c part of thesofproject/linux PR 5616 at bfa17b7c1a43 (Kevin Cuperus, GPL-2.0), applied by anchor because the
# patch's context does not match this 6.18.21 tree. Same code as the PR: a fixup that binds four MAX98390 HDA side codecs
# (I2C, ACPI MAX98390) as components, its model name, and the five Samsung SSIDs. Called by kernel/max98390/apply.sh.
# Usage: alc269_max98390.py <kernel source root>. Idempotent.
import sys

p = sys.argv[1] + '/sound/hda/codecs/realtek/alc269.c'
s = open(p).read()
if 'ALC298_FIXUP_SAMSUNG_MAX98390_4_AMPS' in s:
    print('alc269_max98390: already')
    sys.exit(0)


def sub(old, new):
    global s
    assert s.count(old) == 1, 'anchor mismatch: ' + old
    s = s.replace(old, new)


# 1. the fixup function, after the CS35L41 ones
sub("static void alc287_fixup_legion_16achg6_speakers(struct hda_codec *cdc, const struct hda_fixup *fix,\n",
    "/* gbos: MAX98390 HDA side codecs, kernel/max98390 (thesofproject/linux PR 5616) */\n"
    "static void max98390_fixup_i2c_four(struct hda_codec *codec, const struct hda_fixup *fix, int action)\n"
    "{\n"
    "\tcomp_generic_fixup(codec, action, \"i2c\", \"MAX98390\", \"-%s:00-max98390-hda.%d\", 4);\n"
    "}\n\n"
    "static void alc287_fixup_legion_16achg6_speakers(struct hda_codec *cdc, const struct hda_fixup *fix,\n")

# 2. enum
sub("\tALC298_FIXUP_SAMSUNG_AMP_V2_4_AMPS,\n",
    "\tALC298_FIXUP_SAMSUNG_AMP_V2_4_AMPS,\n\tALC298_FIXUP_SAMSUNG_MAX98390_4_AMPS,\n")

# 3. fixup table entry
sub("\t[ALC298_FIXUP_SAMSUNG_HEADPHONE_VERY_QUIET] = {\n",
    "\t[ALC298_FIXUP_SAMSUNG_MAX98390_4_AMPS] = {\n"
    "\t\t.type = HDA_FIXUP_FUNC,\n"
    "\t\t.v.func = max98390_fixup_i2c_four\n"
    "\t},\n"
    "\t[ALC298_FIXUP_SAMSUNG_HEADPHONE_VERY_QUIET] = {\n")

# 4. quirks (after the Book3 360, as in the PR)
quirks = [
    (0xca07, 'Samsung Galaxy Book4 Pro 14-inch (NP940XGK)'),
    (0xc890, 'Samsung Galaxy Book4 Pro 16 inch (NP960XGK)'),
    (0xc892, 'Samsung Galaxy Book4 Pro 360 (NP960QGK)'),
    (0xc1d8, 'Samsung Galaxy Book4 Ultra (NP960XGL)'),
    (0xc1da, 'Samsung Galaxy Book5 Pro 360 (NP960QHA)'),
]
for ssid, _ in quirks:
    assert 'SND_PCI_QUIRK(0x144d, 0x%04x,' % ssid not in s, 'SSID already in the table: %04x' % ssid
anchor = '\tSND_PCI_QUIRK(0x144d, 0xca06, "Samsung Galaxy Book3 360 (NP730QFG)", ALC298_FIXUP_SAMSUNG_HEADPHONE_VERY_QUIET),\n'
sub(anchor, anchor + ''.join('\tSND_PCI_QUIRK(0x144d, 0x%04x, "%s", ALC298_FIXUP_SAMSUNG_MAX98390_4_AMPS),\n' % q for q in quirks))

# 5. model name
anchor = '\t{.id = ALC298_FIXUP_SAMSUNG_AMP_V2_4_AMPS, .name = "alc298-samsung-amp-v2-4-amps"},\n'
sub(anchor, anchor + '\t{.id = ALC298_FIXUP_SAMSUNG_MAX98390_4_AMPS, .name = "alc298-samsung-max98390-4-amps"},\n')

open(p, 'w').write(s)
print('alc269_max98390: applied')
