/* Minimal headless libretro frontend.
 *
 * Enough of the libretro API to load pcsx2_libretro.so, boot a disc with the
 * software renderer, and run N frames with video and audio discarded. Exists
 * so the recompiler instrumentation can be read on a machine with no GPU and
 * no display.
 *
 * Not a general frontend: it answers only the environment calls this core
 * makes during boot, and refuses hardware rendering.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

/* ---- the slice of libretro.h we need ---- */
#define RETRO_ENVIRONMENT_SET_ROTATION            1
#define RETRO_ENVIRONMENT_GET_OVERSCAN            2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE            3
#define RETRO_ENVIRONMENT_SET_MESSAGE             6
#define RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL   8
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY    9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT       10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS  11
#define RETRO_ENVIRONMENT_GET_VARIABLE           15
#define RETRO_ENVIRONMENT_SET_VARIABLES          16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE    17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME    18
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE      27
#define RETRO_ENVIRONMENT_GET_PERF_INTERFACE     28
#define RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY 30
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO     32
#define RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO     34
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO    35
#define RETRO_ENVIRONMENT_SET_GEOMETRY           37
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY     31
#define RETRO_ENVIRONMENT_SET_HW_RENDER          14
#define RETRO_ENVIRONMENT_GET_LANGUAGE           39
#define RETRO_ENVIRONMENT_GET_INPUT_BITMASKS     (51 | 0x10000)
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS       53
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL  54
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY 55
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2    67
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL 68
#define RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
#define RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK 62
#define RETRO_ENVIRONMENT_GET_THROTTLE_STATE     (71 | 0x10000)
#define RETRO_ENVIRONMENT_GET_FASTFORWARDING     (57 | 0x10000)

struct retro_game_info { const char* path; const void* data; size_t size; const char* meta; };
struct retro_variable { const char* key; const char* value; };
struct retro_log_callback { void (*log)(int level, const char* fmt, ...); };

typedef void (*rt_void)(void);


static char g_sysdir[512];
static int  g_quiet = 0;

static void core_log(int level, const char* fmt, ...)
{
    va_list ap;
    if (g_quiet && level < 2) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Core options we force. Everything else falls through to the core default. */
static const char* opt_get(const char* key)
{
    if (!strcmp(key, "pcsx2_renderer"))       return "Software (SW)";
    if (!strcmp(key, "pcsx2_fastboot"))       return getenv("FASTBOOT") ? "enabled" : "disabled";
    if (!strcmp(key, "pcsx2_fastcdvd"))       return "enabled";
    if (!strcmp(key, "pcsx2_bios"))           return getenv("PS2_BIOS");
    if (!strcmp(key, "pcsx2_ee_cycle_rate"))  return NULL;
    return NULL;
}

static int env_cb(unsigned cmd, void* data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *(const char**)data = g_sysdir;
        return 1;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback*)data)->log = core_log;
        return 1;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(char*)data = 1; return 1;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return 1;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable* v = (struct retro_variable*)data;
        v->value = opt_get(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(char*)data = 0; return 1;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned*)data = 2; return 1;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        return 0;                      /* force software */
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_GET_THROTTLE_STATE:
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
        return 0;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned*)data = 0; return 1;
    default:
        return 1;                      /* accept the rest silently */
    }
}

/* Framebuffer oracle.
 *
 * The JIT hashes prove the recompilers emit the same bytes; they say nothing
 * about what ends up on screen, so any change to the interpreter paths that
 * feed the GS -- VIF unpack, the software rasterizer, the memory handlers --
 * has been landing without a pixel-level gate. PCSX2_FBHASH=1 prints an FNV
 * hash of every Nth frame's pixels (N from PCSX2_FBHASH, default 1) so two
 * builds can be compared frame for frame.
 *
 * Duped frames (d == NULL) are reported as such rather than skipped: whether
 * a frame duped is itself part of the behaviour being compared. */
static void video_cb(const void* d, unsigned w, unsigned h, size_t p)
{
    static long fbn = 0, every = -1;
    unsigned long long hsh = 1469598103934665603ULL;
    const unsigned char* row;
    unsigned y, x, bytes;

    if (every < 0)
    {
        const char* e = getenv("PCSX2_FBHASH");
        every = (e && e[0] != '0') ? (atoi(e) > 0 ? atoi(e) : 1) : 0;
    }
    fbn++;
    if (!every || (fbn % every) != 0)
        return;

    if (!d)
    {
        /* A duped frame carries no pixels, and the dupe pattern is not
         * stable across a savestate reload, so counting them would
         * misalign two otherwise identical streams. Not counted. */
        fbn--;
        return;
    }

    /* 16bpp unless the core asked for XRGB8888; the harness accepts either,
     * so hash the row bytes the core actually handed over. */
    bytes = (unsigned)(p / (w ? w : 1));
    row = (const unsigned char*)d;
    for (y = 0; y < h; y++, row += p)
        for (x = 0; x < w * bytes; x++)
        {
            hsh ^= row[x];
            hsh *= 1099511628211ULL;
        }
    fprintf(stderr, "[FBHASH] %ld %ux%u %016llx\n", fbn, w, h, hsh);
}
static void audio_cb(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t audio_batch_cb(const int16_t* d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d)
{ (void)a;(void)b;(void)c;(void)d; return 0; }

static double now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char** argv)
{
    void* h;
    void (*p_set_environment)(int(*)(unsigned, void*));
    void (*p_set_video_refresh)(void(*)(const void*, unsigned, unsigned, size_t));
    void (*p_set_audio_sample)(void(*)(int16_t, int16_t));
    void (*p_set_audio_sample_batch)(size_t(*)(const int16_t*, size_t));
    void (*p_set_input_poll)(void(*)(void));
    void (*p_set_input_state)(int16_t(*)(unsigned, unsigned, unsigned, unsigned));
    void (*p_init)(void);
    void (*p_deinit)(void);
    size_t (*p_serialize_size)(void);
    int  (*p_serialize)(void*, size_t);
    int  (*p_unserialize)(const void*, size_t);
    int  (*p_load_game)(const struct retro_game_info*);
    void (*p_run)(void);
    struct retro_game_info gi;
    int frames, i;
    double t0, t1;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <core.so> <game.cue> <frames> [sysdir]\n", argv[0]);
        return 2;
    }
    frames = atoi(argv[3]);
    snprintf(g_sysdir, sizeof(g_sysdir), "%s", argc > 4 ? argv[4] : ".");
    g_quiet = getenv("QUIET") != NULL;

    h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }

#define SYM(v, n) do { *(void**)(&v) = dlsym(h, n); \
    if (!v) { fprintf(stderr, "missing %s\n", n); return 4; } } while (0)
    SYM(p_set_environment,        "retro_set_environment");
    SYM(p_set_video_refresh,      "retro_set_video_refresh");
    SYM(p_set_audio_sample,       "retro_set_audio_sample");
    SYM(p_set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(p_set_input_poll,         "retro_set_input_poll");
    SYM(p_set_input_state,        "retro_set_input_state");
    SYM(p_init,                   "retro_init");
    SYM(p_deinit,                 "retro_deinit");
    SYM(p_serialize_size,         "retro_serialize_size");
    SYM(p_serialize,              "retro_serialize");
    SYM(p_unserialize,            "retro_unserialize");
    SYM(p_load_game,              "retro_load_game");
    SYM(p_run,                    "retro_run");
#undef SYM

    p_set_environment(env_cb);
    p_set_video_refresh(video_cb);
    p_set_audio_sample(audio_cb);
    p_set_audio_sample_batch(audio_batch_cb);
    p_set_input_poll(input_poll_cb);
    p_set_input_state(input_state_cb);

    { double a_=now(); p_init(); fprintf(stderr, "[phase] retro_init %.3f s\n", now()-a_); }

    memset(&gi, 0, sizeof(gi));
    gi.path = argv[2];
    { double a_=now(); int ok_=p_load_game(&gi); fprintf(stderr, "[phase] retro_load_game %.3f s\n", now()-a_);
      if (!ok_) { fprintf(stderr, "retro_load_game failed\n"); return 5; } }
    fprintf(stderr, "[headless] game loaded, running %d frames\n", frames);

    t0 = now();
    {
        const char* ss_env = getenv("PS2_SAVESTATE");
        const int   ss_at  = ss_env ? atoi(ss_env) : -1;
        void*  ss_buf  = NULL;
        size_t ss_size = 0;

        for (i = 0; i < frames; i++) {
            p_run();
            if ((i % 20) == 0)
                { fprintf(stderr, "[headless] frame %d  (%.1f s)\n", i, now() - t0); fflush(stderr); }

            if (ss_at >= 0 && i == ss_at && p_serialize_size) {
                ss_size = p_serialize_size();
                ss_buf  = malloc(ss_size);
                if (!ss_buf || !p_serialize(ss_buf, ss_size)) {
                    fprintf(stderr, "[SAVESTATE] serialize failed (size %zu)\n", ss_size);
                    free(ss_buf);
                    ss_buf = NULL;
                } else {
                    fprintf(stderr, "[SAVESTATE] saved at frame %d, %zu bytes\n", i, ss_size);
                }
            }
        }

        if (ss_buf) {
            if (!p_unserialize(ss_buf, ss_size))
                fprintf(stderr, "[SAVESTATE] unserialize failed\n");
            else {
                /* The replay's hash stream must reappear verbatim as a run
                 * of the pre-save stream: same content, offset by wherever
                 * the save landed. Compare the two with
                 *   grep '^\[FBHASH\]' log | awk '{print $4}'
                 * and look for the tail as a substring of the head. */
                fprintf(stderr, "[SAVESTATE] reloaded, replaying %d frames\n", frames - ss_at - 1);
                for (i = ss_at + 1; i < frames; i++)
                    p_run();
            }
            free(ss_buf);
        }
    }
    /* Savestate round trip.
     *
     * PS2_SAVESTATE=N saves after frame N, runs on, then reloads and runs
     * the same number of frames again. With PCSX2_FBHASH set the two
     * post-N hash streams have to match: a savestate that restores an
     * incomplete machine shows up as diverging pixels rather than as a
     * crash, and nothing in this harness tested serialize at all before. */
    t1 = now();
    fprintf(stderr, "[headless] %d frames in %.2f s (%.2f fps)\n",
            frames, t1 - t0, frames / (t1 - t0));

    /* JIT emission oracle: if the core exports the hash dump (and the
     * PCSX2_JITHASH env var asks for it), print the per-cache hashes at a
     * deterministic point -- right after the frame loop, before teardown. */
    {
        void (*p_jithash)(void) = (void (*)(void))dlsym(h, "pcsx2_jithash_dump");
        if (p_jithash)
            p_jithash();
    }

    /* Teardown. This used to be skipped -- the harness _exit(0)'d after
     * printing -- which meant retro_unload_game and retro_deinit were never
     * exercised by any gate. Timed, because shutdown cost is exactly the
     * kind of regression that hides behind an early exit. */
    if (getenv("PS2_SKIP_DEINIT"))
    {
        fflush(stderr);
        _exit(0);
    }
    {
        void (*p_unload)(void) = (void (*)(void))dlsym(h, "retro_unload_game");
        double a_ = now();
        if (p_unload) p_unload();
        fprintf(stderr, "[phase] retro_unload_game %.3f s\n", now() - a_);
        a_ = now();
        p_deinit();
        fprintf(stderr, "[phase] retro_deinit %.3f s\n", now() - a_);
    }
    fflush(stderr);
    _exit(0);
}
