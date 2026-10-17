#!/usr/bin/env python3
"""Make a replacement-kernel .config for a Whiskey Lake laptop (i5-8365U, UHD 620).

  configure_whl.py RECOVERY_KERNEL.config OUT.config

Starts from the Googlebook recovery kernel's embedded config and applies the
overrides below, adapted from GBRUNCH's AMD replacement-kernel profile
(tools/build_amd_replacement_kernel.py). Run `make olddefconfig` afterwards.
"""
import re
import sys

OVERRIDES = {
    'LOCALVERSION': '"-gbos-whl"', 'LOCALVERSION_AUTO': 'n',
    # Gen9.5 graphics: xe.ko only supports Gen12+, so build i915.
    'DRM_I915': 'm', 'DRM_SIMPLEDRM': 'y', 'SYSFB_SIMPLEFB': 'y',
    # Let rebuilt modules use any export (GBRUNCH found the trimmed list too small).
    'TRIM_UNUSED_KSYMS': 'n', 'MODULE_SIG_PROTECT': 'n', 'MODULE_SIG_PROTECT_LIST': '""',
    # No Rust toolchain is pinned here, so use the C binder like GBRUNCH.
    'RUST': 'n', 'ANDROID_BINDER_IPC': 'y', 'ANDROID_BINDERFS': 'y',
    'ANDROID_BINDER_DEVICES': '"binder,hwbinder,vndbinder"', 'DMABUF_HEAPS_SYSTEM': 'y',
    # Smaller, faster build.
    'DEBUG_INFO_NONE': 'y', 'DEBUG_INFO': 'n', 'DEBUG_INFO_BTF': 'n',
    'DEBUG_INFO_BTF_MODULES': 'n', 'IKHEADERS': 'n',
    'DEBUG_INFO_DWARF4': 'n', 'DEBUG_INFO_DWARF5': 'n', 'DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT': 'n',
    # Boot-critical laptop hardware built in.
    'BLK_DEV_NVME': 'y', 'NVME_CORE': 'y', 'SATA_AHCI': 'y', 'ATA': 'y',
    'SCSI': 'y', 'BLK_DEV_SD': 'y',
    'USB_XHCI_HCD': 'y', 'USB_XHCI_PCI': 'y', 'USB_STORAGE': 'y', 'USB_UAS': 'y',
    'HID_GENERIC': 'y', 'USB_HID': 'y', 'HID_MULTITOUCH': 'y',
    'I2C_HID': 'y', 'I2C_HID_ACPI': 'y', 'I2C_DESIGNWARE_CORE': 'y', 'I2C_DESIGNWARE_PLATFORM': 'y',
    'PINCTRL_CANNONLAKE': 'y', 'KEYBOARD_ATKBD': 'y', 'MOUSE_PS2': 'y',
    'BLK_DEV_DM': 'y', 'DM_VERITY': 'y', 'EXT4_FS': 'y', 'EROFS_FS': 'y',
    # Intel Wi-Fi (the Latitude 5300 has a CNVi 9560, 8086:9df0).
    'CFG80211': 'm', 'MAC80211': 'm', 'WLAN_VENDOR_INTEL': 'y', 'IWLWIFI': 'm', 'IWLMVM': 'm',
    # Battery/AC: Google builds these as modules, which won't load on this kernel.
    'ACPI_BATTERY': 'y', 'ACPI_AC': 'y',
    # Touchpad: the I2C controllers (8086:9de8/9de9) are LPSS PCI devices. Without this the touchpad
    # is only its PS/2 fallback (no scrolling or gestures).
    'MFD_INTEL_LPSS': 'y', 'MFD_INTEL_LPSS_PCI': 'y',
    # Audio: legacy HDA + Realtek ALC3254 (ALC269 family) + HDMI, instead of Google's SOF modules.
    # Needs snd_intel_dspcfg.dsp_driver=1 on the cmdline (isa_emu_patch/debug_cmdline.txt). The internal
    # mic is on the codec (pin 0x12), so it works with this driver too.
    'SND_HDA_INTEL': 'y', 'SND_HDA_GENERIC': 'y', 'SND_HDA_CODEC_REALTEK': 'y', 'SND_HDA_CODEC_ALC269': 'y',
    'SND_HDA_CODEC_HDMI': 'y', 'SND_HDA_CODEC_HDMI_INTEL': 'y', 'SND_HDA_I915': 'y',
    # Bluetooth: the 9560's Bluetooth is USB (8087:0aaa); Google's kernel only has PCIe Bluetooth.
    'BT_HCIBTUSB': 'm', 'BT_HCIBTUSB_AUTOSUSPEND': 'y',
}

# Generic Intel gen6+ laptop support (kernel #31). Everything is a module, loaded at boot by modalias coldplug
# (usb/hwdetect.sh), so one image covers many machines. The Latitude 5300 built-ins above stay built in.
GENERIC = {}
def _m(*names):
    for n in names:
        GENERIC[n] = 'm'
def _y(*names):
    for n in names:
        GENERIC[n] = 'y'
# Wi-Fi: Intel (old DVM + new MVM), Realtek, MediaTek, Qualcomm Atheros, Broadcom.
_y('WLAN_VENDOR_REALTEK', 'WLAN_VENDOR_MEDIATEK', 'WLAN_VENDOR_ATH', 'WLAN_VENDOR_BROADCOM')
_m('IWLDVM', 'RTW88', 'RTW88_PCI', 'RTW88_8822BE', 'RTW88_8822CE', 'RTW88_8723DE', 'RTW88_8821CE',
   'RTW89', 'RTW89_PCI', 'RTW89_8852AE', 'RTW89_8852BE', 'RTW89_8852CE', 'RTW89_8851BE', 'RTW89_8922AE',
   'MT76_CORE', 'MT7921E', 'MT7921U', 'MT7925E', 'MT7615E', 'MT76x2E',
   'ATH10K', 'ATH10K_PCI', 'ATH11K', 'ATH11K_PCI', 'ATH12K', 'BRCMFMAC', 'BRCMFMAC_PCIE', 'BRCMFMAC_USB')
# Bluetooth (USB btusb covers Intel/Realtek/MediaTek/Qualcomm/Broadcom; Intel 8087:* on USB, newer on PCIe).
_m('BT_INTEL', 'BT_RTL', 'BT_MTK', 'BT_QCA', 'BT_BCM', 'BT_INTEL_PCIE', 'BT_HCIBTUSB')
_y('BT_HCIBTUSB_BCM', 'BT_HCIBTUSB_MTK', 'BT_HCIBTUSB_RTL', 'BT_HCIBTUSB_INTEL')
# Graphics/brightness: i915 handles gen6..Raptor Lake; ACPI video + backlight class give brightness keys.
_m('DRM_I915')
_y('ACPI_VIDEO', 'BACKLIGHT_CLASS_DEVICE', 'DRM_I915_BACKLIGHT_HELPER')
_m('DRM_XE')  # Arrow Lake and newer; optional
# Touchpad / keyboard / sensors / ISH
_y('PINCTRL_INTEL', 'PINCTRL_SUNRISEPOINT', 'PINCTRL_ICELAKE', 'PINCTRL_TIGERLAKE', 'PINCTRL_ALDERLAKE',
   'PINCTRL_METEORLAKE', 'PINCTRL_JASPERLAKE', 'PINCTRL_ELKHARTLAKE', 'PINCTRL_LAKEFIELD', 'PINCTRL_GEMINILAKE',
   'PINCTRL_BROXTON', 'PINCTRL_CEDARFORK', 'PINCTRL_EMMITSBURG', 'PINCTRL_LEWISBURG', 'PINCTRL_TIGERLAKE',
   'INTEL_PCH_THERMAL', 'SERIAL_8250_DW', 'SPI_PXA2XX', 'SPI_PXA2XX_PCI')
_m('INTEL_ISH_HID', 'INTEL_ISH_HOST', 'HID_RMI', 'RMI4_CORE', 'RMI4_I2C', 'RMI4_SMB', 'RMI4_F11', 'RMI4_F12', 'RMI4_F30',
   'MOUSE_PS2_SYNAPTICS', 'MOUSE_ELAN_I2C', 'MOUSE_SYNAPTICS_I2C', 'HID_SENSOR_HUB', 'I2C_I801', 'I2C_SMBUS',
   'HID_ALPS', 'HID_GOOGLE_HAMMER', 'INPUT_SPARSEKMAP')
# Laptop platform drivers (hotkeys, rfkill, fan/thermal, battery thresholds). The vendor menus gate the drivers.
_y('X86_PLATFORM_DEVICES', 'X86_PLATFORM_DRIVERS_DELL', 'X86_PLATFORM_DRIVERS_HP', 'ACPI_VIDEO', 'RFKILL_INPUT')
_m('INTEL_PMT_TELEMETRY', 'INTEL_PMT_CLASS')
_m('DELL_LAPTOP', 'DELL_WMI', 'DELL_SMBIOS', 'DELL_WMI_DESCRIPTOR', 'THINKPAD_ACPI', 'HP_WMI', 'IDEAPAD_LAPTOP',
   'ASUS_WMI', 'ASUS_NB_WMI', 'ACER_WMI', 'SAMSUNG_LAPTOP', 'MSI_WMI', 'TOSHIBA_ACPI', 'SONY_LAPTOP',
   'FUJITSU_LAPTOP', 'PANASONIC_LAPTOP', 'LG_LAPTOP', 'INTEL_HID_EVENT', 'INTEL_VBTN', 'INTEL_WMI_SBL_FW_UPDATE',
   'INTEL_PMC_CORE', 'INT3400_THERMAL', 'INT340X_THERMAL', 'INTEL_RAPL', 'X86_PKG_TEMP_THERMAL', 'WMI',
   'CHROME_PLATFORMS', 'ACPI_WMI', 'RFKILL_INPUT')
# Storage / card reader / webcam / ethernet
_y('MMC', 'USB_VIDEO_CLASS')
_m('MMC_SDHCI', 'MMC_SDHCI_PCI', 'MMC_REALTEK_PCI', 'MISC_RTSX_PCI', 'MISC_RTSX_USB', 'R8169', 'E1000E', 'IGC',
   'IGB', 'R8152', 'USB_USBNET', 'USB_NET_CDCETHER', 'USB_RTL8152', 'USB_NET_AX88179_178A', 'USB_NET_CDC_NCM')
# Audio: every common HDA codec, plus SOF for machines with DMIC/SoundWire (HDA is still preferred when it works).
# SND_SOC_INTEL_SKL_HDA_DSP_GENERIC_MACH is the machine driver (skl_hda_dsp_generic) SOF registers for HDA codecs + DMIC:
# without it the DSP boots but no card ever appears (Dell Latitude 7391, issue #1).
_m('SND_HDA_CODEC_CIRRUS', 'SND_HDA_CODEC_CS420X', 'SND_HDA_CODEC_CS421X', 'SND_HDA_CODEC_CS8409', 'SND_HDA_CODEC_CONEXANT',
   'SND_HDA_CODEC_SIGMATEL', 'SND_HDA_CODEC_VIA', 'SND_HDA_CODEC_ANALOG', 'SND_HDA_CODEC_CMEDIA', 'SND_HDA_CODEC_SI3054',
   'SND_HDA_CODEC_SENARYTECH', 'SND_HDA_CODEC_CA0132', 'SND_HDA_SCODEC_CS35L41_I2C', 'SND_HDA_SCODEC_CS35L56_I2C',
   'SND_HDA_SCODEC_TAS2781_I2C', 'SND_HDA_SCODEC_COMPONENT')
_y('SND_SOC_SOF_TOPLEVEL', 'SND_SOC_SOF_INTEL_TOPLEVEL', 'SND_SOC_INTEL_MACH', 'SND_SOC_INTEL_USER_FRIENDLY_LONG_NAMES',
   'SND_SOC_SOF_HDA_LINK', 'SND_SOC_SOF_HDA_AUDIO_CODEC', 'SND_SOC_SOF_HDA_LINK_BASELINE')
_m('SND_SOC_SOF_PCI', 'SND_SOC_SOF_ACPI', 'SND_SOC_SOF_INTEL_COMMON', 'SND_SOC_SOF_ICELAKE', 'SND_SOC_SOF_TIGERLAKE',
   'SND_SOC_SOF_ELKHARTLAKE', 'SND_SOC_SOF_JASPERLAKE', 'SND_SOC_SOF_ALDERLAKE', 'SND_SOC_SOF_METEORLAKE',
   'SND_SOC_SOF_LUNARLAKE', 'SND_SOC_SOF_COMETLAKE', 'SND_SOC_SOF_CANNONLAKE', 'SND_SOC_SOF_APOLLOLAKE',
   'SND_SOC_SOF_GEMINILAKE', 'SND_SOC_SOF_HDA_PROBES', 'SOUNDWIRE', 'SOUNDWIRE_INTEL',
   'SND_SOC_INTEL_SOUNDWIRE_SOF_MACH', 'SND_SOC_INTEL_SOF_RT5682_MACH', 'SND_SOC_INTEL_SOF_SSP_AMP_MACH',
   'SND_SOC_INTEL_SOF_CS42L42_MACH', 'SND_SOC_INTEL_SOF_ES8336_MACH', 'SND_SOC_INTEL_SOF_NAU8825_MACH',
   'SND_SOC_INTEL_SOF_DA7219_MACH', 'SND_SOC_INTEL_SOF_PCM512x_MACH', 'SND_SOC_INTEL_SOF_CIRRUS_COMMON',
   'SND_SOC_INTEL_HDA_DSP_COMMON', 'SND_SOC_INTEL_SKL_HDA_DSP_GENERIC_MACH', 'SND_SOC_DMIC', 'SND_SOC_HDAC_HDMI', 'SND_SOC_RT5682_I2C', 'SND_SOC_RT5682_SDW',
   'SND_SOC_RT711_SDW', 'SND_SOC_RT711_SDCA_SDW', 'SND_SOC_RT715_SDW', 'SND_SOC_RT1308_SDW', 'SND_SOC_RT1316_SDW',
   'SND_SOC_RT1318_SDW', 'SND_SOC_RT712_SDCA_SDW', 'SND_SOC_RT722_SDCA_SDW', 'SND_SOC_MAX98357A', 'SND_SOC_MAX98373_I2C',
   'SND_SOC_MAX98390', 'SND_SOC_CS42L42', 'SND_SOC_ES8336', 'SND_SOC_NAU8825', 'SND_SOC_DA7219', 'SND_SOC_RT1011',
   'SND_SOC_RT1015', 'SND_SOC_RT1015P', 'SND_SOC_TAS2562')

# These must come out exactly as requested after olddefconfig.
REQUIRED = ['DRM_I915', 'TRIM_UNUSED_KSYMS', 'MODULE_SIG_PROTECT', 'ANDROID_BINDER_IPC', 'BLK_DEV_NVME']


def parse(text):
    values = {}
    for line in text.splitlines():
        m = re.match(r'CONFIG_(\w+)=(.*)', line)
        if m:
            values[m.group(1)] = m.group(2)
            continue
        m = re.match(r'# CONFIG_(\w+) is not set', line)
        if m:
            values[m.group(1)] = 'n'
    return values


def render(values):
    return ''.join('# CONFIG_%s is not set\n' % k if v == 'n' else 'CONFIG_%s=%s\n' % (k, v)
                   for k, v in values.items())


def main():
    if sys.argv[1] == '--check':
        resolved = parse(open(sys.argv[2]).read())
        bad = {k: resolved.get(k, 'n') for k in REQUIRED if resolved.get(k, 'n') != OVERRIDES[k]}
        missed = {k: resolved.get(k, 'n') for k, v in OVERRIDES.items() if resolved.get(k, 'n') != v}
        print('not resolved as requested:', missed or 'none')
        gmiss = sorted(k for k, v in GENERIC.items() if resolved.get(k, 'n') != v)
        print('generic symbols not resolved (dependency unmet or absent in this tree):', gmiss or 'none')
        sys.exit(1 if bad else 0)
    args = [a for a in sys.argv if a != '--generic']
    values = parse(open(args[1]).read())
    values.update(OVERRIDES)
    if '--generic' in sys.argv:
        values.update(GENERIC)
    open(args[2], 'w').write(render(values))


if __name__ == '__main__':
    main()
