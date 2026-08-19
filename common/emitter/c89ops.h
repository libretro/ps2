/* c89ops.h -- flat emission API for recompiler call sites.
 *
 * This is the destination surface of the call-site migration: converted
 * recompiler code calls these instead of the C++ objects in instructions.h.
 * Each macro takes plain integer register ids (the same 0-15 the allocators
 * already traffic in) and emits through the C89 core in c89emit.h.
 *
 * Two idioms, one contract:
 *
 *   Per-call form (xe_*): loads the x86Ptr cursor, emits one instruction,
 *   stores it back. Safe to interleave freely with C++-API emission and
 *   with allocator calls that may spill (which emit through the C++ API
 *   themselves during the migration).
 *
 *   Region form (XE_OPEN/XE_CLOSE + raw E_* on `xep`): one cursor
 *   load/store around a straight-line run of instructions. This is also
 *   the optimization the migration buys at call sites: the C++ path pays a
 *   thread-local load and store around every instruction, the region form
 *   pays one of each per run. MUST NOT span any call that can emit --
 *   allocator calls, C++ API calls, xe_* calls -- because those move the
 *   real cursor while `xep` still points at the stale one.
 *
 * Byte behavior is defined by c89emit.h, which is byte-verified against the
 * reference emitter; every conversion is additionally gated by the JIT
 * emission oracle (pcsx2_jithash_dump) on a fixed trace.
 */

#ifndef PCSX2_C89OPS_H
#define PCSX2_C89OPS_H

#include <string.h> /* memcpy for the shadow snapshot */

#include "common/emitter/c89emit.h"

/* Migration-time A/B oracle. Cross-BUILD hash comparison is unsound:
 * emitted code embeds absolute host pointers, and any code change shifts
 * the binary's data layout, moving those pointers -- the first cross-build
 * comparison diverged in caches whose recompilers were never touched.
 * Sound comparison must be same-binary: every xe_* macro carries both its
 * C89 body and the exact C++ call it replaced, selected at runtime by
 * PCSX2_XE_CPP=1. Same binary, same layout, same embedded pointers; a hash
 * difference between the two modes is the conversion and nothing else.
 * XE_AB and the C++ twins are deleted at the end of the migration. */
/* XE_AB is OPT-IN (build with PCSX2_XE_AB defined; `make XE_AB=1` or
 * -DXE_AB=ON in cmake). Default builds compile ONLY the C89 arm: the C++
 * twin tokens are discarded by the preprocessor, so they cost nothing --
 * no template instantiation, no runtime branch, no counter. Twins
 * compiled into every build was a compile-time and code-size regression
 * on every platform for the benefit of exactly one verification
 * workflow; that workflow now pays for itself alone. */
#ifdef PCSX2_XE_AB
#define XE_AB 1
#else
#define XE_AB 0
#endif
/* SHADOW-COMPARE oracle, all compile time, no selection anywhere.
 * There is no runtime mode flag, no env read, no per-site branch, and no
 * two-build protocol. Exactly two configurations exist:
 *   default      -> C89 arm only; twin tokens preprocessor-discarded
 *   PCSX2_XE_AB  -> EVERY converted site verifies itself on EVERY
 *                   emission: the C++ twin emits at the cursor, the
 *                   bytes are snapshotted, the cursor rewinds, the C89
 *                   arm emits at the SAME address, and a mismatch in
 *                   length or bytes aborts with file:line and both hex
 *                   dumps. The C89 bytes are what remains in the buffer.
 * Same address means embedded host pointers and RIP-relative
 * displacements are identical by construction -- this is the same-binary
 * guarantee of the retired two-run oracle, strengthened: continuous,
 * per-site, per-emission, in one build and one run. A full boot under
 * XE_AB is 58,301 byte-compared emissions or an abort.
 *
 * ARGUMENT DISCIPLINE: both arms evaluate the macro arguments, so every
 * argument must be idempotent -- a side-effecting expression like
 * scaleblockcycles_clear() yields different values to the two arms and
 * the oracle reports it as a byte divergence in the immediate field
 * (which is how this rule was discovered: iCOP0's MTC0 Status path,
 * cpp arm saw 12, c89 arm saw 1). Hoist such expressions into a local.
 * Lean builds evaluate once and are unaffected either way. */
#if XE_AB
extern "C" unsigned long long xe_site_hits; /* defined in JitHash.cpp */
extern "C" void xe_shadow_check(const void* at, const void* end,
	const void* want, unsigned long want_len, unsigned long cap,
	const char* file, int line);
#define XE_SHADOW_MAX 64
#define XE_HIT() (xe_site_hits++)
#define XE_2(c89body, cppbody) do { \
	u8* xe_p0_ = x86Ptr; \
	{ cppbody; } \
	{ e_u8 xe_sbuf_[XE_SHADOW_MAX]; \
	  unsigned long xe_slen_ = (unsigned long)(x86Ptr - xe_p0_); \
	  if (xe_slen_ && xe_slen_ <= (unsigned long)XE_SHADOW_MAX) \
		memcpy(xe_sbuf_, xe_p0_, xe_slen_); \
	  x86Ptr = xe_p0_; \
	  { XE_OPEN(); c89body; XE_CLOSE(); } \
	  xe_shadow_check(xe_p0_, x86Ptr, xe_sbuf_, xe_slen_, \
		(unsigned long)XE_SHADOW_MAX, __FILE__, __LINE__); \
	  XE_HIT(); } \
	} while (0)
#else
#define XE_HIT() ((void)0)
#define XE_2(c89body, cppbody) do { XE_OPEN(); c89body; XE_CLOSE(); } while (0)
#endif

/* The cursor protocol. x86Ptr is the global cursor shared with the C++ API
 * during the migration; these are the only two places this header touches
 * it. */
#define XE_OPEN() e_u8* xep = (e_u8*)x86Ptr
#define XE_CLOSE() (x86Ptr = (u8*)xep)
/* Refresh after a call that may have emitted (allocator spill paths). */
#define XE_REOPEN() (xep = (e_u8*)x86Ptr)

/* Register ids, spelled like the hardware. Same numbering as
 * xRegister32(n).Id, so allocator results pass straight through. */
#define XE_AX 0
#define XE_CX 1
#define XE_DX 2
#define XE_BX 3
#define XE_SP 4
#define XE_BP 5
#define XE_SI 6
#define XE_DI 7

/* ---- per-call wrappers ------------------------------------------------ */
/* Naming: xe_<op><width>_<operands>; rr = reg,reg; ri = reg,imm;
 * mr/rm = absolute-address memory forms. Width is the operand size of the
 * instruction, not of any one operand. */

#define xe_mov32_rr(to, from)      XE_2(E_MOV_RR(xep, 0, (to), (from)), \
	x86Emitter::xMOV(x86Emitter::xRegister32(to), x86Emitter::xRegister32(from)))
#define xe_mov64_rr(to, from)      XE_2(E_MOV_RR(xep, 1, (to), (from)), \
	x86Emitter::xMOV(x86Emitter::xRegister64(to), x86Emitter::xRegister64(from)))
#define xe_mov32_ri(to, imm)       XE_2(E_MOV_RI(xep, 0, 0, (to), (e_sptr)(imm)), \
	x86Emitter::xMOV(x86Emitter::xRegister32(to), (imm)))
#define xe_mov64_ri(to, imm)       XE_2(E_MOV64_RI(xep, 0, (to), (imm)), \
	x86Emitter::xMOV64(x86Emitter::xRegister64(to), (s64)(imm)))
#define xe_mov64_mr(addr, reg)     XE_2(E_MOV_M_R(xep, 1, (reg), (e_uptr)(addr)), \
	x86Emitter::xMOV(x86Emitter::ptr64[(void*)(addr)], x86Emitter::xRegister64(reg)))
#define xe_mov32_mr(addr, reg)     XE_2(E_MOV_M_R(xep, 0, (reg), (e_uptr)(addr)), \
	x86Emitter::xMOV(x86Emitter::ptr32[(void*)(addr)], x86Emitter::xRegister32(reg)))
#define xe_mov64_rm(reg, addr)     XE_2(E_MOV_R_M(xep, 1, (reg), (e_uptr)(addr)), \
	x86Emitter::xMOV(x86Emitter::xRegister64(reg), x86Emitter::ptr64[(void*)(addr)]))
#define xe_mov32_rm(reg, addr)     XE_2(E_MOV_R_M(xep, 0, (reg), (e_uptr)(addr)), \
	x86Emitter::xMOV(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))

/* movsxd r64, r32: opcode 0x63, a plain opcode with no 0F escape --
 * E_MOVEXT_RR only covers the 0F BE/BF byte and word sources. */
#define xe_movsxd_rr(to, from)     XE_2( \
	E_REX(xep, 1, (to), 0, (from)); \
	EW8(xep, 0x63); \
	E_MODRM_RR(xep, (to), (from)), \
	x86Emitter::xMOVSX(x86Emitter::xRegister64(to), x86Emitter::xRegister32(from)))

/* cdq / cdqe */
#define xe_cdq()                   XE_2(EW8(xep, 0x99), xCDQ())  /* xCDQ is a macro; no namespace */
#define xe_cdqe()                  XE_2(EW8(xep, 0x48); EW8(xep, 0x98), xCDQE())  /* macro twin */

/* pinsrd/pinsrq xmm, r32/r64, imm8 (66 0f 3a 22 /r ib, REX.W selects Q) */
#define xe_pinsrd(xmm, gpr, imm)   XE_2(E_SSE_RRI_W(xep, 0x66, 0x223a, (xmm), (gpr), (imm), 0), \
	x86Emitter::xPINSR.D(x86Emitter::xRegisterSSE(xmm), x86Emitter::xRegister32(gpr), (imm)))
#define xe_pinsrq(xmm, gpr, imm)   XE_2(E_SSE_RRI_W(xep, 0x66, 0x223a, (xmm), (gpr), (imm), 1), \
	x86Emitter::xPINSR.Q(x86Emitter::xRegisterSSE(xmm), x86Emitter::xRegister64(gpr), (imm)))

/* mov [abs], imm64 via a scratch register when it does not fit s32 --
 * byte-identical to the reference xImm64Op(xMOV, ptr64[addr], tmp, imm)
 * composition: op(dst, imm) when s32-representable, else MOV64 tmp, imm
 * followed by the store. */
#define xe_mov64_mi(addr, tmpreg, imm) XE_2( \
	{ if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		E_MOV_M_I64(xep, (e_uptr)(addr), (e_s32)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_MOV_M_R(xep, 1, (tmpreg), (e_uptr)(addr)); \
	  } }, \
	x86Emitter::xImm64Op(x86Emitter::xMOV, x86Emitter::ptr64[(void*)(addr)], \
		x86Emitter::xRegister64(tmpreg), (s64)(imm)))

/* xImm64Op(xMOV, xRegister64(dst), tmp, imm), byte-for-byte: when the
 * immediate fits s32 the reference calls op(dst, imm) directly; when it
 * does not, it loads the SCRATCH register and movs scratch to dst -- two
 * instructions, not one B8+imm64 to the destination. Matching the
 * reference's suboptimality is the contract; shaving it is a later,
 * oracle-visible change of its own. */
#define xe_imm64op_mov_rr(dst, tmpreg, imm) XE_2( \
	{ if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		E_MOV_RI(xep, 1, 0, (dst), (e_sptr)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_MOV_RR(xep, 1, (dst), (tmpreg)); \
	  } }, \
	x86Emitter::xImm64Op(x86Emitter::xMOV, x86Emitter::xRegister64(dst), \
		x86Emitter::xRegister64(tmpreg), (s64)(imm)))


/* group1: op codes 0 ADD, 2 ADC, 6 XOR, 7 CMP -- ri keeps the reference's
 * s8 narrowing to 0x83 and the AX short form; rm is the absolute-address
 * load form (op reg, [abs]). */
#define xe_add32_ri(reg, imm)  XE_2(E_G1_RI(xep, 0, 0, (reg), (e_s32)(imm)), \
	x86Emitter::xADD(x86Emitter::xRegister32(reg), (u32)(imm)))
#define xe_adc32_ri(reg, imm)  XE_2(E_G1_RI(xep, 0, 2, (reg), (e_s32)(imm)), \
	x86Emitter::xADC(x86Emitter::xRegister32(reg), (u32)(imm)))
#define xe_cmp32_ri(reg, imm)  XE_2(E_G1_RI(xep, 0, 7, (reg), (e_s32)(imm)), \
	x86Emitter::xCMP(x86Emitter::xRegister32(reg), (u32)(imm)))
#define xe_xor32_rr(dst, src)  XE_2(E_G1_RR(xep, 0, 6, (dst), (src)), \
	x86Emitter::xXOR(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src)))
#define xe_add32_rm(reg, addr) XE_2(E_G1_RM(xep, 0, 0, (reg), (e_uptr)(addr)), \
	x86Emitter::xADD(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))
#define xe_adc32_rm(reg, addr) XE_2(E_G1_RM(xep, 0, 2, (reg), (e_uptr)(addr)), \
	x86Emitter::xADC(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))

/* group2 shifts by immediate: 4 SHL, 7 SAR; the by-1 short form (0xD1) is
 * inside E_G2_RI, byte-verified. */
#define xe_shl32_ri(reg, imm)  XE_2(E_G2_RI(xep, 0, 4, (reg), (imm)), \
	x86Emitter::xSHL(x86Emitter::xRegister32(reg), (imm)))
#define xe_sar32_ri(reg, imm)  XE_2(E_G2_RI(xep, 0, 7, (reg), (imm)), \
	x86Emitter::xSAR(x86Emitter::xRegister32(reg), (imm)))

/* group3 one-operand: 2 NOT, 4 MUL, 5 IMUL, 6 DIV, 7 IDIV; register and
 * absolute-memory forms. The C++ xMUL/xUMUL/xDIV/xUDIV one-operand
 * spellings map to IMUL/MUL/IDIV/DIV respectively. */
#define xe_not32_r(reg)        XE_2(E_G3_R(xep, 0, 2, (reg)), \
	x86Emitter::xNOT(x86Emitter::xRegister32(reg)))
#define xe_imul32_r(reg)       XE_2(E_G3_R(xep, 0, 5, (reg)), \
	x86Emitter::xMUL(x86Emitter::xRegister32(reg)))
#define xe_mul32_r(reg)        XE_2(E_G3_R(xep, 0, 4, (reg)), \
	x86Emitter::xUMUL(x86Emitter::xRegister32(reg)))
#define xe_idiv32_r(reg)       XE_2(E_G3_R(xep, 0, 7, (reg)), \
	x86Emitter::xDIV(x86Emitter::xRegister32(reg)))
#define xe_div32_r(reg)        XE_2(E_G3_R(xep, 0, 6, (reg)), \
	x86Emitter::xUDIV(x86Emitter::xRegister32(reg)))
#define xe_imul32_m(addr)      XE_2(E_G3_M(xep, 5, (e_uptr)(addr)), \
	x86Emitter::xMUL(x86Emitter::ptr32[(void*)(addr)]))
#define xe_mul32_m(addr)       XE_2(E_G3_M(xep, 4, (e_uptr)(addr)), \
	x86Emitter::xUMUL(x86Emitter::ptr32[(void*)(addr)]))


/* group1 rr/rm at both widths, per-op with twins. */
#define XE_G1_RR_(name, opn, cxx, w, RT) \
	/* nothing -- expanded manually below for twin fidelity */
#define xe_add32_rr(dst, src)  XE_2(E_G1_RR(xep, 0, 0, (dst), (src)), \
	x86Emitter::xADD(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src)))
#define xe_add64_rr(dst, src)  XE_2(E_G1_RR(xep, 1, 0, (dst), (src)), \
	x86Emitter::xADD(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)))
#define xe_sub32_rr(dst, src)  XE_2(E_G1_RR(xep, 0, 5, (dst), (src)), \
	x86Emitter::xSUB(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src)))
#define xe_sub64_rr(dst, src)  XE_2(E_G1_RR(xep, 1, 5, (dst), (src)), \
	x86Emitter::xSUB(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)))
#define xe_xor64_rr(dst, src)  XE_2(E_G1_RR(xep, 1, 6, (dst), (src)), \
	x86Emitter::xXOR(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)))
#define xe_cmp64_rr(a, b)      XE_2(E_G1_RR(xep, 1, 7, (a), (b)), \
	x86Emitter::xCMP(x86Emitter::xRegister64(a), x86Emitter::xRegister64(b)))
#define xe_add64_rm(reg, addr) XE_2(E_G1_RM(xep, 1, 0, (reg), (e_uptr)(addr)), \
	x86Emitter::xADD(x86Emitter::xRegister64(reg), x86Emitter::ptr64[(void*)(addr)]))
#define xe_sub32_rm(reg, addr) XE_2(E_G1_RM(xep, 0, 5, (reg), (e_uptr)(addr)), \
	x86Emitter::xSUB(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))
#define xe_sub64_rm(reg, addr) XE_2(E_G1_RM(xep, 1, 5, (reg), (e_uptr)(addr)), \
	x86Emitter::xSUB(x86Emitter::xRegister64(reg), x86Emitter::ptr64[(void*)(addr)]))
#define xe_cmp64_rm(reg, addr) XE_2(E_G1_RM(xep, 1, 7, (reg), (e_uptr)(addr)), \
	x86Emitter::xCMP(x86Emitter::xRegister64(reg), x86Emitter::ptr64[(void*)(addr)]))
#define xe_sub32_ri(reg, imm)  XE_2(E_G1_RI(xep, 0, 5, (reg), (e_s32)(imm)), \
	x86Emitter::xSUB(x86Emitter::xRegister32(reg), (u32)(imm)))

/* group1 with the operator chosen at runtime -- for dispatch sites like
 * recLogicalOp. The twin switches over the same C++ objects the site used
 * to pick between; both arms take the identical g1 code. */
#define XE_G1_CXX_RR(g1op, dst, src, W) do { \
	switch (g1op) { \
	case 0: x86Emitter::xADD(x86Emitter::xRegister##W(dst), x86Emitter::xRegister##W(src)); break; \
	case 1: x86Emitter::xOR (x86Emitter::xRegister##W(dst), x86Emitter::xRegister##W(src)); break; \
	case 4: x86Emitter::xAND(x86Emitter::xRegister##W(dst), x86Emitter::xRegister##W(src)); break; \
	case 6: x86Emitter::xXOR(x86Emitter::xRegister##W(dst), x86Emitter::xRegister##W(src)); break; \
	default: break; } } while (0)
#define XE_G1_CXX_RM(g1op, reg, addr, W) do { \
	switch (g1op) { \
	case 0: x86Emitter::xADD(x86Emitter::xRegister##W(reg), x86Emitter::ptr##W[(void*)(addr)]); break; \
	case 1: x86Emitter::xOR (x86Emitter::xRegister##W(reg), x86Emitter::ptr##W[(void*)(addr)]); break; \
	case 4: x86Emitter::xAND(x86Emitter::xRegister##W(reg), x86Emitter::ptr##W[(void*)(addr)]); break; \
	case 6: x86Emitter::xXOR(x86Emitter::xRegister##W(reg), x86Emitter::ptr##W[(void*)(addr)]); break; \
	default: break; } } while (0)
#define xe_g1op64_rr(g1op, dst, src)  XE_2(E_G1_RR(xep, 1, (g1op), (dst), (src)), \
	XE_G1_CXX_RR((g1op), (dst), (src), 64))
#define xe_g1op64_rm(g1op, reg, addr) XE_2(E_G1_RM(xep, 1, (g1op), (reg), (e_uptr)(addr)), \
	XE_G1_CXX_RM((g1op), (reg), (addr), 64))

#define xe_not64_r(reg)        XE_2(E_G3_R(xep, 1, 2, (reg)), \
	x86Emitter::xNOT(x86Emitter::xRegister64(reg)))

/* setcc r8 on an allocator id. cc is the Jcc_* comparison number (the low
 * opcode nibble). Ids 4-7 are spl/bpl/sil/dil and need a bare REX. */
#define xe_setcc_r8(cc, reg)   XE_2( \
	{ e_u8 xrex_ = (e_u8)(0x40 | (((reg) >= 8) ? 1 : 0)); \
	  if (xrex_ != 0x40 || ((reg) >= 4 && (reg) <= 7)) EW8(xep, xrex_); } \
	EW8(xep, 0x0f); EW8(xep, (e_u8)(0x90 | (cc))); E_MODRM_RR(xep, 0, (reg)), \
	XE_SETCC_CXX((cc), (reg)))
#define XE_SETCC_CXX(cc, reg) do { \
	switch (cc) { \
	case x86Emitter::Jcc_Below:   x86Emitter::xSETB(x86Emitter::xRegister8(reg)); break; \
	case x86Emitter::Jcc_Above:   x86Emitter::xSETA(x86Emitter::xRegister8(reg)); break; \
	case x86Emitter::Jcc_Less:    x86Emitter::xSETL(x86Emitter::xRegister8(reg)); break; \
	case x86Emitter::Jcc_Greater: x86Emitter::xSETG(x86Emitter::xRegister8(reg)); break; \
	default: break; } } while (0)

/* xImm64Op over group1 (r64 dest and abs-mem dest), per-op, matching the
 * reference composition exactly including the scratch round-trip. */
#define XE_IMM64_G1_RR(opn, cxxop, dst, tmpreg, imm) XE_2( \
	{ if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		E_G1_RI(xep, 1, opn, (dst), (e_s32)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_RR(xep, 1, opn, (dst), (tmpreg)); \
	  } }, \
	x86Emitter::xImm64Op(x86Emitter::cxxop, x86Emitter::xRegister64(dst), \
		x86Emitter::xRegister64(tmpreg), (s64)(imm)))
#define xe_imm64op_add64_ri(dst, tmp, imm) XE_IMM64_G1_RR(0, xADD, dst, tmp, imm)
#define xe_imm64op_sub64_ri(dst, tmp, imm) XE_IMM64_G1_RR(5, xSUB, dst, tmp, imm)
#define xe_imm64op_cmp64_ri(dst, tmp, imm) XE_IMM64_G1_RR(7, xCMP, dst, tmp, imm)

#define xe_imm64op_cmp64_mi(addr, tmpreg, imm) XE_2( \
	{ if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		/* E_G1_MI is width-less (32-bit); this is the 64-bit form: REX.W
		 * then the same s8-narrowed body. */ \
		E_REX(xep, 1, 0, 0, 0); \
		if (E_IS_S8((e_s32)(imm))) { \
			EW8(xep, 0x83); E_MODRM_ABS(xep, 7, (e_uptr)(addr), 1); EW8(xep, (e_u8)(imm)); \
		} else { \
			EW8(xep, 0x81); E_MODRM_ABS(xep, 7, (e_uptr)(addr), 4); EW32(xep, (e_s32)(imm)); \
		} \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_MR(xep, 1, 7, (tmpreg), (e_uptr)(addr)); \
	  } }, \
	x86Emitter::xImm64Op(x86Emitter::xCMP, x86Emitter::ptr64[(void*)(addr)], \
		x86Emitter::xRegister64(tmpreg), (s64)(imm)))


/* xImm64Op with the group1 operator chosen at runtime (recLogicalOp_constv). */
#define XE_IMM64_G1_CXX(g1op, dst, tmpreg, imm) do { \
	switch (g1op) { \
	case 0: x86Emitter::xImm64Op(x86Emitter::xADD, x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	case 1: x86Emitter::xImm64Op(x86Emitter::xOR,  x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	case 4: x86Emitter::xImm64Op(x86Emitter::xAND, x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	case 6: x86Emitter::xImm64Op(x86Emitter::xXOR, x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	default: break; } } while (0)
#define xe_imm64op_g1op64_ri(g1op, dst, tmpreg, imm) XE_2( \
	{ if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		E_G1_RI(xep, 1, (g1op), (dst), (e_s32)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_RR(xep, 1, (g1op), (dst), (tmpreg)); \
	  } }, \
	XE_IMM64_G1_CXX((g1op), (dst), (tmpreg), (imm)))


/* remaining group2 named-immediate forms */
#define xe_shl64_ri(reg, imm)  XE_2(E_G2_RI(xep, 1, 4, (reg), (imm)), \
	x86Emitter::xSHL(x86Emitter::xRegister64(reg), (imm)))
#define xe_shr32_ri(reg, imm)  XE_2(E_G2_RI(xep, 0, 5, (reg), (imm)), \
	x86Emitter::xSHR(x86Emitter::xRegister32(reg), (imm)))
#define xe_shr64_ri(reg, imm)  XE_2(E_G2_RI(xep, 1, 5, (reg), (imm)), \
	x86Emitter::xSHR(x86Emitter::xRegister64(reg), (imm)))
#define xe_sar64_ri(reg, imm)  XE_2(E_G2_RI(xep, 1, 7, (reg), (imm)), \
	x86Emitter::xSAR(x86Emitter::xRegister64(reg), (imm)))

/* group2 by CL with the operator chosen at runtime (the ShiftOp template
 * dispatch in iR5900Shift): 4 SHL, 5 SHR, 7 SAR. */
#define XE_G2_CXX_RCL(g2op, reg, W) do { \
	switch (g2op) { \
	case 4: x86Emitter::xSHL(x86Emitter::xRegister##W(reg), x86Emitter::cl); break; \
	case 5: x86Emitter::xSHR(x86Emitter::xRegister##W(reg), x86Emitter::cl); break; \
	case 7: x86Emitter::xSAR(x86Emitter::xRegister##W(reg), x86Emitter::cl); break; \
	default: break; } } while (0)
#define xe_g2op32_rcl(g2op, reg)  XE_2(E_G2_RCL(xep, 0, (g2op), (reg)), \
	XE_G2_CXX_RCL((g2op), (reg), 32))
#define xe_g2op64_rcl(g2op, reg)  XE_2(E_G2_RCL(xep, 1, (g2op), (reg)), \
	XE_G2_CXX_RCL((g2op), (reg), 64))


/* absolute-address e_mem, for the SSE/CMov wrappers below */
#define XE_MEM_ABS(m, addr) E_MEM(m, E_NOREG, E_NOREG, 0, (e_sptr)(addr))

/* SSE register moves used by the HILO<->xmm paths */
#define xe_movhlps(dx, sx)     XE_2(E_SSE_RR(xep, 0x00, 0x12, (dx), (sx)), \
	x86Emitter::xMOVHL.PS(x86Emitter::xRegisterSSE(dx), x86Emitter::xRegisterSSE(sx)))
#define xe_movlhps(dx, sx)     XE_2(E_SSE_RR(xep, 0x00, 0x16, (dx), (sx)), \
	x86Emitter::xMOVLH.PS(x86Emitter::xRegisterSSE(dx), x86Emitter::xRegisterSSE(sx)))
#define xe_movsd_xx(dx, sx)    XE_2(E_SSE_RR(xep, 0xf2, 0x10, (dx), (sx)), \
	x86Emitter::xMOVSD(x86Emitter::xRegisterSSE(dx), x86Emitter::xRegisterSSE(sx)))

/* movq gpr64<-xmm (the reference spells it xMOVD on a 64-bit GPR) and
 * movq [abs]<-xmm / xmm<-nothing else needed yet */
#define xe_movq_rx(gpr, xmm)   XE_2(E_SSE_RR_W(xep, 0x66, 0x7e, (xmm), (gpr), 1), \
	x86Emitter::xMOVD(x86Emitter::xRegister64(gpr), x86Emitter::xRegisterSSE(xmm)))
#define xe_movq_mx(addr, xmm)  XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0x66, 0xd6, (xmm), xm_, 0); }, \
	x86Emitter::xMOVQ(x86Emitter::ptr64[(void*)(addr)], x86Emitter::xRegisterSSE(xmm)))

/* pinsrq/pextrq with memory forms on absolute addresses */
#define xe_pinsrq_xm(xmm, addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_I_W(xep, 0x66, 0x223a, (xmm), xm_, (imm), 1); }, \
	x86Emitter::xPINSR.Q(x86Emitter::xRegisterSSE(xmm), x86Emitter::ptr64[(void*)(addr)], (imm)))
#define xe_pextrq_rx(gpr, xmm, imm) XE_2( \
	E_SSE_RRI_W(xep, 0x66, 0x163a, (xmm), (gpr), (imm), 1), \
	x86Emitter::xPEXTR.Q(x86Emitter::xRegister64(gpr), x86Emitter::xRegisterSSE(xmm), (imm)))
#define xe_pextrq_mx(addr, xmm, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_I_W(xep, 0x66, 0x163a, (xmm), xm_, (imm), 1); }, \
	x86Emitter::xPEXTR.Q(x86Emitter::ptr64[(void*)(addr)], x86Emitter::xRegisterSSE(xmm), (imm)))

/* test r64, r64 */
#define xe_test64_rr(a, b)     XE_2(E_TEST_RR_SZ(xep, 8, (a), (b)), \
	x86Emitter::xTEST(x86Emitter::xRegister64(a), x86Emitter::xRegister64(b)))

/* cmp qword [abs], imm -- E_G1_MEM_I with an absolute e_mem */
#define xe_cmp64_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 8, 7, xm_, (e_s32)(imm)); }, \
	x86Emitter::xCMP(x86Emitter::ptr64[(void*)(addr)], (imm)))

/* cmovcc r64, r64 / r64, [abs]; cc is the Jcc_ number */
#define XE_CMOV_CXX_RR(cc, dst, src) do { \
	switch (cc) { \
	case x86Emitter::Jcc_Equal:    x86Emitter::xCMOVE (x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)); break; \
	case x86Emitter::Jcc_NotEqual: x86Emitter::xCMOVNE(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)); break; \
	default: break; } } while (0)
#define XE_CMOV_CXX_RM(cc, dst, addr) do { \
	switch (cc) { \
	case x86Emitter::Jcc_Equal:    x86Emitter::xCMOVE (x86Emitter::xRegister64(dst), x86Emitter::ptr64[(void*)(addr)]); break; \
	case x86Emitter::Jcc_NotEqual: x86Emitter::xCMOVNE(x86Emitter::xRegister64(dst), x86Emitter::ptr64[(void*)(addr)]); break; \
	case x86Emitter::Jcc_Signed:   x86Emitter::xCMOVS (x86Emitter::xRegister64(dst), x86Emitter::ptr64[(void*)(addr)]); break; \
	default: break; } } while (0)
#define xe_cmovcc64_rr(cc, dst, src) XE_2( \
	E_REX(xep, 1, (dst), 0, (src)); EW8(xep, 0x0f); \
	EW8(xep, (e_u8)(0x40 | (cc))); E_MODRM_RR(xep, (dst), (src)), \
	XE_CMOV_CXX_RR((cc), (dst), (src)))
#define xe_cmovcc64_rm(cc, dst, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 1, (dst), xm_); EW8(xep, 0x0f); \
	  EW8(xep, (e_u8)(0x40 | (cc))); E_MODRM_MEM(xep, (dst), xm_, 0); }, \
	XE_CMOV_CXX_RM((cc), (dst), (addr)))


/* mem-dest forms on absolute addresses: mov/sub with reg and imm sources,
 * 32-bit; plus the 32-bit test/cmov/cmp companions the IOP core uses. */
#define xe_mov32_mi(addr, imm) XE_2( \
	E_MOV_M_I(xep, (e_uptr)(addr), (e_u32)(imm)), \
	x86Emitter::xMOV(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))
#define xe_sub32_mr(addr, reg) XE_2(E_G1_MR(xep, 0, 5, (reg), (e_uptr)(addr)), \
	x86Emitter::xSUB(x86Emitter::ptr32[(void*)(addr)], x86Emitter::xRegister32(reg)))
#define xe_sub32_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 5, xm_, (e_s32)(imm)); }, \
	x86Emitter::xSUB(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))
#define xe_cmp32_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 7, xm_, (e_s32)(imm)); }, \
	x86Emitter::xCMP(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))
#define xe_cmp32_rm(reg, addr) XE_2(E_G1_RM(xep, 0, 7, (reg), (e_uptr)(addr)), \
	x86Emitter::xCMP(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))
#define xe_test32_rr(a, b)     XE_2(E_TEST_RR_SZ(xep, 4, (a), (b)), \
	x86Emitter::xTEST(x86Emitter::xRegister32(a), x86Emitter::xRegister32(b)))
#define XE_CMOV32_CXX_RM(cc, dst, addr) do { \
	switch (cc) { \
	case x86Emitter::Jcc_Unsigned: /* NS -- the reference names 0x9 "Unsigned" */ x86Emitter::xCMOVNS(x86Emitter::xRegister32(dst), x86Emitter::ptr32[(void*)(addr)]); break; \
	default: break; } } while (0)
#define xe_cmovcc32_rm(cc, dst, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 0, (dst), xm_); EW8(xep, 0x0f); \
	  EW8(xep, (e_u8)(0x40 | (cc))); E_MODRM_MEM(xep, (dst), xm_, 0); }, \
	XE_CMOV32_CXX_RM((cc), (dst), (addr)))


/* the 32-bit group1 forms the IOP tables add: and/or at rr/ri/rm, plus
 * generic runtime-operator 32-bit pair, mem-dest reg/imm adds, neg. */
#define xe_and32_rr(dst, src)  XE_2(E_G1_RR(xep, 0, 4, (dst), (src)), \
	x86Emitter::xAND(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src)))
#define xe_and32_ri(reg, imm)  XE_2(E_G1_RI(xep, 0, 4, (reg), (e_s32)(imm)), \
	x86Emitter::xAND(x86Emitter::xRegister32(reg), (u32)(imm)))
#define xe_and32_rm(reg, addr) XE_2(E_G1_RM(xep, 0, 4, (reg), (e_uptr)(addr)), \
	x86Emitter::xAND(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))
#define xe_or32_rr(dst, src)   XE_2(E_G1_RR(xep, 0, 1, (dst), (src)), \
	x86Emitter::xOR(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src)))
#define xe_or32_ri(reg, imm)   XE_2(E_G1_RI(xep, 0, 1, (reg), (e_s32)(imm)), \
	x86Emitter::xOR(x86Emitter::xRegister32(reg), (u32)(imm)))
#define xe_or32_rm(reg, addr)  XE_2(E_G1_RM(xep, 0, 1, (reg), (e_uptr)(addr)), \
	x86Emitter::xOR(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))
#define xe_xor32_ri(reg, imm)  XE_2(E_G1_RI(xep, 0, 6, (reg), (e_s32)(imm)), \
	x86Emitter::xXOR(x86Emitter::xRegister32(reg), (u32)(imm)))
#define xe_xor32_rm(reg, addr) XE_2(E_G1_RM(xep, 0, 6, (reg), (e_uptr)(addr)), \
	x86Emitter::xXOR(x86Emitter::xRegister32(reg), x86Emitter::ptr32[(void*)(addr)]))
#define xe_cmp32_rr(a, b)      XE_2(E_G1_RR(xep, 0, 7, (a), (b)), \
	x86Emitter::xCMP(x86Emitter::xRegister32(a), x86Emitter::xRegister32(b)))
#define xe_add32_mr(addr, reg) XE_2(E_G1_MR(xep, 0, 0, (reg), (e_uptr)(addr)), \
	x86Emitter::xADD(x86Emitter::ptr32[(void*)(addr)], x86Emitter::xRegister32(reg)))
#define xe_add32_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 0, xm_, (e_s32)(imm)); }, \
	x86Emitter::xADD(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))
#define xe_g1op32_ri(g1op, reg, imm) XE_2(E_G1_RI(xep, 0, (g1op), (reg), (e_s32)(imm)), \
	XE_G1_CXX_RI32((g1op), (reg), (imm)))
#define XE_G1_CXX_RI32(g1op, reg, imm) do { \
	switch (g1op) { \
	case 0: x86Emitter::xADD(x86Emitter::xRegister32(reg), (u32)(imm)); break; \
	case 1: x86Emitter::xOR (x86Emitter::xRegister32(reg), (u32)(imm)); break; \
	case 4: x86Emitter::xAND(x86Emitter::xRegister32(reg), (u32)(imm)); break; \
	case 6: x86Emitter::xXOR(x86Emitter::xRegister32(reg), (u32)(imm)); break; \
	default: break; } } while (0)
#define xe_g1op32_rr(g1op, dst, src)  XE_2(E_G1_RR(xep, 0, (g1op), (dst), (src)), \
	XE_G1_CXX_RR((g1op), (dst), (src), 32))
#define xe_g1op32_rm(g1op, reg, addr) XE_2(E_G1_RM(xep, 0, (g1op), (reg), (e_uptr)(addr)), \
	XE_G1_CXX_RM((g1op), (reg), (addr), 32))

#define xe_neg32_r(reg)        XE_2(E_G3_R(xep, 0, 3, (reg)), \
	x86Emitter::xNEG(x86Emitter::xRegister32(reg)))
#define xe_idiv32_m(addr)      XE_2(E_G3_M(xep, 7, (e_uptr)(addr)), \
	x86Emitter::xDIV(x86Emitter::ptr32[(void*)(addr)]))
#define xe_div32_m(addr)       XE_2(E_G3_M(xep, 6, (e_uptr)(addr)), \
	x86Emitter::xUDIV(x86Emitter::ptr32[(void*)(addr)]))
#define xe_not32_m(addr)       XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G3_MEM(xep, 0, 2, xm_); }, \
	x86Emitter::xNOT(x86Emitter::ptr32[(void*)(addr)]))

/* movzx r32 <- byte [abs] and r32 <- r8; the byte-register REX rule
 * (bare 0x40 for ids 4-7) lives in E_MOVEXT_RR's srcw==0 branch. */
#define xe_movzx32_r8(dst, src8) XE_2(E_MOVEXT_RR(xep, 0, 0, 0, (dst), (src8)), \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(dst), x86Emitter::xRegister8(src8)))


#define xe_movzx32_r16(dst, src) XE_2(E_MOVEXT_RR(xep, 0, 0, 1, (dst), (src)), \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(dst), x86Emitter::xRegister16(src)))
#define xe_movzx32_m8(dst, addr) XE_2( \
	E_REX(xep, 0, (dst), 0, 0); EW8(xep, 0x0f); EW8(xep, 0xb6); \
	E_MODRM_ABS(xep, (dst), (e_uptr)(addr), 0), \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(dst), x86Emitter::ptr8[(void*)(addr)]))
#define xe_movzx32_m16(dst, addr) XE_2( \
	E_REX(xep, 0, (dst), 0, 0); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	E_MODRM_ABS(xep, (dst), (e_uptr)(addr), 0), \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(dst), x86Emitter::ptr16[(void*)(addr)]))


/* movsx r32 <- r8/r16 (0F BE / 0F BF) */
#define xe_movsx32_r8(dst, src8) XE_2(E_MOVEXT_RR(xep, 0, 1, 0, (dst), (src8)), \
	x86Emitter::xMOVSX(x86Emitter::xRegister32(dst), x86Emitter::xRegister8(src8)))
#define xe_movsx32_r16(dst, src) XE_2(E_MOVEXT_RR(xep, 0, 1, 1, (dst), (src)), \
	x86Emitter::xMOVSX(x86Emitter::xRegister32(dst), x86Emitter::xRegister16(src)))

/* shifts by CL, named */
#define xe_shl32_rcl(reg)      XE_2(E_G2_RCL(xep, 0, 4, (reg)), \
	x86Emitter::xSHL(x86Emitter::xRegister32(reg), x86Emitter::cl))
#define xe_shr32_rcl(reg)      XE_2(E_G2_RCL(xep, 0, 5, (reg)), \
	x86Emitter::xSHR(x86Emitter::xRegister32(reg), x86Emitter::cl))


#define xe_test32_ri(reg, imm) XE_2(E_TEST_RI_SZ(xep, 4, (reg), (imm)), \
	x86Emitter::xTEST(x86Emitter::xRegister32(reg), (imm)))
#define xe_and32_mr(addr, reg) XE_2(E_G1_MR(xep, 0, 4, (reg), (e_uptr)(addr)), \
	x86Emitter::xAND(x86Emitter::ptr32[(void*)(addr)], x86Emitter::xRegister32(reg)))
#define xe_or32_mr(addr, reg)  XE_2(E_G1_MR(xep, 0, 1, (reg), (e_uptr)(addr)), \
	x86Emitter::xOR(x86Emitter::ptr32[(void*)(addr)], x86Emitter::xRegister32(reg)))


/* ---- control flow ---------------------------------------------------- */

/* fastcall to a fixed target: near call when the displacement fits s32,
 * else lea rax, [target]; call rax -- the reference's exact choice
 * (jmp.cpp:84-93), so emitted length depends on where the cache landed. */
#define xe_fastcall0(fn) XE_2( \
	{ e_sptr xd_ = ((e_sptr)xep + 5) - (e_sptr)(fn); \
	  if (xd_ == (e_sptr)(e_s32)xd_) { E_CALL_REL(xep, (fn)); } \
	  else { \
		struct e_mem xm_; XE_MEM_ABS(xm_, (fn)); \
		E_LEA(xep, 1, 0, 0 /* rax */, xm_); \
		E_CALL_R(xep, 0); \
	  } }, \
	x86Emitter::xFastCall((void*)(fn)))

/* forward jumps: emit with a zero displacement, remember the slot as a
 * plain e_u8*, patch at the target. The C++ twins use the legacy
 * byte-writer macros (same 0xEB / 0x70|cc encodings, same byte-verified
 * lineage) because a twin arm cannot share an xForwardJump8 object whose
 * constructor emits. */
#define xe_fwd_jmp8(slot) XE_2( \
	EW8(xep, 0xeb); (slot) = xep; EW8(xep, 0), \
	{ (slot) = (e_u8*)JMP8(0); })
#define xe_fwd_jcc8(cc, slot) XE_2( \
	EW8(xep, (e_u8)(0x70 | (cc))); (slot) = xep; EW8(xep, 0), \
	{ xWrite8((e_u8)(0x70 | (cc))); (slot) = (e_u8*)x86Ptr; xWrite8(0); })
#define xe_fwd_set8(slot) XE_2( \
	*(slot) = (e_u8)(xep - ((slot) + 1)), \
	x86SetJ8((u8*)(slot)))

/* complex-address loads. The builder mirrors xComplexAddress: fold the
 * absolute base into the displacement when it fits s32, else lea
 * tmp,[base] and use tmp as the e_mem index with the guest register as
 * base -- exactly `offset + tmpRegister`. Both arms fill the same e_mem,
 * and the C++ twin arm emits its LEA through the C++ API so the composed
 * bytes match the original expression shape. */
/* e_mem is stored POST-Reduce: .scale holds SIB bits, not the factor.
 * The C++ address object wants the factor back -- 1 << bits. Getting this
 * wrong for factor >= 2 sent the reference arm through xAddressVoid's own
 * Reduce with factor 3, which rewrites the operand (base = index) and the
 * dispatcher loaded from the wrong slot; the cpp-mode run aborted before
 * ever reaching the hash dump. */
#define XE_MEM_TO_ADDR(m) x86Emitter::xAddressVoid( \
	(m).base  == E_NOREG ? x86Emitter::xEmptyReg : x86Emitter::xAddressReg((m).base), \
	(m).index == E_NOREG ? x86Emitter::xEmptyReg : x86Emitter::xAddressReg((m).index), \
	1 << (m).scale, (sptr)(m).disp)
#define xe_complexaddr(m, tmpreg, baseptr, idxreg) do { \
	if ((e_sptr)(baseptr) == (e_sptr)(e_s32)(e_sptr)(baseptr)) { \
		E_MEM(m, (idxreg), E_NOREG, 0, (e_sptr)(baseptr)); \
	} else { \
		XE_2( \
			{ struct e_mem xb_; XE_MEM_ABS(xb_, (baseptr)); \
			  E_LEA(xep, 1, 0, (tmpreg), xb_); }, \
			x86Emitter::xLEA(x86Emitter::xAddressReg(tmpreg), x86Emitter::ptr[(void*)(baseptr)])); \
		E_MEM(m, (idxreg), (tmpreg), 1, 0); \
	} } while (0)

#define xe_mov32_rmem(reg, m) XE_2( \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x8b); \
	E_MODRM_MEM(xep, (reg), (m), 0), \
	x86Emitter::xMOV(x86Emitter::xRegister32(reg), x86Emitter::ptr32[XE_MEM_TO_ADDR(m)]))
#define xe_movzx32_mem8(reg, m) XE_2( \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x0f); EW8(xep, 0xb6); \
	E_MODRM_MEM(xep, (reg), (m), 0), \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(reg), x86Emitter::ptr8[XE_MEM_TO_ADDR(m)]))
#define xe_movzx32_mem16(reg, m) XE_2( \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	E_MODRM_MEM(xep, (reg), (m), 0), \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(reg), x86Emitter::ptr16[XE_MEM_TO_ADDR(m)]))


/* ---- dispatcher control flow ----------------------------------------- */

/* jcc/jmp to a known target: 8-bit when it reaches, else 32 -- E_JCC_TO
 * is the reference's selection. */
#define xe_jcc_to(cc, target) XE_2(E_JCC_TO(xep, (cc), (target)), \
	XE_JCC_CXX((cc), (target)))
#define XE_JCC_CXX(cc, target) do { \
	switch (cc) { \
	case x86Emitter::Jcc_NotEqual: /* == Jcc_NotZero: same 0x5 on x86 */ \
		x86Emitter::xJNE((const void*)(target)); break; \
	case x86Emitter::Jcc_LessOrEqual: x86Emitter::xJLE((const void*)(target)); break; \
	case x86Emitter::Jcc_Signed:      x86Emitter::xJS ((const void*)(target)); break; \
	case x86Emitter::Jcc_Below:       x86Emitter::xJC ((const void*)(target)); break; \
	default: break; } } while (0)
#define xe_jmp_to(target) XE_2(E_JCC_TO(xep, E_CC_UNC, (target)), \
	x86Emitter::xJMP((const void*)(target)))

/* wide jump with a patchable displacement: emits E9/0F8x rel32 with the
 * given displacement and stores the address of the 32-bit slot -- the
 * xJcc32 contract, as an out-parameter since macros do not return. */
/* the slot is assigned by BOTH arms in a shadow build; the C89 arm runs
 * second at the same address, so the surviving value points into the
 * bytes that remain in the buffer. */
#define xe_jcc32_slot(cc, disp, slot) XE_2( \
	{ if ((cc) == x86Emitter::Jcc_Unconditional) { EW8(xep, 0xe9); } \
	  else { EW8(xep, 0x0f); EW8(xep, (e_u8)(0x80 | (cc))); } \
	  (slot) = (s32*)xep; EW32(xep, (e_u32)(e_s32)(disp)); }, \
	(slot) = x86Emitter::xJcc32((x86Emitter::JccComparisonType)(cc), (disp)))

/* indirect jmp through a base+index memory operand (the dispatcher's LUT
 * double-indirection): FF /4, never REX.W (jmp.cpp:41-46). */
#define xe_jmp_mem(m) XE_2( \
	E_REX_MEM(xep, 0, 0, (m)); EW8(xep, 0xff); \
	E_MODRM_MEM(xep, 4, (m), 0), \
	x86Emitter::xJMP(x86Emitter::ptrNative[XE_MEM_TO_ADDR(m)]))

/* mov r64 <- base+index+scale+disp (the recLUT load) */
#define xe_mov64_rmem(reg, m) XE_2( \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x8b); \
	E_MODRM_MEM(xep, (reg), (m), 0), \
	x86Emitter::xMOV(x86Emitter::xRegister64(reg), x86Emitter::ptr64[XE_MEM_TO_ADDR(m)]))

/* complex address with a scaled index register: offset is idx*scale. */
#define xe_complexaddr_si(m, tmpreg, baseptr, idxreg, sc) do { \
	if ((e_sptr)(baseptr) == (e_sptr)(e_s32)(e_sptr)(baseptr)) { \
		E_MEM(m, E_NOREG, (idxreg), (sc), (e_sptr)(baseptr)); \
	} else { \
		XE_2( \
			{ struct e_mem xb_; XE_MEM_ABS(xb_, (baseptr)); \
			  E_LEA(xep, 1, 0, (tmpreg), xb_); }, \
			x86Emitter::xLEA(x86Emitter::xAddressReg(tmpreg), x86Emitter::ptr[(void*)(baseptr)])); \
		E_MEM(m, (tmpreg), (idxreg), (sc), 0); \
	} } while (0)

/* fastcall compositions actually used by the dispatchers: (imm, imm) and
 * (mem32) argument shapes -- transcribed from the reference overloads,
 * which stage arg1/arg2 then call near. */
#define xe_fastcall2_ii(fn, a1, a2) do { \
	xe_mov32_ri(x86Emitter::arg1regd.Id, (a1)); \
	xe_mov32_ri(x86Emitter::arg2regd.Id, (a2)); \
	xe_fastcall0(fn); } while (0)
#define xe_fastcall1_m32(fn, addr) do { \
	xe_mov32_rm(x86Emitter::arg1regd.Id, (addr)); \
	xe_fastcall0(fn); } while (0)

#define xe_ret() XE_2(EW8(xep, 0xc3), xRET())  /* xRET is a macro */
#define xe_test8_rr(a, b) XE_2(E_TEST_RR_SZ(xep, 1, (a), (b)), \
	x86Emitter::xTEST(x86Emitter::xRegister8(a), x86Emitter::xRegister8(b)))


#define xe_cmp64_ri(reg, imm)  XE_2(E_G1_RI(xep, 1, 7, (reg), (e_s32)(imm)), \
	x86Emitter::xCMP(x86Emitter::xRegister64(reg), (imm)))
#define xe_test8_ri(reg, imm)  XE_2(E_TEST_RI_SZ(xep, 1, (reg), (imm)), \
	x86Emitter::xTEST(x86Emitter::xRegister8(reg), (imm)))
/* movmskps r32, xmm: 0F 50 /r, gpr in the reg field */
#define xe_movmskps_rx(gpr, xmm) XE_2(E_SSE_RR(xep, 0x00, 0x50, (gpr), (xmm)), \
	x86Emitter::xMOVMSKPS(x86Emitter::xRegister32(gpr), x86Emitter::xRegisterSSE(xmm)))


/* movd r32 <- xmm: 66 0F 7E, no REX.W (the 64-bit spelling is movq) */
#define xe_movd_rx(gpr, xmm)   XE_2(E_SSE_RR_W(xep, 0x66, 0x7e, (xmm), (gpr), 0), \
	x86Emitter::xMOVD(x86Emitter::xRegister32(gpr), x86Emitter::xRegisterSSE(xmm)))


#define xe_and64_rr(dst, src)  XE_2(E_G1_RR(xep, 1, 4, (dst), (src)), \
	x86Emitter::xAND(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)))
#define xe_or64_rr(dst, src)   XE_2(E_G1_RR(xep, 1, 1, (dst), (src)), \
	x86Emitter::xOR(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src)))
#define XE_G2_CXX_RI(g2op, reg, imm, W) do { \
	switch (g2op) { \
	case 4: x86Emitter::xSHL(x86Emitter::xRegister##W(reg), (imm)); break; \
	case 5: x86Emitter::xSHR(x86Emitter::xRegister##W(reg), (imm)); break; \
	case 7: x86Emitter::xSAR(x86Emitter::xRegister##W(reg), (imm)); break; \
	default: break; } } while (0)
#define xe_g2op64_ri(g2op, reg, imm) XE_2(E_G2_RI(xep, 1, (g2op), (reg), (imm)), \
	XE_G2_CXX_RI((g2op), (reg), (imm), 64))
#define xe_or32_rr_(a, b) xe_or32_rr(a, b)


/* movss/blend forms for the SA register paths */
#define xe_movss_xm(xmm, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0xf3, 0x10, (xmm), xm_, 0); }, \
	x86Emitter::xMOVSSZX(x86Emitter::xRegisterSSE(xmm), x86Emitter::ptr32[(void*)(addr)]))
#define xe_movss_mx(addr, xmm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0xf3, 0x11, (xmm), xm_, 0); }, \
	x86Emitter::xMOVSS(x86Emitter::ptr[(void*)(addr)], x86Emitter::xRegisterSSE(xmm)))
#define xe_blendpd_xxi(dx, sx, imm) XE_2( \
	E_SSE_RRI_W(xep, 0x66, 0x0d3a, (dx), (sx), (imm), 0), \
	x86Emitter::xBLEND.PD(x86Emitter::xRegisterSSE(dx), x86Emitter::xRegisterSSE(sx), (imm)))
#define xe_and32_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 4, xm_, (e_s32)(imm)); }, \
	x86Emitter::xAND(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))


#define xe_sub64_ri(reg, imm)  XE_2(E_G1_RI(xep, 1, 5, (reg), (e_s32)(imm)), \
	x86Emitter::xSUB(x86Emitter::xRegister64(reg), (imm)))
#define xe_add64_ri(reg, imm)  XE_2(E_G1_RI(xep, 1, 0, (reg), (e_s32)(imm)), \
	x86Emitter::xADD(x86Emitter::xRegister64(reg), (imm)))
/* add qword [abs], imm: E_G1_MEM_I already speaks sz=8 (REX.W). The
 * earlier cmp64 composition hand-rolled this before reading far enough
 * into the macro; this one does not repeat that. */
#define xe_add64_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 8, 0, xm_, (e_s32)(imm)); }, \
	x86Emitter::xADD(x86Emitter::ptr64[(void*)(addr)], (imm)))
/* add word [abs], imm: 0x66 prefix, then the s8-narrowed body with a
 * 16-bit wide immediate. Byte behavior from E_G1_MEM_I sz=2. */
#define xe_add16_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 2, 0, xm_, (e_s32)(imm)); }, \
	x86Emitter::xADD(x86Emitter::ptr16[(void*)(addr)], (imm)))
/* lea r64, [base + index*scale + disp] from an e_mem */
#define xe_lea64_mem(reg, m)   XE_2(E_LEA(xep, 1, 0, (reg), (m)), \
	x86Emitter::xLEA(x86Emitter::xAddressReg(reg), x86Emitter::ptrNative[XE_MEM_TO_ADDR(m)]))
#define xe_fastcall1_i(fn, a1) do { \
	xe_mov32_ri(x86Emitter::arg1regd.Id, (a1)); \
	xe_fastcall0(fn); } while (0)
#define XE_CMOVB64_RR(dst, src) x86Emitter::xCMOVB(x86Emitter::xRegister64(dst), x86Emitter::xRegister64(src))
#define xe_cmovb64_rr(dst, src) XE_2( \
	E_REX(xep, 1, (dst), 0, (src)); EW8(xep, 0x0f); \
	EW8(xep, (e_u8)(0x40 | x86Emitter::Jcc_Below)); E_MODRM_RR(xep, (dst), (src)), \
	XE_CMOVB64_RR((dst), (src)))


/* iCOP0 vocabulary: base+disp32 memory forms (the TLB entry walks), the
 * three-operand IMUL, movsxd from absolute memory, the far-address LEA
 * composition, add-mem-imm, and the known-target jcc with the
 * reference's rel8/rel32 choice. */
#define XE_MEM_BD(m, base, disp) E_MEM(m, (base), E_NOREG, 0, (e_sptr)(disp))
#define xe_mov32_rbd(reg, base, disp) XE_2( \
	{ struct e_mem xm_; XE_MEM_BD(xm_, (base), (disp)); \
	  E_MOV_R_MEM(xep, 0, (reg), xm_); }, \
	x86Emitter::xMOV(x86Emitter::xRegister32(reg), \
		x86Emitter::ptr32[x86Emitter::xAddressVoid(x86Emitter::xAddressReg(base), (disp))]))
#define xe_and32_rbd(reg, base, disp) XE_2( \
	{ struct e_mem xm_; XE_MEM_BD(xm_, (base), (disp)); \
	  E_G1_R_MEM_SZ(xep, 4, 4, (reg), xm_); }, \
	x86Emitter::xAND(x86Emitter::xRegister32(reg), \
		x86Emitter::ptr32[x86Emitter::xAddressVoid(x86Emitter::xAddressReg(base), (disp))]))
#define xe_cmp32_rbd(reg, base, disp) XE_2( \
	{ struct e_mem xm_; XE_MEM_BD(xm_, (base), (disp)); \
	  E_G1_R_MEM_SZ(xep, 4, 7, (reg), xm_); }, \
	x86Emitter::xCMP(x86Emitter::xRegister32(reg), \
		x86Emitter::ptr32[x86Emitter::xAddressVoid(x86Emitter::xAddressReg(base), (disp))]))
#define xe_imul32_rri(dst, src, imm) XE_2( \
	E_IMUL_RRI(xep, 0, (dst), (src), (imm)), \
	x86Emitter::xMUL(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src), (s32)(imm)))
#define xe_movsxd_rm(reg, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 1, (reg), xm_); EW8(xep, 0x63); \
	  E_MODRM_MEM(xep, (reg), xm_, 0); }, \
	x86Emitter::xMOVSX(x86Emitter::xRegister64(reg), x86Emitter::ptr32[(void*)(addr)]))
/* xLoadFarAddr, byte for byte: rip-relative LEA when the displacement
 * from the END of the 7-byte LEA fits s32, else MOV64. The predicate is
 * replicated against the c89 cursor so both arms take the same branch. */
#define xe_lea_far(reg, addr) XE_2( \
	{ e_sptr xdisp_ = (e_sptr)(addr) - ((e_sptr)xep + 7); \
	  if (xdisp_ == (e_sptr)(e_s32)xdisp_) { \
		struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
		E_LEA(xep, 1, 0, (reg), xm_); \
	  } else { \
		E_MOV64_RI(xep, 0, (reg), (e_sptr)(addr)); \
	  } }, \
	x86Emitter::xLoadFarAddr(x86Emitter::xAddressReg(reg), (void*)(addr)))
#define xe_add32_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 0, xm_, (e_s32)(imm)); }, \
	x86Emitter::xADD(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))
/* add qword [abs], r64 */
#define xe_add64_mr(addr, reg) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 1, (reg), xm_); EW8(xep, 0x01); \
	  E_MODRM_MEM(xep, (reg), xm_, 0); }, \
	x86Emitter::xADD(x86Emitter::ptr[(void*)(addr)], x86Emitter::xRegister64(reg)))
/* jcc to a KNOWN backward/forward target, choosing rel8 when it fits
 * exactly as xJccKnownTarget(cc, target, false) does. */
#define xe_jcc_known(cc, target) XE_2( \
	{ e_sptr xd8_ = (e_sptr)(target) - ((e_sptr)xep + 2); \
	  if (xd8_ == (e_sptr)(e_s8)xd8_) { \
		EW8(xep, (e_u8)(0x70 | (cc))); EW8(xep, (e_u8)xd8_); \
	  } else { \
		e_sptr xd32_ = (e_sptr)(target) - ((e_sptr)xep + 6); \
		EW8(xep, 0x0f); EW8(xep, (e_u8)(0x80 | (cc))); \
		EW32(xep, (e_u32)(e_s32)xd32_); \
	  } }, \
	x86Emitter::xJccKnownTarget((x86Emitter::JccComparisonType)(cc), (const void*)(target), false))


/* ================= iMMI SSE vocabulary =================
 * Generated from the shim's own opcode tables (instructions.h
 * initializers and struct layouts) -- transcribed, not re-derived;
 * the shadow oracle byte-checks every one of these on execution. */
#define xe_blendps_xxi(d, s, i) XE_2(E_SSE_RRI(xep, 0x66, 0xc3a, (d), (s), (i)), \
	x86Emitter::xBLEND.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s), (i)))
#define xe_movdqa_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x6f, (d), (s)), \
	x86Emitter::xMOVDQA(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_movhlps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x12, (d), (s)), \
	x86Emitter::xMOVHL.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_movlhps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x16, (d), (s)), \
	x86Emitter::xMOVLH.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_movqzx_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x7e, (d), (s)), \
	x86Emitter::xMOVQZX(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pabsd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x1e38, (d), (s)), \
	x86Emitter::xPABS.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pabsw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x1d38, (d), (s)), \
	x86Emitter::xPABS.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_packssdw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x6b, (d), (s)), \
	x86Emitter::xPACK.SSDW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_packuswb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x67, (d), (s)), \
	x86Emitter::xPACK.USWB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xfc, (d), (s)), \
	x86Emitter::xPADD.B(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xfe, (d), (s)), \
	x86Emitter::xPADD.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd4, (d), (s)), \
	x86Emitter::xPADD.Q(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddsb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xec, (d), (s)), \
	x86Emitter::xPADD.SB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddsw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xed, (d), (s)), \
	x86Emitter::xPADD.SW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddusb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xdc, (d), (s)), \
	x86Emitter::xPADD.USB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddusw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xdd, (d), (s)), \
	x86Emitter::xPADD.USW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_paddw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xfd, (d), (s)), \
	x86Emitter::xPADD.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pand_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xdb, (d), (s)), \
	x86Emitter::xPAND(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pandn_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xdf, (d), (s)), \
	x86Emitter::xPANDN(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pcmpeqb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x74, (d), (s)), \
	x86Emitter::xPCMP.EQB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pcmpeqd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x76, (d), (s)), \
	x86Emitter::xPCMP.EQD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pcmpeqw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x75, (d), (s)), \
	x86Emitter::xPCMP.EQW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pcmpgtb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x64, (d), (s)), \
	x86Emitter::xPCMP.GTB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pcmpgtd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x66, (d), (s)), \
	x86Emitter::xPCMP.GTD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pcmpgtw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x65, (d), (s)), \
	x86Emitter::xPCMP.GTW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmaddwd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf5, (d), (s)), \
	x86Emitter::xPMADD.WD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmaxsd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x3d38, (d), (s)), \
	x86Emitter::xPMAX.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmaxsw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xee, (d), (s)), \
	x86Emitter::xPMAX.SW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pminsd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x3938, (d), (s)), \
	x86Emitter::xPMIN.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pminsw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xea, (d), (s)), \
	x86Emitter::xPMIN.SW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmovsxdq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x2538, (d), (s)), \
	x86Emitter::xPMOVSX.DQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmuldq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x2838, (d), (s)), \
	x86Emitter::xPMUL.DQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmulhw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xe5, (d), (s)), \
	x86Emitter::xPMUL.HW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmullw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd5, (d), (s)), \
	x86Emitter::xPMUL.LW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pmuludq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf4, (d), (s)), \
	x86Emitter::xPMUL.UDQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_por_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xeb, (d), (s)), \
	x86Emitter::xPOR(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pshufd_xxi(d, s, i) XE_2(E_SSE_RRI(xep, 0x66, 0x70, (d), (s), (i)), \
	x86Emitter::xPSHUF.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s), (i)))
#define xe_pshufhw_xxi(d, s, i) XE_2(E_SSE_RRI(xep, 0xf3, 0x70, (d), (s), (i)), \
	x86Emitter::xPSHUF.HW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s), (i)))
#define xe_pshuflw_xxi(d, s, i) XE_2(E_SSE_RRI(xep, 0xf2, 0x70, (d), (s), (i)), \
	x86Emitter::xPSHUF.LW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s), (i)))
#define xe_psubb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf8, (d), (s)), \
	x86Emitter::xPSUB.B(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xfa, (d), (s)), \
	x86Emitter::xPSUB.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xfb, (d), (s)), \
	x86Emitter::xPSUB.Q(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubsb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xe8, (d), (s)), \
	x86Emitter::xPSUB.SB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubsw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xe9, (d), (s)), \
	x86Emitter::xPSUB.SW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubusb_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd8, (d), (s)), \
	x86Emitter::xPSUB.USB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubusw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd9, (d), (s)), \
	x86Emitter::xPSUB.USW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf9, (d), (s)), \
	x86Emitter::xPSUB.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpckhbw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x68, (d), (s)), \
	x86Emitter::xPUNPCK.HBW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpckhdq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x6a, (d), (s)), \
	x86Emitter::xPUNPCK.HDQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpckhqdq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x6d, (d), (s)), \
	x86Emitter::xPUNPCK.HQDQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpckhwd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x69, (d), (s)), \
	x86Emitter::xPUNPCK.HWD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpcklbw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x60, (d), (s)), \
	x86Emitter::xPUNPCK.LBW(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpckldq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x62, (d), (s)), \
	x86Emitter::xPUNPCK.LDQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpcklqdq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x6c, (d), (s)), \
	x86Emitter::xPUNPCK.LQDQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_punpcklwd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x61, (d), (s)), \
	x86Emitter::xPUNPCK.LWD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pxor_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xef, (d), (s)), \
	x86Emitter::xPXOR(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_shufps_xxi(d, s, i) XE_2(E_SSE_RRI(xep, 0x00, 0xc6, (d), (s), (i)), \
	x86Emitter::xSHUF.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s), (i)))
#define xe_psllw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf1, (d), (s)), \
	x86Emitter::xPSLL.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psllw_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x71, 6, (d), (i)), \
	x86Emitter::xPSLL.W(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_pslld_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf2, (d), (s)), \
	x86Emitter::xPSLL.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pslld_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x72, 6, (d), (i)), \
	x86Emitter::xPSLL.D(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psllq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xf3, (d), (s)), \
	x86Emitter::xPSLL.Q(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psllq_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x73, 6, (d), (i)), \
	x86Emitter::xPSLL.Q(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psrlw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd1, (d), (s)), \
	x86Emitter::xPSRL.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psrlw_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x71, 2, (d), (i)), \
	x86Emitter::xPSRL.W(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psrld_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd2, (d), (s)), \
	x86Emitter::xPSRL.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psrld_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x72, 2, (d), (i)), \
	x86Emitter::xPSRL.D(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psrlq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xd3, (d), (s)), \
	x86Emitter::xPSRL.Q(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psrlq_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x73, 2, (d), (i)), \
	x86Emitter::xPSRL.Q(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psraw_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xe1, (d), (s)), \
	x86Emitter::xPSRA.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psraw_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x71, 4, (d), (i)), \
	x86Emitter::xPSRA.W(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psrad_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xe2, (d), (s)), \
	x86Emitter::xPSRA.D(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psrad_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x72, 4, (d), (i)), \
	x86Emitter::xPSRA.D(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_pslldq_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x73, 7, (d), (i)), \
	x86Emitter::xPSLL.DQ(x86Emitter::xRegisterSSE(d), (u8)(i)))
#define xe_psrldq_xi(d, i) XE_2(E_SSE_RRI(xep, 0x66, 0x73, 3, (d), (i)), \
	x86Emitter::xPSRL.DQ(x86Emitter::xRegisterSSE(d), (u8)(i)))


/* iMMI scalar stragglers */
#define xe_bsr32_rr(d, s)      XE_2( \
	{ EW8(xep, 0x0f); EW8(xep, 0xbd); E_MODRM_RR(xep, (d), (s)); }, \
	x86Emitter::xBSR(x86Emitter::xRegister32(d), x86Emitter::xRegister32(s)))
#define xe_dec32_r(reg)        XE_2(E_INCDEC_R_SZ(xep, 4, 1, (reg)), \
	x86Emitter::xDEC(x86Emitter::xRegister32(reg)))
#define xe_udiv32_r(reg)       XE_2(E_G3_R(xep, 0, 6, (reg)), \
	x86Emitter::xUDIV(x86Emitter::xRegister32(reg)))
/* pextrd gpr, xmm, imm: 66 0F 3A 16, xmm in reg field */
#define xe_pextrd_rxi(gpr, xmm, imm) XE_2( \
	E_SSE_RRI_W(xep, 0x66, 0x163a, (xmm), (gpr), (imm), 0), \
	x86Emitter::xPEXTR.D(x86Emitter::xRegister32(gpr), x86Emitter::xRegisterSSE(xmm), (imm)))
#define XE_MEM_TO_PTR32(m) x86Emitter::ptr32[XE_MEM_TO_ADDR(m)]
/* SSE ops against absolute memory the transform left behind */
#define xe_pand_xm(xmm, addr)  XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM(xep, 0x66, 0xdb, (xmm), xm_); }, \
	x86Emitter::xPAND(x86Emitter::xRegisterSSE(xmm), x86Emitter::ptr[(void*)(addr)]))
/* movdqa/movdqu through a register-based address (the QFSRV shifter) */
#define xe_movdqu_xmem(xmm, m) XE_2( \
	E_SSE_R_MEM(xep, 0xf3, 0x6f, (xmm), (m)), \
	x86Emitter::xMOVDQU(x86Emitter::xRegisterSSE(xmm), XE_MEM_TO_PTR32(m)))
#define xe_movdqa_memx(m, xmm) XE_2( \
	E_SSE_R_MEM(xep, 0x66, 0x7f, (xmm), (m)), \
	x86Emitter::xMOVDQA(XE_MEM_TO_PTR32(m), x86Emitter::xRegisterSSE(xmm)))
/* lea r64, [abs] (fits-s32 asserted by context: tempqw/GPR are in the
 * low image on linux; the reference emits the rip-relative form and so
 * do we via the absolute e_mem path) */
#define xe_lea64_m(reg, addr)  XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_LEA(xep, 1, 0, (reg), xm_); }, \
	x86Emitter::xLEA(x86Emitter::xAddressReg(reg), x86Emitter::ptr[(void*)(addr)]))
#define xe_movsxd_rm16(reg, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_P16(xep); E_REX_MEM(xep, 0, (reg), xm_); \
	  EW8(xep, 0x0f); EW8(xep, 0xbf); E_MODRM_MEM(xep, (reg), xm_, 0); }, \
	x86Emitter::xMOVSX(x86Emitter::xRegister32(reg), x86Emitter::ptr16[(void*)(addr)]))

/* ================= iFPU vocabulary ================= */
#define xe_addsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x58, (d), (s)), \
	x86Emitter::xADD.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_addss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x58, (d), (s)), \
	x86Emitter::xADD.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_andpd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x54, (d), (s)), \
	x86Emitter::xAND.PD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_andps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x54, (d), (s)), \
	x86Emitter::xAND.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_cvtdq2ps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x5b, (d), (s)), \
	x86Emitter::xCVTDQ2PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_cvtsd2ss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x5a, (d), (s)), \
	x86Emitter::xCVTSD2SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_cvtss2sd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x5a, (d), (s)), \
	x86Emitter::xCVTSS2SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_divsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x5e, (d), (s)), \
	x86Emitter::xDIV.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_divss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x5e, (d), (s)), \
	x86Emitter::xDIV.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_maxsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x5f, (d), (s)), \
	x86Emitter::xMAX.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_maxss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x5f, (d), (s)), \
	x86Emitter::xMAX.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_minsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x5d, (d), (s)), \
	x86Emitter::xMIN.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_minss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x5d, (d), (s)), \
	x86Emitter::xMIN.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_movaps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x28, (d), (s)), \
	x86Emitter::xMOVAPS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_mulsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x59, (d), (s)), \
	x86Emitter::xMUL.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_mulss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x59, (d), (s)), \
	x86Emitter::xMUL.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_orps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x56, (d), (s)), \
	x86Emitter::xOR.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_pminud_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x3b38, (d), (s)), \
	x86Emitter::xPMIN.UD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_psubq_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0xfb, (d), (s)), \
	x86Emitter::xPSUB.Q(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_sqrtsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x51, (d), (s)), \
	x86Emitter::xSQRT.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_sqrtss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x51, (d), (s)), \
	x86Emitter::xSQRT.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_subsd_xx(d, s) XE_2(E_SSE_RR(xep, 0xf2, 0x5c, (d), (s)), \
	x86Emitter::xSUB.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_subss_xx(d, s) XE_2(E_SSE_RR(xep, 0xf3, 0x5c, (d), (s)), \
	x86Emitter::xSUB.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_ucomisd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x2e, (d), (s)), \
	x86Emitter::xUCOMI.SD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_ucomiss_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x2e, (d), (s)), \
	x86Emitter::xUCOMI.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_xorpd_xx(d, s) XE_2(E_SSE_RR(xep, 0x66, 0x57, (d), (s)), \
	x86Emitter::xXOR.PD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_xorps_xx(d, s) XE_2(E_SSE_RR(xep, 0x00, 0x57, (d), (s)), \
	x86Emitter::xXOR.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
/* movss dst,src elides self-moves in the reference (shim_xMOVSS); the
 * c89 arm replicates the elision so both arms stay in step. */
#define xe_movss_xx(d, s) XE_2( \
	{ if ((d) != (s)) E_SSE_RR(xep, 0xf3, 0x10, (d), (s)); }, \
	x86Emitter::xMOVSS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
/* cmpeq.ss = CMPSS with imm 0 appended */
#define xe_cmpeqss_xx(d, s) XE_2(E_SSE_RRI(xep, 0xf3, 0xc2, (d), (s), 0), \
	x86Emitter::xCMPEQ.SS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s)))
#define xe_movsszx_xm(xmm, addr) xe_movss_xm(xmm, addr)
#define xe_movdzx_xr(xmm, gpr) XE_2(E_SSE_RR_W(xep, 0x66, 0x6e, (xmm), (gpr), 0), \
	x86Emitter::xMOVDZX(x86Emitter::xRegisterSSE(xmm), x86Emitter::xRegister32(gpr)))
#define xe_movd_mx(addr, xmm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0x66, 0x7e, (xmm), xm_, 0); }, \
	x86Emitter::xMOVD(x86Emitter::ptr32[(void*)(addr)], x86Emitter::xRegisterSSE(xmm)))
#define xe_movaps_xm(xmm, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM(xep, 0x00, 0x28, (xmm), xm_); }, \
	x86Emitter::xMOVAPS(x86Emitter::xRegisterSSE(xmm), x86Emitter::ptr128[(void*)(addr)]))
#define xe_ldmxcsr_m(addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 0, 2, xm_); EW8(xep, 0x0f); EW8(xep, 0xae); \
	  E_MODRM_MEM(xep, 2, xm_, 0); }, \
	x86Emitter::xLDMXCSR(x86Emitter::ptr32[(void*)(addr)]))
#define xe_ucomiss_xm(xmm, addr) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM(xep, 0x00, 0x2e, (xmm), xm_); }, \
	x86Emitter::xUCOMI.SS(x86Emitter::xRegisterSSE(xmm), x86Emitter::ptr32[(void*)(addr)]))


/* iFPU memory-operand SSE forms and the CVT family */
#define XE_SSE_XM(name, pre, opc, cxxexpr) /* doc helper only */
#define xe_andps_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x54, (x), xm_); }, \
	x86Emitter::xAND.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_orps_xm(x, addr)   XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x56, (x), xm_); }, \
	x86Emitter::xOR.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_xorps_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x57, (x), xm_); }, \
	x86Emitter::xXOR.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_por_xm(x, addr)    XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xeb, (x), xm_); }, \
	x86Emitter::xPOR(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_minss_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x5d, (x), xm_); }, \
	x86Emitter::xMIN.SS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_maxss_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x5f, (x), xm_); }, \
	x86Emitter::xMAX.SS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_ucomisd_xm(x, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x2e, (x), xm_); }, \
	x86Emitter::xUCOMI.SD(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_movd_xr(xmm, gpr)  xe_movdzx_xr(xmm, gpr)
#define xe_movd_rx32(gpr, xmm) xe_movd_rx(gpr, xmm)
#define xe_cvttss2si_rx(gpr, xmm) XE_2(E_SSE_RR(xep, 0xf3, 0x2c, (gpr), (xmm)), \
	x86Emitter::xCVTTSS2SI(x86Emitter::xRegister32(gpr), x86Emitter::xRegisterSSE(xmm)))
#define xe_cvtsi2ss_xr(xmm, gpr) XE_2(E_SSE_RR(xep, 0xf3, 0x2a, (xmm), (gpr)), \
	x86Emitter::xCVTSI2SS(x86Emitter::xRegisterSSE(xmm), x86Emitter::xRegister32(gpr)))
#define xe_insertps_xxi(d, s, i) XE_2(E_SSE_RRI_W(xep, 0x66, 0x213a, (d), (s), (i), 0), \
	x86Emitter::xINSERTPS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s), (i)))

/* last iFPU/iFPUd gaps: packed-integer mem forms over the double-domain
 * constants, andpd, cvt with memory sources, cvttss2si from memory,
 * test-mem-imm, and the 32-bit register cmov. */
#define xe_pminsd_xm(x, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x3938, (x), xm_); }, \
	x86Emitter::xPMIN.SD(x86Emitter::xRegisterSSE(x), x86Emitter::ptr128[(void*)(addr)]))
#define xe_pminud_xm(x, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x3b38, (x), xm_); }, \
	x86Emitter::xPMIN.UD(x86Emitter::xRegisterSSE(x), x86Emitter::ptr128[(void*)(addr)]))
#define xe_psubd_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xfa, (x), xm_); }, \
	x86Emitter::xPSUB.D(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_paddd_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xfe, (x), xm_); }, \
	x86Emitter::xPADD.D(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_psubq_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xfb, (x), xm_); }, \
	x86Emitter::xPSUB.Q(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_paddq_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xd4, (x), xm_); }, \
	x86Emitter::xPADD.Q(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_andpd_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x54, (x), xm_); }, \
	x86Emitter::xAND.PD(x86Emitter::xRegisterSSE(x), x86Emitter::ptr[(void*)(addr)]))
#define xe_cvtsi2ss_xm(x, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x2a, (x), xm_); }, \
	x86Emitter::xCVTSI2SS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr32[(void*)(addr)]))
#define xe_cvttss2si_rm(gpr, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x2c, (gpr), xm_); }, \
	x86Emitter::xCVTTSS2SI(x86Emitter::xRegister32(gpr), x86Emitter::ptr32[(void*)(addr)]))
#define xe_test32_mi(addr, imm) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 0, 0, xm_); EW8(xep, 0xf7); \
	E_MODRM_MEM(xep, 0, xm_, 4); EW32(xep, (e_u32)(imm)); }, \
	x86Emitter::xTEST(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))
#define xe_cmovge32_rr(dst, src) XE_2( \
	{ E_REX(xep, 0, (dst), 0, (src)); \
	  EW8(xep, 0x0f); EW8(xep, (e_u8)(0x40 | x86Emitter::Jcc_GreaterOrEqual)); \
	  E_MODRM_RR(xep, (dst), (src)); }, \
	x86Emitter::xCMOVGE(x86Emitter::xRegister32(dst), x86Emitter::xRegister32(src)))

/* or dword [abs], imm (the FPU flag-set sites) */
#define xe_or32_mi(addr, imm) XE_2( \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 1, xm_, (e_s32)(imm)); }, \
	x86Emitter::xOR(x86Emitter::ptr32[(void*)(addr)], (u32)(imm)))

/* microVU_Macro vocabulary: 128-bit moves to/from absolute memory, the
 * 16-bit VI-register traffic, zero-extension in both source shapes, and
 * and64 with an immediate. */
#define xe_movaps_mx(addr, xmm) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x29, (xmm), xm_); }, \
	x86Emitter::xMOVAPS(x86Emitter::ptr128[(void*)(addr)], x86Emitter::xRegisterSSE(xmm)))
#define xe_movzx32_rr16(dst, src) XE_2( \
	{ E_REX(xep, 0, (dst), 0, (src)); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	  E_MODRM_RR(xep, (dst), (src)); }, \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(dst), x86Emitter::xRegister16(src)))
#define xe_movzx32_rm16(dst, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 0, (dst), xm_); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	E_MODRM_MEM(xep, (dst), xm_, 0); }, \
	x86Emitter::xMOVZX(x86Emitter::xRegister32(dst), x86Emitter::ptr16[(void*)(addr)]))
#define xe_mov16_mr(addr, reg) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_P16(xep); E_REX_MEM(xep, 0, (reg), xm_); EW8(xep, 0x89); \
	E_MODRM_MEM(xep, (reg), xm_, 0); }, \
	x86Emitter::xMOV(x86Emitter::ptr16[(void*)(addr)], x86Emitter::xRegister16(reg)))
#define xe_mov16_mi(addr, imm) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_P16(xep); E_REX_MEM(xep, 0, 0, xm_); EW8(xep, 0xc7); \
	E_MODRM_MEM(xep, 0, xm_, 2); \
	EW8(xep, (e_u8)((imm) & 0xff)); EW8(xep, (e_u8)(((imm) >> 8) & 0xff)); }, \
	x86Emitter::xMOV(x86Emitter::ptr16[(void*)(addr)], (u16)(imm)))
#define xe_and64_ri(reg, imm)  XE_2(E_G1_RI(xep, 1, 4, (reg), (e_s32)(imm)), \
	x86Emitter::xAND(x86Emitter::xRegister64(reg), (u32)(imm)))

/* two-register fastcall, replicating prepare()'s argument-shuffle guard
 * byte for byte, including the self-moves the reference emits when the
 * values already sit in the ABI registers, and the push/pop spill when
 * the pair is exactly crossed. */
#define xe_fastcall2_rr(fn, r1, r2) do { \
	if ((r2) != XE_ARG1) { \
		xe_mov64_rr(XE_ARG1, (r1)); \
		xe_mov64_rr(XE_ARG2, (r2)); \
	} else if ((r1) != XE_ARG2) { \
		xe_mov64_rr(XE_ARG2, (r2)); \
		xe_mov64_rr(XE_ARG1, (r1)); \
	} else { \
		xe_push64_r(r1); \
		xe_mov64_rr(XE_ARG2, (r2)); \
		xe_pop64_r(XE_ARG1); \
	} \
	xe_fastcall0(fn); } while (0)
#define XE_ARG1 (x86Emitter::arg1reg.Id)
#define XE_ARG2 (x86Emitter::arg2reg.Id)
#define xe_push64_r(reg) XE_2( \
	{ if ((reg) >= 8) EW8(xep, 0x41); EW8(xep, (e_u8)(0x50 | ((reg) & 7))); }, \
	x86Emitter::xPUSH(x86Emitter::xRegister64(reg)))
#define xe_pop64_r(reg) XE_2( \
	{ if ((reg) >= 8) EW8(xep, 0x41); EW8(xep, (e_u8)(0x58 | ((reg) & 7))); }, \
	x86Emitter::xPOP(x86Emitter::xRegister64(reg)))

/* microVU compile-path vocabulary: packed min/max against memory, the
 * direct near call (xCALL is always rel32 in the reference's
 * void* overload), and the two-operand pmin forms over indexed tables. */
#define xe_minps_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x5d, (x), xm_); }, \
	x86Emitter::xMIN.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr32[(void*)(addr)]))
#define xe_maxps_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x5f, (x), xm_); }, \
	x86Emitter::xMAX.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr32[(void*)(addr)]))
#define xe_call_ptr(fn) XE_2(E_CALL_REL(xep, (const void*)(fn)), \
	x86Emitter::xCALL((void*)(fn)))

/* mov qword [abs], imm32 (sign-extended REX.W C7 /0) */
#define xe_mov64_mi_s32(addr, imm) XE_2( \
	E_MOV_M_I64(xep, (e_uptr)(addr), (e_s32)(imm)), \
	x86Emitter::xMOV(x86Emitter::ptr64[(void*)(addr)], (imm)))

/* Bridge from the C++ xAddressVoid (base+index*factor+disp address
 * object, factor form) into e_mem, with an extra displacement for the
 * lane offsets mVUsaveReg/mVUloadReg add at each site. E_MEM performs
 * the factor->SIB reduction, same as the xIndirect constructors. */
#define XE_MEM_XAV(m, av, extra) E_MEM(m, \
	(av).Base.IsEmpty()  ? E_NOREG : (av).Base.Id, \
	(av).Index.IsEmpty() ? E_NOREG : (av).Index.Id, \
	(av).Index.IsEmpty() ? 0 : (av).Factor, \
	(e_sptr)(av).Displacement + (e_sptr)(extra))

/* generic-e_mem SSE moves for address-object and rsp-relative operands */
#define xe_movss_xmemg(x, m) XE_2(E_SSE_R_MEM(xep, 0xf3, 0x10, (x), (m)), \
	x86Emitter::xMOVSSZX(x86Emitter::xRegisterSSE(x), x86Emitter::ptr32[XE_MEM_TO_ADDR(m)]))
#define xe_movss_memxg(m, x) XE_2(E_SSE_R_MEM(xep, 0xf3, 0x11, (x), (m)), \
	x86Emitter::xMOVSS(x86Emitter::ptr32[XE_MEM_TO_ADDR(m)], x86Emitter::xRegisterSSE(x)))
#define xe_movaps_xmemg(x, m) XE_2(E_SSE_R_MEM(xep, 0x00, 0x28, (x), (m)), \
	x86Emitter::xMOVAPS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr128[XE_MEM_TO_ADDR(m)]))
#define xe_movaps_memxg(m, x) XE_2(E_SSE_R_MEM(xep, 0x00, 0x29, (x), (m)), \
	x86Emitter::xMOVAPS(x86Emitter::ptr128[XE_MEM_TO_ADDR(m)], x86Emitter::xRegisterSSE(x)))
#define xe_movlps_memxg(m, x) XE_2(E_SSE_R_MEM(xep, 0x00, 0x13, (x), (m)), \
	x86Emitter::xMOVL.PS(x86Emitter::ptr64[XE_MEM_TO_ADDR(m)], x86Emitter::xRegisterSSE(x)))
#define xe_movhps_memxg(m, x) XE_2(E_SSE_R_MEM(xep, 0x00, 0x17, (x), (m)), \
	x86Emitter::xMOVH.PS(x86Emitter::ptr64[XE_MEM_TO_ADDR(m)], x86Emitter::xRegisterSSE(x)))
/* extractps [mem], xmm, imm: 66 0F 3A 17, xmm in the reg field */
#define xe_extractps_memxi(m, x, i) XE_2( \
	{ EW8(xep, 0x66); E_REX_MEM(xep, 0, (x), (m)); \
	  EW8(xep, 0x0f); EW8(xep, 0x3a); EW8(xep, 0x17); \
	  E_MODRM_MEM(xep, (x), (m), 1); EW8(xep, (e_u8)(i)); }, \
	x86Emitter::xEXTRACTPS(x86Emitter::ptr32[XE_MEM_TO_ADDR(m)], x86Emitter::xRegisterSSE(x), (i)))
#define xe_minpd_xx(d, s2) XE_2(E_SSE_RR(xep, 0x66, 0x5d, (d), (s2)), \
	x86Emitter::xMIN.PD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s2)))
#define xe_maxpd_xx(d, s2) XE_2(E_SSE_RR(xep, 0x66, 0x5f, (d), (s2)), \
	x86Emitter::xMAX.PD(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s2)))

/* microVU_Upper vocabulary: packed compares (CMPPS with the type
 * immediate, 0=EQ 5=NLT per SSE2_ComparisonType), packed multiply and
 * gt-compare against memory, the float<->int packed converts, the clip
 * pack/blend/movmsk trio, and movmskps into any gpr. */
#define xe_cmpeqps_xx(d, s2)  XE_2(E_SSE_RRI(xep, 0x00, 0xc2, (d), (s2), 0), \
	x86Emitter::xCMPEQ.PS(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s2)))
#define xe_cmpnltps_xm(x, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0xc2, (x), xm_); EW8(xep, 5); }, \
	x86Emitter::xCMPNLT.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr128[(void*)(addr)]))
#define xe_mulps_xm(x, addr)  XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x59, (x), xm_); }, \
	x86Emitter::xMUL.PS(x86Emitter::xRegisterSSE(x), x86Emitter::ptr128[(void*)(addr)]))
#define xe_pcmpgtd_xm(x, addr) XE_2({ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x66, (x), xm_); }, \
	x86Emitter::xPCMP.GTD(x86Emitter::xRegisterSSE(x), x86Emitter::ptr128[(void*)(addr)]))
#define xe_cvttps2dq_xx(d, s2) XE_2(E_SSE_RR(xep, 0xf3, 0x5b, (d), (s2)), \
	x86Emitter::xCVTTPS2DQ(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s2)))
#define xe_pblendw_xxi(d, s2, i) XE_2(E_SSE_RRI(xep, 0x66, 0x0e3a, (d), (s2), (i)), \
	x86Emitter::xPBLEND.W(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s2), (i)))
#define xe_packsswb_xx(d, s2) XE_2(E_SSE_RR(xep, 0x66, 0x63, (d), (s2)), \
	x86Emitter::xPACK.SSWB(x86Emitter::xRegisterSSE(d), x86Emitter::xRegisterSSE(s2)))
#define xe_pmovmskb_rx(gpr, x) XE_2(E_SSE_RR(xep, 0x66, 0xd7, (gpr), (x)), \
	x86Emitter::xPMOVMSKB(x86Emitter::xRegister32(gpr), x86Emitter::xRegisterSSE(x)))
#endif /* PCSX2_C89OPS_H */
