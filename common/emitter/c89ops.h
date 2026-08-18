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
#define XE_AB 1
#if XE_AB
extern "C" int xe_cpp_mode; /* defined in JitHash.cpp, set from env at load */
extern "C" unsigned long long xe_site_hits; /* ditto; both modes count */
#define XE_2(c89body, cppbody) do { \
	xe_site_hits++; \
	if (xe_cpp_mode) { cppbody; } else { XE_OPEN(); c89body; XE_CLOSE(); } } while (0)
#else
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
#define xe_mov64_mi(addr, tmpreg, imm) do { \
	xe_site_hits++; \
	if (xe_cpp_mode) { \
		x86Emitter::xImm64Op(x86Emitter::xMOV, x86Emitter::ptr64[(void*)(addr)], \
			x86Emitter::xRegister64(tmpreg), (s64)(imm)); \
	} else if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		XE_OPEN(); E_MOV_M_I64(xep, (e_uptr)(addr), (e_s32)(imm)); XE_CLOSE(); \
	} else { \
		xe_mov64_ri((tmpreg), (imm)); \
		xe_mov64_mr((addr), (tmpreg)); \
	} } while (0)

/* xImm64Op(xMOV, xRegister64(dst), tmp, imm), byte-for-byte: when the
 * immediate fits s32 the reference calls op(dst, imm) directly; when it
 * does not, it loads the SCRATCH register and movs scratch to dst -- two
 * instructions, not one B8+imm64 to the destination. Matching the
 * reference's suboptimality is the contract; shaving it is a later,
 * oracle-visible change of its own. */
#define xe_imm64op_mov_rr(dst, tmpreg, imm) do { \
	xe_site_hits++; \
	if (xe_cpp_mode) { \
		x86Emitter::xImm64Op(x86Emitter::xMOV, x86Emitter::xRegister64(dst), \
			x86Emitter::xRegister64(tmpreg), (s64)(imm)); \
	} else if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		XE_OPEN(); E_MOV_RI(xep, 1, 0, (dst), (e_sptr)(imm)); XE_CLOSE(); \
	} else { \
		xe_mov64_ri((tmpreg), (imm)); \
		xe_mov64_rr((dst), (tmpreg)); \
	} } while (0)


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
#define XE_IMM64_G1_RR(opn, cxxop, dst, tmpreg, imm) do { \
	if (xe_cpp_mode) { \
		xe_site_hits++; \
		x86Emitter::xImm64Op(x86Emitter::cxxop, x86Emitter::xRegister64(dst), \
			x86Emitter::xRegister64(tmpreg), (s64)(imm)); \
	} else { \
		xe_site_hits++; \
		if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
			XE_OPEN(); E_G1_RI(xep, 1, opn, (dst), (e_s32)(imm)); XE_CLOSE(); \
		} else { \
			XE_OPEN(); E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
			E_G1_RR(xep, 1, opn, (dst), (tmpreg)); XE_CLOSE(); \
		} } } while (0)
#define xe_imm64op_add64_ri(dst, tmp, imm) XE_IMM64_G1_RR(0, xADD, dst, tmp, imm)
#define xe_imm64op_sub64_ri(dst, tmp, imm) XE_IMM64_G1_RR(5, xSUB, dst, tmp, imm)
#define xe_imm64op_cmp64_ri(dst, tmp, imm) XE_IMM64_G1_RR(7, xCMP, dst, tmp, imm)

#define xe_imm64op_cmp64_mi(addr, tmpreg, imm) do { \
	xe_site_hits++; \
	if (xe_cpp_mode) { \
		x86Emitter::xImm64Op(x86Emitter::xCMP, x86Emitter::ptr64[(void*)(addr)], \
			x86Emitter::xRegister64(tmpreg), (s64)(imm)); \
	} else if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		/* E_G1_MI is width-less (32-bit); this is the 64-bit form: REX.W
		 * then the same s8-narrowed body. */ \
		XE_OPEN(); \
		E_REX(xep, 1, 0, 0, 0); \
		if (E_IS_S8((e_s32)(imm))) { \
			EW8(xep, 0x83); E_MODRM_ABS(xep, 7, (e_uptr)(addr), 1); EW8(xep, (e_u8)(imm)); \
		} else { \
			EW8(xep, 0x81); E_MODRM_ABS(xep, 7, (e_uptr)(addr), 4); EW32(xep, (e_s32)(imm)); \
		} \
		XE_CLOSE(); \
	} else { \
		XE_OPEN(); E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_MR(xep, 1, 7, (tmpreg), (e_uptr)(addr)); XE_CLOSE(); \
	} } while (0)


/* xImm64Op with the group1 operator chosen at runtime (recLogicalOp_constv). */
#define XE_IMM64_G1_CXX(g1op, dst, tmpreg, imm) do { \
	switch (g1op) { \
	case 0: x86Emitter::xImm64Op(x86Emitter::xADD, x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	case 1: x86Emitter::xImm64Op(x86Emitter::xOR,  x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	case 4: x86Emitter::xImm64Op(x86Emitter::xAND, x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	case 6: x86Emitter::xImm64Op(x86Emitter::xXOR, x86Emitter::xRegister64(dst), x86Emitter::xRegister64(tmpreg), (s64)(imm)); break; \
	default: break; } } while (0)
#define xe_imm64op_g1op64_ri(g1op, dst, tmpreg, imm) do { \
	xe_site_hits++; \
	if (xe_cpp_mode) { XE_IMM64_G1_CXX((g1op), (dst), (tmpreg), (imm)); } \
	else if ((e_s64)(imm) == (e_s64)(e_s32)(imm)) { \
		XE_OPEN(); E_G1_RI(xep, 1, (g1op), (dst), (e_s32)(imm)); XE_CLOSE(); \
	} else { \
		XE_OPEN(); E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_RR(xep, 1, (g1op), (dst), (tmpreg)); XE_CLOSE(); \
	} } while (0)


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

#endif /* PCSX2_C89OPS_H */
