/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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
 * ix86 core v0.9.1
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

#include "x86emitter.h"
#include "../VectorIntrin.h"

//------------------------------------------------------------------
// Legacy Helper Macros and Functions (depreciated)
//------------------------------------------------------------------

using namespace x86Emitter;

// warning: suggest braces around initialization of subobject [-Wmissing-braces]
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif

#define xCVTDQ2PD(to, from)  xOpWrite0F(0xf3, 0xe6, to, from)

// ------------------------------------------------------------------------
// Notes on Thread Local Storage:
//  * TLS is pretty simple, and "just works" from a programmer perspective, with only
//    some minor additional computational overhead (see performance notes below).
//
//  * MSVC and GCC handle TLS differently internally, but behavior to the programmer is
//    generally identical.
//
// Performance Considerations:
//  * GCC's implementation involves an extra dereference from normal storage (possibly
//    applies to x86-32 only -- x86-64 is untested).
//
//  * MSVC's implementation involves *two* extra dereferences from normal storage because
//    it has to look up the TLS heap pointer from the Windows Thread Storage Area.  (in
//    generated ASM code, this dereference is denoted by access to the fs:[2ch] address),
//
//  * However, in either case, the optimizer usually optimizes it to a register so the
//    extra overhead is minimal over a series of instructions.
//
// MSVC Notes:
//  * Important!! the Full Optimization [/Ox] option effectively disables TLS optimizations
//    in MSVC 2008 and earlier, causing generally significant code bloat.  Not tested in
//    VC2010 yet.
//
//  * VC2010 generally does a superior job of optimizing TLS across inlined functions and
//    class methods, compared to predecessors.
//


thread_local u8* x86Ptr;
thread_local XMMSSEType g_xmmtypes[iREGCNT_XMM] = {XMMT_INT};

// ------------------------------------------------------------------------
//                         Begin SSE-Only Part!
// ------------------------------------------------------------------------

__fi void SSE_SUBSS_XMM_to_XMM (int to, int from) { xSUB.SS(xRegisterSSE(to), xRegisterSSE(from)); }
__fi void SSE_ADDSS_XMM_to_XMM (int to, int from) { xADD.SS(xRegisterSSE(to), xRegisterSSE(from)); }
__fi void SSE_MINSS_XMM_to_XMM (int to, int from) { xMIN.SS(xRegisterSSE(to), xRegisterSSE(from)); }
__fi void SSE_MAXSS_XMM_to_XMM (int to, int from) { xMAX.SS(xRegisterSSE(to), xRegisterSSE(from)); }
__fi void SSE2_SUBSD_XMM_to_XMM(int to, int from) { xSUB.SD(xRegisterSSE(to), xRegisterSSE(from)); }
__fi void SSE2_ADDSD_XMM_to_XMM(int to, int from) { xADD.SD(xRegisterSSE(to), xRegisterSSE(from)); }

namespace x86Emitter
{
	const xImplSimd_DestRegEither xPAND = {0x66, 0xdb};
	const xImplSimd_DestRegEither xPANDN = {0x66, 0xdf};
	const xImplSimd_DestRegEither xPOR = {0x66, 0xeb};
	const xImplSimd_DestRegEither xPXOR = {0x66, 0xef};

	// [SSE-4.1] Performs a bitwise AND of dest against src, and sets the ZF flag
	// only if all bits in the result are 0.  PTEST also sets the CF flag according
	// to the following condition: (xmm2/m128 AND NOT xmm1) == 0;
	const xImplSimd_DestRegSSE xPTEST = {0x66, 0x1738};

	// ------------------------------------------------------------------------

	void xImplSimd_DestRegSSE::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(Prefix, Opcode, to, from); }
	void xImplSimd_DestRegSSE::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(Prefix, Opcode, to, from); }

	void xImplSimd_DestRegImmSSE::operator()(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm) const { xOpWrite0F(Prefix, Opcode, to, from, imm); }
	void xImplSimd_DestRegImmSSE::operator()(const xRegisterSSE& to, const xIndirectVoid& from, u8 imm) const { xOpWrite0F(Prefix, Opcode, to, from, imm); }


	void xImplSimd_DestRegEither::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(Prefix, Opcode, to, from); }
	void xImplSimd_DestRegEither::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(Prefix, Opcode, to, from); }


	void xImplSimd_DestSSE_CmpImm::operator()(const xRegisterSSE& to, const xRegisterSSE& from, SSE2_ComparisonType imm) const { xOpWrite0F(Prefix, Opcode, to, from, imm); }
	void xImplSimd_DestSSE_CmpImm::operator()(const xRegisterSSE& to, const xIndirectVoid& from, SSE2_ComparisonType imm) const { xOpWrite0F(Prefix, Opcode, to, from, imm); }

	// =====================================================================================================
	//  SIMD Arithmetic Instructions
	// =====================================================================================================

	void _SimdShiftHelper::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(Prefix, Opcode, to, from); }
	void _SimdShiftHelper::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(Prefix, Opcode, to, from); }


	void _SimdShiftHelper::operator()(const xRegisterSSE& to, u8 imm8) const
	{
		xOpWrite0F(0x66, OpcodeImm, (int)Modcode, to);
		*(u8*)x86Ptr = imm8;
		x86Ptr += sizeof(u8);
	}

	void xImplSimd_Shift::DQ(const xRegisterSSE& to, u8 imm8) const
	{
		xOpWrite0F(0x66, 0x73, (int)Q.Modcode + 1, to, imm8);
	}


	const xImplSimd_ShiftWithoutQ xPSRA =
		{
			{0x66, 0xe1, 0x71, 4}, // W
			{0x66, 0xe2, 0x72, 4} // D
	};

	const xImplSimd_Shift xPSRL =
		{
			{0x66, 0xd1, 0x71, 2}, // W
			{0x66, 0xd2, 0x72, 2}, // D
			{0x66, 0xd3, 0x73, 2}, // Q
	};

	const xImplSimd_Shift xPSLL =
		{
			{0x66, 0xf1, 0x71, 6}, // W
			{0x66, 0xf2, 0x72, 6}, // D
			{0x66, 0xf3, 0x73, 6}, // Q
	};

	const xImplSimd_AddSub xPADD =
		{
			{0x66, 0xdc + 0x20}, // B
			{0x66, 0xdc + 0x21}, // W
			{0x66, 0xdc + 0x22}, // D
			{0x66, 0xd4}, // Q

			{0x66, 0xdc + 0x10}, // SB
			{0x66, 0xdc + 0x11}, // SW
			{0x66, 0xdc}, // USB
			{0x66, 0xdc + 1}, // USW
	};

	const xImplSimd_AddSub xPSUB =
		{
			{0x66, 0xd8 + 0x20}, // B
			{0x66, 0xd8 + 0x21}, // W
			{0x66, 0xd8 + 0x22}, // D
			{0x66, 0xfb}, // Q

			{0x66, 0xd8 + 0x10}, // SB
			{0x66, 0xd8 + 0x11}, // SW
			{0x66, 0xd8}, // USB
			{0x66, 0xd8 + 1}, // USW
	};

	const xImplSimd_PMul xPMUL =
		{
			{0x66, 0xd5}, // LW
			{0x66, 0xe5}, // HW
			{0x66, 0xe4}, // HUW
			{0x66, 0xf4}, // UDQ

			{0x66, 0x0b38}, // HRSW
			{0x66, 0x4038}, // LD
			{0x66, 0x2838}, // DQ
	};

	const xImplSimd_rSqrt xRSQRT =
		{
			{0x00, 0x52}, // PS
			{0xf3, 0x52} // SS
	};

	const xImplSimd_rSqrt xRCP =
		{
			{0x00, 0x53}, // PS
			{0xf3, 0x53} // SS
	};

	const xImplSimd_Sqrt xSQRT =
		{
			{0x00, 0x51}, // PS
			{0xf3, 0x51}, // SS
			{0xf2, 0x51} // SS
	};

	const xImplSimd_AndNot xANDN =
		{
			{0x00, 0x55}, // PS
			{0x66, 0x55} // PD
	};

	const xImplSimd_PAbsolute xPABS =
		{
			{0x66, 0x1c38}, // B
			{0x66, 0x1d38}, // W
			{0x66, 0x1e38} // D
	};

	const xImplSimd_PSign xPSIGN =
		{
			{0x66, 0x0838}, // B
			{0x66, 0x0938}, // W
			{0x66, 0x0a38}, // D
	};

	const xImplSimd_PMultAdd xPMADD =
		{
			{0x66, 0xf5}, // WD
			{0x66, 0xf438}, // UBSW
	};

	const xImplSimd_HorizAdd xHADD =
		{
			{0xf2, 0x7c}, // PS
			{0x66, 0x7c}, // PD
	};

	const xImplSimd_DotProduct xDP =
		{
			{0x66, 0x403a}, // PS
			{0x66, 0x413a}, // PD
	};

	const xImplSimd_Round xROUND =
		{
			{0x66, 0x083a}, // PS
			{0x66, 0x093a}, // PD
			{0x66, 0x0a3a}, // SS
			{0x66, 0x0b3a}, // SD
	};

	// =====================================================================================================
	//  SIMD Comparison Instructions
	// =====================================================================================================

	void xImplSimd_Compare::PS(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x00, 0xc2, to, from, (u8)CType); }
	void xImplSimd_Compare::PS(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(0x00, 0xc2, to, from, (u8)CType); }

	void xImplSimd_Compare::PD(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, 0xc2, to, from, (u8)CType); }
	void xImplSimd_Compare::PD(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(0x66, 0xc2, to, from, (u8)CType); }

	void xImplSimd_Compare::SS(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0xf3, 0xc2, to, from, (u8)CType); }
	void xImplSimd_Compare::SS(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(0xf3, 0xc2, to, from, (u8)CType); }

	void xImplSimd_Compare::SD(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0xf2, 0xc2, to, from, (u8)CType); }
	void xImplSimd_Compare::SD(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(0xf2, 0xc2, to, from, (u8)CType); }

	const xImplSimd_MinMax xMIN =
		{
			{0x00, 0x5d}, // PS
			{0x66, 0x5d}, // PD
			{0xf3, 0x5d}, // SS
			{0xf2, 0x5d}, // SD
	};

	const xImplSimd_MinMax xMAX =
		{
			{0x00, 0x5f}, // PS
			{0x66, 0x5f}, // PD
			{0xf3, 0x5f}, // SS
			{0xf2, 0x5f}, // SD
	};

	// [TODO] : Merge this into the xCMP class, so that they are notation as: xCMP.EQ

	const xImplSimd_Compare xCMPEQ = {SSE2_Equal};
	const xImplSimd_Compare xCMPLT = {SSE2_Less};
	const xImplSimd_Compare xCMPLE = {SSE2_LessOrEqual};
	const xImplSimd_Compare xCMPUNORD = {SSE2_LessOrEqual};
	const xImplSimd_Compare xCMPNE = {SSE2_NotEqual};
	const xImplSimd_Compare xCMPNLT = {SSE2_NotLess};
	const xImplSimd_Compare xCMPNLE = {SSE2_NotLessOrEqual};
	const xImplSimd_Compare xCMPORD = {SSE2_Ordered};

	const xImplSimd_COMI xCOMI =
		{
			{0x00, 0x2f}, // SS
			{0x66, 0x2f}, // SD
	};

	const xImplSimd_COMI xUCOMI =
		{
			{0x00, 0x2e}, // SS
			{0x66, 0x2e}, // SD
	};

	const xImplSimd_PCompare xPCMP =
		{
			{0x66, 0x74}, // EQB
			{0x66, 0x75}, // EQW
			{0x66, 0x76}, // EQD

			{0x66, 0x64}, // GTB
			{0x66, 0x65}, // GTW
			{0x66, 0x66}, // GTD
	};

	const xImplSimd_PMinMax xPMIN =
		{
			{0x66, 0xda}, // UB
			{0x66, 0xea}, // SW
			{0x66, 0x3838}, // SB
			{0x66, 0x3938}, // SD

			{0x66, 0x3a38}, // UW
			{0x66, 0x3b38}, // UD
	};

	const xImplSimd_PMinMax xPMAX =
		{
			{0x66, 0xde}, // UB
			{0x66, 0xee}, // SW
			{0x66, 0x3c38}, // SB
			{0x66, 0x3d38}, // SD

			{0x66, 0x3e38}, // UW
			{0x66, 0x3f38}, // UD
	};

	// =====================================================================================================
	//  SIMD Shuffle/Pack  (Shuffle puck?)
	// =====================================================================================================

	void xImplSimd_Shuffle::PS(const xRegisterSSE& to, const xRegisterSSE& from, u8 selector) const
	{
		xOpWrite0F(0, 0xc6, to, from, selector);
	}

	void xImplSimd_Shuffle::PS(const xRegisterSSE& to, const xIndirectVoid& from, u8 selector) const
	{
		xOpWrite0F(0, 0xc6, to, from, selector);
	}

	void xImplSimd_Shuffle::PD(const xRegisterSSE& to, const xRegisterSSE& from, u8 selector) const
	{
		xOpWrite0F(0x66, 0xc6, to, from, selector & 0x3);
	}

	void xImplSimd_Shuffle::PD(const xRegisterSSE& to, const xIndirectVoid& from, u8 selector) const
	{
		xOpWrite0F(0x66, 0xc6, to, from, selector & 0x3);
	}

	void xImplSimd_PInsert::B(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const { xOpWrite0F(0x66, 0x203a, to, from, imm8); }
	void xImplSimd_PInsert::B(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const { xOpWrite0F(0x66, 0x203a, to, from, imm8); }

	void xImplSimd_PInsert::W(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const { xOpWrite0F(0x66, 0xc4, to, from, imm8); }
	void xImplSimd_PInsert::W(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const { xOpWrite0F(0x66, 0xc4, to, from, imm8); }

	void xImplSimd_PInsert::D(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const { xOpWrite0F(0x66, 0x223a, to, from, imm8); }
	void xImplSimd_PInsert::D(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const { xOpWrite0F(0x66, 0x223a, to, from, imm8); }

	void xImplSimd_PInsert::Q(const xRegisterSSE& to, const xRegister64& from, u8 imm8) const { xOpWrite0F(0x66, 0x223a, to, from, imm8); }
	void xImplSimd_PInsert::Q(const xRegisterSSE& to, const xIndirect64& from, u8 imm8) const { xOpWrite0F(0x66, 0x223a, to, from, imm8); }

	void SimdImpl_PExtract::B(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x143a, from, to, imm8); }
	void SimdImpl_PExtract::B(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x143a, from, dest, imm8); }

	void SimdImpl_PExtract::W(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0xc5, from, to, imm8); }
	void SimdImpl_PExtract::W(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x153a, from, dest, imm8); }

	void SimdImpl_PExtract::D(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x163a, from, to, imm8); }
	void SimdImpl_PExtract::D(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x163a, from, dest, imm8); }

	void SimdImpl_PExtract::Q(const xRegister64& to, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x163a, from, to, imm8); }
	void SimdImpl_PExtract::Q(const xIndirect64& dest, const xRegisterSSE& from, u8 imm8) const { xOpWrite0F(0x66, 0x163a, from, dest, imm8); }

	const xImplSimd_Shuffle xSHUF = {};

	const xImplSimd_PShuffle xPSHUF =
		{
			{0x66, 0x70}, // D
			{0xf2, 0x70}, // LW
			{0xf3, 0x70}, // HW

			{0x66, 0x0038}, // B
	};

	const SimdImpl_PUnpack xPUNPCK =
		{
			{0x66, 0x60}, // LBW
			{0x66, 0x61}, // LWD
			{0x66, 0x62}, // LDQ
			{0x66, 0x6c}, // LQDQ

			{0x66, 0x68}, // HBW
			{0x66, 0x69}, // HWD
			{0x66, 0x6a}, // HDQ
			{0x66, 0x6d}, // HQDQ
	};

	const SimdImpl_Pack xPACK =
		{
			{0x66, 0x63}, // SSWB
			{0x66, 0x6b}, // SSDW
			{0x66, 0x67}, // USWB
			{0x66, 0x2b38}, // USDW
	};

	const xImplSimd_Unpack xUNPCK =
		{
			{0x00, 0x15}, // HPS
			{0x66, 0x15}, // HPD
			{0x00, 0x14}, // LPS
			{0x66, 0x14}, // LPD
	};

	const xImplSimd_PInsert xPINSR;
	const SimdImpl_PExtract xPEXTR;

	// =====================================================================================================
	//  SIMD Move And Blend Instructions
	// =====================================================================================================

	void xImplSimd_MovHL::PS(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(0, Opcode, to, from); }
	void xImplSimd_MovHL::PS(const xIndirectVoid& to, const xRegisterSSE& from) const { xOpWrite0F(0, Opcode + 1, from, to); }

	void xImplSimd_MovHL::PD(const xRegisterSSE& to, const xIndirectVoid& from) const { xOpWrite0F(0x66, Opcode, to, from); }
	void xImplSimd_MovHL::PD(const xIndirectVoid& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, Opcode + 1, from, to); }

	void xImplSimd_MovHL_RtoR::PS(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0, Opcode, to, from); }
	void xImplSimd_MovHL_RtoR::PD(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, Opcode, to, from); }

	static const u16 MovPS_OpAligned = 0x28; // Aligned [aps] form
	static const u16 MovPS_OpUnaligned = 0x10; // unaligned [ups] form

	void xImplSimd_MoveSSE::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
	{
		if (to != from)
			xOpWrite0F(Prefix, MovPS_OpAligned, to, from);
	}

	void xImplSimd_MoveSSE::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
	{
		// ModSib form is aligned if it's displacement-only and the displacement is aligned:
		bool isReallyAligned = isAligned || (((from.Displacement & 0x0f) == 0) && from.Index.IsEmpty() && from.Base.IsEmpty());

		xOpWrite0F(Prefix, isReallyAligned ? MovPS_OpAligned : MovPS_OpUnaligned, to, from);
	}

	void xImplSimd_MoveSSE::operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
	{
		// ModSib form is aligned if it's displacement-only and the displacement is aligned:
		bool isReallyAligned = isAligned || ((to.Displacement & 0x0f) == 0 && to.Index.IsEmpty() && to.Base.IsEmpty());
		xOpWrite0F(Prefix, isReallyAligned ? MovPS_OpAligned + 1 : MovPS_OpUnaligned + 1, from, to);
	}

	static const u8 MovDQ_PrefixAligned = 0x66; // Aligned [dqa] form
	static const u8 MovDQ_PrefixUnaligned = 0xf3; // unaligned [dqu] form

	void xImplSimd_MoveDQ::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
	{
		if (to != from)
			xOpWrite0F(MovDQ_PrefixAligned, 0x6f, to, from);
	}

	void xImplSimd_MoveDQ::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
	{
		// ModSib form is aligned if it's displacement-only and the displacement is aligned:
		bool isReallyAligned = isAligned || ((from.Displacement & 0x0f) == 0 && from.Index.IsEmpty() && from.Base.IsEmpty());
		xOpWrite0F(isReallyAligned ? MovDQ_PrefixAligned : MovDQ_PrefixUnaligned, 0x6f, to, from);
	}

	void xImplSimd_MoveDQ::operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
	{
		// ModSib form is aligned if it's displacement-only and the displacement is aligned:
		bool isReallyAligned = isAligned || ((to.Displacement & 0x0f) == 0 && to.Index.IsEmpty() && to.Base.IsEmpty());

		// use opcode 0x7f : alternate ModRM encoding (reverse src/dst)
		xOpWrite0F(isReallyAligned ? MovDQ_PrefixAligned : MovDQ_PrefixUnaligned, 0x7f, from, to);
	}

	void xImplSimd_PMove::BW(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, OpcodeBase, to, from); }
	void xImplSimd_PMove::BW(const xRegisterSSE& to, const xIndirect64& from) const { xOpWrite0F(0x66, OpcodeBase, to, from); }

	void xImplSimd_PMove::BD(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, OpcodeBase + 0x100, to, from); }
	void xImplSimd_PMove::BD(const xRegisterSSE& to, const xIndirect32& from) const { xOpWrite0F(0x66, OpcodeBase + 0x100, to, from); }

	void xImplSimd_PMove::BQ(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, OpcodeBase + 0x200, to, from); }
	void xImplSimd_PMove::BQ(const xRegisterSSE& to, const xIndirect16& from) const { xOpWrite0F(0x66, OpcodeBase + 0x200, to, from); }

	void xImplSimd_PMove::WD(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, OpcodeBase + 0x300, to, from); }
	void xImplSimd_PMove::WD(const xRegisterSSE& to, const xIndirect64& from) const { xOpWrite0F(0x66, OpcodeBase + 0x300, to, from); }

	void xImplSimd_PMove::WQ(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, OpcodeBase + 0x400, to, from); }
	void xImplSimd_PMove::WQ(const xRegisterSSE& to, const xIndirect32& from) const { xOpWrite0F(0x66, OpcodeBase + 0x400, to, from); }

	void xImplSimd_PMove::DQ(const xRegisterSSE& to, const xRegisterSSE& from) const { xOpWrite0F(0x66, OpcodeBase + 0x500, to, from); }
	void xImplSimd_PMove::DQ(const xRegisterSSE& to, const xIndirect64& from) const { xOpWrite0F(0x66, OpcodeBase + 0x500, to, from); }


	const xImplSimd_MoveSSE xMOVAPS = {0x00, true};
	const xImplSimd_MoveSSE xMOVUPS = {0x00, false};

#ifdef ALWAYS_USE_MOVAPS
	const xImplSimd_MoveSSE xMOVDQA = {0x00, true};
	const xImplSimd_MoveSSE xMOVAPD = {0x00, true};

	const xImplSimd_MoveSSE xMOVDQU = {0x00, false};
	const xImplSimd_MoveSSE xMOVUPD = {0x00, false};
#else
	const xImplSimd_MoveDQ xMOVDQA = {0x66, true};
	const xImplSimd_MoveSSE xMOVAPD = {0x66, true};

	const xImplSimd_MoveDQ xMOVDQU = {0xf3, false};
	const xImplSimd_MoveSSE xMOVUPD = {0x66, false};
#endif


	const xImplSimd_MovHL xMOVH = {0x16};
	const xImplSimd_MovHL xMOVL = {0x12};

	const xImplSimd_MovHL_RtoR xMOVLH = {0x16};
	const xImplSimd_MovHL_RtoR xMOVHL = {0x12};

	const xImplSimd_Blend xBLEND =
		{
			{0x66, 0x0c3a}, // PS
			{0x66, 0x0d3a}, // PD
			{0x66, 0x1438}, // VPS
			{0x66, 0x1538}, // VPD
	};

	const xImplSimd_PMove xPMOVSX = {0x2038};
	const xImplSimd_PMove xPMOVZX = {0x3038};

	// [SSE-3]
	const xImplSimd_DestRegSSE xMOVSLDUP = {0xf3, 0x12};

	// [SSE-3]
	const xImplSimd_DestRegSSE xMOVSHDUP = {0xf3, 0x16};

	//////////////////////////////////////////////////////////////////////////////////////////
	//

#define IMPLEMENT_xMOVS(ssd, prefix) \
	__fi void xMOV##ssd(const xRegisterSSE& to, const xRegisterSSE& from) \
	{ \
		if (to != from) \
			xOpWrite0F(prefix, 0x10, to, from); \
	} \
	__fi void xMOV##ssd##ZX(const xRegisterSSE& to, const xIndirectVoid& from) { xOpWrite0F(prefix, 0x10, to, from); } \
	__fi void xMOV##ssd(const xIndirectVoid& to, const xRegisterSSE& from) { xOpWrite0F(prefix, 0x11, from, to); }

	IMPLEMENT_xMOVS(SS, 0xf3)
	IMPLEMENT_xMOVS(SD, 0xf2)

	// --------------------------------------------------------------------------------------
	//  INSERTPS / EXTRACTPS   [SSE4.1 only!]
	// --------------------------------------------------------------------------------------
	// [TODO] these might be served better as classes, especially if other instructions use
	// the M32,sse,imm form (I forget offhand if any do).

	// [SSE-4.1] Insert a single-precision floating-point value from src into a specified
	// location in dest, and selectively zero out the data elements in dest according to
	// the mask  field in the immediate byte. The source operand can be a memory location
	// (32 bits) or an XMM register (lower 32 bits used).
	//
	// Imm8 provides three fields:
	//  * COUNT_S: The value of Imm8[7:6] selects the dword element from src.  It is 0 if
	//    the source is a memory operand.
	//  * COUNT_D: The value of Imm8[5:4] selects the target dword element in dest.
	//  * ZMASK: Each bit of Imm8[3:0] selects a dword element in dest to  be written
	//    with 0.0 if set to 1.
	//
	__fi void xINSERTPS(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm8) { xOpWrite0F(0x66, 0x213a, to, from, imm8); }
	__fi void xINSERTPS(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) { xOpWrite0F(0x66, 0x213a, to, from, imm8); }

	// [SSE-4.1] Extract a single-precision floating-point value from src at an offset
	// determined by imm8[1-0]*32. The extracted single precision floating-point value
	// is stored into the low 32-bits of dest (or at a 32-bit memory pointer).
	//
	__fi void xEXTRACTPS(const xRegister32or64& to, const xRegisterSSE& from, u8 imm8) { xOpWrite0F(0x66, 0x173a, to, from, imm8); }
	__fi void xEXTRACTPS(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) { xOpWrite0F(0x66, 0x173a, from, dest, imm8); }
	const xImpl_Mov xMOV;
	const xImpl_MovImm64 xMOV64;
	const xImpl_MovExtend xMOVSX = {true};
	const xImpl_MovExtend xMOVZX = {false};

	const xImpl_CMov xCMOVA      = {Jcc_Above};
	const xImpl_CMov xCMOVAE     = {Jcc_AboveOrEqual};
	const xImpl_CMov xCMOVB      = {Jcc_Below};
	const xImpl_CMov xCMOVBE     = {Jcc_BelowOrEqual};

	const xImpl_CMov xCMOVG      = {Jcc_Greater};
	const xImpl_CMov xCMOVGE     = {Jcc_GreaterOrEqual};
	const xImpl_CMov xCMOVL      = {Jcc_Less};
	const xImpl_CMov xCMOVLE     = {Jcc_LessOrEqual};

	const xImpl_CMov xCMOVZ      = {Jcc_Zero};
	const xImpl_CMov xCMOVE      = {Jcc_Equal};
	const xImpl_CMov xCMOVNZ     = {Jcc_NotZero};
	const xImpl_CMov xCMOVNE     = {Jcc_NotEqual};

	const xImpl_CMov xCMOVO      = {Jcc_Overflow};
	const xImpl_CMov xCMOVNO     = {Jcc_NotOverflow};
	const xImpl_CMov xCMOVC      = {Jcc_Carry};
	const xImpl_CMov xCMOVNC     = {Jcc_NotCarry};

	const xImpl_CMov xCMOVS      = {Jcc_Signed};
	const xImpl_CMov xCMOVNS     = {Jcc_Unsigned};
	const xImpl_CMov xCMOVPE     = {Jcc_ParityEven};
	const xImpl_CMov xCMOVPO     = {Jcc_ParityOdd};


	const xImpl_Set xSETA        = {Jcc_Above};
	const xImpl_Set xSETAE       = {Jcc_AboveOrEqual};
	const xImpl_Set xSETB        = {Jcc_Below};
	const xImpl_Set xSETBE       = {Jcc_BelowOrEqual};

	const xImpl_Set xSETG        = {Jcc_Greater};
	const xImpl_Set xSETGE       = {Jcc_GreaterOrEqual};
	const xImpl_Set xSETL        = {Jcc_Less};
	const xImpl_Set xSETLE       = {Jcc_LessOrEqual};

	const xImpl_Set xSETZ        = {Jcc_Zero};
	const xImpl_Set xSETE        = {Jcc_Equal};
	const xImpl_Set xSETNZ       = {Jcc_NotZero};
	const xImpl_Set xSETNE       = {Jcc_NotEqual};

	const xImpl_Set xSETO        = {Jcc_Overflow};
	const xImpl_Set xSETNO       = {Jcc_NotOverflow};
	const xImpl_Set xSETC        = {Jcc_Carry};
	const xImpl_Set xSETNC       = {Jcc_NotCarry};

	const xImpl_Set xSETS        = {Jcc_Signed};
	const xImpl_Set xSETNS       = {Jcc_Unsigned};
	const xImpl_Set xSETPE       = {Jcc_ParityEven};
	const xImpl_Set xSETPO       = {Jcc_ParityOdd};

	void xImpl_Mov::operator()(const xRegisterInt& to, const xRegisterInt& from) const
	{
		// FIXME WTF?
		if (to != from)
			_xMovRtoR(to, from);
	}

	void xImpl_Mov::operator()(const xIndirectVoid& dest, const xRegisterInt& from) const
	{
		// mov eax has a special from when writing directly to a DISP32 address
		// (sans any register index/base registers).

		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0x88 : 0x89, from, dest, 0);
	}

	void xImpl_Mov::operator()(const xRegisterInt& to, const xIndirectVoid& src) const
	{
		// mov eax has a special from when reading directly from a DISP32 address
		// (sans any register index/base registers).

		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0x8a : 0x8b, to, src, 0);
	}

	void xImpl_Mov::operator()(const xIndirect64orLess& dest, intptr_t imm) const
	{
		xOpWrite(dest.GetPrefix16(), dest.Is8BitOp() ? 0xc6 : 0xc7, 0, dest, dest.GetImmSize());
		dest.xWriteImm(imm);
	}

	// preserve_flags  - set to true to disable optimizations which could alter the state of
	//   the flags (namely replacing mov reg,0 with xor).
	void xImpl_Mov::operator()(const xRegisterInt& to, intptr_t imm, bool preserve_flags) const
	{
		const xRegisterInt& to_ = to.GetNonWide();
		if (!preserve_flags && (imm == 0))
		{
			_g1_EmitOp(G1Type_XOR, to_, to_);
		}
		else if (imm == (intptr_t)(u32)imm || !(to._operandSize == 8))
		{
			// Note: MOV does not have (reg16/32,imm8) forms.
			u8 opcode = (to_.Is8BitOp() ? 0xb0 : 0xb8) | to_.Id;
			xOpAccWrite(to_.GetPrefix16(), opcode, 0, to_);
			to_.xWriteImm(imm);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), 0xc7, 0, to, 0);
			to.xWriteImm(imm);
		}
	}


	void xImpl_MovImm64::operator()(const xRegister64& to, s64 imm, bool preserve_flags) const
	{
		if (imm == (u32)imm || imm == (s32)imm)
		{
			xMOV(to, imm, preserve_flags);
		}
		else
		{
			u8 opcode = 0xb8 | to.Id;
			xOpAccWrite(to.GetPrefix16(), opcode, 0, to);
			*(u64*)x86Ptr = imm;
			x86Ptr += sizeof(u64);
		}
	}

	// --------------------------------------------------------------------------------------
	//  CMOVcc
	// --------------------------------------------------------------------------------------

	void xImpl_CMov::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
	{
		xOpWrite0F(to->GetPrefix16(), 0x40 | ccType, to, from);
	}

	void xImpl_CMov::operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const
	{
		xOpWrite0F(to->GetPrefix16(), 0x40 | ccType, to, sibsrc);
	}

	void xImpl_Set::operator()(const xRegister8& to) const
	{
		xOpWrite0F(0, 0x90 | ccType, 0, to);
	}
	void xImpl_Set::operator()(const xIndirect8& dest) const
	{
		xOpWrite0F(0, 0x90 | ccType, 0, dest);
	}

	void xImpl_MovExtend::operator()(const xRegister16or32or64& to, const xRegister8& from) const
	{
		xOpWrite0F(
			(to->_operandSize == 2) ? 0x66 : 0,
			SignExtend ? 0xbe : 0xb6,
			to, from);
	}

	void xImpl_MovExtend::operator()(const xRegister16or32or64& to, const xIndirect8& sibsrc) const
	{
		xOpWrite0F(
			(to->_operandSize == 2) ? 0x66 : 0,
			SignExtend ? 0xbe : 0xb6,
			to, sibsrc);
	}

	void xImpl_MovExtend::operator()(const xRegister32or64& to, const xRegister16& from) const
	{
		xOpWrite0F(0, SignExtend ? 0xbf : 0xb7, to, from);
	}

	void xImpl_MovExtend::operator()(const xRegister32or64& to, const xIndirect16& sibsrc) const
	{
		xOpWrite0F(0, SignExtend ? 0xbf : 0xb7, to, sibsrc);
	}

	void xImpl_MovExtend::operator()(const xRegister64& to, const xRegister32& from) const
	{
		xOpWrite(0, 0x63, to, from, 0);
	}

	void xImpl_MovExtend::operator()(const xRegister64& to, const xIndirect32& sibsrc) const
	{
		xOpWrite(0, 0x63, to, sibsrc, 0);
	}
	const xImpl_JmpCall xJMP       = {true};
	const xImpl_JmpCall xCALL      = {false};
	const xImpl_FastCall xFastCall = {};

	void xImpl_JmpCall::operator()(const xAddressReg& absreg) const
	{
		// Jumps are always wide and don't need the rex.W
		xOpWrite(0, 0xff, isJmp ? 4 : 2, absreg.GetNonWide(), 0);
	}
	void xImpl_JmpCall::operator()(const xIndirectNative& src) const
	{
		// Jumps are always wide and don't need the rex.W
		EmitRex(0, xIndirect32(src.Base, src.Index, 1, 0));
		*(u8*)x86Ptr = 0xff;
		x86Ptr += sizeof(u8);
		EmitSibMagic(isJmp ? 4 : 2, src);
	}

	template <typename Reg1, typename Reg2>
	static void prepareRegsForFastcall(const Reg1& a1, const Reg2& a2)
	{
		// Make sure we don't mess up if someone tries to fastcall with a1 in arg2reg and a2 in arg1reg
		if (a2.Id != arg1reg.Id)
		{
			xMOV(Reg1(arg1reg), a1);
			if (!a2.IsEmpty())
			{
				xMOV(Reg2(arg2reg), a2);
			}
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

	void xImpl_FastCall::operator()(const void* f, const xRegister32& a1, const xRegister32& a2) const
	{
		if (!a1.IsEmpty())
			prepareRegsForFastcall(a1, a2);
		uintptr_t disp = ((uintptr_t)x86Ptr + 5) - (uintptr_t)f;
		if ((intptr_t)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		if (!a1.IsEmpty())
			prepareRegsForFastcall(a1, a2);
		uintptr_t disp = ((uintptr_t)x86Ptr + 5) - (uintptr_t)f;
		if ((intptr_t)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegisterLong& a2) const
	{
		if (!a2.IsEmpty())
		{
			xMOV(arg2reg, a2);
		}
		xMOV(arg1reg, a1);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, void* a1) const
	{
		xLEA(arg1reg, ptr[a1]);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegister32& a2) const
	{
		if (!a2.IsEmpty())
		{
			xMOV(arg2regd, a2);
		}
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const void* f, const xIndirect32& a1) const
	{
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, u32 a2) const
	{
		xMOV(arg1regd, a1);
		xMOV(arg2regd, a2);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const xIndirectNative& f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		if (!a1.IsEmpty())
			prepareRegsForFastcall(a1, a2);
		xCALL(f);
	}

	// ------------------------------------------------------------------------
	// Emits a 32 bit jump, and returns a pointer to the 32 bit displacement.
	// (displacements should be assigned relative to the end of the jump instruction,
	// or in other words *(retval+1) )
	__fi s32* xJcc32(JccComparisonType comparison, s32 displacement)
	{
		if (comparison == Jcc_Unconditional)
			*(u8*)x86Ptr = 0xe9;
		else
		{
			*(u8*)x86Ptr = 0x0f;
			x86Ptr += sizeof(u8);
			*(u8*)x86Ptr = 0x80 | comparison;
		}
		x86Ptr += sizeof(u8);
		*(s32*)x86Ptr = displacement;
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
	__fi void xJccKnownTarget(JccComparisonType comparison, const void* target)
	{
		// Calculate the potential j8 displacement first, assuming an instruction length of 2:
		intptr_t displacement8 = (intptr_t)target - (intptr_t)(x86Ptr + 2);

		if (is_s8(displacement8))
		{
			// Emits a 32 bit jump.
			// (displacements should be assigned relative to the end of the jump instruction,
			// or in other words *(retval+1) )
			*(u8*)x86Ptr = (comparison == Jcc_Unconditional) ? 0xeb : (0x70 | comparison);
			x86Ptr += sizeof(u8);
			*(s8*)x86Ptr = displacement8;
			x86Ptr += sizeof(s8);
		}
		else
		{
			// Perform a 32 bit jump instead. :(
			s32* bah      = xJcc32(comparison, 0);
			intptr_t distance = (intptr_t)target - (intptr_t)x86Ptr;
			*bah          = (s32)distance;
		}
	}

	xForwardJumpBase::xForwardJumpBase(uint opsize, JccComparisonType cctype)
	{
		BasePtr = (s8*)x86Ptr +
				  ((opsize == 1) ? 2 : // j8's are always 2 bytes.
                                   ((cctype == Jcc_Unconditional) ? 5 : 6)); // j32's are either 5 or 6 bytes

		if (opsize == 1)
			*(u8*)x86Ptr = (cctype == Jcc_Unconditional) ? 0xeb : (0x70 | cctype);
		else
		{
			if (cctype == Jcc_Unconditional)
				*(u8*)x86Ptr = 0xe9;
			else
			{
				*(u8*)x86Ptr = 0x0f;
				x86Ptr += sizeof(u8);
				*(u8*)x86Ptr = 0x80 | cctype;
			}
		}
		x86Ptr += sizeof(u8);
		x86Ptr += opsize;
	}
	const xImpl_Group8 xBT = {G8Type_BT};
	const xImpl_Group8 xBTR = {G8Type_BTR};
	const xImpl_Group8 xBTS = {G8Type_BTS};
	const xImpl_Group8 xBTC = {G8Type_BTC};

	const xImpl_G1Logic xAND = {G1Type_AND, {0x00, 0x54}, {0x66, 0x54}};
	const xImpl_G1Logic xOR = {G1Type_OR, {0x00, 0x56}, {0x66, 0x56}};
	const xImpl_G1Logic xXOR = {G1Type_XOR, {0x00, 0x57}, {0x66, 0x57}};

	const xImpl_G1Arith xADD = {G1Type_ADD, {0x00, 0x58}, {0x66, 0x58}, {0xf3, 0x58}, {0xf2, 0x58}};
	const xImpl_G1Arith xSUB = {G1Type_SUB, {0x00, 0x5c}, {0x66, 0x5c}, {0xf3, 0x5c}, {0xf2, 0x5c}};
	const xImpl_G1Compare xCMP = {{0x00, 0xc2}, {0x66, 0xc2}, {0xf3, 0xc2}, {0xf2, 0xc2}};

	const xImpl_Group1 xADC = {G1Type_ADC};
	const xImpl_Group1 xSBB = {G1Type_SBB};
	const xImpl_Group2 xROL = {G2Type_ROL};
	const xImpl_Group2 xROR = {G2Type_ROR};
	const xImpl_Group2 xRCL = {G2Type_RCL};
	const xImpl_Group2 xRCR = {G2Type_RCR};
	const xImpl_Group2 xSHL = {G2Type_SHL};
	const xImpl_Group2 xSHR = {G2Type_SHR};
	const xImpl_Group2 xSAR = {G2Type_SAR};

	const xImpl_Group3 xNOT = {G3Type_NOT};
	const xImpl_Group3 xNEG = {G3Type_NEG};
	const xImpl_Group3 xUMUL = {G3Type_MUL};
	const xImpl_Group3 xUDIV = {G3Type_DIV};

	const xImpl_iDiv xDIV = {{0x00, 0x5e}, {0x66, 0x5e}, {0xf3, 0x5e}, {0xf2, 0x5e}};
	const xImpl_iMul xMUL = {{0x00, 0x59}, {0x66, 0x59}, {0xf3, 0x59}, {0xf2, 0x59}};

	// =====================================================================================================
	//  Group 1 Instructions - ADD, SUB, ADC, etc.
	// =====================================================================================================

	// Note on "[Indirect],Imm" forms : use int as the source operand since it's "reasonably inert" from a
	// compiler perspective.  (using uint tends to make the compiler try and fail to match signed immediates
	// with one of the other overloads).
	static void _g1_IndirectImm(G1Type InstType, const xIndirect64orLess& sibdest, int imm)
	{
		if (sibdest.Is8BitOp())
		{
			xOpWrite(sibdest.GetPrefix16(), 0x80, InstType, sibdest, 1);

			*(s8*)x86Ptr = imm;
			x86Ptr += sizeof(s8);
		}
		else
		{
			bool is_signed = is_s8(imm);
			u8 opcode      = is_signed ? 0x83 : 0x81;
			xOpWrite(sibdest.GetPrefix16(), opcode, InstType, sibdest, is_signed ? 1 : sibdest.GetImmSize());

			if (is_signed)
			{
				*(s8*)x86Ptr  = imm;
				x86Ptr       += sizeof(s8);
			}
			else
				sibdest.xWriteImm(imm);
		}
	}

	void _g1_EmitOp(G1Type InstType, const xRegisterInt& to, const xRegisterInt& from)
	{
		u8 opcode = (to.Is8BitOp() ? 0 : 1) | (InstType << 3);
		xOpWrite(to.GetPrefix16(), opcode, from, to, 0);
	}

	static void _g1_EmitOp(G1Type InstType, const xIndirectVoid& sibdest, const xRegisterInt& from)
	{
		u8 opcode = (from.Is8BitOp() ? 0 : 1) | (InstType << 3);
		xOpWrite(from.GetPrefix16(), opcode, from, sibdest, 0);
	}

	static void _g1_EmitOp(G1Type InstType, const xRegisterInt& to, const xIndirectVoid& sibsrc)
	{
		u8 opcode = (to.Is8BitOp() ? 2 : 3) | (InstType << 3);
		xOpWrite(to.GetPrefix16(), opcode, to, sibsrc, 0);
	}

	static void _g1_EmitOp(G1Type InstType, const xRegisterInt& to, int imm)
	{
		if (!to.Is8BitOp() && is_s8(imm))
		{
			xOpWrite(to.GetPrefix16(), 0x83, InstType, to, 0);
			*(s8*)x86Ptr = imm;
			x86Ptr += sizeof(s8);
		}
		else
		{
			if (to.Id == 0)
			{
				u8 opcode = (to.Is8BitOp() ? 4 : 5) | (InstType << 3);
				xOpAccWrite(to.GetPrefix16(), opcode, InstType, to);
			}
			else
			{
				u8 opcode = to.Is8BitOp() ? 0x80 : 0x81;
				xOpWrite(to.GetPrefix16(), opcode, InstType, to, 0);
			}
			to.xWriteImm(imm);
		}
	}

#define ImplementGroup1(g1type, insttype)                                                                                 \
    void g1type::operator()(const xRegisterInt& to, const xRegisterInt& from) const { _g1_EmitOp(insttype, to, from); }   \
    void g1type::operator()(const xIndirectVoid& to, const xRegisterInt& from) const { _g1_EmitOp(insttype, to, from); }  \
    void g1type::operator()(const xRegisterInt& to, const xIndirectVoid& from) const { _g1_EmitOp(insttype, to, from); }  \
    void g1type::operator()(const xRegisterInt& to, int imm) const { _g1_EmitOp(insttype, to, imm); }                     \
    void g1type::operator()(const xIndirect64orLess& sibdest, int imm) const { _g1_IndirectImm(insttype, sibdest, imm); }

	ImplementGroup1(xImpl_Group1, InstType)
	ImplementGroup1(xImpl_G1Logic, InstType)
	ImplementGroup1(xImpl_G1Arith, InstType)
	ImplementGroup1(xImpl_G1Compare, G1Type_CMP)

	// =====================================================================================================
	//  Group 2 Instructions - SHR, SHL, etc.
	// =====================================================================================================

	void xImpl_Group2::operator()(const xRegisterInt& to, const xRegisterCL& from) const
	{
		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xd2 : 0xd3, InstType, to, 0);
	}

	void xImpl_Group2::operator()(const xRegisterInt& to, u8 imm) const
	{
		if (imm == 0)
			return;

		if (imm == 1)
		{
			// special encoding of 1's
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xd0 : 0xd1, InstType, to, 0);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xc0 : 0xc1, InstType, to, 0);
			*(u8*)x86Ptr = imm;
			x86Ptr += sizeof(u8);
		}
	}

	void xImpl_Group2::operator()(const xIndirect64orLess& sibdest, const xRegisterCL& from) const
	{
		xOpWrite(sibdest.GetPrefix16(), sibdest.Is8BitOp() ? 0xd2 : 0xd3, InstType, sibdest, 0);
	}

	void xImpl_Group2::operator()(const xIndirect64orLess& sibdest, u8 imm) const
	{
		if (imm == 0)
			return;

		if (imm == 1)
		{
			// special encoding of 1's
			xOpWrite(sibdest.GetPrefix16(), sibdest.Is8BitOp() ? 0xd0 : 0xd1, InstType, sibdest, 0);
		}
		else
		{
			xOpWrite(sibdest.GetPrefix16(), sibdest.Is8BitOp() ? 0xc0 : 0xc1, InstType, sibdest, 1);
			*(u8*)x86Ptr = imm;
			x86Ptr += sizeof(u8);
		}
	}

	// =====================================================================================================
	//  Group 3 Instructions - NOT, NEG, MUL, DIV
	// =====================================================================================================

	void xImpl_Group3::operator()(const xRegisterInt& from) const { xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, InstType, from, 0); }
	void xImpl_Group3::operator()(const xIndirect64orLess& from) const { xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, InstType, from, 0); }

	void xImpl_iDiv::operator()(const xRegisterInt& from) const { xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, G3Type_iDIV, from, 0); }
	void xImpl_iDiv::operator()(const xIndirect64orLess& from) const { xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, G3Type_iDIV, from, 0); }

	void xImpl_iMul::operator()(const xRegisterInt& from) const { xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, G3Type_iMUL, from, 0); }
	void xImpl_iMul::operator()(const xIndirect64orLess& from) const { xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, G3Type_iMUL, from, 0); }

	void xImpl_iMul::operator()(const xRegister32& to, const xRegister32& from) const { xOpWrite0F(0, 0xaf, to, from); }
	void xImpl_iMul::operator()(const xRegister32& to, const xIndirectVoid& src) const { xOpWrite0F(0, 0xaf, to, src); }
	void xImpl_iMul::operator()(const xRegister16& to, const xRegister16& from) const { xOpWrite0F(0x66, 0xaf, to, from); }
	void xImpl_iMul::operator()(const xRegister16& to, const xIndirectVoid& src) const { xOpWrite0F(0x66, 0xaf, to, src); }

	void xImpl_iMul::operator()(const xRegister32& to, const xRegister32& from, s32 imm) const
	{
		bool is_signed = is_s8(imm);
		xOpWrite0F(to.GetPrefix16(), is_signed ? 0x6b : 0x69, to, from, is_signed ? 1 : to.GetImmSize());

		if (is_signed)
		{
			*(u8*)x86Ptr = (u8)imm;
			x86Ptr += sizeof(u8);
		}
		else
			to.xWriteImm(imm);
	}

	void xImpl_iMul::operator()(const xRegister32& to, const xIndirectVoid& from, s32 imm) const
	{
		bool is_signed = is_s8(imm);
		xOpWrite0F(to.GetPrefix16(), is_signed ? 0x6b : 0x69, to, from, is_signed ? 1 : to.GetImmSize());

		if (is_signed)
		{
			*(u8*)x86Ptr = (u8)imm;
			x86Ptr += sizeof(u8);
		}
		else
			to.xWriteImm(imm);
	}

	void xImpl_iMul::operator()(const xRegister16& to, const xRegister16& from, s16 imm) const
	{
		bool is_signed = is_s8(imm);
		xOpWrite0F(to.GetPrefix16(), is_signed ? 0x6b : 0x69, to, from, is_signed ? 1 : to.GetImmSize());

		if (is_signed)
		{
			*(u8*)x86Ptr = (u8)imm;
			x86Ptr += sizeof(u8);
		}
		else
			to.xWriteImm(imm);
	}

	void xImpl_iMul::operator()(const xRegister16& to, const xIndirectVoid& from, s16 imm) const
	{
		bool is_signed = is_s8(imm);
		xOpWrite0F(to.GetPrefix16(), is_signed ? 0x6b : 0x69, to, from, is_signed ? 1 : to.GetImmSize());

		if (is_signed)
		{
			*(u8*)x86Ptr = (u8)imm;
			x86Ptr += sizeof(u8);
		}
		else
			to.xWriteImm(imm);
	}

	// =====================================================================================================
	//  Group 8 Instructions
	// =====================================================================================================

	void xImpl_Group8::operator()(const xRegister16or32or64& bitbase, const xRegister16or32or64& bitoffset) const
	{
		xOpWrite0F(bitbase->GetPrefix16(), 0xa3 | (InstType << 3), bitbase, bitoffset);
	}
	void xImpl_Group8::operator()(const xIndirect64& bitbase, u8 bitoffset) const { xOpWrite0F(0, 0xba, InstType, bitbase, bitoffset); }
	void xImpl_Group8::operator()(const xIndirect32& bitbase, u8 bitoffset) const { xOpWrite0F(0, 0xba, InstType, bitbase, bitoffset); }
	void xImpl_Group8::operator()(const xIndirect16& bitbase, u8 bitoffset) const { xOpWrite0F(0x66, 0xba, InstType, bitbase, bitoffset); }

	void xImpl_Group8::operator()(const xRegister16or32or64& bitbase, u8 bitoffset) const
	{
		xOpWrite0F(bitbase->GetPrefix16(), 0xba, InstType, bitbase, bitoffset);
	}

	void xImpl_Group8::operator()(const xIndirectVoid& bitbase, const xRegister16or32or64& bitoffset) const
	{
		xOpWrite0F(bitoffset->GetPrefix16(), 0xa3 | (InstType << 3), bitoffset, bitbase);
	}
	// Empty initializers are due to frivolously pointless GCC errors (it demands the
	// objects be initialized even though they have no actual variable members).

	const xAddressIndexer<xIndirectVoid> ptr = {};
	const xAddressIndexer<xIndirectNative> ptrNative = {};
	const xAddressIndexer<xIndirect128> ptr128 = {};
	const xAddressIndexer<xIndirect64> ptr64 = {};
	const xAddressIndexer<xIndirect32> ptr32 = {};
	const xAddressIndexer<xIndirect16> ptr16 = {};
	const xAddressIndexer<xIndirect8> ptr8 = {};

	// ------------------------------------------------------------------------

	const xRegisterEmpty xEmptyReg = {};

	// clang-format off

	const xRegisterSSE
		xmm0(0), xmm1(1),
		xmm2(2), xmm3(3),
		xmm4(4), xmm5(5),
		xmm6(6), xmm7(7),
		xmm8(8), xmm9(9),
		xmm10(10), xmm11(11),
		xmm12(12), xmm13(13),
		xmm14(14), xmm15(15);

	const xRegisterSSE
		ymm0(0, xRegisterYMMTag()), ymm1(1, xRegisterYMMTag()),
		ymm2(2, xRegisterYMMTag()), ymm3(3, xRegisterYMMTag()),
		ymm4(4, xRegisterYMMTag()), ymm5(5, xRegisterYMMTag()),
		ymm6(6, xRegisterYMMTag()), ymm7(7, xRegisterYMMTag()),
		ymm8(8, xRegisterYMMTag()), ymm9(9, xRegisterYMMTag()),
		ymm10(10, xRegisterYMMTag()), ymm11(11, xRegisterYMMTag()),
		ymm12(12, xRegisterYMMTag()), ymm13(13, xRegisterYMMTag()),
		ymm14(14, xRegisterYMMTag()), ymm15(15, xRegisterYMMTag());

	const xAddressReg
		rax(0), rbx(3),
		rcx(1), rdx(2),
		rsp(4), rbp(5),
		rsi(6), rdi(7),
		r8(8), r9(9),
		r10(10), r11(11),
		r12(12), r13(13),
		r14(14), r15(15);

	const xRegister32
		eax(0), ebx(3),
		ecx(1), edx(2),
		esp(4), ebp(5),
		esi(6), edi(7),
		r8d(8), r9d(9),
		r10d(10), r11d(11),
		r12d(12), r13d(13),
		r14d(14), r15d(15);

	const xRegister16
		ax(0), bx(3),
		cx(1), dx(2),
		sp(4), bp(5),
		si(6), di(7);

	const xRegister8
		al(0),
		dl(2), bl(3),
		ah(4), ch(5),
		dh(6), bh(7),
		spl(4, true), bpl(5, true),
		sil(6, true), dil(7, true),
		r8b(8), r9b(9),
		r10b(10), r11b(11),
		r12b(12), r13b(13),
		r14b(14), r15b(15);

#if defined(_WIN32)
	const xAddressReg
		arg1reg = rcx,
			arg2reg = rdx,
			arg3reg = r8,
			arg4reg = r9,
			calleeSavedReg1 = rdi,
			calleeSavedReg2 = rsi;

	const xRegister32
		arg1regd = ecx,
			 arg2regd = edx,
			 calleeSavedReg1d = edi,
			 calleeSavedReg2d = esi;
#else
	const xAddressReg
		arg1reg = rdi,
			arg2reg = rsi,
			arg3reg = rdx,
			arg4reg = rcx,
			calleeSavedReg1 = r12,
			calleeSavedReg2 = r13;

	const xRegister32
		arg1regd = edi,
			 arg2regd = esi,
			 calleeSavedReg1d = r12d,
			 calleeSavedReg2d = r13d;
#endif

	// clang-format on

	const xRegisterCL cl;

	void EmitSibMagic(uint regfield, const void* address, int extraRIPOffset)
	{
		intptr_t displacement = (intptr_t)address;
		intptr_t ripRelative = (intptr_t)address - ((intptr_t)x86Ptr + sizeof(s8) + sizeof(s32) + extraRIPOffset);
		// Can we use a rip-relative address?  (Prefer this over eiz because it's a byte shorter)
		if (ripRelative == (s32)ripRelative)
		{
			*(u8*)x86Ptr = (regfield << 3) | ModRm_UseDisp32;
			displacement = ripRelative;
		}
		else
		{
			*(u8*)x86Ptr = (regfield << 3) | ModRm_UseSib;
			x86Ptr += sizeof(u8);
			*(u8*)x86Ptr = (Sib_EIZ << 3) | Sib_UseDisp32;
		}
		x86Ptr += sizeof(u8);

		*(s32*)x86Ptr = (s32)displacement;
		x86Ptr       += sizeof(s32);
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// returns TRUE if this instruction requires SIB to be encoded, or FALSE if the
	// instruction can be encoded as ModRm alone.
	static __fi bool NeedsSibMagic(const xIndirectVoid& info)
	{
		// no registers? no sibs!
		// (xIndirectVoid::Reduce always places a register in Index, and optionally leaves
		// Base empty if only register is specified)
		if (!info.Index.IsEmpty())
		{
			// A scaled register needs a SIB
			if (info.Scale != 0)
				return true;
			// two registers needs a SIB
			if (!info.Base.IsEmpty())
				return true;
		}
		return false;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// Conditionally generates Sib encoding information!
	//
	// regfield - register field to be written to the ModRm.  This is either a register specifier
	//   or an opcode extension.  In either case, the instruction determines the value for us.
	//
	void EmitSibMagic(uint regfield, const xIndirectVoid& info, int extraRIPOffset)
	{
		// 3 bits also on x86_64 (so max is 8)
		// We might need to mask it on x86_64
		int displacement_size = (info.Displacement == 0)   ? 0 :
			((is_s8(info.Displacement)) ? 1 : 2);

		if (!NeedsSibMagic(info))
		{
			// Use ModRm-only encoding, with the rm field holding an index/base register, if
			// one has been specified.  If neither register is specified then use Disp32 form,
			// which is encoded as "EBP w/o displacement" (which is why EBP must always be
			// encoded *with* a displacement of 0, if it would otherwise not have one).

			if (info.Index.IsEmpty())
			{
				EmitSibMagic(regfield, (void*)info.Displacement, extraRIPOffset);
				return;
			}
			if (info.Index == rbp && displacement_size == 0)
				displacement_size = 1; // forces [ebp] to be encoded as [ebp+0]!

			*(u8*)x86Ptr = (displacement_size << 6) | (regfield << 3) | (info.Index.Id & 7);
		}
		else
		{
			// In order to encode "just" index*scale (and no base), we have to encode
			// it as a special [index*scale + displacement] form, which is done by
			// specifying EBP as the base register and setting the displacement field
			// to zero. (same as ModRm w/o SIB form above, basically, except the
			// ModRm_UseDisp flag is specified in the SIB instead of the ModRM field).

			if (info.Base.IsEmpty())
			{
				*(u8*)x86Ptr = (regfield << 3) | ModRm_UseSib;
				x86Ptr += sizeof(u8);
				*(u8*)x86Ptr = (info.Scale << 6) | (info.Index.Id << 3) | Sib_UseDisp32;
				x86Ptr += sizeof(u8);
				*(s32*)x86Ptr = info.Displacement;
				x86Ptr += sizeof(s32);
				return;
			}
			if (info.Base == rbp && displacement_size == 0)
				displacement_size = 1; // forces [ebp] to be encoded as [ebp+0]!

			*(u8*)x86Ptr = (displacement_size << 6) | (regfield << 3) | ModRm_UseSib;
			x86Ptr += sizeof(u8);
			*(u8*)x86Ptr = (info.Scale << 6) | ((info.Index.Id & 7) << 3) | (info.Base.Id & 7);
		}
		x86Ptr += sizeof(u8);

		if (displacement_size != 0)
		{
			if (displacement_size == 1)
			{
				*(s8*)x86Ptr = info.Displacement;
				x86Ptr += sizeof(s8);
			}
			else
			{
				*(s32*)x86Ptr = info.Displacement;
				x86Ptr += sizeof(s32);
			}
		}
	}

	// Writes a ModRM byte for "Direct" register access forms, which is used for all
	// instructions taking a form of [reg,reg].
	void EmitSibMagic(uint reg1, const xRegisterBase& reg2, int)
	{
		*(u8*)x86Ptr = (Mod_Direct << 6) | (reg1 << 3) | (reg2.Id & 7);
		x86Ptr += sizeof(u8);
	}

	void EmitSibMagic(const xRegisterBase& reg1, const xRegisterBase& reg2, int)
	{
		*(u8*)x86Ptr = (Mod_Direct << 6) | ((reg1.Id & 7) << 3) | (reg2.Id & 7);
		x86Ptr += sizeof(u8);
	}

	void EmitSibMagic(const xRegisterBase& reg1, const void* src, int extraRIPOffset)
	{
		EmitSibMagic(reg1.Id & 7, src, extraRIPOffset);
	}

	void EmitSibMagic(const xRegisterBase& reg1, const xIndirectVoid& sib, int extraRIPOffset)
	{
		EmitSibMagic(reg1.Id & 7, sib, extraRIPOffset);
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	void EmitRex(uint regfield, const void* address)
	{
	}

	void EmitRex(uint regfield, const xIndirectVoid& info)
	{
		bool w = info._operandSize == 8;
		bool r = false;
		bool x = info.Index.IsExtended();
		bool b = info.Base.IsExtended();
		if (!NeedsSibMagic(info))
		{
			b = x;
			x = false;
		}
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (x << 1) | (u8)b;
		if (rex != 0x40)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	void EmitRex(uint reg1, const xRegisterBase& reg2)
	{
		bool w       = reg2._operandSize == 8;
		bool b       = reg2.IsExtended();
		bool ext8bit = (reg2._operandSize == 1 && reg2.Id >= 0x10);
		const u8 rex = 0x40 | (w << 3) | (u8)b;
		if (rex != 0x40 || ext8bit)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	void EmitRex(const xRegisterBase& reg1, const xRegisterBase& reg2)
	{
		bool w       = (reg1._operandSize == 8) || (reg2._operandSize == 8);
		bool r       = reg1.IsExtended();
		bool b       = reg2.IsExtended();
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (u8)b;
		bool ext8bit = (reg2._operandSize == 1 && reg2.Id >= 0x10);
		if (rex != 0x40 || ext8bit)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	void EmitRex(const xRegisterBase& reg1, const void* src)
	{
		bool w       = reg1._operandSize == 8;
		bool r       = reg1.IsExtended();
		const u8 rex = 0x40 | (w << 3) | (r << 2);
		bool ext8bit = (reg1._operandSize == 1 && reg1.Id >= 0x10);
		if (rex != 0x40 || ext8bit)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	void EmitRex(const xRegisterBase& reg1, const xIndirectVoid& sib)
	{
		bool w = reg1._operandSize == 8 || sib._operandSize == 8;
		bool r = reg1.IsExtended();
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (x << 1) | (u8)b;
		bool ext8bit = (reg1._operandSize == 1 && reg1.Id >= 0x10);
		if (rex != 0x40 || ext8bit)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	// For use by instructions that are implicitly wide
	void EmitRexImplicitlyWide(const xRegisterBase& reg)
	{
		const u8 rex = 0x40 | (u8)reg.IsExtended();
		if (rex != 0x40)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	void EmitRexImplicitlyWide(const xIndirectVoid& sib)
	{
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		const u8 rex = 0x40 | (x << 1) | (u8)b;
		if (rex != 0x40)
		{
			*(u8*)x86Ptr = rex;
			x86Ptr += sizeof(u8);
		}
	}

	// --------------------------------------------------------------------------------------
	//  xRegisterInt  (method implementations)
	// --------------------------------------------------------------------------------------
	xRegisterInt xRegisterInt::MatchSizeTo(xRegisterInt other) const
	{
		return other._operandSize == 1 ? xRegisterInt(xRegister8(*this)) : xRegisterInt(other._operandSize, Id);
	}

	// --------------------------------------------------------------------------------------
	//  xAddressReg  (operator overloads)
	// --------------------------------------------------------------------------------------
	xAddressVoid xAddressReg::operator+(const xAddressReg& right) const
	{
		return xAddressVoid(*this, right);
	}

	xAddressVoid xAddressReg::operator+(intptr_t right) const
	{
		return xAddressVoid(*this, right);
	}

	xAddressVoid xAddressReg::operator+(const void* right) const
	{
		return xAddressVoid(*this, (intptr_t)right);
	}

	xAddressVoid xAddressReg::operator-(intptr_t right) const
	{
		return xAddressVoid(*this, -right);
	}

	xAddressVoid xAddressReg::operator-(const void* right) const
	{
		return xAddressVoid(*this, -(intptr_t)right);
	}

	xAddressVoid xAddressReg::operator*(int factor) const
	{
		return xAddressVoid(xEmptyReg, *this, factor);
	}

	xAddressVoid xAddressReg::operator<<(u32 shift) const
	{
		return xAddressVoid(xEmptyReg, *this, 1 << shift);
	}


	// --------------------------------------------------------------------------------------
	//  xAddressVoid  (method implementations)
	// --------------------------------------------------------------------------------------

	xAddressVoid::xAddressVoid(const xAddressReg& base, const xAddressReg& index, int factor, intptr_t displacement)
	{
		Base = base;
		Index = index;
		Factor = factor;
		Displacement = displacement;
	}

	xAddressVoid::xAddressVoid(const xAddressReg& index, intptr_t displacement)
	{
		Base = xEmptyReg;
		Index = index;
		Factor = 0;
		Displacement = displacement;
	}

	xAddressVoid::xAddressVoid(intptr_t displacement)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Factor = 0;
		Displacement = displacement;
	}

	xAddressVoid::xAddressVoid(const void* displacement)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Factor = 0;
		Displacement = (intptr_t)displacement;
	}

	xAddressVoid& xAddressVoid::Add(const xAddressReg& src)
	{
		if (src == Index)
			Factor++;
		else if (src == Base)
		{
			// Compound the existing register reference into the Index/Scale pair.
			Base = xEmptyReg;

			if (src == Index)
				Factor++;
			else
			{
				Index = src;
				Factor = 2;
			}
		}
		else if (Base.IsEmpty())
			Base = src;
		else if (Index.IsEmpty())
			Index = src;

		return *this;
	}

	xAddressVoid& xAddressVoid::Add(const xAddressVoid& src)
	{
		Add(src.Base);
		Add(src.Displacement);

		// If the factor is 1, we can just treat index like a base register also.
		if (src.Factor == 1)
			Add(src.Index);
		else if (Index.IsEmpty())
		{
			Index = src.Index;
			Factor = src.Factor;
		}
		else if (Index == src.Index)
			Factor += src.Factor;

		return *this;
	}

	xIndirectVoid::xIndirectVoid(const xAddressVoid& src)
	{
		Base = src.Base;
		Index = src.Index;
		Scale = src.Factor;
		Displacement = src.Displacement;

		Reduce();
	}

	xIndirectVoid::xIndirectVoid(intptr_t disp)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Scale = 0;
		Displacement = disp;

		// no reduction necessary :D
	}

	xIndirectVoid::xIndirectVoid(xAddressReg base, xAddressReg index, int scale, intptr_t displacement)
	{
		Base = base;
		Index = index;
		Scale = scale;
		Displacement = displacement;

		Reduce();
	}

	// Generates a 'reduced' ModSib form, which has valid Base, Index, and Scale values.
	// Necessary because by default ModSib compounds registers into Index when possible.
	//
	// If the ModSib is in illegal form ([Base + Index*5] for example) then an assertion
	// followed by an InvalidParameter Exception will be tossed around in haphazard
	// fashion.
	//
	// Optimization Note: Currently VC does a piss poor job of inlining this, even though
	// constant propagation *should* resove it to little or no code (VC's constprop fails
	// on C++ class initializers).  There is a work around [using array initializers instead]
	// but it's too much trouble for code that isn't performance critical anyway.
	// And, with luck, maybe VC10 will optimize it better and make it a non-issue. :D
	//
	void xIndirectVoid::Reduce()
	{
		if (Index.Id == 4) /* is stack pointer? */
		{
			// esp cannot be encoded as the index, so move it to the Base, if possible.
			// note: intentionally leave index assigned to esp also (generates correct
			// encoding later, since ESP cannot be encoded 'alone')
			Base = Index;
			return;
		}

		// If no index reg, then load the base register into the index slot.
		if (Index.IsEmpty())
		{
			Index = Base;
			Scale = 0;
			if (!(Base.Id == 4)) // prevent ESP from being encoded 'alone'
				Base = xEmptyReg;
			return;
		}

		// The Scale has a series of valid forms, all shown here:

		switch (Scale)
		{
			case 1:
				Scale = 0;
				break;
			case 3: // becomes [reg*2+reg]
				Base = Index;
				// fallthrough
			case 2:
				Scale = 1;
				break;
			case 5: // becomes [reg*4+reg]
				Base = Index;
				// fallthrough
			case 4:
				Scale = 2;
				break;

			case 9: // becomes [reg*8+reg]
				Base = Index;
				// fallthrough
			case 8:
				Scale = 3;
				break;
			case 0:
			case 6: // invalid!
			case 7: // so invalid!
			default:
				break;
		}
	}

	xIndirectVoid& xIndirectVoid::Add(intptr_t imm)
	{
		Displacement += imm;
		return *this;
	}

	// ------------------------------------------------------------------------
	// Internal implementation of EmitSibMagic which has been custom tailored
	// to optimize special forms of the Lea instructions accordingly, such
	// as when a LEA can be replaced with a "MOV reg,imm" or "MOV reg,reg".
	//
	// preserve_flags - set to ture to disable use of SHL on [Index*Base] forms
	// of LEA, which alters flags states.
	//
	static void EmitLeaMagic(const xRegisterInt& to, const xIndirectVoid& src, bool preserve_flags)
	{
		int displacement_size = (src.Displacement == 0)    ? 0 :
			((is_s8(src.Displacement)) ? 1 : 2);

		// See EmitSibMagic for commenting on SIB encoding.

		if (!NeedsSibMagic(src) && src.Displacement == (s32)src.Displacement)
		{
			// LEA Land: means we have either 1-register encoding or just an offset.
			// offset is encodable as an immediate MOV, and a register is encodable
			// as a register MOV.

			if (src.Index.IsEmpty())
			{
				xMOV(to, src.Displacement);
				return;
			}
			else if (displacement_size == 0)
			{
				const xRegisterInt& from = src.Index.MatchSizeTo(to);
				if (to != from)
					_xMovRtoR(to, from);
				return;
			}
			else if (!preserve_flags)
			{
				// encode as MOV and ADD combo.  Make sure to use the immediate on the
				// ADD since it can encode as an 8-bit sign-extended value.
				const xRegisterInt& from = src.Index.MatchSizeTo(to);
				if (to != from)
					_xMovRtoR(to, from);
				xADD(to, src.Displacement);
				return;
			}
		}
		else
		{
			if (src.Base.IsEmpty())
			{
				if (!preserve_flags && (displacement_size == 0))
				{
					// Encode [Index*Scale] as a combination of Mov and Shl.
					// This is more efficient because of the bloated LEA format which requires
					// a 32 bit displacement, and the compact nature of the alternative.
					//
					// (this does not apply to older model P4s with the broken barrel shifter,
					//  but we currently aren't optimizing for that target anyway).
					const xRegisterInt& from = src.Index;
					if (to != from)
						_xMovRtoR(to, from);
					xSHL(to, src.Scale);
					return;
				}
			}
			else
			{
				if (src.Scale == 0)
				{
					if (!preserve_flags)
					{
						if (src.Index == rsp)
						{
							// ESP is not encodable as an index (ix86 ignores it), thus:
							const xRegisterInt& from = src.Base.MatchSizeTo(to);
							if (to != from)
								_xMovRtoR(to, from); // will do the trick!
							if (src.Displacement)
								xADD(to, src.Displacement);
							return;
						}
						else if (src.Displacement == 0)
						{
							const xRegisterInt& from = src.Base.MatchSizeTo(to);
							if (to != from)
								_xMovRtoR(to, from);
							_g1_EmitOp(G1Type_ADD, to, src.Index.MatchSizeTo(to));
							return;
						}
					}
					else if ((src.Index == rsp) && (src.Displacement == 0))
					{
						// special case handling of ESP as Index, which is replaceable with
						// a single MOV even when preserve_flags is set! :D
						const xRegisterInt& from = src.Base.MatchSizeTo(to);
						if (to != from)
							_xMovRtoR(to, from);
						return;
					}
				}
			}
		}

		xOpWrite(0, 0x8d, to, src, 0);
	}

	__fi void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags)
	{
		EmitLeaMagic(to, src, preserve_flags);
	}

	__fi void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags)
	{
		EmitLeaMagic(to, src, preserve_flags);
	}

	__fi void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags)
	{
		*(u8*)x86Ptr = 0x66;
		x86Ptr += sizeof(u8);
		EmitLeaMagic(to, src, preserve_flags);
	}

	// =====================================================================================================
	//  TEST / INC / DEC
	// =====================================================================================================
	void xImpl_Test::operator()(const xRegisterInt& to, const xRegisterInt& from) const
	{
		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0x84 : 0x85, from, to, 0);
	}

	void xImpl_Test::operator()(const xIndirect64orLess& dest, int imm) const
	{
		xOpWrite(dest.GetPrefix16(), dest.Is8BitOp() ? 0xf6 : 0xf7, 0, dest, dest.GetImmSize());
		dest.xWriteImm(imm);
	}

	void xImpl_Test::operator()(const xRegisterInt& to, int imm) const
	{
		if (to.Id == 0)
		{
			xOpAccWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xa8 : 0xa9, 0, to);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xf6 : 0xf7, 0, to, 0);
		}
		to.xWriteImm(imm);
	}

	void xImpl_BitScan::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
	{
		xOpWrite0F(from->GetPrefix16(), Opcode, to, from);
	}
	void xImpl_BitScan::operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const
	{
		xOpWrite0F(to->GetPrefix16(), Opcode, to, sibsrc);
	}

	void xImpl_IncDec::operator()(const xRegisterInt& to) const
	{
		if (to.Is8BitOp())
		{
			u8 regfield = isDec ? 1 : 0;
			xOpWrite(to.GetPrefix16(), 0xfe, regfield, to, 0);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), 0xff, isDec ? 1 : 0, to, 0);
		}
	}

	void xImpl_IncDec::operator()(const xIndirect64orLess& to) const
	{
		if (to._operandSize == 2)
		{
			*(u8*)x86Ptr = 0x66;
			x86Ptr += sizeof(u8);
		}
		*(u8*)x86Ptr = to.Is8BitOp() ? 0xfe : 0xff;
		x86Ptr += sizeof(u8);
		EmitSibMagic(isDec ? 1 : 0, to);
	}

	void xImpl_DwordShift::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, const xRegisterCL& /* clreg */) const
	{
		xOpWrite0F(from->GetPrefix16(), OpcodeBase + 1, to, from);
	}

	void xImpl_DwordShift::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, u8 shiftcnt) const
	{
		if (shiftcnt != 0)
			xOpWrite0F(from->GetPrefix16(), OpcodeBase, to, from, shiftcnt);
	}

	void xImpl_DwordShift::operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, const xRegisterCL& /* clreg */) const
	{
		xOpWrite0F(from->GetPrefix16(), OpcodeBase + 1, from, dest);
	}

	void xImpl_DwordShift::operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, u8 shiftcnt) const
	{
		if (shiftcnt != 0)
			xOpWrite0F(from->GetPrefix16(), OpcodeBase, from, dest, shiftcnt);
	}

	const xImpl_Test xTEST = {};

	const xImpl_BitScan xBSF = {0xbc};
	const xImpl_BitScan xBSR = {0xbd};

	const xImpl_IncDec xINC = {false};
	const xImpl_IncDec xDEC = {true};

	const xImpl_DwordShift xSHLD = {0xa4};
	const xImpl_DwordShift xSHRD = {0xac};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Push / Pop Emitters
	//
	// Note: pushad/popad implementations are intentionally left out.  The instructions are
	// invalid in x64, and are super slow on x32.  Use multiple Push/Pop instructions instead.

	__fi void xPOP(const xIndirectVoid& from)
	{
		EmitRexImplicitlyWide(from);
		*(u8*)x86Ptr = 0x8f;
		x86Ptr += sizeof(u8);
		EmitSibMagic(0, from);
	}

	__fi void xPUSH(const xIndirectVoid& from)
	{
		EmitRexImplicitlyWide(from);
		*(u8*)x86Ptr = 0xff;
		x86Ptr += sizeof(u8);
		EmitSibMagic(6, from);
	}

	__fi void xPOP(xRegister32or64 from)
	{
		EmitRexImplicitlyWide(from);
		*(u8*)x86Ptr = 0x58 | (from->Id & 7);
		x86Ptr += sizeof(u8);
	}

	__fi void xPUSH(u32 imm)
	{
		if (is_s8(imm))
		{
			*(u8*)x86Ptr = 0x6a;
			x86Ptr += sizeof(u8);
			*(u8*)x86Ptr = imm;
			x86Ptr += sizeof(u8);
		}
		else
		{
			*(u8*)x86Ptr = 0x68;
			x86Ptr += sizeof(u8);
			*(u32*)x86Ptr = imm;
			x86Ptr += sizeof(u32);
		}
	}

	__fi void xPUSH(xRegister32or64 from)
	{
		EmitRexImplicitlyWide(from);
		*(u8*)x86Ptr = 0x50 | (from->Id & 7);
		x86Ptr += sizeof(u8);
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	//

	xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset)
	{
		if ((intptr_t)base == (s32)(intptr_t)base)
			return offset + base;
		xLEA(tmpRegister, ptr[base]);
		return offset + tmpRegister;
	}

	void xLoadFarAddr(const xAddressReg& dst, void* addr)
	{
		intptr_t iaddr = (intptr_t)addr;
		intptr_t rip   = (intptr_t)x86Ptr + 7; // LEA will be 7 bytes
		intptr_t disp  = iaddr - rip;
		if (disp == (s32)disp)
		{
			xLEA(dst, ptr[addr]);
		}
		else
		{
			xMOV64(dst, iaddr);
		}
	}
	const xImplAVX_Move xVMOVAPS = {0x00, 0x28, 0x29};
	const xImplAVX_Move xVMOVUPS = {0x00, 0x10, 0x11};

	const xImplAVX_ArithFloat xVADD = {
		{0x00, 0x58}, // VADDPS
		{0x66, 0x58}, // VADDPD
		{0xF3, 0x58}, // VADDSS
		{0xF2, 0x58}, // VADDSD
	};
	const xImplAVX_ArithFloat xVSUB = {
		{0x00, 0x5C}, // VSUBPS
		{0x66, 0x5C}, // VSUBPD
		{0xF3, 0x5C}, // VSUBSS
		{0xF2, 0x5C}, // VSUBSD
	};
	const xImplAVX_ArithFloat xVMUL = {
		{0x00, 0x59}, // VMULPS
		{0x66, 0x59}, // VMULPD
		{0xF3, 0x59}, // VMULSS
		{0xF2, 0x59}, // VMULSD
	};
	const xImplAVX_ArithFloat xVDIV = {
		{0x00, 0x5E}, // VDIVPS
		{0x66, 0x5E}, // VDIVPD
		{0xF3, 0x5E}, // VDIVSS
		{0xF2, 0x5E}, // VDIVSD
	};
	const xImplAVX_CmpFloat xVCMP = {
		{SSE2_Equal},
		{SSE2_Less},
		{SSE2_LessOrEqual},
		{SSE2_Unordered},
		{SSE2_NotEqual},
		{SSE2_NotLess},
		{SSE2_NotLessOrEqual},
		{SSE2_Ordered},
	};
	const xImplAVX_ThreeArgYMM xVPAND = {0x66, 0xDB};
	const xImplAVX_ThreeArgYMM xVPANDN = {0x66, 0xDF};
	const xImplAVX_ThreeArgYMM xVPOR = {0x66, 0xEB};
	const xImplAVX_ThreeArgYMM xVPXOR = {0x66, 0xEF};
	const xImplAVX_CmpInt xVPCMP = {
		{0x66, 0x74}, // VPCMPEQB
		{0x66, 0x75}, // VPCMPEQW
		{0x66, 0x76}, // VPCMPEQD
		{0x66, 0x64}, // VPCMPGTB
		{0x66, 0x65}, // VPCMPGTW
		{0x66, 0x66}, // VPCMPGTD
	};

	void xImplAVX_Move::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
	{
		if (to != from)
			xOpWriteC5(Prefix, LoadOpcode, to, xRegisterSSE(), from);
	}

	void xImplAVX_Move::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
	{
		xOpWriteC5(Prefix, LoadOpcode, to, xRegisterSSE(), from);
	}

	void xImplAVX_Move::operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
	{
		xOpWriteC5(Prefix, StoreOpcode, from, xRegisterSSE(), to);
	}

	void xImplAVX_ThreeArg::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_ThreeArg::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_ThreeArgYMM::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_ThreeArgYMM::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_CmpFloatHelper::PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0x00, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0x00, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0x66, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0x66, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0xF3, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0xF3, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0xF2, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}

	void xImplAVX_CmpFloatHelper::SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0xF2, 0xC2, to, from1, from2);
		*(u8*)x86Ptr = static_cast<u8>(CType);
		x86Ptr += sizeof(u8);
	}
} // namespace x86Emitter
