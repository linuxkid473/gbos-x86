# Latitude 7320 camera and rotation test

**Test image:** `build/camera-test-current-20261116/gbOS86-20261116.img`

**SHA-256:** `8524b4d4b726af58429e99662afbbd15fd11d3eeac760bc6741fb1194d307f18`

This Live image combines the opt-in front-camera bridge with the Latitude 7320
rotation configuration that was confirmed working on hardware. It sets
`CAMERA_BRIDGE=1`, `SENSOR_HAL=1`, `DEBUG_LOGS=1`, and `FINGERPRINT=0`; the
experimental Goodix service is disabled for this test.

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
   `camera-bridge-runtime.txt`, `kmsg.log`, `logcat.txt`, and
   `rotation-policy.txt` from the stick. Report Android camera enumeration,
   preview/frame results, and whether auto-rotation still works.

The image is a local build artifact and is not committed. Camera provider
discovery, preview, image quality, and clean firmware shutdown still require
hardware verification. The image does not install to NVMe or alter UEFI unless
the user explicitly selects an install/update path; avoid those menu choices.
