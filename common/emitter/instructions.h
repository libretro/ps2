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

/*
 * ix86 definitions v0.9.1
 *
 * Original Authors (v0.6.2 and prior):
 *		linuzappz <linuzappz@pcsx.net>
 *		alexey silinov
 *		goldfinger
 *		zerofrog(@gmail.com)
 *
 * Authors of v0.9.1:
 *		Jake.Stine(@gmail.com)
 *		cottonvibes(@gmail.com)
 *		sudonim(1@gmail.com)
 */

#pragma once
#include <vector>

#include "internal.h"

// rather than dealing with nonexistant operands..
#define xVZEROUPPER() \
	xWrite8(0xc5); \
	xWrite8(0xf8); \
	xWrite8(0x77)

#define xVPMOVMSKB(to, from) xOpWriteC5(0x66, 0xd7, to, xRegister32(), from)

// xMASKMOV:
// Selectively write bytes from mm1/xmm1 to memory location using the byte mask in mm2/xmm2.
// The default memory location is specified by DS:EDI.  The most significant bit in each byte
// of the mask operand determines whether the corresponding byte in the source operand is
// written to the corresponding byte location in memory.
#define xMASKMOV(to, from) xOpWrite0F(0x66, 0xf7, to, from)

// xPMOVMSKB:
// Creates a mask made up of the most significant bit of each byte of the source
// operand and stores the result in the low byte or word of the destination operand.
// Upper bits of the destination are cleared to zero.
//
// When operating on a 64-bit (MMX) source, the byte mask is 8 bits; when operating on
// 128-bit (SSE) source, the byte mask is 16-bits.
//
#define xPMOVMSKB(to, from) xOpWrite0F(0x66, 0xd7, to, from)

// [sSSE-3] Concatenates dest and source operands into an intermediate composite,
// shifts the composite at byte granularity to the right by a constant immediate,
// and extracts the right-aligned result into the destination.
//
#define xPALIGNR(to, from, imm8) xOpWrite0F(0x66, 0x0f3a, to, from, imm8)

// ----- Miscellaneous Instructions  -----
// Various Instructions with no parameter and no special encoding logic.

#define xRET() xWrite8(0xC3)
#define xCBW() xWrite16(0x9866)
#define xCWD() xWrite8(0x98)
#define xCDQ() xWrite8(0x99)

#define xCWDE() xWrite8(0x98)
#define xCDQE() xWrite16(0x9848)

#define xSAHF() xWrite8(0x9e)
#define xLAHF() xWrite8(0x9f)

#define xCLC() xWrite8(0xF8)
#define xSTC() xWrite8(0xF9)

// NOP 1-byte
#define xNOP() xWrite8(0x90)

// ------------------------------------------------------------------------
// Conditional jumps to fixed targets.
// Jumps accept any pointer as a valid target (function or data), and will generate either
// 8 or 32 bit displacement versions of the jump, depending on relative displacement of
// the target (efficient!)
//

#define xJNE(func) xJccKnownTarget(Jcc_NotEqual, (void*)(uptr)(func), false)
#define xJNZ(func) xJccKnownTarget(Jcc_NotZero, (void*)(uptr)(func), false)
#define xJS(func) xJccKnownTarget(Jcc_Signed, (void*)(uptr)(func), false)
#define xJLE(func) xJccKnownTarget(Jcc_LessOrEqual, (void*)(uptr)(func), false)
#define xJC(func) xJccKnownTarget(Jcc_Carry, (void*)(uptr)(func), false)

namespace x86Emitter
{
	// ------------------------------------------------------------------------
	// Group 1 Instruction Class

	extern const xImpl_Group1 xADC;

	extern const xImpl_G1Logic xAND;
	extern const xImpl_G1Logic xOR;
	extern const xImpl_G1Logic xXOR;

	extern const xImpl_G1Arith xADD;
	extern const xImpl_G1Arith xSUB;
	extern const xImpl_G1Compare xCMP;

	// ------------------------------------------------------------------------
	// Group 2 Instruction Class
	//
	// Optimization Note: For Imm forms, we ignore the instruction if the shift count is
	// zero.  This is a safe optimization since any zero-value shift does not affect any
	// flags.

	extern const xImpl_Mov xMOV;
	extern const xImpl_MovImm64 xMOV64;
	extern const xImpl_Test xTEST;
	extern const xImpl_Group2 xROL, xROR,
		xRCL, xRCR,
		xSHL, xSHR,
		xSAR;

	// ------------------------------------------------------------------------
	// Group 3 Instruction Class

	extern const xImpl_Group3 xNOT, xNEG;
	extern const xImpl_Group3 xUMUL, xUDIV;
	extern const xImpl_iDiv xDIV;
	extern const xImpl_iMul xMUL;

	extern const xImpl_IncDec xINC, xDEC;

	extern const xImpl_MovExtend xMOVZX, xMOVSX;

	extern const xImpl_BitScan xBSR;

	extern const xImpl_JmpCall xJMP;
	extern const xImpl_JmpCall xCALL;
	extern const xImpl_FastCall xFastCall;

	// ------------------------------------------------------------------------
	extern const xImpl_CMov 
		xCMOVB,

		xCMOVE,
		xCMOVNE,

		xCMOVS, xCMOVNS;

	// ------------------------------------------------------------------------
	extern const xImpl_Set xSETA,
		xSETB,
		xSETG,
		xSETL;

	//////////////////////////////////////////////////////////////////////////////////////////
	// Miscellaneous Instructions
	// These are all defined inline or in ix86.cpp.
	//

	// ----- Lea Instructions (Load Effective Address) -----
	// Note: alternate (void*) forms of these instructions are not provided since those
	// forms are functionally equivalent to Mov reg,imm, and thus better written as MOVs
	// instead.

	extern void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags = false);
	extern void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags = false);
	extern void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags = false);

	// ----- Push / Pop Instructions  -----
	// Note: pushad/popad implementations are intentionally left out.  The instructions are
	// invalid in x64, and are super slow on x32.  Use multiple Push/Pop instructions instead.

	extern void xPOP(const xIndirectVoid& from);
	extern void xPUSH(const xIndirectVoid& from);

	extern void xPOP(xRegister32or64 from);

	extern void xPUSH(u32 imm);
	extern void xPUSH(xRegister32or64 from);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to calculate base+offset taking into account the limitations of x86-64's RIP-relative addressing
	/// (Will either return `base+offset` or LEA `base` into `tmpRegister` and return `tmpRegister+offset`)
	xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to load addresses that may be far from the current instruction pointer
	/// On i386, resolves to `mov dst, (sptr)addr`
	/// On x86-64, resolves to either `mov dst, (sptr)addr` or `lea dst, [addr]` depending on the distance from RIP
	void xLoadFarAddr(const xAddressReg& dst, void* addr);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to write a 64-bit constant to memory
	/// May use `tmp` on x86-64
	void xWriteImm64ToMem(u64* addr, const xAddressReg& tmp, u64 imm);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to run operations with large immediates
	/// If the immediate fits in 32 bits, runs op(target, imm)
	/// Otherwise, loads imm into tmpRegister and then runs op(dst, tmp)
	template <typename Op, typename Dst>
	void xImm64Op(const Op& op, const Dst& dst, const xRegister64& tmpRegister, s64 imm)
	{
		if (imm == (s32)imm)
		{
			op(dst, imm);
		}
		else
		{
			xMOV64(tmpRegister, imm);
			op(dst, tmpRegister);
		}
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// JMP / Jcc Instructions!

	extern s32* xJcc32(JccComparisonType comparison, s32 displacement);

	// ------------------------------------------------------------------------
	// Forward Jump Helpers (act as labels!)

#define DEFINE_FORWARD_JUMP(label, cond) \
	template <typename OperandType> \
	class xForward##label : public xForwardJump<OperandType> \
	{ \
	public: \
		xForward##label() \
			: xForwardJump<OperandType>(cond) \
		{ \
		} \
	};

	// ------------------------------------------------------------------------
	// Note: typedefs below  are defined individually in order to appease Intellisense
	// resolution.  Including them into the class definition macro above breaks it.

	typedef xForwardJump<s8> xForwardJump8;
	typedef xForwardJump<s32> xForwardJump32;

	DEFINE_FORWARD_JUMP(JB, Jcc_Below);

	typedef xForwardJB<s8> xForwardJB8;
	typedef xForwardJB<s32> xForwardJB32;

	DEFINE_FORWARD_JUMP(JL, Jcc_Less);
	DEFINE_FORWARD_JUMP(JLE, Jcc_LessOrEqual);

	typedef xForwardJL<s8> xForwardJL8;
	typedef xForwardJL<s32> xForwardJL32;
	typedef xForwardJLE<s8> xForwardJLE8;

	DEFINE_FORWARD_JUMP(JZ, Jcc_Zero);
	DEFINE_FORWARD_JUMP(JE, Jcc_Equal);
	DEFINE_FORWARD_JUMP(JNZ, Jcc_NotZero);

	typedef xForwardJZ<s8> xForwardJZ8;
	typedef xForwardJZ<s32> xForwardJZ32;
	typedef xForwardJE<s8> xForwardJE8;
	typedef xForwardJNZ<s8> xForwardJNZ8;

	DEFINE_FORWARD_JUMP(JS, Jcc_Signed);
	DEFINE_FORWARD_JUMP(JNS, Jcc_Unsigned);

	typedef xForwardJS<s8> xForwardJS8;
	typedef xForwardJNS<s32> xForwardJNS32;

	// ------------------------------------------------------------------------

	extern void xLDMXCSR(const xIndirect32& src);

	extern void xMOVDZX(const xRegisterSSE& to, const xRegister32or64& from);
	extern void xMOVDZX(const xRegisterSSE& to, const xIndirectVoid& src);

	extern void xMOVD(const xRegister32or64& to, const xRegisterSSE& from);
	extern void xMOVD(const xIndirectVoid& dest, const xRegisterSSE& from);

	extern void xMOVQ(const xIndirectVoid& dest, const xRegisterSSE& from);

	extern void xMOVQZX(const xRegisterSSE& to, const xIndirectVoid& src);
	extern void xMOVQZX(const xRegisterSSE& to, const xRegisterSSE& from);

	extern void xMOVSS(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xMOVSS(const xIndirectVoid& to, const xRegisterSSE& from);
	extern void xMOVSD(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xMOVSD(const xIndirectVoid& to, const xRegisterSSE& from);

	extern void xMOVSSZX(const xRegisterSSE& to, const xIndirectVoid& from);
	extern void xMOVSDZX(const xRegisterSSE& to, const xIndirectVoid& from);

	extern void xMOVMSKPS(const xRegister32& to, const xRegisterSSE& from);
	extern void xMOVMSKPD(const xRegister32& to, const xRegisterSSE& from);

	// ------------------------------------------------------------------------

	extern const xImplSimd_MoveSSE xMOVAPS;
	extern const xImplSimd_MoveSSE xMOVUPS;

#ifdef ALWAYS_USE_MOVAPS
	extern const xImplSimd_MoveSSE xMOVDQA;
	extern const xImplSimd_MoveSSE xMOVDQU;
#else
	extern const xImplSimd_MoveDQ xMOVDQA;
	extern const xImplSimd_MoveDQ xMOVDQU;
#endif

	extern const xImplSimd_MovHL xMOVH;
	extern const xImplSimd_MovHL xMOVL;
	extern const xImplSimd_MovHL_RtoR xMOVLH;
	extern const xImplSimd_MovHL_RtoR xMOVHL;

	extern const xImplSimd_Blend xBLEND;
	extern const xImplSimd_PMove xPMOVSX;
	extern const xImplSimd_PMove xPMOVZX;

	extern void xINSERTPS(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm8);
	extern void xINSERTPS(const xRegisterSSE& to, const xIndirect32& from, u8 imm8);

	extern void xEXTRACTPS(const xRegister32or64& to, const xRegisterSSE& from, u8 imm8);
	extern void xEXTRACTPS(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8);

	// ------------------------------------------------------------------------

	extern const xImplSimd_DestRegEither xPAND;
	extern const xImplSimd_DestRegEither xPANDN;
	extern const xImplSimd_DestRegEither xPOR;
	extern const xImplSimd_DestRegEither xPXOR;

	extern const xImplSimd_Shuffle xSHUF;

	// ------------------------------------------------------------------------

	extern const xImplSimd_DestRegSSE xPTEST;

	extern const xImplSimd_MinMax xMIN;
	extern const xImplSimd_MinMax xMAX;

	extern const xImplSimd_Compare xCMPEQ, xCMPLT,
		xCMPNLT,
		xCMPNLE;

	extern const xImplSimd_COMI xUCOMI;

	extern const xImplSimd_PCompare xPCMP;
	extern const xImplSimd_PMinMax xPMIN;
	extern const xImplSimd_PMinMax xPMAX;

	// ------------------------------------------------------------------------
	//
	//
	extern void xCVTDQ2PD(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xCVTDQ2PD(const xRegisterSSE& to, const xIndirect64& from);
	extern void xCVTDQ2PS(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xCVTDQ2PS(const xRegisterSSE& to, const xIndirect128& from);

	extern void xCVTSD2SS(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xCVTSD2SS(const xRegisterSSE& to, const xIndirect64& from);
	extern void xCVTSI2SS(const xRegisterSSE& to, const xRegister32or64& from);
	extern void xCVTSI2SS(const xRegisterSSE& to, const xIndirect32& from);

	extern void xCVTSS2SD(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xCVTSS2SD(const xRegisterSSE& to, const xIndirect32& from);
	extern void xCVTSS2SI(const xRegister32or64& to, const xRegisterSSE& from);
	extern void xCVTSS2SI(const xRegister32or64& to, const xIndirect32& from);

	extern void xCVTTPS2DQ(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xCVTTPS2DQ(const xRegisterSSE& to, const xIndirect128& from);

	extern void xCVTTSD2SI(const xRegister32or64& to, const xRegisterSSE& from);
	extern void xCVTTSD2SI(const xRegister32or64& to, const xIndirect64& from);
	extern void xCVTTSS2SI(const xRegister32or64& to, const xRegisterSSE& from);
	extern void xCVTTSS2SI(const xRegister32or64& to, const xIndirect32& from);

	// ------------------------------------------------------------------------

	extern const xImplSimd_Sqrt xSQRT;

	extern const xImplSimd_Shift xPSLL;
	extern const xImplSimd_Shift xPSRL;
	extern const xImplSimd_ShiftWithoutQ xPSRA;
	extern const xImplSimd_AddSub xPADD;
	extern const xImplSimd_AddSub xPSUB;
	extern const xImplSimd_PMul xPMUL;
	extern const xImplSimd_PAbsolute xPABS;
	extern const xImplSimd_PMultAdd xPMADD;
	extern const xImplSimd_DotProduct xDP;

	extern const xImplSimd_PShuffle xPSHUF;
	extern const SimdImpl_PUnpack xPUNPCK;
	extern const xImplSimd_Unpack xUNPCK;
	extern const SimdImpl_Pack xPACK;
	extern const xImplSimd_PInsert xPINSR;
	extern const SimdImpl_PExtract xPEXTR;

	// ------------------------------------------------------------------------

	extern const xImplAVX_Move xVMOVAPS;
	extern const xImplAVX_Move xVMOVUPS;
	extern const xImplAVX_ThreeArgYMM xVPAND;
	extern const xImplAVX_CmpInt xVPCMP;

	extern void xVMOVMSKPS(const xRegister32& to, const xRegisterSSE& from);
	extern void xVMOVMSKPD(const xRegister32& to, const xRegisterSSE& from);

} // namespace x86Emitter
