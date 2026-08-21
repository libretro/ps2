/* Switchover equivalence harness.
 *
 * This file is compiled twice from the same source -- once as it ships, once
 * with PCSX2_C89_EMITTER -- and the emitted bytes are diffed. It answers a
 * question neither oracle can: those compare the two emitters side by side in
 * one build, but with the flag set the reference emitter is not present in the
 * translation unit at all, so what runs is the switched configuration
 * including the header restructure and the object bindings.
 *
 * Everything below is written the way the recompilers write it. That is the
 * point: if the switchover changes what a call site emits, it shows up here.
 */
#include "common/emitter/x86emitter.h"
#include <cstdio>
#include <cstdint>
#include <sys/mman.h>

using namespace x86Emitter;

/* Fixed absolute addresses rather than the address of a static: the two
 * builds are separately linked, so a static lands at a different address in
 * each and the disp32 would differ for reasons that have nothing to do with
 * the switchover. */
static void* const kAddrs[4] = {
	(void*)0x200001000ull,   /* RIP-reachable from the emit buffer */
	(void*)0x1fffff000ull,   /* negative RIP delta */
	(void*)0x0000000000410000ull,
	(void*)0x00007fff00000000ull };

int main()
{
	/* MAP_FIXED so RIP-relative displacements are reproducible between the
	 * two builds; without it the two runs would differ for reasons that have
	 * nothing to do with the switchover. */
	u8* buf = (u8*)mmap((void*)0x200000000ull, 1 << 20, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 2; }
	x86Ptr = (u8*)(buf);

	const s32 imms[] = { 0, 1, 0x7f, 0x80, -1, -0x80, -0x81, 0x1234, 0x7fffffff };
	const int nimm = (int)(sizeof(imms) / sizeof(imms[0]));

	/* group1: every operation, every operand shape the recompilers use. */
	for (int r = 0; r < 16; r++)
	{
		for (int s = 0; s < 16; s++)
		{
			xADD(xRegister32(r), xRegister32(s));
			xSUB(xRegister64(r), xRegister64(s));
			xAND(xRegister32(r), xRegister32(s));
			xOR (xRegister64(r), xRegister64(s));
			xXOR(xRegister32(r), xRegister32(s));
			xCMP(xRegister64(r), xRegister64(s));
			xADC(xRegister32(r), xRegister32(s));
		}
		for (int i = 0; i < nimm; i++)
		{
			xADD(xRegister32(r), imms[i]);
			xSUB(xRegister64(r), imms[i]);
			xAND(xRegister32(r), imms[i]);
			xOR (xRegister64(r), imms[i]);
			xXOR(xRegister32(r), imms[i]);
			xCMP(xRegister32(r), imms[i]);
		}
	}

	/* memory operands: absolute, base+index*scale+disp, and every width --
	 * the immediate forms are where the RIP correction varies with the
	 * immediate's size. */
	for (int b = 0; b < 8; b++)
	{
		for (int i = 0; i < nimm; i++)
		{
			const xAddressVoid m = xAddressVoid(xAddressReg(b), xAddressReg(2), 4, 0x30);
			xADD(ptr8[m],  imms[i]);
			xAND(ptr16[m], imms[i]);
			xOR (ptr32[m], imms[i]);
			xSUB(ptr64[m], imms[i]);
			xCMP(ptr32[kAddrs[b & 3]], imms[i]);
		}
		xADD(xRegister32(1), ptr32[xAddressVoid(xAddressReg(b), xAddressReg(3), 8, 0x40)]);
		xXOR(ptr32[xAddressVoid(xAddressReg(b), xAddressReg(3), 8, 0x40)], xRegister32(1));
	}

	/* the SSE members carried by the arithmetic and logic variants */
	for (int a = 0; a < 16; a++)
	{
		for (int b = 0; b < 16; b++)
		{
			xADD.PS(xRegisterSSE(a), xRegisterSSE(b));
			xADD.SD(xRegisterSSE(a), xRegisterSSE(b));
			xSUB.PD(xRegisterSSE(a), xRegisterSSE(b));
			xAND.PS(xRegisterSSE(a), xRegisterSSE(b));
			xXOR.PD(xRegisterSSE(a), xRegisterSSE(b));
		}
		xOR.PS(xRegisterSSE(a), ptr128[kAddrs[a & 3]]);
	}


	/* group2: shifts by immediate and by CL, both widths, including the
	 * zero-count case the emitter elides and the by-one short form. */
	for (int r = 0; r < 16; r++)
	{
		for (int c = 0; c < 34; c++)
		{
			xSHL(xRegister32(r), (u8)c);
			xSHR(xRegister64(r), (u8)c);
			xSAR(xRegister32(r), (u8)c);
			xROL(xRegister64(r), (u8)c);
			xROR(xRegister32(r), (u8)c);
			xRCL(xRegister32(r), (u8)c);
			xRCR(xRegister64(r), (u8)c);
		}
		xSHL(xRegister32(r), cl);
		xSAR(xRegister64(r), cl);
		xROR(xRegister32(r), cl);
	}

	/* group3 */
	for (int r = 0; r < 16; r++)
	{
		xNOT (xRegister32(r));
		xNEG (xRegister64(r));
		xUMUL(xRegister32(r));
		xUDIV(xRegister64(r));
	}
	for (int b = 0; b < 8; b++)
	{
		const xAddressVoid m = xAddressVoid(xAddressReg(b), xAddressReg(5), 2, 0x18);
		xNOT (ptr32[m]);
		xNEG (ptr32[m]);
		xUMUL(ptr32[m]);
		xUDIV(ptr32[m]);
	}


	/* mov, test, inc/dec. The immediate forms of mov are where the
	 * interesting branching lives: xor for zero when flags may be clobbered,
	 * the accumulator form when the value fits 32 bits, C7 /0 sign-extended
	 * otherwise, and movabs only when nothing shorter holds it. */
	{
		const s64 imm64s[] = { 0, 1, -1, 0x7fffffffLL, -0x80000000LL,
			0x80000000LL, 0xffffffffLL, 0x100000000LL, -0x80000001LL,
			0x123456789abcdefLL };
		for (int r = 0; r < 16; r++)
		{
			for (int s2 = 0; s2 < 16; s2++)
			{
				xMOV(xRegister32(r), xRegister32(s2));
				xMOV(xRegister64(r), xRegister64(s2));
				xMOV(xRegister16(r), xRegister16(s2));
				xTEST(xRegister32(r), xRegister32(s2));
			}
			for (int i = 0; i < 10; i++)
			{
				xMOV(xRegister64(r), (sptr)imm64s[i], false);
				xMOV(xRegister64(r), (sptr)imm64s[i], true);
				xMOV64(xRegister64(r), imm64s[i]);
			}
			for (int i = 0; i < nimm; i++)
				xTEST(xRegister32(r), imms[i]);
			xINC(xRegister32(r));
			xDEC(xRegister64(r));
			xINC(xRegister64(r));
			xDEC(xRegister32(r));
		}
		for (int b = 0; b < 8; b++)
		{
			const xAddressVoid m = xAddressVoid(xAddressReg(b), xAddressReg(6), 8, 0x24);
			xMOV(xRegister32(1), ptr32[m]);
			xMOV(ptr64[m], xRegister64(1));
			xMOV(xRegister64(2), ptrNative[kAddrs[b & 3]]);
			xMOV(ptr32[kAddrs[b & 3]], xRegister32(3));
		}
	}


	/* xDIV and xMUL: the single-operand group3 forms they inherit, the SSE
	 * members they carry, and the two- and three-operand IMUL forms. */
	for (int r = 0; r < 16; r++)
	{
		xDIV(xRegister32(r));
		xDIV(xRegister64(r));
		xMUL(xRegister32(r));
		for (int s2 = 0; s2 < 16; s2++)
		{
			xMUL(xRegister32(r), xRegister32(s2));
			xDIV.PS(xRegisterSSE(r), xRegisterSSE(s2));
			xDIV.SD(xRegisterSSE(r), xRegisterSSE(s2));
			xMUL.PD(xRegisterSSE(r), xRegisterSSE(s2));
			xMUL.SS(xRegisterSSE(r), xRegisterSSE(s2));
		}
		for (int i = 0; i < nimm; i++)
			xMUL(xRegister32(r), xRegister32((r + 3) & 15), imms[i]);
	}
	for (int b = 0; b < 8; b++)
	{
		const xAddressVoid m = xAddressVoid(xAddressReg(b), xAddressReg(4), 2, 0x14);
		xMUL(xRegister32(2), ptr32[m]);
		xMUL(xRegister32(2), ptr32[m], 0x1234);
		xDIV(ptr32[m]);
	}


	/* SSE aggregates: the logic ops, the compare family, the two shuffle
	 * families whose selectors are masked differently, and unpack. */
	for (int a = 0; a < 16; a++)
	{
		for (int b = 0; b < 16; b++)
		{
			xPAND(xRegisterSSE(a), xRegisterSSE(b));
			xPOR (xRegisterSSE(a), xRegisterSSE(b));
			xPXOR(xRegisterSSE(a), xRegisterSSE(b));
			xPCMP.EQB(xRegisterSSE(a), xRegisterSSE(b));
			xPCMP.EQW(xRegisterSSE(a), xRegisterSSE(b));
			xPCMP.EQD(xRegisterSSE(a), xRegisterSSE(b));
			xPCMP.GTB(xRegisterSSE(a), xRegisterSSE(b));
			xPCMP.GTW(xRegisterSSE(a), xRegisterSSE(b));
			xPCMP.GTD(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.LBW(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.LWD(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.LDQ(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.LQDQ(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.HBW(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.HWD(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.HDQ(xRegisterSSE(a), xRegisterSSE(b));
			xPUNPCK.HQDQ(xRegisterSSE(a), xRegisterSSE(b));
			xPSHUF.D (xRegisterSSE(a), xRegisterSSE(b), 0x1b);
			xPSHUF.LW(xRegisterSSE(a), xRegisterSSE(b), 0x55);
			xPSHUF.HW(xRegisterSSE(a), xRegisterSSE(b), 0xaa);
			xPSHUF.B (xRegisterSSE(a), xRegisterSSE(b));
		}
		xPAND(xRegisterSSE(a), ptr128[kAddrs[a & 3]]);
		xPXOR(xRegisterSSE(a), ptr128[kAddrs[a & 3]]);
		xPCMP.EQD(xRegisterSSE(a), ptr128[kAddrs[a & 3]]);
		xPSHUF.D(xRegisterSSE(a), ptr128[kAddrs[a & 3]], 0x39);
	}


	/* The Move families. The alignment rule is the point: a memory operand
	 * counts as aligned when isAligned is set OR when it is displacement-only
	 * and that displacement is 16-byte aligned. The choice then lands in the
	 * opcode for MOVAPS and in the prefix for MOVDQA, and the store forms
	 * differ again -- opcode+1 for one, a reversed-ModRM 0x7f for the other.
	 * Four independent ways to be wrong, so drive displacements that straddle
	 * the boundary in both displacement-only and base+index forms. */
	{
		const s32 mdisp[] = { 0, 4, 8, 0x0f, 0x10, 0x1f, 0x20, 0x100, 0x104, 0x110 };
		for (int r = 0; r < 16; r++)
		{
			for (int b = 0; b < 16; b++)
			{
				xMOVAPS(xRegisterSSE(r), xRegisterSSE(b));
				xMOVUPS(xRegisterSSE(r), xRegisterSSE(b));
				xMOVDQA(xRegisterSSE(r), xRegisterSSE(b));
				xMOVDQU(xRegisterSSE(r), xRegisterSSE(b));
			}
			for (int d = 0; d < 10; d++)
			{
				/* displacement-only: the alignment rule applies */
				void* const abs = (void*)(0x200001000ull + (unsigned)mdisp[d]);
				xMOVAPS(xRegisterSSE(r), ptr128[abs]);
				xMOVAPS(ptr128[abs], xRegisterSSE(r));
				xMOVUPS(xRegisterSSE(r), ptr128[abs]);
				xMOVUPS(ptr128[abs], xRegisterSSE(r));
				xMOVDQA(xRegisterSSE(r), ptr128[abs]);
				xMOVDQA(ptr128[abs], xRegisterSSE(r));
				xMOVDQU(xRegisterSSE(r), ptr128[abs]);
				xMOVDQU(ptr128[abs], xRegisterSSE(r));
				/* base+index: never counts as aligned, whatever the disp */
				const xAddressVoid m =
					xAddressVoid(xAddressReg(r & 7), xAddressReg(3), 4, mdisp[d]);
				xMOVAPS(xRegisterSSE(r), ptr128[m]);
				xMOVAPS(ptr128[m], xRegisterSSE(r));
				xMOVDQA(xRegisterSSE(r), ptr128[m]);
				xMOVDQA(ptr128[m], xRegisterSSE(r));
			}
		}
	}


	/* The tail: shuffle, compare-and-set-flags, packed multiply, the
	 * sign/zero-extending moves, and the two free functions. */
	for (int a = 0; a < 16; a++)
	{
		for (int b = 0; b < 16; b++)
		{
			xSHUF.PS(xRegisterSSE(a), xRegisterSSE(b), 0x1b);
			xSHUF.PD(xRegisterSSE(a), xRegisterSSE(b), 0xaa); /* masked to 2 bits */
			xUCOMI.SS(xRegisterSSE(a), xRegisterSSE(b));
			xUCOMI.SD(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.LW(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.HW(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.HUW(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.UDQ(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.HRSW(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.LD(xRegisterSSE(a), xRegisterSSE(b));
			xPMUL.DQ(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVSX.BW(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVSX.BD(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVSX.BQ(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVSX.WD(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVSX.WQ(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVSX.DQ(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVZX.BW(xRegisterSSE(a), xRegisterSSE(b));
			xPMOVZX.DQ(xRegisterSSE(a), xRegisterSSE(b));
			xMOVMSKPS(xRegister32(a), xRegisterSSE(b));
		}
		xLDMXCSR(ptr32[kAddrs[a & 3]]);
	}

	const size_t n = (size_t)(x86Ptr - buf);
	fprintf(stderr, "emitted %zu bytes\n", n);
	for (size_t i = 0; i < n; i++)
		printf("%02x%s", buf[i], (i % 32 == 31) ? "\n" : " ");
	printf("\n");
	return 0;
}
