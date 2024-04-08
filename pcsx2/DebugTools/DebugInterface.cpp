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

#include "PrecompiledHeader.h"

#include "DebugInterface.h"
#include "Memory.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "VU.h"
#include "GS.h" // Required for gsNonMirroredRead()
#include "Counters.h"

#include "R3000A.h"
#include "IopMem.h"
#include "SymbolMap.h"

#include "common/StringUtil.h"

R5900DebugInterface r5900Debug;
R3000DebugInterface r3000Debug;

#ifdef _WIN32
#define strcasecmp stricmp
#endif

enum ReferenceIndexType
{
	REF_INDEX_PC       = 32,
	REF_INDEX_HI       = 33,
	REF_INDEX_LO       = 34,
	REF_INDEX_OPTARGET = 0x800,
	REF_INDEX_OPSTORE  = 0x1000,
	REF_INDEX_OPLOAD   = 0x2000,
	REF_INDEX_IS_OPSL  = REF_INDEX_OPTARGET | REF_INDEX_OPSTORE | REF_INDEX_OPLOAD,
	REF_INDEX_FPU      = 0x4000,
	REF_INDEX_FPU_INT  = 0x8000,
	REF_INDEX_VFPU     = 0x10000,
	REF_INDEX_VFPU_INT = 0x20000,
	REF_INDEX_IS_FLOAT = REF_INDEX_FPU | REF_INDEX_VFPU,

};

//
// R5900DebugInterface
//

u32 R5900DebugInterface::read8(u32 address)
{
	if (!isValidAddress(address))
		return -1;

	return memRead8(address);
}

u32 R5900DebugInterface::read8(u32 address, bool& valid)
{
	if (!(valid = isValidAddress(address)))
		return -1;

	return memRead8(address);
}


u32 R5900DebugInterface::read16(u32 address)
{
	if (!isValidAddress(address) || address % 2)
		return -1;

	return memRead16(address);
}

u32 R5900DebugInterface::read16(u32 address, bool& valid)
{
	if (!(valid = (isValidAddress(address) || address % 2)))
		return -1;

	return memRead16(address);
}

u32 R5900DebugInterface::read32(u32 address)
{
	if (!isValidAddress(address) || address % 4)
		return -1;

	return memRead32(address);
}

u32 R5900DebugInterface::read32(u32 address, bool& valid)
{
	if (!(valid = (isValidAddress(address) || address % 4)))
		return -1;

	return memRead32(address);
}

u64 R5900DebugInterface::read64(u32 address)
{
	if (!isValidAddress(address) || address % 8)
		return -1;

	return memRead64(address);
}

u64 R5900DebugInterface::read64(u32 address, bool& valid)
{
	if (!(valid = (isValidAddress(address) || address % 8)))
		return -1;

	return memRead64(address);
}

u128 R5900DebugInterface::read128(u32 address)
{
	alignas(16) u128 result;
	if (!isValidAddress(address) || address % 16)
	{
		result.hi = result.lo = -1;
		return result;
	}

	memRead128(address, result);
	return result;
}

void R5900DebugInterface::write8(u32 address, u8 value)
{
	if (!isValidAddress(address))
		return;

	memWrite8(address, value);
}

void R5900DebugInterface::write32(u32 address, u32 value)
{
	if (!isValidAddress(address))
		return;

	memWrite32(address, value);
}

u128 R5900DebugInterface::getHI()
{
	return cpuRegs.HI.UQ;
}

u128 R5900DebugInterface::getLO()
{
	return cpuRegs.LO.UQ;
}

u32 R5900DebugInterface::getPC()
{
	return cpuRegs.pc;
}

// Taken from COP0.cpp
bool R5900DebugInterface::getCPCOND0()
{
	return (((dmacRegs.stat.CIS | ~dmacRegs.pcr.CPC) & 0x3FF) == 0x3ff);
}

bool R5900DebugInterface::isValidAddress(u32 addr)
{
	u32 lopart = addr & 0xfFFffFF;

	// get rid of ee ram mirrors
	switch (addr >> 28)
	{
		case 0:
		case 2:
			// case 3: throw exception (not mapped ?)
			// [ 0000_8000 - 01FF_FFFF ] RAM
			// [ 2000_8000 - 21FF_FFFF ] RAM MIRROR
			// [ 3000_8000 - 31FF_FFFF ] RAM MIRROR
			if (lopart >= 0x80000 && lopart <= 0x1ffFFff)
				return !!vtlb_GetPhyPtr(lopart);
			break;
		case 1:
			// [ 1000_0000 - 1000_CFFF ] EE register
			if (lopart <= 0xcfff)
				return true;

			// [ 1100_0000 - 1100_FFFF ] VU mem
			if (lopart >= 0x1000000 && lopart <= 0x100FFff)
				return true;

			// [ 1200_0000 - 1200_FFFF ] GS regs
			if (lopart >= 0x2000000 && lopart <= 0x20010ff)
				return true;

			// [ 1E00_0000 - 1FFF_FFFF ] ROM
			// if (lopart >= 0xe000000)
			// 	return true; throw exception (not mapped ?)
			break;
		case 7:
			// [ 7000_0000 - 7000_3FFF ] Scratchpad
			if (lopart <= 0x3fff)
				return true;
			break;
		case 8:
		case 9:
		case 0xA:
		case 0xB:
			// [ 8000_0000 - BFFF_FFFF ] kernel
			return true;
		case 0xF:
			// [ 8000_0000 - BFFF_FFFF ] IOP or kernel stack
			if (lopart >= 0xfff8000)
				return true;
			break;
	}

	return false;
}

//
// R3000DebugInterface
//


u32 R3000DebugInterface::read8(u32 address)
{
	if (!isValidAddress(address))
		return -1;
	return iopMemRead8(address);
}

u32 R3000DebugInterface::read8(u32 address, bool& valid)
{
	if (!(valid = isValidAddress(address)))
		return -1;
	return iopMemRead8(address);
}

u32 R3000DebugInterface::read16(u32 address)
{
	if (!isValidAddress(address))
		return -1;
	return iopMemRead16(address);
}

u32 R3000DebugInterface::read16(u32 address, bool& valid)
{
	if (!(valid = isValidAddress(address)))
		return -1;
	return iopMemRead16(address);
}

u32 R3000DebugInterface::read32(u32 address)
{
	if (!isValidAddress(address))
		return -1;
	return iopMemRead32(address);
}

u32 R3000DebugInterface::read32(u32 address, bool& valid)
{
	if (!(valid = isValidAddress(address)))
		return -1;
	return iopMemRead32(address);

}

u64 R3000DebugInterface::read64(u32 address)
{
	return 0;
}

u64 R3000DebugInterface::read64(u32 address, bool& valid)
{
	return 0;
}


u128 R3000DebugInterface::read128(u32 address)
{
	return u128::From32(0);
}

void R3000DebugInterface::write8(u32 address, u8 value)
{
	if (!isValidAddress(address))
		return;

	iopMemWrite8(address, value);
}

void R3000DebugInterface::write32(u32 address, u32 value)
{
	if (!isValidAddress(address))
		return;

	iopMemWrite32(address, value);
}

u128 R3000DebugInterface::getHI()
{
	return u128::From32(psxRegs.GPR.n.hi);
}

u128 R3000DebugInterface::getLO()
{
	return u128::From32(psxRegs.GPR.n.lo);
}

u32 R3000DebugInterface::getPC()
{
	return psxRegs.pc;
}

bool R3000DebugInterface::getCPCOND0()
{
	return false;
}

bool R3000DebugInterface::isValidAddress(u32 addr)
{
	if (addr >= 0x10000000 && addr < 0x10010000)
		return true;
	if (addr >= 0x12000000 && addr < 0x12001100)
		return true;
	if (addr >= 0x70000000 && addr < 0x70004000)
		return true;

	return !(addr & 0x40000000) && vtlb_GetPhyPtr(addr & 0x1FFFFFFF) != NULL;
}
