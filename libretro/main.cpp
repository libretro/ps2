#ifdef WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <libretro.h>
#include <streams/file_stream.h>
#include <string>
#include <vector>
#include <type_traits>
#include <thread>

#include "libretro_core_options.h"
#include "GS.h"
#include "SPU2/Global.h"
#include "ps2/BiosTools.h"
#include "CDVD/CDVD.h"
#include "MTVU.h"
#include "Counters.h"
#include "Host.h"
#include "common/Path.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#ifdef ENABLE_VULKAN
#ifdef HAVE_PARALLEL_GS
#include "GS/Renderers/parallel-gs/GSRendererPGS.h"
#endif
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include <libretro_vulkan.h>
#endif
#include "pcsx2/Frontend/InputManager.h"
#include "pcsx2/Frontend/LayeredSettingsInterface.h"
#include "pcsx2/VMManager.h"

#include "SPU2/spu2.h"
#include "PAD/PAD.h"

//#define PERF_TEST
#ifdef PERF_TEST
static struct retro_perf_callback perf_cb;

#define RETRO_PERFORMANCE_INIT(name)                 \
	retro_perf_tick_t current_ticks;                 \
	static struct retro_perf_counter name = {#name}; \
	if (!name.registered)                            \
		perf_cb.perf_register(&(name));              \
	current_ticks = name.total

#define RETRO_PERFORMANCE_START(name) perf_cb.perf_start(&(name))
#define RETRO_PERFORMANCE_STOP(name) \
	perf_cb.perf_stop(&(name));      \
	current_ticks = name.total - current_ticks;
#else
#define RETRO_PERFORMANCE_INIT(name)
#define RETRO_PERFORMANCE_START(name)
#define RETRO_PERFORMANCE_STOP(name)
#endif

retro_environment_t environ_cb;
retro_video_refresh_t video_cb;
struct retro_hw_render_callback hw_render;
static retro_audio_sample_batch_t batch_cb;
retro_log_printf_t log_cb;
retro_audio_sample_t sample_cb;

static VMState cpu_thread_state;
MemorySettingsInterface s_settings_interface;
static std::thread cpu_thread;

struct BiosInfo {
	std::string filename;
	std::string description;
};

static std::vector<BiosInfo> bios_info;
static std::string bios;
static std::string renderer;
static int upscale_multiplier;
static int pgs_super_sampling;
static int pgs_high_res_scanout;
static int pgs_disable_mipmaps;
static int axis_scale1;
static int axis_scale2;
static bool fast_boot;
static bool mipmapping;
float pad_axis_scale[2];

static bool show_parallel_options = true;
static bool show_gsdx_hw_only_options = true;
static bool show_gsdx_options = true;

static bool update_option_visibility()
{
	struct retro_core_option_display option_display;
	struct retro_variable var;
	bool updated = false;

	// Show/hide video options
	bool show_parallel_options_prev = show_parallel_options;
	bool show_gsdx_hw_only_options_prev = show_gsdx_hw_only_options;
	bool show_gsdx_options_prev = show_gsdx_options;
	show_parallel_options = true;
	show_gsdx_hw_only_options = true;
	show_gsdx_options = true;

	var.key = "pcsx2_renderer";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool parallel_renderer = !strcmp(var.value, "paraLLEl-GS");
		const bool software_renderer = !strcmp(var.value, "Software");
		const bool null_renderer = !strcmp(var.value, "Null");

		if (parallel_renderer || null_renderer)
			show_gsdx_options = false;
		if (parallel_renderer || software_renderer || null_renderer)
			show_gsdx_hw_only_options = false;
		if (!parallel_renderer)
			show_parallel_options = false;
	}

	if (show_parallel_options != show_parallel_options_prev)
	{
		option_display.visible = show_parallel_options;
		option_display.key = "pcsx2_pgs_ssaa";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key = "pcsx2_pgs_high_res_scanout";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key = "pcsx2_pgs_disable_mipmaps";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated = true;
	}

	if (show_gsdx_hw_only_options != show_gsdx_hw_only_options_prev)
	{
		option_display.visible = show_gsdx_hw_only_options;
		option_display.key = "pcsx2_upscale_multiplier";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated = true;
	}

	if (show_gsdx_options != show_gsdx_options_prev)
	{
		option_display.visible = show_gsdx_options;
		option_display.key = "pcsx2_mipmapping";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated = true;
	}

	return updated;
}

static void check_variables(bool first_run)
{
	struct retro_variable var;
	int i_prev;

	if (first_run)
	{
		var.key = "pcsx2_bios";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bios = var.value;
		}

		var.key = "pcsx2_fastboot";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "enabled"))
				fast_boot = true;
			else
				fast_boot = false;
		}

		var.key = "pcsx2_renderer";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			renderer = var.value;
		}
	}

	var.key = "pcsx2_pgs_ssaa";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		i_prev = pgs_super_sampling;
		if (!strcmp(var.value, "Native"))
			pgs_super_sampling = 0;
		else if (!strcmp(var.value, "2x SSAA"))
			pgs_super_sampling = 1;
		else if (!strcmp(var.value, "4x SSAA (sparse grid)"))
			pgs_super_sampling = 2;
		else if (!strcmp(var.value, "4x SSAA (ordered, can high-res)"))
			pgs_super_sampling = 3;
		else if (!strcmp(var.value, "8x SSAA (can high-res)"))
			pgs_super_sampling = 4;
		else if (!strcmp(var.value, "16x SSAA (can high-res)"))
			pgs_super_sampling = 5;

		if (pgs_super_sampling != i_prev && !first_run)
		{
			EmuConfig.GS.PGSSuperSampling = pgs_super_sampling;
			// FIXME: This seems to hang sometimes.
			//VMManager::ApplySettings();
		}
	}

	var.key = "pcsx2_pgs_high_res_scanout";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		i_prev = pgs_high_res_scanout;
		if (!strcmp(var.value, "enabled"))
			pgs_high_res_scanout = 1;
		else
			pgs_high_res_scanout = 0;

		if (pgs_high_res_scanout != i_prev && !first_run)
		{
			EmuConfig.GS.PGSHighResScanout = pgs_high_res_scanout;
			// FIXME: This seems to hang sometimes.
			//VMManager::ApplySettings();
		}
	}

	var.key = "pcsx2_pgs_disable_mipmaps";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		i_prev = pgs_disable_mipmaps;
		if (!strcmp(var.value, "enabled"))
			pgs_disable_mipmaps = 1;
		else
			pgs_disable_mipmaps = 0;

		if (pgs_disable_mipmaps != i_prev && !first_run)
		{
			EmuConfig.GS.PGSDisableMipmaps = pgs_disable_mipmaps;
			// FIXME: This seems to hang sometimes.
			//VMManager::ApplySettings();
		}
	}

	var.key = "pcsx2_upscale_multiplier";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		i_prev = upscale_multiplier;
		upscale_multiplier = atoi(var.value);

		if (upscale_multiplier != i_prev && !first_run)
		{
			retro_system_av_info av_info;
			retro_get_system_av_info(&av_info);
#if 1
			environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
#else
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
#endif
		}
	}

	var.key = "pcsx2_mipmapping";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "enabled"))
			mipmapping = true;
		else
			mipmapping = false;
	}

	var.key = "pcsx2_axis_scale1";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		pad_axis_scale[0] = atof(var.value) / 100;
	}

	var.key = "pcsx2_axis_scale2";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		pad_axis_scale[1] = atof(var.value) / 100;
	}

	update_option_visibility();
}

#ifdef ENABLE_VULKAN
static retro_hw_render_interface_vulkan *vulkan;
void vk_libretro_init_wraps(void);
void vk_libretro_init(VkInstance instance, VkPhysicalDevice gpu, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features);
void vk_libretro_shutdown(void);
void vk_libretro_set_hwrender_interface(retro_hw_render_interface_vulkan *hw_render_interface);
#endif

static void cpu_thread_pause(void)
{
	VMManager::SetPaused(true);
	while(cpu_thread_state != VMState::Paused)
		MTGS::MainLoop(true);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	batch_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
	sample_cb = cb;
}

void retro_set_environment(retro_environment_t cb)
{
	struct retro_vfs_interface_info vfs_iface_info;
	bool no_game = true;

	environ_cb = cb;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
#ifdef PERF_TEST
	environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
#endif

	struct retro_core_options_update_display_callback update_display_cb;
	update_display_cb.callback = update_option_visibility;
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update_display_cb);

	vfs_iface_info.required_interface_version = 1;
	vfs_iface_info.iface                      = NULL;
	if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
		filestream_vfs_init(&vfs_iface_info);
}

static std::vector<std::string> disk_images;
static int image_index = 0;

static bool RETRO_CALLCONV get_eject_state(void)
{
	return cdvdRead(0x0B);
}

static bool RETRO_CALLCONV set_eject_state(bool ejected)
{
	if (get_eject_state() == ejected)
		return false;

	cpu_thread_pause();

	if (ejected)
		cdvdCtrlTrayOpen();
	else
	{
		if (image_index < 0 || image_index >= (int)disk_images.size())
			VMManager::ChangeDisc(CDVD_SourceType::NoDisc, "");
		else
			VMManager::ChangeDisc(CDVD_SourceType::Iso, disk_images[image_index]);
	}

	VMManager::SetPaused(false);
	return true;
}

static unsigned RETRO_CALLCONV get_image_index(void)
{
	return image_index;
}

static bool RETRO_CALLCONV set_image_index(unsigned index)
{
	if (get_eject_state())
	{
		image_index = index;
		return true;
	}

	return false;
}

static unsigned RETRO_CALLCONV get_num_images(void)
{
	return disk_images.size();
}

static bool RETRO_CALLCONV replace_image_index(unsigned index, const struct retro_game_info* info)
{
	if (index >= disk_images.size())
		return false;

	if (!info->path)
	{
		disk_images.erase(disk_images.begin() + index);
		if (!disk_images.size())
			image_index = -1;
		else if (image_index > (int)index)
			image_index--;
	}
	else
		disk_images[index] = info->path;

	return true;
}

static bool RETRO_CALLCONV add_image_index(void)
{
	disk_images.push_back("");
	return true;
}

static bool RETRO_CALLCONV set_initial_image(unsigned index, const char* path)
{
	if (index >= disk_images.size())
		index = 0;

	image_index = index;

	return true;
}

static bool RETRO_CALLCONV get_image_path(unsigned index, char* path, size_t len)
{
	if (index >= disk_images.size())
		return false;

	if (disk_images[index].empty())
		return false;

	strncpy(path, disk_images[index].c_str(), len);
	return true;
}
static bool RETRO_CALLCONV get_image_label(unsigned index, char* label, size_t len)
{
	if (index >= disk_images.size())
		return false;

	if (disk_images[index].empty())
		return false;

	strncpy(label, disk_images[index].c_str(), len);
	return true;
}

void retro_deinit(void)
{
	// WIN32 doesn't allow canceling threads from global constructors/destructors in a shared library.
	vu1Thread.Close();
#ifdef PERF_TEST
	perf_cb.perf_log();
#endif
}

void retro_get_system_info(retro_system_info* info)
{
	info->library_version  = "1";
	info->library_name     = "LRPS2";
	info->valid_extensions = "elf|iso|ciso|cue|bin|gz|chd|cso|zso";
	info->need_fullpath    = true;
	info->block_extract    = true;
}

void retro_get_system_av_info(retro_system_av_info* info)
{
	if (renderer == "Software" || renderer == "paraLLEl-GS" || renderer == "Null")
	{
		info->geometry.base_width = 640;
		info->geometry.base_height = 448;
	}
	else
	{
		info->geometry.base_width = 640 * upscale_multiplier;
		info->geometry.base_height = 448 * upscale_multiplier;
	}

	info->geometry.max_width = info->geometry.base_width;
	info->geometry.max_height = info->geometry.base_height;

	if (renderer == "paraLLEl-GS" && pgs_high_res_scanout)
	{
		info->geometry.max_width *= 2;
		info->geometry.max_height *= 2;
	}

	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = (retro_get_region() == RETRO_REGION_NTSC) ? (60.0f / 1.001f) : 50.0f;
	info->timing.sample_rate = 48000;
}

void retro_reset(void)
{
	cpu_thread_pause();
	VMManager::Reset();
	VMManager::SetPaused(false);
}

static void libretro_context_reset(void)
{
	const GSHWMipmapMode mipmap_mode = mipmapping ? GSHWMipmapMode::Enabled : GSHWMipmapMode::Unclamped;
	s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale_multiplier);
	s_settings_interface.SetFloatValue("EmuCore/GS", "pgsSuperSampling", pgs_super_sampling);
	s_settings_interface.SetFloatValue("EmuCore/GS", "pgsHighResScanout", pgs_high_res_scanout);
	s_settings_interface.SetFloatValue("EmuCore/GS", "pgsDisableMipmaps", pgs_disable_mipmaps);
	s_settings_interface.SetUIntValue("EmuCore/GS", "hw_mipmap_mode", (u8)mipmap_mode);
	s_settings_interface.SetBoolValue("EmuCore/GS", "mipmap", mipmapping);
	GSConfig.UpscaleMultiplier     = upscale_multiplier;
	GSConfig.PGSSuperSampling      = pgs_super_sampling;
	GSConfig.PGSHighResScanout     = pgs_high_res_scanout;
	GSConfig.PGSDisableMipmaps     = pgs_disable_mipmaps;
	GSConfig.HWMipmapMode          = mipmap_mode;
	GSConfig.Mipmap                = mipmapping;
	EmuConfig.GS.UpscaleMultiplier = upscale_multiplier;
	EmuConfig.GS.PGSSuperSampling  = pgs_super_sampling;
	EmuConfig.GS.PGSHighResScanout = pgs_high_res_scanout;
	EmuConfig.GS.PGSDisableMipmaps = pgs_disable_mipmaps;
	EmuConfig.GS.HWMipmapMode      = mipmap_mode;
	EmuConfig.GS.Mipmap            = mipmapping;
#ifdef ENABLE_VULKAN
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
	{
		if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&vulkan) || !vulkan)
			log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
		if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
			log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u, got %u!\n", RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION, vulkan->interface_version);
		vk_libretro_set_hwrender_interface(vulkan);
#ifdef HAVE_PARALLEL_GS
		pgs_set_hwrender_interface(vulkan);
#endif
	}
#endif
	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	VMManager::SetPaused(false);
}

static void libretro_context_destroy(void)
{
	cpu_thread_pause();

	MTGS::CloseGS();
#ifdef ENABLE_VULKAN
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
		vk_libretro_shutdown();
#ifdef HAVE_PARALLEL_GS
	pgs_destroy_device();
#endif
#endif
}

static bool libretro_set_hw_render(retro_hw_context_type type)
{
	hw_render.context_type       = type;
	hw_render.context_reset      = libretro_context_reset;
	hw_render.context_destroy    = libretro_context_destroy;
	hw_render.bottom_left_origin = true;
	hw_render.depth              = true;
	hw_render.cache_context      = false;

	switch (type)
	{
#ifdef _WIN32
		case RETRO_HW_CONTEXT_D3D11:
			hw_render.version_major = 11;
			hw_render.version_minor = 0;
			break;
		case RETRO_HW_CONTEXT_D3D12:
			hw_render.version_major = 12;
			hw_render.version_minor = 0;
			break;
#endif
#ifdef ENABLE_VULKAN
		case RETRO_HW_CONTEXT_VULKAN:
			hw_render.version_major = VK_API_VERSION_1_1;
			hw_render.version_minor = 0;
			break;
#endif
		case RETRO_HW_CONTEXT_OPENGL_CORE:
			hw_render.version_major = 3;
			hw_render.version_minor = 3;
			break;

		case RETRO_HW_CONTEXT_OPENGL:
			hw_render.version_major = 3;
			hw_render.version_minor = 0;
			break;

		case RETRO_HW_CONTEXT_OPENGLES3:
			hw_render.version_major = 3;
			hw_render.version_minor = 0;
			break;

		case RETRO_HW_CONTEXT_NONE:
			return true;
		default:
			return false;
	}

	return environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
}

static bool libretro_select_hw_render(void)
{
	if (renderer == "Auto" || renderer == "Software")
	{
#if defined(__APPLE__)
        if (libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN))
            return true;
#else
		retro_hw_context_type context_type = RETRO_HW_CONTEXT_NONE;
		environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &context_type);
		if (context_type != RETRO_HW_CONTEXT_NONE && libretro_set_hw_render(context_type))
			return true;
#endif
	}
#ifdef _WIN32
	if (renderer == "D3D11")
	{
		hw_render.version_major = 11;
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11);
	}
	if (renderer == "D3D12")
	{
		hw_render.version_major = 12;
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12);
	}
#endif
#ifdef ENABLE_VULKAN
	if (renderer == "Vulkan" || renderer == "paraLLEl-GS")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN);
#endif
	if (renderer == "Null")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);

	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL_CORE))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGLES3))
		return true;
#ifdef _WIN32
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12))
		return true;
#endif
	if (renderer == "Software")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);

	return false;
}

static void cpu_thread_entry(VMBootParameters boot_params)
{
	VMManager::Initialize(boot_params);
	VMManager::SetState(VMState::Running);

	while (VMManager::GetState() != VMState::Shutdown)
	{
		if (VMManager::HasValidVM())
		{
			for (;;)
			{
				cpu_thread_state = VMManager::GetState();
				switch (cpu_thread_state)
				{
					case VMState::Initializing:
						MTGS::MainLoop(false);
						continue;

					case VMState::Running:
						VMManager::Execute();
						continue;

					case VMState::Resetting:
						VMManager::Reset();
						continue;

					case VMState::Stopping:
#if 0
						VMManager::Shutdown(fals);
#endif
						return;

					case VMState::Paused:
					default:
						continue;
				}
			}
		}
	}
}

#ifdef ENABLE_VULKAN
/* Forward declarations */
bool create_device_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions, unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features);
const VkApplicationInfo *get_application_info_vulkan(void);
#endif

void retro_init(void)
{
	enum retro_pixel_format xrgb888 = RETRO_PIXEL_FORMAT_XRGB8888;
	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &xrgb888);
	struct retro_log_callback log;
	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		log_cb = log.log;

	vu1Thread.Reset();

	if (bios.empty())
	{
		const char* system_base = nullptr;
		environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_base);

		FileSystem::FindResultsArray results;
		if (FileSystem::FindFiles(Path::Combine(system_base, "/pcsx2/bios").c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		{
			static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
			static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
			u32 version, region;
			std::string description, zone;
			for (const FILESYSTEM_FIND_DATA& fd : results)
			{
				if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
					continue;

				if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
					bios_info.push_back({ std::string(Path::GetFileName(fd.FileName)), description });
			}

			// Find the BIOS core option and fill its values/labels/default_values
			for (unsigned i = 0; option_defs_us[i].key != NULL; i++)
			{
				if (!strcmp(option_defs_us[i].key, "pcsx2_bios"))
				{
					unsigned j;
					for (j = 0; j < bios_info.size() && j < (RETRO_NUM_CORE_OPTION_VALUES_MAX - 1); j++)
						option_defs_us[i].values[j] = { bios_info[j].filename.c_str(), bios_info[j].description.c_str() };

					// Make sure the next array is null and set the first BIOS found as the default value
					option_defs_us[i].values[j] = { NULL, NULL };
					option_defs_us[i].default_value = option_defs_us[i].values[0].value;
					break;
				}
			}
		}
	}

   	bool option_categories = false;
   	libretro_set_core_options(environ_cb, &option_categories);

	static retro_disk_control_ext_callback disk_control = {
		set_eject_state,
		get_eject_state,
		get_image_index,
		set_image_index,
		get_num_images,
		replace_image_index,
		add_image_index,
		set_initial_image,
		get_image_path,
		get_image_label,
	};

	environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control);
}

bool retro_load_game(const struct retro_game_info* game)
{
	const char* system_base = nullptr;
	int format = RETRO_PIXEL_FORMAT_XRGB8888;
	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_base);

	EmuFolders::AppRoot   = Path::Combine(system_base, "pcsx2");
	EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
	EmuFolders::DataRoot  = EmuFolders::AppRoot;

	Host::Internal::SetBaseSettingsLayer(&s_settings_interface);
	EmuFolders::SetDefaults(s_settings_interface);
	VMManager::SetDefaultSettings(s_settings_interface);

	SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
	EmuFolders::LoadConfig(*bsi);
	EmuFolders::EnsureFoldersExist();
	VMManager::Internal::CPUThreadInitialize();
	VMManager::LoadSettings();

	check_variables(true);

	if (bios.empty())
	{
		log_cb(RETRO_LOG_ERROR, "Could not find any valid PS2 BIOS File in %s\n", EmuFolders::Bios.c_str());
		return false;
	}

	s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale_multiplier);
	s_settings_interface.SetBoolValue("EmuCore", "EnableFastBoot", fast_boot);
	s_settings_interface.SetStringValue("Filenames", "BIOS", bios.c_str());

	Input::Init();

	if(!libretro_select_hw_render())
		return false;

	if(renderer == "Software")
		s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::SW);
	else
	{
		switch (hw_render.context_type)
		{
			case RETRO_HW_CONTEXT_D3D12:
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::DX12);
				break;
			case RETRO_HW_CONTEXT_D3D11:
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::DX11);
				break;
#ifdef ENABLE_VULKAN
			case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAVE_PARALLEL_GS
				if (renderer == "paraLLEl-GS")
				{
					s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::ParallelGS);
					static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
						RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
						RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
						pgs_get_application_info,
						pgs_create_device, // Legacy create device
						nullptr,
						pgs_create_instance,
						pgs_create_device2,
					};
					environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);
				}
				else
#endif
				{
					s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::VK);
					{
						static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
							RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
							RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
							get_application_info_vulkan,
							create_device_vulkan, // Callback above.
							nullptr,
						};
						environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);
					}
					Vulkan::LoadVulkanLibrary();
					vk_libretro_init_wraps();
				}
				break;
#endif
			case RETRO_HW_CONTEXT_NONE:
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::Null);
				break;
			default:
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::OGL);
				break;
		}
	}

	VMManager::ApplySettings();

	image_index = 0;
	disk_images.clear();
	VMBootParameters boot_params;
	if(game && game->path)
	{
		disk_images.push_back(game->path);
		boot_params.filename = game->path;
	}

	cpu_thread = std::thread(cpu_thread_entry, boot_params);

	if(hw_render.context_type == RETRO_HW_CONTEXT_NONE)
		if (!MTGS::IsOpen())
			MTGS::TryOpenGS();

	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info,
		size_t num_info)
{
	return false;
}

void retro_unload_game(void)
{
	if(MTGS::IsOpen())
	{
		cpu_thread_pause();
		MTGS::CloseGS();
	}

	VMManager::Shutdown();
	Input::Shutdown();
	cpu_thread.join();
#ifdef ENABLE_VULKAN
	if(hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
		Vulkan::UnloadVulkanLibrary();
#endif
	VMManager::Internal::CPUThreadShutdown();

	((LayeredSettingsInterface*)Host::GetSettingsInterface())->SetLayer(LayeredSettingsInterface::LAYER_BASE, nullptr);
}


void retro_run(void)
{
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		check_variables(false);

	Input::Update();

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	if (cpu_thread_state == VMState::Paused)
		VMManager::SetState(VMState::Running);

	RETRO_PERFORMANCE_INIT(pcsx2_run);
	RETRO_PERFORMANCE_START(pcsx2_run);

	MTGS::MainLoop(false);

	RETRO_PERFORMANCE_STOP(pcsx2_run);

	if (EmuConfig.GS.Renderer == GSRendererType::Null)
		video_cb(NULL, 0, 0, 0);
}

std::optional<WindowInfo> Host::AcquireRenderWindow(void)
{
	WindowInfo wi;
	wi.surface_width  = 640 * upscale_multiplier;
	wi.surface_height = 448 * upscale_multiplier;
	return wi;
}

size_t retro_serialize_size(void)
{
	freezeData fP = {0, nullptr};

	size_t size = _8mb;
	size += Ps2MemSize::MainRam;
	size += Ps2MemSize::IopRam;
	size += Ps2MemSize::Hardware;
	size += Ps2MemSize::IopHardware;
	size += Ps2MemSize::Scratch;
	size += VU0_MEMSIZE;
	size += VU1_MEMSIZE;
	size += VU0_PROGSIZE;
	size += VU1_PROGSIZE;
	SPU2freeze(FreezeAction::Size, &fP);
	size += fP.size;
	PADfreeze(FreezeAction::Size, &fP);
	size += fP.size;
	GSfreeze(FreezeAction::Size, &fP);
	size += fP.size;

	return size;
}

bool retro_serialize(void* data, size_t size)
{
	cpu_thread_pause();

	std::vector<u8> buffer;
	memSavingState saveme(buffer);
	freezeData fP;

	saveme.FreezeBios();
	saveme.FreezeInternals();

	saveme.FreezeMem(eeMem->Main, sizeof(eeMem->Main));
	saveme.FreezeMem(iopMem->Main, sizeof(iopMem->Main));
	saveme.FreezeMem(eeHw, sizeof(eeHw));
	saveme.FreezeMem(iopHw, sizeof(iopHw));
	saveme.FreezeMem(eeMem->Scratch, sizeof(eeMem->Scratch));
	saveme.FreezeMem(vuRegs[0].Mem, VU0_MEMSIZE);
	saveme.FreezeMem(vuRegs[1].Mem, VU1_MEMSIZE);
	saveme.FreezeMem(vuRegs[0].Micro, VU0_PROGSIZE);
	saveme.FreezeMem(vuRegs[1].Micro, VU1_PROGSIZE);

	fP.size = 0;
	fP.data = nullptr;
	SPU2freeze(FreezeAction::Size, &fP);
	saveme.PrepBlock(fP.size);
	fP.data = saveme.GetBlockPtr();
	SPU2freeze(FreezeAction::Save, &fP);
	saveme.CommitBlock(fP.size);

	fP.size = 0;
	fP.data = nullptr;
	PADfreeze(FreezeAction::Size, &fP);
	saveme.PrepBlock(fP.size);
	fP.data = saveme.GetBlockPtr();
	PADfreeze(FreezeAction::Save, &fP);
	saveme.CommitBlock(fP.size);

	fP.size = 0;
	fP.data = nullptr;
	GSfreeze(FreezeAction::Size, &fP);
	saveme.PrepBlock(fP.size);
	fP.data = saveme.GetBlockPtr();
	GSfreeze(FreezeAction::Save, &fP);
	saveme.CommitBlock(fP.size);

	memcpy(data, buffer.data(), buffer.size());


	VMManager::SetPaused(false);
	return true;
}

bool retro_unserialize(const void* data, size_t size)
{
	cpu_thread_pause();

	std::vector<u8> buffer;
	buffer.reserve(size);
	memcpy(buffer.data(), data, size);
	memLoadingState loadme(buffer);
	freezeData fP;

	loadme.FreezeBios();
	loadme.FreezeInternals();

	VMManager::Internal::ClearCPUExecutionCaches();
	loadme.FreezeMem(eeMem->Main, sizeof(eeMem->Main));
	loadme.FreezeMem(iopMem->Main, sizeof(iopMem->Main));
	loadme.FreezeMem(eeHw, sizeof(eeHw));
	loadme.FreezeMem(iopHw, sizeof(iopHw));
	loadme.FreezeMem(eeMem->Scratch, sizeof(eeMem->Scratch));
	loadme.FreezeMem(vuRegs[0].Mem, VU0_MEMSIZE);
	loadme.FreezeMem(vuRegs[1].Mem, VU1_MEMSIZE);
	loadme.FreezeMem(vuRegs[0].Micro, VU0_PROGSIZE);
	loadme.FreezeMem(vuRegs[1].Micro, VU1_PROGSIZE);

	fP.size = 0;
	fP.data = nullptr;
	SPU2freeze(FreezeAction::Size, &fP);
	loadme.PrepBlock(fP.size);
	fP.data = loadme.GetBlockPtr();
	SPU2freeze(FreezeAction::Load, &fP);
	loadme.CommitBlock(fP.size);

	fP.size = 0;
	fP.data = nullptr;
	PADfreeze(FreezeAction::Size, &fP);
	loadme.PrepBlock(fP.size);
	fP.data = loadme.GetBlockPtr();
	PADfreeze(FreezeAction::Load, &fP);
	loadme.CommitBlock(fP.size);

	fP.size = 0;
	fP.data = nullptr;
	GSfreeze(FreezeAction::Size, &fP);
	loadme.PrepBlock(fP.size);
	fP.data = loadme.GetBlockPtr();
	GSfreeze(FreezeAction::Load, &fP);
	loadme.CommitBlock(fP.size);

	VMManager::SetPaused(false);
	return true;
}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

size_t retro_get_memory_size(unsigned id)
{
	if (RETRO_MEMORY_SYSTEM_RAM == id)
		/* this only works because Scratch comes right after Main in eeMem */
		return Ps2MemSize::MainRam + Ps2MemSize::Scratch;
	return 0;
}

void* retro_get_memory_data(unsigned id)
{
	if (RETRO_MEMORY_SYSTEM_RAM == id)
		return eeMem->Main;
	return 0;
}

void retro_cheat_reset(void) { }

void retro_cheat_set(unsigned index, bool enabled, const char* code) { }

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
	if (!ret.has_value())
		log_cb(RETRO_LOG_ERROR, "Failed to read resource file '%s', path '%s'\n", filename, path.c_str());
	return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
	if (!ret.has_value())
		log_cb(RETRO_LOG_ERROR, "Failed to read resource file to string '%s', path '%s'\n", filename, path.c_str());
	return ret;
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& elf_override, const std::string& game_serial,
	u32 game_crc)
{
}
