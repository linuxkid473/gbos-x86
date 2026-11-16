#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
PACKAGE=$ROOT/usb/installer/bin/camera-media-graph

${CC:-cc} -std=gnu11 -Wall -Wextra -Werror \
    "$ROOT/tools/test_camera_media_graph.c" -o "$TMP/test"
"$TMP/test" > "$TMP/output"
grep -q '^media model=ipu6 driver=intel-ipu6 version=1$' "$TMP/output"
grep -q '^topology entities=1 interfaces=1 pads=1 links=1$' "$TMP/output"
grep -q '^entity id=42 function=0x0 name=ov5675 1-0036$' "$TMP/output"
grep -q '^pad id=43 entity=42 index=0 flags=0x0$' "$TMP/output"
grep -q '^link id=0 source=43 sink=44 flags=0x1$' "$TMP/output"
grep -q '^CAMERA_MEDIA_GRAPH_TEST_PASS$' "$TMP/output"

${CC:-cc} -std=gnu11 -Wall -Wextra -Werror \
    "$ROOT/tools/camera_media_graph.c" -o "$TMP/graph"
if "$TMP/graph" /dev/null > "$TMP/output" 2> "$TMP/error"; then
    echo "media graph accepted a non-media device" >&2
    exit 1
fi
grep -q '^media graph: device info:' "$TMP/error"

test -x "$PACKAGE"
mkdir "$TMP/image"
cp -R "$ROOT/usb/installer" "$TMP/image/installer"
cmp "$PACKAGE" "$TMP/image/installer/bin/camera-media-graph"
if "$PACKAGE" /dev/null > "$TMP/output" 2> "$TMP/error"; then
    echo "packaged media graph helper accepted a non-media device" >&2
    exit 1
fi
grep -q '^media graph: device info:' "$TMP/error"
echo CAMERA_MEDIA_GRAPH_TEST_PASS
