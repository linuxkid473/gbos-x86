/*
 * GPU (Vulkan compute) form of the converter's image stages, built into the Android-native
 * bridge (-DIPU6_GPU; tools/build_ipu6_loopback_gpu.sh). Included by ipu6-loopback.c, so it
 * shares its tables and tuning.
 *
 * The CPU still does what reads the frame sparsely: white-balance and exposure statistics,
 * autofocus, choosing the colour matrix and lens shading (calculate_white_balance, update_lsc).
 * The GPU does the four per-pixel stages, as compute shaders in kernel/ipu6-gpu/ that use the
 * same integer arithmetic as the CPU code: colour grid (rgb_linear_at), chroma box average,
 * colour mix to Y'U'V' (linear_colour_mix_rows) and the full-resolution output with detail,
 * temporal filter and YUYV packing (output_rows). Host tests (tools/test_ipu6_gpu.sh, Mesa's
 * lavapipe) compare both paths on the same frames.
 *
 * libvulkan is opened at run time: without it, or on any Vulkan error, the bridge says so and
 * converts on the CPU as before. All buffers are host-visible (the Iris Xe GPU shares memory with
 * the CPU); the raw frame is copied into its buffer, the YUYV result copied out.
 */
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include "ipu6-gpu/spirv.h"

#define GPU_FN(name) static PFN_##name name
GPU_FN(vkGetInstanceProcAddr);
GPU_FN(vkCreateInstance);
GPU_FN(vkDestroyInstance);
GPU_FN(vkEnumeratePhysicalDevices);
GPU_FN(vkGetPhysicalDeviceProperties);
GPU_FN(vkGetPhysicalDeviceQueueFamilyProperties);
GPU_FN(vkGetPhysicalDeviceMemoryProperties);
GPU_FN(vkCreateDevice);
GPU_FN(vkGetDeviceProcAddr);
GPU_FN(vkDestroyDevice);
GPU_FN(vkGetDeviceQueue);
GPU_FN(vkCreateBuffer);
GPU_FN(vkDestroyBuffer);
GPU_FN(vkGetBufferMemoryRequirements);
GPU_FN(vkAllocateMemory);
GPU_FN(vkFreeMemory);
GPU_FN(vkBindBufferMemory);
GPU_FN(vkMapMemory);
GPU_FN(vkCreateDescriptorSetLayout);
GPU_FN(vkCreatePipelineLayout);
GPU_FN(vkCreateShaderModule);
GPU_FN(vkDestroyShaderModule);
GPU_FN(vkCreateComputePipelines);
GPU_FN(vkCreateDescriptorPool);
GPU_FN(vkAllocateDescriptorSets);
GPU_FN(vkUpdateDescriptorSets);
GPU_FN(vkCreateCommandPool);
GPU_FN(vkAllocateCommandBuffers);
GPU_FN(vkCreateFence);
GPU_FN(vkResetFences);
GPU_FN(vkWaitForFences);
GPU_FN(vkBeginCommandBuffer);
GPU_FN(vkEndCommandBuffer);
GPU_FN(vkResetCommandBuffer);
GPU_FN(vkCmdBindPipeline);
GPU_FN(vkCmdBindDescriptorSets);
GPU_FN(vkCmdPushConstants);
GPU_FN(vkCmdDispatch);
GPU_FN(vkCmdPipelineBarrier);
GPU_FN(vkQueueSubmit);
GPU_FN(vkDeviceWaitIdle);

/* Mirrors Params in kernel/ipu6-gpu/common.glsl (std430: 4-byte scalars and arrays). */
struct gpu_params {
	int32_t width, height, stride_px, col_w, col_h, out_w, out_h;
	int32_t gain0, gain2, ccm_active, saturation_q12;
	int32_t ccm[9], nir_keep[3];
	int32_t ir_visible_q12, has_ir, highlight_start, highlight_full, lsc_active, chroma_guard;
	float wb_nir_ratio, nir_free, nir_half, hf_start, hf_full, hf_min;
	int32_t radius, edges;
	float edge_desaturation;
	int32_t detail_q12, tnr_threshold, tnr_have_previous, rotate180;
	int32_t cfa[16], cfa_count[4];
	int32_t crop_x, crop_y, crop_w, crop_h;
};

enum {
	GB_PARAMS, GB_RAW, GB_LUT_RAW, GB_LUT_GAMMA, GB_LUT_DETAIL, GB_LSC, GB_LIN, GB_AVG, GB_TMP,
	GB_GREEN, GB_YUV, GB_TNR, GB_OUT, GB_COUNT
};
enum { GP_GRID, GP_BOX, GP_MIX, GP_OUT, GP_COUNT };

struct gpu_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *map;
	VkDeviceSize size;
};

static struct {
	int ok;			/* converting on the GPU */
	int allow_cpu_device;	/* host tests: accept a CPU implementation (lavapipe) */
	void *lib;
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice device;
	VkQueue queue;
	uint32_t family, memory_type;
	struct gpu_buffer buf[GB_COUNT];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline[GP_COUNT];
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;
	VkFence fence;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	unsigned int lsc_generation, detail_q12, frames;
	double ms_total;
} gpu = { .lsc_generation = ~0u, .detail_q12 = ~0u };

static int gpu_fail(const char *what, VkResult r)
{
	fprintf(stderr, "gpu: %s failed (VkResult %d); converting on the CPU\n", what, (int)r);
	return -1;
}

static int gpu_load_instance_functions(void)
{
#define LOAD_I(name) if (!(name = (PFN_##name)vkGetInstanceProcAddr(gpu.instance, #name))) return -1
	LOAD_I(vkDestroyInstance);
	LOAD_I(vkEnumeratePhysicalDevices);
	LOAD_I(vkGetPhysicalDeviceProperties);
	LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
	LOAD_I(vkGetPhysicalDeviceMemoryProperties);
	LOAD_I(vkCreateDevice);
	LOAD_I(vkGetDeviceProcAddr);
	return 0;
#undef LOAD_I
}

static int gpu_load_device_functions(void)
{
#define LOAD_D(name) if (!(name = (PFN_##name)vkGetDeviceProcAddr(gpu.device, #name))) return -1
	LOAD_D(vkDestroyDevice);
	LOAD_D(vkGetDeviceQueue);
	LOAD_D(vkCreateBuffer);
	LOAD_D(vkDestroyBuffer);
	LOAD_D(vkGetBufferMemoryRequirements);
	LOAD_D(vkAllocateMemory);
	LOAD_D(vkFreeMemory);
	LOAD_D(vkBindBufferMemory);
	LOAD_D(vkMapMemory);
	LOAD_D(vkCreateDescriptorSetLayout);
	LOAD_D(vkCreatePipelineLayout);
	LOAD_D(vkCreateShaderModule);
	LOAD_D(vkDestroyShaderModule);
	LOAD_D(vkCreateComputePipelines);
	LOAD_D(vkCreateDescriptorPool);
	LOAD_D(vkAllocateDescriptorSets);
	LOAD_D(vkUpdateDescriptorSets);
	LOAD_D(vkCreateCommandPool);
	LOAD_D(vkAllocateCommandBuffers);
	LOAD_D(vkCreateFence);
	LOAD_D(vkResetFences);
	LOAD_D(vkWaitForFences);
	LOAD_D(vkBeginCommandBuffer);
	LOAD_D(vkEndCommandBuffer);
	LOAD_D(vkResetCommandBuffer);
	LOAD_D(vkCmdBindPipeline);
	LOAD_D(vkCmdBindDescriptorSets);
	LOAD_D(vkCmdPushConstants);
	LOAD_D(vkCmdDispatch);
	LOAD_D(vkCmdPipelineBarrier);
	LOAD_D(vkQueueSubmit);
	LOAD_D(vkDeviceWaitIdle);
	return 0;
#undef LOAD_D
}

static int gpu_buffer_create(struct gpu_buffer *b, VkDeviceSize size)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryRequirements req;
	VkResult r;

	if ((r = vkCreateBuffer(gpu.device, &info, NULL, &b->buffer)))
		return gpu_fail("vkCreateBuffer", r);
	vkGetBufferMemoryRequirements(gpu.device, b->buffer, &req);
	if (!(req.memoryTypeBits & (1u << gpu.memory_type)))
		return gpu_fail("buffer memory type", VK_ERROR_FEATURE_NOT_PRESENT);
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		.memoryTypeIndex = gpu.memory_type,
	};
	if ((r = vkAllocateMemory(gpu.device, &alloc, NULL, &b->memory)))
		return gpu_fail("vkAllocateMemory", r);
	if ((r = vkBindBufferMemory(gpu.device, b->buffer, b->memory, 0)))
		return gpu_fail("vkBindBufferMemory", r);
	if ((r = vkMapMemory(gpu.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map)))
		return gpu_fail("vkMapMemory", r);
	b->size = size;
	memset(b->map, 0, size);
	return 0;
}

static void gpu_buffer_destroy(struct gpu_buffer *b)
{
	if (b->buffer)
		vkDestroyBuffer(gpu.device, b->buffer, NULL);
	if (b->memory)
		vkFreeMemory(gpu.device, b->memory, NULL);
	memset(b, 0, sizeof(*b));
}

static void gpu_write_descriptors(void)
{
	VkDescriptorBufferInfo info[GB_COUNT];
	VkWriteDescriptorSet write[GB_COUNT];

	for (int i = 0; i < GB_COUNT; i++) {
		info[i] = (VkDescriptorBufferInfo){ gpu.buf[i].buffer, 0, VK_WHOLE_SIZE };
		write[i] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = gpu.set,
			.dstBinding = (uint32_t)i, .descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info[i],
		};
	}
	vkUpdateDescriptorSets(gpu.device, GB_COUNT, write, 0, NULL);
}

static int gpu_pipeline(int which, const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo mi = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code,
	};
	VkShaderModule module;
	VkResult r;

	if ((r = vkCreateShaderModule(gpu.device, &mi, NULL, &module)))
		return gpu_fail("vkCreateShaderModule", r);
	VkComputePipelineCreateInfo pi = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
		},
		.layout = gpu.layout,
	};
	r = vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &pi, NULL, &gpu.pipeline[which]);
	vkDestroyShaderModule(gpu.device, module, NULL);
	return r ? gpu_fail("vkCreateComputePipelines", r) : 0;
}

static const VkApplicationInfo gpu_app = {
	.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ipu6-loopback",
	.apiVersion = VK_API_VERSION_1_1,
};

/* Create the instance through vkGetInstanceProcAddr (set by the caller) and list the devices. */
static int gpu_instance_and_devices(const char *via, VkPhysicalDevice *devices, uint32_t *count)
{
	VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &gpu_app };
	VkResult r;

	if (!vkGetInstanceProcAddr ||
	    !(vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance"))) {
		fprintf(stderr, "gpu: %s: no vkCreateInstance\n", via);
		return -1;
	}
	if ((r = vkCreateInstance(&ii, NULL, &gpu.instance))) {
		fprintf(stderr, "gpu: %s: vkCreateInstance failed (VkResult %d)\n", via, (int)r);
		gpu.instance = VK_NULL_HANDLE;
		return -1;
	}
	if (gpu_load_instance_functions()) {
		fprintf(stderr, "gpu: %s: instance functions missing\n", via);
		return -1;
	}
	if ((r = vkEnumeratePhysicalDevices(gpu.instance, count, devices)) < 0 || !*count) {
		fprintf(stderr, "gpu: %s: vkEnumeratePhysicalDevices failed (VkResult %d, %u devices)\n",
			via, (int)r, *count);
		vkDestroyInstance(gpu.instance, NULL);
		gpu.instance = VK_NULL_HANDLE;
		return -1;
	}
	return 0;
}

#ifdef __ANDROID__
/*
 * Android's libvulkan is a thin loader: it opens the vendor driver as a hardware module
 * (/vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so, here Mesa's vulkan.intel.so) and calls its
 * entry points. On boot-16 (2026-11-20) the loader found no device for this binary, started from
 * the stick (vkEnumeratePhysicalDevices: VK_ERROR_INITIALIZATION_FAILED), probably because the
 * loader's driver lookup depends on the linker namespace of where a binary lives. So when the
 * loader yields nothing, open the driver module ourselves: the module (symbol "HMI") opens device
 * "vk0", whose GetInstanceProcAddr is the driver's own. Layouts as in AOSP
 * hardware/libhardware/include/hardware/{hardware,hwvulkan}.h (64-bit).
 */
#include <sys/system_properties.h>

struct gbos_hw_module;
struct gbos_hw_device {
	uint32_t tag, version;
	struct gbos_hw_module *module;
	uint64_t reserved[12];
	int (*close)(struct gbos_hw_device *device);
};
struct gbos_hw_module_methods {
	int (*open)(const struct gbos_hw_module *module, const char *id, struct gbos_hw_device **device);
};
struct gbos_hw_module {
	uint32_t tag;
	uint16_t module_api_version, hal_api_version;
	const char *id, *name, *author;
	struct gbos_hw_module_methods *methods;
	void *dso;
	uint64_t reserved[32 - 7];
};
struct gbos_hwvulkan_device {
	struct gbos_hw_device common;
	PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
	PFN_vkCreateInstance CreateInstance;
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};
#define GBOS_HWVULKAN_DEVICE_0 "vk0"

static int gpu_open_vendor_driver(void)
{
	char name[PROP_VALUE_MAX] = "", path[256];

	if (__system_property_get("ro.hardware.vulkan", name) <= 0)
		snprintf(name, sizeof(name), "intel");
	snprintf(path, sizeof(path), "/vendor/lib64/hw/vulkan.%s.so", name);
	void *dso = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!dso) {
		fprintf(stderr, "gpu: vendor driver: dlopen %s: %s\n", path, dlerror());
		return -1;
	}
	struct gbos_hw_module *module = dlsym(dso, "HMI");
	struct gbos_hw_device *device = NULL;
	if (!module || !module->methods || !module->methods->open) {
		fprintf(stderr, "gpu: vendor driver: %s has no hardware module (HMI)\n", path);
		return -1;
	}
	int err = module->methods->open(module, GBOS_HWVULKAN_DEVICE_0, &device);
	if (err || !device) {
		fprintf(stderr, "gpu: vendor driver: open(vk0) failed (%d)\n", err);
		return -1;
	}
	vkGetInstanceProcAddr = ((struct gbos_hwvulkan_device *)device)->GetInstanceProcAddr;
	printf("gpu: using the vendor driver directly (%s)\n", path);
	return 0;
}
#endif

/* 0 when the GPU path is ready. Prints why not otherwise. */
static int gpu_init(void)
{
	static const char *libs[] = { "libvulkan.so", "libvulkan.so.1" };
	VkPhysicalDevice devices[8];
	uint32_t count = 8;
	int ready = -1;
	VkResult r;

	for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]) && !gpu.lib; i++)
		gpu.lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
	if (gpu.lib) {
		vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(gpu.lib, "vkGetInstanceProcAddr");
		ready = gpu_instance_and_devices("libvulkan", devices, &count);
	} else {
		fprintf(stderr, "gpu: no libvulkan (%s)\n", dlerror());
	}
#ifdef __ANDROID__
	if (ready) {
		count = 8;
		if (gpu_open_vendor_driver() == 0)
			ready = gpu_instance_and_devices("vendor driver", devices, &count);
	}
#endif
	if (ready)
		return gpu_fail("finding a Vulkan device", VK_ERROR_INITIALIZATION_FAILED);
	for (uint32_t i = 0; i < count && !gpu.phys; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(devices[i], &p);
		int gpu_type = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
			       p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
		if (!gpu_type && !(gpu.allow_cpu_device && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU))
			continue;
		VkQueueFamilyProperties q[16];
		uint32_t nq = 16;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &nq, q);
		for (uint32_t f = 0; f < nq; f++)
			if (q[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
				gpu.phys = devices[i];
				gpu.family = f;
				snprintf(gpu.name, sizeof(gpu.name), "%s", p.deviceName);
				break;
			}
	}
	if (!gpu.phys)
		return gpu_fail("finding a GPU with a compute queue", VK_ERROR_INITIALIZATION_FAILED);

	/* Host-visible, coherent memory; device-local too where there is such a type (shared memory). */
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu.phys, &mp);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	int found = -1;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if ((f & want) != want)
			continue;
		if (found < 0 || (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			found = (int)i;
		if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			break;
	}
	if (found < 0)
		return gpu_fail("finding host-visible memory", VK_ERROR_INITIALIZATION_FAILED);
	gpu.memory_type = (uint32_t)found;

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qi = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = gpu.family,
		.queueCount = 1, .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo di = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
	};
	if ((r = vkCreateDevice(gpu.phys, &di, NULL, &gpu.device)))
		return gpu_fail("vkCreateDevice", r);
	if (gpu_load_device_functions())
		return gpu_fail("loading device functions", VK_ERROR_INITIALIZATION_FAILED);
	vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);

	const VkDeviceSize col = (VkDeviceSize)COL_WIDTH * COL_HEIGHT * 4;
	const VkDeviceSize sizes[GB_COUNT] = {
		[GB_PARAMS] = sizeof(struct gpu_params), [GB_RAW] = 4096,	/* grows with the frame */
		[GB_LUT_RAW] = 1024 * 4, [GB_LUT_GAMMA] = (LINEAR_MAX + 1) * 4,
		[GB_LUT_DETAIL] = (DETAIL_LUT_MAX + 1) * 4, [GB_LSC] = 3 * col, [GB_LIN] = 3 * col,
		[GB_AVG] = 3 * col, [GB_TMP] = 3 * col, [GB_GREEN] = col, [GB_YUV] = 3 * col,
		[GB_TNR] = (VkDeviceSize)OUT_WIDTH * OUT_HEIGHT * 4, [GB_OUT] = OUT_FRAME_BYTES,
	};
	for (int i = 0; i < GB_COUNT; i++)
		if (gpu_buffer_create(&gpu.buf[i], sizes[i]))
			return -1;

	VkDescriptorSetLayoutBinding bind[GB_COUNT];
	for (int i = 0; i < GB_COUNT; i++)
		bind[i] = (VkDescriptorSetLayoutBinding){
			.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		};
	VkDescriptorSetLayoutCreateInfo li = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = GB_COUNT, .pBindings = bind,
	};
	if ((r = vkCreateDescriptorSetLayout(gpu.device, &li, NULL, &gpu.set_layout)))
		return gpu_fail("vkCreateDescriptorSetLayout", r);
	VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
	VkPipelineLayoutCreateInfo pli = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
		.pSetLayouts = &gpu.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
	};
	if ((r = vkCreatePipelineLayout(gpu.device, &pli, NULL, &gpu.layout)))
		return gpu_fail("vkCreatePipelineLayout", r);
	if (gpu_pipeline(GP_GRID, spv_grid, sizeof(spv_grid)) || gpu_pipeline(GP_BOX, spv_box, sizeof(spv_box)) ||
	    gpu_pipeline(GP_MIX, spv_mix, sizeof(spv_mix)) || gpu_pipeline(GP_OUT, spv_out, sizeof(spv_out)))
		return -1;

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GB_COUNT };
	VkDescriptorPoolCreateInfo dpi = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
	};
	if ((r = vkCreateDescriptorPool(gpu.device, &dpi, NULL, &gpu.pool)))
		return gpu_fail("vkCreateDescriptorPool", r);
	VkDescriptorSetAllocateInfo dsi = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = gpu.pool,
		.descriptorSetCount = 1, .pSetLayouts = &gpu.set_layout,
	};
	if ((r = vkAllocateDescriptorSets(gpu.device, &dsi, &gpu.set)))
		return gpu_fail("vkAllocateDescriptorSets", r);
	gpu_write_descriptors();

	VkCommandPoolCreateInfo cpi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = gpu.family,
	};
	if ((r = vkCreateCommandPool(gpu.device, &cpi, NULL, &gpu.cmd_pool)))
		return gpu_fail("vkCreateCommandPool", r);
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = gpu.cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
	};
	if ((r = vkAllocateCommandBuffers(gpu.device, &cai, &gpu.cmd)))
		return gpu_fail("vkAllocateCommandBuffers", r);
	VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if ((r = vkCreateFence(gpu.device, &fi, NULL, &gpu.fence)))
		return gpu_fail("vkCreateFence", r);

	/* Tables that never change. */
	init_raw_lut();
	init_gamma();
	uint32_t *lut = gpu.buf[GB_LUT_RAW].map;
	for (int i = 0; i < 1024; i++)
		lut[i] = raw_lin_lut[i];
	lut = gpu.buf[GB_LUT_GAMMA].map;
	for (unsigned int i = 0; i <= LINEAR_MAX; i++)
		lut[i] = gamma_lut[i];
	gpu.ok = 1;
	return 0;
}

static void gpu_barrier(void)
{
	VkMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(gpu.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			     0, 1, &b, 0, NULL, 0, NULL);
}

static void gpu_dispatch(int which, uint32_t x, uint32_t y, uint32_t z, int push)
{
	vkCmdBindPipeline(gpu.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline[which]);
	vkCmdPushConstants(gpu.cmd, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &push);
	vkCmdDispatch(gpu.cmd, (x + 15) / 16, (y + 15) / 16, z);
}

/*
 * The per-pixel stages on the GPU, after convert_bayer_yuyv has done the statistics and chosen
 * the tables. 0 on success; on failure the caller converts this frame on the CPU.
 */
static int gpu_convert(const uint16_t *raw, unsigned int width, unsigned int height,
		       unsigned int stride, const unsigned int gain_q12[3], int radius, double gain,
		       unsigned char *out)
{
	VkResult r;
	double t0 = monotonic_s();
	VkDeviceSize raw_bytes = (VkDeviceSize)stride * height;

	if (gpu.buf[GB_RAW].size < raw_bytes) {
		vkDeviceWaitIdle(gpu.device);
		gpu_buffer_destroy(&gpu.buf[GB_RAW]);
		if (gpu_buffer_create(&gpu.buf[GB_RAW], raw_bytes))
			return -1;
		gpu_write_descriptors();
	}
	memcpy(gpu.buf[GB_RAW].map, raw, raw_bytes);
	if (lsc_active && gpu.lsc_generation != lsc_generation) {
		uint32_t *l = gpu.buf[GB_LSC].map;
		for (int c = 0; c < 3; c++)
			for (int y = 0; y < COL_HEIGHT; y++)
				for (int x = 0; x < COL_WIDTH; x++)
					*l++ = lsc_map[c][y][x];
		gpu.lsc_generation = lsc_generation;
	}
	int detail_q12 = detail_q12_for_gain(gain);
	detail_lut_build(detail_q12);
	if (gpu.detail_q12 != (unsigned int)detail_q12) {
		uint32_t *l = gpu.buf[GB_LUT_DETAIL].map;
		for (int i = 0; i <= DETAIL_LUT_MAX; i++)
			l[i] = detail_lut[i];
		gpu.detail_q12 = (unsigned int)detail_q12;
	}

	struct gpu_params *p = gpu.buf[GB_PARAMS].map;
	memset(p, 0, sizeof(*p));
	p->width = (int32_t)width; p->height = (int32_t)height; p->stride_px = (int32_t)(stride / 2);
	p->col_w = COL_WIDTH; p->col_h = COL_HEIGHT; p->out_w = OUT_WIDTH; p->out_h = OUT_HEIGHT;
	p->gain0 = (int32_t)gain_q12[0]; p->gain2 = (int32_t)gain_q12[2];
	p->ccm_active = ccm_active; p->saturation_q12 = saturation_q12;
	for (int i = 0; i < 9; i++)
		p->ccm[i] = ccm_q12[i];
	for (int i = 0; i < 3; i++)
		p->nir_keep[i] = nir_keep_q12[i];
	p->ir_visible_q12 = ir_visible_q12;
	p->has_ir = cfa_count[CFA_IR] != 0;
	p->highlight_start = (int32_t)highlight_start_lin;
	p->highlight_full = (int32_t)highlight_full_lin;
	p->lsc_active = lsc_active;
	p->chroma_guard = chroma_guard;
	p->wb_nir_ratio = (float)wb_nir_ratio;
	p->nir_free = NIR_CHROMA_FREE; p->nir_half = NIR_CHROMA_HALF;
	p->hf_start = HF_CHROMA_START; p->hf_full = HF_CHROMA_FULL; p->hf_min = HF_CHROMA_MIN;
	p->radius = radius;
	p->edges = radius > 0 && gain > HIGH_GAIN_X;
	p->edge_desaturation = EDGE_DESATURATION;
	p->detail_q12 = detail_q12;
	p->tnr_threshold = tnr_threshold_for_gain(gain);
	p->tnr_have_previous = tnr_have_previous;
	p->rotate180 = output_rotate180;
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			p->cfa[y * 4 + x] = cfa[y][x];
	for (int c = 0; c < 4; c++)
		p->cfa_count[c] = (int32_t)cfa_count[c];
	p->crop_x = (int32_t)crop_x; p->crop_y = (int32_t)crop_y;
	p->crop_w = (int32_t)crop_w; p->crop_h = (int32_t)crop_h;

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkResetCommandBuffer(gpu.cmd, 0);
	if ((r = vkBeginCommandBuffer(gpu.cmd, &bi)))
		return gpu_fail("vkBeginCommandBuffer", r);
	vkCmdBindDescriptorSets(gpu.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &gpu.set, 0, NULL);
	gpu_dispatch(GP_GRID, COL_WIDTH, COL_HEIGHT, 1, 0);
	gpu_barrier();
	if (radius > 0) {
		gpu_dispatch(GP_BOX, COL_WIDTH, COL_HEIGHT, 3, 0);
		gpu_barrier();
		gpu_dispatch(GP_BOX, COL_WIDTH, COL_HEIGHT, 3, 1);
		gpu_barrier();
	}
	gpu_dispatch(GP_MIX, COL_WIDTH, COL_HEIGHT, 1, 0);
	gpu_barrier();
	gpu_dispatch(GP_OUT, OUT_WIDTH / 2, OUT_HEIGHT, 1, 0);
	VkMemoryBarrier host = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vkCmdPipelineBarrier(gpu.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0, 1, &host, 0, NULL, 0, NULL);
	if ((r = vkEndCommandBuffer(gpu.cmd)))
		return gpu_fail("vkEndCommandBuffer", r);
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &gpu.cmd };
	vkResetFences(gpu.device, 1, &gpu.fence);
	if ((r = vkQueueSubmit(gpu.queue, 1, &si, gpu.fence)))
		return gpu_fail("vkQueueSubmit", r);
	if ((r = vkWaitForFences(gpu.device, 1, &gpu.fence, VK_TRUE, 2000000000ull)))
		return gpu_fail("vkWaitForFences", r);
	memcpy(out, gpu.buf[GB_OUT].map, OUT_FRAME_BYTES);
	gpu.ms_total += (monotonic_s() - t0) * 1000.0;
	if (++gpu.frames % 300 == 0) {
		printf("gpu: %.1f ms per frame on %s (frame copy in and out, wait included)\n",
		       gpu.ms_total / 300, gpu.name);
		gpu.ms_total = 0;
	}
	return 0;
}
