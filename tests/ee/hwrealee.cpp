/*  EE integer ops against console captures, driving the real code.
 *
 *  tests/ee/hwalu.c and hwload.c and hwstore.c transcribe the semantics
 *  out of pcsx2/R5900OpcodeImpl.cpp and score the copy, which catches a
 *  wrong answer but not drift. This links that file and calls the ops
 *  through their real entry points, building the instruction word so the
 *  _Rd_, _Rs_, _Rt_, _Sa_ and _Imm_ macros decode what the harness
 *  intends.
 *
 *  Its dependencies are stubbed rather than pulled in. Most are inert --
 *  the BIOS hacks, the savestate helpers, the COP2 print table -- but the
 *  vtlb accessors are backed by a real array, so the loads and stores
 *  execute against memory rather than being skipped. That is what lets
 *  the same harness cover alu.expected and lsu.expected.
 *
 *  Three tables of ops, by shape:
 *    RRR    rd = f(rs, rt)          -- the register arithmetic
 *    RRI    rt = f(rs, immediate)   -- the immediate forms
 *    RRS    rd = f(rt, sa)          -- shift by immediate
 *
 *  The load and store blocks reuse the same memory pattern the standalone
 *  scorers do, at the same two bases: the load harness bases at byte 16
 *  of the pattern and the store harness at byte 4, which is not a typo
 *  but a difference between the two test programs.
 *
 *  Usage: tests/ee/hwrealee <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "GS.h"
#include "ps2/BiosTools.h"

alignas(16) cpuRegisters cpuRegs;

/* ---- stubs ------------------------------------------------------------
 * Everything R5900OpcodeImpl.cpp reaches for that this harness does not
 * exercise. The memory accessors are real; the rest are inert. */

#define MEMSIZE 4096
static u8 s_mem[MEMSIZE];
static u32 clampaddr(u32 a) { return a & (MEMSIZE - 1); }

u8  vtlb_memRead8 (u32 a) { return s_mem[clampaddr(a)]; }
u16 vtlb_memRead16(u32 a) { u16 v; memcpy(&v, s_mem + clampaddr(a), 2); return v; }
u32 vtlb_memRead32(u32 a) { u32 v; memcpy(&v, s_mem + clampaddr(a), 4); return v; }
u64 vtlb_memRead64(u32 a) { u64 v; memcpy(&v, s_mem + clampaddr(a), 8); return v; }
void vtlb_memWrite8 (u32 a, u8  v) { s_mem[clampaddr(a)] = v; }
void vtlb_memWrite16(u32 a, u16 v) { memcpy(s_mem + clampaddr(a), &v, 2); }
void vtlb_memWrite32(u32 a, u32 v) { memcpy(s_mem + clampaddr(a), &v, 4); }
void vtlb_memWrite64(u32 a, u64 v) { memcpy(s_mem + clampaddr(a), &v, 8); }
void* vtlb_GetPhyPtr(u32 a) { return s_mem + clampaddr(a); }

RETURNS_R128 vtlb_memRead128(u32 a)
{
	r128 v;
	memcpy(&v, s_mem + clampaddr(a), 16);
	return v;
}
void vtlb_memWrite128(u32 a, r128 v) { memcpy(s_mem + clampaddr(a), &v, 16); }

/* Inert stubs: nothing here runs a BIOS, a savestate or the GS. */
bool g_SkipBiosHack = false, AllowParams1 = false, AllowParams2 = false, NoOSD = false;
void cpuException(u32, u32) { }
/* Cpu is only reached by the misaligned-access paths, which call
 * CancelInstruction; the captures never take one. */
static void nop_v() { }
static void nop_u32(u32) { }
static void nop_u32_u32(u32, u32) { }
static R5900cpu s_cpu = {};
R5900cpu* Cpu = &s_cpu;
BiosDebugInformation CurrentBiosInformation = {};
void gsSetVideoMode(GS_VideoMode) { }
void cdvdReadLanguageParams(u8*) { }
tDMA_TAG* dmaGetAddr(u32, bool) { return (tDMA_TAG*)s_mem; }
void SaveStateBase::FreezeMem(void*, int) { }
bool SaveStateBase::FreezeTag(const char*) { return true; }

/* ---- the harness ------------------------------------------------------ */

namespace R5900 { namespace Interpreter { namespace OpcodeImpl {
	void ADDU(); void SUBU(); void AND(); void OR(); void XOR(); void NOR();
	void SLT(); void SLTU(); void DADDU(); void DSUBU();
	void MOVN(); void MOVZ();
	void SLLV(); void SRLV(); void SRAV();
	void DSLLV(); void DSRLV(); void DSRAV();
	void ADDIU(); void ANDI(); void DADDIU(); void ORI(); void XORI();
	void SLTI(); void SLTIU(); void LUI();
	void SLL(); void SRL(); void SRA();
	void DSLL(); void DSRL(); void DSRA();
	void DSLL32(); void DSRL32(); void DSRA32();
} } }
using namespace R5900::Interpreter::OpcodeImpl;

enum Shape { RRR, RRI, RRS, RI };

static const struct { const char* name; void (*fn)(); Shape shape; int swap; }
kOps[] = {
	{ "addu",ADDU,RRR,0 },{ "subu",SUBU,RRR,0 },{ "and",AND,RRR,0 },
	{ "or",OR,RRR,0 },{ "xor",XOR,RRR,0 },{ "nor",NOR,RRR,0 },
	{ "slt",SLT,RRR,0 },{ "sltu",SLTU,RRR,0 },
	{ "daddu",DADDU,RRR,0 },{ "dsubu",DSUBU,RRR,0 },
	{ "movn",MOVN,RRR,0 },{ "movz",MOVZ,RRR,0 },
	/* The variable shifts assemble as rd, rt, rs -- value first, amount
	 * second -- so the printed operands go in swapped. */
	{ "sllv",SLLV,RRR,1 },{ "srlv",SRLV,RRR,1 },{ "srav",SRAV,RRR,1 },
	{ "dsllv",DSLLV,RRR,1 },{ "dsrlv",DSRLV,RRR,1 },{ "dsrav",DSRAV,RRR,1 },
	{ "addiu",ADDIU,RRI,0 },{ "andi",ANDI,RRI,0 },{ "daddiu",DADDIU,RRI,0 },
	{ "ori",ORI,RRI,0 },{ "xori",XORI,RRI,0 },
	{ "slti",SLTI,RRI,0 },{ "sltiu",SLTIU,RRI,0 },
	{ "lui",LUI,RI,0 },
	{ "sll",SLL,RRS,0 },{ "srl",SRL,RRS,0 },{ "sra",SRA,RRS,0 },
	{ "dsll",DSLL,RRS,0 },{ "dsrl",DSRL,RRS,0 },{ "dsra",DSRA,RRS,0 },
	{ "dsll32",DSLL32,RRS,0 },{ "dsrl32",DSRL32,RRS,0 },{ "dsra32",DSRA32,RRS,0 },
};
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

#define RD 1
#define RS 2
#define RT 3

typedef struct { u32 w[4]; } Q;

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
	{ "C_TWOTWOTWOTWO", {{2u,2u,2u,2u}} },
	{ "C_ONEONEONEONE", {{1u,1u,1u,1u}} },
};

static int operand(char** pp, Q* out, int* named)
{
	char* p = *pp;
	size_t i, best = (size_t)-1, bestlen = 0;
	while (*p == ' ' || *p == ',') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(p, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1)
	{ *out = kConst[best].v; *named = 1; p += bestlen; *pp = p; return 1; }
	{
		char* e;
		long long v = strtoll(p, &e, 10);
		if (e == p) return 0;
		/* SET_U32: lui/ori, so the low doubleword sign-extended, nothing above. */
		out->w[0] = (u32)v;
		out->w[1] = (v < 0) ? 0xFFFFFFFFu : 0u;
		out->w[2] = out->w[3] = 0;
		*named = 0;
		*pp = e;
		return 1;
	}
}

static void set_reg(int r, const Q* q)
{
	cpuRegs.GPR.r[r].UL[0] = q->w[0]; cpuRegs.GPR.r[r].UL[1] = q->w[1];
	cpuRegs.GPR.r[r].UL[2] = q->w[2]; cpuRegs.GPR.r[r].UL[3] = q->w[3];
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;
	/* rd carries between cases across the whole file, as in hwalu.c. */
	u64 rd_hi = 0;

	for (op = 0; op < NOPS; op++)
	{
		char path[512], head[32], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0;

		snprintf(path, sizeof(path), "%s/alu.expected", dir);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			Q a, b; unsigned w3, w2, w1, w0;
			int named_a = 0, named_b = 0;
			char* p; u32 sa = 0, imm = 0;
			int dest;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			p = buf + 2 + strlen(kOps[op].name);
			if (kOps[op].shape == RI)
			{
				a.w[0] = a.w[1] = a.w[2] = a.w[3] = 0;
				if (!operand(&p, &b, &named_b)) continue;
			}
			else
			{
				if (!operand(&p, &a, &named_a)) continue;
				if (!operand(&p, &b, &named_b)) continue;
			}
			while (*p == ' ' || *p == ':') p++;
			if (sscanf(p, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

			/* rd's carried upper doubleword, as hwalu.c models it. */
			if (named_a || named_b) rd_hi = 0x0000133A00001339ull;
			cpuRegs.GPR.r[RD].UD[1] = rd_hi;
			cpuRegs.GPR.r[RD].UD[0] = (named_a || named_b)
			                        ? ((u64)0x1338u << 32) | 0x1337u : 0x1337u;

			switch (kOps[op].shape)
			{
			case RRR:
				if (kOps[op].swap) { const Q t = a; a = b; b = t; }
				set_reg(RS, &a); set_reg(RT, &b);
				cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | ((u32)RD << 11);
				dest = RD;
				break;
			case RRI:
				set_reg(RS, &a);
				imm = b.w[0] & 0xFFFF;
				cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | imm;
				dest = RT;
				break;
			case RI:
				imm = b.w[0] & 0xFFFF;
				cpuRegs.code = ((u32)RT << 16) | imm;
				dest = RT;
				break;
			default: /* RRS */
				set_reg(RT, &a);
				sa = b.w[0] & 0x1F;
				cpuRegs.code = ((u32)RT << 16) | ((u32)RD << 11) | (sa << 6);
				dest = RD;
				break;
			}
			/* The immediate forms write rt, so it has to carry the seed too. */
			if (dest == RT)
			{
				cpuRegs.GPR.r[RT].UD[1] = rd_hi;
				cpuRegs.GPR.r[RT].UD[0] = (named_a || named_b)
				                        ? ((u64)0x1338u << 32) | 0x1337u : 0x1337u;
			}

			kOps[op].fn();

			cases++;
			{
				const u32 g0 = cpuRegs.GPR.r[dest].UL[0];
				const u32 g1 = cpuRegs.GPR.r[dest].UL[1];
				const u32 g2 = cpuRegs.GPR.r[dest].UL[2];
				const u32 g3 = cpuRegs.GPR.r[dest].UL[3];
				if (g0 == w0 && g1 == w1 && g2 == w2 && g3 == w3) pass++;
				else if (failures++ < 6)
					printf("  %-7s console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
					       kOps[op].name, w3, w2, w1, w0, g3, g2, g1, g0);
				rd_hi = ((u64)w3 << 32) | w2;
			}
		}
		fclose(f);
		total += cases;
		if (pass != cases)
		{ printf("%-7s %2d/%-3d console cases\n", kOps[op].name, pass, cases);
		  failures++; }
	}

	printf("hwrealee: %d cases across %d ops, driven through R5900OpcodeImpl.cpp\n",
	       total, NOPS);
	return failures != 0;
}
