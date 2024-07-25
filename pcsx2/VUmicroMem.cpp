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

#include <cstring> /* memset */

#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"

alignas(16) VURegs vuRegs[2]{};

/* VU0/1 on-chip memory */
vuMemoryReserve::vuMemoryReserve() : _parent() { }
vuMemoryReserve::~vuMemoryReserve() { Release(); }

void vuMemoryReserve::Assign(VirtualMemoryManagerPtr allocator)
{
	static constexpr u32 VU_MEMORY_RESERVE_SIZE = VU1_PROGSIZE + VU1_MEMSIZE + VU0_PROGSIZE + VU0_MEMSIZE;

	_parent::Assign(std::move(allocator), HostMemoryMap::VUmemOffset, VU_MEMORY_RESERVE_SIZE);

	u8* curpos = GetPtr();
	vuRegs[0].Micro	= curpos;
	curpos += VU0_PROGSIZE;
	vuRegs[0].Mem	= curpos;
	curpos += VU0_MEMSIZE;
	vuRegs[1].Micro	= curpos;
	curpos += VU1_PROGSIZE;
	vuRegs[1].Mem	= curpos;
	curpos += VU1_MEMSIZE;
}

void vuMemoryReserve::Release()
{
	_parent::Release();

	vuRegs[0].Micro = nullptr;
	vuRegs[0].Mem   = nullptr;
	vuRegs[1].Micro = nullptr;
	vuRegs[1].Mem   = nullptr;
}

void vuMemoryReserve::Reset()
{
	_parent::Reset();

	// === VU0 Initialization ===
	memset(&vuRegs[0].ACC, 0, sizeof(vuRegs[0].ACC));
	memset(vuRegs[0].VF, 0, sizeof(vuRegs[0].VF));
	memset(vuRegs[0].VI, 0, sizeof(vuRegs[0].VI));
	vuRegs[0].VF[0].f.x = 0.0f;
	vuRegs[0].VF[0].f.y = 0.0f;
	vuRegs[0].VF[0].f.z = 0.0f;
	vuRegs[0].VF[0].f.w = 1.0f;
	vuRegs[0].VI[0].UL  = 0;

	// === VU1 Initialization ===
	memset(&vuRegs[1].ACC, 0, sizeof(vuRegs[1].ACC));
	memset(vuRegs[1].VF, 0, sizeof(vuRegs[1].VF));
	memset(vuRegs[1].VI, 0, sizeof(vuRegs[1].VI));
	vuRegs[1].VF[0].f.x = 0.0f;
	vuRegs[1].VF[0].f.y = 0.0f;
	vuRegs[1].VF[0].f.z = 0.0f;
	vuRegs[1].VF[0].f.w = 1.0f;
	vuRegs[1].VI[0].UL  = 0;
}

bool SaveStateBase::vuMicroFreeze()
{
	if(IsSaving())
		vu1Thread.WaitVU();

	if (!(FreezeTag( "vuMicroRegs" )))
		return false;

	// VU0 state information

	Freeze(vuRegs[0].ACC);
	Freeze(vuRegs[0].VF);
	Freeze(vuRegs[0].VI);
	Freeze(vuRegs[0].q);

	Freeze(vuRegs[0].cycle);
	Freeze(vuRegs[0].flags);
	Freeze(vuRegs[0].code);
	Freeze(vuRegs[0].start_pc);
	Freeze(vuRegs[0].branch);
	Freeze(vuRegs[0].branchpc);
	Freeze(vuRegs[0].delaybranchpc);
	Freeze(vuRegs[0].takedelaybranch);
	Freeze(vuRegs[0].ebit);
	Freeze(vuRegs[0].pending_q);
	Freeze(vuRegs[0].pending_p);
	Freeze(vuRegs[0].micro_macflags);
	Freeze(vuRegs[0].micro_clipflags);
	Freeze(vuRegs[0].micro_statusflags);
	Freeze(vuRegs[0].macflag);
	Freeze(vuRegs[0].statusflag);
	Freeze(vuRegs[0].clipflag);
	Freeze(vuRegs[0].nextBlockCycles);
	Freeze(vuRegs[0].VIBackupCycles);
	Freeze(vuRegs[0].VIOldValue);
	Freeze(vuRegs[0].VIRegNumber);
	Freeze(vuRegs[0].fmac);
	Freeze(vuRegs[0].fmacreadpos);
	Freeze(vuRegs[0].fmacwritepos);
	Freeze(vuRegs[0].fmaccount);
	Freeze(vuRegs[0].fdiv);
	Freeze(vuRegs[0].efu);
	Freeze(vuRegs[0].ialu);
	Freeze(vuRegs[0].ialureadpos);
	Freeze(vuRegs[0].ialuwritepos);
	Freeze(vuRegs[0].ialucount);

	// VU1 state information
	Freeze(vuRegs[1].ACC);
	Freeze(vuRegs[1].VF);
	Freeze(vuRegs[1].VI);
	Freeze(vuRegs[1].q);
	Freeze(vuRegs[1].p);

	Freeze(vuRegs[1].cycle);
	Freeze(vuRegs[1].flags);
	Freeze(vuRegs[1].code);
	Freeze(vuRegs[1].start_pc);
	Freeze(vuRegs[1].branch);
	Freeze(vuRegs[1].branchpc);
	Freeze(vuRegs[1].delaybranchpc);
	Freeze(vuRegs[1].takedelaybranch);
	Freeze(vuRegs[1].ebit);
	Freeze(vuRegs[1].pending_q);
	Freeze(vuRegs[1].pending_p);
	Freeze(vuRegs[1].micro_macflags);
	Freeze(vuRegs[1].micro_clipflags);
	Freeze(vuRegs[1].micro_statusflags);
	Freeze(vuRegs[1].macflag);
	Freeze(vuRegs[1].statusflag);
	Freeze(vuRegs[1].clipflag);
	Freeze(vuRegs[1].nextBlockCycles);
	Freeze(vuRegs[1].xgkickaddr);
	Freeze(vuRegs[1].xgkickdiff);
	Freeze(vuRegs[1].xgkicksizeremaining);
	Freeze(vuRegs[1].xgkicklastcycle);
	Freeze(vuRegs[1].xgkickcyclecount);
	Freeze(vuRegs[1].xgkickenable);
	Freeze(vuRegs[1].xgkickendpacket);
	Freeze(vuRegs[1].VIBackupCycles);
	Freeze(vuRegs[1].VIOldValue);
	Freeze(vuRegs[1].VIRegNumber);
	Freeze(vuRegs[1].fmac);
	Freeze(vuRegs[1].fmacreadpos);
	Freeze(vuRegs[1].fmacwritepos);
	Freeze(vuRegs[1].fmaccount);
	Freeze(vuRegs[1].fdiv);
	Freeze(vuRegs[1].efu);
	Freeze(vuRegs[1].ialu);
	Freeze(vuRegs[1].ialureadpos);
	Freeze(vuRegs[1].ialuwritepos);
	Freeze(vuRegs[1].ialucount);

	return IsOkay();
}
