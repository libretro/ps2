#include <cstring>
#include <cassert>
#include <vector>

#define VK_NO_PROTOTYPES
#include <libretro_vulkan.h>

static retro_hw_render_interface_vulkan *vulkan;
extern retro_log_printf_t log_cb;

static struct {
	VkInstance instance;
	VkPhysicalDevice gpu;
	const char **required_device_extensions;
	unsigned num_required_device_extensions;
	const char **required_device_layers;
	unsigned num_required_device_layers;
	const VkPhysicalDeviceFeatures *required_features;
} vk_init_info;

extern "C"
{
extern PFN_vkCreateInstance pcsx2_vkCreateInstance;
extern PFN_vkDestroyInstance pcsx2_vkDestroyInstance;

#define LIBRETRO_VK_WARP_LIST()                                      \
	LIBRETRO_VK_WARP_FUNC(vkGetDeviceProcAddr);                       \
	LIBRETRO_VK_WARP_FUNC(vkCreateDevice);                            \
	LIBRETRO_VK_WARP_FUNC(vkDestroyDevice);                           \
	LIBRETRO_VK_WARP_FUNC(vkQueueSubmit);                             \
	LIBRETRO_VK_WARP_FUNC(vkQueueWaitIdle);

#define LIBRETRO_VK_WARP_FUNC(x)                                     \
	extern PFN_##x pcsx2_##x;                                                 \
	PFN_##x x##_org

LIBRETRO_VK_WARP_FUNC(vkGetInstanceProcAddr);
LIBRETRO_VK_WARP_LIST();
}

static VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance_libretro(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
	*pInstance = vk_init_info.instance;
	return VK_SUCCESS;
}

static void add_name_unique(std::vector<const char *> &list, const char *value) {
	for (const char *name : list)
		if (!strcmp(value, name))
			return;

	list.push_back(value);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice_libretro(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
	VkDeviceCreateInfo info = *pCreateInfo;
	std::vector<const char *> EnabledLayerNames(info.ppEnabledLayerNames, info.ppEnabledLayerNames + info.enabledLayerCount);
	std::vector<const char *> EnabledExtensionNames(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
	VkPhysicalDeviceFeatures EnabledFeatures = *info.pEnabledFeatures;

	for (unsigned i = 0; i < vk_init_info.num_required_device_layers; i++)
		add_name_unique(EnabledLayerNames, vk_init_info.required_device_layers[i]);

	for (unsigned i = 0; i < vk_init_info.num_required_device_extensions; i++)
		add_name_unique(EnabledExtensionNames, vk_init_info.required_device_extensions[i]);

	add_name_unique(EnabledExtensionNames, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	for (unsigned i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++) {
		if (((VkBool32 *)vk_init_info.required_features)[i])
			((VkBool32 *)&EnabledFeatures)[i] = VK_TRUE;
	}

	info.enabledLayerCount = (uint32_t)EnabledLayerNames.size();
	info.ppEnabledLayerNames = info.enabledLayerCount ? EnabledLayerNames.data() : nullptr;
	info.enabledExtensionCount = (uint32_t)EnabledExtensionNames.size();
	info.ppEnabledExtensionNames = info.enabledExtensionCount ? EnabledExtensionNames.data() : nullptr;
	info.pEnabledFeatures = &EnabledFeatures;

	return vkCreateDevice_org(physicalDevice, &info, pAllocator, pDevice);
}

static VKAPI_ATTR void VKAPI_CALL vkDestroyInstance_libretro(VkInstance instance, const VkAllocationCallbacks *pAllocator) {}
static VKAPI_ATTR void VKAPI_CALL vkDestroyDevice_libretro(VkDevice device, const VkAllocationCallbacks *pAllocator) {}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit_libretro(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
	vulkan->lock_queue(vulkan->handle);
	VkResult res = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
	vulkan->unlock_queue(vulkan->handle);
	return res;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle_libretro(VkQueue queue) {
	vulkan->lock_queue(vulkan->handle);
	VkResult res = vkQueueWaitIdle_org(queue);
	vulkan->unlock_queue(vulkan->handle);
	return res;
}

#undef LIBRETRO_VK_WARP_FUNC
#define LIBRETRO_VK_WARP_FUNC(x)                    \
	if (!strcmp(pName, #x)) {                     \
		x##_org = (PFN_##x)fptr;                   \
		return (PFN_vkVoidFunction)x##_libretro;   \
	}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr_libretro(VkDevice device, const char *pName) {
	PFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);
	if (!fptr)
		return fptr;

	LIBRETRO_VK_WARP_LIST();

	return fptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr_libretro(VkInstance instance, const char *pName) {
	PFN_vkVoidFunction fptr = vkGetInstanceProcAddr_org(instance, pName);
	if (!fptr)
		return fptr;

	LIBRETRO_VK_WARP_LIST();

	return fptr;
}

void vk_libretro_init_wraps(void)
{
	vkGetInstanceProcAddr_org = pcsx2_vkGetInstanceProcAddr;
	pcsx2_vkGetInstanceProcAddr = vkGetInstanceProcAddr_libretro;
	pcsx2_vkCreateInstance = vkCreateInstance_libretro;
	pcsx2_vkDestroyInstance = vkDestroyInstance_libretro;
}

void vk_libretro_init(VkInstance instance, VkPhysicalDevice gpu, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features) {
	vk_init_info.instance = instance;
	vk_init_info.gpu = gpu;
	vk_init_info.required_device_extensions = required_device_extensions;
	vk_init_info.num_required_device_extensions = num_required_device_extensions;
	vk_init_info.required_device_layers = required_device_layers;
	vk_init_info.num_required_device_layers = num_required_device_layers;
	vk_init_info.required_features = required_features;
}


void vk_libretro_set_hwrender_interface(retro_hw_render_interface_vulkan *hw_render_interface) {
   vulkan = (retro_hw_render_interface_vulkan *)hw_render_interface;
}

void vk_libretro_shutdown() {
	memset(&vk_init_info, 0, sizeof(vk_init_info));
	vulkan = nullptr;
}
