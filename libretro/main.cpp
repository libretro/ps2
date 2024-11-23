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
#include "pcsx2/Patch.h"

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
static int setting_upscale_multiplier          = 1;
static int setting_half_pixel_offset           = 0;
static int setting_native_scaling              = 0;
static u8 setting_plugin_type                  = 0;
static u8 setting_pgs_super_sampling           = 0;
static u8 setting_pgs_high_res_scanout         = 0;
static u8 setting_pgs_disable_mipmaps          = 0;
static u8 setting_deinterlace_mode             = 0;
static u8 setting_texture_filtering            = 0;
static u8 setting_anisotropic_filtering        = 0;
static u8 setting_dithering                    = 0;
static u8 setting_blending_accuracy            = 0;
static u8 setting_cpu_sprite_size              = 0;
static u8 setting_cpu_sprite_level             = 0;
static u8 setting_software_clut_render         = 0;
static u8 setting_gpu_target_clut              = 0;
static u8 setting_auto_flush                   = 0;
static u8 setting_round_sprite                 = 0;
static u8 setting_texture_inside_rt            = 0;
static u8 setting_ee_cycle_skip                = 0;
static s8 setting_ee_cycle_rate                = 0;
static s8 setting_game_enhancements_hint       = 0;
static s8 setting_uncapped_framerate_hint      = 0;
static s8 setting_trilinear_filtering          = 0;
static bool setting_hint_nointerlacing         = false;
static bool setting_pcrtc_antiblur             = false;
static bool setting_enable_cheats              = false;
static bool setting_enable_hw_hacks            = false;
static bool setting_auto_flush_software        = false;
static bool setting_disable_depth_conversion   = false;
static bool setting_framebuffer_conversion     = false;
static bool setting_disable_partial_invalid    = false;
static bool setting_gpu_palette_conversion     = false;
static bool setting_preload_frame_data         = false;
static bool setting_align_sprite               = false;
static bool setting_merge_sprite               = false;
static bool setting_unscaled_palette_draw      = false;
static bool setting_force_sprite_position      = false;
static bool setting_pcrtc_screen_offsets       = false;
static bool setting_disable_interlace_offset   = false;

static bool setting_show_parallel_options      = true;
static bool setting_show_gsdx_options          = true;
static bool setting_show_gsdx_hw_only_options  = true;
static bool setting_show_gsdx_sw_only_options  = true;
static bool setting_show_shared_options        = true;
static bool setting_show_hw_hacks              = true;

static bool update_option_visibility(void)
{
	struct retro_variable var;
	struct retro_core_option_display option_display;
	bool updated                        = false;

	bool show_parallel_options_prev     = setting_show_parallel_options;
	bool show_gsdx_options_prev         = setting_show_gsdx_options;
	bool show_gsdx_hw_only_options_prev = setting_show_gsdx_hw_only_options;
	bool show_gsdx_sw_only_options_prev = setting_show_gsdx_sw_only_options;
	bool show_shared_options_prev       = setting_show_shared_options;
	bool show_hw_hacks_prev             = setting_show_hw_hacks;

	setting_show_parallel_options       = true;
	setting_show_gsdx_options           = true;
	setting_show_gsdx_hw_only_options   = true;
	setting_show_gsdx_sw_only_options   = true;
	setting_show_shared_options         = true;
	setting_show_hw_hacks               = true;

	// Show/hide video options
	var.key = "pcsx2_renderer";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool parallel_renderer = !strcmp(var.value, "paraLLEl-GS");
		const bool gsdx_sw_renderer  = !strcmp(var.value, "Software");
		const bool null_renderer     = !strcmp(var.value, "Null");
		const bool gsdx_hw_renderer  = !parallel_renderer && !gsdx_sw_renderer && !null_renderer;
		const bool gsdx_renderer     = gsdx_hw_renderer || gsdx_sw_renderer;

		if (null_renderer)
			setting_show_shared_options       = false;
		if (!gsdx_renderer)
			setting_show_gsdx_options         = false;
		if (!gsdx_hw_renderer)
			setting_show_gsdx_hw_only_options = false;
		if (!gsdx_sw_renderer)
			setting_show_gsdx_sw_only_options = false;
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

		updated                = true;
	}

	// GSdx HW/SW options
	if (setting_show_gsdx_options != show_gsdx_options_prev)
	{
		option_display.visible = setting_show_gsdx_options;
		option_display.key     = "pcsx2_texture_filtering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// GSdx HW only options, not compatible with SW
	if (setting_show_gsdx_hw_only_options != show_gsdx_hw_only_options_prev)
	{
		option_display.visible = setting_show_gsdx_hw_only_options;
		option_display.key     = "pcsx2_upscale_multiplier";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_trilinear_filtering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_anisotropic_filtering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_dithering";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_blending_accuracy";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_enable_hw_hacks";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// GSdx SW only options, not compatible with HW
	if (setting_show_gsdx_sw_only_options != show_gsdx_sw_only_options_prev)
	{
		option_display.visible = setting_show_gsdx_sw_only_options;
		option_display.key     = "pcsx2_auto_flush_software";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// Options compatible with both paraLLEl-GS and GSdx HW/SW
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
		option_display.key     = "pcsx2_pcrtc_screen_offsets";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_disable_interlace_offset";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

		updated                = true;
	}

	// Show/hide HW hacks
	var.key = "pcsx2_enable_hw_hacks";
	if ((environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !strcmp(var.value, "disabled")) ||
			!setting_show_gsdx_hw_only_options)
		setting_show_hw_hacks = false;

	if (setting_show_hw_hacks != show_hw_hacks_prev)
	{
		option_display.visible = setting_show_hw_hacks;
		option_display.key     = "pcsx2_cpu_sprite_size";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_cpu_sprite_level";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_software_clut_render";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_gpu_target_clut";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_auto_flush";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_texture_inside_rt";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_disable_depth_conversion";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_framebuffer_conversion";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_disable_partial_invalidation";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_gpu_palette_conversion";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_preload_frame_data";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_half_pixel_offset";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_native_scaling";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_round_sprite";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_align_sprite";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_merge_sprite";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_unscaled_palette_draw";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_force_sprite_position";
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
			setting_pgs_high_res_scanout = !strcmp(var.value, "enabled");

			if (first_run)
				s_settings_interface.SetUIntValue("EmuCore/GS", "pgsHighResScanout", setting_pgs_high_res_scanout);
#if 0
			// TODO: ATM it crashes when changed on-the-fly, re-enable when fixed
			// also remove "(Restart)" from the core option label
			else if (setting_pgs_high_res_scanout != pgs_high_res_scanout_prev)
			{
				retro_system_av_info av_info;
				s_settings_interface.SetUIntValue("EmuCore/GS", "pgsHighResScanout", setting_pgs_high_res_scanout);

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
			setting_pgs_disable_mipmaps = !strcmp(var.value, "enabled");

			if (first_run || setting_pgs_disable_mipmaps != pgs_disable_mipmaps_prev)
			{
				const u8 mipmap_mode = (u8)(setting_pgs_disable_mipmaps ? GSHWMipmapMode::Unclamped : GSHWMipmapMode::Enabled);
				s_settings_interface.SetUIntValue("EmuCore/GS", "hw_mipmap_mode", mipmap_mode);
				s_settings_interface.SetBoolValue("EmuCore/GS", "mipmap", !setting_pgs_disable_mipmaps);
				s_settings_interface.SetUIntValue("EmuCore/GS", "pgsDisableMipmaps", setting_pgs_disable_mipmaps);
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

		var.key = "pcsx2_pcrtc_screen_offsets";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool pcrtc_screen_offsets_prev = setting_pcrtc_screen_offsets;
			setting_pcrtc_screen_offsets = !strcmp(var.value, "enabled");

			if (first_run || setting_pcrtc_screen_offsets != pcrtc_screen_offsets_prev)
			{
				s_settings_interface.SetBoolValue("EmuCore/GS", "pcrtc_offsets", setting_pcrtc_screen_offsets);
				updated = true;
			}
		}

		var.key = "pcsx2_disable_interlace_offset";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool disable_interlace_offset_prev = setting_disable_interlace_offset;
			setting_disable_interlace_offset = !strcmp(var.value, "enabled");

			if (first_run || setting_disable_interlace_offset != disable_interlace_offset_prev)
			{
				s_settings_interface.SetBoolValue("EmuCore/GS", "disable_interlace_offset", setting_disable_interlace_offset);
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
				s_settings_interface.SetUIntValue("EmuCore/GS", "deinterlace_mode", setting_deinterlace_mode);
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

		var.key = "pcsx2_trilinear_filtering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			s8 trilinear_filtering_prev = setting_trilinear_filtering;
			if (!strcmp(var.value, "Automatic"))
				setting_trilinear_filtering = (s8)TriFiltering::Automatic;
			else if (!strcmp(var.value, "disabled"))
				setting_trilinear_filtering = (s8)TriFiltering::Off;
			else if (!strcmp(var.value, "Trilinear (PS2)"))
				setting_trilinear_filtering = (s8)TriFiltering::PS2;
			else if (!strcmp(var.value, "Trilinear (Forced)"))
				setting_trilinear_filtering = (s8)TriFiltering::Forced;

			if (first_run || setting_trilinear_filtering != trilinear_filtering_prev)
			{
				s_settings_interface.SetIntValue("EmuCore/GS", "TriFilter", setting_trilinear_filtering);
				updated = true;
			}
		}

		var.key = "pcsx2_anisotropic_filtering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 anisotropic_filtering_prev = setting_anisotropic_filtering;
			setting_anisotropic_filtering = atoi(var.value);

			if (first_run || setting_anisotropic_filtering != anisotropic_filtering_prev)
			{
				s_settings_interface.SetUIntValue("EmuCore/GS", "MaxAnisotropy", setting_anisotropic_filtering);
				updated = true;
			}
		}

		var.key = "pcsx2_dithering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 dithering_prev = setting_dithering;
			if (!strcmp(var.value, "disabled"))
				setting_dithering = 0;
			else if (!strcmp(var.value, "Scaled"))
				setting_dithering = 1;
			else if (!strcmp(var.value, "Unscaled"))
				setting_dithering = 2;
			else if (!strcmp(var.value, "Force 32bit"))
				setting_dithering = 3;

			if (first_run || setting_dithering != dithering_prev)
			{
				s_settings_interface.SetUIntValue("EmuCore/GS", "dithering_ps2", setting_dithering);
				updated = true;
			}
		}

		var.key = "pcsx2_blending_accuracy";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 blending_accuracy_prev = setting_blending_accuracy;
			if (!strcmp(var.value, "Minimum"))
				setting_blending_accuracy = (u8)AccBlendLevel::Minimum;
			else if (!strcmp(var.value, "Basic"))
				setting_blending_accuracy = (u8)AccBlendLevel::Basic;
			else if (!strcmp(var.value, "Medium"))
				setting_blending_accuracy = (u8)AccBlendLevel::Medium;
			else if (!strcmp(var.value, "High"))
				setting_blending_accuracy = (u8)AccBlendLevel::High;
			else if (!strcmp(var.value, "Full"))
				setting_blending_accuracy = (u8)AccBlendLevel::Full;
			else if (!strcmp(var.value, "Maximum"))
				setting_blending_accuracy = (u8)AccBlendLevel::Maximum;

			if (first_run || setting_blending_accuracy != blending_accuracy_prev)
			{
				s_settings_interface.SetUIntValue("EmuCore/GS", "accurate_blending_unit", setting_blending_accuracy);
				updated = true;
			}
		}

		var.key = "pcsx2_enable_hw_hacks";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool enable_hw_hacks_prev = setting_enable_hw_hacks;
			setting_enable_hw_hacks = !strcmp(var.value, "enabled");

			if (first_run || setting_enable_hw_hacks != enable_hw_hacks_prev)
			{
				s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", setting_enable_hw_hacks);
				updated = true;
			}
		}

		if (setting_enable_hw_hacks)
		{
			var.key = "pcsx2_cpu_sprite_size";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 cpu_sprite_size_prev = setting_cpu_sprite_size;
				setting_cpu_sprite_size = atoi(var.value);

				if (first_run || setting_cpu_sprite_size != cpu_sprite_size_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_CPUSpriteRenderBW", setting_cpu_sprite_size);
					updated = true;
				}
			}

			var.key = "pcsx2_cpu_sprite_level";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 cpu_sprite_level_prev = setting_cpu_sprite_level;
				if (!strcmp(var.value, "Sprites Only"))
					setting_cpu_sprite_level = 0;
				else if (!strcmp(var.value, "Sprites/Triangles"))
					setting_cpu_sprite_level = 1;
				else if (!strcmp(var.value, "Blended Sprites/Triangles"))
					setting_cpu_sprite_level = 2;

				if (first_run || setting_cpu_sprite_level != cpu_sprite_level_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_CPUSpriteRenderLevel", setting_cpu_sprite_level);
					updated = true;
				}
			}

			var.key = "pcsx2_software_clut_render";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 software_clut_render_prev = setting_software_clut_render;
				if (!strcmp(var.value, "disabled"))
					setting_software_clut_render = 0;
				else if (!strcmp(var.value, "Normal"))
					setting_software_clut_render = 1;
				else if (!strcmp(var.value, "Aggressive"))
					setting_software_clut_render = 2;

				if (first_run || setting_software_clut_render != software_clut_render_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_CPUCLUTRender", setting_software_clut_render);
					updated = true;
				}
			}

			var.key = "pcsx2_gpu_target_clut";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 gpu_target_clut_prev = setting_gpu_target_clut;
				if (!strcmp(var.value, "disabled"))
					setting_gpu_target_clut = (u8)GSGPUTargetCLUTMode::Disabled;
				else if (!strcmp(var.value, "Exact Match"))
					setting_gpu_target_clut = (u8)GSGPUTargetCLUTMode::Enabled;
				else if (!strcmp(var.value, "Check Inside Target"))
					setting_gpu_target_clut = (u8)GSGPUTargetCLUTMode::InsideTarget;

				if (first_run || setting_gpu_target_clut != gpu_target_clut_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_GPUTargetCLUTMode", setting_gpu_target_clut);
					updated = true;
				}
			}

			var.key = "pcsx2_auto_flush";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 auto_flush_prev = setting_auto_flush;
				if (!strcmp(var.value, "disabled"))
					setting_auto_flush = (u8)GSHWAutoFlushLevel::Disabled;
				else if (!strcmp(var.value, "Sprites Only"))
					setting_auto_flush = (u8)GSHWAutoFlushLevel::SpritesOnly;
				else if (!strcmp(var.value, "All Primitives"))
					setting_auto_flush = (u8)GSHWAutoFlushLevel::Enabled;

				if (first_run || setting_auto_flush != auto_flush_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_AutoFlushLevel", setting_auto_flush);
					updated = true;
				}
			}

			var.key = "pcsx2_texture_inside_rt";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 texture_inside_rt_prev = setting_texture_inside_rt;
				if (!strcmp(var.value, "disabled"))
					setting_texture_inside_rt = (u8)GSTextureInRtMode::Disabled;
				else if (!strcmp(var.value, "Inside Target"))
					setting_texture_inside_rt = (u8)GSTextureInRtMode::InsideTargets;
				else if (!strcmp(var.value, "Merge Targets"))
					setting_texture_inside_rt = (u8)GSTextureInRtMode::MergeTargets;

				if (first_run || setting_texture_inside_rt != texture_inside_rt_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", setting_texture_inside_rt);
					updated = true;
				}
			}

			var.key = "pcsx2_disable_depth_conversion";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool disable_depth_conversion_prev = setting_disable_depth_conversion;
				setting_disable_depth_conversion = !strcmp(var.value, "enabled");

				if (first_run || setting_disable_depth_conversion != disable_depth_conversion_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", setting_disable_depth_conversion);
					updated = true;
				}
			}

			var.key = "pcsx2_framebuffer_conversion";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool framebuffer_conversion_prev = setting_framebuffer_conversion;
				setting_framebuffer_conversion = !strcmp(var.value, "enabled");

				if (first_run || setting_framebuffer_conversion != framebuffer_conversion_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_CPU_FB_Conversion", setting_framebuffer_conversion);
					updated = true;
				}
			}

			var.key = "pcsx2_disable_partial_invalidation";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool disable_partial_invalid_prev = setting_disable_partial_invalid;
				setting_disable_partial_invalid = !strcmp(var.value, "enabled");

				if (first_run || setting_disable_partial_invalid != disable_partial_invalid_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", setting_disable_partial_invalid);
					updated = true;
				}
			}

			var.key = "pcsx2_gpu_palette_conversion";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool gpu_palette_conversion_prev = setting_gpu_palette_conversion;
				setting_gpu_palette_conversion = !strcmp(var.value, "enabled");

				if (first_run || setting_gpu_palette_conversion != gpu_palette_conversion_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "paltex", setting_gpu_palette_conversion);
					updated = true;
				}
			}

			var.key = "pcsx2_preload_frame_data";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool preload_frame_data_prev = setting_preload_frame_data;
				setting_preload_frame_data = !strcmp(var.value, "enabled");

				if (first_run || setting_preload_frame_data != preload_frame_data_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", setting_preload_frame_data);
					updated = true;
				}
			}

			var.key = "pcsx2_half_pixel_offset";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				int half_pixel_offset_prev = setting_half_pixel_offset;
				if (!strcmp(var.value, "disabled"))
					setting_half_pixel_offset = GSHalfPixelOffset::Off;
				else if (!strcmp(var.value, "Normal (Vertex)"))
					setting_half_pixel_offset = GSHalfPixelOffset::Normal;
				else if (!strcmp(var.value, "Special (Texture)"))
					setting_half_pixel_offset = GSHalfPixelOffset::Special;
				else if (!strcmp(var.value, "Special (Texture - Aggressive)"))
					setting_half_pixel_offset = GSHalfPixelOffset::SpecialAggressive;
				else if (!strcmp(var.value, "Align to Native"))
					setting_half_pixel_offset = GSHalfPixelOffset::Native;

				if (first_run || setting_half_pixel_offset != half_pixel_offset_prev)
				{
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", setting_half_pixel_offset);
					updated = true;
				}
			}

			var.key = "pcsx2_native_scaling";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				int native_scaling_prev = setting_native_scaling;
				if (!strcmp(var.value, "disabled"))
					setting_native_scaling = GSNativeScaling::NativeScaling_Off;
				else if (!strcmp(var.value, "Normal"))
					setting_native_scaling = GSNativeScaling::NativeScaling_Normal;
				else if (!strcmp(var.value, "Aggressive"))
					setting_native_scaling = GSNativeScaling::NativeScaling_Aggressive;

				if (first_run || setting_native_scaling != native_scaling_prev)
				{
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_native_scaling", setting_native_scaling);
					updated = true;
				}
			}

			var.key = "pcsx2_round_sprite";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				u8 round_sprite_prev = setting_round_sprite;
				if (!strcmp(var.value, "disabled"))
					setting_round_sprite = 0;
				else if (!strcmp(var.value, "Normal"))
					setting_round_sprite = 1;
				else if (!strcmp(var.value, "Aggressive"))
					setting_round_sprite = 2;

				if (first_run || setting_round_sprite != round_sprite_prev)
				{
					s_settings_interface.SetUIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", setting_round_sprite);
					updated = true;
				}
			}

			var.key = "pcsx2_align_sprite";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool align_sprite_prev = setting_align_sprite;
				setting_align_sprite = !strcmp(var.value, "enabled");

				if (first_run || setting_align_sprite != align_sprite_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", setting_align_sprite);
					updated = true;
				}
			}

			var.key = "pcsx2_merge_sprite";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool merge_sprite_prev = setting_merge_sprite;
				setting_merge_sprite = !strcmp(var.value, "enabled");

				if (first_run || setting_merge_sprite != merge_sprite_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", setting_merge_sprite);
					updated = true;
				}
			}

			var.key = "pcsx2_unscaled_palette_draw";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool unscaled_palette_draw_prev = setting_unscaled_palette_draw;
				setting_unscaled_palette_draw = !strcmp(var.value, "enabled");

				if (first_run || setting_unscaled_palette_draw != unscaled_palette_draw_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_NativePaletteDraw", setting_unscaled_palette_draw);
					updated = true;
				}
			}

			var.key = "pcsx2_force_sprite_position";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				bool force_sprite_position_prev = setting_force_sprite_position;
				setting_force_sprite_position = !strcmp(var.value, "enabled");

				if (first_run || setting_force_sprite_position != force_sprite_position_prev)
				{
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", setting_force_sprite_position);
					updated = true;
				}
			}
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_SW)
	{
		var.key = "pcsx2_auto_flush_software";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool auto_flush_software_prev = setting_auto_flush_software;
			setting_auto_flush_software = !strcmp(var.value, "enabled");

			if (first_run || setting_auto_flush_software != auto_flush_software_prev)
			{
				s_settings_interface.SetBoolValue("EmuCore/GS", "autoflush_sw", setting_auto_flush_software);
				updated = true;
			}
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_HW || setting_plugin_type == PLUGIN_GSDX_SW)
	{
		var.key = "pcsx2_texture_filtering";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 texture_filtering_prev = setting_texture_filtering;
			if (!strcmp(var.value, "Nearest"))
				setting_texture_filtering = (u8)BiFiltering::Nearest;
			else if (!strcmp(var.value, "Bilinear (Forced)"))
				setting_texture_filtering = (u8)BiFiltering::Forced;
			else if (!strcmp(var.value, "Bilinear (PS2)"))
				setting_texture_filtering = (u8)BiFiltering::PS2;
			else if (!strcmp(var.value, "Bilinear (Forced excluding sprite)"))
				setting_texture_filtering = (u8)BiFiltering::Forced_But_Sprite;

			if (first_run || setting_texture_filtering != texture_filtering_prev)
			{
				s_settings_interface.SetUIntValue("EmuCore/GS", "filter", setting_texture_filtering);
				updated = true;
			}
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

	var.key = "pcsx2_uncapped_framerate_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u8 uncapped_framerate_hint_prev = setting_uncapped_framerate_hint;
		if (!strcmp(var.value, "disabled"))
			setting_uncapped_framerate_hint = 0;
		else if (!strcmp(var.value, "enabled"))
			setting_uncapped_framerate_hint = 1;

		if (setting_uncapped_framerate_hint != uncapped_framerate_hint_prev)
			updated = true;
	}

	var.key = "pcsx2_game_enhancements_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u8 game_enhancements_hint_prev = setting_game_enhancements_hint;
		if (!strcmp(var.value, "disabled"))
			setting_game_enhancements_hint = 0;
		else if (!strcmp(var.value, "enabled"))
			setting_game_enhancements_hint = 1;

		if (setting_game_enhancements_hint != game_enhancements_hint_prev)
			updated = true;
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
	struct retro_variable var;

	log_cb(RETRO_LOG_INFO, "serial: %s\n", serial);

	if (nointerlacing_hint)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
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
			/* Ace Combat 5 - The Unsung War (NTSC-U) [CRC: 39B574F0] */
			else if (!strcmp(serial, "SLUS-20851"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,2032CA34,extended,0C03FFF3",
					"patch=1,EE,2032CA3C,extended,00000000",
					"patch=1,EE,200FFFCC,extended,341B0001",
					"patch=1,EE,200FFFD0,extended,147B0004",
					"patch=1,EE,200FFFD4,extended,34030001",
					"patch=1,EE,200FFFD8,extended,FC430000",
					"patch=1,EE,200FFFDC,extended,03E00008",
					"patch=1,EE,200FFFE0,extended,DE020010",
					"patch=1,EE,200FFFE4,extended,FC430000",
					"patch=1,EE,200FFFE8,extended,DE020010",
					"patch=1,EE,200FFFEC,extended,03E00008",
					"patch=1,EE,200FFFF0,extended,30429400"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ace Combat Zero - The Belkan War (NTSC-U) [CRC: 65729657] */
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
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				int i;
				char *patches[] = {
					/* Always ask for progressive scan */
					"patch=0,EE,2019778C,extended,10A2001C"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Drakengard (NTSC-U) [CRC: 9679D44C] */
			else if (!strcmp(serial, "SLUS-20732"))
			{
				/* TODO/FIXME - screen cutoff a little on the bottom */
				int i;
				char *patches[] = {
					/* NOP interlacing */
					"patch=1,EE,204F2668,extended,00000050",
					"patch=1,EE,204F2674,extended,000001E0",
					"patch=1,EE,204F2684,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Drakengard 2 (NTSC-U) [CRC: 1648E3C9] */
			else if (!strcmp(serial, "SLUS-21373"))
			{
				/* Patch courtesy: umechan */
				/* TODO/FIXME - screen cutoff a little on the bottom */
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
			/* God Hand (NTSC-U) [CRC: 6FB69282] */
			else if (!strcmp(serial, "SLUS-21503"))
			{
				int i;
				char *patches[] = {
					"patch=0,EE,002BE190,extended,24050000",
					"patch=0,EE,002BE194,extended,24060050",
					"patch=0,EE,2030CD10,extended,240E0070",
					"patch=0,EE,2030CD8C,extended,00000000"
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
			/* Rumble Roses (NTSC-U) [CRC: C1C91715] */
			else if (!strcmp(serial, "SLUS-20970"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* Framebuffer Display and no interlacing */
				if (setting_renderer == "paraLLEl-GS")
				{
					int i;
					char *patches[] = {
						"patch=1,EE,2010291C,extended,00000000",
						"patch=1,EE,20102B84,extended,00000000",
						"patch=1,EE,E0041100,extended,01D4ADA0",
						"patch=1,EE,21D4AD98,extended,00000001",
						"patch=1,EE,21D4ADA0,extended,00001000",
						"patch=1,EE,21D4ADC0,extended,00000001",
						"patch=1,EE,21D4ADC8,extended,00001000"
						/* TODO/FIXME - we're missing the upscaling 
						 * of the menu/startup screens */
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
				else
				{
					int i;
					char *patches[] = {
						"patch=1,EE,2010291C,extended,00000000",
						"patch=1,EE,20102B84,extended,00000000",
						"patch=1,EE,E0041100,extended,01D4ADA0",
						"patch=1,EE,21D4AD98,extended,00000001",
						"patch=1,EE,21D4ADA0,extended,00001000",
						"patch=1,EE,21D4ADC0,extended,00000001",
						"patch=1,EE,21D4ADC8,extended,00001000"
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
			}
			/* Shaun Palmer's Pro Snowboarder (NTSC-U) [CRC: 3A8E10D7] */
			else if (!strcmp(serial, "SLUS-20199"))
			{
				/* Patch courtesy: felixthechat1970 */
				int i;
				char *patches[] = {
					/* test s.backbuffer - frame mode by felixthecat1970 */
					/* menu is field render, use deinterlacing=auto */
					"patch=0,EE,2012B6C4,extended,0000102D",
					"patch=0,EE,2012B6E8,extended,00041803",
					"patch=0,EE,2012B714,extended,0000502D",
					"patch=0,EE,2012B730,extended,0000282D",
					"patch=0,EE,2012B750,extended,00083003",
					"patch=0,EE,2012B780,extended,0000502D"
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
			/* Tekken 5 (NTSC-U) [CRC: 652050D2] */
			else if (!strcmp(serial, "SLUS-21059")) 
			{
				/* Patch courtesy: felixthecat1970 */
				/* TODO/FIXME - decouple widescreen */
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
			/* Urban Reign (NTSC-U) [CRC: BDD9BAAD] */
			else if (!strcmp(serial, "SLUS-21209"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,201372e0,extended,0C04DCEC",
					"patch=1,EE,201372e8,extended,0C04DCEC"
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
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			if (!strcmp(serial, "SCUS-97481"))
			{
				int i;
				char *patches[] = {
					/* Default to progressive scan at first run */
					"patch=1,EE,0025a608,word,a04986dc",
					"patch=1,EE,001E45D4,word,24020001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Gran Turismo 4 (NTSC-U) [CRC: 77E61C8A] */
			else if (!strcmp(serial, "SCUS-97328"))
			{
				int i;
				char *patches[] = {
					/* Autoboot mode NTSC=0 / 480p=1 / 1080i=2 
					 * (change last number) or disable this code. */
					"patch=1,EE,20A461F0,extended,00000001"
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
				/* Stops company logos and intro FMV from shaking. 
				 * Menus and in-game never had an issue */
				int i;
				char *patches[] = {
					"patch=1,EE,201ABB34,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Ace Combat: Squadron Leader (PAL) [CRC: 1D54FEA9] */
			if (!strcmp(serial, "SCES-52424"))
			{
				int i;
				char *patches[] = {
					/* NOP the addition of front buffer address */
					"patch=1,EE,0032B0A8,word,00000000", /* 00A22825 */
					/* set the SMODE2 register to FRAME mode */
					"patch=1,EE,003311B8,word,00000000", /* 14400002 */
					/* force the 448 height for GS_DISPLAY2 
					 * register calculations (back buffer height is 448) */
					"patch=1,EE,00331124,word,241200E0", /* 00079403 */
					/* Last minute lazy fix for stuttering FMVs. Game does 
					 * render the prerecorded movies into the two interleaved 
					 * buffers. We need to remove the first patch when the 
					 * FMVs are played. */
					"patch=1,EE,E0011400,extended,0059660C",
					"patch=1,EE,2032B0A8,extended,00A22825"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ico (PAL) [CRC: 5C991F4E] */
			else if (!strcmp(serial, "SCES-50760"))
			{
				/* Patch courtesy: agrippa */
				int i;
				char *patches[] = {
					/* Set the back buffer */
					"patch=1,EE,2028F500,extended,00001040",
					"patch=1,EE,2028F528,extended,00001040",
					/* Switch to the interlaced mode with FFMD set to 0. 
					 * Progressive mode, applied by default,
					 * does add a black bar at the bottom in the NTSC mode 
					 * when the back buffer is enabled */
					"patch=1,EE,2028F4F8,extended,00000001",
					"patch=1,EE,2028F520,extended,00000001",
					/* Check if the PAL mode is turned on to extend 
					 * the display buffer from 256 to 512 */
					"patch=1,EE,E0024290,extended,0028F508",
					"patch=1,EE,2028F50C,extended,001FF9FF",
					"patch=1,EE,2028F534,extended,001FF9FF",
					/* Check if the NTSC mode is turned on to extend 
					 * the display buffer from 224 to 448 */
					"patch=1,EE,E002927C,extended,0028F508",
					"patch=1,EE,2028F50C,extended,001DF9FF",
					"patch=1,EE,2028F534,extended,001DF9FF"
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
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Colin McRae Rally 3 (PAL) [CRC: 7DEAE69C] */
			if (!strcmp(serial, "SLES-51117")) 
			{
				/* Patch courtesy: agrippa */
				int i;
				char *patches[] = {
					"patch=1,EE,00246B90,word,24040001", 
					/* set FFMD to 0 in SMODE2 register to 
					 * disable field mode */
					"patch=1,EE,00247A64,word,00000000"  
					/* NOP the switch to the front buffer 
					 * A full height back buffer enabled, 
					 * instead of a downsampled front buffer. */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Resident Evil - Dead Aim (PAL) [CRC: F79AF536] */
			else if (!strcmp(serial, "SLES-51448")) 
			{
				/* Patch courtesy: dante3732 */
				int i;
				char *patches[] = {
					"patch=1,EE,2028AB88,extended,00000050",
					"patch=1,EE,2028AB94,extended,000001E0",
					"patch=1,EE,2028ABA4,extended,00000000"
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
					/* in-battle anti-cheat checks? I have 
					 * not seen the game to get there though. */
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
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* Alpine Racer 3 (NTSC-J) [CRC: 771C3B47] */
			if (!strcmp(serial, "SLPS-20181"))
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
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Mushihimesama (NTSC-J) [CRC: F0C24BB1] */
			if (!strcmp(serial, "SLPM-66056"))
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
		}

	}

	if (setting_pgs_disable_mipmaps == 1)
	{
		/* The games listed below need patches when mipmapping
		 * is set to unclamped */

		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Ace Combat 5 - The Unsung War (NTSC-U) [CRC: 39B574F0] */
			if (!strcmp(serial, "SLUS-20851"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0011F2CC,word,00000000",
					"patch=1,EE,0011F2DC,word,00000000",
					"patch=1,EE,0011F2E8,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Aggressive Inline (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20327"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,001090B0,word,45010009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
			else if (!strcmp(serial, "SLUS-20685"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0034CE88,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* F1 Career Challenge (SLUS-20693) [CRC: 2C1173B0] */
			else if (!strcmp(serial, "SLUS-20693"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00257a40,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter - Quidditch World Cup (NTSC-U) [CRC: 39E7ECF4] */
			else if (!strcmp(serial, "SLUS-20769"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002ABD7C,word,00000000"
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
			/* Harry Potter and the Goblet of Fire (NTSC-U) [CRC: B38CC628] */
			else if (!strcmp(serial, "SLUS-21325"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002CF158,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Jak II: Renegade (NTSC-U) [CRC: 9184AAF1] */
			if (!strcmp(serial, "SCUS-97265"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,005F8D08,word,10000016"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Jak III (NTSC-U) [CRC: 644CFD03] */
			else if (!strcmp(serial, "SCUS-97330"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0059F570,word,10000016"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Jak X [CRC: 3091E6FB] */
			else if (!strcmp(serial, "SCUS-97574"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,007AEB70,word,10000016",
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Aggressive Inline (PAL) [CRC: ] */
			if (!strcmp(serial, "SLES-50480"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00109130,word,45010009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* F1 Career Challenge (SLES-51584) [CRC: 2C1173B0] */
			else if (!strcmp(serial, "SLES-51584"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00257a40,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter - Quidditch World Cup (PAL) */
			else if (!strcmp(serial, "SLES-51787"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002ABD4C,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter and the Goblet of Fire (NTSC-U) [CRC: B38CC628] */
			else if (    
					   !strcmp(serial, "SLES-53728")
					|| !strcmp(serial, "SLES-53727")
					|| !strcmp(serial, "SLES-53726")
				)
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002CF158,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Harry Potter - Quidditch World Cup (NTSC-J) */
			if (!strcmp(serial, "SLPM-62408"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002ABC04,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* F1 Career Challenge (NTSC-J) [CRC: 5CBB11E6] */
			if (!strcmp(serial, "SLPS-20295"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002581d8,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}

	var.key = "pcsx2_fastcdvd";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) 
			&& var.value && !strcmp(var.value, "enabled"))
	{
		/* Shadow Man: 2econd Coming (NTSC-U) [CRC: 60AD8FA7] */
		if (!strcmp(serial, "SLUS-20413"))
		{
			/* Only works with fastcdvd when enabling these patches */
			int i;
			char *patches[] = {
				"patch=1,IOP,000884e8,word,34048800",
				"patch=1,IOP,000884ec,word,34048800",
				"patch=1,IOP,00088500,word,34048800",
				"patch=1,IOP,0008850c,word,34048800",
				"patch=1,IOP,000555e8,word,34048800",
				"patch=1,IOP,000555ec,word,34048800",
				"patch=1,IOP,00055600,word,34048800",
				"patch=1,IOP,0005560c,word,34048800"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
	}

	if (setting_game_enhancements_hint)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			if (!strcmp(serial, "SLUS-21050"))
			{
				int i;
				char *patches[] = {
					/* Enable props in Road Rage mode */
					"patch=0,EE,201B9F60,extended,00000000",
					"patch=0,EE,202F9A44,extended,00000000",
					/* Unlimited explosions (also affects crash mode) */
					"patch=0,EE,201BBA08,extended,00000000",
					/* Render extra particles while driving */
					"patch=0,EE,20261EB8,extended,24040001",
					/* Use 255 colors in garage. 
					 * (Doesn't jump to 254 after the 8th color.) */
					"patch=1,EE,2042BCE8,extended,70A028E8",
					/* bypass PVS/force render all immediate units */
					"patch=1,EE,20301EAC,extended,00000000",
					/* Force specific LOD */
					"patch=0,EE,00151ABF,extended,00000010",
					/* Last digit is LOD level, 
					 * 0, 1, 2, 3, and 4 (4 being the most detailed iirc) */
					"patch=0,EE,20151B78,extended,24070004",
					"patch=0,EE,20261E6C,extended,24120001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				int i;
				char *patches[] = {
					/* Enable props in World Tour Road Rage */
					"patch=0,EE,20129FF8,extended,00000000",
					/* Enable props in Multiplayer/Single Event Road Rage */
					"patch=0,EE,2012648C,extended,00000000",
					/* Enable props in Traffic Attack mode */
					"patch=0,EE,20123C1C,extended,00000000",
					/* Force race cars LOD to 5 */
					"patch=0,EE,202D1660,extended,03E00008",
					"patch=0,EE,202D1664,extended,24020004",
					/* Prevent race cars reflections from fading further away */
					"patch=0,EE,202D165C,extended,E4C30000",
					/* Falling car parts while driving 
					 * (takedowns and traffic checks) */
					"patch=0,EE,20210FA8,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dynasty Warriors 4 (NTSC-U) [CRC: 6C89132B] [UNDUB] [CRC: 6C881C2B] */
			else if (!strcmp(serial, "SLUS-20653"))
			{
				int i;
				char *patches[] = {
					/* Disable Distance Based Model Disappearing */
					"patch=1,EE,001ce0d0,word,00000000",
					/* High LOD */
					"patch=1,EE,0018C8d0,word,00000000",
					"patch=1,EE,0018CE9C,word,00000000",
					/* Skip Events with X Button (DUELS ACCEPT IS SQUARE) */
					"patch=1,EE,0020BB98,word,24034008",
					"patch=1,EE,0020BA94,word,30638000",
					/* FMV Skip with X button */
					"patch=1,EE,002100A4,word,30424008",
					/* Able to Skip Koei Logo */
					"patch=1,EE,00362CEC,word,00210090",
					/* Increase default of 24 max units 
					 * rendered at the same time to 26. */
					"patch=1,EE,001CDFB0,word,2403001a"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dynasty Warriors 4: Empires (NTSC-U] [CRC: BD3DBCF9] */
			else if (!strcmp(serial, "SLUS-20938"))
			{
				int i;
				char *patches[] = {
					/* Increased draw distance Empires */
					"patch=1,EE,0015648C,word,00000000",
					"patch=1,EE,0015643C,word,00000000",
					"patch=1,EE,20508F1C,word,463b8000", /* 1P Mode */
					"patch=1,EE,20508F40,word,463b8000",
					"patch=1,EE,20508F64,word,463b8000",
					"patch=1,EE,20508FAC,word,463b8000",
					"patch=1,EE,20508FD0,word,463b8000",
					"patch=1,EE,20508F18,word,4633b000",
					"patch=1,EE,20508F3c,word,4633b000",
					"patch=1,EE,20508F60,word,4633b000",
					"patch=1,EE,20508FA8,word,4633b000",
					"patch=1,EE,20508Fcc,word,4633b000",
					/* Increases default of 24 maximum units 
					 * rendered at the same time to 28. */
					"patch=1,EE,001cbd34,word,2402001c"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Gran Turismo 4 (NTSC-U) [CRC: 77E61C8A] */
			else if (!strcmp(serial, "SCUS-97328"))
			{
				int i;
				char *patches[] = {
					/* Max LOD cars */
					"patch=1,EE,204539C0,extended,10000009",
					"patch=1,EE,20454FBC,extended,1000000E"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCPS-", serial, strlen("SCPS-")))
		{
			/* Gran Turismo 3 A-Spec (NTSC-J) [CRC: 9DE5CF65] */
			if (!strcmp(serial, "SCPS-15009"))
			{
				int i;
				char *patches[] = {
					/* Max car LODs */
					"patch=1,EE,21BD8A,short,1000",
					"patch=1,EE,21CA16,short,1000",
					"patch=1,EE,21F2E2,short,1000",
					"patch=1,EE,2212A2,short,1000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Gran Turismo 4 Prologue (NTSC-J) [CRC: EF258742] */
			else if (!strcmp(serial, "SCPS-15055"))
			{
				int i;
				char *patches[] = {
					/* car higher LOD - higher LOD wheels */
					"patch=1,EE,2057702C,extended,756E656D",
					"patch=1,EE,00577030,extended,0000002F",
					"patch=1,EE,2055C344,extended,6E656D2F",
					"patch=1,EE,2055C348,extended,73252F75"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			if (!strcmp(serial, "SLPM-66212"))
			{
				int i;
				char *patches[] = {
					/* Render Distance Patch (required, adds +25%) */
					"patch=1,EE,2017B150,extended,00000000", 
					/* +100% Render Distance (0.35f, max without glitching) */
					"patch=1,EE,203832EC,word,3EB33333"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}

	if (setting_uncapped_framerate_hint)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* 24 - The Game (NTSC-U) [CRC: F1C7201E] */
			if (!strcmp(serial, "SLUS-21268"))
			{
				/* Patch courtesy: Red-tv */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					/* 60fps */
					"patch=1,EE,005F9808,word,00000001",
					/* Fix FMV */
					"patch=1,EE,e0010001,extended,0058EEF4",
					"patch=1,EE,205F9808,extended,00000002"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Baroque (NTSC-U) [CRC: 4566213C] */
			else if (!strcmp(serial, "SLUS-21714"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00556E70,word,00000000",
					/* Revert to 30fps in FMV and cutscenes */
					"patch=1,EE,e0010000,extended,005179C0",
					"patch=1,EE,20556E70,extended,00000001",
					/* Player Speed Modifier */
					"patch=1,EE,0013D770,word,3C033F00",
					"patch=1,EE,00143CA4,word,3C023F00",
					"patch=1,EE,00146FEC,word,3C033F00",
					/* Enemy and NPC Animation Speed Modifier */
					"patch=1,EE,00146E08,word,3C023f00",
					"patch=1,EE,00146DF0,word,3C033eCC",
					/* Camera Speed Modifier */
					"patch=1,EE,0013DCBC,word,3C023F80",
					/* Player's Gauge Speed Modifier */
					"patch=1,EE,001341d8,word,3c024000",
					"patch=1,EE,00133ff4,word,3c024000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Batman - Rise of Sin Tzu (NTSC-U) [CRC: 24280F22] */
			else if (!strcmp(serial, "SLUS-20709"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					"patch=1,EE,00534720,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Black (NTSC-U) [CRC: 5C891FF1] */
			else if (!strcmp(serial, "SLUS-21376"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,1040DF74,extended,00000001", /* 60 fps */
					"patch=1,EE,205A8A9C,extended,3C888889", /* speed */
					"patch=1,EE,204BC13C,extended,3C888889",
					"patch=1,EE,2040EBAC,extended,3C888889"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21050"))
			{
				int i;
				char *patches[] = {
					/* Fix FMVs playback speed while using 60 FPS patches */
					"patch=0,EE,201D3F2C,extended,1000000A",
					"patch=0,EE,20130DD8,extended,C7958074",
					"patch=0,EE,20130DDC,extended,3C084000",
					"patch=0,EE,20130DE0,extended,4488A000",
					"patch=0,EE,20130DE4,extended,4614AD03",
					"patch=0,EE,20130DE8,extended,00000000",

					"patch=0,EE,201320D8,extended,1000004B",

					"patch=0,EE,20437758,extended,100000F1"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				int i;
				char *patches[] = {
					/* 60fps Split Screen */
					"patch=1,EE,20104BC0,extended,080680A0",
					"patch=1,EE,20104BC4,extended,00000000",
					/* 60 FPS Front End */
					"patch=1,EE,201125F4,word,24040001",
					"patch=1,EE,201125EC,word,00108002",
					/* 60 FPS Crashes & Crash Mode */
					"patch=1,EE,20104B9C,word,90850608"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Cold Fear (NTSC-U) [CRC: ECFBAB36] */
			else if (!strcmp(serial, "SLUS-21047"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0046E484,extended,00000001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dark Angel - James Cameron's (NTSC-U) [CRC: 29BA2F04] */
			else if (!strcmp(serial, "SLUS-20379"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130% */
				int i;
				char *patches[] = {
					"patch=1,EE,0027F154,word,10400012"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dawn of Mana (NTSC-U) [CRC: 9DC6EE5A] */
			else if (!strcmp(serial, "SLUS-21574"))
			{
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					/* 28620002 fps without doubling speed */
					"patch=1,EE,20113010,extended,28620001",
					/* condition to avoid hang and skip FMVs */
					"patch=1,EE,E0010001,extended,005D7338", 
					"patch=1,EE,20113010,extended,28620002"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Destroy All Humans! (NTSC-U) [CRC: 67A29886] */
			else if (!strcmp(serial, "SLUS-20945"))
			{
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					"patch=1,EE,203EF80C,extended,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Deus Ex: The Conspiracy (NTSC) [CRC: 3AD6CF7E] */
			else if (!strcmp(serial, "SLUS-20111"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock to be stable. */
				int i;
				char *patches[] = {
					"patch=1,EE,2030D234,word,28420001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Echo Night - Beyond (NTSC) [CRC: 2DE16D21] */
			else if (!strcmp(serial, "SLUS-20928"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,2013FFDC,word,10000014"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Grand Theft Auto III (NTSC-U) [CRC: 5E115FB6] */
			else if (!strcmp(serial, "SLUS-20062"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2027CEAC,extended,28420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Grand Theft Auto: Vice City (NTSC-U) [CRC: 20B19E49] */
			else if (!strcmp(serial, "SLUS-20552"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,20272204,extended,28420001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Grand Theft Auto: San Andreas (NTSC-U) [CRC: 399A49CA] */
			else if (!strcmp(serial, "SLUS-20946"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,2039B53C,extended,24040001", /* Set VSync Mode to 60 FPS */
					"patch=1,EE,0066804C,word,10000001",
					"patch=1,EE,D066804C,word,10000001",
					"patch=1,EE,006678CC,extended,00000001" /* Framerate boost */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Jurassic: The Hunted (NTSC-U) [CRC:EFE4448F] */
			else if (!strcmp(serial, "SLUS-21907"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2017D480,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Lord of the Rings, Return of the King (NTSC-U) [CRC: 4CE187F6] */
			else if (!strcmp(serial, "SLUS-20770"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2014B768,extended,10000013" /* 14400003 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Max Payne 2: The Fall of Max Payne (NTSC-U) [CRC: CD68E44A] */
			else if (!strcmp(serial, "SLUS-20814"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,005D8DF8,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Gear Solid 2: Substance (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20554"))
			{
				/* Patch courtesy: flcl8193 */
				/* 60fps uncapped cutscenes. */
				int i;
				char *patches[] = {
					"patch=1,EE,001914F4,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Gear Solid 3: Subsistence (NTSC-U) (Disc 1) [CRC: ] */
			else if (!strcmp(serial, "SLUS-21359"))
			{
				/* Patch courtesy: felixthecat1970 */
				int i;
				char *patches[] = {
					/* override FPS lock functions calls, 
					 * disable others FPS codes */
					"patch=1,EE,20145830,extended,0C03FFE8",
					"patch=1,EE,200FFFA0,extended,241B0001",
					"patch=1,EE,200FFFA4,extended,145B0008",
					"patch=1,EE,200FFFA8,extended,00000000",
					"patch=1,EE,200FFFAC,extended,149B0006",
					"patch=1,EE,200FFFB0,extended,00000000",
					"patch=1,EE,200FFFB4,extended,161B0004",
					"patch=1,EE,200FFFB8,extended,00000000",
					"patch=1,EE,200FFFBC,extended,0000102D",
					"patch=1,EE,200FFFC0,extended,0000202D",
					"patch=1,EE,200FFFC4,extended,0000802D",
					"patch=1,EE,200FFFC8,extended,03E00008",
					"patch=1,EE,20145570,extended,24060001",
					"patch=1,EE,201453B4,extended,240B0001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Arms - Glitch in the System (NTSC-U) [CRC: E8C504C8] */
			else if (!strcmp(serial, "SLUS-20786"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,004B2C98,word,00000001" /* 00000002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Midnight Club 3 - DUB Edition (NTSC-U) v1.0 [CRC: 4A0E5B3A] */
			else if (!strcmp(serial, "SLUS-21029"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00617AB4,word,00000001" /* 00000002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed - Hot Pursuit 2 (NTSC-U) [CRC: 1D2818AF] */
			else if (!strcmp(serial, "SLUS-20362"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,0032F638,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed Underground 1 (NTSC-U) [CRC: CB99CD12] */
			else if (!strcmp(serial, "SLUS-20811"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2011060C,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed Underground 2 (NTSC-U) [CRC: F5C7B45F] */
			else if (!strcmp(serial, "SLUS-21065"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,201D7ED4,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Power Rangers - Dino Thunder (NTSC-U) [CRC: FCD89DC3] */
			else if (!strcmp(serial, "SLUS-20944"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,101400D4,extended,2403003C",
					"patch=0,EE,2020A6BC,extended,241B0001",
					"patch=0,EE,2020A6C4,extended,03E00008",
					"patch=0,EE,2020A6C8,extended,A39B8520",
					"patch=0,EE,2020A7A8,extended,241B0002",
					"patch=0,EE,2020A7F8,extended,A39B8520"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Prince of Persia: The Sands of Time (NTSC-U) [CRC: 7F6EB3D0] */
			else if (!strcmp(serial, "SLUS-20743"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0066D044,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Psi-Ops: The Mindgate Conspiracy (NTSC-U) [CRC: 9C71B59E] */
			else if (!strcmp(serial, "SLUS-20688"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2017AB28,extended,00000000" /* 1640FFE5 fps1 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Red Faction (NTSC-U) [CRC: FBF28175] */
			else if (!strcmp(serial, "SLUS-20073"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,20164F9C,extended,24040001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Red Faction II (NTSC-U) [CRC: 8E7FF6F8] */
			else if (!strcmp(serial, "SLUS-20442"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,201218A0,word,24040001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Reign of Fire (NTSC-U) [CRC: D10945CE] */
			else if (!strcmp(serial, "SLUS-20556"))
			{
				/* Patch courtesy: Gabominated */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00264E70,word,00000001", /* 00000002 */
					"patch=1,EE,001409b4,word,2402003c"  /* 2402001e native global speed */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));

			}
			/* Return to Castle Wolfenstein: Operation Resurrection (NTSC-U) [CRC: 5F4DB1DD] */
			else if (!strcmp(serial, "SLUS-20297"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,2017437C,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Rune - Viking Warlord (NTSC-U) [CRC: 1259612B] */
			else if (!strcmp(serial, "SLUS-20109"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,001305A4,extended,28420001" /* 28420002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Serious Sam - Next Encounter (NTSC-U) [CRC: 155466E8] */
			else if (!strcmp(serial, "SLUS-20907"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,20127580,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Sonic Heroes (NTSC-U) [CRC: 78FF4E3B] */
			else if (!strcmp(serial, "SLUS-20718"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					"patch=1,EE,004777C0,word,00000001", /* fps */
					"patch=1,EE,2028FF5C,word,24020001" /* speed */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Spawn - Armageddon (NTSC-U) [CRC: B7E7D66F] */
			else if (!strcmp(serial, "SLUS-20707"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=0,EE,00226830,word,24020001" /* 24020002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Splinter Cell - Pandora Tomorrow (NTSC-U) [CRC: 0277247B] */
			else if (!strcmp(serial, "SLUS-20958"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0018D778,word,24030001" /* 24030002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* SSX On Tour (NTSC-U) [CRC: 0F27ED9B] */
			else if (!strcmp(serial, "SLUS-21278"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					/* Forces the FrameHalver variable to 1
					 * 1 = 60fps, 2 = 30fps, and probably 3 = 15fps. */
					"patch=1,EE,003132b4,extended,01001124",
					/* Skipping some nonsense code that's probably 
					 * no longer needed */
					"patch=1,EE,003132b8,extended,15000010"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Summoner 2 (NTSC-U) [CRC: 93551583] */
			else if (!strcmp(serial, "SLUS-20448"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,2017BC34,word,24040001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Super Monkey Ball Deluxe (NTSC-U) [CRC: 43B1CD7F] */
			else if (!strcmp(serial, "SLUS-20918"))
			{
				/* Patch courtesy: gamehacking.org, by Josh_7774, & Gabominated, PCSX2 forum */
				/* 60fps uncapped. Breaks Golf & Tennis. */
				int i;
				char *patches[] = {
					"patch=1,EE,20146D04,extended,24020001",
					/* Following patches fixes FMVs */
					"patch=1,EE,004C318C,extended,00000001", 
					"patch=1,EE,E0010001,extended,00473478",
					"patch=1,EE,204C318C,extended,00000002"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Unreal Tournament (NTSC-U) [CRC: 5751CAC1] */
			else if (!strcmp(serial, "SLUS-20034"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,0012D134,extended,28420001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* XGRA - Extreme G Racing Association (NTSC-U) [CRC: 56B36513] */
			else if (!strcmp(serial, "SLUS-20632"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,002052B4,extended,30420004",
					"patch=1,EE,E0010000,extended,01FFE32C",
					"patch=1,EE,002052B4,extended,30420008"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Dark Angel (PAL) [CRC: 5BE3F481] */
			if (!strcmp(serial, "SLES-53414"))
			{
				/* Patch courtesy: PeterDelta */
				/* Uncapped. Need EE Overclock at 130% */
				int i;
				char *patches[] = {
					"patch=1,EE,00280B74,word,1040000D"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Echo Night - Beyond (PAL) [CRC: BBF8C3D6] */
			else if (!strcmp(serial, "SLES-53414"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. Select 60Hz */
				int i;
				char *patches[] = {
					"patch=1,EE,E001001E,extended,0028A348",
					"patch=1,EE,0028A348,extended,0000003C"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Arms - Glitch in the System (PAL) [CRC: AF399CCC] */
			else if (!strcmp(serial, "SLES-51758"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,004BEA90,word,00000001" /* 00000002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Psi-Ops: The Mindgate Conspiracy (PAL-M) [CRC: 5E7EB5E2] */
			else if (!strcmp(serial, "SLES-52702"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50/60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,0017ACD8,word,00000000" /* 1640FFE5 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Reign of Fire (PAL) [CRC: 79464D5E] */
			else if (!strcmp(serial, "SLES-50873"))
			{
				/* Patch courtesy: Gabominated */
				/* 50fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00265C70,word,00000001", /* 00000002 */
					"patch=1,EE,00140A50,word,24020032"  /* 24020019 native global speed */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));

			}
			/* Rune - Viking Warlord (PAL) [CRC: 52638022] */
			else if (!strcmp(serial, "SLES-50335"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,001307AC,extended,28420001" /* 28420002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Spawn - Armageddon (PAL) [CRC: 8C9BF4F9] */
			else if (!strcmp(serial, "SLES-52326"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=0,EE,00227CB0,word,24020001" /* 24020002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Splinter Cell - Pandora Tomorrow (PAL) [CRC: 80FAC91D] */
			else if (!strcmp(serial, "SLES-52149"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0018D7C8,word,24030001" /* 24030002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Primal (NTSC-U) [CRC: FCD89DC3] */
			if (!strcmp(serial, "SCUS-97142"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,204874FC,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}
}

void Host::OnGameChanged(const std::string& disc_path,
	const std::string& elf_override, const std::string& game_serial,
	u32 game_crc)
{
	lrps2_ingame_patches(game_serial.c_str(), setting_renderer.c_str(), setting_hint_nointerlacing);
}
