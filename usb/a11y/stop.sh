# Sourced by stage0_init.sh right before switch_root: stop the stage0 screen reader (Android has TalkBack, see ACCESSIBILITY in gbos.cfg).
if [ -s /var/run/espeakup.pid ]; then
  kill $(cat /var/run/espeakup.pid) 2>/dev/null
  log "a11y: espeakup stopped"
fi
# Release the Speakup synthesizer too: with no espeakup reading /dev/softsynth (no sound card here, or speech failed) nothing should keep
# queueing console text for it while Android runs. Harmless if no synth is loaded.
[ -w /sys/accessibility/speakup/synth ] && echo none > /sys/accessibility/speakup/synth 2>/dev/null
# Give the kernel console back the levels the command line chose (start.sh saved them): quiet stays quiet, debug stays verbose.
[ -f /a11y/printk.sh ] && . /a11y/printk.sh && a11y_console_restore /a11y/printk.saved
