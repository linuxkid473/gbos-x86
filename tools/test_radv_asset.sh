#!/bin/sh
# Test of the RADV release asset: tools/package_radv.sh packs it reproducibly, and build.sh's RADV step (run on its own with a stub
# fetch, no downloads) fetches it only when RADV_SHA is set, GBOS_RADV is not 0 and usb/patched/ has no vulkan.radv.so yet; a failed
# download or a bad archive is a warning and leaves no vulkan.radv.so behind.
# Run: sh tools/test_radv_asset.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build.sh
W=$(mktemp -d "${TMPDIR:-/tmp}/radvasset.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0; n=0
ok()  { n=$((n+1)); echo "ok   $*"; }
bad() { n=$((n+1)); fail=$((fail+1)); echo "FAIL $*"; }
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# --- package_radv.sh ---------------------------------------------------------------------------------------------------
printf '\177ELFfake-radv' > $W/vulkan.radv.so
sh "$ROOT/tools/package_radv.sh" $W/vulkan.radv.so $W/a.tar.xz > $W/pk.out 2>&1 && ok "package_radv.sh packs an ELF" || bad "package: $(cat $W/pk.out)"
sleep 1; touch $W/vulkan.radv.so
sh "$ROOT/tools/package_radv.sh" $W/vulkan.radv.so $W/b.tar.xz > /dev/null 2>&1
[ "$(sha $W/a.tar.xz)" = "$(sha $W/b.tar.xz)" ] && ok "archive is reproducible (same sha256 after touch)" || bad "archive differs between runs"
grep -q "RADV_SHA=$(sha $W/a.tar.xz)" $W/pk.out && ok "prints the RADV_SHA line" || bad "RADV_SHA line: $(cat $W/pk.out)"
[ "$(tar -tJf $W/a.tar.xz | grep -v '/$' | tr '\n' ' ')" = "patched/vulkan.radv.so " ] && ok "archive holds only patched/vulkan.radv.so" \
  || bad "archive content: $(tar -tJf $W/a.tar.xz | tr '\n' ' ')"
mkdir -p $W/x && tar -xJf $W/a.tar.xz -C $W/x && cmp -s $W/x/patched/vulkan.radv.so $W/vulkan.radv.so && ok "unpacks to the same bytes" || bad "unpacked file differs"
printf 'not an elf' > $W/bad.so
sh "$ROOT/tools/package_radv.sh" $W/bad.so $W/c.tar.xz > /dev/null 2>&1 && bad "non-ELF accepted" || ok "non-ELF refused"

# --- build.sh's RADV step ----------------------------------------------------------------------------------------------
grep -q '^want_radv() {' "$B" && ok "build.sh has want_radv" || { bad "want_radv missing"; exit 1; }
grep '^want_radv() {' "$B" > $W/want.sh
s=$(grep -n '^if want_radv; then' "$B" | cut -d: -f1)
[ -n "$s" ] && ok "build.sh has the RADV fetch step" || { bad "RADV fetch step missing"; exit 1; }
awk -v s="$s" 'NR >= s { print } NR > s && /^elif .*fi$/ { exit }' "$B" > $W/step.sh
m34=$(grep -n 'say "3/4 kernel modules and firmware"' "$B" | cut -d: -f1)
[ "$s" -gt "$m34" ] && ok "RADV step runs in step 3/4" || bad "RADV step at line $s, before step 3/4 ($m34)"
grep -q '^RADV=.*RADV_SHA=' "$B" && ok "build.sh defines RADV and RADV_SHA" || bad "RADV/RADV_SHA not defined"

run() { # run RADV_SHA GBOS_RADV FETCH(ok|fail|junk) PRESENT(0|1): output in $W/out, stub fetch calls in $W/calls
  rm -rf $W/t; mkdir -p $W/t/usb/patched $W/t/build/dl; rm -f $W/calls
  [ "$4" = 1 ] && printf '\177ELFlocal' > $W/t/usb/patched/vulkan.radv.so
  ( cd $W/t; RADV_SHA=$1; REL_RADV=https://example.invalid; RADV=gbos86-radv-r1.tar.xz; MODE=$3
    if [ "$2" = - ]; then unset GBOS_RADV; else GBOS_RADV=$2; fi
    say() { echo "== $*"; }
    fetch() { echo "$1" >> $W/calls; case $MODE in ok) cp $W/a.tar.xz "$2";; junk) printf junk > "$2";; *) echo "download failed" >&2; exit 1;; esac; }
    . $W/want.sh; . $W/step.sh; echo after ) > $W/out 2>&1
}
run "" - ok 0
[ ! -e $W/calls ] && [ ! -e $W/t/usb/patched/vulkan.radv.so ] && grep -q '^after$' $W/out && ok "empty RADV_SHA: not fetched" || bad "empty sha: $(cat $W/out)"
run abc - ok 0
[ -e $W/calls ] && cmp -s $W/t/usb/patched/vulkan.radv.so $W/vulkan.radv.so && ! grep -q WARNING $W/out && ok "RADV_SHA set: fetched and unpacked to usb/patched/" || bad "fetch ok: $(cat $W/out)"
run abc 0 ok 0
[ ! -e $W/calls ] && [ ! -e $W/t/usb/patched/vulkan.radv.so ] && ok "GBOS_RADV=0: not fetched" || bad "GBOS_RADV=0: $(cat $W/out)"
run abc - ok 1
[ ! -e $W/calls ] && grep -q 'AMD GPU driver (RADV): present' $W/out && grep -q 'ELFlocal' $W/t/usb/patched/vulkan.radv.so && ok "local vulkan.radv.so kept, not fetched" || bad "present: $(cat $W/out)"
run abc - fail 0
grep -q '^after$' $W/out && grep -q 'WARNING: no RADV' $W/out && [ ! -e $W/t/usb/patched/vulkan.radv.so ] && ok "failed download: warning, build goes on" || bad "fetch fail: $(cat $W/out)"
run abc - junk 0
grep -q '^after$' $W/out && grep -q 'WARNING: no RADV' $W/out && [ ! -e $W/t/usb/patched/vulkan.radv.so ] && ok "bad archive: warning, nothing left behind" || bad "junk: $(cat $W/out)"
sh -n "$B" && sh -n "$ROOT/tools/package_radv.sh" && ok "sh -n build.sh package_radv.sh" || bad "syntax"

echo "$n checks, $fail failed"
[ $fail = 0 ] && echo RADV_ASSET_TEST_PASS
exit $fail
