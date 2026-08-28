/*  VU EFU results against console captures.
 *
 *  Scores ESQRT, ERSQRT and ERCPR against ps2autotests
 *  tests/vu/lower/efu.expected.
 *
 *  These three go through ps2f_esqrt, ps2f_ersqrt and ps2f_ercpr in
 *  pcsx2/ps2float.c, and that file is standalone C, so this links the
 *  real implementation rather than transcribing it. Nothing here can
 *  drift from the emulator: if ps2float.c changes, this is testing the
 *  change.
 *
 *  The EFU tests are value tests despite living among the timing ones --
 *  the harness issues the op, then WAITP, then MFP, so the pipeline is
 *  drained before the result is read and only the arithmetic is being
 *  measured. The scalar ops read field Z of the source.
 *
 *  Not covered: the remaining EFU ops. ESADD, ERSADD, ELENG, ERLENG and
 *  ESUM are built from vuDouble and plain arithmetic inside VUops.cpp
 *  rather than ps2float.c, and EATAN, EEXP and ESIN are polynomial
 *  approximations there too, so scoring any of them means transcribing
 *  code instead of linking it. That is worth doing, but it is a different
 *  and more error-prone job than this one.
 *
 *  Usage: tests/vu/hwefu <path-to-efu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ps2float.h"

typedef uint32_t u32;

/* The constants efu.cpp loads, by the name the capture prints. Field Z is
 * what the scalar ops read, so only that lane is needed. */
static const struct { const char* name; u32 z; } kConst[] = {
	{ "CVF_ZERO",          0x00000000u },
	{ "CVF_NEGZERO",       0x80000000u },
	{ "CVF_MAX_MANTISSA",  0x3FFFFFFFu },
	{ "CVF_MAX_EXP",       0x7F800001u },
	{ "CVF_MIN_EXP",       0x00000001u },
	{ "CVF_MAX",           0x7FFFFFFFu },
	{ "CVF_MIN",           0xFFFFFFFFu },
	{ "CVF_ONE",           0x3F800000u },
	{ "CVF_NEGONE",        0xBF800000u },
	{ "CVF_GARBAGE1",      0x00001337u },
	{ "CVF_GARBAGE2",      0xDEADBEEFu },
	{ "CVF_INCREASING",    0x40400000u },
	{ "CVF_DECREASING",    0x40000000u },
};

static int lookup(const char* tok, u32* out)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen)
		{ best = i; bestlen = n; }
	}
	if (best == (size_t)-1) return 0;
	*out = kConst[best].z;
	return 1;
}

enum { E_ESQRT, E_ERSQRT, E_ERCPR, E_COUNT };
static const char* const kOpName[E_COUNT] = { "ESQRT", "ERSQRT", "ERCPR" };

static u32 apply(int op, u32 v)
{
	switch (op)
	{
	case E_ESQRT:  return ps2f_raw(ps2f_esqrt(v));
	case E_ERSQRT: return ps2f_raw(ps2f_ersqrt(v));
	case E_ERCPR:  return ps2f_raw(ps2f_ercpr(v));
	}
	return 0;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "efu.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < E_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOpName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			char *args, *colon;
			unsigned want;
			u32 in, got;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;

			args = buf + 2 + strlen(kOpName[op]);
			while (*args == ' ') args++;
			*colon = '\0';
			if (!lookup(args, &in)) { *colon = ':'; continue; }
			*colon = ':';

			got = apply(op, in);
			if (got == (u32)want) pass++;
			else if (failures++ < 8)
				printf("  %s %-18s console %08x  ours %08x\n",
				       kOpName[op], args, want, got);
			cases++;
		}
		fclose(f);
		printf("%-7s %2d/%2d console cases\n", kOpName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("vuefu: %d cases across %d ops\n", total, E_COUNT);
	return failures != 0;
}
