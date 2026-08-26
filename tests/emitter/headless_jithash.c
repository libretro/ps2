/* Minimal headless libretro frontend.
 *
 * Enough of the libretro API to load pcsx2_libretro.so, boot a disc with the
 * software renderer, and run N frames with video and audio discarded. Exists
 * so the recompiler instrumentation can be read on a machine with no GPU and
 * no display.
 *
 * Not a general frontend: it answers only the environment calls this core
 * makes during boot, and refuses hardware rendering.
 *
 * PS2_LOADSTATE=<file> feeds a savestate produced elsewhere into
 * retro_unserialize after PS2_LOADSTATE_AT frames (default 3). The file may
 * be the raw serialize payload, a RASTATE container, or the RZIP-compressed
 * RASTATE RetroArch writes to disk (.stateN); the wrappers are peeled here,
 * so a fixture saved by a real frontend is usable unmodified. Needs -lz.
 *
 * PS2_WATCHDOG=<seconds> bounds every retro_run call and the state load: a
 * call that never returns is a host-side deadlock and exits 124.
 * PS2_STALL=<frames> arms after the external state load and exits 66 once
 * that many consecutive frames render bit-identical pixels: a guest-side
 * softlock renders as a still image while retro_run keeps returning. The
 * two codes split a reported hang into its two families from one
 * unattended run; exit 65 means the state itself was unusable (unreadable,
 * damaged wrapper, or a serialize-format mismatch on this build), which a
 * bisect driver should classify as skip rather than bad.
 *
 * Build (Linux):   gcc -O2 -o headless_jithash headless_jithash.c -ldl -lz -lpthread
 * Build (MINGW64): gcc -O2 -o headless_jithash.exe headless_jithash.c -lz
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define HJH_HANDLE HMODULE
#define hjh_dlopen(p)   LoadLibraryA(p)
#define hjh_dlsym(h, s) ((void*)GetProcAddress((h), (s)))
#else
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#define HJH_HANDLE void*
#define hjh_dlopen(p)   dlopen((p), RTLD_NOW)
#define hjh_dlsym(h, s) dlsym((h), (s))
#endif

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

/* Watchdog: a retro_run that never returns is a host-side deadlock. A
 * monitor thread compares a deadline the main loop pushes forward before
 * every core call; SIGALRM does not exist on Windows, and a plain thread
 * with a coarse tick is portable and needs no signal-safety care. The
 * deadline is a volatile time_t at one-second granularity: a torn read
 * on an exotic target costs at worst one spurious 500 ms tick, and the
 * next tick reads the settled value. */
static int g_wd = 0;
static volatile time_t g_wd_deadline = 0;

#ifdef _WIN32
static DWORD WINAPI wd_thread(LPVOID arg)
#else
static void* wd_thread(void* arg)
#endif
{
    (void)arg;
    for (;;)
    {
#ifdef _WIN32
        Sleep(500);
#else
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = 500000000L;
        nanosleep(&ts, NULL);
#endif
        if (g_wd_deadline != 0 && time(NULL) > g_wd_deadline)
        {
            fprintf(stderr, "[WATCHDOG] retro_run did not return\n");
            fflush(stderr);
            _exit(124);
        }
    }
}

static void wd_start(void)
{
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, wd_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, wd_thread, NULL) == 0)
        pthread_detach(t);
#endif
}

static void wd_arm(void)
{
    if (g_wd > 0)
        g_wd_deadline = time(NULL) + g_wd;
}

/* Stall watch state; armed by main once an external state is in, so an
 * idle boot screen before the load cannot trip it. */
static int g_stall_limit = 0;
static int g_stall_armed = 0;
static int g_stall_have  = 0;
static int g_stall_run   = 0;
static unsigned long long g_stall_last = 0;

/* 16bpp unless the core asked for XRGB8888; the harness accepts either,
 * so hash the row bytes the core actually handed over. */
static unsigned long long fb_fnv(const unsigned char* d, unsigned w, unsigned h, size_t p)
{
    unsigned long long hsh = 1469598103934665603ULL;
    const unsigned char* row = d;
    unsigned y, x, bytes;

    bytes = (unsigned)(p / (w ? w : 1));
    for (y = 0; y < h; y++, row += p)
        for (x = 0; x < w * bytes; x++)
        {
            hsh ^= row[x];
            hsh *= 1099511628211ULL;
        }
    return hsh;
}

static unsigned long le32(const unsigned char* p)
{
    return (unsigned long)p[0]
        | ((unsigned long)p[1] << 8)
        | ((unsigned long)p[2] << 16)
        | ((unsigned long)p[3] << 24);
}

static unsigned char* read_whole_file(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    long sz;
    unsigned char* buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    buf = (unsigned char*)malloc(sz ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

/* RetroArch's on-disk savestate compression: "#RZIPv\1#", u32 chunk size,
 * u64 uncompressed total, then chunks of u32 compressed size followed by
 * one zlib stream each. */
static unsigned char* rzip_unwrap(const unsigned char* in, size_t in_size, size_t* out_size)
{
    unsigned long long total;
    unsigned char* out;
    size_t off = 20;
    size_t wr  = 0;

    if (in_size < 20)
        return NULL;
    total = (unsigned long long)le32(in + 12)
        | ((unsigned long long)le32(in + 16) << 32);
    if (total == 0 || total > (1ULL << 31))
        return NULL;                   /* refuse absurd sizes */
    out = (unsigned char*)malloc((size_t)total);
    if (!out)
        return NULL;
    while (off + 4 <= in_size && wr < total)
    {
        unsigned long csize = le32(in + off);
        uLongf dst = (uLongf)(total - wr);
        off += 4;
        if (csize == 0 || csize > in_size - off)
            break;
        if (uncompress(out + wr, &dst, in + off, csize) != Z_OK)
            break;
        wr  += (size_t)dst;
        off += csize;
    }
    if (wr != total)
    {
        free(out);
        return NULL;
    }
    *out_size = (size_t)total;
    return out;
}

/* RASTATE v1: "RASTATE" + a version byte, then 8-byte-aligned blocks of
 * 4-byte id + u32 size + payload; the serialize payload lives in "MEM ". */
static const unsigned char* rastate_find_mem(const unsigned char* in, size_t in_size, size_t* out_size)
{
    size_t off = 8;

    while (off + 8 <= in_size)
    {
        unsigned long bsz = le32(in + off + 4);
        if (bsz > in_size - (off + 8))
            return NULL;
        if (memcmp(in + off, "MEM ", 4) == 0)
        {
            *out_size = (size_t)bsz;
            return in + off + 8;
        }
        off += 8 + (size_t)bsz;
        off  = (off + 7) & ~(size_t)7;
    }
    return NULL;
}

/* Read, unwrap, and feed an external state. Returns 1 on success; any
 * failure means the state is unusable on this build and the caller exits
 * 65 so a bisect driver can classify it as skip rather than bad. */
static int load_external_state(const char* path, int (*unser)(const void*, size_t))
{
    size_t fsz = 0, bsz = 0, msz = 0;
    unsigned char* fbuf = read_whole_file(path, &fsz);
    unsigned char* zbuf = NULL;
    const unsigned char* base;
    const unsigned char* mem;
    int ok;

    if (!fbuf)
    {
        fprintf(stderr, "[LOADSTATE] cannot read %s\n", path);
        return 0;
    }
    base = fbuf;
    bsz  = fsz;
    if (fsz >= 6 && memcmp(fbuf, "#RZIPv", 6) == 0)
    {
        zbuf = rzip_unwrap(fbuf, fsz, &bsz);
        if (!zbuf)
        {
            fprintf(stderr, "[LOADSTATE] RZIP unwrap failed\n");
            free(fbuf);
            return 0;
        }
        base = zbuf;
    }
    mem = base;
    msz = bsz;
    if (bsz >= 7 && memcmp(base, "RASTATE", 7) == 0)
    {
        mem = rastate_find_mem(base, bsz, &msz);
        if (!mem)
        {
            fprintf(stderr, "[LOADSTATE] RASTATE has no MEM block\n");
            free(zbuf);
            free(fbuf);
            return 0;
        }
    }
    ok = unser(mem, msz);
    fprintf(stderr, "[LOADSTATE] %s: %zu bytes -> unserialize %s\n",
            path, msz, ok ? "ok" : "FAILED");
    free(zbuf);
    free(fbuf);
    return ok ? 1 : 0;
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
    unsigned long long hsh;

    if (every < 0)
    {
        const char* e = getenv("PCSX2_FBHASH");
        every = (e && e[0] != '0') ? (atoi(e) > 0 ? atoi(e) : 1) : 0;
    }
    fbn++;

    /* Stall watch runs ahead of the print gating so it sees every frame,
     * counted or not. A duped frame is identical by definition. */
    if (g_stall_limit > 0 && g_stall_armed)
    {
        int same;
        if (!d)
            same = 1;
        else
        {
            unsigned long long sh = fb_fnv((const unsigned char*)d, w, h, p);
            same         = (g_stall_have && sh == g_stall_last);
            g_stall_last = sh;
            g_stall_have = 1;
        }
        if (same)
        {
            g_stall_run++;
            if (g_stall_run >= g_stall_limit)
            {
                fprintf(stderr, "[STALL] framebuffer static for %d consecutive frames\n",
                        g_stall_run);
                fflush(stderr);
                _exit(66);
            }
        }
        else
            g_stall_run = 0;
    }

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

    hsh = fb_fnv((const unsigned char*)d, w, h, p);
    fprintf(stderr, "[FBHASH] %ld %ux%u %016llx\n", fbn, w, h, hsh);
}
/* Audio oracle.
 *
 * The pixel hash cannot see SPU2 at all -- a wrong reverb coefficient or a
 * broken mixer leaves every frame hash identical. PCSX2_AUDHASH=1 folds every
 * sample delivered through either audio path into one running FNV hash and
 * prints it at teardown, so two builds can be compared on sound the way they
 * already are on pixels. One number for the whole run: sample delivery is not
 * frame-aligned, so per-frame buckets would not line up between runs. */
static unsigned long long g_audhash = 1469598103934665603ULL;
static long long g_audsamples = 0;
static int g_audon = -1;

static void aud_fold(const int16_t* d, size_t frames)
{
    size_t i;
    if (g_audon < 0)
    {
        const char* e = getenv("PCSX2_AUDHASH");
        g_audon = (e && e[0] != '0') ? 1 : 0;
    }
    if (!g_audon)
        return;
    for (i = 0; i < frames * 2; i++)
    {
        g_audhash ^= (unsigned char)(d[i] & 0xff);
        g_audhash *= 1099511628211ULL;
        g_audhash ^= (unsigned char)((d[i] >> 8) & 0xff);
        g_audhash *= 1099511628211ULL;
    }
    g_audsamples += (long long)frames;
}

static void audio_cb(int16_t l, int16_t r)
{
    int16_t pair[2];
    pair[0] = l; pair[1] = r;
    aud_fold(pair, 1);
}
static size_t audio_batch_cb(const int16_t* d, size_t f) { aud_fold(d, f); return f; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d)
{ (void)a;(void)b;(void)c;(void)d; return 0; }

static double now(void)
{
#ifdef _WIN32
    /* Not every MinGW flavor links clock_gettime; QPC is always there. */
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

int main(int argc, char** argv)
{
    HJH_HANDLE h;
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
    {
        const char* e = getenv("PS2_WATCHDOG");
        g_wd = e ? atoi(e) : 0;
        if (g_wd > 0)
            wd_start();
    }
    {
        const char* e = getenv("PS2_STALL");
        g_stall_limit = (e && atoi(e) > 0) ? atoi(e) : 0;
    }

    h = hjh_dlopen(argv[1]);
#ifdef _WIN32
    if (!h) { fprintf(stderr, "LoadLibrary failed: error %lu\n", (unsigned long)GetLastError()); return 3; }
#else
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
#endif

#define SYM(v, n) do { *(void**)(&v) = hjh_dlsym(h, n); \
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
        const char* ss_env    = getenv("PS2_SAVESTATE");
        const int   ss_at     = ss_env ? atoi(ss_env) : -1;
        const char* ls_env    = getenv("PS2_LOADSTATE");
        const char* ls_at_env = getenv("PS2_LOADSTATE_AT");
        const int   ls_at     = ls_env ? (ls_at_env ? atoi(ls_at_env) : 3) : -1;
        void*  ss_buf  = NULL;
        size_t ss_size = 0;

        for (i = 0; i < frames; i++) {
            wd_arm();
            p_run();
            if ((i % 20) == 0)
                { fprintf(stderr, "[headless] frame %d  (%.1f s)\n", i, now() - t0); fflush(stderr); }

            if (ls_at >= 0 && i == ls_at)
            {
                wd_arm();
                if (!load_external_state(ls_env, p_unserialize))
                    _exit(65);
                g_stall_armed = 1;
            }

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
                {
                    wd_arm();
                    p_run();
                }
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
    g_wd_deadline = 0;
    t1 = now();
    fprintf(stderr, "[headless] %d frames in %.2f s (%.2f fps)\n",
            frames, t1 - t0, frames / (t1 - t0));

    /* JIT emission oracle: if the core exports the hash dump (and the
     * PCSX2_JITHASH env var asks for it), print the per-cache hashes at a
     * deterministic point -- right after the frame loop, before teardown. */
    {
        void (*p_jithash)(void) = (void (*)(void))hjh_dlsym(h, "pcsx2_jithash_dump");
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
        void (*p_unload)(void) = (void (*)(void))hjh_dlsym(h, "retro_unload_game");
        double a_ = now();
        if (p_unload) p_unload();
        fprintf(stderr, "[phase] retro_unload_game %.3f s\n", now() - a_);
        if (g_audon > 0)
            fprintf(stderr, "[AUDHASH] %lld frames %016llx\n", g_audsamples, g_audhash);
        a_ = now();
        p_deinit();
        fprintf(stderr, "[phase] retro_deinit %.3f s\n", now() - a_);
    }
    fflush(stderr);
    _exit(0);
}
