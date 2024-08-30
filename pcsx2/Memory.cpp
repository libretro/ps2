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

#include <cstring> /* memset/memcpy */
#include <cinttypes>
#include <map>
#include <unordered_set>
#include <unordered_map>

#include "common/Align.h"
#include "common/Console.h"

#include "Common.h"
#include "Cache.h"
#include "vtlb.h"

#include "COP0.h"
#include "Cache.h"
#include "IopMem.h"
#include "Host.h"
#include "VMManager.h"
#include "VirtualMemory.h"

/* The lower parts of a cache tags structure is as follows:
 * 31 - 12: The physical address cache tag.
 * 11 - 7: Unused.
 * 6: Dirty flag.
 * 5: Valid flag.
 * 4: LRF flag - least recently filled flag.
 * 3: Lock flag.
 * 2-0: Unused.
 */

#define DIRTY_FLAG 0x40
#define VALID_FLAG 0x20
#define LRF_FLAG 0x10
#define LOCK_FLAG 0x8
#define ALL_FLAGS 0xFFF

#define cpuTlbMiss(addr, bd, excode) \
	cpuRegs.CP0.n.BadVAddr = addr; \
	cpuRegs.CP0.n.Context &= 0xFF80000F; \
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0; \
	cpuRegs.CP0.n.EntryHi  = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF); \
	cpuRegs.pc            -= 4; \
	cpuException(excode, bd)

union alignas(64) CacheData
{
	uint8_t bytes[64];
};

struct CacheSet
{
	uintptr_t tags[2];
	CacheData data[2];
};

struct Cache
{
	CacheSet sets[64];
};

struct CacheLine
{
	uintptr_t tag;
	CacheData& data;
	int set;
};

static Cache cache = {};

template <typename Op>
static void doCacheHitOp(u32 addr, Op op)
{
	const int index = (addr >> 6) & 0x3F;
	CacheSet& set   = cache.sets[index];
	vtlb_private::VTLBVirtual vmv = vtlb_private::vtlbdata.vmap[addr >> vtlb_private::VTLB_PAGE_BITS];
	uintptr_t ppf   = vmv.assumePtr(addr);
	if (!(!(set.tags[0] & VALID_FLAG) && (set.tags[0] & ~ALL_FLAGS) == (ppf & ~ALL_FLAGS)))
		op({ cache.sets[index].tags[0], cache.sets[index].data[0], index });
	else if (!(!(set.tags[1] & VALID_FLAG) && (set.tags[1] & ~ALL_FLAGS) == (ppf & ~ALL_FLAGS)))
		op({ cache.sets[index].tags[1], cache.sets[index].data[1], index });
}

/*
	EE physical map :
	[0000 0000,1000 0000) -> Ram (mirrored ?)
	[1000 0000,1400 0000) -> Registers
	[1400 0000,1fc0 0000) -> Reserved (ingored writes, 'random' reads)
	[1fc0 0000,2000 0000) -> Boot ROM

	[2000 0000,4000 0000) -> Unmapped (BUS ERROR)
	[4000 0000,8000 0000) -> "Extended memory", probably unmapped (BUS ERROR) on retail ps2's :)
	[8000 0000,FFFF FFFF] -> Unmapped (BUS ERROR)

	vtlb/phy only supports the [0000 0000,2000 0000) region, with 4k pages.
	vtlb/vmap supports mapping to either of these locations, or some other (externaly) specified address.
*/

using namespace vtlb_private;

namespace vtlb_private
{
	alignas(64) MapData vtlbdata;

	static bool PageFaultHandler(const PageFaultInfo& info);
} // namespace vtlb_private

static u32 vtlbHandlerCount = 0;

static u32 DefaultPhyHandler;
static u32 UnmappedVirtHandler;
static u32 UnmappedPhyHandler;

struct FastmemVirtualMapping
{
	uint32_t offset;
	uint32_t size;
};

struct LoadstoreBackpatchInfo
{
	uint32_t guest_pc;
	uint32_t gpr_bitmask;
	uint32_t fpr_bitmask;
	u8 code_size;
	u8 address_register;
	u8 data_register;
	u8 size_in_bits;
	bool is_signed;
	bool is_load;
	bool is_fpr;
};

static constexpr size_t FASTMEM_AREA_SIZE = 0x100000000ULL;
static constexpr uint32_t FASTMEM_PAGE_COUNT = FASTMEM_AREA_SIZE / VTLB_PAGE_SIZE;
static constexpr uint32_t NO_FASTMEM_MAPPING = 0xFFFFFFFFu;

static std::unique_ptr<SharedMemoryMappingArea> s_fastmem_area;
static std::vector<uint32_t> s_fastmem_virtual_mapping; // maps vaddr -> mainmem offset
static std::unordered_multimap<u32, u32> s_fastmem_physical_mapping; // maps mainmem offset -> vaddr
static std::unordered_map<uintptr_t, LoadstoreBackpatchInfo> s_fastmem_backpatch_info;
static std::unordered_set<u32> s_fastmem_faulting_pcs;

vtlb_private::VTLBPhysical vtlb_private::VTLBPhysical::fromPointer(intptr_t ptr)
{
	return VTLBPhysical(ptr);
}

vtlb_private::VTLBPhysical vtlb_private::VTLBPhysical::fromHandler(u32 handler)
{
	return VTLBPhysical(handler | POINTER_SIGN_BIT);
}

vtlb_private::VTLBVirtual::VTLBVirtual(VTLBPhysical phys, u32 paddr, u32 vaddr)
{
	if (phys.isHandler())
		value = phys.raw() + paddr - vaddr;
	else
		value = phys.raw() - vaddr;
}

static __inline int CheckCache(uint32_t addr)
{
	if (((cpuRegs.CP0.n.Config >> 16) & 0x1) != 0)
	{
		uint32_t mask;
		for (int i = 1; i < 48; i++)
		{
			if (((tlb[i].EntryLo1 & 0x38) >> 3) == 0x3)
			{
				mask = tlb[i].PageMask;

				if ((addr >= tlb[i].PFN1) && (addr <= tlb[i].PFN1 + mask))
					return true;
			}
			if (((tlb[i].EntryLo0 & 0x38) >> 3) == 0x3)
			{
				mask = tlb[i].PageMask;

				if ((addr >= tlb[i].PFN0) && (addr <= tlb[i].PFN0 + mask))
					return true;
			}
		}
	}
	return false;
}
// --------------------------------------------------------------------------------------
// Interpreter Implementations of VTLB Memory Operations.
// --------------------------------------------------------------------------------------
// See recVTLB.cpp for the dynarec versions.

static int getFreeCache(u32 mem, int* way)
{
	const int setIdx               = (mem >> 6) & 0x3F;
	CacheSet& set                  = cache.sets[setIdx];
	vtlb_private::VTLBVirtual vmv  = vtlb_private::vtlbdata.vmap[mem >> vtlb_private::VTLB_PAGE_BITS];
	uintptr_t ppf                  = vmv.assumePtr(mem);

	if (!(!(set.tags[0] & VALID_FLAG) && (set.tags[0] & ~ALL_FLAGS) == (ppf & ~ALL_FLAGS))) *way = 0;
	else if (!(!(set.tags[1] & VALID_FLAG) && (set.tags[1] & ~ALL_FLAGS) == (ppf & ~ALL_FLAGS))) *way = 1;
	else
	{
		*way           = (set.tags[0] & LRF_FLAG) ^ (set.tags[1] & LRF_FLAG);
		CacheLine line = { cache.sets[setIdx].tags[*way], cache.sets[setIdx].data[*way], setIdx };

		if (((line.tag & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
		{
			uintptr_t target = (line.tag & ~ALL_FLAGS) | (line.set << 6);
			*reinterpret_cast<CacheData*>(target) = line.data;
			line.tag &= ~DIRTY_FLAG;
		}
		line.tag &= ALL_FLAGS;
		line.tag |= (ppf & ~ALL_FLAGS);
		memcpy(&line.data, reinterpret_cast<void*>(ppf & ~0x3FULL), sizeof(line.data));
		line.tag |=  VALID_FLAG;
		line.tag &= ~DIRTY_FLAG;
		line.tag ^= LRF_FLAG;
	}

	return setIdx;
}


template <bool Write, int Bytes>
static void* prepareCacheAccess(u32 mem)
{
	int way        = 0;
	int idx        = getFreeCache(mem, &way);
	CacheLine line = { cache.sets[idx].tags[way], cache.sets[idx].data[way], idx };
	if (Write)
		line.tag |= DIRTY_FLAG;
	u32 aligned = mem & ~(Bytes - 1);
	return &line.data.bytes[aligned & 0x3f];
}


uint8_t vtlb_memRead8(uint32_t addr)
{
	static const uint DataSize = sizeof(uint8_t) * 8;
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr))
	{
		if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
		{
			void *_addr = prepareCacheAccess<false, sizeof(uint8_t)>(addr);
			return *reinterpret_cast<uint8_t*>(_addr);
		}
		return *reinterpret_cast<uint8_t*>(vmv.assumePtr(addr));
	}
	//has to: translate, find function, call function
	uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
	return vmv.assumeHandler<8, false>()(paddr);
}

uint16_t vtlb_memRead16(uint32_t addr)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr))
	{
		if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
		{
			void *_addr = prepareCacheAccess<false, sizeof(uint16_t)>(addr);
			return *reinterpret_cast<uint16_t*>(_addr);
		}
		return *reinterpret_cast<uint16_t*>(vmv.assumePtr(addr));
	}

	//has to: translate, find function, call function
	uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
	return vmv.assumeHandler<16, false>()(paddr);
}

uint32_t vtlb_memRead32(uint32_t addr)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr))
	{
		if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
		{
			void *_addr = prepareCacheAccess<false, sizeof(uint32_t)>(addr);
			return *reinterpret_cast<uint32_t*>(_addr);
		}

		return *reinterpret_cast<uint32_t*>(vmv.assumePtr(addr));
	}

	//has to: translate, find function, call function
	uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
	return vmv.assumeHandler<32, false>()(paddr);
}

uint64_t vtlb_memRead64(uint32_t addr)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(addr))
	{
		if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
		{
			void *_addr = prepareCacheAccess<false, sizeof(uint64_t)>(addr);
			return *reinterpret_cast<uint64_t*>(_addr);
		}

		return *reinterpret_cast<uint64_t*>(vmv.assumePtr(addr));
	}

	//has to: translate, find function, call function
	uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
	return vmv.assumeHandler<64, false>()(paddr);
}

RETURNS_R128 vtlb_memRead128(uint32_t mem)
{
	auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	if (vmv.isHandler(mem))
	{
		//has to: translate, find function, call function
		uint32_t paddr = vmv.assumeHandlerGetPAddr(mem);
		return vmv.assumeHandler<128, false>()(paddr);
	}
	if (!CHECK_EEREC)
	{
		if (CHECK_CACHE && CheckCache(mem))
		{
			void *addr = prepareCacheAccess<false, sizeof(mem128_t)>(mem);
			r128 value = r128_load(addr);
			u64* vptr = reinterpret_cast<u64*>(&value);
			return value;
		}
	}

	return r128_load(reinterpret_cast<const void*>(vmv.assumePtr(mem)));
}

void vtlb_memWrite8(uint32_t addr, uint8_t data)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (vmv.isHandler(addr))
	{
		//has to: translate, find function, call function
		uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
		return vmv.assumeHandler<sizeof(uint8_t) * 8, true>()(paddr, data);
	}

	if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
	{
		void *_addr = prepareCacheAccess<true, sizeof(uint8_t)>(addr);
		*reinterpret_cast<uint8_t*>(_addr) = data;
	}

	*reinterpret_cast<uint8_t*>(vmv.assumePtr(addr)) = data;
}

void vtlb_memWrite16(uint32_t addr, uint16_t data)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (vmv.isHandler(addr))
	{
		//has to: translate, find function, call function
		uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
		return vmv.assumeHandler<sizeof(uint16_t) * 8, true>()(paddr, data);
	}

	if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
	{
		void *_addr = prepareCacheAccess<true, sizeof(uint16_t)>(addr);
		*reinterpret_cast<uint16_t*>(_addr) = data;
	}

	*reinterpret_cast<uint16_t*>(vmv.assumePtr(addr)) = data;
}

void vtlb_memWrite32(uint32_t addr, uint32_t data)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (vmv.isHandler(addr))
	{
		//has to: translate, find function, call function
		uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
		return vmv.assumeHandler<sizeof(uint32_t) * 8, true>()(paddr, data);
	}

	if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
	{
		void *_addr = prepareCacheAccess<true, sizeof(uint32_t)>(addr);
		*reinterpret_cast<uint32_t*>(_addr) = data;
	}

	*reinterpret_cast<uint32_t*>(vmv.assumePtr(addr)) = data;
}

void vtlb_memWrite64(uint32_t addr, uint64_t data)
{
	auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];

	if (vmv.isHandler(addr))
	{
		//has to: translate, find function, call function
		uint32_t paddr = vmv.assumeHandlerGetPAddr(addr);
		return vmv.assumeHandler<sizeof(uint64_t) * 8, true>()(paddr, data);
	}

	if (!CHECK_EEREC && CHECK_CACHE && CheckCache(addr))
	{
		void *_addr = prepareCacheAccess<true, sizeof(uint64_t)>(addr);
		*reinterpret_cast<uint64_t*>(_addr) = data;
	}

	*reinterpret_cast<uint64_t*>(vmv.assumePtr(addr)) = data;
}

void TAKES_R128 vtlb_memWrite128(uint32_t mem, r128 value)
{
	auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	if (vmv.isHandler(mem))
	{
		//has to: translate, find function, call function
		uint32_t paddr = vmv.assumeHandlerGetPAddr(mem);
		vmv.assumeHandler<128, true>()(paddr, value);
	}
	else
	{
		if (!CHECK_EEREC && CHECK_CACHE && CheckCache(mem))
		{
			alignas(16) const u128 r = r128_to_u128(value);
			void* addr = prepareCacheAccess<true, sizeof(mem128_t)>(mem);
			*reinterpret_cast<mem128_t*>(addr) = r;
			return;
		}

		r128_store_unaligned((void*)vmv.assumePtr(mem), value);
	}
}

bool vtlb_ramRead8(u32 addr, uint8_t* value)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
	{
		memset(value, 0, sizeof(uint8_t));
		return false;
	}
	memcpy(value, reinterpret_cast<uint8_t*>(vmv.assumePtr(addr)), sizeof(uint8_t));
	return true;
}

bool vtlb_ramRead16(u32 addr, uint16_t* value)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
	{
		memset(value, 0, sizeof(uint16_t));
		return false;
	}

	memcpy(value, reinterpret_cast<uint16_t*>(vmv.assumePtr(addr)), sizeof(uint16_t));
	return true;
}

bool vtlb_ramRead32(u32 addr, uint32_t* value)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
	{
		memset(value, 0, sizeof(uint32_t));
		return false;
	}

	memcpy(value, reinterpret_cast<uint32_t*>(vmv.assumePtr(addr)), sizeof(uint32_t));
	return true;
}

bool vtlb_ramRead64(u32 addr, uint64_t* value)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
	{
		memset(value, 0, sizeof(uint64_t));
		return false;
	}

	memcpy(value, reinterpret_cast<uint64_t*>(vmv.assumePtr(addr)), sizeof(uint64_t));
	return true;
}

bool vtlb_ramWrite8(u32 addr, const uint8_t& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;
	memcpy(reinterpret_cast<uint8_t*>(vmv.assumePtr(addr)), &data, sizeof(uint8_t));
	return true;
}

bool vtlb_ramWrite16(u32 addr, const uint16_t& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;

	memcpy(reinterpret_cast<uint16_t*>(vmv.assumePtr(addr)), &data, sizeof(uint16_t));
	return true;
}

bool vtlb_ramWrite32(u32 addr, const uint32_t& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;

	memcpy(reinterpret_cast<uint32_t*>(vmv.assumePtr(addr)), &data, sizeof(uint32_t));
	return true;
}

bool vtlb_ramWrite64(u32 addr, const uint64_t& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;

	memcpy(reinterpret_cast<uint64_t*>(vmv.assumePtr(addr)), &data, sizeof(uint64_t));
	return true;
}

bool vtlb_ramWrite128(u32 addr, const mem128_t& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;

	memcpy(reinterpret_cast<mem128_t*>(vmv.assumePtr(addr)), &data, sizeof(mem128_t));
	return true;
}

// --------------------------------------------------------------------------------------
//  TLB Miss / BusError Handlers
// --------------------------------------------------------------------------------------
// These are valid VM memory errors that should typically be handled by the VM itself via
// its own cpu exception system.
//
// [TODO]  Add first-chance debugging hooks to these exceptions!
//
// Important recompiler note: Mid-block Exception handling isn't reliable *yet* because
// memory ops don't flush the PC prior to invoking the indirect handlers.
void GoemonPreloadTlb(void)
{
	// 0x3d5580 is the address of the TLB cache table
	GoemonTlb* tlb = (GoemonTlb*)&eeMem->Main[0x3d5580];

	for (u32 i = 0; i < 150; i++)
	{
		if (tlb[i].valid == 0x1 && tlb[i].low_add != tlb[i].high_add)
		{

			u32 size = tlb[i].high_add - tlb[i].low_add;
			u32 vaddr = tlb[i].low_add;
			u32 paddr = tlb[i].physical_add;

			// TODO: The old code (commented below) seems to check specifically for handler 0.  Is this really correct?
			//if ((uintptr_t)vtlbdata.vmap[vaddr>>VTLB_PAGE_BITS] == POINTER_SIGN_BIT) {
			auto vmv = vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS];
			if (vmv.isHandler(vaddr) && vmv.assumeHandlerGetID() == 0)
			{
				vtlb_VMap(vaddr, paddr, size);
				vtlb_VMap(0x20000000 | vaddr, paddr, size);
			}
		}
	}
}

void GoemonUnloadTlb(u32 key)
{
	// 0x3d5580 is the address of the TLB cache table
	GoemonTlb* tlb = (GoemonTlb*)&eeMem->Main[0x3d5580];
	for (u32 i = 0; i < 150; i++)
	{
		if (tlb[i].key == key)
		{
			if (tlb[i].valid == 0x1)
			{
				u32 size = tlb[i].high_add - tlb[i].low_add;
				u32 vaddr = tlb[i].low_add;

				vtlb_VMapUnmap(vaddr, size);
				vtlb_VMapUnmap(0x20000000 | vaddr, size);

				// Unmap the tlb in game cache table
				// Note: Game copy FEFEFEFE for others data
				tlb[i].valid = 0;
				tlb[i].key = 0xFEFEFEFE;
				tlb[i].low_add = 0xFEFEFEFE;
				tlb[i].high_add = 0xFEFEFEFE;
			}
		}
	}
}

// clang-format off
template <typename OperandType>
static OperandType vtlbUnmappedVReadSm(u32 addr)
{
	if (Cpu == &intCpu)
	{
		cpuTlbMiss(addr, cpuRegs.branch, EXC_CODE_TLBL);
		Cpu->CancelInstruction();
	}
	return 0;
}

static RETURNS_R128 vtlbUnmappedVReadLg(u32 addr)
{
	if (Cpu == &intCpu)
	{
		cpuTlbMiss(addr, cpuRegs.branch, EXC_CODE_TLBL);
		Cpu->CancelInstruction();
	}
	return r128_zero();
}

template <typename OperandType>
static void vtlbUnmappedVWriteSm(u32 addr, OperandType data)
{
	if (Cpu == &intCpu)
	{
		cpuTlbMiss(addr, cpuRegs.branch, EXC_CODE_TLBS);
		Cpu->CancelInstruction();
	}
}

static void TAKES_R128 vtlbUnmappedVWriteLg(u32 addr, r128 data)
{
	if (Cpu == &intCpu)
	{
		cpuTlbMiss(addr, cpuRegs.branch, EXC_CODE_TLBS);
		Cpu->CancelInstruction();
	}
}

template <typename OperandType>
static OperandType vtlbUnmappedPReadSm(u32 addr) { return 0; }
static RETURNS_R128 vtlbUnmappedPReadLg(u32 addr) { return r128_zero(); }

template <typename OperandType>
static void vtlbUnmappedPWriteSm(u32 addr, OperandType data) { }
static void TAKES_R128 vtlbUnmappedPWriteLg(u32 addr, r128 data) { }
// clang-format on

// --------------------------------------------------------------------------------------
//  VTLB mapping errors
// --------------------------------------------------------------------------------------
// These errors are assertion/logic errors that should never occur if PCSX2 has been initialized
// properly.  All addressable physical memory should be configured as TLBMiss or Bus Error.
//

static uint8_t vtlbDefaultPhyRead8(u32 addr) { return 0; }
static uint16_t vtlbDefaultPhyRead16(u32 addr) { return 0; }
static uint32_t vtlbDefaultPhyRead32(u32 addr) { return 0; }
static uint64_t vtlbDefaultPhyRead64(u32 addr) { return 0; }
static RETURNS_R128 vtlbDefaultPhyRead128(u32 addr) { return r128_zero(); }
static void vtlbDefaultPhyWrite8(u32 addr, uint8_t data) { }
static void vtlbDefaultPhyWrite16(u32 addr, uint16_t data) { }
static void vtlbDefaultPhyWrite32(u32 addr, uint32_t data) { }
static void vtlbDefaultPhyWrite64(u32 addr, uint64_t data) { }
static void TAKES_R128 vtlbDefaultPhyWrite128(u32 addr, r128 data) { }

// ===========================================================================================
//  VTLB Public API -- Init/Term/RegisterHandler stuff
// ===========================================================================================
//

// Assigns or re-assigns the callbacks for a VTLB memory handler.  The handler defines specific behavior
// for how memory pages bound to the handler are read from / written to.  If any of the handler pointers
// are NULL, the memory operations will be mapped to the BusError handler (thus generating BusError
// exceptions if the emulated app attempts to access them).
//
// Note: All handlers persist across calls to vtlb_Reset(), but are wiped/invalidated by calls to vtlb_Init()
//
static __ri void vtlb_ReassignHandler(u32 rv,
	vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	vtlbdata.RWFT[0][0][rv] = (void*)((r8 != 0) ? r8 : vtlbDefaultPhyRead8);
	vtlbdata.RWFT[1][0][rv] = (void*)((r16 != 0) ? r16 : vtlbDefaultPhyRead16);
	vtlbdata.RWFT[2][0][rv] = (void*)((r32 != 0) ? r32 : vtlbDefaultPhyRead32);
	vtlbdata.RWFT[3][0][rv] = (void*)((r64 != 0) ? r64 : vtlbDefaultPhyRead64);
	vtlbdata.RWFT[4][0][rv] = (void*)((r128 != 0) ? r128 : vtlbDefaultPhyRead128);

	vtlbdata.RWFT[0][1][rv] = (void*)((w8 != 0) ? w8 : vtlbDefaultPhyWrite8);
	vtlbdata.RWFT[1][1][rv] = (void*)((w16 != 0) ? w16 : vtlbDefaultPhyWrite16);
	vtlbdata.RWFT[2][1][rv] = (void*)((w32 != 0) ? w32 : vtlbDefaultPhyWrite32);
	vtlbdata.RWFT[3][1][rv] = (void*)((w64 != 0) ? w64 : vtlbDefaultPhyWrite64);
	vtlbdata.RWFT[4][1][rv] = (void*)((w128 != 0) ? w128 : vtlbDefaultPhyWrite128);
}

static u32 vtlb_NewHandler(void) { return vtlbHandlerCount++; }

// Registers a handler into the VTLB's internal handler array.  The handler defines specific behavior
// for how memory pages bound to the handler are read from / written to.  If any of the handler pointers
// are NULL, the memory operations will be mapped to the BusError handler (thus generating BusError
// exceptions if the emulated app attempts to access them).
//
// Note: All handlers persist across calls to vtlb_Reset(), but are wiped/invalidated by calls to vtlb_Init()
//
// Returns a handle for the newly created handler  See vtlb_MapHandler for use of the return value.
//
static __ri u32 vtlb_RegisterHandler(vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	u32 rv = vtlb_NewHandler();
	vtlb_ReassignHandler(rv, r8, r16, r32, r64, r128, w8, w16, w32, w64, w128);
	return rv;
}


// Maps the given hander (created with vtlb_RegisterHandler) to the specified memory region.
// New mappings always assume priority over previous mappings, so place "generic" mappings for
// large areas of memory first, and then specialize specific small regions of memory afterward.
// A single handler can be mapped to many different regions by using multiple calls to this
// function.
//
// The memory region start and size parameters must be pagesize aligned.
static void vtlb_MapHandler(u32 handler, u32 start, u32 size)
{
	u32 end = start + (size - VTLB_PAGE_SIZE);

	while (start <= end)
	{
		vtlbdata.pmap[start >> VTLB_PAGE_BITS] = VTLBPhysical::fromHandler(handler);
		start += VTLB_PAGE_SIZE;
	}
}

static void vtlb_MapBlock(void* base, u32 start, u32 size, u32 blocksize)
{
	intptr_t baseint = (intptr_t)base;
	uint32_t end     = start + (size - VTLB_PAGE_SIZE);

	while (start <= end)
	{
		uint32_t loopsz = blocksize;
		intptr_t ptr    = baseint;

		while (loopsz > 0)
		{
			vtlbdata.pmap[start >> VTLB_PAGE_BITS] = VTLBPhysical::fromPointer(ptr);

			start  += VTLB_PAGE_SIZE;
			ptr    += VTLB_PAGE_SIZE;
			loopsz -= VTLB_PAGE_SIZE;
		}
	}
}

__fi void* vtlb_GetPhyPtr(u32 paddr)
{
	if (paddr >= VTLB_PMAP_SZ || vtlbdata.pmap[paddr >> VTLB_PAGE_BITS].isHandler())
		return NULL;
	return reinterpret_cast<void*>(vtlbdata.pmap[paddr >> VTLB_PAGE_BITS].assumePtr() + (paddr & VTLB_PAGE_MASK));
}

__fi u32 vtlb_V2P(u32 vaddr)
{
	u32 paddr = vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS];
	paddr    |= vaddr & VTLB_PAGE_MASK;
	return paddr;
}

static bool vtlb_IsHostAligned(u32 paddr)
{
	if constexpr (__pagesize == VTLB_PAGE_SIZE)
		return true;
	return ((paddr & __pagemask) == 0);
}

static u32 vtlb_HostPage(u32 page)
{
	if constexpr (__pagesize == VTLB_PAGE_SIZE)
		return page;
	return page >> (__pageshift - VTLB_PAGE_BITS);
}

static u32 vtlb_HostAlignOffset(u32 offset)
{
	if constexpr (__pagesize == VTLB_PAGE_SIZE)
		return offset;
	return offset & ~__pagemask;
}

static bool vtlb_IsHostCoalesced(u32 page)
{
	if constexpr (__pagesize != VTLB_PAGE_SIZE)
	{
		static constexpr u32 shift = __pageshift - VTLB_PAGE_BITS;
		static constexpr u32 count = (1u << shift);
		static constexpr u32 mask  = count - 1;

		const u32 base = page & ~mask;
		const u32 base_offset = s_fastmem_virtual_mapping[base];
		if ((base_offset & __pagemask) != 0)
			return false;

		for (u32 i = 0, expected_offset = base_offset; i < count; i++, expected_offset += VTLB_PAGE_SIZE)
		{
			if (s_fastmem_virtual_mapping[base + i] != expected_offset)
				return false;
		}
	}
	return true;
}

static bool vtlb_GetMainMemoryOffsetFromPtr(uintptr_t ptr, u32* mainmem_offset, u32* mainmem_size, PageProtectionMode* prot)
{
	const uintptr_t page_end = ptr + VTLB_PAGE_SIZE;
	SysMainMemory& vmmem = GetVmMemory();

	// EE memory and ROMs.
	if (ptr >= (uintptr_t)eeMem->Main && page_end <= (uintptr_t)eeMem->ZeroRead)
	{
		const u32 eemem_offset = static_cast<u32>(ptr - (uintptr_t)eeMem->Main);
		const bool writeable   = ((eemem_offset < Ps2MemSize::MainRam) ? (mmap_GetRAMPageInfo(eemem_offset) != ProtMode_Write) : true);
		*mainmem_offset        = (eemem_offset + HostMemoryMap::EEmemOffset);
		*mainmem_size          = (offsetof(EEVM_MemoryAllocMess, ZeroRead) - eemem_offset);
		prot->m_read           = true;
		prot->m_write          = writeable;
		prot->m_exec           = false;
		return true;
	}

	// IOP memory.
	if (ptr >= (uintptr_t)iopMem->Main && page_end <= (uintptr_t)iopMem->P)
	{
		const u32 iopmem_offset = static_cast<u32>(ptr - (uintptr_t)iopMem->Main);
		*mainmem_offset = iopmem_offset + HostMemoryMap::IOPmemOffset;
		*mainmem_size = (offsetof(IopVM_MemoryAllocMess, P) - iopmem_offset);
		prot->m_read  = true;
		prot->m_write = true;
		prot->m_exec  = false;
		return true;
	}

	// VU memory - this includes both data and code for VU0/VU1.
	// Practically speaking, this is only data, because the code goes through a handler.
	if (ptr >= (uintptr_t)vmmem.VUMemory().GetPtr() && page_end <= (uintptr_t)vmmem.VUMemory().GetPtrEnd())
	{
		const u32 vumem_offset = static_cast<u32>(ptr - (uintptr_t)vmmem.VUMemory().GetPtr());
		*mainmem_offset        = vumem_offset + HostMemoryMap::VUmemOffset;
		*mainmem_size          = vmmem.VUMemory().GetSize() - vumem_offset;
		prot->m_read  = true;
		prot->m_write = true;
		prot->m_exec  = false;
		return true;
	}

	// We end up with some unknown mappings here; currently the IOP memory, instead of being physically mapped
	// as 2MB, ends up being mapped as 8MB. But this shouldn't be virtual mapped anyway, so fallback to slowmem
	// in such cases.
	return false;
}

static bool vtlb_GetMainMemoryOffset(u32 paddr, u32* mainmem_offset, u32* mainmem_size, PageProtectionMode* prot)
{
	if (paddr >= VTLB_PMAP_SZ)
		return false;

	// Handlers aren't in our shared memory, obviously.
	const VTLBPhysical& vm = vtlbdata.pmap[paddr >> VTLB_PAGE_BITS];
	if (vm.isHandler())
		return false;

	return vtlb_GetMainMemoryOffsetFromPtr(vm.raw(), mainmem_offset, mainmem_size, prot);
}

static void vtlb_CreateFastmemMapping(u32 vaddr, u32 mainmem_offset, const PageProtectionMode mode)
{
	const u32 page = vaddr / VTLB_PAGE_SIZE;

	// current mapping is fine
	if (s_fastmem_virtual_mapping[page] == mainmem_offset)
		return;

	if (s_fastmem_virtual_mapping[page] != NO_FASTMEM_MAPPING)
	{
		// current mapping needs to be removed
		const bool was_coalesced = vtlb_IsHostCoalesced(page);

		s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;
		if (was_coalesced && !s_fastmem_area->Unmap(s_fastmem_area->PagePointer(vtlb_HostPage(page)), __pagesize))
			Console.Error("Failed to unmap vaddr %08X", vaddr);

		// remove reverse mapping
		auto range = s_fastmem_physical_mapping.equal_range(mainmem_offset);
		for (auto it = range.first; it != range.second;)
		{
			auto this_it = it++;
			if (this_it->second == vaddr)
				s_fastmem_physical_mapping.erase(this_it);
		}
	}

	s_fastmem_virtual_mapping[page] = mainmem_offset;
	if (vtlb_IsHostCoalesced(page))
	{
		const u32 host_page = vtlb_HostPage(page);
		const u32 host_offset = vtlb_HostAlignOffset(mainmem_offset);

		if (!s_fastmem_area->Map(GetVmMemory().MainMemory()->GetFileHandle(), host_offset,
				s_fastmem_area->PagePointer(host_page), __pagesize, mode))
		{
			Console.Error("Failed to map vaddr %08X to mainmem offset %08X", vtlb_HostAlignOffset(vaddr), host_offset);
			s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;
			return;
		}
	}

	s_fastmem_physical_mapping.emplace(mainmem_offset, vaddr);
}

static void vtlb_RemoveFastmemMapping(u32 vaddr)
{
	const u32 page = vaddr / VTLB_PAGE_SIZE;
	if (s_fastmem_virtual_mapping[page] == NO_FASTMEM_MAPPING)
		return;

	const u32 mainmem_offset = s_fastmem_virtual_mapping[page];
	const bool was_coalesced = vtlb_IsHostCoalesced(page);
	s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;

	if (was_coalesced && !s_fastmem_area->Unmap(s_fastmem_area->PagePointer(vtlb_HostPage(page)), __pagesize))
		Console.Error("Failed to unmap vaddr %08X", vtlb_HostAlignOffset(vaddr));

	// remove from reverse map
	auto range = s_fastmem_physical_mapping.equal_range(mainmem_offset);
	for (auto it = range.first; it != range.second;)
	{
		auto this_it = it++;
		if (this_it->second == vaddr)
			s_fastmem_physical_mapping.erase(this_it);
	}
}

static void vtlb_RemoveFastmemMappings(u32 vaddr, u32 size)
{
	const u32 num_pages = size / VTLB_PAGE_SIZE;
	for (u32 i = 0; i < num_pages; i++, vaddr += VTLB_PAGE_SIZE)
		vtlb_RemoveFastmemMapping(vaddr);
}

static void vtlb_RemoveFastmemMappings(void)
{
	// not initialized yet
	if (s_fastmem_virtual_mapping.empty())
		return;

	for (u32 page = 0; page < FASTMEM_PAGE_COUNT; page++)
	{
		if (s_fastmem_virtual_mapping[page] == NO_FASTMEM_MAPPING)
			continue;

		if (vtlb_IsHostCoalesced(page))
			s_fastmem_area->Unmap(s_fastmem_area->PagePointer(vtlb_HostPage(page)), __pagesize);

		s_fastmem_virtual_mapping[page] = NO_FASTMEM_MAPPING;
	}

	s_fastmem_physical_mapping.clear();
}

static bool vtlb_GetGuestAddress(uintptr_t host_addr, u32* guest_addr)
{
	uintptr_t fastmem_start = (uintptr_t)vtlbdata.fastmem_base;
	uintptr_t fastmem_end = fastmem_start + 0xFFFFFFFFu;
	if (host_addr < fastmem_start || host_addr > fastmem_end)
		return false;

	*guest_addr = static_cast<u32>(host_addr - fastmem_start);
	return true;
}

static void vtlb_UpdateFastmemProtection(u32 paddr, u32 size, const PageProtectionMode prot)
{
	u32 mainmem_start, mainmem_size;
	PageProtectionMode old_prot;
	if (!vtlb_GetMainMemoryOffset(paddr, &mainmem_start, &mainmem_size, &old_prot))
		return;

	u32 current_mainmem = mainmem_start;
	const u32 num_pages = std::min(size, mainmem_size) / VTLB_PAGE_SIZE;
	for (u32 i = 0; i < num_pages; i++, current_mainmem += VTLB_PAGE_SIZE)
	{
		// update virtual mapping mapping
		auto range = s_fastmem_physical_mapping.equal_range(current_mainmem);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (vtlb_IsHostAligned(it->second))
				HostSys::MemProtect(s_fastmem_area->OffsetPointer(it->second), __pagesize, prot);
		}
	}
}

void vtlb_ClearLoadStoreInfo(void)
{
	s_fastmem_backpatch_info.clear();
	s_fastmem_faulting_pcs.clear();
}

void vtlb_AddLoadStoreInfo(uintptr_t code_address, u32 code_size, u32 guest_pc, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr)
{
	auto iter = s_fastmem_backpatch_info.find(code_address);
	if (iter != s_fastmem_backpatch_info.end())
		s_fastmem_backpatch_info.erase(iter);

	LoadstoreBackpatchInfo info{guest_pc, gpr_bitmask, fpr_bitmask, static_cast<u8>(code_size), address_register, data_register, size_in_bits, is_signed, is_load, is_fpr};
	s_fastmem_backpatch_info.emplace(code_address, info);
}

static bool vtlb_BackpatchLoadStore(uintptr_t code_address, uintptr_t fault_address)
{
	uintptr_t fastmem_start = (uintptr_t)vtlbdata.fastmem_base;
	uintptr_t fastmem_end = fastmem_start + 0xFFFFFFFFu;
	if (fault_address < fastmem_start || fault_address > fastmem_end)
		return false;

	auto iter = s_fastmem_backpatch_info.find(code_address);
	if (iter == s_fastmem_backpatch_info.end())
		return false;

	const LoadstoreBackpatchInfo& info = iter->second;
	const uint32_t guest_addr = static_cast<uint32_t>(fault_address - fastmem_start);
	vtlb_DynBackpatchLoadStore(code_address, info.code_size, info.guest_pc, guest_addr,
		info.gpr_bitmask, info.fpr_bitmask, info.address_register, info.data_register,
		info.size_in_bits, info.is_signed, info.is_load, info.is_fpr);

	// queue block for recompilation later
	Cpu->Clear(info.guest_pc, 1);

	// and store the pc in the faulting list, so that we don't emit another fastmem loadstore
	s_fastmem_faulting_pcs.insert(info.guest_pc);
	s_fastmem_backpatch_info.erase(iter);
	return true;
}

bool vtlb_IsFaultingPC(u32 guest_pc)
{
	return (s_fastmem_faulting_pcs.find(guest_pc) != s_fastmem_faulting_pcs.end());
}

//virtual mappings
//TODO: Add invalid paddr checks
void vtlb_VMap(u32 vaddr, u32 paddr, u32 size)
{
	if (CHECK_FASTMEM)
	{
		const u32 num_pages = size / VTLB_PAGE_SIZE;
		u32 current_vaddr = vaddr;
		u32 current_paddr = paddr;

		for (u32 i = 0; i < num_pages; i++, current_vaddr += VTLB_PAGE_SIZE, current_paddr += VTLB_PAGE_SIZE)
		{
			u32 hoffset, hsize;
			PageProtectionMode mode;
			if (vtlb_GetMainMemoryOffset(current_paddr, &hoffset, &hsize, &mode))
				vtlb_CreateFastmemMapping(current_vaddr, hoffset, mode);
			else
				vtlb_RemoveFastmemMapping(current_vaddr);
		}
	}

	while (size > 0)
	{
		VTLBVirtual vmv;
		if (paddr >= VTLB_PMAP_SZ)
			vmv = VTLBVirtual(VTLBPhysical::fromHandler(UnmappedPhyHandler), paddr, vaddr);
		else
			vmv = VTLBVirtual(vtlbdata.pmap[paddr >> VTLB_PAGE_BITS], paddr, vaddr);

		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = vmv;
		if (vtlbdata.ppmap)
		{
			if (!(vaddr & 0x80000000)) // those address are already physical don't change them
				vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS] = paddr & ~VTLB_PAGE_MASK;
		}

		vaddr += VTLB_PAGE_SIZE;
		paddr += VTLB_PAGE_SIZE;
		size -= VTLB_PAGE_SIZE;
	}
}

void vtlb_VMapBuffer(u32 vaddr, void* buffer, u32 size)
{
	if (CHECK_FASTMEM)
	{
		if (buffer == eeMem->Scratch && size == Ps2MemSize::Scratch)
		{
			PageProtectionMode mode;
			u32 fm_vaddr      = vaddr;
			u32 fm_hostoffset = HostMemoryMap::EEmemOffset + offsetof(EEVM_MemoryAllocMess, Scratch);
			mode.m_read       = true;
			mode.m_write      = true;
			mode.m_exec       = false;
			for (u32 i = 0; i < (Ps2MemSize::Scratch / VTLB_PAGE_SIZE); i++, fm_vaddr += VTLB_PAGE_SIZE, fm_hostoffset += VTLB_PAGE_SIZE)
				vtlb_CreateFastmemMapping(fm_vaddr, fm_hostoffset, mode);
		}
		else
		{
			vtlb_RemoveFastmemMappings(vaddr, size);
		}
	}

	uintptr_t bu8 = (uintptr_t)buffer;
	while (size > 0)
	{
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = VTLBVirtual(VTLBPhysical::fromPointer(bu8), 0, vaddr);
		vaddr += VTLB_PAGE_SIZE;
		bu8 += VTLB_PAGE_SIZE;
		size -= VTLB_PAGE_SIZE;
	}
}

void vtlb_VMapUnmap(u32 vaddr, u32 size)
{
	vtlb_RemoveFastmemMappings(vaddr, size);

	while (size > 0)
	{
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = VTLBVirtual(VTLBPhysical::fromHandler(UnmappedVirtHandler), vaddr, vaddr);
		vaddr += VTLB_PAGE_SIZE;
		size -= VTLB_PAGE_SIZE;
	}
}

static constexpr size_t PPMAP_SIZE = sizeof(*vtlbdata.ppmap) * VTLB_VMAP_ITEMS;

/* The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
 * However automatic gamefix is done after the standard init so a new init function was done. */
void vtlb_Alloc_Ppmap(void)
{
	PageProtectionMode mode;
	if (vtlbdata.ppmap)
		return;

	static u32* ppmap = nullptr;

	if (!ppmap)
		ppmap  = (u32*)GetVmMemory().BumpAllocator().Alloc(PPMAP_SIZE);

	mode.m_read    = true;
	mode.m_write   = true;
	mode.m_exec    = false;
	HostSys::MemProtect(ppmap, PPMAP_SIZE, mode);
	vtlbdata.ppmap = ppmap;

	// By default a 1:1 virtual to physical mapping
	for (u32 i = 0; i < VTLB_VMAP_ITEMS; i++)
		vtlbdata.ppmap[i] = i << VTLB_PAGE_BITS;
}

/* Clears vtlb handlers and memory mappings. */
void vtlb_Init(void)
{
	vtlbHandlerCount = 0;
	memset(vtlbdata.RWFT, 0, sizeof(vtlbdata.RWFT));

#define VTLB_BuildUnmappedHandler(baseName) \
	baseName##ReadSm<uint8_t>, baseName##ReadSm<uint16_t>, baseName##ReadSm<uint32_t>, \
		baseName##ReadSm<uint64_t>, baseName##ReadLg, \
		baseName##WriteSm<uint8_t>, baseName##WriteSm<uint16_t>, baseName##WriteSm<uint32_t>, \
		baseName##WriteSm<uint64_t>, baseName##WriteLg

	//Register default handlers
	//Unmapped Virt handlers _MUST_ be registered first.
	//On address translation the top bit cannot be preserved.This is not normaly a problem since
	//the physical address space can be 'compressed' to just 29 bits.However, to properly handle exceptions
	//there must be a way to get the full address back.Thats why i use these 2 functions and encode the hi bit directly into em :)

	UnmappedVirtHandler = vtlb_RegisterHandler(VTLB_BuildUnmappedHandler(vtlbUnmappedV));
	UnmappedPhyHandler = vtlb_RegisterHandler(VTLB_BuildUnmappedHandler(vtlbUnmappedP));
	DefaultPhyHandler = vtlb_RegisterHandler(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

	//done !

	//Setup the initial mappings
	vtlb_MapHandler(DefaultPhyHandler, 0, VTLB_PMAP_SZ);

	//Set the V space as unmapped
	vtlb_VMapUnmap(0, (VTLB_VMAP_ITEMS - 1) * VTLB_PAGE_SIZE);
	//yeah i know, its stupid .. but this code has to be here for now ;p
	vtlb_VMapUnmap((VTLB_VMAP_ITEMS - 1) * VTLB_PAGE_SIZE, VTLB_PAGE_SIZE);

	// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		vtlb_Alloc_Ppmap();
}

void vtlb_Shutdown(void)
{
	vtlb_RemoveFastmemMappings();
	s_fastmem_backpatch_info.clear();
	s_fastmem_faulting_pcs.clear();
}

void vtlb_ResetFastmem(void)
{
	vtlb_RemoveFastmemMappings();
	s_fastmem_backpatch_info.clear();
	s_fastmem_faulting_pcs.clear();

	if (!CHECK_FASTMEM || !CHECK_EEREC || !vtlbdata.vmap)
		return;

	// we need to go through and look at the vtlb pointers, to remap the host area
	for (size_t i = 0; i < VTLB_VMAP_ITEMS; i++)
	{
		const VTLBVirtual& vm = vtlbdata.vmap[i];
		const u32 vaddr = static_cast<u32>(i) << VTLB_PAGE_BITS;
		// Handlers should be unmapped.
		if (vm.isHandler(vaddr))
			continue;

		// Check if it's a physical mapping to our main memory area.
		u32 mainmem_offset, mainmem_size;
		PageProtectionMode prot;
		if (vtlb_GetMainMemoryOffsetFromPtr(vm.assumePtr(vaddr), &mainmem_offset, &mainmem_size, &prot))
			vtlb_CreateFastmemMapping(vaddr, mainmem_offset, prot);
	}
}

static constexpr size_t VMAP_SIZE = sizeof(VTLBVirtual) * VTLB_VMAP_ITEMS;

// Reserves the vtlb core allocation used by various emulation components!
// [TODO] basemem - request allocating memory at the specified virtual location, which can allow
//    for easier debugging and/or 3rd party cheat programs.  If 0, the operating system
//    default is used.
static bool vtlb_Core_Alloc(void)
{
	// Can't return regions to the bump allocator
	static VTLBVirtual* vmap = nullptr;
	if (!vmap)
	{
		vmap = (VTLBVirtual*)GetVmMemory().BumpAllocator().Alloc(VMAP_SIZE);
		if (!vmap)
		{
			Console.Error("Failed to allocate vtlb vmap");
			return false;
		}
	}

	if (!vtlbdata.vmap)
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = false;
		HostSys::MemProtect(vmap, VMAP_SIZE, mode);
		vtlbdata.vmap = vmap;
	}

	if (!vtlbdata.fastmem_base)
	{
		s_fastmem_area = SharedMemoryMappingArea::Create(FASTMEM_AREA_SIZE);
		if (!s_fastmem_area)
		{
			Console.Error("Failed to allocate fastmem area");
			return false;
		}

		s_fastmem_virtual_mapping.resize(FASTMEM_PAGE_COUNT, NO_FASTMEM_MAPPING);
		vtlbdata.fastmem_base = (uintptr_t)s_fastmem_area->BasePointer();
		Console.WriteLn(Color_StrongGreen, "Fastmem area: %p - %p",
			vtlbdata.fastmem_base, vtlbdata.fastmem_base + (FASTMEM_AREA_SIZE - 1));
	}

	if (!HostSys::InstallPageFaultHandler(&vtlb_private::PageFaultHandler))
	{
		Console.Error("Failed to install page fault handler.");
		return false;
	}

	return true;
}

static void vtlb_Core_Free(void)
{
	PageProtectionMode mode;
	HostSys::RemovePageFaultHandler(&vtlb_private::PageFaultHandler);

	mode.m_read  = false;
	mode.m_write = false;
	mode.m_exec  = false;

	if (vtlbdata.vmap)
	{
		HostSys::MemProtect(vtlbdata.vmap, VMAP_SIZE, mode);
		vtlbdata.vmap = nullptr;
	}
	if (vtlbdata.ppmap)
	{
		HostSys::MemProtect(vtlbdata.ppmap, PPMAP_SIZE, mode);
		vtlbdata.ppmap = nullptr;
	}

	vtlb_RemoveFastmemMappings();
	vtlb_ClearLoadStoreInfo();

	vtlbdata.fastmem_base = 0;
	decltype(s_fastmem_physical_mapping)().swap(s_fastmem_physical_mapping);
	decltype(s_fastmem_virtual_mapping)().swap(s_fastmem_virtual_mapping);
	s_fastmem_area.reset();
}

// --------------------------------------------------------------------------------------
//  VtlbMemoryReserve  (implementations)
// --------------------------------------------------------------------------------------
VtlbMemoryReserve::VtlbMemoryReserve() : VirtualMemoryReserve() { }

void VtlbMemoryReserve::Assign(VirtualMemoryManagerPtr allocator, size_t offset, size_t size)
{
	// Anything passed to the memory allocator must be page aligned.
	size     = Common::PageAlign(size);
	// Since the memory has already been allocated as part of the main memory map, this should never fail.
	u8* base = allocator->Alloc(offset, size);
	VirtualMemoryReserve::Assign(std::move(allocator), base, size);
}

void VtlbMemoryReserve::Reset()
{
	memset(GetPtr(), 0, GetSize());
}


// ===========================================================================================
//  Memory Protection and Block Checking, vtlb Style!
// ===========================================================================================
// For the first time code is recompiled (executed), the PS2 ram page for that code is
// protected using Virtual Memory (mprotect).  If the game modifies its own code then this
// protection causes an *exception* to be raised (signal in Linux), which is handled by
// unprotecting the page and switching the recompiled block to "manual" protection.
//
// Manual protection uses a simple brute-force memcmp of the recompiled code to the code
// currently in RAM for *each time* the block is executed.  Fool-proof, but slow, which
// is why we default to using the exception-based protection scheme described above.
//
// Why manual blocks?  Because many games contain code and data in the same 4k page, so
// we *cannot* automatically recompile and reprotect pages, lest we end up recompiling and
// reprotecting them constantly (Which would be very slow).  As a counter, the R5900 side
// of the block checking code does try to periodically re-protect blocks [going from manual
// back to protected], so that blocks which underwent a single invalidation don't need to
// incur a permanent performance penalty.
//
// Page Granularity:
// Fortunately for us MIPS and x86 use the same page granularity for TLB and memory
// protection, so we can use a 1:1 correspondence when protecting pages.  Page granularity
// is 4096 (4k), which is why you'll see a lot of 0xfff's, >><< 12's, and 0x1000's in the
// code below.
//

struct vtlb_PageProtectionInfo
{
	// Ram De-mapping -- used to convert fully translated/mapped offsets (which reside with
	// in the eeMem->Main block) back into their originating ps2 physical ram address.
	// Values are assigned when pages are marked for protection.  since pages are automatically
	// cleared and reset when TLB-remapped, stale values in this table (due to on-the-fly TLB
	// changes) will be re-assigned the next time the page is accessed.
	u32 ReverseRamMap;

	vtlb_ProtectionMode Mode;
};

alignas(16) static vtlb_PageProtectionInfo m_PageProtectInfo[Ps2MemSize::MainRam >> __pageshift];


// returns:
//  ProtMode_NotRequired - unchecked block (resides in ROM, thus is integrity is constant)
//  Or the current mode
//
vtlb_ProtectionMode mmap_GetRAMPageInfo(u32 paddr)
{
	paddr &= ~0xfff;

	uintptr_t ptr = (uintptr_t)PSM(paddr);
	uintptr_t rampage = ptr - (uintptr_t)eeMem->Main;

	if (!ptr || rampage >= Ps2MemSize::MainRam)
		return ProtMode_NotRequired; //not in ram, no tracking done ...

	rampage >>= __pageshift;

	return m_PageProtectInfo[rampage].Mode;
}

// paddr - physically mapped PS2 address
void mmap_MarkCountedRAMPage(u32 paddr)
{
	PageProtectionMode mode;
	paddr &= ~__pagemask;

	uintptr_t ptr = (uintptr_t)PSM(paddr);
	int rampage = (ptr - (uintptr_t)eeMem->Main) >> __pageshift;

	// Important: Update the ReverseRamMap here because TLB changes could alter the paddr
	// mapping into eeMem->Main.

	m_PageProtectInfo[rampage].ReverseRamMap = paddr;

	if (m_PageProtectInfo[rampage].Mode == ProtMode_Write)
		return; // skip town if we're already protected.

	m_PageProtectInfo[rampage].Mode = ProtMode_Write;
	mode.m_read  = true;
	mode.m_write = false;
	mode.m_exec  = false;
	HostSys::MemProtect(&eeMem->Main[rampage << __pageshift], __pagesize, mode);
	if (CHECK_FASTMEM)
		vtlb_UpdateFastmemProtection(rampage << __pageshift, __pagesize, mode);
}

bool vtlb_private::PageFaultHandler(const PageFaultInfo& info)
{
	u32 vaddr;
	int ram_page;
	uintptr_t offset;
	PageProtectionMode mode;
	if (CHECK_FASTMEM && vtlb_GetGuestAddress(info.addr, &vaddr))
	{
		uintptr_t ptr = (uintptr_t)PSM(vaddr);
		offset        = (ptr - (uintptr_t)eeMem->Main);
		if (!(ptr && m_PageProtectInfo[offset >> __pageshift].Mode == ProtMode_Write))
			return vtlb_BackpatchLoadStore(info.pc, info.addr);
	}
	else
	{
		/* get bad virtual address */
		offset = info.addr - (uintptr_t)eeMem->Main;
		if (offset >= Ps2MemSize::MainRam)
			return false;
	}
	/* All recompiled blocks belonging to the page are cleared, 
	 * and any new blocks recompiled from code residing in this 
	 * page will use manual protection. */
	ram_page     = offset >> __pageshift;
	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	HostSys::MemProtect(&eeMem->Main[ram_page << __pageshift], __pagesize, mode);
	if (CHECK_FASTMEM)
		vtlb_UpdateFastmemProtection(ram_page << __pageshift, __pagesize, mode);
	m_PageProtectInfo[ram_page].Mode = ProtMode_Manual;
	Cpu->Clear(m_PageProtectInfo[ram_page].ReverseRamMap, __pagesize);
	return true;
}

// Clears all block tracking statuses, manual protection flags, and write protection.
// This does not clear any recompiler blocks.  It is assumed (and necessary) for the caller
// to ensure the EErec is also reset in conjunction with calling this function.
//  (this function is called by default from the eerecReset).
void mmap_ResetBlockTracking(void)
{
	PageProtectionMode mode;
	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	memset(m_PageProtectInfo, 0, sizeof(m_PageProtectInfo));
	if (eeMem)
		HostSys::MemProtect(eeMem->Main, Ps2MemSize::MainRam, mode);
	if (CHECK_FASTMEM)
		vtlb_UpdateFastmemProtection(0, Ps2MemSize::MainRam, mode);
}

namespace R5900
{
	namespace Interpreter
	{
		namespace OpcodeImpl
		{

			void CACHE(void)
			{
				u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

				switch (_Rt_)
				{
					case 0x1a: /* DHIN (Data Cache Hit Invalidate) */
						doCacheHitOp(addr, [](CacheLine line)
								{
								line.tag &= LRF_FLAG;
								memset(&line.data, 0, sizeof(line.data));
								});
						break;

					case 0x18: /* DHWBIN (Data Cache Hit WriteBack with Invalidate) */
						doCacheHitOp(addr, [](CacheLine line)
								{
								if (((line.tag & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
								{
									uintptr_t target = (line.tag & ~ALL_FLAGS) | (line.set << 6);
									*reinterpret_cast<CacheData*>(target) = line.data;
									line.tag &= ~DIRTY_FLAG;
								}
								line.tag &= LRF_FLAG;
								memset(&line.data, 0, sizeof(line.data));
								});
						break;

					case 0x1c: /* DHWOIN (Data Cache Hit WriteBack Without Invalidate) */
						doCacheHitOp(addr, [](CacheLine line)
								{
								if (((line.tag & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
								{
									uintptr_t target = (line.tag & ~ALL_FLAGS) | (line.set << 6);
									*reinterpret_cast<CacheData*>(target) = line.data;
									line.tag &= ~DIRTY_FLAG;
								}
								});
						break;

					case 0x16: /* DXIN (Data Cache Index Invalidate) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							line.tag &= LRF_FLAG;
							memset(&line.data, 0, sizeof(line.data));
							break;
						}

					case 0x11: /* DXLDT (Data Cache Load Data into TagLo) */
						{
							const int index     = (addr >> 6) & 0x3F;
							const int way       = addr & 0x1;
							CacheLine line      = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							cpuRegs.CP0.n.TagLo = *reinterpret_cast<u32*>(&line.data.bytes[addr & 0x3C]);

							break;
						}

					case 0x10: /* DXLTG (Data Cache Load Tag into TagLo) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							/* DXLTG demands that SYNC.L is called before this command, which forces the cache to write back, 
							 * so presumably games are checking the cache has updated the memory for speed, we will do it here. */
							if (((line.tag & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
							{
								uintptr_t target = (line.tag & ~ALL_FLAGS) | (line.set << 6);
								*reinterpret_cast<CacheData*>(target) = line.data;
								line.tag &= ~DIRTY_FLAG;
							}

							/* Our tags don't contain PS2 paddrs (instead they contain x86 addrs) */
							cpuRegs.CP0.n.TagLo = line.tag & ALL_FLAGS;
							break;
						}

					case 0x13: /* DXSDT (Data Cache Store 32bits from TagLo) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							*reinterpret_cast<u32*>(&line.data.bytes[addr & 0x3C]) = cpuRegs.CP0.n.TagLo;
							break;
						}

					case 0x12: /* DXSTG (Data Cache Store Tag from TagLo) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							line.tag &= ~ALL_FLAGS;
							line.tag |= (cpuRegs.CP0.n.TagLo & ALL_FLAGS);
							break;
						}

					case 0x14: /* DXWBIN (Data Cache Index WriteBack Invalidate) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };
							if (((line.tag & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
							{
								uintptr_t target = (line.tag & ~ALL_FLAGS) | (line.set << 6);
								*reinterpret_cast<CacheData*>(target) = line.data;
								line.tag &= ~DIRTY_FLAG;
							}
							line.tag &= LRF_FLAG;
							memset(&line.data, 0, sizeof(line.data));
							break;
						}

					case 0x7: /* IXIN (Instruction Cache Index Invalidate) Not Implemented as we do not have instruction cache */
					case 0xC: /* BFH (BTAC Flush) Not Implemented as we do not cache Branch Target Addresses. */
					default:
						break;
				}
			}
		}
	}
}

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
static u32
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
	else               vtlb_MapBlock  (vuRegs[1].Mem,     0x1100c000,0x00004000, 0x00004000);
}

static void memMapPhy(void)
{
	/* Main memory */
	vtlb_MapBlock(eeMem->Main,	0x00000000,Ps2MemSize::MainRam, Ps2MemSize::MainRam); /* mirrored on first 256 MB ? */
	// High memory, uninstalled on the configuration we emulate
	vtlb_MapHandler(null_handler, Ps2MemSize::MainRam, 0x10000000 - Ps2MemSize::MainRam);

	// Various ROMs (all read-only)
	vtlb_MapBlock(eeMem->ROM,	0x1fc00000, Ps2MemSize::Rom, Ps2MemSize::Rom);
	vtlb_MapBlock(eeMem->ROM1,	0x1e000000, Ps2MemSize::Rom1, Ps2MemSize::Rom1);
	vtlb_MapBlock(eeMem->ROM2,	0x1e400000, Ps2MemSize::Rom2, Ps2MemSize::Rom2);

	// IOP memory
	// (used by the EE Bios Kernel during initial hardware initialization, Apps/Games
	//  are "supposed" to use the thread-safe SIF instead.)
	vtlb_MapBlock(iopMem->Main,0x1c000000,0x00800000, 0x00800000);

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
		gifUnit.Execute<false>(); // Resume paused transfers
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

/* --------------------------------------------------------------------------------------
 *  VirtualMemoryManager  (implementations)
 * --------------------------------------------------------------------------------------
 */

VirtualMemoryManager::VirtualMemoryManager(const char* file_mapping_name, uintptr_t base, size_t size, uintptr_t upper_bounds, bool strict)
	: m_file_handle(nullptr)
	, m_baseptr(0)
	, m_pageuse(nullptr)
	, m_pages_reserved(0)
{
	if (!size)
		return;

	size_t reserved_bytes = Common::PageAlign(size);
	m_pages_reserved = reserved_bytes / __pagesize;

	if (file_mapping_name && file_mapping_name[0])
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = false;
		std::string real_file_mapping_name(HostSys::GetFileMappingName(file_mapping_name));
		m_file_handle = HostSys::CreateSharedMemory(real_file_mapping_name.c_str(), reserved_bytes);
		if (!m_file_handle)
			return;

		m_baseptr = static_cast<u8*>(HostSys::MapSharedMemory(m_file_handle, 0, (void*)base, reserved_bytes, mode));
		if (!m_baseptr || (upper_bounds != 0 && (((uintptr_t)m_baseptr + reserved_bytes) > upper_bounds)))
		{
			HostSys::Munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			/* Let's try again at an OS-picked memory area, and then hope it meets needed
			 * boundschecking criteria below. */
			if (base)
				m_baseptr = static_cast<u8*>(HostSys::MapSharedMemory(m_file_handle, 0, nullptr, reserved_bytes, mode));
		}
	}
	else
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = true;
		m_baseptr    = static_cast<u8*>(HostSys::Mmap((void*)base, reserved_bytes, mode));

		if (!m_baseptr || (upper_bounds != 0 && (((uintptr_t)m_baseptr + reserved_bytes) > upper_bounds)))
		{
			HostSys::Munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			/* Let's try again at an OS-picked memory area, and then hope it meets needed
			 * boundschecking criteria below. */
			if (base)
				m_baseptr = static_cast<u8*>(HostSys::Mmap(0, reserved_bytes, mode));
		}
	}

	bool fulfillsRequirements = true;
	if (strict && (uintptr_t)m_baseptr != base)
		fulfillsRequirements = false;
	if ((upper_bounds != 0) && ((uintptr_t)(m_baseptr + reserved_bytes) > upper_bounds))
		fulfillsRequirements = false;
	if (!fulfillsRequirements)
	{
		if (m_file_handle)
		{
			if (m_baseptr)
				HostSys::UnmapSharedMemory(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			HostSys::DestroySharedMemory(m_file_handle);
			m_file_handle = nullptr;
		}
		else
		{
			HostSys::Munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;
		}
	}

	if (!m_baseptr)
		return;

	m_pageuse = new std::atomic<bool>[m_pages_reserved]();
}

VirtualMemoryManager::~VirtualMemoryManager()
{
	if (m_pageuse)
		delete[] m_pageuse;
	if (m_baseptr)
	{
		if (m_file_handle)
			HostSys::UnmapSharedMemory((void*)m_baseptr, m_pages_reserved * __pagesize);
		else
			HostSys::Munmap(m_baseptr, m_pages_reserved * __pagesize);
	}
	if (m_file_handle)
		HostSys::DestroySharedMemory(m_file_handle);
}

static bool VMMMarkPagesAsInUse(std::atomic<bool>* begin, std::atomic<bool>* end)
{
	for (auto current = begin; current < end; current++)
	{
		bool expected = false;
		if (!current->compare_exchange_strong(expected, true, std::memory_order_relaxed))
		{
			/* This was already allocated!  Undo the things we've set until this point */
			while (--current >= begin)
			{
				/* In the time we were doing this, someone set one of the things we just set to true back to false
				 * This should never happen, but if it does we'll just stop and hope nothing bad happens */
				if (!current->compare_exchange_strong(expected, false, std::memory_order_relaxed))
					return false;
			}
			return false;
		}
	}
	return true;
}

u8* VirtualMemoryManager::Alloc(uintptr_t offsetLocation, size_t size) const
{
	size = Common::PageAlign(size);
	if (!(offsetLocation % __pagesize == 0))
		return nullptr;
	if (!(size + offsetLocation <= m_pages_reserved * __pagesize))
		return nullptr;
	if (m_baseptr == 0)
		return nullptr;
	auto puStart = &m_pageuse[offsetLocation / __pagesize];
	auto puEnd = &m_pageuse[(offsetLocation + size) / __pagesize];
	if (!(VMMMarkPagesAsInUse(puStart, puEnd)))
		return nullptr;
	return m_baseptr + offsetLocation;
}

void VirtualMemoryManager::Free(void* address, size_t size) const
{
	uintptr_t offsetLocation = (uintptr_t)address - (uintptr_t)m_baseptr;
	if (!(offsetLocation % __pagesize == 0))
	{
		uintptr_t newLoc = Common::PageAlign(offsetLocation);
		size -= (offsetLocation - newLoc);
		offsetLocation = newLoc;
	}
	if (!(size % __pagesize == 0))
		size -= size % __pagesize;
	if (!(size + offsetLocation <= m_pages_reserved * __pagesize))
		return;
	auto puStart = &m_pageuse[offsetLocation / __pagesize];
	auto puEnd = &m_pageuse[(offsetLocation + size) / __pagesize];
	for (; puStart < puEnd; puStart++)
	{
		bool expected = true;
		if (!puStart->compare_exchange_strong(expected, false, std::memory_order_relaxed)) { }
	}
}

/* --------------------------------------------------------------------------------------
 *  VirtualMemoryBumpAllocator  (implementations)
 * --------------------------------------------------------------------------------------
 */
VirtualMemoryBumpAllocator::VirtualMemoryBumpAllocator(VirtualMemoryManagerPtr allocator, uintptr_t offsetLocation, size_t size)
	: m_allocator(std::move(allocator))
	, m_baseptr(m_allocator->Alloc(offsetLocation, size))
	, m_endptr(m_baseptr + size)
{
}

u8* VirtualMemoryBumpAllocator::Alloc(size_t size)
{
	if (m_baseptr.load() == 0) /* True if constructed from bad VirtualMemoryManager (assertion was on initialization) */
		return nullptr;
	size_t reservedSize = Common::PageAlign(size);
	return m_baseptr.fetch_add(reservedSize, std::memory_order_relaxed);
}

/* --------------------------------------------------------------------------------------
 *  VirtualMemoryReserve  (implementations)
 * --------------------------------------------------------------------------------------
 */
VirtualMemoryReserve::VirtualMemoryReserve() { }
VirtualMemoryReserve::~VirtualMemoryReserve() { }

/* Notes:
 *  * This method should be called if the object is already in an released (unreserved) state.
 *    Subsequent calls will be ignored, and the existing reserve will be returned.
 *
 * Parameters:
 *   baseptr - the new base pointer that's about to be assigned
 *   size - size of the region pointed to by baseptr
 */
void VirtualMemoryReserve::Assign(VirtualMemoryManagerPtr allocator, u8* baseptr, size_t size)
{
	m_allocator = std::move(allocator);
	m_baseptr = baseptr;
	m_size = size;
}

u8* VirtualMemoryReserve::BumpAllocate(VirtualMemoryBumpAllocator& allocator, size_t size)
{
	u8* base = allocator.Alloc(size);
	if (base)
		Assign(allocator.GetAllocator(), base, size);

	return base;
}

void VirtualMemoryReserve::Release()
{
	if (!m_baseptr)
		return;

	m_allocator->Free(m_baseptr, m_size);
	m_allocator.reset();
	m_baseptr = nullptr;
	m_size = 0;
}

/* --------------------------------------------------------------------------------------
 *  RecompiledCodeReserve  (implementations)
 * --------------------------------------------------------------------------------------
 */

/* Constructor!
 * Parameters:
 *   name - a nice long name that accurately describes the contents of this reserve.
 */
RecompiledCodeReserve::RecompiledCodeReserve() : VirtualMemoryReserve() { }
RecompiledCodeReserve::~RecompiledCodeReserve() { Release(); }

void RecompiledCodeReserve::Assign(VirtualMemoryManagerPtr allocator, size_t offset, size_t size)
{
	/* Anything passed to the memory allocator must be page aligned. */
	size = Common::PageAlign(size);

	/* Since the memory has already been allocated as part of the main memory map, this should never fail. */
	u8* base = allocator->Alloc(offset, size);
	VirtualMemoryReserve::Assign(std::move(allocator), base, size);
}

void RecompiledCodeReserve::Reset() { }

void RecompiledCodeReserve::AllowModification()
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = true;
	HostSys::MemProtect(m_baseptr, m_size, pg);
}

void RecompiledCodeReserve::ForbidModification()
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = false;
	HostSys::MemProtect(m_baseptr, m_size, pg);
}
