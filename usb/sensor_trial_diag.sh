#!/system/bin/sh
# Observe framework state without opening or reconfiguring the IIO device.
OUT=${1:-/postinstall/gbos/logs/sensor-trial.txt}
sleep 60
{
  echo "Experimental sensor trial: framework snapshots; no IIO controls changed"
  i=0
  while [ "$i" -lt 11 ]; do
    echo "== snapshot $i: $(date '+%Y-%m-%d %H:%M:%S %z') uptime=$(cat /proc/uptime)"
    echo "-- sensorservice"
    timeout 15 dumpsys sensorservice 2>&1 || echo "ERROR: sensorservice dump failed or timed out"
    echo "-- display orientation"
    timeout 15 dumpsys window displays 2>&1 | grep -iE 'rotation|orientation' || echo "No window orientation output"
    echo "-- fixed-to-user-rotation mode"
    cmd window fixed-to-user-rotation 2>&1 || echo "ERROR: cannot query fixed-to-user-rotation mode"
    echo "-- enabled framework resource overlays"
    cmd overlay list android 2>&1
    # which overlay supplies the value (answers why mSupportAutoRotation was already true with the Lapis RRO's false)
    echo "-- config_supportAutoRotation resolution"
    timeout 15 cmd overlay lookup --verbose android android:bool/config_supportAutoRotation 2>&1 ||
      echo "ERROR: overlay lookup failed"
    echo "-- adaptive brightness (screen_brightness_mode $(settings get system screen_brightness_mode 2>&1))"
    ab=$(timeout 15 dumpsys display 2>&1 | grep -E 'mLoadedFrom|AutoBrightnessAvailable|mUseSoftwareAutoBrightnessConfig|mAutoBrightnessEnabled|mLightSensor=|mAmbientLightSensor=|mAmbientLux=|BrightnessEvent' | tail -n 12)
    [ -n "$ab" ] && echo "$ab" || echo "No adaptive brightness output"
    i=$((i + 1))
    [ "$i" -ge 11 ] || sleep 30
  done
} > "$OUT" 2>&1
