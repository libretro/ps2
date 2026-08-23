/* lrps2_smoke: load the freshly linked core and walk the libretro
 * lifecycle a frontend performs before any content exists: resolve the
 * exports, wire the callbacks, retro_init, retro_load_game with no
 * content and an empty system directory (the BIOS-less failure path),
 * retro_deinit, and unload.  Every environment query the harness cannot
 * answer returns false, which is the API's contract for a frontend
 * without the feature - so the run exercises the core's default paths:
 * option parsing without a frontend value store, the VFS negotiation
 * fallback chain, the BIOS scan over an empty directory, and static
 * destructor teardown at unload.
 *
 * The gate is the process exit: a crash anywhere in that walk fails the
 * build before an artifact exists.  A false from retro_load_game is the
 * expected answer (there is no BIOS to boot) and does not fail the gate.
 *
 * What this does not cover: nothing past a successful boot runs here -
 * no BIOS ships to CI, so EE execution, GS bring-up, and the first
 * vsync remain runtime-untested.  The gate bounds the blast radius of
 * an init/teardown regression; it does not replace booting the core. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#define SMOKE_HANDLE HMODULE
#define smoke_dlopen(p) LoadLibraryA(p)
#define smoke_dlsym(h, s) ((void*)GetProcAddress((h), (s)))
#define smoke_dlclose(h) FreeLibrary(h)
#else
#include <dlfcn.h>
#define SMOKE_HANDLE void*
#define smoke_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define smoke_dlsym(h, s) dlsym((h), (s))
#define smoke_dlclose(h) dlclose(h)
#endif

typedef void (*smoke_fn_t)(void);

typedef unsigned retro_api_version_t(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef void retro_get_system_info_t(struct retro_system_info*);
typedef bool retro_load_game_t(const struct retro_game_info*);

/* POSIX blesses the dlsym object-to-function conversion but ISO C does
 * not name it; going through memcpy keeps the pedantic build silent.
 * On Windows, GetProcAddress already returns a function pointer and the
 * copy is the same no-op. */
static smoke_fn_t smoke_sym(SMOKE_HANDLE h, const char *name)
{
   void *obj = smoke_dlsym(h, name);
   smoke_fn_t fn = 0;
   memcpy(&fn, &obj, sizeof(fn));
   return fn;
}

/* The phase string names the last lifecycle step entered; on a crash
 * it is the final line in the CI log before the nonzero exit. */
static void smoke_phase(const char *name)
{
   printf("lrps2_smoke: %s\n", name);
   fflush(stdout);
}

static void smoke_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   (void)level;
   va_start(ap, fmt);
   printf("  [core] ");
   vprintf(fmt, ap);
   va_end(ap);
   fflush(stdout);
}

static void smoke_video_refresh(const void *data, unsigned width,
      unsigned height, size_t pitch)
{
   (void)data; (void)width; (void)height; (void)pitch;
}

static void smoke_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t smoke_audio_batch(const int16_t *data, size_t frames)
{
   (void)data;
   return frames;
}

static void smoke_input_poll(void) { }

static int16_t smoke_input_state(unsigned port, unsigned device,
      unsigned index, unsigned id)
{
   (void)port; (void)device; (void)index; (void)id;
   return 0;
}

/* Answer only what an artifact-gating run needs; everything else is a
 * false, the contract for an absent frontend feature.  The system and
 * save directories point at the harness working directory, so the BIOS
 * scan walks a real (empty) tree through the core's own path code. */
static bool smoke_environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = smoke_log;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = ".";
         return true;
      case RETRO_ENVIRONMENT_GET_LANGUAGE:
         *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
         return true;
      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned*)data = 2;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
         /* No value store: the core takes every option default. */
         ((struct retro_variable*)data)->value = NULL;
         return false;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      default:
         return false;
   }
}

int main(int argc, char **argv)
{
   SMOKE_HANDLE h;
   retro_api_version_t *api_version;
   retro_set_environment_t *set_environment;
   retro_set_video_refresh_t *set_video_refresh;
   retro_set_audio_sample_t *set_audio_sample;
   retro_set_audio_sample_batch_t *set_audio_sample_batch;
   retro_set_input_poll_t *set_input_poll;
   retro_set_input_state_t *set_input_state;
   retro_simple_t *init_fn;
   retro_simple_t *deinit_fn;
   retro_simple_t *unload_game;
   retro_get_system_info_t *get_system_info;
   retro_load_game_t *load_game;
   struct retro_system_info sysinfo;
   unsigned version;
   bool loaded;

   if (argc < 2)
   {
      fprintf(stderr, "usage: lrps2_smoke <path-to-core>\n");
      return 1;
   }

#ifdef _WIN32
   /* A crash must exit the process, not raise a WER dialog that parks
    * the CI runner until the job times out. */
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
         SEM_NOOPENFILEERRORBOX);
#ifdef _MSC_VER
   _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#endif

   smoke_phase("dlopen");
   h = smoke_dlopen(argv[1]);
   if (!h)
   {
      fprintf(stderr, "lrps2_smoke: cannot load %s\n", argv[1]);
      return 1;
   }

   smoke_phase("resolve exports");
   api_version            = (retro_api_version_t*)smoke_sym(h, "retro_api_version");
   set_environment        = (retro_set_environment_t*)smoke_sym(h, "retro_set_environment");
   set_video_refresh      = (retro_set_video_refresh_t*)smoke_sym(h, "retro_set_video_refresh");
   set_audio_sample       = (retro_set_audio_sample_t*)smoke_sym(h, "retro_set_audio_sample");
   set_audio_sample_batch = (retro_set_audio_sample_batch_t*)smoke_sym(h, "retro_set_audio_sample_batch");
   set_input_poll         = (retro_set_input_poll_t*)smoke_sym(h, "retro_set_input_poll");
   set_input_state        = (retro_set_input_state_t*)smoke_sym(h, "retro_set_input_state");
   init_fn                = (retro_simple_t*)smoke_sym(h, "retro_init");
   deinit_fn              = (retro_simple_t*)smoke_sym(h, "retro_deinit");
   unload_game            = (retro_simple_t*)smoke_sym(h, "retro_unload_game");
   get_system_info        = (retro_get_system_info_t*)smoke_sym(h, "retro_get_system_info");
   load_game              = (retro_load_game_t*)smoke_sym(h, "retro_load_game");

   if (!api_version || !set_environment || !set_video_refresh ||
         !set_audio_sample || !set_audio_sample_batch || !set_input_poll ||
         !set_input_state || !init_fn || !deinit_fn || !unload_game ||
         !get_system_info || !load_game)
   {
      fprintf(stderr, "lrps2_smoke: core is missing a libretro export\n");
      return 2;
   }

   smoke_phase("retro_api_version");
   version = api_version();
   if (version != RETRO_API_VERSION)
   {
      fprintf(stderr, "lrps2_smoke: API version %u, expected %u\n",
            version, (unsigned)RETRO_API_VERSION);
      return 3;
   }

   smoke_phase("retro_get_system_info");
   memset(&sysinfo, 0, sizeof(sysinfo));
   get_system_info(&sysinfo);
   printf("  core: %s %s\n",
         sysinfo.library_name    ? sysinfo.library_name    : "(null)",
         sysinfo.library_version ? sysinfo.library_version : "(null)");

   smoke_phase("retro_set_environment");
   set_environment(smoke_environment);

   smoke_phase("set frontend callbacks");
   set_video_refresh(smoke_video_refresh);
   set_audio_sample(smoke_audio_sample);
   set_audio_sample_batch(smoke_audio_batch);
   set_input_poll(smoke_input_poll);
   set_input_state(smoke_input_state);

   smoke_phase("retro_init");
   init_fn();

   smoke_phase("retro_load_game (no content, no BIOS)");
   loaded = load_game(NULL);
   printf("  retro_load_game: %s\n", loaded ? "true" : "false (expected)");
   fflush(stdout);

   if (loaded)
   {
      smoke_phase("retro_unload_game");
      unload_game();
   }

   smoke_phase("retro_deinit");
   deinit_fn();

   smoke_phase("dlclose");
   smoke_dlclose(h);

   smoke_phase("clean exit");
   return 0;
}
