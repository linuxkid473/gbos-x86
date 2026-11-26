#!/usr/bin/env python3
"""Run from the Mesa source root; idempotent.

Fixes Android 17 swapchain / AHardwareBuffer / multi-planar YUV video / gralloc
support in Mesa's frozen Gen 7/7.5/8 `intel_hasvk` driver (`src/intel/vulkan_hasvk/`):

1. Deferred Android swapchain binding (`anv_image.c` + `anv_private.h` + `anv_android.c`):
   Android 14+ `libvulkan.so` creates swapchain images with `VkImageSwapchainCreateInfoKHR`
   and no `VkNativeBufferANDROID`, passing `VkNativeBufferANDROID` later at
   `vkBindImageMemory2`. Defer surface creation until `vkBindImageMemory2` and forward
   `gralloc_info->stride * bpp` into `isl_surf_init` (`.row_pitch_B`).
2. Multi-planar YUV `AHardwareBuffer` import & `CrOS Gralloc` (`vk_android.c`, `anv_android.c`, `anv_image.c`):
   - Android `MediaCodec` software decoders (`swcodec`) output `HAL_PIXEL_FORMAT_YCbCr_420_888`
     (`0x23`), which ChromeOS `minigbm` (`i915`) backs with 3-plane `DRM_FORMAT_YVU420` /
     `DRM_FORMAT_YVU420_ANDROID` (`YV12`, Y-Cr-Cb) when CPU-written, or 2-plane `DRM_FORMAT_NV12`.
   - Route `vkGetAndroidHardwareBufferPropertiesANDROID` to Mesa's common implementation
     (`vk_common_GetAndroidHardwareBufferPropertiesANDROID`), which queries `minigbm` via
     `u_gralloc_cros_api` for the true `drm_fourcc` and BT.709/BT.601 YUV dataspace/range.
   - Port modern `anv`'s `resolve_ahb_image` (`vk_android_get_ahb_layout` +
     `add_all_surfaces_explicit_layout`), out-of-order plane offset support in
     `image_binding_grow` (needed when U/V planes are swapped for `YVU420`), `choose_isl_surf_usage`
     in `add_all_surfaces_explicit_layout`, and YCbCr per-plane stride / YV12 plane swap in
     `add_all_surfaces_implicit_layout`. Without this, `hasvk` misread 3-plane `YVU420` video
     buffers as 2-plane `NV12` with full-width chroma pitch, turning all Android app video green.
3. Implicit i915 KMS DMA-BUF fencing (`anv_android.c`):
   Adds `ANV_BO_ALLOC_EXTERNAL | ANV_BO_ALLOC_IMPLICIT_SYNC | ANV_BO_ALLOC_IMPLICIT_WRITE`
   when importing `AHardwareBuffer` / gralloc dma-bufs.
"""
import sys

P_AND = 'src/intel/vulkan_hasvk/anv_android.c'
P_IMG = 'src/intel/vulkan_hasvk/anv_image.c'
P_PRV = 'src/intel/vulkan_hasvk/anv_private.h'
P_VKA = 'src/vulkan/runtime/vk_android.c'

VKA_SWAP_OLD = '''   if (info.drm_fourcc == DRM_FORMAT_YVU420) {
      /* Swap the U and V planes to match the
       * VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM */
      VkSubresourceLayout tmp = out_layouts[1];
      out_layouts[1] = out_layouts[2];
      out_layouts[2] = tmp;
   }'''

VKA_SWAP_NEW = '''   if (info.drm_fourcc == DRM_FORMAT_YVU420 ||
       info.drm_fourcc == 0x37393939 /* DRM_FORMAT_YVU420_ANDROID */) {
      /* Swap the U and V planes to match the
       * VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM */
      VkSubresourceLayout tmp = out_layouts[1];
      out_layouts[1] = out_layouts[2];
      out_layouts[2] = tmp;
   }'''

VKA_FMT_OLD = '''   switch (info.drm_fourcc) {
   case DRM_FORMAT_YVU420:
      /* Assuming that U and V planes are swapped earlier */
      external_format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
      break;
   case DRM_FORMAT_NV12:
      external_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      break;
   case DRM_FORMAT_P010:
      external_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
      break;
   case DRM_FORMAT_P210:
      external_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
      break;
   case DRM_FORMAT_XBGR8888:
      /* This can be resolved from IMPLEMENTATION_DEFINED AHB format */
      external_format = VK_FORMAT_R8G8B8A8_UNORM;
      break;
   default:
      mesa_loge("Unsupported external DRM format: %d", info.drm_fourcc);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }'''

VKA_FMT_NEW = '''   switch (info.drm_fourcc) {
   case DRM_FORMAT_YVU420:
   case 0x37393939: /* DRM_FORMAT_YVU420_ANDROID ('9997') */
   case DRM_FORMAT_YUV420:
      /* U and V planes are swapped earlier for YVU420 */
      external_format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
      break;
   case DRM_FORMAT_NV12:
      external_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      break;
   case DRM_FORMAT_P010:
      external_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
      break;
   case DRM_FORMAT_P210:
      external_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
      break;
   case DRM_FORMAT_ABGR8888:
   case DRM_FORMAT_XBGR8888:
      /* This can be resolved from IMPLEMENTATION_DEFINED AHB format */
      external_format = VK_FORMAT_R8G8B8A8_UNORM;
      break;
   case DRM_FORMAT_ARGB8888:
   case DRM_FORMAT_XRGB8888:
      external_format = VK_FORMAT_B8G8R8A8_UNORM;
      break;
   case DRM_FORMAT_RGB565:
      external_format = VK_FORMAT_R5G6B5_UNORM_PACK16;
      break;
   default:
      mesa_loge("Unsupported external DRM format: %d", info.drm_fourcc);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }'''

FMT_OLD = '''inline VkFormat
vk_format_from_android(unsigned android_format, unsigned android_usage)
{
   switch (android_format) {
   case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
      return VK_FORMAT_R8G8B8_UNORM;
   case AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
      return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   case AHARDWAREBUFFER_FORMAT_IMPLEMENTATION_DEFINED:
      if (android_usage & AHARDWAREBUFFER_USAGE_CAMERA_MASK)
         return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      else
         return VK_FORMAT_R8G8B8_UNORM;
   default:
      return vk_ahb_format_to_image_format(android_format);
   }
}

unsigned
anv_ahb_format_for_vk_format(VkFormat vk_format)
{
   switch (vk_format) {
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
      return AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
   default:
      return vk_image_format_to_ahb_format(vk_format);
   }
}'''

FMT_NEW = '''inline VkFormat
vk_format_from_android(unsigned android_format, unsigned android_usage)
{
   switch (android_format) {
   case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
      return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   case 0x32315659: /* HAL_PIXEL_FORMAT_YV12 */
      return VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
   case AHARDWAREBUFFER_FORMAT_IMPLEMENTATION_DEFINED:
      if (android_usage & AHARDWAREBUFFER_USAGE_CAMERA_MASK)
         return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      else
         return VK_FORMAT_R8G8B8A8_UNORM;
   default:
      return vk_ahb_format_to_image_format(android_format);
   }
}

unsigned
anv_ahb_format_for_vk_format(VkFormat vk_format)
{
   switch (vk_format) {
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
      return AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
   case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
      return 0x32315659; /* HAL_PIXEL_FORMAT_YV12 */
   default:
      return vk_image_format_to_ahb_format(vk_format);
   }
}'''

AHB_PROPS_OLD = '''VkResult
anv_GetAndroidHardwareBufferPropertiesANDROID('''

AHB_PROPS_NEW = '''static VkResult __attribute__((unused))
gbos_unused_anv_GetAndroidHardwareBufferPropertiesANDROID('''

IMP_OLD = '''   VkResult result = anv_device_import_bo(device, dma_buf, 0,
                                          0 /* client_address */,
                                          &mem->bo);'''

IMP_NEW = '''   VkResult result = anv_device_import_bo(device, dma_buf,
                                          ANV_BO_ALLOC_EXTERNAL |
                                          ANV_BO_ALLOC_IMPLICIT_SYNC |
                                          ANV_BO_ALLOC_IMPLICIT_WRITE,
                                          0 /* client_address */,
                                          &mem->bo);'''

GR_INIT_OLD = '''   result = anv_device_import_bo(device, dma_buf,
                                 ANV_BO_ALLOC_IMPLICIT_SYNC |
                                 ANV_BO_ALLOC_IMPLICIT_WRITE,
                                 0 /* client_address */,
                                 &bo);
   if (result != VK_SUCCESS) {
      return vk_errorf(device, result,
                       "failed to import dma-buf from VkNativeBufferANDROID");
   }

   enum isl_tiling tiling;
   result = anv_device_get_bo_tiling(device, bo, &tiling);
   if (result != VK_SUCCESS) {
      return vk_errorf(device, result,
                       "failed to get tiling from VkNativeBufferANDROID");
   }
   anv_info.isl_tiling_flags = 1u << tiling;

   enum isl_format format = anv_get_isl_format(device->info,
                                               base_info->format,
                                               VK_IMAGE_ASPECT_COLOR_BIT,
                                               base_info->tiling);
   assert(format != ISL_FORMAT_UNSUPPORTED);

   result = anv_image_init(device, image, &anv_info);'''

GR_INIT_NEW = '''   result = anv_device_import_bo(device, dma_buf,
                                 ANV_BO_ALLOC_EXTERNAL |
                                 ANV_BO_ALLOC_IMPLICIT_SYNC |
                                 ANV_BO_ALLOC_IMPLICIT_WRITE,
                                 0 /* client_address */,
                                 &bo);
   if (result != VK_SUCCESS) {
      return vk_errorf(device, result,
                       "failed to import dma-buf from VkNativeBufferANDROID");
   }

   enum isl_tiling tiling;
   result = anv_device_get_bo_tiling(device, bo, &tiling);
   if (result != VK_SUCCESS) {
      anv_device_release_bo(device, bo);
      return vk_errorf(device, result,
                       "failed to get tiling from VkNativeBufferANDROID");
   }
   anv_info.isl_tiling_flags = 1u << tiling;

   VkImageTiling vk_tiling =
      tiling == ISL_TILING_LINEAR ? VK_IMAGE_TILING_LINEAR :
                                    VK_IMAGE_TILING_OPTIMAL;
   enum isl_format format = anv_get_isl_format(device->info,
                                               base_info->format,
                                               VK_IMAGE_ASPECT_COLOR_BIT,
                                               vk_tiling);
   if (isl_format_is_rgb(format))
      format = isl_format_rgb_to_rgba(format);
   assert(format != ISL_FORMAT_UNSUPPORTED);
   anv_info.stride = gralloc_info->stride *
                     (isl_format_get_layout(format)->bpb / 8);

   result = anv_image_init(device, image, &anv_info);'''

SZ_OLD = '''   uint64_t img_size = image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].memory_range.size;
   if (img_size < bo->size) {'''

SZ_NEW = '''   uint64_t img_size = image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].memory_range.size;
   if (bo->size < img_size) {'''

PRV_OLD = '''struct anv_image_create_info {
   const VkImageCreateInfo *vk_info;

   /** An opt-in bitmask which filters an ISL-mapping of the Vulkan tiling. */
   isl_tiling_flags_t isl_tiling_flags;

   /** These flags will be added to any derived from VkImageCreateInfo. */
   isl_surf_usage_flags_t isl_extra_usage_flags;
};'''

PRV_NEW = '''struct anv_image_create_info {
   const VkImageCreateInfo *vk_info;

   /** An opt-in bitmask which filters an ISL-mapping of the Vulkan tiling. */
   isl_tiling_flags_t isl_tiling_flags;

   /** These flags will be added to any derived from VkImageCreateInfo. */
   isl_surf_usage_flags_t isl_extra_usage_flags;

   /** Explicit row pitch in bytes when imported from gralloc (0 = implicit). */
   uint32_t stride;
};'''

IMG_INC_OLD = '''#include "vk_format.h"'''

IMG_INC_NEW = '''#include "vk_format.h"
#if DETECT_OS_ANDROID && ANDROID_API_LEVEL >= 26
#include "vk_android.h"
#include "util/u_gralloc/u_gralloc.h"
#endif'''

IMG_GROW_OLD = '''      /* We require that surfaces be added in memory-order. This simplifies the
       * layout validation required by
       * VkImageDrmFormatModifierExplicitCreateInfoEXT,
       */
      if (unlikely(offset < container->size)) {
         return vk_errorf(device,
                          VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT,
                          "VkImageDrmFormatModifierExplicitCreateInfoEXT::"
                          "pPlaneLayouts[]::offset is too small");
      }
   }

   if (__builtin_add_overflow(offset, size, &container->size)) {'''

IMG_GROW_NEW = '''   }

   /* Surfaces can be added out of memory-order (e.g. swapped U/V planes in
    * DRM_FORMAT_YVU420). Track the maximum end offset across planes.
    */
   uint64_t range_end;
   if (__builtin_add_overflow(offset, size, &range_end)) {'''

IMG_GROW2_OLD = '''   container->alignment = MAX2(container->alignment, alignment);'''

IMG_GROW2_NEW = '''   container->size = MAX2(container->size, range_end);
   container->alignment = MAX2(container->alignment, alignment);'''

IMG_CHK_OLD = '''   assert(test_range->binding == p->expect_binding);
   assert(test_range->offset >= memory_range_end(*accum_range));
   assert(memory_range_is_aligned(*test_range));'''

IMG_CHK_NEW = '''   assert(test_range->binding == p->expect_binding);
   assert(memory_range_is_aligned(*test_range));'''

IMG_IMPL_OLD = '''   u_foreach_bit(b, image->vk.aspects) {
      VkImageAspectFlagBits aspect = 1 << b;
      const uint32_t plane = anv_image_aspect_to_plane(image, aspect);
      const  struct anv_format_plane plane_format =
         anv_get_format_plane(devinfo, image->vk.format, plane, image->vk.tiling);

      VkImageUsageFlags vk_usage = vk_image_usage(&image->vk, aspect);
      isl_surf_usage_flags_t isl_usage =
         choose_isl_surf_usage(image->vk.create_flags, vk_usage,
                               isl_extra_usage_flags, aspect);

      /* Must call this before adding any surfaces because it may modify
       * isl_tiling_flags.
       */
      bool needs_shadow =
         anv_image_plane_needs_shadow_surface(devinfo, plane_format,
                                              image->vk.tiling, vk_usage,
                                              image->vk.create_flags,
                                              &isl_tiling_flags);

      result = add_primary_surface(device, image, plane, plane_format,
                                   ANV_OFFSET_IMPLICIT, stride,
                                   isl_tiling_flags, isl_usage);'''

IMG_IMPL_NEW = '''   unsigned num_aspects = 0;
   VkImageAspectFlagBits aspects[3];
   u_foreach_bit(b, image->vk.aspects) {
      assert(num_aspects < 3);
      aspects[num_aspects++] = 1 << b;
   }
   if (image->from_ahb && image->vk.format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM &&
       num_aspects == 3) {
      /* Android YV12 stores Y-Cr-Cb, while Vulkan 3PLANE_420 expects Y-Cb-Cr. */
      aspects[1] = VK_IMAGE_ASPECT_PLANE_2_BIT;
      aspects[2] = VK_IMAGE_ASPECT_PLANE_1_BIT;
   }

   for (unsigned idx = 0; idx < num_aspects; idx++) {
      VkImageAspectFlagBits aspect = aspects[idx];
      const uint32_t plane = anv_image_aspect_to_plane(image, aspect);
      const struct anv_format_plane plane_format =
         anv_get_format_plane(devinfo, image->vk.format, plane, image->vk.tiling);

      uint32_t plane_stride = stride;
      if (plane_stride && plane_format.denominator_scales[0] > 1)
         plane_stride /= plane_format.denominator_scales[0];

      VkImageUsageFlags vk_usage = vk_image_usage(&image->vk, aspect);
      isl_surf_usage_flags_t isl_usage =
         choose_isl_surf_usage(image->vk.create_flags, vk_usage,
                               isl_extra_usage_flags, aspect);

      /* Must call this before adding any surfaces because it may modify
       * isl_tiling_flags.
       */
      bool needs_shadow =
         anv_image_plane_needs_shadow_surface(devinfo, plane_format,
                                              image->vk.tiling, vk_usage,
                                              image->vk.create_flags,
                                              &isl_tiling_flags);

      result = add_primary_surface(device, image, plane, plane_format,
                                   ANV_OFFSET_IMPLICIT, plane_stride,
                                   isl_tiling_flags, isl_usage);'''

IMG_EXPL_OLD = '''      result = add_primary_surface(device, image, plane,
                                   format_plane,
                                   primary_layout->offset,
                                   primary_layout->rowPitch,
                                   isl_tiling_flags,
                                   isl_extra_usage_flags);'''

IMG_EXPL_NEW = '''      VkImageUsageFlags vk_usage = vk_image_usage(&image->vk, aspect);
      isl_surf_usage_flags_t isl_usage =
         choose_isl_surf_usage(image->vk.create_flags, vk_usage,
                               isl_extra_usage_flags, aspect);
      result = add_primary_surface(device, image, plane,
                                   format_plane,
                                   primary_layout->offset,
                                   primary_layout->rowPitch,
                                   isl_tiling_flags,
                                   isl_usage);'''

IMG_DEFER_OLD = '''   /* In case of AHardwareBuffer import, we don't know the layout yet */
   if (image->vk.external_handle_types &
       VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
      image->from_ahb = true;
#if DETECT_OS_ANDROID
      image->vk.ahb_format = anv_ahb_format_for_vk_format(image->vk.format);
#endif
      return VK_SUCCESS;
   }'''

IMG_DEFER_NEW = '''   /* In case of AHardwareBuffer import, we don't know the layout yet */
   if (image->vk.external_handle_types &
       VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
      image->from_ahb = true;
#if DETECT_OS_ANDROID
      image->vk.ahb_format = anv_ahb_format_for_vk_format(image->vk.format);
#endif
      return VK_SUCCESS;
   }

#ifdef VK_USE_PLATFORM_ANDROID_KHR
   /* In the case of gralloc-backed swap chain image, we don't know the
    * tiling/stride layout until vkBindImageMemory2 passes VkNativeBufferANDROID.
    */
   if (vk_find_struct_const(pCreateInfo->pNext,
                            IMAGE_SWAPCHAIN_CREATE_INFO_KHR) != NULL)
      return VK_SUCCESS;
#endif'''

IMG_STRIDE_OLD = '''   } else {
      r = add_all_surfaces_implicit_layout(device, image, fmt_list, 0,
                                           isl_tiling_flags,
                                           create_info->isl_extra_usage_flags);
   }'''

IMG_STRIDE_NEW = '''   } else {
      r = add_all_surfaces_implicit_layout(device, image, fmt_list, create_info->stride,
                                           isl_tiling_flags,
                                           create_info->isl_extra_usage_flags);
   }'''

IMG_AHW_OLD = '''static void
resolve_ahw_image(struct anv_device *device,
                  struct anv_image *image,
                  struct anv_device_memory *mem)
{
#if DETECT_OS_ANDROID && ANDROID_API_LEVEL >= 26
   assert(mem->ahw);
   AHardwareBuffer_Desc desc;
   AHardwareBuffer_describe(mem->ahw, &desc);
   VkResult result;

   /* Check tiling. */
   enum isl_tiling tiling;
   result = anv_device_get_bo_tiling(device, mem->bo, &tiling);
   assert(result == VK_SUCCESS);

   VkImageTiling vk_tiling =
      tiling == ISL_TILING_LINEAR ? VK_IMAGE_TILING_LINEAR :
                                    VK_IMAGE_TILING_OPTIMAL;
   isl_tiling_flags_t isl_tiling_flags = (1u << tiling);

   /* Check format. */
   VkFormat vk_format = vk_format_from_android(desc.format, desc.usage);
   enum isl_format isl_fmt = anv_get_isl_format(device->info,
                                                vk_format,
                                                VK_IMAGE_ASPECT_COLOR_BIT,
                                                vk_tiling);
   assert(isl_fmt != ISL_FORMAT_UNSUPPORTED);

   /* Handle RGB(X)->RGBA fallback. */
   switch (desc.format) {
   case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
   case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
      if (isl_format_is_rgb(isl_fmt))
         isl_fmt = isl_format_rgb_to_rgba(isl_fmt);
      break;
   }

   /* Now we are able to fill anv_image fields properly and create
    * isl_surface for it.
    */
   vk_image_set_format(&image->vk, vk_format);
   image->n_planes = anv_get_format_planes(image->vk.format);

   uint32_t stride = desc.stride *
                     (isl_format_get_layout(isl_fmt)->bpb / 8);

   result = add_all_surfaces_implicit_layout(device, image, NULL, stride,
                                             isl_tiling_flags,
                                             ISL_SURF_USAGE_DISABLE_AUX_BIT);
   assert(result == VK_SUCCESS);
#endif
}'''

IMG_AHW_NEW = '''static void
resolve_ahw_image(struct anv_device *device,
                  struct anv_image *image,
                  struct anv_device_memory *mem)
{
#if DETECT_OS_ANDROID && ANDROID_API_LEVEL >= 26
   assert(mem->ahw);
   AHardwareBuffer_Desc desc;
   AHardwareBuffer_describe(mem->ahw, &desc);
   VkResult result;

   /* Preserve image->vk.format if already resolved from pCreateInfo->format or
    * VkExternalFormatANDROID; otherwise query CrOS gralloc for the actual
    * allocated DRM fourcc (e.g. 3-plane YVU420 vs 2-plane NV12 for 0x23).
    */
   if (image->vk.format == VK_FORMAT_UNDEFINED) {
      VkFormat vk_format = VK_FORMAT_UNDEFINED;
      struct u_gralloc *u_gralloc = vk_android_get_ugralloc();
      if (u_gralloc) {
         struct u_gralloc_buffer_handle gr_handle = {
            .handle = AHardwareBuffer_getNativeHandle(mem->ahw),
            .pixel_stride = desc.stride,
            .hal_format = desc.format,
         };
         struct u_gralloc_buffer_basic_info info;
         if (u_gralloc_get_buffer_basic_info(u_gralloc, &gr_handle, &info) == 0) {
            switch (info.drm_fourcc) {
            case DRM_FORMAT_YVU420:
            case 0x37393939: /* DRM_FORMAT_YVU420_ANDROID */
            case DRM_FORMAT_YUV420:
               vk_format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
               break;
            case DRM_FORMAT_NV12:
               vk_format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
               break;
            case DRM_FORMAT_P010:
               vk_format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
               break;
            case DRM_FORMAT_ABGR8888:
            case DRM_FORMAT_XBGR8888:
               vk_format = VK_FORMAT_R8G8B8A8_UNORM;
               break;
            case DRM_FORMAT_ARGB8888:
            case DRM_FORMAT_XRGB8888:
               vk_format = VK_FORMAT_B8G8R8A8_UNORM;
               break;
            case DRM_FORMAT_RGB565:
               vk_format = VK_FORMAT_R5G6B5_UNORM_PACK16;
               break;
            default:
               break;
            }
         }
      }
      if (vk_format == VK_FORMAT_UNDEFINED)
         vk_format = vk_format_from_android(desc.format, desc.usage);
      assert(vk_format != VK_FORMAT_UNDEFINED);
      vk_image_set_format(&image->vk, vk_format);
   }

   image->n_planes = anv_get_format_planes(image->vk.format);

   /* Query exact per-plane offsets, row pitches, and YV12 U/V swap from CrOS gralloc. */
   VkImageDrmFormatModifierExplicitCreateInfoEXT mod_explicit_info;
   VkSubresourceLayout layouts[ISL_MODIFIER_MAX_PLANES];
   result = vk_android_get_ahb_layout(mem->ahw, &mod_explicit_info,
                                      layouts, ISL_MODIFIER_MAX_PLANES);
   if (result == VK_SUCCESS) {
      isl_tiling_flags_t isl_tiling_flags = 0;
      const struct isl_drm_modifier_info *isl_mod_info =
         mod_explicit_info.drmFormatModifier != DRM_FORMAT_MOD_INVALID
            ? isl_drm_modifier_get_info(mod_explicit_info.drmFormatModifier)
            : NULL;
      if (isl_mod_info) {
         image->vk.drm_format_mod = mod_explicit_info.drmFormatModifier;
         image->vk.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
         isl_tiling_flags = 1u << isl_mod_info->tiling;
      } else {
         enum isl_tiling tiling = ISL_TILING_LINEAR;
         anv_device_get_bo_tiling(device, mem->bo, &tiling);
         image->vk.tiling = tiling == ISL_TILING_LINEAR ? VK_IMAGE_TILING_LINEAR
                                                        : VK_IMAGE_TILING_OPTIMAL;
         isl_tiling_flags = 1u << tiling;
      }
      result = add_all_surfaces_explicit_layout(device, image, NULL,
                                                &mod_explicit_info,
                                                isl_tiling_flags,
                                                ISL_SURF_USAGE_DISABLE_AUX_BIT);
      if (result == VK_SUCCESS) {
         check_memory_bindings(device, image);
         return;
      }
      /* Reset binding accumulator before falling back to implicit layout. */
      for (int i = 0; i < ANV_IMAGE_MEMORY_BINDING_END; ++i)
         image->bindings[i].memory_range = (struct anv_image_memory_range) { .binding = i };
      image->vk.drm_format_mod = DRM_FORMAT_MOD_INVALID;
   }

   /* Fallback: implicit layout with BO tiling query and per-plane chroma stride. */
   enum isl_tiling tiling;
   result = anv_device_get_bo_tiling(device, mem->bo, &tiling);
   assert(result == VK_SUCCESS);

   VkImageTiling vk_tiling =
      tiling == ISL_TILING_LINEAR ? VK_IMAGE_TILING_LINEAR :
                                    VK_IMAGE_TILING_OPTIMAL;
   image->vk.tiling = vk_tiling;
   isl_tiling_flags_t isl_tiling_flags = (1u << tiling);

   struct anv_format_plane plane0_fmt =
      anv_get_format_plane(device->info, image->vk.format, 0, vk_tiling);
   enum isl_format isl_fmt = plane0_fmt.isl_format;
   if (isl_format_is_rgb(isl_fmt))
      isl_fmt = isl_format_rgb_to_rgba(isl_fmt);
   assert(isl_fmt != ISL_FORMAT_UNSUPPORTED);

   uint32_t stride = desc.stride *
                     (isl_format_get_layout(isl_fmt)->bpb / 8);

   result = add_all_surfaces_implicit_layout(device, image, NULL, stride,
                                             isl_tiling_flags,
                                             ISL_SURF_USAGE_DISABLE_AUX_BIT);
   assert(result == VK_SUCCESS);
   check_memory_bindings(device, image);
#endif
}'''

IMG_BIND_OLD = '''      case VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID: {
         const VkNativeBufferANDROID *gralloc_info =
            (const VkNativeBufferANDROID *)s;
         VkResult result = anv_image_bind_from_gralloc(device, image,
                                                       gralloc_info);
         if (result != VK_SUCCESS)
            return result;
         did_bind = true;
         break;
      }'''

IMG_BIND_NEW = '''      case VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID: {
         const VkNativeBufferANDROID *gralloc_info =
            (const VkNativeBufferANDROID *)s;
         int dma_buf = (gralloc_info->handle && gralloc_info->handle->numFds) ?
                       gralloc_info->handle->data[0] : -1;
         if (dma_buf < 0)
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
         struct anv_bo *bo = NULL;
         VkResult result = anv_device_import_bo(device, dma_buf,
                                                ANV_BO_ALLOC_EXTERNAL |
                                                ANV_BO_ALLOC_IMPLICIT_SYNC |
                                                ANV_BO_ALLOC_IMPLICIT_WRITE,
                                                0 /* client_address */,
                                                &bo);
         if (result != VK_SUCCESS)
            return vk_errorf(device, result, "failed to import dma-buf from VkNativeBufferANDROID");
         if (image->n_planes == 0) {
            enum isl_tiling tiling;
            result = anv_device_get_bo_tiling(device, bo, &tiling);
            if (result != VK_SUCCESS) {
               anv_device_release_bo(device, bo);
               return vk_errorf(device, result, "failed to get tiling from VkNativeBufferANDROID");
            }
            VkImageTiling vk_tiling =
               tiling == ISL_TILING_LINEAR ? VK_IMAGE_TILING_LINEAR :
                                             VK_IMAGE_TILING_OPTIMAL;
            image->vk.tiling = vk_tiling;
            image->n_planes = anv_get_format_planes(image->vk.format);
            enum isl_format isl_fmt = anv_get_isl_format(device->info,
                                                         image->vk.format,
                                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                                         vk_tiling);
            if (isl_format_is_rgb(isl_fmt))
               isl_fmt = isl_format_rgb_to_rgba(isl_fmt);
            uint32_t stride = gralloc_info->stride *
                              (isl_format_get_layout(isl_fmt)->bpb / 8);
            result = add_all_surfaces_implicit_layout(device, image, NULL, stride,
                                                      1u << tiling,
                                                      ISL_SURF_USAGE_DISABLE_AUX_BIT);
            if (result != VK_SUCCESS) {
               anv_device_release_bo(device, bo);
               return vk_errorf(device, result, "failed to add surfaces from VkNativeBufferANDROID");
            }
            check_memory_bindings(device, image);
         }
         VkMemoryRequirements2 mem_reqs = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
         };
         anv_image_get_memory_requirements(device, image, image->vk.aspects, &mem_reqs);
         VkDeviceSize aligned_image_size =
            align64(mem_reqs.memoryRequirements.size, mem_reqs.memoryRequirements.alignment);
         if (bo->size < aligned_image_size) {
            anv_device_release_bo(device, bo);
            return vk_errorf(device, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                             "dma-buf from VkNativeBufferANDROID is too small for VkImage: %"PRIu64"B < %"PRIu64"B",
                             bo->size, aligned_image_size);
         }
         image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].address.bo = bo;
         image->from_gralloc = true;
         did_bind = true;
         break;
      }'''


def patch_once(path, old, new):
    source = open(path).read()
    if new in source:
        print('already patched:', path)
    elif source.count(old) != 1:
        sys.exit(f'{path}: anchor found {source.count(old)} times:\n{old[:120]}')
    else:
        open(path, 'w').write(source.replace(old, new))
        print('patched:', path)


for old, new in [(VKA_SWAP_OLD, VKA_SWAP_NEW), (VKA_FMT_OLD, VKA_FMT_NEW)]:
    patch_once(P_VKA, old, new)

for old, new in [(FMT_OLD, FMT_NEW), (AHB_PROPS_OLD, AHB_PROPS_NEW),
                 (IMP_OLD, IMP_NEW), (GR_INIT_OLD, GR_INIT_NEW), (SZ_OLD, SZ_NEW)]:
    patch_once(P_AND, old, new)

patch_once(P_PRV, PRV_OLD, PRV_NEW)

for old, new in [(IMG_INC_OLD, IMG_INC_NEW), (IMG_GROW_OLD, IMG_GROW_NEW),
                 (IMG_GROW2_OLD, IMG_GROW2_NEW), (IMG_CHK_OLD, IMG_CHK_NEW),
                 (IMG_IMPL_OLD, IMG_IMPL_NEW), (IMG_EXPL_OLD, IMG_EXPL_NEW),
                 (IMG_DEFER_OLD, IMG_DEFER_NEW), (IMG_STRIDE_OLD, IMG_STRIDE_NEW),
                 (IMG_AHW_OLD, IMG_AHW_NEW), (IMG_BIND_OLD, IMG_BIND_NEW)]:
    patch_once(P_IMG, old, new)
