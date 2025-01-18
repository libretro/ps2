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

// Micro VU recompiler! - author: cottonvibes(@gmail.com)

#include <cstring> /* memset */

#include <cpuinfo.h>

#include "microVU.h"

#include "../../common/AlignedMalloc.h"

//------------------------------------------------------------------
// Micro VU - Main Functions
//------------------------------------------------------------------
alignas(__pagesize) static u8 vu0_RecDispatchers[mVUdispCacheSize];
alignas(__pagesize) static u8 vu1_RecDispatchers[mVUdispCacheSize];

static void mVUreserveCache(microVU& mVU)
{
	/* Micro VU Recompiler Cache */
	mVU.cache_reserve = new RecompiledCodeReserve();

	const size_t alloc_offset = mVU.index ? HostMemoryMap::mVU0recOffset : HostMemoryMap::mVU1recOffset;
	mVU.cache_reserve->Assign(GetVmMemory().CodeMemory(), alloc_offset, mVU.cacheSize * _1mb);
	mVU.cache = mVU.cache_reserve->GetPtr();
}

// Only run this once per VU! ;)
void mVUinit(microVU& mVU, uint vuIndex)
{
	memset(&mVU.prog, 0, sizeof(mVU.prog));

	mVU.index        =  vuIndex;
	mVU.cop2         =  0;
	mVU.vuMemSize    = (mVU.index ? 0x4000 : 0x1000);
	mVU.microMemSize = (mVU.index ? 0x4000 : 0x1000);
	mVU.progSize     = (mVU.index ? 0x4000 : 0x1000) / 4;
	mVU.progMemMask  =  mVU.progSize-1;
	mVU.cacheSize    =  mVUcacheReserve;
	mVU.cache        = NULL;
	mVU.dispCache    = NULL;
	mVU.startFunct   = NULL;
	mVU.exitFunct    = NULL;

	mVUreserveCache(mVU);

	if (vuIndex)
		mVU.dispCache = vu1_RecDispatchers;
	else
		mVU.dispCache = vu0_RecDispatchers;

	mVU.regAlloc.reset(new microRegAlloc(mVU.index));
}

// Resets Rec Data
void mVUreset(microVU& mVU, bool resetReserve)
{
	PageProtectionMode mode;
	if (THREAD_VU1)
	{
		// If MTVU is toggled on during gameplay we need 
		// to flush the running VU1 program, else it gets in a mess
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
			CpuVU1->Execute(vu1RunCycles);
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100;
	}
	// Restore reserve to uncommitted state
	if (resetReserve)
		mVU.cache_reserve->Reset();

	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	HostSys::MemProtect(mVU.dispCache, mVUdispCacheSize, mode);
	memset(mVU.dispCache, 0xcc, mVUdispCacheSize);

	xSetPtr(mVU.dispCache);
	mVUdispatcherAB(mVU);
	mVUdispatcherCD(mVU);
	mVUGenerateWaitMTVU(mVU);
	mVUGenerateCopyPipelineState(mVU);
	mVUGenerateCompareState(mVU);

	vuRegs[mVU.index].nextBlockCycles = 0;
	memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));

	// Program Variables
	mVU.prog.cleared  =  1;
	mVU.prog.isSame   = -1;
	mVU.prog.cur      = NULL;
	mVU.prog.total    =  0;
	mVU.prog.curFrame =  0;

	// Setup Dynarec Cache Limits for Each Program
	u8* z = mVU.cache;
	mVU.prog.x86start = z;
	mVU.prog.x86ptr   = z;
	mVU.prog.x86end   = z + ((mVU.cacheSize - mVUcacheSafeZone) * _1mb);

	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
		{
			mVU.prog.prog[i] = new std::deque<microProgram*>();
			continue;
		}
		std::deque<microProgram*>::iterator it(mVU.prog.prog[i]->begin());
		for (; it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		mVU.prog.prog[i]->clear();
		mVU.prog.quick[i].block = NULL;
		mVU.prog.quick[i].prog = NULL;
	}

	mode.m_write = false;
	mode.m_exec  = true;
	HostSys::MemProtect(mVU.dispCache, mVUdispCacheSize, mode);
}

// Free Allocated Resources
void mVUclose(microVU& mVU)
{

	delete mVU.cache_reserve;
	mVU.cache_reserve = NULL;

	// Delete Programs and Block Managers
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
			continue;
		std::deque<microProgram*>::iterator it(mVU.prog.prog[i]->begin());
		for (; it != mVU.prog.prog[i]->end(); ++it)
			mVUdeleteProg(mVU, it[0]);
		delete mVU.prog.prog[i];
		mVU.prog.prog[i] = NULL;
	}
}

// Clears Block Data in specified range
__fi void mVUclear(mV, u32 addr, u32 size)
{
	if (!mVU.prog.cleared)
	{
		mVU.prog.cleared = 1; // Next execution searches/creates a new microprogram
		memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState)); // Clear pipeline state
		for (u32 i = 0; i < (mVU.progSize / 2); i++)
		{
			mVU.prog.quick[i].block = NULL; // Clear current quick-reference block
			mVU.prog.quick[i].prog = NULL; // Clear current quick-reference prog
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Private Functions
//------------------------------------------------------------------

// Deletes a program
__ri void mVUdeleteProg(microVU& mVU, microProgram*& prog)
{
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		delete prog->block[i];
		prog->block[i] = NULL;
	}
	delete prog->ranges;
	prog->ranges = NULL;
	safe_aligned_free(prog);
}

// Creates a new Micro Program
__ri microProgram* mVUcreateProg(microVU& mVU, int startPC)
{
	microProgram* prog = (microProgram*)_aligned_malloc(sizeof(microProgram), 64);
	memset(prog, 0, sizeof(microProgram));
	prog->idx = mVU.prog.total++;
	prog->ranges = new std::deque<microRange>();
	prog->startPC = startPC;
	if(doWholeProgCompare)
		mVUcacheProg(mVU, *prog); // Cache Micro Program
	return prog;
}

// Caches Micro Program
__ri void mVUcacheProg(microVU& mVU, microProgram& prog)
{
	if (!doWholeProgCompare)
	{
		auto cmpOffset = [&](void* x) { return (u8*)x + mVUrange.start; };
		memcpy(cmpOffset(prog.data), cmpOffset(vuRegs[mVU.index].Micro), (mVUrange.end - mVUrange.start));
	}
	else
	{
		if (!mVU.index)
			memcpy(prog.data, vuRegs[mVU.index].Micro, 0x1000);
		else
			memcpy(prog.data, vuRegs[mVU.index].Micro, 0x4000);
	}
}

// Generate Hash for partial program based on compiled ranges...
u64 mVUrangesHash(microVU& mVU, microProgram& prog)
{
	union
	{
		u64 v64;
		u32 v32[2];
	} hash = {0};

	std::deque<microRange>::const_iterator it(prog.ranges->begin());
	for (; it != prog.ranges->end(); ++it)
	{
		for (int i = it[0].start / 4; i < it[0].end / 4; i++)
		{
			hash.v32[0] -= prog.data[i];
			hash.v32[1] ^= prog.data[i];
		}
	}
	return hash.v64;
}

// Compare Cached microProgram to vuRegs[mVU.index].Micro
__fi bool mVUcmpProg(microVU& mVU, microProgram& prog)
{
	if (doWholeProgCompare)
	{
		if (memcmp((u8*)prog.data, vuRegs[mVU.index].Micro, mVU.microMemSize))
			return false;
	}
	else
	{
		for (const auto& range : *prog.ranges)
		{
			auto cmpOffset = [&](void* x) { return (u8*)x + range.start; };

			if (memcmp(cmpOffset(prog.data), cmpOffset(vuRegs[mVU.index].Micro), (range.end - range.start)))
				return false;
		}
	}
	mVU.prog.cleared = 0;
	mVU.prog.cur = &prog;
	mVU.prog.isSame = doWholeProgCompare ? 1 : -1;
	return true;
}

// Searches for Cached Micro Program and sets prog.cur to it (returns entry-point to program)
_mVUt __fi void* mVUsearchProg(u32 startPC, uptr pState)
{
	microVU& mVU = mVUx;
	microProgramQuick& quick = mVU.prog.quick[vuRegs[mVU.index].start_pc / 8];
	microProgramList*  list  = mVU.prog.prog [vuRegs[mVU.index].start_pc / 8];

	if (!quick.prog) // If null, we need to search for new program
	{
		std::deque<microProgram*>::iterator it(list->begin());
		for (; it != list->end(); ++it)
		{
			bool b = mVUcmpProg(mVU, *it[0]);

			if (b)
			{
				quick.block = it[0]->block[startPC / 8];
				quick.prog  = it[0];
				list->erase(it);
				list->push_front(quick.prog);

				// Sanity check, in case for some reason the program compilation aborted half way through (JALR for example)
				if (quick.block == nullptr)
				{
					void* entryPoint = mVUblockFetch(mVU, startPC, pState);
					return entryPoint;
				}
				return mVUentryGet(mVU, quick.block, startPC, pState);
			}
		}

		// If cleared and program not found, make a new program instance
		mVU.prog.cleared = 0;
		mVU.prog.isSame  = 1;
		mVU.prog.cur     = mVUcreateProg(mVU, vuRegs[mVU.index].start_pc/8);
		void* entryPoint = mVUblockFetch(mVU,  startPC, pState);
		quick.block      = mVU.prog.cur->block[startPC/8];
		quick.prog       = mVU.prog.cur;
		list->push_front(mVU.prog.cur);
		return entryPoint;
	}

	// If list.quick, then we've already found and recompiled the program ;)
	mVU.prog.isSame = -1;
	mVU.prog.cur = quick.prog;
	// Because the VU's can now run in sections and not whole programs at once
	// we need to set the current block so it gets the right program back
	quick.block = mVU.prog.cur->block[startPC / 8];

	// Sanity check, in case for some reason the program compilation aborted half way through
	if (quick.block == nullptr)
	{
		void* entryPoint = mVUblockFetch(mVU, startPC, pState);
		return entryPoint;
	}
	return mVUentryGet(mVU, quick.block, startPC, pState);
}

//------------------------------------------------------------------
// recMicroVU0 / recMicroVU1
//------------------------------------------------------------------

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;

recMicroVU0::recMicroVU0() { m_Idx = 0; IsInterpreter = false; }
recMicroVU1::recMicroVU1() { m_Idx = 1; IsInterpreter = false; }

void recMicroVU0::Reserve()
{
	mVUinit(microVU0, 0);
}
void recMicroVU1::Reserve()
{
	mVUinit(microVU1, 1);
	vu1Thread.Open();
}

void recMicroVU0::Shutdown()
{
	mVUclose(microVU0);
}
void recMicroVU1::Shutdown()
{
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	mVUclose(microVU1);
}

void recMicroVU0::Reset()
{
	mVUreset(microVU0, true);
}

void recMicroVU1::Reset()
{
	vu1Thread.WaitVU();
	vu1Thread.Get_MTVUChanges();
	mVUreset(microVU1, true);
}

void recMicroVU0::SetStartPC(u32 startPC)
{
	vuRegs[0].start_pc = startPC;
}

void recMicroVU0::Execute(u32 cycles)
{
	vuRegs[0].flags &= ~VUFLAG_MFLAGSET;

	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 1))
		return;
	vuRegs[0].VI[REG_TPC].UL <<= 3;

	((mVUrecCall)microVU0.startFunct)(vuRegs[0].VI[REG_TPC].UL, cycles);
	vuRegs[0].VI[REG_TPC].UL >>= 3;
	if (vuRegs[microVU0.index].flags & 0x4)
	{
		vuRegs[microVU0.index].flags &= ~0x4;
		hwIntcIrq(6);
	}
}

void recMicroVU1::SetStartPC(u32 startPC)
{
	vuRegs[1].start_pc = startPC;
}

void recMicroVU1::Execute(u32 cycles)
{
	if (!THREAD_VU1)
	{
		if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			return;
	}
	vuRegs[1].VI[REG_TPC].UL <<= 3;
	((mVUrecCall)microVU1.startFunct)(vuRegs[1].VI[REG_TPC].UL, cycles);
	vuRegs[1].VI[REG_TPC].UL >>= 3;
	if (vuRegs[microVU1.index].flags & 0x4 && !THREAD_VU1)
	{
		vuRegs[microVU1.index].flags &= ~0x4;
		hwIntcIrq(7);
	}
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
	mVUclear(microVU0, addr, size);
}
void recMicroVU1::Clear(u32 addr, u32 size)
{
	mVUclear(microVU1, addr, size);
}

void recMicroVU1::ResumeXGkick()
{
	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
		return;
	((mVUrecCallXG)microVU1.startFunctXG)();
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Freeze(microVU0.prog.lpState);
	Freeze(microVU1.prog.lpState);

	return IsOkay();
}
