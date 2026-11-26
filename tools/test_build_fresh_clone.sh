#!/bin/sh
# Test of the two build.sh steps that stopped a build from a fresh clone (issue 65), run on their own with stub tools (no downloads):
#   - the Latitude 7320 camera payload check runs after step 3/4 (modules and firmware), and only for GBOS_IPU6_CAMERA_BRIDGE=1;
#     left unset, an incomplete payload turns the bridge off with a warning; an explicit =1 stays an error; =0 skips the check
#   - a failing fingerprint HAL build (no kernel build setup) is a warning, not the end of the build
#   - make_gbos86_image.sh checks the camera payload only for a bridge image
# Run: sh tools/test_build_fresh_clone.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build.sh
M=$ROOT/tools/make_gbos86_image.sh
W=$(mktemp -d "${TMPDIR:-/tmp}/freshclone.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }

# the camera block: from its "if" (one line above the say) to the next line that is just "fi"
c=$(grep -n 'Latitude 7320 cameras: checking the kernel, modules and firmware' "$B" | cut -d: -f1)
[ -n "$c" ] && ok "build.sh has the camera check step" || { bad "camera check step missing"; exit 1; }
awk -v s=$((c - 1)) 'NR >= s { print } NR > s && /^fi$/ { exit }' "$B" > $W/camera.sh
f=$(grep -n '^say "Building Goodix' "$B" | cut -d: -f1)
awk -v s="$f" 'NR >= s { print } NR > s && /^fi$/ { exit }' "$B" > $W/fp.sh

# order: the check comes after step 3/4 (module tree + firmware), and nowhere before it
m34=$(grep -n 'say "3/4 kernel modules and firmware"' "$B" | cut -d: -f1)
first=$(grep -n 'check_camera_payload.sh' "$B" | head -1 | cut -d: -f1)
[ -n "$m34" ] && [ "$first" -gt "$m34" ] && ok "camera check runs after step 3/4 (line $first > $m34)" || bad "camera check before step 3/4: $first vs $m34"

# stub tree: tools/extract_ikconfig.py and tools/check_camera_payload.sh; CHECK_RC decides the check's result
mkdir -p $W/t/tools $W/t/fingerprint $W/t/usb/out $W/bin
printf 'CONFIG_X=y\n' > $W/t/tools/extract_ikconfig.py
printf '#!/bin/sh\necho checked >> %s/calls\n[ "$CHECK_RC" = 0 ] || echo "camera payload: missing module x" >&2\nexit $CHECK_RC\n' "$W" > $W/t/tools/check_camera_payload.sh
printf '#!/bin/sh\necho "missing local gbos-erofs image" >&2\nexit $FP_RC\n' > $W/t/fingerprint/build_image.sh
printf '#!/bin/sh\ncat "$1"\n' > $W/bin/python3; chmod +x $W/bin/python3
cam() { # cam EXPLICIT BRIDGE CHECK_RC: runs the camera block; output and the final bridge value in $W/out
  rm -f $W/calls
  ( cd $W/t; PATH=$W/bin:$PATH; KREL=6.18.21-gbos-whl; CAMERA_EXPLICIT=$1; GBOS_IPU6_CAMERA_BRIDGE=$2; CHECK_RC=$3; export CHECK_RC
    say() { echo "== $*"; }; die() { echo "ERROR: $*" >&2; exit 1; }
    . $W/camera.sh; echo "bridge=$GBOS_IPU6_CAMERA_BRIDGE" ) > $W/out 2>&1
}
cam "" 1 1; rc=$?
[ $rc = 0 ] && grep -q '^bridge=0$' $W/out && grep -q 'WARNING: Latitude 7320 cameras left out' $W/out \
  && ok "unset + incomplete payload: bridge off with a warning, build goes on" || bad "unset/incomplete: rc=$rc $(cat $W/out)"
cam 1 1 1; rc=$?
[ $rc != 0 ] && grep -q 'ERROR: GBOS_IPU6_CAMERA_BRIDGE=1' $W/out && ok "explicit =1 + incomplete payload: error" || bad "explicit: rc=$rc $(cat $W/out)"
cam "" 1 0; rc=$?
[ $rc = 0 ] && grep -q '^bridge=1$' $W/out && ! grep -q WARNING $W/out && ok "complete payload: bridge stays on" || bad "complete: rc=$rc $(cat $W/out)"
cam 1 0 1; rc=$?
[ $rc = 0 ] && [ ! -e $W/calls ] && grep -q '^bridge=0$' $W/out && ok "=0: check not run" || bad "=0: rc=$rc calls=$(cat $W/calls 2>&1)"

fp() { ( cd $W/t; FP_RC=$1; export FP_RC; say() { echo "== $*"; }; . $W/fp.sh; echo after ) > $W/out 2>&1; }
fp 1; rc=$?
[ $rc = 0 ] && grep -q '^after$' $W/out && grep -q 'WARNING: no Goodix fingerprint HAL' $W/out \
  && ok "fingerprint HAL without the toolchain: warning, build goes on" || bad "fp fail: rc=$rc $(cat $W/out)"
fp 0; rc=$?
[ $rc = 0 ] && ! grep -q WARNING $W/out && ok "fingerprint HAL built: no warning" || bad "fp ok: $(cat $W/out)"

# make_gbos86_image.sh: the check is behind the bridge condition (a few lines above it)
g=$(grep -n 'GBOS_IPU6_CAMERA_BRIDGE:-0}" != 1 ] || (' "$M" | head -1 | cut -d: -f1)
k=$(grep -n 'check_camera_payload.sh' "$M" | head -1 | cut -d: -f1)
[ -n "$g" ] && [ -n "$k" ] && [ "$k" -gt "$g" ] && [ $((k - g)) -le 8 ] && ok "make_gbos86_image.sh: camera check only for GBOS_IPU6_CAMERA_BRIDGE=1" \
  || bad "make_gbos86_image.sh guard: guard=$g check=$k"
sh -n "$B" && sh -n "$M" && ok "sh -n build.sh make_gbos86_image.sh" || bad "syntax"

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo BUILD_FRESH_CLONE_TEST_PASS
exit $fail
