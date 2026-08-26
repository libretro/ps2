/* C89 macro emitter prototype.
 *
 * Same encoding decisions the C++ emitter makes at runtime:
 *   - REX.W / REX.R / REX.B computed from operand width and register number
 *   - RIP-relative vs absolute-SIB chosen by displacement reachability
 *   - imm8 vs imm32 shortening for group1 ops
 * The difference is purely structural: every decision is a macro, so it is
 * inlined by construction at the call site and constant-folds whenever the
 * inputs are constants. No cross-TU call, no reliance on always_inline.
 *
 * Cursor discipline: macros take an explicit lvalue cursor `p` and advance it.
 * The caller loads it once and stores it back once, rather than touching a
 * thread_local per byte.
 *
 * C89 style: no // comments, no declarations after statements, no inline
 * keyword. The one C99 dependency is <stdint.h> below.
 */
#ifndef C89EMIT_H
#define C89EMIT_H

/* Fixed-width types come from <stdint.h>. This header used to roll its own
 * (e_u8 through e_uptr) with a hand-written LP64/LLP64 selection for the
 * pointer width, because C89 has no <stdint.h>. Every compiler this emitter
 * is built with has had it since C99, and intptr_t/uintptr_t are the pointer
 * width by definition, so the selection and the negative-array-size assert
 * that guarded it are gone with the typedefs. */
#include <stdint.h>
#include <stdlib.h> /* abort: unencodable-operand hard failure */


/* Fired when a forward jump's 8-bit displacement would not fit. Emitting a
 * wrapped displacement silently corrupts the following instruction (see the
 * FBRST incident); the reference emitter asserted here. Deterministic trap,
 * no libc dependency, pure C89. */
#define E_FWD_OVERFLOW_TRAP() do { \
	volatile int* e_fwd_null_ = (volatile int*)0; *e_fwd_null_ = 0; } while (0)

#define EW8(p, v)  do { *(p) = (uint8_t)(v); (p) += 1; } while (0)
#define EW32(p, v) do { uint32_t v_ = (uint32_t)(v); \
        (p)[0]=(uint8_t)(v_); (p)[1]=(uint8_t)(v_>>8); \
        (p)[2]=(uint8_t)(v_>>16); (p)[3]=(uint8_t)(v_>>24); (p) += 4; } while (0)

#define E_IS_S8(x) ((int32_t)(x) == (int8_t)(x))

/* REX. Emitted only when needed, exactly like EmitRex. */
#define E_REX(p, w, r, x, b) do { \
        uint8_t rex_ = (uint8_t)(0x40 | ((w)?8:0) | (((r)>>3)?4:0) | (((x)>>3)?2:0) | (((b)>>3)?1:0)); \
        if (rex_ != 0x40) EW8((p), rex_); } while (0)

/* ModRM for reg,reg (mod=11). */
#define E_MODRM_RR(p, reg, rm) EW8((p), (uint8_t)(0xc0 | (((reg)&7)<<3) | ((rm)&7)))

/* ModRM for an absolute address operand.
 * Prefers RIP-relative when the displacement fits in s32 (one byte shorter),
 * falls back to mod=00 rm=100 SIB=0x25 disp32. `after` is the number of bytes
 * still to be written after the disp32 (immediates), needed for the RIP delta.
 */
#define E_MODRM_ABS(p, reg, addr, after) do { \
        intptr_t a_ = (intptr_t)(addr); \
        intptr_t rip_ = a_ - ((intptr_t)(p) + 5 + (after)); \
        if (rip_ == (int32_t)rip_) { \
            EW8((p), (uint8_t)(0x05 | (((reg)&7)<<3))); \
            EW32((p), (uint32_t)(int32_t)rip_); \
        } else { \
            /* abs32 is sign-extended: only the low/high 2GB are
             * encodable. A target that fits neither rip-rel nor abs32
             * cannot be expressed in this operand shape -- the caller
             * must materialize the address in a register first
             * (xe_complexaddr*). Truncating here would emit an operand
             * that reads the wrong page once the image maps high, so
             * fail at emission instead. */ \
            if (a_ != (intptr_t)(int32_t)a_) \
                abort(); \
            EW8((p), (uint8_t)(0x04 | (((reg)&7)<<3))); \
            EW8((p), 0x25); \
            EW32((p), (uint32_t)(int32_t)a_); \
        } } while (0)


/* --- instructions used by the benchmark block --- */

/* mov r32, [abs] / mov r64, [abs] */
#define E_MOV_R_M(p, w, reg, addr) do { \
        E_REX((p), (w), (reg), 0, 0); \
        EW8((p), 0x8b); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)

/* mov [abs], r32 / r64 */
#define E_MOV_M_R(p, w, reg, addr) do { \
        E_REX((p), (w), (reg), 0, 0); \
        EW8((p), 0x89); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)

/* mov dword [abs], imm32 */
#define E_MOV_M_I(p, addr, imm) do { \
        EW8((p), 0xc7); E_MODRM_ABS((p), 0, (addr), 4); EW32((p), (imm)); } while (0)

/* mov r32, imm32 (also the LEA-of-absolute peephole) */
#define E_MOV_R_I(p, reg) do { \
        E_REX((p), 0, 0, 0, (reg)); \
        EW8((p), (uint8_t)(0xb8 | ((reg)&7))); } while (0)
#define E_MOV_R_I32(p, reg, imm) do { E_MOV_R_I((p), (reg)); EW32((p), (imm)); } while (0)

/* group1 reg,reg: op = 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp */
#define E_G1_RR(p, w, op, dst, src) do { \
        E_REX((p), (w), (src), 0, (dst)); \
        EW8((p), (uint8_t)(((op)<<3) | 0x01)); \
        E_MODRM_RR((p), (src), (dst)); } while (0)

/* group1 r32, imm  (imm8 shortened when it fits, like the C++ path) */
#define E_G1_RI(p, w, op, reg, imm) do { \
        E_REX((p), (w), 0, 0, (reg)); \
        if (E_IS_S8(imm)) { \
            EW8((p), 0x83); E_MODRM_RR((p), (op), (reg)); EW8((p), (uint8_t)(imm)); \
        } else if ((reg) == 0) { \
            EW8((p), (uint8_t)(((op)<<3) | 0x05)); EW32((p), (imm)); \
        } else { \
            EW8((p), 0x81); E_MODRM_RR((p), (op), (reg)); EW32((p), (imm)); \
        } } while (0)

/* group1 dword [abs], imm */
#define E_G1_MI(p, op, addr, imm) do { \
        if (E_IS_S8(imm)) { \
            EW8((p), 0x83); E_MODRM_ABS((p), (op), (addr), 1); EW8((p), (uint8_t)(imm)); \
        } else { \
            EW8((p), 0x81); E_MODRM_ABS((p), (op), (addr), 4); EW32((p), (imm)); \
        } } while (0)

/* shl r32, imm8. Shift-by-one has a one-byte-shorter dedicated opcode
 * (D1 /4); the C++ emitter prefers it, so we must too. */
#define E_SHL_RI(p, reg, imm) do { \
        E_REX((p), 0, 0, 0, (reg)); \
        if ((imm) == 1) { \
            EW8((p), 0xd1); E_MODRM_RR((p), 4, (reg)); \
        } else { \
            EW8((p), 0xc1); E_MODRM_RR((p), 4, (reg)); EW8((p), (uint8_t)(imm)); \
        } } while (0)

/* test r32, r32 */
/* test r,r. `sz` is the operand size in bytes: the 8-bit form is 0x84 rather
 * than 0x85, and the 16-bit form takes the 0x66 prefix
 * (x86emitter.cpp:799-802). */
#define E_TEST_RR_SZ(p, sz, a, b) do { \
        if ((sz) == 1) { \
            /* 8-bit ids carry the 0x10 marker for spl/bpl/sil/dil, which the \
             * generic REX helper reads as an extended-register bit. */ \
            E_REX8((p), (b), (a)); \
        } else { \
            if ((sz) == 2) E_P16(p); \
            E_REX((p), ((sz) == 8), (b), 0, (a)); \
        } \
        EW8((p), (uint8_t)(((sz) == 1) ? 0x84 : 0x85)); \
        E_MODRM_RR((p), (b), (a)); } while (0)
#define E_TEST_RR(p, a, b) E_TEST_RR_SZ((p), 4, (a), (b))

/* movss xmm, [abs] / movss [abs], xmm */
#define E_MOVSS_R_M(p, reg, addr) do { \
        EW8((p), 0xf3); E_REX((p), 0, (reg), 0, 0); \
        EW8((p), 0x0f); EW8((p), 0x10); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)
#define E_MOVSS_M_R(p, reg, addr) do { \
        EW8((p), 0xf3); E_REX((p), 0, (reg), 0, 0); \
        EW8((p), 0x0f); EW8((p), 0x11); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)

/* short forward Jcc: reserve, then patch. cc is the low nibble of 0x70|cc. */
#define E_JCC8_FWD(p, cc, slot) do { \
        EW8((p), (uint8_t)(0x70 | (cc))); (slot) = (p); EW8((p), 0); } while (0)
#define E_JCC8_SET(p, slot) do { *(slot) = (uint8_t)((p) - ((slot) + 1)); } while (0)

#define E_CC_L  0xc
#define E_CC_Z  0x4


/* ===================================================================
 * Batch 2: group1 memory forms, group2 shifts, mov/movzx/movsx,
 * group3, inc/dec, push/pop/lea, jmp/jcc/setcc/call.
 * Every form below is verified against the C++ emitter by oracle.cpp.
 * =================================================================== */

/* group1 r, [abs]  and  [abs], r  (op<<3 | 0x03 / 0x01) */
#define E_G1_RM(p, w, op, reg, addr) do { \
        E_REX((p), (w), (reg), 0, 0); \
        EW8((p), (uint8_t)(((op)<<3) | 0x03)); \
        E_MODRM_ABS((p), (reg), (addr), 0); } while (0)
#define E_G1_MR(p, w, op, reg, addr) do { \
        E_REX((p), (w), (reg), 0, 0); \
        EW8((p), (uint8_t)(((op)<<3) | 0x01)); \
        E_MODRM_ABS((p), (reg), (addr), 0); } while (0)

/* group2 shifts. g2: 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 7 SAR.
 * Shift-by-zero is elided (the C++ emitter drops it: no flag effect).
 * Shift-by-one uses the short D1 form. */
/* shift r,imm. 0xd0/0xc0 for 8-bit against 0xd1/0xc1 otherwise; a count of
 * zero emits nothing and a count of one takes the short form
 * (groups.cpp:132-146). */
#define E_G2_RI_SZ(p, sz, g2, reg, imm) do { \
        if ((imm) != 0) { \
            if ((sz) == 1) { E_REX8((p), 0, (reg)); } \
            else { if ((sz) == 2) E_P16(p); E_REX((p), ((sz) == 8), 0, 0, (reg)); } \
            if ((imm) == 1) { EW8((p), (uint8_t)(((sz) == 1) ? 0xd0 : 0xd1)); \
                              E_MODRM_RR((p), (g2), (reg)); } \
            else { EW8((p), (uint8_t)(((sz) == 1) ? 0xc0 : 0xc1)); \
                   E_MODRM_RR((p), (g2), (reg)); EW8((p), (uint8_t)(imm)); } \
        } } while (0)
#define E_G2_RI(p, w, g2, reg, imm) E_G2_RI_SZ((p), (w) ? 8 : 4, (g2), (reg), (imm))
/* shift by CL */
#define E_G2_RCL_SZ(p, sz, g2, reg) do { \
        if ((sz) == 1) { E_REX8((p), 0, (reg)); } \
        else { if ((sz) == 2) E_P16(p); E_REX((p), ((sz) == 8), 0, 0, (reg)); } \
        EW8((p), (uint8_t)(((sz) == 1) ? 0xd2 : 0xd3)); \
        E_MODRM_RR((p), (g2), (reg)); } while (0)
#define E_G2_RCL(p, w, g2, reg) E_G2_RCL_SZ((p), (w) ? 8 : 4, (g2), (reg))

/* mov r,r. Self-moves are elided to match _xMovRtoR's "ignore redundant MOVs".
 * NOTE: for the 32-bit form this is not strictly a no-op -- `mov eax,eax`
 * zero-extends into rax -- but the C++ emitter drops it, and byte-exactness
 * with the reference is the requirement here. See the note in the port log. */
#define E_MOV_RR_RAW(p, w, dst, src) do { \
        E_REX((p), (w), (src), 0, (dst)); \
        EW8((p), 0x89); E_MODRM_RR((p), (src), (dst)); } while (0)
#define E_MOV_RR(p, w, dst, src) do { \
        if ((dst) != (src)) { E_MOV_RR_RAW((p), (w), (dst), (src)); } } while (0)

/* movzx / movsx from 8- or 16-bit register. srcw: 0 = byte, 1 = word.
 * sx selects movsx (0xbe/0xbf) over movzx (0xb6/0xb7). */
/* movsx/movzx. The *source* may be an 8-bit register, whose id carries the
 * 0x10 marker for spl/bpl/sil/dil -- the generic REX helper reads that as an
 * extended-register bit and emits a spurious REX.B. The destination is always
 * 16/32/64-bit, so only the source needs the 8-bit treatment. */
#define E_MOVEXT_RR(p, w, sx, srcw, dst, src) do { \
        if (!(srcw)) { \
            uint8_t rex_ = (uint8_t)(0x40 | ((w) ? 8 : 0) \
                    | ((((dst) >= 8) ? 1 : 0) << 2) | E_R8_EXT(src)); \
            if (rex_ != 0x40 || E_R8_NEEDREX(src)) EW8((p), rex_); \
        } else E_REX((p), (w), (dst), 0, (src)); \
        EW8((p), 0x0f); \
        EW8((p), (uint8_t)(((sx) ? 0xbe : 0xb6) + ((srcw) ? 1 : 0))); \
        E_MODRM_RR((p), (dst), (src)); } while (0)

/* group3: 2 NOT, 3 NEG, 4 MUL, 5 IMUL, 6 DIV, 7 IDIV */
/* group3 at any operand size: 0xf6 for 8-bit, 0xf7 otherwise, with 16-bit
 * taking the 0x66 prefix (groups.cpp). */
#define E_G3_R_SZ(p, sz, g3, reg) do { \
        if ((sz) == 2) E_P16(p); \
        if ((sz) == 1) E_REX8_RM((p), (reg)); \
        else E_REX((p), ((sz) == 8), 0, 0, (reg)); \
        EW8((p), (uint8_t)(((sz) == 1) ? 0xf6 : 0xf7)); \
        E_MODRM_RR((p), (g3), (reg)); } while (0)
#define E_G3_R(p, w, g3, reg) E_G3_R_SZ((p), (w) ? 8 : 4, (g3), (reg))
#define E_G3_M(p, g3, addr) do { \
        EW8((p), 0xf7); E_MODRM_ABS((p), (g3), (addr), 0); } while (0)

/* inc / dec (64-bit mode: FF /0 and FF /1; no single-byte 0x4x form) */
/* inc/dec at any operand size: 0xfe for 8-bit, 0xff otherwise. */
#define E_INCDEC_R_SZ(p, sz, dec, reg) do { \
        if ((sz) == 2) E_P16(p); \
        if ((sz) == 1) E_REX8_RM((p), (reg)); \
        else E_REX((p), ((sz) == 8), 0, 0, (reg)); \
        EW8((p), (uint8_t)(((sz) == 1) ? 0xfe : 0xff)); \
        E_MODRM_RR((p), ((dec) ? 1 : 0), (reg)); } while (0)
#define E_INCDEC_R(p, w, dec, reg) E_INCDEC_R_SZ((p), (w) ? 8 : 4, (dec), (reg))

/* push / pop reg. Default operand size is 64-bit, so REX.W is never set;
 * only REX.B for r8-r15. */
#define E_PUSH_R(p, reg) do { \
        if ((reg) >= 8) EW8((p), 0x41); \
        EW8((p), (uint8_t)(0x50 | ((reg)&7))); } while (0)
#define E_POP_R(p, reg) do { \
        if ((reg) >= 8) EW8((p), 0x41); \
        EW8((p), (uint8_t)(0x58 | ((reg)&7))); } while (0)

/* test r, imm32 (A9 short form when the destination is the accumulator) */
/* test r,imm. 0xa8/0xa9 is the accumulator short form; 0xf6/0xf7 otherwise.
 * The immediate is written at the operand's size
 * (x86emitter.cpp:810-821). */
#define E_TEST_RI_SZ(p, sz, reg, imm) do { \
        if ((sz) == 1) { E_REX8((p), 0, (reg)); } \
        else { if ((sz) == 2) E_P16(p); E_REX((p), ((sz) == 8), 0, 0, (reg)); } \
        if ((reg) == 0) { EW8((p), (uint8_t)(((sz) == 1) ? 0xa8 : 0xa9)); } \
        else { EW8((p), (uint8_t)(((sz) == 1) ? 0xf6 : 0xf7)); \
               E_MODRM_RR((p), 0, (reg)); } \
        if ((sz) == 1) { EW8((p), (uint8_t)(imm)); } \
        else if ((sz) == 2) { EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); } \
        else { EW32((p), (imm)); } } while (0)
#define E_TEST_RI(p, w, reg, imm) E_TEST_RI_SZ((p), (w) ? 8 : 4, (reg), (imm))

/* setcc r8 */
#define E_SETCC_R(p, cc, reg) do { \
        if ((reg) >= 4) EW8((p), (uint8_t)(0x40 | (((reg)>>3) ? 1 : 0))); \
        EW8((p), 0x0f); EW8((p), (uint8_t)(0x90 | (cc))); \
        E_MODRM_RR((p), 0, (reg)); } while (0)

/* near jmp / jcc with a 32-bit displacement to a known target */
#define E_JMP32(p, target) do { \
        intptr_t d_ = (intptr_t)(target) - ((intptr_t)(p) + 5); \
        EW8((p), 0xe9); EW32((p), (uint32_t)(int32_t)d_); } while (0)
#define E_JCC32(p, cc, target) do { \
        intptr_t d_ = (intptr_t)(target) - ((intptr_t)(p) + 6); \
        EW8((p), 0x0f); EW8((p), (uint8_t)(0x80 | (cc))); \
        EW32((p), (uint32_t)(int32_t)d_); } while (0)

/* condition codes */
#define E_CC_O  0x0
#define E_CC_NO 0x1
#define E_CC_B  0x2
#define E_CC_AE 0x3
#define E_CC_E  0x4
#define E_CC_NE 0x5
#define E_CC_BE 0x6
#define E_CC_A  0x7
#define E_CC_S  0x8
#define E_CC_NS 0x9
#define E_CC_GE 0xd
#define E_CC_LE 0xe
#define E_CC_G  0xf

/* ===================================================================
 * Batch 3: full memory operands (base + index*scale + disp).
 * Mirrors xIndirectVoid::Reduce, NeedsSibMagic and EmitSibMagic.
 * E_NOREG marks an absent register (xEmptyReg).
 * =================================================================== */

#define E_NOREG (-1)


struct e_mem { int base; int index; int scale; intptr_t disp; };


/* xIndirectVoid::Reduce, verbatim in behaviour. Note it is applied once at
 * construction, exactly as the C++ constructor does. */
#define E_MEM(m, b, i, sc, d) do { \
        (m).base = (b); (m).index = (i); (m).scale = (sc); (m).disp = (d); \
        if ((m).index == 4) { (m).base = (m).index; } \
        else if ((m).index == E_NOREG) { \
            (m).index = (m).base; (m).scale = 0; \
            if ((m).base != 4) (m).base = E_NOREG; \
        } else { \
            switch ((m).scale) { \
            case 1: (m).scale = 0; break; \
            case 2: (m).scale = 1; break; \
            case 3: (m).base = (m).index; (m).scale = 1; break; \
            case 4: (m).scale = 2; break; \
            case 5: (m).base = (m).index; (m).scale = 2; break; \
            case 8: (m).scale = 3; break; \
            case 9: (m).base = (m).index; (m).scale = 3; break; \
            default: break; \
            } \
        } } while (0)

/* e_mem value builders: construct operands directly, no xAddressVoid.
 * e_mem_bd routes through E_MEM so the operand carries the same Reduce()d
 * form every emission helper was written against; e_mem_off only shifts the
 * displacement, which commutes with the reduction. By-value struct returns
 * are plain C89. */
static struct e_mem e_mem_bd(int base_reg, intptr_t disp)
{
	struct e_mem m;
	E_MEM(m, base_reg, E_NOREG, 0, disp);
	return m;
}
static struct e_mem e_mem_off(struct e_mem m, intptr_t extra)
{
	m.disp += extra;
	return m;
}
/* Optional e_mem, C89-style: has==0 means absent. */
struct e_memopt { int has; struct e_mem m; };
static struct e_memopt e_memopt_none(void)
{
	struct e_memopt o; o.has = 0; o.m.base = E_NOREG; o.m.index = E_NOREG; o.m.scale = 0; o.m.disp = 0; return o;
}
static struct e_memopt e_memopt_of(struct e_mem m)
{
	struct e_memopt o; o.has = 1; o.m = m; return o;
}

static struct e_mem e_mem_abs(const void* addr)
{
	struct e_mem m;
	E_MEM(m, E_NOREG, E_NOREG, 0, (intptr_t)(uintptr_t)addr);
	return m;
}

#define E_NEEDS_SIB(m) \
        (((m).index != E_NOREG) && (((m).scale != 0) || ((m).base != E_NOREG)))

/* REX for reg + memory operand. Mirrors EmitRex(xRegisterBase, xIndirectVoid):
 * when no SIB is needed the index register's extension bit moves to B. */
#define E_REX_MEM(p, w, reg, m) do { \
        int rx_ = ((m).index != E_NOREG && (m).index >= 8) ? 1 : 0; \
        int rb_ = ((m).base  != E_NOREG && (m).base  >= 8) ? 1 : 0; \
        uint8_t rex_; \
        if (!E_NEEDS_SIB(m)) { rb_ = rx_; rx_ = 0; } \
        /* (int) cast: callers pass group-code ENUMS here (they occupy the reg \
 * field and never take REX.R); Apple clang's enum range analysis flags \
 * the >=8 comparison as always-false and spams four warnings per TU. */ \
        rex_ = (uint8_t)(0x40 | ((w)?8:0) | ((((int)(reg)>=8)?1:0)<<2) | (rx_<<1) | rb_); \
        if (rex_ != 0x40) EW8((p), rex_); } while (0)

/* EmitSibMagic for the register form. `after` counts bytes emitted after the
 * displacement (immediates), needed only by the absolute/RIP fallback. */
#define E_MODRM_MEM(p, reg, m, after) do { \
        int ds_ = ((m).disp == 0) ? 0 : (E_IS_S8((m).disp) ? 1 : 2); \
        if (!E_NEEDS_SIB(m)) { \
            if ((m).index == E_NOREG) { \
                E_MODRM_ABS((p), (reg), (m).disp, (after)); \
                ds_ = -1; \
            } else { \
                if (((m).index & 7) == 5 && ds_ == 0) ds_ = 1; \
                EW8((p), (uint8_t)((ds_ << 6) | (((reg)&7) << 3) | ((m).index & 7))); \
                /* rm=100 always means a SIB follows. rsp is diverted into the \
                 * SIB branch by E_MEM; r12 reaches here and needs one. */ \
                if (((m).index & 7) == 4) \
                    EW8((p), (uint8_t)((0 << 6) | (4 << 3) | ((m).index & 7))); \
            } \
        } else { \
            if ((m).base == E_NOREG) { \
                /* [index*scale + disp32]: the displacement field is 32 bits \
                 * and sign-extended. A displacement that does not survive \
                 * the round-trip cannot be expressed in this operand shape; \
                 * truncating would index the table at the wrong page once \
                 * the address maps far, which is exactly the layout Windows \
                 * produces. Same discipline as E_MODRM_ABS: the caller must \
                 * materialize the base (xe_complexaddr_si) first. */ \
                if ((intptr_t)(m).disp != (intptr_t)(int32_t)(m).disp) \
                    abort(); \
                EW8((p), (uint8_t)((0 << 6) | (((reg)&7) << 3) | 4)); \
                EW8((p), (uint8_t)(((m).scale << 6) | (((m).index) << 3) | 5)); \
                EW32((p), (uint32_t)(int32_t)(m).disp); \
                ds_ = -1; \
            } else { \
                if (((m).base & 7) == 5 && ds_ == 0) ds_ = 1; \
                EW8((p), (uint8_t)((ds_ << 6) | (((reg)&7) << 3) | 4)); \
                EW8((p), (uint8_t)(((m).scale << 6) | (((m).index & 7) << 3) | ((m).base & 7))); \
            } \
        } \
        if (ds_ == 1) { EW8((p), (uint8_t)(m).disp); } \
        else if (ds_ == 2) { EW32((p), (uint32_t)(int32_t)(m).disp); } } while (0)

/* mov r,[mem] / mov [mem],r over the full addressing form */
#define E_MOV_R_MEM(p, w, reg, m) do { \
        E_REX_MEM((p), (w), (reg), (m)); \
        EW8((p), 0x8b); E_MODRM_MEM((p), (reg), (m), 0); } while (0)
/* mov r,[mem] and mov [mem],r at any operand size: 0x8a/0x88 for 8-bit
 * against 0x8b/0x89 otherwise, 16-bit taking the 0x66 prefix. */
#define E_MOV_R_MEM_SZ(p, sz, reg, m) do { \
        if ((sz) == 2) E_P16(p); \
        if ((sz) == 1) E_REX8_MEM((p), (reg), (m)); \
        else E_REX_MEM((p), ((sz) == 8), (reg), (m)); \
        EW8((p), (uint8_t)(((sz) == 1) ? 0x8a : 0x8b)); \
        E_MODRM_MEM((p), (reg), (m), 0); } while (0)
#define E_MOV_MEM_R_SZ(p, sz, reg, m) do { \
        if ((sz) == 2) E_P16(p); \
        if ((sz) == 1) E_REX8_MEM((p), (reg), (m)); \
        else E_REX_MEM((p), ((sz) == 8), (reg), (m)); \
        EW8((p), (uint8_t)(((sz) == 1) ? 0x88 : 0x89)); \
        E_MODRM_MEM((p), (reg), (m), 0); } while (0)
#define E_MOV_MEM_R(p, w, reg, m) do { \
        E_REX_MEM((p), (w), (reg), (m)); \
        EW8((p), 0x89); E_MODRM_MEM((p), (reg), (m), 0); } while (0)


/* group1 with a memory destination and an immediate, over the full addressing
 * form and every operand width. Mirrors xImpl_Group1::operator()(
 * xIndirect64orLess, int) in groups.cpp:43-65.
 *
 * `sz` is the operand size in bytes (1, 2, 4 or 8). The immediate is one byte
 * for the 8-bit form and for the sign-extended 0x83 form, two for 16-bit and
 * four otherwise -- and the RIP-relative displacement has to be corrected by
 * that count, which is what the reference passes as extraRIPOffset. */
#define E_G1_MEM_I(p, sz, op, m, imm) do { \
        if ((sz) == 1) { \
            E_REX_MEM((p), 0, (op), (m)); \
            EW8((p), 0x80); E_MODRM_MEM((p), (op), (m), 1); \
            EW8((p), (uint8_t)(imm)); \
        } else if (E_IS_S8(imm)) { \
            if ((sz) == 2) E_P16(p); \
            E_REX_MEM((p), ((sz) == 8), (op), (m)); \
            EW8((p), 0x83); E_MODRM_MEM((p), (op), (m), 1); \
            EW8((p), (uint8_t)(imm)); \
        } else if ((sz) == 2) { \
            E_P16(p); \
            E_REX_MEM((p), 0, (op), (m)); \
            EW8((p), 0x81); E_MODRM_MEM((p), (op), (m), 2); \
            EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); \
        } else { \
            E_REX_MEM((p), ((sz) == 8), (op), (m)); \
            EW8((p), 0x81); E_MODRM_MEM((p), (op), (m), 4); \
            EW32((p), (uint32_t)(imm)); \
        } } while (0)


/* mov [mem], imm and test [mem], imm. Both write the immediate at the
 * operand's size -- one byte, two, or four with the 64-bit form taking a
 * sign-extended dword -- and the RIP-relative displacement is corrected by
 * that same count, which is what the reference passes as extraRIPOffset.
 * mov is movs.cpp:66-70, test is x86emitter.cpp:804-808; they differ only in
 * the opcode pair. */
#define E_MEMIMM_(p, sz, op8, op32, m, imm) do { \
        if ((sz) == 2) E_P16(p); \
        E_REX_MEM((p), ((sz) == 8), 0, (m)); \
        EW8((p), (uint8_t)(((sz) == 1) ? (op8) : (op32))); \
        E_MODRM_MEM((p), 0, (m), ((sz) == 1) ? 1 : (((sz) == 2) ? 2 : 4)); \
        if ((sz) == 1) { EW8((p), (uint8_t)(imm)); } \
        else if ((sz) == 2) { EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); } \
        else { EW32((p), (uint32_t)(imm)); } } while (0)

#define E_MOV_MEM_I(p, sz, m, imm)  E_MEMIMM_((p), (sz), 0xc6, 0xc7, (m), (imm))
#define E_TEST_MEM_I(p, sz, m, imm) E_MEMIMM_((p), (sz), 0xf6, 0xf7, (m), (imm))

/* group1 r,[mem] and [mem],r */
/* group1 against memory, at any operand size. The low bit of the opcode
 * selects the width -- 0x02/0x00 for 8-bit against 0x03/0x01 otherwise -- and
 * 16-bit additionally takes the 0x66 prefix (groups.cpp:73-84). The register
 * forms already dispatched on size; these did not, which meant an 8-bit
 * operand emitted the 32-bit encoding and read or wrote three bytes too many. */
#define E_G1_R_MEM_SZ(p, sz, op, reg, m) do { \
        if ((sz) == 2) E_P16(p); \
        if ((sz) == 1) E_REX8_MEM((p), (reg), (m)); \
        else E_REX_MEM((p), ((sz) == 8), (reg), (m)); \
        EW8((p), (uint8_t)(((op)<<3) | (((sz) == 1) ? 0x02 : 0x03))); \
        E_MODRM_MEM((p), (reg), (m), 0); } while (0)
#define E_G1_MEM_R_SZ(p, sz, op, reg, m) do { \
        if ((sz) == 2) E_P16(p); \
        if ((sz) == 1) E_REX8_MEM((p), (reg), (m)); \
        else E_REX_MEM((p), ((sz) == 8), (reg), (m)); \
        EW8((p), (uint8_t)(((op)<<3) | (((sz) == 1) ? 0x00 : 0x01))); \
        E_MODRM_MEM((p), (reg), (m), 0); } while (0)
#define E_G1_R_MEM(p, w, op, reg, m) E_G1_R_MEM_SZ((p), (w) ? 8 : 4, (op), (reg), (m))
#define E_G1_MEM_R(p, w, op, reg, m) E_G1_MEM_R_SZ((p), (w) ? 8 : 4, (op), (reg), (m))

/* ===================================================================
 * Batch 4: group3 memory forms, MUL/DIV family, IMUL two- and
 * three-operand forms, LEA with its peepholes.
 * =================================================================== */

/* group3 over a full memory operand (F7 /g3) */
#define E_G3_MEM_SZ(p, sz, g3, m) do { \
        if ((sz) == 2) E_P16(p); \
        E_REX_MEM((p), ((sz) == 8), 0, (m)); \
        EW8((p), (uint8_t)(((sz) == 1) ? 0xf6 : 0xf7)); \
        E_MODRM_MEM((p), (g3), (m), 0); } while (0)
#define E_G3_MEM(p, w, g3, m) E_G3_MEM_SZ((p), (w) ? 8 : 4, (g3), (m))

/* two-operand IMUL: 0F AF /r */
#define E_IMUL_RR(p, w, dst, src) do { \
        E_REX((p), (w), (dst), 0, (src)); \
        EW8((p), 0x0f); EW8((p), 0xaf); \
        E_MODRM_RR((p), (dst), (src)); } while (0)
#define E_IMUL_R_MEM(p, w, dst, m) do { \
        E_REX_MEM((p), (w), (dst), (m)); \
        EW8((p), 0x0f); EW8((p), 0xaf); \
        E_MODRM_MEM((p), (dst), (m), 0); } while (0)

/* three-operand IMUL: 6B /r ib when the immediate fits in s8, else 69 /r id. */
#define E_IMUL_RRI(p, w, dst, src, imm) do { \
        E_REX((p), (w), (dst), 0, (src)); \
        if (E_IS_S8(imm)) { \
            EW8((p), 0x6b); E_MODRM_RR((p), (dst), (src)); EW8((p), (uint8_t)(imm)); \
        } else { \
            EW8((p), 0x69); E_MODRM_RR((p), (dst), (src)); EW32((p), (imm)); \
        } } while (0)
#define E_IMUL_RMI(p, w, dst, m, imm) do { \
        E_REX_MEM((p), (w), (dst), (m)); \
        if (E_IS_S8(imm)) { \
            EW8((p), 0x6b); E_MODRM_MEM((p), (dst), (m), 1); EW8((p), (uint8_t)(imm)); \
        } else { \
            EW8((p), 0x69); E_MODRM_MEM((p), (dst), (m), 4); EW32((p), (imm)); \
        } } while (0)

/* LEA. Mirrors EmitLeaMagic (x86emitter.cpp:678-763) branch for branch.
 * Most operand shapes are rewritten into shorter sequences; only the residual
 * cases reach an actual 8D. `pf` is preserve_flags.
 *
 * Note the reference compares Index/Base against rsp/rbp by Id, so r12/r13
 * take different paths than their low counterparts -- see the port log. */
#define E_LEA_SZ(p, sz, pf, dst, m) do { \
        const int leaw_ = ((sz) == 8); \
        int ds_ = ((m).disp == 0) ? 0 : (E_IS_S8((m).disp) ? 1 : 2); \
        int done_ = 0; \
        if (!E_NEEDS_SIB(m) && (m).disp == (intptr_t)(int32_t)(m).disp) { \
            if ((m).index == E_NOREG) { \
                /* The reference calls xMOV(to, disp) with preserve_flags \
                 * defaulted -- so a zero displacement becomes XOR even when \
                 * the caller asked for flags to be preserved, and the MOV is \
                 * emitted at the destination's own width. Using the raw \
                 * B8+imm32 form here diverged on both counts. */ \
                E_MOV_RI_SZ((p), (sz), 0, (dst), (intptr_t)(m).disp); done_ = 1; \
            } else if (ds_ == 0) { \
                E_MOV_RR((p), leaw_, (dst), (m).index); done_ = 1; \
            } else if (!(pf)) { \
                E_MOV_RR((p), leaw_, (dst), (m).index); \
                E_G1_RI((p), leaw_, 0, (dst), (int32_t)(m).disp); done_ = 1; \
            } \
        } else if ((m).base == E_NOREG) { \
            if (!(pf) && ds_ == 0) { \
                E_MOV_RR((p), leaw_, (dst), (m).index); \
                E_G2_RI((p), leaw_, 4, (dst), (m).scale); done_ = 1; \
            } \
        } else if ((m).scale == 0) { \
            if (!(pf)) { \
                if ((m).index == 4) { \
                    E_MOV_RR((p), leaw_, (dst), (m).base); \
                    if ((m).disp) E_G1_RI((p), leaw_, 0, (dst), (int32_t)(m).disp); \
                    done_ = 1; \
                } else if ((m).disp == 0) { \
                    E_MOV_RR((p), leaw_, (dst), (m).base); \
                    E_G1_RR((p), leaw_, 0, (dst), (m).index); \
                    done_ = 1; \
                } \
            } else if ((m).index == 4 && (m).disp == 0) { \
                E_MOV_RR((p), leaw_, (dst), (m).base); done_ = 1; \
            } \
        } \
        if (!done_) { \
            E_REX_MEM((p), leaw_, (dst), (m)); \
            EW8((p), 0x8d); E_MODRM_MEM((p), (dst), (m), 0); \
        } } while (0)

#define E_LEA(p, w, pf, dst, m) E_LEA_SZ((p), (w) ? 8 : 4, (pf), (dst), (m))

/* ===================================================================
 * Batch 5: jumps and calls.
 *   - known-target Jcc/JMP with short-vs-near selection
 *   - the xForwardJump construct/patch protocol
 *   - CALL, RET, NOP
 * E_CC_UNC is the unconditional pseudo-condition; the reference uses a
 * distinct opcode for it in every form, so it is not a real cc value.
 * =================================================================== */

#define E_CC_UNC 0x10

/* Known-target jump. Tries the 2-byte short form first, assuming a 2-byte
 * instruction, exactly as xJccKnownTarget does; falls back to the near form
 * and patches the displacement afterwards. */
#define E_JCC_TO(p, cc, target) do { \
        intptr_t d8_ = (intptr_t)(target) - ((intptr_t)(p) + 2); \
        if (E_IS_S8(d8_)) { \
            EW8((p), (uint8_t)(((cc) == E_CC_UNC) ? 0xeb : (0x70 | (cc)))); \
            EW8((p), (uint8_t)d8_); \
        } else { \
            uint8_t* slot_; \
            intptr_t dn_; \
            if ((cc) == E_CC_UNC) { EW8((p), 0xe9); } \
            else { EW8((p), 0x0f); EW8((p), (uint8_t)(0x80 | (cc))); } \
            slot_ = (p); EW32((p), 0); \
            dn_ = (intptr_t)(target) - (intptr_t)(p); \
            slot_[0]=(uint8_t)dn_; slot_[1]=(uint8_t)(dn_>>8); \
            slot_[2]=(uint8_t)(dn_>>16); slot_[3]=(uint8_t)(dn_>>24); \
        } } while (0)

/* Forward jump, short (1-byte displacement) form.
 * `base` receives the address just past the instruction, matching
 * xForwardJump<s8>::BasePtr; E_FWD8_SET patches BasePtr[-1]. */
#define E_FWD8(p, cc, base) do { \
        (base) = (p) + 2; \
        EW8((p), (uint8_t)(((cc) == E_CC_UNC) ? 0xeb : (0x70 | (cc)))); \
        (p) += 1; } while (0)
#define E_FWD8_SET(p, base) do { \
        (base)[-1] = (uint8_t)((intptr_t)(p) - (intptr_t)(base)); } while (0)

/* Forward jump, near (4-byte displacement) form. BasePtr is 5 bytes past for
 * an unconditional jump and 6 for a conditional one. */
#define E_FWD32(p, cc, base) do { \
        (base) = (p) + (((cc) == E_CC_UNC) ? 5 : 6); \
        if ((cc) == E_CC_UNC) { EW8((p), 0xe9); } \
        else { EW8((p), 0x0f); EW8((p), (uint8_t)(0x80 | (cc))); } \
        (p) += 4; } while (0)
#define E_FWD32_SET(p, base) do { \
        intptr_t d_ = (intptr_t)(p) - (intptr_t)(base); \
        (base)[-4]=(uint8_t)d_; (base)[-3]=(uint8_t)(d_>>8); \
        (base)[-2]=(uint8_t)(d_>>16); (base)[-1]=(uint8_t)(d_>>24); } while (0)

/* call rel32 / call r/m64 / ret / nop */
#define E_CALL_REL(p, target) do { \
        intptr_t d_ = (intptr_t)(target) - ((intptr_t)(p) + 5); \
        EW8((p), 0xe8); EW32((p), (uint32_t)(int32_t)d_); } while (0)
#define E_CALL_R(p, reg) do { \
        if ((reg) >= 8) EW8((p), 0x41); \
        EW8((p), 0xff); E_MODRM_RR((p), 2, (reg)); } while (0)
#define E_JMP_R(p, reg) do { \
        if ((reg) >= 8) EW8((p), 0x41); \
        EW8((p), 0xff); E_MODRM_RR((p), 4, (reg)); } while (0)
#define E_RET(p)  EW8((p), 0xc3)
#define E_NOP(p)  EW8((p), 0x90)

/* ===================================================================
 * Batch 5b: 16-bit operand forms. These are the 32-bit encodings with a
 * 0x66 operand-size prefix, emitted before REX.
 * =================================================================== */

#define E_P16(p) EW8((p), 0x66)

#define E_MOV16_RR(p, dst, src) do { \
        if ((dst) != (src)) { \
            E_P16(p); E_REX((p), 0, (src), 0, (dst)); \
            EW8((p), 0x89); E_MODRM_RR((p), (src), (dst)); \
        } } while (0)
#define E_MOV16_R_M(p, reg, addr) do { \
        E_P16(p); E_REX((p), 0, (reg), 0, 0); \
        EW8((p), 0x8b); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)
#define E_MOV16_M_R(p, reg, addr) do { \
        E_P16(p); E_REX((p), 0, (reg), 0, 0); \
        EW8((p), 0x89); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)
#define E_G1_16_RR(p, op, dst, src) do { \
        E_P16(p); E_REX((p), 0, (src), 0, (dst)); \
        EW8((p), (uint8_t)(((op)<<3) | 0x01)); \
        E_MODRM_RR((p), (src), (dst)); } while (0)
/* 16-bit group1 with an immediate: imm8 short form, else imm16 */
#define E_G1_16_RI(p, op, reg, imm) do { \
        E_P16(p); E_REX((p), 0, 0, 0, (reg)); \
        if (E_IS_S8(imm)) { \
            EW8((p), 0x83); E_MODRM_RR((p), (op), (reg)); EW8((p), (uint8_t)(imm)); \
        } else if ((reg) == 0) { \
            EW8((p), (uint8_t)(((op)<<3) | 0x05)); \
            EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); \
        } else { \
            EW8((p), 0x81); E_MODRM_RR((p), (op), (reg)); \
            EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); \
        } } while (0)

/* ===================================================================
 * Batch 6: 8-bit operand forms.
 *
 * The 8-bit register file overlaps itself. Ids 4-7 name ah/ch/dh/bh when no
 * REX byte is present, and spl/bpl/sil/dil when one is. The reference marks
 * the latter by setting bit 0x10 in the id (x86types.h:329) and forces a REX
 * byte for them even when it would be an empty 0x40 (x86emitter.cpp:386).
 *
 * So an 8-bit register argument here carries the same encoding: the low three
 * bits select, bit 0x10 means "needs REX", and IsExtended tests (Id & 0x0F) > 7
 * rather than Id > 7, so 0x14 (spl) is NOT extended.
 *
 * E_R8H(n)  -> ah/ch/dh/bh for n in 4..7
 * E_R8L(n)  -> spl/bpl/sil/dil for n in 4..7
 * =================================================================== */

#define E_R8H(n) (n)
#define E_R8L(n) ((n) | 0x10)
#define E_R8_EXT(r)  ((((r) & 0x0F) > 7) ? 1 : 0)
#define E_R8_NEEDREX(r) (((r) >= 0x10) ? 1 : 0)

/* REX for an 8-bit reg,reg pair. Emitted when any bit is set OR when either
 * operand is one of spl/bpl/sil/dil. */
#define E_REX8(p, reg1, reg2) do { \
        uint8_t rex_ = (uint8_t)(0x40 | (E_R8_EXT(reg1) << 2) | E_R8_EXT(reg2)); \
        if (rex_ != 0x40 || E_R8_NEEDREX(reg1) || E_R8_NEEDREX(reg2)) \
            EW8((p), rex_); } while (0)

/* REX for an 8-bit reg used as the *reg* field (with a memory operand). */
/* REX for an 8-bit reg field combined with a memory operand: the reg's 0x10
 * marker decides whether a REX byte is mandatory at all (spl/bpl/sil/dil),
 * while the base and index supply B and X exactly as E_REX_MEM computes them. */
#define E_REX8_MEM(p, reg, m) do { \
        int rx_ = ((m).index != E_NOREG && (m).index >= 8) ? 1 : 0; \
        int rb_ = ((m).base  != E_NOREG && (m).base  >= 8) ? 1 : 0; \
        uint8_t rex_; \
        if (!E_NEEDS_SIB(m)) { rb_ = rx_; rx_ = 0; } \
        rex_ = (uint8_t)(0x40 | (E_R8_EXT(reg) << 2) | (rx_ << 1) | rb_); \
        if (rex_ != 0x40 || E_R8_NEEDREX(reg)) EW8((p), rex_); } while (0)

#define E_REX8_M(p, reg) do { \
        uint8_t rex_ = (uint8_t)(0x40 | (E_R8_EXT(reg) << 2)); \
        if (rex_ != 0x40 || E_R8_NEEDREX(reg)) EW8((p), rex_); } while (0)

/* REX for an 8-bit reg used as the *rm* field (single-operand forms such as
 * group1-with-immediate and setcc). The extension bit is B, not R. */
#define E_REX8_RM(p, reg) do { \
        uint8_t rex_ = (uint8_t)(0x40 | E_R8_EXT(reg)); \
        if (rex_ != 0x40 || E_R8_NEEDREX(reg)) EW8((p), rex_); } while (0)

#define E_MOV8_RR(p, dst, src) do { \
        if ((dst) != (src)) { \
            E_REX8((p), (src), (dst)); \
            EW8((p), 0x88); E_MODRM_RR((p), (src), (dst)); \
        } } while (0)
#define E_MOV8_R_M(p, reg, addr) do { \
        E_REX8_M((p), (reg)); \
        EW8((p), 0x8a); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)
#define E_MOV8_M_R(p, reg, addr) do { \
        E_REX8_M((p), (reg)); \
        EW8((p), 0x88); E_MODRM_ABS((p), (reg), (addr), 0); } while (0)

/* 8-bit group1: reg,reg uses op<<3|0x00; reg,imm8 uses 0x80 /op ib, with the
 * 0x04|op<<3 accumulator short form for al. */
#define E_G1_8_RR(p, op, dst, src) do { \
        E_REX8((p), (src), (dst)); \
        EW8((p), (uint8_t)((op) << 3)); \
        E_MODRM_RR((p), (src), (dst)); } while (0)
#define E_G1_8_RI(p, op, reg, imm) do { \
        E_REX8_RM((p), (reg)); \
        if ((reg) == 0) { EW8((p), (uint8_t)(((op)<<3) | 0x04)); } \
        else { EW8((p), 0x80); E_MODRM_RR((p), (op), (reg)); } \
        EW8((p), (uint8_t)(imm)); } while (0)

/* setcc into an 8-bit register: 0F 90+cc /0 */
#define E_SETCC_R8(p, cc, reg) do { \
        E_REX8_RM((p), (reg)); \
        EW8((p), 0x0f); EW8((p), (uint8_t)(0x90 | (cc))); \
        E_MODRM_RR((p), 0, (reg)); } while (0)

/* ===================================================================
 * Batch 7: SSE.
 *
 * Every SSE form in the reference funnels through xOpWrite0F, so the whole
 * family reduces to a handful of encoding shapes:
 *
 *   [prefix] [REX] 0F opcode         ModRM/SIB [imm8]     two-byte opcode
 *   [prefix] [REX] 0F 3A|38 opcode   ModRM/SIB [imm8]     three-byte opcode
 *
 * The three-byte form is selected by the low byte of the 16-bit opcode value
 * being 0x38 or 0x3a, matching xOpWrite0F's is16BitOpcode test; the high byte
 * is then the real opcode. xRegisterSSE has operand size 16, so REX.W is never
 * set and the only bits that matter are R and B.
 *
 * The register is always the ModRM.reg field, for loads and stores alike --
 * stores just use a different opcode and pass the register first.
 * =================================================================== */

#define E_REX_SSE(p, reg1, reg2) do { \
        uint8_t rex_ = (uint8_t)(0x40 | (((((reg1) & 0x0F) > 7) ? 1 : 0) << 2) \
                                | ((((reg2) & 0x0F) > 7) ? 1 : 0)); \
        if (rex_ != 0x40) EW8((p), rex_); } while (0)

/* Emits 0F + opcode, or 0F 3A/38 + opcode for the three-byte forms. */
/* Selection by switch rather than `(op & 0xff) == 0x38 || == 0x3a`: the
 * opcode is a literal at nearly every call site, GCC folds the masked
 * comparisons to constant false and emits -Wtautological-compare twice
 * per expansion -- across every shim body that carries an immediate,
 * that was a solid wall of warnings in the aarch64 log. The switch folds
 * to the same code with nothing to warn about. */
#define E_SSE_OP(p, opcode) do { \
        switch ((opcode) & 0xff) { \
        case 0x38: \
        case 0x3a: \
            EW8((p), 0x0f); \
            EW8((p), (uint8_t)((opcode) & 0xff)); \
            EW8((p), (uint8_t)((opcode) >> 8)); \
            break; \
        default: \
            EW8((p), 0x0f); \
            EW8((p), (uint8_t)((opcode) & 0xff)); \
            break; \
        } } while (0)

/* reg,reg */
#define E_SSE_RR(p, pre, opcode, r1, r2) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        E_REX_SSE((p), (r1), (r2)); \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_RR((p), (r1), (r2)); } while (0)

/* reg + absolute address */
#define E_SSE_R_M(p, pre, opcode, r1, addr) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        E_REX_SSE((p), (r1), 0); \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_ABS((p), (r1), (addr), 0); } while (0)

/* reg + full base/index/scale/disp operand.
 * `w` is REX.W. It is not always zero for SSE: EmitRex sets it when *either*
 * operand is 64-bit, and xIndirectVoid carries its own operand size, so a
 * ptr64[] source makes MOVSD emit F2 48 0F 10 where a ptr32[] source makes
 * MOVSS emit F3 0F 10. Callers pass the memory operand's width. */
#define E_SSE_R_MEM_W(p, pre, opcode, r1, m, w) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        E_REX_MEM((p), (w), (r1), (m)); \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_MEM((p), (r1), (m), 0); } while (0)
#define E_SSE_R_MEM(p, pre, opcode, r1, m) \
        E_SSE_R_MEM_W((p), (pre), (opcode), (r1), (m), 0)

/* the imm8 variants (shufps, pshufd, cmpps, pinsr/pextr, insertps, ...).
 * The trailing byte is part of the instruction, so the RIP-relative
 * displacement has to be computed one byte short -- the reference passes
 * extraRIPOffset=1 here (internal.h:111). */
#define E_SSE_RRI(p, pre, opcode, r1, r2, imm) do { \
        E_SSE_RR((p), (pre), (opcode), (r1), (r2)); \
        EW8((p), (uint8_t)(imm)); } while (0)

/* Width-aware forms. PINSRQ and PEXTRQ take a 64-bit GPR, so REX.W is set
 * from the operand rather than always clear as E_REX_SSE assumes. */
#define E_SSE_RRI_W(p, pre, opcode, r1, r2, imm, w) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        { uint8_t rex_ = (uint8_t)(0x40 | ((w) ? 8 : 0) \
                | ((((r1) >= 8) ? 1 : 0) << 2) | (((r2) >= 8) ? 1 : 0)); \
          if (rex_ != 0x40) EW8((p), rex_); } \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_RR((p), (r1), (r2)); \
        EW8((p), (uint8_t)(imm)); } while (0)

/* register-register with explicit REX.W, no immediate */
#define E_SSE_RR_W(p, pre, opcode, r1, r2, w) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        { uint8_t rex_ = (uint8_t)(0x40 | ((w) ? 8 : 0) \
                | ((((r1) >= 8) ? 1 : 0) << 2) | (((r2) >= 8) ? 1 : 0)); \
          if (rex_ != 0x40) EW8((p), rex_); } \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_RR((p), (r1), (r2)); } while (0)

#define E_SSE_R_MEM_I_W(p, pre, opcode, r1, m, imm, w) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        E_REX_MEM((p), (w), (r1), (m)); \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_MEM((p), (r1), (m), 1); \
        EW8((p), (uint8_t)(imm)); } while (0)
#define E_SSE_R_MI(p, pre, opcode, r1, addr, imm) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        E_REX_SSE((p), (r1), 0); \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_ABS((p), (r1), (addr), 1); \
        EW8((p), (uint8_t)(imm)); } while (0)
#define E_SSE_R_MEMI(p, pre, opcode, r1, m, imm) do { \
        if (pre) EW8((p), (uint8_t)(pre)); \
        E_REX_MEM((p), 0, (r1), (m)); \
        E_SSE_OP((p), (opcode)); \
        E_MODRM_MEM((p), (r1), (m), 1); \
        EW8((p), (uint8_t)(imm)); } while (0)

/* ===================================================================
 * Batch 8: AVX.
 *
 * This fork emits only the two-byte VEX form (0xC5), via xOpWriteC5:
 *
 *   C5  [~R vvvv L pp]  opcode  ModRM/SIB
 *
 * where ~R is the inverted extension bit of the ModRM.reg register, vvvv is
 * the inverted id of the second source (all ones when absent), L is set for
 * 256-bit operands, and pp encodes the legacy prefix (none/66/F3/F2 -> 0/1/2/3).
 *
 * No REX is emitted, and the two-byte form carries no B or X bit, so an
 * extended base or index register cannot be expressed -- see the port log.
 *
 * `regfield` is the ModRM.reg register, `vvvv` the second source or E_NOREG,
 * `ymm` selects the 256-bit L bit.
 * =================================================================== */

#define E_VEX_PP(pre) ((pre) == 0xF2 ? 3 : (pre) == 0xF3 ? 2 : (pre) == 0x66 ? 1 : 0)

/* Emits the VEX prefix. The two-byte form (C5) carries only the inverted R
 * bit; when the rm operand needs X or B -- an extended register, base or
 * index -- the three-byte form (C4) is required, or the register silently
 * decodes as its low counterpart. mmmmm is always 1 (the 0F escape) and W is
 * always 0 for every caller in this emitter. */
#define E_VEX(p, pre, opcode, regfield, vvvv, ymm, needx, needb) do { \
        uint8_t nv_ = (uint8_t)((((vvvv) == E_NOREG) ? 0xF : (~(vvvv) & 0xF)) << 3); \
        uint8_t L_  = (uint8_t)((ymm) ? 4 : 0); \
        int  rx_ = ((((regfield) & 0x0F) > 7) ? 1 : 0); \
        if ((needx) || (needb)) { \
            EW8((p), 0xC4); \
            EW8((p), (uint8_t)((rx_ ? 0x00 : 0x80) | ((needx) ? 0x00 : 0x40) | \
                            ((needb) ? 0x00 : 0x20) | 0x01)); \
            EW8((p), (uint8_t)(nv_ | L_ | E_VEX_PP(pre))); \
        } else { \
            EW8((p), 0xC5); \
            EW8((p), (uint8_t)((rx_ ? 0x00 : 0x80) | nv_ | L_ | E_VEX_PP(pre))); \
        } \
        EW8((p), (uint8_t)(opcode)); } while (0)

/* three-operand register form: vop dst, src1, src2 */
#define E_VEX_RRR(p, pre, opcode, dst, src1, src2, ymm) do { \
        E_VEX((p), (pre), (opcode), (dst), (src1), (ymm), \
              0, ((((src2) & 0x0F) > 7) ? 1 : 0)); \
        E_MODRM_RR((p), (dst), (src2)); } while (0)

/* three-operand with an absolute memory source (no base or index) */
#define E_VEX_RRM(p, pre, opcode, dst, src1, addr, ymm) do { \
        E_VEX((p), (pre), (opcode), (dst), (src1), (ymm), 0, 0); \
        E_MODRM_ABS((p), (dst), (addr), 0); } while (0)

/* three-operand with a base+index*scale memory source */
#define E_VEX_RRMEM(p, pre, opcode, dst, src1, m, ymm) do { \
        int vx_ = ((m).index != E_NOREG && ((m).index & 0x0F) > 7) ? 1 : 0; \
        int vb_ = ((m).base  != E_NOREG && ((m).base  & 0x0F) > 7) ? 1 : 0; \
        if (!E_NEEDS_SIB(m)) { vb_ = vx_; vx_ = 0; } \
        E_VEX((p), (pre), (opcode), (dst), (src1), (ymm), vx_, vb_); \
        E_MODRM_MEM((p), (dst), (m), 0); } while (0)

/* two-operand move forms: vvvv is absent. Self-moves are elided, matching
 * xImplAVX_Move::operator()(reg, reg). */
#define E_VEX_RR(p, pre, opcode, dst, src, ymm) do { \
        if ((dst) != (src)) { \
            E_VEX_RRR((p), (pre), (opcode), (dst), E_NOREG, (src), (ymm)); \
        } } while (0)
#define E_VEX_RM(p, pre, opcode, dst, addr, ymm) \
        E_VEX_RRM((p), (pre), (opcode), (dst), E_NOREG, (addr), (ymm))

/* ===================================================================
 * Batch 9: the mov-immediate family.
 *
 * Found by auditing every direct xWrite site in the emitter for paths that do
 * not funnel through xOpWrite/xOpWrite0F/xOpWriteC5. Most turned out to be
 * trailing immediates after a helper call, already covered -- these three
 * were not.
 * =================================================================== */

/* mov r, imm  (xImpl_Mov::operator()(xRegisterInt, sptr, bool)).
 * Three outcomes: xor for a zero immediate when flags may be clobbered, the
 * B8+r accumulator form when the value fits in 32 bits or the register is
 * narrower, and C7 /0 for a sign-extended 64-bit store.
 * `w` is 1 for a 64-bit destination, `pf` is preserve_flags. */
/* mov r,imm at any operand size. The zero optimisation substitutes an XOR of
 * the *same width* -- 0x30 for 8-bit, not 0x31 -- and the B8+r accumulator
 * form is 0xb0+r with a one-byte immediate for 8-bit and takes the 0x66
 * prefix with a two-byte immediate for 16-bit (movs.cpp:74-90). */
#define E_MOV_RI_SZ(p, sz, pf, reg, imm) do { \
        if (!(pf) && (imm) == 0) { \
            /* The reference XORs to.GetNonWide(), so a 64-bit destination \
             * gets the 32-bit XOR -- it zeroes the whole register anyway and \
             * saves the REX byte. Only 8- and 16-bit keep their own width. */ \
            if ((sz) == 1) { E_G1_8_RR((p), 6, (reg), (reg)); } \
            else if ((sz) == 2) { E_G1_16_RR((p), 6, (reg), (reg)); } \
            else { E_G1_RR((p), 0, 6, (reg), (reg)); } \
        } else if ((sz) == 1) { \
            E_REX8((p), 0, (reg)); \
            EW8((p), (uint8_t)(0xb0 | ((reg) & 7))); \
            EW8((p), (uint8_t)(imm)); \
        } else if ((sz) == 2) { \
            E_P16(p); \
            E_REX((p), 0, 0, 0, (reg)); \
            EW8((p), (uint8_t)(0xb8 | ((reg) & 7))); \
            EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); \
        } else E_MOV_RI_W((p), ((sz) == 8), (pf), (reg), (imm)); } while (0)

#define E_MOV_RI_W(p, w, pf, reg, imm) do { \
        if (!(pf) && (imm) == 0) { \
            E_G1_RR((p), 0, 6, (reg), (reg)); \
        } else if ((imm) == (intptr_t)(uint32_t)(imm) || !(w)) { \
            E_REX((p), 0, 0, 0, (reg)); \
            EW8((p), (uint8_t)(0xb8 | ((reg) & 7))); \
            EW32((p), (uint32_t)(imm)); \
        } else if ((w) && (imm) != (intptr_t)(int32_t)(imm)) { \
            /* Outside [-2G, 4G): neither dword form can hold it, so movabs \
             * is the only correct encoding. The reference used to fall \
             * through to C7 /0 here and truncate; fixed upstream. */ \
            E_REX((p), 1, 0, 0, (reg)); \
            EW8((p), (uint8_t)(0xb8 | ((reg) & 7))); \
            EW32((p), (uint32_t)(imm)); \
            EW32((p), (uint32_t)(((uint64_t)(imm)) >> 32)); \
        } else { \
            E_REX((p), (w), 0, 0, (reg)); \
            EW8((p), 0xc7); E_MODRM_RR((p), 0, (reg)); \
            EW32((p), (uint32_t)(imm)); \
        } } while (0)
#define E_MOV_RI(p, w, pf, reg, imm) E_MOV_RI_W((p), (w), (pf), (reg), (imm))

/* mov r64, imm64 (xImpl_MovImm64). Falls back to the 32-bit path whenever the
 * value fits, so the full ten-byte movabs is emitted only when it must be. */
#define E_MOV64_RI(p, pf, reg, imm) do { \
        uint64_t i_ = (uint64_t)(imm); \
        if ((int64_t)(imm) == (int64_t)(uint32_t)(imm) || (int64_t)(imm) == (int64_t)(int32_t)(imm)) { \
            E_MOV_RI((p), 1, (pf), (reg), (intptr_t)(imm)); \
        } else { \
            E_REX((p), 1, 0, 0, (reg)); \
            EW8((p), (uint8_t)(0xb8 | ((reg) & 7))); \
            EW32((p), (uint32_t)i_); EW32((p), (uint32_t)(i_ >> 32)); \
        } } while (0)

/* mov [abs], imm at 8/16/32/64-bit width. The immediate is written at the
 * operand width, except the 64-bit form which sign-extends a dword, so the
 * RIP-relative displacement is short by that many bytes. */
#define E_MOV_M_I8(p, addr, imm) do { \
        EW8((p), 0xc6); E_MODRM_ABS((p), 0, (addr), 1); \
        EW8((p), (uint8_t)(imm)); } while (0)
#define E_MOV_M_I16(p, addr, imm) do { \
        E_P16(p); \
        EW8((p), 0xc7); E_MODRM_ABS((p), 0, (addr), 2); \
        EW8((p), (uint8_t)(imm)); EW8((p), (uint8_t)((imm) >> 8)); } while (0)
#define E_MOV_M_I64(p, addr, imm) do { \
        E_REX((p), 1, 0, 0, 0); \
        EW8((p), 0xc7); E_MODRM_ABS((p), 0, (addr), 4); \
        EW32((p), (uint32_t)(imm)); } while (0)

/* ===================================================================
 * Batch 10: the legacy layer and the remaining odd forms.
 *
 * legacy.cpp is described in its own comment as instructions "NOT been
 * implemented in the new emitter", but it is not dead: 67 call sites across
 * iFPU, iFPUd, iR3000A, iR3000Atables and iR5900MultDiv still use it. Unlike
 * the xJcc family these take a raw displacement rather than a target, and
 * return a pointer to the displacement field for later patching.
 * =================================================================== */

/* JMP8/Jcc8 rel8. `slot` receives the displacement byte's address, matching
 * the u8* these return. */
#define E_L_JMP8(p, to, slot) do { \
        EW8((p), 0xEB); (slot) = (p); EW8((p), (uint8_t)(to)); } while (0)
#define E_L_JCC8(p, cc, to, slot) do { \
        EW8((p), (uint8_t)(0x70 | (cc))); (slot) = (p); EW8((p), (uint8_t)(to)); } while (0)

/* JMP32/Jcc32 rel32. `slot` receives the dword's address. */
#define E_L_JMP32(p, to, slot) do { \
        EW8((p), 0xE9); (slot) = (p); EW32((p), (uint32_t)(to)); } while (0)
#define E_L_JCC32(p, cc, to, slot) do { \
        EW8((p), 0x0F); EW8((p), (uint8_t)(0x80 | (cc))); \
        (slot) = (p); EW32((p), (uint32_t)(to)); } while (0)

/* Raw ModRM, as legacy.cpp exposes it. */
#define E_L_MODRM(p, mod, reg, rm) \
        EW8((p), (uint8_t)(((mod) << 6) | ((reg) << 3) | (rm)))

/* push imm, with the 6A imm8 short form when it fits. */
#define E_PUSH_I(p, imm) do { \
        if (E_IS_S8(imm)) { EW8((p), 0x6a); EW8((p), (uint8_t)(imm)); } \
        else { EW8((p), 0x68); EW32((p), (uint32_t)(imm)); } } while (0)

/* push/pop through an absolute memory operand */
#define E_PUSH_M(p, addr) do { \
        EW8((p), 0xff); E_MODRM_ABS((p), 6, (addr), 0); } while (0)
#define E_POP_M(p, addr) do { \
        EW8((p), 0x8f); E_MODRM_ABS((p), 0, (addr), 0); } while (0)

/* inc/dec through an absolute memory operand */
#define E_INCDEC_M(p, dec, addr) do { \
        EW8((p), 0xff); E_MODRM_ABS((p), ((dec) ? 1 : 0), (addr), 0); } while (0)

#endif /* C89EMIT_H */
