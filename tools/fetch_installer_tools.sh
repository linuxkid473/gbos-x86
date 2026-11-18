#!/bin/sh
# Populate usb/installer/{bin,lib} with the partitioning tools install.sh runs from the stick:
# sfdisk, mke2fs, e2fsck, mkfs.fat, fsck.fat and blkid from Alpine (amd64), plus their musl libraries.
# They run under the bundled loader (lib/ld-musl-x86_64.so.1 --library-path lib), so stage0's
# busybox environment needs nothing else. Needs docker; safe to re-run.
set -e
OUT=$(cd "$(dirname "$0")/.." && pwd)/usb/installer
mkdir -p "$OUT"
# Fedora/RHEL (SELinux enforcing): the bind mount needs :z (relabels only this repo directory).
Z=; [ "$(getenforce 2>/dev/null)" = Enforcing ] && Z=:z
docker run --rm --platform linux/amd64 -v "$OUT":/out$Z alpine:latest sh -c '
apk add -q sfdisk e2fsprogs dosfstools util-linux-misc partx efibootmgr efivar-libs >/dev/null
rm -rf /out/bin /out/lib; mkdir -p /out/bin /out/lib
for b in sfdisk mke2fs e2fsck mkfs.fat fsck.fat blkid partx efibootmgr; do
  r=$(command -v $b); cp -L $r /out/bin/
  ldd $r | awk "/=>/{print \$3}" | while read l; do [ -e "$l" ] && cp -L $l /out/lib/; done
done
cp -L /lib/ld-musl-x86_64.so.1 /out/lib/
apk info -v sfdisk e2fsprogs dosfstools 2>/dev/null | sed "s/^/  /"'
ls "$OUT/bin"
