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

#include <algorithm>
#include <cstdlib> /* bsearch, realloc, free */
#include <cstring> /* memset */
#include <retro_atomic.h>

#include "../common/Threading.h" /* Threading::Timeslice */
#include <map>
#include <unordered_map>

#include "../common/Align.h"
#include "../common/Console.h"

#include "Common.h"
#include "vtlb.h"
#include "COP0.h"
#include "Cache.h"
#include "IopMem.h"
#include "Host.h"
#include "R5900.h"

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
	u32 offset;
	u32 size;
};

struct LoadstoreBackpatchInfo
{
	u32 guest_pc;
	u32 gpr_bitmask;
	u32 fpr_bitmask;
	u8 code_size;
	u8 address_register;
	u8 data_register;
	u8 size_in_bits;
	bool is_signed;
	bool is_load;
	bool is_fpr;
};

static constexpr size_t FASTMEM_AREA_SIZE = 0x100000000ULL;
static constexpr u32 FASTMEM_PAGE_COUNT = FASTMEM_AREA_SIZE / VTLB_PAGE_SIZE;
static constexpr u32 NO_FASTMEM_MAPPING = 0xFFFFFFFFu;

static std::unique_ptr<SharedMemoryMappingArea> s_fastmem_area;
static std::vector<u32> s_fastmem_virtual_mapping; // maps vaddr -> mainmem offset
static std::unordered_multimap<u32, u32> s_fastmem_physical_mapping; // maps mainmem offset -> vaddr
// Backpatch info for fastmem loadstores, keyed by host code address.
// code_address values are inserted in recompilation order (monotonically
// increasing within a run) and the table is cleared on every cache reset,
// so a sorted flat array kept ordered by code_address gives O(1) amortized
// append on the common path and bsearch lookup on the fault path, without
// the per-entry node allocation of std::unordered_map.
struct LoadstoreBackpatchEntry
{
	uptr                  code_address;
	LoadstoreBackpatchInfo info;
};
static LoadstoreBackpatchEntry* s_fastmem_backpatch       = NULL;
static size_t                   s_fastmem_backpatch_count = 0;
static size_t                   s_fastmem_backpatch_cap   = 0;

// Returns the index of code_address, or the insertion point (== count if it
// belongs at the end). *found is set to whether an exact match was present.
static size_t vtlb_BackpatchLowerBound(uptr code_address, bool* found)
{
	size_t lo = 0;
	size_t hi = s_fastmem_backpatch_count;
	while (lo < hi)
	{
		size_t mid = lo + ((hi - lo) >> 1);
		if (s_fastmem_backpatch[mid].code_address < code_address)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = (lo < s_fastmem_backpatch_count && s_fastmem_backpatch[lo].code_address == code_address);
	return lo;
}

static void vtlb_BackpatchClear(void)
{
	s_fastmem_backpatch_count = 0;
}

static void vtlb_BackpatchFree(void)
{
	free(s_fastmem_backpatch);
	s_fastmem_backpatch       = NULL;
	s_fastmem_backpatch_count = 0;
	s_fastmem_backpatch_cap   = 0;
}

// Sorted, unique list of guest PCs that have faulted out of fastmem.
// Raw buffer + realloc growth; bsearch for lookup.  Insert keeps the
// array sorted by shifting tail entries (rare path - one append per
// distinct faulting PC discovered, never reaches more than a few k).
/* Guards s_fastmem_backpatch* and s_fastmem_faulting_pcs*.
 *
 * These were serialized only INCIDENTALLY, by the global mutex the
 * page-fault filter used to take on entry.  That mutex is gone (it was
 * not async-signal-safe and it serialized every JIT fault
 * process-wide), so the exclusion has to be explicit and it has to
 * live here.  Without it, two threads faulting at once - EE and the
 * MTVU worker, which is the normal configuration - corrupt the
 * backpatch array and the sorted faulting-PC list.
 *
 * An atomic-flag spinlock, not a mutex: this runs inside the
 * SIGSEGV/VEH handler where pthread locks are not async-signal-safe.
 * Spinning is safe because nothing under the lock blocks, allocates,
 * or touches guest fastmem, so a nested fault while holding it cannot
 * occur and the holder is always another thread finishing bounded
 * work.
 *
 * The spin issues a CPU relax hint.  A bare exchange loop hammers the
 * cache line and starves the holder; measured on Windows/x86 it made
 * the lock-free path LOSE to a CRITICAL_SECTION (1.81 vs 1.48
 * us/fault, 4 threads), which is what a naive spin does under
 * contention. */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#if defined(_MSC_VER)
#define VTLB_CPU_RELAX() _mm_pause()
#else
#define VTLB_CPU_RELAX() __builtin_ia32_pause()
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
#if defined(_MSC_VER)
#define VTLB_CPU_RELAX() __yield()
#else
#define VTLB_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#endif
#else
#define VTLB_CPU_RELAX() ((void)0)
#endif

static retro_atomic_int_t s_fastmem_lock;

/* Spins before yielding.  Sized so an uncontended acquire and a normal
 * short hold never reach the yield, while a thread that has clearly
 * lost the race stops burning its slice.  sched_yield/SwitchToThread
 * are async-signal-safe, unlike a mutex. */
#define VTLB_SPIN_BEFORE_YIELD 256

static __fi void fastmem_lock(void)
{
	while (retro_atomic_exchange_int(&s_fastmem_lock, 1))
	{
		int spins = 0;
		/* Read-only spin until it looks free, so the contended path
		 * does not keep issuing exclusive-ownership RMWs at the
		 * holder. */
		while (retro_atomic_load_acquire_int(&s_fastmem_lock))
		{
			if (++spins < VTLB_SPIN_BEFORE_YIELD)
				VTLB_CPU_RELAX();
			else
			{
				spins = 0;
				/* common/ already abstracts this (SwitchToThread /
				 * sched_yield); both are async-signal-safe, unlike a
				 * mutex.  Platform #ifdefs do not belong here. */
				Threading::Timeslice();
			}
		}
	}
}

static __fi void fastmem_unlock(void)
{
	retro_atomic_store_release_int(&s_fastmem_lock, 0);
}

/* RAII scope for the functions below that have multiple return paths. */
struct FastmemLockScope
{
	__fi FastmemLockScope() { fastmem_lock(); }
	__fi ~FastmemLockScope() { fastmem_unlock(); }
};

static u32*   s_fastmem_faulting_pcs       = NULL;
static size_t s_fastmem_faulting_pcs_count = 0;
static size_t s_fastmem_faulting_pcs_cap   = 0;

static int u32_compare(const void* a, const void* b)
{
	u32 av = *(const u32*)a;
	u32 bv = *(const u32*)b;
	return (av > bv) - (av < bv);
}

static void s_fastmem_faulting_pcs_free(void)
{
	/* OS pages, not heap - see the growth site in
	 * vtlb_BackpatchLoadStore. */
	HostSys::Munmap(s_fastmem_faulting_pcs,
		s_fastmem_faulting_pcs_cap * sizeof(u32));
	s_fastmem_faulting_pcs       = NULL;
	s_fastmem_faulting_pcs_count = 0;
	s_fastmem_faulting_pcs_cap   = 0;
}

vtlb_private::VTLBPhysical vtlb_private::VTLBPhysical::fromPointer(sptr ptr)
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

/* The TLB entry's PageMask register field is not a usable address mask; the
 * bits above 12 encode the page size as a run of set bits. Convert it into an
 * actual ((1 << (12 + n)) - 1) mask so the cacheable-range check below is
 * correct. */
__inline int ConvertPageMask(u32 PageMask)
{
	/* Count the set bits in the page-size field portably (MSVC has no
	 * __builtin_popcount). This is only called during TLB setup, so a small
	 * loop is fine. */
	u32 bits = PageMask >> 13;
	u32 mask = 0;
	while (bits)
	{
		mask += bits & 1u;
		bits >>= 1;
	}

	return (1 << (12 + mask)) - 1;
}

__inline int CheckCache(u32 addr)
{
	if (((cpuRegs.CP0.n.Config >> 16) & 0x1) != 0)
	{
		u32 mask;
		for (int i = 1; i < 48; i++)
		{
			if (((tlb[i].EntryLo1 & 0x38) >> 3) == 0x3)
			{
				mask = ConvertPageMask(tlb[i].PageMask);

				if ((addr >= tlb[i].PFN1) && (addr <= tlb[i].PFN1 + mask))
					return true;
			}
			if (((tlb[i].EntryLo0 & 0x38) >> 3) == 0x3)
			{
				mask = ConvertPageMask(tlb[i].PageMask);

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

/* One concrete function per width. These were a single
 * template<typename DataType> whose body switched on sizeof(DataType) * 8 --
 * every arm of those switches was dead in each instantiation, and
 * assumeHandler<Width, Write> was a compile-time index into RWFT. Both fold
 * away here: the width, the cache accessor and the RWFT row are literals in
 * the macro expansion, so each function is the straight-line path its
 * instantiation always compiled to. */
#define VTLB_DEFINE_MEMREAD(bits, type, rwft_row, cache_read, fp_type)         \
type vtlb_memRead##bits(u32 addr)                                              \
{                                                                              \
	VTLBVirtual vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];                   \
	u32 paddr;                                                                 \
                                                                               \
	if (!vmv.isHandler(addr))                                                  \
	{                                                                          \
		if (!CHECK_EEREC)                                                      \
		{                                                                      \
			if (CHECK_CACHE && CheckCache(addr))                               \
				return cache_read(addr);                                       \
		}                                                                      \
		return *(type*)vmv.assumePtr(addr);                                    \
	}                                                                          \
                                                                               \
	/* has to: translate, find function, call function */                      \
	paddr = vmv.assumeHandlerGetPAddr(addr);                                   \
	return ((fp_type*)vtlbdata.RWFT[rwft_row][0][vmv.assumeHandlerGetID()])(paddr); \
}

VTLB_DEFINE_MEMREAD( 8, mem8_t,  0, readCache8,  vtlbMemR8FP)
VTLB_DEFINE_MEMREAD(16, mem16_t, 1, readCache16, vtlbMemR16FP)
VTLB_DEFINE_MEMREAD(32, mem32_t, 2, readCache32, vtlbMemR32FP)
VTLB_DEFINE_MEMREAD(64, mem64_t, 3, readCache64, vtlbMemR64FP)

RETURNS_R128 vtlb_memRead128(u32 mem)
{
	auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(mem))
	{
		if (!CHECK_EEREC)
		{
			if (CHECK_CACHE && CheckCache(mem))
				return readCache128(mem);
		}

		return r128_load(reinterpret_cast<const void*>(vmv.assumePtr(mem)));
	}
	else
	{
		//has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(mem);
		return vmv.assumeHandler<128, false>()(paddr);
	}
}

#define VTLB_DEFINE_MEMWRITE(bits, type, rwft_row, cache_write, fp_type)       \
void vtlb_memWrite##bits(u32 addr, type data)                                  \
{                                                                              \
	VTLBVirtual vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];                   \
	u32 paddr;                                                                 \
                                                                               \
	if (!vmv.isHandler(addr))                                                  \
	{                                                                          \
		if (!CHECK_EEREC)                                                      \
		{                                                                      \
			if (CHECK_CACHE && CheckCache(addr))                               \
			{                                                                  \
				cache_write(addr, data);                                       \
				return;                                                        \
			}                                                                  \
		}                                                                      \
		*(type*)vmv.assumePtr(addr) = data;                                    \
		return;                                                                \
	}                                                                          \
                                                                               \
	/* has to: translate, find function, call function */                      \
	paddr = vmv.assumeHandlerGetPAddr(addr);                                   \
	((fp_type*)vtlbdata.RWFT[rwft_row][1][vmv.assumeHandlerGetID()])(paddr, data); \
}

VTLB_DEFINE_MEMWRITE( 8, mem8_t,  0, writeCache8,  vtlbMemW8FP)
VTLB_DEFINE_MEMWRITE(16, mem16_t, 1, writeCache16, vtlbMemW16FP)
VTLB_DEFINE_MEMWRITE(32, mem32_t, 2, writeCache32, vtlbMemW32FP)
VTLB_DEFINE_MEMWRITE(64, mem64_t, 3, writeCache64, vtlbMemW64FP)

#if PCSX2_MINGW_R128_BY_PTR
void vtlb_memWrite128(u32 mem, const r128* value_ptr)
{
	const r128 value = r128_load(value_ptr);
#else
void TAKES_R128 vtlb_memWrite128(u32 mem, r128 value)
{
#endif
	auto vmv = vtlbdata.vmap[mem >> VTLB_PAGE_BITS];

	if (!vmv.isHandler(mem))
	{
		if (!CHECK_EEREC)
		{
			if (CHECK_CACHE && CheckCache(mem))
			{
				alignas(16) const u128 r = r128_to_u128(value);
				writeCache128(mem, &r);
				return;
			}
		}

		r128_store_unaligned((void*)vmv.assumePtr(mem), value);
	}
	else
	{
		//has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(mem);
#if PCSX2_MINGW_R128_BY_PTR
		vmv.assumeHandler<128, true>()(paddr, value_ptr);
#else
		vmv.assumeHandler<128, true>()(paddr, value);
#endif
	}
}


template <typename DataType>
bool vtlb_ramRead(u32 addr, DataType* value)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
	{
		memset(value, 0, sizeof(DataType));
		return false;
	}

	memcpy(value, reinterpret_cast<DataType*>(vmv.assumePtr(addr)), sizeof(DataType));
	return true;
}

template <typename DataType>
bool vtlb_ramWrite(u32 addr, const DataType& data)
{
	const auto vmv = vtlbdata.vmap[addr >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr))
		return false;

	memcpy(reinterpret_cast<DataType*>(vmv.assumePtr(addr)), &data, sizeof(DataType));
	return true;
}


template bool vtlb_ramRead<mem8_t>(u32 mem, mem8_t* value);
template bool vtlb_ramRead<mem16_t>(u32 mem, mem16_t* value);
template bool vtlb_ramRead<mem32_t>(u32 mem, mem32_t* value);
template bool vtlb_ramRead<mem64_t>(u32 mem, mem64_t* value);
template bool vtlb_ramRead<mem128_t>(u32 mem, mem128_t* value);
template bool vtlb_ramWrite<mem8_t>(u32 mem, const mem8_t& data);
template bool vtlb_ramWrite<mem16_t>(u32 mem, const mem16_t& data);
template bool vtlb_ramWrite<mem32_t>(u32 mem, const mem32_t& data);
template bool vtlb_ramWrite<mem64_t>(u32 mem, const mem64_t& data);
template bool vtlb_ramWrite<mem128_t>(u32 mem, const mem128_t& data);

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

static GoemonTlb* FindGoemonTlbCacheAddress()
{
	u32 tlbAddrs[] = {
		0x3d5580, // Cache table address for JPN final
		0x3db400, // Cache table address for June 22 prototype
		0x3dcd80 // Cache table address for August 26 prototype
	};
	for (u32 i = 0; i < sizeof(tlbAddrs); i++)
	{
		GoemonTlb* testTlb = (GoemonTlb*)&eeMem->Main[tlbAddrs[i]];
		if (testTlb[0].valid < 2)
			return testTlb;
	}
	return NULL;
}

void GoemonPreloadTlb(void)
{
	GoemonTlb* tlb = FindGoemonTlbCacheAddress();

	for (u32 i = 0; i < 150; i++)
	{
		if (tlb[i].valid == 0x1 && tlb[i].low_add != tlb[i].high_add)
		{

			u32 size = tlb[i].high_add - tlb[i].low_add;
			u32 vaddr = tlb[i].low_add;
			u32 paddr = tlb[i].physical_add;

			// TODO: The old code (commented below) seems to check specifically for handler 0.  Is this really correct?
			//if ((uptr)vtlbdata.vmap[vaddr>>VTLB_PAGE_BITS] == POINTER_SIGN_BIT) {
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
	GoemonTlb* tlb = FindGoemonTlbCacheAddress();

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
			cpuTlbMiss(addr, cpuRegs.branch, EXC_CODE_TLBS);
		else
			cpuTlbMiss(addr, cpuRegs.branch, EXC_CODE_TLBL);

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
#if PCSX2_MINGW_R128_BY_PTR
static void vtlbUnmappedVWriteLg(u32 addr, const r128* data) { vtlb_Miss(addr, 1); (void)data; }
#else
static void TAKES_R128 vtlbUnmappedVWriteLg(u32 addr, r128 data) { vtlb_Miss(addr, 1); }
#endif

template <typename OperandType>
static OperandType vtlbUnmappedPReadSm(u32 addr) { return 0; }
static RETURNS_R128 vtlbUnmappedPReadLg(u32 addr) { return r128_zero(); }

template <typename OperandType>
static void vtlbUnmappedPWriteSm(u32 addr, OperandType data) { }
#if PCSX2_MINGW_R128_BY_PTR
static void vtlbUnmappedPWriteLg(u32 addr, const r128* data) { (void)addr; (void)data; }
#else
static void TAKES_R128 vtlbUnmappedPWriteLg(u32 addr, r128 data) { }
#endif
// clang-format on

// --------------------------------------------------------------------------------------
//  VTLB mapping errors
// --------------------------------------------------------------------------------------
// These errors are assertion/logic errors that should never occur if PCSX2 has been initialized
// properly.  All addressable physical memory should be configured as TLBMiss or Bus Error.
//

static mem8_t vtlbDefaultPhyRead8(u32 addr) { return 0; }
static mem16_t vtlbDefaultPhyRead16(u32 addr) { return 0; }
static mem32_t vtlbDefaultPhyRead32(u32 addr) { return 0; }
static mem64_t vtlbDefaultPhyRead64(u32 addr) { return 0; }
static RETURNS_R128 vtlbDefaultPhyRead128(u32 addr) { return r128_zero(); }
static void vtlbDefaultPhyWrite8(u32 addr, mem8_t data) { }
static void vtlbDefaultPhyWrite16(u32 addr, mem16_t data) { }
static void vtlbDefaultPhyWrite32(u32 addr, mem32_t data) { }
static void vtlbDefaultPhyWrite64(u32 addr, mem64_t data) { }
#if PCSX2_MINGW_R128_BY_PTR
static void vtlbDefaultPhyWrite128(u32 addr, const r128* data) { (void)addr; (void)data; }
#else
static void TAKES_R128 vtlbDefaultPhyWrite128(u32 addr, r128 data) { }
#endif

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

vtlbHandler vtlb_NewHandler(void)
{
	return vtlbHandlerCount++;
}

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

	sptr baseint = (sptr)base;
	u32 end = start + (size - VTLB_PAGE_SIZE);

	while (start <= end)
	{
		u32 loopsz = blocksize;
		sptr ptr   = baseint;

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

static bool vtlb_GetMainMemoryOffsetFromPtr(uptr ptr, u32* mainmem_offset, u32* mainmem_size, PageProtectionMode* prot)
{
	const uptr page_end = ptr + VTLB_PAGE_SIZE;
	SysMainMemory& vmmem = GetVmMemory();

	// EE memory and ROMs.
	if (ptr >= (uptr)eeMem->Main && page_end <= (uptr)eeMem->ZeroRead)
	{
		const u32 eemem_offset = static_cast<u32>(ptr - (uptr)eeMem->Main);
		const bool writeable   = ((eemem_offset < Ps2MemSize::MainRam) ? (mmap_GetRamPageInfo(eemem_offset) != ProtMode_Write) : true);
		*mainmem_offset        = (eemem_offset + HostMemoryMap::EEmemOffset);
		*mainmem_size          = (offsetof(EEVM_MemoryAllocMess, ZeroRead) - eemem_offset);
		prot->m_read           = true;
		prot->m_write          = writeable;
		prot->m_exec           = false;
		return true;
	}

	// IOP memory.
	if (ptr >= (uptr)iopMem->Main && page_end <= (uptr)iopMem->P)
	{
		const u32 iopmem_offset = static_cast<u32>(ptr - (uptr)iopMem->Main);
		*mainmem_offset = iopmem_offset + HostMemoryMap::IOPmemOffset;
		*mainmem_size = (offsetof(IopVM_MemoryAllocMess, P) - iopmem_offset);
		prot->m_read  = true;
		prot->m_write = true;
		prot->m_exec  = false;
		return true;
	}

	// VU memory - this includes both data and code for VU0/VU1.
	// Practically speaking, this is only data, because the code goes through a handler.
	if (ptr >= (uptr)vmmem.VUMemory().GetPtr() && page_end <= (uptr)vmmem.VUMemory().GetPtrEnd())
	{
		const u32 vumem_offset = static_cast<u32>(ptr - (uptr)vmmem.VUMemory().GetPtr());
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
	/* Fastmem not initialized (allocation failed at init time). The
	 * VMap/VMapUnmap callers do not all gate on CHECK_FASTMEM, so
	 * defend here against indexing an empty vector. */
	if (s_fastmem_virtual_mapping.empty())
		return;

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

static bool vtlb_GetGuestAddress(uptr host_addr, u32* guest_addr)
{
	uptr fastmem_start = (uptr)vtlbdata.fastmem_base;
	uptr fastmem_end = fastmem_start + 0xFFFFFFFFu;
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
	const u32 num_pages = pcsx2_min_i(size, mainmem_size) / VTLB_PAGE_SIZE;
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
	FastmemLockScope fastmem_scope_;
	vtlb_BackpatchClear();
	s_fastmem_faulting_pcs_count = 0;
}

void vtlb_AddLoadStoreInfo(uptr code_address, u32 code_size, u32 guest_pc, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr)
{
	FastmemLockScope fastmem_scope_;
	LoadstoreBackpatchInfo info{guest_pc, gpr_bitmask, fpr_bitmask, static_cast<u8>(code_size), address_register, data_register, size_in_bits, is_signed, is_load, is_fpr};

	bool   found;
	size_t pos = vtlb_BackpatchLowerBound(code_address, &found);
	if (found)
	{
		// Replace existing entry in place.
		s_fastmem_backpatch[pos].info = info;
		return;
	}

	if (s_fastmem_backpatch_count == s_fastmem_backpatch_cap)
	{
		size_t newcap = s_fastmem_backpatch_cap ?
			s_fastmem_backpatch_cap + (s_fastmem_backpatch_cap >> 1) : 64;
		s_fastmem_backpatch     = (LoadstoreBackpatchEntry*)realloc(s_fastmem_backpatch, newcap * sizeof(LoadstoreBackpatchEntry));
		s_fastmem_backpatch_cap = newcap;
	}
	memmove(&s_fastmem_backpatch[pos + 1], &s_fastmem_backpatch[pos],
		(s_fastmem_backpatch_count - pos) * sizeof(LoadstoreBackpatchEntry));
	s_fastmem_backpatch[pos].code_address = code_address;
	s_fastmem_backpatch[pos].info         = info;
	s_fastmem_backpatch_count++;
}

#ifdef ARCH_ARM64
extern "C" bool eeFastmemFault_arm64(uintptr_t code_address); // arm64 EE rec (C.50)
#endif

static bool vtlb_BackpatchLoadStore(uptr code_address, uptr fault_address)
{
	uptr fastmem_start = (uptr)vtlbdata.fastmem_base;
	uptr fastmem_end = fastmem_start + 0xFFFFFFFFu;
	if (fault_address < fastmem_start || fault_address > fastmem_end)
		return false;

	/* Everything below touches the shared backpatch bookkeeping. */
	FastmemLockScope fastmem_scope_;

#ifdef ARCH_ARM64
	// The arm64 EE rec (C.50) keeps its own site registry and pre-emits the slow
	// stub next to every fastmem access, so backpatching is a single instruction
	// rewrite over there -- none of the info block below is needed.
	return eeFastmemFault_arm64(code_address);
#endif

	bool   found;
	size_t pos = vtlb_BackpatchLowerBound(code_address, &found);
	if (!found)
		return false;

	const LoadstoreBackpatchInfo& info = s_fastmem_backpatch[pos].info;
	const u32 info_guest_pc = info.guest_pc;
#ifdef ARCH_ARM64
	// No x86 recompiler / fastmem backpatching on arm64 (interpreter only).
	return false;
#else
	const u32 guest_addr = static_cast<u32>(fault_address - fastmem_start);
	vtlb_DynBackpatchLoadStore(code_address, info.code_size, info.guest_pc, guest_addr,
		info.gpr_bitmask, info.fpr_bitmask, info.address_register, info.data_register,
		info.size_in_bits, info.is_signed, info.is_load, info.is_fpr);
#endif

	// queue block for recompilation later
	Cpu->Clear(info_guest_pc, 1);

	// and store the pc in the faulting list, so that we don't emit another fastmem loadstore
	{
		size_t lo = 0;
		size_t hi = s_fastmem_faulting_pcs_count;
		while (lo < hi)
		{
			size_t mid = lo + ((hi - lo) >> 1);
			if (s_fastmem_faulting_pcs[mid] < info_guest_pc)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo == s_fastmem_faulting_pcs_count || s_fastmem_faulting_pcs[lo] != info_guest_pc)
		{
			if (s_fastmem_faulting_pcs_count == s_fastmem_faulting_pcs_cap)
			{
				/* We are inside the page-fault signal handler here:
				 * realloc() is not async-signal-safe.  In practice the
				 * faulting thread is executing recompiled code and so
				 * cannot itself hold the allocator lock, but that
				 * invariant is fragile (instrumented builds, future
				 * callers), so grow with raw OS pages instead and keep
				 * the handler allocator-free.  On failure, skip the
				 * bookkeeping: the only consequence is that this PC's
				 * fastmem loadstore gets backpatched again on a later
				 * fault, which is a performance detail, not a
				 * correctness one. */
				size_t newcap = s_fastmem_faulting_pcs_cap ?
					s_fastmem_faulting_pcs_cap + (s_fastmem_faulting_pcs_cap >> 1) : 16;
				const PageProtectionMode rw = {true, true, false};
				u32* newbuf = (u32*)HostSys::Mmap(nullptr, newcap * sizeof(u32), rw);
				if (!newbuf)
					goto skip_faulting_pc_insert;
				if (s_fastmem_faulting_pcs)
				{
					memcpy(newbuf, s_fastmem_faulting_pcs,
						s_fastmem_faulting_pcs_count * sizeof(u32));
					HostSys::Munmap(s_fastmem_faulting_pcs,
						s_fastmem_faulting_pcs_cap * sizeof(u32));
				}
				s_fastmem_faulting_pcs     = newbuf;
				s_fastmem_faulting_pcs_cap = newcap;
			}
			memmove(&s_fastmem_faulting_pcs[lo + 1], &s_fastmem_faulting_pcs[lo],
				(s_fastmem_faulting_pcs_count - lo) * sizeof(u32));
			s_fastmem_faulting_pcs[lo] = info_guest_pc;
			s_fastmem_faulting_pcs_count++;
		}
skip_faulting_pc_insert:;
	}
	memmove(&s_fastmem_backpatch[pos], &s_fastmem_backpatch[pos + 1],
		(s_fastmem_backpatch_count - pos - 1) * sizeof(LoadstoreBackpatchEntry));
	s_fastmem_backpatch_count--;
	return true;
}

bool vtlb_IsFaultingPC(u32 guest_pc)
{
	FastmemLockScope fastmem_scope_;
	if (!s_fastmem_faulting_pcs_count)
		return false;
	return bsearch(&guest_pc, s_fastmem_faulting_pcs, s_fastmem_faulting_pcs_count, sizeof(u32), u32_compare) != NULL;
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

	uptr bu8 = (uptr)buffer;
	while (size > 0)
	{
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = VTLBVirtual::fromPointer(bu8, vaddr);
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
	baseName##ReadSm<mem8_t>, baseName##ReadSm<mem16_t>, baseName##ReadSm<mem32_t>, \
		baseName##ReadSm<mem64_t>, baseName##ReadLg, \
		baseName##WriteSm<mem8_t>, baseName##WriteSm<mem16_t>, baseName##WriteSm<mem32_t>, \
		baseName##WriteSm<mem64_t>, baseName##WriteLg

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
	vtlb_BackpatchFree();
	s_fastmem_faulting_pcs_free();
}

void vtlb_ResetFastmem(void)
{
	vtlb_RemoveFastmemMappings();
	vtlb_BackpatchClear();
	s_fastmem_faulting_pcs_count = 0;

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
			/* Fastmem could not be allocated (Win10 placeholder APIs
			 * missing, or address space too fragmented to place a 4 GB
			 * region above the 4 GB boundary). Don't fail the whole
			 * init - disable fastmem and let the recompiler emit
			 * slowpath load/stores for every memory access. Slower but
			 * functional. */
			Console.Error("Failed to allocate fastmem area, disabling fastmem.");
			EmuConfig.Cpu.Recompiler.EnableFastmem = false;
		}
		else
		{
			s_fastmem_virtual_mapping.resize(FASTMEM_PAGE_COUNT, NO_FASTMEM_MAPPING);
			vtlbdata.fastmem_base = (uptr)s_fastmem_area->BasePointer();
			Console.WriteLn("Fastmem area: %p - %p",
				(void*)vtlbdata.fastmem_base,
				(void*)(vtlbdata.fastmem_base + (FASTMEM_AREA_SIZE - 1)));
		}
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
		ppmap = (u32*)GetVmMemory().BumpAllocator().Alloc(PPMAP_SIZE);

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

	uptr ptr = (uptr)PSM(paddr);
	uptr rampage = ptr - (uptr)eeMem->Main;

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

	uptr ptr = (uptr)PSM(paddr);
	int rampage = (ptr - (uptr)eeMem->Main) >> __pageshift;

	// Defensive: PSM() can yield a pointer outside Main (handler-mapped page,
	// TLB-remapped, SPR) -- protecting a page outside Main both corrupts
	// m_PageProtectInfo (OOB) and mprotects unrelated host memory.
	if (!ptr || rampage < 0 || rampage >= (int)(Ps2MemSize::MainRam >> __pageshift))
	{
		return;
	}

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
	// Clear takes an instruction count, not a byte count -- every other
	// caller passes one (COP0 uses 0x400 for a TLB page, CDVD uses
	// BlockSize / 4, IopMem uses 1 for a word). Passing __pagesize here
	// asks for four pages' worth of blocks.
	Cpu->Clear(m_PageProtectInfo[rampage].ReverseRamMap, __pagesize / 4);
}

bool vtlb_private::PageFaultHandler(const PageFaultInfo& info)
{
	u32 vaddr;
	if (CHECK_FASTMEM && vtlb_GetGuestAddress(info.addr, &vaddr))
	{
		uptr ptr = (uptr)PSM(vaddr);
		uptr offset = (ptr - (uptr)eeMem->Main);
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
		uptr offset = info.addr - (uptr)eeMem->Main;
		if (offset >= Ps2MemSize::MainRam)
		{
			return false;
		}

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
