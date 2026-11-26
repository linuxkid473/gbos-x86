#!/bin/sh
# open_report.sh: the maintainer's side of the Updater app's encrypted bug reports (docs/bug-report-plan.md).
#
#   tools/open_report.sh keygen DIR                 make DIR/report-private.pem (RSA-3072) and DIR/report.pub. DIR must be outside every
#                                                   git repository. Copy report.pub to usb/report.pub and commit it (it is public).
#   tools/open_report.sh pubkey KEY.pem             print the report.pub of KEY (what usb/report.pub must hold)
#   tools/open_report.sh open REPORT.zip|REPORT.enc --key KEY.pem [--out DIR]
#                                                   check and decrypt a report from a GitHub issue, unpack it, print REPORT.md's path
#
# File format (ReportCrypto.java writes it): "gbos86-report-encrypted 1\n", 384 bytes RSA-OAEP (SHA-256, MGF1-SHA-256) of 64 random
# bytes (32 AES-256 key, 32 HMAC key), 16 bytes IV, AES-256-CTR of the report's .tar.gz, 32 bytes HMAC-SHA256 of everything before it.
# Needs openssl and (for .zip) unzip or python3.
set -eu
HDR="gbos86-report-encrypted 1"
die() { echo "open_report: $*" >&2; exit 1; }
pub_of() {
  _m=$(openssl rsa -in "$1" -noout -modulus 2>/dev/null | sed 's/^Modulus=//') || die "cannot read $1"
  [ ${#_m} = 768 ] || die "$1 is not an RSA-3072 key"
  printf 'gbos86-report-key 1\nrsa3072 %s\n' "$_m"
}
case "${1:-}" in
  keygen)
    D=${2:?keygen DIR}; mkdir -p "$D"
    git -C "$D" rev-parse --show-toplevel >/dev/null 2>&1 && die "$D is inside a git work tree: keep the private key outside every repository"
    [ -e "$D/report-private.pem" ] && die "$D/report-private.pem exists"
    ( umask 077; openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$D/report-private.pem" 2>/dev/null )
    pub_of "$D/report-private.pem" > "$D/report.pub"
    echo "private key: $D/report-private.pem (keep it, back it up: without it no report can be read)"
    echo "public key:  $D/report.pub (copy it to usb/report.pub and commit it)";;
  pubkey) pub_of "${2:?pubkey KEY.pem}";;
  open)
    F=${2:?open FILE}; shift 2; KEY=; OUT=
    while [ $# -gt 0 ]; do case "$1" in --key) KEY=$2; shift 2;; --out) OUT=$2; shift 2;; *) die "unknown option $1";; esac; done
    [ -f "$KEY" ] || die "needs --key report-private.pem"
    T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
    case "$F" in
      *.zip)
        if command -v unzip >/dev/null 2>&1; then unzip -q -o "$F" '*.enc' -d "$T/z"
        else python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); [z.extract(n, sys.argv[2]) for n in z.namelist() if n.endswith(".enc") and "/" not in n]' "$F" "$T/z"; fi
        E=$(ls "$T"/z/*.enc 2>/dev/null | head -n 1); [ -n "$E" ] || die "no .enc file in $F";;
      *) E=$F;;
    esac
    [ "$(head -c 26 "$E" | od -An -c | tr -d ' \n')" = "$(printf '%s\n' "$HDR" | od -An -c | tr -d ' \n')" ] || die "$E is not a gbOS86 encrypted report"
    S=$(wc -c < "$E"); [ "$S" -gt $((26 + 384 + 16 + 32)) ] || die "$E is too short"
    head -c $((S - 32)) "$E" > "$T/signed"; tail -c 32 "$E" > "$T/mac"
    tail -c +27 "$E" | head -c 384 > "$T/wrapped"
    openssl pkeyutl -decrypt -inkey "$KEY" -in "$T/wrapped" -out "$T/keys" -pkeyopt rsa_padding_mode:oaep -pkeyopt rsa_oaep_md:sha256 \
      -pkeyopt rsa_mgf1_md:sha256 2>/dev/null || die "the report was not encrypted for this key"
    [ "$(wc -c < "$T/keys")" = 64 ] || die "bad key block"
    AK=$(head -c 32 "$T/keys" | od -An -tx1 -v | tr -d ' \n'); MK=$(tail -c 32 "$T/keys" | od -An -tx1 -v | tr -d ' \n')
    WANT=$(od -An -tx1 -v "$T/mac" | tr -d ' \n')
    GOT=$(openssl dgst -sha256 -mac HMAC -macopt "hexkey:$MK" -r "$T/signed" | cut -d' ' -f1)
    [ "$GOT" = "$WANT" ] || die "the report is damaged or was changed (HMAC mismatch)"
    IV=$(tail -c +$((26 + 384 + 1)) "$E" | head -c 16 | od -An -tx1 -v | tr -d ' \n')
    tail -c +$((26 + 384 + 16 + 1)) "$T/signed" | openssl enc -d -aes-256-ctr -K "$AK" -iv "$IV" > "$T/report.tar.gz"
    [ -n "$OUT" ] || OUT=$(pwd)
    mkdir -p "$OUT"; tar -C "$OUT" -xzf "$T/report.tar.gz"
    N=$(tar -tzf "$T/report.tar.gz" | head -n 1 | cut -d/ -f1)
    ( cd "$OUT/$N" && sha256sum -c --quiet SHA256SUMS ) || echo "open_report: WARNING: SHA256SUMS does not match" >&2
    echo "$OUT/$N/REPORT.md";;
  *) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 2;;
esac
