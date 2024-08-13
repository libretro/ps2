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

#include "common/VectorIntrin.h"

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "iR5900.h"
#include "iFPU.h"

using namespace x86Emitter;

alignas(16) const u32 g_minvals[4] = {0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff};
alignas(16) const u32 g_maxvals[4] = {0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff};

//------------------------------------------------------------------
namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP1 {

namespace DOUBLE
{

	void recABS_S_xmm(int info);
	void recADD_S_xmm(int info);
	void recADDA_S_xmm(int info);
	void recC_EQ_xmm(int info);
	void recC_LE_xmm(int info);
	void recC_LT_xmm(int info);
	void recCVT_S_xmm(int info);
	void recCVT_W();
	void recDIV_S_xmm(int info);
	void recMADD_S_xmm(int info);
	void recMADDA_S_xmm(int info);
	void recMAX_S_xmm(int info);
	void recMIN_S_xmm(int info);
	void recMOV_S_xmm(int info);
	void recMSUB_S_xmm(int info);
	void recMSUBA_S_xmm(int info);
	void recMUL_S_xmm(int info);
	void recMULA_S_xmm(int info);
	void recNEG_S_xmm(int info);
	void recSUB_S_xmm(int info);
	void recSUBA_S_xmm(int info);
	void recSQRT_S_xmm(int info);
	void recRSQRT_S_xmm(int info);

}; // namespace DOUBLE

//------------------------------------------------------------------
// Helper Macros
//------------------------------------------------------------------
#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

// FCR31 Flags
#define FPUflagC  0x00800000
#define FPUflagI  0x00020000
#define FPUflagD  0x00010000
#define FPUflagO  0x00008000
#define FPUflagU  0x00004000
#define FPUflagSI 0x00000040
#define FPUflagSD 0x00000020
#define FPUflagSO 0x00000010
#define FPUflagSU 0x00000008

// Add/Sub opcodes produce the same results as the ps2
#define FPU_CORRECT_ADD_SUB 1

alignas(16) static const u32 s_neg[4] = {0x80000000, 0xffffffff, 0xffffffff, 0xffffffff};
alignas(16) static const u32 s_pos[4] = {0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff};

#define REC_FPUBRANCH(f) \
	void f(); \
	void rec##f() \
	{ \
		iFlushCall(FLUSH_INTERPRETER); \
		xFastCall((void*)(uptr)R5900::Interpreter::OpcodeImpl::COP1::f); \
		g_branch = 2; \
	}

#define REC_FPUFUNC(f) \
	void f(); \
	void rec##f() \
	{ \
		iFlushCall(FLUSH_INTERPRETER); \
		xFastCall((void*)(uptr)R5900::Interpreter::OpcodeImpl::COP1::f); \
	}
//------------------------------------------------------------------

//------------------------------------------------------------------
// *FPU Opcodes!*
//------------------------------------------------------------------

// Those opcode are marked as special ! But I don't understand why we can't run them in the interpreter
#ifndef FPU_RECOMPILE

REC_FPUFUNC(CFC1);
REC_FPUFUNC(CTC1);
REC_FPUFUNC(MFC1);
REC_FPUFUNC(MTC1);

#else

//------------------------------------------------------------------
// CFC1 / CTC1
//------------------------------------------------------------------
void recCFC1(void)
{
	if (!_Rt_)
		return;

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
	if (_Fs_ >= 16)
	{
		xMOV(xRegister32(regt), ptr32[&fpuRegs.fprc[31]]);
		xAND(xRegister32(regt), 0x0083c078); //remove always-zero bits
		xOR(xRegister32(regt), 0x01000001); //set always-one bits
		xMOVSX(xRegister64(regt), xRegister32(regt));
	}
	else
	{
		xMOVSX(xRegister64(regt), ptr32[&fpuRegs.fprc[0]]);
	}
}

void recCTC1(void)
{
	if (_Fs_ != 31)
		return;

	if (GPR_IS_CONST1(_Rt_))
	{
		xMOV(ptr32[&fpuRegs.fprc[_Fs_]], g_cpuConstRegs[_Rt_].UL[0]);
	}
	else
	{
		int mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);

		if (mmreg >= 0)
		{
			xMOVSS(ptr[&fpuRegs.fprc[_Fs_]], xRegisterSSE(mmreg));
		}
		else if ((mmreg = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ)) >= 0)
		{
			xMOV(ptr32[&fpuRegs.fprc[_Fs_]], xRegister32(mmreg));
		}
		else
		{
			_deleteGPRtoXMMreg(_Rt_, 1);

			xMOV(eax, ptr[&cpuRegs.GPR.r[_Rt_].UL[0]]);
			xMOV(ptr[&fpuRegs.fprc[_Fs_]], eax);
		}
	}
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// MFC1
//------------------------------------------------------------------

void recMFC1(void)
{
	if (!_Rt_)
		return;

	const int xmmregt = _allocIfUsedGPRtoXMM(_Rt_, MODE_READ | MODE_WRITE);
	const int regs = _allocIfUsedFPUtoXMM(_Fs_, MODE_READ);
	if (regs >= 0 && xmmregt >= 0)
	{
		// both in xmm, sign extend and insert lower bits
		const int temp = _allocTempXMMreg(XMMT_FPS);
		xMOVAPS(xRegisterSSE(temp), xRegisterSSE(regs));
		xPSRA.D(xRegisterSSE(temp), 31);
		xMOVSS(xRegisterSSE(xmmregt), xRegisterSSE(regs));
		xINSERTPS(xRegisterSSE(xmmregt), xRegisterSSE(temp), _MM_MK_INSERTPS_NDX(0, 1, 0));
		_freeXMMreg(temp);
		return;
	}

	// storing to a gpr..
	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);

	if (regs >= 0)
	{
		// xmm -> gpr
		xMOVD(xRegister32(regt), xRegisterSSE(regs));
		xMOVSX(xRegister64(regt), xRegister32(regt));
	}
	else
	{
		// mem -> gpr
		xMOVSX(xRegister64(regt), ptr32[&fpuRegs.fpr[_Fs_].UL]);
	}
}

//------------------------------------------------------------------


//------------------------------------------------------------------
// MTC1
//------------------------------------------------------------------
void recMTC1(void)
{
	if (GPR_IS_CONST1(_Rt_))
	{
		const int xmmreg = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);
		if (xmmreg >= 0)
		{
			// common case: mtc1 zero, fnn
			if (g_cpuConstRegs[_Rt_].UL[0] == 0)
			{
				xPXOR(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
			}
			else
			{
				// may as well flush the constant register, since we're needing it in a gpr anyway
				const int x86reg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
				xMOVDZX(xRegisterSSE(xmmreg), xRegister32(x86reg));
			}
		}
		else
		{
			xMOV(ptr32[&fpuRegs.fpr[_Fs_].UL], g_cpuConstRegs[_Rt_].UL[0]);
		}
	}
	else
	{
		const int xmmgpr = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
		if (xmmgpr >= 0)
		{
			if (g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE)
			{
				// transfer the reg directly
				_deleteFPtoXMMreg(_Fs_, DELETE_REG_FREE_NO_WRITEBACK);
				_reallocateXMMreg(xmmgpr, XMMTYPE_FPREG, _Fs_, MODE_WRITE);
			}
			else
			{
				const int xmmreg2 = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);
				if (xmmreg2 >= 0)
					xMOVSS(xRegisterSSE(xmmreg2), xRegisterSSE(xmmgpr));
				else
					xMOVSS(ptr[&fpuRegs.fpr[_Fs_].UL], xRegisterSSE(xmmgpr));
			}
		}
		else
		{
			// may as well cache it..
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
			const int mmreg2 = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);

			if (mmreg2 >= 0)
			{
				xMOVDZX(xRegisterSSE(mmreg2), xRegister32(regt));
			}
			else
			{
				xMOV(ptr32[&fpuRegs.fpr[_Fs_].UL], xRegister32(regt));
			}
		}
	}
}
#endif
//------------------------------------------------------------------


#ifndef FPU_RECOMPILE // If FPU_RECOMPILE is not defined, then use the interpreter opcodes. (CFC1, CTC1, MFC1, and MTC1 are special because they work specifically with the EE rec so they're defined above)

REC_FPUFUNC(ABS_S);
REC_FPUFUNC(ADD_S);
REC_FPUFUNC(ADDA_S);
REC_FPUBRANCH(BC1F);
REC_FPUBRANCH(BC1T);
REC_FPUBRANCH(BC1FL);
REC_FPUBRANCH(BC1TL);
REC_FPUFUNC(C_EQ);
REC_FPUFUNC(C_F);
REC_FPUFUNC(C_LE);
REC_FPUFUNC(C_LT);
REC_FPUFUNC(CVT_S);
REC_FPUFUNC(CVT_W);
REC_FPUFUNC(DIV_S);
REC_FPUFUNC(MAX_S);
REC_FPUFUNC(MIN_S);
REC_FPUFUNC(MADD_S);
REC_FPUFUNC(MADDA_S);
REC_FPUFUNC(MOV_S);
REC_FPUFUNC(MSUB_S);
REC_FPUFUNC(MSUBA_S);
REC_FPUFUNC(MUL_S);
REC_FPUFUNC(MULA_S);
REC_FPUFUNC(NEG_S);
REC_FPUFUNC(SUB_S);
REC_FPUFUNC(SUBA_S);
REC_FPUFUNC(SQRT_S);
REC_FPUFUNC(RSQRT_S);

#else // FPU_RECOMPILE

//------------------------------------------------------------------
// Clamp Functions (Converts NaN's and Infinities to Normal Numbers)
//------------------------------------------------------------------

static int fpuCopyToTempForClamp(int fpureg, int xmmreg)
{
	if (FPUINST_USEDTEST(fpureg))
	{
		const int tempreg = _allocTempXMMreg(XMMT_FPS);
		xMOVSS(xRegisterSSE(tempreg), xRegisterSSE(xmmreg));
		return tempreg;
	}

	// flush back the original value, before we mess with it below
	if (FPUINST_LIVETEST(fpureg))
		_flushXMMreg(xmmreg);

	// turn it into a temp, so in case the liveness was incorrect, we don't reuse it after clamp
	_reallocateXMMreg(xmmreg, XMMTYPE_TEMP, 0, 0, true);
	return xmmreg;
}

static void fpuFreeIfTemp(int xmmreg)
{
	if (xmmregs[xmmreg].inuse && xmmregs[xmmreg].type == XMMTYPE_TEMP)
		_freeXMMreg(xmmreg);
}

static __fi void fpuFloat3(int regd) // +NaN -> +fMax, -NaN -> -fMax, +Inf -> +fMax, -Inf -> -fMax
{
	/* SSE4 codepath */
#if _M_SSE >= 0x401
	xPMIN.SD(xRegisterSSE(regd), ptr128[&g_maxvals[0]]);
	xPMIN.UD(xRegisterSSE(regd), ptr128[&g_minvals[0]]);
	/* SSE2 codepath */
#elif _M_SSE >= 0x200
	const int t1reg = _allocTempXMMreg(XMMT_FPS);
	xMOVSS(xRegisterSSE(t1reg), xRegisterSSE(regd));
	xAND.PS(xRegisterSSE(t1reg), ptr[&s_neg[0]]);
	xMIN.SS(xRegisterSSE(regd), ptr[&g_maxvals[0]]);
	xMAX.SS(xRegisterSSE(regd), ptr[&g_minvals[0]]);
	xOR.PS(xRegisterSSE(regd), xRegisterSSE(t1reg));
	_freeXMMreg(t1reg);
#endif
}

static __fi void fpuFloat(int regd) // +/-NaN -> +fMax, +Inf -> +fMax, -Inf -> -fMax
{
	if (CHECK_FPU_OVERFLOW)
	{
		xMIN.SS(xRegisterSSE(regd), ptr[&g_maxvals[0]]); // MIN() must be before MAX()! So that NaN's become +Maximum
		xMAX.SS(xRegisterSSE(regd), ptr[&g_minvals[0]]);
	}
}

static __fi void fpuFloat2(int regd) // +NaN -> +fMax, -NaN -> -fMax, +Inf -> +fMax, -Inf -> -fMax
{
	if (CHECK_FPU_OVERFLOW)
	{
		fpuFloat3(regd);
	}
}

//------------------------------------------------------------------


//------------------------------------------------------------------
// ABS XMM
//------------------------------------------------------------------
void recABS_S_xmm(int info)
{
	if (info & PROCESS_EE_S)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);

	xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_pos[0]]);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags

	if (CHECK_FPU_OVERFLOW) // Only need to do positive clamp, since EEREC_D is positive
		xMIN.SS(xRegisterSSE(EEREC_D), ptr[&g_maxvals[0]]);
}

FPURECOMPILE_CONSTCODE(ABS_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// FPU_ADD_SUB (Used to mimic PS2's FPU add/sub behavior)
//------------------------------------------------------------------
// Compliant IEEE FPU uses, in computations, uses additional "guard" bits to the right of the mantissa
// but EE-FPU doesn't. Substraction (and addition of positive and negative) may shift the mantissa left,
// causing those bits to appear in the result; this function masks out the bits of the mantissa that will
// get shifted right to the guard bits to ensure that the guard bits are empty.
// The difference of the exponents = the amount that the smaller operand will be shifted right by.
// Modification - the PS2 uses a single guard bit? (Coded by Nneeve)
//------------------------------------------------------------------
static void FPU_ADD_SUB(int regd, int regt, int issub)
{
	u8 *j8Ptr0, *j8Ptr1, *j8Ptr2, *j8Ptr3;
	u8 *j8Ptr4, *j8Ptr5, *j8Ptr6, *j8Ptr7;
	const int xmmtemp = _allocTempXMMreg(XMMT_FPS); //temporary for anding with regd/regt
	xMOVD(ecx, xRegisterSSE(regd)); // ecx receives regd
	xMOVD(eax, xRegisterSSE(regt)); // eax receives regt

	//mask the exponents
	xSHR(ecx, 23);
	xSHR(eax, 23);
	xAND(ecx, 0xff);
	xAND(eax, 0xff);

	xSUB(ecx, eax); //tempecx = exponent difference
	xCMP(ecx, 25);
	xWrite8(JGE8);
	xWrite8(0);
	j8Ptr0 = (u8*)(x86Ptr - 1);
	xCMP(ecx, 0);
	xWrite8(JG8);
	xWrite8(0);
	j8Ptr1 = (u8*)(x86Ptr - 1);
	xWrite8(JE8);
	xWrite8(0);
	j8Ptr2 = (u8*)(x86Ptr - 1);
	xCMP(ecx, -25);
	xWrite8(JBE8);
	xWrite8(0);
	j8Ptr3 = (u8*)(x86Ptr - 1);

	//diff = -24 .. -1 , expd < expt
	xNEG(ecx);
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl); //temp2 = 0xffffffff << tempecx
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr4   =  x86Ptr - 1;

	*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
	//diff = 25 .. 255 , expt < expd
	xMOVAPS(xRegisterSSE(xmmtemp), xRegisterSSE(regt));
	xAND.PS(xRegisterSSE(xmmtemp), ptr[s_neg]);
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr5   =  x86Ptr - 1;

	*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
	//diff = 1 .. 24, expt < expd
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl); //temp2 = 0xffffffff << tempecx
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(xmmtemp), xRegisterSSE(regt));
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(xmmtemp));
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr6   =  x86Ptr - 1;

	*j8Ptr3  = (u8)((x86Ptr - j8Ptr3) - 1);
	//diff = -255 .. -25, expd < expt
	xAND.PS(xRegisterSSE(regd), ptr[s_neg]);
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr7   =  x86Ptr - 1;

	*j8Ptr2  = (u8)((x86Ptr - j8Ptr2) - 1);
	//diff == 0
	if (issub)
		xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
	else
		xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));

	*j8Ptr4  = (u8)((x86Ptr - j8Ptr4) - 1);
	*j8Ptr5  = (u8)((x86Ptr - j8Ptr5) - 1);
	*j8Ptr6  = (u8)((x86Ptr - j8Ptr6) - 1);
	*j8Ptr7  = (u8)((x86Ptr - j8Ptr7) - 1);

	_freeXMMreg(xmmtemp);
}

#ifdef FPU_CORRECT_ADD_SUB
static void FPU_ADD_WRAP(int regd, int regt) { FPU_ADD_SUB(regd, regt, 0); }
#define FPU_ADD(a, b) FPU_ADD_SUB(a, b, 0)
#define FPU_SUB(a, b) FPU_ADD_SUB(a, b, 1)
#else
static void FPU_ADD(int regd, int regt)
{
	xADD.SS(xRegisterSSE(regd), xRegisterSSE(regt));
}

static void FPU_SUB(int regd, int regt)
{
	xSUB.SS(xRegisterSSE(regd), xRegisterSSE(regt));
}
#endif

//------------------------------------------------------------------
// Note: PS2's multiplication uses some variant of booth multiplication with wallace trees:
// It cuts off some bits, resulting in inaccurate and non-commutative results.
// The PS2's result mantissa is either equal to x86's rounding to zero result mantissa
// or SMALLER (by 0x1). (this means that x86's other rounding modes are only less similar to PS2's mul)
//------------------------------------------------------------------
static void FPU_MUL(int regd, int regt, bool reverseOperands)
{
	u8 *endMul = nullptr;

	if (CHECK_FPUMULHACK)
	{
		alignas(16) static constexpr const u32 result[4] = { 0x3f490fda };
		u8 *noHack;

		xMOVD(ecx, xRegisterSSE(reverseOperands ? regt : regd));
		xMOVD(edx, xRegisterSSE(reverseOperands ? regd : regt));

		// if (((s ^ 0x3e800000) | (t ^ 0x40490fdb)) != 0) { hack; }
		xXOR(ecx, 0x3e800000);
		xXOR(edx, 0x40490fdb);
		xOR(edx, ecx);

		xWrite8(JNZ8);
		xWrite8(0);
		noHack   = (u8*)(x86Ptr - 1);
		xMOVAPS(xRegisterSSE(regd), ptr128[result]);
		xWrite8(0xEB);
		xWrite8(0);
		endMul   =  x86Ptr - 1;
		*noHack  = (u8)((x86Ptr - noHack) - 1);
	}

	xMUL.SS(xRegisterSSE(regd), xRegisterSSE(regt));

	if (CHECK_FPUMULHACK)
		*endMul  = (u8)((x86Ptr - endMul) - 1);
}

static void FPU_MUL_WRAP(int regd, int regt) { FPU_MUL(regd, regt, false); }
static void FPU_MUL_WRAP_REV(int regd, int regt) { FPU_MUL(regd, regt, true); } //reversed operands

//------------------------------------------------------------------
// CommutativeOp XMM (used for ADD, MUL, MAX, and MIN opcodes)
//------------------------------------------------------------------
#ifdef FPU_CORRECT_ADD_SUB
static void (*recComOpXMM_to_XMM[])(int, int) = {
	FPU_ADD_WRAP, FPU_MUL_WRAP,     SSE_MAXSS_XMM_to_XMM, SSE_MINSS_XMM_to_XMM};
static void (*recComOpXMM_to_XMM_REV[])(int, int) = { //reversed operands
	FPU_ADD_WRAP, FPU_MUL_WRAP_REV, SSE_MAXSS_XMM_to_XMM, SSE_MINSS_XMM_to_XMM};
#else
static void (*recComOpXMM_to_XMM[])(int, int) = {
	FPU_ADD, FPU_MUL_WRAP,     SSE_MAXSS_XMM_to_XMM, SSE_MINSS_XMM_to_XMM};
static void (*recComOpXMM_to_XMM_REV[])(int, int) = { //reversed operands
	FPU_ADD, FPU_MUL_WRAP_REV, SSE_MAXSS_XMM_to_XMM, SSE_MINSS_XMM_to_XMM};
#endif

static int recCommutativeOp(int info, int regd, int op)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW /*&& !CHECK_FPUCLAMPHACK */ || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(t0reg);
				}
				recComOpXMM_to_XMM[op](regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_S);
				}
				recComOpXMM_to_XMM_REV[op](regd, EEREC_S);
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(t0reg);
				}
				recComOpXMM_to_XMM_REV[op](regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_T);
				}
				recComOpXMM_to_XMM[op](regd, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_S);
				}
				recComOpXMM_to_XMM_REV[op](regd, EEREC_S);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_T);
				}
				recComOpXMM_to_XMM[op](regd, EEREC_T);
			}
			break;
		default:
			xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
			{
				fpuFloat2(regd);
				fpuFloat2(t0reg);
			}
			recComOpXMM_to_XMM[op](regd, t0reg);
			break;
	}

	_freeXMMreg(t0reg);
	return regd;
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// ADD XMM
//------------------------------------------------------------------
void recADD_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_D, 0));
}

FPURECOMPILE_CONSTCODE(ADD_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recADDA_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_ACC, 0));
}

FPURECOMPILE_CONSTCODE(ADDA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------

//------------------------------------------------------------------
// BC1x XMM
//------------------------------------------------------------------

// COP1 branch conditionals are based on the following equation:
// (fpuRegs.fprc[31] & 0x00800000)
// BC2F checks if the statement is false, BC2T checks if the statement is true.
#define _setupBranchTest() \
	_eeFlushAllDirty(); \
	xMOV(eax, ptr[&fpuRegs.fprc[31]]); \
	xTEST(eax, FPUflagC)

void recBC1F(void)
{
	u32 *jmpskip;
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, true);
	_setupBranchTest();
	xWrite8(0x0F);
	xWrite8(JNZ32);
	xWrite32(0);
	jmpskip = (u32*)(x86Ptr - 4);
	recDoBranchImm(branchTo, jmpskip, false, swap);
}

void recBC1T(void)
{
	u32 *jmpskip;
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, true);
	_setupBranchTest();
	xWrite8(0x0F);
	xWrite8(JZ32);
	xWrite32(0);
	jmpskip = (u32*)(x86Ptr - 4);
	recDoBranchImm(branchTo, jmpskip, false, swap);
}

void recBC1FL(void)
{
	u32 *jmpskip;
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	xWrite8(0x0F);
	xWrite8(JNZ32);
	xWrite32(0);
	jmpskip = (u32*)(x86Ptr - 4);
	recDoBranchImm(branchTo, jmpskip, true, false);
}

void recBC1TL(void)
{
	u32 *jmpskip;
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	xWrite8(0x0F);
	xWrite8(JZ32);
	xWrite32(0);
	jmpskip = (u32*)(x86Ptr - 4);
	recDoBranchImm(branchTo, jmpskip, true, false);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// C.x.S XMM
//------------------------------------------------------------------
void recC_EQ_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			{
				const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
				fpuFloat3(regs);

				const int t0reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				fpuFloat3(t0reg);

				xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(t0reg));

				_freeXMMreg(t0reg);
				fpuFreeIfTemp(regs);
			}
			break;

		case PROCESS_EE_T:
			{
				const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
				fpuFloat3(regt);

				const int t0reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				fpuFloat3(t0reg);

				xUCOMI.SS(xRegisterSSE(t0reg), xRegisterSSE(regt));

				_freeXMMreg(t0reg);
				fpuFreeIfTemp(regt);
			}
			break;

		case (PROCESS_EE_S | PROCESS_EE_T):
			{
				const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
				fpuFloat3(regs);

				const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
				fpuFloat3(regt);

				xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(regt));

				fpuFreeIfTemp(regs);
				fpuFreeIfTemp(regt);
			}
			break;

		default:
			xMOV(eax, ptr[&fpuRegs.fpr[_Fs_]]);
			xCMP(eax, ptr[&fpuRegs.fpr[_Ft_]]);

			xWrite8(JZ8);
			xWrite8(0);
			j8Ptr0 = (u8*)(x86Ptr - 1);
			xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
			xWrite8(0xEB);
			xWrite8(0);
			j8Ptr1   =  x86Ptr - 1;
			*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
			*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
			return;
	}

	xWrite8(JZ8);
	xWrite8(0);
	j8Ptr0   = (u8*)(x86Ptr - 1);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr1   =  x86Ptr - 1;
	*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
}

FPURECOMPILE_CONSTCODE(C_EQ, XMMINFO_READS | XMMINFO_READT);

void recC_F()
{
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
}

void recC_LE_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(t0reg));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regs);
		}
		break;

		case PROCESS_EE_T:
		{
			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(t0reg), xRegisterSSE(regt));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regt);
		}
		break;

		case (PROCESS_EE_S | PROCESS_EE_T):
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(regt));

			fpuFreeIfTemp(regs);
			fpuFreeIfTemp(regt);
		}
		break;

		default: // Untested and incorrect, but this case is never reached AFAIK (cottonvibes)
			xMOV(eax, ptr[&fpuRegs.fpr[_Fs_]]);
			xCMP(eax, ptr[&fpuRegs.fpr[_Ft_]]);

			xWrite8(JBE8);
			xWrite8(0);
			j8Ptr0 = (u8*)(x86Ptr - 1);
			xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
			xWrite8(0xEB);
			xWrite8(0);
			j8Ptr1   =  x86Ptr - 1;
			*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
			*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
			return;
	}

	xWrite8(JBE8);
	xWrite8(0);
	j8Ptr0 = (u8*)(x86Ptr - 1);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr1   =  x86Ptr - 1;
	*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
}

FPURECOMPILE_CONSTCODE(C_LE, XMMINFO_READS | XMMINFO_READT);

void recC_LT_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(t0reg));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regs);
		}
		break;

		case PROCESS_EE_T:
		{
			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			const int t0reg = _allocTempXMMreg(XMMT_FPS);
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
			fpuFloat3(t0reg);

			xUCOMI.SS(xRegisterSSE(t0reg), xRegisterSSE(regt));

			_freeXMMreg(t0reg);
			fpuFreeIfTemp(regt);
		}
		break;

		case (PROCESS_EE_S | PROCESS_EE_T):
		{
			const int regs = fpuCopyToTempForClamp(_Fs_, EEREC_S);
			fpuFloat3(regs);

			const int regt = fpuCopyToTempForClamp(_Ft_, EEREC_T);
			fpuFloat3(regt);

			xUCOMI.SS(xRegisterSSE(regs), xRegisterSSE(regt));

			fpuFreeIfTemp(regs);
			fpuFreeIfTemp(regt);
		}
		break;

		default:
			xMOV(eax, ptr[&fpuRegs.fpr[_Fs_]]);
			xCMP(eax, ptr[&fpuRegs.fpr[_Ft_]]);

			xWrite8(JL8);
			xWrite8(0);
			j8Ptr0 = (u8*)(x86Ptr - 1);
			xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
			xWrite8(0xEB);
			xWrite8(0);
			j8Ptr1   =  x86Ptr - 1;
			*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
			*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
			return;
	}

	xWrite8(JB8);
	xWrite8(0);
	j8Ptr0 = (u8*)(x86Ptr - 1);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr1   =  x86Ptr - 1;
	*j8Ptr0  = (u8)((x86Ptr - j8Ptr0) - 1);
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	*j8Ptr1  = (u8)((x86Ptr - j8Ptr1) - 1);
}

FPURECOMPILE_CONSTCODE(C_LT, XMMINFO_READS | XMMINFO_READT);
//REC_FPUFUNC(C_LT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// CVT.x XMM
//------------------------------------------------------------------
void recCVT_S_xmm(int info)
{
	if (info & PROCESS_EE_D)
	{
		if (info & PROCESS_EE_S)
			xCVTDQ2PS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		else
			xCVTSI2SS(xRegisterSSE(EEREC_D), ptr32[&fpuRegs.fpr[_Fs_]]);
	}
	else
	{
		const int temp = _allocTempXMMreg(XMMT_FPS);
		xCVTSI2SS(xRegisterSSE(temp), ptr32[&fpuRegs.fpr[_Fs_]]);
		xMOVSS(ptr32[&fpuRegs.fpr[_Fd_]], xRegisterSSE(temp));
		_freeXMMreg(temp);
	}
}

FPURECOMPILE_CONSTCODE(CVT_S, XMMINFO_WRITED | XMMINFO_READS);

void recCVT_W(void)
{
	if (CHECK_FPU_FULL)
	{
		DOUBLE::recCVT_W();
		return;
	}

	int regs = _checkXMMreg(XMMTYPE_FPREG, _Fs_, MODE_READ);

	if (regs >= 0)
	{
		if (CHECK_FPU_EXTRA_OVERFLOW)
			fpuFloat2(regs);
		xCVTTSS2SI(eax, xRegisterSSE(regs));
		xMOVMSKPS(edx, xRegisterSSE(regs)); //extract the signs
		xAND(edx, 1); // keep only LSB
	}
	else
	{
		xCVTTSS2SI(eax, ptr32[&fpuRegs.fpr[_Fs_]]);
		xMOV(edx, ptr[&fpuRegs.fpr[_Fs_]]);
		xSHR(edx, 31); // mov sign to lsb
	}

	//kill register allocation for dst because we write directly to fpuRegs.fpr[_Fd_]
	_deleteFPtoXMMreg(_Fd_, DELETE_REG_FREE_NO_WRITEBACK);

	xADD(edx, 0x7FFFFFFF); // 0x7FFFFFFF if positive, 0x8000 0000 if negative

	xCMP(eax, 0x80000000); // If the result is indefinitive
	xCMOVE(eax, edx);      // Saturate it

	//Write the result
	xMOV(ptr[&fpuRegs.fpr[_Fd_]], eax);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// DIV XMM
//------------------------------------------------------------------
static void recDIVhelper1(int regd, int regt) // Sets flags
{
	u8 *pjmp1, *pjmp2;
	u32 *ajmp32, *bjmp32;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	/*--- Check for divide by zero ---*/
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regt));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regt == zero, sign will be set)
	xWrite8(0x0F);
	xWrite8(JZ32);
	xWrite32(0);
	ajmp32 = (u32*)(x86Ptr - 4); /* Skip if not set */

	/*--- Check for 0/0 ---*/
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regd == zero, sign will be set)
	xWrite8(JZ8);
	xWrite8(0);
	pjmp1  = (u8*)(x86Ptr - 1); // Skip if not set
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
	xWrite8(0xEB);
	xWrite8(0);
	pjmp2    =  x86Ptr - 1;
	*pjmp1   = (u8)((x86Ptr - pjmp1) - 1); //x/0 but not 0/0
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
	*pjmp2   = (u8)((x86Ptr - pjmp2) - 1);

	/*--- Make regd +/- Maximum ---*/
	xXOR.PS(xRegisterSSE(regd), xRegisterSSE(regt)); // Make regd Positive or Negative
	xAND.PS(xRegisterSSE(regd), ptr[&s_neg[0]]); // Get the sign bit
	xOR.PS(xRegisterSSE(regd), ptr[&g_maxvals[0]]); // regd = +/- Maximum
	//xMOVSSZX(xRegisterSSE(regd), ptr[&g_maxvals[0]]);
	xWrite8(0xE9);
	xWrite32(0);
	bjmp32  = (u32*)(x86Ptr - 4);

	*ajmp32 = (x86Ptr - (u8*)ajmp32) - 4;

	/*--- Normal Divide ---*/
	if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(regt); }
	xDIV.SS(xRegisterSSE(regd), xRegisterSSE(regt));

	fpuFloat(regd);
	*bjmp32 = (x86Ptr - (u8*)bjmp32) - 4;

	_freeXMMreg(t1reg);
}

alignas(16) static FPControlRegister roundmode_nearest, roundmode_neg;

void recDIV_S_xmm(int info)
{
	bool roundmodeFlag = false;
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUDivFPCR.bitmask]);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			// Sets D/I flags on FPU instructions
			recDIVhelper1(EEREC_D, t0reg);
			break;
		case PROCESS_EE_T:
			if (EEREC_D == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (EEREC_D == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, EEREC_T);
			}
			break;
		default:
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
			// Sets D/I flags on FPU instructions
			recDIVhelper1(EEREC_D, t0reg);
			break;
	}

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
	_freeXMMreg(t0reg);
}

FPURECOMPILE_CONSTCODE(DIV_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------



//------------------------------------------------------------------
// MADD XMM
//------------------------------------------------------------------
void recMADDtemp(int info, int regd)
{
	const int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_S); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_T); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_S)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		default:
			if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				const int t1reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t1reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(t1reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(t1reg));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
				_freeXMMreg(t1reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
	}

	fpuFloat(regd);
	_freeXMMreg(t0reg);
}

void recMADD_S_xmm(int info)
{
	recMADDtemp(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE(MADD_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMADDA_S_xmm(int info)
{
	recMADDtemp(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE(MADDA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MAX / MIN XMM
//------------------------------------------------------------------
void recMAX_S_xmm(int info)
{
	recCommutativeOp(info, EEREC_D, 2);
}

FPURECOMPILE_CONSTCODE(MAX_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMIN_S_xmm(int info)
{
	recCommutativeOp(info, EEREC_D, 3);
}

FPURECOMPILE_CONSTCODE(MIN_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MOV XMM
//------------------------------------------------------------------
void recMOV_S_xmm(int info)
{
	if (info & PROCESS_EE_S)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
}

FPURECOMPILE_CONSTCODE(MOV_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MSUB XMM
//------------------------------------------------------------------
void recMSUBtemp(int info, int regd)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_S); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_T); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_S)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(EEREC_T));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
		default:
			if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				const int t1reg = _allocTempXMMreg(XMMT_FPS);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t1reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(t1reg); }
				xMUL.SS(xRegisterSSE(t0reg), xRegisterSSE(t1reg));
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
				_freeXMMreg(t1reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xMUL.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));
				if (info & PROCESS_EE_ACC)
					xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_ACC));
				else
					xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.ACC]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(t0reg));
			}
			break;
	}

	fpuFloat(regd);
	_freeXMMreg(t0reg);
}

void recMSUB_S_xmm(int info)
{
	recMSUBtemp(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE(MSUB_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMSUBA_S_xmm(int info)
{
	recMSUBtemp(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE(MSUBA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MUL XMM
//------------------------------------------------------------------
void recMUL_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_D, 1));
}

FPURECOMPILE_CONSTCODE(MUL_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMULA_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_ACC, 1));
}

FPURECOMPILE_CONSTCODE(MULA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// NEG XMM
//------------------------------------------------------------------
void recNEG_S_xmm(int info)
{
	if (info & PROCESS_EE_S)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);

	xXOR.PS(xRegisterSSE(EEREC_D), ptr[&s_neg[0]]);

	// Always preserve sign. Using float clamping here would result in
	// +inf to become +fMax instead of -fMax, which is definitely wrong.
	fpuFloat3(EEREC_D);
}

FPURECOMPILE_CONSTCODE(NEG_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SUB XMM
//------------------------------------------------------------------
static void recSUBhelper(int regd, int regt)
{
	if (CHECK_FPU_EXTRA_OVERFLOW /*&& !CHECK_FPUCLAMPHACK*/) { fpuFloat2(regd); fpuFloat2(regt); }
	FPU_SUB(regd, regt);
}

static void recSUBop(int info, int regd)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd != EEREC_S)
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			recSUBhelper(regd, t0reg);
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				recSUBhelper(regd, t0reg);
			}
			else
			{
				xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
				recSUBhelper(regd, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_T)
			{
				xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				recSUBhelper(regd, t0reg);
			}
			else
			{
				xMOVSS(xRegisterSSE(regd), xRegisterSSE(EEREC_S));
				recSUBhelper(regd, EEREC_T);
			}
			break;
		default:
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			xMOVSSZX(xRegisterSSE(regd), ptr[&fpuRegs.fpr[_Fs_]]);
			recSUBhelper(regd, t0reg);
			break;
	}

	fpuFloat(regd);
	_freeXMMreg(t0reg);
}

void recSUB_S_xmm(int info)
{
	recSUBop(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE(SUB_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


void recSUBA_S_xmm(int info)
{
	recSUBop(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE(SUBA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SQRT XMM
//------------------------------------------------------------------
void recSQRT_S_xmm(int info)
{
	bool roundmodeFlag = false;

	if (EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::Nearest)
	{
		// Set roundmode to nearest if it isn't already
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest.SetRoundMode(FPRoundMode::Nearest);
		xLDMXCSR(ptr32[&roundmode_nearest.bitmask]);
		roundmodeFlag = true;
	}

	if (info & PROCESS_EE_T)
		xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	else
		xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Ft_]]);

	// Sets D/I flags on FPU instructions
	{
		u8 *pjmp;
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

		/*--- Check for negative SQRT ---*/
		xMOVMSKPS(eax, xRegisterSSE(EEREC_D));
		xAND(eax, 1); //Check sign
		xWrite8(JZ8);
		xWrite8(0);
		pjmp  = (u8*)(x86Ptr - 1); // Skip if none are
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags
		xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_pos[0]]); // Make EEREC_D Positive
		*pjmp   = (u8)((x86Ptr - pjmp) - 1);
	}

	if (CHECK_FPU_OVERFLOW) // Only need to do positive clamp, since EEREC_D is positive
		xMIN.SS(xRegisterSSE(EEREC_D), ptr[&g_maxvals[0]]);
	xSQRT.SS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
	if (CHECK_FPU_EXTRA_OVERFLOW) // Shouldn't need to clamp again since SQRT of a number will always be smaller than the original number, doing it just incase :/
		fpuFloat(EEREC_D);

	if (roundmodeFlag)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
}

FPURECOMPILE_CONSTCODE(SQRT_S, XMMINFO_WRITED | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// RSQRT XMM
//------------------------------------------------------------------
static void recRSQRThelper1(int regd, int t0reg) // Preforms the RSQRT function when regd <- Fs and t0reg <- Ft (Sets correct flags)
{
	u8 *pjmp1, *pjmp2;
	u32 *pjmp32;
	u8 *qjmp1, *qjmp2;
	int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	/*--- (first) Check for negative SQRT ---*/
	xMOVMSKPS(eax, xRegisterSSE(t0reg));
	xAND(eax, 1); //Check sign
	xWrite8(JZ8);
	xWrite8(0);
	pjmp2  = (u8*)(x86Ptr - 1); // Skip if not set
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags
	xAND.PS(xRegisterSSE(t0reg), ptr[&s_pos[0]]); // Make t0reg Positive
	*pjmp2   = (u8)((x86Ptr - pjmp2) - 1);

	/*--- Check for zero ---*/
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(t0reg));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if t0reg == zero, sign will be set)
	xWrite8(JZ8);
	xWrite8(0);
	pjmp1  = (u8*)(x86Ptr - 1); // Skip if not set
		
	/*--- Check for 0/0 ---*/
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regd == zero, sign will be set)
	xWrite8(JZ8);
	xWrite8(0);
	qjmp1  = (u8*)(x86Ptr - 1); // Skip if not set
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
	xWrite8(0xEB);
	xWrite8(0);
	qjmp2    =  x86Ptr - 1;
	*qjmp1   = (u8)((x86Ptr - qjmp1) - 1); //x/0 but not 0/0
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
	*qjmp2   = (u8)((x86Ptr - qjmp2) - 1);

	/*--- Make regd +/- Maximum ---*/
	xAND.PS(xRegisterSSE(regd), ptr[&s_neg[0]]); // Get the sign bit
	xOR.PS(xRegisterSSE(regd), ptr[&g_maxvals[0]]); // regd = +/- Maximum
	xWrite8(0xE9);
	xWrite32(0);
	pjmp32   = (u32*)(x86Ptr - 4);
	*pjmp1   = (u8)((x86Ptr - pjmp1) - 1);

	if (CHECK_FPU_EXTRA_OVERFLOW)
	{
		xMIN.SS(xRegisterSSE(t0reg), ptr[&g_maxvals[0]]); // Only need to do positive clamp, since t0reg is positive
		fpuFloat2(regd);
	}

	xSQRT.SS(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
	xDIV.SS(xRegisterSSE(regd), xRegisterSSE(t0reg));

	fpuFloat(regd);
	*pjmp32 = (x86Ptr - (u8*)pjmp32) - 4;

	_freeXMMreg(t1reg);
}

void recRSQRT_S_xmm(int info)
{
	// RSQRT doesn't change the round mode, because RSQRTSS ignores the rounding mode in MXCSR.
	const int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			break;
		case PROCESS_EE_T:
			xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
			xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			xMOVSS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
			xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			break;
		default:
			xMOVSSZX(xRegisterSSE(t0reg), ptr[&fpuRegs.fpr[_Ft_]]);
			xMOVSSZX(xRegisterSSE(EEREC_D), ptr[&fpuRegs.fpr[_Fs_]]);
			break;
	}
	// Sets D/I flags on FPU instructions
	recRSQRThelper1(EEREC_D, t0reg);
	_freeXMMreg(t0reg);
}

FPURECOMPILE_CONSTCODE(RSQRT_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

#endif // FPU_RECOMPILE

} // namespace COP1
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
