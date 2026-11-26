#!/system/bin/sh
# GBOS System, Performance, RAM and CPU Optimization
# Configured for PC hardware running gbOS86

sleep 4

# ==============================================================================
# 1. LOGGING
# ==============================================================================
# Left out on purpose (2026-11-25, first time this script is shipped; it had never run on hardware): forcing the
# performance governor/EPP (battery, heat), USB autosuspend off, rewriting every cpuset to all CPUs, and
# "stop touchpad-inhibitor" (hook.sh: it must keep running, it inhibits the touchpad when the keyboard is folded).
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
# 3. SWAP: left alone. Google's zram swap and vm sysctls stay; turning zram off would mean more lmkd kills on 8 GB.
# ==============================================================================

# ==============================================================================
# 4. OPTIMIZE HEADLESS USER 0 (Frees 2.5 - 3.0 GB of RAM)
# User 0 is headless (system only); User 10 is the interactive human user.
# Disable duplicate client apps & Google services in User 0 ONLY.
# User 10 remains 100% untouched so the user can run all apps normally.
# NOT TalkBack: gbos_talkback (ACCESSIBILITY=talkback) turns it on in user 0 too.
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
