#!/system/bin/sh
# GBOS System, Performance, RAM and CPU Optimization
# Configured for PC hardware running gbOS86

sleep 4

# ==============================================================================
# 1. HARDWARE & CPU PERFORMANCE
# ==============================================================================
# Set CPU governor to maximum performance to prevent throttling on software rendering
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null
done
for g in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
    echo performance > "$g" 2>/dev/null
done

# Keep interactive USB HID devices (mice, keyboards, touchscreens) awake while screen is on so there is no input lag,
# while allowing USB root hubs, webcams, Bluetooth, and sensor hubs to autosuspend (required for Intel Package C6/C7).
for u in /sys/bus/usb/devices/*; do
    [ -w "$u/power/control" ] || continue
    case "${u##*/}" in *:*) continue ;; esac
    _is_hid=0
    _is_sensor=0
    for _if in "$u":*; do
        [ -e "$_if/driver" ] || continue
        case "$(readlink "$_if/driver" 2>/dev/null)" in */usbhid) _is_hid=1 ;; esac
        for _hd in "$_if"/0003:*; do
            [ -e "$_hd/driver" ] || continue
            case "$(readlink "$_hd/driver" 2>/dev/null)" in */hid-sensor-hub) _is_sensor=1 ;; esac
        done
    done
    if [ "$_is_hid" = 1 ] && [ "$_is_sensor" = 0 ]; then
        echo on > "$u/power/control" 2>/dev/null
    else
        echo auto > "$u/power/control" 2>/dev/null
    fi
done

# Enable sleep/lid behavior and wakeup sources (ACPI power button, lid, PS/2 keyboard & touchpad, USB HID)
settings put global lid_behavior 1 2>/dev/null
for w in /sys/bus/acpi/devices/PNP0C0C:*/power/wakeup \
         /sys/bus/acpi/devices/PNP0C0D:*/power/wakeup \
         /sys/bus/acpi/devices/LNXPWRBN:*/power/wakeup \
         /sys/bus/acpi/devices/LNXSLPBN:*/power/wakeup \
         /sys/devices/platform/i8042/serio*/power/wakeup; do
    [ -w "$w" ] && echo enabled > "$w" 2>/dev/null
done
for u in /sys/bus/usb/devices/*; do
    [ -w "$u/power/wakeup" ] || continue
    if ls -d "$u":* 2>/dev/null | xargs -I{} readlink "{}/driver" 2>/dev/null | grep -q 'usbhid$'; then
        echo enabled > "$u/power/wakeup" 2>/dev/null
    fi
done
[ -w /sys/devices/platform/sony-laptop/lid_resume_S3 ] && echo 1 > /sys/devices/platform/sony-laptop/lid_resume_S3 2>/dev/null

# Ensure all cpusets are valid for the actual online CPU count (e.g. 0-3 on a 4-thread CPU)
# Google's powerhint assumes 8 cores (4-7) which fails with EINVAL and starves background tasks
NUM_CPUS=$(nproc 2>/dev/null || echo 4)
VALID_CPUS="0-$((NUM_CPUS - 1))"
for cs in /dev/cpuset/*/cpus /dev/cpuset/cpus; do
    echo "$VALID_CPUS" > "$cs" 2>/dev/null
done
setprop vendor.sys_tuner.disable_cpuset_config true

# Stop OEM proprietary touchpad inhibitor if it is spinning in a 1-second busy loop
stop touchpad-inhibitor 2>/dev/null

# Suppress drmhwc 60 FPS gamma log spam that causes SurfaceFlinger frame pacing jitter
setprop log.tag.drmhwc WARN
setprop persist.log.tag.drmhwc WARN

# ==============================================================================
# 2. DISABLE WINDOW BLURS & STREAMLINE ANIMATIONS
# ==============================================================================
# Only while the CPU composites (hook.sh sets ro.gbos.software_rendering=1 then). With a hardware GPU the animations and blur stay at
# Google's defaults: forcing them off on every boot (builds from 2026-11-04) made the UI look broken and overrode the user's settings.
if [ "$(getprop ro.gbos.software_rendering)" = 1 ]; then
    settings put global disable_window_blurs 1 2>/dev/null
    cmd window disable-blur 1 2>/dev/null
    setprop persist.sys.sf.disable_blurs 1 2>/dev/null
    settings put global window_animation_scale 0 2>/dev/null
    settings put global transition_animation_scale 0 2>/dev/null
    settings put global animator_duration_scale 0 2>/dev/null
elif [ "$(getprop persist.gbos.anim_restored)" != 1 ]; then
    # One-time repair for installs that already got the values above written into /data by the 2026-11-04 builds.
    settings put global disable_window_blurs 0 2>/dev/null
    cmd window disable-blur 0 2>/dev/null
    setprop persist.sys.sf.disable_blurs 0 2>/dev/null
    settings put global window_animation_scale 1 2>/dev/null
    settings put global transition_animation_scale 1 2>/dev/null
    settings put global animator_duration_scale 1 2>/dev/null
    setprop persist.gbos.anim_restored 1 2>/dev/null
fi

# ==============================================================================
# 3. DISABLE SWAP (Protects storage from wear, latency spikes, and stalls)
# ==============================================================================
swapoff -a 2>/dev/null
if [ -e /sys/block/zram0/reset ]; then
    echo 1 > /sys/block/zram0/reset 2>/dev/null
fi
rm -rf /data/per_boot/zram_swap /data/vendor/swap 2>/dev/null

sysctl -w vm.swappiness=0 2>/dev/null
sysctl -w vm.dirty_background_ratio=5 2>/dev/null
sysctl -w vm.dirty_ratio=10 2>/dev/null
sysctl -w vm.vfs_cache_pressure=100 2>/dev/null

# ==============================================================================
# 4. OPTIMIZE HEADLESS USER 0 (Frees 2.5 - 3.0 GB of RAM)
# User 0 is headless (system only); User 10 is the interactive human user.
# Disable duplicate client apps & Google services in User 0 ONLY.
# User 10 remains 100% untouched so the user can run all apps normally.
# NOT com.android.vending: Play Store in user 10 binds its MultiUserCoordinatorService in user 0 before every download, and with
# the user-0 copy disabled every update and "system services" update stayed "Pending" (HANDOFF.md 2026-11-17).
# ==============================================================================
# Installs that ran this script from 2026-11-04 on keep the user-0 disable in /data, so undo it (no-op when already enabled).
pm enable --user 0 com.android.vending >/dev/null 2>&1
USER0_BLOAT="
com.google.android.apps.messaging
com.google.android.apps.photos
com.google.android.youtube
com.google.android.keep
com.google.android.apps.recorder
com.google.android.apps.bard
com.google.android.retaildemo
com.google.android.federatedcompute
com.google.android.ondevicepersonalization.services
com.google.android.aicore
com.google.android.marvin.talkback
com.google.android.googlequicksearchbox
com.android.chrome
"
for pkg in $USER0_BLOAT; do
    am force-stop --user 0 "$pkg" 2>/dev/null
    pm disable-user --user 0 "$pkg" 2>/dev/null
done

# ==============================================================================
# 5. DISABLE GLOBAL SYSTEM BLOATWARE & TELEMETRY (For All Users)
# Things that serve zero purpose on a laptop PC (retail demo, telemetry trackers)
# ==============================================================================
GLOBAL_BLOAT="
com.google.android.retaildemo
com.google.android.federatedcompute
com.google.android.ondevicepersonalization.services
com.google.android.snapshotservice
com.android.simappdialog
com.android.emergency
com.android.eyedropper
"
for pkg in $GLOBAL_BLOAT; do
    am force-stop "$pkg" 2>/dev/null
    pm disable-user --user 10 "$pkg" 2>/dev/null
done

# ==============================================================================
# 6. SUPPORT USER BACKGROUND PROCESSES & MULTITASKING (User 10)
# ==============================================================================
setprop persist.sys.max_cached_processes 64
setprop ro.sys.fw.bg_apps_limit 64
setprop sys.use_fifo_ui 1

pm grant --user 10 com.google.ambient.streaming android.permission.ACCESS_LOCAL_NETWORK 2>/dev/null

sync
