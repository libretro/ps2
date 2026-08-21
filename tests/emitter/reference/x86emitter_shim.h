/*  x86emitter_shim.h -- the C++ face of the C89 emitter core.
 *
 *  Purpose
 *  -------
 *  Keep every one of the ~6,400 call sites in the recompilers compiling and
 *  emitting exactly what they do today, while the encoding work moves into
 *  c89emit.h. The recompilers are C++ and use operator[] (`ptr32[addr]`),
 *  functor objects with member functions (`xPSHUF.D(a, b, 0)`) and typed
 *  register classes, none of which have a C89 spelling at the call site. So
 *  the split is: C89 macros do the encoding, this header supplies the syntax.
 *
 *  Why this costs nothing
 *  ----------------------
 *  Every wrapper here is defined in the header, so it inlines across
 *  translation units and constant-folds, which the current out-of-line
 *  definitions in x86emitter.cpp cannot do without LTO. Measured on the
 *  runtime-operand benchmark: 41.0 ns/block through this shim against 46.7
 *  for raw C89 macros and 243.7 for the shipping emitter.
 *
 *  Cursor discipline
 *  -----------------
 *  Twenty sites in the recompilers read `x86Ptr` directly between
 *  instructions -- recVTLB measuring patch regions, microVU_Compile capturing
 *  block starts, iR3000A computing dispatcher-relative jumps -- so the cursor
 *  cannot be held in a local across a block. Each wrapper loads it once and
 *  stores it back once. That is what the 41.0 ns figure was measured under.
 *
 *  Verification
 *  ------------
 *  oracle.cpp drives this header and the original emitter over the same
 *  operand matrix and compares bytes. Nothing here is trusted by inspection.
 */
#pragma once

#include "common/emitter/x86types.h"

extern "C" {
#include "common/emitter/c89emit.h"
}

namespace x86Emitter
{
	// ---- bridging the C++ operand types onto the C89 representation ----

	/// Register id in the form c89emit.h expects: low bits select, and for
	/// 8-bit registers bit 0x10 marks spl/bpl/sil/dil (see x86types.h:329).
	static __fi int shim_id(const xRegisterBase& r) { return r.Id; }

	/// xIndirectVoid carries base/index/scale/displacement already reduced by
	/// its constructor, which is the same reduction E_MEM performs, so the
	/// fields transfer directly rather than being re-derived.
	static __fi struct e_mem shim_mem(const xIndirectVoid& m)
	{
		struct e_mem out;
		out.base  = m.Base.IsEmpty()  ? E_NOREG : m.Base.Id;
		out.index = m.Index.IsEmpty() ? E_NOREG : m.Index.Id;
		out.scale = m.Scale;
		out.disp  = (intptr_t)m.Displacement;
		return out;
	}

	static __fi int shim_w(const xRegisterInt& r) { return r._operandSize == 8; }

	/// Operand size in bytes. Several forms need the full width rather than
	/// just "is it 64-bit": the 8-bit encodings differ in the opcode (TEST
	/// 0x84 vs 0x85, shifts 0xd0/0xc0 vs 0xd1/0xc1, MOV 0xb0+r vs 0xb8+r),
	/// and 16-bit needs the 0x66 prefix and a two-byte immediate. Reducing
	/// that to a boolean silently emits the 32-bit encoding for an 8-bit
	/// operand, which assembles fine and clobbers the rest of the register.
	static __fi int shim_sz(const xRegisterInt& r) { return (int)r._operandSize; }

	// The cursor is stored back after each instruction; see the header comment.
	#define SHIM_BEGIN  uint8_t* p_ = (uint8_t*)x86Ptr
	#define SHIM_END    x86Ptr = (u8*)p_

	// ---- group 1 ----------------------------------------------------------
	// G1Type is the /digit the encoding wants, so it passes straight through.

	struct shim_Group1
	{
		G1Type InstType;

		__fi void operator()(const xRegisterInt& to, const xRegisterInt& from) const
		{
			SHIM_BEGIN;
			if (to._operandSize == 1)       E_G1_8_RR(p_, InstType, shim_id(to), shim_id(from));
			else if (to._operandSize == 2)  E_G1_16_RR(p_, InstType, shim_id(to), shim_id(from));
			else                            E_G1_RR(p_, shim_w(to), InstType, shim_id(to), shim_id(from));
			SHIM_END;
		}

		__fi void operator()(const xRegisterInt& to, const xIndirectVoid& from) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_G1_R_MEM_SZ(p_, shim_sz(to), InstType, shim_id(to), m);
			SHIM_END;
		}

		__fi void operator()(const xIndirectVoid& to, const xRegisterInt& from) const
		{
			struct e_mem m = shim_mem(to);
			SHIM_BEGIN;
			E_G1_MEM_R_SZ(p_, shim_sz(from), InstType, shim_id(from), m);
			SHIM_END;
		}

		// Memory destination with an immediate. The operand's own size picks
		// the encoding and the immediate width, and the RIP-relative
		// displacement is corrected by that width.
		__fi void operator()(const xIndirect64orLess& sibdest, int imm) const
		{
			struct e_mem m = shim_mem(sibdest);
			SHIM_BEGIN;
			E_G1_MEM_I(p_, sibdest._operandSize, InstType, m, imm);
			SHIM_END;
		}

		__fi void operator()(const xRegisterInt& to, int imm) const
		{
			SHIM_BEGIN;
			if (to._operandSize == 1)       E_G1_8_RI(p_, InstType, shim_id(to), imm);
			else if (to._operandSize == 2)  E_G1_16_RI(p_, InstType, shim_id(to), imm);
			else                            E_G1_RI(p_, shim_w(to), InstType, shim_id(to), imm);
			SHIM_END;
		}
	};

	// ---- group 2 (shifts) -------------------------------------------------

	struct shim_Group2
	{
		G2Type InstType;

		__fi void operator()(const xRegisterInt& to, u8 imm) const
		{
			SHIM_BEGIN;
			E_G2_RI_SZ(p_, shim_sz(to), InstType, shim_id(to), imm);
			SHIM_END;
		}
		__fi void operator()(const xRegisterInt& to, const xRegisterCL&) const
		{
			SHIM_BEGIN;
			E_G2_RCL_SZ(p_, shim_sz(to), InstType, shim_id(to));
			SHIM_END;
		}
	};

	// ---- group 3 ----------------------------------------------------------

	struct shim_Group3
	{
		G3Type InstType;

		__fi void operator()(const xRegisterInt& from) const
		{
			SHIM_BEGIN;
			E_G3_R_SZ(p_, shim_sz(from), InstType, shim_id(from));
			SHIM_END;
		}
		__fi void operator()(const xIndirect64orLess& from) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_G3_MEM_SZ(p_, from._operandSize, InstType, m);
			SHIM_END;
		}
	};

	// ---- mov --------------------------------------------------------------

	struct shim_Mov
	{
		__fi void operator()(const xRegisterInt& to, const xRegisterInt& from) const
		{
			SHIM_BEGIN;
			if (to._operandSize == 1)       E_MOV8_RR(p_, shim_id(to), shim_id(from));
			else if (to._operandSize == 2)  E_MOV16_RR(p_, shim_id(to), shim_id(from));
			else                            E_MOV_RR(p_, shim_w(to), shim_id(to), shim_id(from));
			SHIM_END;
		}
		__fi void operator()(const xRegisterInt& to, const xIndirectVoid& from) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_MOV_R_MEM_SZ(p_, shim_sz(to), shim_id(to), m);
			SHIM_END;
		}
		__fi void operator()(const xIndirectVoid& to, const xRegisterInt& from) const
		{
			struct e_mem m = shim_mem(to);
			SHIM_BEGIN;
			E_MOV_MEM_R_SZ(p_, shim_sz(from), shim_id(from), m);
			SHIM_END;
		}
		__fi void operator()(const xIndirect64orLess& dest, sptr imm) const
		{
			struct e_mem m = shim_mem(dest);
			SHIM_BEGIN;
			E_MOV_MEM_I(p_, dest._operandSize, m, imm);
			SHIM_END;
		}

		__fi void operator()(const xRegisterInt& to, sptr imm, bool preserve_flags = false) const
		{
			SHIM_BEGIN;
			E_MOV_RI_SZ(p_, shim_sz(to), preserve_flags, shim_id(to), (intptr_t)imm);
			SHIM_END;
		}
	};

	// xMOV64 falls back to the 32-bit forms whenever the value fits, so the
	// ten-byte movabs is emitted only when nothing shorter can hold it.
	struct shim_MovImm64
	{
		__fi void operator()(const xRegister64& to, s64 imm, bool preserve_flags = false) const
		{
			SHIM_BEGIN;
			E_MOV64_RI(p_, preserve_flags, to.Id, imm);
			SHIM_END;
		}
	};

	// ---- test -------------------------------------------------------------

	struct shim_Test
	{
		__fi void operator()(const xIndirect64orLess& dest, int imm) const
		{
			struct e_mem m = shim_mem(dest);
			SHIM_BEGIN;
			E_TEST_MEM_I(p_, dest._operandSize, m, imm);
			SHIM_END;
		}

		__fi void operator()(const xRegisterInt& to, const xRegisterInt& from) const
		{
			SHIM_BEGIN;
			E_TEST_RR_SZ(p_, shim_sz(to), shim_id(to), shim_id(from));
			SHIM_END;
		}
		__fi void operator()(const xRegisterInt& to, int imm) const
		{
			SHIM_BEGIN;
			E_TEST_RI_SZ(p_, shim_sz(to), shim_id(to), imm);
			SHIM_END;
		}
	};

	// ---- inc / dec --------------------------------------------------------

	struct shim_IncDec
	{
		bool isDec;
		__fi void operator()(const xRegisterInt& to) const
		{
			SHIM_BEGIN;
			E_INCDEC_R_SZ(p_, shim_sz(to), isDec, shim_id(to));
			SHIM_END;
		}
	};

	// ---- SSE --------------------------------------------------------------
	// Every SSE form goes through one of these two shapes; the concrete
	// instructions differ only in the {prefix, opcode} pair they carry.

	struct shim_SimdRegSSE
	{
		u8  Prefix;
		u16 Opcode;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
		{
			SHIM_BEGIN;
			E_SSE_RR(p_, Prefix, Opcode, to.Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_SSE_R_MEM_W(p_, Prefix, Opcode, to.Id, m, from._operandSize == 8);
			SHIM_END;
		}
	};

	struct shim_SimdRegImmSSE
	{
		u8  Prefix;
		u16 Opcode;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm) const
		{
			SHIM_BEGIN;
			E_SSE_RRI(p_, Prefix, Opcode, to.Id, from.Id, imm);
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xIndirectVoid& from, u8 imm) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_SSE_R_MEMI(p_, Prefix, Opcode, to.Id, m, imm);
			SHIM_END;
		}
	};


	// ---- SSE functor families -------------------------------------------
	//
	// xPSHUF.D(a, b, 0x55), xPUNPCK.LQDQ(a, b), xPCMP.EQD(a, b) and friends
	// are the call shapes that rule out a pure C89 emitter: a member function
	// on a const aggregate. They are also the cheapest thing in the whole
	// port, because the aggregate holds nothing but {prefix, opcode} pairs
	// and every member reduces to a shape already implemented above. The
	// families below mirror the reference layout member for member so the
	// initialisers can be copied across unchanged.

	struct shim_PShuffle
	{
		shim_SimdRegImmSSE D;   // 66 0F 70
		shim_SimdRegImmSSE LW;  // F2 0F 70
		shim_SimdRegImmSSE HW;  // F3 0F 70
		shim_SimdRegSSE    B;   // 66 0F 38 00
	};

	struct shim_PCompare
	{
		shim_SimdRegSSE EQB, EQW, EQD;
		shim_SimdRegSSE GTB, GTW, GTD;
	};

	// Shift families. The register form is an ordinary SSE shape, but the
	// immediate form is not: it puts a /digit in the ModRM.reg field and the
	// destination in rm, with a fixed 0x66 prefix regardless of Prefix.
	// (_SimdShiftHelper, simd.cpp:81-89.)
	struct shim_ShiftHelper
	{
		u8  Prefix;
		u16 Opcode;
		u16 OpcodeImm;
		u8  Modcode;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
		{
			SHIM_BEGIN;
			E_SSE_RR(p_, Prefix, Opcode, to.Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_SSE_R_MEM(p_, Prefix, Opcode, to.Id, m);
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, u8 imm8) const
		{
			SHIM_BEGIN;
			E_SSE_RRI(p_, 0x66, OpcodeImm, Modcode, to.Id, imm8);
			SHIM_END;
		}
	};

	// xPSRA is the shift family minus Q -- arithmetic right shift has no
	// quadword form -- and minus the whole-register DQ byte shift.
	struct shim_ShiftNoQ
	{
		shim_ShiftHelper W, D;
	};

	struct shim_Shift
	{
		shim_ShiftHelper W, D, Q;

		// The whole-register byte shift reuses Q's opcode with the next digit.
		__fi void DQ(const xRegisterSSE& to, u8 imm8) const
		{
			SHIM_BEGIN;
			E_SSE_RRI(p_, 0x66, 0x73, Q.Modcode + 1, to.Id, imm8);
			SHIM_END;
		}
	};


	// ---- SSE move families -----------------------------------------------
	//
	// These pick the aligned or unaligned encoding at emit time, and the rule
	// is not just the isAligned flag: a memory operand also counts as aligned
	// when it is displacement-only and that displacement is 16-byte aligned
	// (simd.cpp:376). Getting that wrong produces working code that is a
	// byte different, which is exactly what the oracle is for.
	//
	// Register-to-register self-moves are elided, as in the reference.

	static const u16 shim_MovPS_Aligned   = 0x28;
	static const u16 shim_MovPS_Unaligned = 0x10;

	static __fi bool shim_mem_is_aligned(const xIndirectVoid& m)
	{
		return ((m.Displacement & 0x0f) == 0) && m.Index.IsEmpty() && m.Base.IsEmpty();
	}

	struct shim_MoveSSE
	{
		u8   Prefix;
		bool isAligned;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
		{
			if (to.Id == from.Id)
				return;
			SHIM_BEGIN;
			E_SSE_RR(p_, Prefix, shim_MovPS_Aligned, to.Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
		{
			const bool aligned = isAligned || shim_mem_is_aligned(from);
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_SSE_R_MEM(p_, Prefix,
				aligned ? shim_MovPS_Aligned : shim_MovPS_Unaligned, to.Id, m);
			SHIM_END;
		}
		__fi void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
		{
			// Stores use opcode+1 and pass the register as the reg field.
			const bool aligned = isAligned || shim_mem_is_aligned(to);
			struct e_mem m = shim_mem(to);
			SHIM_BEGIN;
			E_SSE_R_MEM(p_, Prefix,
				(aligned ? shim_MovPS_Aligned : shim_MovPS_Unaligned) + 1, from.Id, m);
			SHIM_END;
		}
	};

	// MOVDQA / MOVDQU. Same alignment rule, but the choice lands in the
	// prefix rather than the opcode, and the store uses 0x7f rather than
	// opcode+1 -- an alternate ModRM encoding with src and dst reversed.
	struct shim_MoveDQ
	{
		// Prefix is carried to match the reference's layout so its
		// initialisers transfer unchanged; the encoding does not use it,
		// picking 0x66 or 0xf3 from the alignment instead.
		u8   Prefix;
		bool isAligned;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
		{
			if (to.Id == from.Id)
				return;
			SHIM_BEGIN;
			E_SSE_RR(p_, 0x66, 0x6f, to.Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
		{
			const u8 pre = (isAligned || shim_mem_is_aligned(from)) ? 0x66 : 0xf3;
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_SSE_R_MEM(p_, pre, 0x6f, to.Id, m);
			SHIM_END;
		}
		__fi void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
		{
			const u8 pre = (isAligned || shim_mem_is_aligned(to)) ? 0x66 : 0xf3;
			struct e_mem m = shim_mem(to);
			SHIM_BEGIN;
			E_SSE_R_MEM(p_, pre, 0x7f, from.Id, m);
			SHIM_END;
		}
	};

	// ---- movzx / movsx ---------------------------------------------------

	// All six overloads the reference declares (implement/movs.h:90-100).
	// The destination widths differ per overload and the 16-bit destination
	// takes a 0x66 prefix; movsxd (0x63) is a plain opcode rather than an 0F
	// escape. Binding only the two I happened to need is what broke eleven
	// translation units.
	struct shim_MovExtend
	{
		bool SignExtend;

		// 8-bit source: destination may be 16, 32 or 64-bit.
		__fi void operator()(const xRegister16or32or64& to, const xRegister8& from) const
		{
			SHIM_BEGIN;
			// A 16-bit destination takes the operand-size prefix; E_MOVEXT_RR
			// has no prefix parameter, so it is emitted here.
			if (to->_operandSize == 2) E_P16(p_);
			E_MOVEXT_RR(p_, to->_operandSize == 8, SignExtend, 0, to->Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegister16or32or64& to, const xIndirect8& src) const
		{
			struct e_mem m = shim_mem(src);
			SHIM_BEGIN;
			if (to->_operandSize == 2) E_P16(p_);
			E_REX_MEM(p_, to->_operandSize == 8, to->Id, m);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)(SignExtend ? 0xbe : 0xb6));
			E_MODRM_MEM(p_, to->Id, m, 0);
			SHIM_END;
		}

		// 16-bit source: destination 32 or 64-bit, never a 0x66 prefix.
		__fi void operator()(const xRegister32or64& to, const xRegister16& from) const
		{
			SHIM_BEGIN;
			E_MOVEXT_RR(p_, to->_operandSize == 8, SignExtend, 1, to->Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegister32or64& to, const xIndirect16& src) const
		{
			struct e_mem m = shim_mem(src);
			SHIM_BEGIN;
			E_REX_MEM(p_, to->_operandSize == 8, to->Id, m);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)(SignExtend ? 0xbf : 0xb7));
			E_MODRM_MEM(p_, to->Id, m, 0);
			SHIM_END;
		}

		// 32-bit source into 64: movsxd, opcode 0x63, no 0F escape. The
		// reference emits it for both settings of SignExtend.
		__fi void operator()(const xRegister64& to, const xRegister32& from) const
		{
			SHIM_BEGIN;
			E_REX(p_, 1, to.Id, 0, from.Id);
			EW8(p_, 0x63);
			E_MODRM_RR(p_, to.Id, from.Id);
			SHIM_END;
		}
		__fi void operator()(const xRegister64& to, const xIndirect32& src) const
		{
			struct e_mem m = shim_mem(src);
			SHIM_BEGIN;
			E_REX_MEM(p_, 1, to.Id, m);
			EW8(p_, 0x63);
			E_MODRM_MEM(p_, to.Id, m, 0);
			SHIM_END;
		}
	};


	// ---- remaining SSE aggregates ---------------------------------------
	// All plain {prefix, opcode} tables over shapes already implemented, so
	// the reference initialisers transfer verbatim.

	struct shim_AddSub
	{
		shim_SimdRegSSE B, W, D, Q;
		shim_SimdRegSSE SB, SW, USB, USW;
	};

	struct shim_PUnpack
	{
		shim_SimdRegSSE LBW, LWD, LDQ, LQDQ;
		shim_SimdRegSSE HBW, HWD, HDQ, HQDQ;
	};

	// ---- MOVSS / MOVSD ---------------------------------------------------
	// IMPLEMENT_xMOVS (simd.cpp:496-503): the register form elides a
	// self-move, the load is 0x10, and the store is 0x11 with the register
	// passed as the reg field. `ZX` is only the load spelling; it emits the
	// same 0x10 and exists to make the zero-extension explicit at call sites.
	struct shim_MovS
	{
		u8 Prefix;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
		{
			if (to.Id == from.Id)
				return;
			SHIM_BEGIN;
			E_SSE_RR(p_, Prefix, 0x10, to.Id, from.Id);
			SHIM_END;
		}
		__fi void ZX(const xRegisterSSE& to, const xIndirectVoid& from) const
		{
			// The memory operand's own width feeds REX.W (EmitRex,
			// x86emitter.cpp), so ptr64[] and ptr32[] differ by a byte here.
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_SSE_R_MEM_W(p_, Prefix, 0x10, to.Id, m, from._operandSize == 8);
			SHIM_END;
		}
		__fi void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
		{
			struct e_mem m = shim_mem(to);
			SHIM_BEGIN;
			E_SSE_R_MEM_W(p_, Prefix, 0x11, from.Id, m, to._operandSize == 8);
			SHIM_END;
		}
	};

	// ---- jumps and calls -------------------------------------------------
	// xFastCall picks a near call when the target is within reach of a
	// signed 32-bit displacement and an indirect call through RAX when it is
	// not (jmp.cpp:84-93), so the emitted length varies with the layout.
	struct shim_JmpCall
	{
		bool isJmp;

		__fi void operator()(const void* func) const
		{
			SHIM_BEGIN;
			if (isJmp)
				E_JCC_TO(p_, E_CC_UNC, func);
			else
				E_CALL_REL(p_, func);
			SHIM_END;
		}
		__fi void operator()(const xAddressReg& absreg) const
		{
			SHIM_BEGIN;
			if (isJmp) { E_JMP_R(p_, absreg.Id); }
			else       { E_CALL_R(p_, absreg.Id); }
			SHIM_END;
		}

		/// Indirect through memory. Jumps are always wide, so REX.W is never
		/// set here however large the operand looks (jmp.cpp:41-46).
		__fi void operator()(const xIndirectNative& src) const
		{
			struct e_mem m = shim_mem(src);
			SHIM_BEGIN;
			E_REX_MEM(p_, 0, 0, m);
			EW8(p_, 0xff);
			E_MODRM_MEM(p_, (isJmp ? 4 : 2), m, 0);
			SHIM_END;
		}
	};

	// xFastCall picks a near call when the target is within a signed 32-bit
	// displacement and an indirect call through RAX when it is not
	// (jmp.cpp:84-93), so the emitted length depends on where the code cache
	// landed relative to the callee.
	static __fi void shim_FastCall(const void* f)
	{
		const sptr disp = ((sptr)x86Ptr + 5) - (sptr)f;
		if (disp == (sptr)(s32)disp)
		{
			SHIM_BEGIN; E_CALL_REL(p_, f); SHIM_END;
		}
		else
		{
			// The reference reaches for xLEA here. Using the core's E_LEA
			// instead -- which mirrors EmitLeaMagic -- keeps this header
			// self-contained, so it can be included before xLEA is declared.
			// That matters: the mov bindings have to sit above the xImm64Op
			// template, which is above xLEA's declaration.
			struct e_mem m;
			E_MEM(m, E_NOREG, E_NOREG, 0, (intptr_t)f);
			SHIM_BEGIN;
			E_LEA(p_, 1, 0, 0 /* rax */, m);
			E_CALL_R(p_, 0);
			SHIM_END;
		}
	}


	// ---- the remaining families -----------------------------------------

	// SHUFPS / SHUFPD. Note PD masks the selector to two bits and PS does
	// not (simd.cpp:264-277) -- the operand only has two lanes to permute.
	struct shim_Shuffle
	{
		__fi void PS(const xRegisterSSE& to, const xRegisterSSE& from, u8 sel) const
		{ SHIM_BEGIN; E_SSE_RRI(p_, 0x00, 0xc6, to.Id, from.Id, sel); SHIM_END; }
		__fi void PS(const xRegisterSSE& to, const xIndirectVoid& from, u8 sel) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEMI(p_, 0x00, 0xc6, to.Id, m, sel); SHIM_END; }
		__fi void PD(const xRegisterSSE& to, const xRegisterSSE& from, u8 sel) const
		{ SHIM_BEGIN; E_SSE_RRI(p_, 0x66, 0xc6, to.Id, from.Id, sel & 0x3); SHIM_END; }
		__fi void PD(const xRegisterSSE& to, const xIndirectVoid& from, u8 sel) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEMI(p_, 0x66, 0xc6, to.Id, m, sel & 0x3); SHIM_END; }
	};

	// MOVD / MOVDZX. The direction is in the opcode, and the register is
	// always the ModRM.reg operand, so the store form passes the SSE
	// register first (simd.cpp:472-476).
	static __fi void shim_MOVDZX(const xRegisterSSE& to, const xRegisterInt& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, 0x6e, to.Id, from.Id); SHIM_END; }
	static __fi void shim_MOVDZX(const xRegisterSSE& to, const xIndirectVoid& src)
	{ struct e_mem m = shim_mem(src); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0x66, 0x6e, to.Id, m, src._operandSize == 8); SHIM_END; }
	static __fi void shim_MOVD(const xRegisterInt& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, 0x7e, from.Id, to.Id); SHIM_END; }
	static __fi void shim_MOVD(const xIndirectVoid& dest, const xRegisterSSE& from)
	{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0x66, 0x7e, from.Id, m, dest._operandSize == 8); SHIM_END; }

	// Two- and three-operand IMUL. The single-operand forms come from
	// group3 and are already covered by shim_Group3.
	struct shim_iMul
	{
		__fi void operator()(const xRegister32& to, const xRegister32& from) const
		{ SHIM_BEGIN; E_IMUL_RR(p_, 0, to.Id, from.Id); SHIM_END; }
		__fi void operator()(const xRegister32& to, const xIndirectVoid& src) const
		{ struct e_mem m = shim_mem(src); SHIM_BEGIN;
		  E_IMUL_R_MEM(p_, 0, to.Id, m); SHIM_END; }
		__fi void operator()(const xRegister32& to, const xRegister32& from, s32 imm) const
		{ SHIM_BEGIN; E_IMUL_RRI(p_, 0, to.Id, from.Id, imm); SHIM_END; }
	};


	// ---- the instruction tail --------------------------------------------

	struct shim_PMul
	{
		// Seven members, not six: DQ is easy to miss because it sits below a
		// long comment block in the reference (simd_arithmetic.h:84-109).
		shim_SimdRegSSE LW, HW, HUW, UDQ, HRSW, LD, DQ;
	};

	struct shim_COMI
	{
		shim_SimdRegSSE SS, SD;
	};

	// PMOVSX / PMOVZX. One three-byte base opcode; each member steps it by
	// 0x100, which is the *high* byte of the 16-bit opcode value, so the
	// walk lands on the second opcode byte rather than the first
	// (simd.cpp:413-428). Source widths differ per member -- BW takes an
	// m64, BD an m32, BQ an m16 -- so the memory forms carry their own
	// operand size.
	struct shim_PMove
	{
		u16 OpcodeBase;

		// Memory forms as well as register: each member takes a source of a
		// different width -- BW an m64, BD an m32, BQ an m16 -- and the
		// operand's own size feeds REX.W through EmitRex, so it is threaded
		// through rather than assumed zero.
		__fi void BW(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, OpcodeBase, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void BD(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, OpcodeBase + 0x100, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void BQ(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, OpcodeBase + 0x200, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void WD(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, OpcodeBase + 0x300, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void WQ(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, OpcodeBase + 0x400, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void DQ(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, OpcodeBase + 0x500, to.Id, m, from._operandSize == 8); SHIM_END; }

		__fi void BW(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, OpcodeBase, to.Id, from.Id); SHIM_END; }
		__fi void BD(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, OpcodeBase + 0x100, to.Id, from.Id); SHIM_END; }
		__fi void BQ(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, OpcodeBase + 0x200, to.Id, from.Id); SHIM_END; }
		__fi void WD(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, OpcodeBase + 0x300, to.Id, from.Id); SHIM_END; }
		__fi void WQ(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, OpcodeBase + 0x400, to.Id, from.Id); SHIM_END; }
		__fi void DQ(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, OpcodeBase + 0x500, to.Id, from.Id); SHIM_END; }
	};

	static __fi void shim_MOVMSKPS(const xRegister32& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0x00, 0x50, to.Id, from.Id); SHIM_END; }
	static __fi void shim_MOVMSKPD(const xRegister32& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, 0x50, to.Id, from.Id); SHIM_END; }

	// LDMXCSR / STMXCSR: 0F AE with a /digit in ModRM.reg, no register operand.
	static __fi void shim_LDMXCSR(const xIndirectVoid& src)
	{ struct e_mem m = shim_mem(src); SHIM_BEGIN;
	  E_SSE_R_MEM(p_, 0x00, 0xae, 2, m); SHIM_END; }
	static __fi void shim_STMXCSR(const xIndirectVoid& dst)
	{ struct e_mem m = shim_mem(dst); SHIM_BEGIN;
	  E_SSE_R_MEM(p_, 0x00, 0xae, 3, m); SHIM_END; }


	// Subclass shapes matching xImpl_G1Logic / xImpl_G1Arith: the same
	// group1 object with SSE members bolted on.
	struct shim_G1Logic : public shim_Group1 { shim_SimdRegSSE PS, PD; };
	struct shim_G1Arith : public shim_Group1 { shim_SimdRegSSE PS, PD, SS, SD; };


	// xDIV and xMUL are group3 objects with SSE members bolted on, and xMUL
	// additionally carries the two- and three-operand IMUL forms. Both
	// inherit group3's single-operand forms rather than redeclaring them,
	// which is why shim_iMul brings them in with a using-declaration.
	struct shim_iDiv : public shim_Group3
	{
		shim_SimdRegSSE PS, PD, SS, SD;
	};

	struct shim_iMulFull : public shim_Group3
	{
		shim_SimdRegSSE PS, PD, SS, SD;

		using shim_Group3::operator();

		__fi void operator()(const xRegister32& to, const xRegister32& from) const
		{ SHIM_BEGIN; E_IMUL_RR(p_, 0, to.Id, from.Id); SHIM_END; }
		__fi void operator()(const xRegister32& to, const xIndirectVoid& src) const
		{ struct e_mem m = shim_mem(src); SHIM_BEGIN;
		  E_IMUL_R_MEM(p_, 0, to.Id, m); SHIM_END; }
		__fi void operator()(const xRegister32& to, const xRegister32& from, s32 imm) const
		{ SHIM_BEGIN; E_IMUL_RRI(p_, 0, to.Id, from.Id, imm); SHIM_END; }
		__fi void operator()(const xRegister32& to, const xIndirectVoid& from, s32 imm) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_IMUL_RMI(p_, 0, to.Id, m, imm); SHIM_END; }
	};


	// SSE families whose members are all plain reg/mem or reg/mem+imm8
	// shapes. Struct layouts and member order are generated from the
	// reference definitions so the initialisers transfer verbatim.
	struct shim_Sqrt { shim_SimdRegSSE PS; shim_SimdRegSSE SS; shim_SimdRegSSE SD; };
	struct shim_PAbsolute { shim_SimdRegSSE B; shim_SimdRegSSE W; shim_SimdRegSSE D; };
	struct shim_PMultAdd { shim_SimdRegSSE WD; shim_SimdRegSSE UBSW; };
	struct shim_DotProduct { shim_SimdRegImmSSE PS; shim_SimdRegImmSSE PD; };
	struct shim_MinMax { shim_SimdRegSSE PS; shim_SimdRegSSE PD; shim_SimdRegSSE SS; shim_SimdRegSSE SD; };
	struct shim_PMinMax { shim_SimdRegSSE UB; shim_SimdRegSSE SW; shim_SimdRegSSE SB; shim_SimdRegSSE SD; shim_SimdRegSSE UW; shim_SimdRegSSE UD; };
	struct shim_Pack { shim_SimdRegSSE SSWB; shim_SimdRegSSE SSDW; shim_SimdRegSSE USWB; shim_SimdRegSSE USDW; };
	struct shim_Unpack { shim_SimdRegSSE HPS; shim_SimdRegSSE HPD; shim_SimdRegSSE LPS; shim_SimdRegSSE LPD; };
	struct shim_PBlend { shim_SimdRegImmSSE W; shim_SimdRegSSE VB; };
	struct shim_Blend { shim_SimdRegImmSSE PS; shim_SimdRegImmSSE PD; shim_SimdRegSSE VPS; shim_SimdRegSSE VPD; };


	// ---- bit scan, and the high/low SSE moves --------------------------

	// BSR/BSF. The prefix comes from the *source* for the register form and
	// from the destination for the memory form -- that asymmetry is in the
	// reference (x86emitter.cpp:823-830), not a slip, and both operands are
	// the same width in practice.
	struct shim_BitScan
	{
		u16 Opcode;

		__fi void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
		{
			SHIM_BEGIN;
			if (from->_operandSize == 2) E_P16(p_);
			E_REX(p_, to->_operandSize == 8, to->Id, 0, from->Id);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)Opcode);
			E_MODRM_RR(p_, to->Id, from->Id);
			SHIM_END;
		}
		__fi void operator()(const xRegister16or32or64& to, const xIndirectVoid& src) const
		{
			struct e_mem m = shim_mem(src);
			SHIM_BEGIN;
			if (to->_operandSize == 2) E_P16(p_);
			E_REX_MEM(p_, to->_operandSize == 8, to->Id, m);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)Opcode);
			E_MODRM_MEM(p_, to->Id, m, 0);
			SHIM_END;
		}
	};

	// MOVHPS/MOVLPS and friends: load uses Opcode, store uses Opcode+1 with
	// the register passed as the reg field (simd.cpp:407-411).
	struct shim_MovHL
	{
		u16 Opcode;

		// These take a ptr64 operand in practice, and EmitRex sets REX.W from
		// the memory operand's own size -- so the width has to be threaded
		// through here rather than defaulted to zero.
		__fi void PS(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x00, Opcode, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void PS(const xIndirectVoid& to, const xRegisterSSE& from) const
		{ struct e_mem m = shim_mem(to); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x00, Opcode + 1, from.Id, m, to._operandSize == 8); SHIM_END; }
		__fi void PD(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, Opcode, to.Id, m, from._operandSize == 8); SHIM_END; }
		__fi void PD(const xIndirectVoid& to, const xRegisterSSE& from) const
		{ struct e_mem m = shim_mem(to); SHIM_BEGIN;
		  E_SSE_R_MEM_W(p_, 0x66, Opcode + 1, from.Id, m, to._operandSize == 8); SHIM_END; }
	};

	// MOVLHPS/MOVHLPS: register to register only, no store form.
	struct shim_MovHL_RtoR
	{
		u16 Opcode;

		__fi void PS(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x00, Opcode, to.Id, from.Id); SHIM_END; }
		__fi void PD(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RR(p_, 0x66, Opcode, to.Id, from.Id); SHIM_END; }
	};


	// PINSR / PEXTR. Both mix an SSE register with a GPR and an imm8, and
	// both have a 64-bit member that sets REX.W. Two asymmetries are
	// transcribed rather than smoothed:
	//
	//   PExtract::W uses 0xc5 for the register destination but 0x153a for
	//   the memory one -- different opcodes, not a prefix difference.
	//   PInsert::D and ::Q share opcode 0x223a and differ only in operand
	//   width.
	//
	// For PINSR the SSE register is the reg field; for PEXTR it is also the
	// reg field, with the GPR or memory operand as rm (simd.cpp:326-348).
	struct shim_PInsert
	{
		__fi void B(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x203a, to.Id, from.Id, imm8, 0); SHIM_END; }
		__fi void B(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x203a, to.Id, m, imm8, 0); SHIM_END; }
		__fi void W(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0xc4, to.Id, from.Id, imm8, 0); SHIM_END; }
		__fi void W(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0xc4, to.Id, m, imm8, 0); SHIM_END; }
		__fi void D(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x223a, to.Id, from.Id, imm8, 0); SHIM_END; }
		__fi void D(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x223a, to.Id, m, imm8, 0); SHIM_END; }
		__fi void Q(const xRegisterSSE& to, const xRegister64& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x223a, to.Id, from.Id, imm8, 1); SHIM_END; }
		__fi void Q(const xRegisterSSE& to, const xIndirect64& from, u8 imm8) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x223a, to.Id, m, imm8, 1); SHIM_END; }
	};

	struct shim_PExtract
	{
		__fi void B(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x143a, from.Id, to.Id, imm8, 0); SHIM_END; }
		__fi void B(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const
		{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x143a, from.Id, m, imm8, 0); SHIM_END; }
		__fi void W(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0xc5, from.Id, to.Id, imm8, 0); SHIM_END; }
		__fi void W(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const
		{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x153a, from.Id, m, imm8, 0); SHIM_END; }
		__fi void D(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x163a, from.Id, to.Id, imm8, 0); SHIM_END; }
		__fi void D(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const
		{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x163a, from.Id, m, imm8, 0); SHIM_END; }
		__fi void Q(const xRegister64& to, const xRegisterSSE& from, u8 imm8) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x163a, from.Id, to.Id, imm8, 1); SHIM_END; }
		__fi void Q(const xIndirect64& dest, const xRegisterSSE& from, u8 imm8) const
		{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0x163a, from.Id, m, imm8, 1); SHIM_END; }
	};


	// xLEA. Three free functions rather than an object, which is why they
	// never appeared in the count of bound instruction objects -- and why
	// xFastCall kept pulling EmitSibMagic in even though its xMOV and xCALL
	// were already switched. E_LEA mirrors EmitLeaMagic including its
	// peephole rewrites (a displacement-only source becomes a MOV, a
	// base-plus-nothing becomes a MOV, and so on), so the width is all that
	// differs here; the 16-bit form takes the operand-size prefix first.
	static __fi void shim_LEA64(const xRegister64& to, const xIndirectVoid& src, bool pf)
	{
		struct e_mem m = shim_mem(src);
		SHIM_BEGIN;
		E_LEA_SZ(p_, 8, pf, to.Id, m);
		SHIM_END;
	}
	static __fi void shim_LEA32(const xRegister32& to, const xIndirectVoid& src, bool pf)
	{
		struct e_mem m = shim_mem(src);
		SHIM_BEGIN;
		E_LEA_SZ(p_, 4, pf, to.Id, m);
		SHIM_END;
	}
	static __fi void shim_LEA16(const xRegister16& to, const xIndirectVoid& src, bool pf)
	{
		struct e_mem m = shim_mem(src);
		SHIM_BEGIN;
		/* The reference writes 0x66 and then calls EmitLeaMagic with a
		 * 16-bit destination, whose inner MOV prefixes again -- so two
		 * 0x66 bytes, not one. */
		E_P16(p_);
		E_LEA_SZ(p_, 2, pf, to.Id, m);
		SHIM_END;
	}


	// ---- AVX ------------------------------------------------------------
	//
	// The L bit is the subtle part. xOpWriteC5 takes it from param3 when that
	// is an xRegisterSSE, and from the first register operand otherwise
	// (internal.h:126-130) -- an asymmetry that exists for 256-bit movemask,
	// where the destination is a GPR and only the source knows the width.
	// For the objects bound here param1 is always the SSE destination, so
	// both rules agree for the memory forms and the register forms take it
	// from the third operand. Transcribed rather than simplified: an L-bit
	// error is invisible at xmm width and only appears at ymm.
	static __fi int shim_ymm(const xRegisterSSE& r) { return r._operandSize == 32; }

	struct shim_AVXMove
	{
		u8 Prefix;
		u8 LoadOpcode;
		u8 StoreOpcode;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
		{
			// Self-moves are elided by the reference.
			if (to.Id == from.Id) return;
			SHIM_BEGIN;
			E_VEX_RR(p_, Prefix, LoadOpcode, to.Id, from.Id, shim_ymm(from));
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
		{
			struct e_mem m = shim_mem(from);
			SHIM_BEGIN;
			E_VEX_RRMEM(p_, Prefix, LoadOpcode, to.Id, E_NOREG, m, shim_ymm(to));
			SHIM_END;
		}
		__fi void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
		{
			struct e_mem m = shim_mem(to);
			SHIM_BEGIN;
			E_VEX_RRMEM(p_, Prefix, StoreOpcode, from.Id, E_NOREG, m, shim_ymm(from));
			SHIM_END;
		}
	};

	struct shim_AVXThreeArg
	{
		u8 Prefix;
		u8 Opcode;

		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from1,
				const xRegisterSSE& from2) const
		{
			SHIM_BEGIN;
			E_VEX_RRR(p_, Prefix, Opcode, to.Id, from1.Id, from2.Id, shim_ymm(from2));
			SHIM_END;
		}
		__fi void operator()(const xRegisterSSE& to, const xRegisterSSE& from1,
				const xIndirectVoid& from2) const
		{
			struct e_mem m = shim_mem(from2);
			SHIM_BEGIN;
			E_VEX_RRMEM(p_, Prefix, Opcode, to.Id, from1.Id, m, shim_ymm(to));
			SHIM_END;
		}
	};

	struct shim_AVXCmpInt
	{
		shim_AVXThreeArg EQB, EQW, EQD, GTB, GTW, GTD;
	};


	// CMOVcc and SETcc. The opcode is 0x40|cc and 0x90|cc; CMov takes the
	// operand-size prefix from the destination and REX.W from its width,
	// SETcc is 8-bit only with the reg field fixed at 0. Same 8-bit
	// destination REX rule as everywhere else: spl/bpl/sil/dil carry the
	// 0x10 marker and need a bare REX, not REX.B.
	struct shim_CMov
	{
		JccComparisonType ccType;

		__fi void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
		{
			SHIM_BEGIN;
			if (to->_operandSize == 2) E_P16(p_);
			E_REX(p_, to->_operandSize == 8, to->Id, 0, from->Id);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)(0x40 | ccType));
			E_MODRM_RR(p_, to->Id, from->Id);
			SHIM_END;
		}
		__fi void operator()(const xRegister16or32or64& to, const xIndirectVoid& src) const
		{
			struct e_mem m = shim_mem(src);
			SHIM_BEGIN;
			if (to->_operandSize == 2) E_P16(p_);
			E_REX_MEM(p_, to->_operandSize == 8, to->Id, m);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)(0x40 | ccType));
			E_MODRM_MEM(p_, to->Id, m, 0);
			SHIM_END;
		}
	};

	struct shim_Set
	{
		JccComparisonType ccType;

		__fi void operator()(const xRegister8& to) const
		{
			SHIM_BEGIN;
			{
				uint8_t rex_ = (uint8_t)(0x40 | E_R8_EXT(to.Id));
				if (rex_ != 0x40 || E_R8_NEEDREX(to.Id)) EW8(p_, rex_);
			}
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)(0x90 | ccType));
			E_MODRM_RR(p_, 0, to.Id);
			SHIM_END;
		}
		__fi void operator()(const xIndirect8& dest) const
		{
			struct e_mem m = shim_mem(dest);
			SHIM_BEGIN;
			E_REX_MEM(p_, 0, 0, m);
			EW8(p_, 0x0f);
			EW8(p_, (uint8_t)(0x90 | ccType));
			E_MODRM_MEM(p_, 0, m, 0);
			SHIM_END;
		}
	};


	// ---- free-function encoders ----------------------------------------
	//
	// The MOVD forms take a GPR that may be 64-bit; xOpWrite0F derives REX.W
	// from that operand, so the width threads through here. MOVSS/MOVSD
	// reg-reg elide self-moves, as the reference macro does.
	static __fi void shim_xMOVDZX(const xRegisterSSE& to, const xRegister32or64& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0x66, 0x6e, to.Id, from->Id, from->_operandSize == 8); SHIM_END; }
	static __fi void shim_xMOVDZX(const xRegisterSSE& to, const xIndirectVoid& src)
	{ struct e_mem m = shim_mem(src); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0x66, 0x6e, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xMOVD(const xRegister32or64& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0x66, 0x7e, from.Id, to->Id, to->_operandSize == 8); SHIM_END; }
	static __fi void shim_xMOVD(const xIndirectVoid& dest, const xRegisterSSE& from)
	{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0x66, 0x7e, from.Id, m, 0); SHIM_END; }

	static __fi void shim_xMOVQZX(const xRegisterSSE& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0xf3, 0x7e, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xMOVQZX(const xRegisterSSE& to, const xIndirectVoid& src)
	{ struct e_mem m = shim_mem(src); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf3, 0x7e, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xMOVQZX(const xRegisterSSE& to, const void* src)
	{ SHIM_BEGIN; E_SSE_R_M(p_, 0xf3, 0x7e, to.Id, (uintptr_t)src); SHIM_END; }
	static __fi void shim_xMOVQ(const xIndirectVoid& dest, const xRegisterSSE& from)
	{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0x66, 0xd6, from.Id, m, 0); SHIM_END; }

	static __fi void shim_xMOVSS(const xRegisterSSE& to, const xRegisterSSE& from)
	{ if (to.Id == from.Id) return; SHIM_BEGIN; E_SSE_RR(p_, 0xf3, 0x10, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xMOVSSZX(const xRegisterSSE& to, const xIndirectVoid& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf3, 0x10, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xMOVSS(const xIndirectVoid& to, const xRegisterSSE& from)
	{ struct e_mem m = shim_mem(to); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf3, 0x11, from.Id, m, 0); SHIM_END; }
	static __fi void shim_xMOVSD(const xRegisterSSE& to, const xRegisterSSE& from)
	{ if (to.Id == from.Id) return; SHIM_BEGIN; E_SSE_RR(p_, 0xf2, 0x10, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xMOVSDZX(const xRegisterSSE& to, const xIndirectVoid& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf2, 0x10, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xMOVSD(const xIndirectVoid& to, const xRegisterSSE& from)
	{ struct e_mem m = shim_mem(to); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf2, 0x11, from.Id, m, 0); SHIM_END; }

	// PUSH/POP: implicitly wide, REX.W never emitted; only REX.B for r8-r15.
	static __fi void shim_xPOP(const xIndirectVoid& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_REX_MEM(p_, 0, 0, m); EW8(p_, 0x8f); E_MODRM_MEM(p_, 0, m, 0); SHIM_END; }
	static __fi void shim_xPUSH(const xIndirectVoid& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_REX_MEM(p_, 0, 0, m); EW8(p_, 0xff); E_MODRM_MEM(p_, 6, m, 0); SHIM_END; }
	static __fi void shim_xPOP(xRegister32or64 from)
	{ SHIM_BEGIN;
	  if (from->Id >= 8) EW8(p_, 0x41);
	  EW8(p_, (uint8_t)(0x58 | (from->Id & 7))); SHIM_END; }
	static __fi void shim_xPUSH(u32 imm)
	{ SHIM_BEGIN;
	  if (E_IS_S8((int32_t)imm)) { EW8(p_, 0x6a); EW8(p_, (uint8_t)imm); }
	  else { EW8(p_, 0x68); EW32(p_, imm); } SHIM_END; }
	static __fi void shim_xPUSH(xRegister32or64 from)
	{ SHIM_BEGIN;
	  if (from->Id >= 8) EW8(p_, 0x41);
	  EW8(p_, (uint8_t)(0x50 | (from->Id & 7))); SHIM_END; }


	// CMPPS/CMPSS family: opcode 0xC2 with the comparison type as the imm8.
	struct shim_SimdCompare
	{
		SSE2_ComparisonType CType;

		__fi void PS(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x00, 0xc2, to.Id, from.Id, (uint8_t)CType, 0); SHIM_END; }
		__fi void PS(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x00, 0xc2, to.Id, m, (uint8_t)CType, 0); SHIM_END; }
		__fi void PD(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0xc2, to.Id, from.Id, (uint8_t)CType, 0); SHIM_END; }
		__fi void PD(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0x66, 0xc2, to.Id, m, (uint8_t)CType, 0); SHIM_END; }
		__fi void SS(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0xf3, 0xc2, to.Id, from.Id, (uint8_t)CType, 0); SHIM_END; }
		__fi void SS(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0xf3, 0xc2, to.Id, m, (uint8_t)CType, 0); SHIM_END; }
		__fi void SD(const xRegisterSSE& to, const xRegisterSSE& from) const
		{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0xf2, 0xc2, to.Id, from.Id, (uint8_t)CType, 0); SHIM_END; }
		__fi void SD(const xRegisterSSE& to, const xIndirectVoid& from) const
		{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
		  E_SSE_R_MEM_I_W(p_, 0xf2, 0xc2, to.Id, m, (uint8_t)CType, 0); SHIM_END; }
	};

	// CVT family. The GPR operand, where present, carries REX.W from its own
	// width -- source for SI2SS, destination for SS2SI/TSS2SI/TSD2SI.
	static __fi void shim_xCVTDQ2PS(const xRegisterSSE& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0x00, 0x5b, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xCVTDQ2PS(const xRegisterSSE& to, const xIndirect128& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN; E_SSE_R_MEM_W(p_, 0x00, 0x5b, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xCVTSD2SS(const xRegisterSSE& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0xf2, 0x5a, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xCVTSD2SS(const xRegisterSSE& to, const xIndirect64& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN; E_SSE_R_MEM_W(p_, 0xf2, 0x5a, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xCVTSI2SS(const xRegisterSSE& to, const xRegister32or64& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0xf3, 0x2a, to.Id, from->Id, from->_operandSize == 8); SHIM_END; }
	static __fi void shim_xCVTSI2SS(const xRegisterSSE& to, const xIndirect32& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN; E_SSE_R_MEM_W(p_, 0xf3, 0x2a, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xCVTSS2SD(const xRegisterSSE& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0xf3, 0x5a, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xCVTSS2SD(const xRegisterSSE& to, const xIndirect32& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN; E_SSE_R_MEM_W(p_, 0xf3, 0x5a, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xCVTSS2SI(const xRegister32or64& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0xf3, 0x2d, to->Id, from.Id, to->_operandSize == 8); SHIM_END; }
	static __fi void shim_xCVTSS2SI(const xRegister32or64& to, const xIndirect32& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf3, 0x2d, to->Id, m, to->_operandSize == 8); SHIM_END; }
	static __fi void shim_xCVTTPS2DQ(const xRegisterSSE& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR(p_, 0xf3, 0x5b, to.Id, from.Id); SHIM_END; }
	static __fi void shim_xCVTTPS2DQ(const xRegisterSSE& to, const xIndirect128& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN; E_SSE_R_MEM_W(p_, 0xf3, 0x5b, to.Id, m, 0); SHIM_END; }
	static __fi void shim_xCVTTSD2SI(const xRegister32or64& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0xf2, 0x2c, to->Id, from.Id, to->_operandSize == 8); SHIM_END; }
	static __fi void shim_xCVTTSD2SI(const xRegister32or64& to, const xIndirect64& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf2, 0x2c, to->Id, m, to->_operandSize == 8); SHIM_END; }
	static __fi void shim_xCVTTSS2SI(const xRegister32or64& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0xf3, 0x2c, to->Id, from.Id, to->_operandSize == 8); SHIM_END; }
	static __fi void shim_xCVTTSS2SI(const xRegister32or64& to, const xIndirect32& from)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_SSE_R_MEM_W(p_, 0xf3, 0x2c, to->Id, m, to->_operandSize == 8); SHIM_END; }

	// INSERTPS / EXTRACTPS (SSE4.1). Only the memory form of EXTRACTPS is
	// provided: the reference's register form passes the GPR where the SSE
	// register belongs (simd.cpp:636 -- compare PEXTR at :338, which passes
	// them the other way), so its reg and rm fields come out swapped. No
	// recompiler calls it; binding it would mean byte-matching what looks
	// like a defect, and leaving it out lets the compile gate tell us if a
	// caller ever appears.
	static __fi void shim_xINSERTPS(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm8)
	{ SHIM_BEGIN; E_SSE_RRI_W(p_, 0x66, 0x213a, to.Id, from.Id, imm8, 0); SHIM_END; }
	static __fi void shim_xINSERTPS(const xRegisterSSE& to, const xIndirect32& from, u8 imm8)
	{ struct e_mem m = shim_mem(from); SHIM_BEGIN;
	  E_SSE_R_MEM_I_W(p_, 0x66, 0x213a, to.Id, m, imm8, 0); SHIM_END; }
	static __fi void shim_xEXTRACTPS(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8)
	{ struct e_mem m = shim_mem(dest); SHIM_BEGIN;
	  E_SSE_R_MEM_I_W(p_, 0x66, 0x173a, from.Id, m, imm8, 0); SHIM_END; }

	static __fi void shim_xPMOVMSKB(const xRegister32or64& to, const xRegisterSSE& from)
	{ SHIM_BEGIN; E_SSE_RR_W(p_, 0x66, 0xd7, to->Id, from.Id, to->_operandSize == 8); SHIM_END; }

	// ---- free functions ---------------------------------------------------

	static __fi void shim_PUSH(xRegister32or64 from)
	{ SHIM_BEGIN; E_PUSH_R(p_, from->Id); SHIM_END; }
	static __fi void shim_POP(xRegister32or64 from)
	{ SHIM_BEGIN; E_POP_R(p_, from->Id); SHIM_END; }
	static __fi void shim_PUSH(u32 imm)
	{ SHIM_BEGIN; E_PUSH_I(p_, imm); SHIM_END; }
	static __fi void shim_RET(void)
	{ SHIM_BEGIN; E_RET(p_); SHIM_END; }
	static __fi void shim_NOP(void)
	{ SHIM_BEGIN; E_NOP(p_); SHIM_END; }

	#undef SHIM_BEGIN
	#undef SHIM_END
}
