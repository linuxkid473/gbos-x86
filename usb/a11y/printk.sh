# Sourced by start.sh and stop.sh. Speakup reads every message printed on the console aloud, so the screen reader silences the
# kernel console while it runs (ignore_loglevel off, console level 1) and stop.sh puts back what the command line chose: the generic
# kernel (loglevel=4) stays quiet after stage0, the debug kernel (loglevel=8 ignore_loglevel) stays verbose. The log on the stick is
# unaffected either way (it reads /dev/kmsg, which ignores the console level).
_a11y_il=${GBOS_SYSFS_ROOT:-/sys}/module/printk/parameters/ignore_loglevel
_a11y_pk=${GBOS_PROC_ROOT:-/proc}/sys/kernel/printk
# $1: the file that keeps the state from before (written once, so a second call cannot save the quiet state).
a11y_console_quiet() {
  if [ ! -f "$1" ]; then
    _il=$(cat "$_a11y_il" 2>/dev/null); _cl=; read -r _cl _rest < "$_a11y_pk" 2>/dev/null
    [ -n "$_il" ] && [ -n "$_cl" ] && echo "$_il $_cl" > "$1"
  fi
  echo 0 > "$_a11y_il" 2>/dev/null
  echo 1 > "$_a11y_pk" 2>/dev/null
}
a11y_console_restore() {
  [ -f "$1" ] || return 0
  read -r _il _cl < "$1"
  case "$_il" in Y|1) echo 1 > "$_a11y_il";; N|0) echo 0 > "$_a11y_il";; esac 2>/dev/null
  case "$_cl" in [0-9]|1[0-5]) echo "$_cl" > "$_a11y_pk" 2>/dev/null;; esac
  rm -f "$1"
}
