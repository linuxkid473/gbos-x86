#!/usr/bin/env python3
"""Patch libdrm (for static libdrm_intel on Android) and intel-vaapi-driver (i965) for Android 17 Codec2 / cros-codecs.

Applied by tools/i965-vaapi-android/build.sh inside the gbos-swsr build workspace:
  1. libdrm (`drm/`): drop unused `libpciaccess` build dependency on Android (matching upstream `intel/Android.bp`;
     `intel_bufmgr_gem.c` queries device ID and capabilities via `DRM_IOCTL_I915_GETPARAM`).
  2. `intel_driver.c`:
     - Query chip revision via `DRM_I915_GETPARAM` (`I915_PARAM_CHIP_REVISION = 32`) instead of opening
       `/sys/devices/pci0000:00/0000:00:02.0/config` (blocked by SELinux / minijail inside `c2-cros-codecs-service`).
     - Accept render-node fds from Android's `vaGetDisplayDRM` regardless of `drm_state->auth_type` (`VA_DRM_AUTH_NONE`).
  3. `i965_drv_video.h` & `i965_drv_video.c`:
     - Route `i965_log_error` to Android logcat (`__android_log_vprint`).
     - Advertise and implement `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` (`VADRMPRIMESurfaceDescriptor`) and
       `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3` (`VADRMPRIME3SurfaceDescriptor`) in `i965_QuerySurfaceAttributes`,
       `i965_CreateSurfaces2` (both composed 1-layer and separate N-layer GBM/AHardwareBuffer layouts, plus
       `I915_FORMAT_MOD_Y_TILED` / `X_TILED` modifier propagation), and `i965_ExportSurfaceHandle`.
     - Ensure `i965_ExportSurfaceHandle` lazily allocates the backing Y-tiled `NV12` BO if called right after
       `vaCreateSurfaces` before `vaBeginPicture` (used by `cros-codecs`'s VA-API surface pool).
     - Relax `attrib_list[i].flags == VA_SURFACE_ATTRIB_SETTABLE` in `i965_CreateSurfaces2` so callers passing
       `flags = 0` or `GETTABLE | SETTABLE` are accepted, and honour `VASurfaceAttribDRMFormatModifiers`.
"""
import os, sys


def replace_exact(path, old, new):
    txt = open(path, 'r', encoding='utf-8', errors='surrogateescape').read()
    if new in txt:
        return
    if old not in txt:
        sys.exit('patch_i965: anchor not found in %s:\n%s' % (path, old[:240]))
    txt = txt.replace(old, new, 1)
    open(path, 'w', encoding='utf-8', errors='surrogateescape').write(txt)


def patch_drm(drm_dir):
    replace_exact(
        os.path.join(drm_dir, 'meson.build'),
        "dep_pciaccess = dependency('pciaccess', version : '>= 0.10', required : get_option('intel'))\n\n"
        "with_intel = get_option('intel') \\\n"
        "  .require(with_atomics, error_message : 'libdrm_intel requires atomics') \\\n"
        "  .require(dep_pciaccess.found(), error_message : 'libdrm_intel requires libpciaccess') \\",
        "dep_pciaccess = declare_dependency()\n\n"
        "with_intel = get_option('intel') \\\n"
        "  .require(with_atomics, error_message : 'libdrm_intel requires atomics') \\",
    )
    replace_exact(
        os.path.join(drm_dir, 'intel', 'intel_bufmgr.c'),
        '#include <pciaccess.h>\n',
        '/* pciaccess not used on Android */\n',
    )


def patch_i965(i965_dir):
    # 1. src/meson.build: link -llog and set SONAME to iHD_drv_video.so so hook.sh can bind directly over /vendor/lib64/iHD_drv_video.so
    replace_exact(
        os.path.join(i965_dir, 'src', 'meson.build'),
        "i965_drv_video = shared_module(\n"
        "  'i965_drv_video',\n"
        "  name_prefix : '',\n"
        "  install : true,\n"
        "  install_dir : driverdir,\n"
        "  link_whole : libi965_drv_video,\n"
        "  dependencies : shared_deps)",
        "log_dep = cc.find_library('log')\n"
        "i965_drv_video = shared_module(\n"
        "  'i965_drv_video',\n"
        "  name_prefix : '',\n"
        "  install : true,\n"
        "  install_dir : driverdir,\n"
        "  link_args : ['-Wl,-soname,iHD_drv_video.so', '-Wl,--no-undefined'],\n"
        "  link_whole : libi965_drv_video,\n"
        "  dependencies : shared_deps + [log_dep])",
    )

    # 2. src/intel_driver.c: ioctl-based chip revision (no /sys open) + render-node auth_type support
    replace_exact(
        os.path.join(i965_dir, 'src', 'intel_driver.c'),
        'static void intel_driver_get_revid(struct intel_driver_data *intel, int *value)\n'
        '{\n'
        '#define PCI_REVID   8\n'
        '    FILE *fp;\n'
        '    char config_data[16];\n'
        '\n'
        '    fp = fopen("/sys/devices/pci0000:00/0000:00:02.0/config", "r");\n'
        '\n'
        '    if (fp) {\n'
        '        if (fread(config_data, 1, 16, fp))\n'
        '            *value = config_data[PCI_REVID];\n'
        '        else\n'
        '            *value = 2; /* assume it is at least  B-steping */\n'
        '        fclose(fp);\n'
        '    } else {\n'
        '        *value = 2; /* assume it is at least  B-steping */\n'
        '    }\n'
        '\n'
        '    return;\n'
        '}',
        'static void intel_driver_get_revid(struct intel_driver_data *intel, int *value)\n'
        '{\n'
        '    int rev = 2;\n'
        '    if (intel_driver_get_param(intel, 32 /* I915_PARAM_CHIP_REVISION */, &rev) && rev >= 0)\n'
        '        *value = rev;\n'
        '    else\n'
        '        *value = 2;\n'
        '}',
    )
    replace_exact(
        os.path.join(i965_dir, 'src', 'intel_driver.c'),
        '    ASSERT_RET(drm_state, false);\n'
        '    ASSERT_RET((VA_CHECK_DRM_AUTH_TYPE(ctx, VA_DRM_AUTH_DRI1) ||\n'
        '                VA_CHECK_DRM_AUTH_TYPE(ctx, VA_DRM_AUTH_DRI2) ||\n'
        '                VA_CHECK_DRM_AUTH_TYPE(ctx, VA_DRM_AUTH_CUSTOM)),\n'
        '               false);\n'
        '\n'
        '    intel->fd = drm_state->fd;\n'
        '    intel->dri2Enabled = (VA_CHECK_DRM_AUTH_TYPE(ctx, VA_DRM_AUTH_DRI2) ||\n'
        '                          VA_CHECK_DRM_AUTH_TYPE(ctx, VA_DRM_AUTH_CUSTOM));\n'
        '\n'
        '    if (!intel->dri2Enabled) {\n'
        '        return false;\n'
        '    }',
        '    ASSERT_RET(drm_state && drm_state->fd >= 0, false);\n'
        '    intel->fd = drm_state->fd;\n'
        '    intel->dri2Enabled = 1;',
    )

    # 3. src/i965_drv_video.h: add DRM_PRIME_2 and DRM_PRIME_3 surface memory types
    replace_exact(
        os.path.join(i965_dir, 'src', 'i965_drv_video.h'),
        '#define I965_SURFACE_MEM_NATIVE             0\n'
        '#define I965_SURFACE_MEM_GEM_FLINK          1\n'
        '#define I965_SURFACE_MEM_DRM_PRIME          2\n',
        '#define I965_SURFACE_MEM_NATIVE             0\n'
        '#define I965_SURFACE_MEM_GEM_FLINK          1\n'
        '#define I965_SURFACE_MEM_DRM_PRIME          2\n'
        '#define I965_SURFACE_MEM_DRM_PRIME_2        3\n'
        '#define I965_SURFACE_MEM_DRM_PRIME_3        4\n',
    )

    # 4. src/i965_drv_video.c: Android logcat errors + DRM_PRIME_2 / DRM_PRIME_3 import, query & export
    drv_c = os.path.join(i965_dir, 'src', 'i965_drv_video.c')
    replace_exact(
        drv_c,
        '#include "sysdeps.h"\n',
        '#include "sysdeps.h"\n'
        '#ifdef __ANDROID__\n'
        '#include <android/log.h>\n'
        '#endif\n',
    )

    # Check i965_log_error to mirror to logcat on Android
    replace_exact(
        drv_c,
        'void i965_log_error(VADriverContextP ctx, const char *format, ...)\n'
        '{\n'
        '    va_list vl;\n'
        '\n'
        '    va_start(vl, format);\n',
        'void i965_log_error(VADriverContextP ctx, const char *format, ...)\n'
        '{\n'
        '    va_list vl;\n'
        '#ifdef __ANDROID__\n'
        '    va_start(vl, format);\n'
        '    __android_log_vprint(ANDROID_LOG_ERROR, "i965_vaapi", format, vl);\n'
        '    va_end(vl);\n'
        '#endif\n'
        '    va_start(vl, format);\n',
    )

    # Add i965_surface_drm_prime2_memory right after i965_suface_external_memory
    prime2_fn = '''
static uint32_t
i965_normalize_prime_fourcc(uint32_t fourcc, const VADRMPRIMESurfaceDescriptor *d)
{
    if (fourcc == 0x37393939U /* DRM_FORMAT_YVU420_ANDROID */)
        return VA_FOURCC_YV12;
    if (fourcc != 0)
        return fourcc;
    if (d->num_layers == 1) {
        if (d->layers[0].drm_format == 0x37393939U)
            return VA_FOURCC_YV12;
        return d->layers[0].drm_format;
    }
    if (d->num_layers == 2)
        return VA_FOURCC_NV12;
    if (d->num_layers == 3)
        return VA_FOURCC_YV12;
    return 0;
}

static VAStatus
i965_surface_drm_prime2_memory(VADriverContextP ctx,
                               struct object_surface *obj_surface,
                               int external_memory_type,
                               void *descriptor_array,
                               int index,
                               int expected_fourcc)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    const VADRMPRIMESurfaceDescriptor *d;
    VASurfaceAttribExternalBuffers ext;
    uintptr_t prime_fd_val;
    unsigned int tiling = I915_TILING_NONE, swizzle = I915_BIT_6_SWIZZLE_NONE;
    uint32_t total_planes = 0, l, p;
    uint32_t max_end = 0;

    if (!descriptor_array)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (external_memory_type == I965_SURFACE_MEM_DRM_PRIME_3)
        d = (const VADRMPRIMESurfaceDescriptor *)((const VADRMPRIME3SurfaceDescriptor *)descriptor_array + index);
    else
        d = (const VADRMPRIMESurfaceDescriptor *)descriptor_array + index;

    if (d->num_objects < 1 || d->num_objects > 4 || d->objects[0].fd < 0 || d->num_layers < 1 || d->num_layers > 4)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    memset(&ext, 0, sizeof(ext));
    ext.pixel_format = i965_normalize_prime_fourcc(d->fourcc ? d->fourcc : (uint32_t)expected_fourcc, d);
    ext.width = d->width ? d->width : obj_surface->orig_width;
    ext.height = d->height ? d->height : obj_surface->orig_height;

    for (l = 0; l < d->num_layers; l++) {
        if (d->layers[l].num_planes < 1 || total_planes + d->layers[l].num_planes > 4)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (p = 0; p < d->layers[l].num_planes; p++) {
            uint32_t oidx = d->layers[l].object_index[p];
            if (oidx >= d->num_objects)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            ext.pitches[total_planes] = d->layers[l].pitch[p];
            ext.offsets[total_planes] = d->layers[l].offset[p];
            if (d->objects[oidx].size > max_end)
                max_end = d->objects[oidx].size;
            total_planes++;
        }
    }
    ext.num_planes = total_planes;

    if (max_end == 0 && ext.num_planes >= 1) {
        uint32_t last = ext.num_planes - 1;
        uint32_t last_h = (last == 0) ? ext.height : (ext.height + 1) / 2;
        max_end = ext.offsets[last] + ext.pitches[last] * ALIGN(last_h, 32);
    }
    ext.data_size = max_end;
    prime_fd_val = (uintptr_t)d->objects[0].fd;
    ext.buffers = &prime_fd_val;
    ext.num_buffers = 1;

    /* Ensure GEM BO tiling reflects explicit DRM format modifier when set by minigbm/gbm */
    obj_surface->bo = drm_intel_bo_gem_create_from_prime(i965->intel.bufmgr, d->objects[0].fd, ext.data_size);
    if (!obj_surface->bo)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* Verify multi-fd descriptors all alias the same underlying GEM handle (i965 uses single-BO planar surfaces) */
    for (l = 1; l < d->num_objects; l++) {
        if (d->objects[l].fd >= 0 && d->objects[l].fd != d->objects[0].fd) {
            dri_bo *other = drm_intel_bo_gem_create_from_prime(i965->intel.bufmgr, d->objects[l].fd, d->objects[l].size);
            bool same = (other == obj_surface->bo);
            if (other)
                dri_bo_unreference(other);
            if (!same) {
                dri_bo_unreference(obj_surface->bo);
                obj_surface->bo = NULL;
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
    }

    dri_bo_get_tiling(obj_surface->bo, &tiling, &swizzle);
    if (tiling == I915_TILING_NONE) {
        if (d->objects[0].drm_format_modifier == I915_FORMAT_MOD_Y_TILED) {
            uint32_t want = I915_TILING_Y;
            drm_intel_bo_set_tiling(obj_surface->bo, &want, ext.pitches[0]);
        } else if (d->objects[0].drm_format_modifier == I915_FORMAT_MOD_X_TILED) {
            uint32_t want = I915_TILING_X;
            drm_intel_bo_set_tiling(obj_surface->bo, &want, ext.pitches[0]);
        }
    }
    dri_bo_unreference(obj_surface->bo);
    obj_surface->bo = NULL;

    return i965_suface_external_memory(ctx, obj_surface, I965_SURFACE_MEM_DRM_PRIME, &ext, 0);
}
'''

    replace_exact(
        drv_c,
        'static VAStatus\n'
        'i965_CreateSurfaces2(',
        prime2_fn + '\nstatic VAStatus\n'
        'i965_CreateSurfaces2(',
    )

    # Update attribute parsing & switch in i965_CreateSurfaces2
    replace_exact(
        drv_c,
        '    for (i = 0; i < num_attribs && attrib_list; i++) {\n'
        '        if ((attrib_list[i].type == VASurfaceAttribPixelFormat) &&\n'
        '            (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE)) {\n'
        '            ASSERT_RET(attrib_list[i].value.type == VAGenericValueTypeInteger, VA_STATUS_ERROR_INVALID_PARAMETER);\n'
        '            expected_fourcc = attrib_list[i].value.value.i;\n'
        '        }\n'
        '\n'
        '        if ((attrib_list[i].type == VASurfaceAttribMemoryType) &&\n'
        '            (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE)) {\n'
        '\n'
        '            ASSERT_RET(attrib_list[i].value.type == VAGenericValueTypeInteger, VA_STATUS_ERROR_INVALID_PARAMETER);\n'
        '\n'
        '            if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM)\n'
        '                memory_type = I965_SURFACE_MEM_GEM_FLINK; /* flinked GEM handle */\n'
        '            else if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME)\n'
        '                memory_type = I965_SURFACE_MEM_DRM_PRIME; /* drm prime fd */\n'
        '            else if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_VA)\n'
        '                memory_type = I965_SURFACE_MEM_NATIVE; /* va native memory, to be allocated */\n'
        '        }\n'
        '\n'
        '        if ((attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) &&\n'
        '            (attrib_list[i].flags == VA_SURFACE_ATTRIB_SETTABLE)) {\n'
        '            ASSERT_RET(attrib_list[i].value.type == VAGenericValueTypePointer, VA_STATUS_ERROR_INVALID_PARAMETER);\n'
        '            memory_attibute = (VASurfaceAttribExternalBuffers *)attrib_list[i].value.value.p;\n'
        '        }\n'
        '    }',
        '    bool modifier_force_linear = false;\n'
        '    for (i = 0; i < num_attribs && attrib_list; i++) {\n'
        '        if ((attrib_list[i].type == VASurfaceAttribPixelFormat) &&\n'
        '            (!attrib_list[i].flags || (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE))) {\n'
        '            ASSERT_RET(attrib_list[i].value.type == VAGenericValueTypeInteger, VA_STATUS_ERROR_INVALID_PARAMETER);\n'
        '            expected_fourcc = attrib_list[i].value.value.i;\n'
        '            if (expected_fourcc == 0x37393939) expected_fourcc = VA_FOURCC_YV12;\n'
        '        }\n'
        '\n'
        '        if ((attrib_list[i].type == VASurfaceAttribMemoryType) &&\n'
        '            (!attrib_list[i].flags || (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE))) {\n'
        '\n'
        '            ASSERT_RET(attrib_list[i].value.type == VAGenericValueTypeInteger, VA_STATUS_ERROR_INVALID_PARAMETER);\n'
        '\n'
        '            if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM)\n'
        '                memory_type = I965_SURFACE_MEM_GEM_FLINK;\n'
        '            else if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME)\n'
        '                memory_type = I965_SURFACE_MEM_DRM_PRIME;\n'
        '            else if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)\n'
        '                memory_type = I965_SURFACE_MEM_DRM_PRIME_2;\n'
        '            else if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3)\n'
        '                memory_type = I965_SURFACE_MEM_DRM_PRIME_3;\n'
        '            else if (attrib_list[i].value.value.i == VA_SURFACE_ATTRIB_MEM_TYPE_VA)\n'
        '                memory_type = I965_SURFACE_MEM_NATIVE;\n'
        '            else\n'
        '                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;\n'
        '        }\n'
        '\n'
        '        if ((attrib_list[i].type == VASurfaceAttribExternalBufferDescriptor) &&\n'
        '            (!attrib_list[i].flags || (attrib_list[i].flags & VA_SURFACE_ATTRIB_SETTABLE))) {\n'
        '            ASSERT_RET(attrib_list[i].value.type == VAGenericValueTypePointer, VA_STATUS_ERROR_INVALID_PARAMETER);\n'
        '            memory_attibute = (VASurfaceAttribExternalBuffers *)attrib_list[i].value.value.p;\n'
        '        }\n'
        '\n'
        '        if (attrib_list[i].type == VASurfaceAttribDRMFormatModifiers &&\n'
        '            attrib_list[i].value.type == VAGenericValueTypePointer &&\n'
        '            attrib_list[i].value.value.p) {\n'
        '            const VADRMFormatModifierList *ml = (const VADRMFormatModifierList *)attrib_list[i].value.value.p;\n'
        '            bool has_y = false, has_lin = false;\n'
        '            for (uint32_t mi = 0; mi < ml->num_modifiers && ml->modifiers; mi++) {\n'
        '                if (ml->modifiers[mi] == I915_FORMAT_MOD_Y_TILED) has_y = true;\n'
        '                if (ml->modifiers[mi] == DRM_FORMAT_MOD_NONE) has_lin = true;\n'
        '            }\n'
        '            if (!has_y && has_lin)\n'
        '                modifier_force_linear = true;\n'
        '        }\n'
        '    }',
    )

    replace_exact(
        drv_c,
        '        switch (memory_type) {\n'
        '        case I965_SURFACE_MEM_NATIVE:\n'
        '            if (memory_attibute) {',
        '        if (modifier_force_linear)\n'
        '            obj_surface->user_disable_tiling = true;\n'
        '        switch (memory_type) {\n'
        '        case I965_SURFACE_MEM_NATIVE:\n'
        '            if (memory_attibute) {',
    )

    replace_exact(
        drv_c,
        '        case I965_SURFACE_MEM_GEM_FLINK:\n'
        '        case I965_SURFACE_MEM_DRM_PRIME:\n'
        '            vaStatus = i965_suface_external_memory(ctx,\n'
        '                                                   obj_surface,\n'
        '                                                   memory_type,\n'
        '                                                   memory_attibute,\n'
        '                                                   i);\n'
        '            break;\n'
        '        }',
        '        case I965_SURFACE_MEM_GEM_FLINK:\n'
        '        case I965_SURFACE_MEM_DRM_PRIME:\n'
        '            vaStatus = i965_suface_external_memory(ctx,\n'
        '                                                   obj_surface,\n'
        '                                                   memory_type,\n'
        '                                                   memory_attibute,\n'
        '                                                   i);\n'
        '            break;\n'
        '        case I965_SURFACE_MEM_DRM_PRIME_2:\n'
        '        case I965_SURFACE_MEM_DRM_PRIME_3:\n'
        '            vaStatus = i965_surface_drm_prime2_memory(ctx,\n'
        '                                                      obj_surface,\n'
        '                                                      memory_type,\n'
        '                                                      memory_attibute,\n'
        '                                                      i,\n'
        '                                                      expected_fourcc);\n'
        '            break;\n'
        '        }',
    )

    # Advertise DRM_PRIME_2 and DRM_PRIME_3 in i965_QuerySurfaceAttributes
    replace_exact(
        drv_c,
        '    attribs[i].type = VASurfaceAttribMemoryType;\n'
        '    attribs[i].value.type = VAGenericValueTypeInteger;\n'
        '    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;\n'
        '    attribs[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |\n'
        '                               VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM |\n'
        '                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;\n',
        '    attribs[i].type = VASurfaceAttribMemoryType;\n'
        '    attribs[i].value.type = VAGenericValueTypeInteger;\n'
        '    attribs[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;\n'
        '    attribs[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |\n'
        '                               VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM |\n'
        '                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |\n'
        '                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 |\n'
        '                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3;\n',
    )

    # Lazy BO allocation + DRM_PRIME_3 support in i965_ExportSurfaceHandle
    replace_exact(
        drv_c,
        '    if (!obj_surface || !obj_surface->bo)\n'
        '        return VA_STATUS_ERROR_INVALID_SURFACE;\n'
        '\n'
        '    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {\n'
        '        i965_log_info(ctx, "vaExportSurfaceHandle: memory type %08x "\n'
        '                      "is not supported.\\n", mem_type);\n'
        '        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;\n'
        '    }',
        '    if (!obj_surface)\n'
        '        return VA_STATUS_ERROR_INVALID_SURFACE;\n'
        '\n'
        '    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 &&\n'
        '        mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3) {\n'
        '        i965_log_info(ctx, "vaExportSurfaceHandle: memory type %08x "\n'
        '                      "is not supported.\\n", mem_type);\n'
        '        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;\n'
        '    }\n'
        '\n'
        '    if (!obj_surface->bo) {\n'
        '        int def_fourcc = obj_surface->fourcc ? obj_surface->fourcc :\n'
        '                         ((obj_surface->expected_format == VA_RT_FORMAT_YUV420_10BPP) ? VA_FOURCC_P010 : VA_FOURCC_NV12);\n'
        '        int use_tiling = HAS_TILED_SURFACE(i965) && !obj_surface->user_disable_tiling;\n'
        '        if (def_fourcc == VA_FOURCC_YV12 || def_fourcc == VA_FOURCC_I420 || def_fourcc == VA_FOURCC_IYUV)\n'
        '            use_tiling = 0;\n'
        '        i965_check_alloc_surface_bo(ctx, obj_surface, use_tiling, def_fourcc, get_sampling_from_fourcc(def_fourcc));\n'
        '        if (!obj_surface->bo)\n'
        '            return VA_STATUS_ERROR_INVALID_SURFACE;\n'
        '    }\n'
        '\n'
        '    if (mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3)\n'
        '        memset(descriptor, 0, sizeof(VADRMPRIME3SurfaceDescriptor));',
    )


def main():
    drm_dir, i965_dir = sys.argv[1], sys.argv[2]
    patch_drm(drm_dir)
    patch_i965(i965_dir)
    print('patch_i965: patched %s and %s' % (drm_dir, i965_dir))


if __name__ == '__main__':
    main()
