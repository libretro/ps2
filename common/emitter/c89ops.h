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
 * XE_AB is 58,301 byte-compared emissions or an abort. */
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

#endif /* PCSX2_C89OPS_H */
