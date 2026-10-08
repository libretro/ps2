/*  The core's built-in game patches (libretro/patches.cpp), driven with
 *  every serial and CRC they name, under every hint setting.
 *
 *  - Each patch block is reachable: every message it logs is logged by
 *    some serial. Two branches of one chain testing the same serial leave
 *    the second one dead.
 *  - One call patches one game: the messages a call logs all name the
 *    same game, so no block hangs off another game's serial.
 *  - Every patch line goes through the real parser without an error.
 *  - Every message is a format string with nothing to format but the
 *    CRC: a bare percent sign is a conversion, reading an argument that
 *    was never passed.
 *
 *  Usage: tests/patch/ingame <path-to-libretro/patches.cpp>
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libretro.h>
#include <vector>
#include <string>

#include "Common.h"
#include "Patch.h"

/* Memory and loader surface Patch.cpp links against; loading from a
 * string enters none of it. */
u8  vtlb_memRead8 (u32) { return 0; }
u16 vtlb_memRead16(u32) { return 0; }
u32 vtlb_memRead32(u32) { return 0; }
u64 vtlb_memRead64(u32) { return 0; }
void vtlb_memWrite8 (u32, u8) { }
void vtlb_memWrite16(u32, u16) { }
void vtlb_memWrite32(u32, u32) { }
void vtlb_memWrite64(u32, u64) { }
void* vtlb_GetPhyPtr(u32) { return 0; }
RETURNS_R128 vtlb_memRead128(u32) { r128 v; memset(&v, 0, sizeof(v)); return v; }
void vtlb_memWrite128(u32, r128) { }
extern "C" void iopMemWrite8 (u32, u8) { }
extern "C" void iopMemWrite16(u32, u16) { }
extern "C" void iopMemWrite32(u32, u32) { }
extern "C" u8  iopMemRead8_slow (u32) { return 0; }
extern "C" u16 iopMemRead16_slow(u32) { return 0; }
extern "C" u32 iopMemRead32_slow(u32) { return 0; }
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;
extern "C" int path_is_directory(const char*) { return 0; }
#include "HostFS.h"
namespace FileSystem {
	bool FindFiles(const char*, const char*, unsigned,
	               std::vector<FILESYSTEM_FIND_DATA>*) { return false; }
}
extern "C" int64_t filestream_read_file(const char*, void**, int64_t*) { return 0; }
extern "C" {
	void* rinflate_new(void) { return 0; }
	void  rinflate_free(void*) { }
	void  rinflate_set_in(void*, const void*, unsigned) { }
	void  rinflate_set_out(void*, void*, unsigned) { }
	int   rinflate_process(void*, unsigned*, unsigned*) { return -1; }
	int   rinflate(void*, const void*, unsigned, void*, unsigned, unsigned*)
	{ return -1; }
}
namespace Path {
	std::string_view GetFileName(const std::string_view& p) { return p; }
}

int lrps2_ingame_patches(const char *serial, u32 game_crc,
		const char *renderer, bool hint_nointerlacing,
		bool hint_disable_mipmaps, bool hint_game_enhancements,
		int8_t hint_widescreen, int8_t hint_uncapped_framerate,
		int8_t hint_language_unlock);

#define MAX_ITEMS 4096
#define MAX_TEXT  256

static int  failures;
#define FAIL(...) do { failures++; printf("  " __VA_ARGS__); } while (0)

/* What the source says: the serials and CRCs the branches test, and
 * the text of every message. */
static char s_serial[MAX_ITEMS][11];
static int  s_nserial;
static u32  s_crc[MAX_ITEMS];
static int  s_ncrc;
static char s_msg[MAX_ITEMS][MAX_TEXT];
static int  s_seen[MAX_ITEMS];
static int  s_nmsg;

/* What the current call did. */
static char s_game[MAX_TEXT];
static int  s_games;
static char s_call[96];

/* The one option the patches read, pcsx2_fastcdvd, is on. */
static bool test_environ(unsigned cmd, void *data)
{
	if (cmd != RETRO_ENVIRONMENT_GET_VARIABLE)
		return false;
	((struct retro_variable*)data)->value = "enabled";
	return true;
}
retro_environment_t environ_cb = test_environ;
void retro_set_region(unsigned) { }

/* "[PATCH] [Name (Region)]: ..." -> name folded to letters and digits,
 * so "Echo Night: Beyond" and "Echo Night - Beyond" are one game. */
static void game_of(const char *fmt, char *out)
{
	const char *p = fmt + strlen("[PATCH] [");
	const char *end = strstr(p, " (");
	size_t n = 0;

	if (!end)
		end = strchr(p, ']');
	if (!end)
		end = p + strlen(p);
	for (; p < end && n + 1 < MAX_TEXT; p++)
		if (isalnum((unsigned char)*p))
			out[n++] = (char)tolower((unsigned char)*p);
	out[n] = '\0';
}

static int s_badfmt[MAX_ITEMS];
static int s_twogames;

static void check_format(const char *fmt, int msg)
{
	const char *p;

	if (msg < 0 || s_badfmt[msg])
		return;

	for (p = fmt; (p = strchr(p, '%')) != NULL; )
	{
		if (p[1] == '%')
			p += 2;
		else if (!strncmp(p, "%08X", 4))
			p += 4;
		else
		{
			FAIL("a conversion with nothing to convert: %s", fmt);
			s_badfmt[msg] = 1;
			return;
		}
	}
}

static void test_log(enum retro_log_level level, const char *fmt, ...)
{
	char game[MAX_TEXT];
	int i, msg = -1;

	if (level == RETRO_LOG_ERROR)
	{
		FAIL("%s: a patch line was refused by the parser\n", s_call);
		return;
	}
	if (strncmp(fmt, "[PATCH] [", 9))
		return;
	for (i = 0; i < s_nmsg; i++)
		if (!strcmp(s_msg[i], fmt))
		{
			s_seen[i] = 1;
			msg = i;
		}
	check_format(fmt, msg);
	game_of(fmt, game);
	if (s_games == 0)
		strcpy(s_game, game);
	else if (strcmp(s_game, game) && !s_twogames)
	{
		FAIL("%s: patches two games, %s and %s\n", s_call, s_game, game);
		s_twogames = 1;
	}
	s_games++;
}
extern "C" retro_log_printf_t log_cb = test_log;

static void add_serial(const char *s)
{
	int i;
	for (i = 0; i < s_nserial; i++)
		if (!strcmp(s_serial[i], s))
			return;
	if (s_nserial < MAX_ITEMS)
		strcpy(s_serial[s_nserial++], s);
}

static void add_crc(u32 c)
{
	int i;
	for (i = 0; i < s_ncrc; i++)
		if (s_crc[i] == c)
			return;
	if (s_ncrc < MAX_ITEMS)
		s_crc[s_ncrc++] = c;
}

static int read_source(const char *path)
{
	static const char log_open[] = "log_cb(RETRO_LOG_INFO, \"[PATCH] [";
	FILE *f = fopen(path, "r");
	char line[1024];

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
	{
		const char *p;

		/* Serials: "SXXX-12345" */
		for (p = line; (p = strchr(p, '"')) != NULL; p++)
			if (isupper((unsigned char)p[1]) && p[5] == '-' && p[11] == '"'
			 && isdigit((unsigned char)p[6]) && isdigit((unsigned char)p[10]))
			{
				char s[11];
				memcpy(s, p + 1, 10);
				s[10] = '\0';
				add_serial(s);
			}
		/* CRCs: game_crc == 0x12345678 */
		for (p = line; (p = strstr(p, "game_crc == 0x")) != NULL; p++)
			add_crc((u32)strtoul(p + strlen("game_crc == "), NULL, 16));
		/* Messages, unescaped as the compiler would. */
		if ((p = strstr(line, log_open)) != NULL && s_nmsg < MAX_ITEMS)
		{
			char *out = s_msg[s_nmsg];
			size_t n = 0;

			for (p += strlen("log_cb(RETRO_LOG_INFO, \""); *p && *p != '"' && n + 1 < MAX_TEXT; p++)
			{
				if (*p == '\\' && p[1] == 'n') { out[n++] = '\n'; p++; }
				else if (*p == '\\' && p[1]) { out[n++] = p[1]; p++; }
				else out[n++] = *p;
			}
			out[n] = '\0';
			s_nmsg++;
		}
	}
	fclose(f);
	return 1;
}

int main(int argc, char **argv)
{
	/* The renderers the patches tell apart, and one they do not. */
	static const char *const renderers[] = {
		"Auto", "paraLLEl-GS", "Software", "Software (HW)", "Software (SW)"
	};
	const char *path = argc > 1 ? argv[1] : "libretro/patches.cpp";
	int s, c, r, ws, fps, i, calls = 0, dead = 0;

	if (!read_source(path))
	{
		fprintf(stderr, "cannot open %s\n", path);
		return 2;
	}
	add_crc(0);

	for (s = 0; s < s_nserial; s++)
		for (c = 0; c < s_ncrc; c++)
			for (r = 0; r < (int)(sizeof(renderers) / sizeof(renderers[0])); r++)
				for (ws = 1; ws <= 4; ws++)
					for (fps = 1; fps <= 2; fps++)
					{
						sprintf(s_call, "%s %08X %s ws%d fps%d", s_serial[s],
							(unsigned)s_crc[c], renderers[r], ws, fps);
						s_games = 0;
						if (c == 0 && r == 0 && ws == 1 && fps == 1)
							s_twogames = 0;
						lrps2_ingame_patches(s_serial[s], s_crc[c], renderers[r],
							true, true, true, (int8_t)ws, (int8_t)fps, 1);
						Patch.clear();
						calls++;
					}

	for (i = 0; i < s_nmsg; i++)
		if (!s_seen[i])
		{
			FAIL("never reached: %s", s_msg[i]);
			dead++;
		}

	printf("ingame: %d serials, %d CRCs, %d calls, %d of %d patch blocks reached\n",
	       s_nserial, s_ncrc, calls, s_nmsg - dead, s_nmsg);
	if (s_nserial == 0 || s_nmsg == 0)
		failures++;
	printf(failures ? "ingame: FAILED (%d)\n" : "ingame: ok\n", failures);
	return failures != 0;
}
