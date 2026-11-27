# PORT_PLAN.md — gbOS86 fingerprint HAL for Goodix 27c6:533c (XPS 13 9310)

Status 26 Nov 2026: the HAL binary **builds and links** (Milan and stub
variants). It has **never run against a sensor**. The open-driver Ubuntu
test on Kanaan's hardware is still the gate before any HAL test means
anything — see "Known risks".

## What was ported

From seaweeduk/goodix53x5-libfprint, branch `pr334` (PR #334, head
`52584a5` "Count only empty waits against the 533c handshake limit"),
into `gbos_fp-533c/`:

- **Transport** (`gm168sec_hal.h`, ~2000 lines, header-only, blocking
  usbfs): wrapped pack framing, protocol messages, command bytes,
  bring-up order (probe → firmware → PSK → reset → chip ID → OTP →
  TLS → config restore → arm FDT), FDT wait, capture, TLS 1.2-PSK
  channel (sensor is TLS client, host is server, cipher
  `PSK-AES128-CBC-SHA256`, identity `"Client_identity"`), SHA-256 PSK
  hash check (plain-C implementation), CRC32-MPEG2 image check, raw12
  decode, config patch + checksum, OTP parse. All with file:line
  references to the PR in the header comments.
- **Milan matcher** (`milan/src/`, 43 `.c` files) + glib shim
  (`milan/milan_shim.h`) + wiring (`milan/milan_matcher.c`). See
  `milan/milan-audit.md` for the full decision record.
- **AIDL plumbing** (`gbos_fp.c`, `gbos_fp_533c_policy.h`): upstream
  gbOS86 `fingerprint/gbos_fp.c` (AIDL v4, Gatekeeper token check,
  lockout/policy, template storage) adapted for the 533c backend.
  The 533c keeps no on-sensor template database; templates are Milan
  blobs stored as files in `/data/vendor/crosfp/gbos_fp/`
  (`tpl-GBOS-<instance>-u<user>-<id>.bin`), so `goodix_list_templates` /
  `goodix_delete_template` operate on files and the policy header
  works unchanged.
- **Hook/cfg patch** (`hook-533c.patch`): extends the exact-ID check
  beyond 6384 to include 533c, unbinds `cdc_acm` as root before the
  service starts, keeps `FINGERPRINT=0` opt-out, WEAK class only.
  Verified: applies cleanly to upstream `gbos-x86` at HEAD `284769a`
  (`patch -p1`), `hook.sh` passes `sh -n`.

## What was left out

- **No full gbOS86 image build.** This is a drop-in `gbos_fp`
  binary for the existing install only.
- **The libfprint driver layer was not ported**: no FpiSsm state
  machine, no GUsb, no glib (shim instead), no `FpPrint`/GVariant
  envelope (templates stored verbatim — see `milan-audit.md`).
- **OTP hash verification** (`goodix_device_verify_otp`) was not
  ported — OTP bytes are used as read. If the sensor's OTP is corrupt
  the calibration will be wrong and match quality will suffer, with
  no explicit error.
- **The McuSetEcCtrl-off command bytes** for `goodix_settle` (the
  command that ends FDT isolation) are not yet pinned — `family.h`
  says isolation ends with three zero bytes; the exact command needs
  confirming from hardware logs.
- **FDT arm uses a zero base on first open**; the real base is learned
  from the first FDT read and re-armed (per `device/scan.c`).
- **Preset-read derivation** (cmd byte `0xe4` → cat `0x0e` cmd `0x02`,
  payload `[length][0][slot][0]`) is to verify against
  `commands.c goodix_cmd_preset_read` on hardware.
- **Firmware reply layout for the probe command** is assumed to be a
  NUL-terminated string; confirm from hardware logs.
- **Debug hooks** (`debug-hooks.c`) are excluded — with
  `GOODIX53X5_DEBUG` off they are no-ops.
- **The proprietary Dell driver** (`libfprint-2-tod1-goodix`, glibc
  TOD fork) was not ported and cannot be — it is closed-source and
  glibc-only. The Ubuntu test (below) is the only place it runs.

## Adaptations to upstream `gbos_fp.c` (all documented in the files)

- `#include "goodix.h"` → `gm168sec_hal.h`; policy header renamed to
  `gbos_fp_533c_policy.h` (comment updated: templates are host files).
- `fw_version` format `"%.8s"` → `"%.39s"` (the 533c firmware string
  `GF5288_GM168SEC_APP_13016` is 26 chars).
- Hand-defined `struct tm` / `struct timespec` / `clock_gettime` /
  `localtime_r` / `CLOCK_*` replaced by `#include <time.h>` — the real
  headers pulled in by `gm168sec_hal.h` define `struct timespec` and
  conflicted with the hand definitions.
- Hand-defined `pthread_mutex_t` / `pthread_cond_t` + pthread
  prototypes replaced by `#include <pthread.h>` for the same reason
  (Bionic's layouts are identical, all-zero = default init).
- Hand-defined `O_*` and errno-number macros deleted — they now come
  from `<fcntl.h>` / `<errno.h>` via `gm168sec_hal.h` (used only after
  that include; values are identical).
- `arc4random_buf` prototype deleted — it comes from `<stdlib.h>` via
  `gm168sec_hal.h`.
- Compatibility defines added to `gm168sec_hal.h` for names the
  adapted `gbos_fp.c` / policy header expect from upstream `goodix.h`:
  `GOODIX_MAX_TEMPLATES` (= 32), `GOODIX_ENROLL_STAGES` (= 12),
  `GOODIX_MAX_UID` (= 32), `GOODIX_NO_RECORD` (= `GOODIX_NORECORD`),
  and a new `GOODIX_NO_SPACE` enum value. `goodix_enroll` now returns
  `GOODIX_NO_SPACE` when the template store already holds 32
  templates (upstream MOC backend reported this from the sensor,
  `goodix.c:609-610`; the 533c store is files, so the check lives in
  the HAL).

## Build notes (what worked, what was faked)

- Toolchain: Android NDK r27c, Clang 18, `--target=x86_64-linux-android35`.
  The NDK has no API-36 lib dir; upstream builds against android36 with
  its own clang. The HAL uses no API-36-only symbol (all binder and
  BoringSSL entry points are hand-declared in `gbos_fp.c`).
- The NDK stub `libc.so` **does** export `__libc_init`, so the
  upstream freestanding `_start` / `__libc_init` entry pattern
  (`gbos_fp.c:1556-1563`) links unchanged. Verified with `nm -D`.
- **libcrypto stub**: the NDK ships no libcrypto/libssl stub, but
  `gbos_fp.c` calls `HMAC()`, `EVP_sha256()` (Gatekeeper token check,
  `gbos_fp.c:500`) and `CRYPTO_memcmp()` (`gbos_fp.c:480`).
  `build.sh` generates a minimal link stub (`build/libcrypto.so`,
  SONAME `libcrypto.so`, 1-byte function definitions — the same
  technique as the NDK's own stubs). It is never loaded at runtime:
  `DT_NEEDED libcrypto.so` resolves to the real BoringSSL libcrypto
  in the lapis image. The TLS layer dlopens libssl/libcrypto instead
  and is not linked against this stub.
- **Binder symbols**: the NDK's `libbinder_ndk.so` stub lacks the
  process/service-manager symbols (`ABinderProcess_*`,
  `AServiceManager_*`; the NDK ships no `binder_process.h` /
  `binder_manager.h`). The real `libbinder_ndk.so` in the lapis image
  exports them — upstream links the same binary against
  image-extracted libs and it runs (boot-13/14). The link therefore
  passes `--unresolved-symbols=ignore-all`, and `build.sh` fails the
  build if the undefined-symbol set is anything other than the
  linked stubs' exports plus those 4 runtime-resolved symbols.
- The link goes through the clang driver (not raw `ld.lld`) with
  `-nostdlib`, so compiler-rt builtins (`__popcountdi2`, used by
  Milan) are pulled in.
- Fortify is off (`-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0`): the host
  build showed `__memcpy_chk` / `__memset_chk` references that Bionic
  does not provide.
- Result (26 Nov 2026): `gbos_fp` 374,712 bytes (Milan), `gbos_fp_stub`
  60,368 bytes (stub matcher). ELF 64-bit LSB PIE, x86-64, interpreter
  `/system/bin/linker64`, `NEEDED libc libbinder_ndk liblog libdl
  libz libcrypto`, `BIND_NOW`, `PIE`. Entry point is the custom
  `_start`.
- Host tests (`host_test/`, gcc + system OpenSSL): pack round-trip,
  proto build/validate incl. the `0x88` handshake marker, cell
  reassembly, CRC32-MPEG2 check values (empty `0xFFFFFFFF`,
  `"123456789"` `0x0376E6E7`), SHA-256(`"abc"`), raw12 decode, wrapped
  image CRC validation, config patch + checksum, and a full TLS-PSK
  loopback through the exact `gm_tls` server code against a real
  OpenSSL PSK client — all pass (`ALL HOST TESTS PASSED`).

## Known risks

1. **The open-driver Ubuntu test is still unproven and gates
   everything.** The sensor works in Ubuntu with the proprietary Dell
   `.deb`, which proves the hardware and firmware are good — it does
   NOT prove the open-source driver works on his firmware. Run the
   open driver / detect probe on an Ubuntu live boot first. If that
   fails, the HAL will fail the same way and no HAL debugging will
   help.
2. **PSK state is unknown on his ex-institutional machine.** Windows
   was factory-reset long before gbOS86; the Windows-side PSK copy is
   likely gone; the sensor-side PSK state is unknown. The driver
   expects the all-zero PSK unless a PSK file was imported. If the
   sensor holds a different PSK, bring-up fails terminally with
   `533c: sensor PSK hash is not the held PSK's hash` — the driver
   never writes a PSK, so there is no recovery from the HAL side; only
   re-flashing the sensor firmware clears it. The Dell driver may
   have re-provisioned it — unknown until the open-driver test.
3. **Firmware 13016.** The port was written against the firmware the
   PR author had. If his sensor's firmware differs (check the
   `533c: firmware ...` log line), command bytes may differ.
4. **Upstream 6384 unlock-bug history.** Boot-13 (19 Nov 2026): one
   touch gave 5 rapid `onAuthenticationFailed` (~290 ms apart), 30 s
   lockout, `sensor open failed: -125` after clear. Boot-14: unlock
   works. The 533c `goodix_identify` carries the lesson (one touch =
   one attempt, wait for finger-up before returning), but the 533c
   path is new code and has never been exercised against a real
   sensor. Expect the first lock-screen test to be the buggiest part.
5. **No Windows PSK copy after factory reset** (see 2).
6. **The 4 runtime-resolved binder symbols** are verified only by
   upstream's image-linked build running, not by this build. If the
   lapis image's `libbinder_ndk.so` somehow lacked them, `gbos_fp`
   would fail to start with a linker error — visible immediately in
   the boot log, and the revert is trivial (restore the original
   binary).
7. **BoringSSL symbol versioning**: the link stub's `HMAC` /
   `EVP_sha256` / `CRYPTO_memcmp` are unversioned. BoringSSL on
   Android does not version its symbols, so this should resolve; if
   the HAL fails to start with a symbol error, check the image's
   libcrypto with `readelf -sW` first.
8. **Template files are host files.** Enroll writes Milan blobs to
   `/data/vendor/crosfp/gbos_fp/`. They are not encrypted and are not
   bound to the sensor; anyone with `/data` access can read them.
   Same exposure as the upstream MOC HAL's template directory, but
   the templates are matcher inputs, not sensor entries.
9. **WEAK class only.** No TEE/GSC on a normal laptop; gbOS86 uses
   software Gatekeeper; auth-bound keystore keys are not done. Same
   as the upstream 6384 HAL.

## Files delivered

- `gbos_fp` — Android HAL binary, Milan matcher (374,712 bytes)
- `gbos_fp_stub` — Android HAL binary, stub matcher (60,368 bytes)
- `gbos_fp.c`, `gbos_fp_533c_policy.h`, `gm168sec_hal.h`,
  `gm168sec_config.h`, `fp_log.h` — HAL sources
- `milan/src/` — vendored Milan matcher (43 `.c` files; see
  `milan/milan-audit.md` for the patches applied to the copy)
- `milan/milan_shim.h`, `milan/milan_matcher.c`,
  `milan/milan_matcher_stub.c`, `milan/milan-audit.md`
- `host_test/` — host test harness (`test_gm168sec.c`, `Makefile`)
- `build.sh` — builds the HAL (`--android`, `--stub`, `--host`, `--all`)
- `hook-533c.patch` — hook.sh / gbos.cfg patch (applies to upstream
  `gbos-x86` at HEAD `284769a`)
- `README_TEST.md` — install + test procedure
- `PORT_PLAN.md` — this file
- `README.md` — deliverable index
