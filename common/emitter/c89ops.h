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
 * reference emitter by the suites in tests/emitter; during the migration
 * every conversion was additionally gated by a shadow-compare oracle and
 * the JIT region hashes (pcsx2_jithash_dump) on a fixed trace.
 */

#ifndef PCSX2_C89OPS_H
#define PCSX2_C89OPS_H


#include "common/emitter/c89emit.h"

/* The shadow-compare A/B oracle is retired. During the migration every
 * xe_* macro carried two arms -- its C89 body and the exact C++ call it
 * replaced -- and an XE_AB build verified each converted site on each
 * emission: the C++ twin emitted at the cursor, the bytes were
 * snapshotted, the cursor rewound, the C89 arm emitted at the SAME
 * address (so embedded host pointers and RIP-relative displacements were
 * identical by construction), and any length or byte mismatch aborted
 * with both hex dumps. At retirement the fixed BIOS boot trace was
 * byte-verifying 60,917 site executions per run, on top of the ~294K
 * case byte suites in tests/emitter. The twins, the XE_2 wrapper, and
 * the PCSX2_XE_AB configuration are gone; what follows is the surviving
 * single-arm surface. The argument-idempotence discipline the oracle
 * enforced (both arms evaluated macro arguments, so side-effecting
 * arguments had to be hoisted) is kept as a style rule: macro arguments
 * may still be evaluated more than once by a single arm.
 */


/* The cursor protocol. x86Ptr is the global cursor shared with the C++ API
 * during the migration; these are the only two places this header touches
 * it. */
#define XE_OPEN() uint8_t* xep = (uint8_t*)x86Ptr
#define XE_CLOSE() (x86Ptr = (u8*)xep)
/* Refresh after a call that may have emitted (allocator spill paths). */
#define XE_REOPEN() (xep = (uint8_t*)x86Ptr)

/* Register ids, spelled like the hardware. Same numbering as
 * xRegister32(n).Id, so allocator results pass straight through. */
#define XE_AX 0
#define XE_CX 1
#define XE_DX 2
#define XE_BX 3
/* C89 min/max, evaluate-twice caveat accepted at the converted sites
 * (all operands there are plain locals or fields). */
#define C89_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define C89_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN_U32(a, b) ((((u32)(a)) < ((u32)(b))) ? ((u32)(a)) : ((u32)(b)))

#define XE_WORDSIZE 8
#define XE_SP 4

/* Caller-saved (volatile) GPR predicate, mirroring
 * xRegisterBase::IsCallerSaved exactly. */
#ifdef _WIN32
#define XE_IS_CALLER_SAVED(id) ((id) <= 2 || ((id) >= 8 && (id) <= 11))
#else
#define XE_IS_CALLER_SAVED(id) ((id) <= 2 || (id) == 6 || (id) == 7 || ((id) >= 8 && (id) <= 11))
#endif
#define XE_BP 5
#define XE_SI 6
#define XE_DI 7

/* ---- per-call wrappers ------------------------------------------------ */
/* Naming: xe_<op><width>_<operands>; rr = reg,reg; ri = reg,imm;
 * mr/rm = absolute-address memory forms. Width is the operand size of the
 * instruction, not of any one operand. */

#define xe_mov32_rr(to, from)      do { XE_OPEN(); E_MOV_RR(xep, 0, (to), (from)); XE_CLOSE(); } while (0)
#define xe_mov64_rr(to, from)      do { XE_OPEN(); E_MOV_RR(xep, 1, (to), (from)); XE_CLOSE(); } while (0)
#define xe_mov32_ri(to, imm)       do { XE_OPEN(); E_MOV_RI(xep, 0, 0, (to), (intptr_t)(imm)); XE_CLOSE(); } while (0)
#define xe_mov64_ri(to, imm)       do { XE_OPEN(); E_MOV64_RI(xep, 0, (to), (imm)); XE_CLOSE(); } while (0)
#define xe_mov64_mr(addr, reg)     do { XE_OPEN(); E_MOV_M_R(xep, 1, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_mov32_mr(addr, reg)     do { XE_OPEN(); E_MOV_M_R(xep, 0, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_mov64_rm(reg, addr)     do { XE_OPEN(); E_MOV_R_M(xep, 1, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_mov32_rm(reg, addr)     do { XE_OPEN(); E_MOV_R_M(xep, 0, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)

/* movsxd r64, r32: opcode 0x63, a plain opcode with no 0F escape --
 * E_MOVEXT_RR only covers the 0F BE/BF byte and word sources. */
#define xe_movsxd_rr(to, from)     do { XE_OPEN();  \
	E_REX(xep, 1, (to), 0, (from)); \
	EW8(xep, 0x63); \
	E_MODRM_RR(xep, (to), (from)); XE_CLOSE(); } while (0)

/* cdq / cdqe */
#define xe_cdq()                   do { XE_OPEN(); EW8(xep, 0x99); XE_CLOSE(); } while (0)  /* xCDQ is a macro; no namespace */
#define xe_cdqe()                  do { XE_OPEN(); EW8(xep, 0x48); EW8(xep, 0x98); XE_CLOSE(); } while (0)  /* macro twin */

/* pinsrd/pinsrq xmm, r32/r64, imm8 (66 0f 3a 22 /r ib, REX.W selects Q) */
#define xe_pinsrd(xmm, gpr, imm)   do { XE_OPEN(); E_SSE_RRI_W(xep, 0x66, 0x223a, (xmm), (gpr), (imm), 0); XE_CLOSE(); } while (0)
#define xe_pinsrq(xmm, gpr, imm)   do { XE_OPEN(); E_SSE_RRI_W(xep, 0x66, 0x223a, (xmm), (gpr), (imm), 1); XE_CLOSE(); } while (0)

/* mov [abs], imm64 via a scratch register when it does not fit s32 --
 * byte-identical to the reference xImm64Op(xMOV, ptr64[addr], tmp, imm)
 * composition: op(dst, imm) when s32-representable, else MOV64 tmp, imm
 * followed by the store. */
#define xe_mov64_mi(addr, tmpreg, imm) do { XE_OPEN();  \
	{ if ((int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
		E_MOV_M_I64(xep, (uintptr_t)(addr), (int32_t)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_MOV_M_R(xep, 1, (tmpreg), (uintptr_t)(addr)); \
	  } }; XE_CLOSE(); } while (0)

/* xImm64Op(xMOV, xRegister64(dst), tmp, imm), byte-for-byte: when the
 * immediate fits s32 the reference calls op(dst, imm) directly; when it
 * does not, it loads the SCRATCH register and movs scratch to dst -- two
 * instructions, not one B8+imm64 to the destination. Matching the
 * reference's suboptimality is the contract; shaving it is a later,
 * oracle-visible change of its own. */
#define xe_imm64op_mov_rr(dst, tmpreg, imm) do { XE_OPEN();  \
	{ if ((int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
		E_MOV_RI(xep, 1, 0, (dst), (intptr_t)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_MOV_RR(xep, 1, (dst), (tmpreg)); \
	  } }; XE_CLOSE(); } while (0)


/* group1: op codes 0 ADD, 2 ADC, 6 XOR, 7 CMP -- ri keeps the reference's
 * s8 narrowing to 0x83 and the AX short form; rm is the absolute-address
 * load form (op reg, [abs]). */
#define xe_add32_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 0, 0, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_adc32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 2, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
#define xe_adc32_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 0, 2, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_cmp32_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 0, 7, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_xor32_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 0, 6, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_add32_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, 0, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_adc32_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, 2, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)

/* group2 shifts by immediate: 4 SHL, 7 SAR; the by-1 short form (0xD1) is
 * inside E_G2_RI, byte-verified. */
#define xe_shl32_ri(reg, imm)  do { XE_OPEN(); E_G2_RI(xep, 0, 4, (reg), (imm)); XE_CLOSE(); } while (0)
#define xe_sar32_ri(reg, imm)  do { XE_OPEN(); E_G2_RI(xep, 0, 7, (reg), (imm)); XE_CLOSE(); } while (0)

/* group3 one-operand: 2 NOT, 4 MUL, 5 IMUL, 6 DIV, 7 IDIV; register and
 * absolute-memory forms. The C++ xMUL/xUMUL/xDIV/xUDIV one-operand
 * spellings map to IMUL/MUL/IDIV/DIV respectively. */
#define xe_not32_r(reg)        do { XE_OPEN(); E_G3_R(xep, 0, 2, (reg)); XE_CLOSE(); } while (0)
#define xe_imul32_r(reg)       do { XE_OPEN(); E_G3_R(xep, 0, 5, (reg)); XE_CLOSE(); } while (0)
#define xe_mul32_r(reg)        do { XE_OPEN(); E_G3_R(xep, 0, 4, (reg)); XE_CLOSE(); } while (0)
#define xe_idiv32_r(reg)       do { XE_OPEN(); E_G3_R(xep, 0, 7, (reg)); XE_CLOSE(); } while (0)
#define xe_div32_r(reg)        do { XE_OPEN(); E_G3_R(xep, 0, 6, (reg)); XE_CLOSE(); } while (0)
#define xe_imul32_m(addr)      do { XE_OPEN(); E_G3_M(xep, 5, (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_mul32_m(addr)       do { XE_OPEN(); E_G3_M(xep, 4, (uintptr_t)(addr)); XE_CLOSE(); } while (0)


/* group1 rr/rm at both widths, per-op with twins. */
#define XE_G1_RR_(name, opn, cxx, w, RT) \
	/* nothing -- expanded manually below for twin fidelity */
#define xe_add32_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 0, 0, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_add64_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 1, 0, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_sub32_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 0, 5, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_sub64_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 1, 5, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_xor64_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 1, 6, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_cmp64_rr(a, b)      do { XE_OPEN(); E_G1_RR(xep, 1, 7, (a), (b)); XE_CLOSE(); } while (0)
#define xe_add64_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 1, 0, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_sub32_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, 5, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_sub64_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 1, 5, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_cmp64_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 1, 7, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_sub32_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 0, 5, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)

#define xe_g1op64_rr(g1op, dst, src)  do { XE_OPEN(); E_G1_RR(xep, 1, (g1op), (dst), (src)); XE_CLOSE(); } while (0)
#define xe_g1op64_rm(g1op, reg, addr) do { XE_OPEN(); E_G1_RM(xep, 1, (g1op), (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)

#define xe_not64_r(reg)        do { XE_OPEN(); E_G3_R(xep, 1, 2, (reg)); XE_CLOSE(); } while (0)

/* setcc r8 on an allocator id. cc is the Jcc_* comparison number (the low
 * opcode nibble). Ids 4-7 are spl/bpl/sil/dil and need a bare REX. */
#define xe_setcc_r8(cc, reg)   do { XE_OPEN();  \
	{ uint8_t xrex_ = (uint8_t)(0x40 | (((reg) >= 8) ? 1 : 0)); \
	  if (xrex_ != 0x40 || ((reg) >= 4 && (reg) <= 7)) EW8(xep, xrex_); } \
	EW8(xep, 0x0f); EW8(xep, (uint8_t)(0x90 | (cc))); E_MODRM_RR(xep, 0, (reg)); XE_CLOSE(); } while (0)

/* xImm64Op over group1 (r64 dest and abs-mem dest), per-op, matching the
 * reference composition exactly including the scratch round-trip. */
#define XE_IMM64_G1_RR(opn, dst, tmpreg, imm) do { XE_OPEN();  \
	{ if ((int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
		E_G1_RI(xep, 1, opn, (dst), (int32_t)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_RR(xep, 1, opn, (dst), (tmpreg)); \
	  } }; XE_CLOSE(); } while (0)
#define xe_imm64op_add64_ri(dst, tmp, imm) XE_IMM64_G1_RR(0, dst, tmp, imm)
#define xe_imm64op_sub64_ri(dst, tmp, imm) XE_IMM64_G1_RR(5, dst, tmp, imm)
#define xe_imm64op_cmp64_ri(dst, tmp, imm) XE_IMM64_G1_RR(7, dst, tmp, imm)

#define xe_imm64op_cmp64_mi(addr, tmpreg, imm) do { XE_OPEN();  \
	{ if ((int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
		/* E_G1_MI is width-less (32-bit); this is the 64-bit form: REX.W \
		 * then the same s8-narrowed body. */ \
		E_REX(xep, 1, 0, 0, 0); \
		if (E_IS_S8((int32_t)(imm))) { \
			EW8(xep, 0x83); E_MODRM_ABS(xep, 7, (uintptr_t)(addr), 1); EW8(xep, (uint8_t)(imm)); \
		} else { \
			EW8(xep, 0x81); E_MODRM_ABS(xep, 7, (uintptr_t)(addr), 4); EW32(xep, (int32_t)(imm)); \
		} \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_MR(xep, 1, 7, (tmpreg), (uintptr_t)(addr)); \
	  } }; XE_CLOSE(); } while (0)


#define xe_imm64op_g1op64_ri(g1op, dst, tmpreg, imm) do { XE_OPEN();  \
	{ if ((int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
		E_G1_RI(xep, 1, (g1op), (dst), (int32_t)(imm)); \
	  } else { \
		E_MOV64_RI(xep, 0, (tmpreg), (imm)); \
		E_G1_RR(xep, 1, (g1op), (dst), (tmpreg)); \
	  } }; XE_CLOSE(); } while (0)


/* remaining group2 named-immediate forms */
#define xe_shl64_ri(reg, imm)  do { XE_OPEN(); E_G2_RI(xep, 1, 4, (reg), (imm)); XE_CLOSE(); } while (0)
#define xe_shr32_ri(reg, imm)  do { XE_OPEN(); E_G2_RI(xep, 0, 5, (reg), (imm)); XE_CLOSE(); } while (0)
#define xe_shr64_ri(reg, imm)  do { XE_OPEN(); E_G2_RI(xep, 1, 5, (reg), (imm)); XE_CLOSE(); } while (0)
#define xe_sar64_ri(reg, imm)  do { XE_OPEN(); E_G2_RI(xep, 1, 7, (reg), (imm)); XE_CLOSE(); } while (0)

#define xe_g2op32_rcl(g2op, reg)  do { XE_OPEN(); E_G2_RCL(xep, 0, (g2op), (reg)); XE_CLOSE(); } while (0)
#define xe_g2op64_rcl(g2op, reg)  do { XE_OPEN(); E_G2_RCL(xep, 1, (g2op), (reg)); XE_CLOSE(); } while (0)


/* absolute-address e_mem, for the SSE/CMov wrappers below */
#define XE_MEM_ABS(m, addr) E_MEM(m, E_NOREG, E_NOREG, 0, (intptr_t)(addr))

/* SSE register moves used by the HILO<->xmm paths */
#define xe_movhlps(dx, sx)     do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x12, (dx), (sx)); XE_CLOSE(); } while (0)
#define xe_movlhps(dx, sx)     do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x16, (dx), (sx)); XE_CLOSE(); } while (0)
#define xe_movsd_xx(dx, sx)    do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x10, (dx), (sx)); XE_CLOSE(); } while (0)

/* movq gpr64<-xmm (the reference spells it xMOVD on a 64-bit GPR) and
 * movq [abs]<-xmm / xmm<-nothing else needed yet */
#define xe_movq_rx(gpr, xmm)   do { XE_OPEN(); E_SSE_RR_W(xep, 0x66, 0x7e, (xmm), (gpr), 1); XE_CLOSE(); } while (0)
#define xe_movq_mx(addr, xmm)  do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0x66, 0xd6, (xmm), xm_, 0); }; XE_CLOSE(); } while (0)

/* pinsrq/pextrq with memory forms on absolute addresses */
#define xe_pinsrq_xm(xmm, addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_I_W(xep, 0x66, 0x223a, (xmm), xm_, (imm), 1); }; XE_CLOSE(); } while (0)
#define xe_pextrq_rx(gpr, xmm, imm) do { XE_OPEN();  \
	E_SSE_RRI_W(xep, 0x66, 0x163a, (xmm), (gpr), (imm), 1); XE_CLOSE(); } while (0)
#define xe_pextrq_mx(addr, xmm, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_I_W(xep, 0x66, 0x163a, (xmm), xm_, (imm), 1); }; XE_CLOSE(); } while (0)

/* test r64, r64 */
#define xe_test64_rr(a, b)     do { XE_OPEN(); E_TEST_RR_SZ(xep, 8, (a), (b)); XE_CLOSE(); } while (0)

/* cmp qword [abs], imm -- E_G1_MEM_I with an absolute e_mem */
#define xe_cmp64_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 8, 7, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)

/* cmovcc r64, r64 / r64, [abs]; cc is the Jcc_ number */
#define xe_cmovcc64_rr(cc, dst, src) do { XE_OPEN();  \
	E_REX(xep, 1, (dst), 0, (src)); EW8(xep, 0x0f); \
	EW8(xep, (uint8_t)(0x40 | (cc))); E_MODRM_RR(xep, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_cmovb64_rr(dst, src) xe_cmovcc64_rr(Jcc_Below, (dst), (src))
#define xe_cmovcc64_rm(cc, dst, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 1, (dst), xm_); EW8(xep, 0x0f); \
	  EW8(xep, (uint8_t)(0x40 | (cc))); E_MODRM_MEM(xep, (dst), xm_, 0); }; XE_CLOSE(); } while (0)


/* mem-dest forms on absolute addresses: mov/sub with reg and imm sources,
 * 32-bit; plus the 32-bit test/cmov/cmp companions the IOP core uses. */
#define xe_mov32_mi(addr, imm) do { XE_OPEN();  \
	E_MOV_M_I(xep, (uintptr_t)(addr), (uint32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_sub32_mr(addr, reg) do { XE_OPEN(); E_G1_MR(xep, 0, 5, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_sub32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 5, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
#define xe_cmp32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 7, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
#define xe_cmp32_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, 7, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_test32_rr(a, b)     do { XE_OPEN(); E_TEST_RR_SZ(xep, 4, (a), (b)); XE_CLOSE(); } while (0)
#define xe_cmovcc32_rm(cc, dst, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 0, (dst), xm_); EW8(xep, 0x0f); \
	  EW8(xep, (uint8_t)(0x40 | (cc))); E_MODRM_MEM(xep, (dst), xm_, 0); }; XE_CLOSE(); } while (0)


/* the 32-bit group1 forms the IOP tables add: and/or at rr/ri/rm, plus
 * generic runtime-operator 32-bit pair, mem-dest reg/imm adds, neg. */
#define xe_and32_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 0, 4, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_and32_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 0, 4, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_and32_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, 4, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_or32_rr(dst, src)   do { XE_OPEN(); E_G1_RR(xep, 0, 1, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_or32_ri(reg, imm)   do { XE_OPEN(); E_G1_RI(xep, 0, 1, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_or32_rm(reg, addr)  do { XE_OPEN(); E_G1_RM(xep, 0, 1, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_xor32_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 0, 6, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_xor32_rm(reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, 6, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_cmp32_rr(a, b)      do { XE_OPEN(); E_G1_RR(xep, 0, 7, (a), (b)); XE_CLOSE(); } while (0)
#define xe_add32_mr(addr, reg) do { XE_OPEN(); E_G1_MR(xep, 0, 0, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_add32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 0, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
#define xe_g1op32_ri(g1op, reg, imm) do { XE_OPEN(); E_G1_RI(xep, 0, (g1op), (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_g1op64_ri(g1op, reg, imm) do { XE_OPEN(); E_G1_RI(xep, 1, (g1op), (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_g1op32_rr(g1op, dst, src)  do { XE_OPEN(); E_G1_RR(xep, 0, (g1op), (dst), (src)); XE_CLOSE(); } while (0)
#define xe_g1op32_rm(g1op, reg, addr) do { XE_OPEN(); E_G1_RM(xep, 0, (g1op), (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)

#define xe_neg32_r(reg)        do { XE_OPEN(); E_G3_R(xep, 0, 3, (reg)); XE_CLOSE(); } while (0)
#define xe_idiv32_m(addr)      do { XE_OPEN(); E_G3_M(xep, 7, (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_div32_m(addr)       do { XE_OPEN(); E_G3_M(xep, 6, (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_not32_m(addr)       do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G3_MEM(xep, 0, 2, xm_); }; XE_CLOSE(); } while (0)

/* movzx r32 <- byte [abs] and r32 <- r8; the byte-register REX rule
 * (bare 0x40 for ids 4-7) lives in E_MOVEXT_RR's srcw==0 branch. */
#define xe_movzx32_r8(dst, src8) do { XE_OPEN(); E_MOVEXT_RR(xep, 0, 0, 0, (dst), (src8)); XE_CLOSE(); } while (0)


#define xe_movzx32_r16(dst, src) do { XE_OPEN(); E_MOVEXT_RR(xep, 0, 0, 1, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_movzx32_m8(dst, addr) do { XE_OPEN();  \
	E_REX(xep, 0, (dst), 0, 0); EW8(xep, 0x0f); EW8(xep, 0xb6); \
	E_MODRM_ABS(xep, (dst), (uintptr_t)(addr), 0); XE_CLOSE(); } while (0)
#define xe_movzx32_m16(dst, addr) do { XE_OPEN();  \
	E_REX(xep, 0, (dst), 0, 0); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	E_MODRM_ABS(xep, (dst), (uintptr_t)(addr), 0); XE_CLOSE(); } while (0)


/* movsx r32 <- r8/r16 (0F BE / 0F BF) */
#define xe_movsx32_r8(dst, src8) do { XE_OPEN(); E_MOVEXT_RR(xep, 0, 1, 0, (dst), (src8)); XE_CLOSE(); } while (0)
#define xe_movsx32_r16(dst, src) do { XE_OPEN(); E_MOVEXT_RR(xep, 0, 1, 1, (dst), (src)); XE_CLOSE(); } while (0)

/* shifts by CL, named */
#define xe_shl32_rcl(reg)      do { XE_OPEN(); E_G2_RCL(xep, 0, 4, (reg)); XE_CLOSE(); } while (0)
#define xe_shr32_rcl(reg)      do { XE_OPEN(); E_G2_RCL(xep, 0, 5, (reg)); XE_CLOSE(); } while (0)


#define xe_test32_ri(reg, imm) do { XE_OPEN(); E_TEST_RI_SZ(xep, 4, (reg), (imm)); XE_CLOSE(); } while (0)
#define xe_and32_mr(addr, reg) do { XE_OPEN(); E_G1_MR(xep, 0, 4, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)
#define xe_or32_mr(addr, reg)  do { XE_OPEN(); E_G1_MR(xep, 0, 1, (reg), (uintptr_t)(addr)); XE_CLOSE(); } while (0)


/* ---- control flow ---------------------------------------------------- */

/* fastcall to a fixed target: near call when the displacement fits s32,
 * else lea rax, [target]; call rax -- the reference's exact choice
 * (jmp.cpp:84-93), so emitted length depends on where the cache landed. */
#define xe_fastcall0(fn) do { XE_OPEN();  \
	{ intptr_t xd_ = ((intptr_t)xep + 5) - (intptr_t)(fn); \
	  if (xd_ == (intptr_t)(int32_t)xd_) { E_CALL_REL(xep, (fn)); } \
	  else { \
		struct e_mem xm_; XE_MEM_ABS(xm_, (fn)); \
		E_LEA(xep, 1, 0, 0 /* rax */, xm_); \
		E_CALL_R(xep, 0); \
	  } }; XE_CLOSE(); } while (0)

/* forward jumps: emit with a zero displacement, remember the slot as a
 * plain uint8_t*, patch at the target. The C++ twins use the legacy
 * byte-writer macros (same 0xEB / 0x70|cc encodings, same byte-verified
 * lineage) because a twin arm cannot share an xForwardJump8 object whose
 * constructor emits. */
#define xe_fwd_jmp8(slot) do { XE_OPEN();  \
	EW8(xep, 0xeb); (slot) = xep; EW8(xep, 0); XE_CLOSE(); } while (0)
#define xe_fwd_jcc8(cc, slot) do { XE_OPEN();  \
	/* mirror xForwardJump<s8> exactly: unconditional is EB, not 0x70|cc. \
	 * Jcc_Unconditional is -1, and 0x70 | -1 truncates to 0xFF -- a real \
	 * opcode byte, and a crash the first time the block runs. */ \
	EW8(xep, ((cc) == Jcc_Unconditional) ? (uint8_t)0xeb \
	                                                 : (uint8_t)(0x70 | (cc))); \
	(slot) = xep; EW8(xep, 0); XE_CLOSE(); } while (0)
/* The reference emitter asserted the displacement fit; the pair must too,
 * because a silent wrap here is exactly the FBRST family of bug: bytes that
 * look plausible and jump somewhere real. Trap hard at recompile time. */
#define xe_fwd_set8(slot) do { XE_OPEN();  \
	intptr_t xfd_ = (intptr_t)(xep - ((slot) + 1)); \
	if (xfd_ != (intptr_t)(int8_t)xfd_) E_FWD_OVERFLOW_TRAP(); \
	*(slot) = (uint8_t)xfd_; XE_CLOSE(); } while (0)

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
/* The disp32-fit test must run at runtime on the loaded image's real
 * addresses. Written directly on a static's address, the truncation
 * round-trip is a constant expression, and MSVC folds it to true at
 * compile time -- the else arm is dead-stripped and the emitted operand
 * carries only the low four address bytes, which is correct exactly
 * until ASLR maps the image above 4GB. A volatile round-trip is an
 * observable access the compiler must perform, so the comparison always
 * sees the runtime value. Emission-time cost only; the emitted bytes
 * for the fitting case are unchanged. */
static uintptr_t xe_opaque_uptr(const void *p)
{
	volatile uintptr_t v = (uintptr_t)p;
	return v;
}

#define xe_complexaddr(m, tmpreg, baseptr, idxreg) do { \
	uintptr_t xb_addr_ = xe_opaque_uptr((const void *)(baseptr)); \
	if ((intptr_t)xb_addr_ == (intptr_t)(int32_t)xb_addr_) { \
		E_MEM(m, (idxreg), E_NOREG, 0, (intptr_t)xb_addr_); \
	} else { \
		do { XE_OPEN(); \
			/* Materialize the base. LEA rip-rel when the target is \
			 * reachable from the emission point (in-image statics from \
			 * the dispatcher buffers); mov r64,imm64 when it is not \
			 * (block code in a cache mapped far from the image). Both \
			 * carry the full 64-bit address. */ \
			intptr_t xrel_ = (intptr_t)xb_addr_ - ((intptr_t)xep + 7); \
			if (xrel_ == (intptr_t)(int32_t)xrel_) { \
				struct e_mem xb_; XE_MEM_ABS(xb_, (const void *)xb_addr_); \
				E_LEA(xep, 1, 0, (tmpreg), xb_); \
			} else { \
				E_MOV_RI(xep, 1, 0, (tmpreg), (intptr_t)xb_addr_); \
			} XE_CLOSE(); } while (0); \
		E_MEM(m, (idxreg), (tmpreg), 1, 0); \
	} } while (0)

#define xe_mov32_rmem(reg, m) do { XE_OPEN();  \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x8b); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_movzx32_mem8(reg, m) do { XE_OPEN();  \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x0f); EW8(xep, 0xb6); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_movzx32_mem16(reg, m) do { XE_OPEN();  \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)


/* ---- dispatcher control flow ----------------------------------------- */

/* jcc/jmp to a known target: 8-bit when it reaches, else 32 -- E_JCC_TO
 * is the reference's selection. */
#define xe_jcc_to(cc, target) do { XE_OPEN(); E_JCC_TO(xep, (cc), (target)); XE_CLOSE(); } while (0)
#define xe_jmp_to(target) do { XE_OPEN(); E_JCC_TO(xep, E_CC_UNC, (target)); XE_CLOSE(); } while (0)

/* wide jump with a patchable displacement: emits E9/0F8x rel32 with the
 * given displacement and stores the address of the 32-bit slot -- the
 * xJcc32 contract, as an out-parameter since macros do not return. */
#define xe_jcc32_slot(cc, disp, slot) do { XE_OPEN();  \
	{ if ((cc) == Jcc_Unconditional) { EW8(xep, 0xe9); } \
	  else { EW8(xep, 0x0f); EW8(xep, (uint8_t)(0x80 | (cc))); } \
	  (slot) = (s32*)xep; EW32(xep, (uint32_t)(int32_t)(disp)); }; XE_CLOSE(); } while (0)

/* indirect jmp through a base+index memory operand (the dispatcher's LUT
 * double-indirection): FF /4, never REX.W (jmp.cpp:41-46). */
#define xe_jmp_mem(m) do { XE_OPEN();  \
	E_REX_MEM(xep, 0, 0, (m)); EW8(xep, 0xff); \
	E_MODRM_MEM(xep, 4, (m), 0); XE_CLOSE(); } while (0)

/* mov r64 <- base+index+scale+disp (the recLUT load) */
#define xe_mov64_rmem(reg, m) do { XE_OPEN();  \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x8b); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)

/* complex address with a scaled index register: offset is idx*scale. */
#define xe_complexaddr_si(m, tmpreg, baseptr, idxreg, sc) do { \
	uintptr_t xb_addr_ = xe_opaque_uptr((const void *)(baseptr)); \
	if ((intptr_t)xb_addr_ == (intptr_t)(int32_t)xb_addr_) { \
		E_MEM(m, E_NOREG, (idxreg), (sc), (intptr_t)xb_addr_); \
	} else { \
		do { XE_OPEN(); \
			/* Materialize the base. LEA rip-rel when the target is \
			 * reachable from the emission point (in-image statics from \
			 * the dispatcher buffers); mov r64,imm64 when it is not \
			 * (block code in a cache mapped far from the image). Both \
			 * carry the full 64-bit address. */ \
			intptr_t xrel_ = (intptr_t)xb_addr_ - ((intptr_t)xep + 7); \
			if (xrel_ == (intptr_t)(int32_t)xrel_) { \
				struct e_mem xb_; XE_MEM_ABS(xb_, (const void *)xb_addr_); \
				E_LEA(xep, 1, 0, (tmpreg), xb_); \
			} else { \
				E_MOV_RI(xep, 1, 0, (tmpreg), (intptr_t)xb_addr_); \
			} XE_CLOSE(); } while (0); \
		E_MEM(m, (tmpreg), (idxreg), (sc), 0); \
	} } while (0)

/* fastcall compositions actually used by the dispatchers: (imm, imm) and
 * (mem32) argument shapes -- transcribed from the reference overloads,
 * which stage arg1/arg2 then call near. */
#define xe_fastcall2_ii(fn, a1, a2) do { \
	xe_mov32_ri(XE_ARG1, (a1)); \
	xe_mov32_ri(XE_ARG2, (a2)); \
	xe_fastcall0(fn); } while (0)
#define xe_fastcall1_m32(fn, addr) do { \
	xe_mov32_rm(XE_ARG1, (addr)); \
	xe_fastcall0(fn); } while (0)

#define xe_ret() do { XE_OPEN(); EW8(xep, 0xc3); XE_CLOSE(); } while (0)  /* xRET is a macro */
#define xe_test8_rr(a, b) do { XE_OPEN(); E_TEST_RR_SZ(xep, 1, (a), (b)); XE_CLOSE(); } while (0)


#define xe_cmp64_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 1, 7, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_test8_ri(reg, imm)  do { XE_OPEN(); E_TEST_RI_SZ(xep, 1, (reg), (imm)); XE_CLOSE(); } while (0)
/* movmskps r32, xmm: 0F 50 /r, gpr in the reg field */
#define xe_movmskps_rx(gpr, xmm) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x50, (gpr), (xmm)); XE_CLOSE(); } while (0)


/* movd r32 <- xmm: 66 0F 7E, no REX.W (the 64-bit spelling is movq) */
#define xe_movd_rx(gpr, xmm)   do { XE_OPEN(); E_SSE_RR_W(xep, 0x66, 0x7e, (xmm), (gpr), 0); XE_CLOSE(); } while (0)


#define xe_and64_rr(dst, src)  do { XE_OPEN(); E_G1_RR(xep, 1, 4, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_or64_rr(dst, src)   do { XE_OPEN(); E_G1_RR(xep, 1, 1, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_g2op64_ri(g2op, reg, imm) do { XE_OPEN(); E_G2_RI(xep, 1, (g2op), (reg), (imm)); XE_CLOSE(); } while (0)
#define xe_or32_rr_(a, b) xe_or32_rr(a, b)


/* movss/blend forms for the SA register paths */
#define xe_movss_xm(xmm, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0xf3, 0x10, (xmm), xm_, 0); }; XE_CLOSE(); } while (0)
#define xe_movss_mx(addr, xmm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0xf3, 0x11, (xmm), xm_, 0); }; XE_CLOSE(); } while (0)
#define xe_blendpd_xxi(dx, sx, imm) do { XE_OPEN();  \
	E_SSE_RRI_W(xep, 0x66, 0x0d3a, (dx), (sx), (imm), 0); XE_CLOSE(); } while (0)
#define xe_and32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 4, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)


#define xe_sub64_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 1, 5, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
#define xe_add64_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 1, 0, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)
/* add qword [abs], imm: E_G1_MEM_I already speaks sz=8 (REX.W). The
 * earlier cmp64 composition hand-rolled this before reading far enough
 * into the macro; this one does not repeat that. */
#define xe_add64_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 8, 0, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
/* add word [abs], imm: 0x66 prefix, then the s8-narrowed body with a
 * 16-bit wide immediate. Byte behavior from E_G1_MEM_I sz=2. */
#define xe_add16_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 2, 0, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
/* lea r64, [base + index*scale + disp] from an e_mem */
#define xe_lea64_mem(reg, m)   do { XE_OPEN(); E_LEA(xep, 1, 0, (reg), (m)); XE_CLOSE(); } while (0)
#define xe_fastcall1_i(fn, a1) do { \
	xe_mov32_ri(XE_ARG1, (a1)); \
	xe_fastcall0(fn); } while (0)


/* iCOP0 vocabulary: base+disp32 memory forms (the TLB entry walks), the
 * three-operand IMUL, movsxd from absolute memory, the far-address LEA
 * composition, add-mem-imm, and the known-target jcc with the
 * reference's rel8/rel32 choice. */
#define XE_MEM_BD(m, base, disp) E_MEM(m, (base), E_NOREG, 0, (intptr_t)(disp))
#define xe_mov32_rbd(reg, base, disp) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_BD(xm_, (base), (disp)); \
	  E_MOV_R_MEM(xep, 0, (reg), xm_); }; XE_CLOSE(); } while (0)
#define xe_and32_rbd(reg, base, disp) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_BD(xm_, (base), (disp)); \
	  E_G1_R_MEM_SZ(xep, 4, 4, (reg), xm_); }; XE_CLOSE(); } while (0)
#define xe_cmp32_rbd(reg, base, disp) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_BD(xm_, (base), (disp)); \
	  E_G1_R_MEM_SZ(xep, 4, 7, (reg), xm_); }; XE_CLOSE(); } while (0)
#define xe_imul32_rri(dst, src, imm) do { XE_OPEN();  \
	E_IMUL_RRI(xep, 0, (dst), (src), (imm)); XE_CLOSE(); } while (0)
#define xe_movsxd_rm(reg, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 1, (reg), xm_); EW8(xep, 0x63); \
	  E_MODRM_MEM(xep, (reg), xm_, 0); }; XE_CLOSE(); } while (0)
/* xLoadFarAddr, byte for byte: rip-relative LEA when the displacement
 * from the END of the 7-byte LEA fits s32, else MOV64. The predicate is
 * replicated against the c89 cursor so both arms take the same branch. */
#define xe_lea_far(reg, addr) do { XE_OPEN();  \
	{ intptr_t xdisp_ = (intptr_t)(addr) - ((intptr_t)xep + 7); \
	  if (xdisp_ == (intptr_t)(int32_t)xdisp_) { \
		struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
		E_LEA(xep, 1, 0, (reg), xm_); \
	  } else { \
		E_MOV64_RI(xep, 0, (reg), (intptr_t)(addr)); \
	  } }; XE_CLOSE(); } while (0)
#define xe_add32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 0, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)
/* add qword [abs], r64 */
#define xe_add64_mr(addr, reg) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 1, (reg), xm_); EW8(xep, 0x01); \
	  E_MODRM_MEM(xep, (reg), xm_, 0); }; XE_CLOSE(); } while (0)
/* jcc to a KNOWN backward/forward target, choosing rel8 when it fits
 * exactly as xJccKnownTarget(cc, target, false) does. */
#define xe_jcc_known(cc, target) do { XE_OPEN();  \
	{ intptr_t xd8_ = (intptr_t)(target) - ((intptr_t)xep + 2); \
	  if (xd8_ == (intptr_t)(int8_t)xd8_) { \
		EW8(xep, (uint8_t)(0x70 | (cc))); EW8(xep, (uint8_t)xd8_); \
	  } else { \
		intptr_t xd32_ = (intptr_t)(target) - ((intptr_t)xep + 6); \
		EW8(xep, 0x0f); EW8(xep, (uint8_t)(0x80 | (cc))); \
		EW32(xep, (uint32_t)(int32_t)xd32_); \
	  } }; XE_CLOSE(); } while (0)


/* ================= iMMI SSE vocabulary =================
 * Generated from the shim's own opcode tables (instructions.h
 * initializers and struct layouts) -- transcribed, not re-derived;
 * every one of these was byte-checked on execution during the migration. */
#define xe_blendps_xxi(d, s, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0xc3a, (d), (s), (i)); XE_CLOSE(); } while (0)
#define xe_movdqa_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x6f, (d), (s)); XE_CLOSE(); } while (0)
#define xe_movhlps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x12, (d), (s)); XE_CLOSE(); } while (0)
#define xe_movlhps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x16, (d), (s)); XE_CLOSE(); } while (0)
#define xe_movqzx_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x7e, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pabsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x1e38, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pabsw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x1d38, (d), (s)); XE_CLOSE(); } while (0)
#define xe_packssdw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x6b, (d), (s)); XE_CLOSE(); } while (0)
#define xe_packuswb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x67, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xfc, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xfe, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd4, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddsb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xec, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddsw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xed, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddusb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xdc, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddusw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xdd, (d), (s)); XE_CLOSE(); } while (0)
#define xe_paddw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xfd, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pand_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xdb, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pandn_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xdf, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pcmpeqb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x74, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pcmpeqd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x76, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pcmpeqw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x75, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pcmpgtb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x64, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pcmpgtd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x66, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pcmpgtw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x65, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmaddwd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf5, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmaxsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x3d38, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmaxsw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xee, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pminsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x3938, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pminsw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xea, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmovsxdq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x2538, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmuldq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x2838, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmulhw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xe5, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmullw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd5, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pmuludq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf4, (d), (s)); XE_CLOSE(); } while (0)
#define xe_por_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xeb, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pshufd_xxi(d, s, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x70, (d), (s), (i)); XE_CLOSE(); } while (0)
#define xe_pshufhw_xxi(d, s, i) do { XE_OPEN(); E_SSE_RRI(xep, 0xf3, 0x70, (d), (s), (i)); XE_CLOSE(); } while (0)
#define xe_pshuflw_xxi(d, s, i) do { XE_OPEN(); E_SSE_RRI(xep, 0xf2, 0x70, (d), (s), (i)); XE_CLOSE(); } while (0)
#define xe_psubb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf8, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xfa, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xfb, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubsb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xe8, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubsw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xe9, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubusb_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd8, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubusw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd9, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf9, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpckhbw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x68, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpckhdq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x6a, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpckhqdq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x6d, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpckhwd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x69, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpcklbw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x60, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpckldq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x62, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpcklqdq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x6c, (d), (s)); XE_CLOSE(); } while (0)
#define xe_punpcklwd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x61, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pxor_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xef, (d), (s)); XE_CLOSE(); } while (0)
#define xe_shufps_xxi(d, s, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x00, 0xc6, (d), (s), (i)); XE_CLOSE(); } while (0)
#define xe_psllw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf1, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psllw_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x71, 6, (d), (i)); XE_CLOSE(); } while (0)
#define xe_pslld_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf2, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pslld_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x72, 6, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psllq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xf3, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psllq_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x73, 6, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psrlw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd1, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psrlw_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x71, 2, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psrld_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd2, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psrld_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x72, 2, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psrlq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd3, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psrlq_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x73, 2, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psraw_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xe1, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psraw_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x71, 4, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psrad_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xe2, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psrad_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x72, 4, (d), (i)); XE_CLOSE(); } while (0)
#define xe_pslldq_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x73, 7, (d), (i)); XE_CLOSE(); } while (0)
#define xe_psrldq_xi(d, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x73, 3, (d), (i)); XE_CLOSE(); } while (0)


/* iMMI scalar stragglers */
#define xe_bsr32_rr(d, s)      do { XE_OPEN();  \
	{ EW8(xep, 0x0f); EW8(xep, 0xbd); E_MODRM_RR(xep, (d), (s)); }; XE_CLOSE(); } while (0)
#define xe_dec32_r(reg)        do { XE_OPEN(); E_INCDEC_R_SZ(xep, 4, 1, (reg)); XE_CLOSE(); } while (0)
#define xe_udiv32_r(reg)       do { XE_OPEN(); E_G3_R(xep, 0, 6, (reg)); XE_CLOSE(); } while (0)
/* pextrd gpr, xmm, imm: 66 0F 3A 16, xmm in reg field */
#define xe_pextrd_rxi(gpr, xmm, imm) do { XE_OPEN();  \
	E_SSE_RRI_W(xep, 0x66, 0x163a, (xmm), (gpr), (imm), 0); XE_CLOSE(); } while (0)
/* SSE ops against absolute memory the transform left behind */
#define xe_pand_xm(xmm, addr)  do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM(xep, 0x66, 0xdb, (xmm), xm_); }; XE_CLOSE(); } while (0)
/* movdqa/movdqu through a register-based address (the QFSRV shifter) */
#define xe_movdqu_xmem(xmm, m) do { XE_OPEN();  \
	E_SSE_R_MEM(xep, 0xf3, 0x6f, (xmm), (m)); XE_CLOSE(); } while (0)
#define xe_movdqa_memx(m, xmm) do { XE_OPEN();  \
	E_SSE_R_MEM(xep, 0x66, 0x7f, (xmm), (m)); XE_CLOSE(); } while (0)
/* lea r64, [abs] (fits-s32 asserted by context: tempqw/GPR are in the
 * low image on linux; the reference emits the rip-relative form and so
 * do we via the absolute e_mem path) */
#define xe_lea64_m(reg, addr)  do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_LEA(xep, 1, 0, (reg), xm_); }; XE_CLOSE(); } while (0)
#define xe_movsxd_rm16(reg, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_P16(xep); E_REX_MEM(xep, 0, (reg), xm_); \
	  EW8(xep, 0x0f); EW8(xep, 0xbf); E_MODRM_MEM(xep, (reg), xm_, 0); }; XE_CLOSE(); } while (0)

/* ================= iFPU vocabulary ================= */
#define xe_addsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x58, (d), (s)); XE_CLOSE(); } while (0)
#define xe_addss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x58, (d), (s)); XE_CLOSE(); } while (0)
#define xe_andpd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x54, (d), (s)); XE_CLOSE(); } while (0)
#define xe_andps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x54, (d), (s)); XE_CLOSE(); } while (0)
#define xe_cvtdq2ps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x5b, (d), (s)); XE_CLOSE(); } while (0)
#define xe_cvtsd2ss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x5a, (d), (s)); XE_CLOSE(); } while (0)
#define xe_cvtss2sd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x5a, (d), (s)); XE_CLOSE(); } while (0)
#define xe_divsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x5e, (d), (s)); XE_CLOSE(); } while (0)
#define xe_divss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x5e, (d), (s)); XE_CLOSE(); } while (0)
#define xe_maxsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x5f, (d), (s)); XE_CLOSE(); } while (0)
#define xe_maxss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x5f, (d), (s)); XE_CLOSE(); } while (0)
#define xe_minsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x5d, (d), (s)); XE_CLOSE(); } while (0)
#define xe_minss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x5d, (d), (s)); XE_CLOSE(); } while (0)
#define xe_movaps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x28, (d), (s)); XE_CLOSE(); } while (0)
#define xe_mulsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x59, (d), (s)); XE_CLOSE(); } while (0)
#define xe_mulss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x59, (d), (s)); XE_CLOSE(); } while (0)
#define xe_orps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x56, (d), (s)); XE_CLOSE(); } while (0)
#define xe_pminud_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x3b38, (d), (s)); XE_CLOSE(); } while (0)
#define xe_psubq_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xfb, (d), (s)); XE_CLOSE(); } while (0)
#define xe_sqrtsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x51, (d), (s)); XE_CLOSE(); } while (0)
#define xe_sqrtss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x51, (d), (s)); XE_CLOSE(); } while (0)
#define xe_subsd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf2, 0x5c, (d), (s)); XE_CLOSE(); } while (0)
#define xe_subss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x5c, (d), (s)); XE_CLOSE(); } while (0)
#define xe_ucomisd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x2e, (d), (s)); XE_CLOSE(); } while (0)
#define xe_ucomiss_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x2e, (d), (s)); XE_CLOSE(); } while (0)
#define xe_xorpd_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x57, (d), (s)); XE_CLOSE(); } while (0)
#define xe_xorps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x57, (d), (s)); XE_CLOSE(); } while (0)
/* movss dst,src elides self-moves in the reference (shim_xMOVSS); the
 * c89 arm replicates the elision so both arms stay in step. */
#define xe_movss_xx(d, s) do { XE_OPEN();  \
	{ if ((d) != (s)) E_SSE_RR(xep, 0xf3, 0x10, (d), (s)); }; XE_CLOSE(); } while (0)
/* cmpeq.ss = CMPSS with imm 0 appended */
#define xe_cmpeqss_xx(d, s) do { XE_OPEN(); E_SSE_RRI(xep, 0xf3, 0xc2, (d), (s), 0); XE_CLOSE(); } while (0)
#define xe_movsszx_xm(xmm, addr) xe_movss_xm(xmm, addr)
#define xe_movdzx_xr(xmm, gpr) do { XE_OPEN(); E_SSE_RR_W(xep, 0x66, 0x6e, (xmm), (gpr), 0); XE_CLOSE(); } while (0)
#define xe_movd_mx(addr, xmm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM_W(xep, 0x66, 0x7e, (xmm), xm_, 0); }; XE_CLOSE(); } while (0)
#define xe_movaps_xm(xmm, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM(xep, 0x00, 0x28, (xmm), xm_); }; XE_CLOSE(); } while (0)
#define xe_ldmxcsr_m(addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 0, 2, xm_); EW8(xep, 0x0f); EW8(xep, 0xae); \
	  E_MODRM_MEM(xep, 2, xm_, 0); }; XE_CLOSE(); } while (0)
#define xe_ucomiss_xm(xmm, addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_SSE_R_MEM(xep, 0x00, 0x2e, (xmm), xm_); }; XE_CLOSE(); } while (0)


/* iFPU memory-operand SSE forms and the CVT family */
#define XE_SSE_XM(name, pre, opc, cxxexpr) /* doc helper only */
#define xe_andps_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x54, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_orps_xm(x, addr)   do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x56, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_xorps_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x57, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_por_xm(x, addr)    do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xeb, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_pxor_xm(x, addr)    do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xef, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_minss_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x5d, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_maxss_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x5f, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_ucomisd_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x2e, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_movd_xr(xmm, gpr)  xe_movdzx_xr(xmm, gpr)
#define xe_movd_rx32(gpr, xmm) xe_movd_rx(gpr, xmm)
#define xe_cvttss2si_rx(gpr, xmm) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x2c, (gpr), (xmm)); XE_CLOSE(); } while (0)
#define xe_cvtsi2ss_xr(xmm, gpr) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x2a, (xmm), (gpr)); XE_CLOSE(); } while (0)
#define xe_insertps_xxi(d, s, i) do { XE_OPEN(); E_SSE_RRI_W(xep, 0x66, 0x213a, (d), (s), (i), 0); XE_CLOSE(); } while (0)

/* last iFPU/iFPUd gaps: packed-integer mem forms over the double-domain
 * constants, andpd, cvt with memory sources, cvttss2si from memory,
 * test-mem-imm, and the 32-bit register cmov. */
#define xe_pminsd_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x3938, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_pminud_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x3b38, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_psubd_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xfa, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_paddd_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xfe, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_psubq_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xfb, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_paddq_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0xd4, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_andpd_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x54, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_cvtsi2ss_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x2a, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_cvttss2si_rm(gpr, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x2c, (gpr), xm_); }; XE_CLOSE(); } while (0)
#define xe_test32_mi(addr, imm) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 0, 0, xm_); EW8(xep, 0xf7); \
	E_MODRM_MEM(xep, 0, xm_, 4); EW32(xep, (uint32_t)(imm)); }; XE_CLOSE(); } while (0)
#define xe_cmovge32_rr(dst, src) do { XE_OPEN();  \
	{ E_REX(xep, 0, (dst), 0, (src)); \
	  EW8(xep, 0x0f); EW8(xep, (uint8_t)(0x40 | Jcc_GreaterOrEqual)); \
	  E_MODRM_RR(xep, (dst), (src)); }; XE_CLOSE(); } while (0)

/* or dword [abs], imm (the FPU flag-set sites) */
#define xe_or32_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 4, 1, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)

/* microVU_Macro vocabulary: 128-bit moves to/from absolute memory, the
 * 16-bit VI-register traffic, zero-extension in both source shapes, and
 * and64 with an immediate. */
#define xe_movaps_mx(addr, xmm) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x29, (xmm), xm_); }; XE_CLOSE(); } while (0)
#define xe_movzx32_rr16(dst, src) do { XE_OPEN();  \
	{ E_REX(xep, 0, (dst), 0, (src)); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	  E_MODRM_RR(xep, (dst), (src)); }; XE_CLOSE(); } while (0)
#define xe_movzx32_rm16(dst, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 0, (dst), xm_); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	E_MODRM_MEM(xep, (dst), xm_, 0); }; XE_CLOSE(); } while (0)
#define xe_mov16_mr(addr, reg) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_P16(xep); E_REX_MEM(xep, 0, (reg), xm_); EW8(xep, 0x89); \
	E_MODRM_MEM(xep, (reg), xm_, 0); }; XE_CLOSE(); } while (0)
#define xe_mov16_mi(addr, imm) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_P16(xep); E_REX_MEM(xep, 0, 0, xm_); EW8(xep, 0xc7); \
	E_MODRM_MEM(xep, 0, xm_, 2); \
	EW8(xep, (uint8_t)((imm) & 0xff)); EW8(xep, (uint8_t)(((imm) >> 8) & 0xff)); }; XE_CLOSE(); } while (0)
#define xe_and64_ri(reg, imm)  do { XE_OPEN(); E_G1_RI(xep, 1, 4, (reg), (int32_t)(imm)); XE_CLOSE(); } while (0)

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
/* Platform ABI argument registers as plain ids (previously read from the
 * x86types arg*reg objects; values are identical). */
#ifdef _WIN32
#define XE_ARG1 1 /* rcx */
#define XE_ARG2 2 /* rdx */
#define XE_ARG3 8 /* r8  */
#define XE_ARG4 9 /* r9  */
#else
#define XE_ARG1 7 /* rdi */
#define XE_ARG2 6 /* rsi */
#define XE_ARG3 2 /* rdx */
#define XE_ARG4 1 /* rcx */
#endif

/* ABI volatility predicates and the SSE argument register, as plain C89.
 * Byte-for-byte the logic of xRegisterBase/xRegisterSSE::IsCallerSaved and
 * xRegisterSSE::GetArgRegister(arg, sse).Id. */
#ifdef _WIN32
#define XE_GPR_CALLER_SAVED(id) ((unsigned)(id) <= 2 || ((unsigned)(id) >= 8 && (unsigned)(id) <= 11))
#define XE_XMM_CALLER_SAVED(id) ((unsigned)(id) < 6)
#define XE_XMM_ARG(argn, ssen)  (argn)
#else
#define XE_GPR_CALLER_SAVED(id) ((unsigned)(id) <= 2 || (id) == 6 || (id) == 7 || ((unsigned)(id) >= 8 && (unsigned)(id) <= 11))
#define XE_XMM_CALLER_SAVED(id) (1)
#define XE_XMM_ARG(argn, ssen)  (ssen)
#endif
#define xe_push64_r(reg) do { XE_OPEN();  \
	{ if ((reg) >= 8) EW8(xep, 0x41); EW8(xep, (uint8_t)(0x50 | ((reg) & 7))); }; XE_CLOSE(); } while (0)
#define xe_pop64_r(reg) do { XE_OPEN();  \
	{ if ((reg) >= 8) EW8(xep, 0x41); EW8(xep, (uint8_t)(0x58 | ((reg) & 7))); }; XE_CLOSE(); } while (0)

/* microVU compile-path vocabulary: packed min/max against memory, the
 * direct near call (xCALL is always rel32 in the reference's
 * void* overload), and the two-operand pmin forms over indexed tables. */
#define xe_minps_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x5d, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_maxps_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x5f, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_call_ptr(fn) do { XE_OPEN(); E_CALL_REL(xep, (const void*)(fn)); XE_CLOSE(); } while (0)

/* mov qword [abs], imm32 (sign-extended REX.W C7 /0) */
#define xe_mov64_mi_s32(addr, imm) do { XE_OPEN();  \
	E_MOV_M_I64(xep, (uintptr_t)(addr), (int32_t)(imm)); XE_CLOSE(); } while (0)

/* Bridge from the C++ xAddressVoid (base+index*factor+disp address
 * object, factor form) into e_mem, with an extra displacement for the
 * lane offsets mVUsaveReg/mVUloadReg add at each site. E_MEM performs
 * the factor->SIB reduction, same as the xIndirect constructors. */

/* generic-e_mem SSE moves for address-object and rsp-relative operands */
#define xe_movss_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM(xep, 0xf3, 0x10, (x), (m)); XE_CLOSE(); } while (0)
#define xe_movss_memxg(m, x) do { XE_OPEN(); E_SSE_R_MEM(xep, 0xf3, 0x11, (x), (m)); XE_CLOSE(); } while (0)
#define xe_movaps_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x00, 0x28, (x), (m)); XE_CLOSE(); } while (0)
#define xe_movaps_memxg(m, x) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x00, 0x29, (x), (m)); XE_CLOSE(); } while (0)
#define xe_movlps_memxg(m, x) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x00, 0x13, (x), (m)); XE_CLOSE(); } while (0)
#define xe_movhps_memxg(m, x) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x00, 0x17, (x), (m)); XE_CLOSE(); } while (0)
/* extractps [mem], xmm, imm: 66 0F 3A 17, xmm in the reg field */
#define xe_extractps_memxi(m, x, i) do { XE_OPEN();  \
	{ EW8(xep, 0x66); E_REX_MEM(xep, 0, (x), (m)); \
	  EW8(xep, 0x0f); EW8(xep, 0x3a); EW8(xep, 0x17); \
	  E_MODRM_MEM(xep, (x), (m), 1); EW8(xep, (uint8_t)(i)); }; XE_CLOSE(); } while (0)
#define xe_minpd_xx(d, s2) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x5d, (d), (s2)); XE_CLOSE(); } while (0)
#define xe_maxpd_xx(d, s2) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x5f, (d), (s2)); XE_CLOSE(); } while (0)

/* microVU_Upper vocabulary: packed compares (CMPPS with the type
 * immediate, 0=EQ 5=NLT per SSE2_ComparisonType), packed multiply and
 * gt-compare against memory, the float<->int packed converts, the clip
 * pack/blend/movmsk trio, and movmskps into any gpr. */
#define xe_cmpeqps_xx(d, s2)  do { XE_OPEN(); E_SSE_RRI(xep, 0x00, 0xc2, (d), (s2), 0); XE_CLOSE(); } while (0)
#define xe_cmpnltps_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0xc2, (x), xm_); EW8(xep, 5); }; XE_CLOSE(); } while (0)
#define xe_mulps_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x00, 0x59, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_pshufb_xx(d, s2) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x0038, (d), (s2)); XE_CLOSE(); } while (0)
#define xe_pshufb_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x0038, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_pcmpgtd_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x66, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_pcmpeqd_xm(x, addr) do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x76, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_cvttps2dq_xx(d, s2) do { XE_OPEN(); E_SSE_RR(xep, 0xf3, 0x5b, (d), (s2)); XE_CLOSE(); } while (0)
#define xe_pblendw_xxi(d, s2, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x0e3a, (d), (s2), (i)); XE_CLOSE(); } while (0)
#define xe_packsswb_xx(d, s2) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x63, (d), (s2)); XE_CLOSE(); } while (0)
#define xe_pmovmskb_rx(gpr, x) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0xd7, (gpr), (x)); XE_CLOSE(); } while (0)

/* microVU_Execute vocabulary: the state copy/compare thunks. AVX forms
 * ride the existing E_VEX_* core exactly as the shim's AVX objects do;
 * the twins ARE those objects, so both arms share one opcode table. */
#define xe_vmovaps_xmemg(x, m, L) do { XE_OPEN(); E_VEX_RRMEM(xep, 0x00, 0x28, (x), E_NOREG, (m), (L)); XE_CLOSE(); } while (0)
#define xe_vmovups_xmemg(x, m, L) do { XE_OPEN(); E_VEX_RRMEM(xep, 0x00, 0x10, (x), E_NOREG, (m), (L)); XE_CLOSE(); } while (0)
#define xe_vmovups_memxg(m, x, L) do { XE_OPEN(); E_VEX_RRMEM(xep, 0x00, 0x11, (x), E_NOREG, (m), (L)); XE_CLOSE(); } while (0)
#define xe_vpcmpeqd_xxmemg(d, s1, m, L) do { XE_OPEN(); E_VEX_RRMEM(xep, 0x66, 0x76, (d), (s1), (m), (L)); XE_CLOSE(); } while (0)
#define xe_vpand_xxx(d, s1, s2, L) do { XE_OPEN(); E_VEX_RRR(xep, 0x66, 0xdb, (d), (s1), (s2), (L)); XE_CLOSE(); } while (0)
#define xe_vpmovmskb_rx(gpr, x, L) do { XE_OPEN();  \
	{ E_VEX(xep, 0x66, 0xd7, (gpr), E_NOREG, (L), 0, ((((x) & 0x0F) > 7) ? 1 : 0)); \
	  E_MODRM_RR(xep, (gpr), (x)); }; XE_CLOSE(); } while (0)
#define xe_vzeroupper() do { XE_OPEN();  \
	{ EW8(xep, 0xc5); EW8(xep, 0xf8); EW8(xep, 0x77); }; XE_CLOSE(); } while (0)
/* SSE fallback path pieces */
#define xe_pcmpeqd_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x66, 0x76, (x), (m)); XE_CLOSE(); } while (0)
#define xe_movups_memxg(m, x) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x00, 0x11, (x), (m)); XE_CLOSE(); } while (0)
#define xe_jmp_r(reg) do { XE_OPEN();  \
	{ E_REX(xep, 0, 0, 0, (reg)); EW8(xep, 0xff); \
	  EW8(xep, (uint8_t)(0xe0 | ((reg) & 7))); }; XE_CLOSE(); } while (0)

#define xe_movdzx_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM_W(xep, 0x66, 0x6e, (x), (m), 0); XE_CLOSE(); } while (0)
#define xe_jmp_mem_abs(addr) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_REX_MEM(xep, 0, 4, xm_); EW8(xep, 0xff); \
	  E_MODRM_MEM(xep, 4, xm_, 0); }; XE_CLOSE(); } while (0)


/* [rsp + disp]: the reference's operand translation for rsp-based
 * addresses has quirks (Reduce leaves rsp in both slots and the scale
 * field's journey is nontrivial); rather than re-deriving them, sites
 * build the e_mem by running shim_mem over the SAME ptr128[rsp + off]
 * expression the twin uses, inheriting the reference translation by
 * construction. The shadow oracle caught two successive re-derivations
 * (SIB 24 vs 64, then 64 vs a4) before this lesson stuck. */

/* cmp word [abs], imm8/imm16 -- 66-prefixed group1 with the s8 narrowing */
#define xe_cmp16_mi(addr, imm) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_I(xep, 2, 7, xm_, (int32_t)(imm)); }; XE_CLOSE(); } while (0)

/* 32-bit forward jump pair, mirroring xForwardJump32: jcc rel32 (or e9
 * for unconditional) with a zero placeholder, patched by set32. Both
 * shadow arms patch the same dword from the same anchor. */
#define xe_fwd_jcc32(cc, slot) do { XE_OPEN();  \
	{ if ((cc) == Jcc_Unconditional) { EW8(xep, 0xe9); } \
	  else { EW8(xep, 0x0f); EW8(xep, (uint8_t)(0x80 | (cc))); } \
	  (slot) = xep; EW32(xep, 0); }; XE_CLOSE(); } while (0)
#define xe_fwd_set32(slot) do { XE_OPEN();  \
	{ *(int32_t*)(slot) = (int32_t)(xep - ((slot) + 4)); }; XE_CLOSE(); } while (0)

/* Pad to a 16-byte boundary with NOPs, then patch. The legacy x86SetJ32A
 * did this so a branch target starts a fresh cache line; the padding is
 * part of the emitted stream, so it has to happen before the displacement
 * is computed. */
#define xe_fwd_set32_aligned(slot) do { XE_OPEN(); \
	while (((uintptr_t)xep) & 0xf) EW8(xep, 0x90); \
	{ *(int32_t*)(slot) = (int32_t)(xep - ((slot) + 4)); }; XE_CLOSE(); } while (0)

/* microVU_Lower vocabulary */
#define xe_mulss_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x59, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_addss_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x58, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_subss_xm(x, addr)  do { XE_OPEN(); { struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0xf3, 0x5c, (x), xm_); }; XE_CLOSE(); } while (0)
#define xe_ptest_xx(a, b) do { XE_OPEN(); E_SSE_RR(xep, 0x66, 0x1738, (a), (b)); XE_CLOSE(); } while (0)
#define xe_dpps_xxi(d, s2, i) do { XE_OPEN(); E_SSE_RRI(xep, 0x66, 0x403a, (d), (s2), (i)); XE_CLOSE(); } while (0)
#define xe_inc32_r(reg) do { XE_OPEN(); E_INCDEC_R_SZ(xep, 4, 0, (reg)); XE_CLOSE(); } while (0)
#define xe_movsx32_rr16(dst, src) do { XE_OPEN();  \
	{ E_REX(xep, 0, (dst), 0, (src)); EW8(xep, 0x0f); EW8(xep, 0xbf); \
	  E_MODRM_RR(xep, (dst), (src)); }; XE_CLOSE(); } while (0)

#define xe_cmp8_ri(reg, imm) do { XE_OPEN();  \
	{ E_REX8_RM(xep, (reg)); \
	  if ((reg) == 0) { EW8(xep, 0x3c); } \
	  else { EW8(xep, 0x80); E_MODRM_RR(xep, 7, (reg)); } \
	  EW8(xep, (uint8_t)(imm)); }; XE_CLOSE(); } while (0)
#define xe_xor32_mr(addr, reg) do { XE_OPEN();  \
	{ struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	  E_G1_MEM_R_SZ(xep, 4, 6, (reg), xm_); }; XE_CLOSE(); } while (0)
#define xe_movzx32_rmemg16(dst, m) do { XE_OPEN();  \
	{ E_REX_MEM(xep, 0, (dst), (m)); EW8(xep, 0x0f); EW8(xep, 0xb7); \
	  E_MODRM_MEM(xep, (dst), (m), 0); }; XE_CLOSE(); } while (0)


/* newVif vocabulary: the PMOVSX/PMOVZX byte/word-to-dword widening
 * loads from a generic e_mem (0F38-escape opcodes spelled high-byte
 * first, as everywhere in this core), and the unaligned load twin. */
#define xe_pmovsxbd_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x66, 0x2138, (x), (m)); XE_CLOSE(); } while (0)
#define xe_pmovzxbd_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x66, 0x3138, (x), (m)); XE_CLOSE(); } while (0)
/* The reference emits REX.W on the WD widening loads -- the ptr64
 * operand size flows into the W bit by the param3 rule, meaningless to
 * the CPU for pmovsx/zx but part of the byte contract (caught by the
 * shadow oracle: 66 48 0f 38 23 vs 66 0f 38 23). */
#define xe_pmovsxwd_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM_W(xep, 0x66, 0x2338, (x), (m), 1); XE_CLOSE(); } while (0)
#define xe_pmovzxwd_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM_W(xep, 0x66, 0x3338, (x), (m), 1); XE_CLOSE(); } while (0)
#define xe_movups_xmemg(x, m) do { XE_OPEN(); E_SSE_R_MEM(xep, 0x00, 0x10, (x), (m)); XE_CLOSE(); } while (0)

#define xe_movdqa_xm(x, addr) do { XE_OPEN(); struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x6f, (x), xm_); XE_CLOSE(); } while (0)
#define xe_movdqa_mx(addr, x) do { XE_OPEN(); struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_SSE_R_MEM(xep, 0x66, 0x7f, (x), xm_); XE_CLOSE(); } while (0)


/* mov qword [abs], imm64 -- the xWriteImm64ToMem contract: C7 sign-
 * extends 32, so a wide immediate stages through tmp then stores. */
#define xe_imm64op_mov64_mi(addr, tmpreg, imm) do { \
	if ((int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
		xe_mov64_mi_s32((addr), (int32_t)(imm)); \
	} else { \
		xe_mov64_ri((tmpreg), (imm)); \
		xe_mov64_mr((addr), (tmpreg)); \
	} } while (0)


/* recVTLB vocabulary: sign/zero-extending loads at 64-bit width from
 * generic e_mem and absolute addresses, generic 64-bit moves, byte
 * register extends, an indirect near call through a generic memory
 * operand, and nop. */
#define xe_movsx64_rmemg8(reg, m) do { XE_OPEN(); \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x0f); EW8(xep, 0xbe); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_movsx64_rmemg16(reg, m) do { XE_OPEN(); \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x0f); EW8(xep, 0xbf); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_movsxd_rmemg(reg, m) do { XE_OPEN(); \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x63); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_mov64_rmemg(reg, m) do { XE_OPEN(); \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x8b); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_mov64_memgr(m, reg) do { XE_OPEN(); \
	E_REX_MEM(xep, 1, (reg), (m)); EW8(xep, 0x89); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_mov32_memgr(m, reg) do { XE_OPEN(); \
	E_REX_MEM(xep, 0, (reg), (m)); EW8(xep, 0x89); \
	E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_movsx64_rm8(reg, addr) do { XE_OPEN(); struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 1, (reg), xm_); EW8(xep, 0x0f); EW8(xep, 0xbe); \
	E_MODRM_MEM(xep, (reg), xm_, 0); XE_CLOSE(); } while (0)
#define xe_movsx64_rm16(reg, addr) do { XE_OPEN(); struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 1, (reg), xm_); EW8(xep, 0x0f); EW8(xep, 0xbf); \
	E_MODRM_MEM(xep, (reg), xm_, 0); XE_CLOSE(); } while (0)
#define xe_movzx32_rm8(reg, addr) do { XE_OPEN(); struct e_mem xm_; XE_MEM_ABS(xm_, addr); \
	E_REX_MEM(xep, 0, (reg), xm_); EW8(xep, 0x0f); EW8(xep, 0xb6); \
	E_MODRM_MEM(xep, (reg), xm_, 0); XE_CLOSE(); } while (0)
/* movsx r64, r8 / r16; movzx r32, r8 (register forms) */
#define xe_movsx64_rr8(dst, src) do { XE_OPEN(); \
	{ uint8_t rex_ = (uint8_t)(0x48 | (((dst) > 7) ? 4 : 0) | (((src) > 7) ? 1 : 0)); \
	  EW8(xep, rex_); } EW8(xep, 0x0f); EW8(xep, 0xbe); \
	E_MODRM_RR(xep, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_movsx64_rr16(dst, src) do { XE_OPEN(); \
	{ uint8_t rex_ = (uint8_t)(0x48 | (((dst) > 7) ? 4 : 0) | (((src) > 7) ? 1 : 0)); \
	  EW8(xep, rex_); } EW8(xep, 0x0f); EW8(xep, 0xbf); \
	E_MODRM_RR(xep, (dst), (src)); XE_CLOSE(); } while (0)
#define xe_movzx32_rr8(dst, src) do { XE_OPEN(); \
	{ int nx_ = ((dst) > 7) || ((src) > 3); \
	  if (nx_) EW8(xep, (uint8_t)(0x40 | (((dst) > 7) ? 4 : 0) | (((src) > 7) ? 1 : 0))); } \
	EW8(xep, 0x0f); EW8(xep, 0xb6); \
	E_MODRM_RR(xep, (dst), (src)); XE_CLOSE(); } while (0)
/* call near through a generic memory operand: FF /2 */
#define xe_call_memg(m) do { XE_OPEN(); \
	E_REX_MEM(xep, 0, 2, (m)); EW8(xep, 0xff); \
	E_MODRM_MEM(xep, 2, (m), 0); XE_CLOSE(); } while (0)
#define xe_nop() do { XE_OPEN(); EW8(xep, 0x90); XE_CLOSE(); } while (0)

/* byte/word stores through a generic e_mem (REX for spl..dil bytes) */
#define xe_mov8_memgr(m, reg) do { XE_OPEN(); \
	{ int rx8_ = ((m).index != E_NOREG && (m).index >= 8) ? 1 : 0; \
	  int rb8_ = ((m).base  != E_NOREG && (m).base  >= 8) ? 1 : 0; \
	  uint8_t rex8_; \
	  if (!E_NEEDS_SIB(m)) { rb8_ = rx8_; rx8_ = 0; } \
	  rex8_ = (uint8_t)(0x40 | ((((int)(reg) >= 8) ? 1 : 0) << 2) | (rx8_ << 1) | rb8_); \
	  /* spl..dil (4-7) as byte operands force a bare REX */ \
	  if (rex8_ != 0x40 || ((reg) >= 4 && (reg) <= 7)) EW8(xep, rex8_); } \
	EW8(xep, 0x88); E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)
#define xe_mov16_memgr(m, reg) do { XE_OPEN(); \
	E_P16(xep); E_REX_MEM(xep, 0, (reg), (m)); \
	EW8(xep, 0x89); E_MODRM_MEM(xep, (reg), (m), 0); XE_CLOSE(); } while (0)


/* movq xmm, r64: 66 REX.W 0F 6E /r */
#define xe_movq_xr(x, gpr) do { XE_OPEN(); E_SSE_RR_W(xep, 0x66, 0x6e, (x), (gpr), 1); XE_CLOSE(); } while (0)


#define xe_addps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x58, (d), (s)); XE_CLOSE(); } while (0)
#define xe_subps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x5c, (d), (s)); XE_CLOSE(); } while (0)
#define xe_mulps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x59, (d), (s)); XE_CLOSE(); } while (0)
#define xe_divps_xx(d, s) do { XE_OPEN(); E_SSE_RR(xep, 0x00, 0x5e, (d), (s)); XE_CLOSE(); } while (0)


/* byte store to an absolute address; spl..dil force a bare REX. */
#define xe_mov8_mr(addr, reg) do { XE_OPEN(); struct e_mem xm8_; XE_MEM_ABS(xm8_, addr); \
	{ int rx8_ = (xm8_.index != E_NOREG && xm8_.index >= 8) ? 1 : 0; \
	  int rb8_ = (xm8_.base  != E_NOREG && xm8_.base  >= 8) ? 1 : 0; \
	  uint8_t rex8_; \
	  if (!E_NEEDS_SIB(xm8_)) { rb8_ = rx8_; rx8_ = 0; } \
	  rex8_ = (uint8_t)(0x40 | ((((int)(reg) >= 8) ? 1 : 0) << 2) | (rx8_ << 1) | rb8_); \
	  if (rex8_ != 0x40 || ((reg) >= 4 && (reg) <= 7)) EW8(xep, rex8_); } \
	EW8(xep, 0x88); E_MODRM_MEM(xep, (reg), xm8_, 0); XE_CLOSE(); } while (0)

#endif /* PCSX2_C89OPS_H */
