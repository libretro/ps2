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

/* Floors, not targets. The three that go through ps2float.c are exact;
 * the polynomial ones are not, and the interpreter's own accuracy is what
 * tests/vu/hwefu.c already measures and documents. What this table adds is
 * a regression bound on the real code rather than on a copy of it.
 *
 * EATAN is the one worth pointing at. hwefu.c's transcription scores 8 of
 * its 13 cases; the real code scores 0 of 16 here, and the misses are a
 * single ULP -- 3f8db70c against the console's 3f8db70b. So the
 * transcription and VUops.cpp do not compute quite the same thing, which
 * is precisely the drift a copy cannot report on itself. Recorded rather
 * than papered over; whichever is closer to the hardware is a separate
 * question from noticing they differ. */
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

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "efu.expected";
	int op, failures = 0, total = 0, shown = 0;

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

	printf("hwrealvu: %d cases across %d ops, driven through VUops.cpp\n",
	       total, NOPS);
	return failures != 0;
}
