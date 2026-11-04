# Sourced by stage0_init.sh right before switch_root: stop the stage0 screen reader (Android has TalkBack, see ACCESSIBILITY in gbos.cfg).
if [ -s /var/run/espeakup.pid ]; then
  kill $(cat /var/run/espeakup.pid) 2>/dev/null
  log "a11y: espeakup stopped"
fi
echo 1 > /sys/module/printk/parameters/ignore_loglevel 2>/dev/null
