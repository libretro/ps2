/*  Every patch line in the game database, through the real parser.
 *
 *  A pnach line the parser does not understand is rejected with a console
 *  error and nothing else happens: no failure, no test, and the game
 *  quietly misbehaves. That is how the "bytes" operand size went
 *  unimplemented while six WRC 4 serials carried patches written in it --
 *  the patches were in GameIndex.yaml, the parser refused them, and the
 *  only sign was a line in a log nobody reads.
 *
 *  So this feeds bin/resources/GameIndex.yaml's patch blocks to
 *  LoadPatchesFromString and requires that every line is accepted. It
 *  links pcsx2/Patch.cpp rather than reimplementing the format, so it
 *  cannot drift from what the emulator actually parses.
 *
 *  What it checks:
 *    - every "patch=" line loads,
 *    - the count loaded matches the count in the file, so a line that is
 *      silently dropped rather than refused is caught too,
 *    - author= and comment= lines are tolerated, since they share the
 *      blocks.
 *
 *  What it does not check: whether a patch is correct for its game -- that
 *  needs the game -- and any operand size the database does not currently
 *  use. Deleting "ledouble" from the parser leaves this at 854 of 854,
 *  because nothing in GameIndex.yaml is written in it. The test tracks
 *  what the database contains, so it catches a type going missing only
 *  once some entry depends on it, which is exactly when it starts to
 *  matter. Deleting "bytes" costs 12 lines and "extended" costs 28.
 *
 *  Usage: tests/patch/dbparse <path-to-GameIndex.yaml>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <vector>
#include <string>

#include "Common.h"
#include "Patch.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

/* Memory the patch application would reach. The parse path does not use
 * it, but _ApplyPatch does, and applying is how a bytes payload is
 * checked for having survived the parse. */
static u8 s_mem[0x10000];
static u32 clamp(u32 a) { return a & 0xFFFF; }
u8  vtlb_memRead8 (u32 a) { return s_mem[clamp(a)]; }
u16 vtlb_memRead16(u32 a) { u16 v; memcpy(&v, s_mem + clamp(a), 2); return v; }
u32 vtlb_memRead32(u32 a) { u32 v; memcpy(&v, s_mem + clamp(a), 4); return v; }
u64 vtlb_memRead64(u32 a) { u64 v; memcpy(&v, s_mem + clamp(a), 8); return v; }
void vtlb_memWrite8 (u32 a, u8  v) { s_mem[clamp(a)] = v; }
void vtlb_memWrite16(u32 a, u16 v) { memcpy(s_mem + clamp(a), &v, 2); }
void vtlb_memWrite32(u32 a, u32 v) { memcpy(s_mem + clamp(a), &v, 4); }
void vtlb_memWrite64(u32 a, u64 v) { memcpy(s_mem + clamp(a), &v, 8); }
void* vtlb_GetPhyPtr(u32 a) { return s_mem + clamp(a); }
RETURNS_R128 vtlb_memRead128(u32) { r128 v; memset(&v, 0, sizeof(v)); return v; }
void vtlb_memWrite128(u32, r128) { }
void iopMemWrite8 (u32 a, u8  v) { s_mem[clamp(a)] = v; }
void iopMemWrite16(u32, u16) { }
void iopMemWrite32(u32, u32) { }
u8  iopMemRead8_slow (u32) { return 0; }
u16 iopMemRead16_slow(u32) { return 0; }
u32 iopMemRead32_slow(u32) { return 0; }
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;

/* The loader's other surface; none of it is entered from a string. */
extern "C" int path_is_directory(const char*) { return 0; }
extern "C" {
	void* rinflate_new(void) { return 0; }
	void  rinflate_free(void*) { }
	void  rinflate_set_in(void*, const void*, unsigned) { }
	void  rinflate_set_out(void*, void*, unsigned) { }
	int   rinflate_process(void*, unsigned*, unsigned*) { return -1; }
	int   rinflate(void*, const void*, unsigned, void*, unsigned, unsigned*)
	{ return -1; }
}
namespace FileSystem {
	bool FindFiles(const char*, const char*, unsigned,
	               std::vector<FILESYSTEM_FIND_DATA>*) { return false; }
	std::optional<std::string> ReadFileToString(const char*)
	{ return std::nullopt; }
}
namespace Path {
	std::string_view GetFileName(const std::string_view& p) { return p; }
}

/* Errors are counted rather than printed one per line: a broken operand
 * size fails hundreds at once and the first few say everything. */
static int g_errors = 0;
static int g_shown  = 0;
bool IConsoleWriter::Error(const char* fmt, ...) const
{
	g_errors++;
	if (g_shown++ < 5)
	{
		va_list a;
		va_start(a, fmt);
		printf("  ");
		vprintf(fmt, a);
		printf("\n");
		va_end(a);
	}
	return true;
}
bool IConsoleWriter::WriteLn(const char*, ...) const { return true; }
IConsoleWriter Console;

extern std::vector<IniPatch> Patch;

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "bin/resources/GameIndex.yaml";
	FILE* f = fopen(path, "r");
	char buf[1024];
	std::string block;
	int in_content = 0, file_lines = 0, blocks = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	/* The patch text sits under "content: |-" as an indented literal
	 * block. Any line indented further than the key belongs to it; the
	 * first line that is not ends the block. */
	while (fgets(buf, sizeof(buf), f))
	{
		const char* p = buf;
		int indent = 0;
		while (*p == ' ') { p++; indent++; }

		if (strstr(buf, "content: |-"))
		{ in_content = 1; blocks++; continue; }

		if (in_content)
		{
			if (*p == '\n' || *p == '\0') continue;
			if (indent < 8) { in_content = 0; }
			else
			{
				if (!strncmp(p, "patch=", 6)) file_lines++;
				block += p;
				continue;
			}
		}
	}
	fclose(f);

	{
		const int loaded = LoadPatchesFromString(block.c_str());
		size_t i, applied = 0;

		printf("  %d patch lines in %d blocks, %d loaded, %d refused\n",
		       file_lines, blocks, loaded, g_errors);

		/* Applying them all is what proves a bytes payload survived the
		 * parse: a line that loaded but kept an empty payload writes
		 * nothing. */
		for (i = 0; i < Patch.size(); i++)
		{
			_ApplyPatch(&Patch[i]);
			if (Patch[i].type == 9 /* BYTES_T */ && Patch[i].bytes.empty())
			{
				printf("  a bytes patch at %08x parsed with no payload\n",
				       Patch[i].addr);
				g_errors++;
			}
			applied++;
		}

		if (loaded != file_lines)
		{
			printf("  %d lines in the file but %d loaded: some were dropped"
			       " without an error\n", file_lines, loaded);
			g_errors++;
		}

		printf("dbparse: %d/%d game database patch lines accepted\n",
		       loaded, file_lines);
		(void)applied;
	}

	return (g_errors != 0 || file_lines == 0);
}
