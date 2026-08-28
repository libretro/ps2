/*  IOP integer multiply and divide against console captures.
 *
 *  Scores DIV, DIVU, MULT and MULTU against ps2autotests
 *  tests/cpu/iop/muldiv.expected, transcribed from
 *  pcsx2/R3000AOpcodeTables.cpp.
 *
 *  The IOP is an R3000A, so HI and LO are 32 bits rather than the EE's
 *  128, and the capture prints one word each instead of the EE's two
 *  doublewords. Otherwise this is the same shape as tests/ee/hwmuldiv.c.
 *
 *  Divide by zero is the part worth having. The R3000A leaves the
 *  dividend in HI and puts 1 or -1 in LO depending on its sign, unsigned
 *  division always giving -1, and the 0x80000000 / -1 case is fixed up by
 *  hand because the host divide instruction would trap on it. All three
 *  are exercised here.
 *
 *  Usage: tests/iop/hwmuldiv <path-to-iop-muldiv.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

enum { I_DIV, I_DIVU, I_MULT, I_MULTU, I_COUNT };
static const char* const kName[I_COUNT] = { "div", "divu", "mult", "multu" };

/* Constants the harness passes by name; the IOP set is 32-bit. */
static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",     0x00000000u }, { "C_S16_MAX",  0x00007FFFu },
	{ "C_S16_MIN",  0xFFFF8000u }, { "C_S32_MAX",  0x7FFFFFFFu },
	{ "C_S32_MIN",  0x80000000u }, { "C_NEGONE",   0xFFFFFFFFu },
	{ "C_ONE",      0x00000001u }, { "C_GARBAGE1", 0x00001337u },
	{ "C_GARBAGE2", 0xDEADBEEFu },
	/* The IOP tests ship without sources in the ps2autotests checkout, so
	 * these four are taken from the EE harness's shared.h, which uses the
	 * same names, reduced to the low word the R3000A sees. The capture
	 * confirms each of them rather than leaving it to trust: C_S64_MIN
	 * divided by itself takes the divide-by-zero path and yields
	 * H 0 / L ffffffff, which only holds if it is zero; C_S64_MAX divided
	 * by itself is 1, so it is not; and TWO over ONE is 2. */
	{ "C_S64_MAX",  0xFFFFFFFFu }, { "C_S64_MIN",  0x00000000u },
	{ "C_TWOTWOTWOTWO", 0x00000002u }, { "C_ONEONEONEONE", 0x00000001u },
};

static int operand(char** pp, u32* out)
{
	char* p = *pp;
	while (*p == ' ' || *p == ',') p++;
	if (*p == 'C' && p[1] == '_')
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
		long v; char* e;
		v = strtol(p, &e, 10);
		if (e == p) return 0;
		*out = (u32)v;
		p = e;
	}
	*pp = p;
	return 1;
}

/* Transcribed from R3000AOpcodeTables.cpp. */
static void apply(int op, u32 rs, u32 rt, u32* hi, u32* lo)
{
	switch (op)
	{
	case I_DIV:
		if (rt == 0)
		{ *lo = ((s32)rs < 0) ? 1u : 0xFFFFFFFFu; *hi = rs; }
		else if (rs == 0x80000000u && rt == 0xFFFFFFFFu)
		{ *lo = 0x80000000u; *hi = 0; }
		else
		{ *lo = (u32)((s32)rs / (s32)rt); *hi = (u32)((s32)rs % (s32)rt); }
		break;
	case I_DIVU:
		if (rt == 0)
		{ *lo = 0xFFFFFFFFu; *hi = rs; }
		else
		{ *lo = rs / rt; *hi = rs % rt; }
		break;
	case I_MULT:
	{
		const u64 res = (u64)((s64)(s32)rs * (s64)(s32)rt);
		*lo = (u32)res; *hi = (u32)(res >> 32);
		break;
	}
	default:
	{
		const u64 res = (u64)rs * (u64)rt;
		*lo = (u32)res; *hi = (u32)(res >> 32);
		break;
	}
	}
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "muldiv.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < I_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			u32 a, b, hi = 0, lo = 0;
			unsigned wh, wl;
			char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			p = buf + 2 + strlen(kName[op]);
			if (!operand(&p, &a)) continue;
			if (!operand(&p, &b)) continue;
			p = strchr(buf, ':');
			if (!p || sscanf(p + 1, " H: %x L: %x", &wh, &wl) != 2) continue;

			apply(op, a, b, &hi, &lo);
			cases++;
			if (hi == wh && lo == wl) pass++;
			else if (failures++ < 6)
				printf("  %-5s %08x, %08x: console H %08x L %08x  ours H %08x L %08x\n",
				       kName[op], a, b, wh, wl, hi, lo);
		}
		fclose(f);
		printf("%-5s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("iop hwmuldiv: %d cases across %d ops\n", total, I_COUNT);
	return failures != 0;
}
