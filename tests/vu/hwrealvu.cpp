/*  VU EFU ops against console captures, driving the real code.
 *
 *  tests/vu/hwefu.c transcribes EATAN, ESIN and EEXP out of
 *  pcsx2/VUops.cpp and scores the copy; the three that go through
 *  ps2float.c it links for real. This links VUops.cpp itself and calls
 *  every one of them through the interpreter's own dispatch table.
 *
 *  Reaching them takes the instruction encoding rather than a function
 *  pointer, because the EFU entry points are static. VU0microInterp.cpp
 *  indexes VU0_LOWER_OPCODE with code >> 25, and the type-3 and type-4
 *  lower ops encode as bit 31 set, the source register at 11, the field
 *  selector at 21, and the opcode in the low ten bits -- the same layout
 *  ps2autotests' own assembler emits. So the harness builds the word and
 *  dispatches exactly as the interpreter does, which is a stronger check
 *  than calling a function directly: it exercises the table as well.
 *
 *  Usage: tests/vu/hwrealvu <path-to-vu/lower/efu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "VUops.h"
#include "VUmicro.h"

alignas(16) VURegs vuRegs[2];

/* VUops.cpp reaches for these on paths this harness never takes: XGKICK
 * and the MTVU handoff. The EFU ops touch none of it, so they are storage
 * and no-ops rather than behaviour. */
#include "Gif_Unit.h"
#include "MTVU.h"

alignas(16) cpuRegisters cpuRegs;
alignas(16) u8 eeHw[0x10000];
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");

/* vuDouble's clamp of a maximal exponent to the largest finite value is
 * gated on Cpu.Recompiler.vu0ExtraOverflow, which defaults off -- it is a
 * per-game fix, not the normal path. The zeroed EmuConfig above therefore
 * matches the default, and that is deliberate: this harness scores the
 * configuration almost every game runs under.
 *
 * It matters more than it looks. With the clamp off, CVF_MAX and CVF_MIN
 * reach the EFU as host NaNs and propagate, so EATAN returns its own
 * input. With it on they clamp and EATAN lands within an ULP -- but ESADD
 * drops from 11 of 16 to 10 and ESUM from 11 to 9, while ERSADD rises
 * from 8 to 11. Neither setting is uniformly closer to the console.
 *
 * This is also what tests/vu/hwefu.c's transcription differs by: it
 * hardcodes the clamp, so it is modelling the non-default configuration.
 * That, rather than any difference in the arithmetic, is why its EATAN
 * scores 8 of 13 where this scores 0 of 16. */
static void vu_config_is_default(void) { }
alignas(16) u8 s_gifunit[sizeof(Gif_Unit)];
asm(".globl gifUnit\n.set gifUnit, s_gifunit");
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");

void CPU_INT(EE_EventType, int) { }
void Gif_AddBlankGSPacket(u32, GIF_PATH) { }
void Gif_AddCompletedGSPacket(GS_Packet&, GIF_PATH) { }
void Gif_FinishIRQ() { }
bool Gif_HandlerAD(u8*) { return false; }
void Gif_HandlerAD_MTVU(u8*) { }
void Gif_MTVU_KickSema() { }
void gifCheckPathStatus(bool) { }
namespace MTGS { void WaitGS(bool) { } }

/* Lower-op encoding, as ps2autotests' assembler builds it. */
#define LOWER_TYPE1345 0x80000000u
#define VS(r)   ((u32)(r) << 11)
#define VT(r)   ((u32)(r) << 16)
#define DEST(d) ((u32)(d) << 21)
#define FSF(f)  ((u32)(f) << 21)

/* The EFU opcodes, from tests/vu/assemble/ops.cpp. */
enum {
	OP_ESADD  = 0x73C, OP_ERSADD = 0x73D, OP_ELENG = 0x73E, OP_ERLENG = 0x73F,
	OP_EATANxy = 0x77C, OP_EATANxz = 0x77D, OP_ESUM = 0x77E,
	/* Read from tests/vu/assemble/ops.cpp rather than guessed: these six
	 * sit in two adjacent groups and transposing them within a group still
	 * dispatches to a real EFU op, so the scores come out low but
	 * plausible rather than obviously broken. */
	OP_ESQRT  = 0x7BC, OP_ERSQRT = 0x7BD, OP_ERCPR = 0x7BE,
	OP_ESIN   = 0x7FC, OP_EATAN  = 0x7FD, OP_EEXP  = 0x7FE,
};

#define VF_SRC 1     /* the source register this harness loads */
#define VI_D   1     /* the integer destination and its two sources */
#define VI_S   2
#define VI_T   3

/* Lower type-1 puts the destination at bit 6; type-5 carries a signed
 * five-bit immediate in the same place; type-8 is a different shape
 * entirely, with the opcode at 25 and a fifteen-bit immediate split in
 * two -- eleven bits at 0 and four more at 21. */
#define VD(r)    ((u32)(r) << 6)
/* IMM5 sits at bit 6, not bit 0, where it would collide with the
 * six-bit opcode. */
#define IMM5(v)  (((u32)(v) & 0x1F) << 6)
#define IMM15(v) (((u32)(v) & 0x7FF) | (((u32)(v) & 0x7800) << 10))

enum {
	OP_IADD = 0x30, OP_ISUB = 0x31, OP_IADDI = 0x32,
	OP_IAND = 0x34, OP_IOR = 0x35,
	OP_IADDIU = 0x08, OP_ISUBIU = 0x09,
	OP_RNEXT = 0x43C, OP_RGET = 0x43D, OP_RINIT = 0x43E, OP_RXOR = 0x43F,
};

/* Floors, not targets. The three that go through ps2float.c are exact;
 * the polynomial ones are not, and the interpreter's own accuracy is what
 * tests/vu/hwefu.c already measures and documents. What this table adds is
 * a regression bound on the real code rather than on a copy of it.
 *
 * EATAN scoring 0 here against hwefu.c's 8 of 13 is a configuration
 * difference, not an arithmetic one: see the note on vuDouble's clamp
 * above. hwefu.c hardcodes the clamp and so models vu0ExtraOverflow being
 * on; this runs the default. */
static const struct { const char* name; u32 op; int vector; int floor; } kOps[] = {
	/* The scalar forms take one component through the fsf selector; the
	 * vector forms read x, y and z of the register. */
	{ "ESQRT",  OP_ESQRT,  0, 16 },
	{ "ERSQRT", OP_ERSQRT, 0, 16 },
	{ "ERCPR",  OP_ERCPR,  0, 16 },
	{ "EATAN",  OP_EATAN,  0,  0 },
	{ "ESIN",   OP_ESIN,   0,  8 },
	{ "EEXP",   OP_EEXP,   0,  9 },
	{ "ESADD",  OP_ESADD,  1, 11 },
	{ "ERSADD", OP_ERSADD, 1,  8 },
	{ "ELENG",  OP_ELENG,  1, 11 },
	{ "ERLENG", OP_ERLENG, 1,  5 },
	{ "ESUM",   OP_ESUM,   1, 11 },
};
#define NOPS ((int)(sizeof(kOps)/sizeof(kOps[0])))

/* The constants the efu harness sets up, scalar and vector. */
static const struct { const char* name; u32 x, y, z, w; } kConst[] = {
	{ "CVF_ZERO",         0,0,0,0 },
	{ "CVF_NEGZERO",      0x80000000u,0x80000000u,0x80000000u,0x80000000u },
	{ "CVF_MAX_MANTISSA", 0x3FFFFFFFu,0x3FFFFFFFu,0x3FFFFFFFu,0x3FFFFFFFu },
	{ "CVF_MAX_EXP",      0x7F800001u,0x7F800001u,0x7F800001u,0x7F800001u },
	{ "CVF_MIN_EXP",      1,1,1,1 },
	{ "CVF_MAX",          0x7FFFFFFFu,0x7FFFFFFFu,0x7FFFFFFFu,0x7FFFFFFFu },
	{ "CVF_MIN",          0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu },
	{ "CVF_ONE",          0x3F800000u,0x3F800000u,0x3F800000u,0x3F800000u },
	{ "CVF_NEGONE",       0xBF800000u,0xBF800000u,0xBF800000u,0xBF800000u },
	{ "CVF_GARBAGE1",     0x00001337u,0x00001337u,0x00001337u,0x00001337u },
	{ "CVF_GARBAGE2",     0xDEADBEEFu,0xDEADBEEFu,0xDEADBEEFu,0xDEADBEEFu },
	{ "CVF_INCREASING",   0x3F800000u,0x40000000u,0x40400000u,0x40800000u },
	{ "CVF_DECREASING",   0x40800000u,0x40400000u,0x40000000u,0x3F800000u },
	{ "CVF_PI_OVER2",     0x3FC90FDBu,0x3FC90FDBu,0x3FC90FDBu,0x3FC90FDBu },
	{ "CVF_PI",           0x40490FDBu,0x40490FDBu,0x40490FDBu,0x40490FDBu },
	{ "CVF_3PI_OVER2",    0x4096CBE4u,0x4096CBE4u,0x4096CBE4u,0x4096CBE4u },
};

static int lookup(const char* tok, u32* x, u32* y, u32* z, u32* w)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best == (size_t)-1) return 0;
	*x = kConst[best].x; *y = kConst[best].y;
	*z = kConst[best].z; *w = kConst[best].w;
	return 1;
}

/* ---- the integer ops --------------------------------------------------
 *
 * These take their operands in VI registers rather than VF, and the
 * capture prints the destination as four hex digits. The harness that
 * produced it cannot load an arbitrary sixteen-bit value in one
 * instruction, so what a line prints as an operand is not always what the
 * register held: WrSetIntegerRegister emits ISUBIU(r, VI00, v & ~0x8000)
 * for anything with bit 15 set, and the register ends up holding the
 * negation. That model is tests/vu/hwint.c's; it is reproduced here
 * because it belongs to the test program, not to the emulator. */

static const struct { const char* name; u16 v; } kIConst[] = {
	{ "CVI_ZERO",     0x0000u }, { "CVI_MAX",      0x7FFFu },
	{ "CVI_MIN",      0x8000u }, { "CVI_NEGONE",   0xFFFFu },
	{ "CVI_ONE",      0x0001u },
	{ "CVI_GARBAGE1", 0x1337u }, { "CVI_GARBAGE2", 0xBEEFu },
	/* The wider names all narrow to sixteen bits in a VI register. */
	{ "CVI_S16_MAX",  0x7FFFu }, { "CVI_S16_MIN",  0x8000u },
	{ "CVI_S32_MAX",  0xFFFFu }, { "CVI_S32_MIN",  0x0000u },
	{ "CVI_S64_MAX",  0xFFFFu }, { "CVI_S64_MIN",  0x0000u },
};

static u16 setvi(u32 v)
{
	if (v == 0x8000u) return (u16)((u16)0xFFFFu - (u16)0x7FFFu);
	if (v & 0x8000u)  return (u16)(0u - (v & ~0x8000u));
	return (u16)v;
}

/* `named` reports whether the operand was a CVI_ constant. Those are
 * loaded into the register from VU memory, so they arrive intact; a plain
 * decimal goes through WrSetIntegerRegister and is transformed. Applying
 * setvi to both scores 17 to 19 of 21 per block -- close enough to look
 * like an arithmetic edge case rather than a loading model. */
static int resolve_i(const char* tok, u16* out, int* named)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	char* end; long v;
	for (i = 0; i < sizeof(kIConst)/sizeof(kIConst[0]); i++)
	{
		const size_t n = strlen(kIConst[i].name);
		if (!strncmp(tok, kIConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kIConst[best].v; *named = 1; return 1; }
	v = strtol(tok, &end, 10);
	if (end == tok) return 0;
	*out = (u16)v; *named = 0;
	return 1;
}

enum IForm { I_RRR, I_IMM5, I_IMM15 };

static const struct { const char* name; u32 op; IForm form; } kIOps[] = {
	{ "iadd",   OP_IADD,   I_RRR },
	{ "isub",   OP_ISUB,   I_RRR },
	{ "iand",   OP_IAND,   I_RRR },
	{ "ior",    OP_IOR,    I_RRR },
	{ "iaddi",  OP_IADDI,  I_IMM5 },
	{ "iaddiu", OP_IADDIU, I_IMM15 },
	{ "isubiu", OP_ISUBIU, I_IMM15 },
};
#define NIOPS ((int)(sizeof(kIOps)/sizeof(kIOps[0])))

static int run_integer(const char* dir, int* total)
{
	char path[512];
	int op, bad = 0;

	snprintf(path, sizeof(path), "%s/integer.expected", dir);
	for (op = 0; op < NIOPS; op++)
	{
		FILE* f = fopen(path, "r");
		char buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) return 0;
		while (fgets(buf, sizeof(buf), f))
		{
			u16 a = 0, b = 0;
			int a_named = 0, b_named = 0;
			unsigned want;
			char *p, *comma, *colon;

			if (!inblock)
			{ if (!strncmp(buf, kIOps[op].name, strlen(kIOps[op].name))
			   && buf[strlen(kIOps[op].name)] == '\n') inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;
			p = buf + 2 + strlen(kIOps[op].name);
			while (*p == ' ') p++;
			*colon = '\0';
			comma = strchr(p, ',');
			if (!comma) { *colon = ':'; continue; }
			*comma = '\0';
			if (!resolve_i(p, &a, &a_named)) { *colon = ':'; continue; }
			{ char* q = comma + 1; while (*q == ' ') q++;
			  if (!resolve_i(q, &b, &b_named)) { *colon = ':'; continue; } }
			*colon = ':';

			memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
			vuRegs[0].VI[VI_S].UL = a_named ? a : setvi(a);
			switch (kIOps[op].form)
			{
			case I_RRR:
				vuRegs[0].VI[VI_T].UL = b_named ? b : setvi(b);
				vuRegs[0].code = LOWER_TYPE1345 | VT(VI_T) | VS(VI_S)
				               | VD(VI_D) | kIOps[op].op;
				break;
			case I_IMM5:
				vuRegs[0].code = LOWER_TYPE1345 | VT(VI_D) | VS(VI_S)
				               | IMM5(b) | kIOps[op].op;
				break;
			default:
				vuRegs[0].code = ((u32)kIOps[op].op << 25) | VT(VI_D)
				               | VS(VI_S) | IMM15(b);
				break;
			}
			VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();

			cases++;
			if ((vuRegs[0].VI[VI_D].UL & 0xFFFFu) == want) pass++;
		}
		fclose(f);
		*total += cases;
		printf("%-7s %2d/%-3d console cases\n", kIOps[op].name, pass, cases);
		if (pass != cases) bad++;
	}
	return bad;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "efu.expected";
	int op, failures = 0, total = 0, shown = 0;

	vu_config_is_default();

	for (op = 0; op < NOPS; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			u32 x, y, z, w;
			unsigned want;
			char *a, *colon;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;
			a = buf + 2 + strlen(kOps[op].name);
			while (*a == ' ') a++;
			*colon = '\0';
			if (!lookup(a, &x, &y, &z, &w)) { *colon = ':'; continue; }
			*colon = ':';

			memset(&vuRegs[0], 0, sizeof(vuRegs[0]));
			vuRegs[0].VF[VF_SRC].UL[0] = x;
			vuRegs[0].VF[VF_SRC].UL[1] = y;
			vuRegs[0].VF[VF_SRC].UL[2] = z;
			vuRegs[0].VF[VF_SRC].UL[3] = w;

			/* Scalar forms read one component through fsf, which the
			 * capture always exercises as x; vector forms read xyz and
			 * carry a dest mask instead. */
			vuRegs[0].code = LOWER_TYPE1345 | VS(VF_SRC) | VT(0)
			               /* The scalar forms are issued with FIELD_Z by the
			                * efu harness (PerformS_M in lower/efu.cpp), not
			                * FIELD_X. With x the constants that hold the same
			                * value in every lane still pass, so the wrong
			                * field only shows on CVF_INCREASING and its kin. */
			               | (kOps[op].vector ? DEST(0xE) : FSF(2))
			               | (u32)kOps[op].op;
			VU0_LOWER_OPCODE[vuRegs[0].code >> 25]();

			cases++;
			/* The EFU writes VU->p; VI[REG_P] only receives it when the
			 * pipeline flushes, which no single op here does. */
			if (vuRegs[0].p.UL == want) pass++;
			else if (shown++ < 4)
				printf("  %-7s %08x: console %08x  ours %08x\n",
				       kOps[op].name, x, want, vuRegs[0].p.UL);
		}
		fclose(f);
		total += cases;
		printf("%-7s %2d/%-3d console cases (floor %d)\n",
		       kOps[op].name, pass, cases, kOps[op].floor);
		if (pass < kOps[op].floor)
		{ printf("  regression: this used to match %d\n", kOps[op].floor);
		  failures++; }
	}

	{
		/* integer.expected sits beside efu.expected. */
		char dir[512];
		const char* slash = strrchr(path, '/');
		size_t n = slash ? (size_t)(slash - path) : 1;
		if (n >= sizeof(dir)) n = sizeof(dir) - 1;
		memcpy(dir, path, n);
		dir[n] = '\0';
		failures += run_integer(slash ? dir : ".", &total);
	}

	printf("hwrealvu: %d cases across %d ops, driven through VUops.cpp\n",
	       total, NOPS + NIOPS);
	return failures != 0;
}
