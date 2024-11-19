#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifndef HAVE_NO_LANGEXTRA
#include "libretro_core_options_intl.h"
#endif

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
      "Show emulation options"
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
      "BIOS",
      NULL,
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
      "Fast Boot",
      NULL,
      NULL,
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
      "Fast CD/DVD Access",
      NULL,
      NULL,
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
      "Enable Cheats",
      NULL,
      NULL,
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
      "Renderer",
      NULL,
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
         { "Null", NULL },
         { NULL, NULL },
      },
      "Auto"
   },
   {
      "pcsx2_pgs_ssaa",
      "paraLLEl super sampling",
      NULL,
      NULL,
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
      "paraLLEl experimental High-res scanout (Restart)",
      NULL,
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
      "pcsx2_pgs_disable_mipmaps",
      "Force Texture LOD0",
      NULL,
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
      "pcsx2_upscale_multiplier",
      "Internal Resolution (Restart)",
      NULL,
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
      "pcsx2_deinterlace_mode",
      "Deinterlacing",
      NULL,
      NULL,
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
      "pcsx2_pcrtc_antiblur",
      "PCRTC Anti-Blur",
      NULL,
      NULL,
      NULL,
      "video",
      {
         { "enabled", NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
#if 0
   {
      "pcsx2_sw_renderer_threads",
      "Software Renderer Threads",
      NULL,
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
      "EE Cycle Rate",
      NULL,
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
      "EE Cycle Skipping",
      NULL,
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
      "pcsx2_axis_scale1",
      "Port 1: Analog Sensitivity",
      NULL,
      NULL,
      NULL,
      "input",
      {
         { "50%", NULL },
         { "60%", NULL },
         { "70%", NULL },
         { "80%", NULL },
         { "90%", NULL },
         { "100%", NULL },
         { "110%", NULL },
         { "120%", NULL },
         { "130%", NULL },
         { "133%", NULL },
         { "140%", NULL },
         { "150%", NULL },
         { "160%", NULL },
         { "170%", NULL },
         { "180%", NULL },
         { "190%", NULL },
         { "200%", NULL },
         { NULL, NULL },
      },
      "133%"
   },
   {
      "pcsx2_axis_scale2",
      "Port 2: Analog Sensitivity",
      NULL,
      NULL,
      NULL,
      "input",
      {
         { "50%", NULL },
         { "60%", NULL },
         { "70%", NULL },
         { "80%", NULL },
         { "90%", NULL },
         { "100%", NULL },
         { "110%", NULL },
         { "120%", NULL },
         { "130%", NULL },
         { "133%", NULL },
         { "140%", NULL },
         { "150%", NULL },
         { "160%", NULL },
         { "170%", NULL },
         { "180%", NULL },
         { "190%", NULL },
         { "200%", NULL },
         { NULL, NULL },
      },
      "133%"
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
