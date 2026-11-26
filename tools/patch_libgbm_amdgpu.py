#!/usr/bin/env python3
"""Let Google's vendor libgbm.so (minigbm) allocate buffers on an AMD GPU, as plain linear dumb buffers.

  patch_libgbm_amdgpu.py IN OUT

minigbm picks a backend by comparing the DRM driver name (drmGetVersion) with each backend's `name`. Google's build
has no amdgpu backend (i915/xe/radeon/nouveau/virtgpu/vkms/... only), so gralloc fails on a Ryzen laptop. The vkms
backend is minigbm's "generic linear dumb buffer" one (every render/texture/scanout format, DRM_IOCTL_MODE_CREATE_DUMB,
nothing driver specific), and libgbm already contains an unreferenced "amdgpu" string. So the vkms backend's name pointer
(.data.rel.ro, 0x2c740; RELR relocation, the file holds the link-time address) is pointed at that string: the vkms
backend now answers to "amdgpu" and, as a side effect, no longer to "vkms" (the virtual test driver, never used here).

Only applied to the exact original (sha256 pinned): it is the offset into one specific build.
"""
import hashlib, struct, sys

ORIG_SHA = '64a0f2537a5917822cb3f538dc74229e7ccf4ca45a30265bb97f742678ba294e'
NAME_PTR_OFF = 0x2c740   # backend_vkms.name
VKMS, AMDGPU = 0x66dd, 0x6b63   # addresses of the strings "vkms" and "amdgpu" in .rodata
CLFLUSHOPT_SITES = (0x14940, 0x19c20)   # i915_bo_flush, xe_bo_flush: 66 0f ae 39 -> 90 0f ae 39 (Haswell/Broadwell lack CLFLUSHOPT)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())
    if hashlib.sha256(d).hexdigest() != ORIG_SHA:
        sys.exit('unexpected libgbm.so (sha256 differs from the build this patch was made for)')
    assert d[VKMS:VKMS + 5] == b'vkms\0' and d[AMDGPU:AMDGPU + 7] == b'amdgpu\0' and d[AMDGPU - 1] == 0
    (cur,) = struct.unpack_from('<Q', d, NAME_PTR_OFF)
    assert cur == VKMS, hex(cur)
    struct.pack_into('<Q', d, NAME_PTR_OFF, AMDGPU)
    for off in CLFLUSHOPT_SITES:
        assert d[off:off + 4] == b'\x66\x0f\xae\x39', hex(off)
        d[off] = 0x90
    open(dst, 'wb').write(d)
    print('patched: backend "vkms" now matches DRM driver "amdgpu"; clflushopt -> clflush')


if __name__ == '__main__':
    main()
