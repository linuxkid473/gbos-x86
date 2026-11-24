#!/bin/sh
# Check a gbOS86 system-update bundle on the PC, two independent ways (docs/system-update.md):
#   1. the device's own code (usb/sysupdate.sh verify) under the stick's busybox: signature in bc, strict manifest, bundle contents, hashes;
#   2. openssl: the same signature against a PEM rebuilt from sysupdate.pub's modulus (openssl asn1parse -genconf).
# Both must pass. Usage: verify_system_update.sh BUNDLE [--pub sysupdate.pub] [--installed GBOSDIR]
#   --pub defaults to usb/sysupdate.pub; --installed also checks the final set against an installed gbos/ folder (e.g. a copy of GBOSSYS).
#   BUSYBOX=/path/to/busybox picks the busybox (default: kernel/out/busybox of the main checkout, as on the stick).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
die() { echo "verify_system_update: $*" >&2; exit 1; }
B=${1:-}; [ -n "$B" ] && [ -d "$B" ] || die "usage: verify_system_update.sh BUNDLE [--pub FILE] [--installed GBOSDIR]"
shift
PUB=$ROOT/usb/sysupdate.pub; INST=
while [ $# -gt 0 ]; do case "$1" in --pub) PUB=$2; shift 2;; --installed) INST=$2; shift 2;; *) die "unknown option $1";; esac; done
BB=${BUSYBOX:-}
if [ -z "$BB" ]; then
  _c=$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null || true)
  [ -n "$_c" ] && _m=$(cd "$ROOT" && cd "$_c/.." && pwd) && [ -x "$_m/kernel/out/busybox" ] && BB=$_m/kernel/out/busybox
  [ -n "$BB" ] || [ ! -x "$ROOT/kernel/out/busybox" ] || BB=$ROOT/kernel/out/busybox
  [ -n "$BB" ] || BB=$(command -v busybox || true)
fi
[ -n "$BB" ] && [ -x "$BB" ] || die "no busybox (set BUSYBOX=, ideally the stick's: kernel/out/busybox)"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT HUP INT TERM

echo "== device code ($BB)"
set -- verify "$B" --pub "$PUB"; [ -n "$INST" ] && set -- "$@" --installed "$INST"
SU_BB=$BB TMPDIR=$T "$BB" sh "$ROOT/usb/sysupdate.sh" "$@" || die "the device code rejects this bundle"

echo "== openssl"
N=$(sed -n '2s/^rsa3072 \([0-9A-Fa-f]\{768\}\)$/\1/p' "$PUB")
[ -n "$N" ] || die "$PUB holds no key"
printf 'asn1=SEQUENCE:pubkey\n[pubkey]\nn=INTEGER:0x%s\ne=INTEGER:65537\n' "$N" > "$T/key.conf"
openssl asn1parse -genconf "$T/key.conf" -out "$T/key.der" -noout >/dev/null
openssl rsa -RSAPublicKey_in -inform DER -in "$T/key.der" -pubout -out "$T/key.pem" 2>/dev/null || die "cannot rebuild the public key"
sed -n '1s/^rsa3072-sha256 \([0-9a-fA-F]\{768\}\)$/\1/p' "$B/manifest.sig" | xxd -r -p > "$T/sig.bin"
[ "$(stat -c %s "$T/sig.bin")" = 384 ] || die "manifest.sig is not a 384-byte signature"
openssl dgst -sha256 -verify "$T/key.pem" -signature "$T/sig.bin" "$B/manifest" >/dev/null || die "openssl: BAD signature"
echo "openssl: signature OK"
echo "BUNDLE OK"
