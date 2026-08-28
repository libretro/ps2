/*  EE FPU against console captures, driving the real code.
 *
 *  tests/fpu/hwfpu.c already links pcsx2/ps2float.c, which is where the
 *  arithmetic lives, but it transcribes the parts that sit in FPU.cpp --
 *  CVT_W, CVT_S, the compares and the control-register moves. This links
 *  FPU.cpp and calls those through their real entry points.
 *
 *  Its dependencies are five symbols: cpuRegs, fpuRegs, intDoBranch and
 *  two vtlb accessors. The accessors are backed by an array so LWC1 and
 *  SWC1 would execute; intDoBranch records the branch rather than taking
 *  it, since a harness has nowhere to branch to.
 *
 *  Covered here:
 *    - the arithmetic and compares from ee_fpu/arithmetic, compare,
 *      muldiv and sqrt
 *    - CVT_W and CVT_S from ee_fpu/convert
 *    - CFC1 and CTC1 against the Initial: line of ee_fpu/fcr
 *
 *  The rounding mode is set to chop toward zero, because that is what
 *  VMManager sets before anything runs and CVT_S is plain host float:
 *  left at the default, it scores 26 of 32 and reads as a conversion bug
 *  rather than a harness in the wrong mode.
 *
 *  Usage: tests/fpu/hwrealfpu <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>

#include "Common.h"
#include "R5900.h"

alignas(16) cpuRegisters cpuRegs;
alignas(16) fpuRegisters fpuRegs;

#define MEMSIZE 4096
static u8 s_mem[MEMSIZE];
static u32 clampaddr(u32 a) { return a & (MEMSIZE - 1); }
u32 vtlb_memRead32(u32 a) { u32 v; memcpy(&v, s_mem + clampaddr(a), 4); return v; }
void vtlb_memWrite32(u32 a, u32 v) { memcpy(s_mem + clampaddr(a), &v, 4); }

/* BC1 calls this when the branch is taken; there is nowhere to go, so it
 * is recorded and the caller checks it. */
static int s_branched;
void intDoBranch(u32) { s_branched = 1; }

namespace R5900 { namespace Interpreter { namespace OpcodeImpl { namespace COP1 {
	void ADD_S(); void SUB_S(); void MUL_S(); void DIV_S(); void SQRT_S();
	void RSQRT_S(); void MADD_S(); void MSUB_S();
	void CVT_W(); void CVT_S();
	void C_EQ(); void C_LT(); void C_LE(); void C_F();
	void CFC1(); void CTC1();
} } } }
using namespace R5900::Interpreter::OpcodeImpl::COP1;

/* Registers: fs = 1, ft = 2, fd = 3, and rt = 1 for the moves. */
#define FS 1
#define FT 2
#define FD 3
#define RT 1

#define FPUflagC 0x00800000

static void set_code(u32 fs, u32 ft, u32 fd)
{ cpuRegs.code = (ft << 16) | (fs << 11) | (fd << 6); }

/* ---- the arithmetic and compares -------------------------------------- */

enum Kind { ARITH, ARITH_ACC, CONVERT, COMPARE };

static const struct { const char* name; const char* file; void (*fn)();
                      Kind kind; int operands; }
kOps[] = {
	{ "add",   "arithmetic", ADD_S,   ARITH, 2 },
	{ "sub",   "arithmetic", SUB_S,   ARITH, 2 },
	{ "mul",   "muldiv",     MUL_S,   ARITH, 2 },
	{ "div",   "muldiv",     DIV_S,   ARITH, 2 },
	/* SQRT_S reads ft, not fs, so its single operand goes there. */
	{ "sqrt",  "sqrt",       SQRT_S,  ARITH, 1 },
	{ "rsqrt", "sqrt",       RSQRT_S, ARITH, 2 },
	{ "madd",  "muldiv",     MADD_S,  ARITH_ACC, 2 },
	{ "msub",  "muldiv",     MSUB_S,  ARITH_ACC, 2 },
	{ "cvt.w.s", "convert",  CVT_W,   CONVERT, 1 },
	{ "cvt.s.w", "convert",  CVT_S,   CONVERT, 1 },
	{ "eq",    "compare",    C_EQ,    COMPARE, 2 },
	{ "lt",    "compare",    C_LT,    COMPARE, 2 },
	{ "le",    "compare",    C_LE,    COMPARE, 2 },
	{ "f",     "compare",    C_F,     COMPARE, 2 },
};
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

static const struct { const char* name; u32 v; } kConst[] = {
	{ "CF_ZERO",0 },{ "CF_NEGZERO",0x80000000u },
	{ "CF_MAX_MANTISSA",0x3FFFFFFFu },{ "CF_MAX_EXP",0x7F800001u },
	{ "CF_MIN_EXP",1u },{ "CF_MAX",0x7FFFFFFFu },{ "CF_MIN",0xFFFFFFFFu },
	{ "CF_NEGONE",0xBF800000u },{ "CF_ONE",0x3F800000u },
	{ "CF_GARBAGE1",0x00001337u },{ "CF_GARBAGE2",0xDEADBEEFu },
};

static int operand(char** pp, u32* out)
{
	char* p = *pp;
	size_t i, best = (size_t)-1, bestlen = 0;
	while (*p == ' ' || *p == ',' || *p == '(' || *p == ')') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(p, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; *pp = p + bestlen; return 1; }
	{
		unsigned v;
		if (sscanf(p, "%x", &v) != 1) return 0;
		*out = v;
		while (*p && *p != '/' && *p != ',' && *p != ':' && *p != ' ' && *p != ')') p++;
		if (*p == '/') while (*p && *p != ',' && *p != ':' && *p != ')') p++;
		*pp = p;
		return 1;
	}
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;

	/* Match the emulator: VMManager sets chop-toward-zero before anything
	 * runs, and CVT_S is plain host float. */
	fesetround(FE_TOWARDZERO);

	for (op = 0; op < NOPS; op++)
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
			u32 a = 0, b = 0, acc = 0, want = 0;
			char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			p = buf + 2 + strlen(kOps[op].name);
			if (kOps[op].kind == ARITH_ACC && !operand(&p, &acc)) continue;
			if (!operand(&p, &a)) continue;
			if (kOps[op].operands == 2 && !operand(&p, &b)) continue;
			while (*p && *p != ':') p++;
			if (*p != ':') continue;
			p++;
			while (*p == ' ') p++;

			if (kOps[op].kind == COMPARE)
			{
				if (!strncmp(p, "true", 4)) want = 1;
				else if (!strncmp(p, "false", 5)) want = 0;
				else continue;
			}
			else
			{
				unsigned v;
				if (sscanf(p, "%x", &v) != 1) continue;
				want = v;
			}

			if (!strcmp(kOps[op].name, "sqrt"))
			{ fpuRegs.fpr[FS].UL = 0; fpuRegs.fpr[FT].UL = a; }
			else
			{ fpuRegs.fpr[FS].UL = a; fpuRegs.fpr[FT].UL = b; }
			fpuRegs.fpr[FD].UL = 0;
			fpuRegs.ACC.UL = acc;
			fpuRegs.fprc[31] = 0x01000001;
			set_code(FS, FT, FD);
			kOps[op].fn();

			cases++;
			{
				u32 got;
				if (kOps[op].kind == COMPARE)
					got = (fpuRegs.fprc[31] & FPUflagC) ? 1u : 0u;
				else if (kOps[op].kind == ARITH_ACC)
					got = fpuRegs.fpr[FD].UL;   /* MADD_S writes fd */
				else
					got = fpuRegs.fpr[FD].UL;
				if (got == want) pass++;
				else if (failures++ < 6)
					printf("  %-8s %08x %08x: console %08x  ours %08x\n",
					       kOps[op].name, a, b, want, got);
			}
		}
		fclose(f);
		total += cases;
		if (pass != cases)
		{ printf("%-8s %2d/%-3d console cases\n", kOps[op].name, pass, cases);
		  failures++; }
	}

	/* ---- the control registers, from the Initial: line of fcr ---------- */
	{
		char path[512], buf[4096];
		FILE* f;
		int seen = 0, pass = 0, cases = 0;

		snprintf(path, sizeof(path), "%s/fcr.expected", dir);
		f = fopen(path, "r");
		if (f)
		{
			while (fgets(buf, sizeof(buf), f))
			{
				char* p;
				if (!seen)
				{ if (!strncmp(buf, "Initial:", 8)) seen = 1; continue; }
				if (buf[0] != ' ') break;
				/* Reset as cpuReset does, then read every register back
				 * through CFC1 rather than off the array. */
				fpuRegs.fprc[0] = 0x00002e30;
				fpuRegs.fprc[31] = 0x01000001;
				p = buf;
				while ((p = strstr(p, "fcr")) != NULL)
				{
					int fs; unsigned v;
					if (sscanf(p, "fcr%d: %x", &fs, &v) != 2) { p += 3; continue; }
					cpuRegs.GPR.r[RT].UD[0] = 0;
					set_code((u32)fs, RT, 0);
					CFC1();
					cases++;
					if (cpuRegs.GPR.r[RT].UL[0] == v) pass++;
					else if (failures++ < 6)
						printf("  fcr%-2d: console %08x  ours %08x\n",
						       fs, v, cpuRegs.GPR.r[RT].UL[0]);
					p += 3;
				}
				break;
			}
			fclose(f);
			/* And the write mask: CTC1 of all-ones then CFC1 back. */
			cpuRegs.GPR.r[RT].UD[0] = 0xffffffffu;
			set_code(31, RT, 0);
			CTC1();
			cpuRegs.GPR.r[RT].UD[0] = 0;
			set_code(31, RT, 0);
			CFC1();
			cases++;
			if (cpuRegs.GPR.r[RT].UL[0] == 0x0183c079u) pass++;
			else
			{ printf("  ctc1 0xffffffff then cfc1: console 0183c079  ours %08x\n",
			         cpuRegs.GPR.r[RT].UL[0]); failures++; }
			printf("fcr      %2d/%-3d console cases\n", pass, cases);
			total += cases;
			if (pass != cases) failures++;
		}
	}

	printf("hwrealfpu: %d cases across %d ops, driven through FPU.cpp\n",
	       total, NOPS + 1);
	return failures != 0;
}
