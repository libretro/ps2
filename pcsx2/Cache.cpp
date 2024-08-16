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

union alignas(64) CacheData
{
	uint8_t bytes[64];
};

struct CacheTag
{
	uintptr_t rawValue;
};

struct CacheSet
{
	CacheTag tags[2];
	CacheData data[2];
};

struct Cache
{
	CacheSet sets[64];
};

struct CacheLine
{
	CacheTag& tag;
	CacheData& data;
	int set;

	void writeBackIfNeeded()
	{
		if (!((tag.rawValue & (DIRTY_FLAG | VALID_FLAG)) == (DIRTY_FLAG | VALID_FLAG)))
			return;

		uintptr_t target = (tag.rawValue & ~ALL_FLAGS) | (set << 6);

		*reinterpret_cast<CacheData*>(target) = data;
		tag.rawValue &= ~DIRTY_FLAG;
	}

	void load(uintptr_t ppf)
	{
		tag.rawValue &= ALL_FLAGS;
		tag.rawValue |= (ppf & ~ALL_FLAGS);
		memcpy(&data, reinterpret_cast<void*>(ppf & ~0x3FULL), sizeof(data));
		tag.rawValue |=  VALID_FLAG;
		tag.rawValue &= ~DIRTY_FLAG;
	}
};

static Cache cache = {};

static bool findInCache(const CacheSet& set, uintptr_t ppf, int* way)
{
	auto check = [&](int checkWay) -> bool
	{
		if (!(set.tags[checkWay].rawValue & VALID_FLAG) && (set.tags[checkWay].rawValue & ~ALL_FLAGS) == (ppf & ~ALL_FLAGS))
			return false;

		*way = checkWay;
		return true;
	};

	return check(0) || check(1);
}

template <typename Op>
static void doCacheHitOp(u32 addr, Op op)
{
	int way;
	const int index = (addr >> 6) & 0x3F;
	CacheSet& set   = cache.sets[index];
	vtlb_private::VTLBVirtual vmv = vtlb_private::vtlbdata.vmap[addr >> vtlb_private::VTLB_PAGE_BITS];
	uintptr_t ppf = vmv.assumePtr(addr);
	if (findInCache(set, ppf, &way))
		op({ cache.sets[index].tags[way], cache.sets[index].data[way], index });
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

static vtlbHandler vtlbHandlerCount = 0;

static vtlbHandler DefaultPhyHandler;
static vtlbHandler UnmappedVirtHandler;
static vtlbHandler UnmappedPhyHandler;

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

vtlb_private::VTLBPhysical vtlb_private::VTLBPhysical::fromHandler(vtlbHandler handler)
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
	const int setIdx = (mem >> 6) & 0x3F;
	CacheSet& set    = cache.sets[setIdx];
	vtlb_private::VTLBVirtual vmv  = vtlb_private::vtlbdata.vmap[mem >> vtlb_private::VTLB_PAGE_BITS];
	uintptr_t ppf         = vmv.assumePtr(mem);

	if (!findInCache(set, ppf, way))
	{
		int newWay     = (set.tags[0].rawValue & LRF_FLAG) ^ (set.tags[1].rawValue & LRF_FLAG);
		*way           = newWay;
		CacheLine line = { cache.sets[setIdx].tags[newWay], cache.sets[setIdx].data[newWay], setIdx };

		line.writeBackIfNeeded();
		line.load(ppf);
		line.tag.rawValue ^= LRF_FLAG;
	}

	return setIdx;
}


template <bool Write, int Bytes>
static void* prepareCacheAccess(u32 mem, int* way, int* idx)
{
	*way = 0;
	*idx = getFreeCache(mem, way);
	CacheLine line = { cache.sets[*idx].tags[*way], cache.sets[*idx].data[*way], *idx };
	if (Write)
		line.tag.rawValue |= DIRTY_FLAG;
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
			int way, idx;
			void *_addr = prepareCacheAccess<false, sizeof(uint8_t)>(addr, &way, &idx);
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
			int way, idx;
			void *_addr = prepareCacheAccess<false, sizeof(uint16_t)>(addr, &way, &idx);
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
			int way, idx;
			void *_addr = prepareCacheAccess<false, sizeof(uint32_t)>(addr, &way, &idx);
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
			int way, idx;
			void *_addr = prepareCacheAccess<false, sizeof(uint64_t)>(addr, &way, &idx);
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
			int way, idx;
			void *addr = prepareCacheAccess<false, sizeof(mem128_t)>(mem, &way, &idx);
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
		int way, idx;
		void *_addr = prepareCacheAccess<true, sizeof(uint8_t)>(addr, &way, &idx);
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
		int way, idx;
		void *_addr = prepareCacheAccess<true, sizeof(uint16_t)>(addr, &way, &idx);
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
		int way, idx;
		void *_addr = prepareCacheAccess<true, sizeof(uint32_t)>(addr, &way, &idx);
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
		int way, idx;
		void *_addr = prepareCacheAccess<true, sizeof(uint64_t)>(addr, &way, &idx);
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
			int way, idx;
			void* addr = prepareCacheAccess<true, sizeof(mem128_t)>(mem, &way, &idx);
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

// Generates a tlbMiss Exception
static __ri void vtlb_Miss(u32 addr, u32 mode)
{
	// Hack to handle expected tlb miss by some games.
	if (Cpu == &intCpu)
	{
		if (mode)
			cpuTlbMissW(addr, cpuRegs.branch);
		else
			cpuTlbMissR(addr, cpuRegs.branch);

		// Exception handled. Current instruction need to be stopped
		Cpu->CancelInstruction();
	}
}

// clang-format off
template <typename OperandType>
static OperandType vtlbUnmappedVReadSm(u32 addr) { vtlb_Miss(addr, 0); return 0; }
static RETURNS_R128 vtlbUnmappedVReadLg(u32 addr) { vtlb_Miss(addr, 0); return r128_zero(); }

template <typename OperandType>
static void vtlbUnmappedVWriteSm(u32 addr, OperandType data) { vtlb_Miss(addr, 1); }
static void TAKES_R128 vtlbUnmappedVWriteLg(u32 addr, r128 data) { vtlb_Miss(addr, 1); }

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
__ri void vtlb_ReassignHandler(vtlbHandler rv,
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

vtlbHandler vtlb_NewHandler(void) { return vtlbHandlerCount++; }

// Registers a handler into the VTLB's internal handler array.  The handler defines specific behavior
// for how memory pages bound to the handler are read from / written to.  If any of the handler pointers
// are NULL, the memory operations will be mapped to the BusError handler (thus generating BusError
// exceptions if the emulated app attempts to access them).
//
// Note: All handlers persist across calls to vtlb_Reset(), but are wiped/invalidated by calls to vtlb_Init()
//
// Returns a handle for the newly created handler  See vtlb_MapHandler for use of the return value.
//
__ri vtlbHandler vtlb_RegisterHandler(vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	vtlbHandler rv = vtlb_NewHandler();
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
void vtlb_MapHandler(vtlbHandler handler, u32 start, u32 size)
{
	u32 end = start + (size - VTLB_PAGE_SIZE);

	while (start <= end)
	{
		vtlbdata.pmap[start >> VTLB_PAGE_BITS] = VTLBPhysical::fromHandler(handler);
		start += VTLB_PAGE_SIZE;
	}
}

void vtlb_MapBlock(void* base, u32 start, u32 size, u32 blocksize)
{
	if (!blocksize)
		blocksize = size;

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
		const bool writeable   = ((eemem_offset < Ps2MemSize::MainRam) ? (mmap_GetRamPageInfo(eemem_offset) != ProtMode_Write) : true);
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

// vtlb_Init -- Clears vtlb handlers and memory mappings.
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

// vtlb_Reset -- Performs a COP0-level reset of the PS2's TLB.
// This function should probably be part of the COP0 rather than here in VTLB.
void vtlb_Reset(void)
{
	vtlb_RemoveFastmemMappings();
	for (int i = 0; i < 48; i++)
		UnmapTLB(tlb[i], i);
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
bool vtlb_Core_Alloc(void)
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

static constexpr size_t PPMAP_SIZE = sizeof(*vtlbdata.ppmap) * VTLB_VMAP_ITEMS;

// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
// However automatic gamefix is done after the standard init so a new init function was done.
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

void vtlb_Core_Free(void)
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
vtlb_ProtectionMode mmap_GetRamPageInfo(u32 paddr)
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
void mmap_MarkCountedRamPage(u32 paddr)
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

// offset - offset of address relative to psM.
// All recompiled blocks belonging to the page are cleared, and any new blocks recompiled
// from code residing in this page will use manual protection.
static __fi void mmap_ClearCpuBlock(uint offset)
{
	PageProtectionMode mode;
	int rampage = offset >> __pageshift;

	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	HostSys::MemProtect(&eeMem->Main[rampage << __pageshift], __pagesize, mode);
	if (CHECK_FASTMEM)
		vtlb_UpdateFastmemProtection(rampage << __pageshift, __pagesize, mode);
	m_PageProtectInfo[rampage].Mode = ProtMode_Manual;
	Cpu->Clear(m_PageProtectInfo[rampage].ReverseRamMap, __pagesize);
}

bool vtlb_private::PageFaultHandler(const PageFaultInfo& info)
{
	u32 vaddr;
	if (CHECK_FASTMEM && vtlb_GetGuestAddress(info.addr, &vaddr))
	{
		uintptr_t ptr = (uintptr_t)PSM(vaddr);
		uintptr_t offset = (ptr - (uintptr_t)eeMem->Main);
		if (ptr && m_PageProtectInfo[offset >> __pageshift].Mode == ProtMode_Write)
		{
			mmap_ClearCpuBlock(offset);
			return true;
		}
		return vtlb_BackpatchLoadStore(info.pc, info.addr);
	}
	else
	{
		// get bad virtual address
		uintptr_t offset = info.addr - (uintptr_t)eeMem->Main;
		if (offset >= Ps2MemSize::MainRam)
			return false;

		mmap_ClearCpuBlock(offset);
		return true;
	}
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
								line.tag.rawValue &= LRF_FLAG;
								memset(&line.data, 0, sizeof(line.data));
								});
						break;

					case 0x18: /* DHWBIN (Data Cache Hit WriteBack with Invalidate) */
						doCacheHitOp(addr, [](CacheLine line)
								{
								line.writeBackIfNeeded();
								line.tag.rawValue &= LRF_FLAG;
								memset(&line.data, 0, sizeof(line.data));
								});
						break;

					case 0x1c: /* DHWOIN (Data Cache Hit WriteBack Without Invalidate) */
						doCacheHitOp(addr, [](CacheLine line)
								{
								line.writeBackIfNeeded();
								});
						break;

					case 0x16: /* DXIN (Data Cache Index Invalidate) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							line.tag.rawValue &= LRF_FLAG;
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
							line.writeBackIfNeeded();

							/* Our tags don't contain PS2 paddrs (instead they contain x86 addrs) */
							cpuRegs.CP0.n.TagLo = line.tag.rawValue & ALL_FLAGS;
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

							line.tag.rawValue &= ~ALL_FLAGS;
							line.tag.rawValue |= (cpuRegs.CP0.n.TagLo & ALL_FLAGS);
							break;
						}

					case 0x14: /* DXWBIN (Data Cache Index WriteBack Invalidate) */
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };
							line.writeBackIfNeeded();
							line.tag.rawValue &= LRF_FLAG;
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
