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
#include "Pcsx2/Patch.h"

#include "SPU2/spu2.h"
#include "PAD/PAD.h"

#if 0
#define PERF_TEST
#endif

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
retro_log_printf_t log_cb;
retro_audio_sample_t sample_cb;
struct retro_hw_render_callback hw_render;

MemorySettingsInterface s_settings_interface;

static retro_audio_sample_batch_t batch_cb;

static VMState cpu_thread_state;
static std::thread cpu_thread;

enum PluginType : u8
{
	PLUGIN_PGS = 0,
	PLUGIN_GSDX_HW,
	PLUGIN_GSDX_SW,
	PLUGIN_NULL
};

struct BiosInfo
{
	std::string filename;
	std::string description;
};

float pad_axis_scale[2];

static std::vector<BiosInfo> bios_info;
static std::string setting_bios;
static std::string setting_renderer;
static PluginType setting_plugin_type;
static int setting_upscale_multiplier          = 1;
static u8 setting_pgs_super_sampling           = 0;
static u8 setting_pgs_high_res_scanout         = 0;
static u8 setting_pgs_disable_mipmaps          = 0;
static u8 setting_deinterlace_mode             = 0;
static u8 setting_ee_cycle_skip                = 0;
static s8 setting_ee_cycle_rate                = 0;
static bool setting_hint_nointerlacing         = false;
static bool setting_pcrtc_antiblur             = false;
static bool setting_enable_cheats              = false;

static bool setting_show_parallel_options      = true;
static bool setting_show_gsdx_hw_only_options  = true;
static bool setting_show_shared_options        = true;

static bool update_option_visibility(void)
{
	struct retro_variable var;
	struct retro_core_option_display option_display;
	bool updated                        = false;

	// Show/hide video options
	bool show_parallel_options_prev     = setting_show_parallel_options;
	bool show_gsdx_hw_only_options_prev = setting_show_gsdx_hw_only_options;
	bool show_shared_options_prev       = setting_show_shared_options;

	setting_show_parallel_options       = true;
	setting_show_gsdx_hw_only_options   = true;
	setting_show_shared_options         = true;

	var.key = "pcsx2_renderer";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool parallel_renderer = !strcmp(var.value, "paraLLEl-GS");
		const bool software_renderer = !strcmp(var.value, "Software");
		const bool null_renderer     = !strcmp(var.value, "Null");

		if (null_renderer)
			setting_show_shared_options       = false;
		if (parallel_renderer || software_renderer || null_renderer)
			setting_show_gsdx_hw_only_options = false;
		if (!parallel_renderer)
			setting_show_parallel_options     = false;
	}

	// paraLLEl-GS options
	if (setting_show_parallel_options != show_parallel_options_prev)
	{
		option_display.visible = setting_show_parallel_options;
		option_display.key     = "pcsx2_pgs_ssaa";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pgs_high_res_scanout";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated = true;
	}

	// GSdx HW options, but NOT compatible with Software and NULL renderers
	if (setting_show_gsdx_hw_only_options != show_gsdx_hw_only_options_prev)
	{
		option_display.visible = setting_show_gsdx_hw_only_options;
		option_display.key     = "pcsx2_upscale_multiplier";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// Options compatible with both paraLLEl-GS and GSdx HW/SW, still not with NULL renderer
	if (setting_show_shared_options != show_shared_options_prev)
	{
		option_display.visible = setting_show_shared_options;
		option_display.key     = "pcsx2_pgs_disable_mipmaps";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_deinterlace_mode";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pcrtc_antiblur";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_nointerlacing_hint";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		updated                = true;
	}

	return updated;
}

static void cpu_thread_pause(void)
{
	VMManager::SetPaused(true);
	while(cpu_thread_state != VMState::Paused)
		MTGS::MainLoop(true);
}

static void check_variables(bool first_run)
{
	struct retro_variable var;
	bool updated = false;

	if (first_run)
	{
		var.key = "pcsx2_renderer";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			setting_renderer = var.value;
			if (setting_renderer == "paraLLEl-GS")
				setting_plugin_type = PLUGIN_PGS;
			else if (setting_renderer == "Software")
				setting_plugin_type = PLUGIN_GSDX_SW;
			else if (setting_renderer == "Null")
				setting_plugin_type = PLUGIN_NULL;
			else
				setting_plugin_type = PLUGIN_GSDX_HW;
		}

		var.key = "pcsx2_bios";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			setting_bios = var.value;
			s_settings_interface.SetStringValue("Filenames", "BIOS", setting_bios.c_str());
		}

		var.key = "pcsx2_fastboot";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool fast_boot = !strcmp(var.value, "enabled");
			s_settings_interface.SetBoolValue("EmuCore", "EnableFastBoot", fast_boot);
		}

		var.key = "pcsx2_fastcdvd";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool fast_cdvd = !strcmp(var.value, "enabled");
			s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "fastCDVD", fast_cdvd);
		}
	}

	if (setting_plugin_type == PLUGIN_PGS)
	{
		var.key = "pcsx2_pgs_ssaa";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_super_sampling_prev = setting_pgs_super_sampling;
			if (!strcmp(var.value, "Native"))
				setting_pgs_super_sampling = 0;
			else if (!strcmp(var.value, "2x SSAA"))
				setting_pgs_super_sampling = 1;
			else if (!strcmp(var.value, "4x SSAA (sparse grid)"))
				setting_pgs_super_sampling = 2;
			else if (!strcmp(var.value, "4x SSAA (ordered, can high-res)"))
				setting_pgs_super_sampling = 3;
			else if (!strcmp(var.value, "8x SSAA (can high-res)"))
				setting_pgs_super_sampling = 4;
			else if (!strcmp(var.value, "16x SSAA (can high-res)"))
				setting_pgs_super_sampling = 5;

			if (first_run || setting_pgs_super_sampling != pgs_super_sampling_prev)
			{
				s_settings_interface.SetIntValue("EmuCore/GS", "pgsSuperSampling", setting_pgs_super_sampling);
				updated = true;
			}
		}

		var.key = "pcsx2_pgs_high_res_scanout";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_high_res_scanout_prev = setting_pgs_high_res_scanout;
			setting_pgs_high_res_scanout = !strcmp(var.value, "enabled") ? 1 : 0;

			if (first_run)
				s_settings_interface.SetIntValue("EmuCore/GS", "pgsHighResScanout", setting_pgs_high_res_scanout);
#if 0
			// TODO: ATM it crashes when changed on-the-fly, re-enable when fixed
			// also remove "(Restart)" from the core option label
			else if (setting_pgs_high_res_scanout != pgs_high_res_scanout_prev)
			{
				retro_system_av_info av_info;
				s_settings_interface.SetIntValue("EmuCore/GS", "pgsHighResScanout", setting_pgs_high_res_scanout);

				retro_get_system_av_info(&av_info);
#if 1
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
#else
				environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
#endif
				updated = true;
			}
#endif
		}
	}

	// Options for both paraLLEl-GS and GSdx HW/SW, just not with NULL renderer
	if (setting_plugin_type != PLUGIN_NULL)
	{
		var.key = "pcsx2_pgs_disable_mipmaps";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_disable_mipmaps_prev = setting_pgs_disable_mipmaps;
			setting_pgs_disable_mipmaps = !strcmp(var.value, "enabled") ? 1 : 0;

			if (first_run || setting_pgs_disable_mipmaps != pgs_disable_mipmaps_prev)
			{
				const u8 mipmap_mode = (u8)(setting_pgs_disable_mipmaps ? GSHWMipmapMode::Unclamped : GSHWMipmapMode::Enabled);
				s_settings_interface.SetIntValue("EmuCore/GS", "hw_mipmap_mode", mipmap_mode);
				s_settings_interface.SetBoolValue("EmuCore/GS", "mipmap", !setting_pgs_disable_mipmaps);
				s_settings_interface.SetIntValue("EmuCore/GS", "pgsDisableMipmaps", setting_pgs_disable_mipmaps);
				updated = true;
			}
		}

		var.key = "pcsx2_nointerlacing_hint";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool hint_nointerlacing_prev = setting_hint_nointerlacing;
			setting_hint_nointerlacing = !strcmp(var.value, "enabled");

			if (first_run || setting_hint_nointerlacing != hint_nointerlacing_prev)
				updated = true;
		}

		var.key = "pcsx2_pcrtc_antiblur";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool pcrtc_antiblur_prev = setting_pcrtc_antiblur;
			setting_pcrtc_antiblur = !strcmp(var.value, "enabled");

			if (first_run || setting_pcrtc_antiblur != pcrtc_antiblur_prev)
			{
				s_settings_interface.SetBoolValue("EmuCore/GS", "pcrtc_antiblur", setting_pcrtc_antiblur);
				updated = true;
			}
		}

		var.key = "pcsx2_deinterlace_mode";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 deinterlace_mode_prev = setting_deinterlace_mode;
			if (!strcmp(var.value, "Automatic"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::Automatic;
			else if (!strcmp(var.value, "Off"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::Off;
			else if (!strcmp(var.value, "Weave TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::WeaveTFF;
			else if (!strcmp(var.value, "Weave BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::WeaveBFF;
			else if (!strcmp(var.value, "Bob TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BobTFF;
			else if (!strcmp(var.value, "Bob BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BobBFF;
			else if (!strcmp(var.value, "Blend TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BlendTFF;
			else if (!strcmp(var.value, "Blend BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::BlendBFF;
			else if (!strcmp(var.value, "Adaptive TFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::AdaptiveTFF;
			else if (!strcmp(var.value, "Adaptive BFF"))
				setting_deinterlace_mode = (u8)GSInterlaceMode::AdaptiveBFF;

			if (first_run || setting_deinterlace_mode != deinterlace_mode_prev)
			{
				s_settings_interface.SetIntValue("EmuCore/GS", "deinterlace_mode", setting_deinterlace_mode);
				updated = true;
			}
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_HW)
	{
		var.key = "pcsx2_upscale_multiplier";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			int upscale_multiplier_prev = setting_upscale_multiplier;
			setting_upscale_multiplier = atoi(var.value);

			if (first_run)
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", setting_upscale_multiplier);
#if 0
			// TODO: ATM it crashes when changed on-the-fly, re-enable when fixed
			// also remove "(Restart)" from the core option label
			else if (setting_upscale_multiplier != upscale_multiplier_prev)
			{
				retro_system_av_info av_info;
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", setting_upscale_multiplier);

				retro_get_system_av_info(&av_info);
#if 1
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
#else
				environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
#endif
				updated = true;
			}
#endif
		}
	}

	var.key = "pcsx2_enable_cheats";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		bool enable_cheats_prev = setting_enable_cheats;
		setting_enable_cheats = !strcmp(var.value, "enabled");

		if (first_run || setting_enable_cheats != enable_cheats_prev)
		{
			s_settings_interface.SetBoolValue("EmuCore", "EnableCheats", setting_enable_cheats);
			updated = true;
		}
	}

	var.key = "pcsx2_ee_cycle_rate";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		s8 ee_cycle_rate_prev = setting_ee_cycle_rate;
		if (!strcmp(var.value, "50% (Underclock)"))
			setting_ee_cycle_rate = -3;
		else if (!strcmp(var.value, "60% (Underclock)"))
			setting_ee_cycle_rate = -2;
		else if (!strcmp(var.value, "75% (Underclock)"))
			setting_ee_cycle_rate = -1;
		else if (!strcmp(var.value, "100% (Normal Speed)"))
			setting_ee_cycle_rate = 0;
		else if (!strcmp(var.value, "130% (Overclock)"))
			setting_ee_cycle_rate = 1;
		else if (!strcmp(var.value, "180% (Overclock)"))
			setting_ee_cycle_rate = 2;
		else if (!strcmp(var.value, "300% (Overclock)"))
			setting_ee_cycle_rate = 3;

		if (first_run || setting_ee_cycle_rate != ee_cycle_rate_prev)
		{
			s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleRate", setting_ee_cycle_rate);
			updated = true;
		}
	}

	var.key = "pcsx2_ee_cycle_skip";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u8 ee_cycle_skip_prev = setting_ee_cycle_skip;
		if (!strcmp(var.value, "disabled"))
			setting_ee_cycle_skip = 0;
		else if (!strcmp(var.value, "Mild Underclock"))
			setting_ee_cycle_skip = 1;
		else if (!strcmp(var.value, "Moderate Underclock"))
			setting_ee_cycle_skip = 2;
		else if (!strcmp(var.value, "Maximum Underclock"))
			setting_ee_cycle_skip = 3;

		if (first_run || setting_ee_cycle_skip != ee_cycle_skip_prev)
		{
			s_settings_interface.SetIntValue("EmuCore/Speedhacks",
				"EECycleSkip", setting_ee_cycle_skip);
			updated = true;
		}
	}

	var.key = "pcsx2_axis_scale1";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		pad_axis_scale[0] = atof(var.value) / 100;

	var.key = "pcsx2_axis_scale2";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		pad_axis_scale[1] = atof(var.value) / 100;

	update_option_visibility();

	if (!first_run && updated)
	{
		cpu_thread_pause();
		VMManager::ApplySettings();
	}
}

#ifdef ENABLE_VULKAN
static retro_hw_render_interface_vulkan *vulkan;
void vk_libretro_init_wraps(void);
void vk_libretro_init(VkInstance instance, VkPhysicalDevice gpu, const char **required_device_extensions,
	unsigned num_required_device_extensions, const char **required_device_layers,
	unsigned num_required_device_layers, const VkPhysicalDeviceFeatures *required_features);
void vk_libretro_shutdown(void);
void vk_libretro_set_hwrender_interface(retro_hw_render_interface_vulkan *hw_render_interface);
#endif

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)   { sample_cb = cb; }

void retro_set_environment(retro_environment_t cb)
{
	bool no_game = true;
	struct retro_vfs_interface_info vfs_iface_info;
	struct retro_core_options_update_display_callback update_display_cb;

	environ_cb = cb;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
#ifdef PERF_TEST
	environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
#endif

	update_display_cb.callback = update_option_visibility;
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update_display_cb);

	vfs_iface_info.required_interface_version = 1;
	vfs_iface_info.iface                      = NULL;
	if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
		filestream_vfs_init(&vfs_iface_info);
}

static std::vector<std::string> disk_images;
static int image_index = 0;

static bool RETRO_CALLCONV get_eject_state(void) { return cdvdRead(0x0B); }
static unsigned RETRO_CALLCONV get_image_index(void) { return image_index; }
static unsigned RETRO_CALLCONV get_num_images(void) { return disk_images.size(); }

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

static bool RETRO_CALLCONV set_image_index(unsigned index)
{
	if (get_eject_state())
	{
		image_index = index;
		return true;
	}

	return false;
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
	if (               setting_renderer == "Software" 
			|| setting_renderer == "paraLLEl-GS" 
			|| setting_renderer == "Null")
	{
		info->geometry.base_width  = 640;
		info->geometry.base_height = 448;
	}
	else
	{
		info->geometry.base_width  = 640 * setting_upscale_multiplier;
		info->geometry.base_height = 448 * setting_upscale_multiplier;
	}

	info->geometry.max_width  = info->geometry.base_width;
	info->geometry.max_height = info->geometry.base_height;

	if (               setting_renderer == "paraLLEl-GS" 
			&& setting_pgs_high_res_scanout)
	{
		info->geometry.max_width  *= 2;
		info->geometry.max_height *= 2;
	}

	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps            = (retro_get_region() == RETRO_REGION_NTSC) ? (60.0f / 1.001f) : 50.0f;
	info->timing.sample_rate    = 48000;
}

void retro_reset(void)
{
	cpu_thread_pause();
	VMManager::Reset();
	VMManager::SetPaused(false);
}

static void libretro_context_reset(void)
{
#ifdef ENABLE_VULKAN
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
	{
		if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&vulkan) || !vulkan)
			log_cb(RETRO_LOG_ERROR, "Failed to get HW rendering interface!\n");
		if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
			log_cb(RETRO_LOG_ERROR, "HW render interface mismatch, expected %u, got %u!\n",
			RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION, vulkan->interface_version);
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
	if (setting_renderer == "Auto" || setting_renderer == "Software")
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
	if (setting_renderer == "D3D11")
	{
		hw_render.version_major = 11;
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11);
	}
	if (setting_renderer == "D3D12")
	{
		hw_render.version_major = 12;
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12);
	}
#endif
#ifdef ENABLE_VULKAN
	if (               setting_renderer == "Vulkan" 
			|| setting_renderer == "paraLLEl-GS")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN);
#endif
	if (setting_renderer == "Null")
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
	if (setting_renderer == "Software")
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
bool create_device_vulkan(retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **required_device_extensions,
	unsigned num_required_device_extensions, const char **required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures *required_features);
const VkApplicationInfo *get_application_info_vulkan(void);
#endif

void retro_init(void)
{
	struct retro_log_callback log;
   	bool option_categories          = false;
	enum retro_pixel_format xrgb888 = RETRO_PIXEL_FORMAT_XRGB8888;

	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &xrgb888);
	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		log_cb = log.log;

	vu1Thread.Reset();

	if (setting_bios.empty())
	{
		const char* system_base = nullptr;
		environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_base);

		FileSystem::FindResultsArray results;
		if (FileSystem::FindFiles(Path::Combine(system_base, "/pcsx2/bios").c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		{
			u32 version, region;
			static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
			static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
			std::string description, zone;
			for (const FILESYSTEM_FIND_DATA& fd : results)
			{
				if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
					continue;

				if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
					bios_info.push_back({ std::string(Path::GetFileName(fd.FileName)), description });
			}

			/* Find the BIOS core option and fill its values/labels/default_values */
			for (unsigned i = 0; option_defs_us[i].key != NULL; i++)
			{
				if (!strcmp(option_defs_us[i].key, "pcsx2_bios"))
				{
					unsigned j;
					for (j = 0; j < bios_info.size() && j < (RETRO_NUM_CORE_OPTION_VALUES_MAX - 1); j++)
						option_defs_us[i].values[j] = { bios_info[j].filename.c_str(), bios_info[j].description.c_str() };

					/* Make sure the next array is NULL 
					 * and set the first BIOS found as the default value */
					option_defs_us[i].values[j]     = { NULL, NULL };
					option_defs_us[i].default_value = option_defs_us[i].values[0].value;
					break;
				}
			}
		}
	}

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
	VMBootParameters boot_params;
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

	if (setting_bios.empty())
	{
		log_cb(RETRO_LOG_ERROR, "Could not find any valid PS2 BIOS File in %s\n", EmuFolders::Bios.c_str());
		return false;
	}

	Input::Init();

	if (!libretro_select_hw_render())
		return false;

	if (setting_renderer == "Software")
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
				if (setting_renderer == "paraLLEl-GS")
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

	if (game && game->path)
	{
		disk_images.push_back(game->path);
		boot_params.filename = game->path;
	}

	cpu_thread = std::thread(cpu_thread_entry, boot_params);

	if (hw_render.context_type == RETRO_HW_CONTEXT_NONE)
		if (!MTGS::IsOpen())
			MTGS::TryOpenGS();

	return true;
}

bool retro_load_game_special(unsigned game_type,
	const struct retro_game_info* info, size_t num_info) { return false; }

void retro_unload_game(void)
{
	if (MTGS::IsOpen())
	{
		cpu_thread_pause();
		MTGS::CloseGS();
	}

	VMManager::Shutdown();
	Input::Shutdown();
	cpu_thread.join();
#ifdef ENABLE_VULKAN
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN)
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
	wi.surface_width  = 640 * setting_upscale_multiplier;
	wi.surface_height = 448 * setting_upscale_multiplier;
	return wi;
}

size_t retro_serialize_size(void)
{
	freezeData fP = {0, nullptr};

	size_t size   = _8mb;
	size         += Ps2MemSize::MainRam;
	size         += Ps2MemSize::IopRam;
	size         += Ps2MemSize::Hardware;
	size         += Ps2MemSize::IopHardware;
	size         += Ps2MemSize::Scratch;
	size         += VU0_MEMSIZE;
	size         += VU1_MEMSIZE;
	size         += VU0_PROGSIZE;
	size         += VU1_PROGSIZE;
	SPU2freeze(FreezeAction::Size, &fP);
	size         += fP.size;
	PADfreeze(FreezeAction::Size, &fP);
	size         += fP.size;
	GSfreeze(FreezeAction::Size, &fP);
	size         += fP.size;

	return size;
}

bool retro_serialize(void* data, size_t size)
{
	freezeData fP;
	std::vector<u8> buffer;

	cpu_thread_pause();

	memSavingState saveme(buffer);

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
	freezeData fP;
	std::vector<u8> buffer;

	cpu_thread_pause();

	buffer.reserve(size);
	memcpy(buffer.data(), data, size);
	memLoadingState loadme(buffer);

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

unsigned retro_get_region(void)  { return RETRO_REGION_NTSC; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

size_t retro_get_memory_size(unsigned id)
{
	/* This only works because Scratch comes right after Main in eeMem */
	if (RETRO_MEMORY_SYSTEM_RAM == id)
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


static void lrps2_ingame_patches(const char *serial, const char *renderer, bool nointerlacing_hint)
{
	log_cb(RETRO_LOG_INFO, "serial: %s\n", serial);

	if (nointerlacing_hint)
	{
		/* Ace Combat 04 - Shattered Skies (NTSC-U) [CRC: A32F7CD0] */
		if (!strcmp(serial, "SLUS-20152"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,E0050003,extended,0029F418",
				"patch=1,EE,0029F418,extended,00000001",
				"patch=1,EE,D029F420,extended,0000948C",
				"patch=1,EE,0029F420,extended,00000000",
				"patch=1,EE,D029F420,extended,00009070",
				"patch=1,EE,0029F420,extended,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Ace Combat zero - The Belkan War (NTSC-U) [CRC: 65729657] */
		else if (!strcmp(serial, "SLUS-21346"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,202F9A14,extended,24020001",
				"patch=1,EE,202F9D58,extended,0C03FFF0",
				"patch=1,EE,200FFFC0,extended,341B9070",
				"patch=1,EE,200FFFC4,extended,145B0002",
				"patch=1,EE,200FFFCC,extended,34029000",
				"patch=1,EE,200FFFD0,extended,FCC20000",
				"patch=1,EE,200FFFD4,extended,03E00008"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Alpine Racer 3 (NTSC-J) [CRC: 771C3B47] */
		else if (!strcmp(serial, "SLPS-20181"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,E00410E0,extended,00686C80",
				"patch=1,EE,20686C78,extended,00000001",
				"patch=1,EE,20686C80,extended,00001000",
				"patch=1,EE,20686CA0,extended,00000001",
				"patch=1,EE,20686CA8,extended,00001000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
		else if (!strcmp(serial, "SLUS-20685"))
		{
			/* Patch courtesy: NineKain */
			int i;
			char *patches[] = {
				"patch=1,EE,00155580,word,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Colin McRae Rally 3 (PAL) [CRC: 7DEAE69C] */
		else if (!strcmp(serial, "SLES-51117")) 
		{
			/* Patch courtesy: agrippa */
			int i;
			char *patches[] = {
				"patch=1,EE,00246B90,word,24040001", /* set FFMD to 0 in SMODE2 register to disable field mode */
				"patch=1,EE,00247A64,word,00000000"  /* nop the switch to the front buffer */
				/* A full height back buffer enabled, instead of a downsampled front buffer. */
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Drakengard 2 (NTSC-U) [CRC: 1648E3C9] */
		else if (!strcmp(serial, "SLUS-21373"))
		{
			/* Patch courtesy: umechan */
			/* TODO/FIXME - text cutoff a little on the bottom
			 * with parallel-gs */
			int i;
			char *patches[] = {
				"patch=1,EE,E0030003,extended,00456DA0",
				"patch=1,EE,20456DA0,extended,00000001",
				"patch=1,EE,20456DB0,extended,00001450",
				"patch=1,EE,20456DBC,extended,001DF9FF",
				"patch=1,EE,E0029400,extended,00456DB0",
				"patch=1,EE,20456DB0,extended,0000948C",
				"patch=1,EE,20456DBC,extended,001DF9FF",
				"patch=1,EE,E0030001,extended,00456D54",
				"patch=1,EE,20456D38,extended,00000050",
				"patch=1,EE,20456D44,extended,000001E1",
				"patch=1,EE,20456D54,extended,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Enthusia - Professional Racing (NTSC-U) [CRC: 81D233DC] */
		else if (!strcmp(serial, "SLUS-20967")) 
		{
			int i;
			char *patches[] = {
				"patch=1,EE,2013363C,word,34060001",
				"patch=1,EE,20383A40,word,00009450"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Eternal Poison (NTSC-U) [CRC: 2BE55519] */
		else if (!strcmp(serial, "SLUS-21779"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,0032DC7C,word,00000000",
				"patch=1,EE,0032DD04,word,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Harry Potter and the Sorcerer's Stone (NTSC-U) [CRC: ] */
		else if (!strcmp(serial, "SLUS-20826")) 
		{
			int i;
			/* TODO/FIXME - decouple FPS unlock */
			char *patches[] = {
				"patch=0,EE,2026E528,extended,3405EA60",
				"patch=0,EE,0026E538,extended,24090001",
				"patch=0,EE,1026E914,extended,24030280",
				"patch=0,EE,202E0870,extended,24080001",
				"patch=0,EE,202E1078,extended,0000282D",
				"patch=0,EE,002E08B8,extended,24040002",
				"patch=0,EE,002E00C4,extended,30840002",
				"patch=0,EE,202E077C,extended,24A5FFFF",
				"patch=0,EE,202E1070,extended,24060050",
				"patch=0,EE,102E0854,extended,24030134"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Ico (PAL) [CRC: ] */
		else if (!strcmp(serial, "SCES-50760"))
		{
			/* Patch courtesy: agrippa */
			int i;
			char *patches[] = {
				//set the back buffer
				"patch=1,EE,2028F500,extended,00001040",
				"patch=1,EE,2028F528,extended,00001040",
				//switch to the interlaced mode with FFMD set to 0. Progressive mode, applied by default,
				//does add a black bar at the bottom in the NTSC mode when the back buffer is enabled
				"patch=1,EE,2028F4F8,extended,00000001",
				"patch=1,EE,2028F520,extended,00000001",
				//check if the PAL mode is turned on to extend the display buffer from 256 to 512
				"patch=1,EE,E0024290,extended,0028F508",
				"patch=1,EE,2028F50C,extended,001FF9FF",
				"patch=1,EE,2028F534,extended,001FF9FF",
				//check if the NTSC mode is turned on to extend the display buffer from 224 to 448
				"patch=1,EE,E002927C,extended,0028F508",
				"patch=1,EE,2028F50C,extended,001DF9FF",
				"patch=1,EE,2028F534,extended,001DF9FF"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Ico (NTSC-U) [CRC: 6F8545DB] */
		else if (!strcmp(serial, "SCUS-97113")) 
		{
			int i;
			char *patches[] = {
				/* enable back buffer */
				"patch=0,EE,00274EF8,extended,00000001",
				"patch=0,EE,00274F20,extended,00000001",
				"patch=0,EE,00274F00,extended,00001040",
				"patch=0,EE,00274F28,extended,00001040",
				/* nointerlacing */
				"patch=1,EE,00274EF8,extended,00000001",
				"patch=1,EE,00274F20,extended,00000001",
				"patch=1,EE,00274F00,extended,00000040",
				"patch=1,EE,00274F28,extended,00000040"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Kinetica (NTSC-U) [CRC: D39C08F5] */
		else if (!strcmp(serial, "SCUS-97132"))
		{
			/* Patch courtesy: Mensa */
			/* Stops company logos and intro FMV from shaking. Menus and in-game never had an issue */
			int i;
			char *patches[] = {
				"patch=1,EE,201ABB34,word,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* MotoGP 2 (NTSC-U) [CRC: 586EA828] */
		else if (!strcmp(serial, "SLUS-20285"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,20265444,extended,FD030000",
				"patch=1,EE,2027FED0,extended,24020001",
				"patch=1,EE,0043C588,extended,00000001",
				"patch=1,EE,0036C798,extended,00000003",
				"patch=1,EE,0036C7C0,extended,00000003"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));

			if (setting_renderer == "paraLLEl-GS" || setting_renderer == "Software")
			{
				int i;
				char *patches[] = {
					/* full frame FMV only in software mode */
					"patch=1,EE,0036C798,extended,00000001",
					"patch=1,EE,0036C7C0,extended,00000001",
					"patch=1,EE,2036C7A0,extended,000018D8",
					"patch=1,EE,2036C7C8,extended,000018D8",
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		/* MotoGP 3 (NTSC-U) [CRC: 46B7FEC5] */
		else if (!strcmp(serial, "SLUS-20625"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,202C16CC,extended,FD030000",
				"patch=1,EE,202DD564,extended,24020001",
				"patch=1,EE,003EF558,extended,00000003",
				"patch=1,EE,003EF580,extended,00000003"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Mushihimesama (NTSC-J) [CRC: F0C24BB1] */
		else if (!strcmp(serial, "SLPM-66056"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,2010C300,extended,34030001",
				"patch=1,EE,2010C314,extended,3402148C",
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Resident Evil - Code - Veronica X (NTSC-U) [CRC: 24036809] */
		else if (!strcmp(serial, "SLUS-20184"))
		{
			int i;
			char *patches[] = {
				"patch=0,EE,002CB0A4,extended,24060050",
				"patch=0,EE,202CB0A0,extended,0000282D",
				"patch=0,EE,202CB0B0,extended,00000000",
				"patch=0,EE,201002F4,extended,10A40029",
				"patch=0,EE,1010030C,extended,260202D0",
				"patch=0,EE,00100370,extended,26450023",
				"patch=0,EE,10100398,extended,64E30134",
				"patch=0,EE,102E1AF0,extended,24420134",
				"patch=0,EE,202EB944,extended,00000000",
				"patch=0,EE,202CB0F4,extended,0000482D",
				/* font fixes */
				"patch=1,EE,002B9A50,word,3C013F40",
				"patch=1,EE,002B9A54,word,44816000",
				"patch=1,EE,002B9A58,word,460C6B02",
				"patch=1,EE,002B9A5c,word,3C010050",
				"patch=1,EE,002B9A60,word,E42C8140",
				"patch=1,EE,002B9A64,word,E42D8138",
				"patch=1,EE,002B9A68,word,03E00008",
				"patch=1,EE,002B9A6c,word,E42E8130"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Resident Evil - Dead Aim (NTSC-U) [CRC: FBB5290C] */
		else if (!strcmp(serial, "SLUS-20669"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,2028A268,extended,00000050",
				"patch=1,EE,2028A274,extended,000001E0",
				"patch=1,EE,2028A284,extended,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Rumble Fish, The (NTSC-J) */
		else if (!strcmp(serial, "SLPM-65919"))
		{
			/* Patch courtesy: felixthecat1970 */
			int i;
			char *patches[] = {
				/* Framebuffer + 480p mode + No interlacing */
				"patch=0,EE,201102A4,extended,3C050000",
				"patch=0,EE,201102AC,extended,3C060050",
				"patch=0,EE,201102B4,extended,3C070001",
				"patch=0,EE,20110948,extended,34030002",
				"patch=1,EE,2034FD50,extended,00009446",
				"patch=1,EE,2034FD5C,extended,001DF4FF",
				"patch=1,EE,2034FD78,extended,00009446",
				"patch=1,EE,2034FD84,extended,001DF4FF",
				/* NULL Int ints */
				"patch=0,EE,20111278,extended,03E00008",
				"patch=0,EE,2011127C,extended,00000000",
				"patch=0,EE,201114E0,extended,03E00008",
				"patch=0,EE,201114E4,extended,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Rumble Roses (NTSC-U) [CRC: C1C91715] */
		else if (!strcmp(serial, "SLUS-20970"))
		{
			/* Patch courtesy: felixthecat1970 */
			/* Framebuffer Display and no interlacing */
			int i;
			char *_patches[] = {
				"patch=1,EE,E0041100,extended,01D4ADA0",
				"patch=1,EE,21D4ADA0,extended,00001000",
				"patch=1,EE,21D4ADC8,extended,00001000",
				"patch=1,EE,201029FC,extended,64420000",
				"patch=1,EE,20102C64,extended,64420000"
			};
			for (i = 0; i < sizeof(_patches) / sizeof((_patches)[0]); i++)
				LoadPatchesFromString(std::string(_patches[i]));
			if (setting_renderer == "paraLLEl-GS")
			{
				char *patches[] = {
					"patch=1,EE,21D4AD98,extended,00000004",
					"patch=1,EE,21D4ADC0,extended,00000004"

					/* TODO/FIXME - we're missing the upscaling of the menu/startup screens */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			else
			{
				char *patches[] = {
					"patch=1,EE,21D4AD98,extended,00000002",
					"patch=1,EE,21D4ADC0,extended,00000002",

					"patch=1,EE,E0041400,extended,01D4ADA0",
					"patch=1,EE,21D4ADA0,extended,00001400",
					"patch=1,EE,21D4ADC8,extended,00001446",
					"patch=1,EE,21D4AD98,extended,00000003",
					"patch=1,EE,21D4ADC0,extended,00000003"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
		else if (!strcmp(serial, "SLPM-66212"))
		{
			/* Patch courtesy: asasega */
			int i;
			char *patches[] = {
				"patch=1,EE,20106FA0,extended,34030001",
				"patch=1,EE,20106FB4,extended,34021040"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Star Ocean: Til the End of Time (PAL) [CRC: E04EA200] */
		else if (!strcmp(serial, "SLES-82028"))
		{
			/* Patch courtesy: agrippa */
			int i;
			char *patches[] = {
				/* Skip the integrity check of the main executable file */
				"patch=1,EE,E0110011,extended,001F7660",
				"patch=1,EE,201e2530,extended,10000016",
				"patch=1,EE,201e2ff8,extended,10000016",
				"patch=1,EE,201e3410,extended,10000016",
				"patch=1,EE,201e3758,extended,10000016",
				"patch=1,EE,201e3968,extended,10000016",
				"patch=1,EE,201e3ba8,extended,10000016",
				"patch=1,EE,201e3d00,extended,10000016",
				"patch=1,EE,201eb5f8,extended,10000016",
				"patch=1,EE,201f68c0,extended,10000016",
				"patch=1,EE,201f6bb0,extended,10000016",
				"patch=1,EE,201f6c50,extended,10000016",
				"patch=1,EE,201f7030,extended,10000016",
				"patch=1,EE,201f7160,extended,10000016",
				"patch=1,EE,201f72a0,extended,10000016",
				"patch=1,EE,201f73d0,extended,10000016",
				"patch=1,EE,201f7500,extended,10000016",
				"patch=1,EE,201f7660,extended,10000016",
				/* in-battle anti-cheat checks? I have not seen the game to get there though. */
				"patch=1,EE,E002FFFA,extended,001EDB44",
				"patch=1,EE,201EDB44,extended,1400fffa",
				"patch=1,EE,201E94E0,extended,1000000F", /* 1440000F */
				/* full height frame buffer and video mode patches */
				"patch=0,EE,00101320,word,A0285C84", /* A0205C84 */
				"patch=1,EE,0012EF60,word,00000000", /* 10C00005 */
				"patch=1,EE,00100634,word,24050001", /* 0000282D */
				"patch=1,EE,00100638,word,24060003", /* 24060050 */
				"patch=1,EE,00100640,word,24070000", /* 24070001 */
				/* Texture fix for the battle mode */
				"patch=1,EE,E0011183,extended,001E0784",
				"patch=1,EE,201E0784,extended,24021D00"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Tales of Legendia (NTSC-U) [CRC: 43AB7214] */
		else if (!strcmp(serial, "SLUS-21201"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,D03F9750,extended,00001000",
				"patch=1,EE,103F9750,extended,000010E0"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Tekken Tag Tournament (NTSC-U) [CRC: 67454C1E] */
		else if (!strcmp(serial, "SLUS-20001")) 
		{
			int i;
			char *patches[] = {
				"patch=0,EE,20398960,extended,0000382D",
				"patch=0,EE,20398AF0,extended,0000502D",
				"patch=0,EE,10398AE0,extended,240701C0",
				"patch=0,EE,20398AF0,extended,0000502D",
				"patch=0,EE,10398B10,extended,240701C0",
				"patch=0,EE,10398B38,extended,240701C0",
				"patch=0,EE,20398B48,extended,0000502D"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Tekken Tag Tournament (PAL) [CRC: 0DD8941C] */
		else if (!strcmp(serial, "SCES-50001")) 
		{
			int i;
			char *patches[] = {
				"patch=0,EE,203993D0,extended,0000382D",
				"patch=0,EE,10399580,extended,240700E0",
				"patch=0,EE,103995A8,extended,240701C0",
				"patch=0,EE,203995B8,extended,0000502D",
				"patch=0,EE,2039DDE8,extended,0000382D"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Tekken 4 (PAL) */
		else if (!strcmp(serial, "SCES-50878"))
		{
			/* Patch courtesy: felixthecat1970 */
			int i;
			char *patches[] = {
				"patch=0,EE,001E2254,extended,24020002",
				"patch=0,EE,0022B138,extended,24050006",
				"patch=0,EE,001EDC24,extended,24020009"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Tekken 5 (NTSC-U) [CRC: 652050D2] */
		else if (!strcmp(serial, "SLUS-21059")) 
		{
			/* Patch courtesy: felixthecat1970 */
			int i;
			char *patches[] = {
				"patch=0,EE,00D05EC8,extended,24050000",
				"patch=0,EE,00D05ECC,extended,24060050",
				"patch=0,EE,20D05ED4,extended,24070001",
				/* Devil Within upscaling */
				"patch=1,EE,E0078870,extended,01FFEF20",
				"patch=1,EE,202DE308,extended,AC940004", /* enable progressive at start - skips Starblade minigame */
				"patch=1,EE,202F06DC,extended,341B0001",
				"patch=1,EE,202F08FC,extended,A07B0000",
				/* sharp backbuffer main game - skips StarBlade intro game */
				"patch=1,EE,0031DA9C,extended,30630000",
				"patch=1,EE,00335A38,extended,24020001",
				"patch=1,EE,20335A5C,extended,00031C02",
				"patch=1,EE,20335E58,extended,00042402",
				/* Devil Within - sharp backbuffer */
				"patch=1,EE,E0020001,extended,0027E448",
				"patch=1,EE,2027E448,extended,00500000",
				"patch=1,EE,203F7330,extended,00500000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Whiplash (NTSC-U) [CRC: 4D22DB95] */
		else if (!strcmp(serial, "SLUS-20684"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,2025DFA4,extended,30630000",
				"patch=1,EE,20353958,extended,34030001",
				"patch=1,EE,2035396C,extended,34029040"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
	}

	if (setting_pgs_disable_mipmaps == 1)
	{
		/* The games listed below need patches when mipmapping
		 * is set to unclamped */

		/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
		if (!strcmp(serial, "SLUS-20685"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,0034CE88,word,00000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Lara Croft Tomb Raider - Legend (NTSC-U) [CRC: BC8B3F50] */
		else if (!strcmp(serial, "SLUS-21203"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,00127390,word,10000022"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Legacy of Kain: Soul Reaver, The (NTSC-U) [CRC: 1771BFE4]) */
		else if (!strcmp(serial, "SLUS-20165"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,0029FC00,word,000000"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Quake III - Revolution (NTSC-U) [CRC: A56A0525] */
		else if (!strcmp(serial, "SLUS-20167"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,002D0398,word,03E00008"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
		/* Whiplash (NTSC-U) [CRC: 4D22DB95] */
		else if (!strcmp(serial, "SLUS-20684"))
		{
			int i;
			char *patches[] = {
				"patch=1,EE,0025D19C,word,10000007"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
	}
}

void Host::OnGameChanged(const std::string& disc_path,
	const std::string& elf_override, const std::string& game_serial,
	u32 game_crc)
{
	lrps2_ingame_patches(game_serial.c_str(), setting_renderer.c_str(), setting_hint_nointerlacing);
}
