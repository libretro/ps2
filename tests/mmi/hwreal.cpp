/*  MMI against console captures, driving the real implementations.
 *
 *  tests/mmi/hwelem.c and hwpmfhl.c transcribe the semantics out of
 *  pcsx2/MMI.cpp and score the copy. That catches a wrong answer but not
 *  drift: if MMI.cpp changes, the transcription keeps passing.
 *
 *  This links MMI.cpp instead and calls the ops through their real entry
 *  points, building the instruction word so the _Rd_, _Rs_ and _Rt_
 *  macros decode the registers this harness intends. The file touches
 *  only cpuRegs, so a definition of it is the whole of what it needs.
 *
 *  It scores the same shuffle, arithmetic, compare and logic captures the
 *  transcribing tests do, so the two disagreeing is itself informative:
 *  it means the transcription has fallen behind the code.
 *
 *  Being real code, it is also worth pointing a sanitizer at, which is
 *  what prompted this -- UBSan over the transcribing tests only ever
 *  checked my own arithmetic.
 *
 *  Usage: tests/mmi/hwreal <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R5900.h"

alignas(16) cpuRegisters cpuRegs;

/* Registers this harness uses: rd = 1, rs = 2, rt = 3. */
#define RD 1
#define RS 2
#define RT 3

typedef struct { u32 w[4]; } Q;

static void set_reg(int r, const Q* q)
{
	cpuRegs.GPR.r[r].UL[0] = q->w[0]; cpuRegs.GPR.r[r].UL[1] = q->w[1];
	cpuRegs.GPR.r[r].UL[2] = q->w[2]; cpuRegs.GPR.r[r].UL[3] = q->w[3];
}
static void get_reg(int r, Q* q)
{
	q->w[0] = cpuRegs.GPR.r[r].UL[0]; q->w[1] = cpuRegs.GPR.r[r].UL[1];
	q->w[2] = cpuRegs.GPR.r[r].UL[2]; q->w[3] = cpuRegs.GPR.r[r].UL[3];
}

/* The ops under test, by their MMI.cpp entry points. */
namespace R5900 { namespace Interpreter { namespace OpcodeImpl { namespace MMI {
	void PADDW(); void PSUBW(); void PCGTW(); void PMAXW();
	void PADDH(); void PSUBH(); void PCGTH(); void PMAXH();
	void PADDB(); void PSUBB(); void PCGTB();
	void PADDSW(); void PSUBSW(); void PADDSH(); void PSUBSH();
	void PADDSB(); void PSUBSB();
	void PADDUW(); void PSUBUW(); void PADDUH(); void PSUBUH();
	void PADDUB(); void PSUBUB();
	void PAND(); void POR(); void PXOR(); void PNOR();
	void PCEQW(); void PCEQH(); void PCEQB();
	void PMINH(); void PMINW();
	void PEXTLW(); void PEXTLH(); void PEXTLB();
	void PEXTUW(); void PEXTUH(); void PEXTUB();
	void PCPYLD(); void PCPYUD(); void PCPYH(); void PREVH();
	void PPACW(); void PPACH(); void PPACB();
} } } }
using namespace R5900::Interpreter::OpcodeImpl::MMI;

static const struct { const char* name; const char* file; void (*fn)(); }
kOps[] = {
	{ "paddw","arithmetic",PADDW },{ "psubw","arithmetic",PSUBW },
	{ "pcgtw","compare",PCGTW },{ "pmaxw","arithmetic",PMAXW },
	{ "paddh","arithmetic",PADDH },{ "psubh","arithmetic",PSUBH },
	{ "pcgth","compare",PCGTH },{ "pmaxh","arithmetic",PMAXH },
	{ "paddb","arithmetic",PADDB },{ "psubb","arithmetic",PSUBB },
	{ "pcgtb","compare",PCGTB },
	{ "paddsw","arithmetic",PADDSW },{ "psubsw","arithmetic",PSUBSW },
	{ "paddsh","arithmetic",PADDSH },{ "psubsh","arithmetic",PSUBSH },
	{ "paddsb","arithmetic",PADDSB },{ "psubsb","arithmetic",PSUBSB },
	{ "padduw","arithmetic",PADDUW },{ "psubuw","arithmetic",PSUBUW },
	{ "padduh","arithmetic",PADDUH },{ "psubuh","arithmetic",PSUBUH },
	{ "paddub","arithmetic",PADDUB },{ "psubub","arithmetic",PSUBUB },
	{ "pand","logic",PAND },{ "por","logic",POR },
	{ "pxor","logic",PXOR },{ "pnor","logic",PNOR },
	{ "pceqw","compare",PCEQW },{ "pceqh","compare",PCEQH },
	{ "pceqb","compare",PCEQB },
	{ "pminh","arithmetic",PMINH },{ "pminw","arithmetic",PMINW },
	{ "pextlw","shuffle",PEXTLW },{ "pextlh","shuffle",PEXTLH },
	{ "pextlb","shuffle",PEXTLB },
	{ "pextuw","shuffle",PEXTUW },{ "pextuh","shuffle",PEXTUH },
	{ "pextub","shuffle",PEXTUB },
	{ "pcpyld","shuffle",PCPYLD },{ "pcpyud","shuffle",PCPYUD },
	{ "pcpyh","shuffle",PCPYH },{ "prevh","shuffle",PREVH },
	{ "ppacw","shuffle",PPACW },{ "ppach","shuffle",PPACH },
	{ "ppacb","shuffle",PPACB },
};
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

static const struct { const char* name; Q v; } kConst[] = {
	{ "C_ZERO",     {{0,0,0,0}} },
	{ "C_S16_MAX",  {{0x7FFFu,0,0,0}} },
	{ "C_S16_MIN",  {{0xFFFF8000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S32_MAX",  {{0x7FFFFFFFu,0,0,0}} },
	{ "C_S32_MIN",  {{0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_S64_MAX",  {{0xFFFFFFFFu,0x7FFFFFFFu,0,0}} },
	{ "C_S64_MIN",  {{0,0x80000000u,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_NEGONE",   {{0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu}} },
	{ "C_ONE",      {{1,0,0,0}} },
	{ "C_GARBAGE1", {{0x1337u,0x1338u,0x1339u,0x133Au}} },
	{ "C_GARBAGE2", {{0xDEADBEEFu,0xDEADBEEEu,0xDEADBEEDu,0xDEADBEECu}} },
	/* The packed saturation constants, as in hwelem.c. Missing them costs
	 * eight cases a block silently -- the parse just skips the line. */
	{ "C_P_S8_A",   {{0x80808080u,0xFFFFFFFFu,0x12345678u,0x7F7F7F7Fu}} },
	{ "C_P_S8_B",   {{0x80FF127Fu,0xFF807F34u,0x567F80FFu,0x7F78FF80u}} },
	{ "C_P_S16_A",  {{0x80008000u,0xFFFFFFFFu,0x12345678u,0x7FFF7FFFu}} },
	{ "C_P_S16_B",  {{0x8000FFFFu,0x12347FFFu,0xFFFF8000u,0x7FFF5678u}} },
	{ "C_P_S32_A",  {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_B",  {{0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0x12345678u}} },
	{ "C_P_S32_C",  {{0x80000000u,0xFFFFFFFFu,0x12345678u,0x7FFFFFFFu}} },
	{ "C_P_S32_D",  {{0x7FFFFFFFu,0x12345678u,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_E",  {{0x00000000u,0x7FFFFFFFu,0xFFFFFFFFu,0x80000000u}} },
	{ "C_P_S32_F",  {{0xFFFFFFFFu,0x80000000u,0x00000000u,0x7FFFFFFFu}} },
};

static int operand(const char* p, Q* out, int* named)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	while (*p == ' ') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(p, kConst[i].name, n) && n > bestlen)
		{ best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; *named = 1; return 1; }
	{
		/* The harness sets both doublewords, not just the low one: the
		 * capture shows a PADDW of 1 and 1 giving 1 in words 0 and 2. A
		 * model that fills only the low half scores 13 of 19 and looks
		 * like broken upper lanes. */
		char* e; long long v = strtoll(p, &e, 10);
		u32 w, sgn;
		if (e == p) return 0;
		w = (u32)v;
		sgn = (w & 0x80000000u) ? 0xFFFFFFFFu : 0u;
		out->w[0] = w; out->w[1] = sgn; out->w[2] = w; out->w[3] = sgn;
		*named = 0;
		return 1;
	}
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;

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
			Q rs, rt, rd; unsigned w3, w2, w1, w0;
			int named_s = 0, named_t = 0;
			char* colon; char* args; char* comma;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;
			args = buf + 2 + strlen(kOps[op].name);
			*colon = '\0';
			comma = strchr(args, ',');
			if (!comma) { *colon = ':'; continue; }
			*comma = '\0';
			if (!operand(args, &rs, &named_s)) { *colon = ':'; continue; }
			if (!operand(comma + 1, &rt, &named_t)) { *colon = ':'; continue; }
			*colon = ':';

			/* rd carries state between cases exactly as in hwelem: the
			 * decimal cases rewrite only its low word, the named ones all
			 * 128 bits from C_GARBAGE1. Ops that write every lane do not
			 * care, but PCPYH and the like leave parts of rd standing. */
			if (named_s)
			{
				const Q g = {{0x1337u,0x1338u,0x1339u,0x133Au}};
				set_reg(RD, &g);
			}
			else
				cpuRegs.GPR.r[RD].UL[0] = 0x1337u;

			set_reg(RS, &rs);
			set_reg(RT, &rt);
			/* SPECIAL-form encoding: rs at 21, rt at 16, rd at 11. */
			cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | ((u32)RD << 11);
			kOps[op].fn();
			get_reg(RD, &rd);

			cases++;
			if (rd.w[0] == w0 && rd.w[1] == w1 && rd.w[2] == w2 && rd.w[3] == w3)
				pass++;
			else if (failures++ < 6)
				printf("  %-7s console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       kOps[op].name, w3, w2, w1, w0,
				       rd.w[3], rd.w[2], rd.w[1], rd.w[0]);
		}
		fclose(f);
		total += cases;
		if (pass != cases)
		{ printf("%-7s %2d/%-3d console cases\n", kOps[op].name, pass, cases);
		  failures++; }
	}

	printf("hwreal: %d cases across %d ops, driven through MMI.cpp\n", total, NOPS);
	return failures != 0;
}
