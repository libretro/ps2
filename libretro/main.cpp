#ifdef WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <libretro.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string>
#include <vector>
#include <type_traits>
#include <thread>
#include <atomic>

#include "libretro_core_options.h"

#include "../pcsx2/GS.h"
#include "../pcsx2/SPU2/Global.h"
#include "../pcsx2/ps2/BiosTools.h"
#include "../pcsx2/CDVD/CDVD.h"
#include "../pcsx2/MTVU.h"
#include "../pcsx2/Counters.h"
#include "../pcsx2/Host.h"

#include "../common/Path.h"
#include "../common/FileSystem.h"
#include "../common/MemorySettingsInterface.h"

#include "../pcsx2/GS/Renderers/Common/GSRenderer.h"
#ifdef ENABLE_VULKAN
#ifdef HAVE_PARALLEL_GS
#include "../pcsx2/GS/Renderers/parallel-gs/GSRendererPGS.h"
#endif
#include "../pcsx2/GS/Renderers/Vulkan/VKLoader.h"
#include "../pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h"
#include "../pcsx2/GS/Renderers/Vulkan/GSTextureVK.h"
#include <libretro_vulkan.h>
#endif
#include "../pcsx2/Frontend/InputManager.h"
#include "../pcsx2/Frontend/LayeredSettingsInterface.h"
#include "../pcsx2/VMManager.h"
#include "../pcsx2/Patch.h"

#include "../pcsx2/SPU2/spu2.h"
#include "../pcsx2/PAD/PAD.h"

#ifdef HAVE_PARALLEL_GS
extern std::unique_ptr<GSRendererPGS> g_pgs_renderer;
#endif

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

static std::atomic<VMState> cpu_thread_state;
static std::thread cpu_thread;

static freezeData fd = {};
static std::unique_ptr<u8[]> fd_data;
static bool defrost_requested = false;

enum PluginType : u8
{
	PLUGIN_PGS = 0,
	PLUGIN_GSDX_HW,
	PLUGIN_GSDX_SW
};

struct BiosInfo
{
	std::string filename;
	std::string description;
};

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
static u8 setting_pgs_ss_tex                   = 0;
static u8 setting_pgs_deblur                   = 0;
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
static s8 setting_hint_language_unlock         = 0;
s8 setting_hint_widescreen                     = 0;
static s8 setting_hint_game_enhancements       = 0;
static s8 setting_hint_uncapped_framerate      = 0;
static s8 internal_setting_region              = RETRO_REGION_NTSC;
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
		const bool gsdx_hw_renderer  = !parallel_renderer && !gsdx_sw_renderer;
		const bool gsdx_renderer     = gsdx_hw_renderer || gsdx_sw_renderer;

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
		option_display.key     = "pcsx2_pgs_ss_tex";
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
		option_display.key     = "pcsx2_pgs_deblur";
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
	while(cpu_thread_state.load(std::memory_order_acquire) != VMState::Paused)
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

		var.key = "pcsx2_pgs_ss_tex";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_ss_tex_prev = setting_pgs_ss_tex;
			setting_pgs_ss_tex = !strcmp(var.value, "enabled");

			if (first_run || setting_pgs_ss_tex != pgs_ss_tex_prev)
			{
				s_settings_interface.SetIntValue("EmuCore/GS", "pgsSuperSampleTextures", setting_pgs_ss_tex);
				updated = true;
			}
		}

		var.key = "pcsx2_pgs_deblur";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 pgs_deblur_prev = setting_pgs_deblur;
			setting_pgs_deblur = !strcmp(var.value, "enabled");

			if (first_run || setting_pgs_deblur != pgs_deblur_prev)
			{
				s_settings_interface.SetIntValue("EmuCore/GS", "pgsSharpBackbuffer", setting_pgs_deblur);
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
		}
	}

	// Options for both paraLLEl-GS and GSdx HW/SW
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

	var.key = "pcsx2_hint_language_unlock";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "enabled"))
			setting_hint_language_unlock = 1;
		else
			setting_hint_language_unlock = 0;
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

	var.key = "pcsx2_widescreen_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		s8 setting_hint_widescreen_prev = setting_hint_widescreen;
		if (!strcmp(var.value, "disabled"))
			setting_hint_widescreen = 0;
		else if (!strcmp(var.value, "enabled (16:9)"))
			setting_hint_widescreen = 1;
		else if (!strcmp(var.value, "enabled (16:10)"))
			setting_hint_widescreen = 2;
		else if (!strcmp(var.value, "enabled (21:9)"))
			setting_hint_widescreen = 3;

		if (setting_hint_widescreen != setting_hint_widescreen_prev)
		{
			retro_system_av_info av_info;
			updated = true;
			retro_get_system_av_info(&av_info);
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info.geometry);
		}
	}

	var.key = "pcsx2_uncapped_framerate_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		s8 uncapped_framerate_hint_prev = setting_hint_uncapped_framerate;
		if (!strcmp(var.value, "disabled"))
			setting_hint_uncapped_framerate = 0;
		else if (!strcmp(var.value, "enabled"))
			setting_hint_uncapped_framerate = 1;
		else if (!strcmp(var.value, "60fps PAL-to-NTSC"))
			setting_hint_uncapped_framerate = 2;

		if (setting_hint_uncapped_framerate != uncapped_framerate_hint_prev)
			updated = true;
	}

	var.key = "pcsx2_game_enhancements_hint";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u8 game_enhancements_hint_prev = setting_hint_game_enhancements;
		if (!strcmp(var.value, "disabled"))
			setting_hint_game_enhancements = 0;
		else if (!strcmp(var.value, "enabled"))
			setting_hint_game_enhancements = 1;

		if (setting_hint_game_enhancements != game_enhancements_hint_prev)
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

	char input_settings[32];
	for (int i = 0; i < 2; ++i)
	{
		var.key = input_settings;
		snprintf(input_settings, sizeof(input_settings), "pcsx2_axis_scale%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].axis_scale = atof(var.value) / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_axis_deadzone%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].axis_deadzone = atoi(var.value) * 32767 / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_button_deadzone%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].button_deadzone = atoi(var.value) * 32767 / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_enable_rumble%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].rumble_scale = atof(var.value) / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_invert_left_stick%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "disabled"))
			{
				pad_settings[i].axis_invert_lx = 1;
				pad_settings[i].axis_invert_ly = 1;
			}
			else if (!strcmp(var.value, "x_axis"))
			{
				pad_settings[i].axis_invert_lx = -1;
				pad_settings[i].axis_invert_ly = 1;
			}
			else if (!strcmp(var.value, "y_axis"))
			{
				pad_settings[i].axis_invert_lx = 1;
				pad_settings[i].axis_invert_ly = -1;
			}
			else if (!strcmp(var.value, "all"))
			{
				pad_settings[i].axis_invert_lx = -1;
				pad_settings[i].axis_invert_ly = -1;
			}
		}

		snprintf(input_settings, sizeof(input_settings), "pcsx2_invert_right_stick%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			if (!strcmp(var.value, "disabled"))
			{
				pad_settings[i].axis_invert_rx = 1;
				pad_settings[i].axis_invert_ry = 1;
			}
			else if (!strcmp(var.value, "x_axis"))
			{
				pad_settings[i].axis_invert_rx = -1;
				pad_settings[i].axis_invert_ry = 1;
			}
			else if (!strcmp(var.value, "y_axis"))
			{
				pad_settings[i].axis_invert_rx = 1;
				pad_settings[i].axis_invert_ry = -1;
			}
			else if (!strcmp(var.value, "all"))
			{
				pad_settings[i].axis_invert_rx = -1;
				pad_settings[i].axis_invert_ry = -1;
			}
		}
	}

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
	info->geometry.base_width  = 640;
	info->geometry.base_height = 448;
	unsigned upscale_mul       = (setting_renderer == "paraLLEl-GS" && setting_pgs_high_res_scanout) ? 2 : setting_upscale_multiplier;


	if (               (  setting_renderer != "Software" 
			   && setting_renderer != "paraLLEl-GS")
			|| (  setting_renderer == "paraLLEl-GS" 
			   && setting_pgs_high_res_scanout))
	{
		info->geometry.base_width  *= upscale_mul;
		info->geometry.base_height *= upscale_mul;
	}

	info->geometry.max_width  = info->geometry.base_width;
	info->geometry.max_height = info->geometry.base_height;

	switch (setting_hint_widescreen)
	{
		case 1:
			info->geometry.aspect_ratio = 16.0f / 9.0f;
			break;
		case 2:
			info->geometry.aspect_ratio = 16.0f / 10.0f;
			break;
		case 3:
			info->geometry.aspect_ratio = 21.0f / 9.0f;
			break;
		case 4:
			info->geometry.aspect_ratio = 32.0f / 9.0f;
			break;
		case 0:
		default:
			info->geometry.aspect_ratio = 4.0f / 3.0f;
			break;
	}
	info->timing.fps            = (retro_get_region() == RETRO_REGION_NTSC) ? (60.0f / 1.001f) : 50.0f;
	info->timing.sample_rate    = 48000;
}

void retro_reset(void)
{
	cpu_thread_pause();
	VMManager::Reset();
	VMManager::SetPaused(false);
}

static bool freeze(void)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
	{
		if (g_pgs_renderer->Freeze(&fd, true) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(context_destroy) Failed to get GS freeze size\n");
			return false;
		}
	}
	else
#endif
	{
		if (g_gs_renderer->Freeze(&fd, true) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(context_destroy) Failed to get GS freeze size\n");
			return false;
		}
	}

	fd_data = std::make_unique<u8[]>(fd.size);
	fd.data = fd_data.get();

#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
	{
		if (g_pgs_renderer->Freeze(&fd, false) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(context_destroy) Failed to freeze GS\n");
			return false;
		}
	}
	else
#endif
	{
		if (g_gs_renderer->Freeze(&fd, false) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(context_destroy) Failed to freeze GS\n");
			return false;
		}
	}

	return true;
}
static void defrost(void)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
	{
		if (g_pgs_renderer->Defrost(&fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(context_reset) Failed to defrost\n");
			return;
		}
	}
	else
#endif
	{
		if (g_gs_renderer->Defrost(&fd) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "(context_reset) Failed to defrost\n");
			return;
		}
	}
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

	if (defrost_requested)
	{
		defrost_requested = false;
		defrost();
	}

	VMManager::SetPaused(false);
}

static void libretro_context_destroy(void)
{
	cpu_thread_pause();

	if (freeze())
		defrost_requested = true;

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
				VMState _st = VMManager::GetState();
				cpu_thread_state.store(_st, std::memory_order_release);
				switch (_st)
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

static void get_first_track_from_cue(std::string &path)
{
	// PCSX2 doesn't handle cue files
	// so just find the first 'FILE "<gametrack>.bin" BINARY' line
	// and extract the track filename from it
	char buffer[1024];
	char basedir[4096];
	const char *line_start = "FILE \"";
	const char *line_end = "\" BINARY";

	snprintf(basedir, sizeof(basedir), "%s", path.c_str());
	path_basedir(basedir);

	FILE *cue_file = fopen(path.c_str(), "r");
	if (!cue_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open cue file.\n");
		return;
	}

	while (fgets(buffer, sizeof(buffer), cue_file))
	{
		std::string line(buffer);
		size_t pos = line.find(line_start);
		if (pos != std::string::npos)
		{
			size_t start = pos + strlen(line_start);
			size_t end = line.find(line_end, start);
			if (end != std::string::npos)
			{
				fclose(cue_file);
				path = basedir + line.substr(start, end - start);
				return;
			}
		}
	}

	log_cb(RETRO_LOG_ERROR, "Failed to find a valid track from cue file.\n");
	fclose(cue_file);
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
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::SW);
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
		std::string game_path = game->path;

		if (!strcmp(path_get_extension(game->path), "cue"))
			get_first_track_from_cue(game_path);

		disk_images.push_back(game_path);
		boot_params.filename = game_path;
	}

	cpu_thread = std::thread(cpu_thread_entry, boot_params);

	return true;
}

void retro_set_region(unsigned val) { internal_setting_region = val; }
unsigned retro_get_region(void) { return internal_setting_region; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

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

	retro_set_region(RETRO_REGION_NTSC); /* set back to default */
}

void retro_run(void)
{
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		check_variables(false);

	Input::Update();

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	if (cpu_thread_state.load(std::memory_order_acquire) == VMState::Paused)
		VMManager::SetState(VMState::Running);

	RETRO_PERFORMANCE_INIT(pcsx2_run);
	RETRO_PERFORMANCE_START(pcsx2_run);

	MTGS::MainLoop(false);

	RETRO_PERFORMANCE_STOP(pcsx2_run);
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

int lrps2_ingame_patches(const char *serial,
		u32 game_crc,
		const char *renderer,
		bool nointerlacing_hint,
		bool disable_mipmaps,
		bool game_enhancements,
		int8_t hint_widescreen,
		int8_t uncapped_framerate,
		int8_t language_unlock);

void Host::OnGameChanged(const std::string& disc_path,
	const std::string& elf_override, const std::string& game_serial,
	u32 game_crc)
{
	int ret = 0;
	const char *serial = game_serial.c_str();
	log_cb(RETRO_LOG_INFO, "serial: %s\n", serial);

	ret = lrps2_ingame_patches(game_serial.c_str(),
			game_crc,
			setting_renderer.c_str(),
			setting_hint_nointerlacing,
			setting_pgs_disable_mipmaps,
			setting_hint_game_enhancements,
			setting_hint_widescreen,
			setting_hint_uncapped_framerate,
			setting_hint_language_unlock);

#if 0
	if (
			(
			   !strncmp("SLES-", serial, strlen("SLES-"))
			|| !strncmp("SCES-", serial, strlen("SCES-")))
			&& ret != 1
	   )
	{
		retro_set_region(RETRO_REGION_PAL);
		ret = 1;
	}
#endif

	if (ret == 1)
	{
		retro_system_av_info av_info;
		retro_get_system_av_info(&av_info);
		environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
	}
}
