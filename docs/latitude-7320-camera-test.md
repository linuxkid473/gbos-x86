# Latitude 7320 camera and rotation test

**Test image:** `build/camera-test-video64-goodix-diagnostic-20261116/gbOS86-20261116.img`

**SHA-256:** `4703c118ba2b639984beb99f89c47dfece3fd630f46fb5e2e4c0efffb2c5828d`

This Live image combines the opt-in front-camera bridge with the Latitude 7320
rotation configuration that was confirmed working on hardware. It sets
`CAMERA_BRIDGE=1`, `SENSOR_HAL=1`, `DEBUG_LOGS=1`, and `FINGERPRINT=1`. The
rebuilt experimental Goodix HAL is enabled only for a no-enrollment startup
diagnostic on USB `27c6:6384`; this does not establish hardware support.

The bridge uses the space-free v4l2loopback card label
`GBOS_IPU6_Camera`. The prior boot showed the module loading but its option
parser rejected the words `IPU6` and `Camera` from the old spaced label, so the
bridge failed validation before installing its fake UVC identity. This corrects
that configuration; camera node creation and Android discovery still need a
device test.

1. Write the image to a spare USB stick with Etcher or Rufus, then boot the
   Latitude 7320 Detachable using F12. Choose **Live**. Do not choose install
   or update.
2. Check that Android auto-rotation still works. Then open a camera app and
   check whether it lists the bridged external camera and displays preview.
   The bridge selects the front OV5675 only; the rear camera is not included.
3. The bridge converts the sensor's RGB-IR raw stream to 640x480 YUYV and keeps
   the sensor streaming until reboot. Previous captures delivered three raw
   buffers, but IPU6 reported stream-stop and close timeouts. Reboot after the
   camera test; do not treat a successful `STREAMOFF` call as proof of clean
   firmware shutdown.
4. Collect `gbos/logs/boot-N/camera-bridge-setup.txt`,
   `camera-bridge-runtime.txt`, `kmsg.log`, `stage0-dmesg.txt`, all
   `logcat.txt*` rotations, and `rotation-policy.txt` from the stick. Do not
   enroll a fingerprint. In the logs, check whether `gbos_fp` starts, reaches
   firmware-ready, and registers its AIDL service; also capture any CrosFP
   lazy start, duplicate-registration error, or SELinux AVC. Report Android
   camera enumeration/preview/frame results and whether auto-rotation still
   works.

The image is a local build artifact and is not committed. Camera provider
discovery, preview, image quality, clean IPU6 firmware shutdown, Goodix service
registration/firmware interaction, and enforcing-mode USB access still require
hardware verification. The image does not install to NVMe or alter UEFI unless
the user explicitly selects an install/update path; choose **Live** and avoid
install/update.
