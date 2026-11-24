#!/bin/sh
# Build the AIDL v4 Goodix HAL against the Android libraries extracted from this image.
set -eu
C=${CLANG_DIR:-/build/inputs/clang-r584948b}
CC=${CC:-$C/bin/clang-22}
LD=${LD:-$C/bin/ld.lld}
READELF=${READELF:-$C/bin/llvm-readelf}
HOSTCC=${HOSTCC:-cc}
LIB_DIR=${LIB_DIR:-libs}
OUTPUT=${OUTPUT:-gbos_fp}

cd "$(dirname "$0")"
for lib in libc.so libdl.so libbinder_ndk.so liblog.so libcrypto.so; do
  [ -s "$LIB_DIR/$lib" ] || { echo "missing image-matched library: $LIB_DIR/$lib" >&2; exit 1; }
done
TMP=$(mktemp -d "${TMPDIR:-/tmp}/gbos-fp-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

$CC --target=x86_64-linux-android36 -O2 -fPIE -fstack-protector-strong -ffreestanding \
  -fvisibility=hidden -Wall -Wextra -Werror -c gbos_fp.c -o "$TMP/gbos_fp.o"
$LD -pie --dynamic-linker=/system/bin/linker64 -z noexecstack -z now -z relro --hash-style=gnu \
  --no-undefined -e _start -o "$OUTPUT" "$TMP/gbos_fp.o" -L"$LIB_DIR" \
  -lc -lbinder_ndk -llog -lcrypto -ldl
$READELF -h "$OUTPUT" | grep -q 'Type:.*DYN' || { echo "HAL is not a PIE executable" >&2; exit 1; }
$READELF -d "$OUTPUT" | grep -E 'NEEDED|FLAGS'
$HOSTCC -std=c11 -O2 -Wall -Wextra -Werror -o "$TMP/goodix_test" goodix_test.c
"$TMP/goodix_test"
$HOSTCC -std=c11 -O2 -Wall -Wextra -Werror -o "$TMP/goodix_sim_test" goodix_sim_test.c
"$TMP/goodix_sim_test"
$HOSTCC -std=c11 -O2 -Wall -Wextra -Werror -o "$TMP/fp_log_test" fp_log_test.c
"$TMP/fp_log_test"
