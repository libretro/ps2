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
#include <cmath>
#include <cfenv>

#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "R5900opcodeTables.h"

#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

#define _Fsf_ ((cpuRegs.code >> 21) & 0x03)
#define _Ftf_ ((cpuRegs.code >> 23) & 0x03)

BaseVUmicroCPU* CpuVU0 = nullptr;
BaseVUmicroCPU* CpuVU1 = nullptr;

InterpVU0 CpuIntVU0;
InterpVU1 CpuIntVU1;

alignas(16) VURegs vuRegs[2]{};

/* Forward declarations */
extern void _vuFlushAll(VURegs* VU);
extern void _vuXGKICKFlush(VURegs* VU);

/* VU0/1 on-chip memory */
vuMemoryReserve::vuMemoryReserve() : _parent() { }
vuMemoryReserve::~vuMemoryReserve() { Release(); }

void COP2_BC2(void) { Int_COP2BC2PrintTable[_Rt_]();}
void COP2_SPECIAL(void) { _vu0FinishMicro(); Int_COP2SPECIAL1PrintTable[_Funct_]();}

void COP2_SPECIAL2(void) {
	Int_COP2SPECIAL2PrintTable[(cpuRegs.code & 0x3) | ((cpuRegs.code >> 4) & 0x7c)]();
}

void COP2_Unknown(void) { }

//****************************************************************************

template<bool breakOnMbit, bool addCycles, bool sync_only> static __fi void _vu0run(void)
{
	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 1)) return;

	/* VU0 is ahead of the EE and M-Bit is already encountered, 
	 * so no need to wait for it, just catch up the EE */
	if ((vuRegs[0].flags & VUFLAG_MFLAGSET) && breakOnMbit && (s32)(cpuRegs.cycle - vuRegs[0].cycle) <= 0)
	{
		cpuRegs.cycle = vuRegs[0].cycle;
		return;
	}

	if(!EmuConfig.Cpu.Recompiler.EnableEE)
		intUpdateCPUCycles();

	u32 startcycle = cpuRegs.cycle;
	s32 runCycles  = 0x7fffffff;

	if (sync_only)
	{
		runCycles  = (s32)(cpuRegs.cycle - vuRegs[0].cycle);

		if (runCycles < 0)
			return;
	}

	do { /* Run VU until it finishes or M-Bit */
		CpuVU0->Execute(runCycles);
	} while ((vuRegs[0].VI[REG_VPU_STAT].UL & 1) /* E-bit Termination */
	  &&	!sync_only && (!breakOnMbit || (!(vuRegs[0].flags & VUFLAG_MFLAGSET) && (s32)(cpuRegs.cycle - vuRegs[0].cycle) > 0)));	/* M-bit Break */

	/* Add cycles if called from EE's COP2 */
	if (addCycles)
	{
		cpuRegs.cycle += (vuRegs[0].cycle - startcycle);
		CpuVU1->ExecuteBlock(false); /* Catch up VU1 as it's likely fallen behind */

		if (vuRegs[0].VI[REG_VPU_STAT].UL & 1)
			cpuSetNextEvent(cpuRegs.cycle, 4);
	}
}

void _vu0WaitMicro(void)   { _vu0run<true, true, false>(); } /* Runs VU0 Micro Until E-bit or M-Bit End */
void _vu0FinishMicro(void) { _vu0run<false, true, false>(); } /* Runs VU0 Micro Until E-Bit End */
void vu0Finish(void)	   { _vu0run<false, false, false>(); } /* Runs VU0 Micro Until E-Bit End (doesn't stall EE) */

namespace R5900 {
namespace Interpreter{
namespace OpcodeImpl
{
	void LQC2(void)
	{
		_vu0run<false, false, true>();
		u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + (s16)cpuRegs.code;
		if (_Ft_)
		{
			memRead128(addr, &vuRegs[0].VF[_Ft_].UQ);
		}
		else
		{
			u128 val;
 			memRead128(addr, &val);
		}
	}

	/* Asadr.Changed
	 * TODO: check this
	 * HUH why ? doesn't make any sense ... */
	void SQC2(void)
	{
		_vu0run<false, false, true>();
		u32 addr = _Imm_ + cpuRegs.GPR.r[_Rs_].UL[0];
		memWrite128(addr, &vuRegs[0].VF[_Ft_].UQ);
	}
}}}


void QMFC2(void)
{
	_vu0run<false, false, true>();

	if (cpuRegs.code & 1)
	{
		_vu0FinishMicro();
	}

	if (_Rt_ != 0)
	{
		cpuRegs.GPR.r[_Rt_].UD[0] = vuRegs[0].VF[_Fs_].UD[0];
		cpuRegs.GPR.r[_Rt_].UD[1] = vuRegs[0].VF[_Fs_].UD[1];
	}
}

void QMTC2(void)
{
	_vu0run<false, false, true>();
	if (cpuRegs.code & 1)
	{
		_vu0WaitMicro();
	}

	if (_Fs_ != 0)
	{
		vuRegs[0].VF[_Fs_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0];
		vuRegs[0].VF[_Fs_].UD[1] = cpuRegs.GPR.r[_Rt_].UD[1];
	}
}

void CFC2(void)
{
	_vu0run<false, false, true>();
	if (cpuRegs.code & 1)
	{
		_vu0FinishMicro();
	}

	if (_Rt_ == 0) return;

	if (_Fs_ == REG_R)
		cpuRegs.GPR.r[_Rt_].UL[0] = vuRegs[0].VI[REG_R].UL & 0x7FFFFF;
	else
	{
		cpuRegs.GPR.r[_Rt_].UL[0] = vuRegs[0].VI[_Fs_].UL;

		if (vuRegs[0].VI[_Fs_].UL & 0x80000000)
			cpuRegs.GPR.r[_Rt_].UL[1] = 0xffffffff;
		else
			cpuRegs.GPR.r[_Rt_].UL[1] = 0;
	}

}

void CTC2(void)
{
	_vu0run<false, false, true>();

	if (cpuRegs.code & 1) {
		_vu0WaitMicro();
	}

	if (_Fs_ == 0)
		return;

	switch(_Fs_)
	{
		case REG_MAC_FLAG: /* read-only */
		case REG_TPC:      /* read-only */
		case REG_VPU_STAT: /* read-only */
			break;
		case REG_R:
			vuRegs[0].VI[REG_R].UL = ((cpuRegs.GPR.r[_Rt_].UL[0] & 0x7FFFFF) | 0x3F800000);
			break;
		case REG_FBRST:
			vuRegs[0].VI[REG_FBRST].UL = cpuRegs.GPR.r[_Rt_].UL[0] & 0x0C0C;
			if (cpuRegs.GPR.r[_Rt_].UL[0] & 0x2)   /* VU0 Reset */
				vu0ResetRegs();
			if (cpuRegs.GPR.r[_Rt_].UL[0] & 0x200) /* VU1 Reset */
				vu1ResetRegs();
			break;
		case REG_CMSAR1: /* REG_CMSAR1 */
			vu1Finish(true);
			vu1ExecMicro(cpuRegs.GPR.r[_Rt_].US[0]); /* Execute VU1 Micro SubRoutine */
			break;
		default:
			vuRegs[0].VI[_Fs_].UL = cpuRegs.GPR.r[_Rt_].UL[0];
			break;
	}
}

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

static void _vu0ExecUpper(VURegs* VU, u32* ptr)
{
	VU->code = ptr[1];
	VU0_UPPER_OPCODE[VU->code & 0x3f]();
}

static void _vu0ExecLower(VURegs* VU, u32* ptr)
{
	VU->code = ptr[0];
	VU0_LOWER_OPCODE[VU->code >> 25]();
}

static void _vu0Exec(VURegs* VU)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	u32* ptr = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL += 8;

	if (ptr[1] & 0x40000000) /* E flag */
		VU->ebit = 2;
	if (ptr[1] & 0x20000000 && VU == &vuRegs[0]) /* M flag */
		VU->flags |= VUFLAG_MFLAGSET;
	if (ptr[1] & 0x10000000) /* D flag */
	{
		if (vuRegs[0].VI[REG_FBRST].UL & 0x4)
		{
			vuRegs[0].VI[REG_VPU_STAT].UL |= 0x2;
			hwIntcIrq(INTC_VU0);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) /* T flag */
	{
		if (vuRegs[0].VI[REG_FBRST].UL & 0x8)
		{
			vuRegs[0].VI[REG_VPU_STAT].UL |= 0x4;
			hwIntcIrq(INTC_VU0);
			VU->ebit = 1;
		}
	}

	VU->code = ptr[1];
	VU0regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);

	u32 cyclesBeforeOp = vuRegs[0].cycle - 1;

	_vuTestUpperStalls(VU, &uregs);

	/* check upper flags */
	if (ptr[1] & 0x80000000) /* I flag */
	{
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(vuRegs[0].cycle - cyclesBeforeOp), VU->VIBackupCycles);

		_vu0ExecUpper(VU, ptr);

		VU->VI[REG_I].UL = ptr[0];
		memset(&lregs, 0, sizeof(lregs));
	}
	else
	{
		VECTOR _VF;
		VECTOR _VFc;
		REG_VI _VI;
		REG_VI _VIc;
		int vfreg = 0;
		int vireg = 0;
		int discard = 0;

		VU->code = ptr[0];
		lregs.cycles = 0;
		VU0regs_LOWER_OPCODE[VU->code >> 25](&lregs);
		_vuTestLowerStalls(VU, &lregs);

		_vuTestPipes(VU);
		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(vuRegs[0].cycle - cyclesBeforeOp), VU->VIBackupCycles);

		if (uregs.VFwrite)
		{
			if (lregs.VFwrite == uregs.VFwrite)
				discard = 1;
			if (       lregs.VFread0 == uregs.VFwrite
				|| lregs.VFread1 == uregs.VFwrite)
			{
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIread & (1 << REG_CLIP_FLAG))
		{
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
				discard = 1;
			if (lregs.VIread & (1 << REG_CLIP_FLAG))
			{
				_VI   = vuRegs[0].VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		_vu0ExecUpper(VU, ptr);

		if (discard == 0)
		{
			if (vfreg)
			{
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg)
			{
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			_vu0ExecLower(VU, ptr);

			if (vfreg)
				VU->VF[vfreg] = _VFc;
			if (vireg)
				VU->VI[vireg] = _VIc;
		}
	}

	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		_vuClearFMAC(VU);

	_vuAddUpperStalls(VU, &uregs);
	_vuAddLowerStalls(VU, &lregs);

	if (VU->branch > 0)
	{
		if (VU->branch-- == 1)
		{
			VU->VI[REG_TPC].UL = VU->branchpc;

			if (VU->takedelaybranch)
			{
				VU->branch = 1;
				VU->branchpc = VU->delaybranchpc;
				VU->takedelaybranch = false;
			}
		}
	}

	if (VU->ebit > 0)
	{
		if (VU->ebit-- == 1)
		{
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x1; /* E flag */
			vif0Regs.stat.VEW = false;
		}
	}

	/* Progress the write position of the FMAC pipeline by one place */
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;
}

void vu0Exec(VURegs* VU)
{
	vuRegs[0].VI[REG_TPC].UL &= VU0_PROGMASK;
	VU->cycle++;
	_vu0Exec(VU);
}

/* --------------------------------------------------------------------------------------
 *  VU0microInterpreter
 * --------------------------------------------------------------------------------------
 */

InterpVU0::InterpVU0()
{
	m_Idx = 0;
	IsInterpreter = true;
}

void InterpVU0::Reset()
{
	vuRegs[0].fmacwritepos = 0;
	vuRegs[0].fmacreadpos = 0;
	vuRegs[0].fmaccount = 0;
	vuRegs[0].ialuwritepos = 0;
	vuRegs[0].ialureadpos = 0;
	vuRegs[0].ialucount = 0;
}

void InterpVU0::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);

	vuRegs[0].VI[REG_TPC].UL <<= 3;
	vuRegs[0].flags &= ~VUFLAG_MFLAGSET;
	u32 startcycles  = vuRegs[0].cycle;
	while ((vuRegs[0].cycle - startcycles) < cycles)
	{
		if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x1))
		{
			/* Branches advance the PC to the new location if there was a branch in the E-Bit delay slot */
			if (vuRegs[0].branch)
			{
				vuRegs[0].VI[REG_TPC].UL = vuRegs[0].branchpc;
				vuRegs[0].branch = 0;
			}
			break;
		}
		if (vuRegs[0].flags & VUFLAG_MFLAGSET)
			break;

		vu0Exec(&vuRegs[0]);
	}

	vuRegs[0].VI[REG_TPC].UL >>= 3;

	if (EmuConfig.Speedhacks.EECycleRate != 0 && (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
	{
		u32 cycle_change = vuRegs[0].cycle - startcycles;
		vuRegs[0].cycle -= cycle_change;
		switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<int>(cycle_change)))
		{
			case -3: /* 50% */
				cycle_change *= 2.0f;
				break;
			case -2: /* 60% */
				cycle_change *= 1.6666667f;
				break;
			case -1: /* 75% */
				cycle_change *= 1.3333333f;
				break;
			case 1: /* 130% */
				cycle_change /= 1.3f;
				break;
			case 2: /* 180% */
				cycle_change /= 1.8f;
				break;
			case 3: /* 300% */
				cycle_change /= 3.0f;
				break;
			default:
				break;
		}
		vuRegs[0].cycle += cycle_change;
	}

	vuRegs[0].nextBlockCycles = (vuRegs[0].cycle - cpuRegs.cycle) + 1;
}

static void _vu1Exec(VURegs* VU)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	u32 *ptr = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL += 8;

	if (ptr[1] & 0x40000000) /* E flag */
		VU->ebit = 2;
	if (ptr[1] & 0x10000000) /* D flag */
	{
		if (vuRegs[0].VI[REG_FBRST].UL & 0x400)
		{
			vuRegs[0].VI[REG_VPU_STAT].UL |= 0x200;
			hwIntcIrq(INTC_VU1);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) /* T flag */
	{
		if (vuRegs[0].VI[REG_FBRST].UL & 0x800)
		{
			vuRegs[0].VI[REG_VPU_STAT].UL |= 0x400;
			hwIntcIrq(INTC_VU1);
			VU->ebit = 1;
		}
	}

	VU->code = ptr[1];
	VU1regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);

	u32 cyclesBeforeOp = vuRegs[1].cycle-1;

	_vuTestUpperStalls(VU, &uregs);

	/* check upper flags */
	if (ptr[1] & 0x80000000) /* I Flag (Lower op is a float) */
	{
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(vuRegs[1].cycle - cyclesBeforeOp), VU->VIBackupCycles);

		VU->code         = ptr[1];
		VU1_UPPER_OPCODE[VU->code & 0x3f]();

		VU->VI[REG_I].UL = ptr[0];
		/* Lower not used, set to 0 to fill in the FMAC stall gap
		 * Could probably get away with just running upper stalls, but lets not tempt fate. */
		memset(&lregs, 0, sizeof(lregs));
	}
	else
	{
		VECTOR _VF;
		VECTOR _VFc;
		REG_VI _VI;
		REG_VI _VIc;
		int vfreg = 0;
		int vireg = 0;
		int discard = 0;

		VU->code = ptr[0];
		lregs.cycles = 0;
		VU1regs_LOWER_OPCODE[VU->code >> 25](&lregs);

		_vuTestLowerStalls(VU, &lregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles-= std::min((u8)(vuRegs[1].cycle- cyclesBeforeOp), VU->VIBackupCycles);

		if (uregs.VFwrite)
		{
			if (lregs.VFwrite == uregs.VFwrite)
				discard = 1;
			if (       lregs.VFread0 == uregs.VFwrite
				|| lregs.VFread1 == uregs.VFwrite)
			{
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIwrite & (1 << REG_CLIP_FLAG))
		{
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
				discard = 1;
			if (lregs.VIread & (1 << REG_CLIP_FLAG))
			{
				_VI = VU->VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		VU->code         = ptr[1];
		VU1_UPPER_OPCODE[VU->code & 0x3f]();

		if (discard == 0)
		{
			if (vfreg)
			{
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg)
			{
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			VU->code = ptr[0];
			VU1_LOWER_OPCODE[VU->code >> 25]();

			if (vfreg)
				VU->VF[vfreg] = _VFc;
			if (vireg)
				VU->VI[vireg] = _VIc;
		}
	}

	/* Clear an FMAC read for use */
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		_vuClearFMAC(VU);

	_vuAddUpperStalls(VU, &uregs);
	_vuAddLowerStalls(VU, &lregs);

	if (VU->branch > 0)
	{
		if (VU->branch-- == 1)
		{
			VU->VI[REG_TPC].UL = VU->branchpc;

			if (VU->takedelaybranch)
			{
				VU->branch = 1;
				VU->branchpc = VU->delaybranchpc;
				VU->takedelaybranch = false;
			}
		}
	}

	if (VU->ebit > 0)
	{
		if (VU->ebit-- == 1)
		{
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100;
			vif1Regs.stat.VEW = false;

			if(vuRegs[1].xgkickenable)
				_vuXGKICKTransfer(0, true);
			// In instant VU mode, VU1 goes WAY ahead of the CPU, 
			// making the XGKick fall way behind
			// We also have some code to update it in VIF Unpacks too, 
			// since in some games (Aggressive Inline) overwrite the XGKick data
			// VU currently flushes XGKICK on end, so this isn't needed, yet
			if (INSTANT_VU1)
				vuRegs[1].xgkicklastcycle = cpuRegs.cycle;
		}
	}

	// Progress the write position of the FMAC pipeline by one place
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;
}

InterpVU1::InterpVU1()
{
	m_Idx = 1;
	IsInterpreter = true;
}

void InterpVU1::Reset()
{
	vuRegs[1].fmacwritepos = 0;
	vuRegs[1].fmacreadpos = 0;
	vuRegs[1].fmaccount = 0;
	vuRegs[1].ialuwritepos = 0;
	vuRegs[1].ialureadpos = 0;
	vuRegs[1].ialucount = 0;
}

void InterpVU1::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR);

	vuRegs[1].VI[REG_TPC].UL <<= 3;
	u32 startcycles = vuRegs[1].cycle;

	while ((vuRegs[1].cycle - startcycles) < cycles)
	{
		if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
		{
			if (vuRegs[1].branch == 1)
			{
				vuRegs[1].VI[REG_TPC].UL = vuRegs[1].branchpc;
				vuRegs[1].branch = 0;
			}
			break;
		}
		vuRegs[1].VI[REG_TPC].UL &= VU1_PROGMASK;
		vuRegs[1].cycle++;
		_vu1Exec(&vuRegs[1]);
	}
	vuRegs[1].VI[REG_TPC].UL >>= 3;
	vuRegs[1].nextBlockCycles = (vuRegs[1].cycle - cpuRegs.cycle) + 1;
}

/* This is called by the COP2 as per the CTC instruction */
void vu0ResetRegs(void)
{
	vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xff; /* stop VU0 */
	vuRegs[0].VI[REG_FBRST].UL    &= ~0xff; /* stop VU0 */
	vif0Regs.stat.VEW              = false;
}

/* from mVUallocSFLAGd() */
#define vu0DenormalizeMicroStatus(nstatus) (((nstatus >> 3) & 0x18u) | ((nstatus >> 11) & 0x1800u) | ((nstatus >> 14) & 0x3cf0000u))

#if 1
#define vu0SetMicroFlags(flags, value) r128_store((flags), r128_from_u32_dup((value)))
#else
/* C fallback version */
#define vu0SetMicroFlags(flags, value) (flags)[0] = (flags)[1] = (flags)[2] = (flags)[3] = (value)
#endif

void vu0ExecMicro(u32 addr)
{
	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x1)
		vu0Finish();

	/* Need to copy the clip flag back to the interpreter in case COP2 has edited it */
	const u32 CLIP       = vuRegs[0].VI[REG_CLIP_FLAG].UL;
	const u32 MAC        = vuRegs[0].VI[REG_MAC_FLAG].UL;
	const u32 STATUS     = vuRegs[0].VI[REG_STATUS_FLAG].UL;
	vuRegs[0].clipflag   = CLIP;
	vuRegs[0].macflag    = MAC;
	vuRegs[0].statusflag = STATUS;

	/* Copy flags to micro instances, since they may be out of sync if COP2 has run.
	 * We do this at program start time, because COP2 can't execute until the program has completed,
	 * but long-running program may be interrupted so we can't do it at dispatch time. */
	vu0SetMicroFlags(vuRegs[0].micro_clipflags, CLIP);
	vu0SetMicroFlags(vuRegs[0].micro_macflags, MAC);
	vu0SetMicroFlags(vuRegs[0].micro_statusflags, vu0DenormalizeMicroStatus(STATUS));

	vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF;
	vuRegs[0].VI[REG_VPU_STAT].UL |=  0x01;
	vuRegs[0].cycle                = cpuRegs.cycle;
	if ((s32)addr != -1)
		vuRegs[0].VI[REG_TPC].UL = addr & 0x1FF;

	vuRegs[0].start_pc = vuRegs[0].VI[REG_TPC].UL << 3;
	CpuVU0->ExecuteBlock(true);
}

/* This is called by the COP2 as per the CTC instruction */
void vu1ResetRegs(void)
{
	vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xff00; // stop vu1
	vuRegs[0].VI[REG_FBRST].UL    &= ~0xff00; // stop vu1
	vif1Regs.stat.VEW              = false;
}

void vu1Finish(bool add_cycles)
{
	if (THREAD_VU1)
	{
		if (INSTANT_VU1 || add_cycles)
			vu1Thread.WaitVU();
		vu1Thread.Get_MTVUChanges();
		return;
	}
	u32 vu1cycles = vuRegs[1].cycle;
	if(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
		CpuVU1->Execute(vu1RunCycles);
	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100;
	if (add_cycles)
		cpuRegs.cycle += vuRegs[1].cycle - vu1cycles;
}

void vu1ExecMicro(u32 addr)
{
	if (THREAD_VU1)
	{
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00;
		// Okay this is a little bit of a hack, but with good reason.
		// Most of the time with MTVU we want to pretend the VU has finished quickly as to gain the benefit from running another thread
		// however with T-Bit games when the T-Bit is enabled, it needs to wait in case a T-Bit happens, so we need to set "Busy"
		// We shouldn't do this all the time as it negates the extra thread and causes games like Ratchet & Clank to be no faster.
		// if (vuRegs[0].VI[REG_FBRST].UL & 0x800)
		// {
		//	vuRegs[0].VI[REG_VPU_STAT].UL |= 0x0100;
		// }
		// Update 25/06/2022: Disabled this for now, let games YOLO it, if it breaks MTVU, disable MTVU (it doesn't work properly anyway) - Refraction
		vu1Thread.ExecuteVU(addr, vif1Regs.top, vif1Regs.itop, vuRegs[0].VI[REG_FBRST].UL);
		return;
	}
	static int count = 0;
	vu1Finish(false);

	vuRegs[1].cycle = cpuRegs.cycle;
	vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00;
	vuRegs[0].VI[REG_VPU_STAT].UL |=  0x0100;
	if ((s32)addr != -1) vuRegs[1].VI[REG_TPC].UL = addr & 0x7FF;

	vuRegs[1].start_pc = vuRegs[1].VI[REG_TPC].UL << 3;
	if(!INSTANT_VU1)
		CpuVU1->ExecuteBlock(true);
	else
		CpuVU1->Execute(vu1RunCycles);
}

__inline uint32_t CalculateMinRunCycles(uint32_t cycles, bool requiresAccurateCycles)
{
	// If we're running an interlocked COP2 operation
	// run for an exact amount of cycles
	if(requiresAccurateCycles)
		return cycles;
	// Allow a minimum of 16 cycles to avoid running small blocks
	// Running a block of like 3 cycles is highly inefficient
	// so while sync isn't tight, it's okay to run ahead a little bit.
	return std::max(16U, cycles);
}

// Executes a Block based on EE delta time
void BaseVUmicroCPU::ExecuteBlock(bool startup)
{
	const uint32_t& stat = vuRegs[0].VI[REG_VPU_STAT].UL;
	const int test       = m_Idx ? 0x100 : 1;

	if (m_Idx && THREAD_VU1)
	{
		vu1Thread.Get_MTVUChanges();
		return;
	}

	if (!(stat & test))
		return;

	if (startup)
		Execute(16U);
	else /* Continue Executing */
	{
		uint32_t cycle = m_Idx ? vuRegs[1].cycle : vuRegs[0].cycle;
		int32_t delta  = (int32_t)(uint32_t)(cpuRegs.cycle - cycle);

		if (delta > 0)
			Execute(std::max(16, delta));
	}
}

// This function is called by VU0 Macro (COP2) after transferring some
// EE data to VU0's registers. We want to run VU0 Micro right after this
// to ensure that the register is used at the correct time.
// This fixes spinning/hanging in some games like Ratchet and Clank's Intro.
void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU* cpu, bool interlocked)
{
	const u32& stat = vuRegs[0].VI[REG_VPU_STAT].UL;
	constexpr int test = 1;

	if (stat & test) // VU is running
	{ 
		s32 delta = (s32)(u32)(cpuRegs.cycle - vuRegs[0].cycle);
		if (delta > 0)
			cpu->Execute(CalculateMinRunCycles(delta, interlocked)); // Execute the time since the last call
	}
}
