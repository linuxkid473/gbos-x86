#!/bin/sh
# Samsung Galaxy Book4 Pro / Pro 360 / Ultra, Book5 Pro 360 (ALC298 + four Maxim MAX98390 I2C amps): speaker driver.
# Mainline has no HDA side-codec driver for the MAX98390, so these laptops' speakers are silent (issue 80, Galaxy Book4 Pro 16"
# NP960XGK, ALC298 SSID 144d:c890: hwdetect "i2c-MAX98390:00 ... driver=", kmsg "Unable to find a module for acpi:MAX98390:").
#
# 0001  thesofproject/linux PR 5616 at bfa17b7c1a43 (Kevin Cuperus, GPL-2.0, not merged yet), minus the ASoC ACPI-match hunk
#       (see the patch header):
#       - serial-multi-instantiate: ACPI MAX98390 -> four I2C clients "max98390-hda" (needs SERIAL_MULTI_INSTANTIATE, configure_whl.py),
#         drivers/acpi/scan.c: the MAX98390 node is no longer enumerated as one plain I2C client;
#       - sound/hda/codecs/side-codecs/max98390_hda*.c: snd-hda-scodec-max98390(-i2c). Each amp: reset, I2S setup, a DSM table by
#         I2C address (0x38/0x39 woofer L/R, 0x3c/0x3d tweeter L/R), amp on at probe;
#       - alc269.c (alc269_max98390.py, by anchor): ALC298_FIXUP_SAMSUNG_MAX98390_4_AMPS (model alc298-samsung-max98390-4-amps) for 144d:ca07, c890, c892, c1d8, c1da;
#       - sound/soc/codecs/max98390.c: exports max98390_regmap for the HDA driver.
#       Known gaps (upstream state): nothing re-initialises the amps after a system suspend; the DSM tables are generic, not per-model.
#
# Applied to the kernel source root (default /build/src) inside gbos-kbuild. Idempotent through a stamp, like kernel/surface/apply.sh.
# Drop it once the tree has an upstream MAX98390 HDA driver.
set -e
P=${P:-/host/kernel/max98390}
SRC=${SRC:-/build/src}
cd "$SRC"

# git apply, not patch: the gbos-kbuild image has git but no patch(1). It works on a plain tree (no .git).
STAMP=.gbos-max98390
mkdir -p $STAMP
apply_once() { # patch file
  n=$(basename "$1")
  if [ -f "$STAMP/$n" ]; then
    echo "max98390: already applied $n"
  else
    git apply -p1 "$1"
    : > "$STAMP/$n"
    echo "max98390: applied $n"
  fi
}
apply_once $P/0001-ALSA-hda-MAX98390-side-codec-for-Samsung-Galaxy-Book4.patch
# The PR's alc269.c hunks don't match this tree's context; the same changes by anchor (idempotent on its own).
python3 $P/alc269_max98390.py "$SRC"
grep -q '"MAX98390", (unsigned long)&max98390_hda' drivers/platform/x86/serial-multi-instantiate.c
grep -q 'SND_PCI_QUIRK(0x144d, 0xc890,' sound/hda/codecs/realtek/alc269.c
