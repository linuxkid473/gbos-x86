#!/bin/sh
# Publish a gbOS86 system update on the project's GitHub Releases, where the gbOS86 Updater app finds it (docs/system-update.md,
# "Updates from GitHub"). One command for the maintainer:
#
#   tools/publish_system_update.sh --key /media/offline/gbos86-key/sysupdate-private.pem --tree /path/to/stick/gbos
#
# 1. builds the signed set from a git commit (default HEAD, which must be pushed: the app shows whether the installed system is a
#    published commit), with the part git does not hold (firmware blobs, modules, ksu, super.img, super.layout) from --tree;
# 2. makes two bundles WITHOUT super.img (GitHub refuses files over 2 GiB, and Google's system image is not ours to hand out):
#      full   every other file of the set: for a laptop whose super.img is already this Google build;
#      delta  only what changed since the last --bases published updates: for a laptop that has one of them unmodified;
# 3. writes the feed, gbos86-update.feed (signed with the same key; the signature is its last line): what the app reads at startup to know there is an update,
#    where the bundles are, and, for a new Google build, how to rebuild super.img from Google's own recovery.zip (its URL, the size and
#    sha256 of each partition and of the result; tools/super_parts.py proves super.img can be rebuilt that way);
# 4. shows what it will upload and asks; then creates the release sysupdate-<seq> with the bundles, and replaces the feed in the
#    release sysupdate-feed (with gh, the GitHub CLI, logged in once with `gh auth login`).
#
# Options:
#   --commit REV        what to publish (default HEAD); the set is git archive of REV, so local edits never slip in
#   --kernel FILE       default: usb/out/BOOTX64.EFI of REV          --seq N   default: UTC yyyymmddHHMM
#   --bases N           how many earlier updates the delta covers (default 3; 0 = no delta)
#   --since SEQ         instead of --bases: the delta covers every published update from SEQ on (tools/release.sh's starting point)
#   --repo OWNER/NAME   default: the origin remote's GitHub repository
#   --parts DIR         Google's partitions of this build (default lapis/parts): proves super.img holds them unmodified
#   --out DIR           default build/releases (keeps each release's files, and manifests/ of the published sets)
#   --no-upload         make and check everything, upload nothing      --yes   upload without asking
#   --allow-unpushed    publish a commit the remote does not have (the app then calls the system "not in the project's sources")
#   --notes FILE        release notes (default: the commit subjects since the previous update)
#   tests: --src USBDIR --commit-id ID (a usb/-like folder instead of git), --bases-dir DIR, --google-url URL, --no-super-check
#
# The private key stays offline (docs/system-update.md, "The signing key"): plug the key stick in, run this, unplug it.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LC_ALL=C; export LC_ALL
die() { echo "publish_system_update: $*" >&2; exit 1; }
say() { echo "== $*"; }
sha() { sha256sum "$1" | cut -c1-64; }

KEY=; TREE=; REV=HEAD; KERNEL=; SEQ=; NBASES=3; REPO=; PARTS=; OUT=$ROOT/build/releases; UPLOAD=1; YES=0; UNPUSHED=0; NOTES=
SRCDIR=; COMMIT_ID=; BASESDIR=; GURL=; SUPERCHECK=; SINCE=
while [ $# -gt 0 ]; do
  case "$1" in
    --key) KEY=$2; shift 2;; --tree) TREE=$2; shift 2;; --commit) REV=$2; shift 2;; --kernel) KERNEL=$2; shift 2;;
    --seq) SEQ=$2; shift 2;; --bases) NBASES=$2; shift 2;; --since) SINCE=$2; shift 2;; --repo) REPO=$2; shift 2;; --parts) PARTS=$2; shift 2;;
    --out) OUT=$2; shift 2;; --no-upload) UPLOAD=0; shift;; --yes) YES=1; shift;; --allow-unpushed) UNPUSHED=1; shift;;
    --notes) NOTES=$2; shift 2;; --src) SRCDIR=$2; shift 2;; --commit-id) COMMIT_ID=$2; shift 2;; --bases-dir) BASESDIR=$2; shift 2;;
    --google-url) GURL=$2; shift 2;; --no-super-check) SUPERCHECK=--no-super-check; shift;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0;;
    *) die "unknown option $1 (--help)";;
  esac
done
[ -n "$KEY" ] && [ -f "$KEY" ] || die "needs --key PRIVATE.pem (the offline signing key)"
[ -n "$TREE" ] && [ -d "$TREE" ] || die "needs --tree GBOSDIR (a stick-like gbos/ folder: super.img, super.layout, firmware, modules ...)"
echo "$NBASES" | grep -qE '^[0-9]+$' || die "--bases must be a number"
[ -z "$SINCE" ] || echo "$SINCE" | grep -qE '^[0-9]{1,12}$' || die "--since must be an update number (seq)"
[ -n "$SEQ" ] || SEQ=$(date -u +%Y%m%d%H%M)
echo "$SEQ" | grep -qE '^[1-9][0-9]{0,11}$' || die "--seq must be a number (1 to 12 digits)"
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
[ -n "$BASESDIR" ] || BASESDIR=$OUT/manifests
mkdir -p "$BASESDIR"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM

# ------------------------------------------------------------------------------------------------ where, and what
if [ -z "$REPO" ]; then
  _u=$(git -C "$ROOT" remote get-url origin 2>/dev/null || true)
  REPO=$(echo "$_u" | sed -n 's#^\(https://github\.com/\|git@github\.com:\|ssh://git@github\.com/\)\([^/]*/[^/]*\)$#\2#p' | sed 's/\.git$//')
  [ -n "$REPO" ] || [ $UPLOAD = 0 ] || die "cannot tell the GitHub repository from the origin remote ($_u); pass --repo OWNER/NAME"
  [ -n "$REPO" ] || REPO=OWNER/NAME
fi
echo "$REPO" | grep -qE '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' || die "--repo must be OWNER/NAME"
TAG=sysupdate-$SEQ
URLBASE=https://github.com/$REPO/releases/download/$TAG/

if [ -n "$SRCDIR" ]; then
  SRCARGS="--src $SRCDIR --commit-id ${COMMIT_ID:-0000000000000000000000000000000000000000}"
  BUILDFILE=$SRCDIR/../lapis/BUILD; COMMIT=${COMMIT_ID:-0000000000000000000000000000000000000000}
else
  COMMIT=$(git -C "$ROOT" rev-parse -q --verify "$REV^{commit}") || die "no such commit: $REV"
  SRCARGS="--commit $COMMIT"
  git -C "$ROOT" show "$COMMIT:lapis/BUILD" > "$T/BUILD" 2>/dev/null || die "commit $COMMIT has no lapis/BUILD"
  BUILDFILE=$T/BUILD
  if [ $UNPUSHED = 0 ]; then
    git -C "$ROOT" fetch -q origin 2>/dev/null || echo "note: could not fetch origin; checking against the last fetched state" >&2
    [ -n "$(git -C "$ROOT" branch -r --contains "$COMMIT" 2>/dev/null)" ] \
      || die "commit $COMMIT is not on the remote: push it first (the app checks the installed system against the published sources), or pass --allow-unpushed"
  fi
  [ "$REV" = HEAD ] && [ -n "$(git -C "$ROOT" status --porcelain -- usb lapis/BUILD)" ] \
    && echo "note: you have uncommitted changes under usb/; they are NOT published (the set is commit $COMMIT exactly)" >&2
fi
GBUILD=$(sed -n 's/^BUILD_ID=//p' "$BUILDFILE" 2>/dev/null | head -n 1)
# the build's name (e.g. CL3B.260622.276.R5): the app shows "Googlebook OS R5" instead of a bare build number
GVERSION=$(sed -n 's/^VERSION=//p' "$BUILDFILE" 2>/dev/null | head -n 1 | grep -E '^[A-Z0-9][A-Z0-9.]{0,39}$' || true)
[ -n "$GURL" ] || GURL=$(sed -n 's/^URL=//p' "$BUILDFILE" 2>/dev/null | head -n 1)

# ------------------------------------------------------------------------------------------------ the bases (earlier published sets)
gh_ok() { command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; }
if [ $UPLOAD = 1 ]; then
  gh_ok || die "gh (the GitHub CLI) is missing or not logged in: install it and run 'gh auth login' (or use --no-upload)"
  say "earlier updates on github.com/$REPO"
  gh release list --repo "$REPO" --limit 1000 --json tagName,isDraft --jq '.[] | select(.isDraft | not) | .tagName' > "$T/tags" 2>/dev/null \
    || die "cannot list the releases of $REPO"
  grep -E '^sysupdate-[0-9]+$' "$T/tags" | sort -t- -k2 -n > "$T/tags.s"
  if [ -n "$SINCE" ]; then awk -F- -v s="$SINCE" '$2 >= s' "$T/tags.s"; else tail -n "$((NBASES + 1))" "$T/tags.s"; fi | while read -r _t; do
    _s=${_t#sysupdate-}
    [ -f "$BASESDIR/$_s.manifest" ] && continue
    rm -rf "$T/dl"; mkdir -p "$T/dl"
    gh release download "$_t" --repo "$REPO" --pattern '*.manifest' --dir "$T/dl" >/dev/null 2>&1 && mv "$T/dl"/*.manifest "$BASESDIR/$_s.manifest" \
      && echo "  fetched the set of $_t"
  done
fi
: > "$T/bases"
for _m in "$BASESDIR"/*.manifest; do
  [ -f "$_m" ] || continue
  _s=$(sed -n '2s/^seq \([0-9]*\)$/\1/p' "$_m"); [ -n "$_s" ] && echo "$_s $_m" >> "$T/bases"
done
sort -n "$T/bases" -o "$T/bases"
_last=$(tail -n 1 "$T/bases" | cut -d' ' -f1)
[ -z "$_last" ] || [ "$SEQ" -gt "$_last" ] || die "seq $SEQ is not newer than the last published update ($_last)"
if [ -n "$SINCE" ]; then awk -v s="$SINCE" '$1 >= s' "$T/bases" > "$T/use"
else tail -n "$NBASES" "$T/bases" > "$T/use"; [ "$NBASES" = 0 ] && : > "$T/use"; fi

# ------------------------------------------------------------------------------------------------ build
say "building the set (seq $SEQ, $( [ -n "$SRCDIR" ] && echo "source $SRCDIR" || echo "commit $COMMIT"))"
REL=$OUT/$SEQ
[ -e "$REL" ] && die "$REL exists (a release with this seq was already made here)"
mkdir -p "$REL/full"
# shellcheck disable=SC2086
if ! sh "$ROOT/tools/make_system_update.sh" build --key "$KEY" --tree "$TREE" $SRCARGS ${KERNEL:+--kernel "$KERNEL"} --seq "$SEQ" \
  --out "$REL/full" --omit super.img $SUPERCHECK > "$T/build.log"; then
  sed 's/^/  /' "$T/build.log"; rm -rf "$REL"; die "building the set failed (see above); nothing was published"
fi
sed 's/^/  /' "$T/build.log"
B=$(ls -d "$REL/full"/gbos86-update-* 2>/dev/null | head -n 1); [ -n "$B" ] || die "internal: no bundle in $REL/full"; NAME=$(basename "$B")
SETID=$(sha "$B/manifest")
GB=$(sed -n 's/^google_build //p' "$B/manifest")
[ -n "$SRCDIR" ] || [ "$GB" = "$GBUILD" ] || die "the set is Google build $GB but lapis/BUILD of the commit says $GBUILD"
COMMIT=$(sed -n 's/^commit //p' "$B/manifest")
case "$COMMIT" in *-dirty) die "the set's commit is $COMMIT: a published update must be exactly a commit's sources";; esac

# delta: what differs from any of the bases (so a laptop on any of them has every other file already)
DELTA=
if [ -s "$T/use" ]; then
  mkdir -p "$REL/delta/$NAME/files"; D=$REL/delta/$NAME
  cp "$B/manifest" "$B/manifest.sig" "$D/"
  awk '$1 == "f" { print $2, $3, $4 } $1 == "k" { print $2, $3, "EFI/BOOTX64.EFI" }' "$B/manifest" > "$T/want"
  # a file goes into the delta unless EVERY base has this exact version of it
  _nb=$(wc -l < "$T/use")
  cut -d' ' -f2 "$T/use" | while read -r _m; do awk '$1 == "f" { print $2, $3, $4 } $1 == "k" { print $2, $3, "EFI/BOOTX64.EFI" }' "$_m" | sort -u; done \
    | sort | uniq -c | awk -v n="$_nb" '$1 == n { print $2, $3, $4 }' > "$T/everywhere"
  awk 'NR == FNR { e[$0] = 1; next } !($0 in e) { print $3 }' "$T/everywhere" "$T/want" | while read -r _p; do
    [ "$_p" = super.img ] && continue
    [ -f "$B/files/$_p" ] || die "internal: $_p missing from the full bundle"
    mkdir -p "$D/files/$(dirname "$_p")"; cp "$B/files/$_p" "$D/files/$_p"
  done
  DELTA=$D
  awk '{ print $1 }' "$T/use" | tr '\n' ' ' > "$T/baseseqs"
fi

say "checking the bundles the way the laptop will"
PUBK=$T/key.pub; sh "$ROOT/tools/make_system_update.sh" pubkey "$KEY" > "$PUBK"
for _b in "$B" $DELTA; do
  sh "$ROOT/tools/verify_system_update.sh" "$_b" --pub "$PUBK" > "$T/v.log" 2>&1 || { cat "$T/v.log" >&2; die "$_b does not verify"; }
done
tar -C "$REL/full" --format=ustar --owner=0 --group=0 --numeric-owner -cf "$REL/$NAME-full.tar" "$NAME" || die "tar failed (paths too long for ustar?)"
[ -n "$DELTA" ] && tar -C "$REL/delta" --format=ustar --owner=0 --group=0 --numeric-owner -cf "$REL/$NAME-delta.tar" "$NAME"
cp "$B/manifest" "$REL/$NAME.manifest"
for _f in "$REL"/*.tar; do
  [ "$(stat -c %s "$_f")" -lt 2147483648 ] || die "$(basename "$_f") is 2 GiB or more: GitHub refuses it"
done

# ------------------------------------------------------------------------------------------------ Google's part (super.img)
say "super.img: can a laptop rebuild it from Google's download?"
SUPER=$TREE/super.img; LAYOUT=$TREE/super.layout
[ "$(sha "$SUPER")" = "$(awk '$1 == "f" && $4 == "super.img" { print $2 }' "$B/manifest")" ] || die "internal: super.img changed during the build"
[ -n "$PARTS" ] || { [ -d "$ROOT/lapis/parts" ] && PARTS=$ROOT/lapis/parts; } || true
[ -n "$PARTS" ] || { _c=$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null || true); [ -n "$_c" ] && _mc=$(cd "$ROOT" && cd "$_c/.." && pwd) && [ -d "$_mc/lapis/parts" ] && PARTS=$_mc/lapis/parts; } || true
GOOGLE=
if python3 -I "$ROOT/tools/super_parts.py" "$SUPER" "$LAYOUT" ${PARTS:+--parts "$PARTS"} > "$T/google" 2> "$T/google.err"; then
  case "$GURL" in https://dl.google.com/*) GOOGLE=1;; *) echo "  no Google URL for build $GB in lapis/BUILD: the app cannot fetch it directly" >&2;; esac
  [ -n "$PARTS" ] && echo "  yes: super.img is make_super.py's image of Google's partitions in $PARTS" \
    || echo "  probably: super.img is make_super.py's layout (no --parts to compare the partitions with; the laptop checks the result)"
else
  echo "  no: $(cat "$T/google.err") (the app will ask for the stick for this Google build)" >&2
fi

# ------------------------------------------------------------------------------------------------ the feed
say "feed"
{
  echo "gbos86-update-feed 1"
  echo "seq $SEQ"; echo "set $SETID"; echo "name $NAME"; echo "commit $COMMIT"; echo "google_build $GB"
  [ "$GB" = "$GBUILD" ] && [ -n "$GVERSION" ] && echo "google_version $GVERSION"
  echo "created $(sed -n 's/^created //p' "$B/manifest")"
  cut -d' ' -f2 "$T/use" | while read -r _m; do [ -n "$_m" ] && echo "base $(sha "$_m")"; done
  echo "url $URLBASE"
  echo "asset full $NAME-full.tar $(stat -c %s "$REL/$NAME-full.tar") $(sha "$REL/$NAME-full.tar")"
  [ -n "$DELTA" ] && echo "asset delta $NAME-delta.tar $(stat -c %s "$REL/$NAME-delta.tar") $(sha "$REL/$NAME-delta.tar")"
  echo "asset manifest $NAME.manifest $(stat -c %s "$REL/$NAME.manifest") $(sha "$REL/$NAME.manifest")"
  if [ -n "$GOOGLE" ]; then echo "google_zip $GURL"; cat "$T/google"; fi
  if [ -n "$NOTES" ]; then cat "$NOTES"
  elif [ -z "$SRCDIR" ]; then
    _pc=$(tail -n 1 "$T/bases" | cut -d' ' -f2); _pc=${_pc:+$(sed -n 's/^commit \([0-9a-f]\{40\}\)$/\1/p' "$_pc")}
    if [ -n "$_pc" ] && git -C "$ROOT" cat-file -e "$_pc^{commit}" 2>/dev/null; then git -C "$ROOT" log --format=%s "$_pc..$COMMIT" -- usb
    else git -C "$ROOT" log --format=%s -n 10 "$COMMIT" -- usb; fi
  fi | tr -cd '\40-\176\n' | cut -c1-200 | grep . | head -n 20 | sed 's/^/note /'
} > "$T/feed.body"
# one file, so replacing it on GitHub is never half done: the signed text (up to "end N"), then its signature as the last line
{ cat "$T/feed.body"; echo "end $(wc -l < "$T/feed.body")"; } > "$T/feed.signed"
openssl dgst -sha256 -sign "$KEY" -out "$T/fsig" "$T/feed.signed" || die "signing the feed failed"
openssl rsa -in "$KEY" -pubout -out "$T/k.pem" 2>/dev/null
openssl dgst -sha256 -verify "$T/k.pem" -signature "$T/fsig" "$T/feed.signed" >/dev/null || die "the feed signature does not verify"
{ cat "$T/feed.signed"; printf 'rsa3072-sha256 %s\n' "$(od -An -tx1 -v "$T/fsig" | tr -d ' \n')"; } > "$REL/gbos86-update.feed"
[ "$(tail -n 1 "$REL/gbos86-update.feed" | wc -c)" = 784 ] || die "the key is not RSA-3072"

echo
echo "release $TAG on github.com/$REPO:"
for _f in "$REL"/*.tar "$REL/$NAME.manifest" "$REL/gbos86-update.feed"; do
  printf '  %-60s %8s KB\n' "$(basename "$_f")" "$(( $(stat -c %s "$_f") / 1024 ))"
done
echo "  set $SETID, seq $SEQ, Google build $GB, commit $COMMIT"
[ -n "$DELTA" ] && echo "  delta covers seq $(cat "$T/baseseqs")" || echo "  no delta (no earlier update to build it from)"
[ -n "$GOOGLE" ] && echo "  new-Google-build laptops can rebuild super.img from $GURL (Settings > General > Google builds in the app; on by default)"
echo "  files: $REL"

if [ $UPLOAD = 0 ]; then echo "not uploaded (--no-upload)"; exit 0; fi
if [ $YES = 0 ]; then
  printf 'Upload this to github.com/%s (public)? Type yes: ' "$REPO"; read -r _a
  [ "$_a" = yes ] || { echo "not uploaded"; exit 0; }
fi
say "uploading"
_nf=$T/notes; { echo "gbOS86 system update $SEQ (Google build $GB, commit $COMMIT)."; echo
  echo "Installed by the gbOS86 Updater app on the laptop. The files are signed; the laptop checks them before installing."
  sed -n 's/^note /- /p' "$REL/gbos86-update.feed"; } > "$_nf"
# shellcheck disable=SC2046
gh release create "$TAG" --repo "$REPO" --title "gbOS86 system update $SEQ" --notes-file "$_nf" --latest=false \
  "$REL"/*.tar "$REL/$NAME.manifest" "$REL/gbos86-update.feed" || die "creating release $TAG failed"
# the feed moves only after the release it points to is complete
gh release view sysupdate-feed --repo "$REPO" >/dev/null 2>&1 \
  || gh release create sysupdate-feed --repo "$REPO" --title "gbOS86 update feed" --latest=false \
       --notes "Where the gbOS86 Updater app looks for system updates. Do not delete: the app reads gbos86-update.feed from here." \
  || die "creating the feed release failed"
gh release upload sysupdate-feed --repo "$REPO" --clobber "$REL/gbos86-update.feed" || die "uploading the feed failed"
cp "$REL/$NAME.manifest" "$BASESDIR/$SEQ.manifest"
echo "published: https://github.com/$REPO/releases/tag/$TAG (laptops see it at their next check)"
