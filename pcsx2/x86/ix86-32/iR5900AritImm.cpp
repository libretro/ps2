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

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"

using namespace x86Emitter;

namespace R5900::Dynarec::OpcodeImpl
{
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

static void recMoveStoT(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister32(EEREC_T), xRegister32(EEREC_S));
	else
		xMOV(xRegister32(EEREC_T), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
}

static void recMoveStoT64(int info)
{
	if (info & PROCESS_EE_S)
		xMOV(xRegister64(EEREC_T), xRegister64(EEREC_S));
	else
		xMOV(xRegister64(EEREC_T), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
}

//// ADDI
static void recADDI_const(void)
{
	g_cpuConstRegs[_Rt_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] + u32(s32(_Imm_))));
}

static void recADDI_(int info)
{
	recMoveStoT(info);
	xADD(xRegister32(EEREC_T), _Imm_);
	xMOVSX(xRegister64(EEREC_T), xRegister32(EEREC_T));
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ADDI, XMMINFO_WRITET | XMMINFO_READS);

////////////////////////////////////////////////////
void recADDIU()
{
	recADDI();
}

////////////////////////////////////////////////////
static void recDADDI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + u64(s64(_Imm_));
}

static void recDADDI_(int info)
{
	recMoveStoT64(info);
	xADD(xRegister64(EEREC_T), _Imm_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, DADDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

//// DADDIU
void recDADDIU()
{
	recDADDI();
}

//// SLTIU
static void recSLTIU_const()
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

//// SLTI
static void recSLTI_const()
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

//// ANDI
static void recANDI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & (u64)_ImmU_; // Zero-extended Immediate
}

namespace
{
enum class LogicalOp
{
	AND,
	OR,
	XOR
};
} // namespace

static void recLogicalOpI(int info, LogicalOp op)
{
	xImpl_G1Logic bad{};
	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
		op == LogicalOp::XOR    ? xXOR :
		bad;
	if (_ImmU_ != 0)
	{
		recMoveStoT64(info);
		xOP(xRegister64(EEREC_T), _ImmU_);
	}
	else
	{
		if (op == LogicalOp::AND)
		{
			xXOR(xRegister32(EEREC_T), xRegister32(EEREC_T));
		}
		else
		{
			recMoveStoT64(info);
		}
	}
}

static void recANDI_(int info)
{
	recLogicalOpI(info, LogicalOp::AND);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ANDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

////////////////////////////////////////////////////
static void recORI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | (u64)_ImmU_; // Zero-extended Immediate
}

static void recORI_(int info)
{
	recLogicalOpI(info, LogicalOp::OR);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

////////////////////////////////////////////////////
static void recXORI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ (u64)_ImmU_; // Zero-extended Immediate
}

static void recXORI_(int info)
{
	recLogicalOpI(info, LogicalOp::XOR);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, XORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP);

#endif

} // namespace R5900::Dynarec::OpcodeImpl
