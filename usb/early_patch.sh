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
