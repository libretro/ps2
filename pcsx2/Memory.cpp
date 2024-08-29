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

/*

RAM
---
0x00100000-0x01ffffff this is the physical address for the ram.its cached there
0x20100000-0x21ffffff uncached
0x30100000-0x31ffffff uncached & accelerated
0xa0000000-0xa1ffffff MIRROR might...???
0x80000000-0x81ffffff MIRROR might... ????

scratch pad
----------
0x70000000-0x70003fff scratch pad

BIOS
----
0x1FC00000 - 0x1FFFFFFF un-cached
0x9FC00000 - 0x9FFFFFFF cached
0xBFC00000 - 0xBFFFFFFF un-cached
*/

#include "IopHw.h"
#include "GS.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "DEV9/DEV9.h"
#include "CDVD/CDVD.h"
#include "Gif_Unit.h"
#include "Counters.h"

#include "HwInternal.h"
#include "BiosTools.h"
#include "SPU2/spu2.h"

#include "x86/microVU.h"

namespace HostMemoryMap
{
	extern "C" {
	uintptr_t EEmem, IOPmem, VUmem, bumpAllocator;
	}
} // namespace HostMemoryMap

/// Attempts to find a spot near static variables for the main memory
static VirtualMemoryManagerPtr AllocateVirtualMemory(const char* name, size_t size, size_t offset_from_base)
{
#if defined(_WIN32)
	// Everything looks nicer when the start of all the sections is a nice round looking number.
	// Also reduces the variation in the address due to small changes in code.
	// Breaks ASLR but so does anything else that tries to make addresses constant for our debugging pleasure
	uintptr_t codeBase = (uintptr_t)(void*)AllocateVirtualMemory / (1 << 28) * (1 << 28);

	// The allocation is ~640MB in size, slighly under 3*2^28.
	// We'll hope that the code generated for the PCSX2 executable stays under 512MB (which is likely)
	// On x86-64, code can reach 8*2^28 from its address [-6*2^28, 4*2^28] is the region that allows for code in the 640MB allocation 
	// to reach 512MB of code that either starts at codeBase or 256MB before it.
	// We start high and count down because on macOS code starts at the beginning of useable address space, so starting as far ahead 
	// as possible reduces address variations due to code size.  Not sure about other platforms.  Obviously this only actually 
	// affects what shows up in a debugger and won't affect performance or correctness of anything.
	for (int offset = 4; offset >= -6; offset--)
	{
		uintptr_t base = codeBase + (offset << 28) + offset_from_base;
		/* VTLB will throw a fit if we try to put EE main memory here */
		if ((intptr_t)base < 0 || (intptr_t)(base + size - 1) < 0)
			continue;
		VirtualMemoryManagerPtr mgr = std::make_shared<VirtualMemoryManager>(name, base, size, /*upper_bounds=*/0, /*strict=*/true);
		if (mgr->IsOk())
			return mgr;
	}
#endif
	return std::make_shared<VirtualMemoryManager>(name, 0, size);
}

// --------------------------------------------------------------------------------------
//  SysReserveVM  (implementations)
// --------------------------------------------------------------------------------------
SysMainMemory::SysMainMemory()
	: m_mainMemory(AllocateVirtualMemory("pcsx2", HostMemoryMap::MainSize, 0))
	, m_codeMemory(AllocateVirtualMemory(nullptr, HostMemoryMap::CodeSize, HostMemoryMap::MainSize))
	, m_bumpAllocator(m_mainMemory, HostMemoryMap::bumpAllocatorOffset, HostMemoryMap::MainSize - HostMemoryMap::bumpAllocatorOffset)
{
	uintptr_t main_base = (uintptr_t)MainMemory()->GetBase();
	uintptr_t code_base = (uintptr_t)MainMemory()->GetBase();
	HostMemoryMap::EEmem = main_base + HostMemoryMap::EEmemOffset;
	HostMemoryMap::IOPmem = main_base + HostMemoryMap::IOPmemOffset;
	HostMemoryMap::VUmem = main_base + HostMemoryMap::VUmemOffset;
	HostMemoryMap::bumpAllocator = main_base + HostMemoryMap::bumpAllocatorOffset;
}

SysMainMemory::~SysMainMemory()
{
	Release();
}

bool SysMainMemory::Allocate()
{
	Console.WriteLn(Color_StrongBlue, "Allocating host memory for virtual systems...");
	m_ee.Assign(MainMemory());
	m_iop.Assign(MainMemory());
	m_vu.Assign(MainMemory());

	vtlb_Core_Alloc();

	return true;
}

void SysMainMemory::Reset()
{
	Console.WriteLn(Color_StrongBlue, "Resetting host memory for virtual systems...");
	m_ee.Reset();
	m_iop.Reset();
	m_vu.Reset();

	// Note: newVif is reset as part of other VIF structures.
	// Software is reset on the GS thread.
}

void SysMainMemory::Release()
{
	Console.WriteLn(Color_Blue, "Releasing host memory for virtual systems...");

	vtlb_Core_Free(); // Just to be sure... (calling order could result in it getting missed during Decommit).

	m_ee.Release();
	m_iop.Release();
	m_vu.Release();
}

static u16 ba0R16(u32 mem)
{
	if (mem == 0x1a000006)
	{
		static int ba6;
		ba6++;
		if (ba6 == 3) ba6 = 0;
		return ba6;
	}
	return 0;
}

/////////////////////////////
// REGULAR MEM START
/////////////////////////////
static vtlbHandler
	null_handler,

	tlb_fallback_0,
	tlb_fallback_2,
	tlb_fallback_3,
	tlb_fallback_4,
	tlb_fallback_5,
	tlb_fallback_6,
	tlb_fallback_7,
	tlb_fallback_8,

	vu0_micro_mem,
	vu1_micro_mem,
	vu1_data_mem,

	hw_by_page[0x10] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},

	gs_page_0,
	gs_page_1,

	iopHw_by_page_01,
	iopHw_by_page_03,
	iopHw_by_page_08;


static void memMapVUmicro(void)
{
	// VU0/VU1 micro mem (instructions)
	// (Like IOP memory, these are generally only used by the EE Bios kernel during
	//  boot-up.  Applications/games are "supposed" to use the thread-safe VIF instead;
	//  or must ensure all VIF/GIF transfers are finished and all VUmicro execution stopped
	//  prior to accessing VU memory directly).

	// The VU0 mapping actually repeats 4 times across the mapped range, but we don't bother
	// to manually mirror it here because the indirect memory handler for it (see vuMicroRead*
	// functions below) automatically mask and wrap the address for us.

	vtlb_MapHandler(vu0_micro_mem,0x11000000,0x00004000);
	vtlb_MapHandler(vu1_micro_mem,0x11008000,0x00004000);

	// VU0/VU1 memory (data)
	// VU0 is 4k, mirrored 4 times across a 16k area.
	vtlb_MapBlock(vuRegs[0].Mem,0x11004000,0x00004000,0x1000);
	// Note: In order for the below conditional to work correctly
	// support needs to be coded to reset the memMappings when MTVU is
	// turned off/on. For now we just always use the vu data handlers...
	if (1||THREAD_VU1) vtlb_MapHandler(vu1_data_mem,0x1100c000,0x00004000);
	else               vtlb_MapBlock  (vuRegs[1].Mem,     0x1100c000,0x00004000);
}

static void memMapPhy(void)
{
	// Main memory
	vtlb_MapBlock(eeMem->Main,	0x00000000,Ps2MemSize::MainRam);//mirrored on first 256 mb ?
	// High memory, uninstalled on the configuration we emulate
	vtlb_MapHandler(null_handler, Ps2MemSize::MainRam, 0x10000000 - Ps2MemSize::MainRam);

	// Various ROMs (all read-only)
	vtlb_MapBlock(eeMem->ROM,	0x1fc00000, Ps2MemSize::Rom);
	vtlb_MapBlock(eeMem->ROM1,	0x1e000000, Ps2MemSize::Rom1);
	vtlb_MapBlock(eeMem->ROM2,	0x1e400000, Ps2MemSize::Rom2);

	// IOP memory
	// (used by the EE Bios Kernel during initial hardware initialization, Apps/Games
	//  are "supposed" to use the thread-safe SIF instead.)
	vtlb_MapBlock(iopMem->Main,0x1c000000,0x00800000);

	// Generic Handlers; These fallback to mem* stuff...
	vtlb_MapHandler(tlb_fallback_7,0x14000000, _64kb);
	vtlb_MapHandler(tlb_fallback_4,0x18000000, _64kb);
	vtlb_MapHandler(tlb_fallback_5,0x1a000000, _64kb);
	vtlb_MapHandler(tlb_fallback_6,0x12000000, _64kb);
	vtlb_MapHandler(tlb_fallback_8,0x1f000000, _64kb);
	vtlb_MapHandler(tlb_fallback_3,0x1f400000, _64kb);
	vtlb_MapHandler(tlb_fallback_2,0x1f800000, _64kb);
	vtlb_MapHandler(tlb_fallback_8,0x1f900000, _64kb);

	// Hardware Register Handlers : specialized/optimized per-page handling of HW register accesses
	// (note that hw_by_page handles are assigned in memReset prior to calling this function)

	for( uint i=0; i<16; ++i)
		vtlb_MapHandler(hw_by_page[i], 0x10000000 + (0x01000 * i), 0x01000);

	vtlb_MapHandler(gs_page_0, 0x12000000, 0x01000);
	vtlb_MapHandler(gs_page_1, 0x12001000, 0x01000);

	// "Secret" IOP HW mappings - Used by EE Bios Kernel during boot and generally
	// left untouched after that, as per EE/IOP thread safety rules.

	vtlb_MapHandler(iopHw_by_page_01, 0x1f801000, 0x01000);
	vtlb_MapHandler(iopHw_by_page_03, 0x1f803000, 0x01000);
	vtlb_MapHandler(iopHw_by_page_08, 0x1f808000, 0x01000);

}

//Why is this required ?
static void memMapKernelMem(void)
{
	//lower 512 mb: direct map
	//vtlb_VMap(0x00000000,0x00000000,0x20000000);
	//0x8* mirror
	vtlb_VMap(0x80000000, 0x00000000, _1mb*512);
	//0xa* mirror
	vtlb_VMap(0xA0000000, 0x00000000, _1mb*512);
}

static uint8_t  nullRead8(u32 mem)  { return 0; }
static uint16_t nullRead16(u32 mem) { return 0; }
static uint32_t nullRead32(u32 mem) { return 0; }
static uint64_t nullRead64(u32 mem) { return 0; }
static RETURNS_R128 nullRead128(u32 mem) { return r128_zero(); }
static void nullWrite8(u32 mem, uint8_t value)   { }
static void nullWrite16(u32 mem, uint16_t value) { }
static void nullWrite32(u32 mem, uint32_t value) { }
static void nullWrite64(u32 mem, uint64_t value) { }

static void TAKES_R128 nullWrite128(u32 mem, r128 value) { }

static uint8_t _ext_memRead8DEV9(u32 mem)
{
	return DEV9read8(mem & ~0xa4000000);
}

static uint8_t _ext_memRead8(u32 mem)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL);
	return 0;
}

static uint16_t _ext_memRead16DEV9(u32 mem)
{
	return DEV9read16(mem & ~0xa4000000);
}

static uint16_t _ext_memRead16_generic(u32 mem)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL);
	return 0;
}

static uint32_t _ext_memRead32DEV9(u32 mem)
{
	return DEV9read32(mem & ~0xa4000000);
}

static uint32_t _ext_memRead32(u32 mem)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL);
	return 0;
}

static u64 _ext_memRead64(u32 mem)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL);
	return 0;
}

static RETURNS_R128 _ext_memRead128GSM(u32 mem)
{
	return r128_load(PS2GS_BASE(mem));
}

static RETURNS_R128 _ext_memRead128(u32 mem)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL);
	return r128_zero();
}

static void _ext_memWrite8DEV9(u32 mem, uint8_t  value)
{
	DEV9write8(mem & ~0xa4000000, value);
}

static void _ext_memWrite8(u32 mem, uint8_t  value)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS);
}

static void _ext_memWrite16DEV9(u32 mem, uint16_t value)
{
	DEV9write16(mem & ~0xa4000000, value);
}

static void _ext_memWrite16(u32 mem, uint16_t value)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS);
}

static void _ext_memWrite32DEV9(u32 mem, uint32_t value)
{
	DEV9write32(mem & ~0xa4000000, value);
}

static void _ext_memWrite32(u32 mem, uint32_t value)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS);
}

static void _ext_memWrite64(u32 mem, uint64_t value)
{
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS);
}

static void TAKES_R128 _ext_memWrite128(u32 mem, r128 value)
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS);
}

// VU Micro Memory Reads...
static uint8_t vuMicroRead8VU0(u32 addr)
{
	return vuRegs[0].Micro[addr & 0xfff];
}

static uint8_t vuMicroRead8VU1(u32 addr)
{
	if (THREAD_VU1) vu1Thread.WaitVU();
	return vuRegs[1].Micro[addr & 0x3fff];
}

static uint16_t vuMicroRead16VU0(u32 addr)
{
	return *(u16*)&vuRegs[0].Micro[addr & 0xfff];
}

static uint16_t vuMicroRead16VU1(u32 addr)
{
	if (THREAD_VU1) vu1Thread.WaitVU();
	return *(u16*)&vuRegs[1].Micro[addr & 0x3fff];
}

static uint32_t vuMicroRead32VU0(u32 addr)
{
	return *(u32*)&vuRegs[0].Micro[addr & 0xfff];
}

static uint32_t vuMicroRead32VU1(u32 addr)
{
	if (THREAD_VU1) vu1Thread.WaitVU();
	return *(u32*)&vuRegs[1].Micro[addr & 0x3fff];
}

static uint64_t vuMicroRead64VU0(u32 addr)
{
	return *(u64*)&vuRegs[0].Micro[addr & 0xfff];
}

static uint64_t vuMicroRead64VU1(u32 addr)
{
	if (THREAD_VU1) vu1Thread.WaitVU();
	return *(u64*)&vuRegs[1].Micro[addr & 0x3fff];
}

static RETURNS_R128 vuMicroRead128VU0(u32 addr)
{
	return r128_load(&vuRegs[0].Micro[addr & 0xfff]);
}

static RETURNS_R128 vuMicroRead128VU1(u32 addr)
{
	if (THREAD_VU1) vu1Thread.WaitVU();
	return r128_load(&vuRegs[1].Micro[addr & 0x3fff]);
}

// Profiled VU writes: Happen very infrequently, with exception of BIOS initialization (at most twice per
//   frame in-game, and usually none at all after BIOS), so cpu clears aren't much of a big deal.
template<int vunum>
static void vuMicroWrite8(u32 addr,uint8_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;

	if (vunum && THREAD_VU1)
	{
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u8));
		return;
	}

	if (vu->Micro[addr] != data) // Clear before writing new data
	{
		/* (clearing 8 bytes because an instruction is 8 bytes) (cottonvibes) */
		if (vunum)
			mVUclear(microVU1, addr, 8);
		else
			mVUclear(microVU0, addr, 8);
		vu->Micro[addr] =data;
	}
}

template<int vunum>
static void vuMicroWrite16(u32 addr, uint16_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;

	if (vunum && THREAD_VU1)
	{
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u16));
		return;
	}

	if (*(u16*)&vu->Micro[addr] != data)
	{
		if (vunum)
			mVUclear(microVU1, addr, 8);
		else
			mVUclear(microVU0, addr, 8);
		*(u16*)&vu->Micro[addr] =data;
	}
}

template<int vunum>
static void vuMicroWrite32(u32 addr, uint32_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;

	if (vunum && THREAD_VU1)
	{
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u32));
		return;
	}

	if (*(u32*)&vu->Micro[addr] != data)
	{
		if (vunum)
			mVUclear(microVU1, addr, 8);
		else
			mVUclear(microVU0, addr, 8);
		*(u32*)&vu->Micro[addr] =data;
	}
}

template<int vunum>
static void vuMicroWrite64(u32 addr, uint64_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;

	if (vunum && THREAD_VU1)
	{
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u64));
		return;
	}

	if (*(u64*)&vu->Micro[addr] != data)
	{
		if (vunum)
			mVUclear(microVU1, addr, 8);
		else
			mVUclear(microVU0, addr, 8);
		*(u64*)&vu->Micro[addr] =data;
	}
}

static void TAKES_R128 vuMicroWrite128VU0(u32 addr, r128 data)
{
	const u128 udata = r128_to_u128(data);
	u128 comp        = (u128&)vuRegs[0].Micro[addr & 0xfff];
	if ((comp.lo != udata.lo) || (comp.hi != udata.hi))
	{
		mVUclear(microVU0, addr & 0xfff, 16);
		r128_store_unaligned(&vuRegs[0].Micro[addr & 0xfff],data);
	}
}

static void TAKES_R128 vuMicroWrite128VU1(u32 addr, r128 data)
{
	const u128 udata = r128_to_u128(data);
	if (THREAD_VU1)
		vu1Thread.WriteMicroMem(addr & 0x3fff, &udata, sizeof(u128));
	else
	{
		u128 comp  = (u128&)vuRegs[1].Micro[addr & 0x3fff];
		if ((comp.lo != udata.lo) || (comp.hi != udata.hi))
		{
			mVUclear(microVU1, addr & 0x3fff, 16);
			r128_store_unaligned(&vuRegs[1].Micro[addr & 0x3fff],data);
		}
	}
}

// VU Data Memory Reads...
template<int vunum>
static uint8_t vuDataRead8(u32 addr)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1) vu1Thread.WaitVU();
	return vu->Mem[addr];
}

template<int vunum>
static uint16_t vuDataRead16(u32 addr)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1) vu1Thread.WaitVU();
	return *(u16*)&vu->Mem[addr];
}

template<int vunum>
static uint32_t vuDataRead32(u32 addr)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1) vu1Thread.WaitVU();
	return *(u32*)&vu->Mem[addr];
}

template<int vunum>
static uint64_t vuDataRead64(u32 addr)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1) vu1Thread.WaitVU();
	return *(u64*)&vu->Mem[addr];
}

template<int vunum>
static RETURNS_R128 vuDataRead128(u32 addr)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1) vu1Thread.WaitVU();
	return r128_load(&vu->Mem[addr]);
}

// VU Data Memory Writes...
template<int vunum>
static void vuDataWrite8(u32 addr, uint8_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1)
		vu1Thread.WriteDataMem(addr, &data, sizeof(u8));
	else
		vu->Mem[addr] = data;
}

template<int vunum>
static void vuDataWrite16(u32 addr, uint16_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1)
		vu1Thread.WriteDataMem(addr, &data, sizeof(u16));
	else
		*(u16*)&vu->Mem[addr] = data;
}

template<int vunum>
static void vuDataWrite32(u32 addr, uint32_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1)
		vu1Thread.WriteDataMem(addr, &data, sizeof(u32));
	else 
		*(u32*)&vu->Mem[addr] = data;
}

template<int vunum>
static void vuDataWrite64(u32 addr, uint64_t data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1)
		vu1Thread.WriteDataMem(addr, &data, sizeof(u64));
	else 
		*(u64*)&vu->Mem[addr] = data;
}

template<int vunum> static void TAKES_R128 vuDataWrite128(u32 addr, r128 data)
{
	VURegs* vu = vunum ?  &vuRegs[1] :  &vuRegs[0];
	addr      &= vunum ? 0x3fff: 0xfff;
	if (vunum && THREAD_VU1)
	{
		alignas(16) const u128 udata = r128_to_u128(data);
		vu1Thread.WriteDataMem(addr, &udata, sizeof(u128));
		return;
	}
	r128_store_unaligned(&vu->Mem[addr], data);
}

void memSetPageAddr(u32 vaddr, u32 paddr)
{
	vtlb_VMap(vaddr,paddr,0x1000);
}

void memClearPageAddr(u32 vaddr)
{
	vtlb_VMapUnmap(vaddr,0x1000); // -> whut ?
}

///////////////////////////////////////////////////////////////////////////
// PS2 Memory Init / Reset / Shutdown

EEVM_MemoryAllocMess* eeMem = NULL;
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];

void memBindConditionalHandlers(void)
{
	if( hw_by_page[0xf] == 0xFFFFFFFF ) return;

	if (EmuConfig.Speedhacks.IntcStat)
	{
		vtlbMemR16FP* page0F16(hwRead16_page_0F_INTC_HACK);
		vtlbMemR32FP* page0F32(hwRead32_page_0F_INTC_HACK);

		vtlb_ReassignHandler( hw_by_page[0xf],
			hwRead8<0x0f>,	page0F16,		page0F32,		hwRead64<0x0f>,		hwRead128<0x0f>,
			hwWrite8<0x0f>,	hwWrite16<0x0f>,	hwWrite32<0x0f>,	hwWrite64<0x0f>,	hwWrite128<0x0f>
		);
	}
	else
	{
		vtlbMemR16FP* page0F16(hwRead16<0x0f>);
		vtlbMemR32FP* page0F32(hwRead32<0x0f>);

		vtlb_ReassignHandler( hw_by_page[0xf],
			hwRead8<0x0f>,	page0F16,		page0F32,		hwRead64<0x0f>,		hwRead128<0x0f>,
			hwWrite8<0x0f>,	hwWrite16<0x0f>,	hwWrite32<0x0f>,	hwWrite64<0x0f>,	hwWrite128<0x0f>
		);
	}
}

static __fi u8 gsRead8(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u8*)PS2GS_BASE(mem);
	/* Only SIGLBLID and CSR are readable, everything else mirrors CSR */
	return *(u8*)PS2GS_BASE(GS_CSR + (mem & 0xF));
}

static __fi u16 gsRead16(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u16*)PS2GS_BASE(mem);
	/* Only SIGLBLID and CSR are readable, everything else mirrors CSR */
	return *(u16*)PS2GS_BASE(GS_CSR + (mem & 0x7));
}

static __fi u32 gsRead32(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u32*)PS2GS_BASE(mem);
	/* Only SIGLBLID and CSR are readable, everything else mirrors CSR */
	return *(u32*)PS2GS_BASE(GS_CSR + (mem & 0xC));
}

static __fi u64 gsRead64(u32 mem)
{
	/* fixme - PS2GS_BASE(mem+4) = (g_RealGSMem+(mem + 4 & 0x13ff)) */
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u64*)PS2GS_BASE(mem);
	/* Only SIGLBLID and CSR are readable, everything else mirrors CSR */
	return *(u64*)PS2GS_BASE(GS_CSR + (mem & 0x8));
}

static __fi void gsCSRwrite( const tGS_CSR& csr )
{
	if (csr.RESET)
	{
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.gsFINISH.gsFINISHFired = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
		/* Privilage registers also reset. */
		memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		GSIMR.reset();
		CSRreg.Reset();
		MTGS::ResetGS(false);
	}

	if(csr.SIGNAL)
	{
		/* SIGNAL : What's not known here is whether
		 * or not the SIGID register should be updated
		 * here or when the IMR is cleared (below). */

		if (gifUnit.gsSIGNAL.queued) /* Firing pending signal */
		{
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~gifUnit.gsSIGNAL.data[1])
				        | (gifUnit.gsSIGNAL.data[0]&gifUnit.gsSIGNAL.data[1]);

			if (!GSIMR.SIGMSK)
				gsIrq();
			CSRreg.SIGNAL    = true; // Just to be sure :p
		}
		else
			CSRreg.SIGNAL    = false;
		gifUnit.gsSIGNAL.queued  = false;
		gifUnit.Execute<false>(true); // Resume paused transfers
	}

	if (csr.FINISH)
	{
		CSRreg.FINISH = false;
		gifUnit.gsFINISH.gsFINISHFired   = false; //Clear the previously fired FINISH (YS, Indiecar 2005, MGS3)
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if(csr.HSINT)	CSRreg.HSINT	= false;
	if(csr.VSINT)	CSRreg.VSINT	= false;
	if(csr.EDWINT)	CSRreg.EDWINT	= false;
}

static __fi void gsWrite8(u32 mem, u8 value)
{
	tGS_CSR tmp;
	tmp._u32 = value;
	switch (mem)
	{
		// CSR 8-bit write handlers.
		// I'm quite sure these would just write the CSR portion with the other
		// bits set to 0 (no action).  The previous implementation masked the 8-bit
		// write value against the previous CSR write value, but that really doesn't
		// make any sense, given that the real hardware's CSR circuit probably has no
		// real "memory" where it saves anything.  (for example, you can't write to
		// and change the GS revision or ID portions -- they're all hard wired.) --air

		case GS_CSR: // GS_CSR
			gsCSRwrite(tmp);
			break;
		case GS_CSR + 1: // GS_CSR
			tmp._u32 <<= 8;
			gsCSRwrite(tmp);
			break;
		case GS_CSR + 2: // GS_CSR
			tmp._u32 <<= 16;
			gsCSRwrite(tmp);
			break;
		case GS_CSR + 3: // GS_CSR
			tmp._u32 <<= 24;
			gsCSRwrite(tmp);
			break;
		default:
			*PS2GS_BASE(mem) = value;
		break;
	}
}

//////////////////////////////////////////////////////////////////////////
// GS Write 16 bit

static __fi void gsWrite16(u32 mem, u16 value)
{
	tGS_CSR tmp;
	tmp._u32 = value;
	switch (mem)
	{
		// See note above about CSR 8 bit writes, and handling them as zero'd bits
		// for all but the written parts.
		
		case GS_CSR+2:
			tmp._u32 <<= 16;
			// fallthrough
		case GS_CSR:
			gsCSRwrite(tmp);
			return; // do not write to MTGS memory
		case GS_IMR:
			if ((CSRreg._u32 & 0x1f) & (~value & GSIMR._u32) >> 8)
				gsIrq();
			GSIMR._u32 = (value & 0x1f00)|0x6000;
			return; // do not write to MTGS memory
	}

	*(u16*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 32 bit

static __fi void gsWrite32(u32 mem, u32 value)
{
	if (mem == GS_CSR)
	{
		tGS_CSR tmp;
		tmp._u32 = value;
		gsCSRwrite(tmp);
	}
	else if (GS_IMR)
	{
		if ((CSRreg._u32 & 0x1f) & (~value & GSIMR._u32) >> 8)
			gsIrq();
		GSIMR._u32 = (value & 0x1f00)|0x6000;
	}
	else
		*(u32*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 64 bit

static void gsWrite64_generic( u32 mem, u64 value )
{
	memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

static void gsWrite64_page_00( u32 mem, u64 value )
{
	s_GSRegistersWritten |= (mem == GS_DISPFB1 || mem == GS_DISPFB2 || mem == GS_PMODE);

	if (mem == GS_SMODE1 || mem == GS_SMODE2)
	{
		if (value != *(u64*)PS2GS_BASE(mem))
			UpdateVSyncRate(false);
	}

	memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

static void gsWrite64_page_01( u32 mem, u64 value )
{
	if (mem == GS_BUSDIR)
	{
		gifUnit.stat.DIR = static_cast<u32>(value) & 1;
		if (gifUnit.stat.DIR) /* Assume will do local->host transfer */
		{
			gifUnit.stat.OPH = true; /* Should we set OPH here? */
			gifUnit.FlushToMTGS(); /* Send any pending GS Primitives to the GS */
		}

		memcpy(PS2GS_BASE(mem), &value, sizeof(value));
	}
	else if (mem == GS_CSR)
	{
		tGS_CSR tmp;
		tmp._u64 = value;
		gsCSRwrite(tmp);
	}
	else if (mem == GS_IMR)
	{
		u32 _value = static_cast<u32>(value);
		if ((CSRreg._u32 & 0x1f) & (~_value & GSIMR._u32) >> 8)
			gsIrq();
		GSIMR._u32 = (_value & 0x1f00)|0x6000;
	}
	else
		memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

//////////////////////////////////////////////////////////////////////////
// GS Write 128 bit

static void TAKES_R128 gsWrite128_page_00( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	r128_store(PS2GS_BASE(mem), value);
}

static void TAKES_R128 gsWrite128_page_01( u32 mem, r128 value )
{
	if (mem == GS_CSR)
	{
		tGS_CSR tmp;
		tmp._u32 = r128_to_u32(value);
		gsCSRwrite(tmp);
	}
	else if (mem == GS_IMR)
	{
		u32 _value = r128_to_u32(value);
		if ((CSRreg._u32 & 0x1f) & (~_value & GSIMR._u32) >> 8)
			gsIrq();
		GSIMR._u32 = (_value & 0x1f00)|0x6000;
	}
	else
	{
		alignas(16) const u128 uvalue = r128_to_u128(value);
		r128_store(PS2GS_BASE(mem), value);
	}
}

static void TAKES_R128 gsWrite128_generic( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	r128_store(PS2GS_BASE(mem), value);
}

// --------------------------------------------------------------------------------------
//  eeMemoryReserve  (implementations)
// --------------------------------------------------------------------------------------
/* EE Main Memory */
eeMemoryReserve::eeMemoryReserve() : _parent() { }
eeMemoryReserve::~eeMemoryReserve() { Release(); }

void eeMemoryReserve::Assign(VirtualMemoryManagerPtr allocator)
{
	_parent::Assign(std::move(allocator), HostMemoryMap::EEmemOffset, sizeof(*eeMem));
	eeMem = reinterpret_cast<EEVM_MemoryAllocMess*>(GetPtr());
}


// Resets memory mappings, unmaps TLBs, reloads bios roms, etc.
void eeMemoryReserve::Reset()
{
	_parent::Reset();

	// Note!!  Ideally the vtlb should only be initialized once, and then subsequent
	// resets of the system hardware would only clear vtlb mappings, but since the
	// rest of the emu is not really set up to support a "soft" reset of that sort
	// we opt for the hard/safe version.
	vtlb_Init();

	null_handler = vtlb_RegisterHandler(nullRead8, nullRead16, nullRead32, nullRead64, nullRead128,
		nullWrite8, nullWrite16, nullWrite32, nullWrite64, nullWrite128);

	tlb_fallback_0 = vtlb_RegisterHandler(_ext_memRead8,_ext_memRead16_generic,_ext_memRead32,_ext_memRead64,_ext_memRead128, _ext_memWrite8, _ext_memWrite16,_ext_memWrite32,_ext_memWrite64,_ext_memWrite128);
	tlb_fallback_3 = vtlb_RegisterHandler(psxHw4Read8,_ext_memRead16_generic,_ext_memRead32,_ext_memRead64,_ext_memRead128, psxHw4Write8, _ext_memWrite16,_ext_memWrite32,_ext_memWrite64,_ext_memWrite128);
	tlb_fallback_4 = vtlb_RegisterHandler(_ext_memRead8, nullRead16,_ext_memRead32,_ext_memRead64,_ext_memRead128, _ext_memWrite8, _ext_memWrite16,_ext_memWrite32,_ext_memWrite64,_ext_memWrite128);
	tlb_fallback_5 = vtlb_RegisterHandler(_ext_memRead8, ba0R16,_ext_memRead32,_ext_memRead64,_ext_memRead128, _ext_memWrite8, nullWrite16,_ext_memWrite32,_ext_memWrite64,_ext_memWrite128);
	tlb_fallback_7 = vtlb_RegisterHandler(_ext_memRead8DEV9,_ext_memRead16DEV9,_ext_memRead32DEV9,_ext_memRead64,_ext_memRead128, _ext_memWrite8DEV9, _ext_memWrite16DEV9,_ext_memWrite32DEV9,_ext_memWrite64,_ext_memWrite128);
	tlb_fallback_8 = vtlb_RegisterHandler(_ext_memRead8, SPU2read,_ext_memRead32,_ext_memRead64,_ext_memRead128, _ext_memWrite8, SPU2write,_ext_memWrite32,_ext_memWrite64,_ext_memWrite128);

	// Dynarec versions of VUs
	vu0_micro_mem = vtlb_RegisterHandler(vuMicroRead8VU0,vuMicroRead16VU0,vuMicroRead32VU0,vuMicroRead64VU0,vuMicroRead128VU0, vuMicroWrite8<0>,vuMicroWrite16<0>,vuMicroWrite32<0>,vuMicroWrite64<0>,vuMicroWrite128VU0);
	vu1_micro_mem = vtlb_RegisterHandler(vuMicroRead8VU1,vuMicroRead16VU1,vuMicroRead32VU1,vuMicroRead64VU1,vuMicroRead128VU1, vuMicroWrite8<1>,vuMicroWrite16<1>,vuMicroWrite32<1>,vuMicroWrite64<1>,vuMicroWrite128VU1);

	vu1_data_mem = vtlb_RegisterHandler(vuDataRead8<1>,vuDataRead16<1>,vuDataRead32<1>,vuDataRead64<1>,vuDataRead128<1>, vuDataWrite8<1>,vuDataWrite16<1>,vuDataWrite32<1>,vuDataWrite64<1>,vuDataWrite128<1>);

	//////////////////////////////////////////////////////////////////////////////////////////
	// IOP's "secret" Hardware Register mapping, accessible from the EE (and meant for use
	// by debugging or BIOS only).  The IOP's hw regs are divided into three main pages in
	// the 0x1f80 segment, and then another oddball page for CDVD in the 0x1f40 segment.
	//

	using namespace IopMemory;

	tlb_fallback_2 = vtlb_RegisterHandler(
		iopHwRead8_generic, iopHwRead16_generic, iopHwRead32_generic, _ext_memRead64, _ext_memRead128,
		iopHwWrite8_generic, iopHwWrite16_generic, iopHwWrite32_generic, _ext_memWrite64, _ext_memWrite128
	);

	iopHw_by_page_01 = vtlb_RegisterHandler(
		iopHwRead8_Page1, iopHwRead16_Page1, iopHwRead32_Page1, _ext_memRead64, _ext_memRead128,
		iopHwWrite8_Page1, iopHwWrite16_Page1, iopHwWrite32_Page1, _ext_memWrite64, _ext_memWrite128
	);

	iopHw_by_page_03 = vtlb_RegisterHandler(
		iopHwRead8_Page3, iopHwRead16_Page3, iopHwRead32_Page3, _ext_memRead64, _ext_memRead128,
		iopHwWrite8_Page3, iopHwWrite16_Page3, iopHwWrite32_Page3, _ext_memWrite64, _ext_memWrite128
	);

	iopHw_by_page_08 = vtlb_RegisterHandler(
		iopHwRead8_Page8, iopHwRead16_Page8, iopHwRead32_Page8, _ext_memRead64, _ext_memRead128,
		iopHwWrite8_Page8, iopHwWrite16_Page8, iopHwWrite32_Page8, _ext_memWrite64, _ext_memWrite128
	);


	// psHw Optimized Mappings
	// The HW Registers have been split into pages to improve optimization.

#define hwHandlerTmpl(page) \
	hwRead8<page>,	hwRead16<page>,	hwRead32<page>,	hwRead64<page>,	hwRead128<page>, \
	hwWrite8<page>,	hwWrite16<page>,hwWrite32<page>,hwWrite64<page>,hwWrite128<page>

	hw_by_page[0x0] = vtlb_RegisterHandler( hwHandlerTmpl(0x00) );
	hw_by_page[0x1] = vtlb_RegisterHandler( hwHandlerTmpl(0x01) );
	hw_by_page[0x2] = vtlb_RegisterHandler( hwHandlerTmpl(0x02) );
	hw_by_page[0x3] = vtlb_RegisterHandler( hwHandlerTmpl(0x03) );
	hw_by_page[0x4] = vtlb_RegisterHandler( hwHandlerTmpl(0x04) );
	hw_by_page[0x5] = vtlb_RegisterHandler( hwHandlerTmpl(0x05) );
	hw_by_page[0x6] = vtlb_RegisterHandler( hwHandlerTmpl(0x06) );
	hw_by_page[0x7] = vtlb_RegisterHandler( hwHandlerTmpl(0x07) );
	hw_by_page[0x8] = vtlb_RegisterHandler( hwHandlerTmpl(0x08) );
	hw_by_page[0x9] = vtlb_RegisterHandler( hwHandlerTmpl(0x09) );
	hw_by_page[0xa] = vtlb_RegisterHandler( hwHandlerTmpl(0x0a) );
	hw_by_page[0xb] = vtlb_RegisterHandler( hwHandlerTmpl(0x0b) );
	hw_by_page[0xc] = vtlb_RegisterHandler( hwHandlerTmpl(0x0c) );
	hw_by_page[0xd] = vtlb_RegisterHandler( hwHandlerTmpl(0x0d) );
	hw_by_page[0xe] = vtlb_RegisterHandler( hwHandlerTmpl(0x0e) );
	hw_by_page[0xf] = vtlb_NewHandler();		// redefined later based on speedhacking prefs
	memBindConditionalHandlers();

	//////////////////////////////////////////////////////////////////////
	// GS Optimized Mappings

	tlb_fallback_6 = vtlb_RegisterHandler(
		gsRead8, gsRead16, gsRead32, gsRead64, _ext_memRead128GSM,
		gsWrite8, gsWrite16, gsWrite32, gsWrite64_generic, gsWrite128_generic
	);

	gs_page_0 = vtlb_RegisterHandler(
		gsRead8, gsRead16, gsRead32, gsRead64, _ext_memRead128GSM,
		gsWrite8, gsWrite16, gsWrite32, gsWrite64_page_00, gsWrite128_page_00
	);

	gs_page_1 = vtlb_RegisterHandler(
		gsRead8, gsRead16, gsRead32, gsRead64, _ext_memRead128GSM,
		gsWrite8, gsWrite16, gsWrite32, gsWrite64_page_01, gsWrite128_page_01
	);

	memMapPhy();
	memMapVUmicro();
	memMapKernelMem();

	vtlb_VMap(0x00000000,0x00000000,0x20000000);
	vtlb_VMapUnmap(0x20000000,0x60000000);

	if (!LoadBIOS())
		Console.Error("Failed to load BIOS");

	// Must happen after BIOS load, depends on BIOS version.
	cdvdLoadNVRAM();
}

void eeMemoryReserve::Release()
{
	eeMem = nullptr;
	_parent::Release();
}
