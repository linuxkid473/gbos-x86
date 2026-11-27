#!/bin/sh
# build.sh - build the gbOS86 Goodix 27c6:533c fingerprint HAL.
#
#   ./build.sh --host      host test harness only (gcc + system OpenSSL)
#   ./build.sh --android   HAL binary for Android x86_64 (NDK clang)
#   ./build.sh --android --stub
#                          HAL binary with the Milan matcher stubbed out
#                          (probes/registers, enroll+identify refuse with
#                          GM168SEC_ERR_NO_MATCHER)
#   ./build.sh --all      host tests, then the Android HAL (default Milan)
#
# This builds ONLY the HAL binary (gbos_fp) - never a gbOS86 image.
# Android mode needs the NDK at ../ndk/android-ndk-r27c (r27c, Clang 18).
# It links against the NDK's API-35 stubs (the NDK has no API-36 dir;
# upstream builds against android36 with its own clang - the HAL uses
# no API-36-only symbol, all binder/BoringSSL entry points are
# hand-declared in gbos_fp.c).
#
# libcrypto: the NDK ships no libcrypto/libssl stub, but gbos_fp.c calls
# HMAC() and EVP_sha256() for the Gatekeeper token check (gbos_fp.c:500).
# A minimal link stub is generated here (build/libcrypto.so, SONAME
# libcrypto.so, symbols HMAC + EVP_sha256 as undefined references via a
# version script - the same technique as the NDK's own stubs). The real
# symbols resolve at runtime from the lapis image's libcrypto. The
# TLS layer (gm168sec_hal.h) dlopens libssl/libcrypto instead and is
# NOT linked against this stub.
set -eu

cd "$(dirname "$0")"

MODE=android
MATCHER=milan
while [ $# -gt 0 ]; do
	case "$1" in
		--host) MODE=host ;;
		--android) MODE=android ;;
		--all) MODE=all ;;
		--stub) MATCHER=stub ;;
		--milan) MATCHER=milan ;;
		--help|-h) sed -n '2,20p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

# ---- host test harness ----
host_tests() {
	echo "== host tests =="
	"${MAKE:-make}" -C host_test check
}

# ---- Android HAL build ----
android_build() {
	NDK_DIR=${NDK_DIR:-../ndk/android-ndk-r27c}
	CLANG_DIR="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64"
	CC="$CLANG_DIR/bin/clang"
	LD="$CLANG_DIR/bin/ld.lld"
	READELF="$CLANG_DIR/bin/llvm-readelf"
	API=35
	LIBDIR="$CLANG_DIR/sysroot/usr/lib/x86_64-linux-android/$API"
	[ -x "$CC" ] || { echo "NDK clang not found at $CC" >&2; echo "expected the NDK at $NDK_DIR (see PORT_PLAN.md)" >&2; exit 1; }
	for lib in libc.so libdl.so libbinder_ndk.so liblog.so libz.so; do
		[ -s "$LIBDIR/$lib" ] || { echo "missing NDK stub: $LIBDIR/$lib" >&2; exit 1; }
	done

	BUILD=build
	mkdir -p "$BUILD/shim" "$BUILD/obj"
	# glib shim as glib.h (milan sources #include <glib.h>)
	cp milan/milan_shim.h "$BUILD/shim/glib.h"
	# "milan/..." includes resolve through milan/src/milan -> .
	[ -L milan/src/milan ] || ln -sfn . milan/src/milan

	# Minimal libcrypto link stub (see header comment). The stub defines
	# 1-byte functions for the BoringSSL libcrypto entry points used by
	# gbos_fp.c (Gatekeeper HAT HMAC) and gm168sec_hal.h (self-contained
	# TLS 1.2 PSK-AES128-CBC-SHA256 server); it is NEVER loaded at runtime -
	# DT_NEEDED libcrypto.so resolves to the real BoringSSL libcrypto in
	# the lapis image.
	cat > "$BUILD/libcrypto_stub.c" <<'STUB'
/* Link-time stub only - never loaded at runtime. */
#include <stddef.h>
unsigned char *HMAC(void) { return 0; }
void *EVP_sha256(void) { return 0; }
int CRYPTO_memcmp(const void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
void *EVP_aes_128_cbc(void) { return 0; }
void *EVP_CIPHER_CTX_new(void) { return 0; }
void EVP_CIPHER_CTX_free(void *c) { (void)c; }
int EVP_CIPHER_CTX_set_padding(void *c, int p) { (void)c; (void)p; return 0; }
int EVP_EncryptInit_ex(void *c, const void *t, void *e, const void *k, const void *i) { (void)c;(void)t;(void)e;(void)k;(void)i; return 0; }
int EVP_EncryptUpdate(void *c, void *o, int *ol, const void *i, int il) { (void)c;(void)o;(void)ol;(void)i;(void)il; return 0; }
int EVP_DecryptInit_ex(void *c, const void *t, void *e, const void *k, const void *i) { (void)c;(void)t;(void)e;(void)k;(void)i; return 0; }
int EVP_DecryptUpdate(void *c, void *o, int *ol, const void *i, int il) { (void)c;(void)o;(void)ol;(void)i;(void)il; return 0; }
int RAND_bytes(void *b, size_t n) { (void)b; (void)n; return 0; }
STUB
	"$CC" --target=x86_64-linux-android$API -O2 -fPIC -shared \
		-Wl,-soname,libcrypto.so \
		-o "$BUILD/libcrypto.so" "$BUILD/libcrypto_stub.c"
	echo "generated libcrypto link stub: $BUILD/libcrypto.so"

	CFLAGS="--target=x86_64-linux-android$API -O2 -fPIE -fstack-protector-strong \
		-ffreestanding -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
		-Wno-cast-function-type -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
		-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I. -Imilan/src -I$BUILD/shim"
	# shellcheck disable=SC2086
	# shellcheck disable=SC2086
	SOURCES="gbos_fp.c"
	if [ "$MATCHER" = milan ]; then
		SOURCES="$SOURCES milan/milan_matcher.c $(find milan/src -name '*.c' ! -name 'debug-hooks.c' | sort)"
	else
		SOURCES="$SOURCES milan/milan_matcher_stub.c"
		echo "NOTE: building with the Milan stub - enroll/identify will refuse"
	fi
	echo "== compiling ($(echo "$SOURCES" | wc -w) sources, matcher=$MATCHER) =="
	i=0
	for src in $SOURCES; do
		obj="$BUILD/obj/$(echo "$src" | tr '/' '_').o"
		# shellcheck disable=SC2086
		$CC $CFLAGS -c "$src" -o "$obj"
		i=$((i + 1))
	done
	echo "compiled $i objects"

	echo "== linking gbos_fp =="
	OBJS=""
	for src in $SOURCES; do
		OBJS="$OBJS $BUILD/obj/$(echo "$src" | tr '/' '_').o"
	done
	# Link through the clang driver (not raw ld) so compiler-rt builtins
	# (__popcountdi2, used by Milan) are pulled in; -nostdlib because
	# gbos_fp.c provides its own _start / __libc_init entry (upstream
	# pattern, gbos_fp.c:1556-1563).
	# No -Wl,--no-undefined: the NDK's libbinder_ndk stub lacks the
	# process/service-manager symbols (ABinderProcess_*, AServiceManager_*;
	# the NDK ships no binder_process.h/binder_manager.h), but the REAL
	# libbinder_ndk.so in the lapis image exports them - upstream links
	# the same binary against image-extracted libs and it runs. Those
	# symbols stay undefined here and resolve at runtime from the
	# image's libbinder_ndk.so, so the link passes
	# --unresolved-symbols=ignore-all. The post-link check below fails
	# the build if the undefined set is anything other than the allowlist.
	# shellcheck disable=SC2086
	$CC --target=x86_64-linux-android$API -pie -nostdlib \
		-Wl,--dynamic-linker=/system/bin/linker64 \
		-Wl,-z,noexecstack -Wl,-z,now -Wl,-z,relro -Wl,--hash-style=gnu \
		-Wl,-e,_start -Wl,--unresolved-symbols=ignore-all \
		-o gbos_fp $OBJS \
		-L"$LIBDIR" -L"$BUILD" -lc -lbinder_ndk -llog -ldl -lz -lcrypto
	"$READELF" -h gbos_fp | grep -q 'Type:.*DYN' || { echo "gbos_fp is not a PIE executable" >&2; exit 1; }
	# Undefined symbols must be either provided by a linked stub
	# (libc/libbinder_ndk/liblog/libdl/libz/libcrypto) or in the
	# runtime-resolved allowlist (binder process/manager symbols the
	# NDK stub lacks but the image's libbinder_ndk.so exports).
	UNDEF=$("$READELF" -sW gbos_fp | awk '$4 == "FUNC" && $7 == "UND" {print $8}' | sed 's/@.*//' | sort -u)
	PROVIDED=$(
		for lib in "$LIBDIR"/libbinder_ndk.so "$LIBDIR"/libc.so "$LIBDIR"/liblog.so "$LIBDIR"/libdl.so "$LIBDIR"/libz.so "$BUILD"/libcrypto.so; do
			"$READELF" -sW "$lib"
		done | awk '($4 == "FUNC" || $4 == "OBJECT") && $7 != "UND" {print $8}' | sed 's/@.*//' | sort -u
	)
	echo "undefined symbols in gbos_fp:"
	echo "$UNDEF"
	for sym in $UNDEF; do
		case "$sym" in
			ABinderProcess_setThreadPoolMaxThreadCount|AIBinder_markVintfStability|AServiceManager_addService|ABinderProcess_joinThreadPool)
				echo "  $sym (runtime-resolved from image libbinder_ndk.so)" ;;
			*)
				if ! echo "$PROVIDED" | grep -qx "$sym"; then
					echo "UNEXPECTED undefined symbol: $sym" >&2
					exit 1
				fi
				;;
		esac
	done
	echo "== gbos_fp =="
	"$READELF" -d gbos_fp | grep -E 'NEEDED|FLAGS' || true
	ls -l gbos_fp
}

case "$MODE" in
	host) host_tests ;;
	android) android_build ;;
	all) host_tests; android_build ;;
esac
