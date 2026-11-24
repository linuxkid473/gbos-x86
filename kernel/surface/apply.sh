#!/bin/sh
# Surface Pro 5 (and the other gen4-6 Surfaces): the linux-surface kernel changes this kernel needs, applied to the kernel source
# root (default /build/src). Run inside gbos-kbuild, after the other patches. Idempotent.
#
#   0001, 0002  mwifiex: PCI bridge reset quirk and bridge_d3 off for Surface gen4+ (Marvell 88W8897 Wi-Fi). Both act only on
#               the Surface models in their DMI tables.
#   0003, 0004  hid: Intel Precise Touch and Stylus (IPTS) driver, drivers/hid/ipts (CONFIG_HID_IPTS): touchscreen and pen on the
#               Surface Pro 4-7 / Book 1-2 / Laptop 1-3; iptsd (usb/surface/iptsd-run.sh) turns its raw frames into input events.
#   iommu       identity mapping for the IPTS HECI function (00:16.4, 8086:9d3e / 8086:34e4). This kernel has
#               INTEL_IOMMU_DEFAULT_ON=y; without it IPTS fails with "DMAR: [DMA Read NO_PASID] ... PTE Read access is not set".
#               Ported from linux-surface's "iommu: Use IOMMU passthrough mode for IPTS" (its context no longer applies to
#               6.18.21) with one change: the quirk also requires DMI sys_vendor "Microsoft Corporation", so no other laptop's
#               DMA mapping changes.
#
# Source: github.com/linux-surface/linux-surface patches/6.18/0003-mwifiex.patch and 0005-ipts.patch at commit 4c057ebc8f38
# (2026-11-23), GPL-2.0, authors as in each patch. The two commits of those series this tree does not need (btusb LE scan
# interval; MEI device id for Ice Lake iTouch) are left out.
set -e
P=${P:-/host/kernel/surface}
SRC=${SRC:-/build/src}
cd "$SRC"

apply_once() { # patch file, a file that exists once it is applied
  if [ -e "$2" ] && patch -p1 -R --dry-run -s -f < "$1" >/dev/null 2>&1; then
    echo "surface: already applied $(basename "$1")"
  else
    patch -p1 -N -s < "$1"
    echo "surface: applied $(basename "$1")"
  fi
}
apply_once $P/0001-mwifiex-Add-quirk-resetting-the-PCI-bridge-on-MS-Sur.patch drivers/net/wireless/marvell/mwifiex/pcie_quirks.c
apply_once $P/0002-mwifiex-pcie-disable-bridge_d3-for-Surface-gen4.patch drivers/net/wireless/marvell/mwifiex/pcie_quirks.c
apply_once $P/0003-hid-Add-support-for-Intel-Precise-Touch-and-Stylus.patch drivers/hid/ipts/main.c
apply_once $P/0004-hid-ipts-Fix-size-of-synthesized-HID-data-report.patch drivers/hid/ipts/main.c

python3 - drivers/iommu/intel/iommu.c <<'EOF'
import sys

path = sys.argv[1]
s = open(path).read()
if 'quirk_iommu_ipts' in s:
    print('surface: iommu IPTS quirk already present')
    sys.exit(0)


def after(anchor, text):
    global s
    if s.count(anchor) != 1:
        sys.exit('surface: iommu anchor not found exactly once: %r' % anchor)
    s = s.replace(anchor, anchor + text)


after('#define IS_AZALIA(pdev) ((pdev)->vendor == 0x8086 && (pdev)->device == 0x3a3e)\n', '''
/* gbos/linux-surface: the IPTS (Intel Precise Touch) HECI function of the Surface Pro 4-7, Book 1-2, Laptop 1-3 */
#define IS_IPTS(pdev) ( \\
		((pdev)->vendor == PCI_VENDOR_ID_INTEL && (pdev)->device == 0x9D3E) || \\
		((pdev)->vendor == PCI_VENDOR_ID_INTEL && (pdev)->device == 0x34E4) \\
	)
''')
after('static int disable_igfx_iommu;\n', 'static int dmar_map_ipts = 1;\n')
after('#define IDENTMAP_AZALIA\t\t4\n', '#define IDENTMAP_IPTS\t\t16\n')
after('''		if ((iommu_identity_mapping & IDENTMAP_AZALIA) && IS_AZALIA(pdev))
			return IOMMU_DOMAIN_IDENTITY;
''', '''
		if ((iommu_identity_mapping & IDENTMAP_IPTS) && IS_IPTS(pdev))
			return IOMMU_DOMAIN_IDENTITY;
''')
if s.count('\tcheck_tylersburg_isoch();\n') != 1:
    sys.exit('surface: iommu anchor check_tylersburg_isoch not found exactly once')
s = s.replace('\tcheck_tylersburg_isoch();\n', '''	if (!dmar_map_ipts)
		iommu_identity_mapping |= IDENTMAP_IPTS;

	check_tylersburg_isoch();
''')
anchor = 'static void quirk_iommu_rwbf(struct pci_dev *dev)\n'
if s.count(anchor) != 1:
    sys.exit('surface: iommu anchor quirk_iommu_rwbf not found exactly once')
s = s.replace(anchor, '''/*
 * IPTS DMA faults with DMA remapping on (linux-surface "iommu: Use IOMMU passthrough mode for IPTS"). gbos: Surfaces only, so the
 * mapping of any other machine that happens to expose 8086:9d3e is unchanged.
 */
static void quirk_iommu_ipts(struct pci_dev *dev)
{
	if (!IS_IPTS(dev))
		return;

	if (!dmi_match(DMI_SYS_VENDOR, "Microsoft Corporation"))
		return;

	if (risky_device(dev))
		return;

	pci_info(dev, "Disabling IOMMU for IPTS\\n");
	dmar_map_ipts = 0;
}
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9D3E, quirk_iommu_ipts);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x34E4, quirk_iommu_ipts);

''' + anchor)
open(path, 'w').write(s)
print('surface: iommu IPTS quirk added')
EOF
