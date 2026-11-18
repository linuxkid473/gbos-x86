# Latitude 7320 front camera: Windows OV5678 settings vs Linux ov5675

Date: 2026-11-18. Hardware: Dell Latitude 7320 Detachable (this WSL host), front camera ACPI
`OVTI5678`, Windows name "Camera Sensor OV5678", module `0BF501T3`. Linux binds it with
`drivers/media/i2c/ov5675.c` (kernel #61, `OVTI5678` ACPI alias added by
`kernel/isa_emu_patch/camera_7320.py`).

Sources (read-only, proprietary, not in this repository):
`C:\Windows\System32\DriverStore\FileRepository\ov5678.inf_amd64_6ba94d9fac3d5e8c\`
`ov5678.sys` (Intel, driver version 60.22000.6053.1, 13 Oct 2021), `OV5678_0BF501T3_TGL.aiqb`,
`graph_settings_OV5678_0BF501T3_TGL.xml`, and the registry under
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\OVTI5678\0`. Reproduce the comparison with
`tools/compare_ov5678_windows.py ov5678.sys ov5675.c OV5678_0BF501T3_TGL.aiqb`.

## Summary

The chip is an OV5675 die as far as both drivers are concerned. The Windows driver accepts chip
ID `0x5675` or `0x5678` at `0x300b`, and Linux's check for `0x5675` passes on this module. The
Windows register tables are Intel's OV5675 tables with a few changes. **Exposure, analogue gain
and digital gain use the same registers and units on both systems**, so Linux sets the gain and
exposure it reports. The high gain does not come from a unit mismatch.

What does differ is the **exposure program**. Windows allows up to 66.7 ms (15 fps) in low light
and caps analogue gain at 15.5x. The Linux bridge stayed at 33.6 ms (30 fps) and went up to 16x
analogue × 4x digital (64x). The bridge now follows the Windows limits (see "Fix" below).

## Sensor control: same on both

| | Windows `ov5678.sys` (disassembled) | Linux `ov5675.c` |
|---|---|---|
| Exposure write | `0x3500` (24-bit) = lines × 8 | `0x3500` = `val << 3` |
| Exposure read-back | lines = (`0x3500` >> 4) × 2 | (comment: register is half the exposure) |
| Frame length | VTS `0x380e` = max(mode VTS, lines + 4), even, + 2 | VBLANK control: VTS = height + vblank + 10; exposure max = height + vblank − 4 |
| Analogue gain | `0x3508` 16-bit, value written as given | `0x3508` 16-bit, 128 = 1x, range 128..2047 |
| Digital gain | `0x5019/0x501b/0x501d` (MWB R/G/B), value & 0xfff, in group hold `0x3208` 0x00/0x10/0xa0 | same registers, 1024 = 1x, range 1024..4095, same group hold |
| Chip ID | `0x300b` 16-bit, accepts 0x5675 or 0x5678 | `0x300a` 24-bit must be 0x5675 (passes) |
| Link | 900 Mbit/s per lane (mode structs), 2592x1944 at 30 fps | 450 MHz link frequency (900 Mbit/s), 2592x1944 at 30 fps |

`graph_settings_*.xml` reports the sensor as `sensor_type="RGB_IR"`,
`bayer_order="GIGI_RGBG_GIGI_BGRG"`. That is the 4x4 layout the bridge measured.

## Register tables: what differs

`ov5678.sys` has four 161-write mode sequences: 2592x1944 (two copies), 2592x1952 and 2600x1952.
Each entry is 16 bytes: u32 type (1 = write, 0x20 = delay, 0xffff = end), u32 address, u32 value,
u32 0. These are the final values that differ between Windows mode 0 and Linux's
`mipi_data_rate_900mbps` + `mode_2592x1944_regs` (every other register is the same):

| Register | Windows | Linux | Assessment |
|---|---|---|---|
| `0x0300 0x0301 0x0302 0x030b 0x030d` (PLL) | 07 01 77 02 4b | 04 (–) 8d (–) 26 | A different PLL setting for the same 900 Mbit/s and 30 fps (Windows' mode struct says 900 Mbit/s). Not a cause of gain. |
| `0x380e/0x380f` VTS | 0x07d0 (2000) | 0x07e4 (2020) | 1% longer frame on Linux. Not significant. |
| `0x3501/0x3502`, `0x3508/0x3509` | exposure 0x0200, gain 0x0080 (1x) | exposure 0x3e60, gain 0x0400 | Start values only; both drivers overwrite them with the exposure/gain controls. |
| `0x5000` ISP control | **0x71** | **0x77** | Windows clears bits 1 and 2. Their function was not checked (no datasheet was used). On OmniVision raw sensors these bits are commonly the on-sensor black/white defect-pixel correction, which assumes a Bayer mosaic. **Not changed**: see below. |
| `0x3d8c/0x3d8d` | 0x71 / 0xe7 | not written | Unknown (OTP register range). **Not changed.** |

## Exposure program: Windows tuning vs the bridge

The `.aiqb` is a `CPFF` container with sections `LCMC` (module calibration), `LAIQ` (AIQ
tuning; `DFLT` and `IRVI` variants), `LISP`, and `LTHR`. Two records matter here:

- `LCMC` record 31 (format 200, floats): black level by exposure and analogue gain. It is
  calibrated at exposures of 1, 33, 66 and **100 ms** and analogue gains of 1, 2, 4, 8, 10, 12 and
  **15.5x**. Black level is 63.8–64.4 throughout, so the bridge's fixed 64 is right.
- `LAIQ/DFLT` record 261 (format 119): the AE exposure program, one row per AE mode. Each row is a
  u32 minimum exposure (µs), five u32 exposure times (µs) and six gains in 16.16 fixed point:

  | Row (our reading) | min | exposure times (µs) | gains |
  |---|---|---|---|
  | 0 (automatic) | 10 | 33333 33333 66666 33333 66666 | 1.0 15.5 4.0 18.0 1.0 2.0 |
  | 1 | 10 | 33333 33333 100000 33333 100000 | 1.0 15.5 4.0 18.0 1.0 2.0 |
  | 2 | 10 | 33333 33333 66666 33333 66666 | 1.0 15.5 4.0 18.0 1.0 2.0 |
  | 3 | 10 | 20000 33000 66666 33333 66666 | 1.0 15.5 2.0 4.0 1.0 1.0 |
  | 4 (night-like) | 10 | 20000 66000 1000000 33333 1000000 | 1.0 15.5 1.0 2.0 1.0 1.0 |
  | 5 (action-like) | 10 | 10000 10000 20000 33333 33333 | 1.0 15.5 2.0 4.0 1.0 1.0 |
  | 6 (fixed) | 8000 | 8000 8000 8000 33333 33333 | all 1.0 |

  Every row has analogue gain 1..15.5x. The automatic row lets exposure grow to 66.7 ms. The
  third and fourth gain columns (4 and 18 in the automatic row) look like a breakpoint: hold
  33.3 ms up to about 4x gain, then lengthen exposure. We have not confirmed that reading.

What Linux did (boot-8, `camera-bridge-runtime.txt`, dim room): exposure 2016 lines (33.6 ms, the
30 fps maximum), analogue 2047 (16x), digital 2250–3424 (2.2–3.3x), so 35–53x in total.

## Fix (bridge, `kernel/ipu6-loopback.c`)

- Analogue gain is capped at 15.5x (1984 for the OV5675's 128 = 1x). That is the Windows maximum
  and the top of the black-level calibration.
- New order for the exposure program:
  1. Exposure time up to the default frame (2016 lines, 30 fps).
  2. Analogue gain up to 4x.
  3. A longer frame through the sensor's `V4L2_CID_VBLANK`, up to twice the default
     (4036 lines, 66.7 ms, 15 fps).
  4. Analogue gain up to 15.5x.
  5. Digital gain.

  The 4x breakpoint is our choice and fits the reading of record 261 above.
- Write order: the driver limits exposure by the current frame, so a longer VBLANK is written
  before a longer exposure and a shorter one after. Original values (including VBLANK) are
  restored on exit. If VBLANK is missing or its ranges don't match, the frame stays fixed, as
  before.
- Lens-shading strength now uses the actual sensor gain (analogue × digital) rather than
  total ÷ maximum exposure.
- Logs: `ae: vblank ...` at start, and every 150 frames `vblank=`, frame time and gain on the
  `ae:` line.

Expected effect in the boot-8 light: 66.7 ms at about 27x instead of 33.6 ms at 53x. That is
twice the light per frame (about 1.4x better signal-to-noise), at 15 fps instead of 30 fps.

Host tests (`tools/test_ipu6_loopback.c`) cover:
- the plan's order and breakpoints;
- the 15.5x cap;
- the frame returning to 30 fps in bright light;
- the write order;
- falling back without VBLANK or with ranges that don't match.

ASan/UBSan and the docker suite pass.

## Not done / open

- **`0x5000` = 0x71 and `0x3d8c/0x3d8d`: available as an A/B switch, off by default.** Windows
  writes these for this module, but we don't know what the bits do. If bits 1–2 are on-sensor
  defect-pixel correction, clearing them would avoid Bayer-assumption correction on RGB-IR sites,
  which may help colour. It would also leave hot pixels uncorrected, and the bridge has no
  defect-pixel stage of its own.
  - `camera_7320.py` adds the `ov5675` module parameter `gbos_windows_isp`. It is writable at
    runtime and read at every stream start. When set, the driver writes the three Windows values
    after the mode table, only on DMI `Latitude 7320 Detachable` / `i2c-OVTI5678:00`.
  - On every stream start the driver logs the values in effect:
    `gbos: Windows ISP values on|off: 0x5000=.. 0x3d8c=.. 0x3d8d=..` (kernel log).
  - The bridge launcher sets the parameter from `OV5678_WINDOWS_ISP=1` in `gbos.cfg`.
  - Only `ov5675.ko` was rebuilt, from `gbos-ksrc` (the tree of the deployed kernel #61; bzImage
    unchanged). It is stripped and signed with that tree's key, and its symbol CRCs match the
    deployed module.
  - Because the sensor streams until reboot, each setting needs its own boot.
- The rear OV8856's Windows tuning (`OV8856_0BA801T3_TGL.aiqb`, record 261) has the same automatic
  limits (analogue 1..15.5x, exposure to 66.7 ms, breakpoint 4), so the shared AE plan applies to
  the rear bridge too.
- AE target (18% of raw range, metered before lens shading) is unchanged. Windows' target was not
  decoded.
- Unverified on hardware:
  - that the IPU6/CSI path and the Android USB camera HAL accept the 15 fps frame;
  - the actual noise improvement;
  - mains flicker at 66.7 ms (a multiple of the 60 Hz period but not of the 50 Hz one; the old
    33.6 ms matched neither).
