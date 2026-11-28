# Surface runtime files (Surface Laptop 1, Surface Pro 5, and Surface Laptop 7)

Copied to the stick as `gbos/surface/` (and by the installer to the internal disk).

| File | What |
|---|---|
| `iptsd-run.sh` | started by `gbos_iptsd`; matches the Surface Laptop 1 touchscreen (1B96:0979), Surface Pro 5 touchscreen, or Surface Laptop 7 touchpad and keeps `iptsd` running. Exits by itself on other laptops. |
| `iptsd.conf` | touchpad calibration for the alex-lentz iptsd fork + LiftGraceMs patch |
| `iptsd-surface-laptop-1.conf` | IPTS EDS v1 touchscreen profile for the first-generation Surface Laptop (1B96:0979), matching upstream's uninverted axes and 13.5-inch 3:2 geometry |
| `Vendor_1b96_Product_0979.idc` | created at post-fs-data by `hook.sh` to classify the IPTSD uinput device as an internal, orientation-aware touchscreen |
| `Vendor_045e_Product_0c9f.idc` | Android Input Device Configuration for the virtual multitouch uinput touchpad device |
| `surface-camera.sh` | started by the `gbos_camera` service in `hook.sh`; loads v4l2loopback, sets up fake UVC sysfs node, and starts `surface-webcam` |
| `fake_uvc_desc.bin` | UVC binary descriptors presented to Android USB camera HAL |
| `sleep-guard.sh` | started by the `gbos_sleep_guard` service in `hook.sh`; monitors lid state, prevents s2idle freezes, and manages power profiles |
| `iptsd`, `iptsd-check-device` | static binaries, **not committed**: built by `tools/build_surface_tools.sh` (alex-lentz/iptsd, a fork of linux-surface/iptsd with Surface Laptop 7 support, plus `tools/iptsd/0004-...patch`) |
| `surface-webcam` | static binary, **not committed**: built from `kernel/surface-webcam.c` by `tools/build_surface_tools.sh`. Bridges raw IPU7 CSI-2 sensor to v4l2loopback with dynamic scaling, center crop (4:3 / 16:9), and on-demand sensor power gating |

Credit: the iptsd fork is alex-lentz's; the calibration, the `LiftGraceMs` patch and touchpad research come from nierdal's write-up in linux-surface/linux-surface issue #1710.
