#!/bin/sh
# Populate usb/a11y/{bin,lib,share} with the text-to-speech pieces the stage0 screen reader runs from the stick:
# espeakup (talks to the kernel's Speakup "soft" synthesizer), espeak-ng with English voice data only, and amixer
# (to unmute the sound card), all from Alpine edge (espeakup is only in edge/testing) plus their musl libraries.
# They run under the bundled loader (lib/ld-musl-x86_64.so.1 --library-path lib), like usb/installer/, so stage0's
# busybox environment needs nothing else. Needs docker and network (about 25 MB download); safe to re-run.
# usb/a11y/start.sh and stop.sh (committed) are what stage0 sources; this script only fills bin/ lib/ share/.
set -e
OUT=$(cd "$(dirname "$0")/.." && pwd)/usb/a11y
mkdir -p "$OUT"
Z=; [ "$(getenforce 2>/dev/null)" = Enforcing ] && Z=:z
docker run --rm --platform linux/amd64 -v "$OUT":/out$Z alpine:edge sh -c '
set -e
apk add -q -X https://dl-cdn.alpinelinux.org/alpine/edge/testing espeakup >/dev/null
apk add -q espeak-ng alsa-utils alsa-lib >/dev/null
rm -rf /out/bin /out/lib /out/share; mkdir -p /out/bin /out/lib /out/share
for b in espeakup amixer; do
  r=$(command -v $b); cp -L $r /out/bin/
  ldd $r | awk "/=>/{print \$3}" | while read l; do [ -e "$l" ] && cp -L $l /out/lib/; done
done
# libraries opened with dlopen (not in ldd): the ALSA plugins and espeak-ng/pcaudiolib backends
for l in /usr/lib/libespeak-ng.so* /usr/lib/libpcaudio.so*; do [ -e "$l" ] && cp -L $l /out/lib/; done
ldd /usr/lib/libespeak-ng.so.1 | awk "/=>/{print \$3}" | while read l; do [ -e "$l" ] && cp -L $l /out/lib/; done
cp -L /lib/ld-musl-x86_64.so.1 /out/lib/
# English only: keep the shared phoneme data and the voice/language tables, drop every other language dictionary
D=/usr/share/espeak-ng-data
cp -a $D /out/share/espeak-ng-data
find /out/share/espeak-ng-data -maxdepth 1 -name "*_dict" ! -name "en_dict" -delete
rm -rf /out/share/espeak-ng-data/voices/mb /out/share/espeak-ng-data/mbrola_ph 2>/dev/null || true
mkdir -p /out/share/alsa && cp -a /usr/share/alsa/. /out/share/alsa/
apk info -v espeakup espeak-ng alsa-utils alsa-lib 2>/dev/null | sed "s/^/  /"
du -sh /out'
ls "$OUT/bin"
