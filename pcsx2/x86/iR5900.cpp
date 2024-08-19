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

#include "common/AlignedMalloc.h"
#include "common/FastJmp.h"

#include "x86emitter.h"

#include "BaseblockEx.h"
#include "iCOP0.h"
#include "iFPU.h"
#include "iMMI.h"
#include "iR5900.h"
#include "iR5900Analysis.h"
#include "iR5900LoadStore.h"
#include "iR3000A.h"
#include "vtlb.h"

#include "../Common.h"
#include "../Elfheader.h"
#include "../GS.h"
#include "../R3000A.h"
#include "../Memory.h"
#include "../Patch.h"
#include "../VU.h"
#include "../VUmicro.h"
#include "../Vif.h"
#include "../R5900OpcodeTables.h"
#include "../VMManager.h"
#include "../VirtualMemory.h"
#include "../vtlb.h"
#include "../CDVD/CDVD.h"

#define PC_GETBLOCK(x) PC_GETBLOCK_(x, recLUT)
#define HWADDR(mem) (hwLUT[(mem) >> 16] + (mem))

namespace R5900 {
namespace Dynarec {

// R5900 branch helper!
// Recompiles code for a branch test and/or skip, complete with delay slot
// handling.  Note, for "likely" branches use iDoBranchImm_Likely instead, which
// handles delay slots differently.
// Parameters:
//   jmpSkip - This parameter is the result of the appropriate J32 instruction
//   (usually JZ32 or JNZ32).
void recDoBranchImm(u32 branchTo, u32* jmpSkip, bool isLikely, bool swappedDelaySlot)
{
	// First up is the Branch Taken Path : Save the recompiler's state, compile the
	// DelaySlot, and issue a BranchTest insertion.  The state is reloaded below for
	// the "did not branch" path (maintains consts, register allocations, and other optimizations).

	if (!swappedDelaySlot)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	// Jump target when the branch is *not* taken, skips the branchtest code
	// insertion above.
	*jmpSkip = (x86Ptr - (u8*)jmpSkip) - 4;

	// if it's a likely branch then we'll need to skip the delay slot here, since
	// MIPS cancels the delay slot instruction when branches aren't taken.
	if (!swappedDelaySlot)
	{
		LoadBranchState();
		if (!isLikely)
		{
			pc -= 4; // instruction rewinder for delay slot, if non-likely.
			recompileNextInstruction(true, false);
		}
	}

	SetBranchImm(pc); // start a new recompiled block.
}

} // namespace Dynarec
} // namespace R5900

using namespace R5900;

// opcode 'code' modifies:
// 1: status
// 2: MAC
// 4: clip
static int cop2flags(u32 code)
{
	if (code >> 26 != 022)
		return 0; // not COP2
	if ((code >> 25 & 1) == 0)
		return 0; // a branch or transfer instruction

	switch (code >> 2 & 15)
	{
		case 15:
			switch (code >> 6 & 0x1f)
			{
				case 4: // ITOF*
				case 5: // FTOI*
				case 12: // MOVE MR32
				case 13: // LQI SQI LQD SQD
				case 15: // MTIR MFIR ILWR ISWR
				case 16: // RNEXT RGET RINIT RXOR
					return 0;
				case 7: // MULAq, ABS, MULAi, CLIP
					if ((code & 3) == 1) // ABS
						return 0;
					if ((code & 3) == 3) // CLIP
						return 4;
					break;
				case 11: // SUBA, MSUBA, OPMULA, NOP
					if ((code & 3) == 3) // NOP
						return 0;
					break;
				case 14: // DIV, SQRT, RSQRT, WAITQ
					if ((code & 3) == 3) // WAITQ
						return 0;
					return 1; // but different timing, ugh
				default:
					break;
			}
			break;
		case 4: // MAXbc
		case 5: // MINbc
		case 12: // IADD, ISUB, IADDI
		case 13: // IAND, IOR
		case 14: // VCALLMS, VCALLMSR
			return 0;
		case 7:
			if ((code & 1) == 1) // MAXi, MINIi
				return 0;
			break;
		case 10:
			if ((code & 3) == 3) // MAX
				return 0;
			break;
		case 11:
			if ((code & 3) == 3) // MINI
				return 0;
			break;
		default:
			break;
	}
	return 3;
}

AnalysisPass::AnalysisPass() = default;

AnalysisPass::~AnalysisPass() = default;

void AnalysisPass::Run(u32 start, u32 end, EEINST* inst_cache)
{
}

template <class F>
void __fi AnalysisPass::ForEachInstruction(u32 start, u32 end, EEINST* inst_cache, const F& func)
{
	EEINST* eeinst = inst_cache;
	for (u32 apc = start; apc < end; apc += 4, eeinst++)
	{
		cpuRegs.code = vtlb_memRead32(apc);
		if (!func(apc, eeinst))
			break;
	}
}

COP2FlagHackPass::COP2FlagHackPass()
	: AnalysisPass()
{
}

COP2FlagHackPass::~COP2FlagHackPass() = default;

void COP2FlagHackPass::Run(u32 start, u32 end, EEINST* inst_cache)
{
	m_status_denormalized = false;
	m_last_status_write = nullptr;
	m_last_mac_write = nullptr;
	m_last_clip_write = nullptr;
	m_cfc2_pc = start;

	ForEachInstruction(start, end, inst_cache, [this, end](u32 apc, EEINST* inst) {
		// catch SB/SH/SW to potential DMA->VIF0->VU0 exec.
		// this is very unlikely in a cop2 chain.
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053)
		{
			CommitAllFlags();
			return true;
		}
		else if (_Opcode_ != 022)
		{
			// not COP2
			return true;
		}

		// Detect ctc2 Status, zero, ..., cfc2 v0, Status pattern where we need accurate sticky bits.
		// Test case: Tekken Tag Tournament.
		if (_Rs_ == 6 && _Rd_ == REG_STATUS_FLAG)
		{
			// Read ahead, looking for cfc2.
			m_cfc2_pc = apc;
			ForEachInstruction(apc, end, inst, [this](u32 capc, EEINST*) {
				if (_Opcode_ == 022 && _Rs_ == 2 && _Rd_ == REG_STATUS_FLAG)
				{
					m_cfc2_pc = capc;
					return false;
				}
				return true;
			});
		}

		// CFC2/CTC2
		if (_Rs_ == 6 || _Rs_ == 2)
		{
			switch (_Rd_)
			{
				case REG_STATUS_FLAG:
					CommitStatusFlag();
					break;
				case REG_MAC_FLAG:
					CommitMACFlag();
					break;
				case REG_CLIP_FLAG:
					CommitClipFlag();
					break;
				case REG_FBRST:
				{
					// only apply to CTC2, is FBRST readable?
					if (_Rs_ == 2)
						CommitAllFlags();
				}
				break;
			}
		}

		if (((cpuRegs.code >> 25 & 1) == 1) && ((cpuRegs.code >> 2 & 15) == 14))
		{
			// VCALLMS, everything needs to be up to date
			CommitAllFlags();
		}

		// 1 - status, 2 - mac, 3 - clip
		const int flags = cop2flags(cpuRegs.code);
		if (flags == 0)
			return true;

		// STATUS
		if (flags & 1)
		{
			if (!m_status_denormalized)
			{
				inst->info |= EEINST_COP2_DENORMALIZE_STATUS_FLAG;
				m_status_denormalized = true;
			}

			// If we're still behind the next CFC2 after the sticky bits got cleared, we need to update flags.
			// Also do this if we're a vsqrt/vrsqrt/vdiv, these update status unconditionally.
			const u32 sub_opcode = (cpuRegs.code & 3) | ((cpuRegs.code >> 4) & 0x7c);
			if (apc < m_cfc2_pc || (_Rs_ >= 020 && _Funct_ >= 074 && sub_opcode >= 070 && sub_opcode <= 072))
				inst->info |= EEINST_COP2_STATUS_FLAG;

			m_last_status_write = inst;
		}

		// MAC
		if (flags & 2)
			m_last_mac_write = inst;

		// CLIP
		if (flags & 4)
		{
			// we don't track the clip flag yet..
			// but it's unlikely that we'll have more than 4 clip flags in a row, because that would be pointless?
			inst->info |= EEINST_COP2_CLIP_FLAG;
			m_last_clip_write = inst;
		}

		return true;
	});

	CommitAllFlags();
}

void COP2FlagHackPass::CommitStatusFlag()
{
	if (m_last_status_write)
	{
		m_last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
		m_status_denormalized = false;
	}
}

void COP2FlagHackPass::CommitMACFlag()
{
	if (m_last_mac_write)
		m_last_mac_write->info |= EEINST_COP2_MAC_FLAG;
}

void COP2FlagHackPass::CommitClipFlag()
{
	if (m_last_clip_write)
		m_last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
}

void COP2FlagHackPass::CommitAllFlags()
{
	CommitStatusFlag();
	CommitMACFlag();
	CommitClipFlag();
}

COP2MicroFinishPass::COP2MicroFinishPass() = default;

COP2MicroFinishPass::~COP2MicroFinishPass() = default;

void COP2MicroFinishPass::Run(u32 start, u32 end, EEINST* inst_cache)
{
	bool needs_vu0_sync = true;
	bool needs_vu0_finish = true;
	bool block_interlocked = CHECK_FULLVU0SYNCHACK;

	// First pass through the block to find out if it's interlocked or not. If it is, we need to use tighter
	// synchronization on all COP2 instructions, otherwise Crash Twinsanity breaks.
	ForEachInstruction(start, end, inst_cache, [&block_interlocked](u32 apc, EEINST* inst) {
		if (_Opcode_ == 022 && (_Rs_ == 001 || _Rs_ == 002 || _Rs_ == 005 || _Rs_ == 006) && cpuRegs.code & 1)
		{
			block_interlocked = true;
			return false;
		}
		return true;
	});

	ForEachInstruction(start, end, inst_cache, [this, end, inst_cache, &needs_vu0_sync, &needs_vu0_finish, block_interlocked](u32 apc, EEINST* inst) {
		// Catch SQ/SB/SH/SW/SD to potential DMA->VIF0->VU0 exec.
		// Also VCALLMS/VCALLMSR, that can start a micro, so the next instruction needs to finish it.
		// This is very unlikely in a cop2 chain.
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053 || _Opcode_ == 077 || (_Opcode_ == 022 && _Rs_ >= 020 && (_Funct_ == 070 || _Funct_ == 071)))
		{
			// If we started a micro, we'll need to finish it before the first COP2 instruction.
			needs_vu0_sync = true;
			needs_vu0_finish = true;
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS;
			return true;
		}

		// LQC2/SQC2 - these don't interlock with VU0, but still sync, so we can persist the cached registers
		// for a LQC2..COP2 sequence. If there's no COP2 instructions following, don't bother, just yolo it.
		// We do either a sync or a finish here depending on which COP2 instruction follows - we don't want
		// to run the program until end if there's nothing which would actually trigger that.
		//
		// In essence, what we're doing is moving the finish from the COP2 instruction to the LQC2 in a LQC2..COP2
		// chain, so that we can preserve the cached registers and not need to reload them.
		//
		const bool is_lqc_sqc = (_Opcode_ == 066 || _Opcode_ == 076);
		const bool is_non_interlocked_move = (_Opcode_ == 022 && _Rs_ < 020 && ((cpuRegs.code & 1) == 0));
		// Moving zero to the VU registers, so likely removing a loop/lock.
		const bool likely_clear = _Opcode_ == 022 && _Rs_ < 020 && _Rs_ > 004 && _Rt_ == 000;
		if ((needs_vu0_sync && (is_lqc_sqc || is_non_interlocked_move)) || likely_clear)
		{
			bool following_needs_finish = false;
			ForEachInstruction(apc + 4, end, inst_cache + 1, [&following_needs_finish](u32 apc2, EEINST* inst2) {
				if (_Opcode_ == 022)
				{
					// For VCALLMS/VCALLMSR, we only sync, because the VCALLMS in itself will finish.
					// Since we're paying the cost of syncing anyway, better to be less risky.
					if (_Rs_ >= 020 && (_Funct_ == 070 || _Funct_ == 071))
						return false;

					// Allow the finish from COP2 to be moved to the first LQC2 of LQC2..QMTC2..COP2.
					// Otherwise, keep searching for a finishing COP2.
					following_needs_finish = _Rs_ >= 020;
					if (following_needs_finish)
						return false;
				}

				return true;
			});
			if (following_needs_finish && !block_interlocked)
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_FINISH_VU0;
				needs_vu0_sync = false;
				needs_vu0_finish = false;
			}
			else
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_SYNC_VU0;
				needs_vu0_sync = block_interlocked || (is_non_interlocked_move && likely_clear);
				needs_vu0_finish = true;
			}

			return true;
		}

		// Look for COP2 instructions.
		if (_Opcode_ != 022)
			return true;

		// Set the flag on the current instruction, and clear it for the next.
		if (_Rs_ >= 020 && needs_vu0_finish)
		{
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_FINISH_VU0;
			needs_vu0_finish = false;
			needs_vu0_sync = false;
		}
		else if (needs_vu0_sync)
		{
			// Starting a sync-free block!
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_SYNC_VU0;
			needs_vu0_sync = block_interlocked;
		}

		return true;
	});
}

/////////////////////////////////////////////////////////////////////
// Back-Prop Function Tables - Gathering Info
// Note to anyone changing these: writes must go before reads.
// Otherwise the last use flag won't get set.
/////////////////////////////////////////////////////////////////////

#define recBackpropSetGPRRead(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			prev->regs[reg]  = (EEINST_LIVE | EEINST_USED); \
			pinst->regs[reg] = (pinst->regs[reg] & ~EEINST_XMM) | EEINST_USED; \
			for (size_t i = 0; i < std::size(pinst->readType); ++i) \
			{ \
				if (pinst->readType[i] == XMMTYPE_TEMP) \
				{ \
					pinst->readType[i] = XMMTYPE_GPRREG; \
					pinst->readReg[i]  = reg; \
					break; \
				} \
			} \
		} \
	} while (0)

#define recBackpropSetGPRWrite(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			prev->regs[reg] &= ~(EEINST_XMM | EEINST_LIVE | EEINST_USED); \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_USED; \
			for (size_t i = 0; i < std::size(pinst->writeType); ++i) \
			{ \
				if (pinst->writeType[i] == XMMTYPE_TEMP) \
				{ \
					pinst->writeType[i] = XMMTYPE_GPRREG; \
					pinst->writeReg[i]  = reg; \
					break; \
				} \
			} \
		} \
	} while (0)

#define recBackpropSetGPRRead128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			prev->regs[reg] |= EEINST_LIVE | EEINST_USED | EEINST_XMM; \
			pinst->regs[reg] |= EEINST_USED | EEINST_XMM; \
			for (size_t i = 0; i < std::size(pinst->readType); ++i) \
			{ \
				if (pinst->readType[i] == XMMTYPE_TEMP) \
				{ \
					pinst->readType[i] = XMMTYPE_GPRREG; \
					pinst->readReg[i]  = reg; \
					break; \
				} \
			} \
		} \
	} while (0)

#define recBackpropSetGPRPartialWrite128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_LIVE | EEINST_USED | EEINST_XMM; \
			prev->regs[reg] |= EEINST_USED | EEINST_XMM; \
			for (size_t i = 0; i < std::size(pinst->writeType); ++i) \
			{ \
				if (pinst->writeType[i] == XMMTYPE_TEMP) \
				{ \
					pinst->writeType[i] = XMMTYPE_GPRREG; \
					pinst->writeReg[i]  = reg; \
					break; \
				} \
			} \
		} \
	} while (0)

#define recBackpropSetGPRWrite128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			prev->regs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_USED | EEINST_XMM; \
			for (size_t i = 0; i < std::size(pinst->writeType); ++i) \
			{ \
				if (pinst->writeType[i] == XMMTYPE_TEMP) \
				{ \
					pinst->writeType[i] = XMMTYPE_GPRREG; \
					pinst->writeReg[i]  = reg; \
					break; \
				} \
			} \
		} \
	} while (0)

#define recBackpropSetFPURead(reg) \
	do \
	{ \
		if (!(pinst->fpuregs[reg] & EEINST_USED)) \
			pinst->fpuregs[reg] |= EEINST_LASTUSE; \
		prev->fpuregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->fpuregs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->readType); ++i) \
		{ \
			if (pinst->readType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->readType[i] = XMMTYPE_FPREG; \
				pinst->readReg[i]  = reg; \
				break; \
			} \
		} \
	} while (0)

#define recBackpropSetFPUWrite(reg) \
	do \
	{ \
		prev->fpuregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->fpuregs[reg] & EEINST_USED)) \
			pinst->fpuregs[reg] |= EEINST_LASTUSE; \
		pinst->fpuregs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->writeType); ++i) \
		{ \
			if (pinst->writeType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->writeType[i] = XMMTYPE_FPREG; \
				pinst->writeReg[i]  = reg; \
				break; \
			} \
		} \
	} while (0)

#define recBackpropSetVFRead(reg) \
	do \
	{ \
		if (!(pinst->vfregs[reg] & EEINST_USED)) \
			pinst->vfregs[reg] |= EEINST_LASTUSE; \
		prev->vfregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->vfregs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->readType); ++i) \
		{ \
			if (pinst->readType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->readType[i] = XMMTYPE_VFREG; \
				pinst->readReg[i]  = reg; \
				break; \
			} \
		} \
	} while (0)

#define recBackpropSetVFWrite(reg) \
	do \
	{ \
		prev->vfregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->vfregs[reg] & EEINST_USED)) \
			pinst->vfregs[reg] |= EEINST_LASTUSE; \
		pinst->vfregs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->writeType); ++i) \
		{ \
			if (pinst->writeType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->writeType[i] = XMMTYPE_VFREG; \
				pinst->writeReg[i]  = reg; \
				break; \
			} \
		} \
	} while (0)

#define recBackpropSetVIRead(reg) \
	if ((reg) < 16) \
	{ \
		if (!(pinst->viregs[reg] & EEINST_USED)) \
			pinst->viregs[reg] |= EEINST_LASTUSE; \
		prev->viregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->viregs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->readType); ++i) \
		{ \
			if (pinst->readType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->readType[i] = X86TYPE_VIREG; \
				pinst->readReg[i]  = reg; \
				break; \
			} \
		} \
	}

#define recBackpropSetVIWrite(reg) \
	if ((reg) < 16) \
	{ \
		prev->viregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->viregs[reg] & EEINST_USED)) \
			pinst->viregs[reg] |= EEINST_LASTUSE; \
		pinst->viregs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->writeType); ++i) \
		{ \
			if (pinst->writeType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->writeType[i] = X86TYPE_VIREG; \
				pinst->writeReg[i]  = reg; \
				break; \
			} \
		} \
	}

static void recBackpropSPECIAL(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropREGIMM(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP0(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP1(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP2(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropMMI(u32 code, EEINST* prev, EEINST* pinst);

void recBackpropBSC(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (code >> 26)
	{
		case 0:
			recBackpropSPECIAL(code, prev, pinst);
			break;
		case 1:
			recBackpropREGIMM(code, prev, pinst);
			break;
		case 3: // jal
			recBackpropSetGPRWrite(31);
			break;
		case 4: // beq
		case 5: // bne
		case 20: // beql
		case 21: // bnel
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 6: // blez
		case 7: // bgtz
		case 22: // blezl
		case 23: // bgtzl
			recBackpropSetGPRRead(rs);
			break;

		case 15: // lui
			recBackpropSetGPRWrite(rt);
			break;

		case 8: // addi
		case 9: // addiu
		case 10: // slti
		case 11: // sltiu
		case 12: // andi
		case 13: // ori
		case 14: // xori
		case 24: // daddi
		case 25: // daddiu
		case 32: // lb
		case 33: // lh
		case 35: // lw
		case 36: // lbu
		case 37: // lhu
		case 39: // lwu
		case 55: // ld
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 30: // lq
			recBackpropSetGPRWrite128(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 26: // ldl
		case 27: // ldr
		case 34: // lwl
		case 38: // lwr
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 40: // sb
		case 41: // sh
		case 42: // swl
		case 43: // sw
		case 44: // sdl
		case 45: // sdr
		case 46: // swr
		case 63: // sd
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 31: // sq
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead128(rs);
			break;

		case 16:
			recBackpropCOP0(code, prev, pinst);
			break;

		case 17:
			recBackpropCOP1(code, prev, pinst);
			break;

		case 18:
			recBackpropCOP2(code, prev, pinst);
			break;

		case 28:
			recBackpropMMI(code, prev, pinst);
			break;

		case 49: // lwc1
		case 57: // swc1
			recBackpropSetGPRRead(rs);
			recBackpropSetFPURead(rt);
			break;

		case 54: // lqc2
			recBackpropSetVFWrite(rt);
			recBackpropSetGPRRead128(rs);
			break;

		case 62: // sqc2
			recBackpropSetGPRRead128(rs);
			recBackpropSetVFRead(rt);
			break;

		case 47: // cache
			recBackpropSetGPRRead(rs);
			break;

		case 51: // pref
		case 2: // j
		default:
			break;
	}
}

void recBackpropSPECIAL(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 rd = ((code >> 11) & 0x1F);
	const u32 funct = (code & 0x3F);

	switch (funct)
	{
		case 0: // sll
		case 2: // srl
		case 3: // sra
		case 56: // dsll
		case 58: // dsrl
		case 59: // dsra
		case 60: // dsll32
		case 62: // dsrl32
		case 63: // dsra32
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rt);
			break;

		case 4: // sllv
		case 6: // srlv
		case 7: // srav
		case 10: // movz
		case 11: // movn
		case 20: // dsllv
		case 22: // dsrlv
		case 23: // dsrav
		case 32: // add
		case 33: // addu
		case 34: // sub
		case 35: // subu
		case 36: // and
		case 37: // or
		case 38: // xor
		case 39: // nor
		case 42: // slt
		case 43: // sltu
		case 44: // dadd
		case 45: // daddu
		case 46: // dsub
		case 47: // dsubu
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 8: // jr
			recBackpropSetGPRRead(rs);
			break;

		case 9: // jalr
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rs);
			break;

		case 24: // mult
		case 25: // multu
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 26: // div
		case 27: // divu
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 16: // mfhi
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(XMMGPR_HI);
			break;

		case 17: // mthi
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			break;

		case 18: // mflo
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(XMMGPR_LO);
			break;

		case 19: // mtlo
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRRead(rs);
			break;

		case 40: // mfsa
			recBackpropSetGPRWrite(rd);
			break;

		case 41: // mtsa
		case 48: // tge
		case 49: // tgeu
		case 50: // tlt
		case 51: // tltu
		case 52: // teq
		case 54: // tne
			recBackpropSetGPRRead(rs);
			break;

		case 12: // syscall
		case 13: // break
			_recClearInst(prev);
			prev->info = 0;
			break;

		case 15: // sync
		default:
			break;
	}
}

void recBackpropREGIMM(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (rt)
	{
		case 0: // bltz
		case 1: // bgez
		case 2: // bltzl
		case 3: // bgezl
		case 9: // tgei
		case 10: // tgeiu
		case 11: // tlti
		case 12: // tltiu
		case 13: // teqi
		case 15: // tnei
		case 24: // mtsab
		case 25: // mtsah
		case 16: // bltzal
		case 17: // bgezal
		case 18: // bltzall
		case 19: // bgezall
			// do not write 31
			recBackpropSetGPRRead(rs);
			break;

		default:
			break;
	}
}

void recBackpropCOP0(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (rs)
	{
		case 0: // mfc0
		case 2: // cfc0
			recBackpropSetGPRWrite(rt);
			break;

		case 4: // mtc0
		case 6: // ctc0
			recBackpropSetGPRRead(rt);
			break;

		case 8: // bc0f/bc0t/bc0fl/bc0tl
		case 16: // tlb/eret/ei/di
		default:
			break;
	}
}

void recBackpropCOP1(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 fmt = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 fs = ((code >> 11) & 0x1F);
	const u32 ft = ((code >> 16) & 0x1F);
	const u32 fd = ((code >> 6) & 0x1F);
	const u32 funct = (code & 0x3F);

	switch (fmt)
	{
		case 0: // mfc1
			recBackpropSetGPRWrite(rt);
			recBackpropSetFPURead(fs);
			break;

		case 2: // cfc1
			recBackpropSetGPRWrite(rt);
			// read fprc[31] or fprc[0]
			break;

		case 4: // mtc1
			recBackpropSetFPUWrite(fs);
			recBackpropSetGPRRead(rt);
			break;

		case 6: // ctc1
			recBackpropSetGPRRead(rt);
			// write fprc[fs]
			break;

		case 8: // bc1f/bc1t/bc1fl/bc1tl
			// read fprc[31]
			break;

		case 16: // cop1.s
		{
			switch (funct)
			{
				case 0: // add.s
				case 1: // sub.s
				case 2: // mul.s
				case 3: // div.s
				case 40: // max.s
				case 41: // min.s
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				case 5: // abs.s
				case 6: // mov.s
				case 7: // neg.s
				case 36: // cvt.w
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					break;

				case 24: // adda.s
				case 25: // suba.s
				case 26: // mula.s
					recBackpropSetFPUWrite(XMMFPU_ACC);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				case 28: // madd.s
				case 29: // msub.s
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					recBackpropSetFPURead(XMMFPU_ACC);
					break;

				case 30: // madda.s
				case 31: // msuba.s
					recBackpropSetFPUWrite(XMMFPU_ACC);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					recBackpropSetFPURead(XMMFPU_ACC);
					break;

				case 4: // sqrt.s
				case 22: // rsqrt.s
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(ft);
					break;

				case 50: // c.eq
				case 52: // c.lt
				case 54: // c.le
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					// read + write fprc
					break;
				case 48: // c.f
					// read + write fprc
				default:
					break;
			}
		}
		break;

		case 20: // cop1.w
		{
			switch (funct)
			{
				case 32: // cvt.s
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					break;

				default:
					break;
			}
		}
		break;

		default:
			break;
	}
}

void recBackpropCOP2(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 fmt = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 fs = ((code >> 11) & 0x1F);
	const u32 ft = ((code >> 16) & 0x1F);
	const u32 fd = ((code >> 6) & 0x1F);
	const u32 funct = (code & 0x3F);

	constexpr u32 VF_ACC = 32;
	constexpr u32 VF_I = 33;

	switch (fmt)
	{
		case 1: // qmfc2
			recBackpropSetGPRWrite128(rt);
			recBackpropSetVFRead(fs);
			break;

		case 2: // cfc1
			recBackpropSetGPRWrite(rt);
			recBackpropSetVIRead(fs);
			break;

		case 5: // qmtc2
			recBackpropSetVFWrite(fs);
			recBackpropSetGPRRead128(rt);
			break;

		case 6: // ctc2
			recBackpropSetVIWrite(fs);
			recBackpropSetGPRRead(rt);
			break;

		case 8: // bc2f/bc2t/bc2fl/bc2tl
			// read vi[29]
			break;

		case 16: // SPEC1
		case 17:
		case 18:
		case 19:
		case 20:
		case 21:
		case 22:
		case 23:
		case 24:
		case 25:
		case 26:
		case 27:
		case 28:
		case 29:
		case 30:
		case 31: // SPEC1
		{
			switch (funct)
			{
				case 0: // VADDx
				case 1: // VADDy
				case 2: // VADDz
				case 3: // VADDw
				case 4: // VSUBx
				case 5: // VSUBy
				case 6: // VSUBz
				case 7: // VSUBw
				case 16: // VMAXx
				case 17: // VMAXy
				case 18: // VMAXz
				case 19: // VMAXw
				case 20: // VMINIx
				case 21: // VMINIy
				case 22: // VMINIz
				case 23: // VMINIw
				case 24: // VMULx
				case 25: // VMULy
				case 26: // VMULz
				case 27: // VMULw
				case 40: // VADD
				case 42: // VMUL
				case 43: // VMAX
				case 44: // VSUB
				case 47: // VMINI
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(ft);
					recBackpropSetVFRead(fd); // unnecessary if _X_Y_Z_W == 0xF
					break;

				case 8: // VMADDx
				case 9: // VMADDy
				case 10: // VMADDz
				case 11: // VMADDw
				case 12: // VMSUBx
				case 13: // VMSUBy
				case 14: // VMSUBz
				case 15: // VMSUBw
				case 41: // VMADD
				case 45: // VMSUB
				case 46: // VOPMSUB
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(ft);
					recBackpropSetVFRead(VF_ACC);
					recBackpropSetVFRead(fd);
					break;

				case 29: // VMAXi
				case 30: // VMULi
				case 31: // VMINIi
				case 34: // VADDi
				case 38: // VSUBi
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_I);
					break;

				case 35: // VMADDi
				case 39: // VMSUBi
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_ACC);
					recBackpropSetVFRead(VF_I);
					break;

				case 28: // VMULq
				case 32: // VADDq
				case 36: // VSUBq
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					break;

				case 33: // VMADDq
				case 37: // VMSUBq
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_ACC);
					break;

				case 48: // VIADD
				case 49: // VISUB
				case 50: // VIADDI
				case 52: // VIAND
				case 53: // VIOR
				{
					const u32 is = fs & 0xFu;
					const u32 it = ft & 0xFu;
					const u32 id = fd & 0xFu;
					recBackpropSetVIWrite(id);
					recBackpropSetVIRead(is);
					recBackpropSetVIRead(it);
					recBackpropSetVIRead(id);
				}
				break;


				case 56: // VCALLMS
				case 57: // VCALLMSR
					break;

				case 60: // COP2_SPEC2
				case 61: // COP2_SPEC2
				case 62: // COP2_SPEC2
				case 63: // COP2_SPEC2
				{
					const u32 idx = (code & 3u) | ((code >> 4) & 0x7cu);
					switch (idx)
					{
						case 0: // VADDAx
						case 1: // VADDAy
						case 2: // VADDAz
						case 3: // VADDAw
						case 4: // VSUBAx
						case 5: // VSUBAy
						case 6: // VSUBAz
						case 7: // VSUBAw
						case 24: // VMULAx
						case 25: // VMULAy
						case 26: // VMULAz
						case 27: // VMULAw
						case 40: // VADDA
						case 42: // VMULA
						case 44: // VSUBA
						case 8: // VMADDAx
						case 9: // VMADDAy
						case 10: // VMADDAz
						case 11: // VMADDAw
						case 12: // VMSUBAx
						case 13: // VMSUBAy
						case 14: // VMSUBAz
						case 15: // VMSUBAw
						case 41: // VMADDA
						case 45: // VMSUBA
						case 46: // VOPMULA
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 16: // VITOF0
						case 17: // VITOF4
						case 18: // VITOF12
						case 19: // VITOF15
						case 20: // VFTOI0
						case 21: // VFTOI4
						case 22: // VFTOI12
						case 23: // VFTOI15
						case 29: // VABS
						case 48: // VMOVE
						case 49: // VMR32
							recBackpropSetVFWrite(ft);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							break;

						case 31: // VCLIP
							recBackpropSetVFRead(fs);
							// Write CLIP
							break;

						case 30: // VMULAi
						case 34: // VADDAi
						case 38: // VSUBAi
						case 35: // VMADDAi
						case 39: // VMSUBAi
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_I);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 32: // VADDAq
						case 36: // VSUBAq
						case 28: // VMULAq
						case 33: // VMADDAq
						case 37: // VMSUBAq
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 52: // VLQI
						case 54: // VLQD
							recBackpropSetVFWrite(ft);
							recBackpropSetVIWrite(fs & 0xFu);
							recBackpropSetVIRead(fs & 0xFu);
							recBackpropSetVFRead(ft);
							break;

						case 53: // VSQI
						case 55: // VSQD
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVIRead(ft & 0xFu);
							recBackpropSetVFRead(fs);
							break;

						case 56: // VDIV
						case 58: // VRSQRT
							// recBackpropSetVIWrite(REG_Q);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							break;

						case 57: // VSQRT
							recBackpropSetVFRead(ft);
							break;


						case 60: // VMTIR
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVFRead(fs);
							break;

						case 61: // VMFIR
							recBackpropSetVFWrite(ft);
							recBackpropSetVIRead(fs & 0xFu);
							break;

						case 62: // VILWR
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVIRead(fs & 0xFu);
							break;

						case 63: // VISWR
							recBackpropSetVIRead(fs & 0xFu);
							recBackpropSetVIRead(ft & 0xFu);
							break;

						case 64: // VRNEXT
						case 65: // VRGET
							recBackpropSetVFWrite(ft);
							break;

						case 66: // VRINIT
						case 67: // VRXOR
							recBackpropSetVFRead(fs);
							break;

						case 47: // VNOP
						case 59: // VWAITQ
						default:
							break;
					}
				}
				break;

				default:
					break;
			}
		}
		break;

		default:
			break;
	}
}

void recBackpropMMI(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 funct = (code & 0x3F);
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 rd = ((code >> 11) & 0x1F);

	switch (funct)
	{
		case 0: // madd
		case 1: // maddu
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead(XMMGPR_LO);
			recBackpropSetGPRRead(XMMGPR_HI);
			break;

		case 32: // madd1
		case 33: // maddu1
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRRead128(XMMGPR_HI);
			break;

		case 24: // mult1
		case 25: // multu1
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRWrite(rd);
			break;

		case 26: // div1
		case 27: // divu1
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 16: // mfhi1
			recBackpropSetGPRRead128(XMMGPR_HI);
			recBackpropSetGPRWrite(rd);
			break;

		case 17: // mthi1
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			break;

		case 18: // mflo1
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRWrite(rd);
			break;

		case 19: // mtlo1
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRRead(rs);
			break;

		case 4: // plzcw
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRWrite(rd);
			break;

		case 48: // pmfhl
			recBackpropSetGPRPartialWrite128(rd);
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRRead128(XMMGPR_HI);
			break;

		case 49: // pmthl
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead128(rs);
			break;

		case 52: // psllh
		case 54: // psrlh
		case 55: // psrah
		case 60: // psllw
		case 62: // psrlw
		case 63: // psraw
			recBackpropSetGPRWrite128(rd);
			recBackpropSetGPRRead128(rt);
			break;

		case 8: // mmi0
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0: // PADDW
				case 1: // PSUBW
				case 2: // PCGTW
				case 3: // PMAXW
				case 4: // PADDH
				case 5: // PSUBH
				case 6: // PCGTH
				case 7: // PMAXH
				case 8: // PADDB
				case 9: // PSUBB
				case 10: // PCGTB
				case 16: // PADDSW
				case 17: // PSUBSW
				case 18: // PEXTLW
				case 19: // PPACW
				case 20: // PADDSH
				case 21: // PSUBSH
				case 22: // PEXTLH
				case 23: // PPACH
				case 24: // PADDSB
				case 25: // PSUBSB
				case 26: // PEXTLB
				case 27: // PPACB
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 30: // PEXT5
				case 31: // PPAC5
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					break;
			}
		}
		break;

		case 40: // mmi1
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 2: // PCEQW
				case 3: // PMINW
				case 4: // PADSBH
				case 6: // PCEQH
				case 7: // PMINH
				case 10: // PCEQB
				case 16: // PADDUW
				case 17: // PSUBUW
				case 18: // PEXTUW
				case 20: // PADDUH
				case 21: // PSUBUH
				case 22: // PEXTUH
				case 24: // PADDUB
				case 25: // PSUBUB
				case 26: // PEXTUB
				case 27: // QFSRV
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 1: // PABSW
				case 5: // PABSH
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				case 0: // MMI_Unknown
				default:
					break;
			}
		}
		break;

		case 9: // mmi2
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0: // PMADDW
				case 4: // PMSUBW
				case 16: // PMADDH
				case 17: // PHMADH
				case 20: // PMSUBH
				case 21: // PHMSBH
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					recBackpropSetGPRRead128(XMMGPR_LO);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 12: // PMULTW
				case 28: // PMULTH
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 13: // PDIVW
				case 29: // PDIVBW
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 2: // PSLLVW
				case 3: // PSRLVW
				case 10: // PINTH
				case 14: // PCPYLD
				case 18: // PAND
				case 19: // PXOR
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 8: // PMFHI
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(XMMGPR_LO);
					break;

				case 9: // PMFLO
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 26: // PEXEH
				case 27: // PREVH
				case 30: // PEXEW
				case 31: // PROT3W
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					break;
			}
		}
		break;

		case 41: // mmi3
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0: // PMADDUW
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					recBackpropSetGPRRead128(XMMGPR_LO);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 3: // PSRAVW
				case 10: // PINTEH
				case 18: // POR
				case 19: // PNOR
				case 14: // PCPYUD
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 26: // PEXCH
				case 27: // PCPYH
				case 30: // PEXCW
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				case 8: // PMTHI
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					break;

				case 9: // PMTLO
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRRead128(rs);
					break;

				case 12: // PMULTUW
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 13: // PDIVUW
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					break;
			}
		}
		break;

		default:
			break;
	}
}

using namespace vtlb_private;
using namespace x86Emitter;

// yay sloppy crap needed until we can remove dependency on this hippopotamic
// landmass of shared code. (air)
extern u32 g_psxConstRegs[32];

// X86 caching
static uint g_x86checknext;

static bool eeRecNeedsReset = false;
static bool eeCpuExecuting = false;
static bool eeRecExitRequested = false;
static bool g_resetEeScalingStats = false;

static u32 maxrecmem = 0;
alignas(16) static uintptr_t recLUT[_64kb];
alignas(16) static u32 hwLUT[_64kb];

static u32 s_nBlockCycles = 0; // cycles of current block recompiling
bool s_nBlockInterlocked = false; // Block is VU0 interlocked
u32 pc; // recompiler pc
int g_branch; // set for branch

alignas(16) GPR_reg64 g_cpuConstRegs[32] = {};
u32 g_cpuHasConstReg = 0, g_cpuFlushedConstReg = 0;
bool g_cpuFlushedPC, g_cpuFlushedCode, g_recompilingDelaySlot, g_maySignalException;

static int RETURN_READ_IN_RAX(void) { return rax.Id; }

////////////////////
// Code Templates //
////////////////////

void _eeOnWriteReg(int reg, int signext)
{
	GPR_DEL_CONST(reg);
}

void _deleteEEreg(int reg, int flush)
{
	if (!reg)
		return;
	if (flush && GPR_IS_CONST1(reg))
	{
		_flushConstReg(reg);
	}
	GPR_DEL_CONST(reg);
	_deleteGPRtoXMMreg(reg, flush ? DELETE_REG_FREE : DELETE_REG_FLUSH_AND_FREE);
	_deleteGPRtoX86reg(reg, flush ? DELETE_REG_FREE : DELETE_REG_FLUSH_AND_FREE);
}

void _deleteEEreg128(int reg)
{
	if (!reg)
		return;

	GPR_DEL_CONST(reg);
	_deleteGPRtoXMMreg(reg, DELETE_REG_FREE_NO_WRITEBACK);
	_deleteGPRtoX86reg(reg, DELETE_REG_FREE_NO_WRITEBACK);
}

void _flushEEreg(int reg, bool clear)
{
	if (!reg)
		return;

	if (GPR_IS_DIRTY_CONST(reg))
		_flushConstReg(reg);
	if (clear)
		GPR_DEL_CONST(reg);

	_deleteGPRtoXMMreg(reg, clear ? DELETE_REG_FLUSH_AND_FREE : DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(reg, clear ? DELETE_REG_FLUSH_AND_FREE : DELETE_REG_FLUSH);
}

int _eeTryRenameReg(int to, int from, int fromx86, int other, int xmminfo)
{
	// can't rename when in form Rd = Rs op Rt and Rd == Rs or Rd == Rt
	if ((xmminfo & XMMINFO_NORENAME) || fromx86 < 0 || to == from || to == other || !EEINST_RENAMETEST(from))
		return -1;

	// flush back when it's been modified
	if (x86regs[fromx86].mode & MODE_WRITE && EEINST_LIVETEST(from))
		_writebackX86Reg(fromx86);

	// remove all references to renamed-to register
	_deleteGPRtoX86reg(to, DELETE_REG_FREE_NO_WRITEBACK);
	_deleteGPRtoXMMreg(to, DELETE_REG_FLUSH_AND_FREE);
	GPR_DEL_CONST(to);

	// and do the actual rename, new register has been modified.
	x86regs[fromx86].reg = to;
	x86regs[fromx86].mode |= MODE_READ | MODE_WRITE;
	return fromx86;
}

static bool FitsInImmediate(int reg, int fprinfo)
{
	if (fprinfo & XMMINFO_64BITOP)
		return (s32)g_cpuConstRegs[reg].SD[0] == g_cpuConstRegs[reg].SD[0];
	return true; // all 32bit ops fit
}

void eeRecompileCodeRC0(R5900FNPTR constcode, R5900FNPTR_INFO constscode, R5900FNPTR_INFO consttcode, R5900FNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_ && (xmminfo & XMMINFO_WRITED))
		return;

	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		if (_Rd_ && (xmminfo & XMMINFO_WRITED))
		{
			_deleteGPRtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
			_deleteGPRtoXMMreg(_Rd_, DELETE_REG_FLUSH_AND_FREE);
			GPR_SET_CONST(_Rd_);
		}
		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const bool s_is_const = GPR_IS_CONST1(_Rs_);
	const bool t_is_const = GPR_IS_CONST1(_Rt_);
	const bool d_is_const = GPR_IS_CONST1(_Rd_);
	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const bool t_is_used = EEINST_USEDTEST(_Rt_);
	const bool s_in_xmm = _hasXMMreg(XMMTYPE_GPRREG, _Rs_);
	const bool t_in_xmm = _hasXMMreg(XMMTYPE_GPRREG, _Rt_);

	// regular x86
	if ((xmminfo & XMMINFO_READS) && !s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if ((xmminfo & XMMINFO_READT) && !t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if ((xmminfo & XMMINFO_READD) && !d_is_const)
		_addNeededGPRtoX86reg(_Rd_);

	// when it doesn't fit in an immediate, we'll flush it to a reg early to save code
	u32 info = 0;
	int regs = -1, regt = -1;
	if (xmminfo & XMMINFO_READS)
	{
		regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		if (regs < 0 && (!s_is_const || !FitsInImmediate(_Rs_, xmminfo)) && (s_is_used || s_in_xmm || ((xmminfo & XMMINFO_WRITED) && _Rd_ == _Rs_) || (xmminfo & XMMINFO_FORCEREGS)))
			regs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		if (regs >= 0)
			info |= PROCESS_EE_SET_S(regs);
	}

	if (xmminfo & XMMINFO_READT)
	{
		regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		if (regt < 0 && (!t_is_const || !FitsInImmediate(_Rt_, xmminfo)) && (t_is_used || t_in_xmm || ((xmminfo & XMMINFO_WRITED) && _Rd_ == _Rt_) || (xmminfo & XMMINFO_FORCEREGT)))
			regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		if (regt >= 0)
			info |= PROCESS_EE_SET_T(regt);
	}

	if (xmminfo & (XMMINFO_WRITED | XMMINFO_READD))
	{
		// _eeTryRenameReg() sets READ | WRITE already, so this is only needed when allocating.
		const int moded = ((xmminfo & XMMINFO_WRITED) ? MODE_WRITE : 0) | ((xmminfo & XMMINFO_READD) ? MODE_READ : 0);

		// If S is no longer live, swap D for S. Saves the move.
		int regd = (_Rd_ && xmminfo & XMMINFO_WRITED) ? _eeTryRenameReg(_Rd_, (xmminfo & XMMINFO_READS) ? _Rs_ : 0, regs, (xmminfo & XMMINFO_READT) ? _Rt_ : 0, xmminfo) : 0;
		if (regd < 0)
			regd = _allocX86reg(X86TYPE_GPR, _Rd_, moded);

		info |= PROCESS_EE_SET_D(regd);
	}

	if (xmminfo & XMMINFO_WRITED)
		GPR_DEL_CONST(_Rd_);

	if (s_is_const && regs < 0)
	{
		constscode(info /*| PROCESS_CONSTS*/);
		return;
	}

	if (t_is_const && regt < 0)
	{
		consttcode(info /*| PROCESS_CONSTT*/);
		return;
	}

	noconstcode(info);
}

void eeRecompileCodeRC1(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rt_)
		return;

	if (GPR_IS_CONST1(_Rs_))
	{
		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		_deleteGPRtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
		GPR_SET_CONST(_Rt_);
		constcode();
		return;
	}

	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const bool s_in_xmm = _hasXMMreg(XMMTYPE_GPRREG, _Rs_);

	u32 info = 0;
	int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	if (regs < 0 && (s_is_used || s_in_xmm || _Rt_ == _Rs_ || (xmminfo & XMMINFO_FORCEREGS)))
		regs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	// If S is no longer live, swap D for S. Saves the move.
	int regt = _eeTryRenameReg(_Rt_, _Rs_, regs, 0, xmminfo);
	if (regt < 0)
		regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);

	info |= PROCESS_EE_SET_T(regt);

	GPR_DEL_CONST(_Rt_);
	noconstcode(info);
}

// rd = rt op sa
void eeRecompileCodeRC2(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_))
	{
		_deleteGPRtoXMMreg(_Rd_, DELETE_REG_FLUSH_AND_FREE);
		_deleteGPRtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		GPR_SET_CONST(_Rd_);
		constcode();
		return;
	}

	const bool t_is_used = EEINST_USEDTEST(_Rt_);
	const bool t_in_xmm = _hasXMMreg(XMMTYPE_GPRREG, _Rt_);

	u32 info = 0;
	int regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	if (regt < 0 && (t_is_used || t_in_xmm || (_Rd_ == _Rt_) || (xmminfo & XMMINFO_FORCEREGT)))
		regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	// If S is no longer live, swap D for T. Saves the move.
	int regd = _eeTryRenameReg(_Rd_, _Rt_, regt, 0, xmminfo);
	if (regd < 0)
		regd = _allocX86reg(X86TYPE_GPR, _Rd_, MODE_WRITE);

	info |= PROCESS_EE_SET_D(regd);

	GPR_DEL_CONST(_Rd_);
	noconstcode(info);
}

// EE XMM allocation code
int eeRecompileCodeXMM(int xmminfo)
{
	int info = PROCESS_EE_XMM;

	// add needed
	if (xmminfo & (XMMINFO_READLO | XMMINFO_WRITELO))
		_addNeededGPRtoXMMreg(XMMGPR_LO);
	if (xmminfo & (XMMINFO_READHI | XMMINFO_WRITEHI))
		_addNeededGPRtoXMMreg(XMMGPR_HI);

	// TODO: we could do memory operands here if not live. but the MMI implementations aren't hooked up to that at the moment.
	if (xmminfo & XMMINFO_READS)
	{
		_addNeededGPRtoXMMreg(_Rs_);
		const int reg = _allocGPRtoXMMreg(_Rs_, MODE_READ);
		info |= PROCESS_EE_SET_S(reg);
	}
	if (xmminfo & XMMINFO_READT)
	{
		_addNeededGPRtoXMMreg(_Rt_);
		const int reg = _allocGPRtoXMMreg(_Rt_, MODE_READ);
		info |= PROCESS_EE_SET_T(reg);
	}

	if (xmminfo & XMMINFO_WRITED)
	{
		_addNeededGPRtoXMMreg(_Rd_);
		int readd = MODE_WRITE | ((xmminfo & XMMINFO_READD) ? MODE_READ : 0);

		int regd = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, readd);

		if (regd < 0)
		{
			if (!(xmminfo & XMMINFO_READD) && (xmminfo & XMMINFO_READT) && EEINST_RENAMETEST(_Rt_))
			{
				_deleteEEreg128(_Rd_);
				_reallocateXMMreg(EEREC_T, XMMTYPE_GPRREG, _Rd_, readd, EEINST_LIVETEST(_Rt_));
				regd = EEREC_T;
			}
			else if (!(xmminfo & XMMINFO_READD) && (xmminfo & XMMINFO_READS) && EEINST_RENAMETEST(_Rs_))
			{
				_deleteEEreg128(_Rd_);
				_reallocateXMMreg(EEREC_S, XMMTYPE_GPRREG, _Rd_, readd, EEINST_LIVETEST(_Rs_));
				regd = EEREC_S;
			}
			else
			{
				regd = _allocGPRtoXMMreg(_Rd_, readd);
			}
		}

		info |= PROCESS_EE_SET_D(regd);
	}
	if (xmminfo & (XMMINFO_READLO | XMMINFO_WRITELO))
	{
		info |= PROCESS_EE_SET_LO(_allocGPRtoXMMreg(XMMGPR_LO, ((xmminfo & XMMINFO_READLO) ? MODE_READ : 0) | ((xmminfo & XMMINFO_WRITELO) ? MODE_WRITE : 0)));
	}
	if (xmminfo & (XMMINFO_READHI | XMMINFO_WRITEHI))
	{
		info |= PROCESS_EE_SET_HI(_allocGPRtoXMMreg(XMMGPR_HI, ((xmminfo & XMMINFO_READHI) ? MODE_READ : 0) | ((xmminfo & XMMINFO_WRITEHI) ? MODE_WRITE : 0)));
	}

	if (xmminfo & XMMINFO_WRITED)
		GPR_DEL_CONST(_Rd_);

	return info;
}

// EE COP1(FPU) XMM allocation code
#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

// rd = rs op rt
void eeFPURecompileCode(R5900FNPTR_INFO xmmcode, R5900FNPTR fpucode, int xmminfo)
{
	int mmregs = -1, mmregt = -1, mmregd = -1, mmregacc = -1;
	int info = PROCESS_EE_XMM;

	if (xmminfo & (XMMINFO_WRITED | XMMINFO_READD))
		_addNeededFPtoXMMreg(_Fd_);
	if (xmminfo & (XMMINFO_WRITEACC | XMMINFO_READACC))
		_addNeededFPACCtoXMMreg();

	if (xmminfo & XMMINFO_READT)
	{
		_addNeededFPtoXMMreg(_Ft_);
		if (g_pCurInstInfo->fpuregs[_Ft_] & EEINST_LASTUSE)
			mmregt = _checkXMMreg(XMMTYPE_FPREG, _Ft_, MODE_READ);
		else
			mmregt = _allocFPtoXMMreg(_Ft_, MODE_READ);
	}

	if (xmminfo & XMMINFO_READS)
	{
		_addNeededFPtoXMMreg(_Fs_);
		if ((!(xmminfo & XMMINFO_READT) || (mmregt >= 0)) && (g_pCurInstInfo->fpuregs[_Fs_] & EEINST_LASTUSE))
			mmregs = _checkXMMreg(XMMTYPE_FPREG, _Fs_, MODE_READ);
		else
		{
			mmregs = _allocFPtoXMMreg(_Fs_, MODE_READ);

			// if we just allocated S and Fs == Ft, share it
			if ((xmminfo & XMMINFO_READT) && _Fs_ == _Ft_)
				mmregt = mmregs;
		}
	}

	if (xmminfo & XMMINFO_READD)
		mmregd = _allocFPtoXMMreg(_Fd_, MODE_READ);

	if (xmminfo & XMMINFO_READACC)
	{
		if (!(xmminfo & XMMINFO_WRITEACC) && (g_pCurInstInfo->fpuregs[XMMFPU_ACC] & EEINST_LASTUSE))
			mmregacc = _checkXMMreg(XMMTYPE_FPACC, 0, MODE_READ);
		else
			mmregacc = _allocFPACCtoXMMreg(MODE_READ);
	}

	if (xmminfo & XMMINFO_WRITEACC)
	{

		// check for last used, if so don't alloc a new XMM reg
		int readacc = MODE_WRITE | ((xmminfo & XMMINFO_READACC) ? MODE_READ : 0);

		mmregacc = _checkXMMreg(XMMTYPE_FPACC, 0, readacc);

		if (mmregacc < 0)
		{
			if ((xmminfo & XMMINFO_READT) && mmregt >= 0 && FPUINST_RENAMETEST(_Ft_))
			{
				if (EE_WRITE_DEAD_VALUES && xmmregs[mmregt].mode & MODE_WRITE)
					_writebackXMMreg(mmregt);

				xmmregs[mmregt].reg = 0;
				xmmregs[mmregt].mode = readacc;
				xmmregs[mmregt].type = XMMTYPE_FPACC;
				mmregacc = mmregt;
			}
			else if ((xmminfo & XMMINFO_READS) && mmregs >= 0 && FPUINST_RENAMETEST(_Fs_))
			{
				if (EE_WRITE_DEAD_VALUES && xmmregs[mmregs].mode & MODE_WRITE)
					_writebackXMMreg(mmregs);

				xmmregs[mmregs].reg = 0;
				xmmregs[mmregs].mode = readacc;
				xmmregs[mmregs].type = XMMTYPE_FPACC;
				mmregacc = mmregs;
			}
			else
				mmregacc = _allocFPACCtoXMMreg(readacc);
		}

		xmmregs[mmregacc].mode |= MODE_WRITE;
	}
	else if (xmminfo & XMMINFO_WRITED)
	{
		// check for last used, if so don't alloc a new XMM reg
		int readd = MODE_WRITE | ((xmminfo & XMMINFO_READD) ? MODE_READ : 0);
		if (xmminfo & XMMINFO_READD)
			mmregd = _allocFPtoXMMreg(_Fd_, readd);
		else
			mmregd = _checkXMMreg(XMMTYPE_FPREG, _Fd_, readd);

		if (mmregd < 0)
		{
			if ((xmminfo & XMMINFO_READT) && mmregt >= 0 && FPUINST_RENAMETEST(_Ft_))
			{
				if (EE_WRITE_DEAD_VALUES && xmmregs[mmregt].mode & MODE_WRITE)
					_writebackXMMreg(mmregt);

				xmmregs[mmregt].reg = _Fd_;
				xmmregs[mmregt].mode = readd;
				mmregd = mmregt;
			}
			else if ((xmminfo & XMMINFO_READS) && mmregs >= 0 && FPUINST_RENAMETEST(_Fs_))
			{
				if (EE_WRITE_DEAD_VALUES && xmmregs[mmregs].mode & MODE_WRITE)
					_writebackXMMreg(mmregs);

				xmmregs[mmregs].inuse = 1;
				xmmregs[mmregs].reg = _Fd_;
				xmmregs[mmregs].mode = readd;
				mmregd = mmregs;
			}
			else if ((xmminfo & XMMINFO_READACC) && mmregacc >= 0 && FPUINST_RENAMETEST(XMMFPU_ACC))
			{
				if (EE_WRITE_DEAD_VALUES && xmmregs[mmregacc].mode & MODE_WRITE)
					_writebackXMMreg(mmregacc);

				xmmregs[mmregacc].reg = _Fd_;
				xmmregs[mmregacc].mode = readd;
				xmmregs[mmregacc].type = XMMTYPE_FPREG;
				mmregd = mmregacc;
			}
			else
				mmregd = _allocFPtoXMMreg(_Fd_, readd);
		}
	}

	if (xmminfo & XMMINFO_WRITED)
		info |= PROCESS_EE_SET_D(mmregd);
	if (xmminfo & (XMMINFO_WRITEACC | XMMINFO_READACC))
	{
		if (mmregacc >= 0)
			info |= PROCESS_EE_SET_ACC(mmregacc) | PROCESS_EE_ACC;
	}

	if (xmminfo & XMMINFO_READS)
	{
		if (mmregs >= 0)
			info |= PROCESS_EE_SET_S(mmregs);
	}
	if (xmminfo & XMMINFO_READT)
	{
		if (mmregt >= 0)
			info |= PROCESS_EE_SET_T(mmregt);
	}

	xmmcode(info);
}

// we need enough for a 32-bit jump forwards (5 bytes)
#define LOADSTORE_PADDING 5

static u32 GetAllocatedGPRBitmask(void)
{
	u32 mask = 0;
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

static u32 GetAllocatedXMMBitmask(void)
{
	u32 mask = 0;
	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

/*
	// Pseudo-Code For the following Dynarec Implementations -->

	u32 vmv = vmap[addr>>VTLB_PAGE_BITS].raw();
	intptr_t ppf=addr+vmv;
	if (!(ppf<0))
	{
		data[0]=*reinterpret_cast<DataType*>(ppf);
		if (DataSize==128)
			data[1]=*reinterpret_cast<DataType*>(ppf+8);
		return 0;
	}
	else
	{
		//has to: translate, find function, call function
		u32 hand=(u8)vmv;
		u32 paddr=(ppf-hand) << 1;
		//Console.WriteLn("Translated 0x%08X to 0x%08X",params addr,paddr);
		return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);
	}

	// And in ASM it looks something like this -->

	mov eax,ecx;
	shr eax,VTLB_PAGE_BITS;
	mov rax,[rax * sizeof(intptr_t) + vmap];
	add rcx,rax;
	js _fullread;

	//these are wrong order, just an example ...
	mov [rax],ecx;
	mov ecx,[rdx];
	mov [rax+4],ecx;
	mov ecx,[rdx+4];
	mov [rax+4+4],ecx;
	mov ecx,[rdx+4+4];
	mov [rax+4+4+4+4],ecx;
	mov ecx,[rdx+4+4+4+4];
	///....

	jmp cont;
	_fullread:
	movzx eax,al;
	sub   ecx,eax;
	call [eax+stuff];
	cont:
	........

*/

namespace vtlb_private
{
	// ------------------------------------------------------------------------
	// Prepares eax, ecx, and, ebx for Direct or Indirect operations.
	// Returns the writeback pointer for ebx (return address from indirect handling)
	//
	static void DynGen_PrepRegs(int addr_reg, int value_reg, u32 sz, bool xmm)
	{
		_freeX86reg(arg1regd);
		xMOV(arg1regd, xRegister32(addr_reg));

		if (value_reg >= 0)
		{
			if (sz == 128)
			{
				_freeXMMreg(xRegisterSSE::GetArgRegister(1, 0).Id);
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			}
			else if (xmm)
			{
				// 32bit xmms are passed in GPRs
				_freeX86reg(arg2regd);
				xMOVD(arg2regd, xRegisterSSE(value_reg));
			}
			else
			{
				_freeX86reg(arg2regd);
				xMOV(arg2reg, xRegister64(value_reg));
			}
		}

		xMOV(eax, arg1regd);
		xSHR(eax, VTLB_PAGE_BITS);
		xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.vmap, rax * sizeof(intptr_t))]);
		xADD(arg1reg, rax);
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectRead(u32 bits, bool sign)
	{
		switch (bits)
		{
			case 8:
				if (sign)
					xMOVSX(rax, ptr8[arg1reg]);
				else
					xMOVZX(rax, ptr8[arg1reg]);
				break;

			case 16:
				if (sign)
					xMOVSX(rax, ptr16[arg1reg]);
				else
					xMOVZX(rax, ptr16[arg1reg]);
				break;

			case 32:
				if (sign)
					xMOVSX(rax, ptr32[arg1reg]);
				else
					xMOV(eax, ptr32[arg1reg]);
				break;

			case 64:
				xMOV(rax, ptr64[arg1reg]);
				break;

			case 128:
				xMOVAPS(xmm0, ptr128[arg1reg]);
				break;
			default:
				break;
		}
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectWrite(u32 bits)
	{
		switch (bits)
		{
			case 8:
				xMOV(ptr[arg1reg], xRegister8(arg2regd));
				break;

			case 16:
				xMOV(ptr[arg1reg], xRegister16(arg2regd));
				break;

			case 32:
				xMOV(ptr[arg1reg], arg2regd);
				break;

			case 64:
				xMOV(ptr[arg1reg], arg2reg);
				break;

			case 128:
				xMOVAPS(ptr[arg1reg], xRegisterSSE::GetArgRegister(1, 0));
				break;
		}
	}
} // namespace vtlb_private

// ------------------------------------------------------------------------
// allocate one page for our naked indirect dispatcher function.
// this *must* be a full page, since we'll give it execution permission later.
// If it were smaller than a page we'd end up allowing execution rights on some
// other vars additionally (bad!).
//
alignas(__pagesize) static u8 m_IndirectDispatchers[__pagesize];

// ------------------------------------------------------------------------
// mode        - 0 for read, 1 for write!
// operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits.
//
static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign = 0)
{
	// Each dispatcher is aligned to 64 bytes.  The actual dispatchers are only like
	// 20-some bytes each, but 64 byte alignment on functions that are called
	// more frequently than a hot sex hotline at 1:15am is probably a good thing.

	// 7*64? 5 widths with two sign extension modes for 8 and 16 bit reads

	// Gregory: a 32 bytes alignment is likely enough and more cache friendly
	const int A = 32;
	return &m_IndirectDispatchers[(mode * (8 * A)) + (sign * 5 * A) + (operandsize * A)];
}

// ------------------------------------------------------------------------
// Generates a JS instruction that targets the appropriate templated instance of
// the vtlb Indirect Dispatcher.
//

template <typename GenDirectFn>
static void DynGen_HandlerTest(const GenDirectFn& gen_direct, int mode, int bits, bool sign = false)
{
	int szidx = 0;
	switch (bits)
	{
		case   8: szidx = 0; break;
		case  16: szidx = 1; break;
		case  32: szidx = 2; break;
		case  64: szidx = 3; break;
		case 128: szidx = 4; break;
		default:
			  break;
	}
	xForwardJS8 to_handler;
	gen_direct();
	xForwardJump8 done;
	to_handler.SetTarget();
	xFastCall(GetIndirectDispatcherPtr(mode, szidx, sign));
	done.SetTarget();
}

// ------------------------------------------------------------------------
// Generates the various instances of the indirect dispatchers
// In: arg1reg: vtlb entry, arg2reg: data ptr (if mode >= 64), rbx: function return ptr
// Out: eax: result (if mode < 64)
static void DynGen_IndirectTlbDispatcher(int mode, int bits, bool sign)
{
	// fixup stack
#ifdef _WIN32
	xSUB(rsp, 32 + 8);
#else
	xSUB(rsp, 8);
#endif

	xMOVZX(eax, al);
#if !(defined(_M_X64) || defined(_M_AMD64) || defined(__amd64__) || defined(__x86_64__) || defined(__x86_64))
	if (sizeof(intptr_t) != 8)
		xSUB(arg1regd, 0x80000000);
#endif
	xSUB(arg1regd, eax);

	// jump to the indirect handler, which is a C++ function.
	// [ecx is address, edx is data]
	intptr_t table = (intptr_t)vtlbdata.RWFT[bits][mode];
	if (table == (s32)table)
	{
		xFastCall(ptrNative[(rax * sizeof(intptr_t)) + table], arg1reg, arg2reg);
	}
	else
	{
		xLEA(arg3reg, ptr[(void*)table]);
		xFastCall(ptrNative[(rax * sizeof(intptr_t)) + arg3reg], arg1reg, arg2reg);
	}

	if (!mode)
	{
		if (bits == 0)
		{
			if (sign)
				xMOVSX(rax, al);
			else
				xMOVZX(rax, al);
		}
		else if (bits == 1)
		{
			if (sign)
				xMOVSX(rax, ax);
			else
				xMOVZX(rax, ax);
		}
		else if (bits == 2)
		{
			if (sign)
			{
				*(u16*)x86Ptr = 0x9848;
				x86Ptr += sizeof(u16);
			}
		}
	}

#ifdef _WIN32
	xADD(rsp, 32 + 8);
#else
	xADD(rsp, 8);
#endif

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

/* One-time initialization procedure.  Multiple subsequent calls during the lifespan of the
 * process will be ignored. */
void vtlb_DynGenDispatchers(void)
{
	PageProtectionMode mode;
	static bool hasBeenCalled = false;
	if (hasBeenCalled)
		return;
	hasBeenCalled = true;

	mode.m_read   = true;
	mode.m_write  = true;
	mode.m_exec   = false;
	// In case init gets called multiple times:
	HostSys::MemProtect(m_IndirectDispatchers, __pagesize, mode);

	// clear the buffer to 0xcc (easier debugging).
	memset(m_IndirectDispatchers, 0xcc, __pagesize);

	for (int mode = 0; mode < 2; ++mode)
	{
		for (int bits = 0; bits < 5; ++bits)
		{
			for (int sign = 0; sign < (!mode && bits < 3 ? 2 : 1); sign++)
			{
				x86Ptr = (u8*)GetIndirectDispatcherPtr(mode, bits, !!sign);

				DynGen_IndirectTlbDispatcher(mode, bits, !!sign);
			}
		}
	}

	mode.m_write  = false;
	mode.m_exec   = true;
	HostSys::MemProtect(m_IndirectDispatchers, __pagesize, mode);
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Load Implementations
// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
static int vtlb_DynGenReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, -1, bits, xmm);
		DynGen_HandlerTest([bits, sign]() { DynGen_DirectRead(bits, sign); }, 0, bits, sign && bits < 64);

		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			xMOV(xRegister64(x86_dest_reg), rax);
		}
		else
		{
			// we shouldn't be loading any FPRs which aren't 32bit..
			// we use MOVD here despite it being floating-point data, because we're going int->float reinterpret.
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
		}

		return x86_dest_reg;
	}

	const u8* codeStart;
	const xAddressReg x86addr(addr_reg);
	if (!xmm)
	{
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
		codeStart = x86Ptr;
		const xRegister64 x86reg(x86_dest_reg);
		switch (bits)
		{
		case 8:
			sign ? xMOVSX(x86reg, ptr8[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr8[RFASTMEMBASE + x86addr]);
			break;
		case 16:
			sign ? xMOVSX(x86reg, ptr16[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr16[RFASTMEMBASE + x86addr]);
			break;
		case 32:
			sign ? xMOVSX(x86reg, ptr32[RFASTMEMBASE + x86addr]) : xMOV(xRegister32(x86reg), ptr32[RFASTMEMBASE + x86addr]);
			break;
		case 64:
			xMOV(x86reg, ptr64[RFASTMEMBASE + x86addr]);
			break;
		default:
			break;
		}
	}
	else
	{
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		codeStart = x86Ptr;
		const xRegisterSSE xmmreg(x86_dest_reg);
		xMOVSSZX(xmmreg, ptr32[RFASTMEMBASE + x86addr]);
	}

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
	{
		*(u8*)x86Ptr = 0x90;
		x86Ptr += sizeof(u8);
	}

	vtlb_AddLoadStoreInfo((uintptr_t)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(x86_dest_reg),
		static_cast<u8>(bits), sign, true, xmm);

	return x86_dest_reg;
}

static int vtlb_DynGenReadNonQuad64_Const(u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr_const))
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		// Shortcut for the INTC_STAT register, which many games like to spin on heavily.
		iFlushCall(FLUSH_FULLVTLB);
		xFastCall(vmv.assumeHandlerGetRaw(3, false), paddr);

		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
		xMOV(xRegister64(x86_dest_reg), rax);
	}
	else
	{
		auto ppf = vmv.assumePtr(addr_const);
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
		xMOV(xRegister64(x86_dest_reg), ptr64[(u64*)ppf]);
	}

	return x86_dest_reg;
}

static int vtlb_DynGenReadNonQuad32_Const(u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (vmv.isHandler(addr_const))
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		// Shortcut for the INTC_STAT register, which many games like to spin on heavily.
		if (!EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			xMOVDZX(xRegisterSSE(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
		}
		else
		{
			iFlushCall(FLUSH_FULLVTLB);
			xFastCall(vmv.assumeHandlerGetRaw(2, false), paddr);
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
		}
	}
	else
	{
		auto ppf = vmv.assumePtr(addr_const);
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		xMOVSSZX(xRegisterSSE(x86_dest_reg), ptr32[(float*)ppf]);
	}

	return x86_dest_reg;
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
//
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
//
static int vtlb_DynGenReadNonQuad_Const(u32 bits, bool sign, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
		switch (bits)
		{
			case 8:
				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr8[(u8*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr8[(u8*)ppf]);
				break;

			case 16:
				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr16[(u16*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr16[(u16*)ppf]);
				break;

			case 32:
				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr32[(u32*)ppf]) : xMOV(xRegister32(x86_dest_reg), ptr32[(u32*)ppf]);
				break;

			case 64:
				xMOV(xRegister64(x86_dest_reg), ptr64[(u64*)ppf]);
				break;
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case  8: break;
			case 16: szidx = 1; break;
			case 32: szidx = 2; break;
			case 64: szidx = 3; break;
		}

		// Shortcut for the INTC_STAT register, which many games like to spin on heavily.
		if ((bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			if (sign)
				xMOVSX(xRegister64(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
			else
				xMOV(xRegister32(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
		}
		else
		{
			iFlushCall(FLUSH_FULLVTLB);
			xFastCall(vmv.assumeHandlerGetRaw(szidx, false), paddr);

			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			switch (bits)
			{
				// save REX prefix by using 32bit dest for zext
				case 8:
					sign ? xMOVSX(xRegister64(x86_dest_reg), al) : xMOVZX(xRegister32(x86_dest_reg), al);
					break;

				case 16:
					sign ? xMOVSX(xRegister64(x86_dest_reg), ax) : xMOVZX(xRegister32(x86_dest_reg), ax);
					break;

				case 32:
					sign ? xMOVSX(xRegister64(x86_dest_reg), eax) : xMOV(xRegister32(x86_dest_reg), eax);
					break;

				case 64:
					xMOV(xRegister64(x86_dest_reg), rax);
					break;
			}
		}
	}

	return x86_dest_reg;
}

int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(arg1regd.Id, -1, bits, true);
		DynGen_HandlerTest([bits]() {DynGen_DirectRead(bits, false); },  0, bits);

		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
		if (reg >= 0)
			xMOVAPS(xRegisterSSE(reg), xmm0);

		return reg;
	}

	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
	const u8* codeStart = x86Ptr;

	xMOVAPS(xRegisterSSE(reg), ptr128[RFASTMEMBASE + arg1reg]);

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
	{
		*(u8*)x86Ptr = 0x90;
		x86Ptr += sizeof(u8);
	}

	vtlb_AddLoadStoreInfo((uintptr_t)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(arg1reg.Id), static_cast<u8>(reg),
		static_cast<u8>(bits), false, true, true);

	return reg;
}


// ------------------------------------------------------------------------
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
int vtlb_DynGenReadQuad_Const(u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		void* ppf = reinterpret_cast<void*>(vmv.assumePtr(addr_const));
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0)
			xMOVAPS(xRegisterSSE(reg), ptr128[ppf]);
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		iFlushCall(FLUSH_FULLVTLB);
		xFastCall(vmv.assumeHandlerGetRaw(4, 0), paddr);

		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		xMOVAPS(xRegisterSSE(reg), xmm0);
	}

	return reg;
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Store Implementations

void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg)
{
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, value_reg, sz, xmm);
		DynGen_HandlerTest([sz]() { DynGen_DirectWrite(sz); }, 1, sz);
		return;
	}

	const u8* codeStart = x86Ptr;

	const xAddressReg vaddr_reg(addr_reg);
	if (!xmm)
	{
		switch (sz)
		{
		case 8:
			xMOV(ptr8[RFASTMEMBASE + vaddr_reg], xRegister8(xRegister32(value_reg)));
			break;
		case 16:
			xMOV(ptr16[RFASTMEMBASE + vaddr_reg], xRegister16(value_reg));
			break;
		case 32:
			xMOV(ptr32[RFASTMEMBASE + vaddr_reg], xRegister32(value_reg));
			break;
		case 64:
			xMOV(ptr64[RFASTMEMBASE + vaddr_reg], xRegister64(value_reg));
			break;
		default:
			break;
		}
	}
	else
	{
		switch (sz)
		{
		case 32:
			xMOVSS(ptr32[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
			break;
		case 128:
			xMOVAPS(ptr128[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
			break;
		default:
			break;
		}
	}

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
	{
		*(u8*)x86Ptr = 0x90;
		x86Ptr += sizeof(u8);
	}

	vtlb_AddLoadStoreInfo((uintptr_t)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(value_reg),
		static_cast<u8>(sz), false, false, xmm);
}


// ------------------------------------------------------------------------
// Generates code for a store instruction, where the address is a known constant.
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg)
{
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
		if (!xmm)
		{
			switch (bits)
			{
				case 8:
					xMOV(ptr[(void*)ppf], xRegister8(xRegister32(value_reg)));
					break;

				case 16:
					xMOV(ptr[(void*)ppf], xRegister16(value_reg));
					break;

				case 32:
					xMOV(ptr[(void*)ppf], xRegister32(value_reg));
					break;

				case 64:
					xMOV(ptr64[(void*)ppf], xRegister64(value_reg));
					break;
				default:
					break;
			}
		}
		else
		{
			switch (bits)
			{
				case 32:
					xMOVSS(ptr[(void*)ppf], xRegisterSSE(value_reg));
					break;

				case 128:
					xMOVAPS(ptr128[(void*)ppf], xRegisterSSE(value_reg));
					break;
				default:
					break;
			}
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
			case 128:
				szidx = 4;
				break;
		}

		iFlushCall(FLUSH_FULLVTLB);

		_freeX86reg(arg1regd);
		xMOV(arg1regd, paddr);
		if (bits == 128)
		{
			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
			_freeXMMreg(argreg.Id);
			xMOVAPS(argreg, xRegisterSSE(value_reg));
		}
		else if (xmm)
		{
			_freeX86reg(arg2regd);
			xMOVD(arg2regd, xRegisterSSE(value_reg));
		}
		else
		{
			_freeX86reg(arg2regd);
			xMOV(arg2reg, xRegister64(value_reg));
		}

		xFastCall(vmv.assumeHandlerGetRaw(szidx, true));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//							Extra Implementations

//   ecx - virtual address
//   Returns physical address in eax.
//   Clobbers edx
#define vtlb_DynV2P() \
	xMOV(eax, ecx); \
	xAND(ecx, VTLB_PAGE_MASK); /* vaddr & VTLB_PAGE_MASK */ \
	xSHR(eax, VTLB_PAGE_BITS); \
	xMOV(eax, ptr[xComplexAddress(rdx, vtlbdata.ppmap, rax * 4)]); /* vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS]; */ \
	xOR(eax, ecx) \

void vtlb_DynBackpatchLoadStore(uintptr_t code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register,
	u8 size_in_bits, bool is_signed, bool is_load, bool is_xmm)
{
	static constexpr u32 GPR_SIZE = 8;
	static constexpr u32 XMM_SIZE = 16;

	// on win32, we need to reserve an additional 32 bytes shadow space when calling out to C
#ifdef _WIN32
	static constexpr u32 SHADOW_SIZE = 32;
#else
	static constexpr u32 SHADOW_SIZE = 0;
#endif
	u8* thunk = recBeginThunk();

	// save regs
	u32 num_gprs = 0;
	u32 num_fprs = 0;

	const u32 rbxid = static_cast<u32>(rbx.Id);
	const u32 arg1id = static_cast<u32>(arg1reg.Id);
	const u32 arg2id = static_cast<u32>(arg2reg.Id);
	const u32 arg3id = static_cast<u32>(arg3reg.Id);

	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if ((gpr_bitmask & (1u << i)) && (i == rbxid || i == arg1id || i == arg2id || Register_IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			num_gprs++;
	}
	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (fpr_bitmask & (1u << i) && RegisterSSE_IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			num_fprs++;
	}

	const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE) + (num_fprs * XMM_SIZE) + SHADOW_SIZE;

	if (stack_size > 0)
	{
		xSUB(rsp, stack_size);

		u32 stack_offset = SHADOW_SIZE;
		for (u32 i = 0; i < iREGCNT_XMM; i++)
		{
			if (fpr_bitmask & (1u << i) && RegisterSSE_IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			{
				xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
				stack_offset += XMM_SIZE;
			}
		}

		for (u32 i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || Register_IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			{
				xMOV(ptr64[rsp + stack_offset], xRegister64(i));
				stack_offset += GPR_SIZE;
			}
		}
	}

	if (is_load)
	{
		DynGen_PrepRegs(address_register, -1, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits, is_signed]() {DynGen_DirectRead(size_in_bits, is_signed); },  0, size_in_bits, is_signed && size_in_bits <= 32);

		if (size_in_bits == 128)
		{
			if (data_register != xmm0.Id)
				xMOVAPS(xRegisterSSE(data_register), xmm0);
		}
		else
		{
			if (is_xmm)
			{
				xMOVDZX(xRegisterSSE(data_register), rax);
			}
			else
			{
				if (data_register != eax.Id)
					xMOV(xRegister64(data_register), rax);
			}
		}
	}
	else
	{
		if (address_register != arg1reg.Id)
			xMOV(arg1regd, xRegister32(address_register));

		if (size_in_bits == 128)
		{
			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
			if (data_register != argreg.Id)
				xMOVAPS(argreg, xRegisterSSE(data_register));
		}
		else
		{
			if (is_xmm)
			{
				xMOVD(arg2reg, xRegisterSSE(data_register));
			}
			else
			{
				if (data_register != arg2reg.Id)
					xMOV(arg2reg, xRegister64(data_register));
			}
		}

		DynGen_PrepRegs(address_register, data_register, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits]() { DynGen_DirectWrite(size_in_bits); }, 1, size_in_bits);
	}

	// restore regs
	if (stack_size > 0)
	{
		u32 stack_offset = SHADOW_SIZE;
		for (u32 i = 0; i < iREGCNT_XMM; i++)
		{
			if (fpr_bitmask & (1u << i) && RegisterSSE_IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			{
				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
				stack_offset += XMM_SIZE;
			}
		}

		for (u32 i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || Register_IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			{
				xMOV(xRegister64(i), ptr64[rsp + stack_offset]);
				stack_offset += GPR_SIZE;
			}
		}

		xADD(rsp, stack_size);
	}

	xJMP((void*)(code_address + code_size));

	recEndThunk();

	// backpatch to a jump to the slowmem handler
	x86Ptr = (u8*)code_address;
	xJMP(thunk);

	// fill the rest of it with nops, if any
	for (u32 i = static_cast<u32>((uintptr_t)x86Ptr - code_address); i < code_size; i++)
	{
		*(u8*)x86Ptr = 0x90;
		x86Ptr += sizeof(u8);
	}
}

namespace R5900::Dynarec::OpcodeImpl
{

void recPREF(void) { }
void recSYNC(void) { }

void recMFSA()
{
	if (!_Rd_)
		return;

	// zero-extended
	if (const int mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE); mmreg >= 0)
	{
		// have to zero out bits 63:32
		const int temp = _allocTempXMMreg(XMMT_INT);
		xMOVSSZX(xRegisterSSE(temp), ptr32[&cpuRegs.sa]);
		xBLEND.PD(xRegisterSSE(mmreg), xRegisterSSE(temp), 1);
		_freeXMMreg(temp);
	}
	else if (const int gprreg = _allocIfUsedGPRtoX86(_Rd_, MODE_WRITE); gprreg >= 0)
	{
		xMOV(xRegister32(gprreg), ptr32[&cpuRegs.sa]);
	}
	else
	{
		_deleteEEreg(_Rd_, 0);
		xMOV(eax, ptr32[&cpuRegs.sa]);
		xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], rax);
	}
}

// SA is 4-bit and contains the amount of bytes to shift
void recMTSA()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], g_cpuConstRegs[_Rs_].UL[0] & 0xf);
	}
	else
	{
		int mmreg;

		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
		{
			xMOVSS(ptr[&cpuRegs.sa], xRegisterSSE(mmreg));
		}
		else if ((mmreg = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ)) >= 0)
		{
			xMOV(ptr[&cpuRegs.sa], xRegister32(mmreg));
		}
		else
		{
			xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
			xMOV(ptr[&cpuRegs.sa], eax);
		}
		xAND(ptr32[&cpuRegs.sa], 0xf);
	}
}

void recMTSAB()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], ((g_cpuConstRegs[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF)));
	}
	else
	{
		_eeMoveGPRtoR(eax, _Rs_);
		xAND(eax, 0xF);
		xXOR(eax, _Imm_ & 0xf);
		xMOV(ptr[&cpuRegs.sa], eax);
	}
}

void recMTSAH()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], ((g_cpuConstRegs[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1);
	}
	else
	{
		_eeMoveGPRtoR(eax, _Rs_);
		xAND(eax, 0x7);
		xXOR(eax, _Imm_ & 0x7);
		xSHL(eax, 1);
		xMOV(ptr[&cpuRegs.sa], eax);
	}
}

////////////////////////////////////////////////////
/* TODO : Unknown ops should throw an exception. */
void recNULL(void) { }
void recUnknown(void) { }
void recMMI_Unknown(void) { }
void recCOP0_Unknown(void) { }
void recCOP1_Unknown(void) { }

/**********************************************************
*    UNHANDLED YET OPCODES
*
**********************************************************/

/* Suikoden 3 uses it a lot */
void recCACHE(void) { }

void recTGE(void)   { recBranchCall(R5900::Interpreter::OpcodeImpl::TGE);   }
void recTGEU(void)  { recBranchCall(R5900::Interpreter::OpcodeImpl::TGEU);  }
void recTLT(void)   { recBranchCall(R5900::Interpreter::OpcodeImpl::TLT);   }
void recTLTU(void)  { recBranchCall(R5900::Interpreter::OpcodeImpl::TLTU);  }
void recTEQ(void)   { recBranchCall(R5900::Interpreter::OpcodeImpl::TEQ);   }
void recTNE(void)   { recBranchCall(R5900::Interpreter::OpcodeImpl::TNE);   }
void recTGEI(void)  { recBranchCall(R5900::Interpreter::OpcodeImpl::TGEI);  }
void recTGEIU(void) { recBranchCall(R5900::Interpreter::OpcodeImpl::TGEIU); } 
void recTLTI(void)  { recBranchCall(R5900::Interpreter::OpcodeImpl::TLTI);  }
void recTLTIU(void) { recBranchCall(R5900::Interpreter::OpcodeImpl::TLTIU); }
void recTEQI(void)  { recBranchCall(R5900::Interpreter::OpcodeImpl::TEQI);  }
void recTNEI(void)  { recBranchCall(R5900::Interpreter::OpcodeImpl::TNEI);  }

/*********************************************************
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

// TODO: overflow checks

#ifndef ARITHMETIC_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(ADD, _Rd_);
REC_FUNC_DEL(ADDU, _Rd_);
REC_FUNC_DEL(DADD, _Rd_);
REC_FUNC_DEL(DADDU, _Rd_);
REC_FUNC_DEL(SUB, _Rd_);
REC_FUNC_DEL(SUBU, _Rd_);
REC_FUNC_DEL(DSUB, _Rd_);
REC_FUNC_DEL(DSUBU, _Rd_);
REC_FUNC_DEL(AND, _Rd_);
REC_FUNC_DEL(OR, _Rd_);
REC_FUNC_DEL(XOR, _Rd_);
REC_FUNC_DEL(NOR, _Rd_);
REC_FUNC_DEL(SLT, _Rd_);
REC_FUNC_DEL(SLTU, _Rd_);

#else

static void recADD_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] + g_cpuConstRegs[_Rt_].UL[0]));
}

static void recADD_consts(int info) /* s is constant */
{
	const s32 cval = g_cpuConstRegs[_Rs_].SL[0];
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	if (cval != 0)
		xADD(xRegister32(EEREC_D), cval);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

// t is constant
static void recADD_constt(int info)
{
	const s32 cval = g_cpuConstRegs[_Rt_].SL[0];
	if (info & PROCESS_EE_S)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (cval != 0)
		xADD(xRegister32(EEREC_D), cval);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

// nothing is constant
static void recADD_(int info)
{
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xADD(xRegister32(EEREC_D), xRegister32(EEREC_T));
		}
		else if (EEREC_D == EEREC_T)
		{
			xADD(xRegister32(EEREC_D), xRegister32(EEREC_S));
		}
		else
		{
			xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
			xADD(xRegister32(EEREC_D), xRegister32(EEREC_T));
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
		xADD(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	}
	else if (info & PROCESS_EE_T)
	{
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
		xADD(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	}
	else
	{
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rs_].UD[0]]);
		xADD(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	}

	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

EERECOMPILE_CODERC0(ADD, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// ADDU
void recADDU(void)
{
	recADD();
}

//// DADD
void recDADD_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + g_cpuConstRegs[_Rt_].UD[0];
}

// s is constant
static void recDADD_consts(int info)
{
	const s64 cval = g_cpuConstRegs[_Rs_].SD[0];
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	if (cval != 0)
		xImm64Op(xADD, xRegister64(EEREC_D), rax, cval);
}

// t is constant
static void recDADD_constt(int info)
{
	const s64 cval = g_cpuConstRegs[_Rt_].SD[0];
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	if (cval != 0)
		xImm64Op(xADD, xRegister64(EEREC_D), rax, cval);
}

// nothing is constant
static void recDADD_(int info)
{
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xADD(xRegister64(EEREC_D), xRegister64(EEREC_T));
		}
		else if (EEREC_D == EEREC_T)
		{
			xADD(xRegister64(EEREC_D), xRegister64(EEREC_S));
		}
		else
		{
			xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
			xADD(xRegister64(EEREC_D), xRegister64(EEREC_T));
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
		xADD(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	}
	else if (info & PROCESS_EE_T)
	{
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
		xADD(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	}
	else
	{
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
		xADD(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	}
}

EERECOMPILE_CODERC0(DADD, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_64BITOP);

//// DADDU
void recDADDU(void)
{
	recDADD();
}

//// SUB

static void recSUB_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] - g_cpuConstRegs[_Rt_].UL[0]));
}

static void recSUB_consts(int info)
{
	const s32 sval = g_cpuConstRegs[_Rs_].SL[0];
	xMOV(eax, sval);

	if (info & PROCESS_EE_T)
		xSUB(eax, xRegister32(EEREC_T));
	else
		xSUB(eax, ptr32[&cpuRegs.GPR.r[_Rt_].SL[0]]);

	xMOVSX(xRegister64(EEREC_D), eax);
}

static void recSUB_constt(int info)
{
	const s32 tval = g_cpuConstRegs[_Rt_].SL[0];
	if (info & PROCESS_EE_S)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (tval != 0)
		xSUB(xRegister32(EEREC_D), tval);

	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSUB_(int info)
{
	if (_Rs_ == _Rt_)
	{
		xXOR(xRegister32(EEREC_D), xRegister32(EEREC_D));
		return;
	}

	// a bit messier here because it's not commutative..
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
			xSUB(xRegister32(EEREC_D), xRegister32(EEREC_T));
			xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
		}
		else if (EEREC_D == EEREC_T)
		{
			// D might equal T
			xMOV(eax, xRegister32(EEREC_S));
			xSUB(eax, xRegister32(EEREC_T));
			xMOVSX(xRegister64(EEREC_D), eax);
		}
		else
		{
			xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
			xSUB(xRegister32(EEREC_D), xRegister32(EEREC_T));
			xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
		xSUB(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
		xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
		xMOV(eax, ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
		xSUB(eax, xRegister32(EEREC_T));
		xMOVSX(xRegister64(EEREC_D), eax);
	}
	else
	{
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
		xSUB(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
		xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
	}
}

EERECOMPILE_CODERC0(SUB, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// SUBU
void recSUBU(void)
{
	recSUB();
}

//// DSUB
static void recDSUB_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] - g_cpuConstRegs[_Rt_].UD[0];
}

static void recDSUB_consts(int info)
{
	// gross, because if d == t, we can't destroy t
	const s64 sval = g_cpuConstRegs[_Rs_].SD[0];
	const xRegister64 regd((info & PROCESS_EE_T && EEREC_D == EEREC_T) ? rax.Id : EEREC_D);
	xMOV64(regd, sval);

	if (info & PROCESS_EE_T)
		xSUB(regd, xRegister64(EEREC_T));
	else
		xSUB(regd, ptr64[&cpuRegs.GPR.r[_Rt_].SD[0]]);

	// emitter will eliminate redundant moves.
	xMOV(xRegister64(EEREC_D), regd);
}

static void recDSUB_constt(int info)
{
	const s64 tval = g_cpuConstRegs[_Rt_].SD[0];
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	if (tval != 0)
		xImm64Op(xSUB, xRegister64(EEREC_D), rax, tval);
}

static void recDSUB_(int info)
{
	if (_Rs_ == _Rt_)
	{
		xXOR(xRegister32(EEREC_D), xRegister32(EEREC_D));
		return;
	}

	// a bit messier here because it's not commutative..
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		// D might equal T
		const xRegister64 regd(EEREC_D == EEREC_T ? rax.Id : EEREC_D);
		xMOV(regd, xRegister64(EEREC_S));
		xSUB(regd, xRegister64(EEREC_T));
		xMOV(xRegister64(EEREC_D), regd);
	}
	else if (info & PROCESS_EE_S)
	{
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
		xSUB(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
		const xRegister64 regd(EEREC_D == EEREC_T ? rax.Id : EEREC_D);
		xMOV(regd, ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
		xSUB(regd, xRegister64(EEREC_T));
		xMOV(xRegister64(EEREC_D), regd);
	}
	else
	{
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
		xSUB(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	}
}

EERECOMPILE_CODERC0(DSUB, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// DSUBU
void recDSUBU(void) { recDSUB(); }

static void recLogicalOp_constv_XOR(int info, int creg, u32 vreg, int regv)
{
	GPR_reg64 cval    = g_cpuConstRegs[creg];

	if (regv >= 0)
		xMOV(xRegister64(EEREC_D), xRegister64(regv));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[vreg].UD[0]]);
	if (cval.SD[0] != 0)
		xImm64Op(xXOR, xRegister64(EEREC_D), rax, cval.UD[0]);
}

static void recLogicalOp_constv_NOR(int info, int creg, u32 vreg, int regv)
{
	GPR_reg64 cval = g_cpuConstRegs[creg];

	if (cval.SD[0] == -1)
	{
		xMOV64(xRegister64(EEREC_D), 0);
	}
	else
	{
		if (regv >= 0)
			xMOV(xRegister64(EEREC_D), xRegister64(regv));
		else
			xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[vreg].UD[0]]);
		if (cval.SD[0] != 0)
			xImm64Op(xOR, xRegister64(EEREC_D), rax, cval.UD[0]);
		xNOT(xRegister64(EEREC_D));
	}
}

static void recLogicalOp_constv_AND(int info, int creg, u32 vreg, int regv)
{
	GPR_reg64 cval = g_cpuConstRegs[creg];

	if (cval.SD[0] == 0)
	{
		xMOV64(xRegister64(EEREC_D), 0);
	}
	else
	{
		if (regv >= 0)
			xMOV(xRegister64(EEREC_D), xRegister64(regv));
		else
			xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[vreg].UD[0]]);
		if (cval.SD[0] != -1)
			xImm64Op(xAND, xRegister64(EEREC_D), rax, cval.UD[0]);
	}
}

static void recLogicalOp_constv_OR(int info, int creg, u32 vreg, int regv)
{
	GPR_reg64 cval = g_cpuConstRegs[creg];

	if (cval.SD[0] == -1)
	{
		xMOV64(xRegister64(EEREC_D), -1);
	}
	else
	{
		if (regv >= 0)
			xMOV(xRegister64(EEREC_D), xRegister64(regv));
		else
			xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[vreg].UD[0]]);
		if (cval.SD[0] != 0)
			xImm64Op(xOR, xRegister64(EEREC_D), rax, cval.UD[0]);
	}
}

//// AND
static void recAND_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & g_cpuConstRegs[_Rt_].UD[0];
}

static void recAND_consts(int info)
{
	recLogicalOp_constv_AND(info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recAND_constt(int info)
{
	recLogicalOp_constv_AND(info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recAND_(int info)
{
	// swap because it's commutative and Rd might be Rt
	u32 rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

	if (regs >= 0)
		xMOV(xRegister64(EEREC_D), xRegister64(regs));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rs].UD[0]]);

	if (regt >= 0)
		xAND(xRegister64(EEREC_D), xRegister64(regt));
	else
		xAND(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rt].UD[0]]);
}

EERECOMPILE_CODERC0(AND, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// OR
static void recOR_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | g_cpuConstRegs[_Rt_].UD[0];
}

static void recOR_consts(int info)
{
	recLogicalOp_constv_OR(info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recOR_constt(int info)
{
	recLogicalOp_constv_OR(info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recOR_(int info)
{
	// swap because it's commutative and Rd might be Rt
	u32 rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

	if (regs >= 0)
		xMOV(xRegister64(EEREC_D), xRegister64(regs));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rs].UD[0]]);

	if (regt >= 0)
		xOR(xRegister64(EEREC_D), xRegister64(regt));
	else
		xOR(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rt].UD[0]]);
}

EERECOMPILE_CODERC0(OR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// XOR
static void recXOR_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ g_cpuConstRegs[_Rt_].UD[0];
}

static void recXOR_consts(int info)
{
	recLogicalOp_constv_XOR(info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recXOR_constt(int info)
{
	recLogicalOp_constv_XOR(info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recXOR_(int info)
{
	// swap because it's commutative and Rd might be Rt
	u32 rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

	if (rs == rt)
	{
		xXOR(xRegister32(EEREC_D), xRegister32(EEREC_D));
	}
	else
	{
		if (regs >= 0)
			xMOV(xRegister64(EEREC_D), xRegister64(regs));
		else
			xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rs].UD[0]]);

		if (regt >= 0)
			xXOR(xRegister64(EEREC_D), xRegister64(regt));
		else
			xXOR(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rt].UD[0]]);
	}
}

EERECOMPILE_CODERC0(XOR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// NOR
static void recNOR_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = ~(g_cpuConstRegs[_Rs_].UD[0] | g_cpuConstRegs[_Rt_].UD[0]);
}

static void recNOR_consts(int info)
{
	recLogicalOp_constv_NOR(info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void recNOR_constt(int info)
{
	recLogicalOp_constv_NOR(info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void recNOR_(int info)
{
	// swap because it's commutative and Rd might be Rt
	u32 rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

	if (regs >= 0)
		xMOV(xRegister64(EEREC_D), xRegister64(regs));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rs].UD[0]]);

	if (regt >= 0)
		xOR(xRegister64(EEREC_D), xRegister64(regt));
	else
		xOR(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[rt].UD[0]]);

	xNOT(xRegister64(EEREC_D));
}

EERECOMPILE_CODERC0(NOR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// SLT - test with silent hill, lemans
static void recSLT_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].SD[0] < g_cpuConstRegs[_Rt_].SD[0];
}

static void recSLTs_const(int info, int sign, int st)
{
	const s64 cval = g_cpuConstRegs[st ? _Rt_ : _Rs_].SD[0];

	const xImpl_Set& SET = st ? (sign ? xSETL : xSETB) : (sign ? xSETG : xSETA);

	// If Rd == Rs or Rt, we can't xor it before it's used.
	// So, allocate a temporary register first, and then reallocate it to Rd.
	const xRegister32 dreg((_Rd_ == (st ? _Rs_ : _Rt_)) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D);
	const int regs = st ? ((info & PROCESS_EE_S) ? EEREC_S : -1) : ((info & PROCESS_EE_T) ? EEREC_T : -1);
	xXOR(dreg, dreg);

	if (regs >= 0)
		xImm64Op(xCMP, xRegister64(regs), rcx, cval);
	else
		xImm64Op(xCMP, ptr64[&cpuRegs.GPR.r[st ? _Rs_ : _Rt_].UD[0]], rcx, cval);
	SET(xRegister8(dreg));

	if (dreg.Id != EEREC_D)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_D]);
		_freeX86reg(EEREC_D);
	}
}

static void recSLTs_(int info, int sign)
{
	const xImpl_Set& SET = sign ? xSETL : xSETB;

	// need to keep Rs/Rt around.
	const xRegister32 dreg((_Rd_ == _Rt_ || _Rd_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D);

	// force Rs into a register, may as well cache it since we're loading anyway.
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);

	xXOR(dreg, dreg);
	if (info & PROCESS_EE_T)
		xCMP(xRegister64(regs), xRegister64(EEREC_T));
	else
		xCMP(xRegister64(regs), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);

	SET(xRegister8(dreg));

	if (dreg.Id != EEREC_D)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_D]);
		_freeX86reg(EEREC_D);
	}
}

static void recSLT_consts(int info)
{
	recSLTs_const(info, 1, 0);
}

static void recSLT_constt(int info)
{
	recSLTs_const(info, 1, 1);
}

static void recSLT_(int info)
{
	recSLTs_(info, 1);
}

EERECOMPILE_CODERC0(SLT, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);

// SLTU - test with silent hill, lemans
static void recSLTU_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] < g_cpuConstRegs[_Rt_].UD[0];
}

static void recSLTU_consts(int info)
{
	recSLTs_const(info, 0, 0);
}

static void recSLTU_constt(int info)
{
	recSLTs_const(info, 0, 1);
}

static void recSLTU_(int info)
{
	recSLTs_(info, 0);
}

EERECOMPILE_CODERC0(SLTU, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);

#endif

/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/

#ifndef ARITHMETICIMM_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(ADDI, _Rt_);
REC_FUNC_DEL(ADDIU, _Rt_);
REC_FUNC_DEL(DADDI, _Rt_);
REC_FUNC_DEL(DADDIU, _Rt_);
REC_FUNC_DEL(ANDI, _Rt_);
REC_FUNC_DEL(ORI, _Rt_);
REC_FUNC_DEL(XORI, _Rt_);

REC_FUNC_DEL(SLTI, _Rt_);
REC_FUNC_DEL(SLTIU, _Rt_);

#else

static void recADDI_const(void)
{
	g_cpuConstRegs[_Rt_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] + u32(s32(_Imm_))));
}

static void recADDI_(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister32(EEREC_T), xRegister32(EEREC_S));
	else
		xMOV(xRegister32(EEREC_T), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xADD(xRegister32(EEREC_T), _Imm_);
	xMOVSX(xRegister64(EEREC_T), xRegister32(EEREC_T));
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ADDI, XMMINFO_WRITET | XMMINFO_READS);

////////////////////////////////////////////////////
void recADDIU(void) { recADDI(); }

////////////////////////////////////////////////////
static void recDADDI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + u64(s64(_Imm_));
}

static void recDADDI_(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_T), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_T), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	xADD(xRegister64(EEREC_T), _Imm_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, DADDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

void recDADDIU(void) { recDADDI(); }

static void recSLTIU_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] < (u64)(_Imm_);
}

static void recSLTIU_(int info)
{
	// TODO(Stenzek): this can be made to suck less by turning Rs into a temp and reallocating Rt.
	const xRegister32 dreg((_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T);
	xXOR(dreg, dreg);

	if (info & PROCESS_EE_S)
		xCMP(xRegister64(EEREC_S), _Imm_);
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], _Imm_);

	xSETB(xRegister8(dreg));

	if (dreg.Id != EEREC_T)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_T]);
		_freeX86reg(EEREC_T);
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, SLTIU, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

static void recSLTI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].SD[0] < (s64)(_Imm_);
}

static void recSLTI_(int info)
{
	const xRegister32 dreg((_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T);
	xXOR(dreg, dreg);

	if (info & PROCESS_EE_S)
		xCMP(xRegister64(EEREC_S), _Imm_);
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], _Imm_);

	xSETL(xRegister8(dreg));

	if (dreg.Id != EEREC_T)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_T]);
		_freeX86reg(EEREC_T);
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, SLTI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

static void recANDI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & (u64)_ImmU_; // Zero-extended Immediate
}

static void recANDI_(int info)
{
	if (_ImmU_ != 0)
	{
		if (info & PROCESS_EE_S)
			xMOV(xRegister64(EEREC_T), xRegister64(EEREC_S));
		else
			xMOV(xRegister64(EEREC_T), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
		xAND(xRegister64(EEREC_T), _ImmU_);
	}
	else
	{
		xXOR(xRegister32(EEREC_T), xRegister32(EEREC_T));
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ANDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

////////////////////////////////////////////////////
static void recORI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | (u64)_ImmU_; // Zero-extended Immediate
}

static void recORI_(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_T), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_T), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	if (_ImmU_ != 0)
	{
		xOR(xRegister64(EEREC_T), _ImmU_);
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

////////////////////////////////////////////////////
static void recXORI_const(void)
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ (u64)_ImmU_; // Zero-extended Immediate
}

static void recXORI_(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_T), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_T), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
	if (_ImmU_ != 0)
	{
		xXOR(xRegister64(EEREC_T), _ImmU_);
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, XORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

#endif

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/
#ifndef BRANCH_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_SYS(BEQ);
REC_SYS(BEQL);
REC_SYS(BNE);
REC_SYS(BNEL);
REC_SYS(BLTZ);
REC_SYS(BGTZ);
REC_SYS(BLEZ);
REC_SYS(BGEZ);
REC_SYS(BGTZL);
REC_SYS(BLTZL);
REC_SYS_DEL(BLTZAL, 31);
REC_SYS_DEL(BLTZALL, 31);
REC_SYS(BLEZL);
REC_SYS(BGEZL);
REC_SYS_DEL(BGEZAL, 31);
REC_SYS_DEL(BGEZALL, 31);

#else

static u32 *recSetBranchEQ(int bne, int process)
{
	// TODO(Stenzek): This is suboptimal if the registers are in XMMs.
	// If the constant register is already in a host register, we don't need the immediate...

	if (process & PROCESS_CONSTS)
	{
		_eeFlushAllDirty();

		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		const int regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		if (regt >= 0)
			xImm64Op(xCMP, xRegister64(regt), rax, g_cpuConstRegs[_Rs_].UD[0]);
		else
			xImm64Op(xCMP, ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]], rax, g_cpuConstRegs[_Rs_].UD[0]);
	}
	else if (process & PROCESS_CONSTT)
	{
		_eeFlushAllDirty();

		_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH_AND_FREE);
		const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		if (regs >= 0)
			xImm64Op(xCMP, xRegister64(regs), rax, g_cpuConstRegs[_Rt_].UD[0]);
		else
			xImm64Op(xCMP, ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], rax, g_cpuConstRegs[_Rt_].UD[0]);
	}
	else
	{
		// force S into register, since we need to load it, may as well cache.
		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		const int regs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		const int regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		_eeFlushAllDirty();

		if (regt >= 0)
			xCMP(xRegister64(regs), xRegister64(regt));
		else
			xCMP(xRegister64(regs), ptr64[&cpuRegs.GPR.r[_Rt_]]);
	}

	*(u8*)x86Ptr = 0x0F;
	x86Ptr += sizeof(u8);
	if (bne)
		*(u8*)x86Ptr = JE32;
	else
		*(u8*)x86Ptr = JNE32;
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = 0;
	x86Ptr += sizeof(u32);
	return (u32*)(x86Ptr - 4);
}

static u32 *recSetBranchL(int ltz)
{
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	const int regsxmm = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regsxmm >= 0)
	{
		xMOVMSKPS(eax, xRegisterSSE(regsxmm));
		xTEST(al, 2);

		*(u8*)x86Ptr = 0x0F;
		x86Ptr += sizeof(u8);
		if (ltz)
			*(u8*)x86Ptr = JZ32;
		else
			*(u8*)x86Ptr = JNZ32;
	}
	else
	{
		if (regs >= 0)
			xCMP(xRegister64(regs), 0);
		else
			xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);

		*(u8*)x86Ptr = 0x0F;
		x86Ptr += sizeof(u8);
		if (ltz)
			*(u8*)x86Ptr = JGE32;
		else
			*(u8*)x86Ptr = JL32;
	}
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = 0;
	x86Ptr += sizeof(u32);
	return (u32*)(x86Ptr - 4);
}

//// BEQ
static void recBEQ_const(void)
{
	u32 branchTo;

	if (g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0])
		branchTo = ((s32)_Imm_ * 4) + pc;
	else
		branchTo = pc + 4;

	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);
}

static void recBEQ_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (_Rs_ == _Rt_)
	{
		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
	}
	else
	{
		const bool swap = TrySwapDelaySlot(_Rs_, _Rt_, 0, true);
		u32 *j32Ptr = recSetBranchEQ(0, process);

		if (!swap)
		{
			SaveBranchState();
			recompileNextInstruction(true, false);
		}

		SetBranchImm(branchTo);

		*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

		if (!swap)
		{
			// recopy the next inst
			pc -= 4;
			LoadBranchState();
			recompileNextInstruction(true, false);
		}

		SetBranchImm(pc);
	}
}

void recBEQ(void)
{
	// prefer using the host register over an immediate, it'll be smaller code.
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBEQ_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBEQ_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBEQ_process(PROCESS_CONSTT);
	else
		recBEQ_process(0);
}

//// BNE
static void recBNE_const(void)
{
	u32 branchTo;

	if (g_cpuConstRegs[_Rs_].SD[0] != g_cpuConstRegs[_Rt_].SD[0])
		branchTo = ((s32)_Imm_ * 4) + pc;
	else
		branchTo = pc + 4;

	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);
}

static void recBNE_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (_Rs_ == _Rt_)
	{
		recompileNextInstruction(true, false);
		SetBranchImm(pc);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, _Rt_, 0, true);

	u32 *j32Ptr = recSetBranchEQ(1, process);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

void recBNE(void)
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBNE_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBNE_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBNE_process(PROCESS_CONSTT);
	else
		recBNE_process(0);
}

//// BEQL
static void recBEQL_const(void)
{
	if (g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0])
	{
		u32 branchTo = ((s32)_Imm_ * 4) + pc;
		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
	}
	else
	{
		SetBranchImm(pc + 4);
	}
}

static void recBEQL_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;
	u32 *j32Ptr = recSetBranchEQ(0, process);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}

void recBEQL(void)
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBEQL_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBEQL_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBEQL_process(PROCESS_CONSTT);
	else
		recBEQL_process(0);
}

//// BNEL
static void recBNEL_const(void)
{
	if (g_cpuConstRegs[_Rs_].SD[0] != g_cpuConstRegs[_Rt_].SD[0])
	{
		u32 branchTo = ((s32)_Imm_ * 4) + pc;
		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
	}
	else
	{
		SetBranchImm(pc + 4);
	}
}

static void recBNEL_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	u32 *j32Ptr = recSetBranchEQ(0, process);

	SaveBranchState();
	SetBranchImm(pc + 4);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	// recopy the next inst
	LoadBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);
}

void recBNEL()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBNEL_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBNEL_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBNEL_process(PROCESS_CONSTT);
	else
		recBNEL_process(0);
}

////////////////////////////////////////////////////
void recBLTZAL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xMOV64(rax, pc + 4);
	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);

	u32 *j32Ptr = recSetBranchL(1);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGEZAL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xMOV64(rax, pc + 4);
	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);

	u32 *j32Ptr = recSetBranchL(0);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBLTZALL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xMOV64(rax, pc + 4);
	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	u32 *j32Ptr = recSetBranchL(1);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGEZALL(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
	xMOV64(rax, pc + 4);
	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	u32 *j32Ptr = recSetBranchL(0);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}


//// BLEZ
void recBLEZ(void)
{
	u32 *j32Ptr;
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] <= 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xCMP(xRegister64(regs), 0);
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);

	*(u8*)x86Ptr = 0x0F;
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = JG32;
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = 0;
	x86Ptr += sizeof(u32);
	j32Ptr = (u32*)(x86Ptr - 4);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

//// BGTZ
void recBGTZ(void)
{
	u32 *j32Ptr;
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] > 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xCMP(xRegister64(regs), 0);
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);

	*(u8*)x86Ptr = 0x0F;
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = JLE32;
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = 0;
	x86Ptr += sizeof(u32);
	j32Ptr = (u32*)(x86Ptr - 4);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBLTZ(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	_eeFlushAllDirty();
	u32 *j32Ptr = recSetBranchL(1);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGEZ(void)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	_eeFlushAllDirty();

	u32 *j32Ptr = recSetBranchL(0);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBLTZL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	_eeFlushAllDirty();
	u32 *j32Ptr = recSetBranchL(1);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}


////////////////////////////////////////////////////
void recBGEZL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	_eeFlushAllDirty();
	u32 *j32Ptr = recSetBranchL(0);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}



/*********************************************************
* Register branch logic  Likely                          *
* Format:  OP rs, offset                                 *
*********************************************************/

////////////////////////////////////////////////////
void recBLEZL(void)
{
	u32 *j32Ptr;
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] <= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xCMP(xRegister64(regs), 0);
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);

	*(u8*)x86Ptr = 0x0F;
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = JG32;
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = 0;
	x86Ptr += sizeof(u32);
	j32Ptr = (u32*)(x86Ptr - 4);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGTZL(void)
{
	u32 *j32Ptr;
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] > 0))
			SetBranchImm(pc + 4);
		else
		{
			_clearNeededXMMregs();
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
		xCMP(xRegister64(regs), 0);
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);

	*(u8*)x86Ptr = 0x0F;
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = JLE32;
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = 0;
	x86Ptr += sizeof(u32);
	j32Ptr = (u32*)(x86Ptr - 4);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

	*j32Ptr = (x86Ptr - (u8*)j32Ptr) - 4;

	LoadBranchState();
	SetBranchImm(pc);
}

#endif

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
#ifndef JUMP_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_SYS(J);
REC_SYS_DEL(JAL, 31);
REC_SYS(JR);
REC_SYS_DEL(JALR, _Rd_);

#else

////////////////////////////////////////////////////
void recJ(void)
{
	// SET_FPUSTATE;
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	recompileNextInstruction(true, false);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

////////////////////////////////////////////////////
void recJAL(void)
{
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	_deleteEEreg(31, 0);
	GPR_SET_CONST(31);
	g_cpuConstRegs[31].UL[0] = pc + 4;
	g_cpuConstRegs[31].UL[1] = 0;

	recompileNextInstruction(true, false);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/

////////////////////////////////////////////////////
void recJR(void)
{
	SetBranchReg(_Rs_);
}

////////////////////////////////////////////////////
void recJALR(void)
{
	const u32 newpc = pc + 4;
	const bool swap = (EmuConfig.Gamefixes.GoemonTlbHack || _Rd_ == _Rs_) ? false : TrySwapDelaySlot(_Rs_, 0, _Rd_, true);

	// uncomment when there are NO instructions that need to call interpreter
	//	int mmreg;
	//	if (GPR_IS_CONST1(_Rs_))
	//		xMOV(ptr32[&cpuRegs.pc], g_cpuConstRegs[_Rs_].UL[0]);
	//	else
	//	{
	//		int mmreg;
	//
	//		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
	//		{
	//			xMOVSS(ptr[&cpuRegs.pc], xRegisterSSE(mmreg));
	//		}
	//		else iR5900-32.cpp{
	//			xMOV(eax, ptr[(void*)((int)&cpuRegs.GPR.r[_Rs_].UL[0])]);
	//			xMOV(ptr[&cpuRegs.pc], eax);
	//		}
	//	}

	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_eeMoveGPRtoR(xRegister32(wbreg), _Rs_);

		if (EmuConfig.Gamefixes.GoemonTlbHack)
		{
			xMOV(ecx, xRegister32(wbreg));
			vtlb_DynV2P();
			xMOV(xRegister32(wbreg), eax);
		}
	}

	if (_Rd_)
	{
		_deleteEEreg(_Rd_, 0);
		GPR_SET_CONST(_Rd_);
		g_cpuConstRegs[_Rd_].UD[0] = newpc;
	}

	if (!swap)
	{
		recompileNextInstruction(true, false);

		// the next instruction may have flushed the register.. so reload it if so.
		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
			xMOV(ptr[&cpuRegs.pc], xRegister32(wbreg));
			x86regs[wbreg].inuse = 0;
		}
		else
		{
			xMOV(eax, ptr[&cpuRegs.pcWriteback]);
			xMOV(ptr[&cpuRegs.pc], eax);
		}
	}
	else
	{
		if (GPR_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_GPR, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
			xMOV(ptr32[&cpuRegs.pc], xRegister32(x86reg));
		}
		else
		{
			_eeMoveGPRtoM((uintptr_t)&cpuRegs.pc, _Rs_);
		}
	}

	SetBranchReg(0xffffffff);
}

#endif

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/
#ifndef LOADSTORE_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(LB, _Rt_);
REC_FUNC_DEL(LBU, _Rt_);
REC_FUNC_DEL(LH, _Rt_);
REC_FUNC_DEL(LHU, _Rt_);
REC_FUNC_DEL(LW, _Rt_);
REC_FUNC_DEL(LWU, _Rt_);
REC_FUNC_DEL(LWL, _Rt_);
REC_FUNC_DEL(LWR, _Rt_);
REC_FUNC_DEL(LD, _Rt_);
REC_FUNC_DEL(LDR, _Rt_);
REC_FUNC_DEL(LDL, _Rt_);
REC_FUNC_DEL(LQ, _Rt_);
REC_FUNC(SB);
REC_FUNC(SH);
REC_FUNC(SW);
REC_FUNC(SWL);
REC_FUNC(SWR);
REC_FUNC(SD);
REC_FUNC(SDL);
REC_FUNC(SDR);
REC_FUNC(SQ);
REC_FUNC(LWC1);
REC_FUNC(SWC1);
REC_FUNC(LQC2);
REC_FUNC(SQC2);

#else

using namespace Interpreter::OpcodeImpl;

//////////////////////////////////////////////////////////////////////////////////////////
//
static void recLoadQuad128(void)
{
	// This mess is so we allocate *after* the vtlb flush, not before.
	vtlb_ReadRegAllocCallback alloc_cb = nullptr;
	if (_Rt_)
		alloc_cb = []() { return _allocGPRtoXMMreg(_Rt_, MODE_WRITE); };

	int xmmreg;
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 srcadr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0x0f;
		xmmreg = vtlb_DynGenReadQuad_Const(srcadr, _Rt_ ? alloc_cb : nullptr);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1reg, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		// force 16 byte alignment on 128 bit reads
		xAND(arg1regd, ~0x0F);

		xmmreg = vtlb_DynGenReadQuad(128, arg1regd.Id, _Rt_ ? alloc_cb : nullptr);
	}

	// if there was a constant, it should have been invalidated.
	if (!_Rt_)
		_freeXMMreg(xmmreg);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
static void recLoad(u32 bits, bool sign)
{
	// This mess is so we allocate *after* the vtlb flush, not before.
	// TODO(Stenzek): If not live, save directly to state, and delete constant.
	vtlb_ReadRegAllocCallback alloc_cb = nullptr;
	if (_Rt_)
		alloc_cb = []() { return _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE); };

	int x86reg;
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		x86reg = vtlb_DynGenReadNonQuad_Const(bits, sign, srcadr, alloc_cb);
	}
	else
	{
		// Load arg1 with the source memory address that we're reading from.
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		x86reg = vtlb_DynGenReadNonQuad(bits, sign, false, arg1regd.Id, alloc_cb);
	}

	// if there was a constant, it should have been invalidated.
	if (!_Rt_)
		_freeX86reg(x86reg);
}

//////////////////////////////////////////////////////////////////////////////////////////
//

static void recStore128(void)
{
	// Performance note: Const prop for the store address is good, always.
	// Constprop for the value being stored is not really worthwhile (better to use register
	// allocation -- simpler code and just as fast)
	int regt = _allocGPRtoXMMreg(_Rt_, MODE_READ);

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		dstadr &= ~0x0f;

		vtlb_DynGenWrite_Const(128, true, dstadr, regt);
	}
	else
	{
		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
			_eeMoveGPRtoR(arg1regd, _Rs_);
			if (_Imm_ != 0)
				xADD(arg1regd, _Imm_);
		}
		else
		{
			xMOV(arg1regd, _Imm_);
		}

		xAND(arg1regd, ~0x0F);

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
		vtlb_DynGenWrite(128, true, arg1regd.Id, regt);
	}
}

static void recStore64(void)
{
	// Performance note: Const prop for the store address is good, always.
	// Constprop for the value being stored is not really worthwhile (better to use register
	// allocation -- simpler code and just as fast)
	int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(64, false, dstadr, regt);
	}
	else
	{
		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
			_eeMoveGPRtoR(arg1regd, _Rs_);
			if (_Imm_ != 0)
				xADD(arg1regd, _Imm_);
		}
		else
		{
			xMOV(arg1regd, _Imm_);
		}

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
		vtlb_DynGenWrite(64, false, arg1regd.Id, regt);
	}
}

static void recStore32(void)
{
	// Performance note: Const prop for the store address is good, always.
	// Constprop for the value being stored is not really worthwhile (better to use register
	// allocation -- simpler code and just as fast)
	int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(32, false, dstadr, regt);
	}
	else
	{
		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
			_eeMoveGPRtoR(arg1regd, _Rs_);
			if (_Imm_ != 0)
				xADD(arg1regd, _Imm_);
		}
		else
		{
			xMOV(arg1regd, _Imm_);
		}

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
		vtlb_DynGenWrite(32, false, arg1regd.Id, regt);
	}
}

static void recStore16(void)
{
	// Performance note: Const prop for the store address is good, always.
	// Constprop for the value being stored is not really worthwhile (better to use register
	// allocation -- simpler code and just as fast)
	int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(16, false, dstadr, regt);
	}
	else
	{
		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
			_eeMoveGPRtoR(arg1regd, _Rs_);
			if (_Imm_ != 0)
				xADD(arg1regd, _Imm_);
		}
		else
		{
			xMOV(arg1regd, _Imm_);
		}

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
		vtlb_DynGenWrite(16, false, arg1regd.Id, regt);
	}
}

static void recStore8(void)
{
	// Performance note: Const prop for the store address is good, always.
	// Constprop for the value being stored is not really worthwhile (better to use register
	// allocation -- simpler code and just as fast)
	int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(8, false, dstadr, regt);
	}
	else
	{
		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
			_eeMoveGPRtoR(arg1regd, _Rs_);
			if (_Imm_ != 0)
				xADD(arg1regd, _Imm_);
		}
		else
		{
			xMOV(arg1regd, _Imm_);
		}

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
		vtlb_DynGenWrite(8, false, arg1regd.Id, regt);
	}
}


//////////////////////////////////////////////////////////////////////////////////////////
//
void recLB(void)  { recLoad(8,  true);       }
void recLBU(void) { recLoad(8,  false);      }
void recLH(void)  { recLoad(16, true);       }
void recLHU(void) { recLoad(16, false);      }
void recLW(void)  { recLoad(32, true);       }
void recLWU(void) { recLoad(32, false);      }
void recLD(void)  { recLoad(64, false);      }
void recLQ(void)  { recLoadQuad128(); }
void recSB(void)  { recStore8();             }
void recSH(void)  { recStore16();            }
void recSW(void)  { recStore32();            }
void recSD(void)  { recStore64();            }
void recSQ(void)  { recStore128();           }

////////////////////////////////////////////////////

void recLWL(void)
{
	_freeX86reg(eax);
	_freeX86reg(ecx);
	_freeX86reg(edx);
	_freeX86reg(arg1regd);

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const xRegister32 temp(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));

	_eeMoveGPRtoR(arg1regd, _Rs_);
	if (_Imm_ != 0)
		xADD(arg1regd, _Imm_);

	// calleeSavedReg1 = bit offset in word
	xMOV(temp, arg1regd);
	xAND(temp, 3);
	xSHL(temp, 3);

	xAND(arg1regd, ~3);
	vtlb_DynGenReadNonQuad(32, false, false, arg1regd.Id, RETURN_READ_IN_RAX);

	if (!_Rt_)
	{
		_freeX86reg(temp);
		return;
	}

	// mask off bytes loaded
	xMOV(ecx, temp);
	_freeX86reg(temp);

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
	xMOV(edx, 0xffffff);
	xSHR(edx, cl);
	xAND(edx, xRegister32(treg));

	// OR in bytes loaded
	xNEG(ecx);
	xADD(ecx, 24);
	xSHL(eax, cl);
	xOR(eax, edx);
	xMOVSX(xRegister64(treg), eax);
}

////////////////////////////////////////////////////
void recLWR(void)
{
	_freeX86reg(eax);
	_freeX86reg(ecx);
	_freeX86reg(edx);
	_freeX86reg(arg1regd);

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const xRegister32 temp(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));

	_eeMoveGPRtoR(arg1regd, _Rs_);
	if (_Imm_ != 0)
		xADD(arg1regd, _Imm_);

	// edi = bit offset in word
	xMOV(temp, arg1regd);

	xAND(arg1regd, ~3);
	vtlb_DynGenReadNonQuad(32, false, false, arg1regd.Id, RETURN_READ_IN_RAX);

	if (!_Rt_)
	{
		_freeX86reg(temp);
		return;
	}

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
	xAND(temp, 3);

	xForwardJE8 nomask;
	xSHL(temp, 3);
	// mask off bytes loaded
	xMOV(ecx, 24);
	xSUB(ecx, temp);
	xMOV(edx, 0xffffff00);
	xSHL(edx, cl);
	xAND(xRegister32(treg), edx);

	// OR in bytes loaded
	xMOV(ecx, temp);
	xSHR(eax, cl);
	xOR(xRegister32(treg), eax);

	xForwardJump8 end;
	nomask.SetTarget();
	// NOTE: This might look wrong, but it's correct - see interpreter.
	xMOVSX(xRegister64(treg), eax);
	end.SetTarget();
	_freeX86reg(temp);
}

////////////////////////////////////////////////////

void recSWL(void)
{
	// avoid flushing and immediately reading back
	_addNeededX86reg(X86TYPE_GPR, _Rs_);

	// preload Rt, since we can't do so inside the branch
	if (!GPR_IS_CONST1(_Rt_))
		_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	const xRegister32 temp(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
	_freeX86reg(eax);
	_freeX86reg(ecx);
	_freeX86reg(arg1regd);
	_freeX86reg(arg2regd);

	_eeMoveGPRtoR(arg1regd, _Rs_);
	if (_Imm_ != 0)
		xADD(arg1regd, _Imm_);

	// edi = bit offset in word
	xMOV(temp, arg1regd);
	xAND(arg1regd, ~3);
	xAND(temp, 3);
	xCMP(temp, 3);

	// If we're not using fastmem, we need to flush early. Because the first read
	// (which would flush) happens inside a branch.
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
		iFlushCall(FLUSH_FULLVTLB);

	xForwardJE8 skip;
	xSHL(temp, 3);

	vtlb_DynGenReadNonQuad(32, false, false, arg1regd.Id, RETURN_READ_IN_RAX);

	// mask read -> arg2
	xMOV(ecx, temp);
	xMOV(arg2regd, 0xffffff00);
	xSHL(arg2regd, cl);
	xAND(arg2regd, eax);

	if (_Rt_)
	{
		// mask write and OR -> edx
		xNEG(ecx);
		xADD(ecx, 24);
		_eeMoveGPRtoR(eax, _Rt_, false);
		xSHR(eax, cl);
		xOR(arg2regd, eax);
	}

	_eeMoveGPRtoR(arg1regd, _Rs_, false);
	if (_Imm_ != 0)
		xADD(arg1regd, _Imm_);
	xAND(arg1regd, ~3);

	xForwardJump8 end;
	skip.SetTarget();
	_eeMoveGPRtoR(arg2regd, _Rt_, false);
	end.SetTarget();

	_freeX86reg(temp);
	vtlb_DynGenWrite(32, false, arg1regd.Id, arg2regd.Id);
}

////////////////////////////////////////////////////
void recSWR(void)
{
	// avoid flushing and immediately reading back
	_addNeededX86reg(X86TYPE_GPR, _Rs_);

	// preload Rt, since we can't do so inside the branch
	if (!GPR_IS_CONST1(_Rt_))
		_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	const xRegister32 temp(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
	_freeX86reg(ecx);
	_freeX86reg(arg1regd);
	_freeX86reg(arg2regd);

	_eeMoveGPRtoR(arg1regd, _Rs_);
	if (_Imm_ != 0)
		xADD(arg1regd, _Imm_);

	// edi = bit offset in word
	xMOV(temp, arg1regd);
	xAND(arg1regd, ~3);
	xAND(temp, 3);

	// If we're not using fastmem, we need to flush early. Because the first read
	// (which would flush) happens inside a branch.
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
		iFlushCall(FLUSH_FULLVTLB);

	xForwardJE8 skip;
	xSHL(temp, 3);

	vtlb_DynGenReadNonQuad(32, false, false, arg1regd.Id, RETURN_READ_IN_RAX);

	// mask read -> edx
	xMOV(ecx, 24);
	xSUB(ecx, temp);
	xMOV(arg2regd, 0xffffff);
	xSHR(arg2regd, cl);
	xAND(arg2regd, eax);

	if (_Rt_)
	{
		// mask write and OR -> edx
		xMOV(ecx, temp);
		_eeMoveGPRtoR(eax, _Rt_, false);
		xSHL(eax, cl);
		xOR(arg2regd, eax);
	}

	_eeMoveGPRtoR(arg1regd, _Rs_, false);
	if (_Imm_ != 0)
		xADD(arg1regd, _Imm_);
	xAND(arg1regd, ~3);

	xForwardJump8 end;
	skip.SetTarget();
	_eeMoveGPRtoR(arg2regd, _Rt_, false);
	end.SetTarget();

	_freeX86reg(temp);
	vtlb_DynGenWrite(32, false, arg1regd.Id, arg2regd.Id);
}

////////////////////////////////////////////////////

/// Masks rt with (0xffffffffffffffff maskshift maskamt), 
/// merges with (value shift amt), leaves result in value
static void ldlrhelper_const(int maskamt, const xImpl_Group2& maskshift, int amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
{
	// Would xor rcx, rcx; not rcx be better here?
	xMOV(rcx, -1);

	maskshift(rcx, maskamt);
	xAND(rt, rcx);

	shift(value, amt);
	xOR(rt, value);
}

/// Masks rt with (0xffffffffffffffff maskshift maskamt), 
/// merges with (value shift amt), leaves result in value
static void ldlrhelper(const xRegister32& maskamt, const xImpl_Group2& maskshift, const xRegister32& amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
{
	// Would xor rcx, rcx; not rcx be better here?
	const xRegister64 maskamt64(maskamt);
	xMOV(ecx, maskamt);
	xMOV(maskamt64, -1);
	maskshift(maskamt64, cl);
	xAND(rt, maskamt64);

	xMOV(ecx, amt);
	shift(value, cl);
	xOR(rt, value);
}

void recLDL(void)
{
	if (!_Rt_)
		return;

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const xRegister32 temp1(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
	_freeX86reg(eax);
	_freeX86reg(ecx);
	_freeX86reg(edx);
	_freeX86reg(arg1regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;

		// If _Rs_ is equal to _Rt_ we need to put the shift in to eax since it won't take the CONST path.
		if (_Rs_ == _Rt_)
			xMOV(temp1, srcadr);

		srcadr &= ~0x07;

		vtlb_DynGenReadNonQuad64_Const(srcadr, RETURN_READ_IN_RAX);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		xMOV(temp1, arg1regd);
		xAND(arg1regd, ~0x07);

		vtlb_DynGenReadNonQuad(64, false, false, arg1regd.Id, RETURN_READ_IN_RAX);
	}

	const xRegister64 treg(_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE));

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 shift = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		shift = ((shift & 0x7) + 1) * 8;
		if (shift != 64)
		{
			ldlrhelper_const(shift, xSHR, 64 - shift, xSHL, rax, treg);
		}
		else
		{
			xMOV(treg, rax);
		}
	}
	else
	{
		xAND(temp1, 0x7);
		xCMP(temp1, 7);
		xCMOVE(treg, rax); // swap register with memory when not shifting
		xForwardJE8 skip;
		// Calculate the shift from top bit to lowest.
		xADD(temp1, 1);
		xMOV(edx, 64);
		xSHL(temp1, 3);
		xSUB(edx, temp1);

		ldlrhelper(temp1, xSHR, edx, xSHL, rax, treg);
		skip.SetTarget();
	}

	_freeX86reg(temp1);
}

////////////////////////////////////////////////////
void recLDR(void)
{
	if (!_Rt_)
		return;

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const xRegister32 temp1(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
	_freeX86reg(eax);
	_freeX86reg(ecx);
	_freeX86reg(edx);
	_freeX86reg(arg1regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;

		// If _Rs_ is equal to _Rt_ we need to put the shift in to eax since it won't take the CONST path.
		if (_Rs_ == _Rt_)
			xMOV(temp1, srcadr);

		srcadr &= ~0x07;

		vtlb_DynGenReadNonQuad64_Const(srcadr, RETURN_READ_IN_RAX);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		xMOV(temp1, arg1regd);
		xAND(arg1regd, ~0x07);

		vtlb_DynGenReadNonQuad(64, false, false, arg1regd.Id, RETURN_READ_IN_RAX);
	}

	const xRegister64 treg(_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE));

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 shift = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		shift = (shift & 0x7) * 8;
		if (shift != 0)
		{
			ldlrhelper_const(64 - shift, xSHL, shift, xSHR, rax, treg);
		}
		else
		{
			xMOV(treg, rax);
		}
	}
	else
	{
		xAND(temp1, 0x7);
		xCMOVE(treg, rax); // swap register with memory when not shifting
		xForwardJE8 skip;
		// Calculate the shift from top bit to lowest.
		xMOV(edx, 64);
		xSHL(temp1, 3);
		xSUB(edx, temp1);

		ldlrhelper(edx, xSHL, temp1, xSHR, rax, treg);
		skip.SetTarget();
	}

	_freeX86reg(temp1);
}

////////////////////////////////////////////////////

/// Masks value with (0xffffffffffffffff maskshift maskamt), 
/// merges with (rt shift amt), saves to dummyValue
static void sdlrhelper_const(int maskamt, const xImpl_Group2& maskshift, int amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
{
	xMOV(rcx, -1);
	maskshift(rcx, maskamt);
	xAND(rcx, value);

	shift(rt, amt);
	xOR(rt, rcx);
}

/// Masks value with (0xffffffffffffffff maskshift maskamt), 
/// merges with (rt shift amt), saves to dummyValue
static void sdlrhelper(const xRegister32& maskamt, const xImpl_Group2& maskshift, const xRegister32& amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
{
	// Generate mask 128-(shiftx8)
	const xRegister64 maskamt64(maskamt);
	xMOV(ecx, maskamt);
	xMOV(maskamt64, -1);
	maskshift(maskamt64, cl);
	xAND(maskamt64, value);

	// Shift over reg value
	xMOV(ecx, amt);
	shift(rt, cl);
	xOR(rt, maskamt64);
}

void recSDL(void)
{
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	_freeX86reg(ecx);
	_freeX86reg(arg2regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 adr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		u32 aligned = adr & ~0x07;
		u32 shift = ((adr & 0x7) + 1) * 8;
		if (shift == 64)
		{
			_eeMoveGPRtoR(arg2reg, _Rt_);
		}
		else
		{
			vtlb_DynGenReadNonQuad64_Const(aligned, RETURN_READ_IN_RAX);
			_eeMoveGPRtoR(arg2reg, _Rt_);
			sdlrhelper_const(shift, xSHL, 64 - shift, xSHR, rax, arg2reg);
		}
		vtlb_DynGenWrite_Const(64, false, aligned, arg2regd.Id);
	}
	else
	{
		if (_Rs_)
			_addNeededX86reg(X86TYPE_GPR, _Rs_);

		// Load ECX with the source memory address that we're reading from.
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		_freeX86reg(ecx);
		_freeX86reg(edx);
		_freeX86reg(arg2regd);
		const xRegister32 temp1(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
		const xRegister64 temp2(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
		_eeMoveGPRtoR(arg2reg, _Rt_);

		xMOV(temp1, arg1regd);
		xMOV(temp2, arg2reg);
		xAND(arg1regd, ~0x07);
		xAND(temp1, 0x7);
		xCMP(temp1, 7);

		// If we're not using fastmem, we need to flush early. Because the first read
		// (which would flush) happens inside a branch.
		if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
			iFlushCall(FLUSH_FULLVTLB);

		xForwardJE8 skip;
		xADD(temp1, 1);
		vtlb_DynGenReadNonQuad(64, false, false, arg1regd.Id, RETURN_READ_IN_RAX);

		//Calculate the shift from top bit to lowest
		xMOV(edx, 64);
		xSHL(temp1, 3);
		xSUB(edx, temp1);

		sdlrhelper(temp1, xSHL, edx, xSHR, rax, temp2);

		_eeMoveGPRtoR(arg1regd, _Rs_, false);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);
		xAND(arg1regd, ~0x7);
		skip.SetTarget();

		vtlb_DynGenWrite(64, false, arg1regd.Id, temp2.Id);
		_freeX86reg(temp2.Id);
		_freeX86reg(temp1.Id);
	}
}

////////////////////////////////////////////////////
void recSDR(void)
{
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	_freeX86reg(ecx);
	_freeX86reg(arg2regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 adr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		u32 aligned = adr & ~0x07;
		u32 shift = (adr & 0x7) * 8;
		if (shift == 0)
		{
			_eeMoveGPRtoR(arg2reg, _Rt_);
		}
		else
		{
			vtlb_DynGenReadNonQuad64_Const(aligned, RETURN_READ_IN_RAX);
			_eeMoveGPRtoR(arg2reg, _Rt_);
			sdlrhelper_const(64 - shift, xSHR, shift, xSHL, rax, arg2reg);
		}

		vtlb_DynGenWrite_Const(64, false, aligned, arg2reg.Id);
	}
	else
	{
		if (_Rs_)
			_addNeededX86reg(X86TYPE_GPR, _Rs_);

		// Load ECX with the source memory address that we're reading from.
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		_freeX86reg(ecx);
		_freeX86reg(edx);
		_freeX86reg(arg2regd);
		const xRegister32 temp1(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
		const xRegister64 temp2(_allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED));
		_eeMoveGPRtoR(arg2reg, _Rt_);

		xMOV(temp1, arg1regd);
		xMOV(temp2, arg2reg);
		xAND(arg1regd, ~0x07);
		xAND(temp1, 0x7);

		// If we're not using fastmem, we need to flush early. Because the first read
		// (which would flush) happens inside a branch.
		if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
			iFlushCall(FLUSH_FULLVTLB);

		xForwardJE8 skip;
		vtlb_DynGenReadNonQuad(64, false, false, arg1regd.Id, RETURN_READ_IN_RAX);

		xMOV(edx, 64);
		xSHL(temp1, 3);
		xSUB(edx, temp1);

		sdlrhelper(edx, xSHR, temp1, xSHL, rax, temp2);

		_eeMoveGPRtoR(arg1regd, _Rs_, false);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);
		xAND(arg1regd, ~0x7);
		xMOV(arg2reg, temp2);
		skip.SetTarget();

		vtlb_DynGenWrite(64, false, arg1regd.Id, temp2.Id);
		_freeX86reg(temp2.Id);
		_freeX86reg(temp1.Id);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
/*********************************************************
* Load and store for COP1                                *
* Format:  OP rt, offset(base)                           *
*********************************************************/

////////////////////////////////////////////////////

void recLWC1(void)
{
#ifndef FPU_RECOMPILE
	recCall(::R5900::Interpreter::OpcodeImpl::LWC1);
#else

	const vtlb_ReadRegAllocCallback alloc_cb = []() { return _allocFPtoXMMreg(_Rt_, MODE_WRITE); };
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenReadNonQuad32_Const(addr, alloc_cb);
	}
	else
	{
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		vtlb_DynGenReadNonQuad(32, false, true, arg1regd.Id, alloc_cb);
	}
#endif
}

//////////////////////////////////////////////////////

void recSWC1(void)
{
#ifndef FPU_RECOMPILE
	recCall(::R5900::Interpreter::OpcodeImpl::SWC1);
#else
	const int regt = _allocFPtoXMMreg(_Rt_, MODE_READ);
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(32, true, addr, regt);
	}
	else
	{
		_freeX86reg(arg1regd);
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);

		vtlb_DynGenWrite(32, true, arg1regd.Id, regt);
	}
#endif
}

#endif

/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
#ifndef MOVE_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(LUI, _Rt_);
REC_FUNC_DEL(MFLO, _Rd_);
REC_FUNC_DEL(MFHI, _Rd_);
REC_FUNC(MTLO);
REC_FUNC(MTHI);

REC_FUNC_DEL(MFLO1, _Rd_);
REC_FUNC_DEL(MFHI1, _Rd_);
REC_FUNC(MTHI1);
REC_FUNC(MTLO1);

REC_FUNC_DEL(MOVZ, _Rd_);
REC_FUNC_DEL(MOVN, _Rd_);

#else

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/

void recLUI(void)
{
	if (!_Rt_)
		return;

	// need to flush the upper 64 bits for xmm
	GPR_DEL_CONST(_Rt_);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);

	GPR_SET_CONST(_Rt_);
	g_cpuConstRegs[_Rt_].UD[0] = (s32)(cpuRegs.code << 16);
}

static void recMFHILO(bool hi, bool upper)
{
	if (!_Rd_)
		return;

	// kill any constants on rd, lower 64 bits get written regardless of upper
	_eeOnWriteReg(_Rd_, 0);

	const int reg = hi ? XMMGPR_HI : XMMGPR_LO;
	const int xmmd = EEINST_XMMUSEDTEST(_Rd_) ? _allocGPRtoXMMreg(_Rd_, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_READ | MODE_WRITE);
	const int xmmhilo = EEINST_XMMUSEDTEST(reg) ? _allocGPRtoXMMreg(reg, MODE_READ) : _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ);
	if (xmmd >= 0)
	{
		if (xmmhilo >= 0)
		{
			if (upper)
				xMOVHL.PS(xRegisterSSE(xmmd), xRegisterSSE(xmmhilo));
			else
				xMOVSD(xRegisterSSE(xmmd), xRegisterSSE(xmmhilo));
		}
		else
		{
			const int gprhilo = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_READ);
			if (gprhilo >= 0)
				xPINSR.Q(xRegisterSSE(xmmd), xRegister64(gprhilo), 0);
			else
				xPINSR.Q(xRegisterSSE(xmmd), ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]], 0);
		}
	}
	else
	{
		// try rename {hi,lo} -> rd
		const int gprreg = upper ? -1 : _checkX86reg(X86TYPE_GPR, reg, MODE_READ);
		if (gprreg >= 0 && _eeTryRenameReg(_Rd_, reg, gprreg, -1, 0) >= 0)
			return;

		const int gprd = _allocIfUsedGPRtoX86(_Rd_, MODE_WRITE);
		if (gprd >= 0 && xmmhilo >= 0)
		{
			if (upper)
				xPEXTR.Q(xRegister64(gprd), xRegisterSSE(xmmhilo), 1);
			else
				xMOVD(xRegister64(gprd), xRegisterSSE(xmmhilo));
		}
		else if (gprd < 0 && xmmhilo >= 0)
		{
			if (upper)
				xPEXTR.Q(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], xRegisterSSE(xmmhilo), 1);
			else
				xMOVQ(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], xRegisterSSE(xmmhilo));
		}
		else if (gprd >= 0)
		{
			if (gprreg >= 0)
				xMOV(xRegister64(gprd), xRegister64(gprreg));
			else
				xMOV(xRegister64(gprd), ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]]);
		}
		else if (gprreg >= 0)
		{
			xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], xRegister64(gprreg));
		}
		else
		{
			xMOV(rax, ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]]);
			xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], rax);
		}
	}
}

static void recMTHILO(bool hi, bool upper)
{
	const int reg = hi ? XMMGPR_HI : XMMGPR_LO;
	_eeOnWriteReg(reg, 0);

	const int xmms = EEINST_XMMUSEDTEST(_Rs_) ? _allocGPRtoXMMreg(_Rs_, MODE_READ) : _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ);
	const int xmmhilo = EEINST_XMMUSEDTEST(reg) ? _allocGPRtoXMMreg(reg, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ | MODE_WRITE);
	if (xmms >= 0)
	{
		if (xmmhilo >= 0)
		{
			if (upper)
				xMOVLH.PS(xRegisterSSE(xmmhilo), xRegisterSSE(xmms));
			else
				xMOVSD(xRegisterSSE(xmmhilo), xRegisterSSE(xmms));
		}
		else
		{
			const int gprhilo = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_WRITE);
			if (gprhilo >= 0)
				xMOVD(xRegister64(gprhilo), xRegisterSSE(xmms)); // actually movq
			else
				xMOVQ(ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]], xRegisterSSE(xmms));
		}
	}
	else
	{
		int gprs = _allocIfUsedGPRtoX86(_Rs_, MODE_READ);

		if (xmmhilo >= 0)
		{
			if (gprs >= 0)
			{
				xPINSR.Q(xRegisterSSE(xmmhilo), xRegister64(gprs), static_cast<u8>(upper));
			}
			else if (GPR_IS_CONST1(_Rs_))
			{
				// force it into a register, since we need to load the constant anyway
				gprs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
				xPINSR.Q(xRegisterSSE(xmmhilo), xRegister64(gprs), static_cast<u8>(upper));
			}
			else
			{
				xPINSR.Q(xRegisterSSE(xmmhilo), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], static_cast<u8>(upper));
			}
		}
		else
		{
			// try rename rs -> {hi,lo}
			if (gprs >= 0 && !upper && _eeTryRenameReg(reg, _Rs_, gprs, -1, 0) >= 0)
				return;

			const int gprreg = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_WRITE);
			if (gprreg >= 0)
			{
				_eeMoveGPRtoR(xRegister64(gprreg), _Rs_);
			}
			else
			{
				// force into a register, since we need to load it to write anyway
				gprs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
				xMOV(ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]], xRegister64(gprs));
			}
		}
	}
}


void recMFHI(void)
{
	recMFHILO(true, false);
}

void recMFLO(void)
{
	recMFHILO(false, false);
}

void recMTHI(void)
{
	recMTHILO(true, false);
}

void recMTLO(void)
{
	recMTHILO(false, false);
}

void recMFHI1(void)
{
	recMFHILO(true, true);
}

void recMFLO1(void)
{
	recMFHILO(false, true);
}

void recMTHI1(void)
{
	recMTHILO(true, true);
}

void recMTLO1(void)
{
	recMTHILO(false, true);
}

//// MOVZ
// if (rt == 0) then rd <- rs
static void recMOVZtemp_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0];
}

static void recMOVZtemp_consts(int info)
{
	// we need the constant anyway, so just force it into a register
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	if (info & PROCESS_EE_T)
		xTEST(xRegister64(EEREC_T), xRegister64(EEREC_T));
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]], 0);

	xCMOVE(xRegister64(EEREC_D), xRegister64(regs));
}

static void recMOVZtemp_constt(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
}

static void recMOVZtemp_(int info)
{
	if (info & PROCESS_EE_T)
		xTEST(xRegister64(EEREC_T), xRegister64(EEREC_T));
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]], 0);

	if (info & PROCESS_EE_S)
		xCMOVE(xRegister64(EEREC_D), xRegister64(EEREC_S));
	else
		xCMOVE(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
}

// Specify READD here, because we might not write to it, and want to preserve the value.
static EERECOMPILE_CODERC0(MOVZtemp, XMMINFO_READS | XMMINFO_READT | XMMINFO_READD | XMMINFO_WRITED | XMMINFO_NORENAME);

void recMOVZ(void)
{
	if (_Rs_ == _Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_) && g_cpuConstRegs[_Rt_].UD[0] != 0)
		return;

	recMOVZtemp();
}

static void recMOVNtemp_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0];
}

static void recMOVNtemp_consts(int info)
{
	// we need the constant anyway, so just force it into a register
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	if (info & PROCESS_EE_T)
		xTEST(xRegister64(EEREC_T), xRegister64(EEREC_T));
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]], 0);

	xCMOVNE(xRegister64(EEREC_D), xRegister64(regs));
}

static void recMOVNtemp_constt(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
}

static void recMOVNtemp_(int info)
{
	if (info & PROCESS_EE_T)
		xTEST(xRegister64(EEREC_T), xRegister64(EEREC_T));
	else
		xCMP(ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]], 0);

	if (info & PROCESS_EE_S)
		xCMOVNE(xRegister64(EEREC_D), xRegister64(EEREC_S));
	else
		xCMOVNE(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
}

static EERECOMPILE_CODERC0(MOVNtemp, XMMINFO_READS | XMMINFO_READT | XMMINFO_READD | XMMINFO_WRITED | XMMINFO_NORENAME);

void recMOVN(void)
{
	if (_Rs_ == _Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_) && g_cpuConstRegs[_Rt_].UD[0] == 0)
		return;

	recMOVNtemp();
}

#endif

/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/
#ifndef MULTDIV_RECOMPILE

REC_FUNC_DEL(MULT, _Rd_);
REC_FUNC_DEL(MULTU, _Rd_);
REC_FUNC_DEL(MULT1, _Rd_);
REC_FUNC_DEL(MULTU1, _Rd_);

REC_FUNC(DIV);
REC_FUNC(DIVU);
REC_FUNC(DIV1);
REC_FUNC(DIVU1);

REC_FUNC_DEL(MADD, _Rd_);
REC_FUNC_DEL(MADDU, _Rd_);
REC_FUNC_DEL(MADD1, _Rd_);
REC_FUNC_DEL(MADDU1, _Rd_);

#else

static void recWritebackHILO(int info, bool writed, bool upper)
{
	// writeback low 32 bits, sign extended to 64 bits
	bool eax_sign_extended = false;

	// case 1: LO is already in an XMM - use the xmm
	// case 2: LO is used as an XMM later in the block - use or allocate the XMM
	// case 3: LO is used as a GPR later in the block - use XMM if upper, otherwise use GPR, so it can be renamed
	// case 4: LO is already in a GPR - write to the GPR, or write to memory if upper
	// case 4: LO is not used - writeback to memory

	if (EEINST_LIVETEST(XMMGPR_LO))
	{
		const bool loused = EEINST_USEDTEST(XMMGPR_LO);
		const bool lousedxmm = loused && (upper || EEINST_XMMUSEDTEST(XMMGPR_LO));
		const int xmmlo = lousedxmm ? _allocGPRtoXMMreg(XMMGPR_LO, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_LO, MODE_WRITE);
		if (xmmlo >= 0)
		{
			// we use CDQE over MOVSX because it's shorter.
			*(u16*)x86Ptr = 0x9848;
			x86Ptr += sizeof(u16);
			xPINSR.Q(xRegisterSSE(xmmlo), rax, static_cast<u8>(upper));
		}
		else
		{
			const int gprlo = upper ? -1 : (loused ? _allocX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE));
			if (gprlo >= 0)
			{
				xMOVSX(xRegister64(gprlo), eax);
			}
			else
			{
				*(u16*)x86Ptr = 0x9848;
				x86Ptr += sizeof(u16);
				eax_sign_extended = true;
				xMOV(ptr64[&cpuRegs.LO.UD[upper]], rax);
			}
		}
	}

	if (EEINST_LIVETEST(XMMGPR_HI))
	{
		const bool hiused = EEINST_USEDTEST(XMMGPR_HI);
		const bool hiusedxmm = hiused && (upper || EEINST_XMMUSEDTEST(XMMGPR_HI));
		const int xmmhi = hiusedxmm ? _allocGPRtoXMMreg(XMMGPR_HI, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_HI, MODE_WRITE);
		if (xmmhi >= 0)
		{
			xMOVSX(rdx, edx);
			xPINSR.Q(xRegisterSSE(xmmhi), rdx, static_cast<u8>(upper));
		}
		else
		{
			const int gprhi = upper ? -1 : (hiused ? _allocX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE));
			if (gprhi >= 0)
			{
				xMOVSX(xRegister64(gprhi), edx);
			}
			else
			{
				xMOVSX(rdx, edx);
				xMOV(ptr64[&cpuRegs.HI.UD[upper]], rdx);
			}
		}
	}

	// writeback lo to Rd if present
	if (writed && _Rd_ && EEINST_LIVETEST(_Rd_))
	{
		// TODO: This can be made optimal by keeping it in an xmm.
		// But currently the templates aren't hooked up for that - we'd need a "allow xmm" flag.
		if (info & PROCESS_EE_D)
		{
			if (eax_sign_extended)
				xMOV(xRegister64(EEREC_D), rax);
			else
				xMOVSX(xRegister64(EEREC_D), eax);
		}
		else
		{
			if (!eax_sign_extended)
			{
				*(u16*)x86Ptr = 0x9848;
				x86Ptr += sizeof(u16);
			}
			xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], rax);
		}
	}
}


static void recWritebackConstHILO(u64 res, bool writed, int upper)
{
	// It's not often that MULT/DIV are entirely constant. So while the MOV64s here are not optimal
	// by any means, it's not something that's going to be hit often enough to worry about a cache.
	// Except for apparently when it's getting set to all-zeros, but that'll be fine with immediates.
	const s64 loval = static_cast<s64>(static_cast<s32>(static_cast<u32>(res)));
	const s64 hival = static_cast<s64>(static_cast<s32>(static_cast<u32>(res >> 32)));

	if (EEINST_LIVETEST(XMMGPR_LO))
	{
		const bool lolive = EEINST_USEDTEST(XMMGPR_LO);
		const bool lolivexmm = lolive && (upper || EEINST_XMMUSEDTEST(XMMGPR_LO));
		const int xmmlo = lolivexmm ? _allocGPRtoXMMreg(XMMGPR_LO, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_LO, MODE_WRITE);
		if (xmmlo >= 0)
		{
			xMOV64(rax, loval);
			xPINSR.Q(xRegisterSSE(xmmlo), rax, static_cast<u8>(upper));
		}
		else
		{
			const int gprlo = upper ? -1 : (lolive ? _allocX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE));
			if (gprlo >= 0)
				xImm64Op(xMOV, xRegister64(gprlo), rax, loval);
			else
				xImm64Op(xMOV, ptr64[&cpuRegs.LO.UD[upper]], rax, loval);
		}
	}

	if (EEINST_LIVETEST(XMMGPR_HI))
	{
		const bool hilive = EEINST_USEDTEST(XMMGPR_HI);
		const bool hilivexmm = hilive && (upper || EEINST_XMMUSEDTEST(XMMGPR_HI));
		const int xmmhi = hilivexmm ? _allocGPRtoXMMreg(XMMGPR_HI, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_HI, MODE_WRITE);
		if (xmmhi >= 0)
		{
			xMOV64(rax, hival);
			xPINSR.Q(xRegisterSSE(xmmhi), rax, static_cast<u8>(upper));
		}
		else
		{
			const int gprhi = upper ? -1 : (hilive ? _allocX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE));
			if (gprhi >= 0)
				xImm64Op(xMOV, xRegister64(gprhi), rax, hival);
			else
				xImm64Op(xMOV, ptr64[&cpuRegs.HI.UD[upper]], rax, hival);
		}
	}

	// writeback lo to Rd if present
	if (writed && _Rd_ && EEINST_LIVETEST(_Rd_))
	{
		_eeOnWriteReg(_Rd_, 0);

		const int regd = _checkX86reg(X86TYPE_GPR, _Rd_, MODE_WRITE);
		if (regd >= 0)
			xImm64Op(xMOV, xRegister64(regd), rax, loval);
		else
			xImm64Op(xMOV, ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], rax, loval);
	}
}

//// MULT
static void recMULT_const(void)
{
	s64 res = (s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0];

	recWritebackConstHILO(res, 1, 0);
}

static void recMULTsuper(int info, bool sign, bool upper, int process)
{
	// TODO(Stenzek): Use MULX where available.
	if (process & PROCESS_CONSTS)
	{
		xMOV(eax, g_cpuConstRegs[_Rs_].UL[0]);
		if (info & PROCESS_EE_T)
			sign ? xMUL(xRegister32(EEREC_T)) : xUMUL(xRegister32(EEREC_T));
		else
			sign ? xMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]) : xUMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}
	else if (process & PROCESS_CONSTT)
	{
		xMOV(eax, g_cpuConstRegs[_Rt_].UL[0]);
		if (info & PROCESS_EE_S)
			sign ? xMUL(xRegister32(EEREC_S)) : xUMUL(xRegister32(EEREC_S));
		else
			sign ? xMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]) : xUMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}
	else
	{
		// S is more likely to be in a register than T (so put T in eax).
		if (info & PROCESS_EE_T)
			xMOV(eax, xRegister32(EEREC_T));
		else
			xMOV(eax, ptr[&cpuRegs.GPR.r[_Rt_].UL[0]]);

		if (info & PROCESS_EE_S)
			sign ? xMUL(xRegister32(EEREC_S)) : xUMUL(xRegister32(EEREC_S));
		else
			sign ? xMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]) : xUMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}

	recWritebackHILO(info, 1, upper);
}

static void recMULT_(int info)
{
	recMULTsuper(info, true, false, 0);
}

static void recMULT_consts(int info)
{
	recMULTsuper(info, true, false, PROCESS_CONSTS);
}

static void recMULT_constt(int info)
{
	recMULTsuper(info, true, false, PROCESS_CONSTT);
}

// lo/hi allocation are taken care of in recWritebackHILO().
EERECOMPILE_CODERC0(MULT, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

//// MULTU
static void recMULTU_const(void)
{
	const u64 res = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

	recWritebackConstHILO(res, 1, 0);
}

static void recMULTU_(int info)
{
	recMULTsuper(info, false, false, 0);
}

static void recMULTU_consts(int info)
{
	recMULTsuper(info, false, false, PROCESS_CONSTS);
}

static void recMULTU_constt(int info)
{
	recMULTsuper(info, false, false, PROCESS_CONSTT);
}

// don't specify XMMINFO_WRITELO or XMMINFO_WRITEHI, that is taken care of
EERECOMPILE_CODERC0(MULTU, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

////////////////////////////////////////////////////
static void recMULT1_const(void)
{
	s64 res = (s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0];

	recWritebackConstHILO((u64)res, 1, 1);
}

static void recMULT1_(int info)
{
	recMULTsuper(info, true, true, 0);
}

static void recMULT1_consts(int info)
{
	recMULTsuper(info, true, true, PROCESS_CONSTS);
}

static void recMULT1_constt(int info)
{
	recMULTsuper(info, true, true, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(MULT1, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

////////////////////////////////////////////////////
static void recMULTU1_const(void)
{
	u64 res = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

	recWritebackConstHILO(res, 1, 1);
}

static void recMULTU1_(int info)
{
	recMULTsuper(info, false, true, 0);
}

static void recMULTU1_consts(int info)
{
	recMULTsuper(info, false, true, PROCESS_CONSTS);
}

static void recMULTU1_constt(int info)
{
	recMULTsuper(info, false, true, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(MULTU1, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

//// DIV

static void recDIVconst(int upper)
{
	s32 quot, rem = 0;
	if (g_cpuConstRegs[_Rs_].UL[0] == 0x80000000 && g_cpuConstRegs[_Rt_].SL[0] == -1)
		quot = (s32)0x80000000;
	else if (g_cpuConstRegs[_Rt_].SL[0] != 0)
	{
		quot = g_cpuConstRegs[_Rs_].SL[0] / g_cpuConstRegs[_Rt_].SL[0];
		rem  = g_cpuConstRegs[_Rs_].SL[0] % g_cpuConstRegs[_Rt_].SL[0];
	}
	else
	{
		quot = (g_cpuConstRegs[_Rs_].SL[0] < 0) ? 1 : -1;
		rem  = g_cpuConstRegs[_Rs_].SL[0];
	}
	recWritebackConstHILO((u64)quot | ((u64)rem << 32), 0, upper);
}

static void recDIV_const(void)
{
	recDIVconst(0);
}

static void recDIVsuper(int info, bool sign, bool upper, int process)
{
	u8 *end1, *end2, *cont3;
	const xRegister32 divisor((info & PROCESS_EE_T) ? EEREC_T : ecx.Id);
	if (!(info & PROCESS_EE_T))
	{
		if (process & PROCESS_CONSTT)
			xMOV(divisor, g_cpuConstRegs[_Rt_].UL[0]);
		else
			xMOV(divisor, ptr[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}

	if (process & PROCESS_CONSTS)
		xMOV(eax, g_cpuConstRegs[_Rs_].UL[0]);
	else
		_eeMoveGPRtoR(rax, _Rs_);

	if (sign) //test for overflow (x86 will just throw an exception)
	{
		u8 *cont1, *cont2;
		xCMP(eax, 0x80000000);
		*(u8*)x86Ptr = JNE8;
		x86Ptr += sizeof(u8);
		*(u8*)x86Ptr = 0;
		x86Ptr += sizeof(u8);
		cont1       = (u8*)(x86Ptr - 1);
		xCMP(divisor, 0xffffffff);
		*(u8*)x86Ptr = JNE8;
		x86Ptr += sizeof(u8);
		*(u8*)x86Ptr = 0;
		x86Ptr += sizeof(u8);
		cont2       = (u8*)(x86Ptr - 1);
		//overflow case:
		xXOR(edx, edx); //EAX remains 0x80000000
		*(u8*)x86Ptr = 0xEB;
		x86Ptr += sizeof(u8);
		*(u8*)x86Ptr = 0;
		x86Ptr += sizeof(u8);
		end1        = x86Ptr - 1;

		*cont1      = (u8)((x86Ptr - cont1) - 1);
		*cont2      = (u8)((x86Ptr - cont2) - 1);
	}

	xCMP(divisor, 0);
	*(u8*)x86Ptr = JNE8;
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = 0;
	x86Ptr += sizeof(u8);
	cont3 = (u8*)(x86Ptr - 1);
	//divide by zero
	xMOV(edx, eax);
	if (sign) //set EAX to (EAX < 0)?1:-1
	{
		xSAR(eax, 31); //(EAX < 0)?-1:0
		xSHL(eax, 1); //(EAX < 0)?-2:0
		xNOT(eax); //(EAX < 0)?1:-1
	}
	else
		xMOV(eax, 0xffffffff);
	*(u8*)x86Ptr = 0xEB;
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = 0;
	x86Ptr += sizeof(u8);
	end2        = x86Ptr - 1;

	*cont3      = (u8)((x86Ptr - cont3) - 1);
	if (sign)
	{
		*(u8*)x86Ptr = 0x99;
		x86Ptr += sizeof(u8);
		xDIV(divisor);
	}
	else
	{
		xXOR(edx, edx);
		xUDIV(divisor);
	}

	if (sign)
		*end1      = (u8)((x86Ptr - end1) - 1);
	*end2      = (u8)((x86Ptr - end2) - 1);

	// need to execute regardless of bad divide
	recWritebackHILO(info, false, upper);
}

static void recDIV_(int info)
{
	recDIVsuper(info, 1, 0, 0);
}

static void recDIV_consts(int info)
{
	recDIVsuper(info, 1, 0, PROCESS_CONSTS);
}

static void recDIV_constt(int info)
{
	recDIVsuper(info, 1, 0, PROCESS_CONSTT);
}

// We handle S reading in the routine itself, since it needs to go into eax.
EERECOMPILE_CODERC0(DIV, /*XMMINFO_READS |*/ XMMINFO_READT);

//// DIVU
static void recDIVUconst(int upper)
{
	u32 quot, rem;
	if (g_cpuConstRegs[_Rt_].UL[0] != 0)
	{
		quot = g_cpuConstRegs[_Rs_].UL[0] / g_cpuConstRegs[_Rt_].UL[0];
		rem = g_cpuConstRegs[_Rs_].UL[0] % g_cpuConstRegs[_Rt_].UL[0];
	}
	else
	{
		quot = 0xffffffff;
		rem = g_cpuConstRegs[_Rs_].UL[0];
	}

	recWritebackConstHILO((u64)quot | ((u64)rem << 32), 0, upper);
}

static void recDIVU_const(void)
{
	recDIVUconst(0);
}

static void recDIVU_(int info)
{
	recDIVsuper(info, false, false, 0);
}

static void recDIVU_consts(int info)
{
	recDIVsuper(info, false, false, PROCESS_CONSTS);
}

static void recDIVU_constt(int info)
{
	recDIVsuper(info, false, false, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(DIVU, /*XMMINFO_READS |*/ XMMINFO_READT);

static void recDIV1_const(void)
{
	recDIVconst(1);
}

static void recDIV1_(int info)
{
	recDIVsuper(info, true, true, 0);
}

static void recDIV1_consts(int info)
{
	recDIVsuper(info, true, true, PROCESS_CONSTS);
}

static void recDIV1_constt(int info)
{
	recDIVsuper(info, true, true, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(DIV1, /*XMMINFO_READS |*/ XMMINFO_READT);

static void recDIVU1_const()
{
	recDIVUconst(1);
}

static void recDIVU1_(int info)
{
	recDIVsuper(info, false, true, 0);
}

static void recDIVU1_consts(int info)
{
	recDIVsuper(info, false, true, PROCESS_CONSTS);
}

static void recDIVU1_constt(int info)
{
	recDIVsuper(info, false, true, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(DIVU1, /*XMMINFO_READS |*/ XMMINFO_READT);

// TODO(Stenzek): All of these :(

static void writeBackMAddToHiLoRd(int hiloID)
{
	// eax -> LO, edx -> HI
	*(u16*)x86Ptr = 0x9848;
	x86Ptr += sizeof(u16);
	if (_Rd_)
	{
		_eeOnWriteReg(_Rd_, 1);
		_deleteEEreg(_Rd_, 0);
		xMOV(ptr[&cpuRegs.GPR.r[_Rd_].UD[0]], rax);
	}
	xMOV(ptr[&cpuRegs.LO.UD[hiloID]], rax);

	xMOVSX(rax, edx);
	xMOV(ptr[&cpuRegs.HI.UD[hiloID]], rax);
}

static void addConstantAndWriteBackToHiLoRd(int hiloID, u64 constant)
{
	const xRegister32& ehi = edx;

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);

	xMOV(eax, ptr[&cpuRegs.LO.UL[hiloID * 2]]);
	xMOV(ehi, ptr[&cpuRegs.HI.UL[hiloID * 2]]);
	xADD(eax, (u32)(constant & 0xffffffff));
	xADC(ehi, (u32)(constant >> 32));
	writeBackMAddToHiLoRd(hiloID);
}

static void addEaxEdxAndWriteBackToHiLoRd(int hiloID)
{
	xADD(eax, ptr[&cpuRegs.LO.UL[hiloID * 2]]);
	xADC(edx, ptr[&cpuRegs.HI.UL[hiloID * 2]]);

	writeBackMAddToHiLoRd(hiloID);
}

void recMADD()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0]);
		addConstantAndWriteBackToHiLoRd(0, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(eax, g_cpuConstRegs[_Rs_].UL[0]);
		xMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xMOV(eax, g_cpuConstRegs[_Rt_].UL[0]);
		xMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
		xMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}

	addEaxEdxAndWriteBackToHiLoRd(0);
}

void recMADDU()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0]);
		addConstantAndWriteBackToHiLoRd(0, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(eax, g_cpuConstRegs[_Rs_].UL[0]);
		xUMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xMOV(eax, g_cpuConstRegs[_Rt_].UL[0]);
		xUMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
		xUMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}

	addEaxEdxAndWriteBackToHiLoRd(0);
}

void recMADD1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0]);
		addConstantAndWriteBackToHiLoRd(1, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(eax, g_cpuConstRegs[_Rs_].UL[0]);
		xMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xMOV(eax, g_cpuConstRegs[_Rt_].UL[0]);
		xMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
		xMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}

	addEaxEdxAndWriteBackToHiLoRd(1);
}

void recMADDU1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0]);
		addConstantAndWriteBackToHiLoRd(1, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(eax, g_cpuConstRegs[_Rs_].UL[0]);
		xUMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xMOV(eax, g_cpuConstRegs[_Rt_].UL[0]);
		xUMUL(ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
		xUMUL(ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	}

	addEaxEdxAndWriteBackToHiLoRd(1);
}
#endif

/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
#ifndef SHIFT_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(SLL, _Rd_);
REC_FUNC_DEL(SRL, _Rd_);
REC_FUNC_DEL(SRA, _Rd_);
REC_FUNC_DEL(DSLL, _Rd_);
REC_FUNC_DEL(DSRL, _Rd_);
REC_FUNC_DEL(DSRA, _Rd_);
REC_FUNC_DEL(DSLL32, _Rd_);
REC_FUNC_DEL(DSRL32, _Rd_);
REC_FUNC_DEL(DSRA32, _Rd_);

REC_FUNC_DEL(SLLV, _Rd_);
REC_FUNC_DEL(SRLV, _Rd_);
REC_FUNC_DEL(SRAV, _Rd_);
REC_FUNC_DEL(DSLLV, _Rd_);
REC_FUNC_DEL(DSRLV, _Rd_);
REC_FUNC_DEL(DSRAV, _Rd_);

#else

static void recSLL_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << _Sa_);
}

static void recSLLs_(int info, int sa)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	if (sa != 0)
		xSHL(xRegister32(EEREC_D), sa);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSLL_(int info)
{
	recSLLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SLL, XMMINFO_WRITED | XMMINFO_READT);

static void recSRL_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> _Sa_);
}

static void recSRLs_(int info, int sa)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	if (sa != 0)
		xSHR(xRegister32(EEREC_D), sa);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSRL_(int info) { recSRLs_(info, _Sa_); }

EERECOMPILE_CODEX(eeRecompileCodeRC2, SRL, XMMINFO_WRITED | XMMINFO_READT);

static void recSRA_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> _Sa_);
}

static void recSRAs_(int info, int sa)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	if (sa != 0)
		xSAR(xRegister32(EEREC_D), sa);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSRA_(int info)
{
	recSRAs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SRA, XMMINFO_WRITED | XMMINFO_READT);

////////////////////////////////////////////////////
static void recDSLL_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << _Sa_);
}

static void recDSLLs_(int info, int sa)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	if (sa != 0)
		xSHL(xRegister64(EEREC_D), sa);
}

static void recDSLL_(int info)
{
	recDSLLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSLL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

////////////////////////////////////////////////////
static void recDSRL_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> _Sa_);
}

static void recDSRLs_(int info, int sa)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	if (sa != 0)
		xSHR(xRegister64(EEREC_D), sa);
}

static void recDSRL_(int info)
{
	recDSRLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRA
static void recDSRA_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (u64)(g_cpuConstRegs[_Rt_].SD[0] >> _Sa_);
}

static void recDSRAs_(int info, int sa)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	if (sa != 0)
		xSAR(xRegister64(EEREC_D), sa);
}

static void recDSRA_(int info)
{
	recDSRAs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRA, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

///// DSLL32
static void recDSLL32_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << (_Sa_ + 32));
}

static void recDSLL32_(int info)
{
	recDSLLs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSLL32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRL32
static void recDSRL32_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> (_Sa_ + 32));
}

static void recDSRL32_(int info)
{
	recDSRLs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRL32, XMMINFO_WRITED | XMMINFO_READT);

//// DSRA32
static void recDSRA32_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (u64)(g_cpuConstRegs[_Rt_].SD[0] >> (_Sa_ + 32));
}

static void recDSRA32_(int info)
{
	recDSRAs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRA32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/

static void recSLLV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSLLV_consts(int info)
{
	recSLLs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSLLV_constt(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xMOV(xRegister32(EEREC_D), g_cpuConstRegs[_Rt_].UL[0]);
	xSHL(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSLLV_(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	xSHL(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

EERECOMPILE_CODERC0(SLLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// SRLV
static void recSRLV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRLV_consts(int info)
{
	recSRLs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSRLV_constt(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xMOV(xRegister32(EEREC_D), g_cpuConstRegs[_Rt_].UL[0]);
	xSHR(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSRLV_(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	xSHR(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

EERECOMPILE_CODERC0(SRLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// SRAV
static void recSRAV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRAV_consts(int info)
{
	recSRAs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSRAV_constt(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xMOV(xRegister32(EEREC_D), g_cpuConstRegs[_Rt_].UL[0]);
	xSAR(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSRAV_(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
	xSAR(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

EERECOMPILE_CODERC0(SRAV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// DSLLV
static void recDSLLV_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSLLV_consts(int info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSLLs_(info, sa);
}

static void recDSLLV_constt(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xMOV64(xRegister64(EEREC_D), g_cpuConstRegs[_Rt_].SD[0]);
	xSHL(xRegister64(EEREC_D), cl);
}

static void recDSLLV_(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	xSHL(xRegister64(EEREC_D), cl);
}

EERECOMPILE_CODERC0(DSLLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// DSRLV
static void recDSRLV_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSRLV_consts(int info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSRLs_(info, sa);
}

static void recDSRLV_constt(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xMOV64(xRegister64(EEREC_D), g_cpuConstRegs[_Rt_].SD[0]);
	xSHR(xRegister64(EEREC_D), cl);
}

static void recDSRLV_(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	xSHR(xRegister64(EEREC_D), cl);
}

EERECOMPILE_CODERC0(DSRLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// DSRAV
static void recDSRAV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s64)(g_cpuConstRegs[_Rt_].SD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSRAV_consts(int info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSRAs_(info, sa);
}

static void recDSRAV_constt(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	xMOV64(xRegister64(EEREC_D), g_cpuConstRegs[_Rt_].SD[0]);
	xSAR(xRegister64(EEREC_D), cl);
}

static void recDSRAV_(int info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
	xSAR(xRegister64(EEREC_D), cl);
}

EERECOMPILE_CODERC0(DSRAV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

#endif

} // namespace R5900::Dynarec::OpcodeImpl

////////////////////////////////////////////////////////////////
// Static Private Variables - R5900 Dynarec

#define X86

static RecompiledCodeReserve* recMem = NULL;
static u8* recRAMCopy = NULL;
static u8* recLutReserve_RAM = NULL;
static const size_t recLutSize = (Ps2MemSize::MainRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) * sizeof(intptr_t) / 4;

static BASEBLOCK* recRAM = NULL; // and the ptr to the blocks here
static BASEBLOCK* recROM = NULL; // and here
static BASEBLOCK* recROM1 = NULL; // also here
static BASEBLOCK* recROM2 = NULL; // also here

static BaseBlocks recBlocks;
static u8* recPtr = NULL;
static EEINST* s_pInstCache = NULL;
static u32 s_nInstCacheSize = 0;

static BASEBLOCK* s_pCurBlock = NULL;
static BASEBLOCKEX* s_pCurBlockEx = NULL;
static u32 s_nEndBlock = 0; // what pc the current block ends
static u32 s_branchTo;
static bool s_nBlockFF;

// save states for branches
static GPR_reg64 s_saveConstRegs[32];
static u32 s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = NULL;

static u32 s_savenBlockCycles = 0;

static void iBranchTest(u32 newpc);
static void ClearRecLUT(BASEBLOCK* base, int count);
static u32 scaleblockcycles(void);

void _eeFlushAllDirty(void)
{
	_flushXMMregs();
	_flushX86regs();

	// flush constants, do them all at once for slightly better codegen
	_flushConstRegs();
}

void _eeMoveGPRtoR(const xRegister32& to, int fromgpr, bool allow_preload)
{
	if (fromgpr == 0)
		xXOR(to, to);
	else if (GPR_IS_CONST1(fromgpr))
		xMOV(to, g_cpuConstRegs[fromgpr].UL[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
			xMOV(to, xRegister32(x86reg));
		else if (xmmreg >= 0)
			xMOVD(to, xRegisterSSE(xmmreg));
		else
			xMOV(to, ptr[&cpuRegs.GPR.r[fromgpr].UL[0]]);
	}
}

void _eeMoveGPRtoR(const xRegister64& to, int fromgpr, bool allow_preload)
{
	if (fromgpr == 0)
		xXOR(xRegister32(to), xRegister32(to));
	else if (GPR_IS_CONST1(fromgpr))
		xMOV64(to, g_cpuConstRegs[fromgpr].UD[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
			xMOV(to, xRegister64(x86reg));
		else if (xmmreg >= 0)
			xMOVD(to, xRegisterSSE(xmmreg));
		else
			xMOV(to, ptr32[&cpuRegs.GPR.r[fromgpr].UD[0]]);
	}
}

void _eeMoveGPRtoM(uintptr_t to, int fromgpr)
{
	if (GPR_IS_CONST1(fromgpr))
		xMOV(ptr32[(u32*)(to)], g_cpuConstRegs[fromgpr].UL[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
		{
			xMOV(ptr32[(void*)(to)], xRegister32(x86reg));
		}
		else if (xmmreg >= 0)
		{
			xMOVSS(ptr32[(void*)(to)], xRegisterSSE(xmmreg));
		}
		else
		{
			xMOV(eax, ptr32[&cpuRegs.GPR.r[fromgpr].UL[0]]);
			xMOV(ptr32[(void*)(to)], eax);
		}
	}
}

// Use this to call into interpreter functions that require an immediate branchtest
// to be done afterward (anything that throws an exception or enables interrupts, etc).
void recBranchCall(void (*func)())
{
	// In order to make sure a branch test is performed, the nextBranchCycle is set
	// to the current cpu cycle.

	xMOV(eax, ptr[&cpuRegs.cycle]);
	xMOV(ptr[&cpuRegs.nextEventCycle], eax);

	recCall(func);
	g_branch = 2;
}

void recCall(void (*func)())
{
	iFlushCall(FLUSH_INTERPRETER);
	xFastCall((const void*)func);
}

// =====================================================================================================
//  R5900 Dispatchers
// =====================================================================================================

static void recRecompile(const u32 startpc);
static void dyna_block_discard(u32 start, u32 sz);
static void dyna_page_reset(u32 start, u32 sz);

// Recompiled code buffer for EE recompiler dispatchers!
alignas(__pagesize) static u8 eeRecDispatchers[__pagesize];

static const void* DispatcherEvent = NULL;
static const void* DispatcherReg = NULL;
static const void* JITCompile = NULL;
static const void* EnterRecompiledCode = NULL;
static const void* DispatchBlockDiscard = NULL;
static const void* DispatchPageReset = NULL;

static fastjmp_buf m_SetJmp_StateCheck;

static void recEventTest(void)
{
	_cpuEventTest_Shared();

	if (eeRecExitRequested)
	{
		eeRecExitRequested = false;
		fastjmp_jmp(&m_SetJmp_StateCheck, 1);
	}
}

// Size is in dwords (4 bytes)
// When called from _DynGen_DispatchBlockDiscard, called when a block under manual protection fails it's pre-execution integrity check.
// (meaning the actual code area has been modified -- ie 
// dynamic modules being loaded or, less likely, self-modifying code)
static void recClear(u32 addr, u32 size)
{
	if ((addr) >= maxrecmem || !(recLUT[(addr) >> 16] + (addr & ~0xFFFFUL)))
		return;
	addr = HWADDR(addr);

	int blockidx = recBlocks.LastIndex(addr + size * 4 - 4);

	if (blockidx == -1)
		return;

	u32 lowerextent = (u32)-1, upperextent = 0, ceiling = (u32)-1;

	BASEBLOCKEX* pexblock = recBlocks[blockidx + 1];
	if (pexblock)
		ceiling = pexblock->startpc;

	int toRemoveLast = blockidx;

	while ((pexblock = recBlocks[blockidx]))
	{
		u32 blockstart = pexblock->startpc;
		u32 blockend = pexblock->startpc + pexblock->size * 4;
		BASEBLOCK* pblock = PC_GETBLOCK(blockstart);

		if (pblock == s_pCurBlock)
		{
			if (toRemoveLast != blockidx)
			{
				recBlocks.Remove((blockidx + 1), toRemoveLast);
			}
			toRemoveLast = --blockidx;
			continue;
		}

		if (blockend <= addr)
		{
			lowerextent = std::max(lowerextent, blockend);
			break;
		}

		lowerextent = std::min(lowerextent, blockstart);
		upperextent = std::max(upperextent, blockend);
		pblock->m_pFnptr = ((uintptr_t)JITCompile);

		blockidx--;
	}

	if (toRemoveLast != blockidx)
		recBlocks.Remove((blockidx + 1), toRemoveLast);

	upperextent = std::min(upperextent, ceiling);

	if (upperextent > lowerextent)
		ClearRecLUT(PC_GETBLOCK(lowerextent), upperextent - lowerextent);
}


// The address for all cleared blocks.  It recompiles the current pc and then
// dispatches to the recompiled block address.
static const void* _DynGen_JITCompile(void)
{
	u8* retval = x86Ptr;

	xFastCall((const void*)recRecompile, ptr32[&cpuRegs.pc]);

	// C equivalent:
	// u32 addr = cpuRegs.pc;
	// void(**base)() = (void(**)())recLUT[addr >> 16];
	// base[addr >> 2]();
	xMOV(eax, ptr[&cpuRegs.pc]);
	xMOV(ebx, eax);
	xSHR(eax, 16);
	xMOV(rcx, ptrNative[xComplexAddress(rcx, recLUT, rax * sizeof(intptr_t))]);
	xJMP(ptrNative[rbx * (sizeof(intptr_t) / 4) + rcx]);

	return retval;
}

// called when jumping to variable pc address
static const void* _DynGen_DispatcherReg(void)
{
	u8* retval = x86Ptr; // fallthrough target, can't align it!

	// C equivalent:
	// u32 addr = cpuRegs.pc;
	// void(**base)() = (void(**)())recLUT[addr >> 16];
	// base[addr >> 2]();
	xMOV(eax, ptr[&cpuRegs.pc]);
	xMOV(ebx, eax);
	xSHR(eax, 16);
	xMOV(rcx, ptrNative[xComplexAddress(rcx, recLUT, rax * sizeof(intptr_t))]);
	xJMP(ptrNative[rbx * (sizeof(intptr_t) / 4) + rcx]);

	return retval;
}

static const void* _DynGen_DispatcherEvent(void)
{
	u8* retval = x86Ptr;

	xFastCall((const void*)recEventTest);

	return retval;
}

static const void* _DynGen_EnterRecompiledCode(void)
{
	u8* retval = x86Ptr;

#ifdef _WIN32
	// Shadow space for Win32
	static constexpr u32 stack_size = 32 + 8;
#else
	// Stack still needs to be aligned
	static constexpr u32 stack_size = 8;
#endif

	// We never return through this function, instead we fastjmp() out.
	// So we don't need to worry about preserving callee-saved registers, but we do need to align the stack.
	xSUB(rsp, stack_size);

	if (CHECK_FASTMEM)
		xMOV(RFASTMEMBASE, ptrNative[&vtlb_private::vtlbdata.fastmem_base]);

	xJMP((const void*)DispatcherReg);

	return retval;
}

static const void* _DynGen_DispatchBlockDiscard(void)
{
	u8* retval = x86Ptr;
	xFastCall((const void*)recClear);
	xJMP((const void*)DispatcherReg);
	return retval;
}

static const void* _DynGen_DispatchPageReset(void)
{
	u8* retval = x86Ptr;
	xFastCall((const void*)dyna_page_reset);
	xJMP((const void*)DispatcherReg);
	return retval;
}

static void _DynGen_Dispatchers(void)
{
	PageProtectionMode mode;
	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	// In case init gets called multiple times:
	HostSys::MemProtect(eeRecDispatchers, __pagesize, mode);

	// clear the buffer to 0xcc (easier debugging).
	memset(eeRecDispatchers, 0xcc, __pagesize);

	x86Ptr = (u8*)eeRecDispatchers;

	// Place the EventTest and DispatcherReg stuff at the top, because they get called the
	// most and stand to benefit from strong alignment and direct referencing.
	DispatcherEvent = _DynGen_DispatcherEvent();
	DispatcherReg = _DynGen_DispatcherReg();

	JITCompile = _DynGen_JITCompile();
	EnterRecompiledCode = _DynGen_EnterRecompiledCode();
	DispatchBlockDiscard = _DynGen_DispatchBlockDiscard();
	DispatchPageReset = _DynGen_DispatchPageReset();

	mode.m_write = false;
	mode.m_exec  = true;
	HostSys::MemProtect(eeRecDispatchers, __pagesize, mode);

	recBlocks.SetJITCompile(JITCompile);
}


//////////////////////////////////////////////////////////////////////////////////////////
//

static __ri void ClearRecLUT(BASEBLOCK* base, int memsize)
{
	for (int i = 0; i < memsize / (int)sizeof(uintptr_t); i++)
		base[i].m_pFnptr = ((uintptr_t)JITCompile);
}

static void recReserve(void)
{
	if (recMem)
		return;

	/* R5900 Recompiler Cache */
	recMem = new RecompiledCodeReserve();
	recMem->Assign(GetVmMemory().CodeMemory(), HostMemoryMap::EErecOffset, 64 * _1mb);
}

static void recAlloc(void)
{
	if (!recRAMCopy)
		recRAMCopy = (u8*)_aligned_malloc(Ps2MemSize::MainRam, 4096);

	if (!recRAM)
		recLutReserve_RAM = (u8*)_aligned_malloc(recLutSize, 4096);

	BASEBLOCK* basepos = (BASEBLOCK*)recLutReserve_RAM;
	recRAM = basepos;
	basepos += (Ps2MemSize::MainRam / 4);
	recROM = basepos;
	basepos += (Ps2MemSize::Rom / 4);
	recROM1 = basepos;
	basepos += (Ps2MemSize::Rom1 / 4);
	recROM2 = basepos;
	basepos += (Ps2MemSize::Rom2 / 4);

	for (int i = 0; i < 0x10000; i++)
		recLUT_SetPage(recLUT, 0, 0, 0, i, 0);

	for (int i = 0x0000; i < (int)(Ps2MemSize::MainRam / 0x10000); i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x0000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x2000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x3000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x8000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xa000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xb000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xc000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xd000, i, i);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}

	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e48; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}

	if (s_pInstCache == NULL)
	{
		s_nInstCacheSize = 128;
		s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	}
}

alignas(16) static u8 manual_counter[Ps2MemSize::MainRam >> 12];


////////////////////////////////////////////////////
static void recResetRaw(void)
{
	recAlloc();

	recMem->Reset();
#if TODOFIXME
	x86Ptr = (u8*)*recMem;
#endif
	_DynGen_Dispatchers();
	vtlb_DynGenDispatchers();
#if TODOFIXME
	recPtr = x86Ptr;
#endif
	ClearRecLUT((BASEBLOCK*)recLutReserve_RAM, recLutSize);
	memset(recRAMCopy, 0, Ps2MemSize::MainRam);

	maxrecmem = 0;

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	recBlocks.Reset();
	mmap_ResetBlockTracking();
	vtlb_ClearLoadStoreInfo();

#ifndef TODOFIXME
	x86Ptr   = (u8*)*recMem;
	recPtr   = x86Ptr;
#endif

	g_branch = 0;
	g_resetEeScalingStats = true;
}

static void recShutdown(void)
{
	delete recMem;
	recMem = NULL;
	safe_aligned_free(recRAMCopy);
	safe_aligned_free(recLutReserve_RAM);

	recBlocks.Reset();

	recRAM = recROM = recROM1 = recROM2 = NULL;

	if (s_pInstCache)
		free(s_pInstCache);
	s_pInstCache     = NULL;
	s_nInstCacheSize = 0;
}

static void recSafeExitExecution(void)
{
	// If we're currently processing events, we can't safely jump out of the recompiler here, because we'll
	// leave things in an inconsistent state. So instead, we flag it for exiting once cpuEventTest() returns.
	// Exiting in the middle of a rec block with the registers unsaved would be a bad idea too..
	eeRecExitRequested = true;

	// Force an event test at the end of this block.
	if (!eeEventTestIsActive)
	{
		// EE is running.
		cpuRegs.nextEventCycle = 0;
	}
	else
	{
		// IOP might be running, so break out if so.
		if (psxRegs.iopCycleEE > 0)
		{
			psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
			psxRegs.iopCycleEE = 0;
		}
	}
}

static void recResetEE(void)
{
	if (eeCpuExecuting)
	{
		// get outta here as soon as we can
		eeRecNeedsReset = true;
		recSafeExitExecution();
		return;
	}

	recResetRaw();
}

static void recCancelInstruction(void) { }

static void recExecute(void)
{
	// Reset before we try to execute any code, if there's one pending.
	// We need to do this here, because if we reset while we're executing, it sets the "needs reset"
	// flag, which triggers a JIT exit (the fastjmp_set below), and eventually loops back here.
	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

	// setjmp will save the register context and will return 0
	// A call to longjmp will restore the context (included the eip/rip)
	// but will return the longjmp 2nd parameter (here 1)
	if (!fastjmp_set(&m_SetJmp_StateCheck))
	{
		eeCpuExecuting = true;

		((void(*)())EnterRecompiledCode)();

		// Generally unreachable code here ...
	}

	eeCpuExecuting = false;
}

////////////////////////////////////////////////////
void R5900::Dynarec::OpcodeImpl::recSYSCALL(void)
{
	if (GPR_IS_CONST1(3))
	{
		// If it's FlushCache or iFlushCache, we can skip it since we don't support cache in the JIT.
		if (g_cpuConstRegs[3].UC[0] == 0x64 || g_cpuConstRegs[3].UC[0] == 0x68)
		{
			// Emulate the amount of cycles it takes for the exception handlers to run
			// This number was found by using github.com/F0bes/flushcache-cycles
			s_nBlockCycles += 5650;
			return;
		}
	}
	recCall(R5900::Interpreter::OpcodeImpl::SYSCALL);
	g_branch = 2; // Indirect branch with event check.
}

////////////////////////////////////////////////////
void R5900::Dynarec::OpcodeImpl::recBREAK(void)
{
	recCall(R5900::Interpreter::OpcodeImpl::BREAK);
	g_branch = 2; // Indirect branch with event check.
}

void SetBranchReg(u32 reg)
{
	g_branch = 1;

	if (reg != 0xffffffff)
	{
		const bool swap = EmuConfig.Gamefixes.GoemonTlbHack ? false : TrySwapDelaySlot(reg, 0, 0, true);
		if (!swap)
		{
			const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
			_eeMoveGPRtoR(xRegister32(wbreg), reg);

			if (EmuConfig.Gamefixes.GoemonTlbHack)
			{
				xMOV(ecx, xRegister32(wbreg));
				vtlb_DynV2P();
				xMOV(xRegister32(wbreg), eax);
			}

			recompileNextInstruction(true, false);

			// the next instruction may have flushed the register.. so reload it if so.
			if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
			{
				xMOV(ptr[&cpuRegs.pc], xRegister32(wbreg));
				x86regs[wbreg].inuse = 0;
			}
			else
			{
				xMOV(eax, ptr[&cpuRegs.pcWriteback]);
				xMOV(ptr[&cpuRegs.pc], eax);
			}
		}
		else
		{
			if (GPR_IS_DIRTY_CONST(reg) || _hasX86reg(X86TYPE_GPR, reg, 0))
			{
				const int x86reg = _allocX86reg(X86TYPE_GPR, reg, MODE_READ);
				xMOV(ptr32[&cpuRegs.pc], xRegister32(x86reg));
			}
			else
			{
				_eeMoveGPRtoM((uintptr_t)&cpuRegs.pc, reg);
			}
		}
	}

	iFlushCall(FLUSH_EVERYTHING);

	iBranchTest(0xffffffff);
}

void SetBranchImm(u32 imm)
{
	g_branch = 1;

	// end the current block
	iFlushCall(FLUSH_EVERYTHING);
	xMOV(ptr32[&cpuRegs.pc], imm);
	iBranchTest(imm);
}

u8* recBeginThunk(void)
{
	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= (recMem->GetPtrEnd() - _64kb))
		eeRecNeedsReset = true;

	x86Ptr = (u8*)recPtr;
	recPtr = x86Ptr;
	x86Ptr = (u8*)recPtr;
	return recPtr;
}

u8* recEndThunk(void)
{
	u8* block_end = x86Ptr;

	recPtr = block_end;
	return block_end;
}

bool TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, bool allow_loadstore)
{
	if (g_recompilingDelaySlot)
		return false;

	const u32 opcode_encoded = *(u32*)PSM(pc);
	if (opcode_encoded == 0)
	{
		recompileNextInstruction(true, true);
		return true;
	}

	const u32 opcode_rs = ((opcode_encoded >> 21) & 0x1F);
	const u32 opcode_rt = ((opcode_encoded >> 16) & 0x1F);
	const u32 opcode_rd = ((opcode_encoded >> 11) & 0x1F);

	switch (opcode_encoded >> 26)
	{
		case 8: // ADDI
		case 9: // ADDIU
		case 10: // SLTI
		case 11: // SLTIU
		case 12: // ANDIU
		case 13: // ORI
		case 14: // XORI
		case 24: // DADDI
		case 25: // DADDIU
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				return false;
			break;

		case 26: // LDL
		case 27: // LDR
		case 30: // LQ
		case 31: // SQ
		case 32: // LB
		case 33: // LH
		case 34: // LWL
		case 35: // LW
		case 36: // LBU
		case 37: // LHU
		case 38: // LWR
		case 39: // LWU
		case 40: // SB
		case 41: // SH
		case 42: // SWL
		case 43: // SW
		case 44: // SDL
		case 45: // SDR
		case 46: // SWR
		case 55: // LD
		case 63: // SD
			 // We can't allow loadstore swaps for BC0x/BC2x, since they could affect the condition.
			if (!allow_loadstore || (rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				return false;
			break;

		case 15: // LUI
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
				return false;
			break;

		case 49: // LWC1
		case 57: // SWC1
		case 54: // LQC2
		case 62: // SQC2
			break;

		case 0: // SPECIAL
			switch (opcode_encoded & 0x3F)
			{
				case 0: // SLL
				case 2: // SRL
				case 3: // SRA
				case 4: // SLLV
				case 6: // SRLV
				case 7: // SRAV
				case 10: // MOVZ
				case 11: // MOVN
				case 20: // DSLLV
				case 22: // DSRLV
				case 23: // DSRAV
				case 24: // MULT
				case 25: // MULTU
				case 32: // ADD
				case 33: // ADDU
				case 34: // SUB
				case 35: // SUBU
				case 36: // AND
				case 37: // OR
				case 38: // XOR
				case 39: // NOR
				case 42: // SLT
				case 43: // SLTU
				case 44: // DADD
				case 45: // DADDU
				case 46: // DSUB
				case 47: // DSUBU
				case 56: // DSLL
				case 58: // DSRL
				case 59: // DSRA
				case 60: // DSLL32
				case 62: // DSRL31
				case 64: // DSRA32
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						return false;
					break;

				case 15: // SYNC
				case 26: // DIV
				case 27: // DIVU
					break;

				default:
					return false;
			}
			break;

		case 16: // COP0
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC0
				case 2: // CFC0
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						return false;
					break;

				case 4: // MTC0
				case 6: // CTC0
					break;

				case 16: // TLB (technically would be safe, but we don't use it anyway)
				default:
					return false;
			}
			break;
		case 17: // COP1
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC1
				case 2: // CFC1
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						return false;
					break;

				case 4: // MTC1
				case 6: // CTC1
				case 16: // S
					{
						const u32 funct = (opcode_encoded & 0x3F);
						// affects flags that we're comparing
						if (funct == 50 || funct == 52 || funct == 54) // C.EQ, C.LT, C.LE
							return false;
					}
					/* fallthrough */

				case 20: // W
					break;

				default:
					return false;
			}
			break;

		case 18: // COP2
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 8: // BC2XX
					return false;

				case 1: // QMFC2
				case 2: // CFC2
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						return false;
					break;

				default:
					break;
			}
			break;

		case 28: // MMI
			switch (opcode_encoded & 0x3F)
			{
				case 8: // MMI0
				case 9: // MMI1
				case 10: // MMI2
				case 40: // MMI3
				case 41: // MMI3
				case 52: // PSLLH
				case 54: // PSRLH
				case 55: // LSRAH
				case 60: // PSLLW
				case 62: // PSRLW
				case 63: // PSRAW
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && rd == opcode_rd))
						return false;
					break;

				default:
					return false;
			}
			break;

		default:
			return false;
	}

	recompileNextInstruction(true, true);
	return true;
}

void SaveBranchState(void)
{
	s_savenBlockCycles = s_nBlockCycles;
	memcpy(s_saveConstRegs, g_cpuConstRegs, sizeof(g_cpuConstRegs));
	s_saveHasConstReg = g_cpuHasConstReg;
	s_saveFlushedConstReg = g_cpuFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	memcpy(s_saveXMMregs, xmmregs, sizeof(xmmregs));
}

void LoadBranchState(void)
{
	s_nBlockCycles = s_savenBlockCycles;

	memcpy(g_cpuConstRegs, s_saveConstRegs, sizeof(g_cpuConstRegs));
	g_cpuHasConstReg = s_saveHasConstReg;
	g_cpuFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	memcpy(xmmregs, s_saveXMMregs, sizeof(xmmregs));
}

void iFlushCall(int flushtype)
{
	// Free registers that are not saved across function calls (x86-32 ABI):
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		if (         Register_IsCallerSaved(i)
			|| ((flushtype & FLUSH_FREE_VU0) && x86regs[i].type == X86TYPE_VIREG)
			|| ((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP)
			|| ((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (!xmmregs[i].inuse)
			continue;

		if (        RegisterSSE_IsCallerSaved(i)
			|| (flushtype & FLUSH_FREE_XMM) 
			|| ((flushtype & FLUSH_FREE_VU0) && xmmregs[i].type == XMMTYPE_VFREG))
		{
			_freeXMMreg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_FLUSH_XMM)
		_flushXMMregs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_flushConstRegs();

	if ((flushtype & FLUSH_PC) && !g_cpuFlushedPC)
	{
		xMOV(ptr32[&cpuRegs.pc], pc);
		g_cpuFlushedPC = true;
	}

	if ((flushtype & FLUSH_CODE) && !g_cpuFlushedCode)
	{
		xMOV(ptr32[&cpuRegs.code], cpuRegs.code);
		g_cpuFlushedCode = true;
	}

#if 0
	if ((flushtype == FLUSH_CAUSE) && !g_maySignalException)
	{
		if (g_recompilingDelaySlot)
			xOR(ptr32[&cpuRegs.CP0.n.Cause], 1 << 31); // BD
		g_maySignalException = true;
	}
#endif
}

// Note: scaleblockcycles() scales s_nBlockCycles respective to the EECycleRate value for manipulating the cycles of current block recompiling.
// s_nBlockCycles is 3 bit fixed point.  Divide by 8 when done!
// Scaling blocks under 40 cycles seems to produce countless problem, so let's try to avoid them.

#define DEFAULT_SCALED_BLOCKS() (s_nBlockCycles >> 3)

static u32 scaleblockcycles_calculation(void)
{
	const bool lowcycles = (s_nBlockCycles <= 40);
	const s8 cyclerate   = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles     = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = DEFAULT_SCALED_BLOCKS();

	else if (cyclerate > 1)
		scale_cycles = s_nBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = DEFAULT_SCALED_BLOCKS() / 1.3f; // Adds a mild 30% increase in clockspeed for value 1.

	else if (cyclerate == -1) // the mildest value.
		// These values were manually tuned to yield mild speedup with high compatibility
		scale_cycles = (s_nBlockCycles <= 80 || s_nBlockCycles > 168 ? 5 : 7) * s_nBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * s_nBlockCycles) >> 5;

	// Ensure block cycle count is never less than 1.
	return (scale_cycles < 1) ? 1 : scale_cycles;
}

static u32 scaleblockcycles(void)
{
	return scaleblockcycles_calculation();
}

u32 scaleblockcycles_clear(void)
{
	const u32 scaled   = scaleblockcycles_calculation();
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	const bool lowcycles = (s_nBlockCycles <= 40);

	if (!lowcycles && cyclerate > 1)
		s_nBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	else
		s_nBlockCycles &= 0x7;

	return scaled;
}

// Generates dynarec code for Event tests followed by a block dispatch (branch).
// Parameters:
//   newpc - address to jump to at the end of the block.  If newpc == 0xffffffff then
//   the jump is assumed to be to a register (dynamic).  For any other value the
//   jump is assumed to be static, in which case the block will be "hardlinked" after
//   the first time it's dispatched.
//
//   noDispatch - When set true, then jump to Dispatcher.  Used by the recs
//   for blocks which perform exception checks without branching (it's enabled by
//   setting "g_branch = 2";
static void iBranchTest(u32 newpc)
{
	// Check the Event scheduler if our "cycle target" has been reached.
	// Equiv code to:
	//    cpuRegs.cycle += blockcycles;
	//    if( cpuRegs.cycle > g_nextEventCycle ) { DoEvents(); }

	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo)
	{
		xMOV(eax, ptr32[&cpuRegs.nextEventCycle]);
		xADD(ptr32[&cpuRegs.cycle], scaleblockcycles());
		xCMP(eax, ptr32[&cpuRegs.cycle]);
		xCMOVS(eax, ptr32[&cpuRegs.cycle]);
		xMOV(ptr32[&cpuRegs.cycle], eax);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.cycle]);
		xADD(eax, scaleblockcycles());
		xMOV(ptr[&cpuRegs.cycle], eax); // update cycles
		xSUB(eax, ptr[&cpuRegs.nextEventCycle]);

		if (newpc == 0xffffffff)
			xJS(DispatcherReg);
		else
			recBlocks.Link(HWADDR(newpc), xJcc32(Jcc_Signed, 0));
	}
	xJMP((const void*)DispatcherEvent);
}

static int COP2DivUnitTimings(u32 code)
{
	// Note: Cycles are off by 1 since the check ignores the actual op, so they are off by 1
	switch (code & 0x3FF)
	{
		case 0x3BC: // DIV
		case 0x3BD: // SQRT
			return 6;
		case 0x3BE: // RSQRT
			return 12;
		default:
			break;
	}
	return 0; // Used mainly for WAITQ
}

static bool COP2IsQOP(u32 code)
{
	if (_Opcode_ == 022) // Not COP2 operation
	{
		if ((code & 0x3f) == 0x20) // VADDq
			return true;
		if ((code & 0x3f) == 0x21) // VMADDq
			return true;
		if ((code & 0x3f) == 0x24) // VSUBq
			return true;
		if ((code & 0x3f) == 0x25) // VMSUBq
			return true;
		if ((code & 0x3f) == 0x1C) // VMULq
			return true;
		if ((code & 0x7FF) == 0x1FC) // VMULAq
			return true;
		if ((code & 0x7FF) == 0x23C) // VADDAq
			return true;
		if ((code & 0x7FF) == 0x23D) // VMADDAq
			return true;
		if ((code & 0x7FF) == 0x27C) // VSUBAq
			return true;
		if ((code & 0x7FF) == 0x27D) // VMSUBAq
			return true;
	}

	return false;
}

/* returns nonzero value if reg has been written between [startpc, endpc-4] */
static u32 _recIsRegReadOrWritten(EEINST* pinst, int size, u8 xmmtype, u8 reg)
{
	u32 inst = 1;

	while (size-- > 0)
	{
		for (u32 i = 0; i < std::size(pinst->writeType); ++i)
		{
			if ((pinst->writeType[i] == xmmtype) && (pinst->writeReg[i] == reg))
				return inst;
		}

		for (u32 i = 0; i < std::size(pinst->readType); ++i)
		{
			if ((pinst->readType[i] == xmmtype) && (pinst->readReg[i] == reg))
				return inst;
		}

		++inst;
		pinst++;
	}

	return 0;
}

void recompileNextInstruction(bool delayslot, bool swapped_delay_slot)
{
	static int* s_pCode;

	if (EmuConfig.EnablePatches)
		ApplyDynamicPatches(pc);

	s_pCode = (int*)PSM(pc);

	const int old_code = cpuRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;

	cpuRegs.code = *(int*)s_pCode;

	if (!delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = false;
		g_cpuFlushedCode = false;
	}
	else
	{
		// increment after recompiling so that pc points to the branch during recompilation
		g_recompilingDelaySlot = true;
	}

	g_pCurInstInfo++;

	// pc might be past s_nEndBlock if the last instruction in the block is a DI.
	if (pc <= s_nEndBlock && (g_pCurInstInfo + (s_nEndBlock - pc) / 4 + 1) <= s_pInstCache + s_nInstCacheSize)
	{
		u32 i;
		int count;
		for (i = 0; i < iREGCNT_GPR; ++i)
		{
			if (x86regs[i].inuse)
			{
				count = _recIsRegReadOrWritten(g_pCurInstInfo, (s_nEndBlock - pc) / 4 + 1, x86regs[i].type, x86regs[i].reg);
				if (count > 0)
					x86regs[i].counter = 1000 - count;
				else
					x86regs[i].counter = 0;
			}
		}

		for (i = 0; i < iREGCNT_XMM; ++i)
		{
			if (xmmregs[i].inuse)
			{
				count = _recIsRegReadOrWritten(g_pCurInstInfo, (s_nEndBlock - pc) / 4 + 1, xmmregs[i].type, xmmregs[i].reg);
				if (count > 0)
					xmmregs[i].counter = 1000 - count;
				else
					xmmregs[i].counter = 0;
			}
		}
	}

	if (g_pCurInstInfo->info & EEINST_COP2_FLUSH_VU0_REGISTERS)
		_flushCOP2regs();

	const OPCODE& opcode = GetCurrentInstruction();

	// if this instruction is a jump or a branch, exit right away
	if (delayslot)
	{
		bool check_branch_delay = false;
		switch (_Opcode_)
		{
			case 0:
				switch (_Funct_)
				{
					case 8: // jr
					case 9: // jalr
						check_branch_delay = true;
						break;
				}
				break;

			case 1:
				switch (_Rt_)
				{
					case 0:
					case 1:
					case 2:
					case 3:
					case 0x10:
					case 0x11:
					case 0x12:
					case 0x13:
						check_branch_delay = true;
						break;
				}
				break;

			case 2:
			case 3:
			case 4:
			case 5:
			case 6:
			case 7:
			case 0x14:
			case 0x15:
			case 0x16:
			case 0x17:
				check_branch_delay = true;
				break;
		}
		// Check for branch in delay slot, new code by FlatOut.
		// Gregory tested this in 2017 using the ps2autotests suite and remarked "So far we return 1 (even with this PR), and the HW 2.
		// Original PR and discussion at https://github.com/PCSX2/pcsx2/pull/1783 so we don't forget this information.
		if (check_branch_delay)
		{
			_clearNeededX86regs();
			_clearNeededXMMregs();
			pc += 4;
			g_cpuFlushedPC = false;
			g_cpuFlushedCode = false;
			if (g_maySignalException)
				xAND(ptr32[&cpuRegs.CP0.n.Cause], ~(1 << 31)); // BD

			g_recompilingDelaySlot = false;
			return;
		}
	}
	// Check for NOP
	if (cpuRegs.code == 0x00000000)
	{
		// Note: Tests on a ps2 suggested more like 5 cycles for a NOP. But there's many factors in this..
		s_nBlockCycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
	}
	else
	{
		//If the COP0 DIE bit is disabled, cycles should be doubled.
		s_nBlockCycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		opcode.recompile();
	}

	if (!swapped_delay_slot)
	{
		_clearNeededX86regs();
		_clearNeededXMMregs();
	}

	if (delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = false;
		g_cpuFlushedCode = false;
		if (g_maySignalException)
			xAND(ptr32[&cpuRegs.CP0.n.Cause], ~(1 << 31)); // BD
		g_recompilingDelaySlot = false;
	}

	g_maySignalException = false;

	// Stalls normally occur as necessary on the R5900, but when using COP2 (VU0 macro mode),
	// there are some exceptions to this.  We probably don't even know all of them.
	// We emulate the R5900 as if it was fully interlocked (which is mostly true), and
	// in fact we don't have good enough cycle counting to do otherwise.  So for now,
	// we'll try to identify problematic code in games create patches.
	// Look ahead is probably the most reasonable way to do this given the deficiency
	// of our cycle counting.  Real cycle counting is complicated and will have to wait.
	// Instead of counting the cycles I'm going to count instructions.  There are a lot of
	// classes of instructions which use different resources and specific restrictions on
	// coissuing but this is just for printing a warning so I'll simplify.
	// Even when simplified this is not simple and it is very very wrong.

	// CFC2 flag register after arithmetic operation: 5 cycles
	// CTC2 flag register after arithmetic operation... um.  TODO.
	// CFC2 address register after increment/decrement load/store: 5 cycles TODO
	// CTC2 CMSAR0, VCALLMSR CMSAR0: 3 cycles but I want to do some tests.
	// Don't even want to think about DIV, SQRT, RSQRT now.

	if (_Opcode_ == 022) // COP2
	{
		if ((cpuRegs.code >> 25 & 1) == 1 && (cpuRegs.code >> 2 & 0x1ff) == 0xdf) // [LS]Q[DI]
			; // TODO
		else if (_Rs_ == 6) // CTC2
			; // TODO
		else if ((cpuRegs.code & 0x7FC) == 0x3BC) // DIV/RSQRT/SQRT/WAITQ
		{
			int cycles = COP2DivUnitTimings(cpuRegs.code);
			for (u32 p = pc; cycles > 0 && p < s_nEndBlock; p += 4, cycles--)
			{
				cpuRegs.code = vtlb_memRead32(p);

				if ((_Opcode_ == 022) && (cpuRegs.code & 0x7FC) == 0x3BC) // WaitQ or another DIV op hit (stalled), we're safe
					break;

				else if (COP2IsQOP(cpuRegs.code))
					break;
			}
		}
		else
		{
			int s = cop2flags(cpuRegs.code);
			int all_count = 0, cop2o_count = 0, cop2m_count = 0;
			for (u32 p = pc; s != 0 && p < s_nEndBlock && all_count < 10 && cop2m_count < 5 && cop2o_count < 4; p += 4)
			{
				// I am so sorry.
				cpuRegs.code = vtlb_memRead32(p);
				if (_Opcode_ == 022 && _Rs_ == 2) // CFC2
					// rd is fs
					if ((_Rd_ == 16 && s & 1) || (_Rd_ == 17 && s & 2) || (_Rd_ == 18 && s & 4))
						break;
				s &= ~cop2flags(cpuRegs.code);
				all_count++;
				if (_Opcode_ == 022 && _Rs_ == 8) // COP2 branch, handled incorrectly like most things
					;
				else if (_Opcode_ == 022 && (cpuRegs.code >> 25 & 1) == 0)
					cop2m_count++;
				else if (_Opcode_ == 022)
					cop2o_count++;
			}
		}
	}
	cpuRegs.code = *s_pCode;

	if (swapped_delay_slot)
	{
		cpuRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}
}


// called when a page under manual protection has been run enough times to be a candidate
// for being reset under the faster vtlb write protection.  All blocks in the page are cleared
// and the block is re-assigned for write protection.
static void dyna_page_reset(u32 start, u32 sz)
{
	recClear(start & ~0xfffUL, 0x400);
	manual_counter[start >> 12]++;
	mmap_MarkCountedRamPage(start);
}

static void memory_protect_recompiled_code(u32 startpc, u32 size)
{
	alignas(16) static u16 manual_page[Ps2MemSize::MainRam >> 12];

	u32 inpage_ptr = HWADDR(startpc);
	u32 inpage_sz = size * 4;

	// The kernel context register is stored @ 0x800010C0-0x80001300
	// The EENULL thread context register is stored @ 0x81000-....
	bool contains_thread_stack = ((startpc >> 12) == 0x81) || ((startpc >> 12) == 0x80001);

	// note: blocks are guaranteed to reside within the confines of a single page.
	const vtlb_ProtectionMode PageType = contains_thread_stack ? ProtMode_Manual : mmap_GetRamPageInfo(inpage_ptr);

	switch (PageType)
	{
		case ProtMode_NotRequired:
			break;

		case ProtMode_None:
		case ProtMode_Write:
			mmap_MarkCountedRamPage(inpage_ptr);
			manual_page[inpage_ptr >> 12] = 0;
			break;

		case ProtMode_Manual:
			xMOV(arg1regd, inpage_ptr);
			xMOV(arg2regd, inpage_sz / 4);
			//xMOV( eax, startpc );		// uncomment this to access startpc (as eax) in dyna_block_discard

			u32 lpc = inpage_ptr;
			u32 stg = inpage_sz;

			while (stg > 0)
			{
				xCMP(ptr32[PSM(lpc)], *(u32*)PSM(lpc));
				xJNE(DispatchBlockDiscard);

				stg -= 4;
				lpc += 4;
			}

			// Tweakpoint!  3 is a 'magic' number representing the number of times a counted block
			// is re-protected before the recompiler gives up and sets it up as an uncounted (permanent)
			// manual block.  Higher thresholds result in more recompilations for blocks that share code
			// and data on the same page.  Side effects of a lower threshold: over extended gameplay
			// with several map changes, a game's overall performance could degrade.

			// (ideally, perhaps, manual_counter should be reset to 0 every few minutes?)

			if (!contains_thread_stack && manual_counter[inpage_ptr >> 12] <= 3)
			{
				// Counted blocks add a weighted (by block size) value into manual_page each time they're
				// run.  If the block gets run a lot, it resets and re-protects itself in the hope
				// that whatever forced it to be manually-checked before was a 1-time deal.

				// Counted blocks have a secondary threshold check in manual_counter, which forces a block
				// to 'uncounted' mode if it's recompiled several times.  This protects against excessive
				// recompilation of blocks that reside on the same codepage as data.

				// fixme? Currently this algo is kinda dumb and results in the forced recompilation of a
				// lot of blocks before it decides to mark a 'busy' page as uncounted.  There might be
				// be a more clever approach that could streamline this process, by doing a first-pass
				// test using the vtlb memory protection (without recompilation!) to reprotect a counted
				// block.  But unless a new algo is relatively simple in implementation, it's probably
				// not worth the effort (tests show that we have lots of recompiler memory to spare, and
				// that the current amount of recompilation is fairly cheap).

				xADD(ptr16[&manual_page[inpage_ptr >> 12]], size);
				xJC(DispatchPageReset);
			}
			break;
	}
}

// Skip MPEG Game-Fix
static bool skipMPEG_By_Pattern(u32 sPC)
{
	if (!CHECK_SKIPMPEGHACK)
		return 0;

	// sceMpegIsEnd: lw reg, 0x40(a0); jr ra; lw v0, 0(reg)
	if ((s_nEndBlock == sPC + 12) && (vtlb_memRead32(sPC + 4) == 0x03e00008))
	{
		u32 code = vtlb_memRead32(sPC);
		u32 p1 = 0x8c800040;
		u32 p2 = 0x8c020000 | (code & 0x1f0000) << 5;
		if ((code & 0xffe0ffff) != p1)
			return 0;
		if (vtlb_memRead32(sPC + 8) != p2)
			return 0;
		xMOV(ptr32[&cpuRegs.GPR.n.v0.UL[0]], 1);
		xMOV(ptr32[&cpuRegs.GPR.n.v0.UL[1]], 0);
		xMOV(eax, ptr32[&cpuRegs.GPR.n.ra.UL[0]]);
		xMOV(ptr32[&cpuRegs.pc], eax);
		iBranchTest(0xffffffff);
		g_branch = 1;
		pc = s_nEndBlock;
		return 1;
	}
	return 0;
}

static bool recSkipTimeoutLoop(s32 reg, bool is_timeout_loop)
{
	if (!EmuConfig.Speedhacks.WaitLoop || !is_timeout_loop)
		return false;

	// basically, if the time it takes the loop to run is shorter than the
	// time to the next event, then we want to skip ahead to the event, but
	// update v0 to reflect how long the loop would have run for.

	// if (cycle >= nextEventCycle) { jump to dispatcher, we're running late }
	// new_cycles = min(v0 * 8, nextEventCycle)
	// new_v0 = (new_cycles - cycles) / 8
	// if new_v0 > 0 { jump to dispatcher because loop exited early }
	// else new_v0 is 0, so exit loop

	xMOV(ebx, ptr32[&cpuRegs.cycle]); // ebx = cycle
	xMOV(ecx, ptr32[&cpuRegs.nextEventCycle]); // ecx = nextEventCycle
	xCMP(ebx, ecx);
	//xJAE((void*)DispatcherEvent); // jump to dispatcher if event immediately

	// TODO: In the case where nextEventCycle < cycle because it's overflowed, tack 8
	// cycles onto the event count, so hopefully it'll wrap around. This is pretty
	// gross, but until we switch to 64-bit counters, not many better options.
	xForwardJB8 not_dispatcher;
	xADD(ebx, 8);
	xMOV(ptr32[&cpuRegs.cycle], ebx);
	xJMP((const void*)DispatcherEvent);
	not_dispatcher.SetTarget();

	xMOV(edx, ptr32[&cpuRegs.GPR.r[reg].UL[0]]); // eax = v0
	xLEA(rax, ptrNative[rdx * 8 + rbx]); // edx = v0 * 8 + cycle
	xCMP(rcx, rax);
	xCMOVB(rax, rcx); // eax = new_cycles = min(v8 * 8, nextEventCycle)
	xMOV(ptr32[&cpuRegs.cycle], eax); // writeback new_cycles
	xSUB(eax, ebx); // new_cycles -= cycle
	xSHR(eax, 3); // compute new v0 value
	xSUB(edx, eax); // v0 -= cycle_diff
	xMOV(ptr32[&cpuRegs.GPR.r[reg].UL[0]], edx); // write back new value of v0
	xJNZ((void*)DispatcherEvent); // jump to dispatcher if new v0 is not zero (i.e. an event)
	xMOV(ptr32[&cpuRegs.pc], s_nEndBlock); // otherwise end of loop
	recBlocks.Link(HWADDR(s_nEndBlock), xJcc32(Jcc_Unconditional, 0));

	g_branch = 1;
	pc = s_nEndBlock;

	return true;
}

static void recRecompile(const u32 startpc)
{
	u32 i = 0;
	u32 willbranch3 = 0;

	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= (recMem->GetPtrEnd() - _64kb))
		eeRecNeedsReset = true;

	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

	x86Ptr        = (u8*)recPtr;
	recPtr        = x86Ptr;

	s_pCurBlock   = PC_GETBLOCK(startpc);

	s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));

	s_pCurBlockEx = recBlocks.New(HWADDR(startpc), (uintptr_t)recPtr);

	if (HWADDR(startpc) == EELOAD_START)
	{
		// The EELOAD _start function is the same across all BIOS versions
		u32 mainjump = vtlb_memRead32(EELOAD_START + 0x9c);
		if (mainjump >> 26 == 3) // JAL
			g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);
	}

	if (g_eeloadMain && HWADDR(startpc) == HWADDR(g_eeloadMain))
	{
		xFastCall((const void*)eeloadHook);
		if (g_SkipBiosHack)
		{
			// There are four known versions of EELOAD, identifiable by the location of the 'jal' to the EELOAD function which
			// calls ExecPS2(). The function itself is at the same address in all BIOSs after v1.00-v1.10.
			const u32 typeAexecjump = vtlb_memRead32(EELOAD_START + 0x470); // v1.00, v1.01?, v1.10?
			const u32 typeBexecjump = vtlb_memRead32(EELOAD_START + 0x5B0); // v1.20, v1.50, v1.60 (3000x models)
			const u32 typeCexecjump = vtlb_memRead32(EELOAD_START + 0x618); // v1.60 (3900x models)
			const u32 typeDexecjump = vtlb_memRead32(EELOAD_START + 0x600); // v1.70, v1.90, v2.00, v2.20, v2.30
			if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3)) // JAL to 0x822B8
				g_eeloadExec = EELOAD_START + 0x2B8;
			else if (typeAexecjump >> 26 == 3) // JAL to 0x82170
				g_eeloadExec = EELOAD_START + 0x170;
		}
	}

	if (g_eeloadExec && HWADDR(startpc) == HWADDR(g_eeloadExec))
		xFastCall((const void*)eeloadHook2);

	// this is the only way patches get applied, doesn't depend on a hack
	if (g_GameLoading && HWADDR(startpc) == ElfEntry)
	{
		xFastCall((const void*)eeGameStarting);
		VMManager::Internal::EntryPointCompilingOnCPUThread();
	}

	g_branch = 0;

	// reset recomp state variables
	s_nBlockCycles = 0;
	s_nBlockInterlocked = false;
	pc = startpc;
	g_cpuHasConstReg = g_cpuFlushedConstReg = 1;

	_initX86regs();
	_initXMMregs();

	if (EmuConfig.Gamefixes.GoemonTlbHack)
	{
		if (pc == 0x33ad48 || pc == 0x35060c)
		{
			// 0x33ad48 and 0x35060c are the return address of the function (0x356250) that populate the TLB cache
			xFastCall((const void*)GoemonPreloadTlb);
		}
		else if (pc == 0x3563b8)
		{
			// Game will unmap some virtual addresses. If a constant address were hardcoded in the block, we would be in a bad situation.
			eeRecNeedsReset = true;
			// 0x3563b8 is the start address of the function that invalidate entry in TLB cache
			xFastCall((const void*)GoemonUnloadTlb, ptr32[&cpuRegs.GPR.n.a0.UL[0]]);
		}
	}

	// go until the next branch
	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	// Timeout loop speedhack.
	// God of War 2 and other games (e.g. NFS series) have these timeout loops which just spin for a few thousand
	// iterations, usually after kicking something which results in an IRQ, but instead of cancelling the loop,
	// they just let it finish anyway. Such loops look like:
	//
	//   00186D6C addiu  v0,v0, -0x1
	//   00186D70 nop
	//   00186D74 nop
	//   00186D78 nop
	//   00186D7C nop
	//   00186D80 bne    v0, zero, ->$0x00186D6C
	//   00186D84 nop
	//
	// Skipping them entirely seems to have no negative effects, but we skip cycles based on the incoming value
	// if the register being decremented, which appears to vary. So far I haven't seen any which increment instead
	// of decrementing, so we'll limit the test to that to be safe.
	//
	s32 timeout_reg = -1;
	bool is_timeout_loop = true;

	for (;;)
	{
		if (i != startpc) // Block size truncation checks.
		{
			if ((i & 0xffc) == 0x0) // breaks blocks at 4k page boundaries
			{
				willbranch3 = 1;
				s_nEndBlock = i;
				break;
			}
		}

		//HUH ? PSM ? whut ? THIS IS VIRTUAL ACCESS GOD DAMMIT
		cpuRegs.code = *(int*)PSM(i);

		if (is_timeout_loop)
		{
			if ((cpuRegs.code >> 26) == 8 || (cpuRegs.code >> 26) == 9)
			{
				// addi/addiu
				if (timeout_reg >= 0 || _Rs_ != _Rt_ || _Imm_ >= 0)
					is_timeout_loop = false;
				else
					timeout_reg = _Rs_;
			}
			else if ((cpuRegs.code >> 26) == 5)
			{
				// bne
				if (timeout_reg != static_cast<s32>(_Rs_) || _Rt_ != 0 || vtlb_memRead32(i + 4) != 0)
					is_timeout_loop = false;
			}
			else if (cpuRegs.code != 0)
				is_timeout_loop = false;
		}

		switch (cpuRegs.code >> 26)
		{
			case 0: // special
				if (_Funct_ == 8 || _Funct_ == 9) // JR, JALR
				{
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				else if (_Funct_ == 12 || _Funct_ == 13) // SYSCALL, BREAK
				{
					s_nEndBlock = i + 4; // No delay slot.
					goto StartRecomp;
				}
				break;

			case 1: // regimm

				if (_Rt_ < 4 || (_Rt_ >= 16 && _Rt_ < 20))
				{
					// branches
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;

			case 2: // J
			case 3: // JAL
				s_branchTo = (_InstrucTarget_ << 2) | ((i + 4) & 0xf0000000);
				s_nEndBlock = i + 8;
				goto StartRecomp;

			// branches
			case 4:
			case 5:
			case 6:
			case 7:
			case 20:
			case 21:
			case 22:
			case 23:
				s_branchTo = _Imm_ * 4 + i + 4;
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;

				goto StartRecomp;

			case 16: // cp0
				if (_Rs_ == 16)
				{
					if (_Funct_ == 24) // eret
					{
						s_nEndBlock = i + 4;
						goto StartRecomp;
					}
				}
				// Fall through!
				// COP0's branch opcodes line up with COP1 and COP2's

			case 17: // cp1
			case 18: // cp2
				if (_Rs_ == 8)
				{
					// BC1F, BC1T, BC1FL, BC1TL
					// BC2F, BC2T, BC2FL, BC2TL
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;
		}

		i += 4;
	}

StartRecomp:

	// The idea here is that as long as a loop doesn't write to a register it's already read
	// (excepting registers initialised with constants or memory loads) or use any instructions
	// which alter the machine state apart from registers, it will do the same thing on every
	// iteration.
	s_nBlockFF = false;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = true;

		u32 reads = 0, loads = 1;

		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i == s_nEndBlock - 8)
				continue;
			cpuRegs.code = *(u32*)PSM(i);
			// nop
			if (cpuRegs.code == 0)
				continue;
			// cache, sync
			else if (_Opcode_ == 057 || (_Opcode_ == 0 && _Funct_ == 017))
				continue;
			// imm arithmetic
			else if ((_Opcode_ & 070) == 010 || (_Opcode_ & 076) == 030)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			// common register arithmetic instructions
			else if (_Opcode_ == 0 && (_Funct_ & 060) == 040 && (_Funct_ & 076) != 050)
			{
				if (loads & 1 << _Rs_ && loads & 1 << _Rt_)
				{
					loads |= 1 << _Rd_;
					continue;
				}
				else
					reads |= 1 << _Rs_ | 1 << _Rt_;
				if (reads & 1 << _Rd_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			// loads
			else if ((_Opcode_ & 070) == 040 || (_Opcode_ & 076) == 032 || _Opcode_ == 067)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			// mfc*, cfc*
			else if ((_Opcode_ & 074) == 020 && _Rs_ < 4)
				loads |= 1 << _Rt_;
			else
			{
				s_nBlockFF = false;
				break;
			}
		}
	}
	else
		is_timeout_loop = false;

	// rec info //
	bool has_cop2_instructions = false;
	{
		EEINST* pcur;

		if (s_nInstCacheSize < (s_nEndBlock - startpc) / 4 + 1)
		{
			free(s_pInstCache);
			s_nInstCacheSize = (s_nEndBlock - startpc) / 4 + 10;
			s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
		}

		pcur = s_pInstCache + (s_nEndBlock - startpc) / 4;
		_recClearInst(pcur);
		pcur->info = 0;

		for (i = s_nEndBlock; i > startpc; i -= 4)
		{
			cpuRegs.code = *(int*)PSM(i - 4);
			pcur[-1] = pcur[0];
			recBackpropBSC(cpuRegs.code, pcur - 1, pcur);
			pcur--;

			has_cop2_instructions |= (_Opcode_ == 022 || _Opcode_ == 066 || _Opcode_ == 076);
		}
	}

	// eventually we'll want to have a vector of passes or something.
	if (has_cop2_instructions)
	{
		COP2MicroFinishPass().Run(startpc, s_nEndBlock, s_pInstCache + 1);

		if (EmuConfig.Speedhacks.vuFlagHack)
			COP2FlagHackPass().Run(startpc, s_nEndBlock, s_pInstCache + 1);
	}

	// Detect and handle self-modified code
	memory_protect_recompiled_code(startpc, (s_nEndBlock - startpc) >> 2);

	// Skip Recompilation if sceMpegIsEnd Pattern detected
	const bool doRecompilation = !skipMPEG_By_Pattern(startpc) && !recSkipTimeoutLoop(timeout_reg, is_timeout_loop);

	if (doRecompilation)
	{
		// Finally: Generate x86 recompiled code!
		g_pCurInstInfo = s_pInstCache;
		while (!g_branch && pc < s_nEndBlock)
			recompileNextInstruction(false, false); // For the love of recursion, batman!
	}

	s_pCurBlockEx->size = (pc - startpc) >> 2;

	s_pCurBlock->m_pFnptr = ((uintptr_t)recPtr);

	if (!(pc & 0x10000000))
		maxrecmem = std::max((pc & ~0xa0000000), maxrecmem);

	if (g_branch == 2)
	{
		// Branch type 2 - This is how I "think" this works (air):
		// Performs a branch/event test but does not actually "break" the block.
		// This allows exceptions to be raised, and is thus sufficient for
		// certain types of things like SYSCALL, EI, etc.  but it is not sufficient
		// for actual branching instructions.

		iFlushCall(FLUSH_EVERYTHING);
		iBranchTest(0xffffffff);
	}
	else
	{
		if (willbranch3 || !g_branch)
		{

			iFlushCall(FLUSH_EVERYTHING);

			// Split Block concatenation mode.
			// This code is run when blocks are split either to keep block sizes manageable
			// or because we're crossing a 4k page protection boundary in ps2 mem.  The latter
			// case can result in very short blocks which should not issue branch tests for
			// performance reasons.

			const int numinsts = (pc - startpc) / 4;
			if (numinsts > 6)
				SetBranchImm(pc);
			else
			{
				xMOV(ptr32[&cpuRegs.pc], pc);
				xADD(ptr32[&cpuRegs.cycle], scaleblockcycles());
				recBlocks.Link(HWADDR(pc), xJcc32(Jcc_Unconditional, 0));
			}
		}
	}

	s_pCurBlockEx->x86size = static_cast<u32>(x86Ptr - recPtr);

	recPtr        = x86Ptr;

	s_pCurBlock   = NULL;
	s_pCurBlockEx = NULL;
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,

	recResetEE,
	recExecute,

	recSafeExitExecution,
	recCancelInstruction,
	recClear
};

// use special x86 register allocation for ia32

void _initX86regs(void)
{
	memset(x86regs, 0, sizeof(x86regs));
	g_x86AllocCounter = 0;
	g_x86checknext = 0;
}

static int _getFreeX86reg(int mode)
{
	int tempi = -1;
	u32 bestcount = 0x10000;

	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		const int reg = (g_x86checknext + i) % iREGCNT_GPR;
		if (x86regs[reg].inuse || !_isAllocatableX86reg(reg))
			continue;

		if ((mode & MODE_CALLEESAVED) && Register_IsCallerSaved(reg))
			continue;

		if ((mode & MODE_COP2) && mVUIsReservedCOP2(reg))
			continue;

		if (x86regs[reg].inuse == 0)
		{
			g_x86checknext = (reg + 1) % iREGCNT_GPR;
			return reg;
		}
	}

	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (!_isAllocatableX86reg(i))
			continue;

		if ((mode & MODE_CALLEESAVED) && Register_IsCallerSaved(i))
			continue;

		if ((mode & MODE_COP2) && mVUIsReservedCOP2(i))
			continue;

		if (x86regs[i].needed)
			continue;

		if (x86regs[i].type != X86TYPE_TEMP)
		{

			if (x86regs[i].counter < bestcount)
			{
				tempi = static_cast<int>(i);
				bestcount = x86regs[i].counter;
			}
			continue;
		}

		_freeX86reg(i);
		return i;
	}

	if (tempi != -1)
	{
		_freeX86reg(tempi);
		return tempi;
	}

	return -1;
}

void _flushConstReg(int reg)
{
	if (GPR_IS_CONST1(reg) && !(g_cpuFlushedConstReg & (1 << reg)))
	{
		xImm64Op(xMOV, ptr64[&cpuRegs.GPR.r[reg].UD[0]], rax, g_cpuConstRegs[reg].SD[0]);
		g_cpuFlushedConstReg |= (1 << reg);
	}
}

void _flushConstRegs(void)
{
	int zero_reg_count = 0;
	int minusone_reg_count = 0;
	for (u32 i = 0; i < 32; i++)
	{
		if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
			continue;

		if (g_cpuConstRegs[i].SD[0] == 0)
			zero_reg_count++;
		else if (g_cpuConstRegs[i].SD[0] == -1)
			minusone_reg_count++;
	}

	// if we have more than one of zero/minus-one, precompute
	bool rax_is_zero = false;
	if (zero_reg_count > 1)
	{
		xXOR(eax, eax);
		for (u32 i = 0; i < 32; i++)
		{
			if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
				continue;

			if (g_cpuConstRegs[i].SD[0] == 0)
			{
				xMOV(ptr64[&cpuRegs.GPR.r[i].UD[0]], rax);
				g_cpuFlushedConstReg |= 1u << i;
			}
		}
		rax_is_zero = true;
	}
	if (minusone_reg_count > 1)
	{
		if (!rax_is_zero)
			xMOV(rax, -1);
		else
			xNOT(rax);

		for (u32 i = 0; i < 32; i++)
		{
			if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
				continue;

			if (g_cpuConstRegs[i].SD[0] == -1)
			{
				xMOV(ptr64[&cpuRegs.GPR.r[i].UD[0]], rax);
				g_cpuFlushedConstReg |= 1u << i;
			}
		}
	}

	// and whatever's left over..
	for (u32 i = 0; i < 32; i++)
	{
		if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
			continue;

		xImm64Op(xMOV, ptr64[&cpuRegs.GPR.r[i].UD[0]], rax, g_cpuConstRegs[i].UD[0]);
		g_cpuFlushedConstReg |= 1u << i;
	}
}

int _allocX86reg(int type, int reg, int mode)
{
	int hostXMMreg = (type == X86TYPE_GPR) ? _checkXMMreg(XMMTYPE_GPRREG, reg, 0) : -1;
	if (type != X86TYPE_TEMP)
	{
		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!x86regs[i].inuse || x86regs[i].type != type || x86regs[i].reg != reg)
				continue;

			if (type == X86TYPE_GPR)
			{
				if (mode & MODE_WRITE)
				{
					if (GPR_IS_CONST1(reg))
						g_cpuHasConstReg &= ~(1 << (reg));

					if (hostXMMreg >= 0)
					{
						// ensure upper bits get written
						_freeXMMreg(hostXMMreg);
					}
				}
			}
			else if (type == X86TYPE_PSX)
			{
				if (mode & MODE_WRITE)
				{
					if (PSX_IS_CONST1(reg))
					{
						PSX_DEL_CONST(reg);
					}
				}
			}
			else if (type == X86TYPE_VIREG)
			{
				// keep VI temporaries separate
				if (reg < 0)
					continue;
			}

			x86regs[i].counter = g_x86AllocCounter++;
			x86regs[i].mode |= mode & ~MODE_CALLEESAVED;
			x86regs[i].needed = true;
			return i;
		}
	}

	const int regnum = _getFreeX86reg(mode);
	xRegister64 new_reg(regnum);
	x86regs[regnum].type = type;
	x86regs[regnum].reg = reg;
	x86regs[regnum].mode = mode & ~MODE_CALLEESAVED;
	x86regs[regnum].counter = g_x86AllocCounter++;
	x86regs[regnum].needed = true;
	x86regs[regnum].inuse = true;

	if (mode & MODE_READ)
	{
		switch (type)
		{
			case X86TYPE_GPR:
			{
				if (reg == 0)
				{
					xXOR(xRegister32(new_reg), xRegister32(new_reg)); // 32-bit is smaller and zexts anyway
				}
				else
				{
					if (hostXMMreg >= 0)
					{
						// is in a XMM. we don't need to free the XMM since we're not writing, and it's still valid
						xMOVD(new_reg, xRegisterSSE(hostXMMreg)); // actually MOVQ

						// if the XMM was dirty, just get rid of it, we don't want to try to sync the values up...
						if (xmmregs[hostXMMreg].mode & MODE_WRITE)
						{
							_freeXMMreg(hostXMMreg);
						}
					}
					else if (GPR_IS_CONST1(reg))
					{
						xMOV64(new_reg, g_cpuConstRegs[reg].SD[0]);
						g_cpuFlushedConstReg |= (1u << reg);
						x86regs[regnum].mode |= MODE_WRITE; // reg is dirty
					}
					else
					{
						// not loaded
						xMOV(new_reg, ptr64[&cpuRegs.GPR.r[reg].UD[0]]);
					}
				}
			}
			break;

			case X86TYPE_FPRC:
				xMOV(xRegister32(regnum), ptr32[&fpuRegs.fprc[reg]]);
				break;

			case X86TYPE_PSX:
			{
				const xRegister32 new_reg32(regnum);
				if (reg == 0)
				{
					xXOR(new_reg32, new_reg32);
				}
				else
				{
					if (PSX_IS_CONST1(reg))
					{
						xMOV(new_reg32, g_psxConstRegs[reg]);
						g_psxFlushedConstReg |= (1u << reg);
						x86regs[regnum].mode |= MODE_WRITE; // reg is dirty
					}
					else
					{
						xMOV(new_reg32, ptr32[&psxRegs.GPR.r[reg]]);
					}
				}
			}
			break;

			case X86TYPE_VIREG:
			{
				xMOVZX(xRegister32(regnum), ptr16[&vuRegs[0].VI[reg].US[0]]);
			}
			break;

			default:
				abort();
				break;
		}
	}

	if (type == X86TYPE_GPR && (mode & MODE_WRITE))
	{
		if (GPR_IS_CONST1(reg))
			g_cpuHasConstReg &= ~(1 << (reg));
		if (hostXMMreg >= 0)
		{
			// writing, so kill the xmm allocation. gotta ensure the upper bits gets stored first.
			_freeXMMreg(hostXMMreg);
		}
	}
	else if (type == X86TYPE_PSX && (mode & MODE_WRITE))
	{
		if (PSX_IS_CONST1(reg))
			g_psxHasConstReg &= ~(1 << (reg));
	}

	return regnum;
}

void _writebackX86Reg(int x86reg)
{
	switch (x86regs[x86reg].type)
	{
		case X86TYPE_GPR:
			xMOV(ptr64[&cpuRegs.GPR.r[x86regs[x86reg].reg].UD[0]], xRegister64(x86reg));
			break;

		case X86TYPE_FPRC:
			xMOV(ptr32[&fpuRegs.fprc[x86regs[x86reg].reg]], xRegister32(x86reg));
			break;

		case X86TYPE_VIREG:
			xMOV(ptr16[&vuRegs[0].VI[x86regs[x86reg].reg].UL], xRegister16(x86reg));
			break;

		case X86TYPE_PCWRITEBACK:
			xMOV(ptr32[&cpuRegs.pcWriteback], xRegister32(x86reg));
			break;

		case X86TYPE_PSX:
			xMOV(ptr32[&psxRegs.GPR.r[x86regs[x86reg].reg]], xRegister32(x86reg));
			break;

		case X86TYPE_PSX_PCWRITEBACK:
			xMOV(ptr32[&psxRegs.pcWriteback], xRegister32(x86reg));
			break;

		default:
			abort();
			break;
	}
}

int _checkX86reg(int type, int reg, int mode)
{
	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse && x86regs[i].reg == reg && x86regs[i].type == type)
		{
			// ensure constants get deleted once we alloc as write
			if (mode & MODE_WRITE)
			{
				// go through the alloc path instead, because we might need to invalidate an xmm.
				if (type == X86TYPE_GPR)
					return _allocX86reg(X86TYPE_GPR, reg, mode);
				else if (type == X86TYPE_PSX)
				{
					PSX_DEL_CONST(reg);
				}
			}

			x86regs[i].mode |= mode;
			x86regs[i].counter = g_x86AllocCounter++;
			x86regs[i].needed = 1;
			return i;
		}
	}

	return -1;
}

void _addNeededX86reg(int type, int reg)
{
	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse || x86regs[i].reg != reg || x86regs[i].type != type)
			continue;

		x86regs[i].counter = g_x86AllocCounter++;
		x86regs[i].needed = 1;
	}
}

void _clearNeededX86regs(void)
{
	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].needed)
		{
			if (x86regs[i].inuse && (x86regs[i].mode & MODE_WRITE))
				x86regs[i].mode |= MODE_READ;
		}
		x86regs[i].needed = 0;
	}
}

void _freeX86reg(const x86Emitter::xRegister32& x86reg)
{
	_freeX86reg(x86reg.Id);
}

void _freeX86reg(int x86reg)
{
	if (x86regs[x86reg].inuse && (x86regs[x86reg].mode & MODE_WRITE))
	{
		_writebackX86Reg(x86reg);
		x86regs[x86reg].mode &= ~MODE_WRITE;
	}

	_freeX86regWithoutWriteback(x86reg);
}

void _freeX86regWithoutWriteback(int x86reg)
{
	x86regs[x86reg].inuse = 0;

	if (x86regs[x86reg].type == X86TYPE_VIREG)
		mVUFreeCOP2GPR(x86reg);
}

void _flushX86regs(void)
{
	for (u32 i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].inuse && x86regs[i].mode & MODE_WRITE)
		{
			_writebackX86Reg(i);
			x86regs[i].mode = (x86regs[i].mode & ~MODE_WRITE) | MODE_READ;
		}
	}
}
