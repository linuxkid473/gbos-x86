#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/a/sub" "$TMP/b/sub"
printf 'kernel\n' > "$TMP/kernel"
printf 'module\n' > "$TMP/a/sub/driver.ko"
cp "$TMP/a/sub/driver.ko" "$TMP/b/sub/driver.ko"
id() { sh "$ROOT/kernel/module_build_id.sh" "$TMP/kernel" "$1"; }
first=$(id "$TMP/a")
[ "$first" = "$(id "$TMP/b")" ]
printf '%s\n' "$first" > "$TMP/a/gbos-build-id"
[ "$first" = "$(id "$TMP/a")" ]
printf 'new module\n' > "$TMP/a/sub/new.ko"
[ "$first" != "$(id "$TMP/a")" ]
rm "$TMP/a/sub/new.ko"
printf 'changed\n' > "$TMP/a/sub/driver.ko"
[ "$first" != "$(id "$TMP/a")" ]
printf 'different kernel\n' > "$TMP/kernel"
[ "$first" != "$(id "$TMP/b")" ]
mkdir "$TMP/empty"
if id "$TMP/empty" > /dev/null 2>&1; then
    echo "module build ID accepted an empty module tree" >&2
    exit 1
fi
echo MODULE_BUILD_ID_TEST_PASS
