#!/system/bin/sh
N=${1:-0}
G=/postinstall/gbos
L=$G/logs/boot-$N; [ -d "$L" ] || L=/data/local/tmp
[ -f $G/gbos.cfg ] && eval "$(grep -E '^CPU_PROFILE=[a-z]*$' $G/gbos.cfg)"
BAT=
for s in /sys/class/power_supply/*; do [ "$(cat $s/type 2>/dev/null)" = Battery ] && BAT=$s; done
DEF=balanced; [ -z "$BAT" ] && DEF=performance; [ "${2:-0}" = 1 ] && DEF=performance
PROFILE=${CPU_PROFILE:-$DEF}
case "$PROFILE" in
  performance) GOV=performance; EPP=performance; PP=performance;;
  powersave) GOV=powersave; EPP=balance_power; PP=low-power;;
  *) PROFILE=balanced; GOV=powersave; EPP=balance_performance; PP=balanced;;
esac
DRV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null)
if [ "$GOV" = powersave ] && [ "$DRV" != intel_pstate ] && [ "$DRV" != amd-pstate-epp ]; then GOV=schedutil; fi
for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do [ -w "$f" ] && echo $GOV > "$f" 2>/dev/null; done
for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/energy_performance_preference; do [ -w "$f" ] && echo $EPP > "$f" 2>/dev/null; done
if [ -w /sys/firmware/acpi/platform_profile ]; then
  grep -qw "$PP" /sys/firmware/acpi/platform_profile_choices 2>/dev/null && echo $PP > /sys/firmware/acpi/platform_profile
fi
echo "$(date +%T) profile=$PROFILE driver=$DRV governor=$GOV epp=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2>/dev/null) platform=$(cat /sys/firmware/acpi/platform_profile 2>/dev/null)" >> $L/power.txt
