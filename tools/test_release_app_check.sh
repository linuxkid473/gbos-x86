#!/bin/sh
# Test tools/release.sh's "3b. Updater app" check and tools/apk_stamp.py with fake APKs (a zip holding a classes.dex with the
# stamp string): the right repo and this commit's updater-app tree passes; another repo (a lab build), other or uncommitted
# sources, no stamp, two stamps and a missing APK are refused. Prints RELEASE_APP_CHECK_TEST_PASS.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM
REPO=$(sed -n 's/^REPO=//p' tools/release.conf | tail -n 1)
BRANCH=main
WT=$(git rev-parse HEAD:updater-app); HC=$(git rev-parse HEAD)
fail() { echo "FAIL: $*" >&2; exit 1; }

# the block under test, as it is in release.sh (from its "3b." header line to the next section)
awk '/^# -+ 3b\. Updater app/ { on = 1 } on && /^# -+ 4\. tests/ { exit } on { print }' tools/release.sh > "$T/block.sh"
grep -q 'apk_stamp.py' "$T/block.sh" || fail "release.sh has no 3b. Updater app block"

fake_apk() { # OUT STAMP... (each STAMP is put into classes.dex; none = no stamp)
  _o=$1; shift
  python3 -I - "$_o" "$@" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    z.writestr('AndroidManifest.xml', b'\0')
    z.writestr('classes.dex', b'dex\n035\0' + b''.join(b'\0junk\0' + s.encode() + b'\0' for s in sys.argv[2:]))
PY
}
run() { # TREE: runs the block; prints its output, returns its status
  ( TREE=$1
    die() { echo "release: FAIL: $*"; exit 1; }
    ok() { echo "  ok: $*"; }
    say() { echo "== $*"; }
    . "$T/block.sh" ) 2>&1
}
expect() { # NAME pass|fail PATTERN TREE
  _out=$(run "$4") && _rc=0 || _rc=$?
  case "$2" in
    pass) [ $_rc = 0 ] || fail "$1: refused: $_out";;
    fail) [ $_rc != 0 ] || fail "$1: passed but should be refused: $_out";;
  esac
  echo "$_out" | grep -q "$3" || fail "$1: output lacks '$3': $_out"
  echo "ok: $1"
}
mk() { mkdir -p "$T/$1"; [ $# -lt 2 ] || { _d=$1; shift; fake_apk "$T/$_d/gbos86-updater.apk" "$@"; }; }

mk good "gbos86-build repo=$REPO tree=$WT commit=$HC"
expect "main build of this commit" pass "checks github.com/$REPO and was built from" "$T/good"

mk lab "gbos86-build repo=Stark869/gbos86-lab tree=$WT commit=$HC"
expect "lab build (other repo)" fail "checks github.com/Stark869/gbos86-lab for updates, not $REPO" "$T/lab"

mk old "gbos86-build repo=$REPO tree=0123456789abcdef0123456789abcdef01234567 commit=$HC"
expect "other updater-app sources" fail "built from other updater-app sources" "$T/old"

mk dirty "gbos86-build repo=$REPO tree=$WT-dirty commit=$HC"
expect "uncommitted updater-app changes" fail "built from other updater-app sources" "$T/dirty"

mk nostamp ""
expect "no stamp (older build)" fail "has no build stamp" "$T/nostamp"

mk two "gbos86-build repo=$REPO tree=$WT commit=$HC" "gbos86-build repo=Stark869/gbos86-lab tree=$WT commit=$HC"
expect "two different stamps" fail "has no build stamp" "$T/two"

mk empty
expect "no APK in the tree" fail "gbos86-updater.apk is missing" "$T/empty"

expect "no --tree" pass "the app is not checked" ""

# apk_stamp.py on its own: exit codes
python3 -I tools/apk_stamp.py "$T/good/gbos86-updater.apk" | grep -qx "repo=$REPO tree=$WT commit=$HC" || fail "apk_stamp output"
python3 -I tools/apk_stamp.py "$T/nostamp/gbos86-updater.apk" 2>/dev/null && fail "apk_stamp: no stamp gave 0" || [ $? = 1 ] || fail "apk_stamp: no stamp not 1"
echo notazip > "$T/bad.apk"
python3 -I tools/apk_stamp.py "$T/bad.apk" 2>/dev/null && fail "apk_stamp: bad zip gave 0" || [ $? = 2 ] || fail "apk_stamp: bad zip not 2"
echo "ok: apk_stamp.py exit codes"

echo RELEASE_APP_CHECK_TEST_PASS
