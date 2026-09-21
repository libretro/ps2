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

/* The 8x8 inverse DCT the slice decoders run on every block, and the two
 * ways its result leaves: clamped to bytes, or stored as 16-bit and added
 * into a destination. Both clear the block on the way out. */

#include "../../common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* block must be 16-byte aligned: the column pass loads and stores it a row
 * at a time with aligned moves. */
void ipu_idct_copy(s16 *block, u8 *dest, const int stride);

/* stride is the increment for dest in 16-bit units, always a multiple of
 * QWC. last == 129 with the low bits of block[0] not 4 takes the DC-only
 * shortcut. */
void ipu_idct_add(const int last, s16 *block, s16 *dest, const int stride);

#ifdef __cplusplus
}
#endif
