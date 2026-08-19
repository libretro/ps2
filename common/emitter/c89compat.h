/* c89compat.h -- the surviving C++-side pieces after the veneer cut.
 *
 * Under PCSX2_C89_EMITTER, x86emitter.h includes x86types.h, this header,
 * c89ops.h, and legacy_instructions.h -- NOT instructions.h and NOT the
 * shim. This header carries the handful of names converted code still
 * spells the C++ way, each either pure metadata or emitting through the
 * C89 core:
 *
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
	inline JccComparisonType xInvertCond(JccComparisonType src)
	{
		return (src == Jcc_Unconditional) ? Jcc_Unconditional : (JccComparisonType)((int)src ^ 1);
	}

#if !defined(PCSX2_C89_EMITTER) || defined(PCSX2_C89_KEEP_TYPES)
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
#endif /* object-typed compat helpers: no users in the switched build */
} // namespace x86Emitter

