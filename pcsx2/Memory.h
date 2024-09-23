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

#pragma once

#include "Config.h"
#include "VirtualMemory.h"
#include "vtlb.h"

#define PSM(mem)	(vtlb_GetPhyPtr((mem)&0x1fffffff)) //pcsx2 is a competition.The one with most hacks wins :D

#define psHu8(mem)	(*(u8 *)&eeHw[(mem) & 0xffff])
#define psHu16(mem)	(*(u16*)&eeHw[(mem) & 0xffff])
#define psHu32(mem)	(*(u32*)&eeHw[(mem) & 0xffff])
#define psHu64(mem)	(*(u64*)&eeHw[(mem) & 0xffff])
#define psHu128(mem)(*(u128*)&eeHw[(mem) & 0xffff])

#define psSu32(mem)	(*(u32 *)&eeMem->Scratch[(mem) & 0x3fff])
#define psSu64(mem)	(*(u64 *)&eeMem->Scratch[(mem) & 0x3fff])
#define psSu128(mem)	(*(u128*)&eeMem->Scratch[(mem) & 0x3fff])

#define memRead8 vtlb_memRead<mem8_t>
#define memRead16 vtlb_memRead<mem16_t>
#define memRead32 vtlb_memRead<mem32_t>
#define memRead64 vtlb_memRead<mem64_t>

#define memWrite8 vtlb_memWrite<mem8_t>
#define memWrite16 vtlb_memWrite<mem16_t>
#define memWrite32 vtlb_memWrite<mem32_t>
#define memWrite64 vtlb_memWrite<mem64_t>

// This is a table of default virtual map addresses for ps2vm components.  These locations
// are provided and used to assist in debugging and possibly hacking; as it makes it possible
// for a programmer to know exactly where to look (consistently!) for the base address of
// the various virtual machine components.  These addresses can be keyed directly into the
// debugger's disasm window to get disassembly of recompiled code, and they can be used to help
// identify recompiled code addresses in the callstack.

// All of these areas should be reserved as soon as possible during program startup, and its
// important that none of the areas overlap.  In all but superVU's case, failure due to overlap
// or other conflict will result in the operating system picking a preferred address for the mapping.

namespace HostMemoryMap
{
	//////////////////////////////////////////////////////////////////////////
	// Main
	//////////////////////////////////////////////////////////////////////////
	static const u32 MainSize = 0x14000000;

	// PS2 main memory, SPR, and ROMs (approximately 40.5MB, but we round up to 64MB for simplicity).
	static const u32 EEmemOffset   = 0x00000000;

	// IOP main memory and ROMs
	static const u32 IOPmemOffset  = 0x04000000;

	// VU0 and VU1 memory.
	static const u32 VUmemOffset   = 0x08000000;

	// Bump allocator for any other small allocations
	// size: Difference between it and HostMemoryMap::Size, so nothing should allocate higher than it!
	static const u32 bumpAllocatorOffset = 0x10000000;

	//////////////////////////////////////////////////////////////////////////
	// Code
	//////////////////////////////////////////////////////////////////////////
	static const u32 CodeSize = 0x13100000; // 305 mb

	// EE recompiler code cache area (64mb)
	static const u32 EErecOffset   = 0x00000000;

	// IOP recompiler code cache area (32mb)
	static const u32 IOPrecOffset  = 0x04000000;

	// newVif0 recompiler code cache area (8mb)
	static const u32 VIF0recOffset = 0x06000000;

	// newVif1 recompiler code cache area (8mb)
	static const u32 VIF1recOffset = 0x06800000;

	// microVU1 recompiler code cache area (64mb)
	static const u32 mVU0recOffset = 0x07000000;

	// microVU0 recompiler code cache area (64mb)
	static const u32 mVU1recOffset = 0x0B000000;

	// SSE-optimized VIF unpack functions (1mb)
	static const u32 VIFUnpackRecOffset = 0x0F000000;

	// Software Renderer JIT buffer (64mb)
	static const u32 SWrecOffset = 0x0F100000;
	static const u32 SWrecSize = 0x04000000;
}

// --------------------------------------------------------------------------------------
//  SysMainMemory
// --------------------------------------------------------------------------------------
// This class provides the main memory for the virtual machines.
class SysMainMemory final
{
protected:
	const VirtualMemoryManagerPtr m_mainMemory;
	const VirtualMemoryManagerPtr m_codeMemory;

	VirtualMemoryBumpAllocator m_bumpAllocator;

	eeMemoryReserve m_ee;
	iopMemoryReserve m_iop;
	vuMemoryReserve m_vu;

public:
	SysMainMemory();
	~SysMainMemory();

	const VirtualMemoryManagerPtr& MainMemory() { return m_mainMemory; }
	const VirtualMemoryManagerPtr& CodeMemory() { return m_codeMemory; }

	VirtualMemoryBumpAllocator& BumpAllocator() { return m_bumpAllocator; }

	const vuMemoryReserve& VUMemory() const { return m_vu; }

	bool Allocate();
	void Reset();
	void Release();
};

extern SysMainMemory& GetVmMemory();

extern void memBindConditionalHandlers(void);

static __fi void memRead128(u32 mem, mem128_t* out)        { r128_store(out, vtlb_memRead128(mem)); }
static __fi void memWrite128(u32 mem, const mem128_t* val) { vtlb_memWrite128(mem, r128_load(val)); }
