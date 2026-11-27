# Milan matcher on Bionic — audit

Verdict: **yes, the Milan matcher builds for Bionic** with a small glib
shim, one 1-line libdeflate→zlib swap, deletion of 5 dead libfprint
includes, and removal of the GVariant FpPrint envelope from `print.c`.
No algorithm rewrite. Verified by compiling all 43 sources with host
gcc (26 Nov 2026) and linking them into the Android HAL binary with
NDK Clang 18 (see `build.sh`, `PORT_PLAN.md`).

## What Milan is

`milan/` is the vendored matcher from seaweeduk/goodix53x5-libfprint,
branch `pr334` (PR #334, head `52584a5`), `drivers/goodix53x5/milan/`.
It replaces the old SIGFM/OpenCV matcher. 73 files, 44 `.c` + 29 `.h`,
30,804 lines. It is pure C: no OpenCV, no OpenSSL, no zlib (except the
one CRC call below), no `<math.h>`, no stdio, no threads, no GObject,
no main loop, no logging in the matcher itself.

## Dependency surface (verified by grep, 26 Nov 2026)

Only two non-libc includes in the whole subtree:

| include | where | resolution |
|---|---|---|
| `<glib.h>` | 4 headers: `match/match.h:22`, `print.h:13`, `runtime.h:13`, `study/queue.h:13` | `milan_shim.h` (installed as `glib.h`) |
| `<libdeflate.h>` | `template/codec.c:19`, one call `template/codec.c:490` `libdeflate_crc32(0, data, size)` inside `milan_template_crc32()` | swapped to zlib `crc32()` from libz — same IEEE polynomial/init; the HAL links `-lz` |

libfprint dependency is **zero**: `drivers_api.h` is included by 4
files (`enrollment/template.c:13`, `match/info.c:13`,
`match/lifecycle.c:22`, `match/study.c:22`) but no symbol from it is
used; `driver-private.h` is included only by `match/study.c:23`,
nothing used from it — deleting that one line severs the only link to
`fpi-device.h`/`fpi-ssm.h` and the device struct. `fpi-print.h` is
included nowhere; `FpPrint` appears nowhere in `milan/`. No stub
header is needed. All 5 dead includes were deleted from the vendored
copy (see "Patches to the vendored copy" below).

## Glib surface actually used (counts from the vendored tree)

- `GBytes` (85 refs, 15 files) — the template currency. Shim provides
  new/new_take/ref/unref/get_data/get_size/equal (~60 lines).
- `GError` — only in `print.c` + `runtime.c`
  (`g_error_new_literal`, `g_set_error_literal`, `g_clear_error`).
  `G_DEFINE_QUARK` defines `func##_quark()` (one quark, `print.c:19`).
- `GPtrArray` — 4 refs (gallery list + `combine_templates` signature
  `match.h:163`): new_with_free_func/add/unref/index.
- `g_malloc/free/new0/new/memdup2/try_malloc/try_malloc0/try_new0/
  malloc0/clear_pointer/steal_pointer` — 1:1 libc mappings.
  `g_free` must be a real function, not a macro: it is passed as a
  `GDestroyNotify` to `g_clear_pointer` (`match/info.c:662`,
  `runtime.c:235`).
- `g_autofree` (16 sites) / `g_autoptr` (9 sites: `GBytes` ×7,
  `GVariant` ×1 in the deleted code, `GoodixMilanEnrollmentTransaction`
  ×1) — Clang `cleanup` attribute. Note: glib's `g_autoptr(T)` expands
  to `__attribute__((cleanup(f))) T *` — attribute first, then the
  **pointer** type. An attribute-only expansion compiles under GCC
  (implicit int, warning) but is an error under Clang.
- `g_return_if_fail` / `g_return_val_if_fail` (13 sites).
- `g_bytes_equal` (1), `g_strcmp0` (1), `GUINT32_TO_LE` (1, → `htole32`).
- `G_STATIC_ASSERT` (in `match/info-private.h` etc.),
  `G_STRUCT_OFFSET` / `G_ALIGNOF` (→ `offsetof` / `_Alignof`),
  `G_N_ELEMENTS`, `MAX`/`MIN`, `G_MAXSIZE`/`G_MAXUINT`/`G_MAXINT` —
  all macros, added to the shim as the compile found them.
- Debug-only glib (`g_compute_checksum_for_data`, `g_strlcpy` in
  `debug-hooks.c:30,49`) is behind `GOODIX53X5_DEBUG`/`PARITY` guards
  with no-op stubs in `debug-hooks.h` — `debug-hooks.c` is excluded
  from the build entirely; the shim does not provide them.

Deliberately **not** shimmed: `GVariant` (only in `print.c`'s
`goodix_milan_print_build_data`/`parse_data`, the libfprint `FpPrint`
envelope — see below), `GList`/`GObject`/`GTask`/`GMutex`/`GByteArray`
(unused anywhere in the tree).

## GVariant / the print envelope

`print.c:149-240` wraps templates in a `GVariant "(uuuusay)"` envelope
(schema 4, profile 9, sensor 12, antifake 1, `"canonical-zero-v1"`).
That envelope exists only to interoperate with libfprint's `FpPrint`;
Android has no `FpPrint`. `build_data`/`parse_data` were deleted from
the vendored `print.c`/`print.h` (with a comment noting the removal);
`goodix_milan_print_validate_template` (`print.c:31`) is kept — it is
the only print function `runtime.c` calls (`runtime.c:474,615`).
Templates are stored verbatim: the published `GBytes` are written to
disk as-is by `goodix_enroll` (`gm168sec_hal.h` `gm_tpl_save`).

## Entry points (what `milan_matcher.c` calls)

- `goodix_milan_runtime_run` / `goodix_milan_runtime_input_new`
  (`runtime.h:185,245`, impl `runtime.c`): take `const guint16
  setup_tx_on[9504]` + `const guint16 live_raw[9504]` (12-bit values
  in 16-bit words, NOT packed raw12) plus a gallery of `GBytes`
  templates, and copy both frames. Purpose is
  `GOODIX_MILAN_PURPOSE_IDENTIFY` (0) or `GOODIX_MILAN_PURPOSE_ENROLL`
  (1) (`preprocess/state.h:31-34`). tcode/dac come from the OTP
  calibration; sensor subtype is 12 (Milan profile 9).
- `goodix_milan_runtime_enrollment_admitted` (`runtime.h:736`): the
  admission gate — preprocess valid, probe template present, quality
  >= 25, coverage >= 65 (`GOODIX_MILAN_ENROLL_MIN_QUALITY/COVERAGE`,
  `runtime.h:173-174`).
- `goodix_milan_enrollment_transaction_*` (`match/match.h:41-56`):
  accumulate accepted samples; `publish` returns the finished
  template as `GBytes`. 12 samples (`GOODIX_ENROLL_SAMPLES`).
- The enroll run already matches the gallery
  (`runtime.c: goodix_milan_runtime_match_gallery` is called for both
  purposes), so duplicate detection reads `output->gallery_results`
  (`accepted` + `gallery_index`) — no second identify pass.
- `goodix_milan_template_pack/unpack` (`milan.h:483-496`) are glib-free
  plain C and are used internally by the runtime; the HAL never calls
  them directly.
- The HAL's verdict comes from `output->status`
  (MATCH/NO_MATCH/RETRY), never from a hand-rolled score threshold
  (threshold tables live in `match/policy.c`).

## Patches to the vendored copy

All inside `milan/src/`, each marked with a `gbOS86-533c` comment:

1. `print.c`: deleted `goodix_milan_print_build_data` and
   `goodix_milan_print_parse_data` (GVariant envelope, upstream
   lines 149-240). `print.h`: deleted their declarations.
2. `template/codec.c:19,490`: `#include <libdeflate.h>` →
   `#include <zlib.h>`; `libdeflate_crc32(0, data, size)` →
   `crc32(0, data, (uInt)size)`.
3. Deleted `#include "drivers_api.h"` from `enrollment/template.c`,
   `match/info.c`, `match/lifecycle.c`, `match/study.c`, and
   `#include "driver-private.h"` from `match/study.c`.
4. `milan/src/milan -> .` symlink so `#include "milan/..."` resolves
   with `-Imilan/src` (created by `build.sh` if missing).

## Build verification (26 Nov 2026)

- Host gcc 13.3, `-std=c11 -O2`, shim as `glib.h`, `-lz`: all 43
  `.c` files (everything except `debug-hooks.c`) compile; `ld -r`
  link of all objects + `milan_matcher.c` succeeds.
- Remaining undefined symbols after the `ld -r` are all resolvable:
  libc (`calloc/free/malloc/memcpy/memcmp/memset/memmove/realloc/
  qsort/snprintf/strdup`), zlib `crc32`, `htole32`, compiler-rt
  `__popcountdi2`, `__stack_chk_fail`, and `__memcpy_chk`/
  `__memset_chk` (host fortify only — the Android build passes
  `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` so they are not emitted).
- Android NDK Clang 18, `--target=x86_64-linux-android35`,
  `-ffreestanding`: all 45 sources (gbos_fp.c + 43 Milan + matcher)
  compile; the full HAL links (see `PORT_PLAN.md` for the binder and
  libcrypto stub notes).

## What was NOT verified

- Nothing was run against a sensor: no runtime/sensor testing. The
  open-driver Ubuntu test on Kanaan's hardware is still the gate
  before any HAL test means anything (see `PORT_PLAN.md`).
- CRC equivalence (`libdeflate_crc32` vs zlib `crc32`) is by spec
  (both IEEE CRC-32, init 0), not by test vector. The host test
  harness checks the HAL's own CRC32-MPEG2 (a different CRC) against
  its published check value, not this one.
- Match quality / false-accept behaviour is entirely Milan's; the HAL
  adds no threshold of its own.
