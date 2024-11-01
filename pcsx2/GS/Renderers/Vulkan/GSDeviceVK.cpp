/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstring>
#include <cassert>
#include <vector>

#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "GS.h"
#include "ShaderCacheVersion.h"

#include "common/Align.h"
#include "common/Console.h"
#include "common/General.h"
#include "common/StringUtil.h"
#include "VKBuilders.h"
#include "VKShaderCache.h"

#include <sstream>
#include <limits>
#include <algorithm>
#include <array>
#include <cstring>

#include <libretro.h>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <time.h>
#endif

extern retro_video_refresh_t video_cb;

#define VK_NO_PROTOTYPES
#include <libretro_vulkan.h>

#include "convert.glsl"
#include "interlace.glsl"
#include "merge.glsl"
#include "tfx.glsl"

static retro_hw_render_interface_vulkan *vulkan;
extern retro_log_printf_t log_cb;

struct vk_init_info_t  vk_init_info;

extern "C"
{
	extern PFN_vkGetInstanceProcAddr pcsx2_vkGetInstanceProcAddr;
	extern PFN_vkGetDeviceProcAddr   pcsx2_vkGetDeviceProcAddr;
	extern PFN_vkCreateDevice        pcsx2_vkCreateDevice;
	PFN_vkGetInstanceProcAddr        vkGetInstanceProcAddr_org;
	PFN_vkGetDeviceProcAddr          vkGetDeviceProcAddr_org;
	PFN_vkCreateDevice               vkCreateDevice_org;
	PFN_vkQueueSubmit                vkQueueSubmit_org;
}

bool create_device_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features)
{
	vk_init_info.instance                       = instance;
	vk_init_info.gpu                            = gpu;
	vk_init_info.required_device_extensions     = required_device_extensions;
	vk_init_info.num_required_device_extensions = num_required_device_extensions;
	vk_init_info.required_device_layers         = required_device_layers;
	vk_init_info.num_required_device_layers     = num_required_device_layers;
	vk_init_info.required_features              = required_features;
	vk_init_info.vkGetInstanceProcAddr          = get_instance_proc_addr;

	if(gpu != VK_NULL_HANDLE) {
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties    = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
		vkGetPhysicalDeviceProperties(gpu, &props);
		GSConfig.Adapter = props.deviceName;
	}

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	context->gpu                             = vk_init_info.gpu;
	context->device                          = vk_init_info.device;
	context->queue                           = GSDeviceVK::GetInstance()->GetGraphicsQueue();
	context->queue_family_index              = GSDeviceVK::GetInstance()->GetGraphicsQueueFamilyIndex();
	context->presentation_queue              = context->queue;
	context->presentation_queue_family_index = context->queue_family_index;

	return true;
}

const VkApplicationInfo *get_application_info_vulkan(void)
{
	static VkApplicationInfo app_info{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName   = "PCSX2";
	app_info.applicationVersion = VK_MAKE_VERSION(1, 7, 0);
	app_info.pEngineName        = "PCSX2";
	app_info.engineVersion      = VK_MAKE_VERSION(1, 7, 0);
	app_info.apiVersion         = VK_API_VERSION_1_1;
	return &app_info;
}

static void add_name_unique(std::vector<const char *> &list, const char *value) {
	for (const char *name : list)
		if (!strcmp(value, name))
			return;

	list.push_back(value);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice_libretro(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	VkDeviceCreateInfo info = *pCreateInfo;
	std::vector<const char *> EnabledLayerNames(info.ppEnabledLayerNames, info.ppEnabledLayerNames + info.enabledLayerCount);
	std::vector<const char *> EnabledExtensionNames(info.ppEnabledExtensionNames, info.ppEnabledExtensionNames + info.enabledExtensionCount);
	VkPhysicalDeviceFeatures EnabledFeatures = *info.pEnabledFeatures;

	for (unsigned i = 0; i < vk_init_info.num_required_device_layers; i++)
		add_name_unique(EnabledLayerNames, vk_init_info.required_device_layers[i]);

	for (unsigned i = 0; i < vk_init_info.num_required_device_extensions; i++)
		add_name_unique(EnabledExtensionNames, vk_init_info.required_device_extensions[i]);

	add_name_unique(EnabledExtensionNames, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	for (unsigned i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); i++)
	{
		if (((VkBool32 *)vk_init_info.required_features)[i])
			((VkBool32 *)&EnabledFeatures)[i] = VK_TRUE;
	}

	info.enabledLayerCount       = (uint32_t)EnabledLayerNames.size();
	info.ppEnabledLayerNames     = info.enabledLayerCount ? EnabledLayerNames.data() : nullptr;
	info.enabledExtensionCount   = (uint32_t)EnabledExtensionNames.size();
	info.ppEnabledExtensionNames = info.enabledExtensionCount ? EnabledExtensionNames.data() : nullptr;
	info.pEnabledFeatures        = &EnabledFeatures;

	return vkCreateDevice_org(physicalDevice, &info, pAllocator, pDevice);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit_libretro(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	vulkan->lock_queue(vulkan->handle);
	VkResult res = vkQueueSubmit_org(queue, submitCount, pSubmits, fence);
	vulkan->unlock_queue(vulkan->handle);
	return res;
}

/* Forward declaration */
static VKAPI_ATTR PFN_vkVoidFunction enumerate_fptrs(PFN_vkVoidFunction fptr, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr_libretro(VkDevice device, const char *pName)
{
	PFN_vkVoidFunction fptr = vkGetDeviceProcAddr_org(device, pName);
	if (!fptr)
		return fptr;
	return enumerate_fptrs(fptr, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr_libretro(VkInstance instance, const char *pName)
{
	PFN_vkVoidFunction fptr = vkGetInstanceProcAddr_org(instance, pName);
	if (!fptr)
		return fptr;
	return enumerate_fptrs(fptr, pName);
}

static VKAPI_ATTR PFN_vkVoidFunction enumerate_fptrs(PFN_vkVoidFunction fptr, const char *pName)
{
	if (!strcmp(pName, "vkGetDeviceProcAddr"))
	{
		vkGetDeviceProcAddr_org = (PFN_vkGetDeviceProcAddr)fptr;
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr_libretro;
	}
	if (!strcmp(pName, "vkCreateDevice"))
	{
		vkCreateDevice_org = (PFN_vkCreateDevice)fptr;
		return (PFN_vkVoidFunction)vkCreateDevice_libretro;
	}
	if (!strcmp(pName, "vkQueueSubmit"))
	{
		vkQueueSubmit_org = (PFN_vkQueueSubmit)fptr;
		return (PFN_vkVoidFunction)vkQueueSubmit_libretro;
	}

	return fptr;
}

void vk_libretro_init_wraps(void)
{
	vkGetInstanceProcAddr_org   = pcsx2_vkGetInstanceProcAddr;
	pcsx2_vkGetInstanceProcAddr = vkGetInstanceProcAddr_libretro;
}

void vk_libretro_set_hwrender_interface(retro_hw_render_interface_vulkan *hw_render_interface)
{
   vulkan = (retro_hw_render_interface_vulkan *)hw_render_interface;
}

void vk_libretro_shutdown(void)
{
	memset(&vk_init_info, 0, sizeof(vk_init_info));
	vulkan = nullptr;
}

// Tweakables
enum : u32
{
	MAX_DRAW_CALLS_PER_FRAME = 8192,
	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME = 2 * MAX_DRAW_CALLS_PER_FRAME,
	MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME, // assume at least half our draws aren't going to be shuffle/blending
	MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME = 4, // Currently used by CAS only
	MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
	MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME * 2
};

static void SafeDestroyShaderModule(VkDevice dev, VkShaderModule& sm)
{
	if (sm != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(dev, sm, nullptr);
		sm = VK_NULL_HANDLE;
	}
}

static void SafeDestroyPipelineLayout(VkDevice dev, VkPipelineLayout& pl)
{
	if (pl != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(dev, pl, nullptr);
		pl = VK_NULL_HANDLE;
	}
}

static void SafeDestroyDescriptorSetLayout(VkDevice dev, VkDescriptorSetLayout& dsl)
{
	if (dsl != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
		dsl = VK_NULL_HANDLE;
	}
}

	GSDeviceVK::GPUNameList GSDeviceVK::EnumerateGPUNames(VkInstance instance)
	{
		u32 gpu_count = 0;
		VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
		if (res != VK_SUCCESS || gpu_count == 0)
			return {};

		GPUList gpus;
		gpus.resize(gpu_count);

		res = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
		if (res != VK_SUCCESS)
			return {};

		GPUNameList gpu_names;
		gpu_names.reserve(gpu_count);
		for (u32 i = 0; i < gpu_count; i++)
		{
			VkPhysicalDeviceProperties props = {};
			vkGetPhysicalDeviceProperties(gpus[i], &props);

			std::string gpu_name(props.deviceName);

			// handle duplicate adapter names
			if (std::any_of(gpu_names.begin(), gpu_names.end(),
					[&gpu_name](const std::string& other) { return (gpu_name == other); }))
			{
				std::string original_adapter_name = std::move(gpu_name);

				u32 current_extra = 2;
				do
				{
					gpu_name = StringUtil::StdStringFromFormat("%s (%u)", original_adapter_name.c_str(), current_extra);
					current_extra++;
				} while (std::any_of(gpu_names.begin(), gpu_names.end(),
					[&gpu_name](const std::string& other) { return (gpu_name == other); }));
			}

			gpu_names.push_back(std::move(gpu_name));
		}

		return gpu_names;
	}

	bool GSDeviceVK::SelectDeviceExtensions(ExtensionList* extension_list)
	{
		u32 extension_count = 0;
		VkResult res = vkEnumerateDeviceExtensionProperties(vk_init_info.gpu, nullptr, &extension_count, nullptr);
		if (res != VK_SUCCESS)
			return false;

		if (extension_count == 0)
		{
			Console.Error("Vulkan: No extensions supported by device.");
			return false;
		}

		std::vector<VkExtensionProperties> available_extension_list(extension_count);
		res = vkEnumerateDeviceExtensionProperties(
			vk_init_info.gpu, nullptr, &extension_count, available_extension_list.data());

		auto SupportsExtension = [&available_extension_list, extension_list](const char* name, bool required) {
			if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
					[name](const VkExtensionProperties& properties) { return !strcmp(name, properties.extensionName); }) !=
				available_extension_list.end())
			{
				if (std::none_of(extension_list->begin(), extension_list->end(),
						[name](const char* existing_name) { return (std::strcmp(existing_name, name) == 0); }))
				{
					Console.WriteLn("Enabling extension: %s", name);
					extension_list->push_back(name);
				}

				return true;
			}

			if (required)
				Console.Error("Vulkan: Missing required extension %s.", name);

			return false;
		};

		// Required extensions.
		if (!SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, true))
		{
			Console.WriteLn("Does not support VK_KHR_push_descriptor extension");
			//return false;
		}

		m_optional_extensions.vk_ext_provoking_vertex =
			SupportsExtension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_memory_budget =
			SupportsExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_line_rasterization =
			SupportsExtension(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, false);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access =
			SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false) ||
			SupportsExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_driver_properties =
			SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_fragment_shader_barycentric =
			SupportsExtension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, false);
		m_optional_extensions.vk_khr_shader_draw_parameters =
			SupportsExtension(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, false);

		return true;
	}

	void GSDeviceVK::SelectDeviceFeatures(const VkPhysicalDeviceFeatures* required_features)
	{
		VkPhysicalDeviceFeatures available_features;
		vkGetPhysicalDeviceFeatures(vk_init_info.gpu, &available_features);

		if (required_features)
			memcpy(&m_device_features, required_features, sizeof(m_device_features));

		// Enable the features we use.
		m_device_features.dualSrcBlend             = available_features.dualSrcBlend;
		m_device_features.largePoints              = available_features.largePoints;
		m_device_features.wideLines                = available_features.wideLines;
		m_device_features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
		m_device_features.textureCompressionBC     = available_features.textureCompressionBC;
		m_device_features.samplerAnisotropy        = available_features.samplerAnisotropy;
		m_device_features.geometryShader           = available_features.geometryShader;
	}

	bool GSDeviceVK::CreateDevice(
		const char** required_device_extensions, u32 num_required_device_extensions,
		const char** required_device_layers, u32 num_required_device_layers,
		const VkPhysicalDeviceFeatures* required_features)
	{
		u32 queue_family_count;
		vkGetPhysicalDeviceQueueFamilyProperties(vk_init_info.gpu, &queue_family_count, nullptr);
		if (queue_family_count == 0)
		{
			Console.Error("No queue families found on specified vulkan physical device.");
			return false;
		}

		std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(
			vk_init_info.gpu, &queue_family_count, queue_family_properties.data());
		Console.WriteLn("%u vulkan queue families", queue_family_count);

		// Find graphics and present queues.
		m_graphics_queue_family_index = queue_family_count;

		for (uint32_t i = 0; i < queue_family_count; i++)
		{
			VkBool32 graphics_supported = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
			if (graphics_supported)
			{
				m_graphics_queue_family_index = i;
				// Quit now, no need for a present queue.
				break;
			}
		}

		if (m_graphics_queue_family_index == queue_family_count)
		{
			Console.Error("Vulkan: Failed to find an acceptable graphics queue.");
			return false;
		}

		VkDeviceCreateInfo device_info   = {};
		device_info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		device_info.pNext                = nullptr;
		device_info.flags                = 0;
		device_info.queueCreateInfoCount = 0;

		static constexpr float queue_priorities[] = {1.0f, 0.0f}; // Low priority for the spin queue
		std::array<VkDeviceQueueCreateInfo, 2> queue_infos;

		VkDeviceQueueCreateInfo& graphics_queue_info = queue_infos[device_info.queueCreateInfoCount++];
		graphics_queue_info.sType                    = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		graphics_queue_info.pNext                    = nullptr;
		graphics_queue_info.flags                    = 0;
		graphics_queue_info.queueFamilyIndex         = m_graphics_queue_family_index;
		graphics_queue_info.queueCount               = 1;
		graphics_queue_info.pQueuePriorities         = queue_priorities;

		device_info.pQueueCreateInfos                = queue_infos.data();

		ExtensionList enabled_extensions;
		for (u32 i = 0; i < num_required_device_extensions; i++)
			enabled_extensions.emplace_back(required_device_extensions[i]);
		if (!SelectDeviceExtensions(&enabled_extensions))
			return false;

		device_info.enabledLayerCount       = num_required_device_layers;
		device_info.ppEnabledLayerNames     = required_device_layers;
		device_info.enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size());
		device_info.ppEnabledExtensionNames = enabled_extensions.data();

		// Check for required features before creating.
		SelectDeviceFeatures(required_features);

		device_info.pEnabledFeatures = &m_device_features;

		// provoking vertex
		VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};

		if (m_optional_extensions.vk_ext_provoking_vertex)
		{
			provoking_vertex_feature.provokingVertexLast = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &provoking_vertex_feature);
		}
		if (m_optional_extensions.vk_ext_line_rasterization)
		{
			line_rasterization_feature.bresenhamLines = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &line_rasterization_feature);
		}
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
		{
			rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &rasterization_order_access_feature);
		}

		VkResult res = vkCreateDevice(vk_init_info.gpu, &device_info, nullptr, &vk_init_info.device);
		if (res != VK_SUCCESS)
			return false;

		// With the device created, we can fill the remaining entry points.
		if (!Vulkan::LoadVulkanDeviceFunctions(vk_init_info.device))
			return false;

		// Grab the graphics and present queues.
		vkGetDeviceQueue(vk_init_info.device, m_graphics_queue_family_index, 0, &m_graphics_queue);

		if (!(ProcessDeviceExtensions()))
			return false;

		return true;
	}

	bool GSDeviceVK::ProcessDeviceExtensions()
	{
		// advanced feature checks
		VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex_features = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
		VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT};
		VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};

		// add in optional feature structs
		if (m_optional_extensions.vk_ext_provoking_vertex)
			Vulkan::AddPointerToChain(&features2, &provoking_vertex_features);
		if (m_optional_extensions.vk_ext_line_rasterization)
			Vulkan::AddPointerToChain(&features2, &line_rasterization_feature);
		if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
			Vulkan::AddPointerToChain(&features2, &rasterization_order_access_feature);

		// query
		vkGetPhysicalDeviceFeatures2(vk_init_info.gpu, &features2);

		// confirm we actually support it
		m_optional_extensions.vk_ext_provoking_vertex &= (provoking_vertex_features.provokingVertexLast == VK_TRUE);
		m_optional_extensions.vk_ext_rasterization_order_attachment_access &= (rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
		m_optional_extensions.vk_ext_line_rasterization &= (line_rasterization_feature.bresenhamLines == VK_TRUE);

		VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

		if (m_optional_extensions.vk_khr_driver_properties)
		{
			m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
			Vulkan::AddPointerToChain(&properties2, &m_device_driver_properties);
		}

		VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
		Vulkan::AddPointerToChain(&properties2, &push_descriptor_properties);

		// query
		vkGetPhysicalDeviceProperties2(vk_init_info.gpu, &properties2);

		// confirm we actually support push descriptor extension
		if (push_descriptor_properties.maxPushDescriptors < 4 /*NUM_TFX_TEXTURES */)
		{
			Console.Error("maxPushDescriptors (%u) is below required (%u)", push_descriptor_properties.maxPushDescriptors,
					NUM_TFX_TEXTURES);
			Console.WriteLn("VK_KHR_push_descriptor is NOT supported");
			//return false;
		}
		else
		{
			Console.WriteLn("VK_KHR_push_descriptor is supported");
		}


		Console.WriteLn("VK_EXT_provoking_vertex is %s",
			m_optional_extensions.vk_ext_provoking_vertex ? "supported" : "NOT supported");
		Console.WriteLn("VK_EXT_line_rasterization is %s",
			m_optional_extensions.vk_ext_line_rasterization ? "supported" : "NOT supported");
		Console.WriteLn("VK_EXT_rasterization_order_attachment_access is %s",
			m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");

		return true;
	}

	bool GSDeviceVK::CreateAllocator()
	{
		VmaAllocatorCreateInfo ci = {};
		ci.vulkanApiVersion       = VK_API_VERSION_1_1;
		ci.flags                  = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
		ci.physicalDevice         = vk_init_info.gpu;
		ci.device                 = vk_init_info.device;
		ci.instance               = vk_init_info.instance;

		if (m_optional_extensions.vk_ext_memory_budget)
			ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

		VkResult res = vmaCreateAllocator(&ci, &m_allocator);
		if (res != VK_SUCCESS)
			return false;

		return true;
	}

	bool GSDeviceVK::CreateCommandBuffers()
	{
		VkResult res;
		uint32_t frame_index = 0;
		VkDevice m_device    = vk_init_info.device;

		for (FrameResources& resources : m_frame_resources)
		{
			VkCommandPoolCreateInfo pool_info = {
				VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, m_graphics_queue_family_index};
			res = vkCreateCommandPool(m_device, &pool_info, nullptr, &resources.command_pool);
			if (res != VK_SUCCESS)
				return false;

			VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
				resources.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				static_cast<u32>(resources.command_buffers.size())};

			res = vkAllocateCommandBuffers(m_device, &buffer_info, resources.command_buffers.data());
			if (res != VK_SUCCESS)
				return false;

			VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

			res = vkCreateFence(m_device, &fence_info, nullptr, &resources.fence);
			if (res != VK_SUCCESS)
				return false;
			// TODO: A better way to choose the number of descriptors.
			VkDescriptorPoolSize pool_sizes[] = {
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 		MAX_SAMPLED_IMAGE_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		MAX_STORAGE_IMAGE_DESCRIPTORS_PER_FRAME},
				{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,		MAX_INPUT_ATTACHMENT_IMAGE_DESCRIPTORS_PER_FRAME},
			};

			VkDescriptorPoolCreateInfo pool_create_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0,
				MAX_DESCRIPTOR_SETS_PER_FRAME, static_cast<u32>(std::size(pool_sizes)), pool_sizes};

			res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &resources.descriptor_pool);
			if (res != VK_SUCCESS)
				return false;

			++frame_index;
		}

		ActivateCommandBuffer(0);
		return true;
	}

	void GSDeviceVK::DestroyCommandBuffers()
	{
		VkDevice m_device    = vk_init_info.device;
		for (FrameResources& resources : m_frame_resources)
		{
			for (auto& it : resources.cleanup_resources)
				it();
			resources.cleanup_resources.clear();

			if (resources.fence != VK_NULL_HANDLE)
			{
				vkDestroyFence(m_device, resources.fence, nullptr);
				resources.fence = VK_NULL_HANDLE;
			}
			if (resources.descriptor_pool != VK_NULL_HANDLE)
			{
				vkDestroyDescriptorPool(m_device, resources.descriptor_pool, nullptr);
				resources.descriptor_pool = VK_NULL_HANDLE;
			}
			if (resources.command_buffers[0] != VK_NULL_HANDLE)
			{
				vkFreeCommandBuffers(m_device, resources.command_pool,
					static_cast<u32>(resources.command_buffers.size()), resources.command_buffers.data());
				resources.command_buffers.fill(VK_NULL_HANDLE);
			}
			if (resources.command_pool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(m_device, resources.command_pool, nullptr);
				resources.command_pool = VK_NULL_HANDLE;
			}
		}
	}

	bool GSDeviceVK::CreateGlobalDescriptorPool()
	{
		static constexpr const VkDescriptorPoolSize pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
		};

		VkDescriptorPoolCreateInfo pool_create_info =  {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
								nullptr,
								VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
								1024, // TODO: tweak this
								static_cast<u32>(std::size(pool_sizes)), pool_sizes};

		VkResult res = vkCreateDescriptorPool(vk_init_info.device, &pool_create_info, nullptr, &m_global_descriptor_pool);
		if (res != VK_SUCCESS)
			return false;

		return true;
	}

	bool GSDeviceVK::CreateTextureStreamBuffer()
	{
		if (!m_texture_upload_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
		{
			Console.Error("Failed to allocate texture upload buffer");
			return false;
		}

		return true;
	}

	VkRenderPass GSDeviceVK::GetRenderPass(
			VkFormat color_format,
			VkFormat depth_format,
			VkAttachmentLoadOp color_load_op,
			VkAttachmentStoreOp color_store_op,
			VkAttachmentLoadOp depth_load_op,
			VkAttachmentStoreOp depth_store_op,
			VkAttachmentLoadOp stencil_load_op,
			VkAttachmentStoreOp stencil_store_op,
			bool color_feedback_loop,
			bool depth_sampling)
	{
		RenderPassCacheKey key = {};
		key.color_format = color_format;
		key.depth_format = depth_format;
		key.color_load_op = color_load_op;
		key.color_store_op = color_store_op;
		key.depth_load_op = depth_load_op;
		key.depth_store_op = depth_store_op;
		key.stencil_load_op = stencil_load_op;
		key.stencil_store_op = stencil_store_op;
		key.color_feedback_loop = color_feedback_loop;
		key.depth_sampling = depth_sampling;

		auto it = m_render_pass_cache.find(key.key);
		if (it != m_render_pass_cache.end())
			return it->second;

		return CreateCachedRenderPass(key);
	}

	VkRenderPass GSDeviceVK::GetRenderPassForRestarting(VkRenderPass pass)
	{
		for (const auto& it : m_render_pass_cache)
		{
			if (it.second != pass)
				continue;

			RenderPassCacheKey modified_key;
			modified_key.key = it.first;
			if (modified_key.color_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				modified_key.color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			if (modified_key.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				modified_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			if (modified_key.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				modified_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

			if (modified_key.key == it.first)
				return pass;

			auto fit = m_render_pass_cache.find(modified_key.key);
			if (fit != m_render_pass_cache.end())
				return fit->second;

			return CreateCachedRenderPass(modified_key);
		}

		return pass;
	}

	VkCommandBuffer GSDeviceVK::GetCurrentInitCommandBuffer()
	{
		FrameResources& res = m_frame_resources[m_current_frame];
		VkCommandBuffer buf = res.command_buffers[0];
		if (res.init_buffer_used)
			return buf;

		VkCommandBufferBeginInfo bi{
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkBeginCommandBuffer(buf, &bi);
		res.init_buffer_used = true;
		return buf;
	}

	VkDescriptorSet GSDeviceVK::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
	{
		VkDescriptorSetAllocateInfo allocate_info = 	{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
								 nullptr,
								 m_frame_resources[m_current_frame].descriptor_pool,
								 1,
								 &set_layout};

		VkDescriptorSet descriptor_set;
		VkResult res = vkAllocateDescriptorSets(vk_init_info.device, &allocate_info, &descriptor_set);
		// Failing to allocate a descriptor set is not a fatal error, we can
		// recover by moving to the next command buffer.
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return descriptor_set;
	}

	VkDescriptorSet GSDeviceVK::AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout)
	{
		VkDescriptorSetAllocateInfo allocate_info = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_global_descriptor_pool, 1, &set_layout};

		VkDescriptorSet descriptor_set;
		VkResult res = vkAllocateDescriptorSets(vk_init_info.device, &allocate_info, &descriptor_set);
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;

		return descriptor_set;
	}

	void GSDeviceVK::FreeGlobalDescriptorSet(VkDescriptorSet set)
	{
		vkFreeDescriptorSets(vk_init_info.device, m_global_descriptor_pool, 1, &set);
	}

	void GSDeviceVK::WaitForFenceCounter(u64 fence_counter)
	{
		if (m_completed_fence_counter >= fence_counter)
			return;

		// Find the first command buffer which covers this counter value.
		u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		while (index != m_current_frame)
		{
			if (m_frame_resources[index].fence_counter >= fence_counter)
				break;

			index = (index + 1) % NUM_COMMAND_BUFFERS;
		}

		WaitForCommandBufferCompletion(index);
	}

	void GSDeviceVK::WaitForGPUIdle()
	{
		vkDeviceWaitIdle(vk_init_info.device);
	}

	void GSDeviceVK::WaitForCommandBufferCompletion(u32 index)
	{
		// Wait for this command buffer to be completed.
		const VkResult res = vkWaitForFences(vk_init_info.device, 1, &m_frame_resources[index].fence, VK_TRUE, UINT64_MAX);
		if (res != VK_SUCCESS)
		{
			m_last_submit_failed = true;
			return;
		}

		// Clean up any resources for command buffers between the last known completed buffer and this
		// now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
		const u64 now_completed_counter = m_frame_resources[index].fence_counter;
		u32 cleanup_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
		while (cleanup_index != m_current_frame)
		{
			FrameResources& resources = m_frame_resources[cleanup_index];
			if (resources.fence_counter > now_completed_counter)
				break;

			if (resources.fence_counter > m_completed_fence_counter)
				CommandBufferCompleted(cleanup_index);

			cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
		}

		m_completed_fence_counter = now_completed_counter;
	}

	void GSDeviceVK::SubmitCommandBuffer()
	{
		FrameResources& resources = m_frame_resources[m_current_frame];

		// End the current command buffer.
		VkResult res;
		if (resources.init_buffer_used)
		{
			res = vkEndCommandBuffer(resources.command_buffers[0]);
			if (res != VK_SUCCESS)
				Console.Error("Failed to end command buffer");
		}

		res = vkEndCommandBuffer(resources.command_buffers[1]);
		if (res != VK_SUCCESS)
			Console.Error("Failed to end command buffer");

		// This command buffer now has commands, so can't be re-used without waiting.
		VkSubmitInfo submit_info       = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit_info.commandBufferCount = resources.init_buffer_used ? 2u : 1u;
		submit_info.pCommandBuffers    = resources.init_buffer_used ? resources.command_buffers.data() : &resources.command_buffers[1];

		res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
		if (res != VK_SUCCESS)
			m_last_submit_failed = true;
	}

	void GSDeviceVK::CommandBufferCompleted(u32 index)
	{
		FrameResources& resources = m_frame_resources[index];

		for (auto& it : resources.cleanup_resources)
			it();
		resources.cleanup_resources.clear();
	}

	void GSDeviceVK::MoveToNextCommandBuffer()
	{
		ActivateCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
		InvalidateCachedState();
		SetInitialState(m_current_command_buffer);
	}

	void GSDeviceVK::ActivateCommandBuffer(u32 index)
	{
		VkDevice m_device         = vk_init_info.device;
		FrameResources& resources = m_frame_resources[index];

		// Wait for the GPU to finish with all resources for this command buffer.
		if (resources.fence_counter > m_completed_fence_counter)
			WaitForCommandBufferCompletion(index);

		// Reset fence to unsignaled before starting.
		vkResetFences(m_device, 1, &resources.fence);
		// Reset command pools to beginning since we can re-use the memory now
		vkResetCommandPool(m_device, resources.command_pool, 0);

		// Enable commands to be recorded to the two buffers again.
		VkCommandBufferBeginInfo begin_info = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkBeginCommandBuffer(resources.command_buffers[1], &begin_info);

		// Also can do the same for the descriptor pools
		vkResetDescriptorPool(m_device, resources.descriptor_pool, 0);

		resources.fence_counter = m_next_fence_counter++;
		resources.init_buffer_used = false;

		m_current_frame = index;
		m_current_command_buffer = resources.command_buffers[1];

		// using the lower 32 bits of the fence index should be sufficient here, I hope...
		vmaSetCurrentFrameIndex(m_allocator, static_cast<u32>(m_next_fence_counter));
	}

	void GSDeviceVK::DeferBufferDestruction(VkBuffer object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyBuffer(vk_init_info.device, object, nullptr); });
	}

	void GSDeviceVK::DeferBufferDestruction(VkBuffer object, VmaAllocation allocation)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back(
			[this, object, allocation]() { vmaDestroyBuffer(m_allocator, object, allocation); });
	}

	void GSDeviceVK::DeferFramebufferDestruction(VkFramebuffer object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyFramebuffer(vk_init_info.device, object, nullptr); });
	}

	void GSDeviceVK::DeferImageDestruction(VkImage object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyImage(vk_init_info.device, object, nullptr); });
	}

	void GSDeviceVK::DeferImageDestruction(VkImage object, VmaAllocation allocation)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back(
			[this, object, allocation]() { vmaDestroyImage(m_allocator, object, allocation); });
	}

	void GSDeviceVK::DeferImageViewDestruction(VkImageView object)
	{
		FrameResources& resources = m_frame_resources[m_current_frame];
		resources.cleanup_resources.push_back([this, object]() { vkDestroyImageView(vk_init_info.device, object, nullptr); });
	}

	VkRenderPass GSDeviceVK::CreateCachedRenderPass(RenderPassCacheKey key)
	{
		VkAttachmentReference color_reference;
		VkAttachmentReference* color_reference_ptr = nullptr;
		VkAttachmentReference depth_reference;
		VkAttachmentReference* depth_reference_ptr = nullptr;
		VkAttachmentReference input_reference;
		VkAttachmentReference* input_reference_ptr = nullptr;
		VkSubpassDependency subpass_dependency;
		VkSubpassDependency* subpass_dependency_ptr = nullptr;
		std::array<VkAttachmentDescription, 2> attachments;
		u32 num_attachments = 0;
		if (key.color_format != VK_FORMAT_UNDEFINED)
		{
			const VkImageLayout layout = key.color_feedback_loop ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments[num_attachments] = {0, static_cast<VkFormat>(key.color_format), VK_SAMPLE_COUNT_1_BIT,
				static_cast<VkAttachmentLoadOp>(key.color_load_op),
				static_cast<VkAttachmentStoreOp>(key.color_store_op),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
			color_reference.attachment = num_attachments;
			color_reference.layout     = layout;
			color_reference_ptr = &color_reference;

			if (key.color_feedback_loop)
			{
				input_reference.attachment = num_attachments;
				input_reference.layout     = layout;
				input_reference_ptr        = &input_reference;

				if (!m_features.framebuffer_fetch)
				{
					// don't need the framebuffer-local dependency when we have rasterization order attachment access
					subpass_dependency.srcSubpass = 0;
					subpass_dependency.dstSubpass = 0;
					subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
					subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					subpass_dependency.srcAccessMask =
						VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
					subpass_dependency.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
					subpass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
					subpass_dependency_ptr = &subpass_dependency;
				}
			}

			num_attachments++;
		}
		if (key.depth_format != VK_FORMAT_UNDEFINED)
		{
			const VkImageLayout layout = key.depth_sampling ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments[num_attachments] = {0, static_cast<VkFormat>(key.depth_format), VK_SAMPLE_COUNT_1_BIT,
				static_cast<VkAttachmentLoadOp>(key.depth_load_op),
				static_cast<VkAttachmentStoreOp>(key.depth_store_op),
				static_cast<VkAttachmentLoadOp>(key.stencil_load_op),
				static_cast<VkAttachmentStoreOp>(key.stencil_store_op),
				layout, layout};
			depth_reference.attachment = num_attachments;
			depth_reference.layout = layout;
			depth_reference_ptr = &depth_reference;
			num_attachments++;
		}

		const VkSubpassDescriptionFlags subpass_flags =
			(key.color_feedback_loop && m_optional_extensions.vk_ext_rasterization_order_attachment_access) ?
				VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT :
				0;
		const VkSubpassDescription subpass = {subpass_flags, VK_PIPELINE_BIND_POINT_GRAPHICS, input_reference_ptr ? 1u : 0u,
			input_reference_ptr ? input_reference_ptr : nullptr, color_reference_ptr ? 1u : 0u,
			color_reference_ptr ? color_reference_ptr : nullptr, nullptr, depth_reference_ptr, 0, nullptr};
		const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0u,
			num_attachments, attachments.data(), 1u, &subpass, subpass_dependency_ptr ? 1u : 0u,
			subpass_dependency_ptr};

		VkRenderPass pass;
		const VkResult res = vkCreateRenderPass(vk_init_info.device, &pass_info, nullptr, &pass);
		if (res != VK_SUCCESS)
			return VK_NULL_HANDLE;

		m_render_pass_cache.emplace(key.key, pass);
		return pass;
	}

	bool GSDeviceVK::AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, VmaAllocation* gpu_allocation,
		VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback)
	{
		// Try to place the fixed index buffer in GPU local memory.
		// Use the staging buffer to copy into it.

		const VkBufferCreateInfo cpu_bci = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0, size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
		const VmaAllocationCreateInfo cpu_aci = {
			VMA_ALLOCATION_CREATE_MAPPED_BIT, VMA_MEMORY_USAGE_CPU_ONLY, 0, 0};
		VkBuffer cpu_buffer;
		VmaAllocation cpu_allocation;
		VmaAllocationInfo cpu_ai;
		VkResult res = vmaCreateBuffer(m_allocator, &cpu_bci, &cpu_aci, &cpu_buffer,
			&cpu_allocation, &cpu_ai);
		if (res != VK_SUCCESS)
			return false;

		const VkBufferCreateInfo gpu_bci = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0, size,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
		const VmaAllocationCreateInfo gpu_aci = {
			0, VMA_MEMORY_USAGE_GPU_ONLY, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
		VmaAllocationInfo ai;
		res = vmaCreateBuffer(m_allocator, &gpu_bci, &gpu_aci, gpu_buffer, gpu_allocation, &ai);
		if (res != VK_SUCCESS)
		{
			vmaDestroyBuffer(m_allocator, cpu_buffer, cpu_allocation);
			return false;
		}

		const VkBufferCopy buf_copy = {0u, 0u, size};
		fill_callback(cpu_ai.pMappedData);
		vmaFlushAllocation(m_allocator, cpu_allocation, 0, size);
		vkCmdCopyBuffer(GetCurrentInitCommandBuffer(), cpu_buffer, *gpu_buffer, 1, &buf_copy);
		DeferBufferDestruction(cpu_buffer, cpu_allocation);
		return true;
	}

static bool IsDATMConvertShader(ShaderConvert i)
{
	return (i == ShaderConvert::DATM_0 || i == ShaderConvert::DATM_1 || i == ShaderConvert::DATM_0_RTA_CORRECTION || i == ShaderConvert::DATM_1_RTA_CORRECTION);
}

static bool IsDATEModePrimIDInit(u32 flag) { return flag == 1 || flag == 2; }

static VkAttachmentLoadOp GetLoadOpForTexture(GSTextureVK* tex)
{
	if (!tex)
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	// clang-format off
	switch (tex->GetState())
	{
	case GSTextureVK::State::Cleared:       tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case GSTextureVK::State::Invalidated:   tex->SetState(GSTexture::State::Dirty); return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	case GSTextureVK::State::Dirty:         return VK_ATTACHMENT_LOAD_OP_LOAD;
	default:                                return VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	// clang-format on
}

GSDeviceVK::GSDeviceVK()
{
	memset(&m_pipeline_selector, 0, sizeof(m_pipeline_selector));
}

GSDeviceVK::~GSDeviceVK()
{
}

void GSDeviceVK::GetAdapters(std::vector<std::string>* adapters)
{
	VkInstance instance = vk_init_info.instance;
	if (Vulkan::LoadVulkanLibrary())
	{
		if (instance != VK_NULL_HANDLE)
		{
			if (adapters && Vulkan::LoadVulkanInstanceFunctions(instance))
				*adapters = EnumerateGPUNames(instance);
		}
		Vulkan::UnloadVulkanLibrary();
	}
}

bool GSDeviceVK::IsSuitableDefaultRenderer()
{
	std::vector<std::string> adapters;
	GetAdapters(&adapters);
	if (adapters.empty())
		return false;

	// Check the first GPU, should be enough.
	const std::string& name = adapters.front();
	Console.WriteLn("Using Vulkan GPU '{%s}' for automatic renderer check.", name.c_str());

	// Any software rendering (LLVMpipe, SwiftShader).
	if (       StringUtil::StartsWithNoCase(name, "llvmpipe")
		|| StringUtil::StartsWithNoCase(name, "SwiftShader"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for software renderer.");
		return false;
	}

	// For Intel, OpenGL usually ends up faster on Linux, because of fbfetch.
	// Plus, the Ivy Bridge and Haswell drivers are incomplete.
	if (StringUtil::StartsWithNoCase(name, "Intel"))
	{
		Console.WriteLn(Color_StrongOrange, "Not using Vulkan for Intel GPU.");
		return false;
	}

	Console.WriteLn(Color_StrongGreen, "Allowing Vulkan as default renderer.");
	return true;
}

RenderAPI GSDeviceVK::GetRenderAPI() const
{
	return RenderAPI::Vulkan;
}

bool GSDeviceVK::Create()
{
	if (!GSDevice::Create())
		return false;

	if (!CreateDeviceAndSwapChain())
		return false;

	VKShaderCache::Create(GSConfig.DisableShaderCache ? std::string_view() : std::string_view(EmuFolders::Cache),
		SHADER_CACHE_VERSION, GSConfig.UseDebugDevice);

	if (!CheckFeatures())
	{
		Console.Error("Your GPU does not support the required Vulkan features.");
		return false;
	}

	if (!CreateNullTexture())
	{
		Console.Error("Failed to create dummy texture");
		return false;
	}

	if (!CreatePipelineLayouts())
	{
		Console.Error("Failed to create pipeline layouts");
		return false;
	}

	if (!CreateRenderPasses())
	{
		Console.Error("Failed to create render passes");
		return false;
	}

	if (!CreateBuffers())
		return false;

	if (!CompileConvertPipelines())
	{
		Console.Error("Failed to compile convert pipelines");
		return false;
	}

	if (!CompileInterlacePipelines())
	{
		Console.Error("Failed to compile interlace pipelines");
		return false;
	}

	if (!CompileMergePipelines())
	{
		Console.Error("Failed to compile merge pipelines");
		return false;
	}

	if (!CreatePersistentDescriptorSets())
	{
		Console.Error("Failed to create persistent descriptor sets");
		return false;
	}

	InitializeState();
	return true;
}

void GSDeviceVK::Destroy()
{
	GSDevice::Destroy();

	EndRenderPass();
	if (GetCurrentCommandBuffer() != VK_NULL_HANDLE)
	{
		ExecuteCommandBuffer(false);
		WaitForGPUIdle();
	}

	DestroyResources();
	VKShaderCache::Destroy();

	VkDevice m_device = vk_init_info.device;

	m_texture_upload_buffer.Destroy(false);

	for (auto& it : m_render_pass_cache)
		vkDestroyRenderPass(m_device, it.second, nullptr);
	m_render_pass_cache.clear();

	if (m_global_descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_device, m_global_descriptor_pool, nullptr);
	m_global_descriptor_pool = VK_NULL_HANDLE;

	DestroyCommandBuffers();

	if (m_allocator != VK_NULL_HANDLE)
		vmaDestroyAllocator(m_allocator);
	m_allocator = VK_NULL_HANDLE;

	Vulkan::UnloadVulkanLibrary();
}

GSDevice::PresentResult GSDeviceVK::BeginPresent(bool frame_skip)
{
	EndRenderPass();
	SubmitCommandBuffer();
	MoveToNextCommandBuffer();

	GSTextureVK* tex = (GSTextureVK*)g_gs_device->GetCurrent();
	if(tex)
	{

		retro_vulkan_image vkimage;
		vkimage.image_view   = tex->GetView();
		vkimage.image_layout = tex->GetVkLayout();
		vkimage.create_info  = {
			VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
			tex->GetImage(), VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
			{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
				VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
			{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
		};
		vulkan->set_image(vulkan->handle, &vkimage, 0, nullptr, vulkan->queue_index);
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, tex->GetWidth(), tex->GetHeight(), 0);
		vulkan->set_image(vulkan->handle, nullptr, 0, nullptr, vulkan->queue_index);
	}
	else
		video_cb(NULL, 0, 0, 0);

	return PresentResult::FrameSkipped;
}

void GSDeviceVK::EndPresent()
{
	VkCommandBuffer cmdbuffer = GetCurrentCommandBuffer();
	vkCmdEndRenderPass(cmdbuffer);

	SubmitCommandBuffer();
	MoveToNextCommandBuffer();
}

void GSDeviceVK::ResetAPIState()
{
	EndRenderPass();
}

void GSDeviceVK::RestoreAPIState()
{
	InvalidateCachedState();
}

bool GSDeviceVK::CreateDeviceAndSwapChain()
{
	if (!Vulkan::LoadVulkanLibrary())
	{
		Console.Error("Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?");
		return false;
	}

	// Read device physical memory properties, we need it for allocating buffers
	vkGetPhysicalDeviceProperties(vk_init_info.gpu, &m_device_properties);

	// We need this to be at least 32 byte aligned for AVX2 stores.
	m_device_properties.limits.minUniformBufferOffsetAlignment =
		std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.minTexelBufferOffsetAlignment =
		std::max(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyOffsetAlignment =
		std::max(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(32));
	m_device_properties.limits.optimalBufferCopyRowPitchAlignment =
		Common::NextPow2(std::max(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(32)));
	m_device_properties.limits.bufferImageGranularity =
		std::max(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(32));

	AcquireWindow();

	if (!Vulkan::LoadVulkanInstanceFunctions(vk_init_info.instance))
	{
		Console.Error("Failed to load Vulkan instance functions");
		Vulkan::UnloadVulkanLibrary();
		return false;
	}

	// Attempt to create the device and critical resources
	if (               !CreateDevice(
				vk_init_info.required_device_extensions,
				vk_init_info.num_required_device_extensions,
				vk_init_info.required_device_layers,
				vk_init_info.num_required_device_layers,
				vk_init_info.required_features)
			|| !CreateAllocator()
			|| !CreateGlobalDescriptorPool()
			|| !CreateCommandBuffers()
			|| !CreateTextureStreamBuffer())
	{
		Console.Error("Failed to create Vulkan context");
		Vulkan::UnloadVulkanLibrary();
		return false;
	}

	return true;
}

bool GSDeviceVK::CheckFeatures()
{
	const VkPhysicalDeviceLimits& limits         = GetDeviceLimits();

	m_features.framebuffer_fetch    = m_optional_extensions.vk_ext_rasterization_order_attachment_access && !GSConfig.DisableFramebufferFetch;
	m_features.texture_barrier      = GSConfig.OverrideTextureBarriers != 0;
	m_features.broken_point_sampler = false;

	// geometryShader is needed because gl_PrimitiveID is part of the Geometry SPIR-V Execution Model.
	m_features.primitive_id         = m_device_features.geometryShader;

#ifdef __APPLE__
	// On Metal (MoltenVK), primid is sometimes available, but broken on some older GPUs and MacOS versions.
	// Officially, it's available on GPUs that support barycentric coordinates (Newer AMD and Apple)
	// Unofficially, it seems to work on older Intel GPUs (but breaks other things on newer Intel GPUs)
	m_features.primitive_id         &= m_optional_extensions.vk_khr_fragment_shader_barycentric;
#endif
	m_features.prefer_new_textures   = true;
	m_features.provoking_vertex_last = m_optional_extensions.vk_ext_provoking_vertex;
	m_features.clip_control          = true;
#ifdef __APPLE__
	m_features.vs_expand             = false;
#else
	m_features.vs_expand =
		!GSConfig.DisableVertexShaderExpand && m_optional_extensions.vk_khr_shader_draw_parameters;
#endif

	if (!m_features.texture_barrier)
		Console.Warning("Texture buffers are disabled. This may break some graphical effects.");

	if (!m_optional_extensions.vk_ext_line_rasterization)
		Console.WriteLn("VK_EXT_line_rasterization or the BRESENHAM mode is not supported, this may cause rendering inaccuracies.");

	// Test for D32S8 support.
	{
		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(vk_init_info.gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
		m_features.stencil_buffer = ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0);
	}

	// Fbfetch is useless if we don't have barriers enabled.
	m_features.framebuffer_fetch &= m_features.texture_barrier;

	// Buggy drivers with broken barriers probably have no chance using GENERAL layout for depth either...
	m_features.test_and_sample_depth = m_features.texture_barrier;

	// Use D32F depth instead of D32S8 when we have framebuffer fetch.
	m_features.stencil_buffer &= !m_features.framebuffer_fetch;

	// whether we can do point/line expand depends on the range of the device
	const float f_upscale = static_cast<float>(GSConfig.UpscaleMultiplier);
	m_features.point_expand =
		(m_device_features.largePoints && limits.pointSizeRange[0] <= f_upscale && limits.pointSizeRange[1] >= f_upscale);
	m_features.line_expand =
		(m_device_features.wideLines && limits.lineWidthRange[0] <= f_upscale && limits.lineWidthRange[1] >= f_upscale);

	Console.WriteLn("Using %s for point expansion and %s for line expansion.",
		m_features.point_expand ? "hardware" : "vertex expanding",
		m_features.line_expand ? "hardware" : "vertex expanding");

	// Check texture format support before we try to create them.
	for (u32 fmt = static_cast<u32>(GSTexture::Format::Color); fmt < static_cast<u32>(GSTexture::Format::PrimID); fmt++)
	{
		const VkFormat vkfmt = LookupNativeFormat(static_cast<GSTexture::Format>(fmt));
		const VkFormatFeatureFlags bits = (static_cast<GSTexture::Format>(fmt) == GSTexture::Format::DepthStencil) ?
		                                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) :
		                                   (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);

		VkFormatProperties props = {};
		vkGetPhysicalDeviceFormatProperties(vk_init_info.gpu, vkfmt, &props);
		if ((props.optimalTilingFeatures & bits) != bits)
		{
			Console.Error("Vulkan Renderer Unavailable",
				"Required format %u is missing bits, you may need to update your driver. (vk:%u, has:0x%x, needs:0x%x)",
				fmt, static_cast<unsigned>(vkfmt), props.optimalTilingFeatures, bits);
			return false;
		}
	}

	m_features.dxt_textures  = m_device_features.textureCompressionBC;
	m_features.bptc_textures = m_device_features.textureCompressionBC;

	return true;
}

void GSDeviceVK::DrawPrimitive()
{
	vkCmdDraw(GetCurrentCommandBuffer(), m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive()
{
	vkCmdDrawIndexed(GetCurrentCommandBuffer(), m_index.count, 1, m_index.start, m_vertex.start, 0);
}

void GSDeviceVK::DrawIndexedPrimitive(int offset, int count)
{
	vkCmdDrawIndexed(GetCurrentCommandBuffer(), count, 1, m_index.start + offset, m_vertex.start, 0);
}

VkFormat GSDeviceVK::LookupNativeFormat(GSTexture::Format format) const
{
	static constexpr std::array<VkFormat, static_cast<int>(GSTexture::Format::BC7) + 1> s_format_mapping = {{
		VK_FORMAT_UNDEFINED, // Invalid
		VK_FORMAT_R8G8B8A8_UNORM, // Color
		VK_FORMAT_R16G16B16A16_UNORM, // HDRColor
		VK_FORMAT_D32_SFLOAT_S8_UINT, // DepthStencil
		VK_FORMAT_R8_UNORM, // UNorm8
		VK_FORMAT_R16_UINT, // UInt16
		VK_FORMAT_R32_UINT, // UInt32
		VK_FORMAT_R32_SFLOAT, // Int32
		VK_FORMAT_BC1_RGBA_UNORM_BLOCK, // BC1
		VK_FORMAT_BC2_UNORM_BLOCK, // BC2
		VK_FORMAT_BC3_UNORM_BLOCK, // BC3
		VK_FORMAT_BC7_UNORM_BLOCK, // BC7
	}};

	return (format != GSTexture::Format::DepthStencil || m_features.stencil_buffer) ?
               s_format_mapping[static_cast<int>(format)] :
               VK_FORMAT_D32_SFLOAT;
}

GSTexture* GSDeviceVK::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	u32 max_img_dim          = GetMaxImageDimension2D();
	const u32 clamped_width  = static_cast<u32>(std::clamp<int>(width, 1, max_img_dim));
	const u32 clamped_height = static_cast<u32>(std::clamp<int>(height, 1, max_img_dim));

	std::unique_ptr<GSTexture> tex(GSTextureVK::Create(type, format, clamped_width, clamped_height, levels));
	if (!tex)
	{
		// We're probably out of vram, try flushing the command buffer to release pending textures.
		PurgePool();
		/* Couldn't allocate texture */
		ExecuteCommandBufferAndRestartRenderPass(true);
		tex = GSTextureVK::Create(type, format, clamped_width, clamped_height, levels);
	}

	return tex.release();
}

std::unique_ptr<GSDownloadTexture> GSDeviceVK::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureVK::Create(width, height, format);
}

void GSDeviceVK::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTextureVK* const sTexVK = static_cast<GSTextureVK*>(sTex);
	GSTextureVK* const dTexVK = static_cast<GSTextureVK*>(dTex);
	const GSVector4i dtex_rc(0, 0, dTexVK->GetWidth(), dTexVK->GetHeight());

	if (sTexVK->GetState() == GSTexture::State::Cleared)
	{
		// source is cleared. if destination is a render target, we can carry the clear forward
		if (dTexVK->IsRenderTargetOrDepthStencil())
		{
			if (dtex_rc.eq(r))
			{
				// pass it forward if we're clearing the whole thing
				if (sTexVK->IsDepthStencil())
					dTexVK->SetClearDepth(sTexVK->GetClearDepth());
				else
					dTexVK->SetClearColor(sTexVK->GetClearColor());

				return;
			}

			if (dTexVK->GetState() == GSTexture::State::Cleared)
			{
				// destination is cleared, if it's the same colour and rect, we can just avoid this entirely
				if (dTexVK->IsDepthStencil())
				{
					if (dTexVK->GetClearDepth() == sTexVK->GetClearDepth())
						return;
				}
				else
				{
					if (dTexVK->GetClearColor() == sTexVK->GetClearColor())
						return;
				}
			}

			// otherwise we need to do an attachment clear
			const bool depth = (dTexVK->GetType() == GSTexture::Type::DepthStencil);
			OMSetRenderTargets(depth ? nullptr : dTexVK, depth ? dTexVK : nullptr, dtex_rc);
			BeginRenderPassForStretchRect(dTexVK, dtex_rc, GSVector4i(destX, destY, destX + r.width(), destY + r.height()));

			// so use an attachment clear
			VkClearAttachment ca;
			ca.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			GSVector4::store<false>(ca.clearValue.color.float32, sTexVK->GetUNormClearColor());
			ca.clearValue.depthStencil.depth = sTexVK->GetClearDepth();
			ca.clearValue.depthStencil.stencil = 0;
			ca.colorAttachment = 0;

			const VkClearRect cr = { {{0, 0}, {static_cast<u32>(r.width()), static_cast<u32>(r.height())}}, 0u, 1u };
			vkCmdClearAttachments(GetCurrentCommandBuffer(), 1, &ca, 1, &cr);
			return;
		}

		// commit the clear to the source first, then do normal copy
		sTexVK->CommitClear();
	}

	// if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
	// (the area outside of where we're copying to)
	if (dTexVK->GetState() == GSTexture::State::Cleared && !dtex_rc.eq(r))
		dTexVK->CommitClear();

	// *now* we can do a normal image copy.
	const VkImageAspectFlags src_aspect = (sTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageAspectFlags dst_aspect = (dTexVK->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageCopy ic = {{src_aspect, 0u, 0u, 1u}, {r.left, r.top, 0u}, {dst_aspect, 0u, 0u, 1u},
		{static_cast<s32>(destX), static_cast<s32>(destY), 0u},
		{static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u}};

	EndRenderPass();

	sTexVK->m_use_fence_counter = GetCurrentFenceCounter();
	dTexVK->m_use_fence_counter = GetCurrentFenceCounter();
	sTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferSrc);
	dTexVK->TransitionToLayout(
		(dTexVK == sTexVK) ? GSTextureVK::Layout::TransferSelf : GSTextureVK::Layout::TransferDst);

	vkCmdCopyImage(GetCurrentCommandBuffer(), sTexVK->GetImage(),
		sTexVK->GetVkLayout(), dTexVK->GetImage(), dTexVK->GetVkLayout(), 1, &ic);

	dTexVK->SetState(GSTexture::State::Dirty);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader /* = ShaderConvert::COPY */, bool linear /* = true */)
{
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect,
		m_convert[static_cast<int>(shader)], linear,
		ShaderConvertWriteMask(shader) == 0xf);
}

void GSDeviceVK::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
	bool green, bool blue, bool alpha, ShaderConvert shader)
{
	const u32 index = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	const bool allow_discard = (index == 0xf);
	int rta_offset = (shader == ShaderConvert::RTA_CORRECTION) ? 16 : 0;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), sRect, static_cast<GSTextureVK*>(dTex), dRect, m_color_copy[index + rta_offset],
		false, allow_discard);
}

void GSDeviceVK::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
}

void GSDeviceVK::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	// Make sure all textures are in shader read only layout, so we don't need to break
	// the render pass to transition.
	for (u32 i = 0; i < num_rects; i++)
	{
		GSTextureVK* const stex = static_cast<GSTextureVK*>(rects[i].src);
		stex->CommitClear();
		if (stex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
		{
			EndRenderPass();
			stex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTextureVK*>(dTex), shader);
}

void GSDeviceVK::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvert shader)
{
	// Set up vertices first.
	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
	{
		/* Uploading bytes to vertex buffer */
		ExecuteCommandBufferAndRestartRenderPass(false);
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u16)))
			Console.Error("Failed to reserve space for vertices");
	}

	// Pain in the arse because the primitive topology for the pipelines is all triangle strips.
	// Don't use primitive restart here, it ends up slower on some drivers.
	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(icount * sizeof(u16));
	SetIndexBuffer(m_index_stream_buffer.GetBuffer());

	// Even though we're batching, a cmdbuffer submit could've messed this up.
	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetUtilityTexture(rects[0].src, rects[0].linear ? m_linear_sampler : m_point_sampler);

	int rta_bit = (shader == ShaderConvert::RTA_CORRECTION) ? 16 : 0;
	SetPipeline(
			(rects[0].wmask.wrgba != 0xf) ? m_color_copy[rects[0].wmask.wrgba | rta_bit] : m_convert[static_cast<int>(shader)]);

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
}

void GSDeviceVK::BeginRenderPassForStretchRect(
	GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	const VkAttachmentLoadOp load_op =
		(allow_discard && dst_rc.eq(dtex_rc)) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : GetLoadOpForTexture(dTex);
	dTex->SetState(GSTexture::State::Dirty);

	if (dTex->GetType() == GSTexture::Type::DepthStencil)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_depth_render_pass_clear, dtex_rc, dTex->GetClearDepth(), 0);
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_depth_render_pass_discard :
                                                                           m_utility_depth_render_pass_load,
				dtex_rc);
	}
	else if (dTex->GetFormat() == GSTexture::Format::Color)
	{
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
			BeginClearRenderPass(m_utility_color_render_pass_clear, dtex_rc, dTex->GetClearColor());
		else
			BeginRenderPass((load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE) ? m_utility_color_render_pass_discard :
                                                                           m_utility_color_render_pass_load,
				dtex_rc);
	}
	else
	{
		// integer formats, etc
		const VkRenderPass rp = GetRenderPass(dTex->GetVkFormat(), VK_FORMAT_UNDEFINED,
			load_op, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
		if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
		{
			BeginClearRenderPass(rp, dtex_rc, dTex->GetClearColor());
		}
		else
		{
			BeginRenderPass(rp, dtex_rc);
		}
	}
}

void GSDeviceVK::DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
	VkPipeline pipeline, bool linear, bool allow_discard)
{
	if (sTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
	{
		// can't transition in a render pass
		EndRenderPass();
		sTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}

	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->GetType() == GSTexture::Type::DepthStencil);
	const GSVector2i size(
		is_present ? GSVector2i(GetWindowWidth(), GetWindowHeight()) : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	// switch rts (which might not end the render pass), so check the bounds
	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
		if (InRenderPass() && dTex->GetState() == GSTexture::State::Cleared)
			EndRenderPass();
	}
	else
	{
		// this is for presenting, we don't want to screw with the viewport/scissor set by display
		m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !InRenderPass())
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

void GSDeviceVK::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	// ia
	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDeviceVK::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	// Super annoying, but apparently NVIDIA doesn't like floats/ints packed together in the same vec4?
	struct Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};

	const Uniforms uniforms = {offsetX, offsetY, dOffset, 0, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		m_convert[static_cast<int>(shader)], false, true);
}

void GSDeviceVK::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 pad1[2];
		float ScaleFactor;
		float pad2[3];
	};

	const Uniforms uniforms = {SBW, DBW, {}, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		m_convert[static_cast<int>(shader)], false, true);
}

void GSDeviceVK::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float pad1[3];
	};

	const Uniforms uniforms = {
		clamp_min, static_cast<int>(downsample_factor), 0, static_cast<float>(downsample_factor * downsample_factor)};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ShaderConvert::DOWNSAMPLE_COPY;
	DoStretchRect(static_cast<GSTextureVK*>(sTex), GSVector4::zero(), static_cast<GSTextureVK*>(dTex), dRect,
		m_convert[static_cast<int>(shader)], false, true);
}

void GSDeviceVK::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4]  = {EXTBUF.EMODA, EXTBUF.EMODC};
	const GSVector4 bg_color    = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const VkSampler& sampler = linear? m_linear_sampler : m_point_sampler;
	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	EndRenderPass();

	// transition everything before starting the new render pass
	const bool has_input_0 =
		(sTex[0] && (sTex[0]->GetState() == GSTexture::State::Dirty ||
						(sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
							 (sTex[1]->GetState() == GSTexture::State::Dirty ||
								 (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
	{
		static_cast<GSTextureVK*>(sTex[0])->CommitClear();
		static_cast<GSTextureVK*>(sTex[0])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	if (has_input_1)
	{
		static_cast<GSTextureVK*>(sTex[1])->CommitClear();
		static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	}
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		if (sTex[1]->GetState() == GSTexture::State::Dirty)
		{
			static_cast<GSTextureVK*>(sTex[1])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
			OMSetRenderTargets(dTex, nullptr, darea);
			SetUtilityTexture(sTex[1], sampler);
			BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
			SetPipeline(m_convert[static_cast<int>(ShaderConvert::COPY)]);
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dTex->SetState(GSTexture::State::Dirty);
			dcleared = true;
		}
	}

	// Upload constant to select YUV algo
	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);
		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetPipeline(m_convert[static_cast<int>(ShaderConvert::YUV)]);
			SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();

		if (sTex[0] == sTex[2])
		{
			// need a barrier here because of the render pass
			static_cast<GSTextureVK*>(sTex[2])->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginClearRenderPass(m_utility_color_render_pass_clear, darea, c);
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(m_utility_color_render_pass_load, darea);
	}

	if (sTex[0] && sTex[0]->GetState() == GSTexture::State::Dirty)
	{
		// 1st output is enabled. It must be blended
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(m_merge[PMODE.MMOD]);
		SetUtilityPushConstants(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		SetPipeline(m_convert[static_cast<int>(ShaderConvert::YUV)]);
		SetUtilityTexture(dTex, sampler);
		SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(m_utility_color_render_pass_load, fbarea);
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	// this texture is going to get used as an input, so make sure we don't read undefined data
	static_cast<GSTextureVK*>(dTex)->CommitClear();
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);

	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityTexture(sTex, linear ? m_linear_sampler : m_point_sampler);
	BeginRenderPassForStretchRect(static_cast<GSTextureVK*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(m_interlace[static_cast<int>(shader)]);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();

	static_cast<GSTextureVK*>(dTex)->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
}

void GSDeviceVK::IASetVertexBuffer(const void* vertex, size_t stride, size_t count)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
	{
		/* Uploading bytes to vertex buffer */
		ExecuteCommandBufferAndRestartRenderPass(false);
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride)))
			Console.Error("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;

	GSVector4i::storent(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, count * stride);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceVK::IASetIndexBuffer(const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
	if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
	{
		/* Uploading bytes to index buffer */
		ExecuteCommandBufferAndRestartRenderPass(false);
		if (!m_index_stream_buffer.ReserveMemory(size, sizeof(u16)))
			Console.Error("Failed to reserve space for vertices");
	}

	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;

	memcpy(m_index_stream_buffer.GetCurrentHostPointer(), index, size);
	m_index_stream_buffer.CommitMemory(size);

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
}

void GSDeviceVK::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor, FeedbackLoopFlag feedback_loop)
{
	GSTextureVK* vkRt = static_cast<GSTextureVK*>(rt);
	GSTextureVK* vkDs = static_cast<GSTextureVK*>(ds);

	if (m_current_render_target != vkRt || m_current_depth_target != vkDs ||
		m_current_framebuffer_feedback_loop != feedback_loop)
	{
		// framebuffer change or feedback loop enabled/disabled
		EndRenderPass();

		if (vkRt)
			m_current_framebuffer = vkRt->GetLinkedFramebuffer(vkDs, (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT) != 0);
		else
			m_current_framebuffer = vkDs->GetLinkedFramebuffer(nullptr, false);
	}
	else if (InRenderPass())
	{
		// Framebuffer unchanged, but check for clears
		// Use an attachment clear to wipe it out without restarting the render pass
		if (IsDeviceNVIDIA())
		{
			// Using vkCmdClearAttachments() within a render pass on NVIDIA seems to cause dependency issues
			// between draws that are testing depth which precede it. The result is flickering where Z tests
			// should be failing. Breaking/restarting the render pass isn't enough to work around the bug,
			// it needs an explicit pipeline barrier.
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkRt->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkRt->GetLayout(), vkRt->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkRt->SetState(GSTexture::State::Dirty);
				}
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					EndRenderPass();
					vkDs->TransitionSubresourcesToLayout(GetCurrentCommandBuffer(), 0, 1,
						vkDs->GetLayout(), vkDs->GetLayout());
				}
				else
				{
					// Invalidated -> Dirty.
					vkDs->SetState(GSTexture::State::Dirty);
				}
			}
		}
		else
		{
			std::array<VkClearAttachment, 2> cas;
			u32 num_ca = 0;
			if (vkRt && vkRt->GetState() != GSTexture::State::Dirty)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					ca.colorAttachment = 0;
					GSVector4::store<false>(ca.clearValue.color.float32, vkRt->GetUNormClearColor());
				}

				vkRt->SetState(GSTexture::State::Dirty);
			}
			if (vkDs && vkDs->GetState() != GSTexture::State::Dirty)
			{
				if (vkDs->GetState() == GSTexture::State::Cleared)
				{
					VkClearAttachment& ca = cas[num_ca++];
					ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
					ca.colorAttachment = 1;
					ca.clearValue.depthStencil = {vkDs->GetClearDepth()};
				}

				vkDs->SetState(GSTexture::State::Dirty);
			}

			if (num_ca > 0)
			{
				const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
				const VkClearRect cr = {{{0, 0}, {static_cast<u32>(size.x), static_cast<u32>(size.y)}}, 0u, 1u};
				vkCmdClearAttachments(GetCurrentCommandBuffer(), num_ca, cas.data(), 1, &cr);
			}
		}
	}

	m_current_render_target = vkRt;
	m_current_depth_target = vkDs;
	m_current_framebuffer_feedback_loop = feedback_loop;

	if (!InRenderPass())
	{
		if (vkRt)
		{
			// NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
			// the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
			if (feedback_loop & FeedbackLoopFlag_ReadAndWriteRT)
			{
				if (vkRt->GetState() == GSTexture::State::Cleared && IsDeviceNVIDIA())
					vkRt->CommitClear();

				if (vkRt->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					// need to update descriptors to reflect the new layout
					m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
					vkRt->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else
			{
				vkRt->TransitionToLayout(GSTextureVK::Layout::ColorAttachment);
			}
		}
		if (vkDs)
		{
			// need to update descriptors to reflect the new layout
			if ((feedback_loop & FeedbackLoopFlag_ReadDS))
			{
				if (vkDs->GetLayout() != GSTextureVK::Layout::FeedbackLoop)
				{
					m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
					vkDs->TransitionToLayout(GSTextureVK::Layout::FeedbackLoop);
				}
			}
			else
			{
				vkDs->TransitionToLayout(GSTextureVK::Layout::DepthStencilAttachment);
			}

		}
	}

	// This is used to set/initialize the framebuffer for tfx rendering.
	const GSVector2i size = vkRt ? vkRt->GetSize() : vkDs->GetSize();
	const VkViewport vp{0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};

	SetViewport(vp);
	SetScissor(scissor);
}

VkSampler GSDeviceVK::GetSampler(GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	const bool aniso = (ss.aniso && GSConfig.MaxAnisotropy > 1 && m_device_features.samplerAnisotropy);

	// See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
	// for the reasoning behind 0.25f here.
	const VkSamplerCreateInfo ci = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, 0,
		ss.IsMagFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // min
		ss.IsMinFilterLinear() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, // mag
		ss.IsMipFilterLinear() ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, // mip
		static_cast<VkSamplerAddressMode>(
			ss.tau ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // u
		static_cast<VkSamplerAddressMode>(
			ss.tav ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE), // v
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // w
		0.0f, // lod bias
		static_cast<VkBool32>(aniso), // anisotropy enable
		aniso ? static_cast<float>(GSConfig.MaxAnisotropy) : 1.0f, // anisotropy
		VK_FALSE, // compare enable
		VK_COMPARE_OP_ALWAYS, // compare op
		0.0f, // min lod
		(ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : VK_LOD_CLAMP_NONE, // max lod
		VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, // border
		VK_FALSE // unnormalized coordinates
	};
	VkSampler sampler = VK_NULL_HANDLE;
	vkCreateSampler(vk_init_info.device, &ci, nullptr, &sampler);

	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

void GSDeviceVK::ClearSamplerCache()
{
	VkDevice m_device = vk_init_info.device;
	ExecuteCommandBuffer(true);
	for (const auto& it : m_samplers)
	{
		if (it.second != VK_NULL_HANDLE)
			vkDestroySampler(m_device, it.second, nullptr);
	}
	m_samplers.clear();
	m_point_sampler   = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler  = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_utility_sampler = m_point_sampler;
	m_tfx_sampler     = m_point_sampler;
}

static void AddMacro(std::stringstream& ss, const char* name, int value)
{
	ss << "#define " << name << " " << value << "\n";
}

static void AddShaderHeader(std::stringstream& ss)
{
	const GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const GSDevice::FeatureSupport features = dev->Features();

	ss << "#version 460 core\n";
	ss << "#extension GL_EXT_samplerless_texture_functions : require\n";

	if (features.vs_expand)
		ss << "#extension GL_ARB_shader_draw_parameters : require\n";

	if (!features.texture_barrier)
		ss << "#define DISABLE_TEXTURE_BARRIER 1\n";
}

static void AddShaderStageMacro(std::stringstream& ss, bool vs, bool gs, bool fs)
{
	if (vs)
		ss << "#define VERTEX_SHADER 1\n";
	else if (gs)
		ss << "#define GEOMETRY_SHADER 1\n";
	else if (fs)
		ss << "#define FRAGMENT_SHADER 1\n";
}

static void AddUtilityVertexAttributes(Vulkan::GraphicsPipelineBuilder& gpb)
{
	gpb.AddVertexBuffer(0, sizeof(GSVertexPT1));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0);
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, 16);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
}

static void SetPipelineProvokingVertex(const GSDevice::FeatureSupport& features, Vulkan::GraphicsPipelineBuilder& gpb)
{
	// We enable provoking vertex here anyway, in case it doesn't support multiple modes in the same pass.
	// Normally we wouldn't enable it on the present/swap chain, but apparently the rule is it applies to the last
	// pipeline bound before the render pass begun, and in this case, we can't bind null.
	if (features.provoking_vertex_last)
		gpb.SetProvokingVertex(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
}

VkShaderModule GSDeviceVK::GetUtilityVertexShader(const char *source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetVertexShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityVertexShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetVertexShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityFragmentShader(const char *source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetFragmentShader(ss.str());
}

VkShaderModule GSDeviceVK::GetUtilityFragmentShader(const std::string& source, const char* replace_main = nullptr)
{
	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	if (replace_main)
		ss << "#define " << replace_main << " main\n";
	ss << source;

	return g_vulkan_shader_cache->GetFragmentShader(ss.str());
}

bool GSDeviceVK::CreateNullTexture()
{
	m_null_texture = GSTextureVK::Create(GSTexture::Type::RenderTarget, GSTexture::Format::Color, 1, 1, 1);
	if (!m_null_texture)
		return false;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	const VkClearColorValue ccv{};
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::ClearDst);
	vkCmdClearColorImage(cmdbuf, m_null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
	m_null_texture->TransitionToLayout(cmdbuf, GSTextureVK::Layout::General);

	return true;
}

bool GSDeviceVK::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (m_features.vs_expand ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0),
			VERTEX_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex buffer");
		return false;
	}

	if (!m_index_stream_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate index buffer");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate vertex uniform buffer");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, FRAGMENT_UNIFORM_BUFFER_SIZE))
	{
		Console.Error("Failed to allocate fragment uniform buffer");
		return false;
	}

	if (!AllocatePreinitializedGPUBuffer(EXPAND_BUFFER_SIZE, &m_expand_index_buffer,
			&m_expand_index_buffer_allocation, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			&GSDevice::GenerateExpansionIndexBuffer))
	{
		Console.Error("Failed to allocate expansion index buffer");
		return false;
	}

	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
	return true;
}

bool GSDeviceVK::CreatePipelineLayouts()
{
	VkDevice dev = vk_init_info.device;
	Vulkan::DescriptorSetLayoutBuilder dslb;
	Vulkan::PipelineLayoutBuilder plb;

	//////////////////////////////////////////////////////////////////////////
	// Convert Pipeline Layout
	//////////////////////////////////////////////////////////////////////////

	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NUM_CONVERT_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_utility_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, CONVERT_PUSH_CONSTANTS_SIZE);
	plb.AddDescriptorSet(m_utility_ds_layout);
	if ((m_utility_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	//////////////////////////////////////////////////////////////////////////
	// Draw/TFX Pipeline Layout
	//////////////////////////////////////////////////////////////////////////
	dslb.AddBinding(
		0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (m_features.vs_expand)
		dslb.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_tfx_ubo_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_sampler_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	dslb.AddBinding(0,
		m_features.texture_barrier
		? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT 
		: VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		1, VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if ((m_tfx_rt_texture_ds_layout = dslb.Create(dev)) == VK_NULL_HANDLE)
		return false;

	plb.AddDescriptorSet(m_tfx_ubo_ds_layout);
	plb.AddDescriptorSet(m_tfx_sampler_ds_layout);
	plb.AddDescriptorSet(m_tfx_rt_texture_ds_layout);
	if ((m_tfx_pipeline_layout = plb.Create(dev)) == VK_NULL_HANDLE)
		return false;
	return true;
}

#define GET(dest, rt, depth, fbl, dsp, opa, opb, opc) \
	do \
	{ \
		dest = GetRenderPass( \
			(rt), (depth), ((rt) != VK_FORMAT_UNDEFINED) ? (opa) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* color load */ \
			((rt) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
                                            VK_ATTACHMENT_STORE_OP_DONT_CARE, /* color store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opb) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* depth load */ \
			((depth) != VK_FORMAT_UNDEFINED) ? VK_ATTACHMENT_STORE_OP_STORE : \
                                               VK_ATTACHMENT_STORE_OP_DONT_CARE, /* depth store */ \
			((depth) != VK_FORMAT_UNDEFINED) ? (opc) : VK_ATTACHMENT_LOAD_OP_DONT_CARE, /* stencil load */ \
			VK_ATTACHMENT_STORE_OP_DONT_CARE, /* stencil store */ \
			(fbl), /* feedback loop */ \
			(dsp) /* depth sampling */ \
		); \
		if (dest == VK_NULL_HANDLE) \
			return false; \
	} while (0)

bool GSDeviceVK::CreateRenderPasses()
{
	const VkFormat rt_format = LookupNativeFormat(GSTexture::Format::Color);
	const VkFormat hdr_rt_format = LookupNativeFormat(GSTexture::Format::HDRColor);
	const VkFormat depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);

	for (u32 rt = 0; rt < 2; rt++)
	{
		for (u32 ds = 0; ds < 2; ds++)
		{
			for (u32 hdr = 0; hdr < 2; hdr++)
			{
				for (u32 stencil = 0; stencil < 2; stencil++)
				{
					for (u32 fbl = 0; fbl < 2; fbl++)
					{
						for (u32 dsp = 0; dsp < 2; dsp++)
						{
							for (u32 opa = VK_ATTACHMENT_LOAD_OP_LOAD; opa <= VK_ATTACHMENT_LOAD_OP_DONT_CARE; opa++)
							{
								for (u32 opb = VK_ATTACHMENT_LOAD_OP_LOAD; opb <= VK_ATTACHMENT_LOAD_OP_DONT_CARE;
									 opb++)
								{
									const VkFormat rp_rt_format =
										(rt != 0) ? ((hdr != 0) ? hdr_rt_format : rt_format) : VK_FORMAT_UNDEFINED;
									const VkFormat rp_depth_format = (ds != 0) ? depth_format : VK_FORMAT_UNDEFINED;
									const VkAttachmentLoadOp opc = (!stencil || !m_features.stencil_buffer) ?
																	   VK_ATTACHMENT_LOAD_OP_DONT_CARE :
																	   VK_ATTACHMENT_LOAD_OP_LOAD;
									GET(m_tfx_render_pass[rt][ds][hdr][stencil][fbl][dsp][opa][opb], rp_rt_format,
										rp_depth_format, (fbl != 0), (dsp != 0), static_cast<VkAttachmentLoadOp>(opa),
										static_cast<VkAttachmentLoadOp>(opb), static_cast<VkAttachmentLoadOp>(opc));
								}
							}
						}
					}
				}
			}
		}
	}

	GET(m_utility_color_render_pass_load, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_clear, rt_format, VK_FORMAT_UNDEFINED, false, false, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_color_render_pass_discard, rt_format, VK_FORMAT_UNDEFINED, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_load, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_clear, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	GET(m_utility_depth_render_pass_discard, VK_FORMAT_UNDEFINED, depth_format, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE);

	m_date_setup_render_pass = GetRenderPass(VK_FORMAT_UNDEFINED, depth_format,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		m_features.stencil_buffer ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		m_features.stencil_buffer ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
	if (m_date_setup_render_pass == VK_NULL_HANDLE)
		return false;

#undef GET

	return true;
}

bool GSDeviceVK::CompileConvertPipelines()
{
	VkDevice m_device = vk_init_info.device;
	VkShaderModule vs = GetUtilityVertexShader(convert_glsl_shader_raw);
	if (vs == VK_NULL_HANDLE)
		return false;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoBlendingState();
	gpb.SetVertexShader(vs);

	for (ShaderConvert i = ShaderConvert::COPY; static_cast<int>(i) < static_cast<int>(ShaderConvert::Count);
		 i = static_cast<ShaderConvert>(static_cast<int>(i) + 1))
	{
		VkShaderModule ps;
		const bool depth = HasDepthOutput(i);
		const int index  = static_cast<int>(i);

		VkRenderPass rp;
		switch (i)
		{
			case ShaderConvert::RGBA8_TO_16_BITS:
			case ShaderConvert::FLOAT32_TO_16_BITS:
				rp = GetRenderPass(LookupNativeFormat(GSTexture::Format::UInt16),
					VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case ShaderConvert::FLOAT32_TO_32_BITS:
				rp = GetRenderPass(LookupNativeFormat(GSTexture::Format::UInt32),
					VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
			case ShaderConvert::DATM_0:
			case ShaderConvert::DATM_1:
			case ShaderConvert::DATM_0_RTA_CORRECTION:
			case ShaderConvert::DATM_1_RTA_CORRECTION:
				rp = m_date_setup_render_pass;
				break;
			default:
				rp = GetRenderPass(
					LookupNativeFormat(depth ? GSTexture::Format::Invalid : GSTexture::Format::Color),
					LookupNativeFormat(
						depth ? GSTexture::Format::DepthStencil : GSTexture::Format::Invalid),
					VK_ATTACHMENT_LOAD_OP_DONT_CARE);
				break;
		}

		if (!rp)
		{
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}

		gpb.SetRenderPass(rp, 0);

		if (IsDATMConvertShader(i))
		{
			const VkStencilOpState sos = {
				VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 1u, 1u, 1u};
			gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
			gpb.SetStencilState(true, sos, sos);
		}
		else
		{
			gpb.SetDepthState(depth, depth, VK_COMPARE_OP_ALWAYS);
			gpb.SetNoStencilState();
		}

		gpb.SetColorWriteMask(0, ShaderConvertWriteMask(i));

		ps = GetUtilityFragmentShader(convert_glsl_shader_raw, shaderName(i));
		if (ps == VK_NULL_HANDLE)
		{
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}

		gpb.SetFragmentShader(ps);

		m_convert[index] =
			gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		if (!m_convert[index])
		{
			SafeDestroyShaderModule(m_device, ps);
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}

		if (i == ShaderConvert::COPY)
		{
			// compile color copy pipelines
			gpb.SetRenderPass(m_utility_color_render_pass_discard, 0);
			for (u32 j = 0; j < 16; j++)
			{
				gpb.ClearBlendAttachments();
				gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, static_cast<VkColorComponentFlags>(j));
				m_color_copy[j] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
				if (!m_color_copy[j])
				{
					SafeDestroyShaderModule(m_device, ps);
					SafeDestroyShaderModule(m_device, vs);
					return false;
				}
			}
		}
		else if (i == ShaderConvert::RTA_CORRECTION)
		{
			// compile color copy pipelines
			gpb.SetRenderPass(m_utility_color_render_pass_discard, 0);
			VkShaderModule ps = GetUtilityFragmentShader(convert_glsl_shader_raw, shaderName(i));
			if (ps == VK_NULL_HANDLE)
				return false;

			gpb.SetFragmentShader(ps);

			for (u32 j = 16; j < 32; j++)
			{
				gpb.ClearBlendAttachments();

				gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, static_cast<VkColorComponentFlags>(j - 16));
				m_color_copy[j] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
				if (!m_color_copy[j])
				{
					SafeDestroyShaderModule(m_device, ps);
					SafeDestroyShaderModule(m_device, vs);
					return false;
				}
			}
		}
		else if (i == ShaderConvert::HDR_INIT || i == ShaderConvert::HDR_RESOLVE)
		{
			const bool is_setup = i == ShaderConvert::HDR_INIT;
			VkPipeline(&arr)[2][2] = *(is_setup ? &m_hdr_setup_pipelines : &m_hdr_finish_pipelines);
			for (u32 ds = 0; ds < 2; ds++)
			{
				for (u32 fbl = 0; fbl < 2; fbl++)
				{
					gpb.SetRenderPass(GetTFXRenderPass(true, ds != 0, is_setup, false, fbl != 0, false,
										  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE),
						0);
					arr[ds][fbl] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
					if (!arr[ds][fbl])
					{
						SafeDestroyShaderModule(m_device, ps);
						SafeDestroyShaderModule(m_device, vs);
						return false;
					}
				}
			}
		}
		SafeDestroyShaderModule(m_device, ps);
	}

	// date image setup
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 clear = 0; clear < 2; clear++)
		{
			m_date_image_setup_render_passes[ds][clear] =
				GetRenderPass(LookupNativeFormat(GSTexture::Format::PrimID),
					ds ? LookupNativeFormat(GSTexture::Format::DepthStencil) : VK_FORMAT_UNDEFINED,
					VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
					ds ? (clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD) : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					ds ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE);
		}
	}

	for (u32 datm = 0; datm < 4; datm++)
	{
		char val[64];
		snprintf(val, sizeof(val), "ps_stencil_image_init_%d", datm);
		VkShaderModule ps = GetUtilityFragmentShader(convert_glsl_shader_raw, val);
		if (ps == VK_NULL_HANDLE)
		{
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}

		gpb.SetPipelineLayout(m_utility_pipeline_layout);
		gpb.SetFragmentShader(ps);
		gpb.SetNoDepthTestState();
		gpb.SetNoStencilState();
		gpb.ClearBlendAttachments();
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);

		for (u32 ds = 0; ds < 2; ds++)
		{
			gpb.SetRenderPass(m_date_image_setup_render_passes[ds][0], 0);
			m_date_image_setup_pipelines[ds][datm] =
				gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
			if (!m_date_image_setup_pipelines[ds][datm])
			{
				SafeDestroyShaderModule(m_device, ps);
				SafeDestroyShaderModule(m_device, vs);
				return false;
			}
		}
		SafeDestroyShaderModule(m_device, ps);
	}

	SafeDestroyShaderModule(m_device, vs);

	return true;
}

bool GSDeviceVK::CompileInterlacePipelines()
{
	VkDevice m_device = vk_init_info.device;

	VkRenderPass rp = GetRenderPass(
		LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(interlace_glsl_shader_raw);
	if (vs == VK_NULL_HANDLE)
		return false;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetNoBlendingState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_interlace.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(interlace_glsl_shader_raw, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
		{
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}

		gpb.SetFragmentShader(ps);

		m_interlace[i] =
			gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		SafeDestroyShaderModule(m_device, ps);
		if (!m_interlace[i])
		{
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}
	}

	SafeDestroyShaderModule(m_device, vs);
	return true;
}

bool GSDeviceVK::CompileMergePipelines()
{
	VkDevice m_device = vk_init_info.device;

	VkRenderPass rp = GetRenderPass(
		LookupNativeFormat(GSTexture::Format::Color), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD);
	if (!rp)
		return false;

	VkShaderModule vs = GetUtilityVertexShader(merge_glsl_shader_raw);
	if (vs == VK_NULL_HANDLE)
		return false;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);
	AddUtilityVertexAttributes(gpb);
	gpb.SetPipelineLayout(m_utility_pipeline_layout);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetNoCullRasterizationState();
	gpb.SetNoDepthTestState();
	gpb.SetRenderPass(rp, 0);
	gpb.SetVertexShader(vs);

	for (int i = 0; i < static_cast<int>(m_merge.size()); i++)
	{
		VkShaderModule ps = GetUtilityFragmentShader(merge_glsl_shader_raw, StringUtil::StdStringFromFormat("ps_main%d", i).c_str());
		if (ps == VK_NULL_HANDLE)
			return false;

		gpb.SetFragmentShader(ps);
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);

		m_merge[i] = gpb.Create(m_device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		SafeDestroyShaderModule(m_device, ps);
		if (!m_merge[i])
		{
			SafeDestroyShaderModule(m_device, vs);
			return false;
		}
	}

	SafeDestroyShaderModule(m_device, vs);
	return true;
}

void GSDeviceVK::DestroyResources()
{
	VkDevice m_device = vk_init_info.device;

	ExecuteCommandBuffer(true);

	if (m_tfx_ubo_descriptor_set != VK_NULL_HANDLE)
		FreeGlobalDescriptorSet(m_tfx_ubo_descriptor_set);

	for (auto& it : m_tfx_pipelines)
	{
		VkPipeline& p = it.second;
		if (p != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	}
	for (auto& it : m_tfx_fragment_shaders)
		SafeDestroyShaderModule(m_device, it.second);
	for (auto& it : m_tfx_vertex_shaders)
		SafeDestroyShaderModule(m_device, it.second);
	for (VkPipeline& it : m_interlace)
	{
		VkPipeline& p = it;
		if (p != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	}
	for (VkPipeline& it : m_merge)
	{
		VkPipeline& p = it;
		if (p != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	}
	for (VkPipeline& it : m_color_copy)
	{
		VkPipeline& p = it;
		if (p != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	}
	for (VkPipeline& it : m_convert)
	{
		VkPipeline& p = it;
		if (p != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	}
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 fbl = 0; fbl < 2; fbl++)
		{
			VkPipeline& p = m_hdr_setup_pipelines[ds][fbl];
			if (p != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(m_device, p, nullptr);
				p = VK_NULL_HANDLE;
			}
			p = m_hdr_finish_pipelines[ds][fbl];
			if (p != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(m_device, p, nullptr);
				p = VK_NULL_HANDLE;
			}
		}
	}
	for (u32 ds = 0; ds < 2; ds++)
	{
		for (u32 datm = 0; datm < 4; datm++)
		{
			VkPipeline& p = m_date_image_setup_pipelines[ds][datm];
			if (p != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(m_device, p, nullptr);
				p = VK_NULL_HANDLE;
			}
		}
	}

	for (auto& it : m_samplers)
	{
		VkSampler& samp = it.second;
		if (samp != VK_NULL_HANDLE)
		{
			vkDestroySampler(m_device, samp, nullptr);
			samp = VK_NULL_HANDLE;
		}
	}

	m_linear_sampler = VK_NULL_HANDLE;
	m_point_sampler = VK_NULL_HANDLE;

	m_utility_color_render_pass_load = VK_NULL_HANDLE;
	m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	m_utility_depth_render_pass_clear = VK_NULL_HANDLE;
	m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	m_date_setup_render_pass = VK_NULL_HANDLE;

	m_fragment_uniform_stream_buffer.Destroy(false);
	m_vertex_uniform_stream_buffer.Destroy(false);
	m_index_stream_buffer.Destroy(false);
	m_vertex_stream_buffer.Destroy(false);
	if (m_expand_index_buffer != VK_NULL_HANDLE)
	{
		vmaDestroyBuffer(GetAllocator(), m_expand_index_buffer, m_expand_index_buffer_allocation);
		m_expand_index_buffer = VK_NULL_HANDLE;
		m_expand_index_buffer_allocation = VK_NULL_HANDLE;
	}

	SafeDestroyPipelineLayout(m_device, m_tfx_pipeline_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_tfx_rt_texture_ds_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_tfx_sampler_ds_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_tfx_ubo_ds_layout);
	SafeDestroyPipelineLayout(m_device, m_utility_pipeline_layout);
	SafeDestroyDescriptorSetLayout(m_device, m_utility_ds_layout);

	if (m_null_texture)
	{
		m_null_texture->Destroy(false);
		m_null_texture.reset();
	}
}

VkShaderModule GSDeviceVK::GetTFXVertexShader(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, true, false, false);
	AddMacro(ss, "VS_TME", sel.tme);
	AddMacro(ss, "VS_FST", sel.fst);
	AddMacro(ss, "VS_IIP", sel.iip);
	AddMacro(ss, "VS_POINT_SIZE", sel.point_size);
	AddMacro(ss, "VS_EXPAND", static_cast<int>(sel.expand));
	AddMacro(ss, "VS_PROVOKING_VERTEX_LAST", static_cast<int>(m_features.provoking_vertex_last));
	ss << tfx_glsl_shader_raw;

	VkShaderModule mod = g_vulkan_shader_cache->GetVertexShader(ss.str());

	m_tfx_vertex_shaders.emplace(sel.key, mod);
	return mod;
}

VkShaderModule GSDeviceVK::GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fragment_shaders.find(sel);
	if (it != m_tfx_fragment_shaders.end())
		return it->second;

	std::stringstream ss;
	AddShaderHeader(ss);
	AddShaderStageMacro(ss, false, false, true);
	AddMacro(ss, "PS_FST", sel.fst);
	AddMacro(ss, "PS_WMS", sel.wms);
	AddMacro(ss, "PS_WMT", sel.wmt);
	AddMacro(ss, "PS_ADJS", sel.adjs);
	AddMacro(ss, "PS_ADJT", sel.adjt);
	AddMacro(ss, "PS_AEM_FMT", sel.aem_fmt);
	AddMacro(ss, "PS_PAL_FMT", sel.pal_fmt);
	AddMacro(ss, "PS_DST_FMT", sel.dst_fmt);
	AddMacro(ss, "PS_DEPTH_FMT", sel.depth_fmt);
	AddMacro(ss, "PS_CHANNEL_FETCH", sel.channel);
	AddMacro(ss, "PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	AddMacro(ss, "PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	AddMacro(ss, "PS_AEM", sel.aem);
	AddMacro(ss, "PS_TFX", sel.tfx);
	AddMacro(ss, "PS_TCC", sel.tcc);
	AddMacro(ss, "PS_ATST", sel.atst);
	AddMacro(ss, "PS_AFAIL", sel.afail);
	AddMacro(ss, "PS_FOG", sel.fog);
	AddMacro(ss, "PS_BLEND_HW", sel.blend_hw);
	AddMacro(ss, "PS_A_MASKED", sel.a_masked);
	AddMacro(ss, "PS_FBA", sel.fba);
	AddMacro(ss, "PS_LTF", sel.ltf);
	AddMacro(ss, "PS_AUTOMATIC_LOD", sel.automatic_lod);
	AddMacro(ss, "PS_MANUAL_LOD", sel.manual_lod);
	AddMacro(ss, "PS_COLCLIP", sel.colclip);
	AddMacro(ss, "PS_DATE", sel.date);
	AddMacro(ss, "PS_TCOFFSETHACK", sel.tcoffsethack);
	AddMacro(ss, "PS_REGION_RECT", sel.region_rect);
	AddMacro(ss, "PS_BLEND_A", sel.blend_a);
	AddMacro(ss, "PS_BLEND_B", sel.blend_b);
	AddMacro(ss, "PS_BLEND_C", sel.blend_c);
	AddMacro(ss, "PS_BLEND_D", sel.blend_d);
	AddMacro(ss, "PS_BLEND_MIX", sel.blend_mix);
	AddMacro(ss, "PS_ROUND_INV", sel.round_inv);
	AddMacro(ss, "PS_FIXED_ONE_A", sel.fixed_one_a);
	AddMacro(ss, "PS_IIP", sel.iip);
	AddMacro(ss, "PS_SHUFFLE", sel.shuffle);
	AddMacro(ss, "PS_SHUFFLE_SAME", sel.shuffle_same);
	AddMacro(ss, "PS_PROCESS_BA", sel.process_ba);
	AddMacro(ss, "PS_PROCESS_RG", sel.process_rg);
	AddMacro(ss, "PS_SHUFFLE_ACROSS", sel.shuffle_across);
	AddMacro(ss, "PS_READ16_SRC", sel.real16src);
	AddMacro(ss, "PS_WRITE_RG", sel.write_rg);
	AddMacro(ss, "PS_FBMASK", sel.fbmask);
	AddMacro(ss, "PS_HDR", sel.hdr);
	AddMacro(ss, "PS_RTA_CORRECTION", sel.rta_correction);
	AddMacro(ss, "PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	AddMacro(ss, "PS_DITHER", sel.dither);
	AddMacro(ss, "PS_DITHER_ADJUST", sel.dither_adjust);
	AddMacro(ss, "PS_ZCLAMP", sel.zclamp);
	AddMacro(ss, "PS_PABE", sel.pabe);
	AddMacro(ss, "PS_SCANMSK", sel.scanmsk);
	AddMacro(ss, "PS_TEX_IS_FB", sel.tex_is_fb);
	AddMacro(ss, "PS_NO_COLOR", sel.no_color);
	AddMacro(ss, "PS_NO_COLOR1", sel.no_color1);
	ss << tfx_glsl_shader_raw;

	VkShaderModule mod = g_vulkan_shader_cache->GetFragmentShader(ss.str());

	m_tfx_fragment_shaders.emplace(sel, mod);
	return mod;
}

VkPipeline GSDeviceVK::CreateTFXPipeline(const PipelineSelector& p)
{
	static constexpr std::array<VkPrimitiveTopology, 3> topology_lookup = {{
		VK_PRIMITIVE_TOPOLOGY_POINT_LIST, // Point
		VK_PRIMITIVE_TOPOLOGY_LINE_LIST, // Line
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // Triangle
	}};

	GSHWDrawConfig::BlendState pbs{p.bs};
	GSHWDrawConfig::PSSelector pps{p.ps};
	if (!p.bs.IsEffective(p.cms))
	{
		// disable blending when colours are masked
		pbs = {};
		pps.no_color1 = true;
	}

	VkShaderModule vs = GetTFXVertexShader(p.vs);
	VkShaderModule fs = GetTFXFragmentShader(pps);
	if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	Vulkan::GraphicsPipelineBuilder gpb;
	SetPipelineProvokingVertex(m_features, gpb);

	// Common state
	gpb.SetPipelineLayout(m_tfx_pipeline_layout);
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// DATE image prepass
		gpb.SetRenderPass(m_date_image_setup_render_passes[p.ds][0], 0);
	}
	else
	{
		gpb.SetRenderPass(
			GetTFXRenderPass(p.rt, p.ds, p.ps.hdr, p.dss.date,
				p.IsRTFeedbackLoop(), p.IsTestingAndSamplingDepth(),
				p.rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				p.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
			0);
	}
	gpb.SetPrimitiveTopology(topology_lookup[p.topology]);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	if (p.topology == static_cast<u8>(GSHWDrawConfig::Topology::Line) && GetOptionalExtensions().vk_ext_line_rasterization)
		gpb.SetLineRasterizationMode(VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT);
	gpb.SetDynamicViewportAndScissorState();
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);

	// Shaders
	gpb.SetVertexShader(vs);
	gpb.SetFragmentShader(fs);

	// IA
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		gpb.AddVertexBuffer(0, sizeof(GSVertex));
		gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
		gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
		gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
		gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
		gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
		gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
		gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG
	}

	// DepthStencil
	static const VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((p.dss.ztst != ZTST_ALWAYS || p.dss.zwe), p.dss.zwe, ztst[p.dss.ztst]);
	if (p.dss.date)
	{
		const VkStencilOpState sos{VK_STENCIL_OP_KEEP, p.dss.date_one ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP,
			VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 1u, 1u, 1u};
		gpb.SetStencilState(true, sos, sos);
	}

	// Blending
	if (IsDATEModePrimIDInit(p.ps.date))
	{
		// image DATE prepass
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MIN, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT);
	}
	else if (pbs.enable)
	{
		// clang-format off
		static constexpr std::array<VkBlendFactor, 16> vk_blend_factors = { {
			VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
			VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
			VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO
		}};
		static constexpr std::array<VkBlendOp, 3> vk_blend_ops = {{
				VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT
		}};
		// clang-format on

		gpb.SetBlendAttachment(0, true, vk_blend_factors[pbs.src_factor], vk_blend_factors[pbs.dst_factor],
			vk_blend_ops[pbs.op], vk_blend_factors[pbs.src_factor_alpha], vk_blend_factors[pbs.dst_factor_alpha],
			VK_BLEND_OP_ADD, p.cms.wrgba);
	}
	else
	{
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, p.cms.wrgba);
	}

	// Tests have shown that it's faster to just enable rast order on the entire pass, rather than alternating
	// between turning it on and off for different draws, and adding the required barrier between non-rast-order
	// and rast-order draws.
	if (m_features.framebuffer_fetch && p.IsRTFeedbackLoop())
		gpb.AddBlendFlags(VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT);

	return gpb.Create(vk_init_info.device, g_vulkan_shader_cache->GetPipelineCache(true));
}

VkPipeline GSDeviceVK::GetTFXPipeline(const PipelineSelector& p)
{
	const auto it = m_tfx_pipelines.find(p);
	if (it != m_tfx_pipelines.end())
		return it->second;

	VkPipeline pipeline = CreateTFXPipeline(p);
	m_tfx_pipelines.emplace(p, pipeline);
	return pipeline;
}

bool GSDeviceVK::BindDrawPipeline(const PipelineSelector& p)
{
	VkPipeline pipeline = GetTFXPipeline(p);
	if (pipeline == VK_NULL_HANDLE)
		return false;

	SetPipeline(pipeline);

	return ApplyTFXState();
}

void GSDeviceVK::InitializeState()
{
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_pass = VK_NULL_HANDLE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();

	m_utility_texture = m_null_texture.get();

	m_point_sampler   = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler  = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_tfx_sampler     = m_point_sampler;

	InvalidateCachedState();
	SetInitialState(m_current_command_buffer);
}

bool GSDeviceVK::CreatePersistentDescriptorSets()
{
	Vulkan::DescriptorSetUpdateBuilder dsub;

	// Allocate UBO descriptor sets for TFX.
	m_tfx_ubo_descriptor_set = AllocatePersistentDescriptorSet(m_tfx_ubo_ds_layout);
	if (m_tfx_ubo_descriptor_set == VK_NULL_HANDLE)
		return false;
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_vertex_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::VSConstantBuffer));
	dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		m_fragment_uniform_stream_buffer.GetBuffer(), 0, sizeof(GSHWDrawConfig::PSConstantBuffer));
	if (m_features.vs_expand)
		dsub.AddBufferDescriptorWrite(m_tfx_ubo_descriptor_set, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			m_vertex_stream_buffer.GetBuffer(), 0, VERTEX_BUFFER_SIZE);
	dsub.Update(vk_init_info.device);
	return true;
}

void GSDeviceVK::ExecuteCommandBuffer(bool wait_for_completion)
{
	EndRenderPass();

	if (m_last_submit_failed)
		return;

	{
		// If we're waiting for completion, don't bother waking the worker thread.
		const u32 current_frame = m_current_frame;
		SubmitCommandBuffer();
		MoveToNextCommandBuffer();

		if (wait_for_completion)
			WaitForCommandBufferCompletion(current_frame);
	}
}

void GSDeviceVK::ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion)
{
	const VkRenderPass render_pass = m_current_render_pass;
	const GSVector4i render_pass_area = m_current_render_pass_area;
	const GSVector4i scissor = m_scissor;
	GSTexture* const current_rt = m_current_render_target;
	GSTexture* const current_ds = m_current_depth_target;
	const FeedbackLoopFlag current_feedback_loop = m_current_framebuffer_feedback_loop;

	EndRenderPass();
	ExecuteCommandBuffer(wait_for_completion);

	if (render_pass != VK_NULL_HANDLE)
	{
		// rebind framebuffer
		OMSetRenderTargets(current_rt, current_ds, scissor, current_feedback_loop);

		// restart render pass
		BeginRenderPass(GetRenderPassForRestarting(render_pass), render_pass_area);
	}
}

void GSDeviceVK::InvalidateCachedState()
{
	m_dirty_flags |= ALL_DIRTY_STATE;

	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();
	m_utility_texture = m_null_texture.get();
	m_current_framebuffer = VK_NULL_HANDLE;
	m_current_render_target = nullptr;
	m_current_depth_target = nullptr;
	m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;

	m_current_pipeline_layout    = PipelineLayout::Undefined;
	m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	m_tfx_rt_descriptor_set      = VK_NULL_HANDLE;
	m_utility_descriptor_set     = VK_NULL_HANDLE;
}

void GSDeviceVK::SetIndexBuffer(VkBuffer buffer)
{
	if (m_index_buffer == buffer)
		return;

	m_index_buffer = buffer;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceVK::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceVK::SetLineWidth(float width)
{
	if (m_current_line_width == width)
		return;

	m_current_line_width = width;
	m_dirty_flags |= DIRTY_FLAG_LINE_WIDTH;
}

void GSDeviceVK::PSSetShaderResource(int i, GSTexture* sr, bool check_state)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(sr);
	if (vkTex)
	{
		if (check_state)
		{
			if (vkTex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly && InRenderPass())
				EndRenderPass();

			vkTex->CommitClear();
			vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}
		vkTex->m_use_fence_counter = GetCurrentFenceCounter();
	}
	else
		vkTex = m_null_texture.get();

	if (m_tfx_textures[i] == vkTex)
		return;

	m_tfx_textures[i] = vkTex;

	m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
}

void GSDeviceVK::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	m_tfx_sampler_sel = sel.key;
	m_tfx_sampler = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_SAMPLERS_DS;
}

void GSDeviceVK::SetUtilityTexture(GSTexture* tex, VkSampler sampler)
{
	GSTextureVK* vkTex = static_cast<GSTextureVK*>(tex);
	if (vkTex)
	{
		vkTex->CommitClear();
		vkTex->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		vkTex->m_use_fence_counter = GetCurrentFenceCounter();
	}
	else
		vkTex = m_null_texture.get();

	if (m_utility_texture == vkTex && m_utility_sampler == sampler)
		return;

	m_utility_texture = vkTex;
	m_utility_sampler = sampler;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceVK::SetUtilityPushConstants(const void* data, u32 size)
{
	vkCmdPushConstants(GetCurrentCommandBuffer(), m_utility_pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void GSDeviceVK::UnbindTexture(GSTextureVK* tex)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex)
		{
			m_tfx_textures[i] = m_null_texture.get();
			m_dirty_flags |= (i < 2) ? DIRTY_FLAG_TFX_SAMPLERS_DS : DIRTY_FLAG_TFX_RT_TEXTURE_DS;
		}
	}
	if (m_utility_texture == tex)
	{
		m_utility_texture = m_null_texture.get();
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (m_current_render_target == tex || m_current_depth_target == tex)
	{
		EndRenderPass();
		m_current_framebuffer = VK_NULL_HANDLE;
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;
	}
}

bool GSDeviceVK::InRenderPass() { return m_current_render_pass != VK_NULL_HANDLE; }

void GSDeviceVK::BeginRenderPass(VkRenderPass rp, const GSVector4i& rect)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}}, 0,
		nullptr};

	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count)
{
	if (m_current_render_pass != VK_NULL_HANDLE)
		EndRenderPass();

	m_current_render_pass = rp;
	m_current_render_pass_area = rect;

	const VkRenderPassBeginInfo begin_info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, m_current_render_pass,
		m_current_framebuffer, {{rect.x, rect.y}, {static_cast<u32>(rect.width()), static_cast<u32>(rect.height())}},
		cv_count, cv};

	vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color)
{
	alignas(16) VkClearValue cv;
	GSVector4::store<true>((void*)cv.color.float32, GSVector4::unorm8(clear_color));
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil)
{
	VkClearValue cv;
	cv.depthStencil.depth = depth;
	cv.depthStencil.stencil = stencil;
	BeginClearRenderPass(rp, rect, &cv, 1);
}

void GSDeviceVK::EndRenderPass()
{
	if (m_current_render_pass == VK_NULL_HANDLE)
		return;

	m_current_render_pass = VK_NULL_HANDLE;

	vkCmdEndRenderPass(GetCurrentCommandBuffer());
}

void GSDeviceVK::SetViewport(const VkViewport& viewport)
{
	if (std::memcmp(&viewport, &m_viewport, sizeof(VkViewport)) == 0)
		return;

	memcpy(&m_viewport, &viewport, sizeof(VkViewport));
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceVK::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceVK::SetPipeline(VkPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

void GSDeviceVK::SetInitialState(VkCommandBuffer cmdbuf)
{
	const VkDeviceSize buffer_offset = 0;
	vkCmdBindVertexBuffers(cmdbuf, 0, 1, m_vertex_stream_buffer.GetBufferPtr(), &buffer_offset);
}

__ri void GSDeviceVK::ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf)
{
	if (flags & DIRTY_FLAG_INDEX_BUFFER)
		vkCmdBindIndexBuffer(cmdbuf, m_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	if (flags & DIRTY_FLAG_PIPELINE)
		vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline);

	if (flags & DIRTY_FLAG_VIEWPORT)
		vkCmdSetViewport(cmdbuf, 0, 1, &m_viewport);

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const VkRect2D vscissor{
			{m_scissor.x, m_scissor.y}, {static_cast<u32>(m_scissor.width()), static_cast<u32>(m_scissor.height())}};
		vkCmdSetScissor(cmdbuf, 0, 1, &vscissor);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const GSVector4 col(static_cast<float>(m_blend_constant_color) / 128.0f);
		vkCmdSetBlendConstants(cmdbuf, col.v);
	}

	if (flags & DIRTY_FLAG_LINE_WIDTH)
		vkCmdSetLineWidth(cmdbuf, m_current_line_width);
}

bool GSDeviceVK::ApplyTFXState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0)
		return true;

	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE | DIRTY_FLAG_TFX_UBO);

	// do cbuffer first, because it's the most likely to cause an exec
	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(
				sizeof(m_vs_cb_cache), GetUniformBufferAlignment()))
		{
			if (already_execed)
			{
				Console.Error("Failed to reserve vertex uniform space");
				return false;
			}

			/* Ran out of vertex uniform space */
			ExecuteCommandBufferAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(
				sizeof(m_ps_cb_cache), GetUniformBufferAlignment()))
		{
			if (already_execed)
			{
				Console.Error("Failed to reserve pixel uniform space");
				return false;
			}

			/* Ran out of pixel uniform space */
			ExecuteCommandBufferAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
		m_fragment_uniform_stream_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	Vulkan::DescriptorSetUpdateBuilder dsub;

	VkDescriptorSet dsets[NUM_TFX_DESCRIPTOR_SETS];
	u32 num_dsets = 0;
	u32 start_dset = 0;
	const bool layout_changed = (m_current_pipeline_layout != PipelineLayout::TFX);

	if (!layout_changed && flags & DIRTY_FLAG_TFX_UBO)
		dsets[num_dsets++] = m_tfx_ubo_descriptor_set;

	if ((flags & DIRTY_FLAG_TFX_SAMPLERS_DS) || m_tfx_texture_descriptor_set == VK_NULL_HANDLE)
	{
		m_tfx_texture_descriptor_set = AllocateDescriptorSet(m_tfx_sampler_ds_layout);
		if (m_tfx_texture_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				Console.Error("Failed to allocate TFX texture descriptors");
				return false;
			}

			/* Ran out of TFX texture descriptors */
			ExecuteCommandBufferAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		dsub.AddCombinedImageSamplerDescriptorWrite(
			m_tfx_texture_descriptor_set, 0, m_tfx_textures[0]->GetView(), m_tfx_sampler, m_tfx_textures[0]->GetVkLayout());
		dsub.AddImageDescriptorWrite(m_tfx_texture_descriptor_set, 1, m_tfx_textures[1]->GetView(), m_tfx_textures[1]->GetVkLayout());
		dsub.Update(vk_init_info.device);

		if (!layout_changed)
		{
			start_dset = (num_dsets == 0) ?
				TFX_DESCRIPTOR_SET_TEXTURES : start_dset;
			dsets[num_dsets++] = m_tfx_texture_descriptor_set;
		}
	}

	if ((flags & DIRTY_FLAG_TFX_RT_TEXTURE_DS) || m_tfx_rt_descriptor_set == VK_NULL_HANDLE)
	{
		m_tfx_rt_descriptor_set = AllocateDescriptorSet(m_tfx_rt_texture_ds_layout);
		if (m_tfx_rt_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				Console.Error("Failed to allocate TFX sampler descriptors");
				return false;
			}

			/* Ran out of TFX sampler descriptors */
			ExecuteCommandBufferAndRestartRenderPass(false);
			return ApplyTFXState(true);
		}

		if (m_features.texture_barrier)
		{
			dsub.AddInputAttachmentDescriptorWrite(
				m_tfx_rt_descriptor_set, 0, m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetView(), VK_IMAGE_LAYOUT_GENERAL);
		}
		else
		{
			dsub.AddImageDescriptorWrite(m_tfx_rt_descriptor_set, 0, m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetView(),
				m_tfx_textures[NUM_TFX_DRAW_TEXTURES]->GetVkLayout());
		}
		dsub.AddImageDescriptorWrite(m_tfx_rt_descriptor_set, 1, m_tfx_textures[NUM_TFX_DRAW_TEXTURES + 1]->GetView(),
			m_tfx_textures[NUM_TFX_DRAW_TEXTURES + 1]->GetVkLayout());
		dsub.Update(vk_init_info.device);

		if (!layout_changed)
		{
			// need to add textures in, can't leave a gap
			if (start_dset == TFX_DESCRIPTOR_SET_UBO && num_dsets == 1)
				dsets[num_dsets++] = m_tfx_texture_descriptor_set;
			else
				start_dset = (num_dsets == 0) ? TFX_DESCRIPTOR_SET_RT : start_dset;

			dsets[num_dsets++] = m_tfx_rt_descriptor_set;
		}
	}

	if (layout_changed)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;

		dsets[0] = m_tfx_ubo_descriptor_set;
		dsets[1] = m_tfx_texture_descriptor_set;
		dsets[2] = m_tfx_rt_descriptor_set;

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, 0,
			NUM_TFX_DESCRIPTOR_SETS, dsets, NUM_TFX_DYNAMIC_OFFSETS,
			m_tfx_dynamic_offsets.data());
	}
	else if (num_dsets > 0)
	{
		u32 dynamic_count;
		const u32* dynamic_offsets;
		if (start_dset == TFX_DESCRIPTOR_SET_UBO)
		{
			dynamic_count   = NUM_TFX_DYNAMIC_OFFSETS;
			dynamic_offsets = m_tfx_dynamic_offsets.data();
		}
		else
		{
			dynamic_count = 0;
			dynamic_offsets = nullptr;
		}

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tfx_pipeline_layout, start_dset, num_dsets,
			dsets, dynamic_count,
			dynamic_offsets);
	}


	ApplyBaseState(flags, cmdbuf);
	return true;
}

bool GSDeviceVK::ApplyUtilityState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	const VkDevice dev           = vk_init_info.device;
	const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
	u32 flags                    = m_dirty_flags;
	m_dirty_flags               &= ~DIRTY_UTILITY_STATE;

	bool rebind                  = (m_current_pipeline_layout != PipelineLayout::Utility);

	if ((flags & DIRTY_FLAG_UTILITY_TEXTURE) || m_utility_descriptor_set == VK_NULL_HANDLE)
	{
		m_utility_descriptor_set = AllocateDescriptorSet(m_utility_ds_layout);
		if (m_utility_descriptor_set == VK_NULL_HANDLE)
		{
			if (already_execed)
			{
				Console.Error("Failed to allocate utility descriptors");
				return false;
			}

			/* Ran out of utility descriptors */
			ExecuteCommandBufferAndRestartRenderPass(false);
			return ApplyUtilityState(true);
		}

		Vulkan::DescriptorSetUpdateBuilder dsub;
		dsub.AddCombinedImageSamplerDescriptorWrite(m_utility_descriptor_set, 0, m_utility_texture->GetView(),
			m_utility_sampler, m_utility_texture->GetVkLayout());
		dsub.Update(dev);
		rebind = true;
	}

	if (rebind)
	{
		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_utility_pipeline_layout, 0, 1,
			&m_utility_descriptor_set, 0, nullptr);
	}

	m_current_pipeline_layout = PipelineLayout::Utility;

	ApplyBaseState(flags, cmdbuf);
	return true;
}

void GSDeviceVK::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

void GSDeviceVK::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows
	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(m_convert[SetDATMShader(datm)]);
	BeginClearRenderPass(m_date_setup_render_pass, bbox, 0.0f, 0);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTextureVK* GSDeviceVK::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	// How this is done:
	// - can't put a barrier for the image in the middle of the normal render pass, so that's out
	// - so, instead of just filling the int texture with INT_MAX, we sample the RT and use -1 for failing values
	// - then, instead of sampling the RT with DATE=1/2, we just do a min() without it, the -1 gets preserved
	// - then, the DATE=3 draw is done as normal
	const GSVector2i rtsize(config.rt->GetSize());
	GSTextureVK* image =
		static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	// setup the fill quad to prefill with existing alpha values
	SetUtilityTexture(config.rt, m_point_sampler);
	OMSetRenderTargets(image, config.ds, config.drawarea);

	// if the depth target has been cleared, we need to preserve that clear
	const VkAttachmentLoadOp ds_load_op = GetLoadOpForTexture(static_cast<GSTextureVK*>(config.ds));
	const u32 ds = (config.ds ? 1 : 0);

	if (ds_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
	{
		VkClearValue cv[2] = {};
		cv[1].depthStencil.depth = static_cast<GSTextureVK*>(config.ds)->GetClearDepth();
		cv[1].depthStencil.stencil = 1;
		BeginClearRenderPass(m_date_image_setup_render_passes[ds][1], GSVector4i::loadh(rtsize), cv, 2);
	}
	else
	{
		BeginRenderPass(m_date_image_setup_render_passes[ds][0], config.drawarea);
	}

	// draw the quad to prefill the image
	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	const VkPipeline pipeline = m_date_image_setup_pipelines[ds][static_cast<u8>(config.datm)];
	SetPipeline(pipeline);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	// image is now filled with either -1 or INT_MAX, so now we can do the prepass
	UploadHWDrawVerticesAndIndices(config);

	// primid texture will get re-bound, so clear it since we're using push descriptors
	PSSetShaderResource(3, m_null_texture.get(), false);

	// cut down the configuration for the prepass, we don't need blending or any feedback loop
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	pipe.dss.zwe = false;
	pipe.cms.wrgba = 0;
	pipe.bs = {};
	pipe.feedback_loop_flags = FeedbackLoopFlag_None;
	pipe.rt = true;
	pipe.ps.blend_a = pipe.ps.blend_b = pipe.ps.blend_c = pipe.ps.blend_d = false;
	pipe.ps.no_color = false;
	pipe.ps.no_color1 = true;
	if (BindDrawPipeline(pipe))
		DrawIndexedPrimitive();

	// image is initialized/prepass is done, so finish up and get ready to do the "real" draw
	EndRenderPass();

	// .. by setting it to DATE=3
	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	// and bind the image to the primitive sampler
	image->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	PSSetShaderResource(3, image, false);
	return image;
}

void GSDeviceVK::RenderHW(GSHWDrawConfig& config)
{
	// Destination Alpha Setup
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::Full: // No setup
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking: // Setup is done below
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne: // setup is done below
		{
			// we only need to do the setup here if we don't have barriers, in which case do full DATE.
			if (!m_features.texture_barrier)
			{
				SetupDATE(config.rt, config.ds, config.datm, config.drawarea);
				config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
			}
		}
		break;

		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(config.rt, config.ds, config.datm, config.drawarea);
			break;
	}

	// stream buffer in first, in case we need to exec
	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	// bind textures before checking the render pass, in case we need to transition them
	if (config.tex)
	{
		PSSetShaderResource(0, config.tex, config.tex != config.rt && config.tex != config.ds);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(1, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	if (config.topology == GSHWDrawConfig::Topology::Line)
		SetLineWidth(config.line_expand ? config.cb_ps.ScaleFactor.z : 1.0f);

	// Primitive ID tracking DATE setup.
	GSTextureVK* date_image = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		date_image = SetupPrimitiveTrackingDATE(config);
		if (!date_image)
			return;
	}

	// figure out the pipeline
	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);

	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());

	GSTextureVK* draw_rt = static_cast<GSTextureVK*>(config.rt);
	GSTextureVK* draw_ds = static_cast<GSTextureVK*>(config.ds);
	GSTextureVK* draw_rt_clone = nullptr;
	GSTextureVK* hdr_rt = nullptr;

	// Switch to hdr target for colclip rendering
	if (pipe.ps.hdr)
	{
		EndRenderPass();

		hdr_rt = static_cast<GSTextureVK*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::HDRColor, false));
		if (!hdr_rt)
		{
			if (date_image)
				Recycle(date_image);
			return;
		}

		// propagate clear value through if the hdr render is the first
		if (draw_rt->GetState() == GSTexture::State::Cleared)
		{
			hdr_rt->SetState(GSTexture::State::Cleared);
			hdr_rt->SetClearColor(draw_rt->GetClearColor());

			// If depth is cleared, we need to commit it, because we're only going to draw to the active part of the FB.
			if (draw_ds && draw_ds->GetState() == GSTexture::State::Cleared && !config.drawarea.eq(GSVector4i::loadh(rtsize)))
				draw_ds->CommitClear(m_current_command_buffer);
		}
		else if (draw_rt->GetState() == GSTexture::State::Dirty)
		{
			draw_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
		}

		// we're not drawing to the RT, so we can use it as a source
		if (config.require_one_barrier && !m_features.texture_barrier)
			PSSetShaderResource(2, draw_rt, true);

		draw_rt = hdr_rt;
	}
	else if (config.require_one_barrier && !m_features.texture_barrier)
	{
		// requires a copy of the RT
		draw_rt_clone = static_cast<GSTextureVK*>(CreateTexture(rtsize.x, rtsize.y, 1, GSTexture::Format::Color, true));
		if (draw_rt_clone)
		{
			EndRenderPass();

			CopyRect(draw_rt, draw_rt_clone, config.drawarea, config.drawarea.left, config.drawarea.top);
			PSSetShaderResource(2, draw_rt_clone, true);
		}
	}

	// clear texture binding when it's bound to RT or DS
	if (!config.tex &&
		((!pipe.IsRTFeedbackLoop() && static_cast<GSTextureVK*>(config.rt) == m_tfx_textures[0]) ||
			(config.ds && static_cast<GSTextureVK*>(config.ds) == m_tfx_textures[0])))
	{
		PSSetShaderResource(0, nullptr, false);
	}

	// render pass restart optimizations
	if (hdr_rt)
	{
		// HDR requires blitting.
		EndRenderPass();
	}
	else if (InRenderPass() && (m_current_render_target == draw_rt || m_current_depth_target == draw_ds))
	{
		// avoid restarting the render pass just to switch from rt+depth to rt and vice versa
		// keep the depth even if doing HDR draws, because the next draw will probably re-enable depth
		if (!draw_rt && m_current_render_target && config.tex != m_current_render_target &&
			m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			m_pipeline_selector.rt = true;
		}
		else if (!draw_ds && m_current_depth_target && config.tex != m_current_depth_target &&
				 m_current_depth_target->GetSize() == draw_rt->GetSize())
		{
			draw_ds = m_current_depth_target;
			m_pipeline_selector.ds = true;
		}

		// Prefer keeping feedback loop enabled, that way we're not constantly restarting render passes
		pipe.feedback_loop_flags |= m_current_framebuffer_feedback_loop;
	}

	// We don't need the very first barrier if this is the first draw after switching to feedback loop,
	// because the layout change in itself enforces the execution dependency. HDR needs a barrier between
	// setup and the first draw to read it. TODO: Make HDR use subpasses instead.
	
	// However, it turns out *not* doing this causes GPU resets on RDNA3, specifically Windows drivers.
	// Despite the layout changing enforcing the execution dependency between previous draws and the first
	// input attachment read, it still wants the region/fragment-local barrier...
	
	const bool skip_first_barrier = 
		(draw_rt && draw_rt->GetLayout() != GSTextureVK::Layout::FeedbackLoop && !pipe.ps.hdr && !IsDeviceAMD());

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));
	if (pipe.IsRTFeedbackLoop())
	{
		PSSetShaderResource(2, draw_rt, false);

		// If this is the first draw to the target as a feedback loop, make sure we re-generate the texture descriptor.
		// Otherwise, we might have a previous descriptor left over, that has the RT in a different state.
		m_dirty_flags |= (skip_first_barrier ? DIRTY_FLAG_TFX_RT_TEXTURE_DS : 0);
	}

	// Begin render pass if new target or out of the area.
	if (!InRenderPass())
	{
		const VkAttachmentLoadOp rt_op = GetLoadOpForTexture(draw_rt);
		const VkAttachmentLoadOp ds_op = GetLoadOpForTexture(draw_ds);
		const VkRenderPass rp = GetTFXRenderPass(pipe.rt, pipe.ds, pipe.ps.hdr, config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil , pipe.IsRTFeedbackLoop(),
			pipe.IsTestingAndSamplingDepth(), rt_op, ds_op);
		const bool is_clearing_rt = (rt_op == VK_ATTACHMENT_LOAD_OP_CLEAR || ds_op == VK_ATTACHMENT_LOAD_OP_CLEAR);

		// Only draw to the active area of the HDR target. Except when depth is cleared, we need to use the full
		// buffer size, otherwise it'll only clear the draw part of the depth buffer.
		const GSVector4i render_area = (pipe.ps.hdr && ds_op != VK_ATTACHMENT_LOAD_OP_CLEAR) ? config.drawarea :
																							   GSVector4i::loadh(rtsize);

		if (is_clearing_rt)
		{
			// when we're clearing, we set the draw area to the whole fb, otherwise part of it will be undefined
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			if (draw_rt)
			{
				GSVector4 clear_color = draw_rt->GetUNormClearColor();
				if (pipe.ps.hdr)
				{
					// Denormalize clear color for HDR.
					clear_color *= GSVector4::cxpr(255.0f / 65535.0f, 255.0f / 65535.0f, 255.0f / 65535.0f, 1.0f);
				}
				GSVector4::store<true>(&cvs[cv_count++].color, clear_color);
			}

			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 0};

			BeginClearRenderPass(
				rp, render_area, cvs, cv_count);
		}
		else
		{
			BeginRenderPass(rp, render_area);
		}
	}
	
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
	{
		VkClearAttachment ca;
		const VkClearRect rc = {{{config.drawarea.left, config.drawarea.top},
									{static_cast<u32>(config.drawarea.width()), static_cast<u32>(config.drawarea.height())}},
			0u, 1u};
		ca.aspectMask              = VK_IMAGE_ASPECT_STENCIL_BIT;
		ca.colorAttachment         = 0u;
		ca.clearValue.depthStencil = {0.0f, 1u};
		vkCmdClearAttachments(m_current_command_buffer, 1, &ca, 1, &rc);
	}

	// rt -> hdr blit if enabled
	if (hdr_rt && config.rt->GetState() == GSTexture::State::Dirty)
	{
		SetUtilityTexture(static_cast<GSTextureVK*>(config.rt), m_point_sampler);
		SetPipeline(m_hdr_setup_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
		
		const GSVector4 drawareaf = GSVector4(config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		DrawStretchRect(sRect, drawareaf, rtsize);
	}

	// VB/IB upload, if we did DATE setup and it's not HDR this has already been done
	if (!date_image || hdr_rt)
		UploadHWDrawVerticesAndIndices(config);

	// now we can do the actual draw
	if (BindDrawPipeline(pipe))
		SendHWDraw(config, draw_rt, skip_first_barrier);

	// blend second pass
	if (config.blend_second_pass.enable)
	{
		if (config.blend_second_pass.blend.constant_enable)
			SetBlendConstants(config.blend_second_pass.blend.constant);

		pipe.bs = config.blend_second_pass.blend;
		pipe.ps.blend_hw = config.blend_second_pass.blend_hw;
		pipe.ps.dither = config.blend_second_pass.dither;
		if (BindDrawPipeline(pipe))
			DrawIndexedPrimitive();
	}

	// and the alpha pass
	if (config.alpha_second_pass.enable)
	{
		// cbuffer will definitely be dirty if aref changes, no need to check it
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.cms = config.alpha_second_pass.colormask;
		pipe.dss = config.alpha_second_pass.depth;
		pipe.bs = config.blend;
		if (BindDrawPipeline(pipe))
			SendHWDraw(config, draw_rt, false);
	}

	if (draw_rt_clone)
		Recycle(draw_rt_clone);

	if (date_image)
		Recycle(date_image);

	// now blit the hdr texture back to the original target
	if (hdr_rt)
	{
		EndRenderPass();
		hdr_rt->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);

		draw_rt = static_cast<GSTextureVK*>(config.rt);
		OMSetRenderTargets(draw_rt, draw_ds, config.scissor, static_cast<FeedbackLoopFlag>(pipe.feedback_loop_flags));

		// if this target was cleared and never drawn to, perform the clear as part of the resolve here.
		if (draw_rt->GetState() == GSTexture::State::Cleared)
		{
			alignas(16) VkClearValue cvs[2];
			u32 cv_count = 0;
			GSVector4::store<true>(&cvs[cv_count++].color, draw_rt->GetUNormClearColor());
			if (draw_ds)
				cvs[cv_count++].depthStencil = {draw_ds->GetClearDepth(), 1};

			BeginClearRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
									 pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_CLEAR,
									 pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
				draw_rt->GetRect(), cvs, cv_count);
			draw_rt->SetState(GSTexture::State::Dirty);
		}
		else
		{
			BeginRenderPass(GetTFXRenderPass(true, pipe.ds, false, false, pipe.IsRTFeedbackLoop(),
								pipe.IsTestingAndSamplingDepth(), VK_ATTACHMENT_LOAD_OP_LOAD,
								pipe.ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
				draw_rt->GetRect());
		}

		const GSVector4 drawareaf = GSVector4(config.drawarea);
		const GSVector4 sRect(drawareaf / GSVector4(rtsize).xyxy());
		SetPipeline(m_hdr_finish_pipelines[pipe.ds][pipe.IsRTFeedbackLoop()]);
		SetUtilityTexture(hdr_rt, m_point_sampler);
		DrawStretchRect(sRect, drawareaf, rtsize);

		Recycle(hdr_rt);
	}
}

void GSDeviceVK::UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	pipe.vs.key = config.vs.key;
	pipe.ps.key_hi = config.ps.key_hi;
	pipe.ps.key_lo = config.ps.key_lo;
	pipe.dss.key = config.depth.key;
	pipe.bs.key = config.blend.key;
	pipe.bs.constant = 0; // don't dupe states with different alpha values
	pipe.cms.key = config.colormask.key;
	pipe.topology = static_cast<u32>(config.topology);
	pipe.rt = config.rt != nullptr;
	pipe.ds = config.ds != nullptr;
	pipe.line_width = config.line_expand;
	pipe.feedback_loop_flags =
		(m_features.texture_barrier &&
			(config.ps.IsFeedbackLoop() || config.require_one_barrier || config.require_full_barrier)) ?
			FeedbackLoopFlag_ReadAndWriteRT :
			FeedbackLoopFlag_None;
	pipe.feedback_loop_flags |=
		(config.tex && config.tex == config.ds) ? FeedbackLoopFlag_ReadDS : FeedbackLoopFlag_None;

	// enable point size in the vertex shader if we're rendering points regardless of upscaling.
	pipe.vs.point_size |= (config.topology == GSHWDrawConfig::Topology::Point);
}

void GSDeviceVK::UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts);

	if (config.vs.UseExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}
}

VkImageMemoryBarrier GSDeviceVK::GetColorBufferBarrier(GSTextureVK* rt) const
{
	return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rt->GetImage(), {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
}

void GSDeviceVK::SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, bool skip_first_barrier)
{
	if (config.drawlist)
	{
		const u32 indices_per_prim = config.indices_per_prim;
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
		const VkImageMemoryBarrier barrier = GetColorBufferBarrier(draw_rt);
		u32 p = 0;
		u32 n = 0;

		if (skip_first_barrier)
		{
			const u32 count = (*config.drawlist)[n] * indices_per_prim;
			DrawIndexedPrimitive(p, count);
			p += count;
			++n;
		}

		for (; n < draw_list_size; n++)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
				nullptr, 0, nullptr, 1, &barrier);

			const u32 count = (*config.drawlist)[n] * indices_per_prim;
			DrawIndexedPrimitive(p, count);
			p += count;
		}

		return;
	}

	if (m_features.texture_barrier && m_pipeline_selector.ps.IsFeedbackLoop())
	{
		const VkImageMemoryBarrier barrier    = GetColorBufferBarrier(draw_rt);

		if (config.require_full_barrier)
		{
			const u32 indices_per_prim = config.indices_per_prim;

			u32 p = 0;
			if (skip_first_barrier)
			{
				DrawIndexedPrimitive(p, indices_per_prim);
				p += indices_per_prim;
			}

			for (; p < config.nindices; p += indices_per_prim)
			{
				vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT,
					0, nullptr, 0, nullptr, 1, &barrier);

				DrawIndexedPrimitive(p, indices_per_prim);
			}

			return;
		}

		if (config.require_one_barrier && !skip_first_barrier)
		{
			vkCmdPipelineBarrier(GetCurrentCommandBuffer(),
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0,
				nullptr, 0, nullptr, 1, &barrier);
		}
	}

	DrawIndexedPrimitive();
}
