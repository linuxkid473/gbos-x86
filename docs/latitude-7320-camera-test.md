# Latitude 7320 camera bridge test

**Test image:** `build/camera-ipu6-loopback-test-20261115/gbOS86-20261115.img`

**SHA-256:** `50416319acc6f6a407d35bd081e2c4647a51bcd697285e5baf89ba393d0ad0ea`

The image is a local build artifact and is intentionally not committed to Git.

1. Write the image to a spare USB stick with Etcher or Rufus, then boot the Latitude 7320 Detachable from it using F12. Select **Live**; do not select install/update.
2. The image enables the experimental front-camera bridge and full diagnostics. It disables the unverified Goodix service to avoid its observed USB reset loop. The bridge exposes the camera as an extenal UVC camera; open the CameraX test app or another app that can select an extenal camera.
3. Expect the camera privacy indicator to remain on while the test runs. The bridge intentionally keeps the sensor streaming until reboot because the previous IPU6 shutdown timed out. Reboot after testing.
4. On the stick's GBOS partition, collect `gbos/logs/boot-N/camera-bridge-setup.txt`, `camera-bridge-runtime.txt`, `kmsg.log`, and `logcat.txt`. Report whether Android listed the extenal camera and whether preview produced frames. Do not infer success from the kenel media graph alone.

The image has not yet been tested on the laptop. It does not write to NVMe or alter UEFI. Camera provider discovery, preview/image quality, RGB-IR output quality, and clean firmware shutdown still need hardware verification.
