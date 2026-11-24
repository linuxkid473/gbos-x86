#!/bin/sh
# release.sh: the maintainer's one command to publish a gbOS86 update from the main branch, with every check first.
#
#   tools/release.sh check   [--tree GBOSDIR]                       checks only: no key, nothing uploaded
#   tools/release.sh publish --key PRIVATE.pem --tree GBOSDIR        checks, then tools/publish_system_update.sh, then checks GitHub
#   tools/release.sh publish ... --dry-run                           everything, files in build/releases/<seq>/, no upload
#
# What it checks, in order (a failure stops it; nothing is uploaded before all of them pass):
#   1. git: on the release branch (main, or --branch), no uncommitted changes in usb/ lapis/ tools/ updater-app/, HEAD pushed
#      and equal to origin/<branch> (the app compares installed systems with the published commit).
#   2. The Google build: lapis/BUILD (BUILD_ID, VERSION, URL) and usb/patched/MANIFEST's "# build" line name the same build, so the
#      patches were made for it (tools/port_build.sh --write). With --tree: the stick tree's super.layout/super.img exist.
#   3. The key: usb/sysupdate.pub is a real key (not the placeholder) and, with --key, the private key's own public half.
#      Laptops refuse every bundle signed with another key.
#   4. Tests: sh -n on the boot scripts, tools/test_system_update.sh (SYSTEM_UPDATE_TEST_PASS) and tools/test_updater_core.sh
#      (UPDATER_CORE_TEST_PASS). --skip-tests skips them (not for a real release).
#   5. Googlebook OS check (what the app's Googlebook OS card relies on before it offers "Download Rn and install"):
#      the published feed's google_build is read from GitHub. If this release brings a NEWER Google build:
#        - Google's recovery.zip at lapis/BUILD's URL must still be served (dl.google.com, HEAD request only);
#        - tools/super_parts.py must prove super.img is make_super.py's image of Google's partitions (--parts, default lapis/parts),
#          so the feed gets its google_zip/google_part/google_super lines and laptops can rebuild super.img without the stick.
#        If either fails it stops, unless --allow-stick-only (laptops then need the stick's menu 1 for the new build).
#      An OLDER Google build than the published one is refused (laptops never go back).
#      --check-google also runs tools/check_new_build.sh (is there an even newer Google build than lapis/BUILD?).
#   After the upload: downloads the feed from GitHub and checks it is byte for byte the one just signed, with the expected seq and
#   (for a new Google build) the Google lines.
#
# Options: --key FILE  --tree DIR  --parts DIR  --out DIR (default build/releases)  --branch NAME (default main)  --repo OWNER/NAME  --seq N  --dry-run  --yes
#          --skip-tests  --allow-stick-only  --check-google  -- ARGS (passed to publish_system_update.sh, e.g. -- --notes notes.txt)
# Needs: git, curl, openssl, python3, a JDK (test_updater_core.sh), busybox (test_system_update.sh), gh logged in (publish).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LC_ALL=C; export LC_ALL
die() { echo "release: FAIL: $*" >&2; exit 1; }
ok() { echo "  ok: $*"; }
say() { echo "== $*"; }

MODE=${1:-}; [ $# -gt 0 ] && shift
case "$MODE" in check|publish) ;; -h|--help|"") awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0;;
  *) die "first argument must be check or publish (--help)";; esac
KEY=; TREE=; PARTS=; OUTDIR=; BRANCH=main; REPO=; SEQ=; DRY=0; YES=0; SKIPT=0; STICKONLY=0; CHECKG=0
while [ $# -gt 0 ]; do
  case "$1" in
    --key) KEY=$2; shift 2;; --tree) TREE=$2; shift 2;; --parts) PARTS=$2; shift 2;; --branch) BRANCH=$2; shift 2;;
    --repo) REPO=$2; shift 2;; --out) OUTDIR=$2; shift 2;; --seq) SEQ=$2; shift 2;; --dry-run) DRY=1; shift;; --yes) YES=1; shift;;
    --skip-tests) SKIPT=1; shift;; --allow-stick-only) STICKONLY=1; shift;; --check-google) CHECKG=1; shift;;
    --) shift; break;;
    *) die "unknown option $1 (--help)";;
  esac
done
if [ "$MODE" = publish ]; then
  [ -n "$KEY" ] && [ -f "$KEY" ] || die "publish needs --key PRIVATE.pem (the offline signing key)"
  [ -n "$TREE" ] && [ -d "$TREE" ] || die "publish needs --tree GBOSDIR (a stick-like gbos/ folder with super.img, firmware, modules ...)"
fi
[ -n "$SEQ" ] || SEQ=$(date -u +%Y%m%d%H%M)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM
cd "$ROOT"

# ------------------------------------------------------------------------------------------------ 1. git
say "1. git"
_b=$(git rev-parse --abbrev-ref HEAD)
[ "$_b" = "$BRANCH" ] || die "on branch $_b, not $BRANCH (releases come from $BRANCH; --branch to change)"
_dirty=$(git status --porcelain -- usb lapis tools updater-app | grep -v '^??' || true)
[ -z "$_dirty" ] || die "uncommitted changes (commit or stash them):
$_dirty"
git fetch -q origin "$BRANCH" || die "cannot fetch origin/$BRANCH"
HEADC=$(git rev-parse HEAD); REMC=$(git rev-parse "origin/$BRANCH")
[ "$HEADC" = "$REMC" ] || die "HEAD $HEADC is not origin/$BRANCH ($REMC): push (or pull) first"
ok "$BRANCH at $(git log -1 --format='%h %s')"
if [ -z "$REPO" ]; then
  REPO=$(git remote get-url origin | sed -n 's#^\(https://github\.com/\|git@github\.com:\|ssh://git@github\.com/\)\([^/]*/[^/]*\)$#\2#p' | sed 's/\.git$//')
  [ -n "$REPO" ] || die "cannot tell the GitHub repository from origin; pass --repo OWNER/NAME"
fi
ok "repository github.com/$REPO"

# ------------------------------------------------------------------------------------------------ 2. Google build
say "2. Google build"
BUILD_ID=$(sed -n 's/^BUILD_ID=//p' lapis/BUILD); VERSION=$(sed -n 's/^VERSION=//p' lapis/BUILD); URL=$(sed -n 's/^URL=//p' lapis/BUILD)
echo "$BUILD_ID" | grep -qE '^[0-9]+$' || die "lapis/BUILD has no numeric BUILD_ID"
MB=$(sed -n 's/^# build //p' usb/patched/MANIFEST)
case "$MB" in */"$VERSION"/"$BUILD_ID":*) ok "lapis/BUILD and patched/MANIFEST: $VERSION ($BUILD_ID)";;
  *) die "usb/patched/MANIFEST is for '$MB', lapis/BUILD says $VERSION/$BUILD_ID: run tools/port_build.sh --write (porting.md)";; esac
case "$URL" in https://dl.google.com/*/"$BUILD_ID"/*) ;; *) die "lapis/BUILD URL ($URL) is not dl.google.com for build $BUILD_ID";; esac
# the Google fingerprint inside an image (FILE, byte offset of system_a), read with dump.erofs in the gbos-erofs docker image
fp_of() {
  _d=$(cd "$(dirname "$1")" && pwd); _z=; [ "$(getenforce 2>/dev/null)" = Enforcing ] && _z=,z
  docker run --rm -v "$_d:/w:ro$_z" gbos-erofs dump.erofs --offset="$2" --path=/system/build.prop --cat "/w/$(basename "$1")" 2>/dev/null \
    | sed -n 's/^ro\.system\.build\.fingerprint=//p' | head -n 1
}
HAVE_ERO=0; command -v docker >/dev/null 2>&1 && docker image inspect gbos-erofs >/dev/null 2>&1 && HAVE_ERO=1
if [ -n "$TREE" ]; then
  for f in super.img super.layout; do [ -f "$TREE/$f" ] || die "$TREE/$f is missing"; done
  if [ $HAVE_ERO = 1 ]; then
    _st=$(awk '$1 == "system_a" { print $2; exit }' "$TREE/super.layout")
    _fp=$(fp_of "$TREE/super.img" $((_st * 512)))
    [ "$_fp" = "$MB" ] || die "the tree's super.img is '${_fp:-unreadable}', not $VERSION ($MB): copy the right super.img into $TREE"
    ok "tree's super.img is $VERSION"
  else echo "  note: no docker image gbos-erofs: the tree's super.img build is not checked (docker build -t gbos-erofs tools/erofs-docker)"; fi
fi
[ $CHECKG = 1 ] && { sh tools/check_new_build.sh | sed 's/^/  /' || true; }

# ------------------------------------------------------------------------------------------------ 3. key
say "3. signing key"
grep -qE '^rsa3072 [0-9A-Fa-f]{768}$' usb/sysupdate.pub || die "usb/sysupdate.pub is the placeholder (no key): laptops would refuse everything"
if [ -n "$KEY" ]; then
  sh tools/make_system_update.sh pubkey "$KEY" > "$T/pub" || die "cannot read the private key (wrong passphrase?)"
  [ "$(grep '^rsa3072 ' "$T/pub" | tr a-f A-F)" = "$(grep '^rsa3072 ' usb/sysupdate.pub | tr a-f A-F)" ] \
    || die "--key is not the key of usb/sysupdate.pub: laptops would refuse this update"
  ok "the private key matches usb/sysupdate.pub"
else ok "usb/sysupdate.pub holds a key (no --key given to compare)"; fi

# ------------------------------------------------------------------------------------------------ 4. tests
say "4. tests"
if [ $SKIPT = 1 ]; then echo "  SKIPPED (--skip-tests)"
else
  sh -n usb/hook.sh usb/install.sh usb/stage0_init.sh usb/apex_patch.sh usb/sysupdate.sh || die "shell syntax"
  ok "sh -n"
  for t in system_update:SYSTEM_UPDATE_TEST_PASS updater_core:UPDATER_CORE_TEST_PASS; do
    n=${t%%:*}; m=${t#*:}
    sh "tools/test_$n.sh" > "$T/$n.log" 2>&1 || true
    grep -q "$m" "$T/$n.log" || { tail -n 30 "$T/$n.log" >&2; die "tools/test_$n.sh did not print $m (log above)"; }
    ok "test_$n.sh"
  done
fi

# ------------------------------------------------------------------------------------------------ 5. Googlebook OS check
say "5. Googlebook OS (Google build in the feed)"
FEEDURL=https://github.com/$REPO/releases/download/sysupdate-feed/gbos86-update.feed
PUBGB=; PUBSEQ=
if curl -fsSL --max-time 30 -o "$T/old.feed" "$FEEDURL" 2>/dev/null; then
  PUBGB=$(sed -n 's/^google_build //p' "$T/old.feed"); PUBSEQ=$(sed -n 's/^seq //p' "$T/old.feed")
  ok "published now: seq $PUBSEQ, Google build $PUBGB"
  [ "$SEQ" -gt "$PUBSEQ" ] || die "seq $SEQ is not newer than the published $PUBSEQ"
else echo "  no published feed yet ($FEEDURL): first release"; fi
NEWGOOGLE=0
if [ -n "$PUBGB" ] && [ "$BUILD_ID" -lt "$PUBGB" ]; then die "Google build $BUILD_ID is OLDER than the published $PUBGB: laptops never go back"; fi
{ [ -z "$PUBGB" ] || [ "$BUILD_ID" -gt "$PUBGB" ]; } && NEWGOOGLE=1
if [ $NEWGOOGLE = 0 ]; then ok "same Google build as published ($BUILD_ID): no Googlebook OS update in this release"
else
  echo "  this release brings Google build $BUILD_ID ($VERSION): the app's Googlebook OS card will offer it"
  GOOGLE_OK=1
  _h=$(curl -sIL --max-time 20 -o /dev/null -w '%{http_code}' "$URL" || true)
  if [ "$_h" = 200 ]; then ok "Google still serves $URL"; else echo "  NO: $URL answers $_h" >&2; GOOGLE_OK=0; fi
  if [ -n "$TREE" ]; then
    [ -n "$PARTS" ] || PARTS=$ROOT/lapis/parts
    [ -d "$PARTS" ] || { _c=$(git rev-parse --git-common-dir); _mc=$(cd "$_c/.." && pwd); [ -d "$_mc/lapis/parts" ] && PARTS=$_mc/lapis/parts; } || true
    if [ ! -d "$PARTS" ]; then echo "  NO: no Google partitions ($PARTS) to prove super.img against (--parts DIR)" >&2; GOOGLE_OK=0
    elif [ $HAVE_ERO = 1 ] && [ "$(fp_of "$PARTS/system_a.img" 0)" != "$MB" ]; then
      echo "  NO: $PARTS/system_a.img is not $VERSION (fill it from Google's zip for this build: ./build.sh or tools/fetch_google_parts.py)" >&2; GOOGLE_OK=0
    elif python3 -I tools/super_parts.py "$TREE/super.img" "$TREE/super.layout" --parts "$PARTS" > "$T/google" 2> "$T/google.err"; then
      ok "super.img is make_super.py's image of Google's partitions ($(grep -c '^google_part ' "$T/google") partitions): laptops can rebuild it"
    else echo "  NO: $(cat "$T/google.err")" >&2; GOOGLE_OK=0; fi
  else echo "  (no --tree: super.img not proved; publish will do it)"; fi
  if [ $GOOGLE_OK = 0 ]; then
    [ $STICKONLY = 1 ] || die "laptops could not download Google build $BUILD_ID from the app (above). Fix it, or --allow-stick-only"
    echo "  --allow-stick-only: the feed will have no Google lines; laptops need the stick (menu 1) for build $BUILD_ID"
  fi
fi

if [ "$MODE" = check ]; then echo; echo "release check PASSED (commit $HEADC, Google build $BUILD_ID)"; exit 0; fi

# ------------------------------------------------------------------------------------------------ publish
say "6. publish (tools/publish_system_update.sh)"
[ -n "$OUTDIR" ] || OUTDIR=$ROOT/build/releases; mkdir -p "$OUTDIR"; OUTDIR=$(cd "$OUTDIR" && pwd)
_up=; [ $DRY = 1 ] && _up=--no-upload; _y=; [ $YES = 1 ] && _y=--yes
# shellcheck disable=SC2086
{ _rc=0; sh tools/publish_system_update.sh --key "$KEY" --tree "$TREE" --commit "$HEADC" --seq "$SEQ" --repo "$REPO" --out "$OUTDIR" \
  ${PARTS:+--parts "$PARTS"} $_up $_y "$@" || _rc=$?; echo $_rc > "$T/rc"; } 2>&1 | tee "$T/publish.log"
[ "$(cat "$T/rc")" = 0 ] || die "publish_system_update.sh failed (above)"
FEED=$OUTDIR/$SEQ/gbos86-update.feed
[ -f "$FEED" ] || die "publish made no feed ($FEED)"
if [ $NEWGOOGLE = 1 ] && [ $STICKONLY = 0 ]; then
  grep -q '^google_zip https://dl.google.com/' "$FEED" && grep -q '^google_super ' "$FEED" \
    || die "the feed has no Google lines although the checks passed: look at $FEED before uploading anything"
  ok "feed carries google_zip/google_part/google_super for build $BUILD_ID"
fi
grep -q 'not uploaded' "$T/publish.log" && { echo "release files in $OUTDIR/$SEQ (not uploaded)"; exit 0; }

say "7. what laptops will read"
curl -fsSL --max-time 60 -o "$T/live.feed" "$FEEDURL?r=$SEQ" || die "cannot download $FEEDURL"
cmp -s "$T/live.feed" "$FEED" || die "the feed on GitHub is not the one just signed (cache? retry in a minute: curl -fsSL $FEEDURL)"
ok "github serves the new feed: seq $SEQ, Google build $BUILD_ID"
echo; echo "released: https://github.com/$REPO/releases/tag/sysupdate-$SEQ"
echo "next: add a dated line to HANDOFF.md (what was released) and commit it"
