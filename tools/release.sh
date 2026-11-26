#!/bin/sh
# release.sh: the maintainer's one command to release a gbOS86 update from the main branch, with every check first.
#
#   tools/release.sh                      the plan: what a release made today would contain and which laptops get a small download
#   tools/release.sh setup                once per computer: where the signing key and the stick folder are (asks; not in the repo)
#   tools/release.sh start VALUE          the starting point (in tools/release.conf, commit it): laptops on any update published
#                                         from VALUE on get a small download (delta); older ones the full bundle. VALUE is
#                                         a date (2026-11-20: the first update published that day or later), an update number
#                                         (202611201230), "last" (the newest published update) or "all"
#   tools/release.sh check                every check below, nothing signed or uploaded
#   tools/release.sh publish [--dry-run]  the checks, then the update made from main as it is today, signed, uploaded (asks first)
#
# The update is always main as it is on the day you run it (its number is today's date and time, UTC). The starting point only
# decides which earlier updates the small download covers. Laptops never go back to an older update or an older Google build.
#
# What it checks, in order (a failure stops it; nothing is uploaded before all of them pass):
#   1. git: on the release branch (main, or BRANCH in release.conf), no uncommitted changes in usb/ lapis/ tools/ updater-app/,
#      HEAD pushed and equal to origin/<branch> (the app compares installed systems with the published commit).
#   2. The Google build: lapis/BUILD (BUILD_ID, VERSION, URL) and usb/patched/MANIFEST's "# build" line name the same build, so the
#      patches were made for it (tools/port_build.sh --write). The stick tree's super.img is that build (dump.erofs, gbos-erofs image).
#   3. The key: usb/sysupdate.pub is a real key (not the placeholder) and, with a key, the private key's own public half.
#   3b. The app: the tree's gbos86-updater.apk checks REPO's feed (not a lab or fork build) and was built from this commit's
#      updater-app/ (its build stamp, tools/apk_stamp.py).
#   4. Tests: sh -n on the boot scripts, tools/test_system_update.sh, tools/test_updater_core.sh and tools/test_release_app_check.sh.
#   5. Googlebook OS check (what the app's Googlebook OS card relies on before it offers "Download Rn and install"):
#      for a Google build NEWER than the published one, Google's recovery.zip must still be served and tools/super_parts.py must
#      prove super.img from Google's partitions (lapis/parts), so the feed carries google_zip/google_part/google_super.
#      Otherwise it stops, unless --allow-stick-only. An OLDER Google build than the published one is refused.
#   After the upload: the feed on GitHub must be byte for byte the one just signed.
#
# Only for the main repository (REPO in tools/release.conf); a clone whose origin is another repository (a fork, the lab) is refused.
# Options (each overrides the config): --key FILE  --tree DIR  --parts DIR  --start VALUE  --out DIR  --branch NAME
#   --dry-run  --yes  --skip-tests  --allow-stick-only  --check-google (also runs tools/check_new_build.sh)
#   -- ARGS    passed to publish_system_update.sh (e.g. -- --notes notes.txt)
# Config: tools/release.conf (REPO, START, BRANCH: shared, committed) and ~/.config/gbos86/release.conf (KEY, TREE, PARTS: this computer).
# Needs: git, curl, openssl, python3, a JDK (test_updater_core.sh), busybox (test_system_update.sh), gh logged in (publish).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LC_ALL=C; export LC_ALL
die() { echo "release: FAIL: $*" >&2; exit 1; }
ok() { echo "  ok: $*"; }
say() { echo "== $*"; }
SHARED=$ROOT/tools/release.conf
LOCAL=${GBOS_RELEASE_LOCAL:-${XDG_CONFIG_HOME:-$HOME/.config}/gbos86/release.conf}
# KEY=VALUE files are read, never sourced (a path with spaces or a $ stays as written)
conf() { [ -f "$2" ] && sed -n "s/^$1=//p" "$2" | tail -n 1 || true; }

MODE=${1:-plan}; case "$MODE" in -h|--help|help) ;; -*) MODE=plan;; *) [ $# -gt 0 ] && shift;; esac
case "$MODE" in plan|setup|start|check|publish) ;; help) MODE=--help;; esac
case "$MODE" in -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0;;
  plan|setup|start|check|publish) ;; *) die "unknown command '$MODE': plan, setup, start, check or publish (--help)";; esac

# ------------------------------------------------------------------------------------------------ setup: this computer's paths
if [ "$MODE" = setup ]; then
  echo "Where things are on this computer (saved in $LOCAL, never in the repository). Enter keeps the value in [ ]."
  ask() { # NAME PROMPT TEST(file|dir)
    _cur=$(conf "$1" "$LOCAL")
    while :; do
      printf '%s [%s]: ' "$2" "$_cur"; read -r _v || exit 1; [ -n "$_v" ] || _v=$_cur
      case "$_v" in "~"/*) _v=$HOME/${_v#"~/"};; esac
      if [ "$3" = file ] && [ -f "$_v" ]; then break; fi
      if [ "$3" = dir ] && [ -d "$_v" ]; then break; fi
      [ "$3" = optdir ] && { [ -z "$_v" ] || [ -d "$_v" ]; } && break
      echo "  not found: '$_v'"
    done
    eval "$1=\$_v"
  }
  ask KEY "Signing key (sysupdate-private.pem on the offline stick)" file
  ask TREE "Stick folder (gbos/ with super.img, super.layout, firmware, modules)" dir
  for f in super.img super.layout; do [ -f "$TREE/$f" ] || echo "  warning: $TREE/$f is missing; publish will refuse until it is there"; done
  ask PARTS "Google partitions of the current build (Enter for lapis/parts)" optdir
  mkdir -p "$(dirname "$LOCAL")"
  { echo "# tools/release.sh setup, $(date -u +%Y-%m-%d)"; echo "KEY=$KEY"; echo "TREE=$TREE"; echo "PARTS=$PARTS"; } > "$LOCAL"
  echo "saved $LOCAL. Next: tools/release.sh   (shows the plan)"
  exit 0
fi

KEY=$(conf KEY "$LOCAL"); TREE=$(conf TREE "$LOCAL"); PARTS=$(conf PARTS "$LOCAL")
START=$(conf START "$SHARED"); BRANCH=$(conf BRANCH "$SHARED"); [ -n "$BRANCH" ] || BRANCH=main
OUTDIR=; REPO=$(conf REPO "$SHARED"); SEQ=; DRY=0; YES=0; SKIPT=0; STICKONLY=0; CHECKG=0; PUBFILE=
NEWSTART=; [ "$MODE" = start ] && { NEWSTART=${1:-}; [ -n "$NEWSTART" ] || die "usage: tools/release.sh start DATE|SEQ|last|all"; shift; }
while [ $# -gt 0 ]; do
  case "$1" in
    --key) KEY=$2; shift 2;; --tree) TREE=$2; shift 2;; --parts) PARTS=$2; shift 2;; --branch) BRANCH=$2; shift 2;;
    --start) START=$2; shift 2;; --out) OUTDIR=$2; shift 2;; --seq) SEQ=$2; shift 2;;
    --dry-run) DRY=1; shift;; --yes) YES=1; shift;;
    --skip-tests) SKIPT=1; shift;; --allow-stick-only) STICKONLY=1; shift;; --check-google) CHECKG=1; shift;;
    --published-file) PUBFILE=$2; shift 2;;   # tests: "seq date manifest-url" lines instead of asking GitHub
    --) shift; break;;
    *) die "unknown option $1 (--help)";;
  esac
done
if [ "$MODE" = publish ]; then
  [ -n "$KEY" ] && [ -f "$KEY" ] || die "no signing key${KEY:+ at $KEY} (plug in the key stick; tools/release.sh setup, or --key FILE)"
  [ -n "$TREE" ] && [ -d "$TREE" ] || die "no stick folder${TREE:+ at $TREE} (tools/release.sh setup, or --tree DIR)"
fi
[ -n "$SEQ" ] || SEQ=$(date -u +%Y%m%d%H%M)
[ -n "$OUTDIR" ] || OUTDIR=$ROOT/build/releases
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM
cd "$ROOT"
# Only the project's main repository (REPO in tools/release.conf), never a fork or the lab repository: origin must be it
echo "$REPO" | grep -qE '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' || die "tools/release.conf has no REPO=OWNER/NAME"
_ou=$(git remote get-url origin 2>/dev/null || true)
_or=$(echo "$_ou" | sed -n 's#^\(https://github\.com/\|git@github\.com:\|ssh://git@github\.com/\)\([^/]*/[^/]*\)$#\2#p' | sed 's/\.git$//')
case "$_ou" in
  *github.com*) [ "$_or" = "$REPO" ] || die "origin is github.com/$_or, not the main repository $REPO: releases are made only from a clone of $REPO";;
  "") die "no origin remote (clone $REPO)";;
  *) [ -n "$PUBFILE" ] || die "origin ($_ou) is not github.com/$REPO";;   # a local origin: tests only
esac

# ------------------------------------------------------------------------------------------------ published updates, the starting point
# $T/published: "seq yyyy-mm-dd manifest-url", oldest first (GitHub's public API: no login needed)
if [ -n "$PUBFILE" ]; then sort -n "$PUBFILE" > "$T/published"
else
  _p=1; : > "$T/rel.json.list"
  while [ $_p -le 20 ]; do
    curl -fsSL --max-time 30 -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$REPO/releases?per_page=100&page=$_p" \
      -o "$T/rel$_p.json" || die "cannot list the releases of github.com/$REPO (network? rate limit?)"
    echo "$T/rel$_p.json" >> "$T/rel.json.list"
    [ "$(python3 -I -c 'import json,sys; print(len(json.load(open(sys.argv[1]))))' "$T/rel$_p.json")" = 100 ] || break
    _p=$((_p + 1))
  done
  python3 -I -c '
import json, re, sys
out = {}
for f in open(sys.argv[1]).read().split():
    for r in json.load(open(f)):
        m = re.fullmatch(r"sysupdate-([0-9]{1,12})", r.get("tag_name", ""))
        if not m or r.get("draft"):
            continue
        u = [a["browser_download_url"] for a in r.get("assets", []) if a["name"].endswith(".manifest")]
        if u:
            out[int(m.group(1))] = "%s %s %s" % (m.group(1), (r.get("published_at") or "")[:10] or "-", u[0])
for k in sorted(out):
    print(out[k])
' "$T/rel.json.list" > "$T/published"
fi
NPUB=$(wc -l < "$T/published" | tr -d ' ')
# START (date / seq / last / all) -> SINCE (a published seq, or empty = no delta)
resolve_start() {
  case "$1" in
    ""|all) head -n 1 "$T/published" | cut -d' ' -f1;;
    last) tail -n 1 "$T/published" | cut -d' ' -f1;;
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9])
      date -u -d "$1" +%F >/dev/null 2>&1 || { echo "BAD:not a date: $1"; return; }
      _d=$(echo "$1" | tr -d -); _s=$(awk -v d="${_d}0000" '$1 >= d { print $1; exit }' "$T/published")
      if [ -n "$_s" ]; then echo "$_s"; elif [ "$NPUB" = 0 ]; then echo; else echo "BAD:no update was published on $1 or later (newest: $(tail -n 1 "$T/published" | cut -d' ' -f1,2))"; fi;;
    *[!0-9]*) echo "BAD:'$1' is not a date (YYYY-MM-DD), an update number, last or all";;
    *) if grep -q "^$1 " "$T/published"; then echo "$1"; else echo "BAD:update $1 is not published on github.com/$REPO (tools/release.sh shows the list)"; fi;;
  esac
}
SINCE=$(resolve_start "$START")
case "$SINCE" in BAD:*) die "starting point START=$START in tools/release.conf: ${SINCE#BAD:}";; esac

if [ "$MODE" = start ]; then
  _s=$(resolve_start "$NEWSTART"); case "$_s" in BAD:*) die "${_s#BAD:}";; esac
  if grep -q '^START=' "$SHARED" 2>/dev/null; then sed -i "s/^START=.*/START=$NEWSTART/" "$SHARED"; else echo "START=$NEWSTART" >> "$SHARED"; fi
  echo "starting point: $NEWSTART${_s:+ (update $_s)}. Commit and push it, so every maintainer uses the same one:"
  echo "  git commit -m 'release: starting point $NEWSTART' tools/release.conf && git push"
  exit 0
fi

# ------------------------------------------------------------------------------------------------ the plan
_c() { git log -1 --format="$1" "${2:-HEAD}"; }
BUILD_ID=$(sed -n 's/^BUILD_ID=//p' lapis/BUILD); VERSION=$(sed -n 's/^VERSION=//p' lapis/BUILD)
say "release plan (made today, $(date -u +%F))"
echo "  update number:  $SEQ (today, UTC)"
echo "  contents:       $BRANCH at $(_c %h) \"$(_c %s)\" ($(_c %cs)), Google build $VERSION ($BUILD_ID)"
if [ "$NPUB" = 0 ]; then
  echo "  published:      none yet. This is the first update: laptops download the full bundle."
else
  _l=$(tail -n 1 "$T/published"); echo "  published:      $NPUB update(s); newest $(echo "$_l" | cut -d' ' -f1) ($(echo "$_l" | cut -d' ' -f2))"
  if [ -n "$SINCE" ]; then
    _n=$(awk -v s="$SINCE" '$1 >= s' "$T/published" | wc -l | tr -d ' ')
    echo "  starting point: ${START:-all} = update $SINCE ($(awk -v s="$SINCE" '$1 == s { print $2 }' "$T/published"))"
    echo "  small download: laptops on any of these $_n update(s): $(awk -v s="$SINCE" '$1 >= s { printf "%s ", $1 }' "$T/published")"
    echo "  full download:  laptops on an update before $SINCE, or on none (the app picks by itself)"
  fi
fi
# the published sets the delta is made against (publish_system_update.sh reads them from $OUTDIR/manifests)
mkdir -p "$OUTDIR/manifests"; OUTDIR=$(cd "$OUTDIR" && pwd)
if [ -n "$SINCE" ]; then
  awk -v s="$SINCE" '$1 >= s { print $1, $3 }' "$T/published" | while read -r _s _u; do
    [ -s "$OUTDIR/manifests/$_s.manifest" ] && continue
    case "$_u" in https://*) curl -fsSL --max-time 120 -o "$T/m" "$_u" && mv "$T/m" "$OUTDIR/manifests/$_s.manifest" \
      || die "cannot download the set of update $_s ($_u)";; *) cp "$_u" "$OUTDIR/manifests/$_s.manifest";; esac
  done
fi
if [ "$NPUB" != 0 ]; then
  _ls=$(tail -n 1 "$T/published" | cut -d' ' -f1)
  [ "$SEQ" -gt "$_ls" ] || die "update number $SEQ is not newer than the published $_ls"
  _lc=$(sed -n 's/^commit \([0-9a-f]\{40\}\)$/\1/p' "$OUTDIR/manifests/$_ls.manifest" 2>/dev/null || true)
  if [ -n "$_lc" ] && git cat-file -e "$_lc^{commit}" 2>/dev/null; then
    _nc=$(git rev-list --count "$_lc..HEAD" -- usb lapis)
    echo "  changes:        $_nc commit(s) touching usb/ or lapis/ since update $_ls ($(git log -1 --format=%h "$_lc"))"
    [ "$_nc" = 0 ] && echo "  NOTE: nothing for the laptops changed since update $_ls; a release now would only repeat it"
  fi
  _n=$(awk -v s="${SINCE:-0}" '$1 >= s' "$T/published" | wc -l | tr -d ' ')
  [ "$_n" -le 8 ] || echo "  NOTE: the small download covers $_n updates and grows with each; move the start forward: tools/release.sh start last"
fi
[ "$MODE" = plan ] && { echo; echo "next: tools/release.sh check   then   tools/release.sh publish   (tools/release.sh --help)"; exit 0; }

# ------------------------------------------------------------------------------------------------ 0. tools on this computer
say "0. tools"
_miss=
for _t in git curl openssl python3 tar sha256sum; do command -v "$_t" >/dev/null 2>&1 || _miss="$_miss $_t"; done
[ $SKIPT = 1 ] || command -v javac >/dev/null 2>&1 || [ -x "${JAVA_HOME:-/nonexistent}/bin/javac" ] || _miss="$_miss javac(JDK 17+)"
[ -z "$_miss" ] || die "missing on this computer:$_miss"
# the busybox the laptop's own checks run under (verify_system_update.sh, test_system_update.sh); found like they find it
if [ -z "${BUSYBOX:-}" ]; then
  _cd=$(git rev-parse --git-common-dir 2>/dev/null || true); _mc=$( [ -n "$_cd" ] && cd "$_cd/.." && pwd || true)
  for _b in "$ROOT/kernel/out/busybox" "${_mc:-/nonexistent}/kernel/out/busybox" "$(command -v busybox || true)"; do
    [ -n "$_b" ] && [ -x "$_b" ] && { BUSYBOX=$_b; break; }
  done
fi
[ -n "${BUSYBOX:-}" ] && [ -x "$BUSYBOX" ] || die "no busybox for the laptop-side checks: tools/fetch_busybox.sh, or BUSYBOX=/path/busybox"
export BUSYBOX
if [ "$MODE" = publish ] && [ $DRY = 0 ]; then
  command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1 || die "gh (the GitHub CLI) is missing or not logged in: gh auth login"
fi
ok "busybox $BUSYBOX${_miss}"

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
    || die "the signing key ($KEY) is not the key of usb/sysupdate.pub: laptops would refuse this update"
  ok "the private key matches usb/sysupdate.pub"
else ok "usb/sysupdate.pub holds a key (no --key given to compare)"; fi

# ------------------------------------------------------------------------------------------------ 3b. Updater app
# The release ships the tree's gbos86-updater.apk to every laptop. It must check this repository's feed (a lab or fork build
# would move laptops to another feed) and be built from this commit's updater-app/ (the app laptops get is the app in $BRANCH).
say "3b. Updater app"
_build="REPO=$REPO APK_OUT=${TREE:-<stick folder>}/gbos86-updater.apk updater-app/build.sh"
if [ -n "$TREE" ]; then
  _apk=$TREE/gbos86-updater.apk
  [ -f "$_apk" ] || die "$_apk is missing: build it ($_build)"
  _st=$(python3 -I tools/apk_stamp.py "$_apk") || die "$_apk has no build stamp (an older build): rebuild it ($_build)"
  _ar=$(echo "$_st" | sed -n 's/.*repo=\([^ ]*\).*/\1/p'); _at=$(echo "$_st" | sed -n 's/.*tree=\([^ ]*\).*/\1/p')
  _ac=$(echo "$_st" | sed -n 's/.*commit=\([^ ]*\).*/\1/p')
  [ "$_ar" = "$REPO" ] || die "$_apk checks github.com/$_ar for updates, not $REPO (a lab or fork build): rebuild it ($_build)"
  _wt=$(git rev-parse "HEAD:updater-app")
  [ "$_at" = "$_wt" ] || die "$_apk was built from other updater-app sources (tree $_at, commit $_ac), not $BRANCH's ($_wt): rebuild it ($_build)"
  ok "gbos86-updater.apk checks github.com/$REPO and was built from $BRANCH's updater-app (tree ${_wt%"${_wt#????????}"}, commit ${_ac%"${_ac#????????}"})"
else echo "  (no --tree: the app is not checked; publish will do it)"; fi

# ------------------------------------------------------------------------------------------------ 4. tests
say "4. tests"
if [ $SKIPT = 1 ]; then echo "  SKIPPED (--skip-tests)"
else
  sh -n usb/hook.sh usb/install.sh usb/stage0_init.sh usb/apex_patch.sh usb/sysupdate.sh || die "shell syntax"
  ok "sh -n"
  for t in system_update:SYSTEM_UPDATE_TEST_PASS updater_core:UPDATER_CORE_TEST_PASS release_app_check:RELEASE_APP_CHECK_TEST_PASS; do
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
_up=; [ $DRY = 1 ] && _up=--no-upload; _y=; [ $YES = 1 ] && _y=--yes
# shellcheck disable=SC2086
{ _rc=0; sh tools/publish_system_update.sh --key "$KEY" --tree "$TREE" --commit "$HEADC" --seq "$SEQ" --repo "$REPO" --out "$OUTDIR" \
  ${SINCE:+--since "$SINCE"} $( [ -n "$SINCE" ] || echo --bases 0 ) ${PARTS:+--parts "$PARTS"} $_up $_y "$@" || _rc=$?; echo $_rc > "$T/rc"; } 2>&1 | tee "$T/publish.log"
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
