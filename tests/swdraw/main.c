/* swdraw: GS draws on the software renderer, driven through the core's
 * libretro exports on a synthetic BIOS image.
 *
 * The reset program copies a GIF packet out of the ROM into main RAM,
 * sends it down the GIF DMA channel once, and then idles with the
 * display on, so the draws in the packet are rendered by the software
 * renderer while the frames run. Each case is one packet of A+D register
 * writes; the harness checks that frames keep coming and the process
 * survives. Run against a core built with SANITIZER=address,undefined,
 * an out-of-bounds access in the renderer fails the run.
 *
 * Usage: swdraw <path-to-core> <scratch-dir> <case> */

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

#define SL_FRAMES   30

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
/* The case's rasterizer threads and scale. */
static const char* s_opt_threads = "0";
static const char* s_opt_scale   = "1";
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

/* FNV-1a of the last frame drawn, row by row. */
static unsigned long s_hash;

static void sl_video(const void* d, unsigned w, unsigned h, size_t p)
{
	s_frames++;
	if (d)
	{
		const unsigned char* row = (const unsigned char*)d;
		unsigned y, x;
		s_drawn++;
		s_hash = 2166136261UL;
		for (y = 0; y < h; y++, row += p)
			for (x = 0; x < w * 4; x++) /* XRGB8888, which the core asks for */
				s_hash = ((s_hash ^ row[x]) * 16777619UL) & 0xffffffffUL;
	}
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
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = "lrps2_swdraw.bin";
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = "Software (SW)";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = "disabled";
			else if (!strcmp(var->key, "pcsx2_sw_renderer_threads")) var->value = s_opt_threads;
			else if (!strcmp(var->key, "pcsx2_upscale_multiplier")) var->value = s_opt_scale;
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

/* MIPS encodings. t0..t7 are 8..15. */
#define SL_LUI(rt, imm)       (0x3C000000u | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_ORI(rt, rs, imm)   (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_ADDIU(rt, rs, imm) (0x24000000u | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFFu))
#define SL_DSLL32(rd, rt)     (0x0000003Cu | ((rt) << 16) | ((rd) << 11))
#define SL_DSRL32(rd, rt)     (0x0000003Eu | ((rt) << 16) | ((rd) << 11))
#define SL_OR(rd, rs, rt)     (0x00000025u | ((rs) << 21) | ((rt) << 16) | ((rd) << 11))
#define SL_SD(rt, off, base)  (0xFC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_SW(rt, off, base)  (0xAC000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
#define SL_LW(rt, off, base)  (0x8C000000u | ((base) << 21) | ((rt) << 16) | ((off) & 0xFFFFu))
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

/* The packet: a GIFtag for A+D, then (data, register) quadwords. */
#define SL_PACKET_ROM 0x2000u
#define SL_MAX_AD     64

static unsigned      s_ad_count;
static unsigned long s_ad[SL_MAX_AD][3]; /* data hi, data lo, register */

static void ad(unsigned reg, unsigned long hi, unsigned long lo)
{
	s_ad[s_ad_count][0] = hi;
	s_ad[s_ad_count][1] = lo;
	s_ad[s_ad_count][2] = reg;
	s_ad_count++;
}

/* GS registers and fields, as the GS manual numbers them. */
#define GS_PRIM     0x00
#define GS_RGBAQ    0x01
#define GS_ST       0x02
#define GS_XYZ2     0x05
#define GS_TEX0_1   0x06
#define GS_TEX1_1   0x14
#define GS_XYOFFSET 0x18
#define GS_SCISSOR  0x40
#define GS_ALPHA_1  0x42
#define GS_TEST_1   0x47
#define GS_FRAME_1  0x4C
#define GS_ZBUF_1   0x4E
#define GS_MIPTBP1  0x34
#define GS_PRMODECONT 0x1A

#define XY(x, y) ((unsigned long)((x) * 16) | ((unsigned long)((y) * 16) << 16))

/* What every case starts from: a 640-wide 32-bit frame at 0, Z off,
 * the whole 2048x2048 drawing area in the scissor. */
static void ad_common(void)
{
	ad(GS_PRMODECONT, 0, 1); /* PRIM's attributes, not PRMODE's */
	ad(GS_FRAME_1, 0, 10ul << 16);
	ad(GS_ZBUF_1, 1, 0);                        /* ZMSK */
	ad(GS_TEST_1, 0, (1ul << 16) | (1ul << 17)); /* ZTE, ZTST always */
	ad(GS_XYOFFSET, 0, 0);
	ad(GS_SCISSOR, (2047ul << 16), (2047ul << 16)); /* x 0..2047, y 0..2047 */
	ad(GS_ALPHA_1, 0, 0x44);
	ad(GS_RGBAQ, 0x3f800000ul, 0x80808080ul);
}

/* An antialiased (AA1) triangle with alpha blending across the whole
 * scissor: its three edges together are longer than the scissor is tall
 * or wide. */
static void case_aa1_triangle(void)
{
	ad_common();
	ad(GS_PRIM, 0, 3 | 0x40 | 0x80); /* triangle, ABE, AA1 */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(2047, 1023));
	ad(GS_XYZ2, 0, XY(0, 2047));
}

/* A small one, whose edges fit however they are flushed. */
static void case_aa1_small(void)
{
	ad_common();
	ad(GS_PRIM, 0, 3 | 0x40 | 0x80);
	ad(GS_XYZ2, 0, XY(100, 60));
	ad(GS_XYZ2, 0, XY(420, 140));
	ad(GS_XYZ2, 0, XY(180, 400));
}

/* An antialiased line the length of the scissor's diagonal. */
static void case_aa1_line(void)
{
	ad_common();
	ad(GS_PRIM, 0, 1 | 0x40 | 0x80); /* line, ABE, AA1 */
	ad(GS_XYZ2, 0, XY(0, 0));
	ad(GS_XYZ2, 0, XY(2047, 2047));
}

/* Each case on one rasterizer, which takes every row, and again at 2x. */
static const struct { const char* name; void (*build)(void); const char* scale; } s_cases[] = {
	{ "aa1_small",       case_aa1_small,    "1" },
	{ "aa1_triangle",    case_aa1_triangle, "1" },
	{ "aa1_triangle_2x", case_aa1_triangle, "2" },
	{ "aa1_line",        case_aa1_line,     "1" },
	{ "aa1_line_2x",     case_aa1_line,     "2" },
};

/* A 4MB image the core's BIOS scan accepts. The reset program turns on a
 * 640x448 interlaced display of read circuit 1, copies the packet to RAM
 * at 0x100000, sends it down the GIF DMA channel, waits for the transfer
 * to end and spins. The ROMDIR follows at 0x1000 and ROMVER holds a USA
 * console version string; the packet sits at SL_PACKET_ROM. */
static int sl_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	unsigned pc, loop, wait, i, qwc = 1 + s_ad_count;
	unsigned char* pk = rom + SL_PACKET_ROM;

	memset(rom, 0, sizeof(rom));
	/* GIFtag: NLOOP = the A+D count, EOP, PACKED, NREG 1, REGS = A+D. */
	sl_put32(pk + 0, s_ad_count | 0x8000u);
	sl_put32(pk + 4, 0x10000000u);
	sl_put32(pk + 8, 0xEu);
	sl_put32(pk + 12, 0);
	for (i = 0; i < s_ad_count; i++)
	{
		unsigned char* q = pk + 16 + i * 16;
		sl_put32(q + 0, (unsigned)s_ad[i][1]);
		sl_put32(q + 4, (unsigned)s_ad[i][0]);
		sl_put32(q + 8, (unsigned)s_ad[i][2]);
		sl_put32(q + 12, 0);
	}

	pc = sl_emit(rom, 0, SL_LUI(8, 0xB200));                         /* t0 = GS privileged regs */
	pc = sl_emit_store64(rom, pc, 8, 0x00, 0, 0xFF25u);              /* PMODE: RC1, alpha FF    */
	pc = sl_emit_store64(rom, pc, 8, 0x20, 0, 1u);                   /* SMODE2: interlaced      */
	pc = sl_emit_store64(rom, pc, 8, 0x70, 0, 10u << 9);             /* DISPFB1: 640 wide       */
	pc = sl_emit_store64(rom, pc, 8, 0x80, 2559u | (447u << 12),     /* DISPLAY1                */
		636u | (50u << 12) | (3u << 23));
	/* Copy the packet: t3 = ROM source, t4 = RAM destination, t6 = words. */
	pc = sl_emit(rom, pc, SL_LUI(11, 0xBFC0));
	pc = sl_emit(rom, pc, SL_ORI(11, 11, SL_PACKET_ROM));
	pc = sl_emit(rom, pc, SL_LUI(12, 0xA010));
	pc = sl_emit(rom, pc, SL_ORI(14, 0, qwc * 4));
	loop = pc;
	pc = sl_emit(rom, pc, SL_LW(9, 0, 11));
	pc = sl_emit(rom, pc, SL_SW(9, 0, 12));
	pc = sl_emit(rom, pc, SL_ADDIU(11, 11, 4));
	pc = sl_emit(rom, pc, SL_ADDIU(12, 12, 4));
	pc = sl_emit(rom, pc, SL_ADDIU(14, 14, -1));
	pc = sl_emit(rom, pc, SL_BNE(14, 0, (loop - (pc + 4)) >> 2));
	pc = sl_emit(rom, pc, 0);
	pc = sl_emit(rom, pc, SL_LUI(10, 0xB000));                       /* D_CTRL = DMAE           */
	pc = sl_emit(rom, pc, SL_ORI(10, 10, 0xE000));
	pc = sl_emit(rom, pc, SL_ORI(9, 0, 1));
	pc = sl_emit(rom, pc, SL_SW(9, 0, 10));
	pc = sl_emit(rom, pc, SL_LUI(13, 0xB000));                       /* t5 = GIF DMA channel    */
	pc = sl_emit(rom, pc, SL_ORI(13, 13, 0xA000));
	pc = sl_emit(rom, pc, SL_LUI(12, 0x0010));                       /* MADR 0x100000           */
	pc = sl_emit(rom, pc, SL_SW(12, 0x10, 13));
	pc = sl_emit(rom, pc, SL_ORI(14, 0, qwc));                       /* QWC                     */
	pc = sl_emit(rom, pc, SL_SW(14, 0x20, 13));
	pc = sl_emit(rom, pc, SL_ORI(15, 0, 0x101));                     /* CHCR: DIR | STR         */
	pc = sl_emit(rom, pc, SL_SW(15, 0x00, 13));
	wait = pc;
	pc = sl_emit(rom, pc, SL_B(-1));                                 /* spin                    */
	sl_emit(rom, pc, 0);
	(void)wait;
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
	unsigned c;
	int frame;

	if (argc < 4)
	{
		fprintf(stderr, "usage: swdraw <path-to-core> <scratch-dir> <case>\n");
		return 1;
	}
	for (c = 0; c < sizeof(s_cases) / sizeof(s_cases[0]); c++)
		if (!strcmp(argv[3], s_cases[c].name))
			break;
	if (c == sizeof(s_cases) / sizeof(s_cases[0]))
	{
		fprintf(stderr, "swdraw: no case %s\n", argv[3]);
		return 1;
	}
	s_cases[c].build();
	s_opt_scale = s_cases[c].scale;
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
	{
		fprintf(stderr, "swdraw: scratch path too long\n");
		return 1;
	}
	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	sl_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	sl_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/lrps2_swdraw.bin", s_system_dir);
	if (!sl_write_bios(path))
	{
		fprintf(stderr, "swdraw: cannot write %s\n", path);
		return 1;
	}

	h = sl_dlopen(argv[1]);
	if (!h)
	{
		fprintf(stderr, "swdraw: cannot load %s\n", argv[1]);
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
		fprintf(stderr, "swdraw: core is missing a libretro export\n");
		return 2;
	}

	set_environment(sl_environment);
	set_video(sl_video);
	set_audio(sl_audio);
	set_audio_batch(sl_audio_batch);
	set_poll(sl_poll);
	set_input(sl_input);
	init_fn();
	if (!load_game(NULL))
	{
		fprintf(stderr, "swdraw: retro_load_game failed\n");
		return 3;
	}
	for (frame = 0; frame < SL_FRAMES; frame++)
		run();
	unload_game();
	deinit_fn();

	if (s_drawn == 0)
	{
		fprintf(stderr, "swdraw: %s: no frame was drawn\n", s_cases[c].name);
		return 4;
	}
	printf("swdraw: %s ok, %u frames, last %08lx\n", s_cases[c].name, s_frames, s_hash);
	return 0;
}
