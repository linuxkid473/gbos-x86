#!/usr/bin/env python3
"""Force Google's desktop USB camera HAL to use its CPU libyuv pipeline on GPUs without R8/R8G8 STORAGE_IMAGE.

  patch_camera_common_cpu.py IN OUT

In Android 17 Desktop (`libdesktop_camera_common.dylib.so`), `vulkan::ImageProcessor::is_supported()` and
`vulkan::ComputeProcessor::is_supported()` (both at 0x0a8300) only check `driver_id != GoogleSwiftshader` (8).
On Intel Gen 7/7.5/8 (`vulkan.hasvk.so`, e.g. 4th Gen Haswell HD 4400) and non-Gen12 GPUs, `is_supported()`
therefore returns `true`, causing `CropRotate` and `StreamAdapter` (`libdesktop_camera_effect.dylib.so`) to:
  1. omit `CPU_READ_OFTEN | CPU_WRITE_OFTEN` (0x33) from camera stream usage (`0x100` instead of `0x133`), and
  2. run `ImageProcessor::nv12_to_nv12` via `DmaBufPlaneImage::new`, which requires `STORAGE_IMAGE` on
     `VK_FORMAT_R8_UNORM` / `VK_FORMAT_R8G8_UNORM` DRM-modifier planes — unsupported by Haswell Gen 7.5 hardware,
     failing every frame and leaving all camera output buffers black.

Returning `false` from `is_supported()` (and jumping to the existing `UnsupportedDevice` error return in
`ImageProcessor::new` and `ComputeProcessor::new_with_config`) activates Google's built-in CPU `libyuv` path
(same path used on SwiftShader), while camera apps and SurfaceFlinger continue compositing on hardware HasVK/RADV.

Only applied to the exact original (sha256 pinned in both this script and `usb/hook.sh`).
"""
import hashlib, struct, sys

ORIG_SHA = '1e3444bd4abd757e74e579a6687d86dd00d7bd770ccb1d272b68900e9b5b1d13'

# 1. ImageProcessor::is_supported / ComputeProcessor::is_supported @ 0x0a8300 -> xor %eax,%eax; ret
IS_SUPPORTED_OFF = 0x0a8300
IS_SUPPORTED_ORIG = bytes.fromhex('554889e54883ec20')
IS_SUPPORTED_NEW = bytes.fromhex('31c0c39090909090')

# 2. ImageProcessor::new @ 0x0acc09: cmpl $0x8, 0x93c(%rax); je 0xadcef -> jmp 0xadcef; nop
IMG_NEW_OFF = 0x0acc09
IMG_NEW_ORIG = bytes.fromhex('83b83c090000080f84d9100000')
IMG_NEW_PATCH = bytes.fromhex('83b83c09000008e9da10000090')

# 3. ComputeProcessor::new_with_config @ 0x0d6d34: cmpl $0x8, 0x93c(%rax); je 0xd8c39 -> jmp 0xd8c39; nop
CMP_NEW_OFF = 0x0d6d34
CMP_NEW_ORIG = bytes.fromhex('83b83c090000080f84f81e0000')
CMP_NEW_PATCH = bytes.fromhex('83b83c09000008e9f91e000090')


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: patch_camera_common_cpu.py IN OUT')
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())
    if hashlib.sha256(d).hexdigest() != ORIG_SHA:
        sys.exit('unexpected libdesktop_camera_common.dylib.so (sha256 differs from pinned build)')

    for off, orig, repl in (
        (IS_SUPPORTED_OFF, IS_SUPPORTED_ORIG, IS_SUPPORTED_NEW),
        (IMG_NEW_OFF, IMG_NEW_ORIG, IMG_NEW_PATCH),
        (CMP_NEW_OFF, CMP_NEW_ORIG, CMP_NEW_PATCH),
    ):
        assert len(orig) == len(repl)
        cur = bytes(d[off:off + len(orig)])
        assert cur == orig, f'mismatch at 0x{off:x}: got {cur.hex()}, expected {orig.hex()}'
        d[off:off + len(repl)] = repl

    open(dst, 'wb').write(d)
    out_sha = hashlib.sha256(d).hexdigest()
    print(f'patched libdesktop_camera_common.dylib.so -> CPU libyuv camera pipeline (sha256 {out_sha[:16]}...)')


if __name__ == '__main__':
    main()
