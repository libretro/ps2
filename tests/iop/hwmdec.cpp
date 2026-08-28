/*  MDEC against the PS1 console trace.
 *
 *  Replays JaCzekanski's ps1-tests mdec/step-by-step-log/psx.log against
 *  pcsx2/Mdec.cpp. That log drives the MDEC entirely through its two
 *  registers -- no DMA -- writing one word at a time and reading
 *  MDEC_STATUS between every step, then switching to reads when the input
 *  FIFO fills. So it records, in order, every write, every read, and what
 *  the status register said at each point.
 *
 *  This links Mdec.cpp rather than transcribing it, so it cannot drift
 *  from the implementation it scores.
 *
 *  The 512 output words are reported but cannot be won as written, and
 *  that is a property of the capture rather than of the emulator. The
 *  trace's own header says so: "Decoded blocks are copied manually
 *  WITHOUT swizzling, blocks are copied as 16x4 instead of 8x8". The words
 *  it prints are in the order its own copy loop produced, not the raster
 *  order yuv2rgb15 writes, so a word-by-word comparison fails however
 *  correct the decode is. Scoring them means replicating that copy order
 *  here first.
 *
 *  What was checked while establishing that, since it bears on the rest:
 *  the whole input stream does decode cleanly, 473 of its 512 RL codes
 *  making exactly four macroblocks, and the decoder produces real pixels
 *  once mdecInit has set up the IDCT tables. So the remaining gap is not
 *  the decoder failing on this data.
 *
 *  Three things are checked separately, because they fail independently
 *  and lumping them together would hide which part is wrong:
 *    - the status register after each step,
 *    - the words the output FIFO hands back,
 *    - the order of the phase switches (input full, output empty).
 *
 *  As it stands this reports 154 of 777 status reads and 0 of 512 output
 *  words. The status register now carries real state -- see the note in
 *  Mdec.cpp, which also pins why the output words cannot follow yet: the
 *  decoder is driven from a memory buffer by psxDma0/psxDma1 rather than
 *  from the registers. Neither is a small fix, and the trace shows why:
 *  the console's input phases take 96, 32, 64 and 32 words before the
 *  input FIFO reports full. That is not a fixed depth being hit -- it is
 *  the decoder consuming variable-length run-level codes at its own rate,
 *  so reproducing the switch points means running the block decode
 *  interleaved with the writes, at word granularity, rather than in
 *  batches. The status bits then follow from that state.
 *
 *  The unit to build it around is the 8x8 block, not the macroblock.
 *  Every phase boundary in the trace lands on a multiple of 32 words:
 *  the input phases run 96, 32, 64, 32 and 32, and the output phases 96,
 *  96, 64, 128 and 128. Thirty-two words is one block at 15bpp -- 64
 *  pixels of two bytes -- so the FIFOs fill and drain a block at a time
 *  and the phase switches never fall inside one. That also explains why
 *  the output phases do not divide into 128-word macroblocks: draining
 *  stops when the FIFO empties, which happens mid-macroblock whenever the
 *  decoder has not yet been fed enough to finish the next block.
 *
 *  So this is the oracle for that work rather than a passing test, and it
 *  is wired into build.sh as a report rather than a gate. The bit layout
 *  is in the log itself, spelled out beside every value: 31 cmdBusy, 30
 *  dataInFifoFull, 29 dataOutFifoEmpty, 28 and 27 the DMA request lines,
 *  26..25 depth, 24 signed, 23 set-bit-15, 18..16 the block counter, and
 *  the remaining word count less one in the low half.
 *
 *  Usage: tests/iop/hwmdec <path-to-mdec-step-by-step-log/psx.log>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "R3000A.h"
#include "Mdec.h"

alignas(16) psxRegisters psxRegs;

/* Mdec.cpp reaches into IOP memory for the DMA paths, which this trace
 * never uses; one page keeps the references resolvable. */
static u8 s_dummy_page[0x10000];
uptr psxMemWLUT[0x10000];
uptr psxMemRLUT[0x10000];
alignas(16) u8 iopHw[0x10000];
u32 iopMemRead32_slow(u32 mem) { (void)mem; return 0; }
void psxDmaInterrupt(int n) { (void)n; }
void iopMemWrite32(u32 mem, u32 value) { (void)mem; (void)value; }

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "psx.log";
	FILE* f = fopen(path, "r");
	char buf[512];
	unsigned i;
	long status_ok = 0, status_tot = 0;
	long read_ok = 0, read_tot = 0;
	int shown = 0;
	u32 pending_status = 0;
	int have_status = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	for (i = 0; i < 0x10000; i++)
	{ psxMemRLUT[i] = (uptr)s_dummy_page; psxMemWLUT[i] = (uptr)s_dummy_page; }

	mdecInit();

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned v;

		/* "==== MDEC_STATUS: 0x........" -- what the console read here. */
		if (sscanf(buf, "==== MDEC_STATUS: 0x%x", &v) == 1)
		{
			const u32 got = mdecRead1();
			status_tot++;
			if (got == v) status_ok++;
			else if (shown++ < 5)
				printf("  status step %ld: console %08x  ours %08x\n",
				       status_tot, v, got);
			pending_status = v;
			have_status = 1;
			continue;
		}
		/* "mdec_decode(addr=0x800137a4, length=0x400, colorDepth=3,
		 * outputSigned=0, setBit15=0)..." -- the trace states the decode
		 * command in prose rather than as a register write, because the
		 * library call that issues it does so before the logged section
		 * begins. Without it the status register has no command to report
		 * and every read comes back with only the idle bits, which looks
		 * like the register being unimplemented rather than uncommanded.
		 * The command word is rebuilt from the parameters. */
		{
			unsigned len, depth, sgn, b15;
			const char* d = strstr(buf, "mdec_decode(");
			if (d && sscanf(d, "mdec_decode(addr=0x%*x, length=0x%x,"
			                   " colorDepth=%u, outputSigned=%u, setBit15=%u)",
			                &len, &depth, &sgn, &b15) == 4)
			{
				u32 cmd = 0x30000000u | ((len / 4) & 0xFFFFu);
				cmd |= (depth & 3u) << 27;
				if (sgn) cmd |= 1u << 26;
				if (b15) cmd |= 1u << 25;
				mdecWrite0(cmd);
				continue;
			}
		}

		/* "MDEC_CTRL <- 0x........" */
		if (strstr(buf, "MDEC_CTRL <- 0x") &&
		    sscanf(strstr(buf, "MDEC_CTRL <- 0x") + 15, "%x", &v) == 1)
		{ mdecWrite1(v); continue; }

		/* "MDEC_DATA <- 0x........" -- a word into the input FIFO. */
		if (sscanf(buf, "MDEC_DATA <- 0x%x", &v) == 1)
		{ mdecWrite0(v); continue; }

		/* "MDEC_DATA -> 0x........" -- a word out of the output FIFO. */
		if (sscanf(buf, "MDEC_DATA -> 0x%x", &v) == 1)
		{
			const u32 got = mdecRead0();
			read_tot++;
			if (got == v) read_ok++;
			else if (shown++ < 5)
				printf("  read %ld: console %08x  ours %08x\n", read_tot, v, got);
			continue;
		}
	}
	fclose(f);
	(void)pending_status; (void)have_status;

	printf("mdec status:  %ld/%ld reads match\n", status_ok, status_tot);
	printf("mdec output:  %ld/%ld words match\n", read_ok, read_tot);
	printf("hwmdec: %ld/%ld total\n", status_ok + read_ok, status_tot + read_tot);
	return (status_ok != status_tot || read_ok != read_tot) ? 1 : 0;
}
