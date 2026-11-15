# Dell Latitude 5290 2-in-1 cameras (kernel side)

Applied by `kernel/build_kernel.sh` after `camera_7320.py`, through `apply.sh` (idempotent, order in `series`).

| Part | Chip | ACPI HID | Kernel driver |
|---|---|---|---|
| CSI-2 receiver | Intel IPU3 CIO2 (PCI 8086:9d32) | | `ipu3-cio2` + `ipu-bridge` |
| Camera PMIC | TI TPS68470 | `INT3472:05` | `intel_skl_int3472_tps68470`, `clk-tps68470`, `tps68470-regulator`, `gpio-tps68470` |
| Front camera | OmniVision OV5670 (5 MP) | `INT3479:00` | `ov5670` |
| Rear camera (not on every SKU) | OmniVision OV8858 (8 MP) | `INT3477:00` | `ov8858` |
| Rear focus motor | Dongwoon DW9714 | `INT3477:00-VCM` | `dw9714` |

Without these patches nothing binds: `int3472-tps68470 ... No board-data found for this model`, and I2C4 (`INT3446`, rear camera bus) fails
with `-EBUSY` because the BIOS's `GEXP` device declares an MMIO range that overlaps it (also needs `MFD_INTEL_LPSS_ACPI`, which Google's
config leaves off). The config symbols are in `kernel/configure_whl.py` (GENERIC).

| Patch | What |
|---|---|
| `0001` | TPS68470 board data can list its clock consumers statically (backport of the 5285 series' clock fix) |
| `0100` | `intel-lpss-acpi`: ignore the GEXP/I2C4 resource conflict on the 5290 2-in-1 (DMI) instead of `acpi_enforce_resources=lax` |
| `0101` | GNVS fix: the BIOS leaves `C0TP`/`L0CL`/`L1CL` at 0, so the OV5670's `_DEP` points at `PCI0` instead of the PMIC; set them to 2 (DMI-gated) |
| `0102` | TPS68470 regulator rails, GPIO lookups (reset/powerdown), both clock consumers and the DW9714 `vcc` supply for the 5290 2-in-1 |
| `0103` | `ipu-bridge`: OV8858 (`INT3477`), one link frequency, 360 MHz |
| `0104` | `ov8858`: ACPI ID `INT3477` and the `vsio` supply (the PMIC's secondary I2C passthrough the sensor sits behind) |
| `0200` | DW9714: system suspend/resume best effort (a lens I2C error no longer aborts sleep) |
| `0201` | OV5670/OV8858: force runtime suspend across system sleep |
| `0900` | OV5670: one power-cycle retry when stream start fails |
| `0901` | OV5670: 20 ms extra settle time after power-up |
| `0902` | re-apply the GNVS fix on resume |
| `0903`, `0904` | replay the TPS68470 clock dividers, regulator voltages, camera GPIO sleep state and secondary I2C bridge (vitovt's hibernate fix) |
| `0905` | run that replay on every resume, not only after hibernate (on hardware, every camera session after an s2idle cycle delivered black frames) |

Source: [vitovt/dell-5290-camera-kernel](https://github.com/vitovt/dell-5290-camera-kernel), tested on 5290 2-in-1 hardware with both
cameras through suspend/resume (Ubuntu 6.17 kernel). It adapts Thierry Chatard's Latitude 5285 series (linux-media / platform-drivers-x86,
"Enable cameras on Dell Latitude 5285 2-in-1"). Kernel patches, GPL-2.0 like the files they change. 0105 and 0905 are ours; vitovt's 13
apply to kernel/common android17-6.18 @ 3938c8d9, with 0001 and 0102 re-anchored (same lines added, other places in `tps68470.h` and
`tps68470_board_data.c`) so they combine with the Latitude 7320 edits of `kernel/isa_emu_patch/camera_7320.py`, whichever runs first.

The OV5670 and OV8858 cannot stream at the same time (shared PMIC, the rear sensor is behind the PMIC's I2C passthrough).

Userspace: the sensors give raw Bayer only (no ISP is used), so `kernel/ipu3-webcam.c` debayers it and feeds Android's USB camera HAL
through v4l2loopback; see `usb/device/README.md`.
