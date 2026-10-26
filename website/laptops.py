"""Laptops people have tried gbOS86 on (reports from the community and the maintainer). One list, used by the Hardware
page, the docs and (by hand) README.md. Add a row per report; keep it to what the reporter actually said.

status: works | partial | pending | no
"""
from html import escape

UPDATED = '2026-10-26'

STATUS = {'works': 'Works', 'partial': 'Partly works', 'pending': 'Fix pending', 'no': 'Does not work'}

LAPTOPS = [
    dict(name='Dell Latitude 5300', cpu='Core i5-8365U (Whiskey Lake), UHD 620, 16 GB', status='works',
         works='Everything tested: boot, sign-in, Play Store, Wi-Fi, Bluetooth, sound, touchpad with gestures, brightness, battery, install to the internal NVMe',
         broken='Suspend and resume (not supported anywhere yet)',
         notes='The maintainer\'s machine; the project was built on it.'),
    dict(name='Acer Aspire Lite AL15-52', cpu='Core i5-1235U, 8 GB, 512 GB NVMe', status='works',
         works='Wi-Fi, display, touchpad, keyboard, sound, brightness, camera, Bluetooth, SD card',
         broken='',
         notes='Reported on kernel #33 in live mode (earlier on #32). Needed two firmware fixes (Realtek Wi-Fi firmware, and the Intel AX201 Bluetooth firmware name '
               '<code>ibt-0040-1050.sfi</code>), both in firmware release v34 and later. A stick made before that needs a rebuild, or an Update from a fresh stick.'),
    dict(name='Lenovo ThinkPad T470', cpu='Intel 7th gen (Kaby Lake)', status='works',
         works='Reported as working with no setup at all',
         broken='',
         notes='Short report, no details on individual devices yet.'),
    dict(name='Intel NUC8BEH (mini PC)', cpu='Core i3-8109U (Coffee Lake), 8 GB DDR4, 512 GB SATA M.2', status='works',
         works='Wi-Fi, Bluetooth, display and brightness (external monitor), keyboard (external, USB receiver), all USB ports',
         broken='',
         notes='Live USB on kernel #32. Sound was not tested; no touchpad, webcam or built-in screen to test. The SSD is SATA, and the installer only targets NVMe or eMMC, so this one runs live from the stick but cannot install.'),
    dict(name='HP Envy 13-aq0xxx', cpu='Core i5-8265U (Whiskey Lake), 8 GB DDR4, 512 GB Kioxia BG3 NVMe', status='works',
         works='Reported as working great, both live from the USB stick and installed to the NVMe',
         broken='',
         notes='Kernel #32. The report thread (the longest on the forum so far) has the per-device detail; the summary here only says what the reporter said.'),
    dict(name='Dell Latitude 5300 2-in-1', cpu='Core i7-8665U (Whiskey Lake), 16 GB, USB-C storage', status='works',
         works='Wi-Fi (AX200), Bluetooth, sound, touchpad, keyboard, backlight, display, touchscreen, hinge detection, webcam, microphone, Caps Lock LED, Wacom pen, suspend and lid close',
         broken='Auto-rotate, microphone-mute LED',
         notes='Kernel #37, live from a Looper image. Auto-rotate: the ISH sensor hub loaded but no accelerometer driver was built; added to the next kernel (needs a rebuild and a sensors HAL check). The fan runs at full speed and C-states look unused: no evidence yet.'),
    dict(name='Dell Latitude 5290 2-in-1', cpu='Core i5 (model not given)', status='works',
         works='Sound, once the speaker volume is raised (see the note)',
         broken='',
         notes='Short report. Speakers were very quiet; the reporter set <code>Master Playback Volume</code> to <code>87</code> in <code>mixer_paths.xml</code> '
               '(<code>gbos-hda-pch/</code>, in the <code>gbos/audio</code> folder of the stick) and the volume was fine. 87 is the maximum; higher values are not safe for the speakers. '
               'The shipped config uses 87 from 2026-10-26, so a stick built or updated after that needs no manual edit. Nothing else was tested or reported.'),
    dict(name='Lenovo ThinkPad T480', cpu='Core i5-8350U (Kaby Lake R), 16 GB DDR4, Samsung PM981 NVMe', status='works',
         works='Wi-Fi, Bluetooth, sound, touchpad, touchscreen, keyboard, brightness, display, webcam (installed to the NVMe)',
         broken='',
         notes='Kernel #37. Quirks: sometimes slow to wake; Wi-Fi must be toggled off and on after sleep; a Bluetooth pairing request to an already paired phone after reboot; the external battery percentage is shown with two batteries fitted. Not diagnosed yet.'),
    dict(name='Dell Latitude 3380', cpu='Core i5-7200U (Kaby Lake), 8 GB DDR4, 256 GB SSD', status='partial',
         works='Wi-Fi, sound, touchpad, keyboard, brightness, display, webcam (installed)',
         broken='Bluetooth',
         notes='Kernel #42. The Bluetooth chip is a Qualcomm Atheros one (USB <code>0cf3:e007</code>): its firmware loads and <code>hci0</code> comes up, so the cause is not known yet; '
               'it needs <code>logcat.txt</code> from a boot where Bluetooth was switched on. The wired Ethernet chip (Realtek <code>10ec:8168</code>) has no driver; nobody has reported whether that matters.'),
    dict(name='HP OmniBook 7', cpu='Core Ultra 5 225U, 16 GB LPDDR5, 512 GB NVMe', status='partial',
         works='Touchpad, keyboard, brightness, display (live USB)',
         broken='Wi-Fi (Intel Wi-Fi 7 BE201)',
         notes='Sound, webcam and Bluetooth were not tested. A very new platform: Wi-Fi has no fix yet.'),
    dict(name='HP Pavilion Power 15-cb0xx', cpu='Core i7-7700HQ (Kaby Lake) + NVIDIA GTX 1050, 16 GB, NVMe + SATA SSD', status='partial',
         works='Wi-Fi (Intel 7265), Bluetooth, touchpad, keyboard, brightness, display, webcam (live USB)',
         broken='Internal speakers (fixed in the next stick update: the audio config was built for the NVIDIA HDMI card), external display via the HP dock / DisplayLink, NVIDIA dGPU (no driver)',
         notes='Kernel #33. The NVIDIA HDA card was card 0 so the audio HAL config targeted it; hook.sh now skips GPU HDMI cards. DisplayLink and the GTX 1050 have no Android driver.'),
    dict(name='Lenovo ThinkPad T14 Gen 2', cpu='Core i5-1135G7 (Tiger Lake), 32 GB DDR4, Samsung NVMe', status='partial',
         works='Wi-Fi, Bluetooth, touchpad, keyboard, brightness, display, hotkeys (installed)',
         broken='Sound, microphone, webcam',
         notes='The attached hwdetect.txt stops before the USB and module sections, so the sound card state is unknown; the Tiger Lake HDA controller (a0c8) had no driver at that point. Needs a full hwdetect.txt and audio.txt from the installed system.'),
    dict(name='Microsoft Surface Pro 4', cpu='Core i5-6300U (Skylake), Marvell Wi-Fi', status='partial',
         works='Screen, keyboard (Type Cover), touchpad, USB, brightness, microphones',
         broken='Wi-Fi (Marvell 88W8897 over PCIe, driver mwifiex), Bluetooth, touchscreen, cameras',
         notes='Kernel #37. The Marvell driver (mwifiex_pcie) is added in the next kernel; its firmware comes with the next firmware release. Touchscreen and cameras need Surface-specific drivers (IPTS, IPU4) that do not exist here.'),
    dict(name='Dell Inspiron 7391 2-in-1', cpu='Core i7-10510U (Comet Lake), 8 GB LPDDR3, NVMe', status='partial',
         works='Wi-Fi, Bluetooth, touchpad, keyboard, brightness, display, webcam, touchscreen (installed to the NVMe)',
         broken='Sound',
         notes='Tagged "mostly works" by the community. The reporter notes that this laptop uses SOF firmware for its audio on regular Linux, so sound here is probably the SOF path '
               '(the audio HAL config is generated for the card that appears: best effort). Post <code>hwdetect.txt</code> and <code>audio.txt</code> to narrow it down.'),
    dict(name='Realme Book Slim (2021)', cpu='Core i5-1135G7 (Tiger Lake), 8 GB, 512 GB', status='partial',
         works='Wi-Fi, Bluetooth, touchpad, keyboard, brightness, display, webcam (live USB)',
         broken='Sound',
         notes='Live USB only so far. The report did not include <code>hwdetect.txt</code> or the kernel build, so the cause of the missing sound is unknown; a Tiger Lake laptop normally uses the SOF audio driver.'),
    dict(name='Fujitsu Arrows Tab V727 (tablet)', cpu='Core m3-7Y30 (Kaby Lake), 8 GB', status='partial',
         works='Wi-Fi (Intel 8265, <code>wlan0</code>), Bluetooth (<code>hci0</code>, btusb), sound card detected (HDA Intel PCH, headphones and HDMI/DP)',
         broken='Not listed in the report',
         notes='Live USB on kernel #33, running from a USB 2.0 port. Tagged "mostly works" by the community; the list of what fails was not in the post summary.'),
    dict(name='Dell XPS 16 (2026)', cpu='Intel Core Ultra (details not given)', status='partial',
         works='Boots and runs',
         broken='Screen refresh rate, Wi-Fi, camera',
         notes='The reporter says a Linux kernel option fixes the screen on this model. <code>MODOPTS</code> in <code>gbos.cfg</code> can now set driver options, so it can be tried without a rebuild. '
               'Wi-Fi and camera are open; a MIPI camera needs more than a driver.'),
    dict(name='Framework Laptop 13 Pro', cpu='Core Ultra 5 325 (Panther Lake), 16 GB, 1 TB NVMe', status='pending',
         works='Boots; black screen after choosing live boot on the reporter\'s stick',
         broken='Display on sticks built before firmware v35',
         notes='Cause found: the <code>xe</code> graphics driver could not load its Panther Lake GuC firmware. Firmware release v35 includes it; a patch zip exists for older sticks. '
               'Waiting for the reporter to confirm.'),
    dict(name='Microsoft Surface Laptop 7 (Intel, for Business)', cpu='Core Ultra 7 266V (Lunar Lake), 16 GB, 1 TB', status='partial',
         works='Wi-Fi (BE201), internal keyboard, battery and charger, fan, speakers, headset jack, touchpad with gestures and click, Bluetooth, stable picture',
         broken='Internal microphone, HDMI audio, camera, touchscreen, suspend and resume',
         notes='Needed several kernel and boot-script changes (Surface Aggregator, SoundWire audio, <code>XE_PARAMS</code> for the display); see the Surface Laptop 7 section of the README and HANDOFF.md. '
               'Before firmware v35 the screen stayed black (the <code>xe</code> GuC firmware was missing).'),
    dict(name='ASUS X441NA', cpu='Celeron N3350 (Apollo Lake)', status='no',
         works='',
         broken='Kernel panic right after choosing live boot: "Attempted to kill init! exitcode=0x00000004"',
         notes='This CPU has no AVX, and Google\'s init uses AVX from its first instructions (exit code 4 = illegal instruction). It cannot be fixed with drivers. Sticks built after the AVX check stop with a message instead of a panic. Same for many other Celeron, Pentium and Atom parts.'),
    dict(name='HP 14-ne0013dx', cpu='Intel (details not given)', status='no',
         works='',
         broken='Did not work for the reporter',
         notes='No logs yet. Post <code>gbos/logs/boot-N/hwdetect.txt</code> from the stick and we can look.'),
]


def table_html():
    """Cards, not a table: five text columns do not fit next to each other (name column hogged the width)."""
    cards = []
    for l in LAPTOPS:
        rows = ''
        if l['works']:
            rows += '<dt>Works</dt><dd>%s</dd>' % escape(l['works'])
        if l['broken']:
            rows += '<dt>Not working</dt><dd>%s</dd>' % escape(l['broken'])
        cards.append('<article class="lap"><div class="lap-head"><div><h3>%s</h3><p class="cpu">%s</p></div><span class="badge %s">%s</span></div>'
                     '<dl>%s</dl><p class="note">%s</p></article>' % (escape(l['name']), escape(l['cpu']), l['status'], STATUS[l['status']], rows, l['notes']))
    return ('<div class="laptops">%s</div>'
            '<p class="mut">Based on reports from people who tried it, last updated %s. Results change as fixes land: a card says what was reported, not what the latest build can do. '
            'Post yours in <a href="/forum/c/hardware">Hardware Compatibility</a> with your <code>hwdetect.txt</code>.</p>') % ('\n'.join(cards), UPDATED)


def summary():
    n = {k: sum(1 for l in LAPTOPS if l['status'] == k) for k in STATUS}
    return '%d laptops tried: %d work, %d partly, %d with a fix pending, %d not working' % (len(LAPTOPS), n['works'], n['partial'], n['pending'], n['no'])
