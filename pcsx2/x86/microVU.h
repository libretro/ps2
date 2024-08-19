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

#include <stdint.h>
#include <deque>
#include <algorithm>
#include <cstring> /* memset/memcpy */
#include <memory>

#include "Common.h"
#include "VU.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"
#include "VirtualMemory.h"
#include "x86emitter.h"
#include "microVU_Misc.h"
#include "microVU_IR.h"

class microBlockManager;

struct microBlockLink
{
	microBlock block;
	microBlockLink* next;
};

struct microBlockLinkRef
{
	microBlock* pBlock;
	uint64_t quick;
};

struct microRange
{
	int32_t start; /* Start PC (The opcode the block starts at) */
	int32_t end;   /* End PC   (The opcode the block ends with) */
};

#define mProgSize (0x4000 / 4)
struct microProgram
{
	uint32_t                data [mProgSize]; /* Holds a copy of the VU microProgram */
	microBlockManager* block[mProgSize / 2];  /* Array of Block Managers */
	std::deque<microRange>* ranges;           /* The ranges of the microProgram that 
						   * have already been recompiled */
	uint32_t startPC; 			  /* Start PC of this program */
	int idx;     				  /* Program index */
};

typedef std::deque<microProgram*> microProgramList;

struct microProgramQuick
{
	microBlockManager* block; // Quick reference to valid microBlockManager for current startPC
	microProgram*      prog;  // The microProgram who is the owner of 'block'
};

struct microProgManager
{
	microIR<mProgSize> IRinfo;             // IR information
	microProgramList*  prog [mProgSize/2]; // List of microPrograms indexed by startPC values
	microProgramQuick  quick[mProgSize/2]; // Quick reference to valid microPrograms for current execution
	microProgram*      cur;                // Pointer to currently running MicroProgram
	int                total;              // Total Number of valid MicroPrograms
	int                isSame;             // Current cached microProgram is Exact Same program as vuRegs[mVU.index].Micro (-1 = unknown, 0 = No, 1 = Yes)
	int                cleared;            // Micro Program is Indeterminate so must be searched for (and if no matches are found then recompile a new one)
	uint32_t                curFrame;      // Frame Counter
	uint8_t*                x86ptr;        // Pointer to program's recompilation code
	uint8_t*                x86start;      // Start of program's rec-cache
	uint8_t*                x86end;        // Limit of program's rec-cache
	microRegInfo       lpState;            // Pipeline state from where program left off 
					       // (useful for continuing execution)
};

static const uint mVUdispCacheSize = __pagesize; // Dispatcher Cache Size (in bytes)
static const uint mVUcacheSafeZone =  3; // Safe-Zone for program recompilation (in megabytes)
static const uint mVUcacheReserve = 64; // mVU0, mVU1 Reserve Cache Size (in megabytes)

struct microVU
{

	alignas(16) uint32_t statFlag[4]; // 4 instances of status flag (backup for xgkick)
	alignas(16) uint32_t macFlag [4]; // 4 instances of mac    flag (used in execution)
	alignas(16) uint32_t clipFlag[4]; // 4 instances of clip   flag (used in execution)
	alignas(16) uint32_t xmmCTemp[4];      // Backup used in mVUclamp2()
	alignas(16) uint32_t xmmBackup[16][4]; // Backup for xmm0~xmm15

	uint32_t index;        // VU Index (VU0 or VU1)
	uint32_t cop2;         // VU is in COP2 mode?  (No/Yes)
	uint32_t vuMemSize;    // VU Main  Memory Size (in bytes)
	uint32_t microMemSize; // VU Micro Memory Size (in bytes)
	uint32_t progSize;     // VU Micro Memory Size (in uint32_t's)
	uint32_t progMemMask;  // VU Micro Memory Size (in uint32_t's)
	uint32_t cacheSize;    // VU Cache Size

	microProgManager               prog;     // Micro Program Data
	std::unique_ptr<microRegAlloc> regAlloc; // Reg Alloc Class

	RecompiledCodeReserve* cache_reserve;
	uint8_t* cache;        // Dynarec Cache Start (where we will start writing the recompiled code to)
	uint8_t* dispCache;    // Dispatchers Cache (where startFunct and exitFunct are written to)
	uint8_t* startFunct;   // Function Ptr to the recompiler dispatcher (start)
	uint8_t* exitFunct;    // Function Ptr to the recompiler dispatcher (exit)
	uint8_t* startFunctXG; // Function Ptr to the recompiler dispatcher (xgkick resume)
	uint8_t* exitFunctXG;  // Function Ptr to the recompiler dispatcher (xgkick exit)
	uint8_t* compareStateF;// Function Ptr to search which compares all state.
	uint8_t* waitMTVU;     // Ptr to function to save registers/sync VU1 thread
	uint8_t* copyPLState;  // Ptr to function to copy pipeline state into microVU
	uint8_t* resumePtrXG;  // Ptr to recompiled code position to resume xgkick
	uint32_t code;         // Contains the current Instruction
	uint32_t divFlag;      // 1 instance of I/D flags
	uint32_t VIbackup;     // Holds a backup of a VI reg if modified before a branch
	uint32_t VIxgkick;     // Holds a backup of a VI reg used for xgkick-delays
	uint32_t branch;       // Holds branch compare result (IBxx) OR Holds address to Jump to (JALR/JR)
	uint32_t badBranch;    // For Branches in Branch Delay Slots, holds Address the first Branch went to + 8
	uint32_t evilBranch;   // For Branches in Branch Delay Slots, holds Address to Jump to
	uint32_t evilevilBranch;// For Branches in Branch Delay Slots (chained), holds Address to Jump to
	uint32_t p;            // Holds current P instance index
	uint32_t q;            // Holds current Q instance index
	uint32_t totalCycles;  // Total Cycles that mVU is expected to run for
	int32_t cycles;        // Cycles Counter
};

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
	microBlock* add(microVU& mVU, microBlock* pBlock)
	{
		microBlock* thisBlock = search(mVU, &pBlock->pState);
		if (!thisBlock)
		{
			uint8_t fullCmp = pBlock->pState.needExactMatch;

			microBlockLink*& blockList = fullCmp ? fBlockList : qBlockList;
			microBlockLink*& blockEnd  = fullCmp ? fBlockEnd  : qBlockEnd;
			microBlockLink*  newBlock  = (microBlockLink*)_aligned_malloc(
					sizeof(microBlockLink), 32);
			newBlock->block.jumpCache  = nullptr;
			newBlock->next             = nullptr;

			if (blockEnd)
			{
				blockEnd->next = newBlock;
				blockEnd       = newBlock;
			}
			else
				blockEnd       = blockList = newBlock;

			memcpy(&newBlock->block, pBlock, sizeof(microBlock));
			thisBlock = &newBlock->block;

			quickLookup.push_back({&newBlock->block, pBlock->pState.quick64[0]});
		}
		return thisBlock;
	}
	__ri microBlock* search(microVU& mVU, microRegInfo* pState)
	{
		if (pState->needExactMatch) // Needs Detailed Search (Exact Match of Pipeline State)
		{
			microBlockLink* prevI = nullptr;
			for (microBlockLink* linkI = fBlockList; linkI != nullptr; prevI = linkI, linkI = linkI->next)
			{
				if (reinterpret_cast<uint32_t(*)(void*, void*)>(mVU.compareStateF)(pState, &linkI->block.pState) == 0)
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
			const uint64_t quick64 = pState->quick64[0];
			for (const microBlockLinkRef& ref : quickLookup)
			{
				if (ref.quick != quick64)
					continue;
				if (doConstProp)
				{
					if ((ref.pBlock->pState.vi15 != pState->vi15))
						continue;
					if ((ref.pBlock->pState.vi15v != pState->vi15v))
						continue;
				}
				return ref.pBlock;
			}
		}
		return nullptr;
	}
};

/* microVU rec structs */
alignas(16) microVU microVU0;
alignas(16) microVU microVU1;

/* Main Functions */
extern void mVUclear(mV, uint32_t, uint32_t);
extern void mVUreset(microVU& mVU, bool resetReserve);
extern void* mVUblockFetch(microVU& mVU, uint32_t startPC, uintptr_t pState);
_mVUt extern void* mVUcompileJIT(uint32_t startPC, uintptr_t ptr);

/* Prototypes */
extern void mVUcleanUpVU0(void);
extern void mVUcleanUpVU1(void);
mVUop(mVUopU);
mVUop(mVUopL);

/* Private Functions */
extern void mVUcacheProg(microVU& mVU, microProgram& prog);
extern void mVUdeleteProg(microVU& mVU, microProgram*& prog);
_mVUt extern void* mVUsearchProg(uint32_t startPC, uintptr_t pState);
extern void* mVUexecuteVU0(uint32_t startPC, uint32_t cycles);
extern void* mVUexecuteVU1(uint32_t startPC, uint32_t cycles);

/* recCall Function Pointer */
typedef void (*mVUrecCall)(uint32_t, uint32_t);
typedef void (*mVUrecCallXG)(void);
