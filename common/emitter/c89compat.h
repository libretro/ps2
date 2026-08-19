/* c89compat.h -- the surviving C++-side pieces after the veneer cut.
 *
 * Under PCSX2_C89_EMITTER, x86emitter.h includes x86types.h, this header,
 * c89ops.h, and legacy_instructions.h -- NOT instructions.h and NOT the
 * shim. This header carries the handful of names converted code still
 * spells the C++ way, each either pure metadata or emitting through the
 * C89 core:
 *
 *   - the xForwardJ* condition typedefs (the class itself is x86types.h
 *     machinery over xWrite8 and the cursor; only the aliases lived in
 *     instructions.h)
 *   - xInvertCond (a table on the condition enum)
 *   - xLoadFarAddr / xComplexAddress / xWriteImm64ToMem, with bodies on
 *     the C89 macros byte-matching the reference forms
 */
#pragma once
#define PCSX2_C89_COMPAT_HELPERS

#include "common/emitter/x86types.h"
#include "common/emitter/c89ops.h"

namespace x86Emitter
{
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

	DEFINE_FORWARD_JUMP(JA, Jcc_Above);
	DEFINE_FORWARD_JUMP(JB, Jcc_Below);
	DEFINE_FORWARD_JUMP(JAE, Jcc_AboveOrEqual);
	DEFINE_FORWARD_JUMP(JBE, Jcc_BelowOrEqual);
	DEFINE_FORWARD_JUMP(JG, Jcc_Greater);
	DEFINE_FORWARD_JUMP(JL, Jcc_Less);
	DEFINE_FORWARD_JUMP(JGE, Jcc_GreaterOrEqual);
	DEFINE_FORWARD_JUMP(JLE, Jcc_LessOrEqual);
	DEFINE_FORWARD_JUMP(JZ, Jcc_Zero);
	DEFINE_FORWARD_JUMP(JE, Jcc_Equal);
	DEFINE_FORWARD_JUMP(JNZ, Jcc_NotZero);
	DEFINE_FORWARD_JUMP(JNE, Jcc_NotEqual);
	DEFINE_FORWARD_JUMP(JS, Jcc_Signed);
	DEFINE_FORWARD_JUMP(JNS, Jcc_Unsigned);
	DEFINE_FORWARD_JUMP(JC, Jcc_Carry);
	DEFINE_FORWARD_JUMP(JNC, Jcc_NotCarry);
	DEFINE_FORWARD_JUMP(JO, Jcc_Overflow);
	DEFINE_FORWARD_JUMP(JNO, Jcc_NotOverflow);

	typedef xForwardJump<s8> xForwardJump8;
	typedef xForwardJump<s32> xForwardJump32;
	typedef xForwardJA<s8> xForwardJA8;
	typedef xForwardJA<s32> xForwardJA32;
	typedef xForwardJB<s8> xForwardJB8;
	typedef xForwardJB<s32> xForwardJB32;
	typedef xForwardJAE<s8> xForwardJAE8;
	typedef xForwardJAE<s32> xForwardJAE32;
	typedef xForwardJBE<s8> xForwardJBE8;
	typedef xForwardJBE<s32> xForwardJBE32;
	typedef xForwardJG<s8> xForwardJG8;
	typedef xForwardJG<s32> xForwardJG32;
	typedef xForwardJL<s8> xForwardJL8;
	typedef xForwardJL<s32> xForwardJL32;
	typedef xForwardJGE<s8> xForwardJGE8;
	typedef xForwardJGE<s32> xForwardJGE32;
	typedef xForwardJLE<s8> xForwardJLE8;
	typedef xForwardJLE<s32> xForwardJLE32;
	typedef xForwardJZ<s8> xForwardJZ8;
	typedef xForwardJZ<s32> xForwardJZ32;
	typedef xForwardJE<s8> xForwardJE8;
	typedef xForwardJE<s32> xForwardJE32;
	typedef xForwardJNZ<s8> xForwardJNZ8;
	typedef xForwardJNZ<s32> xForwardJNZ32;
	typedef xForwardJNE<s8> xForwardJNE8;
	typedef xForwardJNE<s32> xForwardJNE32;
	typedef xForwardJS<s8> xForwardJS8;
	typedef xForwardJS<s32> xForwardJS32;
	typedef xForwardJNS<s8> xForwardJNS8;
	typedef xForwardJNS<s32> xForwardJNS32;
	typedef xForwardJC<s8> xForwardJC8;
	typedef xForwardJC<s32> xForwardJC32;
	typedef xForwardJNC<s8> xForwardJNC8;
	typedef xForwardJNC<s32> xForwardJNC32;
	typedef xForwardJO<s8> xForwardJO8;
	typedef xForwardJO<s32> xForwardJO32;
	typedef xForwardJNO<s8> xForwardJNO8;
	typedef xForwardJNO<s32> xForwardJNO32;


	inline JccComparisonType xInvertCond(JccComparisonType src)
	{
		return (src == Jcc_Unconditional) ? Jcc_Unconditional : (JccComparisonType)((int)src ^ 1);
	}

	inline void xLoadFarAddr(const xAddressReg& dst, void* addr)
	{
		xe_lea_far(dst.Id, addr);
	}

	inline xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset)
	{
		if ((sptr)base == (s32)(sptr)base)
			return offset + base;
		xLoadFarAddr(tmpRegister, base);
		return offset + tmpRegister;
	}

	inline void xWriteImm64ToMem(u64* addr, const xAddressReg& tmp, u64 imm)
	{
		xe_imm64op_mov64_mi(addr, tmp.Id, imm);
	}
} // namespace x86Emitter
