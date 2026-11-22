# Experimental HID IIO Sensors sub-HAL

**2026-11-17: gyroscope and ambient light added (Latitude 7320).** On the Latitude profile the
`gyro_3d` device now gets the same treatment as the accelerometer: its own current
`in_anglvel_sampling_frequency` as the only rate, a provisional ±2000 °/s range (no range
attribute is exposed), and the identity axis mapping. The accelerometer's `(-x,-y,-z)` is the
gravity sign convention (HID/Windows report -1 g on the axis facing up, Android +9.81) on the
same axes, which angular velocity does not have. A `LightSensor` (Android `LIGHT`, on-change)
polls the first HID `als` device's `in_illuminance_raw` at 5 Hz while active, converts with
`in_illuminance_scale`/`_offset`, and reports on activation and whenever the value moves by more
than 1 lux and more than 2%; it needs no IIO buffer or extra permissions. Both are unverified on
hardware: check that rotating the laptop gives gyroscope readings with the right signs and that
Settings offers adaptive brightness.

These files are based on the Apache-2.0 Trogdor Sensors 2.1 sub-HAL at
[`a8a85fe07e1489a48eb5ac519fd8f400e21d2307`](https://github.com/YAJATapps/android_device_google_trogdor/tree/a8a85fe07e1489a48eb5ac519fd8f400e21d2307/hal/sensors).

**Experimental Latitude 7320 trial.** Auto-rotation was confirmed working by
the user on the 2026-11-15 Live boot: Android enumerated the accelerometer and
the display followed sensor-driven rotations. When the image contains this
library and `SENSOR_HAL=1`, the boot hook selects it only
for Dell Inc. / Latitude 7320 Detachable / board 0TNW0H. The HID path checks
the kernel scan layout, decodes signed samples, applies IIO offset/scale and
converts timestamps. The trial uses a provisional ±2 g range only because this
device exposes no `raw_available` range attribute, and a pose-derived
`(-raw_x, -raw_y, -raw_z)` axis transform. The transform worked for the
observed test poses, but the physical full-scale remains unknown; the ±2 g
range is not authoritative. Set `SENSOR_HAL=0` in `gbos.cfg`
to keep the stock HAL selected. Because this IIO device does not advertise an
available-rate list, the trial uses its captured current 10 Hz setting as the
only rate rather than applying the source HAL's generic 20 Hz minimum.

The host regression test exercises discovery, axis validation, IIO decoding,
offset/scale, scan setup and sysfs write errors:

```
g++ -std=c++17 -Isensors/trogdor/tests/include -Isensors/trogdor \
  sensors/trogdor/test_hid_iio_support.cpp sensors/trogdor/iio_utils.cpp \
  -o /tmp/gbos-hid-iio-test && /tmp/gbos-hid-iio-test
```

## Experimental compact build

`build_compact.py` builds and links without a full Android platform checkout.
It requires separately prepared inputs, not downloads performed by the script:

```
INPUTS/
  ndk/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/sysroot/
  vndk-headers/
  libcxx17/
  multihal-headers/
  icu/unicode/
  xsdc-output/include/sensor_hal_configuration_V1_0.h
  xsdc-output/sensor_hal_configuration_V1_0.cpp
  xsdc-output/sensor_hal_configuration_V1_0_enums.cpp
  vendor-lib64/
```

For the local build, the sysroot headers and x86-64 API 35 link stubs came
from Android NDK r27d; the HIDL headers came from VNDK v30; and the platform
libc++, multi-HAL and ICU headers came from Android 17. The generated parser
was produced with Android 17 `xsdc` and the public
[sensor schema](https://github.com/boundarydevices/android_hardware_imx/blob/257f3930d5a95045d3618ad91e7d2d5cf53aa135/sensor/config/sensor_hal_configuration.xsd).
The shared libraries were copied read-only from the exact target vendor image,
not the repository. Do not commit the image or extracted libraries.

Without `xsdc-output/` the script uses `xsdc_stub/` instead: the same parser types with a
`read()` that finds no `sensor_hal_configuration.xml`, which is what Google's desktop images
(none ship that file) always produced. That removes the `xsdc-output/`, `icu/` and `libxml2.so`
inputs. The 2026-11-17 build used: Android NDK r27d (`android-ndk-r27d-linux.zip`, SHA-256
`601246087a682d1944e1e16dd85bc6e49560fe8b6d61255be2829178c8ed15d9`, sysroot only);
`platform/prebuilts/vndk/v30` at `5f9884aa352825291757dfd6694b874ad8c1805e` (`x86_64/include` only, as `vndk-headers/`);
`platform/hardware/interfaces` at `1a56e38edc2f2f6189ef405ee1edce554e15cbc0`
(`sensors/common/default/2.X/multihal/include` as `multihal-headers/`); the libc++ headers of the
local Android clang r584948b (`include/c++/v1`, `std::__1`, ABI 1) as `libcxx17/`; and the
thirteen libraries from the target `vendor_a.img` (liblog from `system_a.img`). Keep the inputs
under the ignored `build/sensor-hal-inputs/`.

With an Android-capable clang and the inputs available in the same environment:

```
python3 sensors/trogdor/build_compact.py --inputs INPUTS --clang CLANG++ --output /tmp/rotation-test.so
```

The compact build produces an x86-64 Android library with the 2.1 sub-HAL entry
point and no unresolved link-time symbols. This does **not** establish runtime ABI
compatibility: the VNDK headers, NDK stubs, platform headers and image libraries
come from different releases, and the schema has not been validated against
the original HAL's schema.

To compile and include the experimental binary in a standard or looper image,
prepare the inputs above, make sure Docker has the existing
`gbos-kbuild` image and `gbos-ksrc` volume, then run:

```
GBOS_SENSOR_BUILD_INPUTS=/path/to/inputs ./build.sh --standard
```

The build uses `/build/inputs/clang-r584948b/bin/clang++` inside the
`gbos-kbuild` container. Override the container-side compiler path with
`GBOS_SENSOR_BUILD_CLANG` only if you prepared a different one there.

The image includes `gbos/sensors/android.hardware.sensors@2.1-trogdor-subhal.so`;
the installer copies it on install or update. On the exact Latitude 7320 DMI,
the early-patch hook creates a tmpfs-backed vendor overlay before existing
library binds, then the Android hook adds the library to `hals.conf`; other
devices keep the stock HAL. The build fails if requested inputs are
missing or compilation fails. Without these variables the regular build omits
the library, so the hook leaves the stock HAL selected. Dual-boot installation
does not support this experimental payload.

The overlay storage is under stage0's writable `/dev` tmpfs, moved into the
new root before `switch_root`. Failed preparation logs the failing command and
error, cleans up mounts when possible and leaves the stock HAL selected for
that boot. When selected, `sensor_trial_diag.sh` records timestamped framework
sensor and display snapshots in `sensor-trial.txt` for about five minutes
without opening or changing the IIO buffer. These snapshots may not contain
events if Android has not activated the accelerometer.

## Latitude 7320 display-policy trial

The cached Android 17 partitions identify the policy blocker: the stock
`framework-res.apk` defaults `config_supportAutoRotation` to `true`, but
`vendor/overlay/framework-res__lapis__auto_generated_rro_vendor.apk` sets it
to `false`. The experimental policy RRO sets only that boolean back to `true`
and has a higher static-overlay priority than the generated vendor RRO. The
boot hook stages it into the same temporary vendor overlay as the Sensors HAL,
only for the exact Latitude 7320 DMI and only while `SENSOR_HAL=1`.

Desktop mode separately defaults the display to fixed-to-user rotation, and
Google's `DisplayRotation$LaptopController` resets it to `default` (fixed on a
PC) and locks the rotation on every device-state callback outside tablet state:
at boot, and after every lid or tablet-switch change. A one-shot
`cmd window fixed-to-user-rotation disabled` therefore only won when it ran
after the boot-time reset (lost on boot-9, 2026-11-18). After
`sys.boot_completed`, `usb/tablet_mode.sh` (service `gbos_tablet_mode`, exact
Latitude 7320 DMI) re-applies `disabled` whenever it reads anything else: every
2 s for the first minute, 1/3/6 s after every lid or tablet-switch event, then
every 30 s. Each re-apply is logged in `tablet-mode.txt`. `ROTATION_OVERRIDE=1`
also sets `config.override_forced_orient` (opt-in, because it applies to
external displays too). The rotation lock tile still decides between free
and locked rotation. `sensor-trial.txt` also captures the mode, active
framework overlays and display dump. This changes neither the accelerometer transform nor the Android rotation
setting. The 2026-11-15 Live test confirmed `mSupportAutoRotation=true`,
`mFixedToUserRotation=false`, active accelerometer events and sensor-driven
transitions through `ROTATION_0`, `ROTATION_90`, `ROTATION_180` and
`ROTATION_270`. Setting `SENSOR_HAL=0` skips both the sensor HAL and the
display-policy changes.

The overlay is committed signed as
`usb/display/gbos-latitude7320-rotation-policy.apk` and ships with `display/`
(`deploy_stick.sh`, `make_gbos86_image.sh`, `install.sh`); `hook.sh` stages it
from `$G/display/`. Rebuild it only when `display-policy/` changes, and check
it, with:

```
sh tools/build_rotation_policy_overlay.sh          # build (throwaway signing key, none kept)
sh tools/build_rotation_policy_overlay.sh --check  # v2 signature + equality with a fresh build of the sources
```

It uses `tools/build_static_rro.py` (needs python3 with `cryptography`, no
aapt2 or framework-res). Its manifest and `resources.arsc` are byte for byte
the aapt2 build used on hardware until 2026-11-19; that build had no APK
signature, so PackageManager never registered it (absent from every
`cmd overlay list android` in boot-8..13) and auto-rotation worked without it.
`sensor-trial.txt` logs `cmd overlay lookup --verbose` for
`config_supportAutoRotation`, which names the overlay that supplies the value.

On the Latitude 7320, three user-confirmed static poses suggest the
accelerometer transform `(x, y, z) = (-raw_x, -raw_y, -raw_z)` after scaling:
upright was approximately `(705, -934620, -376174)`, clockwise
`(1019743, -41522, -52941)`, and flat screen-up
`(17533, -27873, -1016356)`. The trial applies this provisional transform on
the exact Latitude 7320 profile. The 2026-11-15 Live test subsequently
confirmed sensor-driven changes through all four Android display rotations,
and the user reports correct rotation for the tested poses. This is behavioral
validation of the tested accelerometer mapping, not a physical range
measurement or broader calibration. The earlier #57 marker-free motion phases
do not independently label each pose.

The former 32768 raw-unit fallback was less than the observed accelerometer
readings. On this one DMI profile, missing `raw_available` now uses a provisional
2,000,000 raw-count max (about 19.612 m/s² at the captured IIO scale); malformed
or present-but-invalid range attributes are still rejected, and other HID
devices still require an explicit valid IIO range. This is a trial value, not
an authoritative sensor setting. In the 2026-11-13 capture, the exact 24-byte scan
returned successfully and its timestamp matched `CLOCK_REALTIME`, while Android
sensor events require boot-time timestamps. The experimental event path now
converts realtime IIO timestamps to `CLOCK_BOOTTIME`; a host test covers the
conversion and invalid/overflow cases.

That capture also showed that ueventd had granted the `system` group access to
the IIO buffer and scan-enable controls, but left
`in_accel_sampling_frequency` and `in_anglvel_sampling_frequency` as
`root:root 0644`. On the 2026-11-13 12:35 boot, the original hook's copy of
the vendor ueventd config failed and those controls remained `root:root 0644`.
The hook now preserves the vendor's imported ueventd directory in a tmpfs
overlay and adds narrowly scoped rules for the two controls. The new overlay
was present in the test path, and the 2026-11-15 boot confirms the HAL had
sufficient access to deliver accelerometer events. This does not validate
access under SELinux enforcing policy. Run `sh tools/test_sensor_ueventd.sh`
to test the rule addition and failure handling.

The physical sensor full-scale range remains unknown and the HAL's ±2 g value
is provisional. The 2026-11-15 Live test verified framework enumeration and
event delivery, and the user confirmed rotation worked for the tested poses.
This validates the trial behavior on this device, not its physical range or
calibration beyond those observations; do not infer either from service
registration alone.

`usb/sensordiag.sh` captures up to 4096 bytes of each Intel ISH HID report
descriptor in `sensors.txt`. The 2026-11-14 kernel #61 capture includes the
accelerometer collection (`001F:8087:0AC2.0008`, `HID-SENSOR-200073.20`).
Its X/Y/Z input fields are signed 32-bit values with logical limits spanning
the full signed integer range, unit exponent -6, and no physical minimum or
maximum. The descriptor has no standard `RANGE_MAXIMUM` or `RANGE_MINIMUM`
feature fields. This describes the HID representation, not the accelerometer's
configured full-scale range; the IIO `in_accel_scale` is a unit conversion,
not that range. The current configured range therefore remains unknown and
requires an authoritative value from the ISH firmware/device configuration
path. Do not infer it from logical limits, the selectable ranges in a part
datasheet, or observed motion samples.

For a one-image motion trace, build with
`GBOS_SENSOR_MOTION_PROBE=1`. This adds `SENSOR_EVENT_PROBE=1` and
`SENSOR_MOTION_PROBE=1` to the staged image's `gbos.cfg` only; it does not edit
`usb/gbos.cfg` or select the experimental HAL. After a 60-second settling
delay, `sensordiag.sh` records six timestamped, 10-second accelerometer phases
and restores the IIO buffer and scan controls. The phases are portrait
top-up, clockwise landscape, portrait top-down, counter-clockwise landscape,
screen-up flat, and screen-down flat (held securely, not placed on the
display). The trace contains raw scan bytes bracketed by phase timestamps.
The timer starts when Android reports boot completion, not when the desktop
first becomes visible, and the logged labels do not independently verify the
tablet's pose. The motion probe filters the input stream in memory and records
only timestamped volume-up and space key events: press either at each known
pose and note the order. The input-event timestamps can be aligned with the
logged uptime/realtime pairs and IIO samples; successful capture on this
hardware still needs verification. This is diagnostic only; it does not prove a
calibrated range or enable rotation.

For a marker-free trace, use `GBOS_SENSOR_CONTINUOUS_PROBE=1` when building a
test image. The diagnostic captures up to 180 seconds of consecutive IIO
scans from boot completion, without reading keyboard or volume keys. Starting
in clockwise landscape, repeatedly turn clockwise through the four screen
orientations, holding each for several seconds. Continue until at least five
minutes after the desktop appears, then shut down cleanly. The logged scan timestamps and
repeated transitions help analyze motion without assuming phase boundaries,
but do not independently label poses or establish physical range. An early
scan-limit warning means the trace ended before the full 180 seconds.
The trace reports the buffer settings immediately before and after enable.
After collecting a trace, run `python3 tools/analyze_iio_trace.py sensors.txt`
on a host to calculate delivered scan rate from the embedded IIO timestamps and
compare the median scan interval with the logged sysfs sampling-frequency
setting. The analyzer also reports the whole-trace average and largest gap, so
an isolated stall is not mistaken for a consistently low sample rate. Any
mismatch does not by itself identify whether the HID device, kernel trigger, or
reader caused the rate difference. Pass `-` instead of a path to read the log
from standard input.

To investigate buffered events on a later test image, add the exact line
`SENSOR_EVENT_PROBE=1` to its `gbos/gbos.cfg`. Once per boot, the diagnostic
checks for one idle `accel_3d` with the expected scan layout, temporarily
enables its channels and buffer, reads one 24-byte scan using a matching `dd`
block size with an eight-second timeout, logs realtime and uptime before and
after the read, and restores the controls. The 2026-11-12 21:00 capture used
`od` directly on the IIO character device and got `EINVAL`; this diagnostic
now reads an exact scan into a temporary file before formatting it. It also
logs the IIO control-file permissions, relevant because the Sensors HAL runs
as `system` while the diagnostic runs as root. It skips active or unexpected
devices and reports errors in `sensors.txt`. Remove the setting to disable the
probe. This is diagnostic only: it does not select the experimental HAL or
prove its runtime access.

## Latitude 7320 adaptive brightness

The sub-HAL's `als` light sensor reaches Android, but Google's framework config
switches automatic brightness off: `config_automatic_brightness_available` is
`false` in `framework-res.apk` and again in the Lapis vendor overlay
(`framework-res__lapis__auto_generated_rro_vendor.apk`, priority 0). With it off,
`DisplayDeviceConfig.isAutoBrightnessAvailable()` is false, DisplayPowerController
opens no light sensor and Settings hides "Adaptive brightness" (boot-13:
`mAutoBrightnessAvailable= false`, `mLightSensor=null`, every `BrightnessEvent`
`lux=-1.0 ... FallbackBrightnessStrategy`).

With the sensor HAL selected (exact Latitude DMI, `SENSOR_HAL=1`) and
`ADAPTIVE_BRIGHTNESS` not `0`, `hook.sh` adds two files to the sensor HAL's
writable vendor overlay:

- `/vendor/overlay/gbos-latitude7320-brightness.apk` (`usb/display/`): a signed
  static overlay of `android`, priority 101, that sets only
  `config_automatic_brightness_available=true`. Source:
  `brightness-policy/res/values/config.xml`. `tools/build_brightness_overlay.sh`
  builds it with `tools/build_static_rro.py` (no aapt2 or apksigner: it writes
  the manifest and `resources.arsc` exactly as aapt2 does, and signs with APK
  Signature Scheme v2 and a throwaway key); `--check` verifies the committed APK
  against the sources.
- `/vendor/etc/displayconfig/display_port_0.xml` (`usb/display/latitude7320_display_port_0.xml`):
  the built-in panel's lux curve, in backlight units, about `0.09 * lux^0.27`
  (0.04 in the dark, 0.40 at 250 lux next to the 0.40 default brightness, full at
  8000 lux). Google ships `display_port_1..10.xml` only. If a later build ships
  its own `display_port_0.xml`, the hook keeps it.

The lux curve can't go in the overlay: Google's product overlay
`FrameworkDesktopOverlay` sets the `config_autoBrightness*` arrays, and product
overlays rank above vendor ones. Without the display config, Google's desktop curve
(0.27 at 300 lux) applies. `screen_brightness_mode` is not written: Google's
desktop SettingsProvider overlay already defaults it to automatic, and the
user's later choice stays. `sensor-trial.txt` logs the overlay list and the
display's auto-brightness state in every snapshot.
