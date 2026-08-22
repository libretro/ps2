#include "../GS/GSVector.h"
#include "Global.h"

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
s32 __forceinline ReverbDownsample_avx(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector8i::load<true>(&filter_down_coefs[0]);
	auto s = GSVector8i::load<false>(&core.RevbDownBuf[right][index]);
	auto acc = s.mul16hrs(c);

	c = GSVector8i::load<true>(&filter_down_coefs[16]);
	s = GSVector8i::load<false>(&core.RevbDownBuf[right][index + 16]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector8i::load<true>(&filter_down_coefs[32]);
	s = GSVector8i::load<false>(&core.RevbDownBuf[right][index + 32]);
	acc = acc.adds16(s.mul16hrs(c));

	acc = acc.adds16(acc.ba());

	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);

	return acc.I16[0];
}
#endif

s32 __forceinline ReverbDownsample_sse(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector4i::load<true>(&filter_down_coefs[0]);
	auto s = GSVector4i::load<false>(&core.RevbDownBuf[right][index]);
	auto acc = s.mul16hrs(c);

	c = GSVector4i::load<true>(&filter_down_coefs[8]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 8]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[16]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 16]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[24]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 24]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[32]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 32]);
	acc = acc.adds16(s.mul16hrs(c));

	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);

	return acc.I16[0];
}

s32 ReverbDownsample(V_Core& core, bool right)
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
StereoOut32 __forceinline ReverbUpsample_avx(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector8i::load<true>(&filter_up_coefs[0]);
	auto l = GSVector8i::load<false>(&core.RevbUpBuf[0][index]);
	auto r = GSVector8i::load<false>(&core.RevbUpBuf[1][index]);

	auto lacc = l.mul16hrs(c);
	auto racc = r.mul16hrs(c);

	c = GSVector8i::load<true>(&filter_up_coefs[16]);
	l = GSVector8i::load<false>(&core.RevbUpBuf[0][index + 16]);
	r = GSVector8i::load<false>(&core.RevbUpBuf[1][index + 16]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector8i::load<true>(&filter_up_coefs[32]);
	l = GSVector8i::load<false>(&core.RevbUpBuf[0][index + 32]);
	r = GSVector8i::load<false>(&core.RevbUpBuf[1][index + 32]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	lacc = lacc.adds16(lacc.ba());
	racc = racc.adds16(racc.ba());

	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);

	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);

	return {lacc.I16[0], racc.I16[0]};
}
#endif

StereoOut32 __forceinline ReverbUpsample_sse(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector4i::load<true>(&filter_up_coefs[0]);
	auto l = GSVector4i::load<false>(&core.RevbUpBuf[0][index]);
	auto r = GSVector4i::load<false>(&core.RevbUpBuf[1][index]);

	auto lacc = l.mul16hrs(c);
	auto racc = r.mul16hrs(c);

	c = GSVector4i::load<true>(&filter_up_coefs[8]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 8]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 8]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[16]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 16]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 16]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[24]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 24]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 24]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[32]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 32]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 32]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);

	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);

	return {lacc.I16[0], racc.I16[0]};
}

StereoOut32 ReverbUpsample(V_Core& core)
{
	make_up_coefs();
#if _M_SSE >= 0x501
	return ReverbUpsample_avx(core);
#else
	return ReverbUpsample_sse(core);
#endif
}

MULTI_ISA_UNSHARED_END
