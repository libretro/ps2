/* Oracle suite for the GS packed-vertex parse kernels (GS/GSVertexKick.h).
 *
 * Each kernel is checked against an independent scalar model of the GIF/GS
 * semantics, written in plain integer C with no GSVector, over directed edge
 * cases plus randomized sweeps. The two sides can only agree by both being
 * right, which is the point: an optimized kernel that lands later is held to
 * the same model rather than to a recording of the current implementation.
 *
 * Ported from yaps2's GV-0 oracle (gs_vertex_tests.cpp) into this tree's
 * standalone-test style -- no gtest, exits non-zero on the first mismatch.
 *
 * Build: sh tests/gs/build.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/GSVertexKick.h"

typedef struct
{
	u32 m0[4];
	u32 m1[4];
} RefVertex;

static u32 Rd32(const u8* rec, int dword)
{
	u32 v;
	memcpy(&v, rec + dword * 4, 4);
	return v;
}

/* Scalar model of packed {STQ, RGBAQ, XYZF2}. rec is 48 bytes (3 qwords). */
static RefVertex RefParseXYZF2(const u8* rec, u32 uv)
{
	RefVertex o;
	u32 q;

	o.m0[0] = Rd32(rec, 0); /* S */
	o.m0[1] = Rd32(rec, 1); /* T */
	o.m0[2] = (Rd32(rec, 4) & 0xff) | ((Rd32(rec, 5) & 0xff) << 8) |
	          ((Rd32(rec, 6) & 0xff) << 16) | ((Rd32(rec, 7) & 0xff) << 24); /* RGBA */

	/* Q is compared as an integer against +0.0, so -0.0 passes through. */
	q = Rd32(rec, 2);
	o.m0[3] = (q == 0) ? 0x00800000u : q;

	o.m1[0] = (Rd32(rec, 8) & 0xffff) | ((Rd32(rec, 9) & 0xffff) << 16); /* X | Y<<16 */
	o.m1[1] = (Rd32(rec, 10) >> 4) & 0x00ffffffu;                        /* Z, 24-bit */
	o.m1[2] = uv;
	o.m1[3] = (Rd32(rec, 11) >> 4) & 0xffu;                              /* F */
	return o;
}

/* Same, for XYZ2: Z keeps all 32 bits, and UV/FOG both come from vertex state. */
static RefVertex RefParseXYZ2(const u8* rec, u64 uvfog)
{
	RefVertex o = RefParseXYZF2(rec, (u32)uvfog);
	o.m1[1] = Rd32(rec, 10);
	o.m1[3] = (u32)(uvfog >> 32);
	return o;
}

static u64 g_seed = 0x243f6a8885a308d3ull;

static u32 Rand32(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 7;
	g_seed ^= g_seed << 17;
	return (u32)(g_seed >> 16);
}

static unsigned long g_checked;
static unsigned long g_failed;

static void Compare(const char* what, const u32* got, const RefVertex* want, const u8* rec)
{
	int i;
	int bad = 0;

	for (i = 0; i < 4; i++)
		if (got[i] != want->m0[i] || got[4 + i] != want->m1[i])
			bad = 1;

	g_checked++;
	if (!bad)
		return;

	g_failed++;
	if (g_failed <= 5)
	{
		printf("MISMATCH %s\n", what);
		for (i = 0; i < 4; i++)
			printf("  m0[%d] kernel=%08x model=%08x%s\n", i, got[i], want->m0[i],
			       got[i] != want->m0[i] ? "   <<<" : "");
		for (i = 0; i < 4; i++)
			printf("  m1[%d] kernel=%08x model=%08x%s\n", i, got[4 + i], want->m1[i],
			       got[4 + i] != want->m1[i] ? "   <<<" : "");
		printf("  record:");
		for (i = 0; i < 12; i++)
			printf(" %08x", Rd32(rec, i));
		printf("\n");
	}
}

static void RunXYZF2(const u8* rec, u32 uv)
{
	alignas(16) GSVertexKernels::GSVertexNative out[2];
	RefVertex want;

	GSVertexKernels::ParsePackedSTQRGBAXYZF2((const GIFPackedReg*)rec, uv, out);
	want = RefParseXYZF2(rec, uv);
	Compare("STQRGBAXYZF2", (const u32*)out, &want, rec);
}

static void RunXYZ2(const u8* rec, u64 uvfog)
{
	alignas(16) GSVertexKernels::GSVertexNative out[2];
	RefVertex want;

	GSVertexKernels::ParsePackedSTQRGBAXYZ2((const GIFPackedReg*)rec, &uvfog, out);
	want = RefParseXYZ2(rec, uvfog);
	Compare("STQRGBAXYZ2", (const u32*)out, &want, rec);
}

static void SetDword(u8* rec, int dword, u32 v)
{
	memcpy(rec + dword * 4, &v, 4);
}

int main(void)
{
	alignas(16) u8 rec[48];
	u64 uvfog;
	u32 uv;
	int i;
	long n;

	/* ---- directed edges ---------------------------------------------- */

	/* Q == +0.0 must become FLT_MIN; Q == -0.0 must pass through unchanged.
	 * The distinction is why the kernel compares Q as an integer. */
	memset(rec, 0, sizeof(rec));
	SetDword(rec, 2, 0x00000000u);
	RunXYZF2(rec, 0);
	SetDword(rec, 2, 0x80000000u);
	RunXYZF2(rec, 0);

	/* Sub-texel boundaries: XY carry 4 fractional bits, Z and F are shifted
	 * down by 4, so the low nibbles must not survive into the result. */
	memset(rec, 0, sizeof(rec));
	SetDword(rec, 8, 0x0000ffffu);
	SetDword(rec, 9, 0x0000ffffu);
	SetDword(rec, 10, 0xffffffffu);
	SetDword(rec, 11, 0xffffffffu);
	RunXYZF2(rec, 0xdeadbeefu);
	RunXYZ2(rec, 0x1122334455667788ull);

	/* All-ones and alternating patterns through every field. */
	memset(rec, 0xff, sizeof(rec));
	RunXYZF2(rec, 0xffffffffu);
	RunXYZ2(rec, 0xffffffffffffffffull);
	memset(rec, 0xaa, sizeof(rec));
	RunXYZF2(rec, 0x55555555u);
	RunXYZ2(rec, 0x5555555555555555ull);

	/* RGBA is gathered from the low byte of four separate dwords: prove the
	 * other 24 bits of each are ignored. */
	memset(rec, 0, sizeof(rec));
	SetDword(rec, 4, 0xffffff01u);
	SetDword(rec, 5, 0xffffff02u);
	SetDword(rec, 6, 0xffffff03u);
	SetDword(rec, 7, 0xffffff04u);
	RunXYZF2(rec, 0);

	/* ---- randomized sweep -------------------------------------------- */

	for (n = 0; n < 2000000; n++)
	{
		for (i = 0; i < 12; i++)
			SetDword(rec, i, Rand32());

		/* Bias Q toward the zero cases the fixup exists for. */
		if ((n & 7) == 0)
			SetDword(rec, 2, (n & 8) ? 0x80000000u : 0x00000000u);

		uv = Rand32();
		uvfog = ((u64)Rand32() << 32) | Rand32();

		RunXYZF2(rec, uv);
		RunXYZ2(rec, uvfog);
	}

	printf("gs_vertex_oracle: %lu comparisons, %lu mismatches\n", g_checked, g_failed);
	return g_failed ? 1 : 0;
}
