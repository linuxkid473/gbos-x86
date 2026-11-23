#!/bin/sh
# deploy_stick.sh must not copy over a stick that holds a published update set (gbos/system.manifest) unless --over-set is given.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM
mkstick() { rm -rf "$T/vol"; mkdir -p "$T/vol/gbos"; for f in hook.sh stick_fat.sh sysupdate.sh; do cp "$ROOT/usb/$f" "$T/vol/gbos/$f"; done; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# 1. a set stick with a file that differs: refused, nothing copied, system.manifest kept
mkstick; printf 'seq 202611222332\n' > "$T/vol/gbos/system.manifest"; echo old > "$T/vol/gbos/hook.sh"
rc=0; sh "$ROOT/tools/deploy_stick.sh" --no-eject "$T/vol" > "$T/out" 2>&1 || rc=$?
[ $rc = 2 ] || { cat "$T/out"; fail "expected exit 2 on a set stick, got $rc"; }
grep -q 'refusing: .* seq 202611222332' "$T/out" || fail "no refusal message"
grep -qx '  hook.sh' "$T/out" || fail "the differing file is not listed"
[ "$(cat "$T/vol/gbos/hook.sh")" = old ] || fail "hook.sh was copied anyway"
[ -f "$T/vol/gbos/system.manifest" ] || fail "system.manifest was removed"
# 2. --over-set: copies and drops system.manifest (the old behaviour, on request). The eject/verify tail may fail off macOS: ignore rc.
sh "$ROOT/tools/deploy_stick.sh" --no-eject --over-set "$T/vol" > "$T/out" 2>&1 || :
cmp -s "$ROOT/usb/hook.sh" "$T/vol/gbos/hook.sh" || fail "--over-set did not copy hook.sh"
[ ! -f "$T/vol/gbos/system.manifest" ] || fail "--over-set kept a system.manifest that no longer describes the stick"
# 3. a stick without system.manifest: deployed as before
mkstick; echo old > "$T/vol/gbos/hook.sh"
sh "$ROOT/tools/deploy_stick.sh" --no-eject "$T/vol" > "$T/out" 2>&1 || :
cmp -s "$ROOT/usb/hook.sh" "$T/vol/gbos/hook.sh" || fail "a stick without a set was not deployed"
grep -q refusing "$T/out" && fail "refused a stick without a set"
echo "DEPLOY_STICK_TEST_PASS"
