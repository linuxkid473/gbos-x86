# Latitude 7320 camera and Goodix diagnostic

**Latest Live image:** `build/camera-goodix-v4l2-bounds-final-20261117/gbOS86-20261117.img`

**SHA-256:** `4e0d6471afe703ea5b2f609e3b1fb819540c404edc7653b4b20abb7e37bd1c7c`

This `MODE=ask` Live image combines the opt-in front-camera bridge, the
Latitude-only 640×480 UVC descriptor and bounded loopback module, and the latest
instrumented Goodix HAL and hook. It sets `CAMERA_BRIDGE=1`, `SENSOR_HAL=1`, `DEBUG_LOGS=1`, and
`FINGERPRINT=1`; the Goodix service is scoped to USB `27c6:6384`. Rotation HAL
and policy assets match the previously working image byte-for-byte. The
fingerprint service is enabled for diagnostics only; do not enroll.

The 19:03 boot showed that the bridge producer exited about one second after
starting. Its init service is `oneshot`, but the setup script had detached the
long-running bridge with `nohup ... &`; the later camera HAL therefore timed out
waiting for frames and the provider died. This image keeps the bridge attached
to its init service until it exits. The bridge also applies per-frame
gray-world white balance to the RGB-IR converter, sampling complete CFA tiles,
excluding IR pixels, and limiting red/blue gains to 0.25–4.0. A synthetic
neutral-scene test checks average YUYV chroma. The supervised bridge stayed
running until shutdown in the later test boot, but preview color remains unverified.

The shared fake UVC descriptor advertises modes up to 1920×1080, but the
Latitude bridge produces fixed 640×480 YUYV frames. A Latitude-only descriptor
advertises only 640×480, but the latest boot still selected 1920×1080 YUYV.
The loopback driver independently enumerated a hard-coded list of frame sizes.
This image filters that list by each device's configured size bounds and clamps
the initial format to those bounds; the Latitude bridge sets both bounds to
640×480. Other loopback devices retain the
default bounds. The Surface camera descriptor is unchanged. Whether the HAL
selects 640×480 and whether this fixes green preview remain hardware-unverified.

The Goodix HAL now ignores the packet-header CRC8/complement but still verifies
the declared-frame CRC32, and no longer issues USB device resets on startup or
protocol errors. These changes address source divergences from libfprint and
the observed reset loop, but no raw packet header was retained and sensor
firmware/protocol compatibility is unconfirmed.
The latest boot's retained Goodix attempts 9–12 timed out with `-110` and the
service did not register. This image adds phase-specific diagnostics; it does
not establish a fingerprint fix.

Boot the image in **Live** mode only; do not choose install or update. The
front OV5675 is the only camera included; the rear camera is out of scope.
Auto-rotation was confirmed working by the user, and its configuration/assets
are unchanged; no rotation retest is needed.

If testing, collect `gbos/logs/boot-N/camera-bridge-setup.txt`,
`camera-bridge-runtime.txt`, `kmsg.log`, `stage0-dmesg.txt`, and all
`logcat.txt*` rotations. Check that the bridge remains alive while the USB
camera provider captures, CameraX stays available, and neutral/colored scenes
display plausibly. Do not enroll a fingerprint; check `gbos_fp` packet,
firmware, registration and USB-reset logs, lazy CrosFP startup,
duplicate-interface errors, SELinux AVCs, and whether Settings detects a
sensor.

The image is a local build artifact; no physical USB/NVMe media or UEFI was
changed. Camera preview color and frame content, clean IPU6 firmware shutdown,
Goodix registration/firmware behavior, and enforcing-mode USB access remain
unverified.
