#!/bin/sh
set -e
P=${P:-$(cd "$(dirname "$0")" && pwd)}
SRC=${SRC:-/build/src}
cd "$SRC"
command -v patch >/dev/null || { echo "dell5290_camera: 'patch' is not installed in the build container" >&2; exit 1; }
n=0; s=0
for f in $(grep -v '^#' "$P/series"); do
  if patch -p1 -R -s -f --dry-run < "$P/$f" >/dev/null 2>&1; then
    s=$((s + 1))
  elif patch -p1 -s -f --dry-run < "$P/$f" >/dev/null 2>&1; then
    patch -p1 -s -f --no-backup-if-mismatch < "$P/$f"
    n=$((n + 1))
  else
    echo "dell5290_camera: $f neither applies nor is already applied" >&2
    patch -p1 -f --dry-run < "$P/$f" >&2 || true
    exit 1
  fi
done
echo "dell5290_camera: $n patch(es) applied, $s already present"
