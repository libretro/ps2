/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2024  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h> /* abs */
#include "ps2float.h"

#define PS2F_MANTISSA_BITS 23

#if defined(_MSC_VER)
#include <intrin.h>
#define PS2F_INLINE __forceinline
#elif defined(__GNUC__)
#define PS2F_INLINE __inline__ __attribute__((always_inline))
#else
#define PS2F_INLINE
#endif

static PS2F_INLINE ps2f_u32 ps2f_mantissa(ps2f_u32 x)
{
	return (x & 0x7fffffu) | 0x800000u;
}

static PS2F_INLINE ps2f_u32 ps2f_exponent(ps2f_u32 x)
{
	return (x >> 23) & 0xffu;
}

static PS2F_INLINE ps2f_u32 ps2f_frac(ps2f_u32 x)
{
	return x & 0x7fffffu;
}

static PS2F_INLINE int ps2f_sign(ps2f_u32 x)
{
	return (int)((x >> 31) & 1u);
}

static PS2F_INLINE ps2f_u32 ps2f_pack(int sign, ps2f_u32 exponent, ps2f_u32 mantissa)
{
	return ((sign ? 1u : 0u) << 31) | ((exponent & 0xffu) << PS2F_MANTISSA_BITS) | (mantissa & 0x7fffffu);
}

static PS2F_INLINE int ps2f_is_denormalized(ps2f_u32 x)
{
	return ps2f_exponent(x) == 0;
}

static PS2F_INLINE ps2f_u32 ps2f_abs_bits(ps2f_u32 x)
{
	return x & PS2F_MAX_FLOATING_POINT_VALUE;
}

static PS2F_INLINE int ps2f_is_zero(ps2f_u32 x)
{
	return ps2f_abs_bits(x) == 0;
}

/* Count-leading-zero for a non-zero value. */
static PS2F_INLINE ps2f_s32 ps2f_bsr(ps2f_u32 b)
{
#if defined(_MSC_VER)
	unsigned long index;
	if (b == 0)
		return -1;
	_BitScanReverse(&index, b);
	return (ps2f_s32)index;
#elif defined(__GNUC__)
	if (b == 0)
		return -1;
	return (ps2f_s32)(31 - __builtin_clz(b));
#else
	ps2f_s32 index;
	if (b == 0)
		return -1;
	index = 0;
	while (b >>= 1)
		index++;
	return index;
#endif
}

static PS2F_INLINE ps2f_u32 ps2f_count_leading_sign_bits(ps2f_s32 n)
{
	/* If the sign bit is 1, we invert the bits to 0 for count-leading-zero. */
	if (n < 0)
		n = ~n;

	/* If BSR is used directly, it would have an undefined value for 0. */
	if (n == 0)
		return 32;

	return (ps2f_u32)(31 - ps2f_bsr((ps2f_u32)n));
}

static const ps2f_s32 ps2f_normalize_amounts[32] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16, 24, 24, 24, 24, 24, 24, 24};

/****************************************************************
 * Radix Divisor
 * Algorithm reference: DOI 10.1109/ARITH.1995.465363
 ****************************************************************/

typedef struct
{
	ps2f_u32 sum;
	ps2f_u32 carry;
} ps2f_csa_t;

static PS2F_INLINE ps2f_csa_t ps2f_csa(ps2f_u32 a, ps2f_u32 b, ps2f_u32 c)
{
	ps2f_csa_t r;
	ps2f_u32 u = a ^ b;
	r.sum = u ^ c;
	r.carry = ((a & b) | (u & c)) << 1;
	return r;
}

/* Remainder estimate for quotient selection.  Note: decimal point is
 * between bits 24 and 25. */
static PS2F_INLINE ps2f_s32 ps2f_quotient_test(ps2f_csa_t current)
{
	ps2f_u32 mask = (1u << 24) - 1; /* Bit 23 needs to be or'd in instead of added */
	return (ps2f_s32)((((current.sum & ~mask) + current.carry)) | (current.sum & mask));
}

/* Branch-free three-way select from the estimate: the remainder is data,
 * so the old compare chain mispredicted once per divider step. */
static PS2F_INLINE ps2f_s32 ps2f_quotient_select_test(ps2f_s32 test)
{
	return (ps2f_s32)(test >= (ps2f_s32)(1 << 23)) - (ps2f_s32)(test < (ps2f_s32)(~0u << 24));
}

/****************************************************************
 * Booth Multiplier
 ****************************************************************/

typedef struct
{
	ps2f_u32 data;
	ps2f_u32 negate;
} ps2f_booth_t;

typedef struct
{
	ps2f_u32 lo;
	ps2f_u32 hi;
} ps2f_addres_t;

static PS2F_INLINE ps2f_booth_t ps2f_booth(ps2f_u32 a, ps2f_u32 b, ps2f_u32 bit)
{
	/* Identical arithmetic to the reference, expressed with masks instead
	 * of conditionals: the recode bits come straight from the operand, so
	 * a branch here is a coin flip per partial product and the eight
	 * recodes per multiply turned the tree into a mispredict storm. */
	ps2f_booth_t r;
	ps2f_u32 test = (bit ? b >> (bit * 2 - 1) : b << 1) & 7u;
	ps2f_u32 dbl  = 0u - (ps2f_u32)((test == 3) | (test == 4)); /* x2 for recode +/-2 */
	ps2f_u32 neg  = 0u - (ps2f_u32)((test - 4u) <= 2u);         /* 4..6 */
	ps2f_u32 any  = 0u - (ps2f_u32)((test - 1u) <= 5u);         /* 1..6 */
	ps2f_u32 pos  = 1u << (bit * 2);
	a <<= (bit * 2);
	a += a & dbl;
	a ^= (neg & (0u - pos));
	a &= any;
	r.data = a;
	r.negate = neg & pos;
	return r;
}

static PS2F_INLINE ps2f_addres_t ps2f_add3(ps2f_u32 a, ps2f_u32 b, ps2f_u32 c)
{
	ps2f_addres_t r;
	ps2f_u32 u = a ^ b;
	r.lo = u ^ c;
	r.hi = ((u & c) | (a & b)) << 1;
	return r;
}

static ps2f_u64 ps2f_mul_mantissa(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_u64 full = (ps2f_u64)a * (ps2f_u64)b;
	ps2f_booth_t b0 = ps2f_booth(a, b, 0);
	ps2f_booth_t b1 = ps2f_booth(a, b, 1);
	ps2f_booth_t b2 = ps2f_booth(a, b, 2);
	ps2f_booth_t b3 = ps2f_booth(a, b, 3);
	ps2f_booth_t b4 = ps2f_booth(a, b, 4);
	ps2f_booth_t b5 = ps2f_booth(a, b, 5);
	ps2f_booth_t b6 = ps2f_booth(a, b, 6);
	ps2f_booth_t b7 = ps2f_booth(a, b, 7);
	ps2f_addres_t t0;
	ps2f_addres_t t1;
	ps2f_addres_t t2;
	ps2f_addres_t t3;
	ps2f_addres_t t4;
	ps2f_addres_t t5;
	ps2f_u32 ps2lo;

	/* First cycle */
	t0 = ps2f_add3(b1.data, b2.data, b3.data);
	t1 = ps2f_add3(b4.data & ~0x7ffu, b5.data & ~0xfffu, b6.data);
	/* A few adds get skipped, squeeze them back in */
	t1.hi |= b6.negate | (b5.data & 0x800u);
	b7.data |= (b5.data & 0x400u) + b5.negate;

	/* Second cycle */
	t2 = ps2f_add3(b0.data, t0.lo, t0.hi);
	t3 = ps2f_add3(b7.data, t1.lo, t1.hi);

	/* Third cycle */
	t4 = ps2f_add3(t2.hi, t3.lo, t3.hi);

	/* Fourth cycle */
	t5 = ps2f_add3(t2.lo, t4.lo, t4.hi);

	/* Discard bits and sum */
	t5.hi += b7.negate;
	t5.lo &= ~0x7fffu;
	t5.hi &= ~0x7fffu;
	ps2lo = t5.lo + t5.hi;
	return full - ((ps2lo ^ full) & 0x8000u);
}

/****************************************************************
 * Operation signs and comparisons
 ****************************************************************/

ps2f_s32 ps2f_compare_operands(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_u32 am = ps2f_abs_bits(a);
	ps2f_u32 bm = ps2f_abs_bits(b);

	if (am < bm)
		return -1;
	else if (am == bm)
		return 0;
	else
		return 1;
}

ps2f_s32 ps2f_compare(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_s32 av = (ps2f_s32)ps2f_abs_bits(a);
	ps2f_s32 bv = (ps2f_s32)ps2f_abs_bits(b);

	if (ps2f_sign(a))
		av = -av;
	if (ps2f_sign(b))
		bv = -bv;

	if (av < bv)
		return -1;
	else if (av == bv)
		return 0;
	else
		return 1;
}

static PS2F_INLINE int ps2f_muldiv_sign(ps2f_u32 a, ps2f_u32 b)
{
	return ps2f_sign(a) ^ ps2f_sign(b);
}

static PS2F_INLINE int ps2f_addition_sign(ps2f_u32 a, ps2f_u32 b)
{
	return ps2f_compare_operands(a, b) >= 0 ? ps2f_sign(a) : ps2f_sign(b);
}

static PS2F_INLINE int ps2f_subtraction_sign(ps2f_u32 a, ps2f_u32 b)
{
	return ps2f_compare_operands(a, b) >= 0 ? ps2f_sign(a) : !ps2f_sign(b);
}

/****************************************************************
 * Float Processor
 ****************************************************************/

static ps2f_u64 ps2f_do_add(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_u32 self_exponent;
	ps2f_s32 res_exponent;
	ps2f_u32 sign1;
	ps2f_u32 sign2;
	ps2f_s32 self_mantissa;
	ps2f_s32 other_mantissa;
	ps2f_s32 man;
	ps2f_s32 abs_man;
	ps2f_s32 raw_exp;
	ps2f_s32 amount;
	ps2f_s32 msb_index;

	/* Order operands by exponent up front instead of recursing: the
	 * reference's tail call kept this function out of line and made a
	 * data-driven branch-and-call of half of all additions.  The swap is
	 * a mask blend - which operand is larger is a coin flip. */
	{
		ps2f_u32 m = 0u - (ps2f_u32)(ps2f_exponent(a) < ps2f_exponent(b));
		ps2f_u32 x = (a & ~m) | (b & m);
		b = (b & ~m) | (a & m);
		a = x;
	}
	self_exponent = ps2f_exponent(a);
	res_exponent  = (ps2f_s32)self_exponent - (ps2f_s32)ps2f_exponent(b);
	if (res_exponent >= 25)
		return a;

	/* roundingMultiplier = 6.
	 * http://graphics.stanford.edu/~seander/bithacks.html#ConditionalNegate
	 * Shifts of possibly-negative mantissas are done in unsigned
	 * arithmetic; two's complement keeps the bit patterns identical to
	 * the reference while staying defined behavior. */
	sign1 = (ps2f_u32)((ps2f_s32)a >> 31);
	self_mantissa = (ps2f_s32)((ps2f_mantissa(a) ^ sign1) - sign1);
	sign2 = (ps2f_u32)((ps2f_s32)b >> 31);
	other_mantissa = (ps2f_s32)((ps2f_mantissa(b) ^ sign2) - sign2);

	man = (ps2f_s32)(((ps2f_u32)self_mantissa << 6) + (ps2f_u32)(((ps2f_s32)((ps2f_u32)other_mantissa << 6)) >> res_exponent));
	abs_man = abs(man);
	if (abs_man == 0)
		return 0;

	raw_exp = (ps2f_s32)self_exponent - 6;

	amount = ps2f_normalize_amounts[ps2f_count_leading_sign_bits(abs_man)];
	raw_exp -= amount;
	abs_man = (ps2f_s32)((ps2f_u32)abs_man << amount);

	msb_index = ps2f_bsr((ps2f_u32)(abs_man >> PS2F_MANTISSA_BITS));
	raw_exp += msb_index;
	abs_man >>= msb_index;

	if (raw_exp > 255)
		return (man < 0 ? PS2F_MIN_FLOATING_POINT_VALUE : PS2F_MAX_FLOATING_POINT_VALUE) | PS2F_OF;
	else if (raw_exp < 1)
		return (ps2f_u64)ps2f_pack(man < 0, 0, 0) | PS2F_UF;

	return ((ps2f_u32)man & PS2F_SIGNMASK) | ((ps2f_u32)raw_exp << PS2F_MANTISSA_BITS) | ((ps2f_u32)abs_man & 0x7fffffu);
}

static ps2f_u64 ps2f_do_mul(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_u32 self_mantissa = ps2f_mantissa(a);
	ps2f_u32 other_mantissa = ps2f_mantissa(b);
	ps2f_u32 sign = (a ^ b) & PS2F_SIGNMASK;
	ps2f_s32 res_exponent = (ps2f_s32)ps2f_exponent(a) + (ps2f_s32)ps2f_exponent(b) - 127;
	ps2f_u32 res_mantissa = (ps2f_u32)(ps2f_mul_mantissa(self_mantissa, other_mantissa) >> PS2F_MANTISSA_BITS);

	/* Carry-out normalize without a branch: the top bit is operand data. */
	{
		ps2f_u32 carry = res_mantissa >> 24;
		res_mantissa >>= carry;
		res_exponent += (ps2f_s32)carry;
	}

	if (res_exponent > 255)
		return (ps2f_u64)(sign | PS2F_MAX_FLOATING_POINT_VALUE) | PS2F_OF;
	else if (res_exponent < 1)
		return (ps2f_u64)sign | PS2F_UF;

	return sign | ((ps2f_u32)res_exponent << PS2F_MANTISSA_BITS) | (res_mantissa & 0x7fffffu);
}

ps2f_u64 ps2f_add(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_s32 exp_diff;

	if (ps2f_is_denormalized(a) || ps2f_is_denormalized(b))
	{
		int sign = ps2f_addition_sign(a, b);

		if (ps2f_is_denormalized(a) && !ps2f_is_denormalized(b))
			return ps2f_pack(sign, ps2f_exponent(b), ps2f_frac(b));
		else if (!ps2f_is_denormalized(a))
			return ps2f_pack(sign, ps2f_exponent(a), ps2f_frac(a));
		/* both denormalized */
		if (!ps2f_sign(a) || !ps2f_sign(b))
			return ps2f_pack(0, 0, 0);
		return ps2f_pack(1, 0, 0);
	}

	/* exponent difference */
	exp_diff = (ps2f_s32)ps2f_exponent(a) - (ps2f_s32)ps2f_exponent(b);

	/* diff = 1 .. 24, expt < expd */
	if (exp_diff > 0 && exp_diff < 25)
	{
		exp_diff = exp_diff - 1;
		b = (PS2F_MIN_FLOATING_POINT_VALUE << exp_diff) & b;
	}
	/* diff = -24 .. -1 , expd < expt */
	else if (exp_diff < 0 && exp_diff > -25)
	{
		exp_diff = -exp_diff;
		exp_diff = exp_diff - 1;
		a = a & (PS2F_MIN_FLOATING_POINT_VALUE << exp_diff);
	}

	return ps2f_do_add(a, b);
}

ps2f_u64 ps2f_sub(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_s32 exp_diff;

	if (ps2f_is_denormalized(a) || ps2f_is_denormalized(b))
	{
		int sign = ps2f_subtraction_sign(a, b);

		if (ps2f_is_denormalized(a) && !ps2f_is_denormalized(b))
			return ps2f_pack(sign, ps2f_exponent(b), ps2f_frac(b));
		else if (!ps2f_is_denormalized(a))
			return ps2f_pack(sign, ps2f_exponent(a), ps2f_frac(a));
		/* both denormalized */
		if (!ps2f_sign(a) || ps2f_sign(b))
			return ps2f_pack(0, 0, 0);
		return ps2f_pack(1, 0, 0);
	}

	/* exponent difference */
	exp_diff = (ps2f_s32)ps2f_exponent(a) - (ps2f_s32)ps2f_exponent(b);

	/* diff = 1 .. 24, expt < expd */
	if (exp_diff > 0 && exp_diff < 25)
	{
		exp_diff = exp_diff - 1;
		b = (PS2F_MIN_FLOATING_POINT_VALUE << exp_diff) & b;
	}
	/* diff = -24 .. -1 , expd < expt */
	else if (exp_diff < 0 && exp_diff > -25)
	{
		exp_diff = -exp_diff;
		exp_diff = exp_diff - 1;
		a = a & (PS2F_MIN_FLOATING_POINT_VALUE << exp_diff);
	}

	return ps2f_do_add(a, b ^ PS2F_SIGNMASK);
}

ps2f_u64 ps2f_mul(ps2f_u32 a, ps2f_u32 b)
{
	if (ps2f_is_denormalized(a) || ps2f_is_denormalized(b) || ps2f_is_zero(a) || ps2f_is_zero(b))
		return ps2f_pack(ps2f_muldiv_sign(a, b), 0, 0);

	return ps2f_do_mul(a, b);
}

ps2f_u64 ps2f_div(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_u32 am;
	ps2f_u32 bm;
	ps2f_csa_t current;
	ps2f_u32 quotient;
	ps2f_s32 quotient_bit;
	ps2f_s32 i;
	ps2f_u32 sign;
	ps2f_u32 dvdt_exp;
	ps2f_u32 dvsr_exp;
	ps2f_s32 cexp;

	if (((a & 0x7f800000u) == 0) && ((b & 0x7f800000u) != 0))
	{
		ps2f_u32 float_result = 0;
		float_result &= PS2F_MAX_FLOATING_POINT_VALUE;
		float_result |= (ps2f_u32)(((ps2f_s32)(b >> 31) != (ps2f_s32)(a >> 31)) ? 1 : (0 & 1)) << 31;
		return float_result;
	}
	if (((a & 0x7f800000u) != 0) && ((b & 0x7f800000u) == 0))
	{
		ps2f_u32 float_result = PS2F_MAX_FLOATING_POINT_VALUE;
		float_result &= PS2F_MAX_FLOATING_POINT_VALUE;
		float_result |= (ps2f_u32)(((ps2f_s32)(b >> 31) != (ps2f_s32)(a >> 31)) ? 1 : (0 & 1)) << 31;
		return (ps2f_u64)float_result | PS2F_DZ;
	}
	if (((a & 0x7f800000u) == 0) && ((b & 0x7f800000u) == 0))
	{
		ps2f_u32 float_result = PS2F_MAX_FLOATING_POINT_VALUE;
		float_result &= PS2F_MAX_FLOATING_POINT_VALUE;
		float_result |= (ps2f_u32)(((ps2f_s32)(b >> 31) != (ps2f_s32)(a >> 31)) ? 1 : (0 & 1)) << 31;
		return (ps2f_u64)float_result | PS2F_IV;
	}

	am = ps2f_mantissa(a) << 2;
	bm = ps2f_mantissa(b) << 2;
	current.sum = am;
	current.carry = 0;
	quotient = 0;
	quotient_bit = 1;
	for (i = 0; i < 25; i++)
	{
		/* quotient_bit is remainder-driven data; every select in this
		 * body is a mask so the 25 steps carry no branches. */
		ps2f_u32 m_any = 0u - (ps2f_u32)(quotient_bit != 0);
		ps2f_u32 m_pos = 0u - (ps2f_u32)(quotient_bit > 0);
		ps2f_u32 add;
		ps2f_csa_t csa;
		quotient = (quotient << 1) + (ps2f_u32)quotient_bit;
		add = (bm ^ m_pos) & m_any;
		current.carry += m_pos & 1u;
		csa = ps2f_csa(current.sum, current.carry, add);
		{
			ps2f_s32 t_csa = ps2f_quotient_test(csa);
			ps2f_s32 t_cur = ps2f_quotient_test(current);
			quotient_bit = ps2f_quotient_select_test((ps2f_s32)(((ps2f_u32)t_csa & m_any) | ((ps2f_u32)t_cur & ~m_any)));
		}
		current.sum = csa.sum << 1;
		current.carry = csa.carry << 1;
	}

	sign = (a ^ b) & 0x80000000u;
	dvdt_exp = ps2f_exponent(a);
	dvsr_exp = ps2f_exponent(b);
	cexp = (ps2f_s32)dvdt_exp - (ps2f_s32)dvsr_exp + 126;

	if (quotient >= (1u << 24))
	{
		cexp += 1;
		quotient >>= 1;
	}

	if (dvdt_exp == 0 && dvsr_exp == 0)
		return (ps2f_u64)(sign | PS2F_MAX_FLOATING_POINT_VALUE) | PS2F_IV;
	else if (dvdt_exp == 0 || dvsr_exp != 0)
	{
		if (dvdt_exp == 0 && dvsr_exp != 0)
			return sign;
	}
	else
		return (ps2f_u64)(sign | PS2F_MAX_FLOATING_POINT_VALUE) | PS2F_DZ;

	if (cexp > 255)
		return (ps2f_u64)(sign | PS2F_MAX_FLOATING_POINT_VALUE) | PS2F_OF;
	else if (cexp < 1)
		return (ps2f_u64)sign | PS2F_UF;

	return (quotient & 0x7fffffu) | ((ps2f_u32)cexp << 23) | sign;
}

ps2f_u64 ps2f_sqrt(ps2f_u32 a)
{
	ps2f_u32 m;
	ps2f_csa_t current;
	ps2f_u32 quotient;
	ps2f_s32 quotient_bit;
	ps2f_s32 i;
	ps2f_s32 dvdt_exp;
	ps2f_u32 res;

	if ((a & 0x7f800000u) == 0)
		return (ps2f_u64)0 | (((a >> 31) & 1u) ? PS2F_IV : 0);

	m = ps2f_mantissa(a) << 1;
	if (!(a & 0x800000u)) /* If exponent is odd after subtracting bias of 127 */
		m <<= 1;

	current.sum = m;
	current.carry = 0;
	quotient = 0;
	quotient_bit = 1;
	for (i = 0; i < 25; i++)
	{
		/* Adding n to quotient adds n * (2*quotient + n) to quotient^2
		 * (which is what we need to subtract from the remainder) */
		ps2f_u32 adjust;
		ps2f_u32 add;
		ps2f_csa_t csa;
		ps2f_u32 m_any = 0u - (ps2f_u32)(quotient_bit != 0);
		ps2f_u32 m_pos = 0u - (ps2f_u32)(quotient_bit > 0);
		adjust = quotient + ((ps2f_u32)quotient_bit << (24 - i));
		quotient += (ps2f_u32)quotient_bit << (25 - i);
		add = (adjust ^ m_pos) & m_any;
		current.carry += m_pos & 1u;
		csa = ps2f_csa(current.sum, current.carry, add);
		{
			ps2f_s32 t_csa = ps2f_quotient_test(csa);
			ps2f_s32 t_cur = ps2f_quotient_test(current);
			quotient_bit = ps2f_quotient_select_test((ps2f_s32)(((ps2f_u32)t_csa & m_any) | ((ps2f_u32)t_cur & ~m_any)));
		}
		current.sum = csa.sum << 1;
		current.carry = csa.carry << 1;
	}

	dvdt_exp = (ps2f_s32)ps2f_exponent(a);
	if (dvdt_exp == 0)
		return 0;
	dvdt_exp = (dvdt_exp + 127) >> 1;

	res = ((quotient >> 2) & 0x7fffffu) | ((ps2f_u32)dvdt_exp << 23);
	if (ps2f_sign(a))
	{
		if (ps2f_sign(res))
			res ^= PS2F_SIGNMASK;
		return (ps2f_u64)res | PS2F_IV;
	}
	return res;
}

ps2f_u64 ps2f_rsqrt(ps2f_u32 a, ps2f_u32 b)
{
	ps2f_u64 sq = ps2f_sqrt(ps2f_pack(0, ps2f_exponent(b), ps2f_frac(b)));
	ps2f_u64 dv = ps2f_div(a, ps2f_raw(sq));
	ps2f_u64 flags = (dv & (PS2F_OF | PS2F_UF)) | ((sq | dv) & (PS2F_DZ | PS2F_IV));
	return (ps2f_u64)ps2f_raw(dv) | flags;
}

/****************************************************************
 * Fused chains (MADD/MSUB and accumulator variants)
 ****************************************************************/

/* Modes 3/8: MADD rounds to max when the product is positive; modes 4/9
 * (MSUB) when it is negative.  acc/acc_of feed the accumulator overflow
 * resolution; result overrides come from the hardware's MAC exception
 * priority. */
static ps2f_u64 ps2f_mac_finish(int round_to_max, ps2f_u32 acc, int acc_of, ps2f_u64 mulres, ps2f_u64 addsub)
{
	ps2f_u32 res = ps2f_raw(addsub);
	ps2f_u64 flags = addsub & (PS2F_OF | PS2F_UF);
	int m_of = (mulres & PS2F_OF) != 0;

	if (!acc_of)
	{
		if (m_of)
		{
			res = round_to_max ? PS2F_MAX_FLOATING_POINT_VALUE : PS2F_MIN_FLOATING_POINT_VALUE;
			flags = PS2F_OF;
		}
	}
	else if (!m_of)
	{
		res = acc;
		flags = PS2F_OF;
	}
	else
	{
		res = round_to_max ? PS2F_MAX_FLOATING_POINT_VALUE : PS2F_MIN_FLOATING_POINT_VALUE;
		flags = PS2F_OF;
	}

	return (ps2f_u64)res | flags;
}

ps2f_u64 ps2f_madd(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of)
{
	ps2f_u64 mulres = ps2f_mul(s, t);
	ps2f_u64 addres = ps2f_add(acc, ps2f_raw(mulres));
	return ps2f_mac_finish(!ps2f_sign(ps2f_raw(mulres)), acc, acc_of, mulres, addres);
}

ps2f_u64 ps2f_madda(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of)
{
	return ps2f_madd(acc, s, t, acc_of);
}

ps2f_u64 ps2f_msub(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of)
{
	ps2f_u64 mulres = ps2f_mul(s, t);
	ps2f_u64 subres = ps2f_sub(acc, ps2f_raw(mulres));
	return ps2f_mac_finish(ps2f_sign(ps2f_raw(mulres)), acc, acc_of, mulres, subres);
}

ps2f_u64 ps2f_msuba(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of)
{
	return ps2f_msub(acc, s, t, acc_of);
}

/****************************************************************
 * VU EFU helpers
 ****************************************************************/

ps2f_u64 ps2f_ercpr(ps2f_u32 a)
{
	return ps2f_div(PS2F_ONE, a);
}

ps2f_u64 ps2f_esqrt(ps2f_u32 a)
{
	return ps2f_sqrt(a);
}

ps2f_u64 ps2f_esqur(ps2f_u32 a)
{
	return ps2f_mul(a, a);
}

ps2f_u64 ps2f_ersqrt(ps2f_u32 a)
{
	return ps2f_rsqrt(PS2F_ONE, a);
}

/****************************************************************
 * CLIP
 ****************************************************************/

ps2f_u32 ps2f_clip(ps2f_u32 f1, ps2f_u32 f2)
{
	ps2f_u32 a;
	ps2f_u32 result = 0;

	if ((f1 & 0x7f800000u) == 0)
		f1 &= 0xff800000u;

	a = f1;

	if ((f2 & 0x7f800000u) == 0)
		f2 &= 0xff800000u;

	f1 = f1 & PS2F_MAX_FLOATING_POINT_VALUE;
	f2 = f2 & PS2F_MAX_FLOATING_POINT_VALUE;

	if ((-1 < (ps2f_s32)a) && (f2 < f1))
		result |= 1u;

	if (((ps2f_s32)a < 0) && (f2 < f1))
		result |= 2u;

	return result;
}
