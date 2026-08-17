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


// Important Note to Future Developers:
//   None of the COP0 instructions are really critical performance items,
//   so don't waste time converting any more them into recompiled code
//   unless it can make them nicely compact.  Calling the C versions will
//   suffice.

#include "../Common.h"
#include "../R5900OpcodeTables.h"
#include "iR5900.h"
#include "iCOP0.h"

namespace Interp = R5900::Interpreter::OpcodeImpl::COP0;
using namespace x86Emitter;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP0 {

/*********************************************************
*   COP0 opcodes                                         *
*                                                        *
*********************************************************/

// emits "setup" code for a COP0 branch test.  The instruction immediately following
// this should be a conditional Jump -- JZ or JNZ normally.
static void _setupBranchTest()
{
	_eeFlushAllDirty();

	// COP0 branch conditionals are based on the following equation:
	//  (((psHu16(DMAC_STAT) | ~psHu16(DMAC_PCR)) & 0x3ff) == 0x3ff)
	// BC0F checks if the statement is false, BC0T checks if the statement is true.

	// note: We only want to compare the 16 bit values of DMAC_STAT and PCR.
	// But using 32-bit loads here is ok (and faster), because we mask off
	// everything except the lower 10 bits away.

	xMOV(eax, ptr[(&psHu32(DMAC_PCR))]);
	xMOV(ecx, 0x3ff); // ECX is our 10-bit mask var
	xNOT(eax);
	xOR(eax, ptr[(&psHu32(DMAC_STAT))]);
	xAND(eax, ecx);
	xCMP(eax, ecx);
}

void recBC0F()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, false);
	_setupBranchTest();
	recDoBranchImm(branchTo, JE32(0), false, swap);
}

void recBC0T()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, false);
	_setupBranchTest();
	recDoBranchImm(branchTo, JNE32(0), false, swap);
}

void recBC0FL()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	recDoBranchImm(branchTo, JE32(0), true, false);
}

void recBC0TL()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	recDoBranchImm(branchTo, JNE32(0), true, false);
}

// TLBR copies one TLB entry into the CP0 registers. It is pure register
// shuffling -- no mapping state is touched -- so unlike the other three TLB
// ops it does not need to call into COP0.cpp at all.
//
//   PageMask = tlb[i].PageMask
//   EntryHi  = tlb[i].EntryHi & ~(tlb[i].PageMask | 0x1f00)
//   EntryLo0 = (tlb[i].EntryLo0 & ~1) | ((tlb[i].EntryHi >> 12) & 1)
//   EntryLo1 = (tlb[i].EntryLo1 & ~1) | ((tlb[i].EntryHi >> 12) & 1)
//
// The whole sequence fits in eax, ecx and edx: rcx holds &tlb[i] so rax is
// free as a scratch, and the G bit is computed once into edx and reused by
// both EntryLo halves.
void recTLBR()
{
	_freeX86reg(eax);
	_freeX86reg(ecx);
	_freeX86reg(edx);

	xMOV(eax, ptr32[&cpuRegs.CP0.n.Index]);
	xAND(eax, 0x3f);
	// Three-operand IMUL, which only started encoding correctly in 661ebc9;
	// before that it emitted PACKSSDW.
	xMUL(eax, eax, (s32)sizeof(tlbs));
	xMOV(rcx, (uptr)tlb);
	xADD(rcx, rax);

	// PageMask, then reuse it to build the EntryHi mask in place.
	xMOV(eax, ptr32[xAddressVoid(rcx, offsetof(tlbs, PageMask))]);
	xMOV(ptr32[&cpuRegs.CP0.n.PageMask], eax);
	xOR(eax, 0x1f00);
	xNOT(eax);
	xAND(eax, ptr32[xAddressVoid(rcx, offsetof(tlbs, EntryHi))]);
	xMOV(ptr32[&cpuRegs.CP0.n.EntryHi], eax);

	// G bit, shared by both EntryLo halves.
	xMOV(edx, ptr32[xAddressVoid(rcx, offsetof(tlbs, EntryHi))]);
	xSHR(edx, 12);
	xAND(edx, 1);

	xMOV(eax, ptr32[xAddressVoid(rcx, offsetof(tlbs, EntryLo0))]);
	xAND(eax, ~1);
	xOR(eax, edx);
	xMOV(ptr32[&cpuRegs.CP0.n.EntryLo0], eax);

	xMOV(eax, ptr32[xAddressVoid(rcx, offsetof(tlbs, EntryLo1))]);
	xAND(eax, ~1);
	xOR(eax, edx);
	xMOV(ptr32[&cpuRegs.CP0.n.EntryLo1], eax);
}
void recTLBP() { recCall(Interp::TLBP); }
void recTLBWI() { recCall(Interp::TLBWI); }
void recTLBWR() { recCall(Interp::TLBWR); }

// recBranchCall forces nextEventCycle = cycle before calling, which makes the
// interpreter's own "if (nextEventCycle - cycle > 4) nextEventCycle = cycle+4"
// unreachable on this path -- the delta is already zero. So both bodies below
// reduce to the register work, with the forced branch test kept verbatim and
// g_branch = 2 still ending the block.
//
// intSetBranch() is likewise a no-op here: it sets branch2, which is static to
// Interpreter.cpp and invisible to recompiled code.
static void recCOP0_ForceBranchTest()
{
	xMOV(rax, ptr64[&cpuRegs.cycle]);
	xMOV(ptr64[&cpuRegs.nextEventCycle], rax);
}

void recERET()
{
	_freeX86reg(eax);
	_freeX86reg(edx);
	recCOP0_ForceBranchTest();

	// ERL selects which EPC to resume from, and which flag to clear.
	xMOV(eax, ptr32[&cpuRegs.CP0.n.Status]);
	xTEST(eax, 0x4); // ERL
	xForwardJZ8 useEPC;
	xMOV(edx, ptr32[&cpuRegs.CP0.n.ErrorEPC]);
	xMOV(ptr32[&cpuRegs.pc], edx);
	xAND(eax, ~(u32)0x4);
	xForwardJump8 done;
	useEPC.SetTarget();
	xMOV(edx, ptr32[&cpuRegs.CP0.n.EPC]);
	xMOV(ptr32[&cpuRegs.pc], edx);
	xAND(eax, ~(u32)0x2); // EXL
	done.SetTarget();
	xMOV(ptr32[&cpuRegs.CP0.n.Status], eax);

	g_branch = 2;
}

void recEI()
{
	// must branch after enabling interrupts, so that anything
	// pending gets triggered properly.
	_freeX86reg(eax);
	recCOP0_ForceBranchTest();

	// Same guard recDI uses, inverted only in what it does to EIE.
	xMOV(eax, ptr32[&cpuRegs.CP0.n.Status]);
	xTEST(eax, 0x20006); // EXL | ERL | EDI
	xForwardJNZ8 privileged;
	xTEST(eax, 0x18); // KSU
	xForwardJNZ8 inUserMode;
	privileged.SetTarget();
	xOR(eax, 0x10000); // EIE
	xMOV(ptr32[&cpuRegs.CP0.n.Status], eax);
	inUserMode.SetTarget();

	g_branch = 2;
}

void recDI()
{
	//// No need to branch after disabling interrupts...

	//iFlushCall(0);

	//xMOV(eax, ptr[&cpuRegs.cycle ]);
	//xMOV(ptr[&g_nextBranchCycle], eax);

	//xFastCall((void*)(uptr)Interp::DI );

	// Fixes booting issues in the following games:
	// Jak X, Namco 50th anniversary, Spongebob the Movie, Spongebob Battle for Bikini Bottom,
	// The Incredibles, The Incredibles rize of the underminer, Soukou kihei armodyne, Garfield Saving Arlene, Tales of Fandom Vol. 2.
	if (!g_recompilingDelaySlot)
		recompileNextInstruction(false, false); // DI execution is delayed by one instruction

	xMOV(eax, ptr[&cpuRegs.CP0.n.Status]);
	xTEST(eax, 0x20006); // EXL | ERL | EDI
	xForwardJNZ8 iHaveNoIdea;
	xTEST(eax, 0x18); // KSU
	xForwardJNZ8 inUserMode;
	iHaveNoIdea.SetTarget();
	xAND(eax, ~(u32)0x10000); // EIE
	xMOV(ptr[&cpuRegs.CP0.n.Status], eax);
	inUserMode.SetTarget();
}


#ifndef CP0_RECOMPILE

REC_SYS(MFC0);
REC_SYS(MTC0);

#else

void recMFC0()
{
	if (_Rd_ == 9)
	{
		// This case needs to be handled even if the write-back is ignored (_Rt_ == 0 )
		xMOV(rcx, ptr64[&cpuRegs.cycle]);
		xADD(rcx, scaleblockcycles_clear());
		xMOV(ptr64[&cpuRegs.cycle], rcx); // update cycles
		xMOV(rax, rcx);
		xSUB(rax, ptr[&cpuRegs.lastCOP0Cycle]);
		xADD(ptr[&cpuRegs.CP0.n.Count], rax);
		xMOV(ptr64[&cpuRegs.lastCOP0Cycle], rcx);

		if (!_Rt_)
			return;

		const int regt = _Rt_ ? _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE) : -1;
		xMOVSX(xRegister64(regt), ptr32[&cpuRegs.CP0.r[_Rd_]]);
		return;
	}

	if (!_Rt_)
		return;

	if (_Rd_ == 25)
	{
		if (0 == (_Imm_ & 1)) // MFPS, register value ignored
		{
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pccr]);
		}
		else if (0 == (_Imm_ & 2)) // MFPC 0, only LSB of register matters
		{
			iFlushCall(FLUSH_INTERPRETER);
			xMOV(rax, ptr64[&cpuRegs.cycle]);
			xADD(rax, scaleblockcycles_clear());
			xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
			xFastCall((void*)COP0_UpdatePCCR);

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pcr0]);
		}
		else // MFPC 1
		{
			iFlushCall(FLUSH_INTERPRETER);
			xMOV(rax, ptr64[&cpuRegs.cycle]);
			xADD(rax, scaleblockcycles_clear());
			xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
			xFastCall((void*)COP0_UpdatePCCR);

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pcr1]);
		}

		return;
	}
	else if (_Rd_ == 24)
		return;

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
	xMOVSX(xRegister64(regt), ptr32[&cpuRegs.CP0.r[_Rd_]]);
}

void recMTC0()
{
	if (GPR_IS_CONST1(_Rt_))
	{
		switch (_Rd_)
		{
			case 12:
				iFlushCall(FLUSH_INTERPRETER);
				xMOV(rax, ptr64[&cpuRegs.cycle]);
				xADD(rax, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
				xFastCall((void*)WriteCP0Status, g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 16:
				iFlushCall(FLUSH_INTERPRETER);
				xFastCall((void*)WriteCP0Config, g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 9:
				xMOV(rcx, ptr64[&cpuRegs.cycle]);
				xADD(rcx, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rcx); // update cycles
				xMOV(ptr64[&cpuRegs.lastCOP0Cycle], rcx);
				xMOV(ptr32[&cpuRegs.CP0.r[9]], g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					// Updates PCRs and sets the PCCR.
					iFlushCall(FLUSH_INTERPRETER);
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
					xFastCall((void*)COP0_UpdatePCCR);
					xMOV(ptr32[&cpuRegs.PERF.n.pccr], g_cpuConstRegs[_Rt_].UL[0]);
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
					xMOV(ptr32[&cpuRegs.PERF.n.pcr0], g_cpuConstRegs[_Rt_].UL[0]);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[0]], rax);
				}
				else // MTPC 1
				{
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xMOV(ptr32[&cpuRegs.PERF.n.pcr1], g_cpuConstRegs[_Rt_].UL[0]);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[1]], rax);
				}
				break;

			case 24:
				break;

			default:
				xMOV(ptr32[&cpuRegs.CP0.r[_Rd_]], g_cpuConstRegs[_Rt_].UL[0]);
				break;
		}
	}
	else
	{
		switch (_Rd_)
		{
			case 12:
				_eeMoveGPRtoR(arg1reg, _Rt_);
				iFlushCall(FLUSH_INTERPRETER);
				xMOV(rax, ptr64[&cpuRegs.cycle]);
				xADD(rax, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
				xFastCall((void*)WriteCP0Status);
				break;

			case 16:
				_eeMoveGPRtoR(arg1reg, _Rt_);
				iFlushCall(FLUSH_INTERPRETER);
				xFastCall((void*)WriteCP0Config);
				break;

			case 9:
				xMOV(rcx, ptr64[&cpuRegs.cycle]);
				xADD(rcx, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rcx); // update cycles
				_eeMoveGPRtoM((uptr)&cpuRegs.CP0.r[9], _Rt_);
				xMOV(ptr64[&cpuRegs.lastCOP0Cycle], rcx);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					iFlushCall(FLUSH_INTERPRETER);
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax); // update cycles
					xFastCall((void*)COP0_UpdatePCCR);
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pccr, _Rt_);
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					xMOV(rcx, ptr64[&cpuRegs.cycle]);
					xADD(rcx, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rcx); // update cycles
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pcr0, _Rt_);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[0]], rcx);
				}
				else // MTPC 1
				{
					xMOV(rcx, ptr64[&cpuRegs.cycle]);
					xADD(rcx, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rcx); // update cycles
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pcr1, _Rt_);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[1]], rcx);
				}
				break;

			case 24:
				break;

			default:
				_eeMoveGPRtoM((uptr)&cpuRegs.CP0.r[_Rd_], _Rt_);
				break;
		}
	}
}
#endif

} // namespace COP0
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
