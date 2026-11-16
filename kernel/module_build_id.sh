#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 KERNEL_IMAGE MODULE_DIRECTORY" >&2
    exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
sha256sum < "$1" > "$TMP/kernel"
(
    cd "$2"
    find . -type f ! -name gbos-build-id -exec sha256sum {} + > "$TMP/modules"
)
[ -s "$TMP/modules" ] || { echo "module build ID: empty module tree" >&2; exit 1; }
LC_ALL=C sort "$TMP/modules" > "$TMP/sorted"
cat "$TMP/kernel" "$TMP/sorted" > "$TMP/manifest"
sha256sum < "$TMP/manifest" | cut -d' ' -f1
