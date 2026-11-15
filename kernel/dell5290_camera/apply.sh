#!/bin/sh
set -e
P=${P:-$(cd "$(dirname "$0")" && pwd)}
SRC=${SRC:-/build/src}
command -v patch >/dev/null || { echo "dell5290_camera: 'patch' is not installed in the build container" >&2; exit 1; }
SERIES=$(grep -v '^#' "$P/series")
N=$(echo "$SERIES" | wc -l)
FILES=$(for f in $SERIES; do sed -n 's#^+++ b/\([^[:space:]]*\).*#\1#p' "$P/$f"; done | sort -u)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fresh() {
  rm -rf "$W/t"; mkdir -p "$W/t"
  for f in $FILES; do mkdir -p "$W/t/$(dirname "$f")"; cp -p "$SRC/$f" "$W/t/$f"; done
}
run() { (cd "$W/t" && patch -p1 -s -f --no-backup-if-mismatch -r - "$@") >> "$W/log" 2>&1; }
k=$N
while :; do
  fresh; ok=1
  for f in $(echo "$SERIES" | head -n "$k" | sed -n '1!G;h;$p'); do
    run -R < "$P/$f" || { ok=0; break; }
  done
  [ $ok = 1 ] && break
  k=$((k - 1))
done
fresh
for f in $(echo "$SERIES" | tail -n +$((k + 1))); do
  : > "$W/log"
  run < "$P/$f" || { echo "dell5290_camera: $f does not apply ($k of $N already present)" >&2; cat "$W/log" >&2; exit 1; }
done
[ "$k" = "$N" ] || for f in $FILES; do cmp -s "$W/t/$f" "$SRC/$f" || cp "$W/t/$f" "$SRC/$f"; done
echo "dell5290_camera: $((N - k)) patch(es) applied, $k already present"
