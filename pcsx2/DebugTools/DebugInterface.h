/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014  PCSX2 Dev Team
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
#include "../MemoryTypes.h"
#include "SymbolMap.h"

enum
{
	EECAT_GPR = 0,
	EECAT_CP0,
	EECAT_FPR,
	EECAT_FCR,
	EECAT_VU0F,
	EECAT_VU0I,
	EECAT_GSPRIV,
	EECAT_COUNT
};

enum
{
	IOPCAT_GPR = 0,
	IOPCAT_COUNT
};

class R5900DebugInterface
{
public:
	enum RegisterType
	{
		NORMAL,
		SPECIAL
	};
	u32 read8(u32 address);
	u32 read8(u32 address, bool& valid);
	u32 read16(u32 address);
	u32 read16(u32 address, bool& valid);
	u32 read32(u32 address);
	u32 read32(u32 address, bool& valid);
	u64 read64(u32 address);
	u64 read64(u32 address, bool& valid);
	u128 read128(u32 address);
	void write8(u32 address, u8 value);
	void write32(u32 address, u32 value);
};


extern R5900DebugInterface r5900Debug;
