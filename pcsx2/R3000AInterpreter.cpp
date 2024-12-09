/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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


#include "R3000A.h"
#include "Common.h"
#include "Config.h"

#include "R5900OpcodeTables.h"
#include "IopBios.h"
#include "IopHw.h"

// Used to flag delay slot instructions when throwig exceptions.
bool iopIsDelaySlot = false;

static bool branch2 = 0;
static u32 branchPC;

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
	branch2 = iopIsDelaySlot = true;
	branchPC = tar;
	execI();
	iopIsDelaySlot = false;
	psxRegs.pc = branchPC;

	iopEventTest();
}


/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void psxBGEZ()         // Branch if Rs >= 0
{
	if (_i32(_rRs_) >= 0) doBranch(_BranchTarget_);
}

void psxBGEZAL()   // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void psxBGTZ()          // Branch if Rs >  0
{
	if (_i32(_rRs_) > 0) doBranch(_BranchTarget_);
}

void psxBLEZ()         // Branch if Rs <= 0
{
	if (_i32(_rRs_) <= 0) doBranch(_BranchTarget_);
}
void psxBLTZ()          // Branch if Rs <  0
{
	if (_i32(_rRs_) < 0) doBranch(_BranchTarget_);
}

void psxBLTZAL()    // Branch if Rs <  0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) < 0)
		{
			doBranch(_BranchTarget_);
		}
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void psxBEQ()   // Branch if Rs == Rt
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
	u32 delayslot = iopMemRead32(psxRegs.pc);
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
static void intClear(u32 Addr, u32 Size) { }
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
