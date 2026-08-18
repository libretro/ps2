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

#endif /* PCSX2_C89OPS_H */
