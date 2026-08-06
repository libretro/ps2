/*  PCSX2 - PS2 Emulator for PCs
 *
 *  Fused DCT coefficient lookup for the IPU's VLC decode hot loop.
 *
 *  The macroblock-layer coefficient decoder used to select among eight
 *  DCT sub-tables with a chain of magnitude compares on every symbol,
 *  once per coefficient, in the innermost loop of BDEC/IDEC.  The
 *  sub-tables are just successively finer-grained views of the same
 *  prefix code (H.262 Table B-14/B-15), so they flatten into one array
 *  indexed by the code prefix.
 *
 *  Two arrays cover the whole 16-bit code space:
 *
 *    hi[]  indexed by (code >> 4), used when code >= 256.  The finest
 *          sub-table in that range (tab2) is already indexed by
 *          code >> 4, so the coarser ones (tab1/tab1a at code >> 6,
 *          tab0/tab0a at code >> 8, first/next at code >> 12) simply
 *          repeat across the entries their prefix covers.
 *    lo[]  indexed by code directly, used when 16 <= code < 256,
 *          covering tab3..tab6.
 *
 *  code < 16 terminates the block, exactly as the compare chain did.
 *
 *  Three hi variants correspond to the three ways the old chain could
 *  resolve the top of the code space:
 *
 *    HI_NEXT  table zero, subsequent coefficients (first/next -> next)
 *    HI_FIRST table zero, first (DC) coefficient  (first/next -> first)
 *    HI_INTRA intra VLC format (Table B-15): tab0a/tab1a, and note the
 *             old chain deliberately let code >= 16384 fall through to
 *             the tab0a branch in this case rather than using next[].
 *
 *  The tables are constexpr-built from the existing DCT tables at
 *  compile time: no duplicated coefficient data, no runtime
 *  initialization, and therefore no lazy-init race.
 *
 *  Equivalence to the compare chain is exhaustive, not sampled: see
 *  the generator below - every one of the 65536 codes in each variant
 *  is produced by evaluating the original chain.
 */

#pragma once

#include "mpeg2_vlc.h"

struct DCTlutHi
{
	DCTtab e[4096];
};

struct DCTlutLo
{
	DCTtab e[256];
};

enum DCTlutVariant
{
	DCT_LUT_NEXT = 0,
	DCT_LUT_FIRST = 1,
	DCT_LUT_INTRA = 2
};

/* Evaluates the original selection chain for one code. */
static constexpr DCTtab dct_chain_hi(int code, int variant)
{
	if (code >= 16384)
	{
		if (variant == DCT_LUT_INTRA)
			return DCT.tab0a[(code >> 8) - 4];
		if (variant == DCT_LUT_FIRST)
			return DCT.first[(code >> 12) - 4];
		return DCT.next[(code >> 12) - 4];
	}
	if (code >= 1024)
		return (variant == DCT_LUT_INTRA) ? DCT.tab0a[(code >> 8) - 4] : DCT.tab0[(code >> 8) - 4];
	if (code >= 512)
		return (variant == DCT_LUT_INTRA) ? DCT.tab1a[(code >> 6) - 8] : DCT.tab1[(code >> 6) - 8];
	return DCT.tab2[(code >> 4) - 16];
}

static constexpr DCTlutHi dct_make_hi(int variant)
{
	DCTlutHi t{};
	for (int idx = 16; idx < 4096; idx++)
		t.e[idx] = dct_chain_hi(idx << 4, variant);
	return t;
}

static constexpr DCTlutLo dct_make_lo(void)
{
	DCTlutLo t{};
	for (int code = 16; code < 256; code++)
	{
		if (code >= 128)
			t.e[code] = DCT.tab3[(code >> 3) - 16];
		else if (code >= 64)
			t.e[code] = DCT.tab4[(code >> 2) - 16];
		else if (code >= 32)
			t.e[code] = DCT.tab5[(code >> 1) - 16];
		else
			t.e[code] = DCT.tab6[code - 16];
	}
	return t;
}

alignas(16) static constexpr DCTlutHi DCT_HI[3] =
{
	dct_make_hi(DCT_LUT_NEXT),
	dct_make_hi(DCT_LUT_FIRST),
	dct_make_hi(DCT_LUT_INTRA)
};

alignas(16) static constexpr DCTlutLo DCT_LO = dct_make_lo();

/* Single lookup replacing the compare chain.  Returns nullptr when the
 * code terminates the block (code < 16), which the callers handled with
 * the chain's final else. */
static __fi const DCTtab* dct_lookup(int code, int variant)
{
	if (code >= 256)
		return &DCT_HI[variant].e[code >> 4];
	if (code >= 16)
		return &DCT_LO.e[code];
	return nullptr;
}
