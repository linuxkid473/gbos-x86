#!/bin/sh
# Focused Goodix protocol and transport regression tests.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/gbosfp.XXXXXX")
trap 'chmod -R u+w "$W" 2>/dev/null; rm -rf "$W"' EXIT HUP INT TERM
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

HOSTCC=${HOSTCC:-cc}
$HOSTCC -std=c11 -O2 -Wall -Wextra -Werror -I"$ROOT/fingerprint" \
  -o "$W/goodix_test" "$ROOT/fingerprint/goodix_test.c"
"$W/goodix_test" | grep -q '^GOODIX_PROTOCOL_TEST_PASS$' \
  && ok "Goodix packet, template, runtime-config, and mock transport tests" \
  || bad "Goodix protocol tests"
$HOSTCC -std=c11 -O2 -Wall -Wextra -Werror -I"$ROOT/fingerprint" \
  -o "$W/goodix_sim_test" "$ROOT/fingerprint/goodix_sim_test.c"
"$W/goodix_sim_test" | grep -q '^GOODIX_SIM_TEST_PASS$' \
  && ok "simulated sensor: stale-template cleanup policy, duplicate handling, cancel recovery" \
  || bad "Goodix simulated-sensor tests"

sh -n "$ROOT/usb/hook.sh" "$ROOT/usb/install.sh" "$ROOT/fingerprint/build.sh" \
  "$ROOT/fingerprint/build_image.sh" \
  "$ROOT/tools/make_gbos86_image.sh" "$ROOT/tools/deploy_stick.sh" \
  && ok "shell syntax checks" || bad "shell syntax"

awk '/^gbos_goodix_fp\(\)/ { emit=1 } emit { print } emit && /^}/ { exit }' \
  "$ROOT/usb/hook.sh" > "$W/detect.sh"
awk '/^gbos_system_log\(\)/ { emit=1 } emit { print } emit && /^}/ { exit }' \
  "$ROOT/usb/hook.sh" >> "$W/detect.sh"
awk '/^  FPRC=$/ { emit=1 } emit && /^  cat > \/tmp\/sysinit\/gbos.rc/ { exit } emit { sub(/^  /, ""); print }' \
  "$ROOT/usb/hook.sh" > "$W/service.sh"
mkdir -p "$W/usb/1-1" "$W/gbos"
printf '27c6\n' > "$W/usb/1-1/idVendor"
printf '6384\n' > "$W/usb/1-1/idProduct"
: > "$W/gbos/gbos_fp"
chmod +x "$W/gbos/gbos_fp"

run_service() (
  G="$W/gbos"
  USBDEVS="$W/usb"
  N=7
  export G USBDEVS N
  if [ "$1" = default ]; then unset FINGERPRINT; else FINGERPRINT=$1; export FINGERPRINT; fi
  log() { :; }
  findfs() { [ -n "${FAKE_GBOSSYS:-}" ] && echo "$FAKE_GBOSSYS"; }
  . "$W/detect.sh"
  . "$W/service.sh"
  printf '%s' "$FPRC"
)

RC=$(run_service default)
printf '%s\n' "$RC" | grep -q 'service gbos_fp /postinstall/gbos/gbos_fp' &&
  printf '%s\n' "$RC" | grep -q 'seclabel u:r:hal_fingerprint_default:s0' &&
  printf '%s\n' "$RC" | grep -q 'disabled' &&
  printf '%s\n' "$RC" | grep -q 'oneshot' &&
  printf '%s\n' "$RC" | grep -q 'interface aidl android.hardware.biometrics.fingerprint.IFingerprint/default' &&
  printf '%s\n' "$RC" | grep -q 'start gbos_fp' \
  && ok "early-start, lazy oneshot HAL service for exact Goodix USB ID" || bad "Goodix service selection"

[ -z "$(run_service 0)" ] && ok "FINGERPRINT=0 opt-out" || bad "fingerprint opt-out"

RC=$(run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp.* --log /postinstall/gbos/logs/boot-7/fingerprint.txt$' &&
  ! printf '%s\n' "$RC" | grep -q 'gbos_fp_log' &&
  grep -q '"--log"' "$ROOT/fingerprint/gbos_fp.c" && grep -q 'O_APPEND' "$ROOT/fingerprint/gbos_fp.c" \
  && ok "gbos_fp appends its own lines to fingerprint.txt (no logcat -f)" || bad "fingerprint log"

# logs/boot-N root-only (tools/test_postinstall_ro.sh): the HAL (uid system) gets fingerprint.txt through /dev/gbos-system-log, a bind
# of logs/boot-7/fingerprint.txt (real mounts: test_postinstall_ro.sh); if the bind fails it keeps the logs path (logcat only then)
mkdir -p "$W/gbos/logs/boot-7" "$W/dev"
RC=$(L="$W/gbos/logs/boot-7" GBOS_DEV="$W/dev" GBOS_LOGS_PRIVATE=1; export L GBOS_DEV GBOS_LOGS_PRIVATE
  mount() { echo "mount $*" >> "$W/mounts"; }; chown() { :; }; run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp.* --log /dev/gbos-system-log/fingerprint.txt$' &&
  grep -qx "mount --bind $W/gbos/logs/boot-7/fingerprint.txt $W/dev/gbos-system-log/fingerprint.txt" "$W/mounts" &&
  [ -f "$W/gbos/logs/boot-7/fingerprint.txt" ] \
  && ok "root-only logs: --log /dev/gbos-system-log/fingerprint.txt, bound to logs/boot-7/fingerprint.txt" || bad "system log path: $RC"
RC=$(L="$W/gbos/logs/boot-7" GBOS_DEV="$W/nodev/x" GBOS_LOGS_PRIVATE=1; export L GBOS_DEV GBOS_LOGS_PRIVATE
  mount() { return 1; }; chown() { :; }; run_service default 2>/dev/null)
printf '%s\n' "$RC" | grep -q -- '--log /postinstall/gbos/logs/boot-7/fingerprint.txt$' && ok "system log bind fails: logs path kept" || bad "bind failure: $RC"

# --no-gbos-install only when neither stage0 ($SYS) nor findfs sees a GBOSSYS install
RC=$(unset SYS FAKE_GBOSSYS; run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp --no-gbos-install --log /postinstall/gbos/logs/boot-7/fingerprint.txt$' \
  && ok "no install anywhere: HAL may clean up earlier live boots' templates" || bad "no-install flag missing"
RC=$(SYS=/dev/nvme0n1p2; FAKE_GBOSSYS=; export SYS FAKE_GBOSSYS; run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp --log /postinstall/gbos/logs/boot-7/fingerprint.txt$' \
  && ok "install seen by stage0: no --no-gbos-install" || bad "no-install flag despite stage0 install"
RC=$(unset SYS; FAKE_GBOSSYS=/dev/nvme0n1p2; export FAKE_GBOSSYS; run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp --log /postinstall/gbos/logs/boot-7/fingerprint.txt$' \
  && ok "install appearing after stage0: no --no-gbos-install" || bad "no-install flag despite late install"
# --installed only when stage0 booted the NVMe install (FROM=nvme)
RC=$(SYS=/dev/nvme0n1p2; FROM=nvme; FAKE_GBOSSYS=/dev/nvme0n1p2; export SYS FROM FAKE_GBOSSYS; run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp --installed --log /postinstall/gbos/logs/boot-7/fingerprint.txt$' \
  && ok "NVMe boot: HAL told it is the install" || bad "--installed missing on an NVMe boot"
RC=$(SYS=/dev/nvme0n1p2; FROM=stick; FAKE_GBOSSYS=/dev/nvme0n1p2; export SYS FROM FAKE_GBOSSYS; run_service default)
printf '%s\n' "$RC" | grep -q '^service gbos_fp /postinstall/gbos/gbos_fp --log /postinstall/gbos/logs/boot-7/fingerprint.txt$' \
  && ok "live boot with an install: neither flag" || bad "flags on a live boot with an install"
grep -q '"--no-gbos-install"' "$ROOT/fingerprint/gbos_fp.c" &&
  grep -q '"--installed"' "$ROOT/fingerprint/gbos_fp.c" \
  && ok "HAL parses --no-gbos-install and --installed" || bad "HAL ignores its flags"

printf '6383\n' > "$W/usb/1-1/idProduct"
[ -z "$(run_service 1)" ] && ok "unmatched USB product does not start HAL" || bad "USB product scoping"

printf '6384\n' > "$W/usb/1-1/idProduct"
mv "$W/gbos/gbos_fp" "$W/gbos/gbos_fp.missing"
[ -z "$(run_service 1)" ] && ok "missing HAL binary does not start service" || bad "missing binary handling"

grep -q 'FINGERPRINT=1' "$ROOT/usb/gbos.cfg" &&
  grep -q 'fingerprint/build_image.sh lapis/parts' "$ROOT/build.sh" &&
  grep -q 'gbos_fp' "$ROOT/tools/make_gbos86_image.sh" &&
  grep -q 'gbos_fp' "$ROOT/tools/deploy_stick.sh" &&
  grep -q 'gbos_fp' "$ROOT/usb/install.sh" \
  && ok "build, image, deploy, and install packaging" || bad "HAL packaging wiring"

grep -q 'sensor initialization attempt %d/12' "$ROOT/fingerprint/gbos_fp.c" &&
  grep -q 'no usable Goodix sensor after 12 attempts' "$ROOT/fingerprint/gbos_fp.c" \
  && ok "bounded Goodix startup probe reports each result" || bad "Goodix probe diagnostics"

awk '
  /^if \[ "\$\{GBOS_IPU6_CAMERA_BRIDGE:-0\}" = 1 \]; then/ { camera=1 }
  camera && /FINGERPRINT\)/ { found = ($0 ~ /value=1/) }
  END { exit !found }
' "$ROOT/tools/make_gbos86_image.sh" \
  && ok "Latitude camera diagnostic image keeps Goodix HAL enabled" \
  || bad "camera image disables Goodix HAL"

[ "$fail" = 0 ] && echo GBOS_FP_TEST_PASS
exit "$fail"
