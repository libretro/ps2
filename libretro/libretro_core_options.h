#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifndef HAVE_NO_LANGEXTRA
#include "libretro_core_options_intl.h"
#endif

#define RUMBLE_OPTS \
   { "disabled", NULL },  { "5%", NULL }, { "10%", NULL }, { "15%", NULL }, { "20%", NULL }, { "25%", NULL },  { "30%", NULL }, \
        { "35%", NULL }, { "40%", NULL }, { "45%", NULL }, { "50%", NULL }, { "55%", NULL }, { "60%", NULL },  { "65%", NULL }, \
        { "70%", NULL }, { "75%", NULL }, { "80%", NULL }, { "85%", NULL }, { "90%", NULL }, { "95%", NULL }, { "100%", NULL }

#define PERCENT_0_50 \
    { "0%", NULL },  { "1%", NULL },  { "2%", NULL },  { "3%", NULL },  { "4%", NULL },  { "5%", NULL },  { "6%", NULL }, \
    { "7%", NULL },  { "8%", NULL },  { "9%", NULL }, { "10%", NULL }, { "11%", NULL }, { "12%", NULL }, { "13%", NULL }, \
   { "14%", NULL }, { "15%", NULL }, { "16%", NULL }, { "17%", NULL }, { "18%", NULL }, { "19%", NULL }, { "20%", NULL }, \
   { "21%", NULL }, { "22%", NULL }, { "23%", NULL }, { "24%", NULL }, { "25%", NULL }, { "26%", NULL }, { "27%", NULL }, \
   { "28%", NULL }, { "29%", NULL }, { "30%", NULL }, { "31%", NULL }, { "32%", NULL }, { "33%", NULL }, { "34%", NULL }, \
   { "35%", NULL }, { "36%", NULL }, { "37%", NULL }, { "38%", NULL }, { "39%", NULL }, { "40%", NULL }, { "41%", NULL }, \
   { "42%", NULL }, { "43%", NULL }, { "44%", NULL }, { "45%", NULL }, { "46%", NULL }, { "47%", NULL }, { "48%", NULL }, \
   { "49%", NULL }, { "50%", NULL }

#define PERCENT_80_200 \
    { "80%", NULL },  { "81%", NULL },  { "82%", NULL },  { "83%", NULL },  { "84%", NULL },  { "85%", NULL },  { "86%", NULL }, \
    { "87%", NULL },  { "88%", NULL },  { "89%", NULL },  { "90%", NULL },  { "91%", NULL },  { "92%", NULL },  { "93%", NULL }, \
    { "94%", NULL },  { "95%", NULL },  { "96%", NULL },  { "97%", NULL },  { "98%", NULL },  { "99%", NULL }, { "100%", NULL }, \
   { "101%", NULL }, { "102%", NULL }, { "103%", NULL }, { "104%", NULL }, { "105%", NULL }, { "106%", NULL }, { "107%", NULL }, \
   { "108%", NULL }, { "109%", NULL }, { "110%", NULL }, { "111%", NULL }, { "112%", NULL }, { "113%", NULL }, { "114%", NULL }, \
   { "115%", NULL }, { "116%", NULL }, { "117%", NULL }, { "118%", NULL }, { "119%", NULL }, { "120%", NULL }, { "121%", NULL }, \
   { "122%", NULL }, { "123%", NULL }, { "124%", NULL }, { "125%", NULL }, { "126%", NULL }, { "127%", NULL }, { "128%", NULL }, \
   { "129%", NULL }, { "130%", NULL }, { "131%", NULL }, { "132%", NULL }, { "133%", NULL }, { "134%", NULL }, { "135%", NULL }, \
   { "136%", NULL }, { "137%", NULL }, { "138%", NULL }, { "139%", NULL }, { "140%", NULL }, { "141%", NULL }, { "142%", NULL }, \
   { "143%", NULL }, { "144%", NULL }, { "145%", NULL }, { "146%", NULL }, { "147%", NULL }, { "148%", NULL }, { "149%", NULL }, \
   { "150%", NULL }, { "151%", NULL }, { "152%", NULL }, { "153%", NULL }, { "154%", NULL }, { "155%", NULL }, { "156%", NULL }, \
   { "157%", NULL }, { "158%", NULL }, { "159%", NULL }, { "160%", NULL }, { "161%", NULL }, { "162%", NULL }, { "163%", NULL }, \
   { "164%", NULL }, { "165%", NULL }, { "166%", NULL }, { "167%", NULL }, { "168%", NULL }, { "169%", NULL }, { "170%", NULL }, \
   { "171%", NULL }, { "172%", NULL }, { "173%", NULL }, { "174%", NULL }, { "175%", NULL }, { "176%", NULL }, { "177%", NULL }, \
   { "178%", NULL }, { "179%", NULL }, { "180%", NULL }, { "181%", NULL }, { "182%", NULL }, { "183%", NULL }, { "184%", NULL }, \
   { "185%", NULL }, { "186%", NULL }, { "187%", NULL }, { "188%", NULL }, { "189%", NULL }, { "190%", NULL }, { "191%", NULL }, \
   { "192%", NULL }, { "193%", NULL }, { "194%", NULL }, { "195%", NULL }, { "196%", NULL }, { "197%", NULL }, { "198%", NULL }, \
   { "199%", NULL }, { "200%", NULL }

/*
 ********************************
 * VERSION: 2.0
 ********************************
 *
 * - 2.0: Add support for core options v2 interface
 * - 1.3: Move translations to libretro_core_options_intl.h
 *        - libretro_core_options_intl.h includes BOM and utf-8
 *          fix for MSVC 2010-2013
 *        - Added HAVE_NO_LANGEXTRA flag to disable translations
 *          on platforms/compilers without BOM support
 * - 1.2: Use core options v1 interface when
 *        RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION is >= 1
 *        (previously required RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION == 1)
 * - 1.1: Support generation of core options v0 retro_core_option_value
 *        arrays containing options with a single value
 * - 1.0: First commit
*/

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/

/* RETRO_LANGUAGE_ENGLISH */

/* Default language:
 * - All other languages must include the same keys and values
 * - Will be used as a fallback in the event that frontend language
 *   is not available
 * - Will be used as a fallback for any missing entries in
 *   frontend language definition */

struct retro_core_option_v2_category option_cats_us[] = {
   {
      "system",
      "System",
      "Show system options."
   },
   {
      "video",
      "Video",
      "Show video options."
   },
   {
      "emulation",
      "Emulation",
      "Show emulation options."
   },
   {
      "hw_hacks",
      "Manual Hardware Renderer Fixes",
      "Show manual hardware renderer fixes."
   },
   {
      "input",
      "Input",
      "Show input options."
   },
   { NULL, NULL, NULL },
};

struct retro_core_option_v2_definition option_defs_us[] = {
   {
      "pcsx2_bios",
      "System > BIOS",
      "BIOS",
      NULL,
      NULL,
      "system",
      {
         // Filled in retro_init()
      },
      NULL
   },
   {
      "pcsx2_fastboot",
      "System > Fast Boot (Restart)",
      "Fast Boot (Restart)",
      "Skips BIOS startup screen and boots straight to the game. Disable this if you want to access the PS2 system settings or access the Memory Card manager.",
      NULL,
      "system",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "pcsx2_fastcdvd",
      "System > Fast CD/DVD Access (Restart)",
      "Fast CD/DVD Access (Restart)",
      "Fast CD/DVD access/seek times. A small handful of games will have compatibility problems with this enabled.",
      NULL,
      "system",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_enable_cheats",
      "System > Enable Cheats",
      "Enable Cheats",
      "Enable cheat files to be read from the 'cheats' directory in the system folder.",
      NULL,
      "system",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_hint_language_unlock",
      "System > Language Unlock",
      "Language Unlock (Restart)",
      "If enabled, will look inside the internal database for language unlock options. Examples include: forcing an international Japanese version to English, unlocking more European languages, etc",
      NULL,
      "system",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_renderer",
      "Video > Renderer",
      "Renderer",
      NULL,
      NULL,
      "video",
      {
         { "Auto", NULL },
         { "OpenGL", NULL },
#ifdef _WIN32
         { "D3D11", NULL },
         { "D3D12", NULL },
#endif
#ifdef ENABLE_VULKAN
         { "Vulkan", NULL },
#endif
#ifdef HAVE_PARALLEL_GS
         { "paraLLEl-GS", NULL },
#endif
         { "Software", NULL },
         { NULL, NULL },
      },
      "Auto"
   },
   {
      "pcsx2_upscale_multiplier",
      "Video > Internal Resolution (Restart)",
      "Internal Resolution (Restart)",
      NULL,
      NULL,
      "video",
      {
         { "1x Native (PS2)", NULL },
         { "2x Native (~720p)", NULL },
         { "3x Native (~1080p)", NULL },
         { "4x Native (~1440p/2K)", NULL },
         { "5x Native (~1800p/3K)", NULL },
         { "6x Native (~2160p/4K)", NULL },
         { "7x Native (~2520p)", NULL },
         { "8x Native (~2880p/5K)", NULL },
         { "9x Native (~3240p)", NULL },
         { "10x Native (~3600p/6K)", NULL },
         { "11x Native (~3960p)", NULL },
         { "12x Native (~4320p/8K)", NULL },
         { "13x Native (~5824p)", NULL },
         { "14x Native (~6272p)", NULL },
         { "15x Native (~6720p)", NULL },
         { "16x Native (~7168p)", NULL },
         { NULL, NULL },
      },
      "1x Native (PS2)"
   },
   {
      "pcsx2_pgs_ssaa",
      "Video > paraLLEl super sampling",
      "paraLLEl super sampling",
      "Apply supersampled anti-aliasing (SSAA). Unlike straight upscaling, supersampling retains a coherent visual look where 3D elements have similar resolution as UI elements. For high-res scanout upscaling to work, you need at least '4x SSAA ordered' (or higher). Setting this to 'Native' disables super sampling.",
      NULL,
      "video",
      {
         { "Native", NULL },
         { "2x SSAA", NULL },
         { "4x SSAA (sparse grid)", NULL },
         { "4x SSAA (ordered, can high-res)", NULL },
         { "8x SSAA (can high-res)", NULL },
         { "16x SSAA (can high-res)", NULL },
         { NULL, NULL },
      },
      "Native"
   },
   {
      "pcsx2_pgs_high_res_scanout",
      "Video > paraLLEl experimental High-res scanout",
      "paraLLEl experimental High-res scanout",
      "Allows upscaling with paraLLEl. Doesn't work with every game, some might require patches on top. Requires Supersampling to be set to at least 4x SSAA ordered or higher for it to work.",
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_pgs_ss_tex",
      "Video > paraLLEl experimental SSAA texture",
      "paraLLEl experimental SSAA texture",
      "Feedback higher resolution textures. May help high-res scanout image quality. Highly experimental and may cause rendering glitches.",
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_pgs_deblur",
      "Video > paraLLEl experimental sharp backbuffer",
      "paraLLEl experimental sharp backbuffer",
      "Attempts to workaround games that add extra blit passes before scanning out. May lead to better image quality in certain games which do this.",
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_pgs_disable_mipmaps",
      "Video > Force Texture LOD0",
      "Force Texture LOD0",
      "Disable this for traditional hardware mipmapping. Enabling this will bypass mipmapping and always use texture LOD0 instead. The result is better image quality. Only a small handful of games have graphics rendering issues with this enabled.",
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_deinterlace_mode",
      "Video > Deinterlacing",
      "Deinterlacing",
      "Select a deinterlacing method. Use 'Automatic' if unsure what to pick.",
      NULL,
      "video",
      {
         { "Automatic", NULL },
         { "Off", NULL },
         { "Weave TFF", NULL },
         { "Weave BFF", NULL },
         { "Bob TFF", NULL },
         { "Bob BFF", NULL },
         { "Blend TFF", NULL },
         { "Blend BFF", NULL },
         { "Adaptive TFF", NULL },
         { "Adaptive BFF", NULL },
         { NULL, NULL },
      },
      "Automatic"
   },
   {
      "pcsx2_nointerlacing_hint",
      "Video > No interlacing hint (Restart)",
      "No interlacing hint (Restart)",
      "If enabled, will look in the internal database if a patch is available for the game to turn the image into either a non-interlaced, full frame, or progressive scan video signal, and/or a combination of all the above. Image will be more stable and Deinterlacing can be turned off.",
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "pcsx2_texture_filtering",
      "Video > Texture Filtering",
      "Texture Filtering",
      NULL,
      NULL,
      "video",
      {
         { "Nearest", NULL },
         { "Bilinear (Forced)", NULL },
         { "Bilinear (PS2)", NULL },
         { "Bilinear (Forced excluding sprite)", NULL },
         { NULL, NULL },
      },
      "Bilinear (PS2)"
   },
   {
      "pcsx2_trilinear_filtering",
      "Video > Trilinear Filtering",
      "Trilinear Filtering",
      NULL,
      NULL,
      "video",
      {
         { "Automatic", NULL },
         { "disabled", NULL },
         { "Trilinear (PS2)", NULL },
         { "Trilinear (Forced)", NULL },
         { NULL, NULL },
      },
      "Automatic"
   },
   {
      "pcsx2_anisotropic_filtering",
      "Video > Anisotropic Filtering",
      "Anisotropic Filtering",
      NULL,
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "2x", NULL },
         { "4x", NULL },
         { "8x", NULL },
         { "16x", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_dithering",
      "Video > Dithering",
      "Dithering",
      NULL,
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "Scaled", NULL },
         { "Unscaled", NULL },
         { "Force 32bit", NULL },
         { NULL, NULL },
      },
      "Unscaled"
   },
   {
      "pcsx2_blending_accuracy",
      "Video > Blending Accuracy",
      "Blending Accuracy",
      NULL,
      NULL,
      "video",
      {
         { "Minimum", NULL },
         { "Basic", NULL },
         { "Medium", NULL },
         { "High", NULL },
         { "Full", NULL },
         { "Maximum", NULL },
         { NULL, NULL },
      },
      "Basic"
   },
   {
      "pcsx2_widescreen_hint",
      "Video > Widescreen hint (Restart)",
      "Widescreen hint (Restart)",
      "Applies a widescreen patch if a patch for the game can be found inside the internal database.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled (16:9)", NULL },
         { "enabled (16:10)", NULL },
         { "enabled (21:9)", NULL },
         { "enabled (32:9)", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_pcrtc_antiblur",
      "Video > PCRTC Anti-Blur",
      "PCRTC Anti-Blur",
      "Disable this for the most accurate output image. Enabling this will attempt to deblur the image. Most noticeable on software renderer and paraLLEl.",
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "pcsx2_pcrtc_screen_offsets",
      "Video > PCRTC Screen Offsets",
      "PCRTC Screen Offsets",
      NULL,
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_disable_interlace_offset",
      "Video > Disable Interlace Offset",
      "Disable Interlace Offset",
      NULL,
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_auto_flush_software",
      "Video > Auto Flush (Software)",
      "Auto Flush (Software)",
      NULL,
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
#if 0
   {
      "pcsx2_sw_renderer_threads",
      "Video > Software Renderer Threads",
      "Software Renderer Threads",
      NULL,
      NULL,
      "video",
      {
         { "2", NULL },
         { "3", NULL },
         { "4", NULL },
         { "5", NULL },
         { "6", NULL },
         { "7", NULL },
         { "8", NULL },
         { "9", NULL },
         { "10", NULL },
         { "11", NULL },
         { NULL, NULL },
      },
      "2"
   },
#endif
   {
      "pcsx2_ee_cycle_rate",
      "Emulation > EE Cycle Rate",
      "EE Cycle Rate",
      NULL,
      NULL,
      "emulation",
      {
         { "50% (Underclock)", NULL },
         { "60% (Underclock)", NULL },
         { "75% (Underclock)", NULL },
         { "100% (Normal Speed)", NULL },
         { "130% (Overclock)", NULL },
         { "180% (Overclock)", NULL },
         { "300% (Overclock)", NULL },
         { NULL, NULL },
      },
      "100% (Normal Speed)"
   },
   {
      "pcsx2_ee_cycle_skip",
      "Emulation > EE Cycle Skipping",
      "EE Cycle Skipping",
      NULL,
      NULL,
      "emulation",
      {
         { "disabled", NULL },
         { "Mild Underclock", NULL },
         { "Moderate Underclock", NULL },
         { "Maximum Underclock", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_game_enhancements_hint",
      "Emulation > Game Enhancements hint (Restart)",
      "Game Enhancements hint (Restart)",
      "Applies game-specific enhancements if patches for the game can be found inside the internal database. Examples of enhancements: LOD [Level of Detail] enhancements, better draw distance, ability to skip FMVs, etc.",
      NULL,
      "emulation",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_uncapped_framerate_hint",
      "Emulation > Uncapped Framerate hint (Restart)",
      "Uncapped Framerate hint (Restart)",
      "Uncaps the framerate if a patch for the game can be found inside the internal database. This can turn a 30fps game into 60fps locked, or stabilize the framerate. You might have to increase EE Cycle Rate in combination with this for a stable locked framerate. 60fps PAL-to-NTSC does what it says if it can find a patch, otherwise it will just try to use the default uncapped framerate patch (50fps for PAL, 60fps for NTSC)",
      NULL,
      "emulation",
      {
         { "disabled", NULL },
         { "enabled", NULL },
	 { "60fps PAL-to-NTSC", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_enable_hw_hacks",
      "HW Hacks > Enable Manual Hardware Renderer Fixes (Not Recommended)",
      "Enable Manual Hardware Renderer Fixes (Not Recommended)",
      "This will disable automatic settings from the database. Unless you know what you are doing it is NOT RECOMMENDED to turn this ON.",
      NULL,
      "hw_hacks",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_cpu_sprite_size",
      "HW Hacks > CPU Sprite Render Size",
      "CPU Sprite Render Size",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "0", "disabled" },
         { "1", "1 (64 Max Width)" },
         { "2", "2 (128 Max Width)" },
         { "3", "3 (192 Max Width)" },
         { "4", "4 (256 Max Width)" },
         { "5", "5 (320 Max Width)" },
         { "6", "6 (384 Max Width)" },
         { "7", "7 (448 Max Width)" },
         { "8", "8 (512 Max Width)" },
         { "9", "9 (576 Max Width)" },
         { "10", "10 (640 Max Width)" },
         { NULL, NULL },
      },
      "0"
   },
   {
      "pcsx2_cpu_sprite_level",
      "HW Hacks > CPU Sprite Render Level",
      "CPU Sprite Render Level",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "Sprites Only", NULL },
         { "Sprites/Triangles", NULL },
         { "Blended Sprites/Triangles", NULL },
         { NULL, NULL },
      },
      "Sprites Only"
   },
   {
      "pcsx2_software_clut_render",
      "HW Hacks > Software CLUT Render",
      "Software CLUT Render",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Normal", NULL },
         { "Aggressive", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_gpu_target_clut",
      "HW Hacks > GPU Target CLUT",
      "GPU Target CLUT",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Exact Match", NULL },
         { "Check Inside Target", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_auto_flush",
      "HW Hacks > Auto Flush",
      "Auto Flush",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Sprites Only", NULL },
         { "All Primitives", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_texture_inside_rt",
      "HW Hacks > Texture Inside RT",
      "Texture Inside RT",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Inside Target", NULL },
         { "Merge Targets", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_disable_depth_conversion",
      "HW Hacks > Disabled Depth Conversion",
      "Disabled Depth Conversion",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_framebuffer_conversion",
      "HW Hacks > Framebuffer Conversion",
      "Framebuffer Conversion",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_disable_partial_invalidation",
      "HW Hacks > Disable Partial Source Invalidation",
      "Disable Partial Source Invalidation",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_gpu_palette_conversion",
      "HW Hacks > GPU Palette Conversion",
      "GPU Palette Conversion",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_preload_frame_data",
      "HW Hacks > Preload Frame Data",
      "Preload Frame Data",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_half_pixel_offset",
      "HW Hacks > Half Pixel Offset",
      "Half Pixel Offset",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Normal (Vertex)", NULL },
         { "Special (Texture)", NULL },
         { "Special (Texture - Aggressive)", NULL },
         { "Align to Native", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_native_scaling",
      "HW Hacks > Native Scaling",
      "Native Scaling",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Normal", NULL },
         { "Aggressive", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_round_sprite",
      "HW Hacks > Round Sprite",
      "Round Sprite",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "Half", NULL },
         { "Full", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_align_sprite",
      "HW Hacks > Align Sprite",
      "Align Sprite",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_merge_sprite",
      "HW Hacks > Merge Sprite",
      "Merge Sprite",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_unscaled_palette_draw",
      "HW Hacks > Unscaled Palette Texture Draws",
      "Unscaled Palette Texture Draws",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_force_sprite_position",
      "HW Hacks > Force Even Sprite Position",
      "Force Even Sprite Position",
      NULL,
      NULL,
      "hw_hacks",
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_axis_deadzone1",
      "Input > Port 1 > Analog Deadzone",
      "Port 1 > Analog Deadzone",
      NULL,
      NULL,
      "input",
      {
         PERCENT_0_50,
         { NULL, NULL },
      },
      "15%"
   },
   {
      "pcsx2_button_deadzone1",
      "Input > Port 1 > Trigger Deadzone",
      "Port 1 > Trigger Deadzone",
      NULL,
      NULL,
      "input",
      {
         PERCENT_0_50,
         { NULL, NULL },
      },
      "0%"
   },
   {
      "pcsx2_axis_scale1",
      "Input > Port 1 > Analog Sensitivity",
      "Port 1 > Analog Sensitivity",
      NULL,
      NULL,
      "input",
      {
         PERCENT_80_200,
         { NULL, NULL },
      },
      "133%"
   },
   {
      "pcsx2_invert_left_stick1",
      "Input > Port 1 > Invert Left Analog Axis",
      "Port 1 > Invert Left Analog Axis",
      NULL,
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "x_axis", "Left/Right" },
         { "y_axis", "Up/Down" },
         { "all", "Up/Down and Left/Right" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_invert_right_stick1",
      "Input > Port 1 > Invert Right Analog Axis",
      "Port 1 > Invert Right Analog Axis",
      NULL,
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "x_axis", "Left/Right" },
         { "y_axis", "Up/Down" },
         { "all", "Up/Down and Left/Right" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_enable_rumble1",
      "Input > Port 1 > Rumble",
      "Port 1 > Rumble",
      NULL,
      NULL,
      "input",
      {
         RUMBLE_OPTS,
         { NULL, NULL },
      },
      "100%"
   },
   {
      "pcsx2_axis_deadzone2",
      "Input > Port 2 > Analog Deadzone",
      "Port 2 > Analog Deadzone",
      NULL,
      NULL,
      "input",
      {
         PERCENT_0_50,
         { NULL, NULL },
      },
      "15%"
   },
   {
      "pcsx2_button_deadzone2",
      "Input > Port 2 > Trigger Deadzone",
      "Port 2 > Trigger Deadzone",
      NULL,
      NULL,
      "input",
      {
         PERCENT_0_50,
         { NULL, NULL },
      },
      "0%"
   },
   {
      "pcsx2_axis_scale2",
      "Input > Port 2 > Analog Sensitivity",
      "Port 2 > Analog Sensitivity",
      NULL,
      NULL,
      "input",
      {
         PERCENT_80_200,
         { NULL, NULL },
      },
      "133%"
   },
   {
      "pcsx2_invert_left_stick2",
      "Input > Port 2 > Invert Left Analog Axis",
      "Port 2 > Invert Left Analog Axis",
      NULL,
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "x_axis", "Left/Right" },
         { "y_axis", "Up/Down" },
         { "all", "Up/Down and Left/Right" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_invert_right_stick2",
      "Input > Port 2 > Invert Right Analog Axis",
      "Port 2 > Invert Right Analog Axis",
      NULL,
      NULL,
      "input",
      {
         { "disabled", NULL },
         { "x_axis", "Left/Right" },
         { "y_axis", "Up/Down" },
         { "all", "Up/Down and Left/Right" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "pcsx2_enable_rumble2",
      "Input > Port 2 > Rumble",
      "Port 2 > Rumble",
      NULL,
      NULL,
      "input",
      {
         RUMBLE_OPTS,
         { NULL, NULL },
      },
      "100%"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

struct retro_core_options_v2 options_us = {
   option_cats_us,
   option_defs_us
};

/*
 ********************************
 * Language Mapping
 ********************************
*/

#ifndef HAVE_NO_LANGEXTRA
struct retro_core_options_v2 *options_intl[RETRO_LANGUAGE_LAST] = {
   &options_us, /* RETRO_LANGUAGE_ENGLISH */
   NULL,        /* RETRO_LANGUAGE_JAPANESE */
   NULL,        /* RETRO_LANGUAGE_FRENCH */
   NULL,        /* RETRO_LANGUAGE_SPANISH */
   NULL,        /* RETRO_LANGUAGE_GERMAN */
   NULL,        /* RETRO_LANGUAGE_ITALIAN */
   NULL,        /* RETRO_LANGUAGE_DUTCH */
   NULL,        /* RETRO_LANGUAGE_PORTUGUESE_BRAZIL */
   NULL,        /* RETRO_LANGUAGE_PORTUGUESE_PORTUGAL */
   NULL,        /* RETRO_LANGUAGE_RUSSIAN */
   NULL,        /* RETRO_LANGUAGE_KOREAN */
   NULL,        /* RETRO_LANGUAGE_CHINESE_TRADITIONAL */
   NULL,        /* RETRO_LANGUAGE_CHINESE_SIMPLIFIED */
   NULL,        /* RETRO_LANGUAGE_ESPERANTO */
   NULL,        /* RETRO_LANGUAGE_POLISH */
   NULL,        /* RETRO_LANGUAGE_VIETNAMESE */
   NULL,        /* RETRO_LANGUAGE_ARABIC */
   NULL,        /* RETRO_LANGUAGE_GREEK */
   NULL,        /* RETRO_LANGUAGE_TURKISH */
   NULL,        /* RETRO_LANGUAGE_SLOVAK */
   NULL,        /* RETRO_LANGUAGE_PERSIAN */
   NULL,        /* RETRO_LANGUAGE_HEBREW */
   NULL,        /* RETRO_LANGUAGE_ASTURIAN */
   NULL,        /* RETRO_LANGUAGE_FINNISH */
   NULL,        /* RETRO_LANGUAGE_INDONESIAN */
   NULL,        /* RETRO_LANGUAGE_SWEDISH */
   NULL,        /* RETRO_LANGUAGE_UKRAINIAN */
   NULL,        /* RETRO_LANGUAGE_CZECH */
   NULL,        /* RETRO_LANGUAGE_CATALAN_VALENCIA */
   NULL,        /* RETRO_LANGUAGE_CATALAN */
   NULL,        /* RETRO_LANGUAGE_BRITISH_ENGLISH */
   NULL,        /* RETRO_LANGUAGE_HUNGARIAN */
   NULL,        /* RETRO_LANGUAGE_BELARUSIAN */
   NULL,        /* RETRO_LANGUAGE_GALICIAN */
   NULL,        /* RETRO_LANGUAGE_NORWEGIAN */
};
#endif

/*
 ********************************
 * Functions
 ********************************
*/

/* Handles configuration/setting of core options.
 * Should be called as early as possible - ideally inside
 * retro_set_environment(), and no later than retro_load_game()
 * > We place the function body in the header to avoid the
 *   necessity of adding more .c files (i.e. want this to
 *   be as painless as possible for core devs)
 */

static INLINE void libretro_set_core_options(retro_environment_t environ_cb,
      bool *categories_supported)
{
   unsigned version  = 0;
#ifndef HAVE_NO_LANGEXTRA
   unsigned language = 0;
#endif

   if (!environ_cb || !categories_supported)
      return;

   *categories_supported = false;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
#ifndef HAVE_NO_LANGEXTRA
      struct retro_core_options_v2_intl core_options_intl;

      core_options_intl.us    = &options_us;
      core_options_intl.local = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
          (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH))
         core_options_intl.local = options_intl[language];

      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL,
            &core_options_intl);
#else
      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
            &options_us);
#endif
   }
   else
   {
      size_t i, j;
      size_t option_index              = 0;
      size_t num_options               = 0;
      struct retro_core_option_definition
            *option_v1_defs_us         = NULL;
#ifndef HAVE_NO_LANGEXTRA
      size_t num_options_intl          = 0;
      struct retro_core_option_v2_definition
            *option_defs_intl          = NULL;
      struct retro_core_option_definition
            *option_v1_defs_intl       = NULL;
      struct retro_core_options_intl
            core_options_v1_intl;
#endif
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine total number of options */
      while (true)
      {
         if (option_defs_us[num_options].key)
            num_options++;
         else
            break;
      }

      if (version >= 1)
      {
         /* Allocate US array */
         option_v1_defs_us = (struct retro_core_option_definition *)
               calloc(num_options + 1, sizeof(struct retro_core_option_definition));

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            struct retro_core_option_v2_definition *option_def_us = &option_defs_us[i];
            struct retro_core_option_value *option_values         = option_def_us->values;
            struct retro_core_option_definition *option_v1_def_us = &option_v1_defs_us[i];
            struct retro_core_option_value *option_v1_values      = option_v1_def_us->values;

            option_v1_def_us->key           = option_def_us->key;
            option_v1_def_us->desc          = option_def_us->desc;
            option_v1_def_us->info          = option_def_us->info;
            option_v1_def_us->default_value = option_def_us->default_value;

            /* Values must be copied individually... */
            while (option_values->value)
            {
               option_v1_values->value = option_values->value;
               option_v1_values->label = option_values->label;

               option_values++;
               option_v1_values++;
            }
         }

#ifndef HAVE_NO_LANGEXTRA
         if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
             (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH) &&
             options_intl[language])
            option_defs_intl = options_intl[language]->definitions;

         if (option_defs_intl)
         {
            /* Determine number of intl options */
            while (true)
            {
               if (option_defs_intl[num_options_intl].key)
                  num_options_intl++;
               else
                  break;
            }

            /* Allocate intl array */
            option_v1_defs_intl = (struct retro_core_option_definition *)
                  calloc(num_options_intl + 1, sizeof(struct retro_core_option_definition));

            /* Copy parameters from option_defs_intl array */
            for (i = 0; i < num_options_intl; i++)
            {
               struct retro_core_option_v2_definition *option_def_intl = &option_defs_intl[i];
               struct retro_core_option_value *option_values           = option_def_intl->values;
               struct retro_core_option_definition *option_v1_def_intl = &option_v1_defs_intl[i];
               struct retro_core_option_value *option_v1_values        = option_v1_def_intl->values;

               option_v1_def_intl->key           = option_def_intl->key;
               option_v1_def_intl->desc          = option_def_intl->desc;
               option_v1_def_intl->info          = option_def_intl->info;
               option_v1_def_intl->default_value = option_def_intl->default_value;

               /* Values must be copied individually... */
               while (option_values->value)
               {
                  option_v1_values->value = option_values->value;
                  option_v1_values->label = option_values->label;

                  option_values++;
                  option_v1_values++;
               }
            }
         }

         core_options_v1_intl.us    = option_v1_defs_us;
         core_options_v1_intl.local = option_v1_defs_intl;

         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &core_options_v1_intl);
#else
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_v1_defs_us);
#endif
      }
      else
      {
         /* Allocate arrays */
         variables  = (struct retro_variable *)calloc(num_options + 1,
               sizeof(struct retro_variable));
         values_buf = (char **)calloc(num_options, sizeof(char *));

         if (!variables || !values_buf)
            goto error;

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            const char *key                        = option_defs_us[i].key;
            const char *desc                       = option_defs_us[i].desc;
            const char *default_value              = option_defs_us[i].default_value;
            struct retro_core_option_value *values = option_defs_us[i].values;
            size_t buf_len                         = 3;
            size_t default_index                   = 0;

            values_buf[i] = NULL;

            if (desc)
            {
               size_t num_values = 0;

               /* Determine number of values */
               while (true)
               {
                  if (values[num_values].value)
                  {
                     /* Check if this is the default value */
                     if (default_value)
                        if (strcmp(values[num_values].value, default_value) == 0)
                           default_index = num_values;

                     buf_len += strlen(values[num_values].value);
                     num_values++;
                  }
                  else
                     break;
               }

               /* Build values string */
               if (num_values > 0)
               {
                  buf_len += num_values - 1;
                  buf_len += strlen(desc);

                  values_buf[i] = (char *)calloc(buf_len, sizeof(char));
                  if (!values_buf[i])
                     goto error;

                  strcpy(values_buf[i], desc);
                  strcat(values_buf[i], "; ");

                  /* Default value goes first */
                  strcat(values_buf[i], values[default_index].value);

                  /* Add remaining values */
                  for (j = 0; j < num_values; j++)
                  {
                     if (j != default_index)
                     {
                        strcat(values_buf[i], "|");
                        strcat(values_buf[i], values[j].value);
                     }
                  }
               }
            }

            variables[option_index].key   = key;
            variables[option_index].value = values_buf[i];
            option_index++;
         }

         /* Set variables */
         environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
      }

error:
      /* Clean up */

      if (option_v1_defs_us)
      {
         free(option_v1_defs_us);
         option_v1_defs_us = NULL;
      }

#ifndef HAVE_NO_LANGEXTRA
      if (option_v1_defs_intl)
      {
         free(option_v1_defs_intl);
         option_v1_defs_intl = NULL;
      }
#endif

      if (values_buf)
      {
         for (i = 0; i < num_options; i++)
         {
            if (values_buf[i])
            {
               free(values_buf[i]);
               values_buf[i] = NULL;
            }
         }

         free(values_buf);
         values_buf = NULL;
      }

      if (variables)
      {
         free(variables);
         variables = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif
