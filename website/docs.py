"""Documentation pages (static HTML). Rendered inside pages.docs_layout()."""

DOCS = []  # (slug, title, summary, html), filled below in reading order


def doc(slug, title, summary):
    def deco(fn):
        DOCS.append((slug, title, summary, fn))
        return fn
    return deco


@doc('overview', 'Overview', 'What gbOS86 is, what is tested, and how the pieces fit.')
def overview():
    return '''<h1>Overview</h1>
<p class="lead">gbOS86 runs Google's Googlebook OS (codename <em>lapis</em>, built on Android) on ordinary Intel x86 laptops.</p>
<p>Googlebook OS is made for Google's own hardware: a particular CPU, a security chip (GSC), a protected VM (pKVM/Trusty) and Google's kernel and firmware. gbOS86 keeps Google's userspace as it ships and replaces what is underneath it:</p>
<ul>
<li><strong>A different kernel</strong>: Android common kernel 6.18 built for generic Intel laptops, with a CPU-instruction emulator.</li>
<li><strong>A boot stage</strong> (<em>stage0</em>) that finds the system on a USB stick or the internal NVMe drive, loads drivers, mounts Google's partitions and starts Google's unmodified Android <code>init</code>.</li>
<li><strong>Binary patches</strong> to the libraries that execute the most missing CPU instructions.</li>
<li><strong>Software stand-ins</strong> for the security hardware an ordinary laptop does not have.</li>
<li><strong>Hardware detection</strong> that loads the right Wi-Fi, Bluetooth, audio, input and platform drivers on each boot.</li>
</ul>
<h2>Status</h2>
<table class="spec"><tbody>
<tr><td>Works (Dell Latitude 5300)</td><td>Boot to desktop, setup and Google sign-in, Play Store installs, Wi-Fi, Bluetooth, sound (speakers, headset jack, internal mic, HDMI), touchpad with gestures, battery/AC, persistent data on the internal NVMe drive, screen-lock password, correct display colours.</td></tr>
<tr><td>Works on other laptops</td><td>Acer Aspire Lite AL15-52 (i5-1235U), Lenovo ThinkPad T470 and an Intel NUC8BEH (live from USB; its SATA drive cannot be installed to), as reported by the people who tried them. See <a href="/docs/tested-laptops">Tested laptops</a>.</td></tr>
<tr><td>Partly works</td><td>HP OmniBook 7 (no Wi-Fi) and Dell XPS 16 (screen refresh, Wi-Fi and camera). A Framework Laptop 13 Pro had a black screen, now fixed in the firmware and waiting for confirmation; an HP 14-ne0013dx did not work. Newer Core Ultra machines are the weak spot.</td></tr>
<tr><td>Not done</td><td>Suspend/resume, Night Light, fingerprint readers, IPU6 cameras, discrete GPUs, OTA updates (manual updates only), Linux Terminal VM, auth-bound keystore keys.</td></tr></tbody></table>
<h2>The pages in this section</h2>
<ol><li><a href="/docs/install">Install</a>: the USB stick, booting, installing and updating.</li>
<li><a href="/docs/how-it-works">How it works</a>: the boot chain.</li>
<li><a href="/docs/cpu-emulation">CPU emulation and patches</a>: why it is slow, and what makes it faster.</li>
<li><a href="/docs/hardware-detection">Hardware detection</a>: drivers, firmware and the per-boot report.</li>
<li><a href="/docs/google-hardware">Google-only hardware</a>: what is replaced and how.</li>
<li><a href="/docs/configuration">Configuration</a>: every <code>gbos.cfg</code> option.</li>
<li><a href="/docs/logs">Logs and debugging</a>: finding out what went wrong.</li>
<li><a href="/docs/updating">Updating</a>: new gbOS86 builds and new Google builds.</li>
<li><a href="/docs/building">Building from source</a>.</li>
<li><a href="/docs/safety">Safety and limits</a>.</li>
<li><a href="/docs/changelog">Changelog</a>.</li></ol>'''


@doc('install', 'Install', 'Create the USB stick, boot, try it live, install or update.')
def install():
    return None  # rendered by pages.guide (kept as the canonical install guide)


@doc('how-it-works', 'How it works', 'The boot chain from UEFI to Android init.')
def how_it_works():
    return '''<h1>How it works</h1>
<p class="lead">From the moment you press the power button to Google's Android <code>init</code> taking over.</p>
<pre><code>UEFI firmware
 └─ EFI/BOOT/BOOTX64.EFI      kernel (android 6.18 + patches) with stage0 built in as its initramfs
     └─ stage0 (busybox shell script)
         ├─ finds the gbos volume: the USB stick (FAT label GBOS) or the NVMe install (GBOSSYS)
         ├─ opens the boot menu (stick only): live / install-or-update / boot installed
         ├─ loads the graphics driver, starts logging to gbos/logs/boot-N/
         ├─ MODE=install: runs gbos/install.sh, never boots Android
         ├─ mounts super.img's read-only erofs partitions (system, vendor, product, ...) with loop + dm-linear
         ├─ runs gbos/hook.sh: every Android-side workaround, plus hardware detection
         ├─ mounts /data and /metadata (NVMe ext4, or RAM for a live boot)
         └─ switch_root into /system/bin/init (Google's Android init, unmodified)</code></pre>
<h2>One kernel image, one volume</h2>
<p>The firmware boots <code>BOOTX64.EFI</code> from the EFI partition. Everything else lives on the <strong>gbos volume</strong>: a FAT32 partition holding <code>gbos/</code> (the Android images, scripts, patched libraries, firmware, kernel modules, audio configs, installer tools, logs). On the stick it is labelled <code>GBOS</code>; on an installed laptop it is the <code>GBOSSYS</code> partition of the NVMe drive.</p>
<h2>Why most behaviour is in scripts, not the kernel</h2>
<p>stage0 and <code>hook.sh</code> are plain shell scripts on the volume. The workarounds for Google's missing hardware, the driver loading and the audio configuration can be changed without rebuilding the kernel: edit a file on the stick, boot, look at the log. Only the CPU emulator, the graphics patches and the built-in drivers need a kernel build.</p>
<h2>The Android partitions</h2>
<p><code>super.img</code> is Google's dynamic-partition image (system, vendor, product, system_ext and the two dlkm partitions, slot A) rebuilt to stay under FAT32's 4 GB file limit. stage0 attaches it as a loop device and maps each logical partition with <code>dm-linear</code>, so Google's own partition layout is used as is. Bind mounts then replace individual files (patched libraries, configs, a hidden service declaration) without altering the images.</p>
<h2>The boot menu</h2>
<p>With the stick inserted, a 10 second menu offers <b>1</b> live from USB, <b>2</b> install or update on the internal SSD, <b>3</b> boot the installed system (the default when an install exists, otherwise 1). The <code>MODE</code> setting in <code>gbos.cfg</code> can skip the menu.</p>
<h2>Installed layout (NVMe)</h2>
<table class="spec"><tbody><tr><td>GBOS_EFI</td><td>512 MB, the EFI system partition with <code>BOOTX64.EFI</code>.</td></tr>
<tr><td>GBOSSYS</td><td>8 GB FAT32: the <code>gbos/</code> files and the per-boot logs.</td></tr>
<tr><td>GBOS_META</td><td>64 MB ext4: Android's <code>/metadata</code>.</td></tr>
<tr><td>GBOS_DATA</td><td>the rest, ext4: Android's <code>/data</code> (apps, accounts, files).</td></tr></tbody></table>'''


@doc('cpu-emulation', 'CPU emulation and patches', 'Why Google\'s OS needs emulation here, and how it is made fast.')
def cpu():
    return '''<h1>CPU emulation and patches</h1>
<p class="lead">Google's binaries use CPU instructions that most laptops do not have. gbOS86 handles that in two layers.</p>
<h2>The problem</h2>
<p>The Googlebook OS build uses newer instruction-set extensions: <strong>GFNI</strong>, <strong>AVX-VNNI</strong>, <strong>SHA-NI</strong>, 256-bit <strong>VAES</strong> and <strong>VPCLMULQDQ</strong>, and <strong>AVX-IFMA</strong>. A CPU without them raises an <em>invalid opcode</em> (#UD) fault when it meets one, and the process would crash. Newer CPUs run them natively; Intel laptops from about 2015 to 2021 do not.</p>
<h2>Layer 1: kernel emulation</h2>
<p>The kernel's #UD handler decodes the faulting instruction, executes the equivalent on the saved vector state, and resumes the program. It is correct, covered by about 13,000 known-answer tests, and slow: roughly a microsecond per emulated instruction. An emulator self-test runs in QEMU on every kernel build and must report 16 of 16 passing.</p>
<h2>Layer 2: binary patches to the hottest code</h2>
<p>Profiling the boot showed two functions causing most of the traps, so they are replaced with versions that avoid the missing instructions:</p>
<table class="spec"><thead><tr><th>Library</th><th>Change</th><th>Share of traps removed</th></tr></thead><tbody>
<tr><td><code>libz.so</code> (system, vendor)</td><td><code>adler32_z</code> always takes its scalar path.</td><td>about half of all emulation</td></tr>
<tr><td><code>libcrypto.so</code> (system, vendor, Conscrypt APEX)</td><td>SHA-NI SHA-256 replaced by scalar SHA-256.</td><td>about half of what was left, and Play Store installs stop crawling</td></tr></tbody></table>
<h2>Binding them early</h2>
<p>The patched files are bind-mounted over the originals. <code>early_patch.sh</code> does this before stage0's first partition-mapping call, because every mapping process loads <code>libcrypto</code>, whose start-up integrity check runs SHA-256 through SHA-NI: about 3.7 million emulated instructions and roughly 2 seconds of boot. <code>hook.sh</code> repeats the binds later as a fail-safe and logs <code>already patched</code> if the early pass got there first.</p>
<h2>Hash pinning</h2>
<p>A patch is only applied when the sha256 of the original file matches the one it was made from. When Google ships a new build the files change, the patches stop applying <em>silently</em> and boot gets slower, so the logs state it explicitly (<code>NOT patched</code>, with the hash that was found). The hashes are recorded in <code>usb/patched/MANIFEST</code>, which <code>tools/port_build.sh</code> regenerates for a new build (see <a href="/docs/updating">Updating</a>).</p>
<h2>Faster on newer CPUs</h2>
<p>Machines that have the instructions natively never trap, so the emulation costs nothing there. The slowest case is a pre-2021 CPU such as the Whiskey Lake i5-8365U the project is tested on.</p>'''


@doc('hardware-detection', 'Hardware detection', 'How drivers and firmware are chosen at each boot, and the hwdetect.txt report.')
def hwdetect():
    return '''<h1>Hardware detection</h1>
<p class="lead">One image for many laptops: at every boot gbOS86 reads the machine's devices and loads the matching drivers.</p>
<h2>What happens at boot</h2>
<ol><li><strong>Module tree.</strong> The volume's <code>gbos/modules/</code> (about 400 kernel modules, built and indexed for the running kernel) is bind-mounted over <code>/lib/modules</code>.</li>
<li><strong>Coldplug.</strong> The script reads the <code>modalias</code> string of every device in sysfs (PCI, USB, ACPI, HID, I2C, DMI) and asks <code>modprobe</code> to load whatever module claims each one. It repeats up to four times, because loading a driver (an audio controller, a USB hub) creates new devices.</li>
<li><strong>Firmware.</strong> The kernel's firmware loader is pointed at <code>gbos/firmware</code>, a trimmed copy of linux-firmware and sof-bin. After the switch to Android it points at the same folder as seen from there.</li>
<li><strong>Wait and report.</strong> It waits a few seconds for Wi-Fi, Bluetooth and sound to appear and writes <code>logs/boot-N/hwdetect.txt</code>.</li></ol>
<p>Every step is non-fatal: a laptop with an unsupported device simply ends up without that driver. Set <code>HWDETECT=0</code> to disable it. If a stick has no <code>gbos/modules</code>, the older Latitude 5300-specific Wi-Fi and Bluetooth sequence runs instead.</p>
<h2>What is covered</h2>
<table class="spec"><thead><tr><th>Area</th><th>Drivers</th></tr></thead><tbody>
<tr><td>Graphics</td><td><code>i915</code> (Skylake to Raptor Lake, loaded by stage0), <code>xe</code> for newer parts (needs its own firmware; see <a href="/docs/tested-laptops">Tested laptops</a>), ACPI video and the backlight class.</td></tr>
<tr><td>Wi-Fi</td><td>Intel <code>iwldvm</code> (7260, 7265) and <code>iwlmvm</code> (8265 to BE200), Realtek <code>rtw88</code>/<code>rtw89</code>, MediaTek <code>mt7921</code>/<code>mt7922</code>/<code>mt7925</code>, Qualcomm <code>ath10k</code>/<code>ath11k</code>, Broadcom <code>brcmfmac</code>.</td></tr>
<tr><td>Bluetooth</td><td><code>btusb</code> with Intel, Realtek, MediaTek, Qualcomm and Broadcom support, plus Intel PCIe Bluetooth.</td></tr>
<tr><td>Audio</td><td>Legacy HDA with Realtek, Conexant, Cirrus, IDT/SigmaTel, VIA and other codecs, or SOF (Intel DSP, digital mics, SoundWire). The kernel's <code>snd_intel_dspcfg</code> picks one per machine.</td></tr>
<tr><td>Input</td><td>Intel LPSS I2C-HID (most touchpads), Intel Integrated Sensor Hub, PS/2 (Synaptics, ALPS, Elan), Synaptics RMI4.</td></tr>
<tr><td>Platform</td><td>Dell, HP, ThinkPad, IdeaPad, Asus, Acer, Samsung, MSI, Sony, Fujitsu, LG and Intel HID hotkey/rfkill/fan drivers; Intel PMC and thermal drivers.</td></tr>
<tr><td>Other</td><td>SD readers (Realtek, SDHCI), USB webcams (UVC), Ethernet (r8169, e1000e, igc, USB adapters).</td></tr></tbody></table>
<h2>Audio</h2>
<p>Google's audio service matches a configuration to the ALSA card id. Nearly every Intel PCH laptop using legacy HDA has the card id <code>PCH</code>, for which a complete configuration ships. For another main card (for example a SOF card) a configuration is generated from the PCH one with that card's id and name. That is a best-effort mapping: mixer control names differ per codec, so some laptops will have no sound or wrong routing until a config is written for them. Reports help.</p>
<h2>The report</h2>
<p><code>hwdetect.txt</code> contains the kernel version, DMI vendor and model, <strong>PCI devices that have no driver</strong>, USB devices with their drivers, input devices, network interfaces, Bluetooth controllers, backlight devices with their range, sound cards, rfkill switches, <strong>missing-firmware</strong> kernel messages, and the list of loaded modules. It is the first thing asked for in any hardware report.</p>
<h2>Firmware layout</h2>
<p>The files are in <code>gbos/firmware/</code> using the names the kernel requests: Intel Wi-Fi <code>iwlwifi-*.ucode</code> at the top level, <code>intel/ibt-*</code> for Intel Bluetooth, <code>i915/</code> for graphics microcontrollers, <code>intel/sof*</code> for SOF audio, and folders for rtw88, rtw89, mediatek, ath10k, ath11k, brcm, qca, rtl_bt and cirrus.</p>'''


@doc('tested-laptops', 'Tested laptops', 'Which laptops people have tried gbOS86 on, and what worked.')
def tested_laptops():
    import laptops
    return '''<h1>Tested laptops</h1>
<p class="lead">''' + laptops.summary() + '''. Every row is a report from someone who booted the stick, not a guarantee.</p>
''' + laptops.table_html() + '''
<h2>Patterns so far</h2>
<ul><li><strong>Business and mainstream laptops from 2017 to 2022</strong> (Latitude 5300, ThinkPad T470, Aspire Lite AL15-52) work with little or no setup.</li>
<li><strong>Newer Core Ultra machines</strong> boot but run into missing pieces: <code>xe</code> graphics needs its firmware (<em>Framework 13 Pro</em>), and Wi-Fi 7 cards (<em>OmniBook 7</em>, <em>XPS 16</em>) have no working driver and firmware pair yet.</li>
<li><strong>Missing firmware</strong> is the usual cause of a device that does nothing. <code>hwdetect.txt</code> lists it under "missing firmware".</li></ul>
<h2>Report yours</h2>
<p>Boot the stick, then open <code>gbos/logs/boot-N/hwdetect.txt</code> on another computer. Post it with the laptop's model, CPU, RAM and what works in <a href="/forum/c/hardware">Hardware Compatibility</a>, or on <a href="https://discord.gg/h9AD9fSnDT" rel="noopener" target="_blank">Discord</a>. To try a driver option on your own, see <code>MODOPTS</code> in <a href="/docs/configuration">Configuration</a>.</p>'''


@doc('google-hardware', 'Google-only hardware', 'The services and devices that do not exist on other laptops, and what replaces them.')
def google_hw():
    return '''<h1>Google-only hardware</h1>
<p class="lead">Google's laptops carry a security chip and run a protected VM. An ordinary laptop has neither, so services that wait for them would hang the boot. Each one is hidden or replaced.</p>
<table class="spec"><thead><tr><th>Symptom</th><th>Cause</th><th>Fix</th></tr></thead><tbody>
<tr><td>"System process not responding"</td><td>LockSettings waits forever for the Weaver service, which lives in Trusty.</td><td>Weaver's declaration is hidden from the manifest.</td></tr>
<tr><td>Setup crashes: "Failed to create shared Bitmap"</td><td>No <code>/dev/ashmem</code> (the Rust ashmem driver is not built).</td><td>A small shim implements the ashmem ioctls on top of memfd.</td></tr>
<tr><td>Setting a password hangs Settings</td><td>Gatekeeper is in Trusty and never answers.</td><td>A software Gatekeeper (<code>soft_gk</code>).</td></tr>
<tr><td>keystore2 aborts, KeyMint hangs</td><td>KeyMint and StrongBox need the security chip.</td><td>A patched <code>libkm_compat</code> (software KeyMint); the StrongBox APEX is hidden.</td></tr>
<tr><td>Linux Terminal VM never starts</td><td>Secretkeeper is in Trusty.</td><td>Its declaration is hidden.</td></tr>
<tr><td>Logs truncated, stick corrupted</td><td>Android's volume daemon ran a filesystem check on the stick while mounted.</td><td>The USB rule is removed from the fstab so Android never touches the stick.</td></tr>
<tr><td>No sound</td><td>Google uses SOF; a laptop's own audio is usually legacy HDA.</td><td>The audio HAL configuration is replaced (see <a href="/docs/hardware-detection">Hardware detection</a>).</td></tr>
<tr><td>White veil, lifted blacks</td><td>Google's display compositor loads a gamma curve meant for its own graphics driver into i915 every frame.</td><td>i915 ignores userspace gamma, degamma and colour matrices; the panel is treated as full-range sRGB.</td></tr>
<tr><td>Everything too big</td><td>Google's panel is set for density 260.</td><td><code>RESOLUTION</code> and <code>DENSITY</code> in <code>gbos.cfg</code>.</td></tr></tbody></table>
<p>The consequence of the colour fix: Night Light has no effect, because the gamma tables it uses are ignored on purpose.</p>
<h2>Drivers Google's modules cannot provide</h2>
<p>Google's vendor kernel modules are built for Google's kernel and do not load on this one. Battery and AC, the Intel LPSS I2C controller (touchpad) and legacy HDA audio are therefore built into the gbOS86 kernel; the rest are loadable modules found by <a href="/docs/hardware-detection">hardware detection</a>.</p>'''


@doc('configuration', 'Configuration', 'Every setting in gbos/gbos.cfg.')
def configuration():
    return '''<h1>Configuration</h1>
<p class="lead">Settings live in <code>gbos/gbos.cfg</code> on the volume. It is a shell-syntax file you can edit from any computer by plugging the stick in; changes take effect on the next boot.</p>
<table class="spec"><thead><tr><th>Setting</th><th>Values</th><th>What it does</th></tr></thead><tbody>
<tr><td><code>MODE</code></td><td><code>ask</code> (default), <code>live</code>, <code>install</code>, <code>boot</code></td><td><code>ask</code> shows the boot menu. <code>live</code> runs from the stick without saving. <code>install</code> goes straight to the installer/updater (it still asks before erasing). <code>boot</code> starts the installed system if there is one, else live.</td></tr>
<tr><td><code>HWDETECT</code></td><td><code>1</code> (default), <code>0</code></td><td>Automatic hardware detection and driver loading. <code>0</code> uses only built-in drivers.</td></tr>
<tr><td><code>DENSITY</code></td><td>dpi number, e.g. <code>180</code></td><td>Android display density. Unset = Google's stock 260 (too big on most laptops); lower = smaller UI.</td></tr>
<tr><td><code>RESOLUTION</code></td><td>e.g. <code>2400x1350</code></td><td>Android renders at this size and the display scales it to the panel. Larger = smaller UI without changing density. Empty = native. Keep the aspect ratio (16:9 for a 1080p panel). If you changed Settings, Display, Display size, set it back to Default.</td></tr>
<tr><td><code>LOAD_I915</code></td><td><code>1</code> (default), <code>0</code></td><td>Load the Intel graphics driver in stage0. Set <code>0</code> if the screen freezes right after "modprobe i915".</td></tr>
<tr><td><code>LOAD_WIFI</code></td><td><code>0</code> (default), <code>1</code></td><td>Legacy: load the Intel Wi-Fi driver in stage0. Hardware detection supersedes it.</td></tr>
<tr><td><code>SHELL_AT</code></td><td><code>none</code>, <code>early</code>, <code>pre_android</code></td><td>Drop to a shell before drivers (<code>early</code>) or just before Android starts (<code>pre_android</code>) for debugging.</td></tr>
<tr><td><code>MODOPTS</code></td><td><code>"module.param=value ..."</code></td><td>Driver options in the same spelling as a Linux kernel command line, for modules only (e.g. <code>xe.enable_panel_replay=0</code>). They are applied when hardware detection loads the module. A module that stage0 already loaded keeps its parameters; the boot log says so. <code>logs/boot-N/hwdetect.txt</code> lists the <code>xe</code> and <code>i915</code> parameters.</td></tr>
<tr><td><code>INSTALL_DISK</code></td><td>e.g. <code>nvme0n1</code>, <code>mmcblk0</code></td><td>Which internal disk the installer may erase when there is more than one (NVMe or eMMC). The serial number is still typed on the laptop first.</td></tr>
<tr><td><code>EXTRA_SLEEP</code></td><td>seconds</td><td>Pause before handing over to Android, so you can read the screen.</td></tr></tbody></table>
<p>An older setting, <code>BOOT_FROM=stick</code>, still works and means <code>live</code>.</p>
<h2>What Update does with your config</h2>
<p>When you update an installed system from a newer stick, <code>gbos.cfg</code> is synced to the installed system <em>except</em> <code>MODE</code> and the installer action, so the installed laptop keeps booting normally while still receiving new options such as the early-patch hook.</p>'''


@doc('logs', 'Logs and debugging', 'Where the logs are, what each file shows, and how to send a useful report.')
def logs():
    return '''<h1>Logs and debugging</h1>
<p class="lead">The laptop has no shell and no adb, so all evidence comes from log files written to the volume. Reading them is how problems are found.</p>
<h2>Where they are</h2>
<p>Every boot creates <code>gbos/logs/boot-N/</code> on the volume it booted from (N counts up). For an installed laptop that volume is its internal GBOSSYS partition, which you cannot read from another computer directly. To collect them, boot the USB stick and choose <b>2</b>, then <b>1 (Update)</b>: it copies the installed system's last three boots to <code>gbos/logs/boot-N/nvme-logs/</code> on the stick. Then plug the stick into a computer.</p>
<h2>Files</h2>
<table class="spec"><tbody>
<tr><td><code>hwdetect.txt</code></td><td>Your hardware and what got a driver. Start here for a hardware report.</td></tr>
<tr><td><code>kmsg.log</code></td><td>The full kernel log. The first lines include <code>Linux version ... #N</code>, which tells you which kernel actually ran.</td></tr>
<tr><td><code>stage0-dmesg.txt</code></td><td>Kernel messages plus stage0 and hook progress. Look for lines starting <code>hook:</code>, <code>hwdetect:</code> and <code>early:</code>, and for <code>NOT patched</code>.</td></tr>
<tr><td><code>logcat.txt*</code></td><td>Android's log, rotated; the highest number is the oldest.</td></tr>
<tr><td><code>crash.txt</code></td><td>Crashes and tombstone summaries.</td></tr>
<tr><td><code>hw.txt</code></td><td>The early hardware inventory: CPU flags, memory, PCI devices and their drivers, block devices.</td></tr>
<tr><td><code>audio.txt</code></td><td>Sound cards, PCMs and the audio service state, captured about a minute after boot.</td></tr>
<tr><td><code>pstore-prev/</code></td><td>The previous boot's kernel panic, if there was one.</td></tr></tbody></table>
<h2>Rules of thumb</h2>
<ul><li><strong>Absence of an error is not success.</strong> Check the log actually covers the time of the problem; logs have been truncated by filesystem corruption or by booting an old kernel.</li>
<li><strong>Confirm the kernel.</strong> The <code>Linux version ... #N</code> line must be the build you meant to test.</li>
<li><strong>Shut down from Android's power menu</strong>, not the power button: FAT has no journal, so a hard power-off can corrupt the logs.</li>
<li>The log directory keeps growing; delete old <code>boot-N</code> folders from another computer if space is short.</li></ul>
<h2>A good bug report</h2>
<ol><li>Laptop model and CPU.</li><li>What you expected and what happened.</li><li><code>hwdetect.txt</code> and <code>stage0-dmesg.txt</code> from the boot where it happened, pasted in code blocks or attached.</li><li>Whether it was live from USB or installed, and the kernel number.</li></ol>
<p>Post it in the <a href="/forum/c/support">support forum</a>.</p>'''


@doc('updating', 'Updating', 'Updating gbOS86 on a laptop, and moving to a newer Google build.')
def updating():
    return '''<h1>Updating</h1>
<h2>Updating an installed laptop to a newer gbOS86</h2>
<ol><li>Write the newer image to a stick (or replace the files on your existing one).</li><li>Boot the stick, choose <b>2</b>. When it finds an installation it offers <b>1) Update</b> and <b>2) Reinstall</b>.</li>
<li>Choose <b>1</b>. It saves the installed system's last three boots of logs to the stick, copies the new files and kernel to the laptop, verifies each one with sha256, and tells you to remove the stick. Apps, accounts and data are untouched.</li></ol>
<p>Update copies the system images, scripts, patched libraries, firmware, kernel modules, audio configs, installer tools and the kernel itself, but not your <code>MODE</code> setting.</p>
<h2>Moving to a newer Google build</h2>
<p>Android's built-in updater cannot work: it expects Google's own A/B partitions and kernel. A newer Googlebook OS build is a manual process:</p>
<ol><li><code>tools/check_new_build.sh</code> says whether a newer build exists; put it in <code>lapis/BUILD</code> and run <code>./build.sh</code> to fetch it and rebuild <code>super.img</code>.</li>
<li><code>tools/port_build.sh --isa</code> re-applies every binary patch (<code>libz</code>, <code>libcrypto</code>, the software KeyMint library, the VINTF manifest) to the new files, checks the paths and lines the boot scripts rely on (renamed vendor files that would stop being hidden are reported) and scans for CPU instructions the emulator does not cover (AVX-512, AMX and similar). <code>--write</code> stores the result once it reports no failures.</li>
<li>Copy to the stick and run <b>1 (Update)</b>. <code>PATCH-STATUS N applied, 0 not applied</code> in <code>stage0-dmesg.txt</code> confirms the patches bound. Data carries over; going back to an older build requires wiping data.</li></ol>
<p>The full procedure is <code>porting.md</code> in the repository.</p>'''


@doc('building', 'Building from source', 'The outline of building the kernel, payload and image.')
def building():
    return '''<h1>Building from source</h1>
<div class="notice warn">The image contains Google's OS, so it is built locally from Google's public recovery image and is not distributed. The source is on <a href="https://github.com/linuxkid473/gbos-x86" rel="noopener" target="_blank">GitHub</a>. You need a Mac or Linux machine with Docker, Python 3, QEMU for tests, and about 30 GB free.</div>
<p>For most people: <code>git clone https://github.com/linuxkid473/gbos-x86 &amp;&amp; cd gbos-x86 &amp;&amp; ./build.sh</code> does steps 1, 2, 8 and 9 below for you (it downloads the prebuilt kernel modules and firmware instead of building them). The full process:</p>
<ol>
<li><strong>Google's OS.</strong> Download the Googlebook recovery image from Google's servers and extract the logical partitions (system, vendor, product, system_ext, system_dlkm, vendor_dlkm; slot A) from its <code>super</code> partition.</li>
<li><strong>super.img.</strong> Rebuild a dynamic-partition image from them, under 4 GB so it fits on FAT32.</li>
<li><strong>Kernel source.</strong> AOSP common kernel, branch <code>android17-6.18</code>, with Google's prebuilt clang, kept in a Docker volume.</li>
<li><strong>Configuration.</strong> Start from the recovery kernel's config, apply the gbOS86 overrides (built-in storage, input and platform drivers; Intel graphics as a module; the generic Wi-Fi, Bluetooth, audio and platform driver set as modules), and set the command line.</li>
<li><strong>Patches.</strong> The CPU-instruction emulator, the ashmem shim, and the i915 colour changes (dummy colour pipeline, eDP full-range RGB, ignoring userspace gamma).</li>
<li><strong>Build.</strong> Compile the kernel and modules, install the modules into a staging tree, pack the stage0 initramfs (busybox, the init script and the early modules), and build again so it is embedded. The result is <code>BOOTX64.EFI</code>.</li>
<li><strong>Test.</strong> The emulator self-test in QEMU must print 16/16 passing; a hardware-detection test boots a virtual machine with network and audio devices and checks that the right modules load.</li>
<li><strong>Payload.</strong> Build the patched libraries, the software Gatekeeper, the installer tools; fetch and trim firmware from linux-firmware and sof-bin.</li>
<li><strong>Image.</strong> One script assembles a GPT image (EFI partition plus the FAT32 gbos volume) of about 6.7 GB, with a checksum.</li></ol>
<p>Questions about building go in the <a href="/forum/c/downloads">Downloads &amp; Builds</a> forum or via <a href="/contact">contact</a>.</p>'''


@doc('safety', 'Safety and limits', 'What can go wrong, and what this is not.')
def safety():
    return '''<h1>Safety and limits</h1>
<h2>Your data</h2>
<ul><li><strong>Installing erases the whole internal NVMe drive</strong> (Windows, files, every partition). The only guard is typing the drive's serial number, which appears on the laptop's own screen. There is no way to skip it.</li>
<li>Live mode never writes to the internal drive.</li>
<li>Update keeps your data but does change the system files; back up anything important first.</li>
<li>FAT filesystems have no journal. Always shut down from Android's power menu.</li></ul>
<h2>Security</h2>
<ul><li>SELinux runs in <strong>permissive</strong> mode and the security services (Gatekeeper, KeyMint) are software stand-ins. <strong>Do not treat this as a secure device</strong>: keys are not hardware-backed and the lock screen is only as strong as its software.</li>
<li>Secure Boot must be off, so the boot chain is not verified.</li>
<li>The CPU emulator runs inside the kernel; a bug there is a kernel bug.</li></ul>
<h2>Compatibility limits</h2>
<ul><li>Intel x86-64 only. AMD CPUs (Ryzen) and ARM are not supported, and AMD graphics is experimental (software rendering only, slow, untested on hardware). The CPU must have AVX (many Celeron, Pentium and Atom parts do not): Google's init uses it from its first instruction.</li><li>Installing needs an internal NVMe drive (live mode works without).</li><li>Suspend and resume, fingerprint readers, IPU6 cameras and discrete GPUs are not supported. On a laptop with AMD Radeon graphics the desktop is drawn by the CPU (experimental).</li>
<li>Twenty-one machines have been tried, eight of them fully working; see <a href="/docs/tested-laptops">Tested laptops</a>. Anything not on that list is untested.</li></ul>
<h2>Legal</h2>
<p>gbOS86 is an independent community project, not affiliated with or endorsed by Google. Googlebook OS, Android and Chrome are Google's. The built image contains Google's OS and is not redistributed. The source repository includes a small number of patched Google libraries (a patched libz, libcrypto and a software KeyMint library) that the build requires; Google's own terms apply to them.</p>'''


@doc('changelog', 'Changelog', 'Milestones of the project.')
def changelog():
    return '''<h1>Changelog</h1>
<p class="lead">Headline milestones. Kernel numbers are the build counter printed in the boot log.</p>
<table class="spec"><thead><tr><th>Build</th><th>Change</th></tr></thead><tbody>
<tr><td>#32</td><td><strong>Generic Intel gen 6+ support.</strong> About 400 driver modules, boot-time hardware detection with a per-boot report, firmware for Intel, Realtek, MediaTek, Qualcomm and Broadcom Wi-Fi and Bluetooth, SOF audio, generated audio configs. HDA versus SOF is now chosen by the kernel. Early binding of the patched libraries.</td></tr>
<tr><td>#30</td><td>gbOS86 boot menu (live / install or update / boot installed) and branding; flashable live and installer USB image.</td></tr>
<tr><td>#29</td><td>i915 ignores userspace gamma, degamma and colour matrices: the white veil and lifted blacks are fixed.</td></tr>
<tr><td>#27</td><td>eDP always full-range RGB.</td></tr>
<tr><td>#25</td><td>No Colorspace property on eDP, to stop the panel being driven in a limited-range mode.</td></tr>
<tr><td>earlier</td><td>NVMe installer and updater with serial-number confirmation; software Gatekeeper and KeyMint; ashmem shim; patched libz and libcrypto; Wi-Fi, Bluetooth, audio and touchpad on the Dell Latitude 5300; kernel CPU-instruction emulator with known-answer tests.</td></tr></tbody></table>'''
