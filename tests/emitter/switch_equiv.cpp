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
	xSetPtr(buf);

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

	const size_t n = (size_t)(xGetPtr() - buf);
	fprintf(stderr, "emitted %zu bytes\n", n);
	for (size_t i = 0; i < n; i++)
		printf("%02x%s", buf[i], (i % 32 == 31) ? "\n" : " ");
	printf("\n");
	return 0;
}
