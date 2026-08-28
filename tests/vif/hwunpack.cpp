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
	return pass != cases;
}
