/* xmmpressure: every VU upper op, compiled by the shipping recompiler with
 * the VU accuracy options on, must fit the 15 allocatable XMM registers.
 *
 * The exact multiply and accurate add/sub composites take their scratch
 * registers from the microVU allocator. An op whose operands, cached ACC,
 * overflow-blend mask and composite temps add up to more than fifteen
 * asks the allocator for a register it does not have. Both allocators
 * then answer -1 -- the micro one from findFreeRegRec, the macro (COP2)
 * one from the EE's _getFreeXMMreg -- and every caller indexes its
 * register map with it. The core now aborts at that point instead; this
 * harness drives the real recompiler through every op shape so the abort
 * fires here rather than in a game.
 *
 * It loads the core, boots it on a synthetic BIOS image (a ROMDIR with a
 * ROMVER entry and a branch-to-self reset vector -- enough for the BIOS
 * scan, and the EE never needs to run it), then for each option set:
 *   - micro mode: every upper op, at five dest masks and three register
 *     patterns, followed by FSAND/FMAND so the flag paths are live, run
 *     through ps2_audit_vu_prep/exec on VU0 and VU1;
 *   - macro mode: the same ops as COP2 macro instructions followed by
 *     CFC2 reads of the status and MAC flags, compiled and run by the EE
 *     recompiler through ps2_audit_ee_prep/exec.
 * The gate is the process exit status.
 *
 * Usage: xmmpressure <path-to-core> <scratch-dir> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define XP_HANDLE HMODULE
#define xp_dlopen(p) LoadLibraryA(p)
#define xp_mkdir(p) _mkdir(p)
#else
#include <dlfcn.h>
#include <sys/stat.h>
#define XP_HANDLE void*
#define xp_dlopen(p) dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define xp_mkdir(p) mkdir((p), 0755)
#endif

typedef void (*xp_fn_t)(void);
typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef void vu_prep_t(int, int, const unsigned char*, unsigned);
typedef void vu_exec_t(int, int, unsigned char*, unsigned);
typedef void ee_prep_t(const unsigned*, unsigned);
typedef void ee_exec_t(int, unsigned char*, unsigned);

static char s_system_dir[1024];
static const char* s_exact_mul   = "disabled";
static const char* s_acc_addsub  = "disabled";

/* GetProcAddress already returns a function pointer; POSIX dlsym returns
 * an object pointer, copied across as lrps2_smoke does. */
static xp_fn_t xp_sym(XP_HANDLE h, const char* name)
{
#ifdef _WIN32
	return (xp_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	xp_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

static void xp_log(enum retro_log_level level, const char* fmt, ...)
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

static void xp_video(const void* d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }

static bool xp_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = xp_log;
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
			if      (!strcmp(var->key, "pcsx2_bios"))               var->value = "lrps2_xmmpressure.bin";
			else if (!strcmp(var->key, "pcsx2_renderer"))           var->value = "Software (SW)";
			else if (!strcmp(var->key, "pcsx2_vu_exact_mul"))       var->value = s_exact_mul;
			else if (!strcmp(var->key, "pcsx2_vu_accurate_addsub")) var->value = s_acc_addsub;
			else if (!strcmp(var->key, "pcsx2_mtvu"))               var->value = "disabled";
			else if (!strcmp(var->key, "pcsx2_ee_cpu"))             var->value = "Recompiler";
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

static void xp_put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* A 4MB image the core's BIOS scan accepts: the reset vector branches to
 * itself, the ROMDIR follows at 0x1000 (RESET covers the vector page,
 * ROMDIR covers its own entries) and ROMVER holds a USA console version
 * string. */
static int xp_write_bios(const char* path)
{
	static unsigned char rom[4 * 1024 * 1024];
	FILE* f;
	memset(rom, 0, sizeof(rom));
	xp_put32(rom + 0, 0x1000FFFFu); /* b . */
	memcpy(rom + 0x1000, "RESET", 5);  xp_put32(rom + 0x1000 + 12, 0x1000);
	memcpy(rom + 0x1010, "ROMDIR", 6); xp_put32(rom + 0x1010 + 12, 0x40);
	memcpy(rom + 0x1020, "ROMVER", 6); xp_put32(rom + 0x1020 + 12, 0x10);
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

/* Upper op function codes: the 48 primary ops, and the four special
 * tables' twelve entries each (fd field selects; CLIP and NOP included,
 * the one unknown slot in the last table skipped). */
#define LNOP   0x8000033Cu
#define UNOP   0x000002FFu
#define EBIT   (1u << 30)
#define FSAND  ((0x16u << 25) | (4u << 16) | (1u << 21) | 0x7FFu) /* fsand vi4, 0xfff */
#define FMAND  ((0x1Au << 25) | (5u << 16) | (6u << 11))          /* fmand vi5, vi6   */

static unsigned xp_upper_word(unsigned funct, unsigned special, unsigned dest,
	unsigned ft, unsigned fs, unsigned fd)
{
	if (funct >= 0x3C)
		fd = special;
	return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}

static int xp_op_count(void)
{
	return 48 + 4 * 12;
}

static void xp_op(int i, unsigned* funct, unsigned* special)
{
	if (i < 48)
	{
		*funct = (unsigned)i;
		*special = 0;
		return;
	}
	i -= 48;
	*funct = 0x3C + (unsigned)(i / 12);
	*special = (unsigned)(i % 12);
}

static const unsigned s_dests[] = {0xF, 0x8, 0x1, 0x5, 0xE};
static const unsigned s_regs[][3] = { /* ft, fs, fd */
	{3, 2, 1}, {1, 1, 1}, {2, 2, 3},
};

static unsigned char s_regs_io[32 * 16 + 32 * 4 + 16 + 4 + 4];
static unsigned char s_ee_io[140 + 32];

static void xp_seed(void)
{
	unsigned i;
	for (i = 0; i < sizeof(s_regs_io); i += 4)
		xp_put32(s_regs_io + i, 0x3F800000u + i * 0x9E3779u);
	memset(s_regs_io + 32 * 16, 0, 32 * 4);
}

static int xp_run_options(vu_prep_t* vu_prep, vu_exec_t* vu_exec, ee_prep_t* ee_prep, ee_exec_t* ee_exec)
{
	int op, unit, cases = 0;
	unsigned d, r;
	for (op = 0; op < xp_op_count(); op++)
	{
		unsigned funct, special;
		xp_op(op, &funct, &special);
		if (funct == 0x3F && special == 10)
			continue; /* unknown slot */
		for (d = 0; d < sizeof(s_dests) / sizeof(s_dests[0]); d++)
		{
			for (r = 0; r < sizeof(s_regs) / sizeof(s_regs[0]); r++)
			{
				const unsigned up = xp_upper_word(funct, special, s_dests[d],
					s_regs[r][0], s_regs[r][1], s_regs[r][2]);
				unsigned prog[10];
				unsigned ee[8];

				/* micro: op, then the flag reads that make its
				 * status and MAC results live */
				prog[0] = LNOP;  prog[1] = up;
				prog[2] = FSAND; prog[3] = up;
				prog[4] = FMAND; prog[5] = UNOP | EBIT;
				prog[6] = LNOP;  prog[7] = UNOP;
				prog[8] = LNOP;  prog[9] = UNOP;
				for (unit = 0; unit < 2; unit++)
				{
					xp_seed();
					vu_prep(unit, 0, (const unsigned char*)prog, sizeof(prog));
					vu_exec(unit, 0, s_regs_io, 64);
					cases++;
				}

				/* macro: COP2 CO form of the same op, twice, then
				 * CFC2 of status and MAC, then a branch to self so
				 * the block ends */
				if (funct >= 0x30 && funct < 0x3C)
					continue;
				ee[0] = (0x12u << 26) | (1u << 25) | up;
				ee[1] = (0x12u << 26) | (1u << 25) | up;
				ee[2] = (0x12u << 26) | (0x02u << 21) | (8u << 16) | (16u << 11); /* cfc2 t0, status */
				ee[3] = (0x12u << 26) | (0x02u << 21) | (9u << 16) | (17u << 11); /* cfc2 t1, mac    */
				ee[4] = 0x1000FFFFu; /* b . */
				ee[5] = 0;
				ee_prep(ee, 6);
				memset(s_ee_io, 0, sizeof(s_ee_io));
				ee_exec(0, s_ee_io, 6);
				cases++;
			}
		}
	}
	return cases;
}

int main(int argc, char** argv)
{
	static const char* const sets[][2] = {
		/* exact mul, accurate add/sub */
		{"disabled", "disabled"},
		{"enabled",  "disabled"},
		{"disabled", "enabled"},
		{"enabled",  "enabled"},
	};
	char path[1100];
	XP_HANDLE h;
	retro_set_environment_t* set_environment;
	retro_simple_t *init_fn, *deinit_fn, *unload_game;
	retro_load_game_t* load_game;
	void (*set_video)(retro_video_refresh_t);
	vu_prep_t* vu_prep;
	vu_exec_t* vu_exec;
	ee_prep_t* ee_prep;
	ee_exec_t* ee_exec;
	unsigned s;

	if (argc < 3)
	{
		fprintf(stderr, "usage: xmmpressure <path-to-core> <scratch-dir>\n");
		return 1;
	}
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif

	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
	{
		fprintf(stderr, "xmmpressure: scratch path too long\n");
		return 1;
	}
	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	xp_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	xp_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/lrps2_xmmpressure.bin", s_system_dir);
	if (!xp_write_bios(path))
	{
		fprintf(stderr, "xmmpressure: cannot write %s\n", path);
		return 1;
	}

	h = xp_dlopen(argv[1]);
	if (!h)
	{
		fprintf(stderr, "xmmpressure: cannot load %s\n", argv[1]);
		return 1;
	}
	set_environment = (retro_set_environment_t*)xp_sym(h, "retro_set_environment");
	init_fn         = (retro_simple_t*)xp_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)xp_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)xp_sym(h, "retro_unload_game");
	load_game       = (retro_load_game_t*)xp_sym(h, "retro_load_game");
	set_video       = (void (*)(retro_video_refresh_t))xp_sym(h, "retro_set_video_refresh");
	vu_prep         = (vu_prep_t*)xp_sym(h, "ps2_audit_vu_prep");
	vu_exec         = (vu_exec_t*)xp_sym(h, "ps2_audit_vu_exec");
	ee_prep         = (ee_prep_t*)xp_sym(h, "ps2_audit_ee_prep");
	ee_exec         = (ee_exec_t*)xp_sym(h, "ps2_audit_ee_exec");
	if (!set_environment || !init_fn || !deinit_fn || !unload_game || !load_game ||
		!set_video || !vu_prep || !vu_exec || !ee_prep || !ee_exec)
	{
		fprintf(stderr, "xmmpressure: core is missing an export\n");
		return 2;
	}

	set_environment(xp_environment);
	set_video(xp_video);
	init_fn();

	for (s = 0; s < sizeof(sets) / sizeof(sets[0]); s++)
	{
		int cases;
		s_exact_mul  = sets[s][0];
		s_acc_addsub = sets[s][1];
		printf("xmmpressure: exact_mul=%s accurate_addsub=%s\n",
			s_exact_mul, s_acc_addsub);
		fflush(stdout);
		if (!load_game(NULL))
		{
			fprintf(stderr, "xmmpressure: retro_load_game failed\n");
			return 3;
		}
		cases = xp_run_options(vu_prep, vu_exec, ee_prep, ee_exec);
		printf("  %d programs compiled and run\n", cases);
		fflush(stdout);
		unload_game();
	}

	deinit_fn();
	printf("xmmpressure: ok\n");
	return 0;
}
