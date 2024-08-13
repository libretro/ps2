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
#include "common/emitter/x86emitter.h"
#include "iR5900.h"
#include "iFPU.h"

/* This is a version of the FPU that emulates an exponent of 0xff and overflow/underflow flags */

/* Can be made faster by not converting stuff back and forth between instructions. */

//----------------------------------------------------------------
// FPU emulation status:
// ADD, SUB (incl. accumulation stage of MADD/MSUB) - no known problems.
// Mul (incl. multiplication stage of MADD/MSUB) - incorrect. PS2's result mantissa is sometimes
//													smaller by 0x1 than IEEE's result (with round to zero).
// DIV, SQRT, RSQRT - incorrect. PS2's result varies between IEEE's result with round to zero
//													and IEEE's result with round to +/-infinity.
// other stuff - no known problems.
//----------------------------------------------------------------


using namespace x86Emitter;

#ifdef FPU_RECOMPILE

//------------------------------------------------------------------
namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP1 {

namespace DOUBLE {

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

//------------------------------------------------------------------

//------------------------------------------------------------------
// *FPU Opcodes!*
//------------------------------------------------------------------

//------------------------------------------------------------------
// PS2 -> DOUBLE
//------------------------------------------------------------------

#define SINGLE(sign, exp, mant) (((u32)(sign) << 31) | ((u32)(exp) << 23) | (u32)(mant))
#define DOUBLE(sign, exp, mant) (((sign##ULL) << 63) | ((exp##ULL) << 52) | (mant##ULL))

struct FPUd_Globals
{
	u32 neg[4], pos[4];

	u32 pos_inf[4], neg_inf[4],
	    one_exp[4];

	u64 dbl_one_exp[2];

	u64 dbl_cvt_overflow, // needs special code if above or equal
	    dbl_ps2_overflow, // overflow & clamp if above or equal
	    dbl_underflow;    // underflow if below

	u64 padding;

	u64 dbl_s_pos[2];
	//u64		dlb_s_neg[2];
};

alignas(32) static const FPUd_Globals s_const =
{
	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
	{0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff},

	{SINGLE(0, 0xff, 0), 0, 0, 0},
	{SINGLE(1, 0xff, 0), 0, 0, 0},
	{SINGLE(0,    1, 0), 0, 0, 0},

	{DOUBLE(0, 1, 0), 0},

	DOUBLE(0, 1151, 0), // cvt_overflow
	DOUBLE(0, 1152, 0), // ps2_overflow
	DOUBLE(0,  897, 0), // underflow

	0,                  // Padding!!

	{0x7fffffffffffffffULL, 0},
	//{0x8000000000000000ULL, 0},
};


// ToDouble : converts single-precision PS2 float to double-precision IEEE float
static void ToDouble(int reg)
{
	u8 *to_complex, *to_complex2, *end;
	xUCOMI.SS(xRegisterSSE(reg), ptr[s_const.pos_inf]); // Sets ZF if reg is equal or incomparable to pos_inf
	xWrite8(JE8);
	xWrite8(0);
	to_complex = (u8*)(x86Ptr - 1); // Complex conversion if positive infinity or NaN
	xUCOMI.SS(xRegisterSSE(reg), ptr[s_const.neg_inf]);
	xWrite8(JE8);
	xWrite8(0);
	to_complex2 = (u8*)(x86Ptr - 1); // Complex conversion if negative infinity

	xCVTSS2SD(xRegisterSSE(reg), xRegisterSSE(reg)); // Simply convert
	xWrite8(0xEB);
	xWrite8(0);
	end           =  x86Ptr - 1;

	*to_complex   = (u8)((x86Ptr - to_complex) - 1);
	*to_complex2  = (u8)((x86Ptr - to_complex2) - 1);

	// Special conversion for when IEEE sees the value in reg as an INF/NaN
	xPSUB.D(xRegisterSSE(reg), ptr[s_const.one_exp]); // Lower exponent by one
	xCVTSS2SD(xRegisterSSE(reg), xRegisterSSE(reg));
	xPADD.Q(xRegisterSSE(reg), ptr[s_const.dbl_one_exp]); // Raise exponent by one

	*end   = (u8)((x86Ptr - end) - 1);
}

//------------------------------------------------------------------
// DOUBLE -> PS2
//------------------------------------------------------------------

// ToPS2FPU_Full - converts double-precision IEEE float to single-precision PS2 float

// converts small normal numbers to PS2 equivalent
// converts large normal numbers to PS2 equivalent (which represent NaN/inf in IEEE)
// converts really large normal numbers to PS2 signed max
// converts really small normal numbers to zero (flush)
// doesn't handle inf/nan/denormal
static void ToPS2FPU_Full(int reg, bool flags, int absreg, bool acc, bool addsub)
{
	u32 *end, *end2;
	u8 *to_complex, *to_overflow, *to_underflow, *end3, *end4;
	if (flags)
	{
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO | FPUflagU));
		if (acc)
			xAND(ptr32[&fpuRegs.ACCflag], ~1);
	}

	xMOVAPS(xRegisterSSE(absreg), xRegisterSSE(reg));
	xAND.PD(xRegisterSSE(absreg), ptr[&s_const.dbl_s_pos]);

	xUCOMI.SD(xRegisterSSE(absreg), ptr[&s_const.dbl_cvt_overflow]);
	xWrite8(JAE8);
	xWrite8(0);
	to_complex = (u8*)(x86Ptr - 1);

	xUCOMI.SD(xRegisterSSE(absreg), ptr[&s_const.dbl_underflow]);
	xWrite8(JB8);
	xWrite8(0);
	to_underflow = (u8*)(x86Ptr - 1);

	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg)); //simply convert

	xWrite8(0xE9);
	xWrite32(0);
	end           = (u32*)(x86Ptr - 4);

	*to_complex   = (u8)((x86Ptr - to_complex) - 1);
	xUCOMI.SD(xRegisterSSE(absreg), ptr[&s_const.dbl_ps2_overflow]);
	xWrite8(JAE8);
	xWrite8(0);
	to_overflow = (u8*)(x86Ptr - 1);

	xPSUB.Q(xRegisterSSE(reg), ptr[&s_const.dbl_one_exp]); //lower exponent
	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg)); //convert
	xPADD.D(xRegisterSSE(reg), ptr[s_const.one_exp]); //raise exponent

	xWrite8(0xE9);
	xWrite32(0);
	end2           = (u32*)(x86Ptr - 4);
	*to_overflow   = (u8)((x86Ptr - to_overflow) - 1);
	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));
	xOR.PS(xRegisterSSE(reg), ptr[&s_const.pos]); //clamp
	if (flags)
	{
		xOR(ptr32[&fpuRegs.fprc[31]], (FPUflagO | FPUflagSO));
		if (acc)
			xOR(ptr32[&fpuRegs.ACCflag], 1);
	}
	xWrite8(0xEB);
	xWrite8(0);
	end3           =  x86Ptr - 1;

	*to_underflow  = (u8)((x86Ptr - to_underflow) - 1);
	end4           = nullptr;
	if (flags) //set underflow flags if not zero
	{
		u8 *is_zero;

		xXOR.PD(xRegisterSSE(absreg), xRegisterSSE(absreg));
		xUCOMI.SD(xRegisterSSE(reg), xRegisterSSE(absreg));
		xWrite8(JE8);
		xWrite8(0);
		is_zero = (u8*)(x86Ptr - 1);

		xOR(ptr32[&fpuRegs.fprc[31]], (FPUflagU | FPUflagSU));
		if (addsub)
		{
			//On ADD/SUB, the PS2 simply leaves the mantissa bits as they are (after normalization)
			//IEEE either clears them (FtZ) or returns the denormalized result.
			//not thoroughly tested : other operations such as MUL and DIV seem to clear all mantissa bits?
			xMOVAPS(xRegisterSSE(absreg), xRegisterSSE(reg));
			xPSLL.Q(xRegisterSSE(reg), 12); //mantissa bits
			xPSRL.Q(xRegisterSSE(reg), 41);
			xPSRL.Q(xRegisterSSE(absreg), 63); //sign bit
			xPSLL.Q(xRegisterSSE(absreg), 31);
			xPOR(xRegisterSSE(reg), xRegisterSSE(absreg));
			xWrite8(0xEB);
			xWrite8(0);
			end4           =  x86Ptr - 1;
		}

		*is_zero   = (u8)((x86Ptr - is_zero) - 1);
	}
	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));
	xAND.PS(xRegisterSSE(reg), ptr[s_const.neg]); //flush to zero

	*end  = (x86Ptr - (u8*)end) - 4;
	*end2 = (x86Ptr - (u8*)end2) - 4;

	*end3 = (u8)((x86Ptr - end3) - 1);
	if (flags && addsub)
		*end4 = (u8)((x86Ptr - end4) - 1);
}

//sets the maximum (positive or negative) value into regd.
#define SetMaxValue(regd) xOR.PS(xRegisterSSE((regd)), ptr[&s_const.pos[0]])

#define GET_S(sreg) \
	do { \
		if (info & PROCESS_EE_S) \
			xMOVSS(xRegisterSSE(sreg), xRegisterSSE(EEREC_S)); \
		else \
			xMOVSSZX(xRegisterSSE(sreg), ptr[&fpuRegs.fpr[_Fs_]]); \
	} while (0)

#define ALLOC_S(sreg) \
	do { \
		(sreg) = _allocTempXMMreg(XMMT_FPS); \
		GET_S(sreg); \
	} while (0)

#define GET_T(treg) \
	do { \
		if (info & PROCESS_EE_T) \
			xMOVSS(xRegisterSSE(treg), xRegisterSSE(EEREC_T)); \
		else \
			xMOVSSZX(xRegisterSSE(treg), ptr[&fpuRegs.fpr[_Ft_]]); \
	} while (0)

#define ALLOC_T(treg) \
	do { \
		(treg) = _allocTempXMMreg(XMMT_FPS); \
		GET_T(treg); \
	} while (0)

#define GET_ACC(areg) \
	do { \
		if (info & PROCESS_EE_ACC) \
			xMOVSS(xRegisterSSE(areg), xRegisterSSE(EEREC_ACC)); \
		else \
			xMOVSSZX(xRegisterSSE(areg), ptr[&fpuRegs.ACC]); \
	} while (0)

#define ALLOC_ACC(areg) \
	do { \
		(areg) = _allocTempXMMreg(XMMT_FPS); \
		GET_ACC(areg); \
	} while (0)

#define CLEAR_OU_FLAGS \
	do { \
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO | FPUflagU)); \
	} while (0)


//------------------------------------------------------------------
// ABS XMM
//------------------------------------------------------------------
void recABS_S_xmm(int info)
{
	GET_S(EEREC_D);

	CLEAR_OU_FLAGS;

	xAND.PS(xRegisterSSE(EEREC_D), ptr[s_const.pos]);
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
static void FPU_ADD_SUB(int tempd, int tempt) //tempd and tempt are overwritten, they are floats
{
	u8 *j8Ptr0, *j8Ptr1, *j8Ptr2, *j8Ptr3, *j8Ptr4, *j8Ptr5, *j8Ptr6;
	const int xmmtemp = _allocTempXMMreg(XMMT_FPS); //temporary for anding with regd/regt
	xMOVD(ecx, xRegisterSSE(tempd)); //receives regd
	xMOVD(eax, xRegisterSSE(tempt)); //receives regt

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
	xAND.PS(xRegisterSSE(tempd), xRegisterSSE(xmmtemp));
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr4  =  x86Ptr - 1;

	*j8Ptr0 = (u8)((x86Ptr - j8Ptr0) - 1);
	//diff = 25 .. 255 , expt < expd
	xAND.PS(xRegisterSSE(tempt), ptr[s_const.neg]);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr5  =  x86Ptr - 1;

	*j8Ptr1 = (u8)((x86Ptr - j8Ptr1) - 1);
	//diff = 1 .. 24, expt < expd
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl); //temp2 = 0xffffffff << tempecx
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(tempt), xRegisterSSE(xmmtemp));
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr6  =  x86Ptr - 1;

	*j8Ptr3 = (u8)((x86Ptr - j8Ptr3) - 1);
	//diff = -255 .. -25, expd < expt
	xAND.PS(xRegisterSSE(tempd), ptr[s_const.neg]);

	*j8Ptr2 = (u8)((x86Ptr - j8Ptr2) - 1);
	//diff == 0

	*j8Ptr4 = (u8)((x86Ptr - j8Ptr4) - 1);
	*j8Ptr5 = (u8)((x86Ptr - j8Ptr5) - 1);
	*j8Ptr6 = (u8)((x86Ptr - j8Ptr6) - 1);

	_freeXMMreg(xmmtemp);
}

static void FPU_MUL(int info, int regd, int sreg, int treg, bool acc)
{
	u32* endMul = nullptr;

	if (CHECK_FPUMULHACK)
	{
		alignas(16) static constexpr const u32 result[4] = { 0x3f490fda };
		u8 *noHack;

		xMOVD(ecx, xRegisterSSE(sreg));
		xMOVD(edx, xRegisterSSE(treg));

		// if (((s ^ 0x3e800000) | (t ^ 0x40490fdb)) != 0) { hack; }
		xXOR(ecx, 0x3e800000);
		xXOR(edx, 0x40490fdb);
		xOR(edx, ecx);

		xWrite8(JNZ8);
		xWrite8(0);
		noHack     = (u8*)(x86Ptr - 1);
		xMOVAPS(xRegisterSSE(regd), ptr128[result]);
		xWrite8(0xE9);
		xWrite32(0);
		endMul     = (u32*)(x86Ptr - 4);
		*noHack    = (u8)((x86Ptr - noHack) - 1);
	}

	ToDouble(sreg);
	ToDouble(treg);
	xMUL.SD(xRegisterSSE(sreg), xRegisterSSE(treg));
	ToPS2FPU_Full(sreg, true, treg, acc, false);
	xMOVSS(xRegisterSSE(regd), xRegisterSSE(sreg));

	if (CHECK_FPUMULHACK)
		*endMul = (x86Ptr - (u8*)endMul) - 4;
}

//------------------------------------------------------------------
// CommutativeOp XMM (used for ADD and SUB opcodes. that's it.)
//------------------------------------------------------------------
static void (*recFPUOpXMM_to_XMM[])(int, int) = {
	SSE2_ADDSD_XMM_to_XMM, SSE2_SUBSD_XMM_to_XMM};

static void recFPUOp(int info, int regd, int op, bool acc)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_ADD_SUB(sreg, treg);

	ToDouble(sreg);
	ToDouble(treg);

	recFPUOpXMM_to_XMM[op](sreg, treg);

	ToPS2FPU_Full(sreg, true, treg, acc, true);
	xMOVSS(xRegisterSSE(regd), xRegisterSSE(sreg));

	_freeXMMreg(sreg); _freeXMMreg(treg);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// ADD XMM
//------------------------------------------------------------------
void recADD_S_xmm(int info)
{
	recFPUOp(info, EEREC_D, 0, false);
}

FPURECOMPILE_CONSTCODE(ADD_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recADDA_S_xmm(int info)
{
	recFPUOp(info, EEREC_ACC, 0, true);
}

FPURECOMPILE_CONSTCODE(ADDA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------

void recCMP(int info)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);
	ToDouble(sreg);
	ToDouble(treg);

	xUCOMI.SD(xRegisterSSE(sreg), xRegisterSSE(treg));

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

//------------------------------------------------------------------
// C.x.S XMM
//------------------------------------------------------------------
void recC_EQ_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	recCMP(info);

	xWrite8(JZ8);
	xWrite8(0);
	j8Ptr0  = (u8*)(x86Ptr - 1);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr1  =  x86Ptr - 1;
	*j8Ptr0 = (u8)((x86Ptr - j8Ptr0) - 1);
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	*j8Ptr1 = (u8)((x86Ptr - j8Ptr1) - 1);
}

FPURECOMPILE_CONSTCODE(C_EQ, XMMINFO_READS | XMMINFO_READT);

void recC_LE_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	recCMP(info);

	xWrite8(JBE8);
	xWrite8(0);
	j8Ptr0 = (u8*)(x86Ptr - 1);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr1  =  x86Ptr - 1;
	*j8Ptr0 = (u8)((x86Ptr - j8Ptr0) - 1);
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	*j8Ptr1 = (u8)((x86Ptr - j8Ptr1) - 1);
}

FPURECOMPILE_CONSTCODE(C_LE, XMMINFO_READS | XMMINFO_READT);

void recC_LT_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	recCMP(info);

	xWrite8(JB8);
	xWrite8(0);
	j8Ptr0 = (u8*)(x86Ptr - 1);
	xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
	xWrite8(0xEB);
	xWrite8(0);
	j8Ptr1  =  x86Ptr - 1;
	*j8Ptr0 = (u8)((x86Ptr - j8Ptr0) - 1);
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	*j8Ptr1 = (u8)((x86Ptr - j8Ptr1) - 1);
}

FPURECOMPILE_CONSTCODE(C_LT, XMMINFO_READS | XMMINFO_READT);
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

void recCVT_W(void) //called from iFPU.cpp's recCVT_W
{
	int regs = _checkXMMreg(XMMTYPE_FPREG, _Fs_, MODE_READ);

	if (regs >= 0)
	{
		xCVTTSS2SI(eax, xRegisterSSE(regs));
		xMOVMSKPS(edx, xRegisterSSE(regs)); // extract the signs
		xAND(edx, 1);                       // keep only LSB
	}
	else
	{
		xCVTTSS2SI(eax, ptr32[&fpuRegs.fpr[_Fs_]]);
		xMOV(edx, ptr[&fpuRegs.fpr[_Fs_]]);
		xSHR(edx, 31); //mov sign to lsb
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

	//--- Check for divide by zero ---
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regt));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regt == zero, sign will be set)
	xWrite8(0x0F);
	xWrite8(JZ32);
	xWrite32(0);
	ajmp32 = (u32*)(x86Ptr - 4); /* Skip if not set */

	//--- Check for 0/0 ---
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regd == zero, sign will be set)
	xWrite8(JZ8);
	xWrite8(0);
	pjmp1 = (u8*)(x86Ptr - 1); /* Skip if not set */
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
	xWrite8(0xEB);
	xWrite8(0);
	pjmp2   =  x86Ptr - 1;
	*pjmp1 = (u8)((x86Ptr - pjmp1) - 1); //x/0 but not 0/0
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
	*pjmp2 = (u8)((x86Ptr - pjmp2) - 1);

	//--- Make regd +/- Maximum ---
	xXOR.PS(xRegisterSSE(regd), xRegisterSSE(regt)); // Make regd Positive or Negative
	SetMaxValue(regd); //clamp to max
	xWrite8(0xE9);
	xWrite32(0);
	bjmp32  = (u32*)(x86Ptr - 4);

	*ajmp32 = (x86Ptr - (u8*)ajmp32) - 4;

	//--- Normal Divide ---
	ToDouble(regd);
	ToDouble(regt);

	xDIV.SD(xRegisterSSE(regd), xRegisterSSE(regt));

	ToPS2FPU_Full(regd, false, regt, false, false);

	*bjmp32 = (x86Ptr - (u8*)bjmp32) - 4;

	_freeXMMreg(t1reg);
}

alignas(16) static FPControlRegister roundmode_nearest, roundmode_neg;

void recDIV_S_xmm(int info)
{
	int sreg, treg;

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUDivFPCR.bitmask]);

	ALLOC_S(sreg);
	ALLOC_T(treg);

	recDIVhelper1(sreg, treg);

	xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(sreg));

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

FPURECOMPILE_CONSTCODE(DIV_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MADD/MSUB XMM
//------------------------------------------------------------------

// Unlike what the documentation implies, it seems that MADD/MSUB support all numbers just like other operations
// The complex overflow conditions the document describes apparently test whether the multiplication's result
// has overflowed and whether the last operation that used ACC as a destination has overflowed.
// For example,   { adda.s -MAX, 0.0 ; madd.s fd, MAX, 1.0 } -> fd = 0
// while          { adda.s -MAX, -MAX ; madd.s fd, MAX, 1.0 } -> fd = -MAX
// (where MAX is 0x7fffffff and -MAX is 0xffffffff)
static void recMaddsub(int info, int regd, int op, bool acc)
{
	u32 *skipall;
	u8 *mulovf, *accovf, *operation;
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_MUL(info, sreg, sreg, treg, false);

	GET_ACC(treg);

	FPU_ADD_SUB(treg, sreg); //might be problematic for something!!!!

	//          TEST FOR ACC/MUL OVERFLOWS, PROPOGATE THEM IF THEY OCCUR

	xTEST(ptr32[&fpuRegs.fprc[31]], FPUflagO);
	xWrite8(JNZ8);
	xWrite8(0);
	mulovf     = (u8*)(x86Ptr - 1);
	ToDouble(sreg); //else, convert

	xTEST(ptr32[&fpuRegs.ACCflag], 1);
	xWrite8(JNZ8);
	xWrite8(0);
	accovf        = (u8*)(x86Ptr - 1);
	ToDouble(treg); //else, convert
	xWrite8(0xEB);
	xWrite8(0);
	operation =  x86Ptr - 1;

	*mulovf = (u8)((x86Ptr - mulovf) - 1);
	if (op == 1) //sub
		xXOR.PS(xRegisterSSE(sreg), ptr[s_const.neg]);
	xMOVAPS(xRegisterSSE(treg), xRegisterSSE(sreg)); //fall through below

	*accovf = (u8)((x86Ptr - accovf) - 1);
	SetMaxValue(treg); //just in case... I think it has to be a MaxValue already here
	CLEAR_OU_FLAGS; //clear U flag
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagO | FPUflagSO);
	if (acc)
		xOR(ptr32[&fpuRegs.ACCflag], 1);
	xWrite8(0xE9);
	xWrite32(0);
	skipall = (u32*)(x86Ptr - 4);

	// PERFORM THE ACCUMULATION AND TEST RESULT. CONVERT TO SINGLE

	*operation = (u8)((x86Ptr - operation) - 1);
	if (op == 1)
		xSUB.SD(xRegisterSSE(treg), xRegisterSSE(sreg));
	else
		xADD.SD(xRegisterSSE(treg), xRegisterSSE(sreg));

	ToPS2FPU_Full(treg, true, sreg, acc, true);
	*skipall = (x86Ptr - (u8*)skipall) - 4;

	xMOVSS(xRegisterSSE(regd), xRegisterSSE(treg));

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

void recMADD_S_xmm(int info)
{
	recMaddsub(info, EEREC_D, 0, false);
}

FPURECOMPILE_CONSTCODE(MADD_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMADDA_S_xmm(int info)
{
	recMaddsub(info, EEREC_ACC, 0, true);
}

FPURECOMPILE_CONSTCODE(MADDA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MAX / MIN XMM
//------------------------------------------------------------------

// FPU's MAX/MIN work with all numbers (including "denormals"). Check VU's logical min max for more info.
static void recMINMAX(int info, bool ismin)
{
	alignas(16) static const u32 minmax_mask[8] =
	{
		0xffffffff, 0x80000000, 0, 0,
		0,          0x40000000, 0, 0,
	};
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	CLEAR_OU_FLAGS;

	xPSHUF.D(xRegisterSSE(sreg), xRegisterSSE(sreg), 0x00);
	xPAND(xRegisterSSE(sreg), ptr[minmax_mask]);
	xPOR(xRegisterSSE(sreg), ptr[&minmax_mask[4]]);
	xPSHUF.D(xRegisterSSE(treg), xRegisterSSE(treg), 0x00);
	xPAND(xRegisterSSE(treg), ptr[minmax_mask]);
	xPOR(xRegisterSSE(treg), ptr[&minmax_mask[4]]);
	if (ismin)
		xMIN.SD(xRegisterSSE(sreg), xRegisterSSE(treg));
	else
		xMAX.SD(xRegisterSSE(sreg), xRegisterSSE(treg));

	xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(sreg));

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

void recMAX_S_xmm(int info)
{
	recMINMAX(info, false);
}

FPURECOMPILE_CONSTCODE(MAX_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMIN_S_xmm(int info)
{
	recMINMAX(info, true);
}

FPURECOMPILE_CONSTCODE(MIN_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MOV XMM
//------------------------------------------------------------------
void recMOV_S_xmm(int info)
{
	GET_S(EEREC_D);
}

FPURECOMPILE_CONSTCODE(MOV_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MSUB XMM
//------------------------------------------------------------------

void recMSUB_S_xmm(int info)
{
	recMaddsub(info, EEREC_D, 1, false);
}

FPURECOMPILE_CONSTCODE(MSUB_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMSUBA_S_xmm(int info)
{
	recMaddsub(info, EEREC_ACC, 1, true);
}

FPURECOMPILE_CONSTCODE(MSUBA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------

//------------------------------------------------------------------
// MUL XMM
//------------------------------------------------------------------
void recMUL_S_xmm(int info)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_MUL(info, EEREC_D, sreg, treg, false);
	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

FPURECOMPILE_CONSTCODE(MUL_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMULA_S_xmm(int info)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_MUL(info, EEREC_ACC, sreg, treg, true);
	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

FPURECOMPILE_CONSTCODE(MULA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// NEG XMM
//------------------------------------------------------------------
void recNEG_S_xmm(int info)
{
	GET_S(EEREC_D);

	CLEAR_OU_FLAGS;

	xXOR.PS(xRegisterSSE(EEREC_D), ptr[&s_const.neg[0]]);
}

FPURECOMPILE_CONSTCODE(NEG_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SUB XMM
//------------------------------------------------------------------

void recSUB_S_xmm(int info)
{
	recFPUOp(info, EEREC_D, 1, false);
}

FPURECOMPILE_CONSTCODE(SUB_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


void recSUBA_S_xmm(int info)
{
	recFPUOp(info, EEREC_ACC, 1, true);
}

FPURECOMPILE_CONSTCODE(SUBA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SQRT XMM
//------------------------------------------------------------------
void recSQRT_S_xmm(int info)
{
	int roundmodeFlag = 0;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	if (EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::Nearest)
	{
		// Set roundmode to nearest if it isn't already
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest.SetRoundMode(FPRoundMode::Nearest);
		xLDMXCSR(ptr32[&roundmode_nearest.bitmask]);
		roundmodeFlag = 1;
	}

	GET_T(EEREC_D);

	{
		u8 *pjmp;
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

		//--- Check for negative SQRT --- (sqrt(-0) = 0, unlike what the docs say)
		xMOVMSKPS(eax, xRegisterSSE(EEREC_D));
		xAND(eax, 1); //Check sign
		xWrite8(JZ8);
		xWrite8(0);
		pjmp = (u8*)(x86Ptr - 1); /* Skip if none are */
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags
		xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_const.pos[0]]); // Make EEREC_D Positive
		*pjmp = (u8)((x86Ptr - pjmp) - 1);
	}


	ToDouble(EEREC_D);

	xSQRT.SD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));

	ToPS2FPU_Full(EEREC_D, false, t1reg, false, false);

	if (roundmodeFlag == 1)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

	_freeXMMreg(t1reg);
}

FPURECOMPILE_CONSTCODE(SQRT_S, XMMINFO_WRITED | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// RSQRT XMM
//------------------------------------------------------------------
static void recRSQRThelper1(int regd, int regt) // Preforms the RSQRT function when regd <- Fs and regt <- Ft (Sets correct flags)
{
	u8 *pjmp1, *pjmp2;
	u8 *qjmp1, *qjmp2;
	u32* pjmp32;
	int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	//--- (first) Check for negative SQRT ---
	xMOVMSKPS(eax, xRegisterSSE(regt));
	xAND(eax, 1); //Check sign
	xWrite8(JZ8);
	xWrite8(0);
	pjmp2 = (u8*)(x86Ptr - 1); /* Skip if not set */
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags
	xAND.PS(xRegisterSSE(regt), ptr[&s_const.pos[0]]); // Make regt Positive
	*pjmp2 = (u8)((x86Ptr - pjmp2) - 1);

	//--- Check for zero ---
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regt));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regt == zero, sign will be set)
	xWrite8(JZ8);
	xWrite8(0);
	pjmp1 = (u8*)(x86Ptr - 1); /* Skip if not set */

	//--- Check for 0/0 ---
	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1); //Check sign (if regd == zero, sign will be set)
	xWrite8(JZ8);
	xWrite8(0);
	qjmp1 = (u8*)(x86Ptr - 1); /* Skip if not set */
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
	xWrite8(0xEB);
	xWrite8(0);
	qjmp2  =  x86Ptr - 1;
	*qjmp1 = (u8)((x86Ptr - qjmp1) - 1); //x/0 but not 0/0
	xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
	*qjmp2 = (u8)((x86Ptr - qjmp2) - 1);

	SetMaxValue(regd); //clamp to max
	xWrite8(0xE9);
	xWrite32(0);
	pjmp32 = (u32*)(x86Ptr - 4);
	*pjmp1 = (u8)((x86Ptr - pjmp1) - 1);

	ToDouble(regt);
	ToDouble(regd);

	xSQRT.SD(xRegisterSSE(regt), xRegisterSSE(regt));
	xDIV.SD(xRegisterSSE(regd), xRegisterSSE(regt));

	ToPS2FPU_Full(regd, false, regt, false, false);
	*pjmp32 = (x86Ptr - (u8*)pjmp32) - 4;

	_freeXMMreg(t1reg);
}

void recRSQRT_S_xmm(int info)
{
	int sreg, treg;

	// iFPU (regular FPU) doesn't touch roundmode for rSQRT.
	// Should this do the same?  or is changing the roundmode to nearest the better
	// behavior for both recs? --air

	bool roundmodeFlag = false;
	if (EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::Nearest)
	{
		// Set roundmode to nearest if it isn't already
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest.SetRoundMode(FPRoundMode::Nearest);
		xLDMXCSR(ptr32[&roundmode_nearest.bitmask]);
		roundmodeFlag = true;
	}

	ALLOC_S(sreg);
	ALLOC_T(treg);

	recRSQRThelper1(sreg, treg);

	xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(sreg));

	_freeXMMreg(treg);
	_freeXMMreg(sreg);

	if (roundmodeFlag)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
}

FPURECOMPILE_CONSTCODE(RSQRT_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


} // namespace DOUBLE
} // namespace COP1
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
#endif
