#!/bin/sh
# gbOS86 system updater: installs a signed "system set" on the installed system (GBOSSYS) without the USB stick.
# Design and user guide: docs/system-update.md. Tests: tools/test_system_update.sh. Bundles: tools/make_system_update.sh.
#
# Trust: the bundle's manifest is signed (RSA-3072, PKCS#1 v1.5, SHA-256) and the check runs here in plain busybox (bc).
# Android never activates anything. Copying a bundle into gbos/update.inbox/ (from Windows, with "stage" below, or with the
# gbOS86 Updater app) only puts files there. The next boot's stage0 checks everything again before Android runs, switches the files
# over with renames (a journal makes a power cut safe), counts boots and rolls back after SYSUPDATE_ATTEMPTS failed ones.
#
#   boot G LOGDIR BOOTN [--efi-dir DIR]    stage0 (early_patch.sh / stage0_init.sh): finish/undo a pending update, else activate the inbox.
#                                          Exit 0 = continue booting, 10 = reboot now (the kernel changed).
#   verify BUNDLE [--pub FILE] [--installed G]   signature, manifest, bundle contents, hashes (and the final set against G). PC and device.
#   stage SRC [--gbos G] [--prune-logs] [--status FILE]   Android, as root: copy a bundle folder (or .tar) into update.inbox (listed files only).
#   cancel [--gbos G]                      remove bundles waiting in update.inbox (nothing installed is touched)
#   status [--gbos G]                      installed set, pending update, last result
#   request                                init service for the app: one request from the app's private folder (docs/system-update.md)
#   boot-ok [G]                            init service a minute after boot_completed while an update is being tried: commits it (one restart)
#   install-app [G] [BOOTN]                init service at boot_completed: install gbos86-updater.apk with pm when it changed, then
#                                          tell the app the laptop started (its update check)
#   audit [G] [--deep]                     does the installed system match its system.manifest? (the app's modification check)
#   cleanup [G]                            Android, root (the app's request): delete the folder the installed update was staged from
#   info [G]                               machine-readable set/key/space facts for the app
#   setinfo DIR                            id/seq/google_build/commit of the set in a gbos folder (install.sh)
#   menu1-check STICKG NVMEG / menu1-finish STICKG NVMEG    the stick's menu 1 (install.sh)
#
# Never sourced: it runs as its own process, so it may exit, and nothing leaks into stage0.

SU_PKG=org.gbos86.updater
SU_PI=${SU_PI:-/postinstall}     # Android's view of the volume (tests: another path)
SU_RW_DIR=${SU_RW_DIR:-/dev/gbos-rw}   # stage0's root-only writable view of it (hook.sh gbos_root_rw)
SU_SMALL=1048576                 # files up to this size are backed up by copying, so they never go missing during the switch
SU_MARGIN_KB=65536               # free space kept on GBOSSYS for logs when staging
SU_KEEP_LOG_BOOTS=3              # --prune-logs never deletes the newest boots' logs
SU_CHUNK_MB=32                   # stage writes big files in chunks of this size, each flushed before the next (see su_copy_checked)
SU_CHECKED=staged.checked        # stage's record of the big files it hashed after copying them (see su_skip_checked)
SU_CHECKED_MIN=$SU_SMALL         # ... files over this size
if [ -n "$SU_TEST" ]; then SU_CHUNK_MB=${SYSUPDATE_TEST_CHUNK_MB:-$SU_CHUNK_MB}; SU_CHECKED_MIN=${SYSUPDATE_TEST_CHECKED_MIN:-$SU_CHECKED_MIN}; fi
SU_TEST=${SYSUPDATE_TEST:-}      # tests only (stage0 runs this with env -i, so it is never set there)
SU_KMSG=${SYSUPDATE_KMSG:-/dev/kmsg}
SU_TTY=${SYSUPDATE_TTY:-/dev/tty1}
SU_ERR=
SU_REBOOT=0
LC_ALL=C; export LC_ALL

su_say() { echo "sysupdate: $*" >&2; }
su_log() { echo "sysupdate: $*" >&2; [ -w "$SU_KMSG" ] && echo "<6>gbos-sysupdate: $*" > "$SU_KMSG" 2>/dev/null; return 0; }
su_tty() { su_log "$*"; [ -n "$SU_TEST" ] || { [ -c "$SU_TTY" ] && echo "  gbOS86 update: $*" > "$SU_TTY" 2>/dev/null; }; return 0; }
su_die() { su_say "ERROR: $*"; su_cleanup_tmp; exit 1; }

# ---------------------------------------------------------------------------------------------------------------- environment
su_tmp() {
  [ -n "${W:-}" ] && return 0
  _tb=${TMPDIR:-/tmp}; [ -d "$_tb" ] && [ -w "$_tb" ] || _tb=/data/local/tmp
  [ -d "$_tb" ] || { _tb=/tmp; mkdir -p "$_tb"; }
  W=$(mktemp -d "$_tb/sysupdate.XXXXXX") || { echo "sysupdate: cannot make a temporary folder" >&2; exit 1; }
  # Inside Android /system/bin has toybox applets (no bc, a different awk): use the stick's busybox for every applet.
  [ -z "${SU_BB:-}" ] && [ -x /postinstall/busybox ] && [ -d /system/bin ] && [ ! -d /new ] && SU_BB=/postinstall/busybox
  if [ -n "${SU_BB:-}" ]; then
    mkdir -p "$W/bin"
    for _a in awk bc cat chmod chown cmp cp cut date df dd du find grep head id ls mkdir mktemp mv od readlink realpath rm rmdir \
      sed sh sha256sum sleep sort stat sync tac tail tar tr uniq wc xxd basename dirname tee touch expr; do ln -s "$SU_BB" "$W/bin/$_a"; done
    PATH=$W/bin:$PATH; export PATH
  fi
}
su_cleanup_tmp() { [ -n "${W:-}" ] && rm -rf "$W"; W=; }
trap 'su_cleanup_tmp' EXIT

su_size() { stat -c %s "$1" 2>/dev/null; }
su_sha() { sha256sum "$1" 2>/dev/null | cut -c1-64; }
su_free_kb() { # DIR
  if [ -n "$SU_TEST" ] && [ -n "${SYSUPDATE_TEST_FREE_KB:-}" ]; then echo "$SYSUPDATE_TEST_FREE_KB"; return; fi
  df -k "$1" 2>/dev/null | awk 'NR==2{print $4}'
}
su_kb() { echo $(( ($1 + 1023) / 1024 )); }
su_dropcache() { sync; [ -n "$SU_TEST" ] || echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; return 0; }
su_dropclean() { [ -n "$SU_TEST" ] || echo 1 > /proc/sys/vm/drop_caches 2>/dev/null; return 0; }   # clean page cache only (no sync)
su_tailsha() { tail -c 1048576 "$1" 2>/dev/null | sha256sum | cut -c1-64; }   # the last MB: cheap, and what a cut-short copy loses first
su_crashpoint() { # tests: simulate a power cut after N journal steps
  [ -n "$SU_TEST" ] && [ -n "${SYSUPDATE_TEST_CRASH_AFTER:-}" ] || return 0
  SU_STEPS=$(( ${SU_STEPS:-0} + 1 ))
  [ "$SU_STEPS" -ge "$SYSUPDATE_TEST_CRASH_AFTER" ] && { echo "sysupdate: TEST power cut after $SU_STEPS steps" >&2; exit 99; }
  return 0
}

# ---------------------------------------------------------------------------------------------------------------- signature
# gbos/sysupdate.pub: "gbos86-sysupdate-key 1" then "rsa3072 <modulus, 768 hex digits>" (e = 65537). The repo ships one with no key.
su_pub_modulus() {
  [ -f "$1" ] && [ "$(head -n 1 "$1")" = "gbos86-sysupdate-key 1" ] || return 1
  _n=$(sed -n '2s/^rsa3072 \([0-9A-Fa-f]\{768\}\)$/\1/p' "$1" | tr a-f A-F)
  case "$_n" in [89A-F]*) echo "$_n";; *) return 1;; esac
}
# su_verify_sig MANIFEST SIGFILE PUBFILE: RSA PKCS#1 v1.5 SHA-256. Rebuilds the whole expected encoded block and compares it.
su_verify_sig() {
  _n=$(su_pub_modulus "$3") || { SU_ERR="no signing key installed (gbos/sysupdate.pub has none): every update is refused"; return 1; }
  [ "$(su_size "$2")" = 784 ] || { SU_ERR="manifest.sig has the wrong size"; return 1; }
  _s=$(sed -n '1s/^rsa3072-sha256 \([0-9a-fA-F]\{768\}\)$/\1/p' "$2" | tr a-f A-F)
  [ -n "$_s" ] || { SU_ERR="manifest.sig is not an rsa3072-sha256 signature"; return 1; }
  _h=$(sha256sum < "$1" | cut -c1-64 | tr a-f A-F)
  _m=$(printf 'obase=16\nibase=16\nn=%s\ns=%s\nibase=A\nif (s >= n) { -1 } else { r=1; b=s; e=65537; while (e > 0) { if (e %% 2 == 1) { r=(r*b) %% n }; b=(b*b) %% n; e=e/2 }; r }\n' "$_n" "$_s" \
    | BC_LINE_LENGTH=0 bc 2>/dev/null | tr -d '\\\n')
  case "$_m" in ''|-*) SU_ERR="bad signature (not smaller than the key)"; return 1;; esac
  _m=$(echo "$_m" | awk '{ s = $0; while (length(s) < 768) s = "0" s; print s }')
  _e=$(awk -v h="$_h" 'BEGIN { s = "0001"; for (i = 0; i < 330; i++) s = s "FF"; print s "00" "3031300D060960864801650304020105000420" h }')
  [ "$_m" = "$_e" ] || { SU_ERR="bad signature: the manifest was not signed by the key in gbos/sysupdate.pub, or it was changed"; return 1; }
  return 0
}

# ---------------------------------------------------------------------------------------------------------------- manifest
# su_parse_manifest MANIFEST OUTDIR: strict check; writes OUTDIR/{seq,commit,google_build,google_fingerprint,created,d,f,k,top}.
# Paths: [A-Za-z0-9._+,@=~-/], no "."/".." or empty parts, no part ending in "." (FAT drops it), no Windows device names,
# no duplicates even ignoring case (FAT), parents declared by "d" lines, d then f lines each sorted (C order), one k line, "end <count>".
SU_AWK_MANIFEST='
function bad(m) { if (!err) printf "manifest line %d: %s\n", NR, m; err = 1; exit 1 }
function pathok(p,   n, a, i, c, u) {
  if (length(p) > 512) return "path too long"
  if (p !~ /^[A-Za-z0-9._+,@=~\/-]+$/) return "character not allowed"
  n = split(p, a, "/")
  for (i = 1; i <= n; i++) {
    c = a[i]
    if (c == "") return "empty path part"
    if (c == "." || c == "..") return "\".\" or \"..\" in the path"
    if (length(c) > 255) return "name too long"
    if (c ~ /\.$/) return "name ends with a dot"
    u = toupper(c); sub(/\..*$/, "", u)
    if (u ~ /^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$/) return "reserved Windows name"
  }
  u = tolower(a[1])
  if (u == "gbos.cfg" || u == "logs" || u ~ /^update/ || u == ".stamps" || u == "metadata.img" || u == "busybox" || u == "system.manifest" || u == "efi" || u == "lost.dir" || u == "system volume information") return "protected path"
  return ""
}
function parent(p,   q) { q = p; if (sub(/\/[^\/]*$/, "", q)) return q; return "" }
NR == 1 { if ($0 != "gbos86-system-set 1") bad("not a gbOS86 system set"); next }
NR == 2 { if ($0 !~ /^seq [1-9][0-9]{0,11}$/) bad("bad seq line"); print substr($0, 5) > (O "/seq"); next }
NR == 3 { if ($0 !~ /^commit [0-9a-f]{40}(-dirty)?$/) bad("bad commit line"); print substr($0, 8) > (O "/commit"); next }
NR == 4 { if ($0 !~ /^google_build [1-9][0-9]{0,11}$/) bad("bad google_build line"); print substr($0, 14) > (O "/google_build"); next }
NR == 5 { if ($0 !~ /^google_fingerprint [A-Za-z0-9._:\/+,-]{1,200}$/) bad("bad google_fingerprint line"); print substr($0, 20) > (O "/google_fingerprint"); next }
NR == 6 { if ($0 !~ /^created [0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$/) bad("bad created line"); print substr($0, 9) > (O "/created"); next }
done { bad("text after the end line") }
/^d / {
  if (st > 0) bad("d line after the f lines"); p = substr($0, 3); r = pathok(p); if (r != "") bad(r ": " p)
  if (nd > 0 && (p "") <= (lastd "")) bad("d lines not sorted: " p); lastd = p; nd++
  lc = tolower(p); if (lc in seen) bad("duplicate path (FAT ignores case): " p); seen[lc] = 1
  q = parent(p); if (q != "" && !(q in dset)) bad("parent folder not declared: " p)
  if (q == "") print p > (O "/top")
  dset[p] = 1; nb++; print p > (O "/d"); next
}
/^f / {
  if (st > 1) bad("f line after the k line"); st = 1
  if ($0 !~ /^f [0-9a-f]{64} (0|[1-9][0-9]{0,9}) [^ ]+$/) bad("bad f line")
  split($0, a, " "); p = a[4]; s = a[3] + 0; if (s >= 4294967296) bad("file too big for FAT32: " p)
  r = pathok(p); if (r != "") bad(r ": " p)
  if (nf > 0 && (p "") <= (lastf "")) bad("f lines not sorted: " p); lastf = p; nf++
  lc = tolower(p); if (lc in seen) bad("duplicate path (FAT ignores case): " p); seen[lc] = 1
  q = parent(p); if (q != "" && !(q in dset)) bad("parent folder not declared: " p)
  have[p] = 1; nb++; print a[2], a[3], p > (O "/f"); next
}
/^k / {
  if (st == 2) bad("more than one k line"); st = 2
  if ($0 !~ /^k [0-9a-f]{64} [1-9][0-9]{0,9} EFI\/BOOTX64\.EFI$/) bad("bad k line")
  split($0, a, " "); nb++; print a[2], a[3] > (O "/k"); next
}
/^end / { c = substr($0, 5); if (c !~ /^[1-9][0-9]*$/ || c + 0 != nb) bad("end count does not match"); done = 1; next }
{ bad("unknown line") }
END {
  if (err) exit 1
  if (NR < 7 || !done) { printf "manifest: incomplete (no end line)\n"; exit 1 }
  if (st != 2) { printf "manifest: no kernel (k) line\n"; exit 1 }
  split("super.img super.layout sysupdate.sh sysupdate.pub early_patch.sh hook.sh", req, " ")
  for (i in req) if (!(req[i] in have)) { printf "manifest: the set lacks %s\n", req[i]; exit 1 }
}'
su_parse_manifest() {
  rm -rf "$2"; mkdir -p "$2"; : > "$2/d"; : > "$2/top"
  _sz=$(su_size "$1")
  [ -n "$_sz" ] && [ "$_sz" -gt 0 ] && [ "$_sz" -le 8388608 ] || { SU_ERR="manifest missing, empty or too big"; return 1; }
  [ "$(tail -c 1 "$1" | od -An -tx1 | tr -d ' \n')" = 0a ] || { SU_ERR="manifest does not end with a newline"; return 1; }
  [ "$(tr -d '\n' < "$1" | tr -d 'A-Za-z0-9._+,@=~/: -' | wc -c)" = 0 ] || { SU_ERR="manifest has characters that are not allowed"; return 1; }
  SU_ERR=$(awk -v O="$2" "$SU_AWK_MANIFEST" "$1" 2>&1) && return 0
  [ -n "$SU_ERR" ] || SU_ERR="manifest rejected"
  return 1
}
su_mval() { cat "$1/$2" 2>/dev/null; }   # su_mval PARSEDDIR key

# su_walk BUNDLE PARSEDDIR: the bundle may hold only manifest, manifest.sig and files/<listed path> (+ files/EFI/BOOTX64.EFI).
# Writes PARSEDDIR/delivered (paths present in files/) and PARSEDDIR/kdelivered (1 if the kernel is in files/).
su_walk() {
  _b=$1; _o=$2
  [ -d "$_b" ] || { SU_ERR="not a folder: $_b"; return 1; }
  _x=$(cd "$_b" && find . ! -type f ! -type d | head -n 3)
  [ -z "$_x" ] || { SU_ERR="the bundle has a link or special file: $_x"; return 1; }
  _x=$( (cd "$_b" && find . -type d; echo ===; find . -type f) | awk -v O="$_o" -v SC="$SU_CHECKED" '
    BEGIN { while ((getline l < (O "/d")) > 0) okd["./files/" l] = 1; okd["."] = 1; okd["./files"] = 1; okd["./files/EFI"] = 1
            while ((getline l < (O "/f")) > 0) { split(l, a, " "); okf["./files/" a[3]] = a[3] } }
    $0 == "===" { files = 1; next }
    $0 !~ /^[A-Za-z0-9._+,@=~\/-]+$/ { print "name not allowed: " $0; exit }
    !files { if (!($0 in okd)) { print "folder not in the manifest: " $0; exit }; next }
    $0 == "./manifest" || $0 == "./manifest.sig" || $0 == "./" SC { next }
    $0 == "./files/EFI/BOOTX64.EFI" { print "1" > (O "/kdelivered"); next }
    ($0 in okf) { print okf[$0] > (O "/delivered"); next }
    { print "file not in the manifest: " $0; exit }')
  [ -z "$_x" ] || { SU_ERR="$_x"; return 1; }
  [ -f "$_o/delivered" ] || : > "$_o/delivered"
  [ -f "$_b/manifest" ] && [ -f "$_b/manifest.sig" ] || { SU_ERR="the bundle lacks manifest or manifest.sig"; return 1; }
  return 0
}

# su_hash_check ROOT LIST: LIST has "sha256  path" lines (relative to ROOT). Fails with the first bad files in SU_ERR.
su_hash_check() {
  [ -s "$2" ] || return 0
  _r=$( (cd "$1" && sha256sum -c "$2" 2>&1) | grep -v ': OK$' | head -n 3)
  [ -z "$_r" ] && return 0
  SU_ERR="hash mismatch: $(echo "$_r" | tr '\n' ' ')"; return 1
}

# ---------------------------------------------------------------------------------------------------------------- set info
# Google build id of a gbos folder: super.img's system build.prop, then system.manifest, then patched/MANIFEST's "# build" line.
su_fp_build() { echo "$1" | sed -n 's#^[^/]*/[^/]*/[^/]*/[^/]*/\([0-9][0-9]*\):.*#\1#p'; }
su_super_fingerprint() { # SUPERIMG LAYOUT LOOPNUM
  [ -f "$1" ] && [ -f "$2" ] || return 1
  if [ -n "$SU_TEST" ]; then sed -n 's/^ro\.system\.build\.fingerprint=//p' "$1" 2>/dev/null | head -n 1; return 0; fi
  _st=$(awk '$1 == "system_a" { print $2; exit }' "$2"); [ -n "$_st" ] || return 1
  _lp=/dev/loop$3; [ -b "$_lp" ] || mknod -m 660 "$_lp" b 7 "$3" 2>/dev/null
  losetup -d "$_lp" 2>/dev/null
  losetup -r -o $((_st * 512)) "$_lp" "$1" 2>/dev/null || return 1
  mkdir -p /mnt/su-probe$3
  if mount -t erofs -o ro "$_lp" /mnt/su-probe$3 2>/dev/null; then
    sed -n 's/^ro\.system\.build\.fingerprint=//p' /mnt/su-probe$3/system/build.prop 2>/dev/null | head -n 1
    umount /mnt/su-probe$3
  fi
  losetup -d "$_lp" 2>/dev/null; return 0
}
su_set_build() { # GBOSDIR [LOOPNUM]
  _fp=$(su_super_fingerprint "$1/super.img" "$1/super.layout" "${2:-14}" 2>/dev/null)
  _b=$(su_fp_build "$_fp"); [ -n "$_b" ] && { echo "$_b"; return; }
  _b=$(sed -n 's/^google_build \([0-9][0-9]*\)$/\1/p' "$1/system.manifest" 2>/dev/null | head -n 1); [ -n "$_b" ] && { echo "$_b"; return; }
  su_fp_build "$(sed -n 's/^# build //p' "$1/patched/MANIFEST" 2>/dev/null | head -n 1)"
}
su_manifest_field() { sed -n "s/^$2 //p" "$1" 2>/dev/null | head -n 1; }   # FILE key (unverified, display only)
cmd_setinfo() {
  _d=$1
  if [ -f "$_d/system.manifest" ]; then
    echo "id=$(su_sha "$_d/system.manifest")"
    echo "seq=$(su_manifest_field "$_d/system.manifest" seq)"
    echo "commit=$(su_manifest_field "$_d/system.manifest" commit)"
  else
    echo "id=none"; echo "seq=0"; echo "commit=unknown"
  fi
  echo "google_build=$(su_set_build "$_d" "${2:-14}")"
}

# ---------------------------------------------------------------------------------------------------------------- verify
# su_verify_bundle BUNDLE PUB OUTDIR: signature (on private copies), manifest, contents, hashes of the delivered files.
su_verify_bundle() {
  _vb=$1; _vo=$3
  mkdir -p "$_vo"
  cp "$_vb/manifest" "$_vo/manifest" 2>/dev/null && cp "$_vb/manifest.sig" "$_vo/manifest.sig" 2>/dev/null \
    || { SU_ERR="the bundle lacks manifest or manifest.sig"; return 1; }
  su_verify_sig "$_vo/manifest" "$_vo/manifest.sig" "$2" || return 1
  su_parse_manifest "$_vo/manifest" "$_vo/p" || return 1
  su_walk "$_vb" "$_vo/p" || return 1
  awk 'NR == FNR { d[$1] = 1; next } ($3 in d) { print $1 "  files/" $3 }' "$_vo/p/delivered" "$_vo/p/f" > "$_vo/chk.delivered"
  [ -f "$_vo/p/kdelivered" ] && echo "$(cut -d' ' -f1 "$_vo/p/k")  files/EFI/BOOTX64.EFI" >> "$_vo/chk.delivered"
  [ "${SU_TRUST_STAGED:-0}" = 1 ] && su_skip_checked "$_vb" "$_vo/chk.delivered"
  su_hash_check "$_vb" "$_vo/chk.delivered" || return 1
  SU_ID=$(su_sha "$_vo/manifest")
  return 0
}
# su_skip_checked BUNDLE LIST (stage0 only): drop from LIST ("sha  files/path" lines) the big files that `stage` hashed right after
# copying them (BUNDLE/staged.checked: "sha size tailsha files/path") and that still have that size and last MB. Hashing R5's 3.7 GB
# super.img again took 70 s of the start after staging (boot 19, 2026-11-24). Only after a clean unmount: a power cut can leave a
# file damaged but the same size, and the full check is what catches it. The signed manifest still decides each file's hash: a line
# only skips a file whose recorded hash is the manifest's. Whoever can write the record can also write the installed files directly,
# so it opens nothing new.
su_skip_checked() {
  [ -f "$1/$SU_CHECKED" ] || return 0
  if su_fat_mounted_dirty; then su_log "not using $SU_CHECKED (the volume was not cleanly unmounted): hashing every file"; return 0; fi
  : > "$2.left"; _sk=0
  while read -r _h _p; do
    _sz=$(su_size "$1/$_p")
    if [ -n "$_sz" ] && grep -qxF "$_h $_sz $(su_tailsha "$1/$_p") $_p" "$1/$SU_CHECKED"; then _sk=$((_sk + 1)); else echo "$_h  $_p" >> "$2.left"; fi
  done < "$2"
  mv -f "$2.left" "$2"
  [ $_sk = 0 ] || su_log "$_sk big file(s) hashed when staged and unchanged since (size, last MB): not hashed again"
  return 0
}
cmd_verify() {
  _b=$1; shift; _pub=; _inst=
  while [ $# -gt 0 ]; do case "$1" in --pub) _pub=$2; shift 2;; --installed) _inst=$2; shift 2;; *) su_die "verify: unknown option $1";; esac; done
  [ -n "$_pub" ] || _pub=$(dirname "$0")/sysupdate.pub
  su_tmp
  su_verify_bundle "$_b" "$_pub" "$W/v" || su_die "$SU_ERR"
  P=$W/v/p
  echo "set $SU_ID"
  echo "seq $(su_mval "$P" seq)  google_build $(su_mval "$P" google_build)  commit $(su_mval "$P" commit)  created $(su_mval "$P" created)"
  echo "files $(wc -l < "$P/f") listed, $(wc -l < "$P/delivered") in the bundle, kernel $([ -f "$P/kdelivered" ] && echo included || echo 'not included')"
  if [ -n "$_inst" ]; then
    su_final_check "$_b" "$_inst" "$P" "$W/v" || su_die "$SU_ERR"
    echo "the final set on $_inst checks out"
  fi
  echo "VERIFY OK"
}

# su_final_check BUNDLE G PARSED WORK: every listed file not in the bundle must already be in G with the listed hash; sh -n on scripts;
# super.layout must fit super.img; required hooks present.
su_final_check() {
  _fb=$1; _fg=$2; _fp=$3; _fw=$4
  awk 'NR == FNR { d[$1] = 1; next } !($3 in d) { print $1 "  " $3 }' "$_fp/delivered" "$_fp/f" > "$_fw/chk.installed"
  su_hash_check "$_fg" "$_fw/chk.installed" || { SU_ERR="this installed system differs from what the bundle was made for ($SU_ERR); use a full bundle or the stick's menu 1"; return 1; }
  # path of each final file (bundle copy or installed copy)
  su_final_path() { if grep -qxF "$1" "$_fp/delivered"; then echo "$_fb/files/$1"; else echo "$_fg/$1"; fi; }
  for _s in $(awk '$3 ~ /\.sh$/ { print $3 }' "$_fp/f"); do
    sh -n "$(su_final_path "$_s")" 2>/dev/null || { SU_ERR="script does not parse (sh -n): $_s"; return 1; }
  done
  grep -q '^gbos_sysupdate_stage0$' "$(su_final_path early_patch.sh)" || { SU_ERR="the set's early_patch.sh does not run the updater (no rollback possible)"; return 1; }
  su_pub_modulus "$(su_final_path sysupdate.pub)" >/dev/null || { SU_ERR="the set's sysupdate.pub has no key (it would refuse every later update)"; return 1; }
  _ss=$(su_size "$(su_final_path super.img)")
  _lay=$(su_final_path super.layout)
  awk -v S="$_ss" 'BEGIN { bad = 0 }
    NF != 3 || $1 !~ /^[a-z][a-z0-9_]*_a$/ || $2 !~ /^[0-9]+$/ || $3 !~ /^[1-9][0-9]*$/ { bad = 1; exit }
    NR == 1 && $1 != "system_a" { bad = 1; exit }
    ($2 + $3) * 512 > S { bad = 1; exit }
    END { if (bad || NR < 1 || NR > 6) exit 1 }' "$_lay" || { SU_ERR="super.layout does not fit super.img"; return 1; }
  return 0
}

# ---------------------------------------------------------------------------------------------------------------- state
# gbos/update/state: "key=value" lines, written atomically, parsed with sed, never sourced.
su_st() { sed -n "s/^$1=//p" "$U/state" 2>/dev/null | head -n 1; }
su_state_write() { # key=value ... (complete new state)
  { echo "gbos86-update-state 1"; for _kv in "$@"; do echo "$_kv"; done; } > "$U/state.part" && sync && mv -f "$U/state.part" "$U/state" && sync
}
su_state_set() { # key value: change one field, keep the others
  { sed "/^$1=/d" "$U/state"; echo "$1=$2"; } > "$U/state.part" && sync && mv -f "$U/state.part" "$U/state" && sync
}
su_status() { # one human-readable result, also appended to history.txt (last 200 lines)
  mkdir -p "$U"
  { echo "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null) boot ${BN:-?}"; echo "$*"; } > "$U/status.txt.part" && mv -f "$U/status.txt.part" "$U/status.txt"
  { tail -n 199 "$U/history.txt" 2>/dev/null; echo "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null) boot ${BN:-?}: $*"; } > "$U/history.part" && mv -f "$U/history.part" "$U/history.txt"
  sync
}
su_reject() { # DIR REASON: keep the manifest and the reason, delete the files (they would only fill GBOSSYS)
  _rn=$(basename "$1"); [ "$_rn" = update.staging ] && _rn=$(cat "$U/staged.name" 2>/dev/null || echo staged)
  mkdir -p "$REJ"; rm -rf "$REJ/$_rn"; mkdir -p "$REJ/$_rn"
  cp "$1/manifest" "$REJ/$_rn/manifest" 2>/dev/null
  echo "$2" > "$REJ/$_rn/reason.txt"
  rm -rf "$1"
  # keep the 5 newest rejections
  ls -t "$REJ" 2>/dev/null | tail -n +6 | while read -r _o; do rm -rf "$REJ/$_o"; done
  sync
}

# ---------------------------------------------------------------------------------------------------------------- EFI partition
# The internal disk's EFI system partition (same disk as GBOSSYS, GPT type, as install.sh finds it) and the kernel's path in it:
# EFI/gbos86/BOOTX64.EFI on a shared (dual-boot) ESP, EFI/BOOT/BOOTX64.EFI on our own GBOS_EFI.
su_efi_mount() {
  KREL=; _ours=0
  if [ -n "${SU_EFI_DIR:-}" ]; then EFIMNT=$SU_EFI_DIR; [ "${SU_EFI_LABEL:-GBOS_EFI}" = GBOS_EFI ] && _ours=1
  else
    _sd=/dev/$(basename "$(readlink -f /sys/class/block/$(basename "$SU_DEV")/.. 2>/dev/null)")
    _ins=$G/installer
    _e=$("$_ins/lib/ld-musl-x86_64.so.1" --library-path "$_ins/lib" "$_ins/bin/sfdisk" -d "$_sd" 2>/dev/null \
      | awk 'toupper($0) ~ /TYPE=C12A7328-F81F-11D2-BA4B-00A0C93EC93B/ { print $1; exit }')
    [ -n "$_e" ] || _e=$(findfs LABEL=GBOS_EFI 2>/dev/null)
    [ -b "$_e" ] || { SU_ERR="cannot find this disk's EFI partition"; return 1; }
    [ "$(findfs LABEL=GBOS_EFI 2>/dev/null)" = "$_e" ] && _ours=1
    EFIMNT=/mnt/su-efi; mkdir -p "$EFIMNT"
    awk -v m="$EFIMNT" '$2 == m { f = 1 } END { exit !f }' /proc/mounts 2>/dev/null || mount -t vfat -o rw,flush "$_e" "$EFIMNT" 2>/dev/null || { SU_ERR="cannot mount the EFI partition $_e"; return 1; }
    SU_EFI_MOUNTED=1
  fi
  if [ -f "$EFIMNT/EFI/gbos86/BOOTX64.EFI" ]; then KREL=EFI/gbos86
  # EFI/BOOT/BOOTX64.EFI is ours only on our own GBOS_EFI partition: on a shared (dual-boot) one it is the other OS's fallback loader
  elif [ -f "$EFIMNT/EFI/BOOT/BOOTX64.EFI" ] && [ $_ours = 1 ]; then KREL=EFI/BOOT
  else SU_ERR="no gbOS86 kernel on the EFI partition (EFI/gbos86/BOOTX64.EFI, or EFI/BOOT on a GBOS_EFI partition)"; return 1; fi
  return 0
}
su_efi_umount() { [ "${SU_EFI_MOUNTED:-0}" = 1 ] && { sync; umount "$EFIMNT" 2>/dev/null; SU_EFI_MOUNTED=0; }; return 0; }

# ---------------------------------------------------------------------------------------------------------------- journal
# Plan lines: R path (replace: old -> update.prev, new from staging), A path (add), D path (old -> update.prev), K (kernel), M (system.manifest).
# Each step can be redone or undone after a power cut, and undoing twice is harmless:
#   R forward: back up (copy if small, else rename) to prev/, then rename-over from staging.   undo: prev/path exists -> rename it back over.
#   A forward: rename from staging.   undo: remove the path.     D forward: rename to prev/.  undo: prev/path exists -> rename it back.
#   K forward: kernel backup in update/kernel.prev, new kernel written as BOOTX64.NEW, then rename-over BOOTX64.EFI (never missing).
#     undo: kernel.prev differs from the installed kernel -> written as BOOTX64.NEW and renamed over again.
su_backup() { # path: current file -> prev/path
  mkdir -p "$(dirname "$PREV/$1")"
  [ -e "$PREV/$1" ] && return 0
  [ -f "$G/$1" ] || return 0
  _bs=$(su_size "$G/$1")
  if [ "$_bs" -le "$SU_SMALL" ]; then
    cp "$G/$1" "$PREV/$1.part" && sync && mv -f "$PREV/$1.part" "$PREV/$1"
  else
    mv -f "$G/$1" "$PREV/$1"
  fi
}
su_op_forward() { # op path
  case "$1" in
    R) su_backup "$2" || return 1; su_crashpoint; mkdir -p "$(dirname "$G/$2")"; mv -f "$ST/files/$2" "$G/$2" || return 1;;
    A) mkdir -p "$(dirname "$G/$2")"; mv -f "$ST/files/$2" "$G/$2" || return 1;;
    D) mkdir -p "$(dirname "$PREV/$2")"; [ -e "$PREV/$2" ] || mv -f "$G/$2" "$PREV/$2" || return 1;;
    M) [ -f "$G/system.manifest" ] && { su_backup system.manifest || return 1; }; cp "$ST/manifest" "$G/system.manifest.part" && sync && mv -f "$G/system.manifest.part" "$G/system.manifest" || return 1;;
    K) su_kernel_forward || return 1;;
  esac
  sync; su_crashpoint; return 0
}
su_op_undo() { # op path
  case "$1" in
    R|D) [ -e "$PREV/$2" ] && { mkdir -p "$(dirname "$G/$2")"; mv -f "$PREV/$2" "$G/$2" || return 1; }; rm -f "$PREV/$2.part";;
    A) rm -f "$G/$2";;
    M) if [ "$(su_sha "$G/system.manifest")" = "$SU_PLANSET" ]; then
         if [ -e "$PREV/system.manifest" ]; then mv -f "$PREV/system.manifest" "$G/system.manifest"; else rm -f "$G/system.manifest"; fi
       fi; rm -f "$G/system.manifest.part";;
    K) su_kernel_undo || return 1;;
  esac
  sync; return 0
}
su_kernel_forward() {
  _kd=$EFIMNT/$KREL
  [ -f "$U/kernel.prev" ] || { cp "$_kd/BOOTX64.EFI" "$U/kernel.prev.part" && sync && mv -f "$U/kernel.prev.part" "$U/kernel.prev"; } || return 1
  _kh=$(cut -d' ' -f1 "$U/p/k")
  [ "$(su_sha "$_kd/BOOTX64.EFI")" = "$_kh" ] && { rm -f "$_kd/BOOTX64.NEW"; return 0; }
  [ "$(su_sha "$_kd/BOOTX64.NEW" 2>/dev/null)" = "$_kh" ] || return 1     # written and checked by su_prepare_kernel
  su_crashpoint
  mv -f "$_kd/BOOTX64.NEW" "$_kd/BOOTX64.EFI" || return 1
  sync; SU_REBOOT=1; su_log "kernel replaced ($KREL/BOOTX64.EFI)"
}
su_kernel_undo() {
  _kd=$EFIMNT/$KREL
  [ -f "$U/kernel.prev" ] || { rm -f "$_kd/BOOTX64.NEW"; return 0; }   # the switch never got to the kernel: drop the prepared copy
  _kh=$(su_sha "$U/kernel.prev")
  [ "$(su_sha "$_kd/BOOTX64.EFI")" = "$_kh" ] && { rm -f "$_kd/BOOTX64.NEW"; return 0; }
  cp "$U/kernel.prev" "$_kd/BOOTX64.NEW" && su_dropcache && [ "$(su_sha "$_kd/BOOTX64.NEW")" = "$_kh" ] || { SU_ERR="could not restore the previous kernel"; return 1; }
  mv -f "$_kd/BOOTX64.NEW" "$_kd/BOOTX64.EFI" || return 1
  sync; SU_REBOOT=1; su_log "previous kernel restored ($KREL/BOOTX64.EFI)"
}
su_fail_cleanup() { # before the switch started: drop the written kernel copy and the plan
  [ -n "${KREL:-}" ] && [ -n "${EFIMNT:-}" ] && rm -f "$EFIMNT/$KREL/BOOTX64.NEW"
  rm -f "$U/plan.part"; su_efi_umount
}
su_needs_efi() { grep -q '^K$' "$U/plan" 2>/dev/null; }

# Undo the whole plan, newest step first. Safe to repeat.
su_undo_plan() {
  [ -f "$U/plan" ] || return 0
  if su_needs_efi; then su_efi_mount || { su_log "undo: $SU_ERR (kernel not restored)"; }; fi
  _ok=0; SU_PLANSET=$(sed -n 's/^set \([0-9a-f]\{64\}\)$/\1/p' "$U/plan" | head -n 1)
  tac "$U/plan" 2>/dev/null > "$U/plan.rev" || awk '{ l[NR] = $0 } END { for (i = NR; i > 0; i--) print l[i] }' "$U/plan" > "$U/plan.rev"
  while read -r _op _p; do
    case "$_op" in R|A|D|M) su_op_undo "$_op" "$_p" || _ok=1;; K) [ -n "${KREL:-}" ] && { su_op_undo K || _ok=1; };; esac
  done < "$U/plan.rev"
  rm -f "$U/plan.rev"
  # folders the update created and that are empty again
  awk '$1 == "A" { print $2 }' "$U/plan" | while read -r _p; do _d=$(dirname "$_p"); [ "$_d" = . ] || (cd "$G" && rmdir -p "$_d" 2>/dev/null); done
  su_efi_umount
  return $_ok
}

# ---------------------------------------------------------------------------------------------------------------- activation
# su_make_plan: from the verified staging folder ($ST, parsed in $ST/p) and the files on disk.
su_make_plan() {
  : > "$W/disk"
  # files on disk under the set's top-level folders (they are the set's: unlisted ones there are removed, as menu 1's rm -rf did)
  while read -r _t; do [ -d "$G/$_t" ] && (cd "$G" && find "$_t" -type f) >> "$W/disk"; done < "$U/p/top"
  # top-level files of the previous set that the new one drops
  [ -f "$G/system.manifest" ] && awk '$1 == "f" && $4 !~ /\// { print $4 }' "$G/system.manifest" >> "$W/disk"
  { echo "gbos86-plan 1"; echo "set $NEWID"
    awk 'FILENAME == ARGV[1] { want[tolower($3)] = 1; next }
         { if (!(tolower($0) in want) && !seen[$0]++) print "D " $0 }' "$U/p/f" "$W/disk" > "$W/plan.d"
    while read -r _p; do if [ -e "$G/$_p" ]; then echo "R $_p"; else echo "A $_p"; fi; done < "$U/p/delivered"
    cat "$W/plan.d"
    [ -n "$SU_KCHANGE" ] && echo "K"
    echo "M"
  } > "$U/plan.part"
  # a listed file where a folder is (or the reverse) is never merged
  while read -r _op _p; do
    case "$_op" in R|A) [ -d "$G/$_p" ] && { SU_ERR="a folder is in the way of $_p"; return 1; };; esac
  done < "$U/plan.part"
  while read -r _p; do [ -f "$G/$_p" ] && { SU_ERR="a file is in the way of folder $_p"; return 1; }; done < "$U/p/d"
  return 0
}
su_prepare_kernel() { # writes BOOTX64.NEW (checked) when the set's kernel differs from the installed one
  SU_KCHANGE=
  su_efi_mount || return 1
  _kh=$(cut -d' ' -f1 "$U/p/k"); _ks=$(cut -d' ' -f2 "$U/p/k")
  if [ "$(su_sha "$EFIMNT/$KREL/BOOTX64.EFI")" = "$_kh" ]; then rm -f "$EFIMNT/$KREL/BOOTX64.NEW"; return 0; fi
  [ -f "$U/p/kdelivered" ] || { SU_ERR="the set has another kernel than the installed one, but the bundle does not include it"; return 1; }
  rm -f "$EFIMNT/$KREL/BOOTX64.NEW"
  _ef=$(su_free_kb "$EFIMNT")
  [ "${_ef:-0}" -ge $(( $(su_kb "$_ks") + 1024 )) ] || { SU_ERR="the EFI partition has only ${_ef:-0} KB free; the new kernel needs $(su_kb "$_ks") KB"; return 1; }
  cp "$ST/files/EFI/BOOTX64.EFI" "$EFIMNT/$KREL/BOOTX64.NEW" && su_dropcache \
    && [ "$(su_sha "$EFIMNT/$KREL/BOOTX64.NEW")" = "$_kh" ] || { rm -f "$EFIMNT/$KREL/BOOTX64.NEW"; SU_ERR="writing the new kernel to the EFI partition failed"; return 1; }
  SU_KCHANGE=1
}

su_activate() { # the bundle is in $ST
  su_tty "checking the system update in update.staging (about a minute) ..."
  PUB=$G/sysupdate.pub
  SU_TRUST_STAGED=1 su_verify_bundle "$ST" "$PUB" "$W/v" || return 1
  rm -rf "$U/p"; cp -r "$W/v/p" "$U/p"
  NEWID=$SU_ID
  _nseq=$(su_mval "$U/p" seq); _nb=$(su_mval "$U/p" google_build); _nfp=$(su_mval "$U/p" google_fingerprint)
  OLDID=none; _oseq=0
  if [ -f "$G/system.manifest" ]; then
    OLDID=$(su_sha "$G/system.manifest")
    _oseq=$(sed -n 's/^seq \([0-9][0-9]*\)$/\1/p' "$G/system.manifest" | head -n 1); _oseq=${_oseq:-0}
  fi
  [ "$NEWID" != "$OLDID" ] || { SU_ERR="this set is already installed"; return 1; }
  [ "$_nseq" -gt "$_oseq" ] || { SU_ERR="not newer than the installed set (seq $_nseq, installed $_oseq)"; return 1; }
  _ob=$(su_set_build "$G" 14); _ob=${_ob:-0}
  [ "$_nb" -ge "$_ob" ] || { SU_ERR="older Google build ($_nb) than the installed one ($_ob): going back needs a data wipe (stick, Reinstall)"; return 1; }
  su_final_check "$ST" "$G" "$U/p" "$W/v" || return 1
  # the final super.img must be the Google build the manifest names
  _sp=$G/super.img; grep -qxF super.img "$U/p/delivered" && _sp=$ST/files/super.img
  _lp=$G/super.layout; grep -qxF super.layout "$U/p/delivered" && _lp=$ST/files/super.layout
  _fp=$(su_super_fingerprint "$_sp" "$_lp" 15)
  [ "$_fp" = "$_nfp" ] || { SU_ERR="super.img is not Google build $_nb (its fingerprint: ${_fp:-unreadable})"; return 1; }
  su_prepare_kernel || return 1
  su_make_plan || { su_fail_cleanup; return 1; }
  # space for the small files' backup copies, the kernel backup and the new system.manifest
  _need=$(awk '$1 == "R" { print $2 }' "$U/plan.part" | while read -r _p; do _s=$(su_size "$G/$_p"); [ "${_s:-0}" -le $SU_SMALL ] && echo "${_s:-0}"; done | awk '{ s += $1 } END { print int(s / 1024) + 1 }')
  [ -n "$SU_KCHANGE" ] && _need=$(( _need + $(su_kb "$(su_size "$EFIMNT/$KREL/BOOTX64.EFI")") ))
  _need=$(( _need + 1024 ))
  _free=$(su_free_kb "$G")
  [ "${_free:-0}" -ge "$_need" ] || { SU_ERR="GBOSSYS needs $_need KB free to switch, it has ${_free:-0} KB"; su_fail_cleanup; return 1; }

  # point of no return for this boot: from here a power cut is undone at the next boot
  cp "$SU_SELF" "$U/engine.sh.part" && sync && mv -f "$U/engine.sh.part" "$U/engine.sh" \
    || { SU_ERR="cannot write gbos/update/engine.sh"; su_fail_cleanup; return 1; }
  rm -rf "$PREV"; mkdir -p "$PREV"
  mv -f "$U/plan.part" "$U/plan"
  su_state_write "phase=switching" "set=$NEWID" "old=$OLDID" "seq=$_nseq" "google_old=$_ob" "google_new=$_nb" "kernel=${SU_KCHANGE:-0}" "tries=0" "boots=" "efi=${KREL:-}"
  su_crashpoint
  su_tty "installing set seq $_nseq (Google build $_nb) ..."
  while read -r _op _p; do
    case "$_op" in R|A|D|M|K) su_op_forward "$_op" "$_p" || { SU_ERR="switch step $_op $_p failed"; su_undo_plan; su_state_clear; return 1; };; esac
  done < "$U/plan"
  # folders of the set's top-level folders that are now empty, and stale update stamps (menu 1 must compare everything again)
  awk '$1 == "D" { print $2 }' "$U/plan" | while read -r _p; do _d=$(dirname "$_p"); [ "$_d" = . ] || (cd "$G" && rmdir -p "$_d" 2>/dev/null); done
  rm -rf "$G/.stamps"
  su_efi_umount
  _t=1; _bs=$BN; [ "$SU_REBOOT" = 1 ] && { _t=0; _bs=; }
  su_state_write "phase=trying" "set=$NEWID" "old=$OLDID" "seq=$_nseq" "google_old=$_ob" "google_new=$_nb" "kernel=${SU_KCHANGE:-0}" "tries=$_t" "boots=$_bs" "efi=${KREL:-}"
  rm -f "$U/boot-ok" "$U/android-started" "$U/rollback-request"
  su_status "installed set seq $_nseq (Google build $_nb, commit $(su_mval "$U/p" commit)); trying it, rollback after ${MAXTRIES} failed boots"
  su_tty "installed. $([ "$SU_REBOOT" = 1 ] && echo 'New kernel: restarting now.' || echo 'Starting it now.')"
  return 0
}
su_state_clear() {
  rm -f "$U/state" "$U/state.part" "$U/plan" "$U/plan.part" "$U/engine.sh" "$U/kernel.prev" "$U/kernel.prev.part" "$U/boot-ok" "$U/android-started" \
    "$U/rollback-request" "$U/staged.name"
  rm -rf "$PREV" "$U/p"; sync
}

su_commit() {
  su_state_set phase committing
  _s=$(su_st set); _q=$(su_st seq)
  rm -rf "$PREV" "$ST"; su_state_clear
  su_status "update committed: set seq $_q works (Android finished booting)"
  su_log "committed set $_s"
}
su_rollback() { # REASON
  su_state_set phase rollingback
  su_tty "$1: going back to the previous system ..."
  su_undo_plan || su_log "rollback: some steps failed ($SU_ERR)"
  _name=$(cat "$U/staged.name" 2>/dev/null || echo "set-$(su_st seq)")
  if [ -d "$ST" ]; then su_reject "$ST" "rolled back: $1"
  else mkdir -p "$REJ/$_name"; echo "rolled back: $1" > "$REJ/$_name/reason.txt"; fi
  su_state_clear
  su_status "rolled back to the previous set: $1"
  su_tty "previous system restored."
}

su_booted_ok() { # did one of the tried boots reach boot_completed?
  _s=$(su_st set)
  [ -n "$_s" ] && [ "$(cat "$U/boot-ok" 2>/dev/null)" = "$_s" ] && return 0
  # Fallback: a tried boot's kmsg.log. Not on a volume the kernel mounted dirty: a boot that ended without a clean unmount can leave
  # its kmsg.log claiming more bytes than its cluster chain holds, and reading past the chain is a FAT error that makes the kernel set
  # the whole volume read-only for this boot (boot-4, 2026-11-22, by stick_fat.sh's check). That boot then counts as not confirmed.
  if su_fat_mounted_dirty; then
    su_log "boot check: not reading the tried boots' kmsg.log (the volume was not cleanly unmounted; a damaged log would make it read-only)"
    return 1
  fi
  for _b in $(su_st boots); do
    case "$_b" in *[!0-9]*|'') continue;; esac
    grep -aqF 'caller=T1;init: processing action (sys.boot_completed=1)' "$G/logs/boot-$_b/kmsg.log" 2>/dev/null && return 0
  done
  return 1
}
su_fat_mounted_dirty() { # did this boot's kernel log "Volume was not properly unmounted" for the volume (SU_DEV)?
  _fd=${SU_DEV##*/}; [ -n "$_fd" ] || return 1
  ${SU_DMESG:-dmesg} 2>/dev/null | grep -qF "FAT-fs ($_fd): Volume was not properly unmounted"
}
su_trying() {
  if su_booted_ok; then su_commit; return 0; fi
  _tries=$(su_st tries); _tries=${_tries:-0}
  _go=$(su_st google_old); _gn=$(su_st google_new)
  if [ -f "$U/rollback-request" ]; then su_rollback "rollback asked for (update/rollback-request)"; return 0; fi
  if [ "$_tries" -ge "$MAXTRIES" ]; then
    if [ "$_go" != "$_gn" ] && [ -f "$U/android-started" ]; then
      [ "$(su_st held)" = 1 ] || su_status "the new set failed to finish booting $_tries times. Not rolled back on its own: it has a new Google build and Android already ran it, so /data may be upgraded and the old build may not start with it. Use the stick's menu 1, or create gbos/update/rollback-request to go back anyway."
      su_state_set held 1
      su_tty "the update failed to boot $_tries times; see gbos/update/status.txt (stick menu 1, or gbos/update/rollback-request)"
      return 0
    fi
    su_rollback "the new set failed to finish booting $_tries times"
    return 0
  fi
  su_state_set tries $((_tries + 1))
  su_state_set boots "$(su_st boots) $BN"
  su_log "trying the new set, boot $((_tries + 1)) of $MAXTRIES"
  return 0
}

su_pick_inbox() { # newest bundle folder in the inbox -> $ST (others are rejected as older)
  [ -d "$INBOX" ] || return 1
  for _d in "$INBOX"/*.part; do [ -d "$_d" ] && { su_log "removing interrupted copy $_d"; rm -rf "$_d"; }; done
  _all=$(ls "$INBOX" 2>/dev/null | grep '^gbos86-update-' | sort)
  _pick=$(echo "$_all" | tail -n 1)
  [ -n "$_pick" ] || return 1
  for _o in $_all; do [ "$_o" = "$_pick" ] || su_reject "$INBOX/$_o" "a newer bundle was in update.inbox at the same time"; done
  rm -rf "$ST"
  mv "$INBOX/$_pick" "$ST" || return 1
  echo "$_pick" > "$U/staged.name"
  sync
}

cmd_boot() {
  G=$1; L=$2; BN=$3; shift 3
  SU_EFI_DIR=; SU_EFI_LABEL=; SU_DEV=
  while [ $# -gt 0 ]; do case "$1" in --efi-dir) SU_EFI_DIR=$2; shift 2;; --efi-label) SU_EFI_LABEL=$2; shift 2;; --dev) SU_DEV=$2; shift 2;; *) shift;; esac; done
  [ -n "$SU_DEV" ] || SU_DEV=$(awk '$2 == "/gbos" { print $1; exit }' /proc/mounts 2>/dev/null)
  U=$G/update; INBOX=$G/update.inbox; ST=$G/update.staging; PREV=$G/update.prev; REJ=$G/update.rejected
  SU_SELF=$0
  MAXTRIES=$(sed -n 's/^SYSUPDATE_ATTEMPTS=\([1-9][0-9]*\)$/\1/p' "$G/gbos.cfg" 2>/dev/null | tail -n 1); MAXTRIES=${MAXTRIES:-3}
  [ "$(sed -n 's/^SYSUPDATE=//p' "$G/gbos.cfg" 2>/dev/null | tail -n 1)" = 0 ] && [ ! -f "$U/state" ] && { su_log "SYSUPDATE=0 in gbos.cfg: not looking at update.inbox"; exit 0; }
  mkdir -p "$U"; su_tmp
  _ph=$(su_st phase)
  [ -z "$_ph" ] && [ -f "$U/plan" ] && _ph=switching   # the journal without its state file: undo it to be safe
  case "$_ph" in
    switching)
      su_tty "the last update was interrupted while switching: undoing it"
      su_undo_plan; [ -d "$ST" ] && su_reject "$ST" "interrupted while switching (power cut?): undone, copy the bundle again"
      su_state_clear; su_status "an update was interrupted while switching files; it was undone (the previous set is in place)";;
    rollingback) su_rollback "finishing an interrupted rollback";;
    committing) su_commit;;
    trying) su_trying;;
    '') ;;
    *) su_log "unknown update state '$_ph' (written by a newer updater?): left alone"; exit 0;;
  esac
  if [ "$SU_REBOOT" = 1 ]; then exit 10; fi
  # one transition per boot: a rollback or a pending update waits for the next boot
  [ -f "$U/state" ] && exit 0
  case "$_ph" in switching|rollingback) exit 0;; esac
  # only the installed sysupdate.sh activates new sets (update/engine.sh only finishes the update that wrote it)
  [ "$SU_SELF" = "$G/sysupdate.sh" ] || exit 0
  # leftovers of an earlier run
  rm -rf "$PREV" "$U/p"; rm -f "$U/kernel.prev" "$U/engine.sh" "$U/plan" "$U/plan.part" "$U/staged.name.old"
  if [ ! -d "$ST" ] || [ ! -f "$ST/manifest" ]; then
    rm -rf "$ST"
    su_pick_inbox || exit 0
  fi
  if su_activate; then
    [ "$SU_REBOOT" = 1 ] && exit 10
    exit 0
  fi
  su_tty "update refused: $SU_ERR"
  su_status "update refused: $SU_ERR"
  rm -f "$U/plan.part"
  su_fail_cleanup
  su_reject "$ST" "$SU_ERR"
  rm -rf "$U/p"; rm -f "$U/staged.name"
  exit 0
}

# ---------------------------------------------------------------------------------------------------------------- Android side
su_gbos() { G=${1:-$SU_PI/gbos}; [ "$G" = "$SU_PI/gbos" ] && [ -n "${SU_GBOS_RW:-}" ] && G=$SU_GBOS_RW; U=$G/update; INBOX=$G/update.inbox; ST=$G/update.staging; PREV=$G/update.prev; REJ=$G/update.rejected; }
su_need_root() { [ -n "$SU_TEST" ] || [ "$(id -u)" = 0 ] || su_die "run as root (su -c sh $0 ...)"; }
su_progress() { [ -n "${SU_STATUSF:-}" ] && { echo "$*" > "$SU_STATUSF.part" && mv -f "$SU_STATUSF.part" "$SU_STATUSF"; }; echo "$*"; }

# su_copy_checked SRC DST: open SRC once and check that the open file is SRC itself (no link anywhere on the way: realpath and the
# fd's target agree, one hard link), then copy from that descriptor. A file swapped after the check cannot be read.
su_copy_checked() {
  _cs=$1; _cd=$2
  [ -f "$_cs" ] && [ ! -L "$_cs" ] || { SU_ERR="not a plain file: $_cs"; return 1; }
  _rp=$(realpath "$_cs" 2>/dev/null)
  [ "$_rp" = "$_cs" ] || { SU_ERR="path goes through a link: $_cs"; return 1; }
  exec 4< "$_cs" || { SU_ERR="cannot open $_cs"; return 1; }
  _fd=$(readlink "/proc/$$/fd/4" 2>/dev/null)
  if [ "$_fd" != "$_cs" ] || [ "$(stat -L -c %h "/proc/$$/fd/4" 2>/dev/null)" != 1 ]; then exec 4<&-; SU_ERR="file changed or has other links: $_cs"; return 1; fi
  mkdir -p "$(dirname "$_cd")"
  # In chunks, each flushed (conv=fsync) before the next is read: one `cat` of R5's 3.7 GB super.img filled memory with dirty pages
  # the stick could not take fast enough, and lmkd killed the launcher a minute into staging (boot 18, 2026-11-24). Every 8 chunks
  # the clean page cache is dropped as well, so the copy (source and target) does not push apps' pages out.
  : > "$_cd.part"; _r=$?; _cn=0
  while [ $_r = 0 ]; do
    _cb=$(su_size "$_cd.part")
    dd bs=1048576 count=$SU_CHUNK_MB iflag=fullblock conv=fsync <&4 >> "$_cd.part" 2>/dev/null || { _r=1; break; }
    _ca=$(su_size "$_cd.part")
    [ "$((_ca - _cb))" -lt $((SU_CHUNK_MB * 1048576)) ] && break   # end of file
    _cn=$((_cn + 1)); [ $((_cn % 8)) = 0 ] && su_dropclean
  done
  exec 4<&-
  [ $_r = 0 ] && mv -f "$_cd.part" "$_cd" || { rm -f "$_cd.part"; SU_ERR="copy failed: $_cs"; return 1; }
}

cmd_stage() {
  _src=; _prune=0; SU_STATUSF=; _g=
  while [ $# -gt 0 ]; do
    case "$1" in --gbos) _g=$2; shift 2;; --prune-logs) _prune=1; shift;; --status) SU_STATUSF=$2; shift 2;; -*) su_die "stage: unknown option $1";; *) _src=$1; shift;; esac
  done
  [ -n "$_src" ] || su_die "usage: stage BUNDLE_FOLDER|BUNDLE.tar [--gbos DIR] [--prune-logs]"
  su_need_root; su_gbos "$_g"; su_rw_check "$G"; su_tmp
  [ -d "$G" ] || su_die "no gbos folder at $G"
  [ -f "$U/state" ] && su_die "an update is already being installed or tried (gbos/update/state); wait for it to finish"
  _src=$(realpath "$_src" 2>/dev/null) || su_die "no such bundle: $1"
  case "$_src" in "$G"/*) su_die "the bundle is already on GBOSSYS; stage copies from elsewhere (move it into update.inbox/ yourself instead)";; esac
  if [ -f "$_src" ]; then   # a tar of the bundle folder: list it, then unpack it on /data and stage from there
    case "$_src" in *.tar) ;; *) su_die "not a folder or .tar: $_src";; esac
    exec 4< "$_src" || su_die "cannot open $_src"
    tar -tvf - <&4 > "$W/tarlist" 2>/dev/null || su_die "not a readable tar: $_src"
    exec 4<&-
    awk '$1 !~ /^[-d]/ { bad = 1 } END { exit bad }' "$W/tarlist" || su_die "the tar has links or special files"
    awk '{ print $NF }' "$W/tarlist" | grep -vqE '^[A-Za-z0-9._+,@=~/-]+$' && su_die "the tar has names that are not allowed"
    awk '{ print $NF }' "$W/tarlist" | grep -qE '(^|/)\.\.(/|$)|^/' && su_die "the tar has .. or absolute paths"
    _top=$(awk '{ print $NF }' "$W/tarlist" | cut -d/ -f1 | sort -u)
    [ "$(echo "$_top" | wc -l)" = 1 ] || su_die "the tar must hold exactly one bundle folder"
    _need=$(awk '{ s += $3 } END { print int(s / 1024) + 1 }' "$W/tarlist")
    _fr=$(su_free_kb "$W"); [ "${_fr:-0}" -gt "$_need" ] || su_die "not enough space to unpack the tar in $W ($_need KB needed)"
    mkdir -p "$W/untar"; exec 4< "$_src"; (cd "$W/untar" && tar -xf - <&4) || su_die "unpacking the tar failed"; exec 4<&-
    _src=$W/untar/$_top
  fi
  [ -d "$_src" ] || su_die "not a folder: $_src"
  su_progress "checking the signature"
  su_verify_sig_dir "$_src" || su_die "$SU_ERR"
  P=$W/s/p
  _seq=$(su_mval "$P" seq); _name=gbos86-update-$(printf '%012d' "$_seq")-$(echo "$SU_ID" | cut -c1-12)
  # which listed files to copy: present in the bundle, and not already installed with the same hash
  su_progress "comparing the bundle with the installed files (up to a minute)"
  : > "$W/copy"; : > "$W/lack"
  while read -r _h _s _p; do
    if [ ! -e "$_src/files/$_p" ]; then   # not in the bundle: the installed copy must already be the set's (as the boot check demands)
      [ "$(su_size "$G/$_p")" = "$_s" ] && [ "$(su_sha "$G/$_p")" = "$_h" ] || echo "$_p" >> "$W/lack"
      continue
    fi
    if [ "$(su_size "$G/$_p")" = "$_s" ] && [ "$(su_sha "$G/$_p")" = "$_h" ]; then continue; fi
    echo "$_h $_s $_p" >> "$W/copy"
  done < "$P/f"
  if [ -s "$W/lack" ]; then
    # exit 3: the app then takes the full bundle (or, for super.img, rebuilds it from Google's download)
    su_say "NEEDS-FILES: this installed system lacks $(wc -l < "$W/lack") file(s) of the set that the bundle does not include: $(head -n 5 "$W/lack" | tr '\n' ' ')"
    su_say "use a full bundle (or the stick's menu 1)"
    exit 3
  fi
  _k=
  if [ -e "$_src/files/EFI/BOOTX64.EFI" ]; then _k=1; fi
  _need=$(awk '{ s += $2 } END { print int(s / 1024) + 1 }' "$W/copy")
  [ -n "$_k" ] && _need=$(( _need + $(su_kb "$(cut -d' ' -f2 "$P/k")") ))
  _need=$(( _need + SU_MARGIN_KB ))
  _free=$(su_free_kb "$G")
  if [ "${_free:-0}" -lt "$_need" ] && [ $_prune = 1 ]; then
    for _b in $(ls "$G/logs" 2>/dev/null | grep '^boot-[0-9]*$' | sort -t- -k2 -n | awk -v k=$SU_KEEP_LOG_BOOTS '{ l[NR] = $0 } END { for (i = 1; i <= NR - k; i++) print l[i] }'); do
      [ "$(su_free_kb "$G")" -ge "$_need" ] && break
      su_progress "deleting old logs: logs/$_b"; rm -rf "$G/logs/$_b"; sync
    done
    _free=$(su_free_kb "$G")
  fi
  if [ "${_free:-0}" -lt "$_need" ]; then
    _lk=$(du -sk "$G/logs" 2>/dev/null | cut -f1)
    su_die "not enough space on GBOSSYS: the update needs $((_need / 1024)) MB, $((${_free:-0} / 1024)) MB are free (old logs use $((${_lk:-0} / 1024)) MB: copy them off and delete them, use --prune-logs, or update with the stick's menu 1)"
  fi
  mkdir -p "$INBOX"; rm -rf "$INBOX/$_name.part"; mkdir -p "$INBOX/$_name.part/files"
  cp "$W/s/manifest" "$INBOX/$_name.part/manifest" && cp "$W/s/manifest.sig" "$INBOX/$_name.part/manifest.sig" || su_die "cannot write to $INBOX"
  _tot=$(wc -l < "$W/copy"); _i=0
  while read -r _h _s _p; do
    _i=$((_i + 1)); su_progress "copying $_i of $_tot: $_p"
    su_copy_checked "$_src/files/$_p" "$INBOX/$_name.part/files/$_p" || { rm -rf "$INBOX/$_name.part"; su_die "$SU_ERR"; }
    [ "$(su_sha "$INBOX/$_name.part/files/$_p")" = "$_h" ] || { rm -rf "$INBOX/$_name.part"; su_die "hash mismatch after copying $_p"; }
    # big files: record the check, so the next start does not hash them again (su_skip_checked); drop the hash's read cache
    if [ "$_s" -gt $SU_CHECKED_MIN ]; then
      echo "$_h $_s $(su_tailsha "$INBOX/$_name.part/files/$_p") files/$_p" >> "$INBOX/$_name.part/$SU_CHECKED"; su_dropclean
    fi
  done < "$W/copy"
  # folders the set declares (so the walk at boot finds only declared ones)
  if [ -n "$_k" ]; then
    su_copy_checked "$_src/files/EFI/BOOTX64.EFI" "$INBOX/$_name.part/files/EFI/BOOTX64.EFI" || { rm -rf "$INBOX/$_name.part"; su_die "$SU_ERR"; }
    [ "$(su_sha "$INBOX/$_name.part/files/EFI/BOOTX64.EFI")" = "$(cut -d' ' -f1 "$P/k")" ] || { rm -rf "$INBOX/$_name.part"; su_die "hash mismatch after copying the kernel"; }
  fi
  sync
  rm -rf "$INBOX/$_name"; mv "$INBOX/$_name.part" "$INBOX/$_name" || su_die "cannot rename the copy in $INBOX"
  sync
  su_progress "staged $_name ($_tot files). Restart the laptop to install it (it is checked again before anything changes)."
}
su_verify_sig_dir() { # BUNDLE: signature + manifest on private copies (stage), no walk (extra files in a download folder are ignored)
  mkdir -p "$W/s"
  cp "$1/manifest" "$W/s/manifest" 2>/dev/null && cp "$1/manifest.sig" "$W/s/manifest.sig" 2>/dev/null || { SU_ERR="no manifest/manifest.sig in $1"; return 1; }
  su_verify_sig "$W/s/manifest" "$W/s/manifest.sig" "$G/sysupdate.pub" || return 1
  su_parse_manifest "$W/s/manifest" "$W/s/p" || return 1
  SU_ID=$(su_sha "$W/s/manifest")
}

cmd_cancel() {
  su_need_root; su_gbos "${1:-}"
  [ -d "$INBOX" ] || { echo "nothing waiting in update.inbox"; return 0; }
  su_rw_check "$G"
  _n=0; for _d in "$INBOX"/*; do [ -e "$_d" ] || continue; rm -rf "$_d"; _n=$((_n + 1)); done; sync
  echo "removed $_n waiting bundle(s); an update that is already installed is not touched"
}

# audit [G] [--deep]: is the installed system exactly the set its system.manifest names? The app's "modifications" check.
# Prints key=value lines, then up to 60 "changed|missing|added PATH" lines. Files over 64 MiB (super.img) are compared by size
# unless --deep. gbos.cfg, logs and update folders are never part of a set, so editing gbos.cfg is not a modification. "added" files sit
# in the set's own folders: an update deletes them. The kernel on the EFI partition is not checked from Android.
cmd_audit() {
  _ag=; _deep=0
  while [ $# -gt 0 ]; do case "$1" in --deep) _deep=1; shift;; -*) su_die "audit: unknown option $1";; *) _ag=$1; shift;; esac; done
  su_gbos "$_ag"; su_tmp
  _m=$G/system.manifest
  if [ ! -f "$_m" ]; then
    echo "manifest=none"; echo "source=unknown"; echo "modified=unknown"; return 0
  fi
  if ! su_parse_manifest "$_m" "$W/a"; then
    echo "manifest=bad"; echo "reason=$(echo "$SU_ERR" | head -n 1)"; echo "source=unknown"; echo "modified=unknown"; return 0
  fi
  _c=$(su_mval "$W/a" commit)
  case "$_c" in 0000000000000000000000000000000000000000) _src=unknown;; *-dirty) _src=dirty;; *) _src=git;; esac
  echo "manifest=ok"; echo "set=$(su_sha "$_m")"; echo "seq=$(su_mval "$W/a" seq)"; echo "commit=$_c"; echo "source=$_src"
  echo "google_build=$(su_mval "$W/a" google_build)"
  : > "$W/a/chk"; : > "$W/a/res"
  while read -r _h _s _p; do
    if [ ! -f "$G/$_p" ]; then echo "missing $_p" >> "$W/a/res"; continue; fi
    [ "$(su_size "$G/$_p")" = "$_s" ] || { echo "changed $_p" >> "$W/a/res"; continue; }
    [ $_deep = 0 ] && [ "$_s" -gt 67108864 ] && continue
    echo "$_h  $_p" >> "$W/a/chk"
  done < "$W/a/f"
  [ -s "$W/a/chk" ] && (cd "$G" && sha256sum -c "$W/a/chk" 2>/dev/null) | sed -n 's/^\(.*\): FAILED$/changed \1/p' >> "$W/a/res"
  while read -r _t; do [ -d "$G/$_t" ] && (cd "$G" && find "$_t" -type f); done < "$W/a/top" \
    | awk 'FILENAME == ARGV[1] { split($0, a, " "); w[a[3]] = 1; next } !($0 in w) { print "added " $0 }' "$W/a/f" - >> "$W/a/res"
  _ch=$(grep -c '^changed ' "$W/a/res"); _mi=$(grep -c '^missing ' "$W/a/res"); _ad=$(grep -c '^added ' "$W/a/res")
  echo "checked=$(wc -l < "$W/a/f")"; echo "changed=$_ch"; echo "missing=$_mi"; echo "added=$_ad"; echo "deep=$_deep"
  if [ $((_ch + _mi + _ad)) = 0 ]; then echo "modified=no"; else echo "modified=yes"; fi
  sort "$W/a/res" | head -n 60
  return 0
}

# info [G]: machine-readable facts for the app (the set, the public key it checks the update feed with, free space, pending update).
cmd_info() {
  su_gbos "${1:-}"
  echo "gbos86-sysupdate-info 1"
  if [ -f "$G/system.manifest" ]; then
    echo "set=$(su_sha "$G/system.manifest")"; echo "seq=$(su_manifest_field "$G/system.manifest" seq)"
    echo "commit=$(su_manifest_field "$G/system.manifest" commit)"
    echo "super=$(awk '$1 == "f" && $4 == "super.img" { print $2; exit }' "$G/system.manifest")"
    _ib=$(su_manifest_field "$G/system.manifest" google_build)
  else
    echo "set=none"; echo "seq=0"; echo "commit=unknown"; echo "super="; _ib=
  fi
  [ -n "$_ib" ] || _ib=$(su_fp_build "$(getprop ro.system.build.fingerprint 2>/dev/null)")
  [ -n "$_ib" ] || _ib=$(su_fp_build "$(sed -n 's/^# build //p' "$G/patched/MANIFEST" 2>/dev/null | head -n 1)")
  echo "google_build=${_ib:-0}"
  _k=$(su_pub_modulus "$G/sysupdate.pub" 2>/dev/null) && echo "pub=$_k" || echo "pub=none"
  echo "free_kb=$(su_free_kb "$G")"
  echo "logs_kb=$(du -sk "$G/logs" 2>/dev/null | cut -f1)"
  if [ -f "$U/state" ]; then echo "pending=$(su_st phase)"; echo "pending_seq=$(su_st seq)"; else echo "pending=none"; fi
  echo "inbox=$(ls "$INBOX" 2>/dev/null | grep '^gbos86-update-' | grep -v '\.part$' | tr '\n' ' ')"
  grep -q '^\[ -f /gbos/gbos/early_patch.sh \] && \. /gbos/gbos/early_patch.sh' "$G/gbos.cfg" 2>/dev/null && echo "hook=ok" || echo "hook=missing"
  [ -f "$U/status.txt" ] && echo "last=$(tail -n 1 "$U/status.txt" | tr -cd '\40-\176')"
  return 0
}

cmd_status() {
  su_gbos "${1:-}"
  echo "installed set: $(cmd_setinfo "$G" | tr '\n' ' ')"
  su_pub_modulus "$G/sysupdate.pub" >/dev/null && echo "signing key: installed (sha256 of the key file $(su_sha "$G/sysupdate.pub" | cut -c1-16))" \
    || echo "signing key: NONE (every update is refused until gbos/sysupdate.pub holds the project's public key)"
  grep -q '^\[ -f /gbos/gbos/early_patch.sh \] && \. /gbos/gbos/early_patch.sh' "$G/gbos.cfg" 2>/dev/null \
    && echo "boot hook: gbos.cfg runs early_patch.sh" || echo "boot hook: MISSING (gbos.cfg does not source early_patch.sh): update once with the stick's menu 1"
  if [ -f "$U/state" ]; then echo "pending: $(grep -v '^gbos86' "$U/state" | tr '\n' ' ')"; else echo "pending: nothing"; fi
  echo "waiting in update.inbox: $(ls "$INBOX" 2>/dev/null | tr '\n' ' ')"
  [ -f "$U/status.txt" ] && { echo "last result:"; sed 's/^/  /' "$U/status.txt"; }
  return 0
}

# One request from the gbOS86 Updater app. init starts this (seclabel u:r:shell:s0, as root) when the app sets the property
# sys.gbos86.sysupdate to "<android user id>.<counter>". The request itself is read from the app's private folder, which only the app
# (and root) can write, and it is deleted after reading, so another app setting the property can at most re-run nothing.
cmd_request() {
  su_gbos ""; su_tmp
  _pv=$(getprop sys.gbos86.sysupdate 2>/dev/null)
  _u=${_pv%%.*}
  case "$_u" in ''|*[!0-9]*) su_die "bad request property '$_pv'";; esac
  _ad=/data/user/$_u/$SU_PKG
  [ -d "$_ad" ] && [ ! -L "$_ad" ] || su_die "the app is not installed for user $_u"
  _uid=$(stat -c %u "$_ad")
  [ "$_uid" -ge 10000 ] 2>/dev/null || su_die "unexpected owner of $_ad"
  _rq=$_ad/files/request.txt; _rs=$_ad/files/response.txt
  [ -f "$_rq" ] && [ ! -L "$_rq" ] && [ "$(stat -c %u "$_rq")" = "$_uid" ] || su_die "no request from the app"
  head -c 4096 "$_rq" > "$W/req"; rm -f "$_rq"
  [ "$(head -n 1 "$W/req")" = "gbos86-sysupdate-request 1" ] || su_die "bad request"
  _id=$(sed -n 's/^id \([0-9]\{1,12\}\)$/\1/p' "$W/req" | head -n 1)
  _act=$(sed -n 's/^action \([a-z-]*\)$/\1/p' "$W/req" | head -n 1)
  _rel=$(sed -n 's/^path \([A-Za-z0-9._+,@=~/ -]*\)$/\1/p' "$W/req" | head -n 1)
  _pr=$(sed -n 's/^prune_logs \([01]\)$/\1/p' "$W/req" | head -n 1)
  _wh=$(sed -En 's/^where (app|shared)$/\1/p' "$W/req" | head -n 1)
  _dp=$(sed -n 's/^deep \([01]\)$/\1/p' "$W/req" | head -n 1)
  # the app's modification warning the user accepted with "Install anyway" (codes: Updater.warningCodes), kept in update/history.txt
  _aw=$(sed -n 's/^accepted_warning \([a-z,]\{1,100\}\)$/\1/p' "$W/req" | head -n 1)
  SU_STATUSF=$W/progress
  respond() { { echo "gbos86-sysupdate-response 1"; echo "id $_id"; echo "state $1"; shift; cat; } > "$_rs.part"; chown "$_uid:$_uid" "$_rs.part"; chmod 600 "$_rs.part"; mv -f "$_rs.part" "$_rs"; }
  echo "working" | respond running
  case "$_act" in
    status) cmd_status 2>&1 | respond done;;
    info) cmd_info 2>&1 | respond done;;
    audit) ( set -- audit; [ "$_dp" = 1 ] && set -- "$@" --deep; if [ -n "${SU_BB:-}" ]; then "$SU_BB" sh "$0" "$@"; else sh "$0" "$@"; fi ) 2>&1 | respond done;;
    cancel) ( if [ -n "${SU_BB:-}" ]; then "$SU_BB" sh "$0" cancel; else sh "$0" cancel; fi ) 2>&1 | respond done;;
    reboot) echo "restarting" | respond done; sync; setprop sys.powerctl reboot,gbos-sysupdate;;
    stage)
      case "/$_rel/" in */../*|*/./*|//) echo "bad path" | respond failed; exit 1;; esac
      # where: shared = this user's shared storage (a folder picked in the app); app = the app's own files/ (its downloads)
      if [ "$_wh" = app ]; then _base=$(realpath "$_ad/files" 2>/dev/null); else _base=/data/media/$_u; fi
      _real=$(realpath "$_base/$_rel" 2>/dev/null)
      [ -n "$_base" ] || _real=
      case "$_real" in "$_base"/?*) ;; *) echo "the bundle must be in this user's shared storage or the app's own folder" | respond failed; exit 1;; esac
      ( while sleep 1; do [ -f "$W/progress" ] && respond running < "$W/progress"; done ) & _pp=$!
      _o=$( ( set -- stage "$_real" --status "$W/progress"; [ "$_pr" = 1 ] && set -- "$@" --prune-logs; if [ -n "${SU_BB:-}" ]; then "$SU_BB" sh "$0" "$@"; else sh "$0" "$@"; fi ) 2>&1); _r=$?
      kill "$_pp" 2>/dev/null; wait "$_pp" 2>/dev/null
      if [ $_r = 0 ] && [ -n "$_aw" ]; then
        su_log "staged although the app warned that the system is modified ($_aw); the user chose to install anyway"
        mkdir -p "$U"
        { tail -n 199 "$U/history.txt" 2>/dev/null
          echo "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null) app: update staged from $(basename "$_real") after the app warned that this system is modified ($_aw); the user chose \"Install anyway\""
        } > "$U/history.part" && mv -f "$U/history.part" "$U/history.txt"; sync
      fi
      [ $_r = 0 ] && su_record_source "$_real" "$_base"
      if [ $_r = 0 ]; then echo "$_o" | tail -n 3 | respond done; else echo "$_o" | tail -n 5 | respond failed; fi;;
    cleanup) ( set -- cleanup "$G"; if [ -n "${SU_BB:-}" ]; then "$SU_BB" sh "$0" "$@"; else sh "$0" "$@"; fi ) 2>&1 | respond done;;
    *) echo "unknown action '$_act'" | respond failed;;
  esac
}

# The bundle folder an update was staged from (the app's download, or a folder in the user's shared storage), kept by root on /data
# (GBOSSYS's stage0 cannot read Android's encrypted storage). After that set is committed, `cleanup` deletes the folder from the
# laptop's own storage: the update finished, the copy on GBOSSYS was installed, and the bundle is only taking space.
SU_SRCREC=${SU_SRCREC:-/data/misc/gbos86/staged-source}
su_record_source() { # REAL_BUNDLE_DIR BASE_DIR
  mkdir -p "${SU_SRCREC%/*}" 2>/dev/null && chmod 700 "${SU_SRCREC%/*}" 2>/dev/null
  { echo "source $1"; echo "base $2"; echo "set $(su_sha "$1/manifest")"; } > "$SU_SRCREC.part" 2>/dev/null &&
    chmod 600 "$SU_SRCREC.part" && mv -f "$SU_SRCREC.part" "$SU_SRCREC" || su_log "could not record the staged bundle's folder"
}
# cleanup [G]: delete the recorded bundle folder once its set is the installed one (system.manifest) and no update is being tried.
# Deletes only a gbos86-update-* folder under the recorded base whose manifest is still that set; otherwise it leaves it alone.
cmd_cleanup() {
  su_gbos "${1:-}"; U=$G/update
  [ -f "$SU_SRCREC" ] || { echo "cleanup: nothing recorded"; return 0; }
  _cs=$(sed -n 's/^source //p' "$SU_SRCREC" | head -n 1); _cb=$(sed -n 's/^base //p' "$SU_SRCREC" | head -n 1)
  _cset=$(sed -n 's/^set \([0-9a-f]\{64\}\)$/\1/p' "$SU_SRCREC" | head -n 1)
  if [ -f "$U/state" ]; then echo "cleanup: an update is being tried; kept $(basename "$_cs")"; return 0; fi
  if ls "$G/update.inbox" 2>/dev/null | grep -q '^gbos86-update-'; then echo "cleanup: an update waits in update.inbox; kept $(basename "$_cs")"; return 0; fi
  _ci=$([ -f "$G/system.manifest" ] && su_sha "$G/system.manifest")
  if [ -z "$_cset" ] || [ "$_cset" != "$_ci" ]; then
    rm -f "$SU_SRCREC"; echo "cleanup: $(basename "$_cs") was not installed (rolled back or replaced); kept it"; return 0
  fi
  case "$(basename "$_cs")" in gbos86-update-*) ;; *) rm -f "$SU_SRCREC"; echo "cleanup: $_cs is not an update folder; kept it"; return 0;; esac
  case "$_cs" in "$_cb"/?*) ;; *) rm -f "$SU_SRCREC"; echo "cleanup: $_cs is outside $_cb; kept it"; return 0;; esac
  if [ -d "$_cs" ] && [ ! -L "$_cs" ]; then
    if [ "$(su_sha "$_cs/manifest" 2>/dev/null)" = "$_cset" ]; then
      rm -rf "$_cs"; su_log "cleanup: deleted $_cs (set ${_cset%"${_cset#????????????}"} is installed)"
      echo "cleanup: deleted $(basename "$_cs") from this laptop's storage (the update is installed)"
    else
      echo "cleanup: $(basename "$_cs") changed since it was staged; kept it"
    fi
  else
    echo "cleanup: $(basename "$_cs") is already gone"
  fi
  rm -f "$SU_SRCREC"
  return 0
}

# init, at boot_completed (hook.sh adds the service when gbos/gbos86-updater.apk exists): install or update the gbOS86 Updater app
# with pm, like the KernelSU manager. Skipped when the same APK (by sha256) was installed before. Log: logs/boot-N/updater-app.txt.
cmd_install_app() {
  su_gbos "${1:-}"; _n=${2:-0}
  _apk=$G/gbos86-updater.apk; _log=$G/logs/boot-$_n/updater-app.txt; _stamp=/data/local/tmp/.gbos86-updater.sha256
  [ -f "$_apk" ] || exit 0
  { echo "mounts: $SU_PI: $(su_mntline "$SU_PI")"; echo "mounts: $SU_RW_DIR/stick: $(su_mntline "$SU_RW_DIR/stick")"; } >> "$_log" 2>/dev/null
  _h=$(su_sha "$_apk")
  if pm path "$SU_PKG" >/dev/null 2>&1 && [ "$(cat "$_stamp" 2>/dev/null)" = "$_h" ]; then echo "updater app $_h already installed" >> "$_log"
  else
    cp "$_apk" /data/local/tmp/gbos86-updater.apk || exit 1
    _new=1; pm path "$SU_PKG" >/dev/null 2>&1 && _new=0
    _o=$(pm install -r /data/local/tmp/gbos86-updater.apk 2>&1)
    echo "pm install gbos86-updater.apk ($_h): $_o" >> "$_log"
    rm -f /data/local/tmp/gbos86-updater.apk
    case "$_o" in *Success*)
      echo "$_h" > "$_stamp"
      # first install only (a later "don't allow" stays): let it show the "update available" notification
      [ $_new = 1 ] && pm grant "$SU_PKG" android.permission.POST_NOTIFICATIONS >> "$_log" 2>&1;;
    esac
  fi
  # Exempt it from app standby and doze, every start (idempotent; the list lives in /data/system/deviceidle.xml). Installed by pm and
  # never opened, it stayed in the NEVER standby bucket, so its jobs (the check after start, the cleanup) did not run on time
  # (JobScheduler "still in NEVER bucket", "0 EJ quota" after each reinstall, boots 16-19, 2026-11-24).
  cmd deviceidle whitelist +"$SU_PKG" >> "$_log" 2>&1 || echo "deviceidle whitelist +$SU_PKG failed" >> "$_log"
  # the app's update check at startup (its own BOOT_COMPLETED receiver does not run until the app was opened once). The app decides
  # (Settings: "Check when the laptop starts") and waits for the network itself.
  am broadcast -a org.gbos86.updater.BOOT_CHECK -n "$SU_PKG/.BootReceiver" -f 0x20 >> "$_log" 2>&1
  return 0
}

# init, at boot_completed, only while an update is being tried (hook.sh adds the service then): wait a minute so a crash loop right
# after boot does not count as success, then record the set id.
cmd_boot_ok() {
  su_gbos "${1:-}"
  [ -f "$U/state" ] || exit 0
  _s=$(su_st set); [ -n "$_s" ] || exit 0
  [ -n "$SU_TEST" ] || sleep 60
  [ -n "$SU_TEST" ] || [ "$(getprop sys.boot_completed 2>/dev/null)" = 1 ] || exit 0
  su_rw_check "$G"
  echo "$_s" > "$U/boot-ok.part" && sync && mv -f "$U/boot-ok.part" "$U/boot-ok" && sync
  # One restart is enough: the tried set reached boot_completed, so commit it now instead of at the next start. A failed tried boot
  # never gets here (no boot-ok), so the next start still rolls back. If this is cut short, phase=committing / boot-ok make the next
  # start's stage0 finish the commit. A set held after failed tries (new Google build) commits the same way once it does start.
  [ "$(su_st phase)" = trying ] || exit 0
  BN=$(su_st boots); BN=${BN##* }
  su_commit
  [ -n "$SU_TEST" ] || am broadcast -a org.gbos86.updater.BOOT_CHECK -n "$SU_PKG/.BootReceiver" -f 0x20 >/dev/null 2>&1   # the app: show it, clean up
  return 0
}

# ---------------------------------------------------------------------------------------------------------------- menu 1 (install.sh)
# The files whose versions must belong to the same Google build as super.img.
SU_PAIRED="super.layout libkm_compat.so system_ext_manifest.xml patched/MANIFEST"
cmd_menu1_check() { # STICKG NVMEG: key=value lines for install.sh
  _sg=$1; _ng=$2
  _sb=$(su_set_build "$_sg" 14); _nb=$(su_set_build "$_ng" 15)
  echo "stick_build=${_sb:-unknown}"; echo "nvme_build=${_nb:-unknown}"
  _dg=0; [ -n "$_sb" ] && [ -n "$_nb" ] && [ "$_sb" -lt "$_nb" ] && _dg=1
  echo "downgrade=$_dg"
  _pc=0; for _f in $SU_PAIRED; do cmp -s "$_sg/$_f" "$_ng/$_f" || _pc=1; done
  echo "pairing_changed=$_pc"
  # stamps are trusted only while the installed set is the one they were saved for
  _cur=none; [ -f "$_ng/system.manifest" ] && _cur=$(su_sha "$_ng/system.manifest")
  _tr=0; [ "$(cat "$_ng/.stamps/set" 2>/dev/null)" = "$_cur" ] && _tr=1
  echo "stamps_trusted=$_tr"
  _pend=0; [ -f "$_ng/update/state" ] && _pend=1; echo "update_pending=$_pend"
  _inb=$(ls "$_ng/update.inbox" 2>/dev/null | grep -c '^gbos86-update-'); echo "inbox=${_inb:-0}"
  echo "stick_set=$(cmd_setinfo "$_sg" 14 | grep -v '^google_build' | tr '\n' ' ')"
  echo "nvme_set=$(cmd_setinfo "$_ng" 15 | grep -v '^google_build' | tr '\n' ' ')"
}
cmd_menu1_finish() { # STICKG NVMEG: after menu 1 copied and verified the stick's set
  _sg=$1; _ng=$2
  # the menu-1 set replaces whatever an update did: forget the update's state, backups and kernel copy
  rm -rf "$_ng/update.prev" "$_ng/update.staging"
  rm -f "$_ng/update/state" "$_ng/update/state.part" "$_ng/update/plan" "$_ng/update/plan.part" "$_ng/update/engine.sh" \
    "$_ng/update/kernel.prev" "$_ng/update/boot-ok" "$_ng/update/android-started" "$_ng/update/rollback-request"
  _ok=0
  if [ -f "$_sg/system.manifest" ]; then
    _ok=1
    for _f in $SU_PAIRED; do
      _want=$(awk -v p="$_f" '$1 == "f" && $4 == p { print $2 }' "$_sg/system.manifest")
      [ -n "$_want" ] && [ "$(su_sha "$_sg/$_f")" = "$_want" ] || _ok=0
    done
    _ws=$(awk '$1 == "f" && $4 == "super.img" { print $3 }' "$_sg/system.manifest")
    [ "$_ws" = "$(su_size "$_sg/super.img")" ] || _ok=0
  fi
  if [ $_ok = 1 ]; then cp "$_sg/system.manifest" "$_ng/system.manifest" && echo "system.manifest: copied (the stick's set)"
  else rm -f "$_ng/system.manifest"; echo "system.manifest: none (the stick has none, or it does not describe the stick's files)"; fi
  mkdir -p "$_ng/update"
  { echo "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null) stick menu 1"; echo "the system was updated from the USB stick (menu 1)"; } > "$_ng/update/status.txt"
  sync
}
cmd_stamp_set() { # NVMEG: written with the stamps (install.sh stamp_save)
  mkdir -p "$1/.stamps"
  if [ -f "$1/system.manifest" ]; then su_sha "$1/system.manifest"; else echo none; fi > "$1/.stamps/set"
}

# ---------------------------------------------------------------------------------------------------------------- writing from Android
# Under the FAT lockdown (hook.sh gbos_postinstall_lockdown) Android's /postinstall is read-only, so stage, cancel and boot-ok, the
# Android-side writers of GBOSSYS, write somewhere else:
#   1. /dev/gbos-rw/stick: the volume read-write, made by stage0 inside /dev/gbos-rw (mode 0700 root: no app, no system_server can go
#      in; the bind's own root is the FAT's umask=000 root, so it must never be the 0700 directory itself). su_gbos maps G to it.
#   2. with an older hook.sh that has no /dev/gbos-rw: a mount namespace of its own where /postinstall is remounted read-write
#      (audio/audio-profile.sh does the same); apps never see it.
# Each run logs the mount state and the way it chose to logs/boot-N/sysupdate.txt, and su_rw_check refuses with the reason when the
# target still cannot be written (also when the kernel made the FAT itself read-only after a FAT error: then nothing can write it).
# SU_MOUNTINFO, SU_RW_DIR, SU_UNSHARE, SU_PI: tests only.
su_mnt() { # MOUNTPOINT: "<mount ro|rw> <filesystem ro|rw>" of the top mount there, from mountinfo; nothing when not a mount point
  awk -v m="$1" '$5 == m { for (i = 7; i <= NF && $i != "-"; i++); split($6, a, ","); split($(i + 3), b, ","); r = a[1] " " b[1] }
    END { if (r != "") print r }' "${SU_MOUNTINFO:-/proc/self/mountinfo}" 2>/dev/null
}
su_mntline() { awk -v m="$1" '$5 == m { r = $0 } END { print (r == "" ? "(not mounted)" : r) }' "${SU_MOUNTINFO:-/proc/self/mountinfo}" 2>/dev/null; }
su_rw_log() { # MESSAGE: one line to stderr and to this boot's logs/boot-N/sysupdate.txt (the only boot-N under the lockdown's tmpfs)
  echo "sysupdate: $*" >&2
  _ld=$SU_PI/gbos/logs; _bn=$(ls "$_ld" 2>/dev/null | sed -n 's/^boot-\([0-9][0-9]*\)$/\1/p' | sort -n | tail -n 1)
  [ -n "$_bn" ] && echo "$(date -u +%H:%M:%S 2>/dev/null) [$$] $*" >> "$_ld/boot-$_bn/sysupdate.txt" 2>/dev/null
  return 0
}
su_rw_state() { # CMD: the facts behind the choice, logged
  su_rw_log "$1: uid $(id -u 2>/dev/null) GBOS_STICK_RW=${GBOS_STICK_RW:-} $SU_PI: $(su_mntline "$SU_PI")"
  su_rw_log "$1: $SU_RW_DIR/stick: $(su_mntline "$SU_RW_DIR/stick")"
}
su_stick_rw() { # CMD ARGS...: choose how this command writes GBOSSYS (may re-run the script in its own mount namespace)
  _pm=$(su_mnt "$SU_PI")
  case "$_pm" in ''|'rw '*) return 0;; esac          # no lockdown (stage0, the PC, POSTINSTALL_RW=1): write directly
  su_rw_state "$1"
  [ -n "${GBOS_STICK_RW:-}" ] && { su_rw_log "$1: in its own mount namespace"; return 0; }   # su_rw_check tells whether it worked
  case "$_pm" in *' ro') su_rw_log "$1: the FAT itself is read-only (the kernel does this after a FAT error)"; return 0;; esac
  if [ "$(su_mnt "$SU_RW_DIR/stick")" = "rw rw" ] && [ -d "$SU_RW_DIR/stick/gbos" ]; then
    SU_GBOS_RW=$SU_RW_DIR/stick/gbos; su_rw_log "$1: writing through $SU_RW_DIR/stick (root only)"; return 0
  fi
  su_rw_log "$1: no $SU_RW_DIR/stick (older hook.sh): re-running in a mount namespace of its own"
  _sh=${SU_BB:-$SU_PI/busybox}
  exec env GBOS_STICK_RW=1 ${SU_UNSHARE:-unshare -m} "$_sh" sh -c \
    'mount -o remount,bind,rw "$1" || { echo "sysupdate: ERROR: remounting $1 read-write in a mount namespace of its own failed" >&2; exit 1; }; shift; exec "$0" sh "$@"' \
    "$_sh" "$SU_PI" "$0" "$@"
}
su_rw_check() { # G: refuse with the reason when G cannot be written (after su_gbos). A real write: busybox test -w is always true for root
  mkdir "$1/.sysupdate-rw.$$" 2>/dev/null && rmdir "$1/.sysupdate-rw.$$" && return 0
  _why="the system partition is read-only here"
  case "$(su_mnt "$SU_PI")" in *' ro') _why="the stick's FAT is read-only for everyone: the kernel made it read-only after a FAT error (FAT-fs lines in kmsg). Boot the stick and choose 1 (it checks the FAT), or check it with chkdsk";;
    'ro '*) [ -n "${GBOS_STICK_RW:-}" ] && _why="$SU_PI stayed read-only in the updater's own mount namespace" || _why="$SU_PI is read-only and $SU_RW_DIR/stick is missing";; esac
  su_rw_log "cannot write $1: $_why"; su_rw_state "check"
  su_die "cannot write $1: $_why (details: logs/boot-N/sysupdate.txt)"
}

# ---------------------------------------------------------------------------------------------------------------- main
_cmd=${1:-}; [ $# -gt 0 ] && shift
case "$_cmd" in stage|cancel|boot-ok) su_stick_rw "$_cmd" "$@";; esac
case "$_cmd" in
  boot) [ $# -ge 3 ] || su_die "usage: boot G LOGDIR BOOTN [--efi-dir DIR] [--dev DEV]"; cmd_boot "$@";;
  verify) [ $# -ge 1 ] || su_die "usage: verify BUNDLE [--pub FILE] [--installed G]"; cmd_verify "$@";;
  stage) cmd_stage "$@";;
  cancel) cmd_cancel "$@";;
  status) cmd_status "$@";;
  request) cmd_request "$@";;
  boot-ok) cmd_boot_ok "$@";;
  install-app) su_tmp; cmd_install_app "$@";;
  cleanup) cmd_cleanup "$@";;
  audit) cmd_audit "$@";;
  info) su_tmp; cmd_info "$@";;
  setinfo) su_tmp; cmd_setinfo "$@";;
  menu1-check) su_tmp; cmd_menu1_check "$@";;
  menu1-finish) su_tmp; cmd_menu1_finish "$@";;
  stamp-set) cmd_stamp_set "$@";;
  *) echo "usage: sh sysupdate.sh {stage BUNDLE|cancel|status|verify BUNDLE} (see the top of this file and docs/system-update.md)" >&2; exit 2;;
esac
