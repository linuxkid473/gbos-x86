#!/bin/sh
# Build the gbOS86 Updater app (updater-app/) into usb/gbos86-updater.apk. No Gradle, no libraries: javac, d8, aapt2, zipalign, apksigner.
#
#   updater-app/build.sh                 release build, signed with the key in $GBOS86_APP_KEYSTORE
#   updater-app/build.sh --test-key      signed with a throwaway key made in build/ (for trying it; it cannot later be updated
#                                        by a release build without uninstalling first: Android requires the same signing key)
#
# Needs (nothing is downloaded by this script; see docs/system-update.md "Building the app"):
#   JAVA_HOME or javac/keytool in PATH       a JDK 17 or newer (Ubuntu: openjdk-17-jdk-headless)
#   ANDROID_HOME (default ~/Android/Sdk)     with platforms/android-$API/android.jar and build-tools/<version>/{aapt2,d8,zipalign,apksigner}
#
# Signing key: GBOS86_APP_KEYSTORE (default ~/.config/gbos86/updater-app.p12), made on the first release build with keytool. It must
# never be inside the repository (this script refuses). Keep a backup: losing it means every laptop has to uninstall the app once.
# Password: GBOS86_APP_KEYSTORE_PASS, or keytool/apksigner ask for it.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
API=${API:-35}
MIN_SDK=30
VERSION_CODE=${VERSION_CODE:-1}
VERSION_NAME=${VERSION_NAME:-1.0}
OUT=$ROOT/build/updater-app
APK=${APK_OUT:-$ROOT/usb/gbos86-updater.apk}
TESTKEY=0; [ "${1:-}" = --test-key ] && TESTKEY=1

missing=
need() { command -v "$1" >/dev/null 2>&1 || missing="$missing $1"; }
[ -n "${JAVA_HOME:-}" ] && PATH=$JAVA_HOME/bin:$PATH
need javac; need keytool; need java
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
JAR=$SDK/platforms/android-$API/android.jar
[ -f "$JAR" ] || missing="$missing $JAR"
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -n 1)
for t in aapt2 d8 zipalign apksigner; do [ -n "$BT" ] && [ -x "$BT/$t" ] || missing="$missing build-tools/$t"; done
if [ -n "$missing" ]; then
  echo "missing:$missing" >&2
  echo "Install a JDK 17+ and the Android SDK (platform android-$API, build-tools) first: docs/system-update.md, \"Building the app\"." >&2
  exit 1
fi

rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/dex"
echo "== aapt2 link (manifest only, no resources)"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version $MIN_SDK --target-sdk-version $API --version-code "$VERSION_CODE" --version-name "$VERSION_NAME"
echo "== javac"
javac -encoding UTF-8 -source 1.8 -target 1.8 -Xlint:-options -bootclasspath "$JAR" -d "$OUT/classes" \
  $(find "$HERE/src" -name '*.java')
echo "== d8"
"$BT/d8" --release --min-api $MIN_SDK --lib "$JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
python3 -I - "$OUT/base.apk" "$OUT/dex/classes.dex" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[2], 'classes.dex')
PY
"$BT/zipalign" -p -f 4 "$OUT/base.apk" "$OUT/aligned.apk"

if [ $TESTKEY = 1 ]; then
  KS=$ROOT/build/updater-app-testkey.p12; PASS=gbos86-test
  [ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storetype PKCS12 -storepass "$PASS" -alias updater -keyalg RSA -keysize 3072 \
    -validity 10000 -dname "CN=gbOS86 Updater test key" >/dev/null
  SIGN="--ks $KS --ks-pass pass:$PASS"
else
  KS=${GBOS86_APP_KEYSTORE:-$HOME/.config/gbos86/updater-app.p12}
  KSDIR=$(mkdir -p "$(dirname "$KS")" && cd "$(dirname "$KS")" && pwd)
  if git -C "$KSDIR" rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "refusing: the keystore $KS is inside a git work tree; keep it outside every repository" >&2; exit 1
  fi
  if [ ! -f "$KS" ]; then
    echo "== making the release signing key $KS (back it up somewhere safe)"
    keytool -genkeypair -keystore "$KS" -storetype PKCS12 ${GBOS86_APP_KEYSTORE_PASS:+-storepass "$GBOS86_APP_KEYSTORE_PASS"} \
      -alias updater -keyalg RSA -keysize 3072 -validity 10000 -dname "CN=gbOS86 Updater"
    chmod 600 "$KS"
  fi
  SIGN="--ks $KS${GBOS86_APP_KEYSTORE_PASS:+ --ks-pass env:GBOS86_APP_KEYSTORE_PASS}"
fi
echo "== apksigner"
# shellcheck disable=SC2086
"$BT/apksigner" sign $SIGN --ks-key-alias updater --out "$APK" "$OUT/aligned.apk"
"$BT/apksigner" verify --print-certs "$APK" | head -n 3
echo "built $APK ($(stat -c %s "$APK") bytes). Copy it to the stick's gbos/ folder (it is in install.sh's FILES when present)."
