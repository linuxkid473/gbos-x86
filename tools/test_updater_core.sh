#!/bin/sh
# Tests of the updater app's core (updater-app/src/org/gbos86/updater/core: feed signature, downloads with resume, tar, the Java
# make_super.py, rebuilding super.img from a Google-like recovery.zip). Runs on the PC with a JDK; no Android SDK, no network (a local
# HTTP server on 127.0.0.1), no Google download (tools/updater_core_fixtures.py makes a small disk image with the same structure).
# The reference super.img comes from the Python tools the maintainer uses (fetch_google_parts.py, make_super.py, super_parts.py).
#   sh tools/test_updater_core.sh      prints UPDATER_CORE_TEST_PASS
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -n "${JAVA_HOME:-}" ] && PATH=$JAVA_HOME/bin:$PATH
command -v javac >/dev/null && command -v openssl >/dev/null && command -v python3 >/dev/null || { echo "needs javac (JDK 17+), openssl, python3" >&2; exit 1; }
T=$(mktemp -d); SRV=
trap '[ -n "$SRV" ] && kill $SRV 2>/dev/null; rm -rf "$T"' EXIT HUP INT TERM
die() { echo "FAIL: $*"; exit 1; }

python3 -I "$ROOT/tools/updater_core_fixtures.py" "$T" || die "fixtures"
python3 -I "$ROOT/tools/fetch_google_parts.py" "$T/google.zip" "$T/parts" system_a vendor_a > /dev/null || die "fetch_google_parts.py"
mkdir -p "$T/exp"
python3 -I "$ROOT/usb/make_super.py" "$T/exp/super.img" "$T/exp/super.layout" system_a="$T/parts/system_a.img" vendor_a="$T/parts/vendor_a.img" > /dev/null || die "make_super.py"
python3 -I "$ROOT/tools/super_parts.py" "$T/exp/super.img" "$T/exp/super.layout" --parts "$T/parts" > "$T/google.lines" || die "super_parts.py"

# keys and signed feeds
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$T/key.pem" 2>/dev/null
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$T/other.pem" 2>/dev/null
for k in key other; do openssl rsa -in "$T/$k.pem" -noout -modulus | sed 's/^Modulus=//' > "$T/$k.mod"; done
sign() { # BODYFILE OUT
  openssl dgst -sha256 -sign "$T/key.pem" -out "$T/s.bin" "$1"
  { cat "$1"; printf 'rsa3072-sha256 %s\n' "$(od -An -tx1 -v "$T/s.bin" | tr -d ' \n')"; } > "$2"
}
SET=$(printf set | sha256sum | cut -c1-64)
feed() { # extra lines on stdin replace the google lines
  {
    echo "gbos86-update-feed 1"; echo "seq 4242"; echo "set $SET"; echo "name gbos86-update-000000004242-$(echo $SET | cut -c1-12)"
    echo "commit 1234567890123456789012345678901234567890"; echo "google_build 16000001"; echo "google_version CL3B.260622.276.R5"; echo "created 2026-11-20T12:00:00Z"
    echo "base $(printf base | sha256sum | cut -c1-64)"; echo "url https://github.com/test/repo/releases/download/sysupdate-4242/"
    echo "asset full x-full.tar 10 $SET"; echo "asset manifest x.manifest 10 $SET"
    echo "google_zip https://dl.google.com/device/recovery/lapis-user/16000001/recovery.zip"; cat; echo "note a test update"
  } > "$T/fb"
  echo "end $(wc -l < "$T/fb")" >> "$T/fb"
}
feed < "$T/google.lines"; sign "$T/fb" "$T/feed.good"
{ cat "$T/google.lines"; echo "surprise 1"; } | feed; sign "$T/fb" "$T/feed.unknown"
feed < "$T/google.lines"; sed -i 's/^end [0-9]*$/end 3/' "$T/fb"; sign "$T/fb" "$T/feed.badend"
feed < "$T/google.lines"; sed -i 's/^google_version .*/google_version cl3b;rm/' "$T/fb"; sign "$T/fb" "$T/feed.badversion"
feed < "$T/google.lines"; sed -i 's#^google_zip .*#google_zip https://example.com/recovery.zip#' "$T/fb"; sign "$T/fb" "$T/feed.badurl"
sed 's/^google_part vendor_a [0-9]* /google_part vendor_a 16384 /' "$T/google.lines" | feed; sign "$T/fb" "$T/feed.wrongsize"
sed "s/^google_part vendor_a \([0-9]*\) .*/google_part vendor_a \1 $SET/" "$T/google.lines" | feed; sign "$T/fb" "$T/feed.wrongsha"
head -c 5000 /dev/urandom > "$T/msg.bin"; openssl dgst -sha256 -sign "$T/key.pem" -out "$T/s.bin" "$T/msg.bin"; od -An -tx1 -v "$T/s.bin" | tr -d ' \n' > "$T/msg.sig"
head -c 700000 /dev/urandom > "$T/payload.bin"; sha256sum "$T/payload.bin" | cut -c1-64 > "$T/payload.sha"

# tars
N=gbos86-update-000000004242-abcdef123456; echo "$N" > "$T/good.top"
mkdir -p "$T/tsrc/$N/files/firmware/intel/a-rather-long-folder-name-for-the-ustar-prefix/and-another-one-to-pass-100-chars"
echo m > "$T/tsrc/$N/manifest"; echo s > "$T/tsrc/$N/manifest.sig"; echo fw > "$T/tsrc/$N/files/firmware/intel/a-rather-long-folder-name-for-the-ustar-prefix/and-another-one-to-pass-100-chars/blob.bin"
tar -C "$T/tsrc" --format=ustar -cf "$T/good.tar" "$N" || die "tar"
mkdir -p "$T/e1/$N/files"; ln -s /etc/passwd "$T/e1/$N/files/hook.sh"; tar -C "$T/e1" -cf "$T/evil-symlink.tar" "$N"
mkdir -p "$T/e2/$N/files"; echo x > "$T/e2/x"; tar -C "$T/e2/$N/files" -cf "$T/evil-dotdot.tar" ../../x 2>/dev/null
mkdir -p "$T/e3/a/files" "$T/e3/b/files"; echo 1 > "$T/e3/a/files/1"; echo 2 > "$T/e3/b/files/2"; tar -C "$T/e3" -cf "$T/evil-twotops.tar" a b
mkdir -p "$T/e4"; echo x > "$T/e4/x"; tar -P -cf "$T/evil-abs.tar" "$T/e4/x"
mkdir -p "$T/e5/$N/files"; echo x > "$T/e5/$N/files/a"; ln "$T/e5/$N/files/a" "$T/e5/$N/files/b"; tar -C "$T/e5" -cf "$T/evil-hardlink.tar" "$N"

# local server, compile, run
python3 -I "$ROOT/tools/updater_core_fixtures.py" serve "$T" "$T/port" & SRV=$!
for i in $(seq 1 50); do [ -f "$T/port" ] && break; sleep 0.1; done
[ -f "$T/port" ] || die "server did not start"
mkdir -p "$T/cls"
javac -encoding UTF-8 --release 8 -Xlint:-options -d "$T/cls" "$ROOT"/updater-app/src/org/gbos86/updater/core/*.java \
  "$ROOT/updater-app/test/org/gbos86/updater/core/CoreTest.java" || die "javac"
java -cp "$T/cls" org.gbos86.updater.core.CoreTest "$T" "$(cat "$T/port")" > "$T/java.log" 2>&1; R=$?
cat "$T/java.log"
if [ $R = 0 ] && grep -qx CORE_JAVA_PASS "$T/java.log"; then
  # what the Java tar extraction made must equal the source tree
  if diff -r "$T/tsrc/$N" "$T/untar/$N" >/dev/null; then echo "ok - tar: unpacked files equal the originals"; echo UPDATER_CORE_TEST_PASS; exit 0; fi
  echo "FAIL: tar: unpacked files differ"
fi
exit 1
