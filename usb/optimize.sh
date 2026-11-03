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

# Disable USB autosuspend on mice/keyboards so there is no wake latency
for d in /sys/bus/usb/devices/*/power/control; do
    echo on > "$d" 2>/dev/null
done

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
settings put global disable_window_blurs 1 2>/dev/null
cmd window disable-blur 1 2>/dev/null
setprop persist.sys.sf.disable_blurs 1 2>/dev/null
settings put global window_animation_scale 0 2>/dev/null
settings put global transition_animation_scale 0 2>/dev/null
settings put global animator_duration_scale 0 2>/dev/null

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
# ==============================================================================
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
com.android.vending
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
