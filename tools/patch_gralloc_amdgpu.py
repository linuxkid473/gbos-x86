#!/usr/bin/env python3
"""Let Google's vendor gralloc (libminigbm_gralloc.so, minigbm statically linked) allocate buffers on an AMD GPU.

  patch_gralloc_amdgpu.py IN OUT

THIS is the library that matters for graphics (gralloc.minigbm.so, mapper.minigbm.so and the allocator service all link it; the
separate /vendor/lib64/libgbm.so is only used by the camera and video-codec HALs, see patch_libgbm_amdgpu.py).

minigbm picks a backend by comparing the DRM driver name (drmGetVersion) with a chain of string literals (inlined drv_get_backend,
.text 0xc4c0..0xc710); each branch loads the matching `struct backend`. Google's build has no amdgpu backend, so gralloc fails on a
Ryzen laptop. Eight backends (komeda, radeon, nouveau, vkms, ...) are the *same* generic linear dumb-buffer one (identical init
0xef30 / bo_create_with_modifiers 0xf040: every render/texture format linear, drv_dumb_bo_create_ex). The "komeda" one (ARM Mali
display processor, never on x86) has a 7-byte string literal "komeda\\0": it is rewritten to "amdgpu\\0", so the comparison chain and
that backend's name both say "amdgpu". Dumb buffers are allowed on render nodes (DRM_RENDER_ALLOW in kernel 6.18's drm_ioctl.c), so
cros_gralloc's render-node-first search (renderD128..) works.

Second edit: cros_gralloc_driver::init sets a "software renderer" flag when the property ro.hardware.vulkan contains "lvp" or "pastel"
(drv_get_os_option + strstr, .text 0x17326..0x1737d; Google's own hook for SwiftShader/lavapipe, whose vulkan.pastel.so the image
does not ship). cros_gralloc_driver::lock (0x19140) refuses CPU access to a buffer that was not allocated with CPU usage unless that
flag is set, but a CPU Vulkan driver must lock every buffer ANGLE/Skia/SurfaceFlinger hand it (AHardwareBuffer_lockPlanes). Setting
ro.hardware.vulkan=pastel would also need a vulkan.pastel.so in /vendor/lib64/hw (read-only erofs: a tmpfs overlay of that directory),
so instead the literal "pastel\\0" is rewritten to "intel\\0\\0": the flag is set for ro.hardware.vulkan=intel, which hook.sh keeps.

Third edit (YV12 on RADV): drv_dumb_bo_create_ex sizes a DRM_FORMAT_YVU420_ANDROID dumb buffer from the width aligned to 32 and
lets amdgpu pick the pitch (256-byte multiples); drv_bo_from_format_and_padding then makes the chroma stride pitch/2. RADV can only import
a linear plane whose pitch is a multiple of 256 (GFX9+), so e.g. 1280 wide gives a 640 chroma pitch and the import fails (SurfaceFlinger
then aborts on "Failed to create a valid texture ... format:842094169"). The width is now aligned to 512 instead, so pitch/2 is a
multiple of 256. The 8 bytes at 0xe4a7 (add r12d,0x1f; and r12d,-32) become `call cave` + nop; the cave is 14 int3 bytes of padding
at 0xe422: add r12d,0x1ff; and r12w,0xfe00; ret.

Fourth edit (KMS scanout on Intel Gen <= 8 & universal displays): cros_gralloc_convert_format maps HAL_PIXEL_FORMAT_RGBA_8888 (1) to
DRM_FORMAT_ABGR8888 ('AB24' = 0x34324241) at 0x1a303. SurfaceFlinger allocates FramebufferSurface with format 1 (RGBA_8888).
On older Intel GPUs (Broadwell, Haswell, Ivy Bridge, Sandy Bridge), the primary display plane hardware does not support AB24 scanout
(only Gen 9+ Skylake introduced universal planes with AB24 support). When drm_hwcomposer imports the framebuffer via
drmModeAddFB2WithModifiers, the i915 driver rejects it with -EINVAL (-22 "unsupported pixel format AB24"), leaving the screen black
and freezing presentation. We rewrite the returned FourCC from 'AB24' (0x34324241) to 'XB24' (0x34324258 = DRM_FORMAT_XBGR8888, the
exact FourCC already returned for RGBX_8888 at 0x1a35d). Because DRM_FORMAT_XBGR8888 shares the identical 32bpp RGBA byte-order memory
layout, CPU/SwiftShader rendering is completely unaffected, while DRM/KMS scanout succeeds on all Intel display generations and AMD.

Only applied to the exact original (sha256 pinned): the offset is into one specific build.
"""
import hashlib, sys

ORIG_SHA = 'd48536951fd0e9643e71a5c8843e90833ce3e212e182782185470e2fbbfad673'
KOMEDA = 0x50b7   # .rodata literal compared at 0xc549 and pointed to by backend_komeda.name (file 0x1c238)
ALIGN_SITE = 0xe4a7   # drv_dumb_bo_create_ex, YVU420_ANDROID: add r12d,0x1f; and r12d,-32 (width in r12)
CAVE = 0xe422         # int3 padding before drv_dumb_bo_create_ex
PASTEL = 0x51af   # .rodata literal strstr()'d against ro.hardware.vulkan at 0x17364 (its only reference)
RGBA8888_SITE = 0x1a304  # cros_gralloc_convert_format: mov $0x34324241, %eax (b'AB24' -> b'XB24')


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())
    if hashlib.sha256(d).hexdigest() != ORIG_SHA:
        sys.exit('unexpected libminigbm_gralloc.so (sha256 differs from the build this patch was made for)')
    assert d[KOMEDA:KOMEDA + 7] == b'komeda\0' and d[KOMEDA - 1] == 0
    assert b'amdgpu' not in d
    d[KOMEDA:KOMEDA + 7] = b'amdgpu\0'
    assert d[PASTEL:PASTEL + 7] == b'pastel\0' and d[PASTEL - 1] == 0 and d.count(b'pastel') == 1
    d[PASTEL:PASTEL + 7] = b'intel\0\0'
    assert d[ALIGN_SITE:ALIGN_SITE + 8] == bytes.fromhex('4183c41f4183e4e0')
    assert d[CAVE:CAVE + 14] == b'\xcc' * 14
    rel = (CAVE - (ALIGN_SITE + 5)) & 0xffffffff
    d[ALIGN_SITE:ALIGN_SITE + 8] = b'\xe8' + rel.to_bytes(4, 'little') + b'\x0f\x1f\x00'
    d[CAVE:CAVE + 14] = bytes.fromhex('4181c4ff010000' '664181e400fe' 'c3')
    assert d[RGBA8888_SITE:RGBA8888_SITE + 4] == b'AB24'
    assert d[RGBA8888_SITE - 1] == 0xb8
    d[RGBA8888_SITE:RGBA8888_SITE + 4] = b'XB24'
    open(dst, 'wb').write(d)
    print('patched: generic linear backend answers to DRM driver "amdgpu"; software-renderer flag set by ro.hardware.vulkan=intel; '
          'YV12 dumb-buffer width aligned to 512; RGBA_8888 mapped to DRM_FORMAT_XBGR8888 (XB24) for universal KMS scanout')


if __name__ == '__main__':
    main()
