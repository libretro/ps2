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

/* C++-side bridge while callers still build xAddressVoid expressions:
 * collapse the object into the plain integer e_mem. Mirrors XE_MEM_XAV. */
static inline struct e_mem e_mem_from_xav(const x86Emitter::xAddressVoid& av)
{
	struct e_mem m;
	m.base  = av.Base.IsEmpty()  ? E_NOREG : av.Base.Id;
	m.index = av.Index.IsEmpty() ? E_NOREG : av.Index.Id;
	m.scale = av.Index.IsEmpty() ? 0 : av.Factor;
	m.disp  = (e_sptr)av.Displacement;
	return m;
}
