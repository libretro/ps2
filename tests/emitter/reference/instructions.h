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

#ifndef PCSX2_C89_EMITTER
#include "tests/emitter/reference/internal.h"
#endif

// rather than dealing with nonexistant operands..
#define xVZEROUPPER() \
	xWrite8(0xc5); \
	xWrite8(0xf8); \
	xWrite8(0x77)

#ifdef PCSX2_C89_EMITTER
// The macro routes through xOpWriteC5, which is reference machinery. Same
// encoding through the core's VEX macros; the L bit follows the source, as
// the reference's param3 rule dictates for a GPR destination -- this is the
// 256-bit movemask that rule exists for.
#define xVPMOVMSKB(to, from) do { \
	e_u8* vpm_p_ = (e_u8*)x86Ptr; \
	E_VEX_RRR(vpm_p_, 0x66, 0xd7, (to).Id, E_NOREG, (from).Id, \
	          x86Emitter::shim_ymm(from)); \
	x86Ptr = (u8*)vpm_p_; } while (0)
#else
#define xVPMOVMSKB(to, from) xOpWriteC5(0x66, 0xd7, to, xRegister32(), from)
#endif

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
#ifdef PCSX2_C89_EMITTER
// Routed to the shim: the reference form expands to xOpWrite0F, whose
// EmitRex/EmitSibMagic calls were the last reference-side imports left in
// the switched recompilers (found via objdump on microVU.o's mVU_CLIP).
#define xPMOVMSKB(to, from) shim_xPMOVMSKB(to, from)
#else
#define xPMOVMSKB(to, from) xOpWrite0F(0x66, 0xd7, to, from)
#endif

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

#ifdef PCSX2_C89_EMITTER
// Ahead of the namespace so a binding can sit next to the declaration it
// replaces. mov's has to: the xImm64Op template below names xMOV64, and a
// non-dependent name must be declared before the template body.
#include "tests/emitter/reference/x86emitter_shim.h"
#endif

namespace x86Emitter
{
	// ------------------------------------------------------------------------
	// Group 1 Instruction Class

#ifndef PCSX2_C89_EMITTER
#ifndef PCSX2_C89_EMITTER
	extern const xImpl_Group1 xADC;
	extern const xImpl_Group1 xCMP;

	extern const xImpl_G1Logic xAND;
	extern const xImpl_G1Logic xOR;
	extern const xImpl_G1Logic xXOR;

	extern const xImpl_G1Arith xADD;
	extern const xImpl_G1Arith xSUB;
#endif
	// Under PCSX2_C89_EMITTER these are objects of the shim types instead,
	// defined at the bottom of this header. Same names, same call syntax,
	// same initialisers -- only the type changes, to one whose members are
	// defined in a header and therefore inline at the call site.
#endif
	// When PCSX2_C89_EMITTER is defined these are objects of the shim types
	// instead, defined at the bottom of this header -- after xLEA, which the
	// shim's far-call path needs. Same names, same call syntax, same
	// initialisers; only the type changes, to one whose members live in a
	// header and therefore inline at the call site.

	// ------------------------------------------------------------------------
	// Group 2 Instruction Class
	//
	// Optimization Note: For Imm forms, we ignore the instruction if the shift count is
	// zero.  This is a safe optimization since any zero-value shift does not affect any
	// flags.

#ifdef PCSX2_C89_EMITTER
	static const shim_Mov      xMOV;
	static const shim_MovImm64 xMOV64;
	static const shim_Test     xTEST;
#else
	extern const xImpl_Mov xMOV;
	extern const xImpl_MovImm64 xMOV64;
	extern const xImpl_Test xTEST;
#endif
#ifndef PCSX2_C89_EMITTER
	extern const xImpl_Group2 xROL, xROR,
		xRCL, xRCR,
		xSHL, xSHR,
		xSAR;
#endif

	// ------------------------------------------------------------------------
	// Group 3 Instruction Class

#ifndef PCSX2_C89_EMITTER
	extern const xImpl_Group3 xNOT, xNEG;
	extern const xImpl_Group3 xUMUL, xUDIV;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_iDiv     xDIV = {{G3Type_iDIV}, {0x00, 0x5e}, {0x66, 0x5e}, {0xf3, 0x5e}, {0xf2, 0x5e}};
	static const shim_iMulFull xMUL = {{G3Type_iMUL}, {0x00, 0x59}, {0x66, 0x59}, {0xf3, 0x59}, {0xf2, 0x59}};
#else
	extern const xImpl_iDiv xDIV;
	extern const xImpl_iMul xMUL;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_IncDec xINC = {false};
	static const shim_IncDec xDEC = {true};
#else
	extern const xImpl_IncDec xINC, xDEC;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_MovExtend xMOVZX = {false};
	static const shim_MovExtend xMOVSX = {true};
#else
	extern const xImpl_MovExtend xMOVZX, xMOVSX;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_BitScan xBSR = {0xbd};
#else
	extern const xImpl_BitScan xBSR;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_JmpCall xJMP = {true};
#else
	extern const xImpl_JmpCall xJMP;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_JmpCall xCALL = {false};
#else
	extern const xImpl_JmpCall xCALL;
#endif
#ifndef PCSX2_C89_EMITTER
	extern const xImpl_FastCall xFastCall;
#endif

	// ------------------------------------------------------------------------
#ifdef PCSX2_C89_EMITTER
	static const shim_CMov xCMOVB = {Jcc_Below};
	static const shim_CMov xCMOVGE = {Jcc_GreaterOrEqual};
	static const shim_CMov xCMOVE = {Jcc_Equal};
	static const shim_CMov xCMOVNE = {Jcc_NotEqual};
	static const shim_CMov xCMOVS = {Jcc_Signed};
	static const shim_CMov xCMOVNS = {Jcc_Unsigned};
	static const shim_Set xSETA = {Jcc_Above};
	static const shim_Set xSETB = {Jcc_Below};
	static const shim_Set xSETG = {Jcc_Greater};
	static const shim_Set xSETL = {Jcc_Less};
#else
	extern const xImpl_CMov 
		xCMOVB,
		xCMOVGE,

		xCMOVE,
		xCMOVNE,

		xCMOVS, xCMOVNS;

	// ------------------------------------------------------------------------
	extern const xImpl_Set xSETA,
		xSETB,
		xSETG,
		xSETL;
#endif

	//////////////////////////////////////////////////////////////////////////////////////////
	// Miscellaneous Instructions
	// These are all defined inline or in ix86.cpp.
	//

	// ----- Lea Instructions (Load Effective Address) -----
	// Note: alternate (void*) forms of these instructions are not provided since those
	// forms are functionally equivalent to Mov reg,imm, and thus better written as MOVs
	// instead.

#ifdef PCSX2_C89_EMITTER
	static __fi void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags = false)
	{ shim_LEA64(to, src, preserve_flags); }
	static __fi void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags = false)
	{ shim_LEA32(to, src, preserve_flags); }
	// xLEA(xRegister16) stays on the reference. EmitLeaMagic's peephole
	// rewrites emit their inner MOV and ADD at the destination's width, so a
	// 16-bit LEA produces a 0x66 on the outer write *and* on each inner
	// operation -- three prefixes in an eight-byte sequence. E_LEA_SZ drives
	// its inner ops from one width flag and cannot express that yet. It has
	// no callers in the recompilers; leaving it out costs nothing and
	// guessing at it would not be verifiable.
	extern void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags = false);
#else
	extern void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags = false);
	extern void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags = false);
	extern void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags = false);
#endif

	// ----- Push / Pop Instructions  -----
	// Note: pushad/popad implementations are intentionally left out.  The instructions are
	// invalid in x64, and are super slow on x32.  Use multiple Push/Pop instructions instead.

#ifdef PCSX2_C89_EMITTER
	static __fi void xPOP(const xIndirectVoid& from) { shim_xPOP(from); }
#else
	extern void xPOP(const xIndirectVoid& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xPUSH(const xIndirectVoid& from) { shim_xPUSH(from); }
#else
	extern void xPUSH(const xIndirectVoid& from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xPOP(xRegister32or64 from) { shim_xPOP(from); }
#else
	extern void xPOP(xRegister32or64 from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xPUSH(u32 imm) { shim_xPUSH(imm); }
#else
	extern void xPUSH(u32 imm);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xPUSH(xRegister32or64 from) { shim_xPUSH(from); }

#ifdef PCSX2_C89_EMITTER
	// xFastCall for the switched build. Every overload is composition over
	// names bound above -- xMOV, xLEA, xCALL, xPUSH, xPOP -- which is why
	// this is defined here, after all of them, rather than in the shim
	// header, which is included before those names exist. Bodies are
	// transcribed from jmp.cpp; the argument-shuffle in prepare() guards the
	// case where a1 already sits in arg2reg and a2 in arg1reg, which a naive
	// pair of MOVs would clobber.
	struct shim_FastCallObj
	{
		template <typename Reg1, typename Reg2>
		__fi void prepare(const Reg1& a1, const Reg2& a2) const
		{
			if (a1.IsEmpty())
				return;
			if (a2.Id != arg1reg.Id)
			{
				xMOV(Reg1(arg1reg), a1);
				if (!a2.IsEmpty())
					xMOV(Reg2(arg2reg), a2);
			}
			else if (a1.Id != arg2reg.Id)
			{
				xMOV(Reg2(arg2reg), a2);
				xMOV(Reg1(arg1reg), a1);
			}
			else
			{
				xPUSH(a1);
				xMOV(Reg2(arg2reg), a2);
				xPOP(Reg1(arg1reg));
			}
		}

		__fi void callNear(const void* f) const
		{
			const sptr disp = ((sptr)x86Ptr + 5) - (sptr)f;
			if (disp == (sptr)(s32)disp)
			{
				xCALL(f);
			}
			else
			{
				xLEA(rax, ptr64[f]);
				xCALL(rax);
			}
		}

		__fi void operator()(const void* f, const xRegister32& a1 = xEmptyReg, const xRegister32& a2 = xEmptyReg) const
		{
			prepare(a1, a2);
			callNear(f);
		}
		__fi void operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2 = xEmptyReg) const
		{
			prepare(a1, a2);
			callNear(f);
		}
		__fi void operator()(const void* f, u32 a1, const xRegisterLong& a2) const
		{
			if (!a2.IsEmpty())
				xMOV(arg2reg, a2);
			xMOV(arg1reg, a1);
			(*this)(f, arg1reg, arg2reg);
		}
		__fi void operator()(const void* f, void* a1) const
		{
			xLEA(arg1reg, ptr[a1]);
			(*this)(f, arg1reg, arg2reg);
		}
		__fi void operator()(const void* f, u32 a1, const xRegister32& a2) const
		{
			if (!a2.IsEmpty())
				xMOV(arg2regd, a2);
			xMOV(arg1regd, a1);
			(*this)(f, arg1regd, arg2regd);
		}
		__fi void operator()(const void* f, const xIndirect32& a1) const
		{
			xMOV(arg1regd, a1);
			(*this)(f, arg1regd);
		}
		__fi void operator()(const void* f, u32 a1, u32 a2) const
		{
			xMOV(arg1regd, a1);
			xMOV(arg2regd, a2);
			(*this)(f, arg1regd, arg2regd);
		}
		__fi void operator()(const xIndirectNative& f, const xRegisterLong& a1, const xRegisterLong& a2 = xEmptyReg) const
		{
			prepare(a1, a2);
			xCALL(f);
		}
		template <typename T>
		__fi void operator()(T* func, u32 a1, const xRegisterLong& a2 = xEmptyReg) const
		{
			(*this)((const void*)func, a1, a2);
		}
		template <typename T>
		__fi void operator()(T* func, const xIndirect32& a1) const
		{
			(*this)((const void*)func, a1);
		}
		template <typename T>
		__fi void operator()(T* func, u32 a1, u32 a2) const
		{
			(*this)((const void*)func, a1, a2);
		}
	};
	static const shim_FastCallObj xFastCall = {};
#endif

#else
	extern void xPUSH(xRegister32or64 from);
#endif

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

#ifdef PCSX2_C89_EMITTER
	// Address and constant helpers: composition over xLEA/xMOV/xMOV64,
	// moved verbatim from x86emitter.cpp so the switched build needs no
#ifndef PCSX2_C89_COMPAT_HELPERS

	// object file for them. Explicit inline: __fi is empty on MinGW.
	inline xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset)
	{
		if ((sptr)base == (s32)(sptr)base)
		{
			return offset + base;
		}
		else
		{
			xLEA(tmpRegister, ptr[base]);
			return offset + tmpRegister;
		}
	}

	inline void xLoadFarAddr(const xAddressReg& dst, void* addr)
	{
		sptr iaddr = (sptr)addr;
		sptr rip = (sptr)x86Ptr + 7; // LEA will be 7 bytes
		sptr disp = iaddr - rip;
		if (disp == (s32)disp)
		{
			xLEA(dst, ptr[addr]);
		}
		else
		{
			xMOV64(dst, iaddr);
		}
	}

	inline void xWriteImm64ToMem(u64* addr, const xAddressReg& tmp, u64 imm)
	{
		xImm64Op(xMOV, ptr64[addr], tmp, imm);
	}
#endif /* !PCSX2_C89_COMPAT_HELPERS */

#endif

	//////////////////////////////////////////////////////////////////////////////////////////
	// JMP / Jcc Instructions!

	// ------------------------------------------------------------------------
	// Emits a 32 bit jump, and returns a pointer to the 32 bit displacement.
	// (displacements should be assigned relative to the end of the jump instruction,
	// or in other words *(retval+1) )
	inline s32* xJcc32(JccComparisonType comparison, s32 displacement)
	{
		if (comparison == Jcc_Unconditional)
		{
			xWrite8(0xe9);
		}
		else
		{
			xWrite8(0x0f);
			xWrite8(0x80 | comparison);
		}
		{
			const s32 disp32_ = (s32)displacement;
			memcpy(x86Ptr, &disp32_, sizeof(s32));
		}
		x86Ptr += sizeof(s32);

		return ((s32*)x86Ptr) - 1;
	}

	// ------------------------------------------------------------------------
	// Writes a jump at the current x86Ptr, which targets a pre-established target address.
	// (usually a backwards jump)
	//
	// slideForward - used internally by xSmartJump to indicate that the jump target is going
	// to slide forward in the event of an 8 bit displacement.
	//
	inline void xJccKnownTarget(JccComparisonType comparison, const void* target, bool slideForward)
	{
		// Calculate the potential j8 displacement first, assuming an instruction length of 2:
		sptr displacement8 = (sptr)target - (sptr)(x86Ptr + 2);

		if (is_s8(displacement8))
		{
			xWrite8((comparison == Jcc_Unconditional) ? 0xeb : (0x70 | comparison));
			*(s8*)x86Ptr = displacement8;
			x86Ptr += sizeof(s8);
		}
		else
		{
			// Perform a 32 bit jump instead. :(
			s32* bah = xJcc32(comparison, 0);
			sptr distance = (sptr)target - (sptr)x86Ptr;

			{
				const s32 dist32_ = (s32)distance;
				memcpy(bah, &dist32_, sizeof(s32));
			}
		}
	}

#ifndef PCSX2_C89_COMPAT_HELPERS
	// returns the inverted conditional type for this Jcc condition.  Ie, JNS will become JS.
	inline JccComparisonType xInvertCond(JccComparisonType src)
	{
		return (src == Jcc_Unconditional) ? Jcc_Unconditional : (JccComparisonType)((int)src ^ 1);
	}

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
#endif /* !PCSX2_C89_COMPAT_HELPERS */

	// ------------------------------------------------------------------------

#ifdef PCSX2_C89_EMITTER
	static __fi void xLDMXCSR(const xIndirect32& src) { shim_LDMXCSR(src); }
#else
	extern void xLDMXCSR(const xIndirect32& src);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVDZX(const xRegisterSSE& to, const xRegister32or64& from) { shim_xMOVDZX(to, from); }
#else
	extern void xMOVDZX(const xRegisterSSE& to, const xRegister32or64& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVDZX(const xRegisterSSE& to, const xIndirectVoid& src) { shim_xMOVDZX(to, src); }
#else
	extern void xMOVDZX(const xRegisterSSE& to, const xIndirectVoid& src);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVD(const xRegister32or64& to, const xRegisterSSE& from) { shim_xMOVD(to, from); }
#else
	extern void xMOVD(const xRegister32or64& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVD(const xIndirectVoid& dest, const xRegisterSSE& from) { shim_xMOVD(dest, from); }
#else
	extern void xMOVD(const xIndirectVoid& dest, const xRegisterSSE& from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVQ(const xIndirectVoid& dest, const xRegisterSSE& from) { shim_xMOVQ(dest, from); }
#else
	extern void xMOVQ(const xIndirectVoid& dest, const xRegisterSSE& from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVQZX(const xRegisterSSE& to, const xIndirectVoid& src) { shim_xMOVQZX(to, src); }
#else
	extern void xMOVQZX(const xRegisterSSE& to, const xIndirectVoid& src);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVQZX(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xMOVQZX(to, from); }
#else
	extern void xMOVQZX(const xRegisterSSE& to, const xRegisterSSE& from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVSS(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xMOVSS(to, from); }
#else
	extern void xMOVSS(const xRegisterSSE& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVSS(const xIndirectVoid& to, const xRegisterSSE& from) { shim_xMOVSS(to, from); }
#else
	extern void xMOVSS(const xIndirectVoid& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVSD(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xMOVSD(to, from); }
#else
	extern void xMOVSD(const xRegisterSSE& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVSD(const xIndirectVoid& to, const xRegisterSSE& from) { shim_xMOVSD(to, from); }
#else
	extern void xMOVSD(const xIndirectVoid& to, const xRegisterSSE& from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVSSZX(const xRegisterSSE& to, const xIndirectVoid& from) { shim_xMOVSSZX(to, from); }
#else
	extern void xMOVSSZX(const xRegisterSSE& to, const xIndirectVoid& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVSDZX(const xRegisterSSE& to, const xIndirectVoid& from) { shim_xMOVSDZX(to, from); }
#else
	extern void xMOVSDZX(const xRegisterSSE& to, const xIndirectVoid& from);
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xMOVMSKPS(const xRegister32& to, const xRegisterSSE& from) { shim_MOVMSKPS(to, from); }
#else
	extern void xMOVMSKPS(const xRegister32& to, const xRegisterSSE& from);
#endif
	extern void xMOVMSKPD(const xRegister32& to, const xRegisterSSE& from);

	// ------------------------------------------------------------------------

#ifdef PCSX2_C89_EMITTER
	static const shim_MoveSSE xMOVAPS = {0x00, true};
#else
	extern const xImplSimd_MoveSSE xMOVAPS;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_MoveSSE xMOVUPS = {0x00, false};
#else
	extern const xImplSimd_MoveSSE xMOVUPS;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_MoveDQ  xMOVDQA = {0x66, true};
#else
	extern const xImplSimd_MoveDQ xMOVDQA;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_MoveDQ  xMOVDQU = {0xf3, false};
#else
	extern const xImplSimd_MoveDQ xMOVDQU;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_MovHL xMOVH = {0x16};
#else
	extern const xImplSimd_MovHL xMOVH;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_MovHL xMOVL = {0x12};
#else
	extern const xImplSimd_MovHL xMOVL;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_MovHL_RtoR xMOVLH = {0x16};
#else
	extern const xImplSimd_MovHL_RtoR xMOVLH;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_MovHL_RtoR xMOVHL = {0x12};
#else
	extern const xImplSimd_MovHL_RtoR xMOVHL;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_PBlend xPBLEND = {
		{0x66, 0x0e3a}, // W
		{0x66, 0x1038}, // VB
	};
#else
	extern const xImplSimd_PBlend xPBLEND;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_Blend xBLEND = {
		{0x66, 0x0c3a}, // PS
		{0x66, 0x0d3a}, // PD
		{0x66, 0x1438}, // VPS
		{0x66, 0x1538}, // VPD
	};
#else
	extern const xImplSimd_Blend xBLEND;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PMove xPMOVSX = {0x2038};
#else
	extern const xImplSimd_PMove xPMOVSX;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PMove xPMOVZX = {0x3038};
#else
	extern const xImplSimd_PMove xPMOVZX;
#endif

#ifdef PCSX2_C89_EMITTER
	static __fi void xINSERTPS(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm8) { shim_xINSERTPS(to, from, imm8); }
#else
	extern void xINSERTPS(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm8);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xINSERTPS(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) { shim_xINSERTPS(to, from, imm8); }
#else
	extern void xINSERTPS(const xRegisterSSE& to, const xIndirect32& from, u8 imm8);

#endif
#ifndef PCSX2_C89_EMITTER
	extern void xEXTRACTPS(const xRegister32or64& to, const xRegisterSSE& from, u8 imm8);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xEXTRACTPS(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) { shim_xEXTRACTPS(dest, from, imm8); }
#else
	extern void xEXTRACTPS(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8);

#endif
	// ------------------------------------------------------------------------

#ifdef PCSX2_C89_EMITTER
	static const shim_SimdRegSSE xPAND = {0x66, 0xdb};
#else
	extern const xImplSimd_DestRegEither xPAND;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_SimdRegSSE xPANDN = {0x66, 0xdf};
#else
	extern const xImplSimd_DestRegEither xPANDN;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_SimdRegSSE xPOR = {0x66, 0xeb};
#else
	extern const xImplSimd_DestRegEither xPOR;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_SimdRegSSE xPXOR = {0x66, 0xef};
#else
	extern const xImplSimd_DestRegEither xPXOR;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_Shuffle xSHUF;
#else
	extern const xImplSimd_Shuffle xSHUF;
#endif

	// ------------------------------------------------------------------------

#ifdef PCSX2_C89_EMITTER
	static const shim_SimdRegSSE xPTEST = {0x66, 0x1738};
#else
	extern const xImplSimd_DestRegSSE xPTEST;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_MinMax xMIN = {
			{0x00, 0x5d}, // PS
			{0x66, 0x5d}, // PD
			{0xf3, 0x5d}, // SS
			{0xf2, 0x5d}, // SD
	};
#else
	extern const xImplSimd_MinMax xMIN;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_MinMax xMAX = {
			{0x00, 0x5f}, // PS
			{0x66, 0x5f}, // PD
			{0xf3, 0x5f}, // SS
			{0xf2, 0x5f}, // SD
	};
#else
	extern const xImplSimd_MinMax xMAX;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_SimdCompare xCMPEQ = {SSE2_Equal};
	static const shim_SimdCompare xCMPLT = {SSE2_Less};
	static const shim_SimdCompare xCMPNLT = {SSE2_NotLess};
	static const shim_SimdCompare xCMPNLE = {SSE2_NotLessOrEqual};
#else
	extern const xImplSimd_Compare xCMPEQ, xCMPLT,
		xCMPNLT,
		xCMPNLE;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_COMI xUCOMI = { {0x00,0x2e},{0x66,0x2e} };
#else
	extern const xImplSimd_COMI xUCOMI;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_PCompare xPCMP = { {0x66,0x74},{0x66,0x75},{0x66,0x76},{0x66,0x64},{0x66,0x65},{0x66,0x66} };
#else
	extern const xImplSimd_PCompare xPCMP;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PMinMax xPMIN = {
			{0x66, 0xda}, // UB
			{0x66, 0xea}, // SW
			{0x66, 0x3838}, // SB
			{0x66, 0x3938}, // SD

			{0x66, 0x3a38}, // UW
			{0x66, 0x3b38}, // UD
	};
#else
	extern const xImplSimd_PMinMax xPMIN;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PMinMax xPMAX = {
			{0x66, 0xde}, // UB
			{0x66, 0xee}, // SW
			{0x66, 0x3c38}, // SB
			{0x66, 0x3d38}, // SD

			{0x66, 0x3e38}, // UW
			{0x66, 0x3f38}, // UD
	};
#else
	extern const xImplSimd_PMinMax xPMAX;
#endif

	// ------------------------------------------------------------------------
	//
	//
	extern void xCVTDQ2PD(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xCVTDQ2PD(const xRegisterSSE& to, const xIndirect64& from);
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTDQ2PS(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xCVTDQ2PS(to, from); }
#else
	extern void xCVTDQ2PS(const xRegisterSSE& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTDQ2PS(const xRegisterSSE& to, const xIndirect128& from) { shim_xCVTDQ2PS(to, from); }
#else
	extern void xCVTDQ2PS(const xRegisterSSE& to, const xIndirect128& from);

#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSD2SS(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xCVTSD2SS(to, from); }
#else
	extern void xCVTSD2SS(const xRegisterSSE& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSD2SS(const xRegisterSSE& to, const xIndirect64& from) { shim_xCVTSD2SS(to, from); }
#else
	extern void xCVTSD2SS(const xRegisterSSE& to, const xIndirect64& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSI2SS(const xRegisterSSE& to, const xRegister32or64& from) { shim_xCVTSI2SS(to, from); }
#else
	extern void xCVTSI2SS(const xRegisterSSE& to, const xRegister32or64& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSI2SS(const xRegisterSSE& to, const xIndirect32& from) { shim_xCVTSI2SS(to, from); }
#else
	extern void xCVTSI2SS(const xRegisterSSE& to, const xIndirect32& from);

#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSS2SD(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xCVTSS2SD(to, from); }
#else
	extern void xCVTSS2SD(const xRegisterSSE& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSS2SD(const xRegisterSSE& to, const xIndirect32& from) { shim_xCVTSS2SD(to, from); }
#else
	extern void xCVTSS2SD(const xRegisterSSE& to, const xIndirect32& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSS2SI(const xRegister32or64& to, const xRegisterSSE& from) { shim_xCVTSS2SI(to, from); }
#else
	extern void xCVTSS2SI(const xRegister32or64& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTSS2SI(const xRegister32or64& to, const xIndirect32& from) { shim_xCVTSS2SI(to, from); }
#else
	extern void xCVTSS2SI(const xRegister32or64& to, const xIndirect32& from);

#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTTPS2DQ(const xRegisterSSE& to, const xRegisterSSE& from) { shim_xCVTTPS2DQ(to, from); }
#else
	extern void xCVTTPS2DQ(const xRegisterSSE& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTTPS2DQ(const xRegisterSSE& to, const xIndirect128& from) { shim_xCVTTPS2DQ(to, from); }
#else
	extern void xCVTTPS2DQ(const xRegisterSSE& to, const xIndirect128& from);

#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTTSD2SI(const xRegister32or64& to, const xRegisterSSE& from) { shim_xCVTTSD2SI(to, from); }
#else
	extern void xCVTTSD2SI(const xRegister32or64& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTTSD2SI(const xRegister32or64& to, const xIndirect64& from) { shim_xCVTTSD2SI(to, from); }
#else
	extern void xCVTTSD2SI(const xRegister32or64& to, const xIndirect64& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTTSS2SI(const xRegister32or64& to, const xRegisterSSE& from) { shim_xCVTTSS2SI(to, from); }
#else
	extern void xCVTTSS2SI(const xRegister32or64& to, const xRegisterSSE& from);
#endif
#ifdef PCSX2_C89_EMITTER
	static __fi void xCVTTSS2SI(const xRegister32or64& to, const xIndirect32& from) { shim_xCVTTSS2SI(to, from); }
#else
	extern void xCVTTSS2SI(const xRegister32or64& to, const xIndirect32& from);

#endif
	// ------------------------------------------------------------------------

#ifdef PCSX2_C89_EMITTER
	static const shim_Sqrt xSQRT = {
			{0x00, 0x51}, // PS
			{0xf3, 0x51}, // SS
			{0xf2, 0x51} // SS
	};
#else
	extern const xImplSimd_Sqrt xSQRT;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_Shift xPSLL = { {0x66,0xf1,0x71,6}, {0x66,0xf2,0x72,6}, {0x66,0xf3,0x73,6} };
#else
	extern const xImplSimd_Shift xPSLL;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_Shift xPSRL = { {0x66,0xd1,0x71,2}, {0x66,0xd2,0x72,2}, {0x66,0xd3,0x73,2} };
#else
	extern const xImplSimd_Shift xPSRL;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_ShiftNoQ xPSRA = { {0x66,0xe1,0x71,4}, {0x66,0xe2,0x72,4} };
#else
	extern const xImplSimd_ShiftWithoutQ xPSRA;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_AddSub xPADD = { {0x66,0xdc+0x20},{0x66,0xdc+0x21},{0x66,0xdc+0x22},{0x66,0xd4},{0x66,0xdc+0x10},{0x66,0xdc+0x11},{0x66,0xdc},{0x66,0xdc+1} };
#else
	extern const xImplSimd_AddSub xPADD;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_AddSub xPSUB = { {0x66,0xd8+0x20},{0x66,0xd8+0x21},{0x66,0xd8+0x22},{0x66,0xfb},{0x66,0xd8+0x10},{0x66,0xd8+0x11},{0x66,0xd8},{0x66,0xd8+1} };
#else
	extern const xImplSimd_AddSub xPSUB;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PMul xPMUL = { {0x66,0xd5},{0x66,0xe5},{0x66,0xe4},{0x66,0xf4},{0x66,0x0b38},{0x66,0x4038},{0x66,0x2838} };
#else
	extern const xImplSimd_PMul xPMUL;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PAbsolute xPABS = {
			{0x66, 0x1c38}, // B
			{0x66, 0x1d38}, // W
			{0x66, 0x1e38} // D
	};
#else
	extern const xImplSimd_PAbsolute xPABS;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PMultAdd xPMADD = {
			{0x66, 0xf5}, // WD
			{0x66, 0xf438}, // UBSW
	};
#else
	extern const xImplSimd_PMultAdd xPMADD;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_DotProduct xDP = {
			{0x66, 0x403a}, // PS
			{0x66, 0x413a}, // PD
	};
#else
	extern const xImplSimd_DotProduct xDP;
#endif

#ifdef PCSX2_C89_EMITTER
	static const shim_PShuffle xPSHUF = { {0x66,0x70},{0xf2,0x70},{0xf3,0x70},{0x66,0x0038} };
#else
	extern const xImplSimd_PShuffle xPSHUF;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PUnpack xPUNPCK = { {0x66,0x60},{0x66,0x61},{0x66,0x62},{0x66,0x6c},{0x66,0x68},{0x66,0x69},{0x66,0x6a},{0x66,0x6d} };
#else
	extern const SimdImpl_PUnpack xPUNPCK;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_Unpack xUNPCK = {
			{0x00, 0x15}, // HPS
			{0x66, 0x15}, // HPD
			{0x00, 0x14}, // LPS
			{0x66, 0x14}, // LPD
	};
#else
	extern const xImplSimd_Unpack xUNPCK;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_Pack xPACK = {
			{0x66, 0x63}, // SSWB
			{0x66, 0x6b}, // SSDW
			{0x66, 0x67}, // USWB
			{0x66, 0x2b38}, // USDW
	};
#else
	extern const SimdImpl_Pack xPACK;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PInsert xPINSR;
#else
	extern const xImplSimd_PInsert xPINSR;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_PExtract xPEXTR;
#else
	extern const SimdImpl_PExtract xPEXTR;
#endif

	// ------------------------------------------------------------------------

#ifdef PCSX2_C89_EMITTER
	static const shim_AVXMove xVMOVAPS = {0x00, 0x28, 0x29};
#else
	extern const xImplAVX_Move xVMOVAPS;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_AVXMove xVMOVUPS = {0x00, 0x10, 0x11};
#else
	extern const xImplAVX_Move xVMOVUPS;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_AVXThreeArg xVPAND = {0x66, 0xDB};
#else
	extern const xImplAVX_ThreeArgYMM xVPAND;
#endif
#ifdef PCSX2_C89_EMITTER
	static const shim_AVXCmpInt xVPCMP = { {0x66,0x74},{0x66,0x75},{0x66,0x76},{0x66,0x64},{0x66,0x65},{0x66,0x66} };
#else
	extern const xImplAVX_CmpInt xVPCMP;
#endif

	extern void xVMOVMSKPS(const xRegister32& to, const xRegisterSSE& from);
	extern void xVMOVMSKPD(const xRegister32& to, const xRegisterSSE& from);

} // namespace x86Emitter

// ---------------------------------------------------------------------------
//  C89 emitter switchover (opt-in)
// ---------------------------------------------------------------------------
// Included here, at the end, so everything the shim references -- xLEA in
// particular -- is already declared. Each instruction object is independent,
// so families migrate one at a time; only group1 is switched today and the
// rest still go through the reference emitter.
// instructions.h carries no include guard of its own, so this block needs
// one: the objects below are definitions, not declarations, and a second
// pass through the file would redefine them.
#if defined(PCSX2_C89_EMITTER) && !defined(PCSX2_C89_EMITTER_BOUND)
#define PCSX2_C89_EMITTER_BOUND 1

namespace x86Emitter
{
	static const shim_Group1 xADC = {G1Type_ADC};
	static const shim_Group1 xCMP = {G1Type_CMP};

	static const shim_G1Logic xAND = {{G1Type_AND}, {0x00, 0x54}, {0x66, 0x54}};
	static const shim_G1Logic xOR  = {{G1Type_OR},  {0x00, 0x56}, {0x66, 0x56}};
	static const shim_G1Logic xXOR = {{G1Type_XOR}, {0x00, 0x57}, {0x66, 0x57}};

	static const shim_G1Arith xADD = {{G1Type_ADD}, {0x00, 0x58}, {0x66, 0x58}, {0xf3, 0x58}, {0xf2, 0x58}};
	static const shim_G1Arith xSUB = {{G1Type_SUB}, {0x00, 0x5c}, {0x66, 0x5c}, {0xf3, 0x5c}, {0xf2, 0x5c}};

	static const shim_Group2 xROL = {G2Type_ROL};
	static const shim_Group2 xROR = {G2Type_ROR};
	static const shim_Group2 xRCL = {G2Type_RCL};
	static const shim_Group2 xRCR = {G2Type_RCR};
	static const shim_Group2 xSHL = {G2Type_SHL};
	static const shim_Group2 xSHR = {G2Type_SHR};
	static const shim_Group2 xSAR = {G2Type_SAR};

	static const shim_Group3 xNOT  = {G3Type_NOT};
	static const shim_Group3 xNEG  = {G3Type_NEG};
	static const shim_Group3 xUMUL = {G3Type_MUL};
	static const shim_Group3 xUDIV = {G3Type_DIV};
} // namespace x86Emitter
#endif

// ---------------------------------------------------------------------------
//  C89 emitter switchover (opt-in, off by default)
// ---------------------------------------------------------------------------
// Included at the end so everything the shim references is already declared;
// xLEA in particular, which the far-call path needs and which is declared
// above. Each instruction object is independent, so families can move across
// one at a time -- only group1 does today, and the rest still go through the
// reference emitter.
//
// Equivalence is checked by tests/emitter/switch_equiv.cpp, which compiles
// one driver both ways and diffs the emitted bytes. That is a different
// question from the two oracles: they compare the emitters side by side in a
// single build, whereas with this flag set the reference is not present in
// the translation unit at all.
// instructions.h has no include guard of its own and is pulled into a single
// translation unit many times over; the objects below are definitions, not
// declarations, so this block needs one of its own.
#if defined(PCSX2_C89_EMITTER) && !defined(PCSX2_C89_EMITTER_BOUND)
#define PCSX2_C89_EMITTER_BOUND 1
#include "tests/emitter/reference/x86emitter_shim.h"

namespace x86Emitter
{
	static const shim_Group1 xADC = {G1Type_ADC};
	static const shim_Group1 xCMP = {G1Type_CMP};

	static const shim_G1Logic xAND = {{G1Type_AND}, {0x00, 0x54}, {0x66, 0x54}};
	static const shim_G1Logic xOR  = {{G1Type_OR},  {0x00, 0x56}, {0x66, 0x56}};
	static const shim_G1Logic xXOR = {{G1Type_XOR}, {0x00, 0x57}, {0x66, 0x57}};

	static const shim_G1Arith xADD = {{G1Type_ADD}, {0x00, 0x58}, {0x66, 0x58}, {0xf3, 0x58}, {0xf2, 0x58}};
	static const shim_G1Arith xSUB = {{G1Type_SUB}, {0x00, 0x5c}, {0x66, 0x5c}, {0xf3, 0x5c}, {0xf2, 0x5c}};
} // namespace x86Emitter
#endif
