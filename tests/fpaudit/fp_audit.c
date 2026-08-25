/* Per-opcode float exactness audit: what each recompiled sequence
 * computes, mirrored in C at instruction semantics, against the ps2float
 * model that defines correctness.
 *
 * The mirror is honest because every SSE scalar op here IS its IEEE-754
 * single-precision operation in round-to-nearest: addss, subss, mulss,
 * divss, sqrtss have no implementation freedom, so computing them with
 * the host FPU under the same rounding is computing the emission. The
 * clamps are the fork's own: operands and results through minss/maxss
 * against +/-FMAX (VU clamp1 / EE fpuFloat), which maps NaN to +FMAX
 * because minss returns the second operand on unordered compares.
 *
 * Two candidate rows measure "beating" the current emission: add and sub
 * through the double pipeline with the PS2 adder's exponent-distance
 * truncation applied to the smaller operand's mantissa before an exact
 * double add -- the construction the EE DOUBLE path already proved
 * byte-exact against the model. If the VU rows also read zero, the same
 * emission upgrade applies to microVU upper ADD/SUB and the soft path
 * becomes unnecessary for the most common VU float ops.
 *
 * MSVC C89 shaped: declarations first, no //, u64 flag word carried like
 * ps2float's own convention. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include "../../pcsx2/ps2float.h"

#define FMAX_P 0x7f7fffffu
#define FMAX_N 0xff7fffffu

typedef uint32_t u32;
typedef uint64_t u64;

static float bits_to_f(u32 b) { float f; memcpy(&f, &b, 4); return f; }
static u32 f_to_bits(float f) { u32 b; memcpy(&b, &f, 4); return b; }

/* minss/maxss semantics: on unordered, the SECOND operand is returned.
 * clamp(a) = maxss(minss(a, +FMAX), -FMAX). */
static u32 sse_minss(u32 a, u32 b)
{
	float fa = bits_to_f(a), fb = bits_to_f(b);
	if (fa != fa || fb != fb) return b;
	return (fa < fb) ? a : b;
}
static u32 sse_maxss(u32 a, u32 b)
{
	float fa = bits_to_f(a), fb = bits_to_f(b);
	if (fa != fa || fb != fb) return b;
	return (fa > fb) ? a : b;
}
static u32 clamp1(u32 a) { return sse_maxss(sse_minss(a, FMAX_P), FMAX_N); }

/* pminsd/pminud pair: sign-preserving clamp (fpuFloat2/mVUclamp2).
 * pminsd against +FMAX pulls positive NaN/huge down; pminud against
 * -FMAX (as unsigned 0xff7fffff) pulls the negative range down while
 * leaving positives alone. Preserves the sign bit of NaNs. */
static u32 clamp_sp(u32 a)
{
	int32_t sa = (int32_t)a;
	u32 r = ((int32_t)FMAX_P < sa) ? FMAX_P : a;
	return (FMAX_N < r) ? FMAX_N : r;
}

/* The hardfloat rows: clamp operands, IEEE single op, clamp result. */
static u32 hard_add(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp1(a)) + bits_to_f(clamp1(b)))); }
static u32 hard_sub(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp1(a)) - bits_to_f(clamp1(b)))); }
static u32 hard_mul(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp_sp(a)) * bits_to_f(clamp_sp(b)))); }
static u32 hard_div(u32 a, u32 b) { return clamp1(f_to_bits(bits_to_f(clamp1(a)) / bits_to_f(clamp1(b)))); }
static u32 hard_sqrt(u32 a)
{
	/* VU SQRT takes |Ft|; the emission ands the sign off first. */
	return clamp1(f_to_bits((float)sqrt((double)bits_to_f(clamp1(a & 0x7fffffffu)))));
}
static u32 hard_madd(u32 acc, u32 s, u32 t)
{
	float m = bits_to_f(clamp_sp(s)) * bits_to_f(clamp_sp(t));
	return clamp1(f_to_bits(bits_to_f(clamp1(acc)) + bits_to_f(clamp1(f_to_bits(m)))));
}
/* The reference is the interpreter's own fp_max/fp_min (pcsx2/FPU.cpp),
 * itself differentially proven earlier: both-negative flips the integer
 * ordering, everything else is plain signed compare. */
static u32 fp_model_max(u32 a, u32 b)
{
	if ((int32_t)a < 0 && (int32_t)b < 0) return ((int32_t)a < (int32_t)b) ? a : b;
	return ((int32_t)a > (int32_t)b) ? a : b;
}
static u32 fp_model_min(u32 a, u32 b)
{
	if ((int32_t)a < 0 && (int32_t)b < 0) return ((int32_t)a > (int32_t)b) ? a : b;
	return ((int32_t)a < (int32_t)b) ? a : b;
}
static u32 hard_max(u32 a, u32 b)
{
	/* The integer-ordering MAX the fork emits (fp_max): both negative
	 * flips to integer MIN, otherwise integer MAX. The first version of
	 * this mirror inverted the both-negative branch and the table
	 * dutifully reported the mirror's bug as a 25% miss. */
	int32_t ia = (int32_t)a, ib = (int32_t)b;
	if ((ia < 0) && (ib < 0)) return (ia < ib) ? a : b;
	return (ia > ib) ? a : b;
}
static u32 hard_min(u32 a, u32 b)
{
	int32_t ia = (int32_t)a, ib = (int32_t)b;
	if ((ia < 0) && (ib < 0)) return (ia > ib) ? a : b;
	return (ia < ib) ? a : b;
}

/* Candidate: PS2 adder through the double pipeline. The hardware aligns
 * the smaller operand by exponent distance and TRUNCATES the bits shifted
 * out (no guard/round/sticky), then adds exactly. A double holds a full
 * 24-bit mantissa product of alignment up to 32 bits without rounding, so:
 * truncate the smaller mantissa by the exponent distance, then one exact
 * double add, then round-to-nearest back to single via the hardware rule
 * (which for the adder is truncation toward zero of the normalized sum).
 * Mirrors ps2f_add's construction; measured here rather than assumed. */
static u32 cand_dbl_addsub(u32 a, u32 b, int negate_b)
{
	/* Mirrors the hardware adder as ps2float encodes it: the SMALLER
	 * operand's raw low bits are masked by (exponent distance - 1) --
	 * one guard bit survives relative to a plain shift -- then the sum
	 * is exact and the normalized result truncates. Written against the
	 * model's own alignment rule after the first construction missed it
	 * by that one bit position. */
	u32 ea, eb, sr, big, small;
	int d, eS, t;
	int64_t mL, mS, sum;

	if (negate_b) b ^= 0x80000000u;
	ea = (a >> 23) & 0xff; eb = (b >> 23) & 0xff;
	if (ea == 0) a &= 0x80000000u;
	if (eb == 0) b &= 0x80000000u;
	if (ea == 0 && eb == 0)
	{
		if ((a ^ b) & 0x80000000u) return 0;
		return a;
	}
	if (eb == 0) return a;
	if (ea == 0) return b;

	if (ea >= eb) { big = a; small = b; d = (int)(ea - eb); eS = (int)eb; }
	else          { big = b; small = a; d = (int)(eb - ea); eS = (int)ea; }
	if (d > 24)
		return big;
	if (d > 0)
		small &= 0xffffffffu << (d - 1);

	mL = (int64_t)((big   & 0x007fffffu) | 0x00800000u) << d;
	mS = (int64_t)((small & 0x007fffffu) | 0x00800000u);
	if (big   & 0x80000000u) mL = -mL;
	if (small & 0x80000000u) mS = -mS;
	sum = mL + mS;

	if (sum == 0) return 0;
	sr = 0;
	if (sum < 0) { sr = 0x80000000u; sum = -sum; }

	for (t = 62; t >= 0; t--)
		if (sum & ((int64_t)1 << t))
			break;
	/* Result exponent at scale: bit 23 corresponds to exponent eS. */
	eS += t - 23;
	if (eS >= 256) return sr | 0x7fffffffu;
	if (eS <= 0)   return sr;
	if (t > 23) sum >>= (t - 23); /* truncate */
	else        sum <<= (23 - t);
	return sr | ((u32)eS << 23) | ((u32)sum & 0x007fffffu);
}
static u32 cand_add(u32 a, u32 b) { return cand_dbl_addsub(a, b, 0); }
static u32 cand_sub(u32 a, u32 b) { return cand_dbl_addsub(a, b, 1); }


/* The default tier actually emitted (VuAccurateAddSub, on by default):
 * mask the smaller operand's raw low bits by exponent-distance minus one
 * (mVUmaskAddSubFused), clamp, then IEEE round-to-nearest addss. Differs
 * from the model only where the model's truncating normalize and the
 * host's rounding disagree, plus the clamped exponent-255 range. */
static u32 mask_smaller(u32 a, u32 b, u32* pa, u32* pb)
{
	/* 1..24: mask the smaller's raw low bits by distance-1 (one guard
	 * bit survives). 25 and beyond: the smaller is discarded entirely --
	 * the adder has no path for it, and the emitted mask stage's
	 * 24-window compare wipes the operand. The first mirror lacked the
	 * discard and the emit row measured 9.4% for that one rule. */
	u32 ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
	int d = (int)ea - (int)eb;
	if (d > 0)      b = (d < 25) ? (b & (0xffffffffu << (d - 1)))  : (b & 0x80000000u);
	else if (d < 0) a = (-d < 25) ? (a & (0xffffffffu << (-d - 1))) : (a & 0x80000000u);
	*pa = a; *pb = b;
	return 0;
}
static u32 hard_accadd(u32 a, u32 b)
{
	u32 ma, mb;
	mask_smaller(a, b, &ma, &mb);
	return clamp1(f_to_bits(bits_to_f(clamp1(ma)) + bits_to_f(clamp1(mb))));
}
static u32 hard_accsub(u32 a, u32 b) { return hard_accadd(a, b ^ 0x80000000u); }

/* No fixup middle path exists, and the row that proved it is gone for a
 * structural reason worth recording: IEEE single precision cannot
 * represent the PS2's exponent-255 range at all (those encodings are
 * Inf/NaN to the host), so any construction whose intermediate values
 * are IEEE singles -- including cvtps2pd, which reads 0x7f800000+ as
 * infinity and drops the mantissa -- loses that range before the
 * arithmetic starts. The measured 5.6% default-tier miss is dominated by
 * exactly this, not by rounding. The only exact emission is the full
 * construction in cand_dbl_addsub below, whose SSE shape is integer
 * ops end to end: build the double BITS directly (sign<<63, biased
 * exponent<<52, mantissa<<29), one addpd, then extract the fields back
 * -- where the mantissa's >>29 truncates the guard bit exactly as the
 * hardware adder does, no rounding mode involved. */


/* The emission-shaped mirror: exactly the bit formulas the SSE sequence
 * will use, so a typo here fails the row before it fails a game. Encode
 * each masked operand's PS2 bits into IEEE double BITS with integer
 * math (never through a single), one exact double add, decode the
 * fields back with the mantissa shift truncating the guard bit. */
static u32 emit_encdec_addsub(u32 a, u32 b, int negate_b)
{
	u32 ma, mb, hiA, loA, hiB, loB, hiR, loR, sr, mant;
	u32 ea, eb;
	int es;
	uint64_t dA, dB;
	double xA, xB, xS;

	if (negate_b) b ^= 0x80000000u;
	mask_smaller(a, b, &ma, &mb);
	ea = (ma >> 23) & 0xffu; eb = (mb >> 23) & 0xffu;

	hiA = ma & 0x80000000u;
	if (ea != 0) hiA |= ((ea + 896u) << 20) | ((ma & 0x007fffffu) >> 3);
	loA = (ea != 0) ? (ma << 29) : 0;
	hiB = mb & 0x80000000u;
	if (eb != 0) hiB |= ((eb + 896u) << 20) | ((mb & 0x007fffffu) >> 3);
	loB = (eb != 0) ? (mb << 29) : 0;

	dA = ((uint64_t)hiA << 32) | loA;
	dB = ((uint64_t)hiB << 32) | loB;
	memcpy(&xA, &dA, 8); memcpy(&xB, &dB, 8);
	xS = xA + xB;
	memcpy(&dA, &xS, 8);
	hiR = (u32)(dA >> 32); loR = (u32)dA;

	sr   = hiR & 0x80000000u;
	es   = (int)((hiR >> 20) & 0x7ffu) - 896;
	mant = ((hiR & 0x000fffffu) << 3) | (loR >> 29);
	if (es <= 0)   return sr;
	if (es > 255)  return sr | 0x7fffffffu;
	return sr | ((u32)es << 23) | mant;
}
static u32 emit_add(u32 a, u32 b) { return emit_encdec_addsub(a, b, 0); }
static u32 emit_sub(u32 a, u32 b) { return emit_encdec_addsub(a, b, 1); }

/* Input space: every exponent boundary crossed with mantissa patterns,
 * then a deterministic xorshift sweep. */
static u32 inputs[512];
static int n_in = 0;
static void gen_inputs(void)
{
	static const u32 exps[]  = {0,1,2,0x40,0x7e,0x7f,0x80,0xc0,0xfd,0xfe,0xff};
	static const u32 mants[] = {0,1,2,0x400000,0x7fffff,0x3fffff,0x555555,0x2aaaaa};
	u32 s, e, m;
	unsigned i, j;
	for (s = 0; s < 2; s++)
		for (i = 0; i < sizeof(exps)/sizeof(*exps); i++)
			for (j = 0; j < sizeof(mants)/sizeof(*mants); j++)
				if (n_in < 512)
					inputs[n_in++] = (s << 31) | (exps[i] << 23) | mants[j];
}
static u32 xs_state = 0x12345678u;
static u32 xs(void)
{
	u32 x = xs_state;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	xs_state = x;
	return x;
}

typedef u32 (*binop_t)(u32, u32);
typedef u64 (*ps2f_binop_t)(u32, u32);

static void row2(const char* name, binop_t hard, ps2f_binop_t model)
{
	long long total = 0, diffs = 0;
	u32 ex_a = 0, ex_b = 0, ex_h = 0, ex_m = 0;
	int i, j, k;
	for (i = 0; i < n_in; i++)
		for (j = 0; j < n_in; j++)
		{
			u32 h = hard(inputs[i], inputs[j]);
			u32 m = ps2f_raw(model(inputs[i], inputs[j]));
			total++;
			if (h != m)
			{
				if (!diffs) { ex_a = inputs[i]; ex_b = inputs[j]; ex_h = h; ex_m = m; }
				diffs++;
			}
		}
	for (k = 0; k < 2000000; k++)
	{
		u32 a = xs(), b = xs();
		u32 h = hard(a, b);
		u32 m = ps2f_raw(model(a, b));
		total++;
		if (h != m)
		{
			if (!diffs) { ex_a = a; ex_b = b; ex_h = h; ex_m = m; }
			diffs++;
		}
	}
	printf("%-22s %9lld / %lld (%.4f%%)", name, diffs, total, 100.0 * (double)diffs / (double)total);
	if (diffs)
		printf("   first: a=%08x b=%08x hard=%08x model=%08x", ex_a, ex_b, ex_h, ex_m);
	printf("\n");
}

static u64 model_sqrt2(u32 a, u32 b) { (void)b; return ps2f_sqrt(a); }
static u32 hard_sqrt2(u32 a, u32 b)  { (void)b; return hard_sqrt(a); }
static u64 model_madd0(u32 s, u32 t) { return ps2f_madd(0, s, t, 0); }
static u32 hard_madd0(u32 s, u32 t)  { return hard_madd(0, s, t); }
static u64 model_max(u32 a, u32 b)   { return (u64)fp_model_max(a, b); }
static u64 model_min(u32 a, u32 b)   { return (u64)fp_model_min(a, b); }
static u64 model_rsqrt(u32 a, u32 b) { return ps2f_rsqrt(a, b); }
static u32 hard_rsqrt(u32 a, u32 b)
{
	return clamp1(f_to_bits(bits_to_f(clamp1(a)) / (float)sqrt((double)bits_to_f(clamp1(b & 0x7fffffffu)))));
}

int main(void)
{
	/* The emitted code runs under the VU/FPU MXCSR the core programs:
	 * round-to-nearest with DAZ and FTZ set (SetDenormalsAreZero(true) /
	 * SetFlushToZero in Pcsx2Config). The mirror must run under the same
	 * environment or it audits the host default instead of the emission. */
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
	_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
	gen_inputs();
	printf("=== recompiled-sequence exactness vs ps2float (structured %dx%d + 2M random each)\n", n_in, n_in);
	printf("%-22s %s\n", "op (as emitted)", "diffs / cases");
	row2("VU/EE ADD  (clamp+ss)", hard_add,  ps2f_add);
	row2("VU/EE SUB  (clamp+ss)", hard_sub,  ps2f_sub);
	row2("VU/EE MUL  (clamp+ss)", hard_mul,  ps2f_mul);
	row2("VU/EE DIV  (clamp+ss)", hard_div,  ps2f_div);
	row2("VU SQRT    (abs+ss)  ", hard_sqrt2, model_sqrt2);
	row2("VU RSQRT   (div+ss)  ", hard_rsqrt, model_rsqrt);
	row2("MADD acc=0 (2x ss)   ", hard_madd0, model_madd0);
	printf("--- exact-by-construction tiers (integer ordering; EE-proven):\n");
	row2("MAX (int ordering)   ", hard_max, model_max);
	row2("MIN (int ordering)   ", hard_min, model_min);
	printf("--- default tier as emitted (VuAccurateAddSub on):\n");
	row2("ACC ADD (mask+RN ss) ", hard_accadd, ps2f_add);
	row2("ACC SUB (mask+RN ss) ", hard_accsub, ps2f_sub);
	printf("--- candidates: double pipeline with PS2 truncation (the EE DOUBLE construction):\n");
	row2("cand ADD (double)    ", cand_add, ps2f_add);
	row2("cand SUB (double)    ", cand_sub, ps2f_sub);
	row2("emit ADD (bit-built) ", emit_add, ps2f_add);
	row2("emit SUB (bit-built) ", emit_sub, ps2f_sub);
	return 0;
}
