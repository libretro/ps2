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

#include "../../common/Pcsx2Defs.h"
#include "../../common/Pcsx2Types.h"

#include <cstring>
#include <cstdlib>

// Every potential jump point in the PS2's addressable memory has a BASEBLOCK
// associated with it. So that means a BASEBLOCK for every 4 bytes of PS2
// addressable memory.  Yay!
struct BASEBLOCK
{
	uptr m_pFnptr;
};

// extra block info (only valid for start of fn)
struct BASEBLOCKEX
{
	uptr fnptr;
	u32 startpc;
	u32 size;    // The size in dwords (equivalent to the number of instructions)
	u32 x86size; // The size in byte of the translated x86 instructions
};

class BaseBlockArray
{
	s32 _Reserved;
	s32 _Size;
	BASEBLOCKEX* blocks;

	__fi void resize(s32 size)
	{
		BASEBLOCKEX* newMem = new BASEBLOCKEX[size];
		if (blocks)
		{
			// Only _Size entries are live; the slots between _Size and
			// _Reserved are uninitialized scratch and don't need copying.
			// insert() zeroes new slots per-entry as it uses them.
			memcpy(newMem, blocks, _Size * sizeof(BASEBLOCKEX));
			delete[] blocks;
		}
		blocks = newMem;
	}

	void reserve(u32 size)
	{
		resize(size);
		_Reserved = size;
	}

public:
	~BaseBlockArray()
	{
		if (blocks)
			delete[] blocks;
	}

	BaseBlockArray(s32 size)
		: _Reserved(0)
		, _Size(0)
		, blocks(NULL)
	{
		reserve(size);
	}

	BASEBLOCKEX* insert(u32 startpc, uptr fnptr)
	{
		if (_Size + 1 >= _Reserved)
		{
			reserve(_Reserved + 0x2000); // some games requires even more!
		}

		// Insert the the new BASEBLOCKEX by startpc order
		int imin = 0, imax = _Size, imid;

		while (imin < imax)
		{
			imid = (imin + imax) >> 1;

			if (blocks[imid].startpc > startpc)
				imax = imid;
			else
				imin = imid + 1;
		}

		if (imin < _Size)
		{
			// make a hole for a new block.
			memmove(blocks + imin + 1, blocks + imin, (_Size - imin) * sizeof(BASEBLOCKEX));
		}

		memset((blocks + imin), 0, sizeof(BASEBLOCKEX));
		blocks[imin].startpc = startpc;
		blocks[imin].fnptr = fnptr;

		_Size++;
		return &blocks[imin];
	}

	__fi BASEBLOCKEX& operator[](int idx) const
	{
		return *(blocks + idx);
	}

	void clear()
	{
		_Size = 0;
	}

	__fi u32 size() const
	{
		return _Size;
	}

	__fi void erase(s32 first, s32 last)
	{
		int range = last - first;

		if (last < _Size)
		{
			memmove(blocks + first, blocks + last, (_Size - last) * sizeof(BASEBLOCKEX));
		}

		_Size -= range;
	}
};

// Maps a guest start PC to the set of host jump sites (u32* patch locations,
// stored as uptr) that target it. Replaces std::multimap<u32, uptr>: the only
// operations needed are insert-with-duplicates, "walk all jump sites for a
// given PC", and clear. No ordering is required, and inserts arrive in
// arbitrary PC order on a hot compile path, so a chained hash gives O(1)
// amortized insert (a sorted array would be O(n) memmove per insert) while a
// bucket walk serves the range query.
//
// Entries live in a single growable POD pool and are chained by index (not
// pointer) so a realloc of the pool never invalidates the chains.
class BlockLinkMap
{
	struct Entry
	{
		u32  pc;
		uptr jumpptr;
		s32  next; // pool index of next entry in this bucket, or -1
	};

	Entry* m_entries;
	s32    m_count;
	s32    m_capacity;
	s32*   m_buckets;
	u32    m_bucket_count; // power of two
	u32    m_bucket_mask;

	__fi u32 bucket_of(u32 pc) const
	{
		// startpc is 4-byte aligned; drop the low 2 bits before masking so
		// adjacent block PCs spread across buckets.
		return (pc >> 2) & m_bucket_mask;
	}

	void grow_entries(void)
	{
		s32 newcap = m_capacity ? m_capacity * 2 : 4096;
		m_entries  = (Entry*)realloc(m_entries, newcap * sizeof(Entry));
		m_capacity = newcap;
	}

public:
	BlockLinkMap()
		: m_entries(NULL)
		, m_count(0)
		, m_capacity(0)
		, m_buckets(NULL)
		, m_bucket_count(0)
		, m_bucket_mask(0)
	{
		m_bucket_count = 0x10000; // 64k buckets, covers typical live-link counts
		m_bucket_mask  = m_bucket_count - 1;
		m_buckets      = (s32*)malloc(m_bucket_count * sizeof(s32));
		memset(m_buckets, 0xFF, m_bucket_count * sizeof(s32)); // all -1
	}

	~BlockLinkMap()
	{
		free(m_entries);
		free(m_buckets);
	}

	__fi void insert(u32 pc, uptr jumpptr)
	{
		if (m_count == m_capacity)
			grow_entries();
		const u32 b = bucket_of(pc);
		Entry& e    = m_entries[m_count];
		e.pc        = pc;
		e.jumpptr   = jumpptr;
		e.next      = m_buckets[b];
		m_buckets[b] = m_count;
		m_count++;
	}

	// Walks every jump site registered for pc and rewrites each one to a 32-bit
	// rel32 displacement pointing at target_addr (target_addr - (site + 4)).
	__fi void patch_links(u32 pc, uptr target_addr) const
	{
		s32 i = m_buckets[bucket_of(pc)];
		while (i != -1)
		{
			const Entry& e = m_entries[i];
			if (e.pc == pc)
			{
				const u32 rel32_ = (u32)(target_addr - (e.jumpptr + 4));
				memcpy((void*)e.jumpptr, &rel32_, sizeof(u32));
			}
			i = e.next;
		}
	}

	__fi void clear(void)
	{
		m_count = 0;
		memset(m_buckets, 0xFF, m_bucket_count * sizeof(s32));
	}
};

class BaseBlocks
{
protected:
	BlockLinkMap links;
	uptr recompiler;
	BaseBlockArray blocks;

public:
	BaseBlocks()
		: recompiler(0)
		, blocks(0x4000)
	{
	}

	void SetJITCompile(const void *recompiler_)
	{
		recompiler = reinterpret_cast<uptr>(recompiler_);
	}

	BASEBLOCKEX* New(u32 startpc, uptr fnptr);
	int LastIndex(u32 startpc) const;

	__fi int Index(u32 startpc) const
	{
		int idx = LastIndex(startpc);

		if ((idx == -1) || (startpc < blocks[idx].startpc) ||
			((blocks[idx].size) && (startpc >= blocks[idx].startpc + blocks[idx].size * 4)))
			return -1;
		return idx;
	}

	__fi BASEBLOCKEX* operator[](int idx)
	{
		if (idx < 0 || idx >= (int)blocks.size())
			return 0;

		return &blocks[idx];
	}

	__fi BASEBLOCKEX* Get(u32 startpc)
	{
		return (*this)[Index(startpc)];
	}

	__fi void Remove(int first, int last)
	{
		int idx = first;
		do
		{
			links.patch_links(blocks[idx].startpc, recompiler);
		} while (idx++ < last);

		// TODO: remove links from this block?
		blocks.erase(first, last + 1);
	}

	void Link(u32 pc, s32* jumpptr);

	__fi void Reset()
	{
		blocks.clear();
		links.clear();
	}
};

#define PC_GETBLOCK_(x, reclut) ((BASEBLOCK*)(reclut[((u32)(x)) >> 16] + (x) * (sizeof(BASEBLOCK) / 4)))

/**
 * Add a page to the recompiler lookup table
 *
 * Will associate `reclut[pagebase + pageidx]` with `mapbase[mappage << 14]`
 * Will associate `hwlut[pagebase + pageidx]` with `pageidx << 16`
 */
static inline void recLUT_SetPage(uptr reclut[0x10000], u32 hwlut[0x10000],
                                  BASEBLOCK* mapbase, uint pagebase, uint pageidx, uint mappage)
{
	// this value is in 64k pages!
	uint page = pagebase + pageidx;

	/* Integer arithmetic on uptr, not pointer arithmetic: mapbase may be
	 * the null sentinel and the page delta is negative for pages after
	 * the mapped one - '&mapbase[negative]' on null and '<<' on a
	 * negative value are both UB.  The stored value is bit-identical. */
	reclut[page] = (uptr)mapbase +
		(uptr)(((sptr)mappage - (sptr)page) * (sptr)0x4000 * (sptr)sizeof(BASEBLOCK));
	if (hwlut)
		hwlut[page] = 0u - (pagebase << 16);
}
