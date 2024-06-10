/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+
//

#pragma once

#include "Vif.h"
#include "Vif_HashBucket.h"
#include "VU.h"

typedef u32  (*nVifCall)(void*, const void*);
typedef void (*nVifrecCall)(uptr dest, uptr src);

extern void _nVifUnpack  (int idx, const u8* data, uint mode, bool isFill);
extern void  dVifReserve (int idx);
extern void  dVifReset   (int idx);
extern void  dVifRelease (int idx);
extern void  VifUnpackSSE_Init();
extern void  VifUnpackSSE_Destroy();

_vifT extern void dVifUnpack(const u8* data, bool isFill);

struct nVifStruct
{
	// Buffer for partial transfers (should always be first to ensure alignment)
	// Maximum buffer size is 256 (vifRegs.Num max range) * 16 (quadword)
	alignas(16) u8 buffer[256*16];
	u32            bSize; // Size of 'buffer'

	// VIF0 or VIF1 - provided for debugging helpfulness only, and is generally unused.
	// (templates are used for most or all VIF indexing)
	u32                     idx;

	RecompiledCodeReserve*  recReserve;
	u8*                     recWritePtr; // current write pos into the reserve

	HashBucket              vifBlocks;   // Vif Blocks


	nVifStruct() = default;
};

extern void resetNewVif(int idx);

alignas(16) extern nVifStruct nVif[2];
alignas(16) extern nVifCall nVifUpk[(2 * 2 * 16) * 4]; // ([USN][Masking][Unpack Type]) [curCycle]
alignas(16) extern u32      nVifMask[3][4][4];         // [MaskNumber][CycleNumber][Vector]
