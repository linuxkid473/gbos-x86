#!/system/bin/sh
OUT=${1:-/postinstall/gbos/logs/rotation-policy.txt}

if ! mkdir -p "$(dirname "$OUT")"; then
  echo "ERROR: cannot create rotation policy log directory: $OUT" >&2
  exit 1
fi

{
  echo "Latitude 7320 display rotation policy: $(date '+%Y-%m-%d %H:%M:%S %z')"
  /system/bin/cmd window fixed-to-user-rotation disabled
  _rotation_rc=$?
  if [ "$_rotation_rc" -ne 0 ]; then
    echo "ERROR: cmd window fixed-to-user-rotation disabled failed (rc=$_rotation_rc)"
    exit "$_rotation_rc"
  fi
  echo "Current fixed-to-user-rotation mode:"
  /system/bin/cmd window fixed-to-user-rotation
  _rotation_rc=$?
  if [ "$_rotation_rc" -ne 0 ]; then
    echo "ERROR: unable to read fixed-to-user-rotation mode (rc=$_rotation_rc)"
    exit "$_rotation_rc"
  fi
} > "$OUT" 2>&1
