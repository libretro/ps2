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

// recompiler reworked to add dynamic linking Jan06
// and added reg caching, const propagation, block analysis Jun06
// zerofrog(@gmail.com)

#include "iR3000A.h"
#include "R3000A.h"
#include "BaseblockEx.h"
#include "R5900OpcodeTables.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopDma.h"
#include "IopGte.h"
#include "Common.h"
#include "VirtualMemory.h"
#include "VMManager.h"

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "iCore.h"

#include "Config.h"

#include "common/AlignedMalloc.h"
#include "common/FileSystem.h"
#include "common/Path.h"

/* Cycle penalties for particularly slow instructions. */
static const int psxInstCycles_Mult = 7;
static const int psxInstCycles_Div = 40;

/* Currently unused (iop mod incomplete) */
static const int psxInstCycles_Peephole_Store = 0;
static const int psxInstCycles_Store = 0;
static const int psxInstCycles_Load = 0;

using namespace x86Emitter;

extern uint32_t g_psxMaxRecMem;
extern void psxBREAK();

uint32_t g_psxMaxRecMem = 0;

static uintptr_t psxRecLUT[0x10000];
static uint32_t psxhwLUT[0x10000];

static RecompiledCodeReserve* recMem = NULL;

static BASEBLOCK* recRAM = NULL; // and the ptr to the blocks here
static BASEBLOCK* recROM = NULL; // and here
static BASEBLOCK* recROM1 = NULL; // also here
static BASEBLOCK* recROM2 = NULL; // also here
static BaseBlocks recBlocks;
static uint8_t* recPtr = NULL;
static uint32_t psxpc; /* recompiler psxpc */
static int psxbranch; /* set for branch */
uint32_t g_iopCyclePenalty;

static EEINST* s_pInstCache = NULL;
static uint32_t s_nInstCacheSize = 0;

static BASEBLOCK* s_pCurBlock = NULL;
static BASEBLOCKEX* s_pCurBlockEx = NULL;

static uint32_t s_nEndBlock = 0; // what psxpc the current block ends
static uint32_t s_branchTo;
static bool s_nBlockFF;

static uint32_t s_saveConstRegs[32];
static uint32_t s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = NULL;

uint32_t s_psxBlockCycles = 0; // cycles of current block recompiling
static uint32_t s_savenBlockCycles = 0;
static bool s_recompilingDelaySlot = false;

extern void (*rpsxBSC[64])();

static void iopRecRecompile(const uint32_t startpc);

// Recompiled code buffer for EE recompiler dispatchers!
alignas(__pagesize) static uint8_t iopRecDispatchers[__pagesize];

static const void* iopDispatcherEvent = NULL;
static const void* iopDispatcherReg = NULL;
static const void* iopJITCompile = NULL;
static const void* iopEnterRecompiledCode = NULL;
static const void* iopExitRecompiledCode = NULL;

static uint8_t* m_recBlockAlloc = NULL;

static const uint m_recBlockAllocSize =
	(((Ps2MemSize::IopRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) / 4) * sizeof(BASEBLOCK));

/* forward declarations */
static void iPsxBranchTest(uint32_t newpc, uint32_t cpuBranch);
static void iopClearRecLUT(BASEBLOCK* base, int count);
static void psxRecompileNextInstruction(bool delayslot, bool swapped_delayslot);
void rpsxpropBSC(EEINST* prev, EEINST* pinst);
extern void psxLWL(void);
extern void psxLWR(void);
extern void psxSWL(void);
extern void psxSWR(void);

static __fi uint32_t HWADDR(uint32_t mem) { return psxhwLUT[mem >> 16] + mem; }

void _psxFlushConstReg(int reg)
{
	if (PSX_IS_CONST1(reg) && !(g_psxFlushedConstReg & (1 << reg)))
	{
		xMOV(ptr32[&psxRegs.GPR.r[reg]], g_psxConstRegs[reg]);
		g_psxFlushedConstReg |= (1 << reg);
	}
}

void _psxFlushConstRegs(void)
{
	// TODO: Combine flushes

	int i;

	// flush constants

	// ignore r0
	for (i = 1; i < 32; ++i)
	{
		if (g_psxHasConstReg & (1 << i))
		{

			if (!(g_psxFlushedConstReg & (1 << i)))
			{
				xMOV(ptr32[&psxRegs.GPR.r[i]], g_psxConstRegs[i]);
				g_psxFlushedConstReg |= 1 << i;
			}

			if (g_psxHasConstReg == g_psxFlushedConstReg)
				break;
		}
	}
}

static void _psxFlushCall(int flushtype)
{
	// Free registers that are not saved across function calls (x86-32 ABI):
	for (uint32_t i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		if (         Register_IsCallerSaved(i)
			|| ((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP)
			|| ((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_psxFlushConstRegs();

	if ((flushtype & FLUSH_PC) /*&& !g_cpuFlushedPC*/)
	{
		xMOV(ptr32[&psxRegs.pc], psxpc);
		//g_cpuFlushedPC = true;
	}
}

void _psxFlushAllDirty()
{
	// TODO: Combine flushes
	for (uint32_t i = 0; i < 32; ++i)
	{
		if (PSX_IS_CONST1(i))
			_psxFlushConstReg(i);
	}

	_flushX86regs();
}


static void psxSaveBranchState(void)
{
	s_savenBlockCycles = s_psxBlockCycles;
	memcpy(s_saveConstRegs, g_psxConstRegs, sizeof(g_psxConstRegs));
	s_saveHasConstReg = g_psxHasConstReg;
	s_saveFlushedConstReg = g_psxFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	// save all regs
	memcpy(s_saveX86regs, x86regs, sizeof(x86regs));
}

static void psxLoadBranchState(void)
{
	s_psxBlockCycles = s_savenBlockCycles;

	memcpy(g_psxConstRegs, s_saveConstRegs, sizeof(g_psxConstRegs));
	g_psxHasConstReg = s_saveHasConstReg;
	g_psxFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	// restore all regs
	memcpy(x86regs, s_saveX86regs, sizeof(x86regs));
}


static int rpsxAllocRegIfUsed(int reg, int mode)
{
	if (EEINST_USEDTEST(reg))
		return _allocX86reg(X86TYPE_PSX, reg, mode);
	return _checkX86reg(X86TYPE_PSX, reg, mode);
}

static void rpsxMoveStoT(int info)
{
	if (EEREC_T == EEREC_S)
		return;

	if (info & PROCESS_EE_S)
		xMOV(xRegister32(EEREC_T), xRegister32(EEREC_S));
	else
		xMOV(xRegister32(EEREC_T), ptr32[&psxRegs.GPR.r[_Rs_]]);
}

static void rpsxMoveStoD(int info)
{
	if (EEREC_D == EEREC_S)
		return;

	if (info & PROCESS_EE_S)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
}

static void rpsxMoveTtoD(int info)
{
	if (EEREC_D == EEREC_T)
		return;

	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
}

static void rpsxMoveSToECX(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(ecx, xRegister32(EEREC_S));
	else
		xMOV(ecx, ptr32[&psxRegs.GPR.r[_Rs_]]);
}

static int psxTryRenameReg(int to, int from, int fromx86, int other, int xmminfo)
{
	// can't rename when in form Rd = Rs op Rt and Rd == Rs or Rd == Rt
	if ((xmminfo & XMMINFO_NORENAME) || fromx86 < 0 || to == from || to == other || !EEINST_RENAMETEST(from))
		return -1;

	// flush back when it's been modified
	if (x86regs[fromx86].mode & MODE_WRITE && EEINST_LIVETEST(from))
		_writebackX86Reg(fromx86);

	// remove all references to renamed-to register
	_deletePSXtoX86reg(to, DELETE_REG_FREE_NO_WRITEBACK);
	PSX_DEL_CONST(to);

	// and do the actual rename, new register has been modified.
	x86regs[fromx86].reg = to;
	x86regs[fromx86].mode |= MODE_READ | MODE_WRITE;
	return fromx86;
}


static void rpsxCopyReg(int dest, int src)
{
	// try a simple rename first...
	const int roldsrc = _checkX86reg(X86TYPE_PSX, src, MODE_READ);
	if (roldsrc >= 0 && psxTryRenameReg(dest, src, roldsrc, 0, 0) >= 0)
		return;

	const int rdest = rpsxAllocRegIfUsed(dest, MODE_WRITE);
	if (PSX_IS_CONST1(src))
	{
		if (dest < 32)
		{
			g_psxConstRegs[dest] = g_psxConstRegs[src];
			PSX_SET_CONST(dest);
		}
		else
		{
			if (rdest >= 0)
				xMOV(xRegister32(rdest), g_psxConstRegs[src]);
			else
				xMOV(ptr32[&psxRegs.GPR.r[dest]], g_psxConstRegs[src]);
		}

		return;
	}

	if (dest < 32)
		PSX_DEL_CONST(dest);

	const int rsrc = rpsxAllocRegIfUsed(src, MODE_READ);
	if (rsrc >= 0 && rdest >= 0)
	{
		xMOV(xRegister32(rdest), xRegister32(rsrc));
	}
	else if (rdest >= 0)
	{
		xMOV(xRegister32(rdest), ptr32[&psxRegs.GPR.r[src]]);
	}
	else if (rsrc >= 0)
	{
		xMOV(ptr32[&psxRegs.GPR.r[dest]], xRegister32(rsrc));
	}
	else
	{
		xMOV(eax, ptr32[&psxRegs.GPR.r[src]]);
		xMOV(ptr32[&psxRegs.GPR.r[dest]], eax);
	}
}

////
static void rpsxADDIU_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] + _Imm_;
}

static void rpsxADDIU_(int info)
{
	// Rt = Rs + Im
	rpsxMoveStoT(info);
	if (_Imm_ != 0)
		xADD(xRegister32(EEREC_T), _Imm_);
}

PSXRECOMPILE_CONSTCODE1(ADDIU, XMMINFO_WRITET | XMMINFO_READS);

void rpsxADDI() { rpsxADDIU(); }

//// SLTI
static void rpsxSLTI_const()
{
	g_psxConstRegs[_Rt_] = *(int*)&g_psxConstRegs[_Rs_] < _Imm_;
}

static void rpsxSLTI_(int info)
{
	const xRegister32 dreg((_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T);
	xXOR(dreg, dreg);

	if (info & PROCESS_EE_S)
		xCMP(xRegister32(EEREC_S), _Imm_);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], _Imm_);

	xSETL(xRegister8(dreg));

	if (dreg.Id != EEREC_T)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_T]);
		_freeX86reg(EEREC_T);
	}
}

PSXRECOMPILE_CONSTCODE1(SLTI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

//// SLTIU
static void rpsxSLTIU_const(void)
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] < (uint32_t)_Imm_;
}

static void rpsxSLTIU_(int info)
{
	const xRegister32 dreg((_Rt_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_T);
	xXOR(dreg, dreg);

	if (info & PROCESS_EE_S)
		xCMP(xRegister32(EEREC_S), _Imm_);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], _Imm_);

	xSETB(xRegister8(dreg));

	if (dreg.Id != EEREC_T)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_T]);
		_freeX86reg(EEREC_T);
	}
}

PSXRECOMPILE_CONSTCODE1(SLTIU, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

static void rpsxLogicalOpI(uint64_t info, int op)
{
	if (_ImmU_ != 0)
	{
		rpsxMoveStoT(info);
		switch (op)
		{
			case 0:
				xAND(xRegister32(EEREC_T), _ImmU_);
				break;
			case 1:
				xOR(xRegister32(EEREC_T), _ImmU_);
				break;
			case 2:
				xXOR(xRegister32(EEREC_T), _ImmU_);
				break;
			default:
				break;
		}
	}
	else
	{
		if (op == 0)
		{
			xXOR(xRegister32(EEREC_T), xRegister32(EEREC_T));
		}
		else if (EEREC_T != EEREC_S)
		{
			rpsxMoveStoT(info);
		}
	}
}

//// ANDI
static void rpsxANDI_const(void)
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] & _ImmU_;
}

static void rpsxANDI_(int info)
{
	rpsxLogicalOpI(info, 0);
}

PSXRECOMPILE_CONSTCODE1(ANDI, XMMINFO_WRITET | XMMINFO_READS);

//// ORI
static void rpsxORI_const(void)
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] | _ImmU_;
}

static void rpsxORI_(int info)
{
	rpsxLogicalOpI(info, 1);
}

PSXRECOMPILE_CONSTCODE1(ORI, XMMINFO_WRITET | XMMINFO_READS);

static void rpsxXORI_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] ^ _ImmU_;
}

static void rpsxXORI_(int info)
{
	rpsxLogicalOpI(info, 2);
}

PSXRECOMPILE_CONSTCODE1(XORI, XMMINFO_WRITET | XMMINFO_READS);

static void _psxDeleteReg(int reg, int flush)
{
	if (!reg)
		return;
	if (flush && PSX_IS_CONST1(reg))
		_psxFlushConstReg(reg);

	PSX_DEL_CONST(reg);
	_deletePSXtoX86reg(reg, flush ? DELETE_REG_FREE : DELETE_REG_FREE_NO_WRITEBACK);
}

static void _psxOnWriteReg(int reg)
{
	PSX_DEL_CONST(reg);
}

void rpsxLUI()
{
	if (!_Rt_)
		return;
	_psxOnWriteReg(_Rt_);
	_psxDeleteReg(_Rt_, 0);
	PSX_SET_CONST(_Rt_);
	g_psxConstRegs[_Rt_] = psxRegs.code << 16;
}

static void rpsxADDU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] + g_psxConstRegs[_Rt_];
}

static void rpsxADDU_consts(int info)
{
	const s32 cval = static_cast<s32>(g_psxConstRegs[_Rs_]);
	rpsxMoveTtoD(info);
	if (cval != 0)
		xADD(xRegister32(EEREC_D), cval);
}

static void rpsxADDU_constt(int info)
{
	const s32 cval = static_cast<s32>(g_psxConstRegs[_Rt_]);
	rpsxMoveStoD(info);
	if (cval != 0)
		xADD(xRegister32(EEREC_D), cval);
}

void rpsxADDU_(int info)
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
		xADD(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
	}
	else if (info & PROCESS_EE_T)
	{
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
		xADD(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
	}
	else
	{
		xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
		xADD(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
	}
}

PSXRECOMPILE_CONSTCODE0(ADDU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void rpsxADD() { rpsxADDU(); }

static void rpsxSUBU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] - g_psxConstRegs[_Rt_];
}

static void rpsxSUBU_consts(int info)
{
	// more complex because Rt can be Rd, and we're reversing the op
	const s32 sval = g_psxConstRegs[_Rs_];
	const xRegister32 dreg((_Rt_ == _Rd_) ? eax.Id : EEREC_D);
	xMOV(dreg, sval);

	if (info & PROCESS_EE_T)
		xSUB(dreg, xRegister32(EEREC_T));
	else
		xSUB(dreg, ptr32[&psxRegs.GPR.r[_Rt_]]);

	xMOV(xRegister32(EEREC_D), dreg);
}

static void rpsxSUBU_constt(int info)
{
	const s32 tval = g_psxConstRegs[_Rt_];
	rpsxMoveStoD(info);
	if (tval != 0)
		xSUB(xRegister32(EEREC_D), tval);
}

static void rpsxSUBU_(int info)
{
	// Rd = Rs - Rt
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
		}
		else if (EEREC_D == EEREC_T)
		{
			// D might equal T
			const xRegister32 dreg((_Rt_ == _Rd_) ? eax.Id : EEREC_D);
			xMOV(dreg, xRegister32(EEREC_S));
			xSUB(dreg, xRegister32(EEREC_T));
			xMOV(xRegister32(EEREC_D), dreg);
		}
		else
		{
			xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
			xSUB(xRegister32(EEREC_D), xRegister32(EEREC_T));
		}
	}
	else if (info & PROCESS_EE_S)
	{
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
		xSUB(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
		const xRegister32 dreg((_Rt_ == _Rd_) ? eax.Id : EEREC_D);
		xMOV(dreg, ptr32[&psxRegs.GPR.r[_Rs_]]);
		xSUB(dreg, xRegister32(EEREC_T));
		xMOV(xRegister32(EEREC_D), dreg);
	}
	else
	{
		xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
		xSUB(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
	}
}

PSXRECOMPILE_CONSTCODE0(SUBU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void rpsxSUB() { rpsxSUBU(); }

namespace
{
	enum class LogicalOp
	{
		AND,
		OR,
		XOR,
		NOR
	};
} // namespace

static void rpsxLogicalOp_constv(LogicalOp op, int info, int creg, uint32_t vreg, int regv)
{
	xImpl_G1Logic bad{};
	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
														 op == LogicalOp::XOR    ? xXOR :
														 op == LogicalOp::NOR    ? xOR :
                                                                                   bad;
	s32 fixedInput, fixedOutput, identityInput;
	bool hasFixed = true;
	switch (op)
	{
		case LogicalOp::AND:
			fixedInput = 0;
			fixedOutput = 0;
			identityInput = -1;
			break;
		case LogicalOp::OR:
			fixedInput = -1;
			fixedOutput = -1;
			identityInput = 0;
			break;
		case LogicalOp::XOR:
			hasFixed = false;
			identityInput = 0;
			break;
		case LogicalOp::NOR:
			fixedInput = -1;
			fixedOutput = 0;
			identityInput = 0;
			break;
		default:
			break;
	}

	const s32 cval = static_cast<s32>(g_psxConstRegs[creg]);

	if (hasFixed && cval == fixedInput)
	{
		xMOV(xRegister32(EEREC_D), fixedOutput);
	}
	else
	{
		if (regv >= 0)
			xMOV(xRegister32(EEREC_D), xRegister32(regv));
		else
			xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[vreg]]);
		if (cval != identityInput)
			xOP(xRegister32(EEREC_D), cval);
		if (op == LogicalOp::NOR)
			xNOT(xRegister32(EEREC_D));
	}
}

static void rpsxLogicalOp(LogicalOp op, int info)
{
	xImpl_G1Logic bad{};
	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
														 op == LogicalOp::XOR    ? xXOR :
														 op == LogicalOp::NOR    ? xOR :
                                                                                   bad;
	// swap because it's commutative and Rd might be Rt
	uint32_t rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

	if (op == LogicalOp::XOR && rs == rt)
	{
		xXOR(xRegister32(EEREC_D), xRegister32(EEREC_D));
	}
	else
	{
		if (regs >= 0)
			xMOV(xRegister32(EEREC_D), xRegister32(regs));
		else
			xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[rs]]);

		if (regt >= 0)
			xOP(xRegister32(EEREC_D), xRegister32(regt));
		else
			xOP(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[rt]]);

		if (op == LogicalOp::NOR)
			xNOT(xRegister32(EEREC_D));
	}
}

static void rpsxAND_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] & g_psxConstRegs[_Rt_];
}

static void rpsxAND_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::AND, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxAND_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::AND, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxAND_(int info)
{
	rpsxLogicalOp(LogicalOp::AND, info);
}

PSXRECOMPILE_CONSTCODE0(AND, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

static void rpsxOR_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] | g_psxConstRegs[_Rt_];
}

static void rpsxOR_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::OR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxOR_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::OR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxOR_(int info)
{
	rpsxLogicalOp(LogicalOp::OR, info);
}

PSXRECOMPILE_CONSTCODE0(OR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// XOR
static void rpsxXOR_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] ^ g_psxConstRegs[_Rt_];
}

static void rpsxXOR_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::XOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxXOR_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::XOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxXOR_(int info)
{
	rpsxLogicalOp(LogicalOp::XOR, info);
}

PSXRECOMPILE_CONSTCODE0(XOR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// NOR
static void rpsxNOR_const()
{
	g_psxConstRegs[_Rd_] = ~(g_psxConstRegs[_Rs_] | g_psxConstRegs[_Rt_]);
}

static void rpsxNOR_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::NOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxNOR_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::NOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxNOR_(int info)
{
	rpsxLogicalOp(LogicalOp::NOR, info);
}

PSXRECOMPILE_CONSTCODE0(NOR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// SLT
static void rpsxSLT_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rs_] < *(int*)&g_psxConstRegs[_Rt_];
}

static void rpsxSLTs_const(int info, int sign, int st)
{
	const s32 cval = g_psxConstRegs[st ? _Rt_ : _Rs_];

	const xImpl_Set& SET = st ? (sign ? xSETL : xSETB) : (sign ? xSETG : xSETA);

	const xRegister32 dreg((_Rd_ == (st ? _Rs_ : _Rt_)) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D);
	const int regs = st ? ((info & PROCESS_EE_S) ? EEREC_S : -1) : ((info & PROCESS_EE_T) ? EEREC_T : -1);
	xXOR(dreg, dreg);

	if (regs >= 0)
		xCMP(xRegister32(regs), cval);
	else
		xCMP(ptr32[&psxRegs.GPR.r[st ? _Rs_ : _Rt_]], cval);
	SET(xRegister8(dreg));

	if (dreg.Id != EEREC_D)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_D]);
		_freeX86reg(EEREC_D);
	}
}

static void rpsxSLTs_(int info, int sign)
{
	const xImpl_Set& SET = sign ? xSETL : xSETB;

	// need to keep Rs/Rt around.
	const xRegister32 dreg((_Rd_ == _Rt_ || _Rd_ == _Rs_) ? _allocX86reg(X86TYPE_TEMP, 0, 0) : EEREC_D);

	// force Rs into a register, may as well cache it since we're loading anyway.
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);

	xXOR(dreg, dreg);
	if (info & PROCESS_EE_T)
		xCMP(xRegister32(regs), xRegister32(EEREC_T));
	else
		xCMP(xRegister32(regs), ptr32[&psxRegs.GPR.r[_Rt_]]);

	SET(xRegister8(dreg));

	if (dreg.Id != EEREC_D)
	{
		std::swap(x86regs[dreg.Id], x86regs[EEREC_D]);
		_freeX86reg(EEREC_D);
	}
}

static void rpsxSLT_consts(int info)
{
	rpsxSLTs_const(info, 1, 0);
}

static void rpsxSLT_constt(int info)
{
	rpsxSLTs_const(info, 1, 1);
}

static void rpsxSLT_(int info)
{
	rpsxSLTs_(info, 1);
}

PSXRECOMPILE_CONSTCODE0(SLT, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// SLTU
static void rpsxSLTU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] < g_psxConstRegs[_Rt_];
}

static void rpsxSLTU_consts(int info)
{
	rpsxSLTs_const(info, 0, 0);
}

static void rpsxSLTU_constt(int info)
{
	rpsxSLTs_const(info, 0, 1);
}

static void rpsxSLTU_(int info)
{
	rpsxSLTs_(info, 0);
}

PSXRECOMPILE_CONSTCODE0(SLTU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// MULT
static void rpsxMULT_const(void)
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	uint64_t res = (int64_t)((int64_t) * (int*)&g_psxConstRegs[_Rs_] * (int64_t) * (int*)&g_psxConstRegs[_Rt_]);

	xMOV(ptr32[&psxRegs.GPR.n.hi], (uint32_t)((res >> 32) & 0xffffffff));
	xMOV(ptr32[&psxRegs.GPR.n.lo], (uint32_t)(res & 0xffffffff));
}

static void rpsxWritebackHILO(int info)
{
	if (EEINST_LIVETEST(PSX_LO))
	{
		if (info & PROCESS_EE_LO)
			xMOV(xRegister32(EEREC_LO), eax);
		else
			xMOV(ptr32[&psxRegs.GPR.n.lo], eax);
	}

	if (EEINST_LIVETEST(PSX_HI))
	{
		if (info & PROCESS_EE_HI)
			xMOV(xRegister32(EEREC_HI), edx);
		else
			xMOV(ptr32[&psxRegs.GPR.n.hi], edx);
	}
}

static void rpsxMULTsuperconst(int info, int sreg, int imm, int sign)
{
	// Lo/Hi = Rs * Rt (signed)
	xMOV(eax, imm);

	const int regs = rpsxAllocRegIfUsed(sreg, MODE_READ);
	if (sign)
	{
		if (regs >= 0)
			xMUL(xRegister32(regs));
		else
			xMUL(ptr32[&psxRegs.GPR.r[sreg]]);
	}
	else
	{
		if (regs >= 0)
			xUMUL(xRegister32(regs));
		else
			xUMUL(ptr32[&psxRegs.GPR.r[sreg]]);
	}

	rpsxWritebackHILO(info);
}

static void _psxMoveGPRtoR(const xRegister32& to, int fromgpr)
{
	if (PSX_IS_CONST1(fromgpr))
	{
		xMOV(to, g_psxConstRegs[fromgpr]);
	}
	else
	{
		const int reg = EEINST_USEDTEST(fromgpr) ? _allocX86reg(X86TYPE_PSX, fromgpr, MODE_READ) : _checkX86reg(X86TYPE_PSX, fromgpr, MODE_READ);
		if (reg >= 0)
			xMOV(to, xRegister32(reg));
		else
			xMOV(to, ptr[&psxRegs.GPR.r[fromgpr]]);
	}
}

static void _psxMoveGPRtoM(uintptr_t to, int fromgpr)
{
	if (PSX_IS_CONST1(fromgpr))
	{
		xMOV(ptr32[(uint32_t*)(to)], g_psxConstRegs[fromgpr]);
	}
	else
	{
		const int reg = EEINST_USEDTEST(fromgpr) ? _allocX86reg(X86TYPE_PSX, fromgpr, MODE_READ) : _checkX86reg(X86TYPE_PSX, fromgpr, MODE_READ);
		if (reg >= 0)
		{
			xMOV(ptr32[(uint32_t*)(to)], xRegister32(reg));
		}
		else
		{
			xMOV(eax, ptr[&psxRegs.GPR.r[fromgpr]]);
			xMOV(ptr32[(uint32_t*)(to)], eax);
		}
	}
}


static void rpsxMULTsuper(int info, int sign)
{
	// Lo/Hi = Rs * Rt (signed)
	_psxMoveGPRtoR(eax, _Rs_);

	const int regt = rpsxAllocRegIfUsed(_Rt_, MODE_READ);
	if (sign)
	{
		if (regt >= 0)
			xMUL(xRegister32(regt));
		else
			xMUL(ptr32[&psxRegs.GPR.r[_Rt_]]);
	}
	else
	{
		if (regt >= 0)
			xUMUL(xRegister32(regt));
		else
			xUMUL(ptr32[&psxRegs.GPR.r[_Rt_]]);
	}

	rpsxWritebackHILO(info);
}

static void rpsxMULT_consts(int info)
{
	rpsxMULTsuperconst(info, _Rt_, g_psxConstRegs[_Rs_], 1);
}

static void rpsxMULT_constt(int info)
{
	rpsxMULTsuperconst(info, _Rs_, g_psxConstRegs[_Rt_], 1);
}

static void rpsxMULT_(int info)
{
	rpsxMULTsuper(info, 1);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(MULT, 1, psxInstCycles_Mult);

//// MULTU
static void rpsxMULTU_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	uint64_t res = (uint64_t)((uint64_t)g_psxConstRegs[_Rs_] * (uint64_t)g_psxConstRegs[_Rt_]);

	xMOV(ptr32[&psxRegs.GPR.n.hi], (uint32_t)((res >> 32) & 0xffffffff));
	xMOV(ptr32[&psxRegs.GPR.n.lo], (uint32_t)(res & 0xffffffff));
}

static void rpsxMULTU_consts(int info)
{
	rpsxMULTsuperconst(info, _Rt_, g_psxConstRegs[_Rs_], 0);
}

static void rpsxMULTU_constt(int info)
{
	rpsxMULTsuperconst(info, _Rs_, g_psxConstRegs[_Rt_], 0);
}

static void rpsxMULTU_(int info)
{
	rpsxMULTsuper(info, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(MULTU, 1, psxInstCycles_Mult);

//// DIV
static void rpsxDIV_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	uint32_t lo, hi;

	/*
	 * Normally, when 0x80000000(-2147483648), the signed minimum value, is divided by 0xFFFFFFFF(-1), the
	 * 	operation will result in overflow. However, in this instruction an overflow exception does not occur and the
	 * 	result will be as follows:
	 * 	Quotient: 0x80000000 (-2147483648), and remainder: 0x00000000 (0)
	 */
	// Of course x86 cpu does overflow !
	if (g_psxConstRegs[_Rs_] == 0x80000000u && g_psxConstRegs[_Rt_] == 0xFFFFFFFFu)
	{
		xMOV(ptr32[&psxRegs.GPR.n.hi], 0);
		xMOV(ptr32[&psxRegs.GPR.n.lo], 0x80000000);
		return;
	}

	if (g_psxConstRegs[_Rt_] != 0)
	{
		lo = *(int*)&g_psxConstRegs[_Rs_] / *(int*)&g_psxConstRegs[_Rt_];
		hi = *(int*)&g_psxConstRegs[_Rs_] % *(int*)&g_psxConstRegs[_Rt_];
		xMOV(ptr32[&psxRegs.GPR.n.hi], hi);
		xMOV(ptr32[&psxRegs.GPR.n.lo], lo);
	}
	else
	{
		xMOV(ptr32[&psxRegs.GPR.n.hi], g_psxConstRegs[_Rs_]);
		if (g_psxConstRegs[_Rs_] & 0x80000000u)
		{
			xMOV(ptr32[&psxRegs.GPR.n.lo], 0x1);
		}
		else
		{
			xMOV(ptr32[&psxRegs.GPR.n.lo], 0xFFFFFFFFu);
		}
	}
}

static void rpsxDIVsuper(int info, int sign, int process = 0)
{
	uint8_t* end1, *end2, *cont3;
	// Lo/Hi = Rs / Rt (signed)
	if (process & PROCESS_CONSTT)
		xMOV(ecx, g_psxConstRegs[_Rt_]);
	else if (info & PROCESS_EE_T)
		xMOV(ecx, xRegister32(EEREC_T));
	else
		xMOV(ecx, ptr32[&psxRegs.GPR.r[_Rt_]]);

	if (process & PROCESS_CONSTS)
		xMOV(eax, g_psxConstRegs[_Rs_]);
	else if (info & PROCESS_EE_S)
		xMOV(eax, xRegister32(EEREC_S));
	else
		xMOV(eax, ptr32[&psxRegs.GPR.r[_Rs_]]);

	if (sign) //test for overflow (x86 will just throw an exception)
	{
		uint8_t *cont1, *cont2;

		xCMP(eax, 0x80000000);
		*(uint8_t*)x86Ptr = JNE8;
		x86Ptr += sizeof(uint8_t);
		*(uint8_t*)x86Ptr = 0;
		x86Ptr += sizeof(uint8_t);
		cont1       = (uint8_t*)(x86Ptr - 1);
		xCMP(ecx, 0xffffffff);
		*(uint8_t*)x86Ptr = JNE8;
		x86Ptr += sizeof(uint8_t);
		*(uint8_t*)x86Ptr = 0;
		x86Ptr += sizeof(uint8_t);
		cont2       = (uint8_t*)(x86Ptr - 1);
		//overflow case:
		xXOR(edx, edx); //EAX remains 0x80000000
		*(uint8_t*)x86Ptr = 0xEB;
		x86Ptr += sizeof(uint8_t);
		*(uint8_t*)x86Ptr = 0;
		x86Ptr += sizeof(uint8_t);
		end1        =  x86Ptr - 1;

		*cont1      = (uint8_t)((x86Ptr - cont1) - 1);
		*cont2      = (uint8_t)((x86Ptr - cont2) - 1);
	}

	xCMP(ecx, 0);
	*(uint8_t*)x86Ptr = JNE8;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint8_t);
	cont3       = (uint8_t*)(x86Ptr - 1);

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
	*(uint8_t*)x86Ptr = 0xEB;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint8_t);
	end2        =  x86Ptr - 1;

	// Normal division
	*cont3      = (uint8_t)((x86Ptr - cont3) - 1);
	if (sign)
	{
		*(uint8_t*)x86Ptr = 0x99;
		x86Ptr += sizeof(uint8_t);
		xDIV(ecx);
	}
	else
	{
		xXOR(edx, edx);
		xUDIV(ecx);
	}

	if (sign)
		*end1      = (uint8_t)((x86Ptr - end1) - 1);
	*end2      = (uint8_t)((x86Ptr - end2) - 1);

	rpsxWritebackHILO(info);
}

static void rpsxDIV_consts(int info)
{
	rpsxDIVsuper(info, 1, PROCESS_CONSTS);
}

static void rpsxDIV_constt(int info)
{
	rpsxDIVsuper(info, 1, PROCESS_CONSTT);
}

static void rpsxDIV_(int info)
{
	rpsxDIVsuper(info, 1);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(DIV, 1, psxInstCycles_Div);

//// DIVU
void rpsxDIVU_const(void)
{
	uint32_t lo, hi;

	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	if (g_psxConstRegs[_Rt_] != 0)
	{
		lo = g_psxConstRegs[_Rs_] / g_psxConstRegs[_Rt_];
		hi = g_psxConstRegs[_Rs_] % g_psxConstRegs[_Rt_];
		xMOV(ptr32[&psxRegs.GPR.n.hi], hi);
		xMOV(ptr32[&psxRegs.GPR.n.lo], lo);
	}
	else
	{
		xMOV(ptr32[&psxRegs.GPR.n.hi], g_psxConstRegs[_Rs_]);
		xMOV(ptr32[&psxRegs.GPR.n.lo], 0xFFFFFFFFu);
	}
}

void rpsxDIVU_consts(int info)
{
	rpsxDIVsuper(info, 0, PROCESS_CONSTS);
}

void rpsxDIVU_constt(int info)
{
	rpsxDIVsuper(info, 0, PROCESS_CONSTT);
}

void rpsxDIVU_(int info)
{
	rpsxDIVsuper(info, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(DIVU, 1, psxInstCycles_Div);

// TLB loadstore functions

static uint8_t* rpsxGetConstantAddressOperand(bool store)
{
	return nullptr;
}

static void rpsxCalcAddressOperand()
{
	// if it's a const register, just flush it, since we'll need to do that
	// when we call the load/store function anyway
	int rs;
	if (PSX_IS_CONST1(_Rs_))
		rs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	else
		rs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);

	_freeX86reg(arg1regd);

	if (rs >= 0)
		xMOV(arg1regd, xRegister32(rs));
	else
		xMOV(arg1regd, ptr32[&psxRegs.GPR.r[_Rs_]]);

	if (_Imm_)
		xADD(arg1regd, _Imm_);
}

static void rpsxCalcStoreOperand()
{
	int rt;
	if (PSX_IS_CONST1(_Rt_))
		rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	else
		rt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);

	_freeX86reg(arg2regd);

	if (rt >= 0)
		xMOV(arg2regd, xRegister32(rt));
	else
		xMOV(arg2regd, ptr32[&psxRegs.GPR.r[_Rt_]]);
}

static void rpsxLoad(int size, bool sign)
{
	rpsxCalcAddressOperand();

	if (_Rt_ != 0)
	{
		PSX_DEL_CONST(_Rt_);
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	}

	_psxFlushCall(FLUSH_FULLVTLB);
	xTEST(arg1regd, 0x10000000);
	xForwardJZ8 is_ram_read;

	switch (size)
	{
		case 8:
			xFastCall((void*)iopMemRead8);
			break;
		case 16:
			xFastCall((void*)iopMemRead16);
			break;
		case 32:
			xFastCall((void*)iopMemRead32);
			break;
		default:
			break;
	}

	if (_Rt_ == 0)
	{
		// dummy read
		is_ram_read.SetTarget();
		return;
	}

	xForwardJump8 done;
	is_ram_read.SetTarget();

	// read from psM directly
	xAND(arg1regd, 0x1fffff);

	auto addr = xComplexAddress(rax, iopMem->Main, arg1reg);
	switch (size)
	{
		case 8:
			xMOVZX(eax, ptr8[addr]);
			break;
		case 16:
			xMOVZX(eax, ptr16[addr]);
			break;
		case 32:
			xMOV(eax, ptr32[addr]);
			break;
		default:
			break;
	}

	done.SetTarget();

	const int rt = rpsxAllocRegIfUsed(_Rt_, MODE_WRITE);
	const xRegister32 dreg((rt < 0) ? eax.Id : rt);

	// sign/zero extend as needed
	switch (size)
	{
		case 8:
			sign ? xMOVSX(dreg, al) : xMOVZX(dreg, al);
			break;
		case 16:
			sign ? xMOVSX(dreg, ax) : xMOVZX(dreg, ax);
			break;
		case 32:
			xMOV(dreg, eax);
			break;
		default:
			break;
	}

	// if not caching, write back
	if (rt < 0)
		xMOV(ptr32[&psxRegs.GPR.r[_Rt_]], eax);
}

static void rpsxLWL(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)psxLWL);
	PSX_DEL_CONST(_Rt_);
}

static void rpsxLWR(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)psxLWR);
	PSX_DEL_CONST(_Rt_);
}

static void rpsxSWL(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)psxSWL);
	PSX_DEL_CONST(_Rt_);
}

static void rpsxSWR(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)psxSWR);
	PSX_DEL_CONST(_Rt_);
}

static void rpsxLB()
{
	rpsxLoad(8, true);
}

static void rpsxLBU()
{
	rpsxLoad(8, false);
}

static void rpsxLH()
{
	rpsxLoad(16, true);
}

static void rpsxLHU()
{
	rpsxLoad(16, false);
}

static void rpsxLW()
{
	rpsxLoad(32, false);
}

static void rpsxSB()
{
	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
	xFastCall((void*)iopMemWrite8);
}

static void rpsxSH()
{
	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
	xFastCall((void*)iopMemWrite16);
}

static void rpsxSW()
{
	uint8_t* ptr = rpsxGetConstantAddressOperand(true);
	if (ptr)
	{
		const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		xMOV(ptr32[ptr], xRegister32(rt));
		return;
	}

	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
	xFastCall((void*)iopMemWrite32);
}

//// SLL
static void rpsxSLL_const(void)
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] << _Sa_;
}

static void rpsxSLLs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0)
		xSHL(xRegister32(EEREC_D), sa);
}

static void rpsxSLL_(int info)
{
	rpsxSLLs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SLL, XMMINFO_WRITED | XMMINFO_READS);

//// SRL
static void rpsxSRL_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] >> _Sa_;
}

static void rpsxSRLs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0)
		xSHR(xRegister32(EEREC_D), sa);
}

static void rpsxSRL_(int info)
{
	rpsxSRLs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SRL, XMMINFO_WRITED | XMMINFO_READS);

//// SRA
static void rpsxSRA_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rt_] >> _Sa_;
}

static void rpsxSRAs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0)
		xSAR(xRegister32(EEREC_D), sa);
}

static void rpsxSRA_(int info)
{
	rpsxSRAs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SRA, XMMINFO_WRITED | XMMINFO_READS);

//// SLLV
static void rpsxShiftV_constt(int info, const xImpl_Group2& shift)
{
	rpsxMoveSToECX(info);
	xMOV(xRegister32(EEREC_D), g_psxConstRegs[_Rt_]);
	shift(xRegister32(EEREC_D), cl);
}

static void rpsxShiftV(int info, const xImpl_Group2& shift)
{
	rpsxMoveSToECX(info);
	rpsxMoveTtoD(info);
	shift(xRegister32(EEREC_D), cl);
}

static void rpsxSLLV_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] << (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSLLV_consts(int info)
{
	rpsxSLLs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSLLV_constt(int info)
{
	rpsxShiftV_constt(info, xSHL);
}

static void rpsxSLLV_(int info)
{
	rpsxShiftV(info, xSHL);
}

PSXRECOMPILE_CONSTCODE0(SLLV, XMMINFO_WRITED | XMMINFO_READS);

//// SRLV
static void rpsxSRLV_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] >> (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRLV_consts(int info)
{
	rpsxSRLs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRLV_constt(int info)
{
	rpsxShiftV_constt(info, xSHR);
}

static void rpsxSRLV_(int info)
{
	rpsxShiftV(info, xSHR);
}

PSXRECOMPILE_CONSTCODE0(SRLV, XMMINFO_WRITED | XMMINFO_READS);

//// SRAV
static void rpsxSRAV_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rt_] >> (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRAV_consts(int info)
{
	rpsxSRAs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRAV_constt(int info)
{
	rpsxShiftV_constt(info, xSAR);
}

static void rpsxSRAV_(int info)
{
	rpsxShiftV(info, xSAR);
}

static void psxSetBranchImm(uint32_t imm)
{
	psxbranch = 1;

	// end the current block
	xMOV(ptr32[&psxRegs.pc], imm);
	_psxFlushCall(FLUSH_EVERYTHING);
	iPsxBranchTest(imm, imm <= psxpc);

	recBlocks.Link(HWADDR(imm), xJcc32(Jcc_Unconditional, 0));
}

/* jmp rel32 */
static uint32_t* JMP32(uintptr_t to)
{
	*(uint8_t*)x86Ptr = 0xE9;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = to;
	x86Ptr += sizeof(uint32_t);
	return (uint32_t*)(x86Ptr - 4);
}

static bool psxTrySwapDelaySlot(uint32_t rs, uint32_t rt, uint32_t rd)
{
	if (s_recompilingDelaySlot)
		return false;

	const uint32_t opcode_encoded = iopMemRead32(psxpc);
	if (opcode_encoded == 0)
	{
		psxRecompileNextInstruction(true, true);
		return true;
	}

	const uint32_t opcode_rs = ((opcode_encoded >> 21) & 0x1F);
	const uint32_t opcode_rt = ((opcode_encoded >> 16) & 0x1F);
	const uint32_t opcode_rd = ((opcode_encoded >> 11) & 0x1F);

	switch (opcode_encoded >> 26)
	{
		case 8: // ADDI
		case 9: // ADDIU
		case 10: // SLTI
		case 11: // SLTIU
		case 12: // ANDIU
		case 13: // ORI
		case 14: // XORI
		case 15: // LUI
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
		case 46: // SWR
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 50: // LWC2
		case 58: // SWC2
			break;

		case 0: // SPECIAL
		{
			switch (opcode_encoded & 0x3F)
			{
				case 0: // SLL
				case 2: // SRL
				case 3: // SRA
				case 4: // SLLV
				case 6: // SRLV
				case 7: // SRAV
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
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						goto is_unsafe;
				}
				break;

				case 15: // SYNC
				case 24: // MULT
				case 25: // MULTU
				case 26: // DIV
				case 27: // DIVU
					break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 16: // COP0
		case 17: // COP1
		case 18: // COP2
		case 19: // COP3
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC0
				case 2: // CFC0
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4: // MTC0
				case 6: // CTC0
					break;

				default:
				{
					// swap when it's GTE
					if ((opcode_encoded >> 26) != 18)
						goto is_unsafe;
				}
				break;
			}
			break;
		}
		break;

		default:
			goto is_unsafe;
	}

	psxRecompileNextInstruction(true, true);
	return true;

is_unsafe:
	return false;
}


static void psxSetBranchReg(uint32_t reg)
{
	psxbranch = 1;

	if (reg != 0xffffffff)
	{
		const bool swap = psxTrySwapDelaySlot(reg, 0, 0);

		if (!swap)
		{
			const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
			_psxMoveGPRtoR(xRegister32(wbreg), reg);

			psxRecompileNextInstruction(true, false);

			if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
			{
				xMOV(ptr32[&psxRegs.pc], xRegister32(wbreg));
				x86regs[wbreg].inuse = 0;
			}
			else
			{
				xMOV(eax, ptr32[&psxRegs.pcWriteback]);
				xMOV(ptr32[&psxRegs.pc], eax);
			}
		}
		else
		{
			if (PSX_IS_DIRTY_CONST(reg) || _hasX86reg(X86TYPE_PSX, reg, 0))
			{
				const int x86reg = _allocX86reg(X86TYPE_PSX, reg, MODE_READ);
				xMOV(ptr32[&psxRegs.pc], xRegister32(x86reg));
			}
			else
			{
				_psxMoveGPRtoM((uintptr_t)&psxRegs.pc, reg);
			}
		}
	}

	_psxFlushCall(FLUSH_EVERYTHING);
	iPsxBranchTest(0xffffffff, 1);

	JMP32((uintptr_t)iopDispatcherReg - ((uintptr_t)x86Ptr + 5));
}

PSXRECOMPILE_CONSTCODE0(SRAV, XMMINFO_WRITED | XMMINFO_READS);

static __fi uint32_t psxScaleBlockCycles(void) { return s_psxBlockCycles; }

void rpsxSYSCALL(void)
{
	uint8_t *j8Ptr;

	xMOV(ptr32[&psxRegs.code], psxRegs.code);
	xMOV(ptr32[&psxRegs.pc], psxpc - 4);
	_psxFlushCall(FLUSH_NODESTROY);

	//xMOV( ecx, 0x20 );			// exception code
	//xMOV( edx, psxbranch==1 );	// branch delay slot?
	xFastCall((const void*)psxException, 0x20, psxbranch == 1);

	xCMP(ptr32[&psxRegs.pc], psxpc - 4);
	*(uint8_t*)x86Ptr = JE8;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint8_t);
	j8Ptr = (uint8_t*)(x86Ptr - 1);

	xADD(ptr32[&psxRegs.cycle], psxScaleBlockCycles());
	xSUB(ptr32[&psxRegs.iopCycleEE], psxScaleBlockCycles() * 8);
	JMP32((uintptr_t)iopDispatcherReg - ((uintptr_t)x86Ptr + 5));

	// jump target for skipping blockCycle updates
	*j8Ptr      = (uint8_t)((x86Ptr - j8Ptr) - 1);
}

void rpsxBREAK(void)
{
	uint8_t *j8Ptr;
	xMOV(ptr32[&psxRegs.code], psxRegs.code);
	xMOV(ptr32[&psxRegs.pc], psxpc - 4);
	_psxFlushCall(FLUSH_NODESTROY);

	//xMOV( ecx, 0x24 );			// exception code
	//xMOV( edx, psxbranch==1 );	// branch delay slot?
	xFastCall((const void*)psxException, 0x24, psxbranch == 1);

	xCMP(ptr32[&psxRegs.pc], psxpc - 4);
	*(uint8_t*)x86Ptr = JE8;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint8_t);
	j8Ptr = (uint8_t*)(x86Ptr - 1);
	xADD(ptr32[&psxRegs.cycle], psxScaleBlockCycles());
	xSUB(ptr32[&psxRegs.iopCycleEE], psxScaleBlockCycles() * 8);
	JMP32((uintptr_t)iopDispatcherReg - ((uintptr_t)x86Ptr + 5));
	*j8Ptr      = (uint8_t)((x86Ptr - j8Ptr) - 1);
}



static void rpsxMFHI()
{
	if (!_Rd_)
		return;

	rpsxCopyReg(_Rd_, PSX_HI);
}

static void rpsxMTHI()
{
	rpsxCopyReg(PSX_HI, _Rs_);
}

static void rpsxMFLO()
{
	if (!_Rd_)
		return;

	rpsxCopyReg(_Rd_, PSX_LO);
}

static void rpsxMTLO()
{
	rpsxCopyReg(PSX_LO, _Rs_);
}

static void rpsxJ()
{
	// j target
	uint32_t newpc = _InstrucTarget_ * 4 + (psxpc & 0xf0000000);
	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(newpc);
}

static void rpsxJAL()
{
	uint32_t newpc = (_InstrucTarget_ << 2) + (psxpc & 0xf0000000);
	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);
	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(newpc);
}

static void rpsxJR()
{
	psxSetBranchReg(_Rs_);
}

static void rpsxJALR()
{
	const uint32_t newpc = psxpc + 4;
	const bool swap = (_Rd_ == _Rs_) ? false : psxTrySwapDelaySlot(_Rs_, 0, _Rd_);

	// jalr Rs
	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_psxMoveGPRtoR(xRegister32(wbreg), _Rs_);
	}

	if (_Rd_)
	{
		_psxDeleteReg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		g_psxConstRegs[_Rd_] = newpc;
	}

	if (!swap)
	{
		psxRecompileNextInstruction(true, false);

		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
			xMOV(ptr32[&psxRegs.pc], xRegister32(wbreg));
			x86regs[wbreg].inuse = 0;
		}
		else
		{
			xMOV(eax, ptr32[&psxRegs.pcWriteback]);
			xMOV(ptr32[&psxRegs.pc], eax);
		}
	}
	else
	{
		if (PSX_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_PSX, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
			xMOV(ptr32[&psxRegs.pc], xRegister32(x86reg));
		}
		else
		{
			_psxMoveGPRtoM((uintptr_t)&psxRegs.pc, _Rs_);
		}
	}

	psxSetBranchReg(0xffffffff);
}

//// BEQ
static uint32_t* s_pbranchjmp;

static void rpsxSetBranchEQ(int process)
{
	if (process & PROCESS_CONSTS)
	{
		const int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		if (regt >= 0)
			xCMP(xRegister32(regt), g_psxConstRegs[_Rs_]);
		else
			xCMP(ptr32[&psxRegs.GPR.r[_Rt_]], g_psxConstRegs[_Rs_]);
	}
	else if (process & PROCESS_CONSTT)
	{
		const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
		if (regs >= 0)
			xCMP(xRegister32(regs), g_psxConstRegs[_Rt_]);
		else
			xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], g_psxConstRegs[_Rt_]);
	}
	else
	{
		// force S into register, since we need to load it, may as well cache.
		const int regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
		const int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);

		if (regt >= 0)
			xCMP(xRegister32(regs), xRegister32(regt));
		else
			xCMP(xRegister32(regs), ptr32[&psxRegs.GPR.r[_Rt_]]);
	}

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JNE32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	s_pbranchjmp = (uint32_t*)(x86Ptr - 4);
}

static void rpsxBEQ_const()
{
	uint32_t branchTo;

	if (g_psxConstRegs[_Rs_] == g_psxConstRegs[_Rt_])
		branchTo = ((s32)_Imm_ * 4) + psxpc;
	else
		branchTo = psxpc + 4;

	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(branchTo);
}

static void rpsxBEQ_process(int process)
{
	uint32_t branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (_Rs_ == _Rt_)
	{
		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
	}
	else
	{
		const bool swap = psxTrySwapDelaySlot(_Rs_, _Rt_, 0);
		_psxFlushAllDirty();
		rpsxSetBranchEQ(process);

		if (!swap)
		{
			psxSaveBranchState();
			psxRecompileNextInstruction(true, false);
		}

		psxSetBranchImm(branchTo);

		while ((uintptr_t)x86Ptr & 0xf)
			*x86Ptr++ = 0x90;
		*s_pbranchjmp = (x86Ptr - (uint8_t*)s_pbranchjmp) - 4;

		if (!swap)
		{
			// recopy the next inst
			psxpc -= 4;
			psxLoadBranchState();
			psxRecompileNextInstruction(true, false);
		}

		psxSetBranchImm(psxpc);
	}
}

static void rpsxBEQ(void)
{
	// prefer using the host register over an immediate, it'll be smaller code.
	if (PSX_IS_CONST2(_Rs_, _Rt_))
		rpsxBEQ_const();
	else if (PSX_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ) < 0)
		rpsxBEQ_process(PROCESS_CONSTS);
	else if (PSX_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ) < 0)
		rpsxBEQ_process(PROCESS_CONSTT);
	else
		rpsxBEQ_process(0);
}

//// BNE
static void rpsxBNE_const(void)
{
	uint32_t branchTo;

	if (g_psxConstRegs[_Rs_] != g_psxConstRegs[_Rt_])
		branchTo = ((s32)_Imm_ * 4) + psxpc;
	else
		branchTo = psxpc + 4;

	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(branchTo);
}

static void rpsxBNE_process(int process)
{
	const uint32_t branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (_Rs_ == _Rt_)
	{
		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(psxpc);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, _Rt_, 0);
	_psxFlushAllDirty();
	rpsxSetBranchEQ(process);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*s_pbranchjmp = (x86Ptr - (uint8_t*)s_pbranchjmp) - 4;

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

static void rpsxBNE(void)
{
	if (PSX_IS_CONST2(_Rs_, _Rt_))
		rpsxBNE_const();
	else if (PSX_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ) < 0)
		rpsxBNE_process(PROCESS_CONSTS);
	else if (PSX_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ) < 0)
		rpsxBNE_process(PROCESS_CONSTT);
	else
		rpsxBNE_process(0);
}

//// BLTZ
static void rpsxBLTZ(void)
{
	uint32_t *pjmp;
	// Branch if Rs < 0
	uint32_t branchTo = (s32)_Imm_ * 4 + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] >= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xCMP(xRegister32(regs), 0);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JL32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	pjmp = (uint32_t*)(x86Ptr - 4);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*pjmp = (x86Ptr - (uint8_t*)pjmp) - 4;

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BGEZ
static void rpsxBGEZ(void)
{
	uint32_t *pjmp;
	uint32_t branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] < 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xCMP(xRegister32(regs), 0);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JGE32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	pjmp = (uint32_t*)(x86Ptr - 4);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*pjmp = (x86Ptr - (uint8_t*)pjmp) - 4;

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BLTZAL
static void rpsxBLTZAL(void)
{
	uint32_t *pjmp;
	// Branch if Rs < 0
	uint32_t branchTo = (s32)_Imm_ * 4 + psxpc;

	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);

	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] >= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xCMP(xRegister32(regs), 0);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JL32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	pjmp = (uint32_t*)(x86Ptr - 4);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*pjmp = (x86Ptr - (uint8_t*)pjmp) - 4;

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BGEZAL
static void rpsxBGEZAL(void)
{
	uint32_t *pjmp;
	uint32_t branchTo = ((s32)_Imm_ * 4) + psxpc;

	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);

	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] < 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xCMP(xRegister32(regs), 0);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JGE32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	pjmp = (uint32_t*)(x86Ptr - 4);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*pjmp = (x86Ptr - (uint8_t*)pjmp) - 4;

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BLEZ
static void rpsxBLEZ(void)
{
	uint32_t *pjmp;
	// Branch if Rs <= 0
	uint32_t branchTo = (s32)_Imm_ * 4 + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] > 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xCMP(xRegister32(regs), 0);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JLE32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	pjmp = (uint32_t*)(x86Ptr - 4);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*pjmp = (x86Ptr - (uint8_t*)pjmp) - 4;

	if (!swap)
	{
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BGTZ
static void rpsxBGTZ(void)
{
	uint32_t *pjmp;
	// Branch if Rs > 0
	uint32_t branchTo = (s32)_Imm_ * 4 + psxpc;

	_psxFlushAllDirty();

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] <= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		xCMP(xRegister32(regs), 0);
	else
		xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);

	*(uint8_t*)x86Ptr = 0x0F;
	x86Ptr += sizeof(uint8_t);
	*(uint8_t*)x86Ptr = JG32;
	x86Ptr += sizeof(uint8_t);
	*(uint32_t*)x86Ptr = 0;
	x86Ptr += sizeof(uint32_t);
	pjmp = (uint32_t*)(x86Ptr - 4);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

	while ((uintptr_t)x86Ptr & 0xf)
		*x86Ptr++ = 0x90;
	*pjmp = (x86Ptr - (uint8_t*)pjmp) - 4;

	if (!swap)
	{
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

static void rpsxMFC0(void)
{
	// Rt = Cop0->Rd
	if (!_Rt_)
		return;

	const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	xMOV(xRegister32(rt), ptr32[&psxRegs.CP0.r[_Rd_]]);
}

static void rpsxCFC0(void)
{
	// Rt = Cop0->Rd
	if (!_Rt_)
		return;

	const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	xMOV(xRegister32(rt), ptr32[&psxRegs.CP0.r[_Rd_]]);
}

static void rpsxMTC0(void)
{
	// Cop0->Rd = Rt
	if (PSX_IS_CONST1(_Rt_))
	{
		xMOV(ptr32[&psxRegs.CP0.r[_Rd_]], g_psxConstRegs[_Rt_]);
	}
	else
	{
		const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		xMOV(ptr32[&psxRegs.CP0.r[_Rd_]], xRegister32(rt));
	}
}


static void rpsxCTC0(void) { rpsxMTC0(); } /* Cop0->Rd = Rt */

static void rpsxRFE(void)
{
	xMOV(eax, ptr32[&psxRegs.CP0.n.Status]);
	xMOV(ecx, eax);
	xAND(eax, 0xfffffff0);
	xAND(ecx, 0x3c);
	xSHR(ecx, 2);
	xOR(eax, ecx);
	xMOV(ptr32[&psxRegs.CP0.n.Status], eax);

	/* Test the IOP's INTC status, so that any pending ints get raised. */

	_psxFlushCall(0);
	xFastCall((void*)(uintptr_t)&iopTestIntc);
}

/* COP2 */
static void rgteRTPS(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteRTPS);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCLIP(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCLIP);
	PSX_DEL_CONST(_Rt_);
}

static void rgteOP(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteOP);
	PSX_DEL_CONST(_Rt_);
}

static void rgteDPCS(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteDPCS);
	PSX_DEL_CONST(_Rt_);
}

static void rgteINTPL(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteINTPL);
	PSX_DEL_CONST(_Rt_);
}

static void rgteMVMVA(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteMVMVA);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCDS(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCDS);
	PSX_DEL_CONST(_Rt_);
}

static void rgteCDP(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteCDP);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCDT(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCDT);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCCS(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCCS);
	PSX_DEL_CONST(_Rt_);
}

static void rgteCC(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteCC);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCS(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCS);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCT(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCT);
	PSX_DEL_CONST(_Rt_);
}

static void rgteSQR(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteSQR);
	PSX_DEL_CONST(_Rt_);
}

static void rgteDCPL(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteDCPL);
	PSX_DEL_CONST(_Rt_);
}

static void rgteDPCT(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteDPCT);
	PSX_DEL_CONST(_Rt_);
}

static void rgteAVSZ3(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteAVSZ3);
	PSX_DEL_CONST(_Rt_);
}

static void rgteAVSZ4(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteAVSZ4);
	PSX_DEL_CONST(_Rt_);
}

static void rgteRTPT(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteRTPT);
	PSX_DEL_CONST(_Rt_);
}

static void rgteGPF(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteGPF);
	PSX_DEL_CONST(_Rt_);
}

static void rgteGPL(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteGPL);
	PSX_DEL_CONST(_Rt_);
}

static void rgteNCCT(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteNCCT);
	PSX_DEL_CONST(_Rt_);
}

static void rgteMFC2(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteMFC2);
	PSX_DEL_CONST(_Rt_);
}

static void rgteCFC2(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteCFC2);
	PSX_DEL_CONST(_Rt_);
}

static void rgteMTC2(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteMTC2);
	PSX_DEL_CONST(_Rt_);
}

static void rgteCTC2(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteCTC2);
	PSX_DEL_CONST(_Rt_);
}

static void rgteLWC2(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteLWC2);
	PSX_DEL_CONST(_Rt_);
}

static void rgteSWC2(void)
{
	xMOV(ptr32[&psxRegs.code], (uint32_t)psxRegs.code);
	_psxFlushCall(FLUSH_EVERYTHING);
	xFastCall((void*)(uintptr_t)gteSWC2);
	PSX_DEL_CONST(_Rt_);
}

/* R3000A tables */
extern void (*rpsxBSC[64])();
extern void (*rpsxSPC[64])();
extern void (*rpsxREG[32])();
extern void (*rpsxCP0[32])();
extern void (*rpsxCP2[64])();
extern void (*rpsxCP2BSC[32])();

static void rpsxSPECIAL(void) { rpsxSPC[_Funct_](); }
static void rpsxREGIMM(void) { rpsxREG[_Rt_](); }
static void rpsxCOP0(void) { rpsxCP0[_Rs_](); }
static void rpsxCOP2(void) { rpsxCP2[_Funct_](); }
static void rpsxBASIC(void) { rpsxCP2BSC[_Rs_](); }

static void rpsxNULL(void) { }

// clang-format off
void (*rpsxBSC[64])() = {
	rpsxSPECIAL, rpsxREGIMM, rpsxJ   , rpsxJAL  , rpsxBEQ , rpsxBNE , rpsxBLEZ, rpsxBGTZ,
	rpsxADDI   , rpsxADDIU , rpsxSLTI, rpsxSLTIU, rpsxANDI, rpsxORI , rpsxXORI, rpsxLUI ,
	rpsxCOP0   , rpsxNULL  , rpsxCOP2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL   , rpsxNULL  , rpsxNULL, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxLB     , rpsxLH    , rpsxLWL , rpsxLW   , rpsxLBU , rpsxLHU , rpsxLWR , rpsxNULL,
	rpsxSB     , rpsxSH    , rpsxSWL , rpsxSW   , rpsxNULL, rpsxNULL, rpsxSWR , rpsxNULL,
	rpsxNULL   , rpsxNULL  , rgteLWC2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL   , rpsxNULL  , rgteSWC2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

static void (*rpsxSPC[64])() = {
	rpsxSLL , rpsxNULL, rpsxSRL , rpsxSRA , rpsxSLLV   , rpsxNULL , rpsxSRLV, rpsxSRAV,
	rpsxJR  , rpsxJALR, rpsxNULL, rpsxNULL, rpsxSYSCALL, rpsxBREAK, rpsxNULL, rpsxNULL,
	rpsxMFHI, rpsxMTHI, rpsxMFLO, rpsxMTLO, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxMULT, rpsxMULTU, rpsxDIV, rpsxDIVU, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxADD , rpsxADDU, rpsxSUB , rpsxSUBU, rpsxAND    , rpsxOR   , rpsxXOR , rpsxNOR ,
	rpsxNULL, rpsxNULL, rpsxSLT , rpsxSLTU, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
};

static void (*rpsxREG[32])() = {
	rpsxBLTZ  , rpsxBGEZ  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL  , rpsxNULL  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxBLTZAL, rpsxBGEZAL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL  , rpsxNULL  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

static void (*rpsxCP0[32])() = {
	rpsxMFC0, rpsxNULL, rpsxCFC0, rpsxNULL, rpsxMTC0, rpsxNULL, rpsxCTC0, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxRFE , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

static void (*rpsxCP2[64])() = {
	rpsxBASIC, rgteRTPS , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rgteNCLIP, rpsxNULL, // 00
	rpsxNULL , rpsxNULL , rpsxNULL , rpsxNULL, rgteOP  , rpsxNULL , rpsxNULL , rpsxNULL, // 08
	rgteDPCS , rgteINTPL, rgteMVMVA, rgteNCDS, rgteCDP , rpsxNULL , rgteNCDT , rpsxNULL, // 10
	rpsxNULL , rpsxNULL , rpsxNULL , rgteNCCS, rgteCC  , rpsxNULL , rgteNCS  , rpsxNULL, // 18
	rgteNCT  , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rpsxNULL , rpsxNULL, // 20
	rgteSQR  , rgteDCPL , rgteDPCT , rpsxNULL, rpsxNULL, rgteAVSZ3, rgteAVSZ4, rpsxNULL, // 28
	rgteRTPT , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rpsxNULL , rpsxNULL, // 30
	rpsxNULL , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rgteGPF  , rgteGPL  , rgteNCCT, // 38
};

static void (*rpsxCP2BSC[32])() = {
	rgteMFC2, rpsxNULL, rgteCFC2, rpsxNULL, rgteMTC2, rpsxNULL, rgteCTC2, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};
// clang-format on

////////////////////////////////////////////////
// Back-Prob Function Tables - Gathering Info //
////////////////////////////////////////////////
#define rpsxpropSetRead(reg) \
	{ \
		if (!(pinst->regs[reg] & EEINST_USED)) \
			pinst->regs[reg] |= EEINST_LASTUSE; \
		prev->regs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->regs[reg] |= EEINST_USED; \
		for (size_t i = 0; i < std::size(pinst->readType); ++i) \
		{ \
			if (pinst->readType[i] == XMMTYPE_TEMP) \
			{ \
				pinst->readType[i] = XMMTYPE_GPRREG; \
				pinst->readReg[i]  = reg; \
				break; \
			} \
		} \
	}

#define rpsxpropSetWrite(reg) \
	{ \
		prev->regs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
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
	}

void rpsxpropBSC(EEINST* prev, EEINST* pinst);
void rpsxpropSPECIAL(EEINST* prev, EEINST* pinst);
void rpsxpropREGIMM(EEINST* prev, EEINST* pinst);
void rpsxpropCP0(EEINST* prev, EEINST* pinst);

// Basic table:
// gteMFC2, psxNULL, gteCFC2, psxNULL, gteMTC2, psxNULL, gteCTC2, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
static void rpsxpropCP2_basic(EEINST* prev, EEINST* pinst)
{
	switch (_Rs_)
	{
		case 0: // mfc2
		case 2: // cfc2
			rpsxpropSetWrite(_Rt_);
			break;

		case 4: // mtc2
		case 6: // ctc2
			rpsxpropSetRead(_Rt_);
			break;

		default:
			break;
	}
}

// Main table:
// psxBASIC, gteRTPS , psxNULL , psxNULL, psxNULL, psxNULL , gteNCLIP, psxNULL, // 00
// psxNULL , psxNULL , psxNULL , psxNULL, gteOP  , psxNULL , psxNULL , psxNULL, // 08
// gteDPCS , gteINTPL, gteMVMVA, gteNCDS, gteCDP , psxNULL , gteNCDT , psxNULL, // 10
// psxNULL , psxNULL , psxNULL , gteNCCS, gteCC  , psxNULL , gteNCS  , psxNULL, // 18
// gteNCT  , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 20
// gteSQR  , gteDCPL , gteDPCT , psxNULL, psxNULL, gteAVSZ3, gteAVSZ4, psxNULL, // 28
// gteRTPT , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 30
// psxNULL , psxNULL , psxNULL , psxNULL, psxNULL, gteGPF  , gteGPL  , gteNCCT, // 38
static void rpsxpropCP2(EEINST* prev, EEINST* pinst)
{
	switch (_Funct_)
	{
		case 0: // Basic opcode
			rpsxpropCP2_basic(prev, pinst);
			break;

		default:
			// COP2 operation are likely done with internal COP2 registers
			// No impact on GPR
			break;
	}
}

//SPECIAL, REGIMM, J   , JAL  , BEQ , BNE , BLEZ, BGTZ,
//ADDI   , ADDIU , SLTI, SLTIU, ANDI, ORI , XORI, LUI ,
//COP0   , NULL  , COP2, NULL , NULL, NULL, NULL, NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL,
//LB     , LH    , LWL , LW   , LBU , LHU , LWR , NULL,
//SB     , SH    , SWL , SW   , NULL, NULL, SWR , NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL
void rpsxpropBSC(EEINST* prev, EEINST* pinst)
{
	switch (psxRegs.code >> 26)
	{
		case 0:
			rpsxpropSPECIAL(prev, pinst);
			break;
		case 1:
			rpsxpropREGIMM(prev, pinst);
			break;
		case 2: // j
			break;
		case 3: // jal
			rpsxpropSetWrite(31);
			break;
		case 4: // beq
		case 5: // bne
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;

		case 6: // blez
		case 7: // bgtz
			rpsxpropSetRead(_Rs_);
			break;

		case 15: // lui
			rpsxpropSetWrite(_Rt_);
			break;

		case 16:
			rpsxpropCP0(prev, pinst);
			break;
		case 18:
			rpsxpropCP2(prev, pinst);
			break;

		// stores
		case 40:
		case 41:
		case 42:
		case 43:
		case 46:
			rpsxpropSetRead(_Rt_);
			rpsxpropSetRead(_Rs_);
			break;

		case 50: // LWC2
		case 58: // SWC2
			// Operation on COP2 registers/memory. GPRs are left untouched
			break;

		default:
			rpsxpropSetWrite(_Rt_);
			rpsxpropSetRead(_Rs_);
			break;
	}
}

//SLL , NULL, SRL , SRA , SLLV   , NULL , SRLV, SRAV,
//JR  , JALR, NULL, NULL, SYSCALL, BREAK, NULL, NULL,
//MFHI, MTHI, MFLO, MTLO, NULL   , NULL , NULL, NULL,
//MULT, MULTU, DIV, DIVU, NULL   , NULL , NULL, NULL,
//ADD , ADDU, SUB , SUBU, AND    , OR   , XOR , NOR ,
//NULL, NULL, SLT , SLTU, NULL   , NULL , NULL, NULL,
//NULL, NULL, NULL, NULL, NULL   , NULL , NULL, NULL,
//NULL, NULL, NULL, NULL, NULL   , NULL , NULL, NULL,
void rpsxpropSPECIAL(EEINST* prev, EEINST* pinst)
{
	switch (_Funct_)
	{
		case 0: // SLL
		case 2: // SRL
		case 3: // SRA
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rt_);
			break;

		case 8: // JR
			rpsxpropSetRead(_Rs_);
			break;
		case 9: // JALR
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rs_);
			break;

		case 12: // syscall
		case 13: // break
			_recClearInst(prev);
			prev->info = 0;
			break;
		case 15: // sync
			break;

		case 16: // mfhi
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(PSX_HI);
			break;
		case 17: // mthi
			rpsxpropSetWrite(PSX_HI);
			rpsxpropSetRead(_Rs_);
			break;
		case 18: // mflo
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(PSX_LO);
			break;
		case 19: // mtlo
			rpsxpropSetWrite(PSX_LO);
			rpsxpropSetRead(_Rs_);
			break;

		case 24: // mult
		case 25: // multu
		case 26: // div
		case 27: // divu
			rpsxpropSetWrite(PSX_LO);
			rpsxpropSetWrite(PSX_HI);
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;

		case 32: // add
		case 33: // addu
		case 34: // sub
		case 35: // subu
			rpsxpropSetWrite(_Rd_);
			if (_Rs_)
				rpsxpropSetRead(_Rs_);
			if (_Rt_)
				rpsxpropSetRead(_Rt_);
			break;

		default:
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;
	}
}

//BLTZ  , BGEZ  , NULL, NULL, NULL, NULL, NULL, NULL,
//NULL  , NULL  , NULL, NULL, NULL, NULL, NULL, NULL,
//BLTZAL, BGEZAL, NULL, NULL, NULL, NULL, NULL, NULL,
//NULL  , NULL  , NULL, NULL, NULL, NULL, NULL, NULL
void rpsxpropREGIMM(EEINST* prev, EEINST* pinst)
{
	switch (_Rt_)
	{
		case 0: // bltz
		case 1: // bgez
			rpsxpropSetRead(_Rs_);
			break;

		case 16: // bltzal
		case 17: // bgezal
			// do not write 31
			rpsxpropSetRead(_Rs_);
			break;
		default:
			break;
	}
}

//MFC0, NULL, CFC0, NULL, MTC0, NULL, CTC0, NULL,
//NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
//RFE , NULL, NULL, NULL, NULL, NULL, NULL, NULL,
//NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
void rpsxpropCP0(EEINST* prev, EEINST* pinst)
{
	switch (_Rs_)
	{
		case 0: // mfc0
		case 2: // cfc0
			rpsxpropSetWrite(_Rt_);
			break;

		case 4: // mtc0
		case 6: // ctc0
			rpsxpropSetRead(_Rt_);
			break;
		case 16: // rfe
		default:
			break;
	}
}

#define PSX_GETBLOCK(x) PC_GETBLOCK_(x, psxRecLUT)

#define PSXREC_CLEARM(mem) \
	(((mem) < g_psxMaxRecMem && (psxRecLUT[(mem) >> 16] + (mem))) ? \
			psxRecClearMem(mem) : \
            4)

// =====================================================================================================
//  Dynamically Compiled Dispatchers - R3000A style
// =====================================================================================================

static void recEventTest(void)
{
	_cpuEventTest_Shared();
}

// The address for all cleared blocks.  It recompiles the current pc and then
// dispatches to the recompiled block address.
static const void* _DynGen_JITCompile(void)
{
	uint8_t* retval = x86Ptr;

	xFastCall((const void*)iopRecRecompile, ptr32[&psxRegs.pc]);

	xMOV(eax, ptr[&psxRegs.pc]);
	xMOV(ebx, eax);
	xSHR(eax, 16);
	xMOV(rcx, ptrNative[xComplexAddress(rcx, psxRecLUT, rax * sizeof(intptr_t))]);
	xJMP(ptrNative[rbx * (sizeof(intptr_t) / 4) + rcx]);

	return retval;
}

// called when jumping to variable pc address
static const void* _DynGen_DispatcherReg(void)
{
	uint8_t* retval = x86Ptr;

	xMOV(eax, ptr[&psxRegs.pc]);
	xMOV(ebx, eax);
	xSHR(eax, 16);
	xMOV(rcx, ptrNative[xComplexAddress(rcx, psxRecLUT, rax * sizeof(intptr_t))]);
	xJMP(ptrNative[rbx * (sizeof(intptr_t) / 4) + rcx]);

	return retval;
}

// --------------------------------------------------------------------------------------
//  EnterRecompiledCode  - dynamic compilation stub!
// --------------------------------------------------------------------------------------
static const void* _DynGen_EnterRecompiledCode(void)
{
	// Optimization: The IOP never uses stack-based parameter invocation, so we can avoid
	// allocating any room on the stack for it (which is important since the IOP's entry
	// code gets invoked quite a lot).

	uint8_t* retval = x86Ptr;

	{ // Properly scope the frame prologue/epilogue
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		xJMP((const void*)iopDispatcherReg);

		// Save an exit point
		iopExitRecompiledCode = x86Ptr;
		SCOPED_STACK_FRAME_END(m_offset);
	}

	*(uint8_t*)x86Ptr = 0xC3;
	x86Ptr += sizeof(uint8_t);

	return retval;
}

static void _DynGen_Dispatchers(void)
{
	PageProtectionMode mode;
	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	// In case init gets called multiple times:
	HostSys::MemProtect(iopRecDispatchers, __pagesize, mode);

	// clear the buffer to 0xcc (easier debugging).
	memset(iopRecDispatchers, 0xcc, __pagesize);

	x86Ptr = (uint8_t*)iopRecDispatchers;

	// Place the EventTest and DispatcherReg stuff at the top, because they get called the
	// most and stand to benefit from strong alignment and direct referencing.
	iopDispatcherEvent = x86Ptr;
	xFastCall((const void*)recEventTest);
	iopDispatcherReg = _DynGen_DispatcherReg();

	iopJITCompile = _DynGen_JITCompile();
	iopEnterRecompiledCode = _DynGen_EnterRecompiledCode();

	mode.m_write = false;
	mode.m_exec  = true;
	HostSys::MemProtect(iopRecDispatchers, __pagesize, mode);

	recBlocks.SetJITCompile(iopJITCompile);
}

////////////////////////////////////////////////////
using namespace R3000A;

////////////////////
// Code Templates //
////////////////////

// rd = rs op rt
void psxRecompileCodeConst0(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_)
		return;

	if (PSX_IS_CONST2(_Rs_, _Rt_))
	{
		_deletePSXtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const bool s_is_const = PSX_IS_CONST1(_Rs_);
	const bool t_is_const = PSX_IS_CONST1(_Rt_);
	const bool d_is_const = PSX_IS_CONST1(_Rd_);
	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const bool t_is_used = EEINST_USEDTEST(_Rt_);

	if (!s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if (!t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if (!d_is_const)
		_addNeededGPRtoX86reg(_Rd_);

	uint32_t info = 0;
	int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs < 0 && ((!s_is_const && s_is_used) || _Rs_ == _Rd_))
		regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt < 0 && ((!t_is_const && t_is_used) || _Rt_ == _Rd_))
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	// If S is no longer live, swap D for S. Saves the move.
	int regd = psxTryRenameReg(_Rd_, _Rs_, regs, _Rt_, xmminfo);
	if (regd < 0)
	{
		// TODO: If not live, write direct to memory.
		regd = _allocX86reg(X86TYPE_PSX, _Rd_, MODE_WRITE);
	}
	if (regd >= 0)
		info |= PROCESS_EE_SET_D(regd);

	if (s_is_const && regs < 0)
	{
		// This *must* go inside the if, because of when _Rs_ =  _Rd_
		PSX_DEL_CONST(_Rd_);
		constscode(info /*| PROCESS_CONSTS*/);
		return;
	}

	if (t_is_const && regt < 0)
	{
		PSX_DEL_CONST(_Rd_);
		consttcode(info /*| PROCESS_CONSTT*/);
		return;
	}

	PSX_DEL_CONST(_Rd_);
	noconstcode(info);
}

static void psxRecompileIrxImport(void)
{
	uint32_t import_table     = irxImportTableAddr(psxpc - 4);
	uint16_t index            = psxRegs.code & 0xffff;
	if (!import_table)
		return;

	const std::string libname = iopMemReadString(import_table + 12, 8);
	irxHLE hle                = irxImportHLE(libname, index);

	if (!hle)
		return;

	xMOV(ptr32[&psxRegs.code], psxRegs.code);
	xMOV(ptr32[&psxRegs.pc], psxpc);
	_psxFlushCall(FLUSH_NODESTROY);

	if (hle)
	{
		xFastCall((const void*)hle);
		xTEST(eax, eax);
		xJNZ(iopDispatcherReg);
	}
}

// rt = rs op imm16
void psxRecompileCodeConst1(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rt_)
	{
		// check for iop module import table magic
		if (psxRegs.code >> 16 == 0x2400)
			psxRecompileIrxImport();
		return;
	}

	if (PSX_IS_CONST1(_Rs_))
	{
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rt_);
		constcode();
		return;
	}

	_addNeededPSXtoX86reg(_Rs_);
	_addNeededPSXtoX86reg(_Rt_);

	uint32_t info = 0;

	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const int regs = s_is_used ? _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ) : _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	int regt = psxTryRenameReg(_Rt_, _Rs_, regs, 0, xmminfo);
	if (regt < 0)
	{
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
	}
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	PSX_DEL_CONST(_Rt_);
	noconstcode(info);
}

// rd = rt op sa
void psxRecompileCodeConst2(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo)
{
	if (!_Rd_)
		return;

	if (PSX_IS_CONST1(_Rt_))
	{
		_deletePSXtoX86reg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		constcode();
		return;
	}

	_addNeededPSXtoX86reg(_Rt_);
	_addNeededPSXtoX86reg(_Rd_);

	uint32_t info = 0;
	const bool s_is_used = EEINST_USEDTEST(_Rt_);
	const int regt = s_is_used ? _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ) : _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	int regd = psxTryRenameReg(_Rd_, _Rt_, regt, 0, xmminfo);
	if (regd < 0)
		regd = _allocX86reg(X86TYPE_PSX, _Rd_, MODE_WRITE);
	if (regd >= 0)
		info |= PROCESS_EE_SET_D(regd);

	PSX_DEL_CONST(_Rd_);
	noconstcode(info);
}

// rd = rt MULT rs  (SPECIAL)
void psxRecompileCodeConst3(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int LOHI)
{
	if (PSX_IS_CONST2(_Rs_, _Rt_))
	{
		if (LOHI)
		{
			_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);
			_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
		}

		constcode();
		return;
	}

	// we have to put these up here, because the register allocator below will wipe out const flags
	// for the destination register when/if it switches it to write mode.
	const bool s_is_const = PSX_IS_CONST1(_Rs_);
	const bool t_is_const = PSX_IS_CONST1(_Rt_);
	const bool s_is_used = EEINST_USEDTEST(_Rs_);
	const bool t_is_used = EEINST_USEDTEST(_Rt_);

	if (!s_is_const)
		_addNeededGPRtoX86reg(_Rs_);
	if (!t_is_const)
		_addNeededGPRtoX86reg(_Rt_);
	if (LOHI)
	{
		if (EEINST_LIVETEST(PSX_LO))
			_addNeededPSXtoX86reg(PSX_LO);
		if (EEINST_LIVETEST(PSX_HI))
			_addNeededPSXtoX86reg(PSX_HI);
	}

	uint32_t info = 0;
	int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs < 0 && !s_is_const && s_is_used)
		regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0)
		info |= PROCESS_EE_SET_S(regs);

	// need at least one in a register
	int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regs < 0 || (regt < 0 && !t_is_const && t_is_used))
		regt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	if (regt >= 0)
		info |= PROCESS_EE_SET_T(regt);

	if (LOHI)
	{
		// going to destroy lo/hi, so invalidate if we're writing it back to state
		const bool lo_is_used = EEINST_USEDTEST(PSX_LO);
		const int reglo = lo_is_used ? _allocX86reg(X86TYPE_PSX, PSX_LO, MODE_WRITE) : -1;
		if (reglo >= 0)
			info |= PROCESS_EE_SET_LO(reglo) | PROCESS_EE_LO;
		else
			_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

		const bool hi_is_live = EEINST_USEDTEST(PSX_HI);
		const int reghi = hi_is_live ? _allocX86reg(X86TYPE_PSX, PSX_HI, MODE_WRITE) : -1;
		if (reghi >= 0)
			info |= PROCESS_EE_SET_HI(reghi) | PROCESS_EE_HI;
		else
			_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	}

	if (s_is_const && regs < 0)
	{
		// This *must* go inside the if, because of when _Rs_ =  _Rd_
		constscode(info);
		return;
	}

	if (t_is_const && regt < 0)
	{
		consttcode(info);
		return;
	}

	noconstcode(info);
}

static void recReserve(void)
{
	if (recMem)
		return;

	/* R3000A Recompiler Cache */
	recMem = new RecompiledCodeReserve();
	recMem->Assign(GetVmMemory().CodeMemory(), HostMemoryMap::IOPrecOffset, 32 * _1mb);
}

static void recAlloc(void)
{
	// Goal: Allocate BASEBLOCKs for every possible branch target in IOP memory.
	// Any 4-byte aligned address makes a valid branch target as per MIPS design (all instructions are
	// always 4 bytes long).

	// We're on 64-bit, if these memory allocations fail, we're in real trouble.
	if (!m_recBlockAlloc)
		m_recBlockAlloc = (uint8_t*)_aligned_malloc(m_recBlockAllocSize, 4096);

	uint8_t* curpos = m_recBlockAlloc;
	recRAM = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::IopRam / 4) * sizeof(BASEBLOCK);
	recROM = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom / 4) * sizeof(BASEBLOCK);
	recROM1 = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom1 / 4) * sizeof(BASEBLOCK);
	recROM2 = (BASEBLOCK*)curpos;
	curpos += (Ps2MemSize::Rom2 / 4) * sizeof(BASEBLOCK);


	if (!s_pInstCache)
	{
		s_nInstCacheSize = 128;
		s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	}

	_DynGen_Dispatchers();
}

void recResetIOP(void)
{
	recAlloc();
	recMem->Reset();

	iopClearRecLUT((BASEBLOCK*)m_recBlockAlloc,
		(((Ps2MemSize::IopRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) / 4)));

	for (int i = 0; i < 0x10000; i++)
		recLUT_SetPage(psxRecLUT, 0, 0, 0, i, 0);

	// IOP knows 64k pages, hence for the 0x10000's

	// The bottom 2 bits of PC are always zero, so we <<14 to "compress"
	// the pc indexer into it's lower common denominator.

	// We're only mapping 20 pages here in 4 places.
	// 0x80 comes from : (Ps2MemSize::IopRam / 0x10000) * 4

	for (int i = 0; i < 0x80; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0x0000, i, i & 0x1f);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0x8000, i, i & 0x1f);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recRAM, 0xa000, i, i & 0x1f);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}

	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e48; i++)
	{
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(psxRecLUT, psxhwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	recBlocks.Reset();
	g_psxMaxRecMem = 0;

	recPtr = *recMem;
	psxbranch = 0;
}

static void recShutdown(void)
{
	delete recMem;
	recMem = NULL;

	safe_aligned_free(m_recBlockAlloc);

	if (s_pInstCache)
		free(s_pInstCache);
	s_pInstCache     = NULL;
	s_nInstCacheSize = 0;
}

static void iopClearRecLUT(BASEBLOCK* base, int count)
{
	for (int i = 0; i < count; i++)
		base[i].m_pFnptr = ((uintptr_t)iopJITCompile);
}

static __noinline s32 recExecuteBlock(s32 eeCycles)
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;

	// [TODO] recExecuteBlock could be replaced by a direct call to the iopEnterRecompiledCode()
	//   (by assigning its address to the psxRec structure).  But for that to happen, we need
	//   to move iopBreak/iopCycleEE update code to emitted assembly code. >_<  --air

	// Likely Disasm, as borrowed from MSVC:

	// Entry:
	// 	mov         eax,dword ptr [esp+4]
	// 	mov         dword ptr [iopBreak (0E88DCCh)],0
	// 	mov         dword ptr [iopCycleEE (832A84h)],eax

	// Exit:
	// 	mov         ecx,dword ptr [iopBreak (0E88DCCh)]
	// 	mov         edx,dword ptr [iopCycleEE (832A84h)]
	// 	lea         eax,[edx+ecx]

	((void(*)())iopEnterRecompiledCode)();

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

// Returns the offset to the next instruction after any cleared memory
static __fi uint32_t psxRecClearMem(uint32_t pc)
{
	BASEBLOCK* pblock = PSX_GETBLOCK(pc);
	if (pblock->m_pFnptr == (uintptr_t)iopJITCompile)
		return 4;

	pc = HWADDR(pc);

	uint32_t lowerextent = pc, upperextent = pc + 4;
	int blockidx = recBlocks.Index(pc);

	while (BASEBLOCKEX* pexblock = recBlocks[blockidx - 1])
	{
		if (pexblock->startpc + pexblock->size * 4 <= lowerextent)
			break;

		lowerextent = std::min(lowerextent, pexblock->startpc);
		blockidx--;
	}

	int toRemoveFirst = blockidx;

	while (BASEBLOCKEX* pexblock = recBlocks[blockidx])
	{
		if (pexblock->startpc >= upperextent)
			break;

		lowerextent = std::min(lowerextent, pexblock->startpc);
		upperextent = std::max(upperextent, pexblock->startpc + pexblock->size * 4);

		blockidx++;
	}

	if (toRemoveFirst != blockidx)
	{
		recBlocks.Remove(toRemoveFirst, (blockidx - 1));
	}

	iopClearRecLUT(PSX_GETBLOCK(lowerextent), (upperextent - lowerextent) / 4);

	return upperextent - pc;
}

static __fi void recClearIOP(uint32_t Addr, uint32_t Size)
{
	uint32_t pc = Addr;
	while (pc < Addr + Size * 4)
		pc += PSXREC_CLEARM(pc);
}

static void iPsxBranchTest(uint32_t newpc, uint32_t cpuBranch)
{
	uint32_t blockCycles = s_psxBlockCycles;

	xMOV(eax, ptr32[&psxRegs.cycle]);

	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo)
	{
		xMOV(ecx, eax);
		xMOV(edx, ptr32[&psxRegs.iopCycleEE]);
		xADD(edx, 7);
		xSHR(edx, 3);
		xADD(eax, edx);
		xCMP(eax, ptr32[&psxRegs.iopNextEventCycle]);
		xCMOVNS(eax, ptr32[&psxRegs.iopNextEventCycle]);
		xMOV(ptr32[&psxRegs.cycle], eax);
		xSUB(eax, ecx);
		xSHL(eax, 3);
		xSUB(ptr32[&psxRegs.iopCycleEE], eax);
		xJLE(iopExitRecompiledCode);

		xFastCall((const void*)iopEventTest);

		if (newpc != 0xffffffff)
		{
			xCMP(ptr32[&psxRegs.pc], newpc);
			xJNE(iopDispatcherReg);
		}
	}
	else
	{
		xForwardJS<uint8_t> nointerruptpending;
		xADD(eax, blockCycles);
		xMOV(ptr32[&psxRegs.cycle], eax); // update cycles

		// jump if iopCycleEE <= 0  (iop's timeslice timed out, so time to return control to the EE)
		xSUB(ptr32[&psxRegs.iopCycleEE], blockCycles * 8);
		xJLE(iopExitRecompiledCode);

		// check if an event is pending
		xSUB(eax, ptr32[&psxRegs.iopNextEventCycle]);

		xFastCall((const void*)iopEventTest);

		if (newpc != 0xffffffff)
		{
			xCMP(ptr32[&psxRegs.pc], newpc);
			xJNE(iopDispatcherReg);
		}

		nointerruptpending.SetTarget();
	}
}

static void psxRecompileNextInstruction(bool delayslot, bool swapped_delayslot)
{
	const int old_code = psxRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;
	s_recompilingDelaySlot = delayslot;

	psxRegs.code = iopMemRead32(psxpc);
	s_psxBlockCycles++;
	psxpc += 4;

	g_pCurInstInfo++;

	g_iopCyclePenalty = 0;
	rpsxBSC[psxRegs.code >> 26]();
	s_psxBlockCycles += g_iopCyclePenalty;

	if (swapped_delayslot)
	{
		psxRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}
	else
	{
		_clearNeededX86regs();
	}
}

static void iopRecRecompile(const uint32_t startpc)
{
	uint32_t i;
	uint32_t willbranch3 = 0;

	// Inject IRX hack
	if (startpc == 0x1630 && EmuConfig.CurrentIRX.length() > 3)
	{
		// FIXME do I need to increase the module count (0x1F -> 0x20)
		if (iopMemRead32(0x20018) == 0x1F)
			iopMemWrite32(0x20094, 0xbffc0000);
	}

	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= (recMem->GetPtrEnd() - _64kb))
		recResetIOP();

	x86Ptr      = (uint8_t*)recPtr;
	recPtr      = x86Ptr;

	s_pCurBlock = PSX_GETBLOCK(startpc);

	s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));

	if (!s_pCurBlockEx || s_pCurBlockEx->startpc != HWADDR(startpc))
		s_pCurBlockEx = recBlocks.New(HWADDR(startpc), (uintptr_t)recPtr);

	psxbranch = 0;

	s_pCurBlock->m_pFnptr = ((uintptr_t)x86Ptr);
	s_psxBlockCycles = 0;

	// reset recomp state variables
	psxpc = startpc;
	g_psxHasConstReg = g_psxFlushedConstReg = 1;

	_initX86regs();

	if ((psxHu32(HW_ICFG) & 8) && (HWADDR(startpc) == 0xa0 || HWADDR(startpc) == 0xb0 || HWADDR(startpc) == 0xc0))
	{
		xFastCall((const void*)psxBiosCall);
		xTEST(al, al);
		xJNZ(iopDispatcherReg);
	}

	// go until the next branch
	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	for (;;)
	{
		psxRegs.code = iopMemRead32(i);

		switch (psxRegs.code >> 26)
		{
			case 0: // special
				if (_Funct_ == 8 || _Funct_ == 9)
				{ // JR, JALR
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				break;

			case 1: // regimm
				if (_Rt_ == 0 || _Rt_ == 1 || _Rt_ == 16 || _Rt_ == 17)
				{
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
				s_branchTo = _Imm_ * 4 + i + 4;
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;
				goto StartRecomp;
		}

		i += 4;
	}

StartRecomp:

	s_nBlockFF = false;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = true;
		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i != s_nEndBlock - 8)
			{
				switch (iopMemRead32(i))
				{
					case 0: // nop
						break;
					default:
						s_nBlockFF = false;
				}
			}
		}
	}

	// rec info //
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
			psxRegs.code = iopMemRead32(i - 4);
			pcur[-1] = pcur[0];
			rpsxpropBSC(pcur - 1, pcur);
			pcur--;
		}
	}

	g_pCurInstInfo = s_pInstCache;
	while (!psxbranch && psxpc < s_nEndBlock)
		psxRecompileNextInstruction(false, false);

	s_pCurBlockEx->size = (psxpc - startpc) >> 2;

	if (!(psxpc & 0x10000000))
		g_psxMaxRecMem = std::max((psxpc & ~0xa0000000), g_psxMaxRecMem);

	if (psxbranch == 2)
	{
		_psxFlushCall(FLUSH_EVERYTHING);

		iPsxBranchTest(0xffffffff, 1);

		JMP32((uintptr_t)iopDispatcherReg - ((uintptr_t)x86Ptr + 5));
	}
	else
	{
		if (!psxbranch)
		{
			xADD(ptr32[&psxRegs.cycle], psxScaleBlockCycles());
			xSUB(ptr32[&psxRegs.iopCycleEE], psxScaleBlockCycles() * 8);
		}

		if (willbranch3 || !psxbranch)
		{
			_psxFlushCall(FLUSH_EVERYTHING);
			xMOV(ptr32[&psxRegs.pc], psxpc);
			recBlocks.Link(HWADDR(s_nEndBlock), xJcc32(Jcc_Unconditional, 0));
			psxbranch = 3;
		}
	}

	s_pCurBlockEx->x86size = x86Ptr - recPtr;

	recPtr        = x86Ptr;

	s_pCurBlock   = NULL;
	s_pCurBlockEx = NULL;
}

R3000Acpu psxRec = {
	recReserve,
	recResetIOP,
	recExecuteBlock,
	recClearIOP,
	recShutdown,
};
