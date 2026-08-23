#ifdef WIN32
#include <windows.h>
#endif

#include <retro_atomic.h>
#include <retro_spsc.h>

#include <cstdint>
#include <libretro.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <vfs/vfs_hybrid.h>
#include <compat/strl.h>
#include <string>
#include <vector>
#include <type_traits>
#include <chrono>

#include "libretro_core_options.h"

#include "../pcsx2/GS.h"
#include "../pcsx2/SPU2/Global.h"
#include "../pcsx2/ps2/BiosTools.h"
#include "../pcsx2/CDVD/CDVD.h"
#include "../pcsx2/USB/USB.h"
#include "../pcsx2/DEV9/ATA/HddCreate.h"
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

retro_environment_t environ_cb;
retro_video_refresh_t video_cb;
retro_log_printf_t log_cb;
static retro_audio_sample_batch_t batch_cb;
struct retro_hw_render_callback hw_render;

MemorySettingsInterface s_settings_interface;

bool pending_update_av_info = false;
std::string libretro_content;

static retro_atomic_int_t cpu_thread_state;
/* Boot handshake: 0 = Initialize() still running, 1 = VM up, -1 = failed.
 * Written by cpu_thread_entry, read by retro_load_game.  VMManager::
 * Initialize() never waits on MTGS (GS opens lazily from retro_run), so
 * blocking retro_load_game on this cannot deadlock. */
static retro_atomic_int_t cpu_thread_boot_result;
static Threading::Thread cpu_thread;

/* Pause/resume coordination for cpu_thread.
 *
 * When the libretro thread asks the VM to pause (savestate, reset,
 * settings change), cpu_thread eventually loops to its 'case Paused'
 * branch and used to busy-spin there waiting for the state to change
 * back. That burned 100% of one core for the entire duration of any
 * libretro-thread side activity that holds the VM paused (multi-MB
 * savestate write/read, GPU context recreate, etc.).
 *
 * Now cpu_thread sleeps on cpu_thread_cv with predicate
 * "state != Paused"; cpu_thread_resume() does the resume-side state
 * transition under the mutex and notifies. A 100 ms timeout on the
 * wait is belt-and-suspenders insurance against a missed notify - the
 * normal path is instant via notify_one(). */
/* Counted semaphore instead of mutex+condvar: a Post is remembered, so
 * the resume side needs no lock to close the check-then-wait window -
 * if cpu_thread saw the old state and is about to Wait(), the Post
 * issued after the state store is already banked and the Wait returns
 * immediately.  Stale posts from earlier cycles are absorbed by the
 * predicate re-check loop around Wait(). */
static Threading::KernelSemaphore cpu_thread_resume_sema;

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
static u8 setting_hw_download_mode             = 0; /* GSHardwareDownloadMode::Enabled */
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
static bool setting_dev9_hdd                   = false;
static bool setting_dev9_eth                   = false;
static u32  setting_dev9_hdd_sectors           = 40u * (1024 * 1024 * 1024 / 512);
static bool setting_enable_hw_hacks            = false;
static bool setting_auto_flush_software        = false;
static bool setting_disable_depth_conversion   = false;
static bool setting_framebuffer_conversion     = false;
static bool setting_disable_partial_invalid    = false;
static bool setting_gpu_palette_conversion     = false;
static bool setting_preload_frame_data         = false;
static bool setting_use_external_gameindex     = false;

// Built-in GameIndex.yaml database, embedded in the core (GameDatabaseBuiltin.cpp).
extern const unsigned char g_gameDatabaseBuiltin[];
extern const size_t g_gameDatabaseBuiltinSize;
static bool setting_align_sprite               = false;
static bool setting_merge_sprite               = false;
static bool setting_unscaled_palette_draw      = false;
static bool setting_force_sprite_position      = false;
static bool setting_pcrtc_screen_offsets       = false;
static bool setting_disable_interlace_offset   = false;
static bool setting_shared_memory_cards        = true;

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
		const bool gsdx_sw_renderer  = !strcmp(var.value, "Software") ||
		                               !strcmp(var.value, "Software (HW)") ||
		                               !strcmp(var.value, "Software (SW)");
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
	while((VMState)retro_atomic_load_acquire_int(&cpu_thread_state) != VMState::Paused)
		MTGS::MainLoop(true);
}

/* Counterpart to cpu_thread_pause().  Store the new state, then Post:
 * because the semaphore counts, the post cannot be lost regardless of
 * where cpu_thread is in its check-then-wait.  Any new resume site
 * (replacing 'VMManager::SetPaused(false)' or
 * 'VMManager::SetState(VMState::Running)' that was paired with a prior
 * cpu_thread_pause) should call this instead. */
static void cpu_thread_resume(void)
{
	VMManager::SetPaused(false);
	cpu_thread_resume_sema.Post();
}

/* Renderer-setting helpers. The "Renderer" menu has two SW entries
 * that both run the PS2 GS in CPU rasterization (GSRendererSW) but
 * differ in which GSDevice is used for the post-rasterization
 * merge/interlace/present pipeline:
 *
 *   "Software (HW)" - uses the HW GSDevice (Vulkan/D3D11/D3D12/OpenGL)
 *     that the frontend offers, so merge/interlace/present happen on
 *     the GPU. Falls back to "Software (SW)" gracefully if no HW
 *     context is available (e.g. SDL2 frontend).
 *
 *   "Software (SW)" - uses GSDeviceSW, the all-CPU device path. No HW
 *     context is requested even when one is available.
 *
 * The legacy value "Software" is treated as "Software (HW)" so old
 * configs still work. */
static bool is_software_setting(const std::string& s)
{
	return s == "Software" || s == "Software (HW)" || s == "Software (SW)";
}

static bool is_software_sw_setting(const std::string& s)
{
	return s == "Software (SW)";
}

/* The HDD image is created lazily so the option works without any file
 * management on the user's part; an image that already exists is used
 * as-is regardless of the size option. */
static void dev9_ensure_hdd_image(u32 size_sectors)
{
	char path[PCSX2_PATH_MAX];
	pcsx2_path_join(path, sizeof(path), EmuFolders::Settings.c_str(), "DEV9hdd.raw");
	if (path_is_valid(path))
		return;
	log_cb(RETRO_LOG_INFO, "DEV9: creating HDD image '%s'\n", path);
	if (hdd_create(path, (u64)size_sectors * 512) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: failed to create HDD image '%s'\n", path);
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
			else if (is_software_setting(setting_renderer))
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
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
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

		var.key = "pcsx2_hw_download_mode";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			u8 hw_download_mode_prev = setting_hw_download_mode;
			if (!strcmp(var.value, "Accurate"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::Enabled;
			else if (!strcmp(var.value, "Disable Readbacks"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::NoReadbacks;
			else if (!strcmp(var.value, "Unsynchronized"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::Unsynchronized;
			else if (!strcmp(var.value, "Disabled"))
				setting_hw_download_mode = (u8)GSHardwareDownloadMode::Disabled;

			if (first_run || setting_hw_download_mode != hw_download_mode_prev)
			{
				s_settings_interface.SetIntValue("EmuCore/GS", "HWDownloadMode", setting_hw_download_mode);
				updated = true;
			}
		}
	}

	if (setting_plugin_type == PLUGIN_GSDX_HW)
	{
		var.key = "pcsx2_upscale_multiplier";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			/* Used only by the #if 0 on-the-fly re-apply block below
			 * (commented out pending the "crashes when changed live"
			 * fix). The (void) cast silences -Wunused-variable while
			 * the block is disabled, without dragging in C++17. */
			int upscale_multiplier_prev = setting_upscale_multiplier;
			(void)upscale_multiplier_prev;
			setting_upscale_multiplier = atoi(var.value);

			if (first_run)
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", setting_upscale_multiplier);
#if 0
			// TODO: ATM it crashes when changed on-the-fly, re-enable when fixed
			// also remove "(Restart)" from the core option label
			//
			// Likely fixed by the set_image retract in libretro_context_destroy
			// (the reinit used to release a view onto a pool texture CloseGS had
			// already destroyed) - retest in a frontend before re-enabling.
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

			var.key = "pcsx2_use_external_gameindex";
			if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
				setting_use_external_gameindex = !strcmp(var.value, "enabled");

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

	var.key = "pcsx2_dev9_hdd_size";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		u32 gib = 40;
		if (!strcmp(var.value, "8 GiB"))
			gib = 8;
		else if (!strcmp(var.value, "16 GiB"))
			gib = 16;
		else if (!strcmp(var.value, "20 GiB"))
			gib = 20;
		else if (!strcmp(var.value, "40 GiB"))
			gib = 40;
		else if (!strcmp(var.value, "80 GiB"))
			gib = 80;
		else if (!strcmp(var.value, "120 GiB"))
			gib = 120;
		setting_dev9_hdd_sectors = gib * (1024 * 1024 * 1024 / 512);
		s_settings_interface.SetUIntValue("DEV9/Hdd", "HddSizeSectors", setting_dev9_hdd_sectors);
	}

	var.key = "pcsx2_dev9_hdd";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool dev9_hdd_prev = setting_dev9_hdd;
		setting_dev9_hdd = !strcmp(var.value, "enabled");

		if (first_run || setting_dev9_hdd != dev9_hdd_prev)
		{
			if (setting_dev9_hdd)
				dev9_ensure_hdd_image(setting_dev9_hdd_sectors);
			s_settings_interface.SetBoolValue("DEV9/Hdd", "HddEnable", setting_dev9_hdd);
			updated = true;
		}
	}

	var.key = "pcsx2_dev9_eth";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		const bool dev9_eth_prev = setting_dev9_eth;
		setting_dev9_eth = !strcmp(var.value, "enabled");

		if (first_run || setting_dev9_eth != dev9_eth_prev)
		{
			s_settings_interface.SetBoolValue("DEV9/Eth", "EthEnable", setting_dev9_eth);
			if (setting_dev9_eth)
			{
				s_settings_interface.SetStringValue("DEV9/Eth", "EthApi", "Sockets");
				s_settings_interface.SetStringValue("DEV9/Eth", "EthDevice", "Auto");
			}
			updated = true;
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

	var.key = "pcsx2_shared_memory_cards";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		setting_shared_memory_cards = !strcmp(var.value, "enabled");
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

	/* MTVU / Instant VU1: restart-only (THREAD_VU1 must not flip while the
	 * MTGS ring is live - see the MainLoop handoff-mutex comment). On arm64
	 * VMManager rewrites vuThread at boot from its hardware-default hook, which
	 * runs after the settings layer is reset -- so the option also feeds
	 * VMManager::g_MtvuMenuDefault, which survives that reset. */
	if (first_run)
	{
		var.key = "pcsx2_mtvu";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			const bool mtvu_on = !strcmp(var.value, "enabled");
			/* Apply the same hardware gate as VMManager's boot hook.
			 * check_variables(true) runs after CPUThreadInitialize, so
			 * an ungated write here would overwrite the gated value and
			 * raise THREAD_VU1 with no MTVU worker spawned - MainLoop
			 * then waits forever on the handoff (boot hang on <3
			 * hardware threads with pcsx2_mtvu=enabled). */
			s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vuThread",
				mtvu_on && VMManager::MtvuHardwareAllowed());
			VMManager::g_MtvuMenuDefault = mtvu_on;
		}

		var.key = "pcsx2_instant_vu1";
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vu1Instant", !strcmp(var.value, "enabled"));
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
			/* Integer percent -> 16.16, exact; no float parse. */
			pad_settings[i].axis_scale_q16 = (u32)atoi(var.value) * 65536u / 100u;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_axis_deadzone%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].axis_deadzone = atoi(var.value) * 32767 / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_button_deadzone%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].button_deadzone = atoi(var.value) * 32767 / 100;

		snprintf(input_settings, sizeof(input_settings), "pcsx2_enable_rumble%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].rumble_scale_q8 = (u32)atoi(var.value) * 256u / 100u;

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

		snprintf(input_settings, sizeof(input_settings), "pcsx2_analog_mode%d", i + 1);
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			pad_settings[i].force_analog = !strcmp(var.value, "enabled");
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
/* The core only ever uses batch_cb (audio is queued in bulk via
 * upload_output_audio_buffer at end-of-frame). The libretro API still
 * requires this symbol to exist, so accept the callback and discard. */
void retro_set_audio_sample(retro_audio_sample_t /*cb*/) { }

/* Audio output buffer.
 *
 * SPU2 writes stereo int16 samples directly into this buffer during
 * retro_run() via the reserve/commit pair below. One bulk batch_cb()
 * upload happens at the end of retro_run().
 *
 * Threading: SPU2 (cpu_thread) produces, upload_output_audio_buffer
 * (libretro thread) consumes.  The MTGS vsync barrier serializes them
 * in the steady state but NOT during boot/loads or whenever the EE
 * runs ahead of retro_run (TSan: commit racing the end-of-frame
 * upload).  The handoff is a retro_spsc byte queue - lock-free SPSC
 * with release/acquire cursors, no mutex anywhere near SPU2 timing
 * paths.  retro_audio_reserve hands SPU2 a pointer into a
 * producer-PRIVATE staging array (cpu_thread only, no sync), and
 * retro_audio_commit publishes the filled samples with one
 * retro_spsc_write.  The consumer drains in bounded chunks.
 *
 * Bounded by design: if the frontend stalls long enough to fill about
 * 1.3 s of audio, further chunks are dropped (counted, logged once)
 * instead of the previous realloc-grow-without-limit. */
#define AUDIO_SPSC_BYTES    (1 << 18) /* 256 KB = 128K int16s, ~1.37 s stereo at 48 kHz */
#define AUDIO_STAGING_INT16 16384     /* above SPU2 SAMPLECOUNT-capped max reserve (~9.6K) */
static retro_spsc_t audio_spsc;
static bool         audio_spsc_ok;
static int16_t      audio_staging[AUDIO_STAGING_INT16];       /* cpu_thread-private */
/* Producer mode for the reserve->commit pair in flight: true when
 * reserve handed out a span inside the ring (zero-copy), false when
 * it fell back to the staging array because the contiguous span at
 * the head was smaller than the request (once per ring lap).
 * cpu_thread-private. */
static bool         audio_reserve_in_ring;
static uint32_t     audio_dropped_samples;
static bool         audio_drop_logged;


static void init_output_audio_buffer(int32_t capacity)
{
   (void)capacity;
   if (!audio_spsc_ok)
      audio_spsc_ok = retro_spsc_init(&audio_spsc, AUDIO_SPSC_BYTES);
   audio_dropped_samples = 0;
   audio_drop_logged     = false;
}

static void free_output_audio_buffer(void)
{
   /* Called with cpu_thread joined and no upload in flight. */
   if (audio_spsc_ok)
   {
      retro_spsc_free(&audio_spsc);
      audio_spsc_ok = false;
   }
}

static void upload_output_audio_buffer(void)
{
   /* Feed batch_cb directly from the ring: read_begin exposes the
    * contiguous readable span, whose region the producer cannot touch
    * until read_end frees it.  Frame granularity (4-byte commits)
    * keeps every span whole-frame-sized and int16-aligned.  At most
    * two iterations per drain (wrap).  An empty frame uploads
    * nothing - synthetic silence would break the 48 kHz contract. */
   const void *span;
   size_t      span_bytes;
   if (!audio_spsc_ok)
      return;
   while ((span_bytes = retro_spsc_read_begin(&audio_spsc, &span)) >= 2 * sizeof(int16_t))
   {
      const size_t take = span_bytes & ~(size_t)(2 * sizeof(int16_t) - 1);
      batch_cb((const int16_t*)span, take / (2 * sizeof(int16_t)));
      retro_spsc_read_end(&audio_spsc, take);
   }
}

/* Reserve room for `max_samples` int16s past the current write position
 * and return a writable pointer to the start of that region. The caller
 * fills as many samples as it wants up to max_samples, then calls
 * retro_audio_commit() with the actual count.
 *
 * This avoids allocating a per-call stack batch (TimeUpdate is __fi /
 * always_inline, called from many sites; a 4800-stereo stack array
 * would balloon every caller's frame) and the intermediate memcpy
 * the previous push-style API required - SPU2's Mix() now writes
 * straight into the persistent buffer. */

/* Discard everything queued.  Producer (cpu_thread) must be paused;
 * drains consumer-side because retro_spsc_clear requires both sides
 * stopped. */
static void discard_buffered_audio(void)
{
   /* Producer (cpu_thread) must be paused; drains consumer-side
    * because retro_spsc_clear requires both sides stopped.  Advances
    * the tail without copying anything. */
   const void *span;
   size_t      span_bytes;
   if (!audio_spsc_ok)
      return;
   while ((span_bytes = retro_spsc_read_begin(&audio_spsc, &span)) != 0)
      retro_spsc_read_end(&audio_spsc, span_bytes);
}

int16_t *retro_audio_reserve(int32_t max_samples)
{
   void  *span;
   size_t span_bytes;
   const size_t need = (size_t)max_samples * sizeof(int16_t);
   if (max_samples > (int32_t)AUDIO_STAGING_INT16 || !audio_spsc_ok)
      return NULL;
   /* Common case: hand SPU2's Mix a span inside the ring itself -
    * zero-copy end to end.  Every commit is a whole number of stereo
    * frames (4 bytes), so head only ever advances by multiples of 4
    * and the span is int16-aligned by construction.  Falls back to
    * the producer-private staging array when the contiguous run to
    * the physical end of the ring is smaller than the request, which
    * happens at most once per ring lap (~1.3 s of audio). */
   span_bytes = retro_spsc_write_begin(&audio_spsc, &span);
   if (span_bytes >= need)
   {
      audio_reserve_in_ring = true;
      return (int16_t*)span;
   }
   audio_reserve_in_ring = false;
   return audio_staging;
}

void retro_audio_commit(int32_t samples)
{
   const size_t bytes = (size_t)samples * sizeof(int16_t);
   if (!samples)
   {
      if (audio_reserve_in_ring)
         retro_spsc_write_end(&audio_spsc, 0); /* abandon */
      return;
   }
   if (audio_reserve_in_ring)
   {
      /* Mix already wrote into the ring; publishing is one
       * release-store. */
      retro_spsc_write_end(&audio_spsc, bytes);
      return;
   }
   /* Wrap fallback: staging -> ring, retro_spsc_write handles the
    * split copy.  Total free space can still be short if the
    * frontend stalled past ~1.3 s of buffered audio; drop rather
    * than grow without bound. */
   if (retro_spsc_write_avail(&audio_spsc) < bytes)
   {
      audio_dropped_samples += (uint32_t)samples;
      if (!audio_drop_logged)
      {
         audio_drop_logged = true;
         log_cb(RETRO_LOG_WARN, "Audio SPSC queue full; dropping samples (frontend stalled?)\n");
      }
      return;
   }
   retro_spsc_write(&audio_spsc, audio_staging, bytes);
}

void retro_set_environment(retro_environment_t cb)
{
	bool no_game = true;
	struct retro_core_options_update_display_callback update_display_cb;

	environ_cb = cb;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	update_display_cb.callback = update_option_visibility;
	environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update_display_cb);

#ifndef STATIC_LINKING
	/*
	   Hybrid VFS replaces the wholesale v1 adoption: plain paths (ISOs,
	   BIOS, memcards, saves) serve through the local implementation
	   with no per-read frontend indirection, and the local VFS can map
	   disc images - which the CDVD readers turn into zero-copy reads
	   and, for flat ISOs, into dropping their worker thread outright.
	   The frontend covers URI paths and sandboxed-platform fallback
	   plus dirent/stat coverage the old wiring lacked.  Compiled out
	   for statically linked frontends, which share one libretro-common
	   with the core.
	*/
	vfs_hybrid_init(environ_cb, NULL);
#endif
}

#define MAX_DISKS 10
static std::vector<std::string> disk_images;
static int image_index = 0;
static bool disk_ejected = false;

static bool RETRO_CALLCONV get_eject_state(void) { return disk_ejected; }
static unsigned RETRO_CALLCONV get_image_index(void) { return image_index; }
static unsigned RETRO_CALLCONV get_num_images(void) { return disk_images.size(); }

static bool RETRO_CALLCONV set_eject_state(bool ejected)
{
	if (get_eject_state() == ejected)
		return false;

	/* Everything below runs on the frontend thread and reaches straight
	 * into a live VM: cdvdCtrlTrayOpen/Close raise an IOP interrupt, and
	 * ChangeDisc closes the CDVD reader and opens another one - while
	 * the EE thread is executing recompiled code that reads those very
	 * registers and streams from that very reader.
	 *
	 * ThreadSanitizer, on three load cycles with three disc swaps each:
	 *
	 *   Write of size 4 by main thread
	 *     iopIntcIrq -> cdvdCtrlTrayOpen -> set_eject_state
	 *   Previous read of size 4 by thread T5
	 *     iopEventTest -> _cpuEventTest_Shared -> recEventTest
	 *   Location is global 'iopHw'
	 *
	 * plus four more of the same shape across iopTestIntc,
	 * iopHwRead32_Page1 and cdvdWrite.  The interrupt controller is the
	 * visible half; swapping the reader underneath a thread that is
	 * reading through it is the half that does not need a sanitizer to
	 * be a problem.
	 *
	 * Park the EE for the swap, the way retro_serialize does for the
	 * same reason.  Guarded on there being a VM at all, because
	 * cpu_thread_pause waits for a thread that has to exist to answer -
	 * a frontend may eject before content is loaded. */
	const bool vm_live = VMManager::HasValidVM();
	if (vm_live)
		cpu_thread_pause();

	if (ejected || image_index < 0 || image_index >= (int)disk_images.size())
	{
		cdvdCtrlTrayOpen();
		VMManager::ChangeDisc(CDVD_SourceType::NoDisc, "");
	}
	else if (disk_images[image_index].empty())
	{
		/* add_image_index() appends an empty slot for the frontend to
		 * fill in later.  Closing the tray on one of those asked the VM
		 * to mount "", which is not a disc. */
		cdvdCtrlTrayOpen();
		VMManager::ChangeDisc(CDVD_SourceType::NoDisc, "");
	}
	else
	{
		VMManager::ChangeDisc(CDVD_SourceType::Iso, disk_images[image_index]);
		cdvdCtrlTrayClose();
	}

	if (vm_live)
		cpu_thread_resume();

	disk_ejected = ejected;
	return true;
}

static bool RETRO_CALLCONV set_image_index(unsigned index)
{
	if (index >= disk_images.size())
		return false;

	image_index = index;
	return true;
}

static bool RETRO_CALLCONV replace_image_index(unsigned index, const struct retro_game_info* info)
{
	if (index >= disk_images.size())
		return false;

	/* libretro.h: "Passing NULL to this function indicates that the
	 * frontend has removed this disk image from its internal list",
	 * and its own example of doing so is replace_image_index(1, NULL).
	 * info was dereferenced without being checked, so the documented
	 * way to remove a disc crashed the core. */
	if (!info || !info->path)
	{
		disk_images.erase(disk_images.begin() + index);
		if (disk_images.empty())
			image_index = -1;
		else if (image_index > (int)index)
			image_index--;
		/* Removing the entry the tray is pointing at shifts whatever
		 * followed it down into that slot; removing the last entry
		 * leaves the index past the end. */
		if (image_index >= (int)disk_images.size())
			image_index = (int)disk_images.size() - 1;
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

/* The frontend calls set_initial_image() immediately before
 * retro_load_game, which is before the playlist has been read - so
 * disk_images is still empty and nothing here can be validated yet.
 * Both arguments are kept for retro_load_game to check once it knows
 * what the playlist contains. */
static int initial_image_index = 0;
static std::string initial_image_path;

static bool RETRO_CALLCONV set_initial_image(unsigned index, const char* path)
{
	initial_image_index = (int)index;
	initial_image_path  = path ? path : "";
	return true;
}

/* Insert the disc the frontend asked to resume on, if the playlist
 * still agrees with what it was told.  libretro's contract is to fall
 * back to index 0 when the index is out of range or the path at that
 * index is not the one the frontend named, which is how a playlist
 * edited between sessions stops the wrong disc being inserted. */
static void apply_initial_image(void)
{
	if (     initial_image_index > 0
		 && initial_image_index < (int)disk_images.size()
		 && (initial_image_path.empty()
			 || disk_images[initial_image_index] == initial_image_path))
		set_image_index((unsigned)initial_image_index);
	else
		set_image_index(0);
}

static bool RETRO_CALLCONV get_image_path(unsigned index, char* path, size_t len)
{
	if (index >= disk_images.size())
		return false;

	if (disk_images[index].empty())
		return false;

	strlcpy(path, disk_images[index].c_str(), len);
	return true;
}

static bool RETRO_CALLCONV get_image_label(unsigned index, char* label, size_t len)
{
	if (index >= disk_images.size())
		return false;

	if (disk_images[index].empty())
		return false;

	strlcpy(label, disk_images[index].c_str(), len);
	return true;
}

extern "C" void pcsx2_jithash_dump(void);

void retro_deinit(void)
{
	pcsx2_jithash_dump();
	free_output_audio_buffer();
	// WIN32 doesn't allow canceling threads from global constructors/destructors in a shared library.
	vu1Thread.Close();
}

void retro_get_system_info(retro_system_info* info)
{
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	memset(info, 0, sizeof(*info));
	/* the version is checked by retroachievements, it should be semver */
	info->library_version  = "v2.0.0-" GIT_VERSION;
	info->library_name     = "LRPS2";
	info->valid_extensions = "elf|iso|ciso|cue|gz|chd|cso|zso|m3u";
	info->need_fullpath    = true;
	info->block_extract    = true;
}

void retro_set_region(unsigned val)
{
	internal_setting_region = val;
}
unsigned retro_get_region(void)
{
	return internal_setting_region;
}

void retro_get_system_av_info(retro_system_av_info* info)
{
	unsigned upscale_mul       = (setting_renderer == "paraLLEl-GS" && setting_pgs_high_res_scanout) ? 2 : setting_upscale_multiplier;

	switch (gsVideoMode)
	{
		case GS_VideoMode::PAL:
		case GS_VideoMode::DVD_PAL:
		case GS_VideoMode::SDTV_576P:
			retro_set_region(RETRO_REGION_PAL);
			break;
		default:
			retro_set_region(RETRO_REGION_NTSC);
			break;
	}

	info->geometry.base_width  = 640;
	info->geometry.base_height = (retro_get_region() == RETRO_REGION_NTSC) ? 448 : 512;

	if (               (  !is_software_setting(setting_renderer)
			   && setting_renderer != "paraLLEl-GS")
			|| (  setting_renderer == "paraLLEl-GS" 
			   && setting_pgs_high_res_scanout))
	{
		info->geometry.base_width  *= upscale_mul;
		info->geometry.base_height *= upscale_mul;
	}

	/* Max always at PAL height to prevent video reinits */
	info->geometry.max_width  = info->geometry.base_width;
	info->geometry.max_height = 512 * upscale_mul;

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

	info->timing.fps         = (retro_get_region() == RETRO_REGION_NTSC) ? (60.0f / 1.001f) : 50.0f;
	info->timing.sample_rate = 48000;
}

void retro_reset(void)
{
	cpu_thread_pause();
	VMManager::Reset();
	/* Discard any audio buffered before the reset; carrying pre-reset
	 * samples into the post-reset stream causes audible glitches and
	 * leaves the buffer in a non-deterministic starting state. */
	discard_buffered_audio();
	cpu_thread_resume();
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

	cpu_thread_resume();
}

static void libretro_context_destroy(void)
{
	cpu_thread_pause();

#ifdef ENABLE_VULKAN
	/* The frontend keeps replaying the last set_image (cached-frame
	 * replay: pause, menu background, its own context-teardown blit) and
	 * releases that view during the reinit that follows this callback.
	 * The registered image points at a pool texture that CloseGS() below
	 * destroys, so retract it and drain the frontend's GPU work first -
	 * otherwise the reinit samples/releases a dangling VkImageView
	 * (crashes observed when SET_SYSTEM_AV_INFO retriggers video init,
	 * e.g. the disabled on-the-fly upscale switch). */
	if (hw_render.context_type == RETRO_HW_CONTEXT_VULKAN && vulkan)
	{
		vulkan->set_image(vulkan->handle, nullptr, 0, nullptr, vulkan->queue_index);
		vulkan->wait_sync_index(vulkan->handle);
	}
#endif

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
	/* "Software (SW)" forces the all-CPU GSDeviceSW path even when a
	 * HW context is available. Tell the frontend NONE up front so we
	 * don't burn a Vulkan/D3D/GL context creation we won't use. */
	if (is_software_sw_setting(setting_renderer))
		return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);

	if (setting_renderer == "Auto" || is_software_setting(setting_renderer))
	{
		retro_hw_context_type context_type = RETRO_HW_CONTEXT_NONE;
		environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &context_type);
		if (context_type != RETRO_HW_CONTEXT_NONE && libretro_set_hw_render(context_type))
			return true;
#if defined(__APPLE__)
		if (libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN))
			return true;
#endif
		/* "Software (HW)" wanted a HW context but none was available
		 * (e.g. SDL2 frontend). Fall back gracefully to GSDeviceSW.
		 * "Auto" doesn't fall back here - it continues into the
		 * default fallback chain below, mirroring previous behavior. */
		if (is_software_setting(setting_renderer))
			return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);
	}
#ifdef _WIN32
	if (setting_renderer == "D3D11")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11);
	if (setting_renderer == "D3D12")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12);
#endif
#ifdef ENABLE_VULKAN
	if (               setting_renderer == "Vulkan" 
			|| setting_renderer == "paraLLEl-GS")
		return libretro_set_hw_render(RETRO_HW_CONTEXT_VULKAN);
#endif
	if (setting_renderer == "OpenGL")
	{
		if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL_CORE))
			return true;
		else if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL))
			return true;
		else if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGLES3))
			return true;
	}

#ifdef _WIN32
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_D3D12))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_D3D11))
		return true;
#endif
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL_CORE))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL))
		return true;
	if (libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGLES3))
		return true;
	return false;
}

/* RAII registration with the lock-free fault filter: this thread
 * executes fastmem-faulting EE JIT (and microVU1 when MTVU is off).
 * Unregistered threads' faults chain to the previously installed
 * handler.  Scope-based so every return path unregisters. */
struct FaultThreadScope
{
	FaultThreadScope() { HostSys::RegisterFaultHandlerThread(); }
	~FaultThreadScope() { HostSys::UnregisterFaultHandlerThread(); }
};

static void cpu_thread_entry(VMBootParameters boot_params)
{
	FaultThreadScope fault_scope_;
	if (!VMManager::Initialize(boot_params))
	{
		/* Initialize() flipped s_state to Shutdown on its way out.
		 * Do NOT stomp it back to Running: HasValidVM() would come
		 * true again and the loop below would reach
		 * VMManager::Execute() -> Cpu->Execute() with Cpu == NULL -
		 * an access violation reading offset 0x18 (offsetof(R5900cpu,
		 * Execute)) on this raw thread.  Publish the result so
		 * retro_load_game's boot handshake can fail the load. */
		retro_atomic_store_release_int(&cpu_thread_state, (int)VMState::Shutdown);
		retro_atomic_store_release_int(&cpu_thread_boot_result, -1);
		return;
	}
	retro_atomic_store_release_int(&cpu_thread_boot_result, 1);
	/* Initialize() left the VM in Paused.  Deliberately do NOT flip it
	 * to Running here: retro_run opens the GS (MTGS::TryOpenGS) before
	 * its Paused->resume check, so keeping the EE parked until then
	 * means GSopen/ResetPCRTC reads SMODE/PMODE with no concurrent
	 * gsWrite from this thread (TSan: GSState::GetVideoMode vs
	 * gsWrite64_page_00).  First retro_run resumes us. */

	while (VMManager::GetState() != VMState::Shutdown)
	{
		if (VMManager::HasValidVM())
		{
			for (;;)
			{
				VMState _st = VMManager::GetState();
				retro_atomic_store_release_int(&cpu_thread_state, (int)_st);
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
						return;

					case VMState::Shutdown:
						/* VMManager::Shutdown() flips s_state to Stopping
						 * and then to Shutdown internally before returning.
						 * The cv.notify_one in retro_unload_game fires
						 * AFTER VMManager::Shutdown() has returned, so by
						 * the time cpu_thread wakes from its cv wait, state
						 * is already Shutdown - never Stopping. Without an
						 * explicit case here, cpu_thread would fall to
						 * 'default: continue;' and spin forever, hanging
						 * cpu_thread.join(). That notify is issued under
						 * cpu_thread_mtx, so the 'case Paused' wait observes
						 * the Shutdown state and exits here immediately. */
						return;

					case VMState::Paused:
					{
						/* Sleep until the libretro thread transitions us out
						 * of Paused.  Every waker stores the new state first
						 * and then Posts; the counted post cannot be lost, so
						 * the re-check loop needs no lock.  A full sleep lets
						 * a paused core idle instead of waking to re-poll. */
						while (VMManager::GetState() == VMState::Paused)
							cpu_thread_resume_sema.Wait();
						continue;
					}
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
		char bios_dir[PCSX2_PATH_MAX];

		pcsx2_path_join(bios_dir, sizeof(bios_dir), system_base, "pcsx2/bios");
		if (FileSystem::FindFiles(bios_dir, "*", FILESYSTEM_FIND_FILES, &results))
		{
			u32 version, region;
			static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
			static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
			char description[BIOS_DESCRIPTION_MAX];
			char zone[BIOS_ZONE_MAX];
			for (const FILESYSTEM_FIND_DATA& fd : results)
			{
				if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
					continue;

				if (IsBIOS(fd.FileName.c_str(), version, description, sizeof(description), region, zone, sizeof(zone)))
					bios_info.push_back({ std::string(Path::GetFileName(fd.FileName)), std::string(description) });
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

	/* PAL: ~960 stereo samples/frame -> 1920 int16. NTSC: ~801 stereo
	 * samples/frame -> 1602 int16. 4096 int16 (2048 stereo) gives us
	 * headroom past the steady-state nominal so we don't realloc on
	 * the first frame. */
	init_output_audio_buffer(4096);
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

	/* Through the VFS, like every other file this core opens.  fopen()
	 * takes the path in the local 8-bit encoding, so on Windows a cue
	 * sitting under a path with any non-ASCII character in it simply
	 * did not open. */
	RFILE *cue_file = filestream_open(path.c_str(),
			RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!cue_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open cue file.\n");
		return;
	}

	while (filestream_gets(cue_file, buffer, sizeof(buffer)))
	{
		std::string line(buffer);
		size_t pos = line.find(line_start);
		if (pos != std::string::npos)
		{
			size_t start = pos + strlen(line_start);
			size_t end = line.find(line_end, start);
			if (end != std::string::npos)
			{
				const std::string track(line.substr(start, end - start));
				filestream_close(cue_file);
				/* An absolute FILE entry is already the path; joining it
				 * to the cue's own directory produced nonsense like
				 * /games//games/disc.bin and the track was not found. */
				if (path_is_absolute(track.c_str()))
					path = track;
				else
					path = basedir + track;
				return;
			}
		}
	}

	log_cb(RETRO_LOG_ERROR, "Failed to find a valid track from cue file.\n");
	filestream_close(cue_file);
}

/* What retro_load_game() has to undo when it fails.
 *
 * The frontend does not call retro_unload_game() after retro_load_game()
 * returns false - there is nothing loaded to unload - so every early
 * return past CPUThreadInitialize() used to leave the whole VM-thread
 * allocation behind: SysMainMemory's EE/IOP/VU RAM and vtlb
 * reservations, the recompiler caches from InitializeCPUProviders, plus
 * GSinit/SPU2::Initialize/USBinit.  The core stayed resident holding
 * all of it, which is what a failed load looked like from the outside -
 * RetroArch still running, memory never coming back.
 *
 * Flagged rather than merely ordered, so it is safe to call from a
 * failure path and again from retro_unload_game without either one
 * having to know whether the other ran. */
static bool s_cpu_thread_initialized = false;
static bool s_input_initialized      = false;
static bool s_vulkan_library_loaded  = false;

static void libretro_teardown_cpu_thread(void)
{
	if (s_input_initialized)
	{
		Input::Shutdown();
		s_input_initialized = false;
	}

#ifdef ENABLE_VULKAN
	if (s_vulkan_library_loaded)
	{
		Vulkan::UnloadVulkanLibrary();
		s_vulkan_library_loaded = false;
	}
#endif

	if (s_cpu_thread_initialized)
	{
		VMManager::Internal::CPUThreadShutdown();
		s_cpu_thread_initialized = false;
		((LayeredSettingsInterface*)Host::GetSettingsInterface())->SetLayer(
				LayeredSettingsInterface::LAYER_BASE, nullptr);
	}
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
	/* Settings is where upstream resolves relative data-file paths like
	 * DEV9's HddFile; a libretro core keeps those in its system subdir. */
	EmuFolders::Settings  = EmuFolders::AppRoot;

	Host::Internal::SetBaseSettingsLayer(&s_settings_interface);

	EmuFolders::SetDefaults(s_settings_interface);
	VMManager::SetDefaultSettings(s_settings_interface);

	SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
	EmuFolders::LoadConfig(*bsi);
	EmuFolders::EnsureFoldersExist();
	VMManager::Internal::CPUThreadInitialize();
	s_cpu_thread_initialized = true;
	VMManager::LoadSettings();

	check_variables(true);

	if (setting_bios.empty())
	{
		log_cb(RETRO_LOG_ERROR, "Could not find any valid PS2 BIOS File in %s\n", EmuFolders::Bios.c_str());
		libretro_teardown_cpu_thread();
		return false;
	}

	Input::Init();
	s_input_initialized = true;

	if (!libretro_select_hw_render())
	{
		libretro_teardown_cpu_thread();
		return false;
	}

	if (is_software_setting(setting_renderer))
		s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::SW);

	switch (hw_render.context_type)
	{
		case RETRO_HW_CONTEXT_D3D12:
			if (!is_software_setting(setting_renderer))
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::DX12);
			break;
		case RETRO_HW_CONTEXT_D3D11:
			if (!is_software_setting(setting_renderer))
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
					pgs_create_device,
					nullptr,
					pgs_create_instance,
					pgs_create_device2,
				};
				environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);
			}
			else
#endif
			{
				if (!is_software_setting(setting_renderer))
					s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::VK);
				{
					static const struct retro_hw_render_context_negotiation_interface_vulkan iface = {
						RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
						RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
						get_application_info_vulkan,
						create_device_vulkan,
						nullptr,
					};
					environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&iface);
				}
				Vulkan::LoadVulkanLibrary();
				s_vulkan_library_loaded = true;
				vk_libretro_init_wraps();
			}
			break;
#endif
		case RETRO_HW_CONTEXT_NONE:
			if (!is_software_setting(setting_renderer))
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::SW);
			break;
		default:
			if (!is_software_setting(setting_renderer))
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", (int)GSRendererType::OGL);
			break;
	}

	libretro_content[0] = '\0';

	if (!setting_shared_memory_cards && game && game->path)
	{
		const char* save_base = nullptr;
		char memcard_path[PCSX2_PATH_MAX];

		environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_base);

		s_settings_interface.SetStringValue("Folders", "MemoryCards", save_base);
		VMManager::Internal::UpdateEmuFolders();

		snprintf(memcard_path, sizeof(memcard_path), "%s", path_basename(game->path));
		path_remove_extension(memcard_path);
		libretro_content = memcard_path;
	}

	VMManager::ApplySettings();

	image_index = 0;
	disk_images.clear();

	if (game && game->path)
	{
		std::string game_path = game->path;

		if (!strcmp(path_get_extension(game->path), "cue"))
			get_first_track_from_cue(game_path);

		/* M3U file list support */
		if (!strcmp(path_get_extension(game->path), "m3u"))
		{
			RFILE *fd = filestream_open(game->path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);

			if (fd)
			{
				int len;
				char linebuf[PCSX2_PATH_MAX];
				char game_dir[PCSX2_PATH_MAX];
				char game_abs[PCSX2_PATH_MAX];

				game_dir[0] = '\0';
				game_abs[0] = '\0';

				snprintf(game_dir, sizeof(game_dir), "%s", game_path.c_str());
				path_basedir(game_dir);

				while ((filestream_gets(fd, linebuf, PCSX2_PATH_MAX) != NULL) && (disk_images.size() < MAX_DISKS))
				{
					if (linebuf[0] == '#' || linebuf[0] == ';')
						continue;

					len = strlen(linebuf);

					if (len == 0)
						continue;

					if (linebuf[len-1] == '\n')
					{
						linebuf[len-1] = 0;
						len--;
					}

					if (len && (linebuf[len-1] == '\r'))
					{
						linebuf[len-1] = 0;
						len--;
					}

					/* The emptiness check above runs before the line
					 * terminators come off, so a blank CRLF line is two
					 * bytes there and zero here.  Joined to the playlist
					 * directory it yields the directory itself, which
					 * path_is_valid() is happy with - a trailing blank
					 * line, which most editors leave, added the folder
					 * as a disc. */
					if (len == 0)
						continue;

					/* An absolute entry is already the path we want.
					 * Joining it to the playlist's directory produced
					 * things like '/tmp//tmp/game.iso', so every M3U
					 * written with absolute paths - which is legal, and
					 * what a generator that does not know where the
					 * playlist will live has to emit - silently listed
					 * no discs at all. */
					if (path_is_absolute(linebuf))
						strlcpy(game_abs, linebuf, sizeof(game_abs));
					else
						fill_pathname_join(game_abs, game_dir, linebuf, sizeof(game_abs));

					if (path_is_valid(game_abs))
					{
						game_path = game_abs;
						disk_images.push_back(game_path);

						if (log_cb)
							log_cb(RETRO_LOG_INFO, "Disk #%d added from M3U: \"%s\".\n", disk_images.size(), game_abs);
					}
				}

				apply_initial_image();
				get_image_path(get_image_index(), game_abs, sizeof(game_abs));
				boot_params.filename = game_abs;

				/* The playlist has been read; the handle was never
				 * released, so every multi-disc load leaked an RFILE and
				 * the 64 KiB buffer the VFS attaches to it. */
				filestream_close(fd);
			}
		}
		else
		{
			disk_images.push_back(game_path);
			boot_params.filename = game_path;
		}
	}

	/* Threading::Thread rather than std::thread for one substantive
	 * reason: an explicit stack size.  This thread runs microVU0 always
	 * and microVU1 whenever MTVU is off - the exact code
	 * EMU_THREAD_STACK_SIZE exists for ("uVU likes recursion") and that
	 * the MTVU worker already requests - while the Windows default for
	 * an unadorned thread is half that. */
	cpu_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	retro_atomic_store_release_int(&cpu_thread_boot_result, 0);
	cpu_thread.Start([boot_params]() { cpu_thread_entry(boot_params); });

	/* Wait for VMManager::Initialize() to succeed or fail so a bad
	 * BIOS path, unreadable disc image, or any other init failure is
	 * reported to the frontend as a failed load instead of a black
	 * screen (or, before the cpu_thread_entry guard above, a null
	 * R5900cpu dereference).  Initialize() is bounded: it performs no
	 * GS waits, so this terminates. */
	for (;;)
	{
		int r = retro_atomic_load_acquire_int(&cpu_thread_boot_result);
		if (r > 0)
			break;
		if (r < 0)
		{
			log_cb(RETRO_LOG_ERROR,
				"VM initialization failed (BIOS/disc/peripheral open); failing content load.\n");
			if (cpu_thread.Joinable())
				cpu_thread.Join();
			libretro_teardown_cpu_thread();
			return false;
		}
		Threading::Timeslice();
	}

	return true;
}


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
	/* Shutdown() flipped state to Stopping; if cpu_thread is sleeping
	 * in its 'case Paused' wait (post cpu_thread_pause above), it
	 * needs a notify to observe the new state and run through to its
	 * 'case Stopping: return;' branch so cpu_thread.join() can
	 * complete.
	 *
	 * Shutdown() already stored the new state; the Post after it is
	 * counted, so whether cpu_thread has not yet checked (it will see
	 * the new state and not sleep) or is already in Wait() (the banked
	 * post wakes it), the wakeup cannot be lost.  The old mutex+condvar
	 * version needed a lock across the notify for the same guarantee. */
	cpu_thread_resume_sema.Post();
	/* Input goes down before the join, as it always has: the ordering
	 * here is not this commit's to change.  The teardown helper is
	 * flagged, so it will not touch input a second time. */
	if (s_input_initialized)
	{
		Input::Shutdown();
		s_input_initialized = false;
	}
	cpu_thread.Join();
	libretro_teardown_cpu_thread();

	retro_set_region(RETRO_REGION_NTSC); /* set back to default */
}

static void update_av_info(void)
{
	retro_system_av_info av_info;
	retro_get_system_av_info(&av_info);
	environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
	pending_update_av_info = false;
}

void retro_run(void)
{
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		check_variables(false);

	if (pending_update_av_info)
		update_av_info();

	Input::Update();

	if (!MTGS::IsOpen())
		MTGS::TryOpenGS();

	if ((VMState)retro_atomic_load_acquire_int(&cpu_thread_state) == VMState::Paused)
		cpu_thread_resume();

	MTGS::MainLoop(false);
	upload_output_audio_buffer();


}

std::optional<WindowInfo> Host::AcquireRenderWindow(void)
{
	WindowInfo wi;
	retro_system_av_info av_info;

	retro_get_system_av_info(&av_info);

	wi.surface_width  = av_info.geometry.max_width;
	wi.surface_height = av_info.geometry.max_height;
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

	/* Quiesce the MTVU worker and GS thread before snapshotting, so the VU
	 * and GS state we capture is consistent and not mid-update by another
	 * thread (matches the load path and the normal save/load invariant).
	 * WaitVU() is a no-op when the multithreaded VU1 is not active, so it is
	 * safe to call unconditionally. */
	vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	/* retro_serialize_size() already computed the exact upper bound on
	 * what we need. Reserve once so SaveStateBase's incremental
	 * resize() calls inside FreezeMem/PrepBlock don't realloc and copy
	 * the partial buffer multiple times as different sections (BIOS,
	 * internals, EE/IOP/VU memory, SPU2/PAD/GS freeze blocks) accumulate. */
	buffer.reserve(size);

	SaveStateBase saveme(buffer, true);

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

	/* Bound the copy by the frontend-provided buffer size: if the
	 * actual saved size somehow exceeds what retro_serialize_size()
	 * predicted, refuse rather than overrun the caller's buffer. */
	if (buffer.size() > size)
	{
		log_cb(RETRO_LOG_ERROR, "retro_serialize: produced %zu bytes, "
			"frontend buffer is only %zu\n", buffer.size(), size);
		cpu_thread_resume();
		return false;
	}
	memcpy(data, buffer.data(), buffer.size());

	cpu_thread_resume();
	return true;
}

bool retro_unserialize(const void* data, size_t size)
{
	freezeData fP;
	std::vector<u8> buffer;

	cpu_thread_pause();

	/* cpu_thread_pause() only stops the EE/main thread. Before we overwrite
	 * VU and GS state below we must also quiesce the MTVU worker and the GS
	 * thread, exactly as the normal reset/load path does - otherwise, when
	 * loading a state while the VM is already running (e.g. RetroArch "Load
	 * State" mid-game) the still-live VU1 thread keeps operating on the VU
	 * memory / micro programs / JIT state we are in the middle of replacing,
	 * corrupting the VM so it can't continue and every later load fails until
	 * the content is closed and relaunched. A freshly booted VM has these
	 * threads idle, which is why a cold load worked while an in-session load
	 * did not. WaitVU() is a no-op when the multithreaded VU1 is not active,
	 * so it is safe to call unconditionally. */
	vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	/* resize() (not reserve()): m_memory.size() is what PrepBlock and
	 * SaveStateBase::FreezeMem uses for bounds-checking. With reserve()
	 * size() stays 0, the very first PrepBlock for SPU2/PAD/GS sets
	 * m_error=true, and every subsequent freeze block silently loads as
	 * zeros - SPU2/PAD/GS state was effectively never restored. */
	buffer.resize(size);
	memcpy(buffer.data(), data, size);
	SaveStateBase loadme(buffer, false);

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

	/* Discard buffered audio: any pre-load samples in the buffer no
	 * longer match the SPU2 state we just restored. */
	discard_buffered_audio();

	cpu_thread_resume();
	if (!loadme.IsOkay())
	{
		log_cb(RETRO_LOG_ERROR, "retro_unserialize: short or "
			"corrupt savestate (size=%zu)\n", size);
		return false;
	}

	/* If the state was loaded before the game booted far enough for the normal
	 * boot path to apply GameDB settings (e.g. RetroArch Auto Load State), the
	 * per-game GS hardware fixes were never applied and the game can render
	 * incorrectly (issue #127). Re-apply them once, and only when actually
	 * missing - ApplySettings() rebuilds the whole config, so we must not run it
	 * on every unserialize. */
	if (VMManager::GameFixesNeedApplying())
		VMManager::ApplySettings();

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
	char path[PCSX2_PATH_MAX];

	pcsx2_path_join(path, sizeof(path), EmuFolders::Resources.c_str(), filename);
	std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path));
	if (!ret.has_value())
		log_cb(RETRO_LOG_ERROR, "Failed to read resource file '%s', path '%s'\n", filename, path);
	return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
	// GameIndex.yaml (the game-compatibility database) is embedded in the core
	// so games work without an external resources file. Behaviour:
	//   "use external gameindex database" disabled (default): always built-in.
	//   enabled: try <systemdir>/resources/GameIndex.yaml, fall back to built-in
	//            if it is missing.
	// This is scoped to GameIndex.yaml; all other resources use the normal
	// external-only path below.
	if (!strcmp(filename, "GameIndex.yaml"))
	{
		if (setting_use_external_gameindex)
		{
			char path[PCSX2_PATH_MAX];

			pcsx2_path_join(path, sizeof(path), EmuFolders::Resources.c_str(), filename);
			std::optional<std::string> ext(FileSystem::ReadFileToString(path));
			if (ext.has_value())
				return ext;
			log_cb(RETRO_LOG_INFO,
				"External GameIndex.yaml not found at '%s', using built-in database.\n",
				path);
		}
		return std::string(reinterpret_cast<const char*>(g_gameDatabaseBuiltin),
			g_gameDatabaseBuiltinSize);
	}

	char path[PCSX2_PATH_MAX];

	pcsx2_path_join(path, sizeof(path), EmuFolders::Resources.c_str(), filename);
	std::optional<std::string> ret(FileSystem::ReadFileToString(path));
	if (!ret.has_value())
	{
		std::string str = std::string(filename) + " is missing, expect bugs.";
		unsigned msg_interface_version = 0;
		environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &msg_interface_version);

		if (msg_interface_version >= 1)
		{
			retro_message_ext msg = {
				str.c_str(),
				3000,
				3,
				RETRO_LOG_WARN,
				RETRO_MESSAGE_TARGET_OSD,
				RETRO_MESSAGE_TYPE_NOTIFICATION,
				-1
			};
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
		}
		else
		{
			retro_message msg = {
				str.c_str(),
				180
			};
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
		}

		// OSD messages should be kept pretty short, but let's give the user a bit more info in logs.
		log_cb(RETRO_LOG_ERROR, "Failed to read resource file to string '%s', path '%s'\n", filename, path);
		log_cb(RETRO_LOG_WARN, "Please check the docs for more informations: https://docs.libretro.com/library/lrps2/\n");
	}
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

	if (!serial[0])
		return;

	log_cb(RETRO_LOG_INFO, "[GameDB] Serial: %s\n", serial);

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
		pending_update_av_info = true;
}
