/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

// --------------------------------------------------------------------------------------
//  r64 / r128 - Types that are guaranteed to fit in one register
// --------------------------------------------------------------------------------------
// Note: Recompilers rely on some of these types and the registers they allocate to,
// so be careful if you want to change them

#pragma once

#include "Pcsx2Defs.h"
#include "Pcsx2Types.h"
#include "VectorIntrin.h"

#include <cstring>

#if defined(_M_X86) || defined(_M_X64) || defined(_M_AMD64) || defined(__amd64__) || defined(__x86_64__) || defined(__x86_64)

/* Can't stick them in structs because it breaks calling convention things, yay */
using r128 = __m128i;

/* Calling convention setting, yay */
#define RETURNS_R128 r128 __vectorcall
#define TAKES_R128 __vectorcall

/* And since we can't stick them in structs, we get lots of static methods, yay! */
__forceinline static r128 r128_load(const void* ptr)
{
	return _mm_load_si128(reinterpret_cast<const r128*>(ptr));
}

__forceinline static void r128_store(void* ptr, r128 val)
{
	return _mm_store_si128(reinterpret_cast<r128*>(ptr), val);
}

__forceinline static void r128_store_unaligned(void* ptr, r128 val)
{
	return _mm_storeu_si128(reinterpret_cast<r128*>(ptr), val);
}

#define r128_zero() _mm_setzero_si128()
/* Expects that r64 came from r64-handling code, and not from a recompiler or something */
#define r128_from_u64_dup(val) _mm_set1_epi64x((val))
#define r128_from_u64_zext(val) _mm_set_epi64x(0, (val))
#define r128_from_u32_dup(val) _mm_set1_epi32((val))
#define r128_from_u32x4(lo0, lo1, hi0, hi1) _mm_setr_epi32(lo0, lo1, hi0, hi1)
#define r128_to_u32(val) _mm_cvtsi128_si32((val))
#define r128_to_u64(val) _mm_cvtsi128_si64((val))

#define CopyQWC(dest, src) _mm_store_ps((float*)(dest), _mm_load_ps((const float*)(src)))
#define ZeroQWC(dest ) _mm_store_ps((float*)(dest), _mm_setzero_ps())

__forceinline static r128 r128_from_u128(const u128& u)
{
	return _mm_loadu_si128(reinterpret_cast<const __m128i*>(&u));
}

__forceinline static u128 r128_to_u128(r128 val)
{
	alignas(16) u128 ret;
	_mm_store_si128(reinterpret_cast<r128*>(&ret), val);
	return ret;
}

#elif defined(_M_ARM64)

using r128 = uint32x4_t;

#define RETURNS_R128 r128 __vectorcall
#define TAKES_R128 __vectorcall

__forceinline static void CopyQWC(void* dest, const void* src)
{
	vst1q_u8(static_cast<u8*>(dest), vld1q_u8(static_cast<const u8*>(src)));
}

__forceinline static void ZeroQWC(void* dest)
{
	vst1q_u8(static_cast<u8*>(dest), vmovq_n_u8(0));
}

__forceinline static void ZeroQWC(u128& dest)
{
	vst1q_u8(&dest._u8[0], vmovq_n_u8(0));
}


__forceinline static r128 r128_load(const void* ptr)
{
	return vld1q_u32(reinterpret_cast<const uint32_t*>(ptr));
}

__forceinline static void r128_store(void* ptr, r128 value)
{
	return vst1q_u32(reinterpret_cast<uint32_t*>(ptr), value);
}

__forceinline static void r128_store_unaligned(void* ptr, r128 value)
{
	return vst1q_u32(reinterpret_cast<uint32_t*>(ptr), value);
}

#define r128_zero() vmovq_n_u32(0)

/* Expects that r64 came from r64-handling code, and not from a recompiler or something */
#define r128_from_u64_dup(val) vreinterpretq_u32_u64(vdupq_n_u64((val)))

#define r128_to_u32(val) vgetq_lane_u32((val), 0)
#define r128_to_u64(val) vgetq_lane_u64(vreinterpretq_u64_u32((val)), 0)

#define r128_from_u64_zext(val) vreinterpretq_u32_u64(vcombine_u64(vcreate_u64((val)), vcreate_u64(0)))
#define r128_from_u32_dup(val) vdupq_n_u32((val))

__forceinline static r128 r128_from_u32x4(u32 lo0, u32 lo1, u32 hi0, u32 hi1)
{
	const u32 values[4] = {lo0, lo1, hi0, hi1};
	return vld1q_u32(values);
}

__forceinline static r128 r128_from_u128(const u128& u)
{
	return vld1q_u32(reinterpret_cast<const uint32_t*>(u._u32));
}

__forceinline static u128 r128_to_u128(r128 val)
{
	alignas(16) u128 ret;
	vst1q_u32(ret._u32, val);
	return ret;
}

#else

#error Unknown architecture.

#endif
