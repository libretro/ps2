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

// Must be included after the backend-specific 'struct microVU' is defined,
// since the add() and search() methods access mVU->compareStateF.

#pragma once

#include "common/AlignedMalloc.h"

class microBlockManager
{
private:
	microBlockLink *qBlockList, *qBlockEnd; // Quick Search
	microBlockLink *fBlockList, *fBlockEnd; // Full  Search
	std::vector<microBlockLinkRef> quickLookup;

public:
	microBlockManager()
	{
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
	}
	~microBlockManager() { reset(); }
	void reset()
	{
		for (microBlockLink* linkI = qBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			delete[](linkI->block.jumpCache);
			linkI->block.jumpCache = NULL;
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		for (microBlockLink* linkI = fBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			delete[](linkI->block.jumpCache);
			linkI->block.jumpCache = NULL;
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
		quickLookup.clear();
	};
	microBlock* add(microVU* mVU, microBlock* pBlock)
	{
		microBlock* thisBlock = search(mVU, &pBlock->pState);
		if (!thisBlock)
		{
			u8 fullCmp = pBlock->pState.needExactMatch;

			microBlockLink*& blockList = fullCmp ? fBlockList : qBlockList;
			microBlockLink*& blockEnd  = fullCmp ? fBlockEnd  : qBlockEnd;
			microBlockLink*  newBlock  = (microBlockLink*)_aligned_malloc(sizeof(microBlockLink), 32);
			newBlock->block.jumpCache  = nullptr;
			newBlock->next             = nullptr;

			if (blockEnd)
			{
				blockEnd->next = newBlock;
				blockEnd       = newBlock;
			}
			else
				blockEnd = blockList = newBlock;

			memcpy(&newBlock->block, pBlock, sizeof(microBlock));
			thisBlock = &newBlock->block;

			quickLookup.push_back({&newBlock->block, pBlock->pState.quick64[0]});
		}
		return thisBlock;
	}
	__ri microBlock* search(microVU* mVU, microRegInfo* pState)
	{
		if (pState->needExactMatch) // Needs Detailed Search (Exact Match of Pipeline State)
		{
			microBlockLink* prevI = nullptr;
			for (microBlockLink* linkI = fBlockList; linkI != nullptr; prevI = linkI, linkI = linkI->next)
			{
				if (reinterpret_cast<u32(*)(void*, void*)>(mVU->compareStateF)(pState, &linkI->block.pState) == 0)
				{
					if (linkI != fBlockList)
					{
						prevI->next = linkI->next;
						linkI->next = fBlockList;
						fBlockList = linkI;
					}

					return &linkI->block;
				}
			}
		}
		else // Can do Simple Search (Only Matches the Important Pipeline Stuff)
		{
			const u64 quick64 = pState->quick64[0];
			for (const microBlockLinkRef& ref : quickLookup)
			{
				// If the flag hack is on, ignore the MAC flags (0x0C04) in the
				// quick match: a non-exact-match block does not need them, so
				// allowing blocks that differ only in MAC flags to match cuts
				// the block count (less recompilation / stuttering).
				if (mVUsFlagHack)
				{
					if ((ref.quick & ~0x0C04) != (quick64 & ~0x0C04)) continue;
				}
				else if (ref.quick != quick64) continue;
				if (doConstProp && (ref.pBlock->pState.vi15 != pState->vi15))  continue;
				if (doConstProp && (ref.pBlock->pState.vi15v != pState->vi15v)) continue;
				return ref.pBlock;
			}
		}
		return nullptr;
	}
};
