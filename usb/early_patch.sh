# Sourced from gbos.cfg, before any partition is mounted. stage0's `dmctl create` calls (one per partition, each
# a fresh process that loads libcrypto) run BoringSSL's load-time module hash through SHA-NI, which traps here:
# ~3.7M emulated instructions, ~2 s of boot, all before hook.sh gets to bind the patched libraries. So bind them
# first: this wraps chroot (dmctl's only external command) and does it once, just before the first call.
# Which files, and the sha256 of the exact original each patch needs, come from patched/MANIFEST ("early" lines; written
# by tools/port_build.py). Fail-safe: if this never fires, hook.sh does the same binds later (gbos_bind_patches).

# Reset one-shot MODE=live back to MODE=ask so subsequent boots prompt the user
_cfg="${G:-/gbos/gbos}/gbos.cfg"
if [ -f "$_cfg" ] && grep -q '^MODE=live' "$_cfg" 2>/dev/null; then
  sed -i 's/^MODE=live/MODE=ask/' "$_cfg" 2>/dev/null || true
  sync 2>/dev/null || true
fi
unset _cfg

# System updates without the stick (usb/sysupdate.sh, docs/system-update.md). gbos.cfg sources this file in stage0 right after the log
# folder exists and before hwdetect loads modules or super.img is mounted, so an update is checked, switched in, finished or rolled back
# before anything uses those files. Only on the installed system (FROM=nvme), only in stage0 itself (PID 1), once per boot.
# The engine runs as its own process with a clean environment (env -i): nothing leaks in from the kernel command line, and nothing it
# does can change stage0's variables. While an update is pending, the copy of the engine that started it (update/engine.sh) runs, so
# the code that undoes a switch is the code that wrote its journal. Exit 10 = the kernel changed: restart at once, so the new kernel
# never runs with the old modules (or the reverse). stage0_init.sh has the same call as a fallback (in kernels built after 2026-11-20), guarded by
# GBOS_SYSUPDATE_RAN, for a gbos.cfg that does not source this file.
gbos_sysupdate_run() { # ENGINE
  env -i PATH=/bin:/sbin sh "$1" boot "$G" "$L" "$N" --dev "$DEV"
}
gbos_sysupdate_stage0() {
  [ "$$" = 1 ] && [ -z "$GBOS_SYSUPDATE_RAN" ] && [ "$FROM" = nvme ] && [ -n "$G" ] && [ -n "$L" ] || return 0
  GBOS_SYSUPDATE_RAN=1
  [ -d "$G/update" ] || [ -d "$G/update.inbox" ] || [ -d "$G/update.staging" ] || return 0
  _sue=
  [ -f "$G/update/state" ] && [ -f "$G/update/engine.sh" ] && sh -n "$G/update/engine.sh" 2>/dev/null && _sue=$G/update/engine.sh
  [ -n "$_sue" ] || { [ -f "$G/sysupdate.sh" ] && sh -n "$G/sysupdate.sh" 2>/dev/null && _sue=$G/sysupdate.sh; }
  [ -n "$_sue" ] || { log "sysupdate: no usable gbos/sysupdate.sh; update folders left alone"; return 0; }
  gbos_sysupdate_run "$_sue"; _sur=$?
  # The engine died part-way (not 0 or 10) in the middle of a switch: run it once more, it undoes the journal first.
  if [ $_sur != 0 ] && [ $_sur != 10 ] && grep -qx 'phase=switching' "$G/update/state" 2>/dev/null; then
    log "sysupdate: engine exited $_sur during a switch; running it again to undo"
    gbos_sysupdate_run "$_sue"; _sur=$?
  fi
  [ $_sur = 0 ] || log "sysupdate: engine exit $_sur"
  if [ $_sur = 10 ]; then
    log "sysupdate: the kernel was changed: restarting now"; sync
    if [ -f "$G/stick_fat.sh" ] && . "$G/stick_fat.sh"; then gbos_fat_release "$DEV" /gbos "$L"; fi
    reboot -f
  fi
  unset _sue _sur
  return 0
}
gbos_sysupdate_stage0

gbos_early_patch() {
  if [ -f "$G/sensor_vendor_overlay.sh" ]; then
    . "$G/sensor_vendor_overlay.sh"
    gbos_prepare_sensor_vendor_overlay || true
  fi
  [ -f "$G/patched/MANIFEST" ] || return 0
  while read -r ep_kind ep_target ep_want ep_file ep_rest; do
    [ "$ep_kind" = early ] || continue
    [ -f "$G/$ep_file" ] && [ -f "$ep_target" ] || continue
    if [ "$(sha256sum "$ep_target" | cut -d' ' -f1)" = "$ep_want" ]; then
      mount --bind "$G/$ep_file" "$ep_target" && log "early: patched $ep_target"
    fi
  done < "$G/patched/MANIFEST"
}
chroot() {
  if [ -z "$GBOS_EARLY_DONE" ] && [ "$1" = /new ]; then
    GBOS_EARLY_DONE=1
    gbos_early_patch
  fi
  command chroot "$@"
}
