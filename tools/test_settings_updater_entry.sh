#!/bin/sh
# hook.sh gbos_settings_updater_entry (Settings > System > Software update > "gbOS86 updates") and its overlay APK.
# The writable-/vendor case only (the tmpfs fallback needs mount: covered on hardware). Prints SETTINGS_UPDATER_TEST_PASS.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
FAILS=0
ok() { echo "ok - $1"; }
bad() { echo "FAIL: $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
sed -n '/^gbos_settings_updater_entry() {$/,/^}$/p' "$ROOT/usb/hook.sh" > "$T/fn.sh"
check "hook.sh defines and calls gbos_settings_updater_entry" "[ -s '$T/fn.sh' ] && grep -qx 'gbos_settings_updater_entry' '$ROOT/usb/hook.sh'"
run() { ( log() { echo "$*" >> "$T/log"; }; G=$T/g; NEWROOT=$T/new; . "$T/fn.sh"; gbos_settings_updater_entry ); }
mkdir -p "$T/g/display" "$T/new/vendor/overlay"; cp "$ROOT/usb/display/gbos-updater-settings.apk" "$T/g/display/"
: > "$T/log"; run
check "no updater APK: nothing added" "[ ! -e '$T/new/vendor/overlay/gbos-updater-settings.apk' ]"
echo apk > "$T/g/gbos86-updater.apk"; : > "$T/log"; run
check "updater installed: overlay copied to /vendor/overlay, 0644, logged" "cmp -s '$ROOT/usb/display/gbos-updater-settings.apk' '$T/new/vendor/overlay/gbos-updater-settings.apk' && [ \"\$(stat -c %a '$T/new/vendor/overlay/gbos-updater-settings.apk')\" = 644 ] && grep -q 'Settings updater entry added' '$T/log'"
: > "$T/log"; run
check "second run: already in place" "grep -q 'entry in place' '$T/log'"
rm -f "$T/new/vendor/overlay/gbos-updater-settings.apk"; : > "$T/log"; ( SETTINGS_UPDATER=0; export SETTINGS_UPDATER; run )
check "SETTINGS_UPDATER=0: skipped" "[ ! -e '$T/new/vendor/overlay/gbos-updater-settings.apk' ] && grep -q 'SETTINGS_UPDATER=0' '$T/log'"
check "the committed overlay matches its sources and is signed" "sh '$ROOT/tools/build_settings_updater_overlay.sh' --check >/dev/null 2>&1"
check "it points at the updater's main activity, which is exported" "grep -q '>org.gbos86.updater.MainActivity<' '$ROOT/updater-app/settings-overlay/res/values/config.xml' && grep -A3 'android:name=\".MainActivity\"' '$ROOT/updater-app/AndroidManifest.xml' | grep -q 'exported=\"true\"'"
M=$ROOT/updater-app/AndroidManifest.xml
check "the launcher icon is an alias of MainActivity (hiding it keeps the Settings target)" "grep -A8 '<activity-alias' '$M' | grep -q 'targetActivity=\".MainActivity\"' && grep -A8 '<activity-alias' '$M' | grep -q 'category.LAUNCHER' && ! sed -n '/android:name=\".MainActivity\"/,/\/>/p' '$M' | grep -q LAUNCHER"
check "Launcher.java names that alias and refuses to hide without the Settings entry" "grep -q 'org.gbos86.updater.LauncherEntry' '$ROOT/updater-app/src/org/gbos86/updater/Launcher.java' && grep -q 'if (hide && !settingsEntry(c))' '$ROOT/updater-app/src/org/gbos86/updater/Launcher.java'"
check "boot shows a hidden icon again when the entry is gone" "grep -q 'Launcher.ensureReachable(c)' '$ROOT/updater-app/src/org/gbos86/updater/BootReceiver.java'"
check "the root service's launcher action sets only show_hidden_icon_apps_enabled" "grep -q '^    launcher)' '$ROOT/usb/sysupdate.sh' && [ \"\$(grep -c 'settings put' '$ROOT/usb/sysupdate.sh')\" = 1 ] && grep -qF 'settings put global show_hidden_icon_apps_enabled' '$ROOT/usb/sysupdate.sh'"
[ $FAILS = 0 ] && { echo SETTINGS_UPDATER_TEST_PASS; exit 0; }
echo "$FAILS FAILED"; exit 1
