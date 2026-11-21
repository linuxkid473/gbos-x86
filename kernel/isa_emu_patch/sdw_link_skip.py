#!/usr/bin/env python3
"""soundwire_intel: a parameter to leave BIOS-enabled but unpopulated SoundWire links powered off. Run in gbos-kbuild:
    python3 kernel/isa_emu_patch/sdw_link_skip.py [KERNEL_SRC]        (default /build/src)
Only soundwire-intel.ko changes (no kernel image change). Idempotent; an anchor that is not found exactly once aborts without
touching the file.

Latitude 7320 Detachable (TGL, SKU 0A45): the BIOS enables all four SoundWire links (intel-quirk-mask has no BUS_DISABLE for
link 2) and declares an RT1308 amplifier on link 2 that is not fitted. Windows enumerates only RT711 (link 0), RT1316 (link 1)
and RT714 (link 3), and Linux selects sof-tgl-rt711-l0-rt1316-l1-mono-rt714-l3.tplg, which does not use link 2. Linux still
powers link 2 up and starts it: with nothing attached, the master reports "Bus clash for control word" / "Bus clash for data
word" (rate-limited to 11 lines), and 3 s later the runtime suspend cannot stop the clock ("Clock stop failed -110",
"intel_stop_bus: cannot stop clock: -110") before it powers the link down anyway.

snd_intel_sdw_acpi.sdw_link_mask=0xB is not a fix: that match-table entry (tgl_3_in_1_sdca_mono) requires link_mask 0xF
("link_mask should be 0xB, but all links are enabled by BIOS"), so removing link 2 from the mask selects the headset-only
sof-tgl-rt711.tplg (no speakers, no internal microphone).

sdw_link_skip_mask (one bit per link, default 0 = upstream behaviour) marks the link hw_disabled, the same state the BIOS
BUS_DISABLE quirk gives at this level: the link stays in the controller's link mask (machine match unchanged) and its auxiliary
device and ACPI peripherals still exist, but intel_link_startup() returns before powering it up, and every PM callback already
skips hw_disabled links. That is the state link 2 reaches today 3 s after boot, minus the failed start and stop.
usb/hwdetect.sh sets it (4 = link 2) on the Latitude 7320 Detachable only."""
import sys

root = (sys.argv[1] if len(sys.argv) > 1 else '/build/src').rstrip('/')
AUX = 'drivers/soundwire/intel_auxdevice.c'


def edit(path, old, new, label):
    p = root + '/' + path
    s = open(p).read()
    if new in s:
        print('already applied: ' + label)
        return
    if s.count(old) != 1:
        sys.exit('ANCHOR NOT FOUND exactly once for: ' + label + ' in ' + path)
    open(p, 'w').write(s.replace(old, new))
    print('applied: ' + label)


edit(AUX,
     'MODULE_PARM_DESC(sdw_mclk_divider, "SoundWire Intel mclk divider");\n',
     'MODULE_PARM_DESC(sdw_mclk_divider, "SoundWire Intel mclk divider");\n'
     '\n'
     '/* gbOS: links the BIOS enables but nothing is fitted on (Latitude 7320 Detachable link 2), see sdw_link_skip.py */\n'
     'static int gbos_link_skip_mask;\n'
     'module_param_named(sdw_link_skip_mask, gbos_link_skip_mask, int, 0444);\n'
     'MODULE_PARM_DESC(sdw_link_skip_mask, "gbOS: links to leave powered off (one bit per link, 0 = none)");\n',
     'sdw_link_skip_mask parameter')

edit(AUX,
     '\tif (quirk_mask & SDW_INTEL_QUIRK_MASK_BUS_DISABLE)\n'
     '\t\tprop->hw_disabled = true;\n',
     '\tif (quirk_mask & SDW_INTEL_QUIRK_MASK_BUS_DISABLE)\n'
     '\t\tprop->hw_disabled = true;\n'
     '\n'
     '\tif (gbos_link_skip_mask & BIT(bus->link_id)) {\n'
     '\t\tprop->hw_disabled = true;\n'
     '\t\tdev_info(bus->dev, "gbos: link %d left powered off (sdw_link_skip_mask=%#x)\\n",\n'
     '\t\t\t bus->link_id, gbos_link_skip_mask);\n'
     '\t}\n',
     'sdw_link_skip_mask marks the link hw_disabled')
