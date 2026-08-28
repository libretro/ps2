/*  GTE against PS1 console captures.
 *
 *  Scores pcsx2/IopGte.cpp against the hardware log in JaCzekanski's
 *  ps1-tests, gte-fuzz/gte_valid_0xc0ffee_50.log -- a fuzz run captured
 *  on a real console. Each test writes all sixty-four GTE registers with
 *  random values, runs one command with random sf/lm/tx/vx/mx, and dumps
 *  all sixty-four back.
 *
 *  This links IopGte.cpp itself rather than transcribing it. The file
 *  touches only psxRegs.CP2C, CP2D, GPR and code, so a definition of
 *  psxRegs is the whole of what it needs, and nothing here can drift from
 *  the implementation it is scoring.
 *
 *  Registers 0-31 are the data set and 32-63 the control set, and the
 *  harness moves them with the instructions rather than by poking the
 *  arrays: MTC2 and CTC2 in, MFC2 and CFC2 out. That matters, because
 *  several of them transform on the way through -- writing IRGB expands
 *  to three IR values, ORGB and LZCR are computed on read -- so bypassing
 *  the moves would score a different machine from the one that runs.
 *
 *  All 1100 tests in the log match, across the twenty-two documented
 *  commands. Two mutations were tried against that: narrowing the IR
 *  saturation bound from 32767 to 32766 fails 44 tests, so the limiters
 *  are genuinely exercised; but narrowing the divider's 0x1FFFF clamp to
 *  0x1FFFE fails none, so this seed never drives the reciprocal into
 *  saturation. That clamp is covered by definition rather than by
 *  capture, in tests/iop/hwgte.c.
 *
 *  Usage: tests/iop/hwgtefuzz <path-to-gte_valid_....log>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "R3000A.h"
#include "IopGte.h"

alignas(16) psxRegisters psxRegs;

/* LWC2 and SWC2 reach into IOP memory. This log drives the register file
 * directly and never issues either, but they still have to link, so the
 * lookup table is given one empty page and the slow paths are stubs. */
static u8 s_dummy_page[0x10000];
uptr* psxMemWLUT = NULL;
uptr psxMemRLUT[0x10000];
static struct rlut_init_t {
	rlut_init_t()
	{
		for (unsigned i = 0; i < 0x10000; i++)
			psxMemRLUT[i] = (uptr)s_dummy_page;
	}
} s_rlut_init;
u32 iopMemRead32_slow(u32 mem) { (void)mem; return 0; }
void iopMemWrite32(u32 mem, u32 value) { (void)mem; (void)value; }

/* Build the instruction word the move functions decode, then call them.
 * GPR[1] carries the value; rt = 1, rd = the register number. */
static void gte_write(int reg, u32 value)
{
	psxRegs.GPR.r[1] = value;
	psxRegs.code = (1u << 16) | (((u32)(reg & 0x1F)) << 11);
	if (reg < 32) gteMTC2(); else gteCTC2();
}
static u32 gte_read(int reg)
{
	psxRegs.GPR.r[1] = 0;
	psxRegs.code = (1u << 16) | (((u32)(reg & 0x1F)) << 11);
	if (reg < 32) gteMFC2(); else gteCFC2();
	return psxRegs.GPR.r[1];
}

/* The command word: opcode in the low six bits, then lm, sf and the
 * MVMVA selectors where the hardware puts them. */
static void gte_exec(int op, int sf, int lm, int tx, int vx, int mx)
{
	psxRegs.code = (u32)(op & 0x3F)
	             | ((u32)(lm & 1) << 10)
	             | ((u32)(sf & 1) << 19)
	             | ((u32)(tx & 3) << 13)
	             | ((u32)(vx & 3) << 15)
	             | ((u32)(mx & 3) << 17);
	switch (op)
	{
	case 0x01: gteRTPS();  break;
	case 0x06: gteNCLIP(); break;
	case 0x0C: gteOP();    break;
	case 0x10: gteDPCS();  break;
	case 0x11: gteINTPL(); break;
	case 0x12: gteMVMVA(); break;
	case 0x13: gteNCDS();  break;
	case 0x14: gteCDP();   break;
	case 0x16: gteNCDT();  break;
	case 0x1B: gteNCCS();  break;
	case 0x1C: gteCC();    break;
	case 0x1E: gteNCS();   break;
	case 0x20: gteNCT();   break;
	case 0x28: gteSQR();   break;
	case 0x29: gteDCPL();  break;
	case 0x2A: gteDPCT();  break;
	case 0x2D: gteAVSZ3(); break;
	case 0x2E: gteAVSZ4(); break;
	case 0x30: gteRTPT();  break;
	case 0x3D: gteGPF();   break;
	case 0x3E: gteGPL();   break;
	case 0x3F: gteNCCT();  break;
	default: break;        /* not a documented command */
	}
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "gte_valid_0xc0ffee_50.log";
	FILE* f = fopen(path, "r");
	char buf[512];
	u32 in[64], want[64];
	int have_in = 0, op = -1, sf = 0, lm = 0, tx = 0, vx = 0, mx = 0;
	int tests = 0, pass = 0, shown = 0;
	/* Per-op tallies, indexed by command number. */
	int op_pass[64], op_tot[64], i;
	char op_name[64][16];

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	for (i = 0; i < 64; i++) { op_pass[i] = 0; op_tot[i] = 0; op_name[i][0] = '\0'; }

	while (fgets(buf, sizeof(buf), f))
	{
		int reg; unsigned val;

		if (!strncmp(buf, "-------------- GTE ", 19))
		{
			unsigned c; char nm[32];
			if (sscanf(buf + 19, "0x%02x %31s", &c, nm) == 2 && c < 64)
				snprintf(op_name[c], sizeof(op_name[0]), "%s", nm);
			continue;
		}
		if (sscanf(buf, " > r[%d] = 0x%x", &reg, &val) == 2)
		{ if (reg >= 0 && reg < 64) { in[reg] = val; have_in = 1; } continue; }

		if (!strncmp(buf, "GTE 0x", 6))
		{
			unsigned c;
			if (sscanf(buf, "GTE 0x%02x %*s (sf=%d, lm=%d, tx=%d, vx=%d, mx=%d)",
			           &c, &sf, &lm, &tx, &vx, &mx) == 6)
				op = (int)c;
			continue;
		}
		if (sscanf(buf, " < r[%d] = 0x%x", &reg, &val) == 2)
		{
			if (reg >= 0 && reg < 64) want[reg] = val;
			if (reg != 63 || !have_in || op < 0) continue;

			/* A whole test is in hand: load, run, read back. */
			memset(&psxRegs, 0, sizeof(psxRegs));
			for (i = 0; i < 64; i++) gte_write(i, in[i]);
			gte_exec(op, sf, lm, tx, vx, mx);

			{
				int ok = 1, firstbad = -1;
				for (i = 0; i < 64; i++)
				{
					const u32 got = gte_read(i);
					if (got != want[i]) { if (firstbad < 0) firstbad = i; ok = 0; }
				}
				tests++;
				op_tot[op]++;
				if (ok) { pass++; op_pass[op]++; }
				else if (shown++ < 5)
					printf("  op 0x%02x %-6s test %d: first mismatch r[%d]"
					       " console %08x ours %08x\n",
					       op, op_name[op], tests, firstbad,
					       want[firstbad], gte_read(firstbad));
			}
			have_in = 0; op = -1;
			continue;
		}
	}
	fclose(f);

	for (i = 0; i < 64; i++)
		if (op_tot[i])
			printf("  0x%02x %-6s %4d/%-4d\n", i, op_name[i], op_pass[i], op_tot[i]);
	printf("hwgtefuzz: %d/%d console tests match\n", pass, tests);
	return pass != tests;
}
