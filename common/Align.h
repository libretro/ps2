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

/* Alignment arithmetic.
 *
 * Inline functions, one per width, rather than macros: several callers
 * pass a function call as the value -- a rectangle's width, an upload
 * pitch computed from one -- and a macro that names its argument more
 * than once calls those once per mention. The templates these replace
 * evaluated the argument once, and so do these.
 *
 * Two widths, because that is what the tree uses: u32 for pitches and
 * texture dimensions, uptr for addresses and sizes. There is no
 * dispatch, so a caller names the one its value is.
 *
 * The _pow2 forms require a power-of-two alignment and use a mask; the
 * others take any alignment and divide.
 */

#ifndef PCSX2_ALIGN_H
#define PCSX2_ALIGN_H

#include "Pcsx2Defs.h"

/* Any alignment. */
static __fi u32  pcsx2_align_up_u32(u32 value, u32 alignment)
{
	return (value + (alignment - 1)) / alignment * alignment;
}

static __fi uptr pcsx2_align_up_ptr(uptr value, uptr alignment)
{
	return (value + (alignment - 1)) / alignment * alignment;
}

static __fi int  pcsx2_is_aligned_u32(u32 value, u32 alignment)
{
	return (value % alignment) == 0;
}

/* Power of two: one mask rather than two divisions. The mask is built
 * at the value's own width, which is the whole reason these are not
 * macros over an int alignment -- ~(alignment - 1) in int clears the
 * top half of a 64-bit value. */
static __fi u32  pcsx2_align_up_pow2_u32(u32 value, u32 alignment)
{
	return (value + (alignment - 1)) & ~(alignment - 1);
}

static __fi uptr pcsx2_align_up_pow2_ptr(uptr value, uptr alignment)
{
	return (value + (alignment - 1)) & ~(alignment - 1);
}

static __fi u32  pcsx2_align_down_pow2_u32(u32 value, u32 alignment)
{
	return value & ~(alignment - 1);
}

static __fi uptr pcsx2_align_down_pow2_ptr(uptr value, uptr alignment)
{
	return value & ~(alignment - 1);
}

/* Round up to a power of two; 0 stays 0. */
static __fi u32 pcsx2_next_pow2_u32(u32 value)
{
	if (value == 0)
		return 0;
	value--;
	value |= value >> 1;
	value |= value >> 2;
	value |= value >> 4;
	value |= value >> 8;
	value |= value >> 16;
	return value + 1;
}

static __fi uptr pcsx2_page_align(uptr size)
{
	return pcsx2_align_up_pow2_ptr(size, (uptr)__pagesize);
}

#endif
