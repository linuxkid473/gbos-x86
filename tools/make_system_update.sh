#!/bin/sh
# Make signed gbOS86 system-update bundles for usb/sysupdate.sh (docs/system-update.md).
#
#   make_system_update.sh keygen DIR [--no-passphrase]   new RSA-3072 key: DIR/sysupdate-private.pem (passphrase-protected unless
#                                                        --no-passphrase) and DIR/sysupdate.pub. DIR must be outside every git repo.
#   make_system_update.sh pubkey KEY.pem                 print the public key file for KEY (what usb/sysupdate.pub must hold)
#   make_system_update.sh build --key KEY.pem --tree GBOSDIR [source] [options]
#       source: --worktree (default: this checkout's tracked usb/ files), --commit REV (git archive of REV), --src USBDIR (a usb/-like
#               folder, every file counts as tracked; for tests)
#       --tree GBOSDIR   a stick-like gbos/ folder for what git does not hold (super.img, super.layout, firmware blobs, modules/, ksu/ ...)
#       --kernel FILE    default: usb/out/BOOTX64.EFI of the source
#       --base MANIFEST  make a delta: only files whose hash differs from MANIFEST (a system.manifest or a bundle's manifest) go in files/
#       --seq N          default: UTC yyyymmddHHMM;  --out DIR (default build/system-updates);  --tar (also write DIR/<name>.tar)
#       --pub FILE       the sysupdate.pub the new set carries (key rotation); default: the source's, or KEY's public key
#       --with-dir NAME  include an optional folder that is left out by default (camera: per-laptop calibration)
#       --commit-id ID   commit recorded with --src (default 40 zeros);  --no-super-check  skip the docker dump.erofs check
#       --omit PATH      listed in the manifest but left out of files/ (repeatable; publish_system_update.sh: --omit super.img)
#   make_system_update.sh manifest --tree GBOSDIR --kernel FILE [--seq N] [--out FILE]
#       an unsigned manifest of exactly what is in GBOSDIR (e.g. the stick, for gbos/system.manifest; menu 1 copies it to the laptop).
#       Its commit is this checkout's HEAD, with "-dirty" when a file of the tree that git tracks (usb/...) differs from HEAD.
#
# The set is install.sh's FILES and DIRS (read from the source's own install.sh), tracked files from git and the rest from --tree.
# The Google build comes from patched/MANIFEST's "# build" line; it must match lapis/BUILD and (with docker) super.img's build.prop.
# The private key never enters the repository. Keep it offline (docs/system-update.md, "The signing key").
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LC_ALL=C; export LC_ALL
die() { echo "make_system_update: $*" >&2; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM

in_git() { git -C "$1" rev-parse --show-toplevel >/dev/null 2>&1; }
pub_of_key() { # KEY.pem -> public key file text
  _m=$(openssl rsa -in "$1" -noout -modulus) || die "cannot read $1"
  _m=${_m#Modulus=}
  [ ${#_m} = 768 ] || die "$1 is not an RSA-3072 key"
  printf 'gbos86-sysupdate-key 1\nrsa3072 %s\n' "$_m"
}
pub_has_key() { [ "$(sed -n 1p "$1" 2>/dev/null)" = "gbos86-sysupdate-key 1" ] && sed -n 2p "$1" | grep -qE '^rsa3072 [0-9A-F]{768}$'; }
sha() { sha256sum "$1" | cut -c1-64; }
fp_build() { echo "$1" | sed -n 's#^[^/]*/[^/]*/[^/]*/[^/]*/\([0-9][0-9]*\):.*#\1#p'; }

cmd_keygen() {
  _d=${1:-}; [ -n "$_d" ] || die "usage: keygen DIR [--no-passphrase]"
  mkdir -p "$_d"; _d=$(cd "$_d" && pwd)
  in_git "$_d" && die "refusing: $_d is inside a git work tree. Keep the private key outside every repository (an offline USB stick)."
  [ -e "$_d/sysupdate-private.pem" ] && die "$_d/sysupdate-private.pem exists; not overwriting it"
  if [ "${2:-}" = --no-passphrase ]; then
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$_d/sysupdate-private.pem" 2>/dev/null
  else
    echo "choose a passphrase for the private key (asked twice):"
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -aes-256-cbc -out "$_d/sysupdate-private.pem"
  fi
  chmod 600 "$_d/sysupdate-private.pem"
  pub_of_key "$_d/sysupdate-private.pem" > "$_d/sysupdate.pub"
  echo "private key: $_d/sysupdate-private.pem  (keep it offline, with a backup; never commit it)"
  echo "public key:  $_d/sysupdate.pub  (copy it to usb/sysupdate.pub and commit it: it is public)"
}

# ---------------------------------------------------------------------------------------------------------------- the set
# compose: fills $T/list ("path<TAB>source file") and $T/kernel_src from FILES/DIRS, the tracked source and the tree.
compose() {
  : > "$T/tracked"
  case "$MODE" in
    commit) git -C "$ROOT" rev-parse -q --verify "$REV^{commit}" >/dev/null || die "no such commit: $REV"
            mkdir -p "$T/src"; git -C "$ROOT" archive "$REV" usb lapis/BUILD | tar -x -C "$T/src"
            SRC=$T/src/usb; BUILDFILE=$T/src/lapis/BUILD
            git -C "$ROOT" ls-tree -r --name-only "$REV" -- usb | sed 's#^usb/##' > "$T/tracked"
            COMMIT=$(git -C "$ROOT" rev-parse "$REV^{commit}");;
    worktree) SRC=$ROOT/usb; BUILDFILE=$ROOT/lapis/BUILD
            git -C "$ROOT" ls-files usb | sed 's#^usb/##' > "$T/tracked"
            COMMIT=$(git -C "$ROOT" rev-parse HEAD)
            [ -z "$(git -C "$ROOT" status --porcelain -- usb)" ] || COMMIT=$COMMIT-dirty;;
    src)    SRC=$SRCDIR; BUILDFILE=$SRCDIR/../lapis/BUILD
            (cd "$SRC" && find . -type f | sed 's#^\./##') > "$T/tracked"
            COMMIT=${COMMIT_ID:-0000000000000000000000000000000000000000};;
    tree)   SRC=$TREE; BUILDFILE=
            COMMIT=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo 0000000000000000000000000000000000000000);;   # tree_dirty adds -dirty
  esac
  [ -f "$SRC/install.sh" ] || die "no install.sh in $SRC"
  # install.sh's FILES/DIRS lines, evaluated against the tree and the source (optional entries present in either count)
  sed -n '/^FILES="/,/^for d in sensors camera thermal/p' "$SRC/install.sh" > "$T/sets.sh"
  grep -q '^FILES="' "$T/sets.sh" && grep -q '^DIRS="' "$T/sets.sh" || die "cannot find FILES/DIRS in $SRC/install.sh"
  SETDIRS=
  for _g in "$TREE" "$SRC"; do SETDIRS="$SETDIRS $(G=$_g; FILES=; DIRS=; . "$T/sets.sh"; echo "$DIRS")"; done
  SETFILES=
  for _g in "$TREE" "$SRC"; do SETFILES="$SETFILES $(G=$_g; FILES=; DIRS=; . "$T/sets.sh"; echo "$FILES")"; done
  SETFILES=$(echo $SETFILES | tr ' ' '\n' | sort -u)
  SETDIRS=$(echo $SETDIRS | tr ' ' '\n' | sort -u | while read -r _d; do
    case " $WITH_DIRS " in *" $_d "*) echo "$_d";; *) [ "$_d" = camera ] || echo "$_d";; esac; done)
  : > "$T/list"
  src_of() { # path -> source file (tracked source first, then the tree)
    if [ "$MODE" != tree ] && grep -qxF "$1" "$T/tracked"; then echo "$SRC/$1"; elif [ -f "$TREE/$1" ]; then echo "$TREE/$1"; fi
  }
  for _f in $SETFILES; do
    _s=$(src_of "$_f")
    [ "$_f" = sysupdate.pub ] && [ -n "$PUB_FILE" ] && _s=$PUB_FILE
    [ -n "$_s" ] || die "the set needs $_f: not in the source and not in $TREE"
    printf '%s\t%s\n' "$_f" "$_s" >> "$T/list"
  done
  for _d in $SETDIRS; do
    { [ "$MODE" = tree ] || grep "^$_d/" "$T/tracked" || :; [ -d "$TREE/$_d" ] && (cd "$TREE" && find "$_d" -type f) || :; } \
      | grep -v -e '/\.DS_Store$' -e '/desktop\.ini$' | sort -u | while read -r _p; do printf '%s\t%s\n' "$_p" "$(src_of "$_p")"; done >> "$T/list"
  done
  sort -t "$(printf '\t')" -k1,1 -u "$T/list" -o "$T/list"
  # sysupdate.pub: it must hold a key, or every later update would be refused
  _ps=$(awk -F '\t' '$1 == "sysupdate.pub" { print $2 }' "$T/list")
  if [ "$MODE" != tree ] && ! pub_has_key "$_ps"; then
    [ -n "$KEY" ] || die "the source's sysupdate.pub has no key; pass --pub"
    pub_of_key "$KEY" > "$T/sysupdate.pub"
    echo "note: the source's sysupdate.pub is the placeholder; the set carries the signing key's public key (commit it to usb/sysupdate.pub)" >&2
    awk -F '\t' -v n="$T/sysupdate.pub" 'BEGIN { OFS = "\t" } $1 == "sysupdate.pub" { $2 = n } { print }' "$T/list" > "$T/list.n" && mv "$T/list.n" "$T/list"
  fi
  KSRC=${KERNEL:-$SRC/out/BOOTX64.EFI}
  [ -f "$KSRC" ] || die "no kernel: $KSRC (pass --kernel)"
  # the Google build
  _pm=$(awk -F '\t' '$1 == "patched/MANIFEST" { print $2 }' "$T/list")
  [ -n "$_pm" ] || die "the set has no patched/MANIFEST"
  GFP=$(sed -n 's/^# build //p' "$_pm" | head -n 1); GB=$(fp_build "$GFP")
  [ -n "$GB" ] || die "patched/MANIFEST has no usable '# build' line"
  if [ -n "$BUILDFILE" ] && [ -f "$BUILDFILE" ]; then
    _lb=$(sed -n 's/^BUILD_ID=//p' "$BUILDFILE")
    [ "$_lb" = "$GB" ] || die "patched/MANIFEST is for Google build $GB but lapis/BUILD says $_lb (run tools/port_build.sh --write)"
  fi
}

# tree_dirty: in tree mode, does any listed file (or the kernel) that git tracks under usb/ differ from HEAD's copy? Then the commit
# is recorded as "<HEAD>-dirty", so the laptop (and the updater app) know the set is not exactly a commit's sources.
tree_dirty() {
  git -C "$ROOT" rev-parse -q --verify HEAD >/dev/null 2>&1 || return 1
  git -C "$ROOT" ls-tree -r HEAD -- usb | awk -F '\t' '{ split($1, a, " "); print substr($2, 5) "\t" a[3] }' | sort > "$T/headblobs"
  { cat "$T/list"; printf 'out/BOOTX64.EFI\t%s\n' "$KSRC"; } | sort -t "$(printf '\t')" -k1,1 \
    | join -t "$(printf '\t')" - "$T/headblobs" > "$T/tracked.cmp"
  [ -s "$T/tracked.cmp" ] || return 1
  cut -f2 "$T/tracked.cmp" | git -C "$ROOT" hash-object --stdin-paths > "$T/tracked.now"
  cut -f3 "$T/tracked.cmp" | cmp -s - "$T/tracked.now" && return 1
  return 0
}

super_check() { # the set's super.img must be that Google build (dump.erofs in the gbos-erofs image)
  [ "$SUPER_CHECK" = 1 ] || { echo "note: super.img build not checked (--no-super-check); the laptop checks it anyway" >&2; return 0; }
  if ! docker image inspect gbos-erofs >/dev/null 2>&1; then
    echo "note: no docker image gbos-erofs, super.img build not checked here (the laptop checks it before installing)" >&2; return 0
  fi
  _si=$(awk -F '\t' '$1 == "super.img" { print $2 }' "$T/list"); _sl=$(awk -F '\t' '$1 == "super.layout" { print $2 }' "$T/list")
  _st=$(awk '$1 == "system_a" { print $2; exit }' "$_sl")
  _d=$(cd "$(dirname "$_si")" && pwd)
  _z=; [ "$(getenforce 2>/dev/null)" = Enforcing ] && _z=,z
  _fp=$(docker run --rm -v "$_d:/w:ro$_z" gbos-erofs dump.erofs --offset=$((_st * 512)) --path=/system/build.prop --cat "/w/$(basename "$_si")" 2>/dev/null \
    | sed -n 's/^ro\.system\.build\.fingerprint=//p' | head -n 1)
  [ "$_fp" = "$GFP" ] || die "super.img's fingerprint is '${_fp:-unreadable}', patched/MANIFEST says '$GFP': they are not the same Google build"
  echo "super.img: Google build $GB (checked with dump.erofs)" >&2
}

write_manifest() { # OUT
  {
    echo "gbos86-system-set 1"
    echo "seq $SEQ"; echo "commit $COMMIT"; echo "google_build $GB"; echo "google_fingerprint $GFP"
    echo "created $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    cut -f1 "$T/list" | awk -F/ '{ p = ""; for (i = 1; i < NF; i++) { p = (p == "" ? $i : p "/" $i); print p } }' | sort -u | sed 's/^/d /'
    while IFS="$(printf '\t')" read -r _p _s; do echo "f $(sha "$_s") $(stat -c %s "$_s") $_p"; done < "$T/list"
    echo "k $(sha "$KSRC") $(stat -c %s "$KSRC") EFI/BOOTX64.EFI"
  } > "$T/m.body"
  _n=$(( $(wc -l < "$T/m.body") - 6 ))
  { cat "$T/m.body"; echo "end $_n"; } > "$1"
}

busybox_path() {
  [ -n "${BUSYBOX:-}" ] && { echo "$BUSYBOX"; return; }
  _c=$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null) && [ -x "$(cd "$ROOT" && cd "$_c/.." && pwd)/kernel/out/busybox" ] \
    && { echo "$(cd "$ROOT" && cd "$_c/.." && pwd)/kernel/out/busybox"; return; }
  [ -x "$ROOT/kernel/out/busybox" ] && { echo "$ROOT/kernel/out/busybox"; return; }
  command -v busybox || true
}

cmd_build() {
  MODE=worktree; REV=; SRCDIR=; TREE=; KERNEL=; KEY=; BASE=; SEQ=; OUT=$ROOT/build/system-updates; TAR=0; PUB_FILE=; WITH_DIRS=; COMMIT_ID=; SUPER_CHECK=1
  OMIT=
  while [ $# -gt 0 ]; do
    case "$1" in
      --worktree) MODE=worktree; shift;; --commit) MODE=commit; REV=$2; shift 2;; --src) MODE=src; SRCDIR=$(cd "$2" && pwd); shift 2;;
      --tree) TREE=$(cd "$2" && pwd); shift 2;; --kernel) KERNEL=$2; shift 2;; --key) KEY=$2; shift 2;; --base) BASE=$2; shift 2;;
      --seq) SEQ=$2; shift 2;; --out) OUT=$2; shift 2;; --tar) TAR=1; shift;; --pub) PUB_FILE=$2; shift 2;;
      --with-dir) WITH_DIRS="$WITH_DIRS $2"; shift 2;; --commit-id) COMMIT_ID=$2; shift 2;; --no-super-check) SUPER_CHECK=0; shift;;
      --omit) OMIT="$OMIT $2"; shift 2;;
      *) die "build: unknown option $1";;
    esac
  done
  [ -n "$KEY" ] && [ -f "$KEY" ] || die "build needs --key PRIVATE.pem"
  [ -n "$TREE" ] || die "build needs --tree GBOSDIR (a stick-like gbos/ folder with super.img, firmware, modules ...)"
  [ -n "$PUB_FILE" ] && { pub_has_key "$PUB_FILE" || die "--pub $PUB_FILE holds no key"; }
  [ -n "$SEQ" ] || SEQ=$(date -u +%Y%m%d%H%M)
  echo "$SEQ" | grep -qE '^[1-9][0-9]{0,11}$' || die "--seq must be a number (1 to 12 digits)"
  compose
  super_check
  if [ -n "$BASE" ]; then
    [ -f "$BASE" ] || die "no such base manifest: $BASE"
    _bs=$(sed -n 's/^seq //p' "$BASE" | head -n 1)
    [ "$SEQ" -gt "${_bs:-0}" ] || die "--seq $SEQ is not newer than the base's seq $_bs"
  fi
  write_manifest "$T/manifest"
  ID=$(sha "$T/manifest")
  NAME=gbos86-update-$(printf '%012d' "$SEQ")-$(echo "$ID" | cut -c1-12)
  B=$OUT/$NAME
  [ -e "$B" ] && die "$B exists"
  mkdir -p "$B/files"
  cp "$T/manifest" "$B/manifest"
  openssl dgst -sha256 -sign "$KEY" -out "$T/sig.bin" "$B/manifest" || die "signing failed"
  printf 'rsa3072-sha256 %s\n' "$(od -An -tx1 -v "$T/sig.bin" | tr -d ' \n')" > "$B/manifest.sig"
  [ "$(stat -c %s "$B/manifest.sig")" = 784 ] || die "the key is not RSA-3072"
  # files/: everything (full) or what differs from the base (delta)
  : > "$T/base.f"; _bk=
  if [ -n "$BASE" ]; then awk '$1 == "f" { print $2, $3, $4 }' "$BASE" > "$T/base.f"; _bk=$(awk '$1 == "k" { print $2 }' "$BASE"); fi
  _n=0; _bytes=0
  for _o in $OMIT; do grep -q "^f [0-9a-f]* [0-9]* $_o\$" "$B/manifest" || die "--omit $_o: not a file of the set"; done
  awk '$1 == "f" { print $2, $3, $4 }' "$B/manifest" | while read -r _h _s _p; do
    grep -qxF "$_h $_s $_p" "$T/base.f" && continue
    case " $OMIT " in *" $_p "*) continue;; esac
    _src=$(awk -F '\t' -v p="$_p" '$1 == p { print $2 }' "$T/list")
    mkdir -p "$B/files/$(dirname "$_p")"; cp "$_src" "$B/files/$_p"
  done
  if [ "$(sha "$KSRC")" != "$_bk" ]; then mkdir -p "$B/files/EFI"; cp "$KSRC" "$B/files/EFI/BOOTX64.EFI"; fi
  find "$B/files" -depth -type d -empty -delete 2>/dev/null || true; mkdir -p "$B/files"
  _n=$(find "$B/files" -type f | wc -l); _bytes=$(du -sk "$B/files" | cut -f1)
  echo "bundle: $B"
  echo "  set $ID, seq $SEQ, Google build $GB, commit $COMMIT"
  echo "  $(grep -c '^f ' "$B/manifest") files in the set, $_n in the bundle ($((_bytes / 1024)) MB)$([ -n "$BASE" ] && echo ", delta from seq $_bs")$([ -n "$OMIT" ] && echo ", left out:$OMIT")"
  # check it the way the laptop will, and the signature again with openssl
  _pub=$T/check.pub
  awk -F '\t' '$1 == "sysupdate.pub" { print $2 }' "$T/list" | xargs cat > "$T/set.pub"
  pub_of_key "$KEY" > "$_pub"
  BUSYBOX=$(busybox_path) sh "$ROOT/tools/verify_system_update.sh" "$B" --pub "$_pub" >/dev/null || die "the new bundle does not verify (tools/verify_system_update.sh)"
  echo "  verified (device code under busybox, and openssl)"
  cmp -s "$T/set.pub" "$_pub" || echo "  note: the set carries a different sysupdate.pub than the signing key (key rotation): later bundles must be signed with the new key"
  if [ $TAR = 1 ]; then
    tar -C "$OUT" --owner=0 --group=0 --numeric-owner -cf "$OUT/$NAME.tar" "$NAME"
    echo "  tar: $OUT/$NAME.tar"
  fi
}

cmd_manifest() {
  MODE=tree; TREE=; KERNEL=; KEY=; SEQ=; OUTF=; PUB_FILE=; WITH_DIRS=camera; SRC=
  while [ $# -gt 0 ]; do
    case "$1" in --tree) TREE=$(cd "$2" && pwd); shift 2;; --kernel) KERNEL=$2; shift 2;; --seq) SEQ=$2; shift 2;; --out) OUTF=$2; shift 2;;
      *) die "manifest: unknown option $1";; esac
  done
  [ -n "$TREE" ] && [ -n "$KERNEL" ] || die "usage: manifest --tree GBOSDIR --kernel BOOTX64.EFI [--seq N] [--out FILE]"
  [ -n "$SEQ" ] || SEQ=$(date -u +%Y%m%d%H%M)
  compose
  case "$COMMIT" in 0000000000000000000000000000000000000000) ;; *) if tree_dirty; then COMMIT=$COMMIT-dirty; echo "note: the tree differs from HEAD's tracked files: commit recorded as $COMMIT" >&2; fi;; esac
  write_manifest "$T/manifest"
  if [ -n "$OUTF" ]; then cp "$T/manifest" "$OUTF"; echo "wrote $OUTF (set $(sha "$T/manifest"))" >&2; else cat "$T/manifest"; fi
}

cmd=${1:-}; [ $# -gt 0 ] && shift
case "$cmd" in
  keygen) cmd_keygen "$@";;
  pubkey) [ -n "${1:-}" ] || die "usage: pubkey KEY.pem"; pub_of_key "$1";;
  build) cmd_build "$@";;
  manifest) cmd_manifest "$@";;
  *) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 2;;
esac
