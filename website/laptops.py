"""Laptops people have tried gbOS86 on (reports from the community and the maintainer). One list, used by the Hardware
page, the docs and (by hand) README.md. Add a row per report; keep it to what the reporter actually said.

status: works | partial | pending | no
"""
from html import escape

UPDATED = '2026-11-18'

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
         works='Wi-Fi, Bluetooth, display and brightness (external monitor), keyboard (external, USB receiver), all USB ports, install to the SATA SSD',
         broken='',
         notes='First tried live on kernel #32, when the installer only targeted NVMe or eMMC. The installer now also installs to SATA disks, and the reporter has installed it to the SATA M.2 SSD. '
               'Sound was not tested; no touchpad, webcam or built-in screen to test.'),
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
    dict(name='MSI Sword 16 HX', cpu='Core i7-13700HX (Raptor Lake)', status='works',
         works='Reported as working perfectly out of the box and used daily for a week with no problems',
         broken='Games (the report names them as the one exception)',
         notes='Hardware Reports thread on Discord, 7 replies. Per-device detail is in the thread.'),
    dict(name='Dell Inspiron 14 5440', cpu='Core i3-1305U (Raptor Lake-U), Realtek RTL8852BE Wi-Fi', status='works',
         works='Display (i915), Wi-Fi, Bluetooth, sound (after the speaker volume change), TalkBack screen reader from the first boot, installed next to another OS',
         broken='',
         notes='Kernel #46. The reporter uses a screen reader. TalkBack worked on the first boot; the spoken boot menu was silent because the HDA controller waits for the i915 audio component (fixed 2026-11-05). '
               'The speakers were about half as loud as they should be until the <code>Master Playback Volume</code> change (87) shipped.'),
    dict(name='HP OmniBook 5', cpu='Ryzen 7 8840U, Radeon 780M', status='works',
         works='Boots to the desktop (AMD graphics through the project\'s RADV Vulkan build), sound',
         broken='Speaker volume reported low',
         notes='Kernel #37. AMD CPUs and graphics are experimental. It crash-looped on Mesa 26 until the RADV protected-memory fix, then hung after the audio re-probe until the <code>/dev/snd</code> node fix; '
               'the reporter confirmed sound after that. Other devices were not listed.'),
    dict(name='Acer Aspire A515-46', cpu='Ryzen 3 3350U (Realtek ALC255 audio)', status='works',
         works='Boots to the desktop, sound (speakers)',
         broken='Speaker volume reported low',
         notes='Kernel #37. AMD CPUs are experimental. The first boots had no sound (the generated audio config named jack devices that do not exist), then a black screen after the HDA re-probe; '
               'both were fixed and the reporter confirmed sound. Other devices were not listed.'),
    dict(name='HP Spectre x360 14-ef0xxx', cpu='Core i7-1255U (Alder Lake)', status='works',
         works='Reported as everything working, live from USB',
         broken='Built-in camera (IPU6 with a Hynix Hi-556 sensor: no driver loaded)',
         notes='Kernel #37, GitHub issue 10 (closed). Sound needed the generated audio config to live on the stick instead of <code>/tmp</code>, and the ALC245 needed Speaker and Headphone volumes instead of a Master volume.'),
    dict(name='Dell Inspiron 14 Plus 7440', cpu='Core Ultra 9 185H (Meteor Lake), Arc graphics, 1 TB NVMe', status='works',
         works='Hardware graphics, Wi-Fi, Bluetooth, USB Ethernet, keyboard, Dell hotkeys and lid switch, touchpad, sound, Widevine, Play Store installs and updates, Night Light, Magic Pointer, root adb, the Linux Terminal VM (Debian, Linux apps in their own windows)',
         broken='',
         notes='Kernel #37, installed by hand next to Fedora. GitHub issue 17 (closed): the reporter found five bugs, each with a fix (sound on Meteor Lake needing the <code>sof-ace-tplg</code> topology, Widevine, Play Store installs, the touchpad), and confirmed the list after applying them.'),
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
    dict(name='Lenovo Legion 5 Pro 16IAH7H', cpu='Core i7-12700H (Alder Lake) + RTX 3070 Ti, Realtek ALC257', status='partial',
         works='Mostly working, per the report',
         broken='NVIDIA RTX 3070 Ti (hybrid graphics, no driver); no sound on the first build (the audio config targeted the NVIDIA HDMI card; fixed since)',
         notes='Kernel #37, GitHub issue 14 (closed). The reporter traced the missing sound to the audio card choice; the fix, which skips GPU HDMI audio cards, is in <code>hook.sh</code>.'),
    dict(name='MSI Modern 15 B7M', cpu='Ryzen 5 7530U (Barcelo), Radeon, MediaTek MT7922 Wi-Fi', status='partial',
         works='Desktop (AMD graphics through the project\'s RADV Vulkan build), Wi-Fi (with the <code>mt7921</code> workaround), speakers (after the audio fixes)',
         broken='HDMI audio; on an early build the mouse pointer drew as a red square (not rechecked)',
         notes='Experimental: AMD CPUs and graphics. The Wi-Fi driver oopsed while parsing optional CLC firmware data and the laptop panicked until <code>MODOPTS="mt7921_common.disable_clc=1"</code> was set in <code>gbos.cfg</code> '
               '(the reporter confirmed it). Laptop audio was confirmed on 2026-10-25; HDMI audio is not.'),
    dict(name='HP ENVY x360 2-in-1 15-ew0xxx', cpu='Core i7-1255U (Alder Lake), 16 GB, 512 GB NVMe', status='partial',
         works='Boots from the live USB; tagged "mostly works" by the reporter',
         broken='Blank or unresponsive app windows (the <code>/dev/null</code> fix of 2026-11-16 is not confirmed here yet)',
         notes='Kernel #46. Animations were also forced off by a bug (fixed 2026-11-12). See the <code>/dev/null</code> note under the Pixelbook Go.'),
    dict(name='Dell Latitude 7320 2-in-1', cpu='Core i7-1180G7 (Tiger Lake), 16 GB, 512 GB', status='partial',
         works='Wi-Fi, Bluetooth, touchpad (from the post summary; the thread has the rest)',
         broken='Sound: the SoundWire codecs were not loaded (the modules were added on 2026-11-05, not confirmed)',
         notes='Live USB, kernel 6.18.21-gbos-whl. Hardware Reports thread on Discord, 9 replies.'),
    dict(name='Framework Laptop 13 Pro', cpu='Core Ultra 5 325 (Panther Lake), 16 GB, 1 TB NVMe', status='pending',
         works='Boots; black screen after choosing live boot on the reporter\'s stick',
         broken='Display on sticks built before firmware v35',
         notes='Cause found: the <code>xe</code> graphics driver could not load its Panther Lake GuC firmware. Firmware release v35 includes it; a patch zip exists for older sticks. '
               'Waiting for the reporter to confirm.'),
    dict(name='Google Pixelbook Go (Atlas)', cpu='Intel (details not given)', status='pending',
         works='',
         broken='Chrome does not load pages and Google sign-in fails',
         notes='Same cause as the app freezes reported on several laptops: <code>/dev/null</code> was a regular file, created by stage0 before its <code>mknod</code>, and Android\'s second zygote rejects it. '
               'Found on 2026-11-16 and repaired in <code>hook.sh</code>; an Update (menu 1) on the stick picks it up. Waiting for a report that it works.'),
    dict(name='Dell Vostro 3400', cpu='Core i3-1115G4 (Tiger Lake), 1366x768 panel', status='pending',
         works='Boots to the home screen, Intel graphics (i915)',
         broken='Apps open as blank, unresponsive windows; Chrome times out waiting for its GPU channel',
         notes='Reported on Google build R5, kernel 6.18.21-gbos-whl. The <code>/dev/null</code> cause matches its logs (every access denial on <code>/dev/null</code> names a regular file); the fix is not confirmed on this laptop yet.'),
    dict(name='Dell Latitude 5310 2-in-1', cpu='Intel 10th gen (Comet Lake), UHD graphics', status='pending',
         works='Desktop, Wi-Fi, sound, Google Photos (installed to the NVMe)',
         broken='Chrome and Firefox never show a page',
         notes='GitHub issue 43 (open). The BIOS SATA mode had to be AHCI for the installer to see the NVMe. Same <code>/dev/null</code> cause as above; waiting for a retest.'),
    dict(name='HP EliteBook 840 G6', cpu='Core i7-8665U (Whiskey Lake), UHD 620, 16 GB', status='pending',
         works='Boots and runs from the internal NVMe; Intel graphics and scanout',
         broken='Most apps crash and Chrome times out waiting for its GPU channel',
         notes='GitHub issue 44 (open). Same <code>/dev/null</code> cause as above; waiting for a retest.'),
    dict(name='HP 14s-dq2509TU', cpu='Core i3-1115G4 (Tiger Lake)', status='pending',
         works='',
         broken='Did not boot: KernelSU panicked at boot on 11th-gen Intel (CET-IBT, "Missing ENDBR")',
         notes='Kernel #38. The reporter confirmed <code>ibt=off</code> gets past it; it is now in the kernel command line (kernel #42). Waiting for a retest.'),
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
