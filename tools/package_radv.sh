#!/bin/sh
# Pack the RADV Android Vulkan HAL (tools/radv-android/build.sh) as the release asset build.sh fetches: one entry,
# patched/vulkan.radv.so, in a tar.xz that build.sh unpacks into usb/.
#   tools/package_radv.sh [SO] [OUT]
#     SO   default build/radv/vulkan.radv.so
#     OUT  default build/dl/gbos86-radv-r1.tar.xz (the name in build.sh's RADV=)
# The archive is reproducible (fixed mtime, owner and mode), so packing the same .so twice gives the same sha256.
# Then: upload OUT to the release named in build.sh's REL_RADV and put the printed sha256 into RADV_SHA.
# Pack only a build that reached the desktop on an AMD laptop (HANDOFF.md has the hashes of the ones that did).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SO=${1:-$ROOT/build/radv/vulkan.radv.so}
OUT=${2:-$ROOT/build/dl/gbos86-radv-r1.tar.xz}
[ -f "$SO" ] || { echo "package_radv: $SO not found (build it with tools/radv-android/build.sh)" >&2; exit 1; }
[ "$(head -c 4 "$SO" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || { echo "package_radv: $SO is not an ELF file" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
python3 - "$SO" "$OUT" <<'EOF'
import sys, tarfile
so, out = sys.argv[1], sys.argv[2]
with tarfile.open(out, 'w:xz', format=tarfile.GNU_FORMAT) as t:
    d = tarfile.TarInfo('patched'); d.type = tarfile.DIRTYPE; d.mode = 0o755; d.mtime = 0
    t.addfile(d)
    i = t.gettarinfo(so, 'patched/vulkan.radv.so')
    i.mode = 0o644; i.mtime = 0; i.uid = i.gid = 0; i.uname = i.gname = ''
    with open(so, 'rb') as f:
        t.addfile(i, f)
EOF
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
echo "vulkan.radv.so  sha256 $(sha "$SO")  ($(wc -c < "$SO" | tr -d ' ') bytes)"
echo "$(basename "$OUT")  sha256 $(sha "$OUT")  ($(wc -c < "$OUT" | tr -d ' ') bytes)"
echo "build.sh: RADV_SHA=$(sha "$OUT")"
