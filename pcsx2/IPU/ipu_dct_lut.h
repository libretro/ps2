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
 *  The tables are constexpr-built at compile time from the standard's
 *  own code assignments in ipu_dct_codes.h: no runtime initialization,
 *  and therefore no lazy-init race.
 *
 *  Equivalence is exhaustive, not sampled: every one of the 65536
 *  codes in each variant resolves to the same {run, level, len} the
 *  previous table layout produced.
 */

#pragma once

#include "ipu_dct_codes.h"

struct DCTtabLut
{
	uint8_t run;
	uint8_t level;
	uint8_t len;
};

struct DCTlutHi
{
	DCTtabLut e[4096];
};

struct DCTlutLo
{
	DCTtabLut e[256];
};

enum DCTlutVariant
{
	DCT_LUT_NEXT = 0,
	DCT_LUT_FIRST = 1,
	DCT_LUT_INTRA = 2
};

/* Codes of 12 bits or fewer land in hi[], indexed by the top 12 bits of
 * the 16-bit window; each code fills the span of indices its prefix
 * covers.  Longer codes have at least eight leading zeros, so their
 * 16-bit window value is below 256 and they land in lo[], indexed by
 * that value directly. */
template <int N>
static constexpr void dct_fill(DCTlutHi& hi, DCTlutLo& lo, const DCTcode (&codes)[N])
{
	for (int c = 0; c < N; c++)
	{
		const DCTtabLut v = {codes[c].run, codes[c].level, codes[c].len};
		if (codes[c].len <= 12)
		{
			const int shift = 12 - codes[c].len;
			const int start = codes[c].bits << shift;
			for (int idx = start; idx < start + (1 << shift); idx++)
			{
				if (idx >= 16)
					hi.e[idx] = v;
			}
		}
		else
		{
			const int shift = 16 - codes[c].len;
			const int start = codes[c].bits << shift;
			for (int code = start; code < start + (1 << shift); code++)
			{
				if (code >= 16 && code < 256)
					lo.e[code] = v;
			}
		}
	}
}

struct DCTlut
{
	DCTlutHi hi[3];
	DCTlutLo lo;
};

static constexpr DCTlut dct_make_lut(void)
{
	DCTlut t{};
	dct_fill(t.hi[DCT_LUT_NEXT], t.lo, DCT_CODES_NEXT);
	dct_fill(t.hi[DCT_LUT_FIRST], t.lo, DCT_CODES_FIRST);
	dct_fill(t.hi[DCT_LUT_INTRA], t.lo, DCT_CODES_INTRA);
	return t;
}

alignas(16) static constexpr DCTlut DCT_LUT = dct_make_lut();

/* Single lookup replacing the old eight-way compare chain.  Returns
 * nullptr when the code terminates the block, as the chain's final else
 * did. */
static __fi const DCTtabLut* dct_lookup(int code, int variant)
{
	if (code >= 256)
		return &DCT_LUT.hi[variant].e[code >> 4];
	if (code >= 16)
		return &DCT_LUT.lo.e[code];
	return nullptr;
}
