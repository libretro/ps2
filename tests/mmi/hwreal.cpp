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
	void PABSW(); void PABSH(); void PADSBH();
	void PEXCH(); void PEXCW(); void PEXEH(); void PEXEW();
	void PEXT5(); void PINTEH(); void PINTH(); void PLZCW(); void PROT3W();
	void PSLLH(); void PSLLW(); void PSRAH(); void PSRAW();
	void PSRLH(); void PSRLW();
	void PSLLVW(); void PSRLVW(); void PSRAVW();
	void PMADDW(); void PMSUBW(); void PMADDUW();
	void PMULTW(); void PMULTUW();
	void PMADDH(); void PMSUBH(); void PMULTH();
	void PHMADH(); void PHMSBH();
	void PDIVW(); void PDIVUW(); void PDIVBW();
	void PMFHI(); void PMFLO(); void PMTHI(); void PMTLO();
} } } }
using namespace R5900::Interpreter::OpcodeImpl::MMI;

/* How a line's operands map onto the instruction word. TWO is the usual
 * rs,rt pair; ONE has a single source, which MMI.cpp reads from rt; IMM
 * takes a shift amount in sa rather than a second register; VSHIFT is the
 * variable-shift trio, which assemble as rd, rt, rs -- value first,
 * amount second -- so the operands go in swapped. */
enum { A_TWO, A_ONE, A_IMM, A_VSHIFT, A_HILO, A_HILO1, A_HILO_TO };

static const struct { const char* name; const char* file; void (*fn)(); int form; }
kOps[] = {
	{ "paddw","arithmetic",PADDW,A_TWO },{ "psubw","arithmetic",PSUBW,A_TWO },
	{ "pcgtw","compare",PCGTW,A_TWO },{ "pmaxw","arithmetic",PMAXW,A_TWO },
	{ "paddh","arithmetic",PADDH,A_TWO },{ "psubh","arithmetic",PSUBH,A_TWO },
	{ "pcgth","compare",PCGTH,A_TWO },{ "pmaxh","arithmetic",PMAXH,A_TWO },
	{ "paddb","arithmetic",PADDB,A_TWO },{ "psubb","arithmetic",PSUBB,A_TWO },
	{ "pcgtb","compare",PCGTB,A_TWO },
	{ "paddsw","arithmetic",PADDSW,A_TWO },{ "psubsw","arithmetic",PSUBSW,A_TWO },
	{ "paddsh","arithmetic",PADDSH,A_TWO },{ "psubsh","arithmetic",PSUBSH,A_TWO },
	{ "paddsb","arithmetic",PADDSB,A_TWO },{ "psubsb","arithmetic",PSUBSB,A_TWO },
	{ "padduw","arithmetic",PADDUW,A_TWO },{ "psubuw","arithmetic",PSUBUW,A_TWO },
	{ "padduh","arithmetic",PADDUH,A_TWO },{ "psubuh","arithmetic",PSUBUH,A_TWO },
	{ "paddub","arithmetic",PADDUB,A_TWO },{ "psubub","arithmetic",PSUBUB,A_TWO },
	{ "pand","logic",PAND,A_TWO },{ "por","logic",POR,A_TWO },
	{ "pxor","logic",PXOR,A_TWO },{ "pnor","logic",PNOR,A_TWO },
	{ "pceqw","compare",PCEQW,A_TWO },{ "pceqh","compare",PCEQH,A_TWO },
	{ "pceqb","compare",PCEQB,A_TWO },
	{ "pminh","arithmetic",PMINH,A_TWO },{ "pminw","arithmetic",PMINW,A_TWO },
	{ "pextlw","shuffle",PEXTLW,A_TWO },{ "pextlh","shuffle",PEXTLH,A_TWO },
	{ "pextlb","shuffle",PEXTLB,A_TWO },
	{ "pextuw","shuffle",PEXTUW,A_TWO },{ "pextuh","shuffle",PEXTUH,A_TWO },
	{ "pextub","shuffle",PEXTUB,A_TWO },
	{ "pcpyld","shuffle",PCPYLD,A_TWO },{ "pcpyud","shuffle",PCPYUD,A_TWO },
	{ "pcpyh","shuffle",PCPYH,A_TWO },{ "prevh","shuffle",PREVH,A_TWO },
	{ "ppacw","shuffle",PPACW,A_TWO },{ "ppach","shuffle",PPACH,A_TWO },
	{ "ppacb","shuffle",PPACB,A_TWO },
	{ "pabsw","arithmetic",PABSW,A_ONE },{ "pabsh","arithmetic",PABSH,A_ONE },
	{ "padsbh","arithmetic",PADSBH,A_TWO },
	{ "pexch","shuffle",PEXCH,A_ONE },{ "pexcw","shuffle",PEXCW,A_ONE },
	{ "pexeh","shuffle",PEXEH,A_ONE },{ "pexew","shuffle",PEXEW,A_ONE },
	{ "pext5","shuffle",PEXT5,A_ONE },{ "prot3w","shuffle",PROT3W,A_ONE },
	{ "pinteh","shuffle",PINTEH,A_TWO },{ "pinth","shuffle",PINTH,A_TWO },
	{ "plzcw","logic",PLZCW,A_ONE },
	{ "psllh","logic",PSLLH,A_IMM },{ "psrlh","logic",PSRLH,A_IMM },
	{ "psrah","logic",PSRAH,A_IMM },{ "psllw","logic",PSLLW,A_IMM },
	{ "psrlw","logic",PSRLW,A_IMM },{ "psraw","logic",PSRAW,A_IMM },
	{ "psllvw","logic",PSLLVW,A_VSHIFT },{ "psrlvw","logic",PSRLVW,A_VSHIFT },
	{ "psravw","logic",PSRAVW,A_VSHIFT },
	/* The accumulators and the HI/LO moves. These read and write HI and LO
	 * as well as rd, and the capture prints all three, so the seed named on
	 * each line has to be applied before the op runs. This is the family
	 * the PMADDW errata bug lived in, which is reason enough to drive it
	 * through the real code rather than a copy. */
	{ "pmaddw","muldiv",PMADDW,A_HILO },{ "pmsubw","muldiv",PMSUBW,A_HILO },
	{ "pmadduw","muldiv",PMADDUW,A_HILO },
	{ "pmultw","muldiv",PMULTW,A_HILO },{ "pmultuw","muldiv",PMULTUW,A_HILO },
	{ "pmaddh","muldiv",PMADDH,A_HILO },{ "pmsubh","muldiv",PMSUBH,A_HILO },
	{ "pmulth","muldiv",PMULTH,A_HILO },
	{ "phmadh","muldiv",PHMADH,A_HILO },{ "phmsbh","muldiv",PHMSBH,A_HILO },
	{ "pdivw","muldiv",PDIVW,A_HILO },{ "pdivuw","muldiv",PDIVUW,A_HILO },
	{ "pdivbw","muldiv",PDIVBW,A_HILO },
	{ "pmfhi","muldiv",PMFHI,A_HILO1 },{ "pmflo","muldiv",PMFLO,A_HILO1 },
	/* PMTHI and PMTLO write HI or LO and leave rd alone, so the quad their
	 * lines print is the source register, not a result. Comparing it as rd
	 * scores 1 of 28 while the HI/LO values it is actually testing match
	 * on every case. */
	{ "pmthi","muldiv",PMTHI,A_HILO_TO },{ "pmtlo","muldiv",PMTLO,A_HILO_TO },
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
	/* Declared in muldiv.cpp rather than shared.h, so easy to miss. */
	{ "C_TWOTWOTWOTWO", {{2u,2u,2u,2u}} },
	{ "C_ONEONEONEONE", {{1u,1u,1u,1u}} },
};

/* The three HILOREGS the muldiv harness seeds: { hi, lo, hi1, lo1 }. */
static const struct { const char* name; u64 hi, lo, hi1, lo1; } kSeed[] = {
	{ "C_HILO_PATTERN", 0x0000000000000000ull, 0x0000000010001234ull,
	                    0x0000000000000000ull, 0x00000000F000ABCDull },
	{ "C_HILO_ZERO",    0, 0, 0, 0 },
	{ "C_HILO",         0x0123456789ABCDEFull, 0x123456789ABCDEF0ull,
	                    0x23456789ABCDEF01ull, 0x456789ABCDEF0123ull },
};

static int seed_hilo(const char* line)
{
	size_t i;
	for (i = 0; i < sizeof(kSeed)/sizeof(kSeed[0]); i++)
	{
		if (!strstr(line, kSeed[i].name)) continue;
		cpuRegs.HI.UD[0] = kSeed[i].hi;  cpuRegs.HI.UD[1] = kSeed[i].hi1;
		cpuRegs.LO.UD[0] = kSeed[i].lo;  cpuRegs.LO.UD[1] = kSeed[i].lo1;
		return 1;
	}
	/* Lines with no seed named use C_HILO, which the harness sets once. */
	cpuRegs.HI.UD[0] = kSeed[2].hi;  cpuRegs.HI.UD[1] = kSeed[2].hi1;
	cpuRegs.LO.UD[0] = kSeed[2].lo;  cpuRegs.LO.UD[1] = kSeed[2].lo1;
	return 1;
}

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
			Q rs, rt, rd; unsigned w3, w2, w1, w0; u32 sa;
			unsigned long long whi = 0, whi1 = 0, wlo = 0, wlo1 = 0;
			int has_rd = 0;
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
			if (kOps[op].form >= A_HILO)
			{
				/* "<op> <ops>: [rd rd rd rd ]H: hi hi1 L: lo lo1" -- some of
				 * these print rd and some do not, so the H: marker decides. */
				const char* h = strstr(colon, "H: ");
				if (!h) continue;
				has_rd = (kOps[op].form != A_HILO_TO)
				      && (sscanf(colon + 2, "%x %x %x %x", &w3, &w2, &w1, &w0) == 4);
				if (sscanf(h, "H: %llx %llx L: %llx %llx",
				           &whi, &whi1, &wlo, &wlo1) != 4) continue;
			}
			else if (sscanf(colon + 2, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4)
				continue;
			args = buf + 2 + strlen(kOps[op].name);
			*colon = '\0';
			sa = 0;
			comma = strchr(args, ',');
			if (kOps[op].form == A_HILO1 || kOps[op].form == A_HILO_TO)
			{
				/* PMFHI and PMFLO print no operand, PMTHI and PMTLO one. */
				int dummy = 0;
				rs.w[0] = rs.w[1] = rs.w[2] = rs.w[3] = 0;
				if (!operand(args, &rs, &dummy))
					rs.w[0] = rs.w[1] = rs.w[2] = rs.w[3] = 0;
				rt = rs;
				named_s = named_t = (strstr(args, "C_") != NULL);
			}
			else if (kOps[op].form == A_ONE)
			{
				/* Single source: MMI.cpp reads it from rt. */
				if (comma) { *colon = ':'; continue; }
				if (!operand(args, &rt, &named_t)) { *colon = ':'; continue; }
				rs = rt; named_s = named_t;
			}
			else
			{
				if (!comma) { *colon = ':'; continue; }
				*comma = '\0';
				if (!operand(args, &rs, &named_s)) { *colon = ':'; continue; }
				if (kOps[op].form == A_IMM)
				{
					/* Second operand is a shift amount, and the value being
					 * shifted is the first one, which MMI.cpp reads from rt. */
					char* q = comma + 1;
					while (*q == ' ') q++;
					sa = (u32)strtoul(q, NULL, 10) & 0x1F;
					rt = rs;
				}
				else if (!operand(comma + 1, &rt, &named_t))
				{ *colon = ':'; continue; }
				if (kOps[op].form == A_VSHIFT)
				{ const Q t = rs; rs = rt; rt = t; }
			}
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
			{
				/* The decimal cases seed rd into both doublewords, the same
				 * way they seed the operands. Setting only the low word
				 * leaves the upper half holding whatever the previous case
				 * left, which PLZCW exposes: it writes two words and leaves
				 * the rest of rd standing. */
				const Q seed = {{0x1337u, 0u, 0x1337u, 0u}};
				set_reg(RD, &seed);
			}

			if (kOps[op].form >= A_HILO)
				seed_hilo(buf);
			set_reg(RS, &rs);
			set_reg(RT, &rt);
			/* SPECIAL-form encoding: rs at 21, rt at 16, rd at 11. */
			cpuRegs.code = ((u32)RS << 21) | ((u32)RT << 16) | ((u32)RD << 11)
			             | (sa << 6);
			kOps[op].fn();
			get_reg(RD, &rd);

			cases++;
			if (kOps[op].form >= A_HILO)
			{
				const int hilo_ok = cpuRegs.HI.UD[0] == whi && cpuRegs.HI.UD[1] == whi1
				                 && cpuRegs.LO.UD[0] == wlo && cpuRegs.LO.UD[1] == wlo1;
				const int rd_ok = !has_rd
				               || (rd.w[0] == w0 && rd.w[1] == w1
				                && rd.w[2] == w2 && rd.w[3] == w3);
				if (hilo_ok && rd_ok) pass++;
				else if (failures++ < 6)
					printf("  %-7s console H %016llx %016llx L %016llx %016llx\n"
					       "          ours    H %016llx %016llx L %016llx %016llx\n",
					       kOps[op].name, whi, whi1, wlo, wlo1,
					       (unsigned long long)cpuRegs.HI.UD[0],
					       (unsigned long long)cpuRegs.HI.UD[1],
					       (unsigned long long)cpuRegs.LO.UD[0],
					       (unsigned long long)cpuRegs.LO.UD[1]);
			}
			else if (rd.w[0] == w0 && rd.w[1] == w1 && rd.w[2] == w2 && rd.w[3] == w3)
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
