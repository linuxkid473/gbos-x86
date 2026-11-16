#!/system/bin/sh
# Hide vendor modules built for Google's kernel before Android ueventd starts.
gbos_mask_vendor_modules() {
  VENDOR_ROOT=${VENDOR_ROOT:-/new}
  VMOD=${VENDOR_MODULES_DIR:-$VENDOR_ROOT/vendor/lib/modules}
  MOUNTS=${MOUNTS_FILE:-/proc/mounts}
  if [ -L "$VMOD" ]; then
    VENDOR_MODULES_TARGET=$(readlink "$VMOD" 2>/dev/null)
    case "$VENDOR_MODULES_TARGET" in
      /vendor_dlkm/*) VMOD="$VENDOR_ROOT$VENDOR_MODULES_TARGET" ;;
      *)
        log "hook: cannot safely mask vendor modules: unsupported symlink $VMOD -> $VENDOR_MODULES_TARGET"
        return 1
        ;;
    esac
  fi
  [ -d "$VMOD" ] || {
    log "hook: vendor module directory not present at expected path $VMOD"
    return 1
  }
  if mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs "$VMOD" \
      && awk -v target="$VMOD" '$2 == target { found=1 } END { exit !found }' "$MOUNTS"; then
    log "hook: incompatible Google vendor modules masked at $VMOD; using gbos/modules"
    return 0
  fi
  log "hook: failed to mask incompatible Google vendor modules; ueventd may report module ABI errors"
  return 1
}
