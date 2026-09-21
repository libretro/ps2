#include "Global.h"

extern "C" {
#include "../GS/gs_vector.h"
}

/* The 256-bit paths stay on raw intrinsics: gs_vector is a 128-bit
 * header, and AVX2 is the only place a 16-lane form exists at all. */
#if _M_SSE >= 0x501
#include <immintrin.h>
#endif

MULTI_ISA_UNSHARED_START

static constexpr u32 NUM_TAPS = 39;
// 39 tap filter, the 0's could be optimized out
alignas(32) static const s16 filter_down_coefs[48] = {
	-1,
	0,
	2,
	0,
	-10,
	0,
	35,
	0,
	-103,
	0,
	266,
	0,
	-616,
	0,
	1332,
	0,
	-2960,
	0,
	10246,
	16384,
	10246,
	0,
	-2960,
	0,
	1332,
	0,
	-616,
	0,
	266,
	0,
	-103,
	0,
	35,
	0,
	-10,
	0,
	2,
	0,
	-1,
};

/* The up coefficients are the down coefficients doubled and clamped. This
 * was a constexpr std::array returned by value from make_up_coefs(); it is
 * filled once at startup instead, since a C array cannot be returned. */
alignas(32) static s16 filter_up_coefs[48];

static void make_up_coefs(void)
{
	static int done = 0;
	u32 i;

	if (done)
		return;
	done = 1;

	for (i = 0; i < NUM_TAPS; i++)
	{
		const s32 v = (s32)filter_down_coefs[i] * 2;
		filter_up_coefs[i] = (s16)pcsx2_clamp_i(v, INT16_MIN, INT16_MAX);
	}
}

/*
 * Reverb resampling is a 39-tap symmetric FIR (filter_down_coefs /
 * filter_up_coefs, Q15) converting between the core rate and the reverb rate.
 * The SPU2 evaluates it on a 16-bit saturating accumulator, so the SIMD paths
 * below are the authoritative implementations: each tap is a rounding
 * fixed-point multiply (mul16hrs) and the running sum saturates at every step
 * (adds16 / hadds16). A naive full-precision scalar accumulate, i.e.
 *
 *     s32 out = 0;
 *     for (i = 0; i < NUM_TAPS; i++)
 *         out += RevbDownBuf[right][index + i] * filter_down_coefs[i];
 *     out = clamp(out >> 15);
 *
 * reads more clearly but is NOT bit-exact: it drops the intermediate
 * saturation and rounds differently, diverging by up to ~7 LSB. It is
 * therefore intentionally not kept as a callable "reference". The SSE and AVX
 * paths are bit-identical to each other.
 */

#if _M_SSE >= 0x501
s32 __forceinline ReverbDownsample_avx(V_Core *core, bool right)
{
	__m256i acc, c, s;
	const s16 *buf = &core->RevbDownBuf[right][(core->RevbSampleBufPos - NUM_TAPS) & 63];

#define TAP(k) _mm256_mulhrs_epi16( \
                  _mm256_loadu_si256((const __m256i *)&buf[k]), \
                  _mm256_load_si256((const __m256i *)&filter_down_coefs[k]))
	acc = TAP(0);
	acc = _mm256_adds_epi16(acc, TAP(16));
	acc = _mm256_adds_epi16(acc, TAP(32));
#undef TAP

	/* fold the upper half onto the lower, then the lower onto itself */
	acc = _mm256_adds_epi16(acc, _mm256_permute2x128_si256(acc, acc, 0x01));
	acc = _mm256_hadds_epi16(acc, acc);
	acc = _mm256_hadds_epi16(acc, acc);
	acc = _mm256_hadds_epi16(acc, acc);

	return (s16)_mm_extract_epi16(_mm256_castsi256_si128(acc), 0);
}
#endif

s32 __forceinline ReverbDownsample_sse(V_Core *core, bool right)
{
	union gs_v4i_view out;
	gs_vec4i acc;
	const s16 *buf = &core->RevbDownBuf[right][(core->RevbSampleBufPos - NUM_TAPS) & 63];

	/* Written out rather than looped: the accumulator chain saturates, so
	 * the order has to stand, and a loop here costs the four multiplies
	 * their overlap. */
#define TAP(k) gs_v4i_mul16hrs(gs_v4i_loadu(&buf[k]), \
                               gs_v4i_load(&filter_down_coefs[k]))
	acc = TAP(0);
	acc = gs_v4i_adds16(acc, TAP(8));
	acc = gs_v4i_adds16(acc, TAP(16));
	acc = gs_v4i_adds16(acc, TAP(24));
	acc = gs_v4i_adds16(acc, TAP(32));
#undef TAP

	acc = gs_v4i_hadds16(acc, acc);
	acc = gs_v4i_hadds16(acc, acc);
	acc = gs_v4i_hadds16(acc, acc);

	out.v = acc;
	return out.i16[0];
}

s32 ReverbDownsample(V_Core *core, bool right)
{
#if _M_SSE >= 0x501
	return ReverbDownsample_avx(core, right);
#else
	return ReverbDownsample_sse(core, right);
#endif
}

/*
 * Upsample counterpart of the FIR above (filter_up_coefs = filter_down_coefs*2,
 * the 2x gain compensating for zero-stuffing). Same reasoning as the downsample
 * comment: the SIMD paths below are the hardware-faithful, saturating
 * implementation; a full-precision scalar accumulate would not be bit-exact.
 */

#if _M_SSE >= 0x501
StereoOut32 __forceinline ReverbUpsample_avx(V_Core *core)
{
	__m256i lacc, racc;
	StereoOut32 ret;
	const int index = (core->RevbSampleBufPos - NUM_TAPS) & 63;
	const s16 *lbuf = &core->RevbUpBuf[0][index];
	const s16 *rbuf = &core->RevbUpBuf[1][index];

#define TAP(b, k) _mm256_mulhrs_epi16( \
                     _mm256_loadu_si256((const __m256i *)&(b)[k]), \
                     _mm256_load_si256((const __m256i *)&filter_up_coefs[k]))
	lacc = TAP(lbuf, 0);
	racc = TAP(rbuf, 0);
	lacc = _mm256_adds_epi16(lacc, TAP(lbuf, 16));
	racc = _mm256_adds_epi16(racc, TAP(rbuf, 16));
	lacc = _mm256_adds_epi16(lacc, TAP(lbuf, 32));
	racc = _mm256_adds_epi16(racc, TAP(rbuf, 32));
#undef TAP

	lacc = _mm256_adds_epi16(lacc, _mm256_permute2x128_si256(lacc, lacc, 0x01));
	racc = _mm256_adds_epi16(racc, _mm256_permute2x128_si256(racc, racc, 0x01));

	lacc = _mm256_hadds_epi16(lacc, lacc);
	lacc = _mm256_hadds_epi16(lacc, lacc);
	lacc = _mm256_hadds_epi16(lacc, lacc);

	racc = _mm256_hadds_epi16(racc, racc);
	racc = _mm256_hadds_epi16(racc, racc);
	racc = _mm256_hadds_epi16(racc, racc);

	ret.Left  = (s16)_mm_extract_epi16(_mm256_castsi256_si128(lacc), 0);
	ret.Right = (s16)_mm_extract_epi16(_mm256_castsi256_si128(racc), 0);
	return ret;
}
#endif

StereoOut32 __forceinline ReverbUpsample_sse(V_Core *core)
{
	union gs_v4i_view lo, ro;
	gs_vec4i lacc, racc;
	StereoOut32 ret;
	const int index = (core->RevbSampleBufPos - NUM_TAPS) & 63;
	const s16 *lbuf = &core->RevbUpBuf[0][index];
	const s16 *rbuf = &core->RevbUpBuf[1][index];

#define TAP(b, k) gs_v4i_mul16hrs(gs_v4i_loadu(&(b)[k]), \
                                  gs_v4i_load(&filter_up_coefs[k]))
	lacc = TAP(lbuf, 0);
	racc = TAP(rbuf, 0);
	lacc = gs_v4i_adds16(lacc, TAP(lbuf, 8));
	racc = gs_v4i_adds16(racc, TAP(rbuf, 8));
	lacc = gs_v4i_adds16(lacc, TAP(lbuf, 16));
	racc = gs_v4i_adds16(racc, TAP(rbuf, 16));
	lacc = gs_v4i_adds16(lacc, TAP(lbuf, 24));
	racc = gs_v4i_adds16(racc, TAP(rbuf, 24));
	lacc = gs_v4i_adds16(lacc, TAP(lbuf, 32));
	racc = gs_v4i_adds16(racc, TAP(rbuf, 32));
#undef TAP

	lacc = gs_v4i_hadds16(lacc, lacc);
	lacc = gs_v4i_hadds16(lacc, lacc);
	lacc = gs_v4i_hadds16(lacc, lacc);

	racc = gs_v4i_hadds16(racc, racc);
	racc = gs_v4i_hadds16(racc, racc);
	racc = gs_v4i_hadds16(racc, racc);

	lo.v = lacc;
	ro.v = racc;
	ret.Left  = lo.i16[0];
	ret.Right = ro.i16[0];
	return ret;
}

StereoOut32 ReverbUpsample(V_Core *core)
{
	make_up_coefs();
#if _M_SSE >= 0x501
	return ReverbUpsample_avx(core);
#else
	return ReverbUpsample_sse(core);
#endif
}

MULTI_ISA_UNSHARED_END
