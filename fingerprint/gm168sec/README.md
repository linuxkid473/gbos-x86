# gbOS86 fingerprint HAL — Goodix 27c6:533c (Dell XPS 13 9310)

Drop-in `gbos_fp` binary for an existing gbOS86 install. **Not** a full
OS image build. Status 26 Nov 2026: builds and links (Milan + stub
variants); never run against a sensor. Read `PORT_PLAN.md` before
installing, and do the Ubuntu open-driver gate in `README_TEST.md`
first.

## Binaries (built 26 Nov 2026, NDK r27c Clang 18, API 35)

| file | what | size |
|---|---|---|
| `gbos_fp` | HAL with the Milan matcher | 374,712 bytes |
| `gbos_fp_stub` | HAL with the matcher stubbed out (probes/registers; enroll/identify refuse with `GM168SEC_ERR_NO_MATCHER`) | 60,368 bytes |

Both are ELF 64-bit LSB PIE, x86-64, interpreter
`/system/bin/linker64`, `NEEDED libc libbinder_ndk liblog libdl libz
libcrypto`, `BIND_NOW`.

## Sources

- `gbos_fp.c` — upstream AIDL v4 plumbing, adapted (see `PORT_PLAN.md`
  for the adaptation list)
- `gm168sec_hal.h` — 533c usbfs backend ported from PR #334 (GM168SEC
  transport, TLS-PSK, FDT, capture, host template files)
- `gm168sec_config.h` — 256-byte default config blob (from
  `device/calibration.c:238`)
- `gbos_fp_533c_policy.h` — template-deletion policy (host files,
  not sensor entries)
- `fp_log.h` — log lines (liblog + `--log` file copy)
- `milan/src/` — vendored Milan matcher (43 `.c` files)
- `milan/milan_shim.h` — glib replacement (installed as `glib.h`)
- `milan/milan_matcher.c` — Milan wiring (`gm_matcher_*`)
- `milan/milan_matcher_stub.c` — stub matcher
- `milan/milan-audit.md` — the Milan-on-Bionic decision record

## Build & test

- `./build.sh --android` — build the HAL (Milan)
- `./build.sh --android --stub` — build the HAL (stub matcher)
- `./build.sh --host` — host test harness (gcc + system OpenSSL)
- `./build.sh --all` — host tests, then the HAL

## Install & test

- `README_TEST.md` — install steps, milestones (probe → enroll →
  lock-screen authenticate), logs to capture, revert
- `hook-533c.patch` — `usb/hook.sh` + `usb/gbos.cfg` patch (applies
  to upstream `gbos-x86` at HEAD `284769a`; extends the exact-ID
  check to 533c, unbinds `cdc_acm`, keeps `FINGERPRINT=0` opt-out)
- `PORT_PLAN.md` — what was ported, what was left out, known risks
- `host_test/` — host test harness (`test_gm168sec.c`, `Makefile`)

## The one-line summary

The 533c is not match-on-chip: the sensor streams encrypted frames
over USB (TLS-PSK, all-zero PSK) and matching runs on the host in the
Milan runtime. The HAL is upstream's AIDL shell + a new blocking
usbfs backend + the vendored Milan matcher, built with the NDK
against API-35 stubs plus a generated libcrypto link stub.
