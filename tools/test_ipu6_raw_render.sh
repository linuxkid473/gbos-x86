#!/bin/sh
# The raw-dump round trip: a frame saved by the bridge's raw_dump() renders back to the same chroma
# (tools/ipu6_raw_render.c --self-test), for each output size; yuyv_to_png.py writes a valid PNG.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
for size in "1920 1080" "1280 720" "640 480" "1920 1440"; do
	set -- $size
	gcc -O2 -Wall -Wextra -Werror -Wno-unused-function -pthread -DOUT_WIDTH=$1 -DOUT_HEIGHT=$2 \
		-o "$tmp/render" "$ROOT/tools/ipu6_raw_render.c" -lm
	"$tmp/render" --self-test | grep -qx IPU6_RAW_RENDER_SELF_TEST_PASS
done
python3 -c "import sys; sys.stdout.buffer.write(bytes([90,128,90,128]) * 8)" > "$tmp/t.yuyv"
python3 "$ROOT/tools/yuyv_to_png.py" "$tmp/t.yuyv" 4 4 "$tmp/t.png"
head -c 8 "$tmp/t.png" | od -An -tx1 | tr -d ' \n' | grep -qx 89504e470d0a1a0a
echo "IPU6_RAW_RENDER_TEST_PASS"
