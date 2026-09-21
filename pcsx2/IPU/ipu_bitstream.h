/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#pragma once

/* Reading bits out of the IPU's two-quadword window.
 *
 * Everything here is a pure function of the window and a bit position: the
 * caller owns the position and the refilling, so these never touch the FIFO
 * and never advance anything. The window is 32 bytes and the position runs
 * 0..127, so a 64-bit read at position 127 reaches byte 23 and stays inside
 * it -- which is why a caller must have filled the second quadword before
 * asking for more bits than the first one holds.
 *
 * base points at the window as bytes. Reads are big-endian: bit 0 of the
 * stream is the top bit of byte 0.
 *
 * The host is assumed little-endian, which every target this builds for is
 * -- Pcsx2Defs.h accepts only x86 and arm64. The byteswap below is
 * unconditional, so on a big-endian host these would return the bytes in
 * the wrong order rather than reaching the same bits by doing nothing. */

#include <string.h>

#include "../../common/Pcsx2Defs.h"
#include "../../common/Pcsx2Types.h"

#if defined(_MSC_VER)
#define IPU_BSWAP32(x) _byteswap_ulong(x)
#else
#define IPU_BSWAP32(x) __builtin_bswap32(x)
#endif

/* The bit position is byte-granular at best, so every window read is an
 * unaligned one and goes through memcpy: a typed load at a computed byte
 * offset is undefined both for the alignment and for the aliasing, and on
 * the targets that matter memcpy of a fixed width is the same single
 * instruction. */
static PCSX2_INLINE u32 load_be32(const u8 *p)
{
	u32 v;
	memcpy(&v, p, sizeof(v));
	return IPU_BSWAP32(v);
}

/* The next `bits` bits at `bp`, zero-extended.
 *
 * These serve the request out of one 32-bit word, so the request has to fit
 * in the part of that word at or after the bit position:
 *
 *     bits + (bp & 7) <= 32
 *
 * which is 25 bits at the worst alignment and 32 on a byte boundary. The
 * decoder's widest read is 24, so it is inside that at every position. A
 * wider one silently returns zeroes in the low bits instead of the bits
 * that follow, so anything that wants more than 25 has to read twice or
 * these have to start loading 64 bits. tests/ipu/bitstream_hash.cpp checks
 * right up to the boundary. */
static PCSX2_INLINE u32 ipu_bits_u32(const u8 *base, u32 bp, u32 bits)
{
	const u32 shift = bp & 7;
	const u32 word  = load_be32(base + (bp / 8));

	/* The shift pair is (32 - bits) overall, and both halves stay below
	 * 32 because bits is 1..32 and shift is 0..7 -- but only once the
	 * left shift has happened, so it is taken in the unsigned domain
	 * where it is defined whatever the top bit was. */
	return (word << shift) >> (32 - bits);
}

static PCSX2_INLINE s32 ipu_bits_s32(const u8 *base, u32 bp, u32 bits)
{
	const u32 shift = bp & 7;
	const u32 word  = load_be32(base + (bp / 8));

	/* Left in the unsigned domain, then right as signed so the top bit
	 * read propagates. Shifting a negative value left is undefined, which
	 * is why the two halves are not both taken as s32. */
	return (s32)(word << shift) >> (32 - bits);
}

static PCSX2_INLINE void ipu_bits_copy8(const u8 *base, u32 bp, u8 *dst)
{
	const u8 * const p   = base + (bp / 8);
	const u32 shift      = bp & 7;

	if (shift)
	{
		const u32 mask = 0xffu >> shift;
		*dst = (u8)((((~mask) & p[1]) >> (8 - shift)) | ((mask & p[0]) << shift));
	}
	else
		*dst = p[0];
}

static PCSX2_INLINE void ipu_bits_copy32(const u8 *base, u32 bp, u8 *dst)
{
	const u8 * const p = base + (bp / 8);
	const u32 shift    = bp & 7;
	u32 lo, hi, merged;

	if (shift)
	{
		u32 mask = 0xffu >> shift;
		mask = mask | (mask << 8) | (mask << 16) | (mask << 24);

		memcpy(&lo, p, sizeof(lo));
		memcpy(&hi, p + 1, sizeof(hi));
		merged = ((~mask & hi) >> (8 - shift)) | ((mask & lo) << shift);
	}
	else
		memcpy(&merged, p, sizeof(merged));

	memcpy(dst, &merged, sizeof(merged));
}

static PCSX2_INLINE void ipu_bits_copy64(const u8 *base, u32 bp, u8 *dst)
{
	const u8 * const p = base + (bp / 8);
	const u32 shift    = bp & 7;
	u64 lo, hi, merged;

	if (shift)
	{
		u64 mask = 0xffu >> shift;
		mask = mask | (mask << 8) | (mask << 16) | (mask << 24)
		     | (mask << 32) | (mask << 40) | (mask << 48) | (mask << 56);

		memcpy(&lo, p, sizeof(lo));
		memcpy(&hi, p + 1, sizeof(hi));
		merged = ((~mask & hi) >> (8 - shift)) | ((mask & lo) << shift);
	}
	else
		memcpy(&merged, p, sizeof(merged));

	memcpy(dst, &merged, sizeof(merged));
}
