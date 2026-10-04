/* secondload: a second session in the same loaded image runs and ends.
 *
 * A frontend closes content with retro_unload_game and retro_deinit and
 * then gives the library back, but it may not get a fresh image for the
 * next load: whether dlclose unmaps is not the core's to decide, and a
 * frontend may keep the library loaded on purpose. Whatever the first
 * session left in the core's statics is then the second session's
 * starting state. This harness keeps the library loaded across three
 * sessions - retro_init, a content-less boot, a few hundred frames,
 * retro_unload_game, retro_deinit - on a synthetic BIOS image (a ROMDIR
 * with a ROMVER entry and a branch-to-self reset vector: the EE spins in
 * place, the vsyncs and the GS ring run as in any boot).
 *
 * A session that cannot drain the GS ring hangs in retro_run or at
 * unload with every thread spinning, so the gate is that the process
 * ends: build.sh runs it under a time limit.
 *
 * Usage: secondload <path-to-core> <scratch-dir> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define SL_HANDLE HMODULE
#define sl_dlopen(p) LoadLibraryA(p)
#define sl_mkdir(p) _mkdir(p)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define SL_HANDLE void*
#define sl_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define sl_mkdir(p) mkdir((p), 0755)
#endif

#define SL_SESSIONS 3
#define SL_FRAMES   60

typedef void (*sl_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);

static char s_system_dir[1024];
static unsigned s_frames;
static unsigned s_drawn;

/* GetProcAddress already returns a function pointer; POSIX dlsym returns
 * an object pointer, copied across as lrps2_smoke does. */
static sl_fn_t sl_sym(SL_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (sl_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	sl_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void sl_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	printf("  [core] ");
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void sl_video(const void* d, unsigned w, unsigned h, size_t p)
{
	(void)w; (void)h; (void)p;
	s_frames++;
	if (d)
		s_drawn++;
}
static void sl_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t sl_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void sl_poll(void) { }
static int16_t sl_input(unsigned p, unsigned d, unsigned i, unsigned id)
{
	(void)p; (void)d; (void)i; (void)id;
	return 0;
}

static bool sl_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = sl_log;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = s_system_dir;
			return true;
		case RETRO_ENVIRONMENT_GET_LANGUAGE:
			*(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = 2;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
			var = (struct retro_variable*)data;
			var->value = NULL;
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = "lrps2_secondload.bin";
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = "Software (SW)";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = "disabled";
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

static void sl_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* MIPS encodings for the reset program: t0 = 8, t1 = 9, t2 = 10. */
#define SL_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define SL_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define SL_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SL_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_ANDI(rt, rs, imm)  (0x30000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_BNE(rs, rt, off)   (0x14000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_B(off)             (0x10000000u | ((off) & 0xFFFFu))

static unsigned sl_emit(unsigned char* rom, unsigned pc, unsigned op)
{
	sl_put32(rom + pc, op);
	return pc + 4;
}

/* t1 = hi:lo, then store it at offset off from base. */
static unsigned sl_emit_store64(unsigned char* rom, unsigned pc, unsigned base,
	unsigned off, unsigned hi, unsigned lo)
{
	pc = sl_emit(rom, pc, SL_LUI(9, hi >> 16));
	pc = sl_emit(rom, pc, SL_ORI(9, 9, hi));
	pc = sl_emit(rom, pc, SL_DSLL32(9, 9));
	pc = sl_emit(rom, pc, SL_LUI(10, lo >> 16));
	pc = sl_emit(rom, pc, SL_ORI(10, 10, lo));
	pc = sl_emit(rom, pc, SL_DSLL32(10, 10));
	pc = sl_emit(rom, pc, SL_DSRL32(10, 10));
	pc = sl_emit(rom, pc, SL_OR(9, 9, 10));
	return sl_emit(rom, pc, SL_SD(9, off, base));
}

/* A 4MB image the core's BIOS scan accepts. Its reset program turns on a
 * 640x448 interlaced display of read circuit 1, writes a two-quadword GIF
 * packet (an A+D write of TEXFLUSH) to main RAM, and then sends it down
 * the GIF DMA channel forever, waiting for each transfer to end: every
 * vsync is scanned out and the GS ring never stops carrying packets,
 * which is what a BIOS screen does. The ROMDIR follows at 0x1000 and
 * ROMVER holds a USA console version string. */
static int sl_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, wait;
	memset(rom, 0, sizeof(rom));
	pc = sl_emit(rom, 0, SL_LUI(8, 0xB200));                         /* t0 = GS privileged regs */
	pc = sl_emit_store64(rom, pc, 8, 0x00, 0, 0xFF25u);              /* PMODE: RC1, alpha FF    */
	pc = sl_emit_store64(rom, pc, 8, 0x20, 0, 1u);                   /* SMODE2: interlaced      */
	pc = sl_emit_store64(rom, pc, 8, 0x70, 0, 10u << 9);             /* DISPFB1: 640 wide       */
	pc = sl_emit_store64(rom, pc, 8, 0x80, 2559u | (447u << 12),     /* DISPLAY1                */
		636u | (50u << 12) | (3u << 23));
	pc = sl_emit(rom, pc, SL_LUI(11, 0xA010));                       /* t3 = RAM 0x100000       */
	pc = sl_emit_store64(rom, pc, 11, 0x00, 0x10000000u, 0x8001u);   /* GIFtag: NLOOP 1, EOP, NREG 1 */
	pc = sl_emit_store64(rom, pc, 11, 0x08, 0, 0xEu);                /* REGS: A+D               */
	pc = sl_emit_store64(rom, pc, 11, 0x10, 0, 0);                   /* data                    */
	pc = sl_emit_store64(rom, pc, 11, 0x18, 0, 0x3Fu);               /* address: TEXFLUSH       */
	pc = sl_emit(rom, pc, SL_LUI(10, 0xB000));                       /* D_CTRL = DMAE           */
	pc = sl_emit(rom, pc, SL_ORI(10, 10, 0xE000));
	pc = sl_emit(rom, pc, SL_ORI(9, 0, 1));
	pc = sl_emit(rom, pc, SL_SW(9, 0, 10));
	pc = sl_emit(rom, pc, SL_LUI(13, 0xB000));                       /* t5 = GIF DMA channel    */
	pc = sl_emit(rom, pc, SL_ORI(13, 13, 0xA000));
	pc = sl_emit(rom, pc, SL_LUI(12, 0x0010));                       /* t4 = MADR 0x100000      */
	pc = sl_emit(rom, pc, SL_ORI(14, 0, 2));                         /* t6 = QWC 2              */
	pc = sl_emit(rom, pc, SL_ORI(15, 0, 0x101));                     /* t7 = CHCR: DIR | STR    */
	loop = pc;
	pc = sl_emit(rom, pc, SL_SW(12, 0x10, 13));
	pc = sl_emit(rom, pc, SL_SW(14, 0x20, 13));
	pc = sl_emit(rom, pc, SL_SW(15, 0x00, 13));
	wait = pc;
	pc = sl_emit(rom, pc, SL_LW(9, 0, 13));
	pc = sl_emit(rom, pc, 0);
	pc = sl_emit(rom, pc, SL_ANDI(9, 9, 0x100));
	pc = sl_emit(rom, pc, SL_BNE(9, 0, (wait - (pc + 4)) >> 2));
	pc = sl_emit(rom, pc, 0);
	pc = sl_emit(rom, pc, SL_B((loop - (pc + 4)) >> 2));
	sl_emit(rom, pc, 0);
	memcpy(rom + 0x1000, "RESET", 5);  sl_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); sl_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); sl_put32(rom + 0x1020 + 12, 0x10);
	memcpy(rom + 0x1040, "0160AC20020207", 14);
	f = fopen(path, "wb");
	if (!f)
		return 0;
	if (fwrite(rom, 1, sizeof(rom), f) != sizeof(rom))
	{
		fclose(f);
		return 0;
	}
	return fclose(f) == 0;
}

int main(int argc, char** argv)
{
	char path[1100];
	SL_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game, *run;
	retro_load_game_t* load_game;
	int session, frame;

	if (argc < 3)
	{
		fprintf(stderr, "usage: secondload <path-to-core> <scratch-dir>\n");
		return 1;
	}
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
	{
		fprintf(stderr, "secondload: scratch path too long\n");
		return 1;
	}
	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	sl_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	sl_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/lrps2_secondload.bin", s_system_dir);
	if (!sl_write_bios(path))
	{
		fprintf(stderr, "secondload: cannot write %s\n", path);
		return 1;
	}

	h = sl_dlopen(argv[1]);
	if (!h)
	{
		fprintf(stderr, "secondload: cannot load %s\n", argv[1]);
		return 1;
	}
	set_environment = (retro_set_environment_t*)sl_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)sl_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)sl_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)sl_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)sl_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)sl_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)sl_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)sl_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)sl_sym(h, "retro_unload_game");
	run             = (retro_simple_t*)sl_sym(h, "retro_run");
	load_game       = (retro_load_game_t*)sl_sym(h, "retro_load_game");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll ||
		!set_input || !init_fn || !deinit_fn || !unload_game || !run || !load_game)
	{
		fprintf(stderr, "secondload: core is missing a libretro export\n");
		return 2;
	}

	for (session = 0; session < SL_SESSIONS; session++)
	{
		printf("secondload: session %d\n", session + 1);
		fflush(stdout);
		set_environment(sl_environment);
		set_video(sl_video);
		set_audio(sl_audio);
		set_audio_batch(sl_audio_batch);
		set_poll(sl_poll);
		set_input(sl_input);
		init_fn();
		if (!load_game(NULL))
		{
			fprintf(stderr, "secondload: retro_load_game failed in session %d\n", session + 1);
			return 3;
		}
		s_frames = 0;
		s_drawn  = 0;
		for (frame = 0; frame < SL_FRAMES; frame++)
			run();
		printf("  %d frames run, %u presented, %u of them drawn\n", SL_FRAMES, s_frames, s_drawn);
		fflush(stdout);
		unload_game();
		deinit_fn();
	}

	printf("secondload: ok\n");
	return 0;
}
