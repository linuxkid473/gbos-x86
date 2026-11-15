# Device support: IPU3 cameras, rotation, tablet mode, power

Copied to the stick as `gbos/device/` (and by the installer to the internal disk). Made for the **Dell Latitude 5290 2-in-1**
(OV5670 front, OV8858 rear with DW9714 autofocus, TPS68470 PMIC); the same path should serve other IPU3 machines whose sensors have
a mainline driver (Surface Pro 4-6, Surface Book, Surface Go: OV5693/OV8865/OV7251).

| File | What |
|---|---|
| `ipu3-camera.sh` | started by the `gbos_ipu3cam` service in `hook.sh` when an IPU3 CIO2 (PCI 8086:9d32) is present: re-probes sensors that probed before their PMIC, loads v4l2loopback (one node per sensor, `/dev/video42`...), dresses each node as a UVC device in sysfs for Google's USB camera HAL (front/back via `physical_location/panel`), runs one `ipu3-webcam` per sensor and restarts the camera HAL |
| `ipu3-webcam` | the bridge, a static binary, **not committed**: `tools/build_camera_tools.sh` builds it from `kernel/ipu3-webcam.c` (`build.sh` does it too) |
| `fake_uvc_desc.bin` | UVC descriptors shown to the HAL (YUYV 1920x1080, 1280x720, 960x540, 640x480, 640x360, 320x240); the script patches the product id per camera |
| `diag.sh` | `gbos_tabletdiag`, 75 s after boot: cameras as Android sees them, IIO rotation sensors, tablet-mode switch -> `logs/boot-N/tablet.txt` |
| `autorotate.sh`, `gbos-autorotate` | `gbos_autorotate`: screen rotation from the IIO accelerometer (Google's image has no sensors HAL, so Android lists no sensors), and tablet mode: from SW_TABLET_MODE the internal i8042 keyboard is unbound (Android then sees no hardware keyboard and switches to its tablet layout) and the on-screen keyboard is allowed; built from `kernel/gbos-autorotate.c` |
| `power.sh` | `gbos_power`: CPU power profile at boot_completed. Laptops (a battery present): `balanced` = intel_pstate `powersave` governor (dynamic frequency) with EPP `balance_performance` (ChromeOS's default; `balance_power` made camera processing twice as slow on battery) and ACPI platform profile `balanced`. Desktops and software-rendering machines: `performance` (the previous behaviour for everyone). `powersave` uses EPP `balance_power`. Log: `power.txt` |

What the bridge does per frame (no ISP: the IPU3 ImgU is not used): unpack the IPU3 10-bit packing, bilinear demosaic straight
into the output size (crop/scale/rotate in one pass on a persistent 4-thread pool, AVX2 build; ~3-4 ms per 1080p frame on a desktop),
white balance, gain, highlight roll-off, sRGB gamma and a mild contrast curve via tables, YUYV out. Auto exposure drives the sensor's
own exposure/analogue/digital gain with anti-flicker steps, pulls clipped highlights down fast, and stretches the frame time down to
20 fps only once analogue gain passes 4x; digital and software gain are capped at 2x each. Grey-world white balance; contrast autofocus
on the rear lens; a Laplacian sharpening pass on luma, strong at low gain and fading at high gain (the OV5670 front module is fixed
focus); motion-adaptive temporal noise reduction that only acts at high gain. In the dark the frame time stretches to 20 fps to keep
analogue gain at 4x, then to 10 fps (`--dark-fps`) before analogue gain goes past that; then digital gain to 4x and software gain to
8x with stronger noise reduction. A sensor that only delivers flat black frames (no read noise, so not just a dark scene) is
power-cycled. Eight capture buffers; when processing falls behind, stale frames are skipped and their buffers returned at once, so
the CIO2 never runs out of buffers ("no ready buffers found on DMA channel"). The sensor streams (and its LED lights) only while an app has the camera open, with a 2 s grace period so an app that closes
and reopens the camera while switching modes does not restart the sensor. The two sensors share the TPS68470 and cannot stream at
once: the bridges hold a shared lock (`/data/local/tmp/ipu3-webcam.lock`), and a bridge in its grace period hands the sensor over
at once when the other camera is asked for. Size requests below 1280 wide use a 1280-class mode, the mode is never lowered while
streaming, and a size change only switches mode after it has held for 300 ms (apps ask for 320x240 for an instant while switching).

Kernel side (drivers, Dell board data, quirks): `kernel/dell5290_camera/`, config in `kernel/configure_whl.py`.

## gbos.cfg

| Setting | Effect |
|---|---|
| `CAMERA=0` | do not start the bridges |
| `CAMERA_FLICKER=50` / `60` / `0` | mains frequency for anti-flicker (default: from the time zone) |
| `CAMERA_ROTATE_ov5670=180` | rotate one sensor's picture (0/90/180/270; default: the ACPI rotation the sensor reports) |
| `CAMERA_MIRROR_ov5670=1` | mirror one sensor's picture |
| `AUTOROTATE=0` / `always` | screen rotation off / also with the keyboard attached (default: only in tablet mode) |
| `AUTOROTATE_MAP=-y,-x,+y,+x` | accelerometer axis that points down in rotation 0, 90, 180, 270 (the default fits the 5290 2-in-1) |
| `CPU_PROFILE=balanced` / `performance` / `powersave` | CPU power profile (default: `balanced` on laptops, `performance` on desktops and with software rendering) |
| `TABLET_KEYBOARD=0` | do not switch the on-screen keyboard on in tablet mode (default: on with the keyboard detached, off when attached; the 5290's built-in PS/2 keyboard device stays present when detached, so Android would otherwise never show it) |

## Logs (on the stick after a boot, `gbos/logs/boot-N/`)

- `hwdetect.txt`, section `== mipi cameras`: ACPI camera devices, which I2C sensor has a driver, `ipu3-webcam --probe`, kernel messages
  (`No board-data found` = the 5290 patches are not in the running kernel; `supply ... not found, using dummy regulator` = the sensor
  probed before the PMIC).
- `camera.txt`: what `ipu3-camera.sh` found and started. `camera-front.txt` / `camera-rear.txt`: the bridges (modes, AE/AWB/AF every
  10 s). `front-WxH.ppm` / `rear-WxH.ppm`: the 90th frame of the first session, to judge colour and exposure.
- `tablet.txt`: `dumpsys media.camera`, sensors, tablet mode, `gbos-autorotate --read`. `autorotate.txt`: rotation decisions.
- Camera logs print fps and processing ms per frame every 10 s.

Self-test without hardware: `usb/device/ipu3-webcam --selftest` (pack/unpack, all four Bayer orders, rotation, mode choice, speed).
