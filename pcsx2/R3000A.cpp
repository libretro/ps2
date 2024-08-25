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

#include "R3000A.h"
#include "Common.h"

#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

#include "Config.h"
#include "Sio.h"
#include "Sif.h"
#include "R5900OpcodeTables.h"
#include "IopCounters.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopGte.h"
#include "IopMem.h"
#include "VMManager.h"

/* Note: Branch instructions of the Interpreter are defined externally because
 * the recompiler shouldn't be using them (it isn't entirely safe, due to the
 * delay slot and event handling differences between recs and ints) */

R3000Acpu *psxCpu;

/* used for constant propagation */
uint32_t g_psxConstRegs[32];
uint32_t g_psxHasConstReg;
uint32_t g_psxFlushedConstReg;

// Used to signal to the EE when important actions that need IOP-attention have
// happened (hsyncs, vsyncs, IOP exceptions, etc).  IOP runs code whenever this
// is true, even if it's already running ahead a bit.
bool iopEventAction = false;

static constexpr uint iopWaitCycles = 384; // Keep inline with EE wait cycle max.

static bool iopEventTestIsActive = false;

alignas(16) psxRegisters psxRegs;

/* Used to flag delay slot instructions when throwing exceptions. */
static bool iopIsDelaySlot = false;

static bool branch2 = 0;
static uint32_t branchPC;

void psxReset(void)
{
	memset(&psxRegs, 0, sizeof(psxRegs));

	psxRegs.pc = 0xbfc00000; // Start in bootstrap
	psxRegs.CP0.n.Status = 0x00400000; // BEV = 1
	psxRegs.CP0.n.PRid   = 0x0000001f; // PRevID = Revision ID, same as the IOP R3000A

	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = -1;
	psxRegs.iopNextEventCycle = psxRegs.cycle + 4;

	psxHwReset();
	PSXCLK = 36864000;
	R3000A::ioman::reset();
	psxBiosReset();
}

void psxException(uint32_t code, uint32_t bd)
{
	// Set the Cause
	psxRegs.CP0.n.Cause &= ~0x7f;
	psxRegs.CP0.n.Cause |= code;

	// Set the EPC & PC
	if (bd)
	{
		psxRegs.CP0.n.Cause|= 0x80000000;
		psxRegs.CP0.n.EPC = (psxRegs.pc - 4);
	}
	else
		psxRegs.CP0.n.EPC = (psxRegs.pc);

	if (psxRegs.CP0.n.Status & 0x400000)
		psxRegs.pc = 0xbfc00180;
	else
		psxRegs.pc = 0x80000080;

	// Set the Status
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status &~0x3f) |
		((psxRegs.CP0.n.Status & 0xf) << 2);
}

__fi void psxSetNextBranch(uint32_t startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(psxRegs.iopNextEventCycle - startCycle) > delta )
		psxRegs.iopNextEventCycle = startCycle + delta;
}

__fi void psxSetNextBranchDelta( s32 delta )
{
	psxSetNextBranch( psxRegs.cycle, delta );
}

static __fi int psxTestCycle(uint32_t startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(psxRegs.cycle - startCycle) >= delta;
}

__fi int psxRemainingCycles(IopEventId n)
{
	if (psxRegs.interrupt & (1 << n))
		return ((psxRegs.cycle - psxRegs.sCycle[n]) + psxRegs.eCycle[n]);
	return 0;
}

__fi void PSX_INT( IopEventId n, s32 ecycle )
{
	psxRegs.interrupt |= 1 << n;

	psxRegs.sCycle[n] = psxRegs.cycle;
	psxRegs.eCycle[n] = ecycle;

	psxSetNextBranchDelta(ecycle);

	const s32 iopDelta = (psxRegs.iopNextEventCycle - psxRegs.cycle) * 8;

	if (psxRegs.iopCycleEE < iopDelta)
	{
		// The EE called this int, so inform it to branch as needed:
		cpuSetNextEvent(cpuRegs.cycle, iopDelta - psxRegs.iopCycleEE);
	}
}

static __fi void IopTestEvent( IopEventId n, void (*callback)() )
{
	if( !(psxRegs.interrupt & (1 << n)) ) return;

	if (psxTestCycle(psxRegs.sCycle[n], psxRegs.eCycle[n]))
	{
		psxRegs.interrupt &= ~(1 << n);
		callback();
	}
	else
		psxSetNextBranch( psxRegs.sCycle[n], psxRegs.eCycle[n] );
}

static __fi void Sio0TestEvent(IopEventId n)
{
	if (!(psxRegs.interrupt & (1 << n)))
		return;

	if (psxTestCycle(psxRegs.sCycle[n], psxRegs.eCycle[n]))
	{
		psxRegs.interrupt &= ~(1 << n);
		sio0.Interrupt(Sio0Interrupt::TEST_EVENT);
	}
	else
		psxSetNextBranch(psxRegs.sCycle[n], psxRegs.eCycle[n]);
}

static __fi void _psxTestInterrupts(void)
{
	IopTestEvent(IopEvt_SIF0,		sif0Interrupt);	// SIF0
	IopTestEvent(IopEvt_SIF1,		sif1Interrupt);	// SIF1
	IopTestEvent(IopEvt_SIF2,		sif2Interrupt);	// SIF2
	Sio0TestEvent(IopEvt_SIO);
	IopTestEvent(IopEvt_CdvdSectorReady, cdvdSectorReady);
	IopTestEvent(IopEvt_CdvdRead,	cdvdReadInterrupt);

	// Profile-guided Optimization (sorta)
	// The following ints are rarely called.  Encasing them in a conditional
	// as follows helps speed up most games.

	if( psxRegs.interrupt & ((1 << IopEvt_Cdvd) | (1 << IopEvt_Dma11) | (1 << IopEvt_Dma12)
		| (1 << IopEvt_Cdrom) | (1 << IopEvt_CdromRead) | (1 << IopEvt_DEV9) | (1 << IopEvt_USB)))
	{
		IopTestEvent(IopEvt_Cdvd,		cdvdActionInterrupt);
		IopTestEvent(IopEvt_Dma11,		psxDMA11Interrupt);	// SIO2
		IopTestEvent(IopEvt_Dma12,		psxDMA12Interrupt);	// SIO2
		IopTestEvent(IopEvt_Cdrom,		cdrInterrupt);
		IopTestEvent(IopEvt_CdromRead,	cdrReadInterrupt);
		IopTestEvent(IopEvt_DEV9,		dev9Interrupt);
		IopTestEvent(IopEvt_USB,		usbInterrupt);
	}
}

__ri void iopEventTest(void)
{
	psxRegs.iopNextEventCycle = psxRegs.cycle + iopWaitCycles;

	if (psxTestCycle(psxNextStartCounter, psxNextDeltaCounter))
	{
		psxRcntUpdate();
		iopEventAction = true;
	}
	else
	{
		// start the next branch at the next counter event by default
		// the interrupt code below will assign nearer branches if needed.
		if (psxNextDeltaCounter < (psxRegs.iopNextEventCycle - psxNextStartCounter))
			psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;
	}

	if (psxRegs.interrupt)
	{
		iopEventTestIsActive = true;
		_psxTestInterrupts();
		iopEventTestIsActive = false;
	}

	if ((psxHu32(0x1078) != 0) && ((psxHu32(0x1070) & psxHu32(0x1074)) != 0))
	{
		if ((psxRegs.CP0.n.Status & 0xFE01) >= 0x401)
		{
			psxException(0, 0);
			iopEventAction = true;
		}
	}
}

void iopTestIntc(void)
{
	if( psxHu32(0x1078) == 0 ) return;
	if( (psxHu32(0x1070) & psxHu32(0x1074)) == 0 ) return;

	if( !eeEventTestIsActive )
	{
		// An iop exception has occurred while the EE is running code.
		// Inform the EE to branch so the IOP can handle it promptly:

		cpuSetNextEvent(cpuRegs.cycle, 16);
		iopEventAction = true;

		// Note: No need to set the iop's branch delta here, since the EE
		// will run an IOP branch test regardless.
	}
	else if( !iopEventTestIsActive )
		psxSetNextBranchDelta( 2 );
}

static __fi void execI(void)
{
	// Inject IRX hack
	if (psxRegs.pc == 0x1630 && EmuConfig.CurrentIRX.length() > 3)
	{
		// FIXME do I need to increase the module count (0x1F -> 0x20)
		if (iopMemRead32(0x20018) == 0x1F)
			iopMemWrite32(0x20094, 0xbffc0000);
	}

	psxRegs.code = iopMemRead32(psxRegs.pc);

	psxRegs.pc+= 4;
	psxRegs.cycle++;

	//One of the Iop to EE delta clocks to be set in PS1 mode.
	if ((psxHu32(HW_ICFG) & (1 << 3)))
		psxRegs.iopCycleEE -= 9;
	else //default ps2 mode value
		psxRegs.iopCycleEE -= 8;
	psxBSC[psxRegs.code >> 26]();
}

static void doBranch(s32 tar)
{
	branch2        = iopIsDelaySlot = true;
	branchPC       = tar;
	execI();
	iopIsDelaySlot = false;
	psxRegs.pc     = branchPC;
	iopEventTest();
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void psxBGEZ(void) // Branch if Rs >= 0
{
	if (_i32(_rRs_) >= 0) doBranch(_BranchTarget_);
}

void psxBGEZAL(void) // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) >= 0) doBranch(_BranchTarget_);
}

void psxBGTZ(void) // Branch if Rs >  0
{
	if (_i32(_rRs_) > 0) doBranch(_BranchTarget_);
}

void psxBLEZ(void) // Branch if Rs <= 0
{
	if (_i32(_rRs_) <= 0) doBranch(_BranchTarget_);
}

void psxBLTZ(void) // Branch if Rs <  0
{
	if (_i32(_rRs_) < 0) doBranch(_BranchTarget_);
}

void psxBLTZAL(void) // Branch if Rs <  0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) < 0) doBranch(_BranchTarget_);
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void psxBEQ(void)   // Branch if Rs == Rt
{
	if (_i32(_rRs_) == _i32(_rRt_)) doBranch(_BranchTarget_);
}

void psxBNE()   // Branch if Rs != Rt
{
	if (_i32(_rRs_) != _i32(_rRt_)) doBranch(_BranchTarget_);
}

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
void psxJ(void)
{
	// check for iop module import table magic
	uint32_t delayslot = iopMemRead32(psxRegs.pc);
	if (delayslot >> 16 == 0x2400 && R3000A::irxImportExec(R3000A::irxImportTableAddr(psxRegs.pc), delayslot & 0xffff))
		return;

	doBranch(_JumpTarget_);
}

void psxJAL(void)
{
	_SetLink(31);
	doBranch(_JumpTarget_);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/
void psxJR(void)
{
	doBranch(_u32(_rRs_));
}

void psxJALR(void)
{
	if (_Rd_)
	{
		_SetLink(_Rd_);
	}
	doBranch(_u32(_rRs_));
}

static void intReserve(void) { }
static void intAlloc(void) { }
static void intReset(void) { intAlloc(); }
static void intClear(uint32_t Addr, uint32_t Size) { }
static void intShutdown(void) { }

static s32 intExecuteBlock( s32 eeCycles )
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;

	while (psxRegs.iopCycleEE > 0)
	{
		if ((psxHu32(HW_ICFG) & 8) && ((psxRegs.pc & 0x1fffffffU) == 0xa0 || (psxRegs.pc & 0x1fffffffU) == 0xb0 || (psxRegs.pc & 0x1fffffffU) == 0xc0))
			psxBiosCall();

		branch2 = 0;
		while (!branch2)
			execI();
	}

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

R3000Acpu psxInt = {
	intReserve,
	intReset,
	intExecuteBlock,
	intClear,
	intShutdown
};

/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/
void psxADDI(void)   { if (_Rt_) _rRt_ = _u32(_rRs_) + _Imm_ ; /* Rt = Rs + Im (Exception on Integer Overflow) */ } 
void psxADDIU(void)  { if (_Rt_) _rRt_ = _u32(_rRs_) + _Imm_ ; /* Rt = Rs + Im */ }
void psxANDI(void)   { if (_Rt_) _rRt_ = _u32(_rRs_) & _ImmU_; }		// Rt = Rs And Im
void psxORI(void)    { if (_Rt_) _rRt_ = _u32(_rRs_) | _ImmU_; }		// Rt = Rs Or  Im
void psxXORI(void)   { if (_Rt_) _rRt_ = _u32(_rRs_) ^ _ImmU_; }		// Rt = Rs Xor Im
void psxSLTI(void)   { if (_Rt_) _rRt_ = _i32(_rRs_) < _Imm_ ; }		// Rt = Rs < Im	(Signed)
void psxSLTIU(void)  { if (_Rt_) _rRt_ = _u32(_rRs_) < (u32)_Imm_; }		// Rt = Rs < Im	(Unsigned)

/*********************************************************
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/
void psxADD(void)	{ if (_Rd_) _rRd_ = _u32(_rRs_) + _u32(_rRt_);   }	// Rd = Rs + Rt		(Exception on Integer Overflow)
void psxADDU(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) + _u32(_rRt_);   }	// Rd = Rs + Rt
void psxSUB(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) - _u32(_rRt_);   }	// Rd = Rs - Rt		(Exception on Integer Overflow)
void psxSUBU(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) - _u32(_rRt_);   }	// Rd = Rs - Rt
void psxAND(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) & _u32(_rRt_);   }	// Rd = Rs And Rt
void psxOR(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) | _u32(_rRt_);   }	// Rd = Rs Or  Rt
void psxXOR(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) ^ _u32(_rRt_);   }	// Rd = Rs Xor Rt
void psxNOR(void) 	{ if (_Rd_) _rRd_ =~(_u32(_rRs_) | _u32(_rRt_)); }	// Rd = Rs Nor Rt
void psxSLT(void) 	{ if (_Rd_) _rRd_ = _i32(_rRs_) < _i32(_rRt_);   }	// Rd = Rs < Rt		(Signed)
void psxSLTU(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) < _u32(_rRt_);   }	// Rd = Rs < Rt		(Unsigned)

/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/
void psxDIV(void)
{
	if (_rRt_ == 0) {
		// Division by 0
		_rLo_ = _i32(_rRs_) < 0 ? 1 : 0xFFFFFFFFu;
		_rHi_ = _rRs_;

	} else if (_rRs_ == 0x80000000u && _rRt_ == 0xFFFFFFFFu) {
		// x86 overflow
		_rLo_ = 0x80000000u;
		_rHi_ = 0;

	} else {
		// Normal behavior
		_rLo_ = _i32(_rRs_) / _i32(_rRt_);
		_rHi_ = _i32(_rRs_) % _i32(_rRt_);
	}
}

void psxDIVU(void)
{
	if (_rRt_ == 0) {
		// Division by 0
		_rLo_ = 0xFFFFFFFFu;
		_rHi_ = _rRs_;

	} else {
		// Normal behavior
		_rLo_ = _rRs_ / _rRt_;
		_rHi_ = _rRs_ % _rRt_;
	}
}

void psxMULT(void)
{
	uint64_t res     = (int64_t)((int64_t)_i32(_rRs_) * (int64_t)_i32(_rRt_));
	psxRegs.GPR.n.lo = (u32)(res & 0xffffffff);
	psxRegs.GPR.n.hi = (u32)((res >> 32) & 0xffffffff);
}

void psxMULTU(void)
{
	uint64_t res = (uint64_t)((uint64_t)_u32(_rRs_) * (uint64_t)_u32(_rRt_));

	psxRegs.GPR.n.lo = (u32)(res & 0xffffffff);
	psxRegs.GPR.n.hi = (u32)((res >> 32) & 0xffffffff);
}

/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
void psxSLL(void) { if (_Rd_) _rRd_ = _u32(_rRt_) << _Sa_; } // Rd = Rt << sa
void psxSRA(void) { if (_Rd_) _rRd_ = _i32(_rRt_) >> _Sa_; } // Rd = Rt >> sa (arithmetic)
void psxSRL(void) { if (_Rd_) _rRd_ = _u32(_rRt_) >> _Sa_; } // Rd = Rt >> sa (logical)

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/
void psxSLLV(void) { if (_Rd_) _rRd_ = _u32(_rRt_) << _u32(_rRs_); } // Rd = Rt << rs
void psxSRAV(void) { if (_Rd_) _rRd_ = _i32(_rRt_) >> _u32(_rRs_); } // Rd = Rt >> rs (arithmetic)
void psxSRLV(void) { if (_Rd_) _rRd_ = _u32(_rRt_) >> _u32(_rRs_); } // Rd = Rt >> rs (logical)

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/
void psxLUI(void) { if (_Rt_) _rRt_ = psxRegs.code << 16; } // Upper halfword of Rt = Im

/*********************************************************
* Move from HI/LO to GPR                                 *
* Format:  OP rd                                         *
*********************************************************/
void psxMFHI(void) { if (_Rd_) _rRd_ = _rHi_; } // Rd = Hi
void psxMFLO(void) { if (_Rd_) _rRd_ = _rLo_; } // Rd = Lo

/*********************************************************
* Move to GPR to HI/LO & Register jump                   *
* Format:  OP rs                                         *
*********************************************************/
void psxMTHI(void) { _rHi_ = _rRs_; } // Hi = Rs
void psxMTLO(void) { _rLo_ = _rRs_; } // Lo = Rs

/*********************************************************
* Special purpose instructions                           *
* Format:  OP                                            *
*********************************************************/
void psxBREAK(void)
{
	/* Break exception - PSX ROM doens't handle this */
	psxRegs.pc -= 4;
	psxException(0x24, iopIsDelaySlot);
}

void psxSYSCALL(void)
{
	psxRegs.pc -= 4;
	psxException(0x20, iopIsDelaySlot);

}

void psxRFE(void)
{
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status & 0xfffffff0)
		            | ((psxRegs.CP0.n.Status & 0x3c) >> 2);
}

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/

#define _oB_ (_u32(_rRs_) + _Imm_)

void psxLB(void)
{
	if (_Rt_)
		_rRt_ = (s8 )iopMemRead8(_oB_);
	else
	{
		iopMemRead8(_oB_);
	}
}

void psxLBU() {
	if (_Rt_) {
		_rRt_ = iopMemRead8(_oB_);
	} else {
		iopMemRead8(_oB_);
	}
}

void psxLH(void)
{
	if (_Rt_)
		_rRt_ = (s16)iopMemRead16(_oB_);
	else
	{
		iopMemRead16(_oB_);
	}
}

void psxLHU(void)
{
	if (_Rt_)
		_rRt_ = iopMemRead16(_oB_);
	else
	{
		iopMemRead16(_oB_);
	}
}

void psxLW(void)
{
	if (_Rt_)
		_rRt_ = iopMemRead32(_oB_);
	else
	{
		iopMemRead32(_oB_);
	}
}

void psxLWL(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	if (_Rt_)
		_rRt_ =	  ( _u32(_rRt_) & (0x00ffffff >> shift) )
			| ( mem << (24 - shift) );

	/*
	Mem = 1234.  Reg = abcd

	0   4bcd   (mem << 24) | (reg & 0x00ffffff)
	1   34cd   (mem << 16) | (reg & 0x0000ffff)
	2   234d   (mem <<  8) | (reg & 0x000000ff)
	3   1234   (mem      ) | (reg & 0x00000000)

	*/
}

void psxLWR(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	if (_Rt_)
		_rRt_ =	  ( _u32(_rRt_) & (0xffffff00 << (24 - shift)) )
			| ( mem  >> shift );

	/*
	Mem = 1234.  Reg = abcd

	0   1234   (mem      ) | (reg & 0x00000000)
	1   a123   (mem >>  8) | (reg & 0xff000000)
	2   ab12   (mem >> 16) | (reg & 0xffff0000)
	3   abc1   (mem >> 24) | (reg & 0xffffff00)

	*/
}

void psxSB(void) { iopMemWrite8 (_oB_, _u8 (_rRt_)); }
void psxSH(void) { iopMemWrite16(_oB_, _u16(_rRt_)); }
void psxSW(void) { iopMemWrite32(_oB_, _u32(_rRt_)); }

void psxSWL(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	iopMemWrite32((_oB_ & 0xfffffffc),  ( ( _u32(_rRt_) >>  (24 - shift) ) ) |
			     (  mem & (0xffffff00 << shift) ));
	/*
	Mem = 1234.  Reg = abcd

	0   123a   (reg >> 24) | (mem & 0xffffff00)
	1   12ab   (reg >> 16) | (mem & 0xffff0000)
	2   1abc   (reg >>  8) | (mem & 0xff000000)
	3   abcd   (reg      ) | (mem & 0x00000000)

	*/
}

void psxSWR(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	iopMemWrite32((_oB_ & 0xfffffffc), ( ( _u32(_rRt_) << shift ) |
			     (mem  & (0x00ffffff >> (24 - shift)) ) ) );
	/*
	Mem = 1234.  Reg = abcd

	0   abcd   (reg      ) | (mem & 0x00000000)
	1   bcd4   (reg <<  8) | (mem & 0x000000ff)
	2   cd34   (reg << 16) | (mem & 0x0000ffff)
	3   d234   (reg << 24) | (mem & 0x00ffffff)

	*/
}

/*********************************************************
* Moves between GPR and COPx                             *
* Format:  OP rt, fs                                     *
*********************************************************/
void psxMFC0(void)    { if (!_Rt_) return; _rRt_ = (int)_rFs_; }
void psxCFC0(void)    { if (!_Rt_) return; _rRt_ = (int)_rFs_; }

void psxMTC0(void)    { _rFs_ = _u32(_rRt_); }
void psxCTC0(void)    { _rFs_ = _u32(_rRt_); }

void psxCTC2(void)    { _c2dRd_ = _u32(_rRt_); };
/*********************************************************
* Unknown instruction (would generate an exception)       *
* Format:  ?                                             *
*********************************************************/
void psxNULL(void)    { }
void psxSPECIAL(void) { psxSPC[_Funct_](); }
void psxREGIMM(void)  { psxREG[_Rt_]();    }
void psxCOP0(void)    { psxCP0[_Rs_]();    }
void psxCOP2(void)    { psxCP2[_Funct_](); }
void psxBASIC(void)   { psxCP2BSC[_Rs_](); }

void(*psxBSC[64])(void) = {
	psxSPECIAL, psxREGIMM, psxJ   , psxJAL  , psxBEQ , psxBNE , psxBLEZ, psxBGTZ, //7
	psxADDI   , psxADDIU , psxSLTI, psxSLTIU, psxANDI, psxORI , psxXORI, psxLUI , //15
	psxCOP0   , psxNULL  , psxCOP2, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL, //23
	psxNULL   , psxNULL  , psxNULL, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL, //31
	psxLB     , psxLH    , psxLWL , psxLW   , psxLBU , psxLHU , psxLWR , psxNULL, //39
	psxSB     , psxSH    , psxSWL , psxSW   , psxNULL, psxNULL, psxSWR , psxNULL, //47
	psxNULL   , psxNULL  , gteLWC2, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL, //55
	psxNULL   , psxNULL  , gteSWC2, psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL //63
};


void(*psxSPC[64])(void) = {
	psxSLL , psxNULL , psxSRL , psxSRA , psxSLLV   , psxNULL , psxSRLV, psxSRAV,
	psxJR  , psxJALR , psxNULL, psxNULL, psxSYSCALL, psxBREAK, psxNULL, psxNULL,
	psxMFHI, psxMTHI , psxMFLO, psxMTLO, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxMULT, psxMULTU, psxDIV , psxDIVU, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxADD , psxADDU , psxSUB , psxSUBU, psxAND    , psxOR   , psxXOR , psxNOR ,
	psxNULL, psxNULL , psxSLT , psxSLTU, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxNULL, psxNULL , psxNULL, psxNULL, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxNULL, psxNULL , psxNULL, psxNULL, psxNULL   , psxNULL , psxNULL, psxNULL
};

void(*psxREG[32])(void) = {
	psxBLTZ  , psxBGEZ  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL  , psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxBLTZAL, psxBGEZAL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL  , psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};

void(*psxCP0[32])(void) = {
	psxMFC0, psxNULL, psxCFC0, psxNULL, psxMTC0, psxNULL, psxCTC0, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxRFE , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};

void (*psxCP2[64])(void) = {
	psxBASIC, gteRTPS , psxNULL , psxNULL, psxNULL, psxNULL , gteNCLIP, psxNULL, // 00
	psxNULL , psxNULL , psxNULL , psxNULL, gteOP  , psxNULL , psxNULL , psxNULL, // 08
	gteDPCS , gteINTPL, gteMVMVA, gteNCDS, gteCDP , psxNULL , gteNCDT , psxNULL, // 10
	psxNULL , psxNULL , psxNULL , gteNCCS, gteCC  , psxNULL , gteNCS  , psxNULL, // 18
	gteNCT  , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 20
	gteSQR  , gteDCPL , gteDPCT , psxNULL, psxNULL, gteAVSZ3, gteAVSZ4, psxNULL, // 28
	gteRTPT , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 30
	psxNULL , psxNULL , psxNULL , psxNULL, psxNULL, gteGPF  , gteGPL  , gteNCCT  // 38
};

void(*psxCP2BSC[32])(void) = {
	gteMFC2, psxNULL, gteCFC2, psxNULL, gteMTC2, psxNULL, gteCTC2, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};
