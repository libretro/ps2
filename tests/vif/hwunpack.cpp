/*  VIF unpack against console captures.
 *
 *  Scores pcsx2/Vif_Unpack.cpp against ps2autotests
 *  tests/dma/vif/unpack.expected. That capture sends a VIF packet of
 *  STCYCL(4,4) then one UNPACK of each format, with a fixed data word,
 *  and prints the quadword the unpack leaves in VU memory. Since the
 *  cycle is 4/4 and one vector is written, each line is a pure function
 *  of the format, the sign-extension flag and the source data -- no DMA
 *  timing, no cycle stealing, nothing that needs the emulator running.
 *
 *  VIFfuncTable is indexed [usn][mask-mode][format], where format packs
 *  the vector width and element size the way the VIF command does: bits
 *  3:2 select S, V2, V3 or V4, bits 1:0 the element size, and the entries
 *  are laid out with the unsigned half following the signed one. The
 *  table is exported and const, so the harness calls it directly rather
 *  than assembling a packet and driving the transfer.
 *
 *  Destination memory starts as all-ones, as the test sets it, because
 *  the narrower formats only write part of the quadword and what is left
 *  standing is part of what the capture records.
 *
 *  Usage: tests/vif/hwunpack <path-to-vif/unpack.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "Common.h"
#include "Vif.h"
#include "Vif_Unpack.h"

/* Vif_Unpack.cpp's transfer entry points reach the recompiled unpackers
 * and the MTVU thread; this harness calls the interpreted table directly
 * and never enters them, so they are storage and no-ops. */
#include "MTVU.h"
alignas(16) u8 eeHw[0x10000];
alignas(16) vifStruct vif0, vif1;
alignas(16) VURegs vuRegs[2];
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");
void dVifReset(int) { }
void vifExecQueue(int) { }
template <int idx> void dVifUnpack(const u8*, int) { }
template void dVifUnpack<0>(const u8*, int);
template void dVifUnpack<1>(const u8*, int);
void VU_Thread::VifUnpack(vifStruct&, VIFregisters&, const u8*, u32) { }

/* The formats the "Simple UNPACK" lines exercise, by the name the capture
 * prints. `fmt` is the low four bits of the VIF unpack command. */
static const struct { const char* name; u32 fmt; int usn; } kFormats[] = {
	{ "S8(signExtend)",   0x02, 0 }, { "S8(zeroExtend)",   0x02, 1 },
	{ "S16(signExtend)",  0x01, 0 }, { "S16(zeroExtend)",  0x01, 1 },
	{ "S32",              0x00, 0 },
	{ "V2-8(signExtend)", 0x06, 0 }, { "V2-8(zeroExtend)", 0x06, 1 },
	{ "V2-16(signExtend)",0x05, 0 }, { "V2-16(zeroExtend)",0x05, 1 },
	{ "V2-32",            0x04, 0 },
	{ "V3-8(signExtend)", 0x0A, 0 }, { "V3-8(zeroExtend)", 0x0A, 1 },
	{ "V3-16(signExtend)",0x09, 0 }, { "V3-16(zeroExtend)",0x09, 1 },
	{ "V3-32",            0x08, 0 },
	{ "V4-8(signExtend)", 0x0E, 0 }, { "V4-8(zeroExtend)", 0x0E, 1 },
	{ "V4-16(signExtend)",0x0D, 0 }, { "V4-16(zeroExtend)",0x0D, 1 },
	{ "V4-32",            0x0C, 0 },
	{ "V4-5",             0x0F, 0 },
};
#define NFMT ((int)(sizeof(kFormats)/sizeof(kFormats[0])))

/* The data words the test feeds each format, from unpack.cpp. */
static u32 source_for(const char* name)
{
	if (!strncmp(name, "S8", 2))    return 0x88u;
	if (!strncmp(name, "S16", 3))   return 0x8888u;
	if (!strncmp(name, "S32", 3))   return 0x88888888u;
	if (!strncmp(name, "V2-8", 4))  return 0x9988u;
	if (!strncmp(name, "V2-16", 5)) return 0x99998888u;
	if (!strncmp(name, "V3-8", 4))  return 0xAA9988u;
	if (!strncmp(name, "V4-8", 4))  return 0xBBAA9988u;
	/* V4-5 unpacks one 16-bit RGBA5551 word; the test feeds all ones. */
	if (!strncmp(name, "V4-5", 4))  return 0x0000FFFFu;
	return 0x88888888u;
}

/* ---- STMOD: the write modes and the mask ------------------------------
 *
 * tests/dma/vif/stmod.expected runs the same shape eight times per VIF
 * unit: STCYCL(4,4), STROW of 0x1000 in every lane, STMASK, STMOD, then a
 * V4-32 unpack of the words 0..15, and prints the four quadwords that
 * land in VU memory along with the row register and the mode afterwards.
 *
 * Row is a result as well as an input. Modes 2 and 3 write back through
 * setVifRow as they go, so the row the capture prints at the end is what
 * pins those two down -- the memory alone does not distinguish a mode that
 * accumulates into row from one that merely reads it.
 *
 * What these sixteen do not reach is column masking. The mask they use,
 * 0x40100401, selects row (n==1) on the diagonal and data (n==0)
 * everywhere else, so the n==2 arm that reads MaskCol never runs:
 * changing it to read MaskRow instead leaves all sixteen passing.
 * Row masking, the offset arithmetic and the mode-2 write-back are all
 * covered -- each fails four scenarios when mutated.
 *
 * vif.cl has to be stepped per vector, since the mask nibble is selected
 * by it: leaving it at zero applies the first row of the mask to all four
 * vectors, which passes the unmasked scenarios and quietly fails the
 * masked ones in a way that looks like a mask decoding bug. */

static int run_stmod(const char* path, int* total)
{
	FILE* f = fopen(path, "r");
	char buf[512];
	int pass = 0, cases = 0, shown = 0, unit = 0, masked = 0;
	u32 want_mem[4][4];
	int have = 0;

	if (!f) return 0;
	while (fgets(buf, sizeof(buf), f))
	{
		unsigned w[4];
		int row_line;

		if (!strncmp(buf, "== VIF1", 7)) { unit = 1; continue; }
		if (!strncmp(buf, "== VIF0", 7)) { unit = 0; continue; }
		/* The scenario heading says whether the mask was enabled, so it is
		 * read rather than guessed. Trying both and accepting whichever
		 * matched would let a scenario pass for the wrong reason. */
		if (buf[0] != ' ' && strchr(buf, ':'))
		{ /* "no mask" contains "mask", so the negative has to be tested
		   * first -- matching on "mask" alone marks every scenario as
		   * masked and compares the masked output against the unmasked
		   * expectations. */
		  masked = strstr(buf, "mask") != NULL && strstr(buf, "no mask") == NULL;
		  continue; }

		if (sscanf(buf, "  vumem(%*x): %x - %x - %x - %x",
		           &w[0], &w[1], &w[2], &w[3]) == 4)
		{
			if (have < 4)
			{ want_mem[have][0]=w[0]; want_mem[have][1]=w[1];
			  want_mem[have][2]=w[2]; want_mem[have][3]=w[3]; have++; }
			continue;
		}
		row_line = (sscanf(buf, "  row: %x - %x - %x - %x",
		                   &w[0], &w[1], &w[2], &w[3]) == 4);
		if (!row_line) continue;
		{
			unsigned mode = 0;
			u32 want_row[4] = { w[0], w[1], w[2], w[3] };
			if (!fgets(buf, sizeof(buf), f)) break;
			if (sscanf(buf, "  mode: %x", &mode) != 1) { have = 0; continue; }
			if (have != 4) { have = 0; continue; }

			{
				/* The mask value is the same in every masked scenario. */
				const int dm = masked;
				int ok = 0;
				{
					alignas(16) u32 mem[16];
					u32 src[4];
					int v, i;
					vifStruct& vif = unit ? vif1 : vif0;
					VIFregisters& regs = unit ? vif1Regs : vif0Regs;

					memset(mem, 0, sizeof(mem));
					memset(&vif, 0, sizeof(vif));
					memset(&regs, 0, sizeof(regs));
					regs.mask = 0x40100401u;
					regs.mode = mode;
					for (i = 0; i < 4; i++) vif.MaskRow._u32[i] = 0x1000u;

					for (v = 0; v < 4; v++)
					{
						const UNPACKFUNCTYPE fn =
							VIFfuncTable[unit][mode][0x0C | (dm ? 0x20 : 0)];
						/* signed/dm1 is +0x20; +0x30 is unsigned/dm1 and
						 * silently unpacks unmasked for a 32-bit format. */
						for (i = 0; i < 4; i++) src[i] = (u32)(v * 4 + i);
						vif.cl = v;          /* selects the mask nibble */
						fn(mem + v * 4, src);
					}

					ok = 1;
					for (v = 0; v < 4 && ok; v++)
						for (i = 0; i < 4; i++)
							if (mem[v * 4 + i] != want_mem[v][i]) { ok = 0; break; }
					for (i = 0; i < 4 && ok; i++)
						if (vif.MaskRow._u32[i] != want_row[i]) ok = 0;
				}
				cases++;
				if (ok) pass++;
				else if (shown++ < 4)
					printf("  stmod VIF%d mode %u %s: does not match the capture\n",
					       unit, mode, masked ? "masked" : "unmasked");
			}
			have = 0;
		}
	}
	fclose(f);
	*total += cases;
	printf("hwstmod:  %d/%d VIF write-mode scenarios match the console\n", pass, cases);
	return pass != cases;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "unpack.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int pass = 0, cases = 0, shown = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned w0, w1, w2, w3;
		char name[64];
		int i;

		if (strncmp(buf, "Simple UNPACK ", 14)) continue;
		if (sscanf(buf + 14, "%63s - %x - %x - %x - %x",
		           name, &w0, &w1, &w2, &w3) != 5) continue;

		for (i = 0; i < NFMT; i++)
			if (!strcmp(kFormats[i].name, name)) break;
		if (i == NFMT) continue;

		{
			alignas(16) u32 dest[4];
			alignas(16) u32 src[4];
			/* The first index is the VIF unit and the second the mask
			 * mode; the unsigned variants are the upper half of the
			 * 64-entry inner dimension, not a separate index. Reading it
			 * as [usn][mode][fmt] gets every signed case right and every
			 * unsigned one wrong, which looks like broken zero-extension
			 * rather than a misread table.
			 *
			 * The inner dimension runs in four blocks of sixteen: signed
			 * with dm 0, unsigned with dm 0, then the same pair with dm 1.
			 * So the unsigned variants are at +0x10, not +0x20 -- +0x20
			 * lands in the signed dm-1 block and still sign-extends. */
			const UNPACKFUNCTYPE fn =
				VIFfuncTable[0][0][kFormats[i].fmt | (kFormats[i].usn ? 0x10 : 0)];

			memset(dest, 0xFF, sizeof(dest));
			src[0] = source_for(name);
			src[1] = src[2] = src[3] = 0;
			if (!strncmp(name, "V2-32", 5))  { src[0] = 0x88888888u; src[1] = 0x99999999u; }
			if (!strncmp(name, "V3-16", 5))  { src[0] = 0x99998888u; src[1] = 0xAAAAu; }
			if (!strncmp(name, "V3-32", 5))
			{ src[0] = 0x88888888u; src[1] = 0x99999999u; src[2] = 0xAAAAAAAAu; }
			if (!strncmp(name, "V4-16", 5))  { src[0] = 0x99998888u; src[1] = 0xBBBBAAAAu; }
			if (!strncmp(name, "V4-32", 5))
			{ src[0] = 0x88888888u; src[1] = 0x99999999u;
			  src[2] = 0xAAAAAAAAu; src[3] = 0xBBBBBBBBu; }

			fn(dest, src);
			cases++;
			if (dest[0] == w0 && dest[1] == w1 && dest[2] == w2 && dest[3] == w3)
				pass++;
			else if (shown++ < 8)
				printf("  %-20s console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       name, w0, w1, w2, w3, dest[0], dest[1], dest[2], dest[3]);
		}
	}
	fclose(f);

	printf("hwunpack: %d/%d VIF unpack formats match the console\n", pass, cases);

	if (argc > 2)
	{
		int stmod_total = 0;
		if (run_stmod(argv[2], &stmod_total)) return 1;
	}
	return pass != cases;
}
