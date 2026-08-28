// SPDX-FileCopyrightText: 2024 Hans-Kristian Arntzen
// SPDX-License-Identifier: LGPL-3.0+

#pragma once

#include "../Vulkan/VKLoaderPlatformDefines.h"
#include "SaveState.h"
#include "Config.h"
#include "common/WindowInfo.h"
#include "gs_interface.hpp"
#include "device.hpp"
#include "context.hpp"
#include "wsi.hpp"

#include <libretro_vulkan.h>

class GSRendererPGS final
{
public:
	explicit GSRendererPGS(u8 *basemem);
	~GSRendererPGS();

	bool Init();
	void Reset(bool hardware_reset);

	void Transfer(const u8 *mem, u32 size);

	void VSync(u32 field, bool registers_written);
	inline ParallelGS::GSInterface &get_interface() { return iface; };
	void ReadFIFO(u8 *mem, u32 size);

	void UpdateConfig();

	void GetInternalResolution(int *width, int *height);

	int Freeze(freezeData *data, bool sizeonly);
	int Defrost(freezeData *data);

	u8 *GetRegsMem();

private:
	ParallelGS::PrivRegisterState *priv;
	Vulkan::Device dev;
	ParallelGS::GSInterface iface;
	WindowInfo window_info = {};

	ParallelGS::SuperSampling current_super_sampling = ParallelGS::SuperSampling::X1;
	bool current_ordered_super_sampling     = false;
	bool current_super_sample_textures      = false;
	bool has_presented_in_current_swapchain = false;
	uint32_t last_internal_width = 0;
	uint32_t last_internal_height = 0;

	static int GetSaveStateSize();
};

// libretro integration.
void pgs_set_hwrender_interface(retro_hw_render_interface_vulkan *iface);

const VkApplicationInfo *pgs_get_application_info();
bool pgs_create_device(retro_vulkan_context *context,
	VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const char **required_device_extensions, unsigned num_required_device_extensions,
	const char **required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures *required_features);
void pgs_destroy_device();
VkInstance pgs_create_instance(PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                               const VkApplicationInfo *app,
                               retro_vulkan_create_instance_wrapper_t create_instance_wrapper,
                               void *opaque);
bool pgs_create_device2(
	struct retro_vulkan_context *context,
	VkInstance instance,
	VkPhysicalDevice gpu,
	VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper,
	void *opaque);
