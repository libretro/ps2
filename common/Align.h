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
 * Macros rather than templates: every one of these is two operators on
 * an integer, the result type is the promoted type of the value, and
 * that is what a template over T computed anyway. Nothing here depends
 * on the width, so nothing here needs to know it.
 *
 * The Pow2 forms require a power of two and use a mask; the others take
 * any alignment and divide. Both evaluate their arguments more than
 * once, so neither takes an argument with a side effect -- no call site
 * in this tree does.
 */

#ifndef PCSX2_ALIGN_H
#define PCSX2_ALIGN_H

#include "Pcsx2Defs.h"

/* Any alignment. */
#define PCSX2_IS_ALIGNED(value, alignment)      (((value) % (alignment)) == 0)
#define PCSX2_ALIGN_UP(value, alignment)        (((value) + ((alignment) - 1)) / (alignment) * (alignment))

/* Power-of-two alignment: one mask instead of two divisions.
 *
 * The mask is built from the value, not from the alignment: an
 * alignment is an int, so ~(alignment - 1) is 32 bits and would clear
 * the whole top half of a 64-bit value. Multiplying by zero gives the
 * value's own promoted type, and the mask with it. Checked against the
 * templates these replaced over the u32 range and 64-bit values. */
#define PCSX2_ALIGN_UP_POW2(value, alignment) \
	(((value) + ((alignment) - 1)) & ~((0 * (value)) + (alignment) - 1))
#define PCSX2_ALIGN_DOWN_POW2(value, alignment) \
	((value) & ~((0 * (value)) + (alignment) - 1))

static __fi u32 pcsx2_bitfold32(u32 v)
{
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v;
}

/* Round up to a power of two; 0 stays 0. Folds the value's bits down
 * and adds one, which saturates at 32 bits -- every caller here passes
 * a u32 or narrower. */
#define PCSX2_NEXT_POW2(value) \
	((value) == 0 ? 0 : (pcsx2_bitfold32((value) - 1) + 1))

#define PCSX2_PAGE_ALIGN(size) PCSX2_ALIGN_UP_POW2(size, __pagesize)

#endif
