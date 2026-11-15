#!/system/bin/sh
N=${1:-0}
G=/postinstall/gbos
L=$G/logs/boot-$N; [ -d "$L" ] || L=/data/local/tmp
[ -f $G/gbos.cfg ] && eval "$(grep -E '^(AUTOROTATE[A-Z_]*|TABLET_KEYBOARD)=[A-Za-z0-9_,+-]*$' $G/gbos.cfg)"
BIN=${0%/*}/gbos-autorotate; [ -x "$BIN" ] || BIN=$G/device/gbos-autorotate
[ -x "$BIN" ] || exit 0
case "$AUTOROTATE" in 0) MODE=off;; always) MODE=always;; *) MODE=tablet;; esac
[ "$MODE" = off ] && [ "$TABLET_KEYBOARD" = 0 ] && exit 0
IME=; [ "$TABLET_KEYBOARD" = 0 ] && IME=--no-ime
exec $BIN --mode $MODE $IME ${AUTOROTATE_MAP:+--map $AUTOROTATE_MAP} >> $L/autorotate.txt 2>&1
