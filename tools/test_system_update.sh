#!/bin/sh
# Tests for the system updater (usb/sysupdate.sh, tools/make_system_update.sh, tools/verify_system_update.sh, the early_patch.sh /
# stage0_init.sh / hook.sh / install.sh wiring). No root, no laptop: real signed bundles made with a throwaway key, a fake installed
# system (gbos/ folder + EFI folder) and the stick's busybox for every applet the device code runs.
#   sh tools/test_system_update.sh              BUSYBOX=/path/busybox to choose the busybox (default: the main checkout's kernel/out/busybox)
# Prints SYSTEM_UPDATE_TEST_PASS at the end.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BB=${BUSYBOX:-}
if [ -z "$BB" ]; then
  _c=$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null || true)
  [ -n "$_c" ] && _m=$(cd "$ROOT" && cd "$_c/.." && pwd) && [ -x "$_m/kernel/out/busybox" ] && BB=$_m/kernel/out/busybox
  [ -n "$BB" ] || [ ! -x "$ROOT/kernel/out/busybox" ] || BB=$ROOT/kernel/out/busybox
  [ -n "$BB" ] || BB=$(command -v busybox || true)
fi
[ -n "$BB" ] && [ -x "$BB" ] || { echo "no busybox: set BUSYBOX=" >&2; exit 1; }
command -v openssl >/dev/null || { echo "needs openssl" >&2; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM
export BUSYBOX=$BB SU_BB=$BB SYSUPDATE_TEST=1 SYSUPDATE_KMSG=/dev/null TMPDIR=$T
FAILS=0
ok() { echo "ok - $*"; }
bad() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
SU() { "$BB" sh "$ROOT/usb/sysupdate.sh" "$@"; }
MK() { sh "$ROOT/tools/make_system_update.sh" "$@"; }

# ------------------------------------------------------------------------------------------------ wiring and syntax
for f in sysupdate.sh early_patch.sh hook.sh install.sh stage0_init.sh; do "$BB" sh -n "$ROOT/usb/$f" || bad "busybox sh -n usb/$f"; done
for f in make_system_update.sh verify_system_update.sh test_system_update.sh publish_system_update.sh; do sh -n "$ROOT/tools/$f" || bad "sh -n tools/$f"; done
check "early_patch.sh calls the updater hook" "grep -qx 'gbos_sysupdate_stage0' '$ROOT/usb/early_patch.sh'"
check "early_patch.sh runs the engine with a clean environment" "grep -q 'env -i PATH=/bin:/sbin sh \"\$1\" boot' '$ROOT/usb/early_patch.sh'"
check "the hook only acts in stage0 (PID 1) on the installed system" "grep -q '\[ \"\$\$\" = 1 \] && \[ -z \"\$GBOS_SYSUPDATE_RAN\" \] && \[ \"\$FROM\" = nvme \]' '$ROOT/usb/early_patch.sh'"
check "gbos.cfg sources early_patch.sh" "grep -q '^\[ -f /gbos/gbos/early_patch.sh \] && \. /gbos/gbos/early_patch.sh' '$ROOT/usb/gbos.cfg'"
_l1=$(grep -n '^\[ \$FROM = nvme \] && MODE=boot$' "$ROOT/usb/stage0_init.sh" | cut -d: -f1)
_l2=$(grep -n 'GBOS_SYSUPDATE_RAN' "$ROOT/usb/stage0_init.sh" | tail -n 1 | cut -d: -f1)
_l3=$(grep -n '^if \[ "\$HWDETECT" != 0 \]' "$ROOT/usb/stage0_init.sh" | cut -d: -f1)
check "stage0_init.sh fallback hook sits after FROM/MODE and before hwdetect" "[ -n '$_l1' ] && [ -n '$_l2' ] && [ -n '$_l3' ] && [ $_l1 -lt ${_l2:-0} ] && [ ${_l2:-0} -lt ${_l3:-0} ]"
check "install.sh ships sysupdate.sh and sysupdate.pub" "grep -q '^FILES=\".* sysupdate.sh sysupdate.pub\"' '$ROOT/usb/install.sh'"
for t in deploy_stick.sh make_gbos86_image.sh install_dualboot.sh; do check "tools/$t ships sysupdate.sh" "grep -q 'sysupdate.sh sysupdate.pub' '$ROOT/tools/$t'"; done
check "the repo's sysupdate.pub is the placeholder or a public key, never a private key" "! grep -q PRIVATE '$ROOT/usb/sysupdate.pub'"
# hook.sh's gbos.rc part: explicit seclabels, no "$" left for init
mkdir -p "$T/h/update"; touch "$T/h/sysupdate.sh" "$T/h/gbos86-updater.apk"; printf 'gbos86-update-state 1\nphase=trying\nset=abc\n' > "$T/h/update/state"
sed -n '/^gbos_sysupdate_rc() {/,/^}/p' "$ROOT/usb/hook.sh" > "$T/h/f.sh"
_rc=$(log() { :; }; G=$T/h; N=4; . "$T/h/f.sh"; gbos_sysupdate_rc; echo "$GBOS_SYSUPDATE_RC"; echo "START $GBOS_SYSUPDATE_START")
check "hook.sh adds the request, installer and boot-ok services" "echo \"\$_rc\" | grep -q '^service gbos_sysupdate_req ' && echo \"\$_rc\" | grep -q '^service gbos_updater_install ' && echo \"\$_rc\" | grep -q '^service gbos_sysupdate_ok '"
check "every updater service has a seclabel" "[ \$(echo \"\$_rc\" | grep -c '^service ') = \$(echo \"\$_rc\" | grep -c '^    seclabel u:r:shell:s0$') ]"
check "no \$ reaches init" "! echo \"\$_rc\" | grep -q '\\\$'"
check "hook.sh marks android-started while trying" "[ \"\$(cat $T/h/update/android-started)\" = abc ]"

# ------------------------------------------------------------------------------------------------ keys, source, tree
MK keygen "$T/keys" --no-passphrase >/dev/null || bad "keygen"
KEY=$T/keys/sysupdate-private.pem; PUB=$T/keys/sysupdate.pub
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$T/other.pem" 2>/dev/null
check "keygen refuses a folder inside a git repo" "! MK keygen '$ROOT/build/should-not-exist-key' --no-passphrase >/dev/null 2>&1 && [ ! -e '$ROOT/build/should-not-exist-key/sysupdate-private.pem' ]"
rmdir "$ROOT/build/should-not-exist-key" 2>/dev/null
FP() { echo "Android/desktop_system/desktop:17/CL3B.TEST/$1:user/release-keys"; }
mk_src() { # DIR BUILD TAG: a usb/-like source with install.sh's FILES/DIRS
  _s=$1/usb; rm -rf "$1"; mkdir -p "$_s/patched" "$_s/firmware/intel" "$_s/audio" "$_s/installer/bin" "$_s/kmod" "$_s/surface" "$_s/ksu" "$_s/out" "$1/lapis"
  cp "$ROOT/usb/install.sh" "$ROOT/usb/sysupdate.sh" "$ROOT/usb/early_patch.sh" "$ROOT/usb/hook.sh" "$_s/"
  for f in stick_fat.sh aidiag.sh native_zygote.sh sensor_vendor_overlay.sh sensor_trial_diag.sh tablet_mode.sh hwdetect.sh sensordiag.sh mask_vendor_modules.sh apex_patch.sh; do echo "true # $3" > "$_s/$f"; done
  for f in crossystem bootdisk.img libkm_compat.so system_ext_manifest.xml soft_gk; do echo "$f $3" > "$_s/$f"; done
  cp "$PUB" "$_s/sysupdate.pub"
  printf '# gbOS86 patch manifest\n# build %s\n' "$(FP "$2")" > "$_s/patched/MANIFEST"
  echo "lib $3" > "$_s/patched/system.libz.so"
  echo fw-a > "$_s/firmware/a.bin"; echo fw-b > "$_s/firmware/intel/b.bin"; echo "fw-c $3" > "$_s/firmware/c.bin"
  echo audio > "$_s/audio/mixer.xml"; echo tool > "$_s/installer/bin/sfdisk"; echo ko > "$_s/kmod/btusb.ko"; echo true > "$_s/surface/x.sh"; echo ksud > "$_s/ksu/ksud"
  echo "kernel $3" > "$_s/out/BOOTX64.EFI"
  printf 'BOARD=lapis\nBUILD_ID=%s\n' "$2" > "$1/lapis/BUILD"
}
mk_tree() { # DIR BUILD TAG: the untracked part (super.img with a build.prop line the test probe reads, modules)
  rm -rf "$1"; mkdir -p "$1/modules/lib/modules/6.18-gbos-whl+"
  { echo "ro.system.build.fingerprint=$(FP "$2")"; echo "super $3"; } > "$1/super.img"; truncate -s 8192 "$1/super.img"
  printf 'system_a 0 8\nvendor_a 8 8\n' > "$1/super.layout"
  echo "mod $3" > "$1/modules/lib/modules/6.18-gbos-whl+/m.ko"
}
build() { # NAME SRCDIR TREE SEQ [options] -> prints the bundle folder
  _o=$T/out/$1; shift; _sd=$1; _tr=$2; _sq=$3; shift 3
  MK build --src "$_sd/usb" --tree "$_tr" --key "$KEY" --seq "$_sq" --out "$_o" --no-super-check --commit-id 1111111111111111111111111111111111111111 "$@" > "$T/build.log" 2>&1 \
    || { cat "$T/build.log" >&2; return 1; }
  ls -d "$_o"/gbos86-update-* | grep -v '\.tar$' | head -n 1
}
mk_src "$T/s1" 16000001 v1; mk_tree "$T/t1" 16000001 v1
B1=$(build v1 "$T/s1" "$T/t1" 100) || bad "build v1"
check "full bundle v1 verifies (device code + openssl)" "sh '$ROOT/tools/verify_system_update.sh' '$B1' --pub '$PUB' >/dev/null 2>&1"
check "verify fails with another key" "! sh '$ROOT/tools/verify_system_update.sh' '$B1' --pub '$ROOT/usb/sysupdate.pub' >/dev/null 2>&1"
check "the set lists super.img, the kernel and nested folders" "grep -q ' super.img$' '$B1/manifest' && grep -q '^k .* EFI/BOOTX64.EFI$' '$B1/manifest' && grep -qx 'd firmware/intel' '$B1/manifest'"
check "the placeholder-free set carries the signing key" "cmp -s '$B1/files/sysupdate.pub' '$PUB'"
check "camera/ is left out by default" "mkdir -p '$T/t1/camera' && echo cal > '$T/t1/camera/c.bin' && B1c=\$(build v1c '$T/s1' '$T/t1' 100) && ! grep -q ' camera/' \"\$B1c/manifest\""
rm -rf "$T/t1/camera"

# ------------------------------------------------------------------------------------------------ the fake installed system
D=$T/dev; G=$D/gbos; EFI=$D/efi; BOOTN=1
install_set() { # BUNDLE: what menu 1 would leave
  rm -rf "$D"; mkdir -p "$G/logs" "$EFI/EFI/BOOT"
  cp -r "$1/files/." "$G/"; if [ -f "$G/EFI/BOOTX64.EFI" ]; then mv "$G/EFI/BOOTX64.EFI" "$EFI/EFI/BOOT/BOOTX64.EFI"; rmdir "$G/EFI"; else echo old > "$EFI/EFI/BOOT/BOOTX64.EFI"; fi
  cp "$1/manifest" "$G/system.manifest"
  printf 'MODE=boot\n[ -f /gbos/gbos/early_patch.sh ] && . /gbos/gbos/early_patch.sh\n' > "$G/gbos.cfg"
  echo user-file > "$G/notes.txt"
}
# the engine early_patch.sh would run: the pending update's own copy while there is one, else the installed sysupdate.sh
EB() { if [ -f "$G/update/state" ] && [ -f "$G/update/engine.sh" ]; then "$BB" sh "$G/update/engine.sh" "$@"; else "$BB" sh "$G/sysupdate.sh" "$@"; fi; }
boot() { # one stage0 pass; prints the engine's exit code
  mkdir -p "$G/logs/boot-$BOOTN"
  EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI" > "$T/boot.log" 2>&1; _r=$?
  BOOTN=$((BOOTN + 1)); echo $_r
}
snap() { (cd "$D" && find . -type f ! -path './gbos/logs/*' ! -path './gbos/update/*' ! -path './gbos/update.rejected/*' ! -path './gbos/update.inbox/*' ! -path './gbos/update.prev/*' ! -path './gbos/update.staging/*' ! -path './gbos/.stamps/*' | sort | xargs sha256sum); }
inbox() { mkdir -p "$G/update.inbox"; cp -r "$1" "$G/update.inbox/"; }
st() { sed -n "s/^$1=//p" "$G/update/state" 2>/dev/null; }
android_ok() { SU boot-ok "$G" >/dev/null 2>&1; }   # what gbos_sysupdate_ok does at boot_completed (test mode: no wait)

# v2: hook.sh and firmware/c.bin change, firmware/intel/b.bin goes, firmware/new/n.bin comes, same Google build and kernel
mk_src "$T/s2" 16000001 v1; echo "# v2" >> "$T/s2/usb/hook.sh"; echo "fw-c v2" > "$T/s2/usb/firmware/c.bin"; rm "$T/s2/usb/firmware/intel/b.bin"
mkdir -p "$T/s2/usb/firmware/new"; echo n > "$T/s2/usb/firmware/new/n.bin"
B2=$(build v2 "$T/s2" "$T/t1" 200 --base "$B1/manifest") || bad "build v2"
check "delta v2 holds only the changed files" "[ \"\$(cd '$B2/files' && find . -type f | sort | tr '\n' ' ')\" = './firmware/c.bin ./firmware/new/n.bin ./hook.sh ' ]"

# --- good update, then commit
install_set "$B1"; S1=$(snap); inbox "$B2"
check "good update: switched in, boot continues (exit 0)" "[ \$(boot) = 0 ]"
check "good update: files are the new set" "cmp -s '$T/s2/usb/hook.sh' '$G/hook.sh' && [ -f '$G/firmware/new/n.bin' ] && [ ! -e '$G/firmware/intel/b.bin' ] && cmp -s '$B2/manifest' '$G/system.manifest'"
check "good update: old files kept in update.prev until commit" "[ -f '$G/update.prev/firmware/intel/b.bin' ] && [ -f '$G/update.prev/hook.sh' ]"
check "good update: unlisted user file untouched" "[ \"\$(cat '$G/notes.txt')\" = user-file ]"
check "good update: phase trying, try 1" "[ \"\$(st phase)\" = trying ] && [ \"\$(st tries)\" = 1 ]"
check "good update: inbox emptied, .stamps dropped" "[ -z \"\$(ls '$G/update.inbox')\" ] && [ ! -e '$G/.stamps' ]"
S2=$(snap)
android_ok
check "boot-ok commits in the same start (one restart)" "[ ! -f '$G/update/state' ] && [ ! -e '$G/update.prev' ] && [ ! -e '$G/update/engine.sh' ] && grep -q committed '$G/update/status.txt'"
check "the start after a boot-ok commit changes nothing" "[ \$(boot) = 0 ] && [ ! -f '$G/update/state' ]"
check "after commit the set is unchanged" "[ \"\$(snap)\" = \"\$S2\" ]"
check "no rollback after commit (three more boots)" "boot >/dev/null; boot >/dev/null; [ \$(boot) = 0 ] && [ \"\$(snap)\" = \"\$S2\" ]"
check "status.txt says committed" "grep -q committed '$G/update/status.txt'"

# --- boot-ok written but the commit cut short (power off right after): the next start commits from boot-ok
install_set "$B1"; inbox "$B2"; boot >/dev/null
sed -n 's/^set=//p' "$G/update/state" > "$G/update/boot-ok"
check "boot-ok file alone: the next start commits" "[ \$(boot) = 0 ] && [ ! -f '$G/update/state' ] && [ ! -e '$G/update.prev' ]"
# --- commit via kmsg.log (no boot-ok file)
install_set "$B1"; inbox "$B2"; boot >/dev/null
echo '14,6193,121590821,-,caller=T1;init: processing action (sys.boot_completed=1) from (/system/etc/init/hw/init.rc:1245)' > "$G/logs/boot-$((BOOTN - 1))/kmsg.log"
check "commit from the tried boot's kmsg.log" "[ \$(boot) = 0 ] && [ ! -f '$G/update/state' ]"

# --- the same, but this boot mounted the volume dirty: the tried boot's kmsg.log is not read (a damaged one makes the FAT read-only)
install_set "$B1"; inbox "$B2"; boot >/dev/null
echo '14,6193,121590821,-,caller=T1;init: processing action (sys.boot_completed=1) from (/system/etc/init/hw/init.rc:1245)' > "$G/logs/boot-$((BOOTN - 1))/kmsg.log"
printf '#!/bin/sh\necho "[    9.6] FAT-fs (sdz2): Volume was not properly unmounted. Some data may be corrupt. Please run fsck."\n' > "$T/fakedmesg"; chmod +x "$T/fakedmesg"
dboot() { mkdir -p "$G/logs/boot-$BOOTN"; SU_DMESG="$T/fakedmesg" EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI" --dev /dev/sdz2 > "$T/boot.log" 2>&1; _r=$?; BOOTN=$((BOOTN + 1)); echo $_r; }
check "dirty mount: kmsg.log not read, no commit yet" "dboot >/dev/null; [ -f '$G/update/state' ] && grep -q 'not reading the tried boots' '$G/update/status.txt' '$T/boot.log' '$G'/logs/boot-*/sysupdate.txt 2>/dev/null"
check "clean mount of another device: kmsg.log still commits" "SU_DMESG='$T/fakedmesg' EB boot '$G' '$G/logs/boot-'\$BOOTN \$BOOTN --efi-dir '$EFI' --dev /dev/sdy2 >/dev/null 2>&1; [ ! -f '$G/update/state' ]"

# --- rollback after N failed boots
install_set "$B1"; inbox "$B2"; boot >/dev/null
boot >/dev/null; boot >/dev/null
check "still trying after 3 boots without success" "[ \"\$(st tries)\" = 3 ]"
check "4th boot rolls back (exit 0)" "[ \$(boot) = 0 ] && [ ! -f '$G/update/state' ]"
check "rollback restores the old set exactly" "[ \"\$(snap)\" = \"\$S1\" ]"
check "rollback leaves a reason in update.rejected" "grep -rq 'rolled back' '$G/update.rejected'"
check "SYSUPDATE_ATTEMPTS=1 rolls back after one failed boot" "install_set '$B1' && echo SYSUPDATE_ATTEMPTS=1 >> '$G/gbos.cfg' && inbox '$B2' && boot >/dev/null && [ \$(boot) = 0 ] && [ ! -f '$G/update/state' ] && cmp -s '$B1/files/hook.sh' '$G/hook.sh'"

# --- refusals: nothing changes, the bundle goes to update.rejected
refused() { # NAME BUNDLE: installs v1, puts BUNDLE in the inbox, one boot
  install_set "$B1"; _s=$(snap); inbox "$2"
  _r=$(boot)
  if [ "$_r" = 0 ] && [ "$(snap)" = "$_s" ] && [ ! -f "$G/update/state" ] && [ -n "$(ls "$G/update.rejected" 2>/dev/null)" ] && [ ! -e "$G/update.staging" ]; then ok "refused: $1"
  else bad "refused: $1 (exit $_r; $(cat "$G/update/status.txt" 2>/dev/null | tail -n 1))"; fi
}
cpb() { rm -rf "$T/x"; mkdir -p "$T/x"; cp -r "$B2" "$T/x/"; echo "$T/x/$(basename "$B2")"; }
X=$(cpb); echo tampered >> "$X/files/hook.sh"; refused "tampered file" "$X"
X=$(cpb); echo junk > "$X/files/extra.bin"; refused "unlisted file" "$X"
X=$(cpb); echo junk > "$X/README.txt"; refused "unlisted top-level file" "$X"
X=$(cpb); ln -s /etc/passwd "$X/files/firmware/link.bin"; refused "symlink in the bundle" "$X"
# flip one hex digit of a signature (the 40th), whatever it is
flipsig() { awk 'NR == 1 { c = substr($2, 40, 1); $2 = substr($2, 1, 39) (c == "0" ? "1" : "0") substr($2, 41) } { print }' "$1" > "$1.n" && mv "$1.n" "$1"; }
X=$(cpb); flipsig "$X/manifest.sig"; refused "bad signature (changed byte)" "$X"
X=$(cpb); openssl dgst -sha256 -sign "$T/other.pem" -out "$T/s.bin" "$X/manifest"; printf 'rsa3072-sha256 %s\n' "$(od -An -tx1 -v "$T/s.bin" | tr -d ' \n')" > "$X/manifest.sig"; refused "signed with another key" "$X"
resign() { openssl dgst -sha256 -sign "$KEY" -out "$T/s.bin" "$1/manifest"; printf 'rsa3072-sha256 %s\n' "$(od -An -tx1 -v "$T/s.bin" | tr -d ' \n')" > "$1/manifest.sig"; }
X=$(cpb); sed -i 's# firmware/new/n.bin$# firmware/../n.bin#' "$X/manifest"; resign "$X"; refused "signed manifest with .. in a path" "$X"
X=$(cpb); sed -i 's# hook.sh$# gbos.cfg#' "$X/manifest"; resign "$X"; refused "signed manifest that replaces gbos.cfg" "$X"
X=$(cpb); sed -i 's# hook.sh$# HOOK.SH#' "$X/manifest"; resign "$X"; refused "signed manifest, unsorted/case tricks" "$X"
X=$(cpb); printf 'f %s 1 extra\n' "$(printf x | sha256sum | cut -c1-64)" > "$T/l"; sed -i "/^k /e cat $T/l" "$X/manifest"; resign "$X"; refused "signed manifest with a wrong end count" "$X"
refused "the installed set again (not newer)" "$B1"
mk_src "$T/s0" 16000001 v0; B0=$(build v0 "$T/s0" "$T/t1" 50); refused "an older seq" "$B0"
mk_src "$T/sb" 15000000 vb; mk_tree "$T/tb" 15000000 vb; Bb=$(build vb "$T/sb" "$T/tb" 300); refused "an older Google build" "$Bb"
mk_src "$T/sm" 16000001 vm; mk_tree "$T/tm" 16000009 vm; Bm=$(build vm "$T/sm" "$T/tm" 300); refused "super.img of another Google build than the manifest" "$Bm"
X=$(cpb); rm -rf "$X/files/firmware/new"; refused "missing file the installed system lacks" "$X"
mk_src "$T/sx" 16000001 vx; echo 'if then fi (' > "$T/sx/usb/tablet_mode.sh"; Bx=$(build vx "$T/sx" "$T/t1" 300 --base "$B1/manifest"); refused "a script that does not parse (sh -n)" "$Bx"
mk_src "$T/sy" 16000001 vy; sed -i 's/^gbos_sysupdate_stage0$/: no hook/' "$T/sy/usb/early_patch.sh"; By=$(build vy "$T/sy" "$T/t1" 300 --base "$B1/manifest"); refused "a set whose early_patch.sh has no updater hook" "$By"
install_set "$B1"; _s=$(snap); inbox "$B2"; mkdir -p "$G/logs/boot-$BOOTN"
(export SYSUPDATE_TEST_FREE_KB=10; EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI") >/dev/null 2>&1; BOOTN=$((BOOTN + 1))
check "refused: not enough space to switch" "[ \"\$(snap)\" = \"\$_s\" ] && grep -q 'needs .* KB free' '$G/update/status.txt'"
# the placeholder as the repo first shipped it (the repo's own file holds the real key once the maintainer commits it)
install_set "$B1"; printf 'gbos86-sysupdate-key 1\nnone: no signing key is installed\n' > "$G/sysupdate.pub"; _s=$(snap); inbox "$B2"
check "refused: no key installed (the placeholder)" "[ \$(boot) = 0 ] && [ \"\$(snap)\" = \"\$_s\" ] && grep -q 'no signing key' '$G/update/status.txt'"
install_set "$B1"; echo SYSUPDATE=0 >> "$G/gbos.cfg"; inbox "$B2"
check "SYSUPDATE=0 leaves the inbox alone" "[ \$(boot) = 0 ] && [ -n \"\$(ls '$G/update.inbox')\" ] && [ ! -f '$G/update/state' ]"

# --- power cut at every step of the switch: the next boot undoes it
install_set "$B1"; inbox "$B2"; mkdir -p "$G/logs/boot-$BOOTN"; (export SYSUPDATE_TEST_CRASH_AFTER=1000; EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI") > "$T/b" 2>&1
_steps=$(grep -c . "$G/update/plan"); BOOTN=$((BOOTN + 1))
_pc=ok
for k in $(seq 1 $((_steps * 2 + 2))); do
  install_set "$B1"; inbox "$B2"; mkdir -p "$G/logs/boot-$BOOTN"
  (export SYSUPDATE_TEST_CRASH_AFTER=$k; EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI") > "$T/b" 2>&1; _r=$?; BOOTN=$((BOOTN + 1))
  [ $_r = 99 ] || break
  _r2=$(boot)
  if [ "$(snap)" != "$S1" ] || [ -f "$G/update/state" ] || [ "$_r2" != 0 ]; then _pc="step $k: not restored (exit $_r2)"; break; fi
  boot >/dev/null   # and the boot after: nothing left to do
  [ "$(snap)" = "$S1" ] || { _pc="step $k: changed on the boot after"; break; }
done
check "power cut at each switch step is undone ($k steps tried)" "[ '$_pc' = ok ]"
check "undoing twice is harmless" "install_set '$B1'; inbox '$B2'; mkdir -p '$G/logs/boot-$BOOTN'; (export SYSUPDATE_TEST_CRASH_AFTER=4; EB boot '$G' '$G/logs/boot-$BOOTN' $BOOTN --efi-dir '$EFI') >/dev/null 2>&1; BOOTN=\$((BOOTN+1)); cp '$G/update/state' '$T/st'; cp '$G/update/plan' '$T/pl'; boot >/dev/null; cp '$T/st' '$G/update/state'; cp '$T/pl' '$G/update/plan'; [ \$(boot) = 0 ] && [ \"\$(snap)\" = \"\$S1\" ]"
check "a journal without its state file is undone" "install_set '$B1'; inbox '$B2'; mkdir -p '$G/logs/boot-$BOOTN'; (export SYSUPDATE_TEST_CRASH_AFTER=5; EB boot '$G' '$G/logs/boot-$BOOTN' $BOOTN --efi-dir '$EFI') >/dev/null 2>&1; BOOTN=\$((BOOTN+1)); rm -f '$G/update/state'; [ \$(boot) = 0 ] && [ \"\$(snap)\" = \"\$S1\" ]"

# --- a new kernel: written beside the old one, renamed over it, restart (exit 10); rollback restores it
mk_src "$T/sk" 16000001 v1; echo "kernel v3" > "$T/sk/usb/out/BOOTX64.EFI"
Bk=$(build vk "$T/sk" "$T/t1" 400 --base "$B1/manifest") || bad "build vk"
_pc=ok; _ks=$(cat "$B1/files/EFI/BOOTX64.EFI")
for k in $(seq 1 30); do
  install_set "$B1"; inbox "$Bk"; mkdir -p "$G/logs/boot-$BOOTN"
  (export SYSUPDATE_TEST_CRASH_AFTER=$k; EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI") > "$T/b" 2>&1; _r=$?; BOOTN=$((BOOTN + 1))
  [ $_r = 99 ] || break
  _r2=$(boot)
  case "$_r2" in 0|10) ;; *) _pc="step $k: exit $_r2"; break;; esac
  if [ "$(snap)" != "$S1" ] || [ -f "$G/update/state" ] || [ -e "$EFI/EFI/BOOT/BOOTX64.NEW" ]; then _pc="step $k: not restored"; break; fi
done
check "power cut at each step of a kernel switch is undone, kernel included ($k steps tried; $_pc)" "[ '$_pc' = ok ] && [ $k -ge 5 ]"
install_set "$B1"; inbox "$Bk"
check "kernel update: exit 10 (restart now)" "[ \$(boot) = 10 ]"
check "kernel update: new kernel in place, old kept in update/kernel.prev, no BOOTX64.NEW" "cmp -s '$T/sk/usb/out/BOOTX64.EFI' '$EFI/EFI/BOOT/BOOTX64.EFI' && cmp -s '$B1/files/EFI/BOOTX64.EFI' '$G/update/kernel.prev' && [ ! -e '$EFI/EFI/BOOT/BOOTX64.NEW' ]"
check "kernel update: the restart boot does not count as a try" "[ \"\$(st tries)\" = 0 ]"
boot >/dev/null; boot >/dev/null; boot >/dev/null
check "kernel update: rollback restores the old kernel and asks for a restart" "[ \$(boot) = 10 ] && cmp -s '$B1/files/EFI/BOOTX64.EFI' '$EFI/EFI/BOOT/BOOTX64.EFI' && [ \"\$(snap)\" = \"\$S1\" ]"
install_set "$B1"; mkdir -p "$EFI/EFI/gbos86"; mv "$EFI/EFI/BOOT/BOOTX64.EFI" "$EFI/EFI/gbos86/"; echo other-os > "$EFI/EFI/BOOT/BOOTX64.EFI"; inbox "$Bk"
check "dual-boot ESP: only EFI/gbos86 is replaced" "[ \$(boot) = 10 ] && cmp -s '$T/sk/usb/out/BOOTX64.EFI' '$EFI/EFI/gbos86/BOOTX64.EFI' && [ \"\$(cat '$EFI/EFI/BOOT/BOOTX64.EFI')\" = other-os ]"
install_set "$B1"; _s=$(snap); inbox "$Bk"; mkdir -p "$G/logs/boot-$BOOTN"
EB boot "$G" "$G/logs/boot-$BOOTN" "$BOOTN" --efi-dir "$EFI" --efi-label SYSTEM >/dev/null 2>&1; BOOTN=$((BOOTN + 1))
check "a shared ESP without EFI/gbos86: EFI/BOOT (the other OS's loader) is never written" "[ \"\$(snap)\" = \"\$_s\" ] && grep -q 'no gbOS86 kernel' '$G/update/status.txt'"
install_set "$B1"; X=$(rm -rf "$T/x" && mkdir -p "$T/x" && cp -r "$Bk" "$T/x/" && echo "$T/x/$(basename "$Bk")"); rm -rf "$X/files/EFI"; _s=$(snap); inbox "$X"
check "refused: another kernel in the set but not in the bundle" "[ \$(boot) = 0 ] && [ \"\$(snap)\" = \"\$_s\" ]"

# --- a new Google build that Android already ran: no automatic rollback, only on request
mk_src "$T/sg" 16000002 vg; mk_tree "$T/tg" 16000002 vg; echo "kernel v1" > "$T/sg/usb/out/BOOTX64.EFI"
Bg=$(build vg "$T/sg" "$T/tg" 500 --base "$B1/manifest") || bad "build vg"
install_set "$B1"; inbox "$Bg"; boot >/dev/null
for i in 1 2 3; do echo "$(st set)" > "$G/update/android-started"; boot >/dev/null; done
check "new Google build + Android ran: held after 3 failed boots, not rolled back" "[ \"\$(st held)\" = 1 ] && [ \"\$(st phase)\" = trying ] && grep -q 'Not rolled back' '$G/update/status.txt'"
touch "$G/update/rollback-request"
check "rollback-request rolls it back anyway" "[ \$(boot) = 0 ] && [ ! -f '$G/update/state' ] && [ \"\$(snap)\" = \"\$S1\" ]"
install_set "$B1"; inbox "$Bg"; boot >/dev/null; boot >/dev/null; boot >/dev/null
check "new Google build that failed before Android ran: rolled back" "[ \$(boot) = 0 ] && [ \"\$(snap)\" = \"\$S1\" ]"

# --- several bundles in the inbox, interrupted copies
install_set "$B1"; inbox "$B0"; inbox "$B2"; mkdir -p "$G/update.inbox/gbos86-update-999999999999-x.part"
check "the newest bundle is taken, older ones rejected, .part removed" "[ \$(boot) = 0 ] && [ \"\$(st seq)\" = 200 ] && [ -z \"\$(ls '$G/update.inbox')\" ]"

# ------------------------------------------------------------------------------------------------ stage (Android side, as root)
install_set "$B1"; mkdir -p "$T/media"; cp -r "$B2" "$T/media/"; SB=$T/media/$(basename "$B2")
check "stage copies the bundle into update.inbox" "SU stage '$SB' --gbos '$G' >/dev/null 2>&1 && [ -d '$G/update.inbox/$(basename "$B2")' ]"
check "staged bundle holds only listed files and verifies" "SU verify '$G/update.inbox/$(basename "$B2")' --pub '$PUB' >/dev/null 2>&1"
rm -rf "$G/update.inbox"; cp -r "$B1" "$T/media/full"
check "stage of a full bundle skips files already installed" "SU stage '$T/media/full' --gbos '$G' >/dev/null 2>&1; [ -z \"\$(ls '$G/update.inbox')\" ] || [ \"\$(find '$G/update.inbox' -path '*/files/*' ! -path '*/files/EFI/*' -type f | wc -l)\" = 0 ]"
rm -rf "$G/update.inbox"; cp -r "$B2" "$T/media/ln"; X=$T/media/ln; rm "$X/files/hook.sh"; ln -s "$T/secret" "$X/files/hook.sh"; echo secret > "$T/secret"
check "stage refuses a listed file that is a symlink, leaves no copy" "! SU stage '$X' --gbos '$G' >/dev/null 2>&1 && ! grep -rqs secret '$G/update.inbox'"
rm -rf "$G/update.inbox"
check "stage refuses a bad signature before copying" "cp -r '$B2' '$T/media/bs' && flipsig '$T/media/bs/manifest.sig' && ! SU stage '$T/media/bs' --gbos '$G' >/dev/null 2>&1 && [ -z \"\$(ls '$G/update.inbox' 2>/dev/null)\" ]"
check "stage refuses when space is short, says how much" "_o=\$( (export SYSUPDATE_TEST_FREE_KB=100; SU stage '$SB' --gbos '$G') 2>&1); echo \"\$_o\" | grep -q 'not enough space'"
mkdir -p "$G/logs/boot-1" "$G/logs/boot-2" "$G/logs/boot-3" "$G/logs/boot-4" "$G/logs/boot-5"
check "stage --prune-logs deletes old logs but keeps the newest 3" "(export SYSUPDATE_TEST_FREE_KB=100; SU stage '$SB' --gbos '$G' --prune-logs) >/dev/null 2>&1; [ ! -e '$G/logs/boot-1' ] && [ -d '$G/logs/boot-5' ] && [ -d '$G/logs/boot-3' ]"
rm -rf "$G/update.inbox"
MK build --src "$T/s2/usb" --tree "$T/t1" --key "$KEY" --seq 201 --out "$T/out/tar" --no-super-check --base "$B1/manifest" --tar >/dev/null 2>&1
check "stage from a .tar" "SU stage \"\$(ls '$T/out/tar'/*.tar)\" --gbos '$G' >/dev/null 2>&1 && SU verify \"\$(ls -d '$G/update.inbox'/gbos86-update-*)\" --pub '$PUB' >/dev/null 2>&1"
rm -rf "$G/update.inbox"; mkdir -p "$T/evil/b"; ln -s /etc "$T/evil/b/files"; (cd "$T/evil" && tar -cf ../evil.tar b)
check "stage refuses a .tar with links" "! SU stage '$T/evil.tar' --gbos '$G' >/dev/null 2>&1"
check "cancel removes waiting bundles" "SU stage '$SB' --gbos '$G' >/dev/null 2>&1 && SU cancel '$G' >/dev/null && [ -z \"\$(ls '$G/update.inbox')\" ]"
check "status reports the set and the key" "SU status '$G' | grep -q 'signing key: installed'"

# --- stage writes big files in flushed chunks and records their hashes; the next start skips hashing them only if nothing changed
mk_src "$T/s3" 16000001 v1; head -c 2621440 /dev/urandom > "$T/s3/usb/firmware/c.bin"
B3=$(build v3 "$T/s3" "$T/t1" 220 --base "$B1/manifest") || bad "build v3"
SB3() { rm -rf "$G/update.inbox"; (export SYSUPDATE_TEST_CHUNK_MB=1 SYSUPDATE_TEST_CHECKED_MIN=1048576; SU stage "$B3" --gbos "$G") >/dev/null 2>&1 || return 1
  I3=$(ls -d "$G/update.inbox"/gbos86-update-*); SC3=$I3/staged.checked; F3=$I3/files/firmware/c.bin; }
install_set "$B1"; SB3
check "stage: a 2.5 MB file copied in 1 MB chunks is identical" "cmp -s '$B3/files/firmware/c.bin' \"\$F3\""
check "stage: only the big file is recorded, with its size and last-MB hash" "[ \"\$(wc -l < \"\$SC3\")\" = 1 ] && grep -q ' 2621440 [0-9a-f]\{64\} files/firmware/c.bin\$' \"\$SC3\""
check "stage: a bundle with staged.checked still verifies" "SU verify \"\$I3\" --pub '$PUB' >/dev/null 2>&1"
check "boot after staging: the recorded file is not hashed again, the set installs" "[ \$(boot) = 0 ] && grep -q '1 big file(s) hashed when staged' '$T/boot.log' && [ \"\$(st seq)\" = 220 ] && cmp -s '$B3/files/firmware/c.bin' '$G/firmware/c.bin'"
install_set "$B1"; SB3; printf 'Z' | dd of="$F3" bs=1 seek=2621439 conv=notrunc 2>/dev/null
check "boot: a recorded file whose last MB changed is hashed, and refused" "[ \$(boot) = 0 ] && ! grep -q 'hashed when staged' '$T/boot.log' && [ ! -f '$G/update/state' ] && grep -q 'hash mismatch' '$G/update/status.txt'"
install_set "$B1"; SB3; printf '#!/bin/sh\necho "[    9.6] FAT-fs (sdz2): Volume was not properly unmounted."\n' > "$T/fakedmesg"; chmod +x "$T/fakedmesg"
check "boot on a volume mounted dirty: the record is not used, every file is hashed" "[ \$(dboot) = 0 ] && grep -q 'not using staged.checked' '$T/boot.log' && [ \"\$(st seq)\" = 220 ]"

# ------------------------------------------------------------------------------------------------ menu 1 (install.sh)
STK=$T/stick/gbos
mkstick() { rm -rf "$T/stick"; mkdir -p "$STK"; cp -r "$1/files/." "$STK/"; rm -rf "$STK/EFI"; cp "$1/manifest" "$STK/system.manifest"; }
mkstick "$Bb"; install_set "$Bg"
check "menu 1: an older Google build on the stick is a downgrade" "SU menu1-check '$STK' '$G' | grep -qx 'downgrade=1'"
mkstick "$Bg"; install_set "$B1"
check "menu 1: a newer one is not" "SU menu1-check '$STK' '$G' | grep -qx 'downgrade=0'"
check "menu 1: pairing files changed (patched/MANIFEST)" "SU menu1-check '$STK' '$G' | grep -qx 'pairing_changed=1'"
mkstick "$B1"; install_set "$B1"
check "menu 1: same pairing files" "SU menu1-check '$STK' '$G' | grep -qx 'pairing_changed=0'"
check "menu 1: stamps untrusted without .stamps/set" "SU menu1-check '$STK' '$G' | grep -qx 'stamps_trusted=0'"
SU stamp-set "$G"
check "menu 1: stamps trusted for the same system.manifest" "SU menu1-check '$STK' '$G' | grep -qx 'stamps_trusted=1'"
cp "$B2/manifest" "$G/system.manifest"
check "menu 1: stamps untrusted after an update changed system.manifest" "SU menu1-check '$STK' '$G' | grep -qx 'stamps_trusted=0'"
mkstick "$B1"; install_set "$B1"; inbox "$B2"; boot >/dev/null
SU menu1-finish "$STK" "$G" >/dev/null
check "menu 1 finish: the stick's system.manifest is copied when it matches the stick" "cmp -s '$STK/system.manifest' '$G/system.manifest'"
check "menu 1 finish: a pending update's state and backups are dropped" "[ ! -f '$G/update/state' ] && [ ! -e '$G/update.prev' ] && [ ! -e '$G/update/engine.sh' ]"
echo changed >> "$STK/super.layout"; SU menu1-finish "$STK" "$G" >/dev/null
check "menu 1 finish: no system.manifest when it does not match the stick's files" "[ ! -e '$G/system.manifest' ]"
check "install.sh: stamps need the set check, super.img never skipped when pairing changed" "grep -q 'M1_STAMPS_TRUSTED:-0}\" = 1' '$ROOT/usb/install.sh' && grep -q 'super.img \] && \[ \"\${M1_PAIR_CHANGED:-1}\" != 0 \]' '$ROOT/usb/install.sh'"
check "install.sh: downgrade needs the word DOWNGRADE" "grep -q '\[ \"\$REPLY\" = DOWNGRADE \]' '$ROOT/usb/install.sh'"

# ------------------------------------------------------------------------------------------------ unsigned stick manifest
mkdir -p "$T/stk2"; cp -r "$B1/files/." "$T/stk2/"; mv "$T/stk2/EFI/BOOTX64.EFI" "$T/k.efi"; rmdir "$T/stk2/EFI"; cp "$T/s1/usb/install.sh" "$T/stk2/"
check "manifest of a stick tree equals the signed set's file list" "MK manifest --tree '$T/stk2' --kernel '$T/k.efi' --seq 100 --out '$T/stickman' 2>/dev/null && [ \"\$(grep '^[fk] ' '$T/stickman')\" = \"\$(grep '^[fk] ' '$B1/manifest')\" ]"

check "manifest of a tree that differs from git HEAD is recorded as -dirty" "grep -q '^commit [0-9a-f]\{40\}-dirty$' '$T/stickman'"

# ------------------------------------------------------------------------------------------------ audit / info (the app's checks)
install_set "$B1"
AU() { SU audit "$G" "$@" 2>/dev/null; }
check "audit: an untouched system is not modified" "AU | grep -qx 'modified=no' && AU | grep -qx 'source=git'"
echo 'MODE=ask' >> "$G/gbos.cfg"; mkdir -p "$G/logs/boot-9"; echo x > "$G/logs/boot-9/kmsg.log"
check "audit: gbos.cfg and logs are not modifications" "AU | grep -qx 'modified=no'"
echo '# my change' >> "$G/hook.sh"; rm "$G/firmware/a.bin"; echo extra > "$G/firmware/intel/extra.bin"
check "audit: a changed, a missing and an added file are reported" "AU | grep -qx 'modified=yes' && AU | grep -qx 'changed hook.sh' && AU | grep -qx 'missing firmware/a.bin' && AU | grep -qx 'added firmware/intel/extra.bin'"
install_set "$B1"; truncate -s 70000000 "$G/super.img"
sed -i "s/^f [0-9a-f]* [0-9]* super.img\$/f $(sha256sum "$G/super.img" | cut -c1-64) 70000000 super.img/" "$G/system.manifest"
printf 'y' | dd of="$G/super.img" bs=1 seek=69000000 conv=notrunc 2>/dev/null
check "audit: files over 64 MiB are compared by size, --deep hashes them" "AU | grep -qx 'modified=no' && AU --deep | grep -qx 'changed super.img'"
install_set "$B1"; sed -i 's/^commit \(.*\)$/commit \1-dirty/' "$G/system.manifest"
check "audit: a set built from modified sources says source=dirty" "AU | grep -qx 'source=dirty'"
rm "$G/system.manifest"
check "audit: no system.manifest (installed from a stick without one) is unknown" "AU | grep -qx 'manifest=none' && AU | grep -qx 'modified=unknown'"
install_set "$B1"
check "info: set, seq, key and free space for the app" "SU info '$G' > '$T/info' 2>/dev/null && grep -qx 'seq=100' '$T/info' && grep -q '^pub=[0-9A-F]\{768\}$' '$T/info' && grep -q '^free_kb=[0-9]' '$T/info' && grep -qx 'google_build=16000001' '$T/info' && grep -qx 'pending=none' '$T/info'"
echo changed > "$G/firmware/a.bin"; cp -r "$B2" "$T/media/needs"
check "stage: a delta for a laptop that lacks a file says NEEDS-FILES (exit 3), copies nothing" "SU stage '$T/media/needs' --gbos '$G' > '$T/o' 2>&1; [ \$? = 3 ] && grep -q NEEDS-FILES '$T/o' && [ -z \"\$(ls '$G/update.inbox' 2>/dev/null)\" ]"

# ------------------------------------------------------------------------------------------------ --omit, publish_system_update.sh
install_set "$B1"
Bo=$(build vo "$T/s2" "$T/t1" 210 --omit super.img) || bad "build --omit"
check "--omit super.img: listed in the manifest, not in files/" "grep -q ' super.img$' '$Bo/manifest' && [ ! -e '$Bo/files/super.img' ] && [ -f '$Bo/files/hook.sh' ]"
check "a full bundle without super.img stages and boots on a laptop with that super.img" "SU stage '$Bo' --gbos '$G' >/dev/null 2>&1 && [ \$(boot) = 0 ] && [ \"\$(st seq)\" = 210 ]"
PUBLISH() { sh "$ROOT/tools/publish_system_update.sh" --key "$KEY" --no-upload --no-super-check --out "$T/rel" --bases-dir "$T/rel/bases" --repo test/repo "$@"; }
mkdir -p "$T/rel/bases"
PUBLISH --tree "$T/t1" --src "$T/s1/usb" --commit-id 2222222222222222222222222222222222222222 --seq 300 > "$T/p1.log" 2>&1 || { cat "$T/p1.log"; bad "publish 1"; }
F1=$T/rel/300/gbos86-update.feed
feedverify() { head -n -1 "$1" > "$T/fb"; tail -n 1 "$1" | sed -n 's/^rsa3072-sha256 \([0-9a-f]\{768\}\)$/\1/p' | xxd -r -p > "$T/fs"; openssl rsa -in "$KEY" -pubout -out "$T/fk.pem" 2>/dev/null; openssl dgst -sha256 -verify "$T/fk.pem" -signature "$T/fs" "$T/fb" >/dev/null 2>&1; }
check "publish: first update has a full bundle, no delta, a signed feed" "[ -f '$F1' ] && ls '$T/rel/300/'*-full.tar >/dev/null 2>&1 && ! ls '$T/rel/300/'*-delta.tar >/dev/null 2>&1 && feedverify '$F1'"
check "publish: the feed names the set, the assets and their hashes" "grep -qx 'seq 300' '$F1' && grep -q '^set [0-9a-f]\{64\}$' '$F1' && grep -q '^url https://github.com/test/repo/releases/download/sysupdate-300/$' '$F1' && _a=\$(sed -n 's/^asset full \([^ ]*\) \([0-9]*\) \([0-9a-f]*\)$/\1 \3/p' '$F1') && [ \"\$(sha256sum '$T/rel/300/'\${_a% *} | cut -c1-64)\" = \"\${_a#* }\" ]"
check "publish: the full tar has no super.img, and verifies once unpacked" "rm -rf '$T/un' && mkdir -p '$T/un' && tar -C '$T/un' -xf '$T/rel/300/'*-full.tar && ! find '$T/un' -name super.img | grep -q . && sh '$ROOT/tools/verify_system_update.sh' '$T/un/'gbos86-update-* --pub '$PUB' >/dev/null 2>&1"
check "publish: a fake super.img (not make_super.py's) gives no Google lines" "! grep -qE '^google_(zip|part|super) ' '$F1'"
check "publish: refuses a seq that is not newer than a published one" "cp '$T/rel/300/'*.manifest '$T/rel/bases/300.manifest' && ! PUBLISH --tree '$T/t1' --src '$T/s2/usb' --seq 299 >/dev/null 2>&1"
cp -a "$T/t1" "$T/tbroken"; rm -f "$T/tbroken/super.layout"
check "publish: stops when the set cannot be built, and leaves no release" "! PUBLISH --tree '$T/tbroken' --src '$T/s2/usb' --commit-id 2222222222222222222222222222222222222222 --seq 350 > '$T/pb.log' 2>&1 && grep -q 'building the set failed' '$T/pb.log' && [ ! -e '$T/rel/350' ]"
# a real make_super.py image, so the feed can describe Google's partitions
mk_tree "$T/t3" 16000001 v1; mkdir -p "$T/parts"
{ echo "ro.system.build.fingerprint=$(FP 16000001)"; head -c 30000 /dev/urandom | od -An -tx1; } > "$T/parts/system_a.img"; head -c 9000 /dev/urandom > "$T/parts/vendor_a.img"
python3 -I "$ROOT/usb/make_super.py" "$T/t3/super.img" "$T/t3/super.layout" system_a="$T/parts/system_a.img" vendor_a="$T/parts/vendor_a.img" >/dev/null
echo "# v3" >> "$T/s2/usb/hook.sh"
PUBLISH --tree "$T/t3" --src "$T/s2/usb" --commit-id 3333333333333333333333333333333333333333 --seq 400 --parts "$T/parts" \
  --google-url https://dl.google.com/device/recovery/lapis-user/16000001/recovery.zip > "$T/p2.log" 2>&1 || { cat "$T/p2.log"; bad "publish 2"; }
F2=$T/rel/400/gbos86-update.feed
check "publish: the delta holds only what changed since the published set (super.img never)" "rm -rf '$T/un' && mkdir -p '$T/un' && tar -C '$T/un' -xf '$T/rel/400/'*-delta.tar && [ \"\$(cd '$T/un/'gbos86-update-*/files && find . -type f | sort | tr '\n' ' ')\" = './firmware/c.bin ./firmware/new/n.bin ./hook.sh ./super.layout ' ]"
check "publish: the feed lists the base it covers" "grep -qx \"base \$(sha256sum '$T/rel/bases/300.manifest' | cut -c1-64)\" '$F2'"
check "publish: Google's zip, each partition and super.img's hash are in the feed" "grep -qx 'google_zip https://dl.google.com/device/recovery/lapis-user/16000001/recovery.zip' '$F2' && [ \$(grep -c '^google_part ' '$F2') = 2 ] && grep -qx \"google_super \$(stat -c %s '$T/t3/super.img') \$(sha256sum '$T/t3/super.img' | cut -c1-64)\" '$F2' && feedverify '$F2'"
# --since (tools/release.sh's starting point): the delta covers every published update from that seq on, not just the last --bases
cp "$T/rel/400/"*.manifest "$T/rel/bases/400.manifest"; echo "# v5" >> "$T/s2/usb/hook.sh"
PUBLISH --tree "$T/t3" --src "$T/s2/usb" --commit-id 5555555555555555555555555555555555555555 --seq 500 --bases 1 --since 300 > "$T/p5.log" 2>&1 || { cat "$T/p5.log"; bad "publish --since 300"; }
check "publish --since 300: the delta covers 300 and 400 (--since wins over --bases 1)" "[ \$(grep -c '^base ' '$T/rel/500/gbos86-update.feed') = 2 ]"
echo "# v6" >> "$T/s2/usb/hook.sh"
PUBLISH --tree "$T/t3" --src "$T/s2/usb" --commit-id 6666666666666666666666666666666666666666 --seq 600 --since 400 > "$T/p6.log" 2>&1 || { cat "$T/p6.log"; bad "publish --since 400"; }
check "publish --since 400: only 400 is covered" "[ \$(grep -c '^base ' '$T/rel/600/gbos86-update.feed') = 1 ] && grep -qx \"base \$(sha256sum '$T/rel/bases/400.manifest' | cut -c1-64)\" '$T/rel/600/gbos86-update.feed'"
check "publish: refuses a malformed --since" "! PUBLISH --tree '$T/t3' --src '$T/s2/usb' --seq 700 --since 2026-11-20 >/dev/null 2>&1"
echo tampered >> "$T/parts/vendor_a.img"
check "super_parts.py refuses partitions that are not the ones in super.img" "! python3 -I '$ROOT/tools/super_parts.py' '$T/t3/super.img' '$T/t3/super.layout' --parts '$T/parts' >/dev/null 2>&1"

# Under the FAT lockdown (/postinstall read-only for Android) the Android-side writers (stage, cancel, boot-ok) write through stage0's
# root-only /dev/gbos-rw/stick, or, with an older hook.sh, re-run in a mount namespace of their own; they refuse with the reason when
# the target is still read-only, and log the mount state to logs/boot-N/sysupdate.txt. Fake mountinfo here; the real mounts are in
# tools/test_postinstall_ro.sh (privileged docker).
PI=$T/pi; RWD=$T/rwdir; mkdir -p "$PI/gbos/logs/boot-4" "$RWD/stick/gbos/update.inbox/gbos86-update-x"
mi() { # PI-mount PI-filesystem [rw-stick-line]
  echo "30 25 8:2 / $PI $1,nosuid,nodev shared:7 - vfat /dev/block/sda2 $2,fmask=0000"
  echo "31 30 0:40 / $PI/gbos/logs rw,nosuid shared:8 - tmpfs gbos-logs rw,size=64k,mode=700"
  [ -n "${3:-}" ] && echo "32 20 8:2 / $RWD/stick rw,nosuid,nodev,noexec shared:9 - vfat /dev/block/sda2 rw,fmask=0000"
  return 0
}
mi ro rw > "$T/mi-ro"; mi ro rw x > "$T/mi-ro-dev"; mi rw rw > "$T/mi-rw"; mi ro ro x > "$T/mi-fatro"
LK() { _m=$1; shift; SU_MOUNTINFO="$T/$_m" SU_PI="$PI" SU_RW_DIR="$RWD" SU_UNSHARE='echo UNSHARE' "$BB" sh "$ROOT/usb/sysupdate.sh" "$@"; }
for c in stage cancel boot-ok; do
  check "lockdown, older hook (no /dev/gbos-rw): $c re-runs in its own namespace" "LK mi-ro $c 2>&1 | grep -q \"^UNSHARE $BB sh -c mount -o remount,bind,rw .* $BB $PI $ROOT/usb/sysupdate.sh $c\$\""
done
for c in request install-app audit status info; do
  check "lockdown: $c does not re-run" "! LK mi-ro $c '$T/none' 2>&1 | grep -q '^UNSHARE'"
done
check "lockdown: no re-run when /postinstall is writable" "! LK mi-rw cancel '$T/none' 2>&1 | grep -q '^UNSHARE'"
check "lockdown: no second re-run inside the namespace" "! GBOS_STICK_RW=1 LK mi-ro boot-ok '$T/none' 2>&1 | grep -q '^UNSHARE'"
check "lockdown: cancel writes through /dev/gbos-rw/stick (no namespace)" "LK mi-ro-dev cancel > '$T/lk1' 2>&1 && ! grep -q '^UNSHARE' '$T/lk1' && grep -q 'removed 1 waiting' '$T/lk1' && [ ! -e '$RWD/stick/gbos/update.inbox/gbos86-update-x' ]"
check "lockdown: the choice and the mount lines are logged in logs/boot-N/sysupdate.txt" "grep -q 'cancel: writing through $RWD/stick' '$PI/gbos/logs/boot-4/sysupdate.txt' && grep -q 'GBOS_STICK_RW= $PI: 30 25 8:2 / $PI ro,' '$PI/gbos/logs/boot-4/sysupdate.txt'"
mkdir -p "$PI/gbos/update.inbox/gbos86-update-y" "$PI/gbos/update"; chmod 555 "$PI/gbos"
check "lockdown: still read-only in its namespace: refused with the reason" "! GBOS_STICK_RW=1 LK mi-ro cancel '$PI/gbos' > '$T/lk2' 2>&1 && grep -q 'stayed read-only in the updater' '$T/lk2' && [ -d '$PI/gbos/update.inbox/gbos86-update-y' ]"
check "lockdown: the FAT itself read-only (FAT error): refused, says why, no namespace" "! LK mi-fatro cancel '$PI/gbos' > '$T/lk3' 2>&1 && ! grep -q '^UNSHARE' '$T/lk3' && grep -q 'FAT is read-only for everyone' '$T/lk3'"
echo x > "$PI/gbos/update/state"
check "lockdown: boot-ok refuses instead of failing silently" "! LK mi-fatro boot-ok '$PI/gbos' > '$T/lk4' 2>&1 || ! grep -q 'cannot write' '$T/lk4'"
chmod 755 "$PI/gbos"
check "lockdown: install-app logs the mount lines every boot" "mkdir -p '$T/ia/logs/boot-4' && : > '$T/ia/gbos86-updater.apk' && PATH=/nonexistent LK mi-ro-dev install-app '$T/ia' 4 >/dev/null 2>&1; grep -q 'mounts: $PI: 30 25' '$T/ia/logs/boot-4/updater-app.txt' && grep -q 'mounts: $RWD/stick: 32 20' '$T/ia/logs/boot-4/updater-app.txt'"

# --- cleanup: the folder an installed update was staged from is deleted from the laptop's storage, and only that one
CG=$T/cg/gbos; CB=$T/cg/media0; CR=$T/cg/rec/staged-source; mkdir -p "$CG/update" "$CB/Download/gbos86-update-5-abc" "$T/cg/rec"
echo 'gbos86-set 1 seq 5' > "$CB/Download/gbos86-update-5-abc/manifest"; echo f > "$CB/Download/gbos86-update-5-abc/hook.sh"
CL() { SU_SRCREC="$CR" "$BB" sh "$ROOT/usb/sysupdate.sh" "$@"; }
CS=$(sha256sum "$CB/Download/gbos86-update-5-abc/manifest" | cut -c1-64)
rec() { { echo "source $1"; echo "base $CB"; echo "set $CS"; } > "$CR"; }
cp "$CB/Download/gbos86-update-5-abc/manifest" "$CG/system.manifest"
check "cleanup: nothing recorded: nothing deleted" "CL cleanup '$CG' | grep -q 'nothing recorded' && [ -d '$CB/Download/gbos86-update-5-abc' ]"
rec "$CB/Download/gbos86-update-5-abc"; echo x > "$CG/update/state"
check "cleanup: update still being tried: kept, record kept" "CL cleanup '$CG' | grep -q 'being tried' && [ -d '$CB/Download/gbos86-update-5-abc' ] && [ -f '$CR' ]"
rm -f "$CG/update/state"; mkdir -p "$CG/update.inbox/gbos86-update-6-def"
check "cleanup: an update waits in update.inbox: kept" "CL cleanup '$CG' | grep -q 'waits in update.inbox' && [ -d '$CB/Download/gbos86-update-5-abc' ] && [ -f '$CR' ]"
rmdir "$CG/update.inbox/gbos86-update-6-def"; echo other > "$CG/system.manifest"
check "cleanup: another set installed (rolled back): kept, record dropped" "CL cleanup '$CG' | grep -q 'was not installed' && [ -d '$CB/Download/gbos86-update-5-abc' ] && [ ! -f '$CR' ]"
cp "$CB/Download/gbos86-update-5-abc/manifest" "$CG/system.manifest"; mkdir -p "$CB/Download/notes"; cp "$CB/Download/gbos86-update-5-abc/manifest" "$CB/Download/notes/manifest"; rec "$CB/Download/notes"
check "cleanup: not a gbos86-update-* folder: kept" "CL cleanup '$CG' | grep -q 'not an update folder' && [ -d '$CB/Download/notes' ]"
mkdir -p "$T/cg/elsewhere/gbos86-update-5-abc"; cp "$CB/Download/gbos86-update-5-abc/manifest" "$T/cg/elsewhere/gbos86-update-5-abc/"; rec "$T/cg/elsewhere/gbos86-update-5-abc"
check "cleanup: outside the recorded base: kept" "CL cleanup '$CG' | grep -q 'is outside' && [ -d '$T/cg/elsewhere/gbos86-update-5-abc' ]"
rec "$CB/Download/gbos86-update-5-abc"; echo changed >> "$CB/Download/gbos86-update-5-abc/manifest"
check "cleanup: folder changed since staging: kept" "CL cleanup '$CG' | grep -q 'changed since' && [ -d '$CB/Download/gbos86-update-5-abc' ]"
cp "$CG/system.manifest" "$CB/Download/gbos86-update-5-abc/manifest"; rec "$CB/Download/gbos86-update-5-abc"
check "cleanup: installed set, nothing pending: folder deleted, record dropped" "CL cleanup '$CG' | grep -q 'deleted gbos86-update-5-abc' && [ ! -e '$CB/Download/gbos86-update-5-abc' ] && [ ! -f '$CR' ] && [ -d '$CB/Download' ]"
rec "$CB/Download/gbos86-update-5-abc"
check "cleanup: already gone: says so" "CL cleanup '$CG' | grep -q 'already gone' && [ ! -f '$CR' ]"
check "request: a successful stage records its folder, cleanup is a request action" "grep -q 'su_record_source \"\$_real\" \"\$_base\"' '$ROOT/usb/sysupdate.sh' && grep -q '^    cleanup) ' '$ROOT/usb/sysupdate.sh'"

[ $FAILS = 0 ] && { echo SYSTEM_UPDATE_TEST_PASS; exit 0; }
echo "$FAILS FAILED"; exit 1
