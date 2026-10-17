# gbos pre-Android hook: sourced by stage0 just before switch_root (Android root at /new,
# helpers log/fail/shell and $G=/gbos/gbos available). Edit freely, no kernel rebuild needed.

# zram is a module here (=m in Google's config too); mmd_setup aborts without /sys/block/zram0.
modprobe zram && log "hook: zram loaded" || log "hook: modprobe zram FAILED"

# Generic hardware bring-up (gbOS86 for any Intel gen6+ laptop): usb/hwdetect.sh binds the full module tree from
# gbos/modules, coldplugs every device by modalias (Wi-Fi, Bluetooth, audio, input, hotkeys, backlight, ...),
# with firmware from gbos/firmware, and writes hwdetect.txt. Falls back to the original Latitude 5300 sequence
# (below) when a stick has no gbos/modules.
FWP=/sys/module/firmware_class/parameters/path
if [ "$HWDETECT" != 0 ] && [ -f $G/hwdetect.sh ] && [ -d $G/modules/lib/modules ]; then
  . $G/hwdetect.sh
  gbos_hwdetect 15
  # Late firmware requests (after switch_root) see the stick as /postinstall.
  echo -n /postinstall/gbos/firmware > $FWP
else
  # Wi-Fi: CNVi 9560/9462 (8086:9df0) needs iwlwifi-9000-pu-b0-jf-b0-46.ucode (linux-firmware), which
  # Google's vendor image doesn't carry. Point the firmware loader at the stick, load the driver, and
  # wait for the firmware so it isn't requested after switch_root (where /gbos no longer exists).
  # Afterwards point it at the same folder as seen from Android (/postinstall = the stick).
  FWP=/sys/module/firmware_class/parameters/path
  if [ -f $G/firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode ]; then
    echo -n $G/firmware > $FWP
    modprobe iwlmvm && log "hook: iwlmvm loaded" || log "hook: modprobe iwlmvm FAILED"
    i=0; while [ $i -lt 15 ] && ! dmesg | grep -q 'iwlwifi.*loaded firmware version'; do sleep 1; i=$((i+1)); done
    log "hook: wifi after ${i}s: $(dmesg | grep -m1 -o 'iwlwifi.*loaded firmware version.*') ifaces: $(ls /sys/class/net | tr '\n' ' ')"
    echo -n /postinstall/gbos/firmware > $FWP
  fi

  # Bluetooth: the 9560's Bluetooth is a USB device (8087:0aaa) and needs btusb, which Google's kernel
  # (PCIe Bluetooth) doesn't build. gbos/kmod/ has btusb/btintel/btrtl from our tree; bluetooth and btbcm
  # are in the initramfs. Firmware: intel/ibt-17-16-1.sfi/.ddc in gbos/firmware.
  if [ -f $G/kmod/btusb.ko ]; then
    [ -f $G/firmware/intel/ibt-17-16-1.sfi ] || log "hook: Bluetooth firmware intel/ibt-17-16-1.sfi missing"
    # btintel loads the firmware during probe, i.e. right now: the loader must look on this volume
    # ($G), not at /postinstall, which only exists after switch_root.
    echo -n $G/firmware > $FWP
    modprobe bluetooth && modprobe btbcm && insmod $G/kmod/btintel.ko && insmod $G/kmod/btrtl.ko \
      && insmod $G/kmod/btusb.ko && log "hook: btusb loaded ($(ls /sys/class/bluetooth 2>/dev/null | tr '\n' ' '))" \
      || log "hook: btusb load FAILED: $(dmesg | grep -iE 'btusb|btintel|bluetooth' | tail -2 | tr '\n' ' ')"
    i=0; while [ $i -lt 20 ] && ! dmesg | grep -qE 'hci0: (Firmware loaded|Failed to load Intel firmware)'; do sleep 1; i=$((i+1)); done
    log "hook: bluetooth: $(dmesg | grep -oE 'hci0: (Firmware loaded in [0-9]+ usecs|Failed to load Intel firmware[^)]*\)|Found device firmware: [^ ]+)' | tr '\n' ' ')"
    echo -n /postinstall/gbos/firmware > $FWP
  fi
fi

# drm.debug=0x04 (KMS only; not ATOMIC, which logs every frame): i915 dumps gamma/CSC mode and range per modeset while
# drm_hwcomposer first sets up the panel. gbos.rc turns it off at boot_completed. Washed-out colours.
echo 0x04 > /sys/module/drm/parameters/debug 2>/dev/null && log "hook: drm.debug=0x04 (KMS) until boot_completed"

# Fake ChromeOS boot disk for the boot HAL: misc + vbmeta_a/vbmeta_b with cgpt slot attributes.
if [ -f $G/bootdisk.img ]; then
  losetup -P /dev/loop6 $G/bootdisk.img; sleep 1; mdev -s
  mknod -m 600 /dev/block/loop6 b 7 6 2>/dev/null
  i=1
  for n in misc vbmeta_a vbmeta_b; do
    [ -e /dev/block/loop6p$i ] || mknod -m 600 /dev/block/loop6p$i b $(tr : ' ' < /sys/class/block/loop6p$i/dev)
    ln -sf /dev/block/loop6p$i /dev/block/by-name/$n
    i=$((i+1))
  done
  log "hook: bootdisk on loop6: $(ls /sys/class/block | grep loop6p | tr '\n' ' ')"
fi

# Fake crossystem (no ChromeOS firmware on this laptop)
[ -f $G/crossystem ] && mount --bind $G/crossystem /new/vendor/bin/crossystem && log "hook: crossystem faked"

# Logcat to the stick: expose the USB volume inside Android at /postinstall, and overlay
# /system/etc/init with a copy plus gbos.rc (a logcat service writing to this boot's log dir).
if mount --bind /gbos /new/postinstall; then
  mkdir -p /tmp/sysinit && cp -a /new/system/etc/init/. /tmp/sysinit/
  rm -f /tmp/sysinit/gscd.rc   # gscd needs Google's security chip (/dev/tpm0); it only crash-loops here
  # RESOLUTION in gbos.cfg: logical display size (e.g. 2400x1350) that SurfaceFlinger renders at and the
  # display hardware scales to the 1920x1080 panel, so everything looks smaller with stock density.
  case "$RESOLUTION" in [0-9]*x[0-9]*) WMSIZE=$RESOLUTION;; *) WMSIZE=reset;; esac
  cat > /tmp/sysinit/gbos.rc <<RC
# Rotate at 1 MB: on this stick, data written past ~4 MB of a file that stays open never reaches
# the FAT cluster chain before power-off (fsck: "size ... should at most be 4046848"). Rotating
# closes each file; logcat.txt is newest, then logcat.txt.1, .2, ...
service gbos_logcat /system/bin/logcat -b all -v threadtime -r 1024 -n 99 -f /postinstall/gbos/logs/boot-$N/logcat.txt
    user root
    group root log
    seclabel u:r:shell:s0

# Crash buffer only (FATAL EXCEPTION / native crashes): small, so it survives a hard power-off.
service gbos_crashlog /system/bin/logcat -b crash -v threadtime -f /postinstall/gbos/logs/boot-$N/crash.txt
    user root
    group root log
    seclabel u:r:shell:s0

# The laptop is usually powered off by hand, so flush the logs to the stick every 5 s.
service gbos_sync /system/bin/sh -c "while true; do sleep 5; sync; done"
    user root
    group root
    seclabel u:r:shell:s0

# Sound diagnostics, 60 s after boot: ALSA cards/PCMs/codec (incl. control names) and audioserver state.
service gbos_audiodiag /system/bin/sh -c "sleep 60; cd /proc/asound; ( head -n 3000 cards pcm devices card0/id card0/codec*; ls -l /dev/snd; dumpsys media.audio_flinger; dumpsys media.audio_policy ) > /postinstall/gbos/logs/boot-$N/audio.txt 2>&1"
    user root
    group root audio
    seclabel u:r:shell:s0
    oneshot
on property:sys.boot_completed=1
    # KMS debug was on during the HWC's first modesets (set early in hook.sh); turn it off again.
    write /sys/module/drm/parameters/debug 0
    start gbos_audiodiag
    start gbos_wmsize
    start gbos_displaydiag
    start gbos_aidiag

# AI diagnostics (Magic Pointer / AICore): usb/aidiag.sh writes logs/boot-N/ai-1.txt (90 s) and ai-2.txt (4 min).
# The gbos_aidns_* services run one ping per app uid so aidiag can compare DNS as AICore (10107) with
# as.oss (10118), the Private Compute Services app that does reach Google.
service gbos_aidiag /system/bin/sh /postinstall/gbos/aidiag.sh $N
    user root
    group root
    seclabel u:r:shell:s0
    oneshot

service gbos_aidns_10107 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_10107.txt 2>&1"
    user 10107
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

service gbos_aidns_10118 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_10118.txt 2>&1"
    user 10118
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

service gbos_aidns_10119 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_10119.txt 2>&1"
    user 10119
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

service gbos_aidns_2000 /system/bin/sh -c "(ping -c 1 -W 5 www.google.com; ping -c 2 -W 5 ondevicesafety-pa.googleapis.com) > /dev/aidns_2000.txt 2>&1"
    user 2000
    group inet
    seclabel u:r:shell:s0
    oneshot
    disabled

# Logical display size (RESOLUTION in gbos.cfg); "reset" = native 1920x1080.
service gbos_wmsize /system/bin/wm size $WMSIZE
    user root
    group root
    seclabel u:r:shell:s0
    oneshot
    disabled

# Display diagnostics, 70 s after boot (washed-out colours): DRM atomic state (CTM, gamma, Broadcast RGB,
# colour pipeline), i915 pipe/dither/bpc info, panel EDID, and SurfaceFlinger/colour-mode settings.
service gbos_displaydiag /system/bin/sh -c "sleep 70; ( mount -t debugfs debugfs /sys/kernel/debug; head -n 20000 /sys/kernel/debug/dri/*/state /sys/kernel/debug/dri/*/i915_display_info /sys/kernel/debug/dri/*/i915_display_capabilities /sys/kernel/debug/dri/*/eDP-1/*; echo EDID; od -A x -t x1 /sys/class/drm/card*-eDP-1/edid; dumpsys SurfaceFlinger; dumpsys color_display; dumpsys display; settings list secure; settings list system ) > /postinstall/gbos/logs/boot-$N/display.txt 2>&1"
    user root
    group root graphics
    seclabel u:r:shell:s0
    oneshot

# Software Gatekeeper (gatekeeper/soft_gk.c) in place of the Trusty one, which never answers.
service gbos_soft_gk /postinstall/gbos/soft_gk
    class hal
    user system
    group system
    seclabel u:r:hal_gatekeeper_default:s0
    interface aidl android.hardware.gatekeeper.IGatekeeper/default

# Trusty security VM (KeyMint/Gatekeeper TEE) as a plain KVM VM: no pKVM on this laptop.
# vendor/build.prop's protected_vm.supported=1 is hidden (see below), so the launcher falls back
# to a non-protected VM; servicemanager starts it on demand for the KeyMint accessors.
on early-init
    setprop ro.boot.hypervisor.vm.supported 1
    # libcutils only uses memfd for apps targeting SDK >= 37; older ones (SetupWizard) go to
    # /dev/ashmem, which needs the Rust ashmem driver. Force memfd (kernel/ashmem_shim answers
    # the ashmem ioctls on memfd).
    setprop sys.use_memfd true
on init && property:trusty.security_vm.enabled=1
    setprop desktop.security_vm.vm_image /system_ext/etc/vm/trusty_vm/desktop_trusty_ext_boot_signed.elf
    setprop trusty.security_vm.vm_cid 200
    setprop trusty.security_vm.nonsecure_vm_ready 1
    setprop ro.hardware.trusty_ipc_dev.gatekeeper VSOCK:200:0
    setprop storageproxyd_system.trusty_ipc_dev VSOCK:200:0

# atrace.rc resets ftrace at late-init; turn the KVM trace back on before the Trusty VM starts.
on early-fs
    write /sys/kernel/tracing/buffer_size_kb 8192
    write /sys/kernel/tracing/events/kvm/kvm_inj_exception/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_emulate_insn/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_userspace_exit/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_exit/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_pio/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_mmio/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_hypercall/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_fast_mmio/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_inj_virq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_pic_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_ioapic_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_msi_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_set_irq/enable 1
    write /sys/kernel/tracing/events/kvm/kvm_apic_accept_irq/enable 1
    # Boot the Trusty VM before any KeyMint/StrongBox client connects (vsock startup race).
    start trusty_security_protected_vm_launcher
    write /sys/kernel/tracing/tracing_on 1

on post-fs
    start gbos_logcat
    start gbos_sync
    start gbos_crashlog
on post-fs-data
    start gbos_logcat
    start gbos_sync
    start gbos_crashlog
RC
  # Conscrypt (and identical) APEX libcrypto: the APEX is signed, so patch a copy and bind it over the
  # original once apexd has activated every APEX (init.rc's "wait_for_prop apexd.status activated" is in
  # on post-fs-data, before zygote-start). See apex_patch.sh.
  if [ -f $G/apex_patch.sh ] && [ "$(grep -c '^    wait_for_prop apexd.status activated$' /tmp/sysinit/hw/init.rc)" = 1 ]; then
    sed -i 's#^    wait_for_prop apexd.status activated$#&\n    exec u:r:shell:s0 root root -- /system/bin/sh /postinstall/gbos/apex_patch.sh#' /tmp/sysinit/hw/init.rc \
      && log "hook: init.rc runs apex_patch.sh after APEX activation"
  else
    log "hook: apex_patch NOT installed (script missing or init.rc anchor not unique)"
  fi
  mount -t tmpfs -o mode=0755 tmpfs /new/system/etc/init && cp -a /tmp/sysinit/. /new/system/etc/init/ \
    && log "hook: logcat -> gbos/logs/boot-$N/logcat.txt ($(ls /new/system/etc/init | wc -l) rc files)" \
    || log "hook: init overlay FAILED"
else
  log "hook: bind /gbos -> /postinstall FAILED"
fi

# vendor/build.prop overrides. Hide protected_vm.supported=1 (no pKVM here). DENSITY in gbos.cfg
# optionally replaces ro.sf.lcd_density (stock 260); unset keeps stock. To make things smaller without
# touching density, use RESOLUTION instead (gbos_wmsize in gbos.rc above).
# The Latitude's LG LP133WF6 is an sRGB panel, not wide-gamut/HDR like Google's: with these true,
# SurfaceFlinger offers DISPLAY_BT2020 and drm_hwcomposer tags eDP as BT.2020 (pale colours; the
# kernel side is fixed in i915_edp_colorspace.sh). Declare it sRGB-only.
case "$DENSITY" in ''|*[!0-9]*) DENSITY=;; esac
DROP='^ro.boot.hypervisor.protected_vm.supported=|^ro.surface_flinger.has_wide_color_display=|^ro.surface_flinger.has_HDR_display='
[ -n "$DENSITY" ] && DROP="$DROP|^ro.sf.lcd_density="
{ grep -v -E "$DROP" /new/vendor/build.prop
  echo "ro.surface_flinger.has_wide_color_display=false"
  echo "ro.surface_flinger.has_HDR_display=false"
  if [ -n "$DENSITY" ]; then echo "ro.sf.lcd_density=$DENSITY"; fi
} > /tmp/vendor.build.prop && mount --bind /tmp/vendor.build.prop /new/vendor/build.prop \
  && log "hook: vendor build.prop: protected_vm.supported hidden, sRGB-only display, lcd_density=${DENSITY:-stock}"
# Only one Trusty VM may own CID 200: drop the plain launcher's auto-start so the on-demand
# (accessor-registering) launcher runs it, falling back to non-protected by itself.
R=/new/system_ext/etc/init/desktop-trusty_security_vm_launcher.rc
sed 's/^    start trusty_security_vm_launcher$/    # gbos: plain launcher disabled/' $R > /tmp/vm_launcher.rc \
  && mount --bind /tmp/vm_launcher.rc $R \
  && log "hook: plain trusty launcher disabled ($(grep -c 'gbos: plain launcher disabled' $R) triggers)"
# Software KeyMint: Trusty's TEE KeyMint needs Google's security chip for its root of trust and
# hangs without it. Hide the Trusty KeyMint/RKP/SecureClock declarations so keystore2 uses
# km_compat, and use a libkm_compat patched to serve its software KeyMint for every security level.
KM=/new/system/lib64/libkm_compat.so
if [ -f $G/libkm_compat.so ] && [ "$(sha256sum $KM | cut -d' ' -f1)" = "be92d0a47d07a4a928ad1395f470195870dfdd16825866b7eaf3dd670d9b2461" ]; then
  mount --bind $G/libkm_compat.so $KM && log "hook: software KeyMint (patched libkm_compat) active"
else
  log "hook: libkm_compat NOT patched (missing patch or unexpected original)"
fi
[ -f $G/system_ext_manifest.xml ] && mount --bind $G/system_ext_manifest.xml /new/system_ext/etc/vintf/manifest.xml \
  && log "hook: Trusty KeyMint VINTF declarations hidden"
# StrongBox KeyMint (vendor APEX) needs the security chip too, and keystore2 aborts boot when a
# declared KeyMint won't accept module info. Overlay /vendor/apex without that one APEX.
mkdir -p /tmp/vapex && mount --bind /new/vendor/apex /tmp/vapex \
  && mount -t tmpfs -o mode=0755 tmpfs /new/vendor/apex && for f in /tmp/vapex/*; do
    n=${f##*/}; case $n in com.android.hardware.keymint.strongbox.desktop.apex) continue;; esac
    touch /new/vendor/apex/$n && mount --bind $f /new/vendor/apex/$n
  done && log "hook: vendor apex without strongbox: $(ls /new/vendor/apex | tr '\n' ' ')"
# Weaver is Trusty/GSC-backed and only starts with protected_vm.supported=1 (hidden above), but
# its VINTF declaration stays, so LockSettingsService waits on it forever and the Watchdog kills
# system_server ("System process not responding"). Undeclared, LSS goes without Weaver.
W=/new/vendor/etc/vintf/manifest/android.hardware.weaver-service.android-desktop.xml
[ -f $W ] && echo '<manifest version="9.0" type="device"/>' > /tmp/weaver.xml \
  && mount --bind /tmp/weaver.xml $W && log "hook: Weaver VINTF declaration hidden"
# Secretkeeper is Trusty-backed too (VSOCK:200) and never answers. virtualizationservice waits for it
# in allocateInstanceId, so the Linux Terminal's VM never starts. Undeclared, AVF goes without it.
SK=/new/vendor/etc/vintf/manifest/android.hardware.security.secretkeeper.trusty.xml
[ -f $SK ] && echo '<manifest version="9.0" type="device"/>' > /tmp/secretkeeper.xml \
  && mount --bind /tmp/secretkeeper.xml $SK && : > /tmp/empty.rc \
  && mount --bind /tmp/empty.rc /new/vendor/etc/init/android.hardware.security.secretkeeper.trusty.desktop.rc \
  && log "hook: Secretkeeper VINTF declaration hidden"
# Gatekeeper is Trusty-backed (VSOCK:200) and enroll never returns, so setting a lock-screen
# password hangs LockSettingsService (and every Settings screen waiting on it). Keep the VINTF
# declaration but don't start the Trusty service: gbos_soft_gk (gbos.rc above) registers
# IGatekeeper/default instead. Without soft_gk on the stick, hide the declaration so enroll fails fast.
GK=/new/vendor/etc/vintf/manifest/android.hardware.gatekeeper-service.trusty.xml
: > /tmp/empty.rc && mount --bind /tmp/empty.rc /new/vendor/etc/init/android.hardware.gatekeeper-service.trusty.rc
if [ -x $G/soft_gk ]; then
  log "hook: Trusty Gatekeeper replaced by software Gatekeeper (soft_gk)"
else
  echo '<manifest version="9.0" type="device"/>' > /tmp/gatekeeper.xml && mount --bind /tmp/gatekeeper.xml $GK \
    && log "hook: Trusty Gatekeeper hidden, no soft_gk (no lock-screen password)"
fi
# Keep vold off USB disks: its voldmanaged=usb rule picks up this stick, and its fsck_msdos
# "repairs" the FAT while stage0 has it mounted (LOST.DIR, lost/truncated logs, cross-linked
# clusters). The stick must only ever be mounted by stage0.
F=/new/vendor/etc/fstab.android-desktop
grep -v 'voldmanaged=usb' $F > /tmp/fstab.android-desktop \
  && mount --bind /tmp/fstab.android-desktop $F && log "hook: vold USB rule removed from fstab"
# Audio: the audio HAL (dras) needs a config whose <soundcard> equals the ALSA card id. Google ships only its SOF
# SoundWire card; gbos/audio/gbos-hda-pch is the legacy-HDA one (card id PCH, true for nearly every Intel PCH HDA
# laptop). For any other main card (SOF: sof-hda-dsp, sof-rt5682, ...) a copy is generated with that card's id and
# name, which is a best-effort mapping (mixer controls differ per codec).
A=/new/vendor/etc/audio
AC=gbos-hda-pch
if [ -f $G/audio/gbos-hda-pch/audio_config.xml ]; then
  CARD=$(grep '\[' /proc/asound/cards 2>/dev/null | grep -v -i hdmi | head -n 1)
  CID=$(echo "$CARD" | sed -n 's/^ *[0-9]* \[\([^] ]*\) *\].*/\1/p'); CNAME=$(echo "$CARD" | sed -n 's/.* - //p')
  mkdir -p /tmp/gbos-audio
  if [ -n "$CID" ] && [ "$CID" != PCH ]; then
    AC=gbos-gen; rm -rf /tmp/gbos-audio/$AC; cp -r $G/audio/gbos-hda-pch /tmp/gbos-audio/$AC
    sed -i "s/HDA Intel PCH/$CNAME/g; s/PCH/$CID/g" /tmp/gbos-audio/$AC/*.xml
    if [ "$CID" = sofhdadsp ]; then
      # SOF + HDA codec (skl_hda_dsp_generic, sof-hda-generic-*.tplg): analog is still PCM 0, but HDMI/DP are PCM 3/4/5
      # (legacy HDA: 3/7/8) and the digital mics are their own capture PCM ("DMIC", not "DMIC16kHz") instead of the codec's.
      sed -i "s/,7</,4</; s/,8</,5</; s/pcm=7/pcm=4/; s/pcm=8/pcm=5/" /tmp/gbos-audio/$AC/audio_config.xml
      DM=$(sed -n 's/^[0-9]*-0*\([0-9][0-9]*\): DMIC (.*capture.*/\1/p' /proc/asound/pcm | head -n 1)
      [ -n "$DM" ] && sed -i "/<InternalMic>/,/<\/InternalMic>/ s/hw:$CID,0/hw:$CID,$DM/" /tmp/gbos-audio/$AC/audio_config.xml
    fi
    sed "s/<soundcard>PCH/<soundcard>$CID/; s#gbos-hda-pch#$AC#" $G/audio/audio_default.xml > /tmp/gbos-audio/audio_default.xml
    ASRC=/tmp/gbos-audio/$AC; ADEF=/tmp/gbos-audio/audio_default.xml
  else
    ASRC=$G/audio/gbos-hda-pch; ADEF=$G/audio/audio_default.xml
  fi
  mkdir -p /tmp/vaudio && mount --bind $A /tmp/vaudio && mount -t tmpfs -o mode=0755 tmpfs $A \
    && for f in /tmp/vaudio/*; do n=${f##*/}; { [ -d $f ] && mkdir -p $A/$n || touch $A/$n; } && mount --bind $f $A/$n; done \
    && mkdir -p $A/$AC && mount --bind $ASRC $A/$AC \
    && mount --bind $ADEF /new/vendor/etc/data/hal_configs/audio_default.xml \
    && log "hook: audio config $AC (card id ${CID:-none}, name ${CNAME:-none}) active"
fi
# apgetty (UART shell) has no UART configured here and crash-loops
: > /tmp/empty.rc && mount --bind /tmp/empty.rc /new/system_ext/etc/init/apgetty.rc && log "hook: apgetty disabled"
log "hook: kvm: $(ls /sys/class/misc | grep -E 'kvm|vhost' | tr '\n' ' ')"

# KVM trace -> kvmtrace.txt: guest exceptions / failed emulation / triple faults, to see why the
# Trusty VM goes silent. Reader opens trace_pipe now so it survives switch_root.
T=/tmp/tracing; mkdir -p $T
if mount -t tracefs tracefs $T; then
  E=$T/events/kvm
  echo 8192 > $T/buffer_size_kb
  for ev in kvm_inj_exception kvm_emulate_insn kvm_userspace_exit kvm_exit kvm_pio kvm_mmio kvm_hypercall kvm_fast_mmio kvm_inj_virq kvm_pic_set_irq kvm_ioapic_set_irq kvm_msi_set_irq kvm_set_irq kvm_apic_accept_irq; do
    [ -d $E/$ev ] && echo 1 > $E/$ev/enable
  done
  ( exec 4<$T/trace_pipe; /gbos/busybox dd bs=4096 count=8000 iflag=fullblock <&4 >> $L/kvmtrace.txt 2>/dev/null ) &
  log "hook: kvm tracing (all exits) -> kvmtrace.txt"
else
  log "hook: tracefs mount FAILED"
fi

# libz: adler32_z's AVX-VNNI helper (vpdpwssd) traps on this CPU (~half of all emulated instructions).
# The patched copies (tools/patch_libz_adler.py) make adler32_z always take its scalar path. Each is only
# bind-mounted over the exact original it was made from.
for p in system:/new/system/lib64/libz.so:371f70d6a9392327cb36838d0cbc7af7388d1689c25d91cd343af8e58433683b vendor:/new/vendor/lib64/libz.so:e45aecb65cbb40f0c3070b05aa9d974076d401ed6f2aff3081af26a14e812590; do
  n=${p%%:*}; r=${p#*:}; t=${r%%:*}; h=${r#*:}
  if grep -q " $t " /proc/mounts; then
    log "hook: $n libz already patched (early_patch.sh)"
  elif [ -f $G/patched/$n.libz.so ] && [ "$(sha256sum $t | cut -d' ' -f1)" = "$h" ]; then
    mount --bind $G/patched/$n.libz.so $t && log "hook: patched $n libz (scalar adler32) active"
  else
    log "hook: $n libz NOT patched (missing patch or unexpected original)"
  fi
done

# libcrypto: SHA-NI block functions trap here; the patched copies (tools/sha_redirect.py) send them to the
# scalar siblings. Hash-gated like libz.
for p in system:/new/system/lib64/libcrypto.so:0ee96c7e0cb1ffca5520d2eaca02b4fa2ccff5a8fcb753a491e85555a91c9f2a vendor:/new/vendor/lib64/libcrypto.so:70cc371baf9925d5f1f857d03177d5193caf60bb13bb2012b4336657adcf48e7; do
  n=${p%%:*}; r=${p#*:}; t=${r%%:*}; h=${r#*:}
  if grep -q " $t " /proc/mounts; then
    log "hook: $n libcrypto already patched (early_patch.sh)"
  elif [ -f $G/patched/$n.libcrypto.so ] && [ "$(sha256sum $t | cut -d' ' -f1)" = "$h" ]; then
    mount --bind $G/patched/$n.libcrypto.so $t && log "hook: patched $n libcrypto (scalar SHA) active"
  else
    log "hook: $n libcrypto NOT patched (missing patch or unexpected original)"
  fi
done
