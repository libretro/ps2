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

/* The two kernels the CSC and PACK commands finish with. Both take what
 * they work on rather than reaching for the decoder or the register file,
 * so neither needs anything else to be tested. */

#include "ipu_macroblock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Colour conversion, then the two alpha thresholds, then the optional sign
 * flip. thresh is the pair at 0x1F... TH0/TH1: a pixel darker than thresh[0]
 * in all three channels goes fully transparent, one darker than thresh[1]
 * gets alpha 0x40. sgn flips the top bit of each channel. */
void ipu_csc(const macroblock_8 *mb8, macroblock_rgb32 *rgb32, int sgn,
             const u16 *thresh);

/* Vector quantisation against a 16-entry CLUT: each pixel becomes the index
 * of the nearest entry by squared distance, two pixels to a byte, low nibble
 * first. Ties go to the lower index, which is the sequential scan the manual
 * describes in 8.6.3. */
void ipu_vq(const macroblock_rgb16 *rgb16, u8 *indx4, const rgb16_t *clut);

#ifdef __cplusplus
}
#endif
