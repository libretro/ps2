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

/* Bit-exact software model of the EE/VU floating point units, ported from
 * the hardware-derived PS2Float implementation (Booth-recoded multiplier
 * through carry-save adder trees, radix-2 SRT divider and square root,
 * flush-to-zero denormals, round-toward-zero).  Pure integer arithmetic:
 * results are identical on every host architecture by construction.
 *
 * Every operation returns the 32-bit result in the low word of a u64 and
 * the exception flags in the high word, so a result travels in one
 * register and no flag storage is written unless the caller asks. */

#ifndef PS2FLOAT_H
#define PS2FLOAT_H

#if defined(__cplusplus)
#include <cstddef>
#include <cstdint>
typedef uint8_t ps2f_u8;
typedef int32_t ps2f_s32;
typedef uint32_t ps2f_u32;
typedef uint64_t ps2f_u64;
extern "C" {
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t ps2f_u8;
typedef int32_t ps2f_s32;
typedef uint32_t ps2f_u32;
typedef uint64_t ps2f_u64;
#endif

#define PS2F_SIGNMASK 0x80000000u
#define PS2F_MAX_FLOATING_POINT_VALUE 0x7FFFFFFFu
#define PS2F_MIN_FLOATING_POINT_VALUE 0xFFFFFFFFu
#define PS2F_ONE 0x3F800000u
#define PS2F_MIN_ONE 0xBF800000u

/* Flag bits in the high word of a result. */
#define PS2F_OF (1ULL << 32) /* overflow */
#define PS2F_UF (1ULL << 33) /* underflow */
#define PS2F_DZ (1ULL << 34) /* divide by zero */
#define PS2F_IV (1ULL << 35) /* invalid */

#define ps2f_raw(r) ((ps2f_u32)(r))

ps2f_u64 ps2f_add(ps2f_u32 a, ps2f_u32 b);
ps2f_u64 ps2f_sub(ps2f_u32 a, ps2f_u32 b);
ps2f_u64 ps2f_mul(ps2f_u32 a, ps2f_u32 b);
ps2f_u64 ps2f_div(ps2f_u32 a, ps2f_u32 b);
ps2f_u64 ps2f_sqrt(ps2f_u32 a);
ps2f_u64 ps2f_rsqrt(ps2f_u32 a, ps2f_u32 b);

/* Fused chains: acc (+/-) s*t.  acc_of is the accumulator's sticky
 * overflow state (kept by the caller across MADDA/MSUBA chains); the
 * returned OF flag is the new sticky state. */
ps2f_u64 ps2f_madd(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of);
ps2f_u64 ps2f_msub(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of);
ps2f_u64 ps2f_madda(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of);
ps2f_u64 ps2f_msuba(ps2f_u32 acc, ps2f_u32 s, ps2f_u32 t, int acc_of);

/* VU EFU helpers. */
ps2f_u64 ps2f_ercpr(ps2f_u32 a);
ps2f_u64 ps2f_esqrt(ps2f_u32 a);
ps2f_u64 ps2f_esqur(ps2f_u32 a);
ps2f_u64 ps2f_ersqrt(ps2f_u32 a);

/* CLIP: returns (plus | (minus << 1)) for value f1 against bound f2. */
ps2f_u32 ps2f_clip(ps2f_u32 f1, ps2f_u32 f2);

/* Ordering of PS2 floats as signed magnitudes: -1, 0, 1. */
ps2f_s32 ps2f_compare(ps2f_u32 a, ps2f_u32 b);
/* Magnitude-only comparison: -1, 0, 1. */
ps2f_s32 ps2f_compare_operands(ps2f_u32 a, ps2f_u32 b);

#if defined(__cplusplus)
}
#endif

#endif /* PS2FLOAT_H */
