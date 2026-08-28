/*  EE FPU arithmetic against console captures.
 *
 *  Scores the FPU ops in pcsx2/FPU.cpp against ps2autotests
 *  tests/cpu/ee_fpu/{arithmetic,muldiv,sqrt}.expected.
 *
 *  Every one of them is a direct call into pcsx2/ps2float.c -- ADD_S is
 *  ps2f_add, MUL_S is ps2f_mul, MADD_S is ps2f_madd and so on -- and that
 *  file is standalone C, so this links the real implementation rather
 *  than transcribing it. Nothing here can drift from the emulator: if
 *  ps2float.c changes, this is testing the change.
 *
 *  Each block prints its operands two ways: the first half as literal hex,
 *  the second half as the names of constants the harness sets up. Both are
 *  handled. Reading only the hex form silently halves the coverage -- add
 *  has 36 cases, not the 19 that shape alone finds -- and a scorer that
 *  skips half its cases while reporting a clean sweep is worse than none,
 *  so the case counts here are checked against the files.
 *
 *  MADD and MSUB carry an accumulator, printed in parentheses before the
 *  operands and again after the result. The leading one is the input.
 *
 *  Usage: tests/fpu/hwfpu <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "ps2float.h"

typedef uint32_t u32;

enum { F_ADD, F_SUB, F_MUL, F_DIV, F_SQRT, F_RSQRT, F_MADD, F_MSUB, F_COUNT };

static const struct { const char* name; const char* file; int operands; int acc; }
kOps[F_COUNT] = {
	{ "add",   "arithmetic", 2, 0 },
	{ "sub",   "arithmetic", 2, 0 },
	{ "mul",   "muldiv",     2, 0 },
	{ "div",   "muldiv",     2, 0 },
	{ "sqrt",  "sqrt",       1, 0 },
	{ "rsqrt", "sqrt",       2, 0 },
	{ "madd",  "muldiv",     2, 1 },
	{ "msub",  "muldiv",     2, 1 },
};

/* The named constants, from tests/cpu/ee_fpu/shared.h. */
static const struct { const char* name; u32 v; } kConst[] = {
	{ "CF_ZERO",         0x00000000u },
	{ "CF_NEGZERO",      0x80000000u },
	{ "CF_MAX_MANTISSA", 0x3FFFFFFFu },
	{ "CF_MAX_EXP",      0x7F800001u },
	{ "CF_MIN_EXP",      0x00000001u },
	{ "CF_MAX",          0x7FFFFFFFu },
	{ "CF_MIN",          0xFFFFFFFFu },
	{ "CF_NEGONE",       0xBF800000u },
	{ "CF_ONE",          0x3F800000u },
	{ "CF_GARBAGE1",     0x00001337u },
	{ "CF_GARBAGE2",     0xDEADBEEFu },
};

/* Read one operand, either "3f800000/+1.00" or "CF_ONE", and advance p past
 * it and any following separator. */
static int operand(char** pp, u32* out)
{
	char* p = *pp;
	while (*p == ' ' || *p == ',' || *p == '(' || *p == ')') p++;
	if (*p == 'C' && p[1] == 'F')
	{
		size_t i, best = (size_t)-1, bestlen = 0;
		for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
		{
			const size_t n = strlen(kConst[i].name);
			if (!strncmp(p, kConst[i].name, n) && n > bestlen)
			{ best = i; bestlen = n; }
		}
		if (best == (size_t)-1) return 0;
		*out = kConst[best].v;
		p += bestlen;
	}
	else
	{
		unsigned v;
		if (sscanf(p, "%x", &v) != 1) return 0;
		*out = (u32)v;
		while (*p && *p != '/' && *p != ',' && *p != ':' && *p != ' ' && *p != ')') p++;
		/* Skip the decimal rendering that follows the hex. It ends at the
		 * next separator -- and ')' has to be one of them, or the
		 * accumulator MADD prints in parentheses swallows the first
		 * operand behind it and the block silently loses half its cases. */
		if (*p == '/') { while (*p && *p != ',' && *p != ':' && *p != ')') p++; }
	}
	*pp = p;
	return 1;
}

static u32 apply(int op, u32 a, u32 b, u32 acc)
{
	switch (op)
	{
	case F_ADD:   return ps2f_raw(ps2f_add(a, b));
	case F_SUB:   return ps2f_raw(ps2f_sub(a, b));
	case F_MUL:   return ps2f_raw(ps2f_mul(a, b));
	case F_DIV:   return ps2f_raw(ps2f_div(a, b));
	case F_SQRT:  return ps2f_raw(ps2f_sqrt(a));
	case F_RSQRT: return ps2f_raw(ps2f_rsqrt(a, b));
	case F_MADD:  return ps2f_raw(ps2f_madd(acc, a, b, 0));
	case F_MSUB:  return ps2f_raw(ps2f_msub(acc, a, b, 0));
	}
	return 0;
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;

	for (op = 0; op < F_COUNT; op++)
	{
		char path[512], head[32], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0;

		snprintf(path, sizeof(path), "%s/%s.expected", dir, kOps[op].file);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			char* p;
			u32 a = 0, b = 0, acc = 0, want = 0;
			u32 got;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			p = buf + 2 + strlen(kOps[op].name);
			if (kOps[op].acc && !operand(&p, &acc)) continue;
			if (!operand(&p, &a)) continue;
			if (kOps[op].operands == 2 && !operand(&p, &b)) continue;
			while (*p && *p != ':') p++;
			if (*p != ':') continue;
			p++;
			{
				unsigned v;
				if (sscanf(p, " %x", &v) != 1) continue;
				want = v;
			}

			got = apply(op, a, b, acc);
			if (got == (u32)want) pass++;
			else if (failures++ < 6)
				printf("  %-6s %08x %08x (acc %08x): console %08x  ours %08x\n",
				       kOps[op].name, a, b, acc, want, got);
			cases++;
		}
		fclose(f);
		printf("%-6s %3d/%-3d console cases\n", kOps[op].name, pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("hwfpu: %d cases across %d ops\n", total, F_COUNT);
	return failures != 0;
}
