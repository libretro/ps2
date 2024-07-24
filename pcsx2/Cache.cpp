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

#include "Common.h"
#include "Cache.h"
#include "vtlb.h"

// The lower parts of a cache tags structure is as follows:
// 31 - 12: The physical address cache tag.
// 11 - 7: Unused.
// 6: Dirty flag.
// 5: Valid flag.
// 4: LRF flag - least recently filled flag.
// 3: Lock flag.
// 2-0: Unused.

#define DIRTY_FLAG 0x40
#define VALID_FLAG 0x20
#define LRF_FLAG 0x10
#define LOCK_FLAG 0x8
#define ALL_FLAGS 0xFFF

namespace
{

	union alignas(64) CacheData
	{
		u8 bytes[64];
	};

	struct CacheTag
	{
		uptr rawValue;
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

			uptr target = (tag.rawValue & ~ALL_FLAGS) | (set << 6);

			*reinterpret_cast<CacheData*>(target) = data;
			tag.rawValue &= ~DIRTY_FLAG;
		}

		void load(uptr ppf)
		{
			tag.rawValue &= ALL_FLAGS;
			tag.rawValue |= (ppf & ~ALL_FLAGS);
			memcpy(&data, reinterpret_cast<void*>(ppf & ~0x3FULL), sizeof(data));
			tag.rawValue |=  VALID_FLAG;
			tag.rawValue &= ~DIRTY_FLAG;
		}
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

	static Cache cache = {};

}

static bool findInCache(const CacheSet& set, uptr ppf, int* way)
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

static int getFreeCache(u32 mem, int* way)
{
	const int setIdx = (mem >> 6) & 0x3F;
	CacheSet& set    = cache.sets[setIdx];
	vtlb_private::VTLBVirtual vmv  = vtlb_private::vtlbdata.vmap[mem >> vtlb_private::VTLB_PAGE_BITS];
	uptr ppf         = vmv.assumePtr(mem);

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

template <typename Int>
static void writeCache(u32 mem, Int value)
{
	int way, idx;
	void* addr = prepareCacheAccess<true, sizeof(Int)>(mem, &way, &idx);
	*reinterpret_cast<Int*>(addr) = value;
}

void writeCache8(u32 mem, u8 value)         { writeCache<u8>(mem, value); }
void writeCache16(u32 mem, u16 value)       { writeCache<u16>(mem, value); }
void writeCache32(u32 mem, u32 value)       { writeCache<u32>(mem, value); }
void writeCache64(u32 mem, const u64 value) { writeCache<u64>(mem, value); }

void writeCache128(u32 mem, const mem128_t* value)
{
	int way, idx;
	void* addr = prepareCacheAccess<true, sizeof(mem128_t)>(mem, &way, &idx);
	*reinterpret_cast<mem128_t*>(addr) = *value;
}

template <typename Int>
static Int readCache(u32 mem)
{
	int way, idx;
	void* addr = prepareCacheAccess<false, sizeof(Int)>(mem, &way, &idx);
	return *reinterpret_cast<Int*>(addr);
}

u8 readCache8(u32 mem)   { return readCache<u8>(mem);  }
u16 readCache16(u32 mem) { return readCache<u16>(mem); }
u32 readCache32(u32 mem) { return readCache<u32>(mem); }
u64 readCache64(u32 mem) { return readCache<u64>(mem); }

RETURNS_R128 readCache128(u32 mem)
{
	int way, idx;
	void* addr = prepareCacheAccess<false, sizeof(mem128_t)>(mem, &way, &idx);
	r128 value = r128_load(addr);
	u64* vptr = reinterpret_cast<u64*>(&value);
	return value;
}

template <typename Op>
static void doCacheHitOp(u32 addr, Op op)
{
	int way;
	const int index = (addr >> 6) & 0x3F;
	CacheSet& set   = cache.sets[index];
	vtlb_private::VTLBVirtual vmv = vtlb_private::vtlbdata.vmap[addr >> vtlb_private::VTLB_PAGE_BITS];
	uptr ppf = vmv.assumePtr(addr);
	if (findInCache(set, ppf, &way))
		op({ cache.sets[index].tags[way], cache.sets[index].data[way], index });
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
					case 0x1a: //DHIN (Data Cache Hit Invalidate)
						doCacheHitOp(addr, [](CacheLine line)
								{
								line.tag.rawValue &= LRF_FLAG;
								memset(&line.data, 0, sizeof(line.data));
								});
						break;

					case 0x18: //DHWBIN (Data Cache Hit WriteBack with Invalidate)
						doCacheHitOp(addr, [](CacheLine line)
								{
								line.writeBackIfNeeded();
								line.tag.rawValue &= LRF_FLAG;
								memset(&line.data, 0, sizeof(line.data));
								});
						break;

					case 0x1c: //DHWOIN (Data Cache Hit WriteBack Without Invalidate)
						doCacheHitOp(addr, [](CacheLine line)
								{
								line.writeBackIfNeeded();
								});
						break;

					case 0x16: //DXIN (Data Cache Index Invalidate)
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							line.tag.rawValue &= LRF_FLAG;
							memset(&line.data, 0, sizeof(line.data));
							break;
						}

					case 0x11: //DXLDT (Data Cache Load Data into TagLo)
						{
							const int index     = (addr >> 6) & 0x3F;
							const int way       = addr & 0x1;
							CacheLine line      = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							cpuRegs.CP0.n.TagLo = *reinterpret_cast<u32*>(&line.data.bytes[addr & 0x3C]);

							break;
						}

					case 0x10: //DXLTG (Data Cache Load Tag into TagLo)
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							// DXLTG demands that SYNC.L is called before this command, which forces the cache to write back, so presumably games are checking the cache has updated the memory
							// For speed, we will do it here.
							line.writeBackIfNeeded();

							// Our tags don't contain PS2 paddrs (instead they contain x86 addrs)
							cpuRegs.CP0.n.TagLo = line.tag.rawValue & ALL_FLAGS;
							break;
						}

					case 0x13: //DXSDT (Data Cache Store 32bits from TagLo)
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							*reinterpret_cast<u32*>(&line.data.bytes[addr & 0x3C]) = cpuRegs.CP0.n.TagLo;
							break;
						}

					case 0x12: //DXSTG (Data Cache Store Tag from TagLo)
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };

							line.tag.rawValue &= ~ALL_FLAGS;
							line.tag.rawValue |= (cpuRegs.CP0.n.TagLo & ALL_FLAGS);
							break;
						}

					case 0x14: //DXWBIN (Data Cache Index WriteBack Invalidate)
						{
							const int index = (addr >> 6) & 0x3F;
							const int way   = addr & 0x1;
							CacheLine line  = { cache.sets[index].tags[way], cache.sets[index].data[way], index };
							line.writeBackIfNeeded();
							line.tag.rawValue &= LRF_FLAG;
							memset(&line.data, 0, sizeof(line.data));
							break;
						}

					case 0x7: //IXIN (Instruction Cache Index Invalidate) Not Implemented as we do not have instruction cache
					case 0xC: //BFH (BTAC Flush) Not Implemented as we do not cache Branch Target Addresses.
					default:
						break;
				}
			}
		} // end namespace OpcodeImpl
	}
}
