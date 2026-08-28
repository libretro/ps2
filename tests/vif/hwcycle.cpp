/*  VIF write and skip cycles against console captures.
 *
 *  Scores pcsx2/Vif_Unpack.cpp's unpack loop against ps2autotests
 *  tests/dma/vif/stcycl.expected. Each line sets CYCLE to one (cl, wl)
 *  pair, a mask of 0xAAAAAAAA and a column of (1,2,3,4), then unpacks
 *  eight V4-32 vectors and prints word 0 of each of the first 64
 *  quadwords as a single hex digit.
 *
 *  Two things make it worth having separately from the unpack and stmod
 *  scorers. It is the loop rather than the per-vector write: cl and wl
 *  decide which vectors are written, which are skipped and which repeat,
 *  and none of that is visible in a single unpack. And the mask is all
 *  twos, so every write takes the MaskCol arm -- the one the stmod
 *  scenarios never reach, as recorded in hwunpack.cpp.
 *
 *  _nVifUnpackLoop and UnpackLoopTable have internal linkage, so this
 *  includes the translation unit rather than linking it. That is the
 *  point: the loop is the thing under test, and reimplementing it here to
 *  get at it would leave the same gap the transcriptions elsewhere in
 *  this tree had.
 *
 *  Usage: tests/vif/hwcycle <path-to-vif/stcycl.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "Common.h"
#include "Vif.h"
#include "Vif_Unpack.h"
#include "MTVU.h"

alignas(16) u8 eeHw[0x10000];
alignas(16) VURegs vuRegs[2];
alignas(16) vifStruct vif0, vif1;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");
/* Only referenced by inline helpers that -O1 elides and -O0 does not. */
void* _aligned_malloc(size_t n, size_t a) { void* p = NULL; posix_memalign(&p, a, n); return p; }
void _aligned_free(void* p) { free(p); }
RETURNS_R128 vtlb_memRead128(u32) { r128 v; memset(&v, 0, sizeof(v)); return v; }
void vtlb_memWrite128(u32, r128) { }
void dVifReset(int) { }
void vifExecQueue(int) { }
template <int idx> void dVifUnpack(const u8*, int) { }
template void dVifUnpack<0>(const u8*, int);
template void dVifUnpack<1>(const u8*, int);
void VU_Thread::VifUnpack(vifStruct&, VIFregisters&, const u8*, u32) { }

/* The unit under test, included rather than linked so the loop and its
 * dispatch table are reachable. */
#include "Vif_Unpack.cpp"

/* The loop calls nVifUpk, the recompiled per-cycle kernels. On arm64 it
 * falls back to the interpreted VIFfuncTable entry when those are null;
 * on x86 it does not, so this harness fills the table with wrappers
 * around the same interpreted function the fallback would use. The loop
 * logic under test -- which vectors are written, skipped or repeated for
 * a given cl and wl -- is common to both paths, and the per-vector write
 * is already scored by hwunpack.cpp against the same captures.
 *
 * The four slots for a format all get the same wrapper: writeXYZW selects
 * the mask row from vif.cl itself, so the per-cycle kernels differ only
 * in what the recompiler folded in. */
template <int idx>
static u32 interp_unpack(void* dest, const void* src)
{
	vifStruct& vif = idx ? vif1 : vif0;
	VIFregisters& regs = idx ? vif1Regs : vif0Regs;
	const int usn = !!vif.usn;
	const int upkNum = vif.cmd & 0x1f;
	VIFfuncTable[idx][regs.mode][(usn * 2 * 16) + upkNum](dest, src);
	return 0;
}

static void fill_upk_table(int unit, int usn, int upkNum)
{
	const int base = ((usn * 2 * 16) + upkNum) * 4;
	int i;
	for (i = 0; i < 4; i++)
		nVifUpk[base + i] = unit ? interp_unpack<1> : interp_unpack<0>;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "stcycl.expected";
	FILE* f = fopen(path, "r");
	char buf[1024];
	int pass = 0, cases = 0, shown = 0, unit = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	/* VURegs::Mem is a pointer the emulator allocates at startup; the
	 * unpack writes straight into it, so the harness has to provide it.
	 * VU0 has 4K of data memory and VU1 16K. */
	vuRegs[0].Mem = (u8*)calloc(1, 0x1000);
	vuRegs[1].Mem = (u8*)calloc(1, 0x4000);
	if (!vuRegs[0].Mem || !vuRegs[1].Mem) return 2;

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned cl, wl, cyclereg;
		char digits[256];
		int truncated;

		if (!strncmp(buf, "== VIF1", 7)) { unit = 1; continue; }
		if (!strncmp(buf, "== VIF0", 7)) { unit = 0; continue; }

		truncated = !strncmp(buf, "truncated", 9);
		{
			const char* p = strstr(buf, "(cl ");
			if (!p) continue;
			if (sscanf(p, "(cl %u, wl %u): CYCLE: %x, %255s",
			           &cl, &wl, &cyclereg, digits) != 4) continue;
		}
		/* The truncated variants send the data in two halves, which is a
		 * transfer-level behaviour rather than a loop one. Skipped here
		 * and counted as skipped rather than silently dropped. */
		if (truncated) continue;

		{
			vifStruct& vif = unit ? vif1 : vif0;
			VIFregisters& regs = unit ? vif1Regs : vif0Regs;
			alignas(16) u32 data[8 * 4];
			char got[256];
			int i, n;

			memset(vuRegs[unit].Mem, 0, 64 * 16);
			memset(&vif, 0, sizeof(vif));
			memset(&regs, 0, sizeof(regs));
			for (i = 0; i < 8 * 4; i++) data[i] = (u32)i;

			regs.cycle.cl = (u8)cl;
			regs.cycle.wl = (u8)wl;
			regs.mask = 0xAAAAAAAAu;   /* every element takes the column */
			regs.mode = 0;
			regs.num  = 8;
			vif.MaskCol._u32[0] = 1; vif.MaskCol._u32[1] = 2;
			vif.MaskCol._u32[2] = 3; vif.MaskCol._u32[3] = 4;
			vif.cmd = 0x6C | 0x10;     /* V4-32 with masking enabled */
			vif.usn = 0;
			vif.tag.addr = 0;
			vif.start_aligned = 0;
			vif.cl = 0;

			fill_upk_table(unit, 0, vif.cmd & 0x1f);
			{
				const uint wl_eff = regs.cycle.wl ? regs.cycle.wl : 256;
				const bool isFill = (regs.cycle.cl < wl_eff);
				UnpackLoopTable[unit][0][isFill]((const u8*)data);
			}

			/* Word 0 of each of the first 64 quadwords, one hex digit
			 * each, exactly as the capture prints them. */
			n = (int)strlen(digits);
			if (n > 64) n = 64;
			for (i = 0; i < n; i++)
			{
				const u32 v = ((u32*)vuRegs[unit].Mem)[i * 4];
				got[i] = "0123456789abcdef"[v & 0xF];
			}
			got[n] = '\0';

			cases++;
			if (!strncmp(got, digits, (size_t)n)) pass++;
			else if (shown++ < 6)
				printf("  VIF%d cl %u wl %u:\n    console %.32s\n    ours    %.32s\n",
				       unit, cl, wl, digits, got);
		}
	}
	fclose(f);

	printf("hwcycle:  %d/%d VIF write-cycle scenarios match the console\n",
	       pass, cases);
	return pass != cases;
}
