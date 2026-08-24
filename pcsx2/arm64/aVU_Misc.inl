// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>

// ARM64 microVU — misc emit helpers (Phase 7, task 7.5). VIXL port of the
// emit-coupled tail of pcsx2/x86/microVU_Misc.inl.
//
// Already ported elsewhere:
//   * the arch-neutral reg shufflers (mVUunpack_xyzw/mVUloadReg/mVUsaveReg/
//     mVUmergeRegs) — aVU_IR.h (task 7.2b / 7.4 part 1b);
//   * the branch/T-Bit/E-Bit/waitMTVU C helpers — aVU.cpp.
//
// Deferred:
//   * mVUoptimizeConstantAddr — its return-type contract (how a constant host
//     address is handed to a load/store op) is defined by its only consumer, the
//     Lower load/store handlers in task 7.5b.

// Computes the destination PC (byte address) of a relative VU branch from the
// current lower-op PC + the signed 11-bit immediate. x86: microVU_Misc.inl
// branchAddr. Pure (no emit) — used by the branch op handlers (B/BAL/IBxx) and
// the branch drivers in aVU_Branch.inl.
static inline u32 branchAddr(const mV)
{
	pxAssumeMsg(islowerOP, "MicroVU: Expected Lower OP code for valid branch addr.");
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU.progMemMask) * 4);
}

//------------------------------------------------------------------
// Small absolute-address memory-access helpers (shared by Tables/Flags/Branch)
//------------------------------------------------------------------
// x86 folded an absolute &global into a ptr32/ptr128 memory operand; ARM64 must
// first materialize the address. armMoveAddressToReg clobbers RSCRATCH (x16/x17),
// so the value regs passed in must never be x16/x17 (callers use w9/w10 or the
// flag GPRs / NEON scratch).

static inline void mvuStr32(microVU& mVU, const void* addr, const a64::Register& wreg)
{
	armAsm->Str(wreg.W(), mvuAbsMem(mVU, addr, 4));
}

static inline void mvuLdr32(microVU& mVU, const a64::Register& wreg, const void* addr)
{
	armAsm->Ldr(wreg.W(), mvuAbsMem(mVU, addr, 4));
}

static inline void mvuStrImm32(microVU& mVU, const void* addr, u32 imm, const a64::Register& tmp)
{
	armAsm->Mov(tmp.W(), imm);
	mvuStr32(mVU, addr, tmp);
}

static inline void mvuStrSS(microVU& mVU, const void* addr, const a64::VRegister& vreg)
{
	armAsm->Str(vreg.S(), mvuAbsMem(mVU, addr, 4));
}

static inline void mvuLdrSS(microVU& mVU, const a64::VRegister& vreg, const void* addr)
{
	armAsm->Ldr(vreg.S(), mvuAbsMem(mVU, addr, 4));
}

static inline void mvuLdrQ(microVU& mVU, const a64::VRegister& vreg, const void* addr)
{
	armAsm->Ldr(vreg.Q(), mvuAbsMem(mVU, addr, 16));
}

static inline void mvuStrQ(microVU& mVU, const void* addr, const a64::VRegister& vreg)
{
	armAsm->Str(vreg.Q(), mvuAbsMem(mVU, addr, 16));
}

static inline void mvuMemAndImm32(microVU& mVU, const void* addr, u32 imm, const a64::Register& tmp)
{
	const a64::MemOperand m = mvuAbsMem(mVU, addr, 4);
	armAsm->Ldr(tmp.W(), m);
	armAsm->And(tmp.W(), tmp.W(), imm);
	armAsm->Str(tmp.W(), m);
}

static inline void mvuMemOrImm32(microVU& mVU, const void* addr, u32 imm, const a64::Register& tmp)
{
	const a64::MemOperand m = mvuAbsMem(mVU, addr, 4);
	armAsm->Ldr(tmp.W(), m);
	armAsm->Orr(tmp.W(), tmp.W(), imm);
	armAsm->Str(tmp.W(), m);
}

//------------------------------------------------------------------
// Volatile-register backup / restore around opaque C calls
//------------------------------------------------------------------
// x86: microVU_Misc.inl mVUbackupRegs/mVUrestoreRegs.
//   * !toMemory (the common case, e.g. normJumpCompile): flush the reg cache and
//     stash only the PQ NEON reg in mVU.vecBackup — the dispatcher's XGKICK resume
//     path (mVUdispatcherCD) reloads it from there.
//   * toMemory (debug-only: handleBadOp / mVUdebugPrintBlocks / DumpVUState): the
//     regAlloc isn't told about the call, so push every caller-saved host reg that
//     can hold live VU state to the stack and pop it after. We save the full fixed
//     caller-saved set (GPR x0-x15, NEON v0-v24 = VF pool + PQ) rather than the x86
//     "onlyNeeded" subset — over-saving is correct and these paths are rare. The
//     frame layout here MUST match mVUrestoreRegs.
static constexpr int kBakGprSave = 16; // x0..x15
static constexpr int kBakVecSave = 25; // v0..v24 (VF pool + PQ)
static constexpr int kBakGprBytes = kBakGprSave * 8;
static constexpr int kBakFrame = kBakGprBytes + ((kBakVecSave * 16 + 15) & ~15);

__fi void mVUbackupRegs(microVU& mVU, bool toMemory = false, bool onlyNeeded = false)
{
	(void)onlyNeeded;
	if (toMemory)
	{
		armAsm->Sub(a64::sp, a64::sp, kBakFrame);
		for (int i = 0; i < kBakGprSave; i += 2)
			armAsm->Stp(armXRegister(i), armXRegister(i + 1), a64::MemOperand(a64::sp, i * 8));
		int voff = kBakGprBytes;
		for (int i = 0; i < kBakVecSave - 1; i += 2, voff += 32)
			armAsm->Stp(armQRegister(i), armQRegister(i + 1), a64::MemOperand(a64::sp, voff));
		armAsm->Str(armQRegister(kBakVecSave - 1), a64::MemOperand(a64::sp, voff));
	}
	else
	{
		mVU.regAlloc->flushAll(); // Flush Regalloc
		armAsm->Str(mVU_xmmPQ.Q(), mvuAbsMem(mVU, &mVU.vecBackup[mVU_xmmPQ.GetCode()][0], 16));
	}
}

__fi void mVUrestoreRegs(microVU& mVU, bool fromMemory = false, bool onlyNeeded = false)
{
	(void)onlyNeeded;
	if (fromMemory)
	{
		int voff = kBakGprBytes;
		for (int i = 0; i < kBakVecSave - 1; i += 2, voff += 32)
			armAsm->Ldp(armQRegister(i), armQRegister(i + 1), a64::MemOperand(a64::sp, voff));
		armAsm->Ldr(armQRegister(kBakVecSave - 1), a64::MemOperand(a64::sp, voff));
		for (int i = 0; i < kBakGprSave; i += 2)
			armAsm->Ldp(armXRegister(i), armXRegister(i + 1), a64::MemOperand(a64::sp, i * 8));
		armAsm->Add(a64::sp, a64::sp, kBakFrame);
	}
	else
	{
		armAsm->Ldr(mVU_xmmPQ.Q(), mvuAbsMem(mVU, &mVU.vecBackup[mVU_xmmPQ.GetCode()][0], 16));
	}
}

// Transforms the VU address (a quadword index) in gprReg into a byte offset into
// VU memory, applying the VU0/VU1 wrap and the VU0->VU1 register-window remap.
// x86: mVUaddrFix. Modifies gprReg in place (and tmpReg on the far-offset path).
// gprReg/tmpReg are 64-bit (X) registers; the masking steps use the 32-bit view
// (W-writes zero bits [63:32], matching the x86 32-bit AND).
__fi void mVUaddrFix(mV, const a64::Register& gprReg, const a64::Register& tmpReg)
{
	if (isVU1)
	{
		armAsm->And(gprReg.W(), gprReg.W(), 0x3ff); // wrap around (1024 quadwords)
		armAsm->Lsl(gprReg.W(), gprReg.W(), 4);     // * 16 -> byte offset
	}
	else
	{
		a64::Label jmpA, jmpB;
		armAsm->Tst(gprReg.W(), 0x400);
		armAsm->B(&jmpA, a64::ne); // if (addr & 0x400): reads VU1's VF/VI regs
			armAsm->And(gprReg.W(), gprReg.W(), 0xff); // else wrap (256 quadwords)
			armAsm->B(&jmpB);
		armAsm->Bind(&jmpA);
			if (THREAD_VU1)
				armEmitCall(reinterpret_cast<const void*>(mVU.waitMTVU));
			armAsm->And(gprReg.W(), gprReg.W(), 0x3f); // ToDo: VU0 may override VU1's VF0/VI0
			const sptr offset = (u128*)VU1.VF - (u128*)VU0.Mem;
			if (offset == static_cast<s32>(offset))
			{
				armAsm->Add(gprReg, gprReg, static_cast<s32>(offset));
			}
			else
			{
				armAsm->Mov(tmpReg, offset);
				armAsm->Add(gprReg, gprReg, tmpReg);
			}
		armAsm->Bind(&jmpB);
		armAsm->Lsl(gprReg, gprReg, 4); // * 16 -> byte offset (64-bit)
	}
}

// If the VU address source register is the always-zero VI0, the effective VU
// memory address is a compile-time constant — return the absolute host pointer
// so the load/store handler can skip the runtime moveVIToGPR + mVUaddrFix dance.
// x86 returned an xAddressVoid (a folded ptr operand); ARM64 has no implicit
// memory operands, so the contract is "absolute host address, materialized by
// the caller via armMoveAddressToReg". std::nullopt ⇒ must compute at runtime.
__fi std::optional<const void*> mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	/* VI const prop, as the old comment here requested: a known
	 * source register folds to an absolute address exactly like vi00
	 * always has, replicating the runtime sum and masking bit for
	 * bit - zero-extended sixteen-bit register plus sign-extended
	 * immediate under the same wrap. */
	if (srcreg != 0 && !(doViConstProp && srcreg < 16 && mVUconstReg[srcreg].isValid))
		return std::nullopt;

	const s32 addr = (s32)(srcreg ? (u32)(u16)mVUconstReg[srcreg].regValue : 0u) + offset;
	if (isVU1)
	{
		return (const void*)(mVU.regs().Mem + ((addr & 0x3FFu) << 4) + offsetSS_);
	}
	else
	{
		if (addr & 0x400)
			return std::nullopt;

		return (const void*)(mVU.regs().Mem + ((addr & 0xFFu) << 4) + offsetSS_);
	}
}

//------------------------------------------------------------------
// Micro VU - Custom SSE Instructions (x86: microVU_Misc.inl SSE_*)
//------------------------------------------------------------------
// VIXL port of the VU FMAC arithmetic primitives. The VU's MIN/MAX are NOT IEEE
// min/max — they're a signed-magnitude *integer* compare on the float bit pattern,
// so MIN_MAX_PS uses the integer-comparison path (the x86 `if (0)` double path is
// dropped). MIN_MAX_SS keeps the double-precision trick (it has no integer form):
// the two float lanes are packed into a finite normal double whose ordering matches
// the float magnitude/sign ordering, so plain FMIN/FMAX on .V2D() is exact (the
// constructed doubles are never NaN, so NEON's NaN-propagation is never hit).
//
// The add/sub/mul/div primitives go through mVUclampedArith: when the extra-overflow
// gamefix (clampE) is on, operands are sign-clamped before and the result range-
// clamped after. clampE is off by default, so it normally emits just the NEON op.

alignas(16) static const u32 mVU_MIN_MAX_1[4] = {0xffffffff, 0x80000000, 0xffffffff, 0x80000000};
alignas(16) static const u32 mVU_MIN_MAX_2[4] = {0x00000000, 0x40000000, 0x00000000, 0x40000000};
alignas(16) static const u32 mVU_ADD_SS[4]    = {0x80000000, 0xffffffff, 0xffffffff, 0xffffffff};

// Warning: Modifies t1 and t2
static void MIN_MAX_PS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1in, const a64::VRegister& t2in, bool min)
{
	const bool t1b = t1in.IsNone();
	const bool t2b = t2in.IsNone();
	const a64::VRegister t1 = t1b ? mVU.regAlloc->allocReg() : t1in;
	const a64::VRegister t2 = t2b ? mVU.regAlloc->allocReg() : t2in;

	// integer comparison (signed-magnitude transform of the float bit pattern)
	const a64::VRegister& c1 = min ? t2 : t1;
	const a64::VRegister& c2 = min ? t1 : t2;

	armAsm->Mov (t1.V16B(), to.V16B());
	armAsm->Sshr(t1.V4S(), t1.V4S(), 31);
	armAsm->Ushr(t1.V4S(), t1.V4S(), 1);
	armAsm->Eor (t1.V16B(), t1.V16B(), to.V16B());

	armAsm->Mov (t2.V16B(), from.V16B());
	armAsm->Sshr(t2.V4S(), t2.V4S(), 31);
	armAsm->Ushr(t2.V4S(), t2.V4S(), 1);
	armAsm->Eor (t2.V16B(), t2.V16B(), from.V16B());

	armAsm->Cmgt(c1.V4S(), c1.V4S(), c2.V4S());      // c1 = (c1 > c2) ? -1 : 0 (signed)
	armAsm->And (to.V16B(), to.V16B(), c1.V16B());
	armAsm->Bic (c1.V16B(), from.V16B(), c1.V16B()); // c1 = from & ~c1 (x86 PANDN)
	armAsm->Orr (to.V16B(), to.V16B(), c1.V16B());

	if (t1b) mVU.regAlloc->clearNeeded(t1);
	if (t2b) mVU.regAlloc->clearNeeded(t2);
}

// Warning: Modifies to's upper 3 vectors, and t1
static void MIN_MAX_SS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1in, bool min)
{
	const bool t1b = t1in.IsNone();
	const a64::VRegister t1 = t1b ? mVU.regAlloc->allocReg() : t1in;

	// to = { to0, to0, from0, from0 }  (x86 xSHUF.PS(to, from, 0))
	armAsm->Ins(to.V4S(), 1, to.V4S(), 0);
	armAsm->Ins(to.V4S(), 2, from.V4S(), 0);
	armAsm->Ins(to.V4S(), 3, from.V4S(), 0);

	mvuLdrQ(mVU, RQSCRATCH, mVU_MIN_MAX_1);
	armAsm->And(to.V16B(), to.V16B(), RQSCRATCH.V16B());
	mvuLdrQ(mVU, RQSCRATCH, mVU_MIN_MAX_2);
	armAsm->Orr(to.V16B(), to.V16B(), RQSCRATCH.V16B());

	mVUshufflePS(t1, to, 0xee); // t1 = { to2, to3, to2, to3 }
	if (min) armAsm->Fmin(to.V2D(), to.V2D(), t1.V2D());
	else     armAsm->Fmax(to.V2D(), to.V2D(), t1.V2D());

	if (t1b) mVU.regAlloc->clearNeeded(t1);
}


// Exponent-distance operand masking for VU ADD/SUB, fused single-keep
// schedule (x86: mVUmaskAddSubFused, whose proofs this port shares: the
// fused algebra measured identical to the reference over 80.7 million
// lanes, and the x86 twin runs bit-identical to the two-pass emission
// it replaces in the emitted-bytes differential). NEON makes it short:
// native Abs, three-operand forms, per-lane Ushl for the keep mask with
// out-of-range counts collapsing safely under the sign-OR, and the
// zero-immediate compare forms are exactly the side tests. Twenty-one
// instructions against the two-pass thirty-two, no recompute.
// Masks a in place and leaves the masked copy of b in vc; b is not
// modified. vm and vs are scratch, as is v0 (free here, like the
// scalar path's use of v2 established).
/* vf0 identities, the aVU twins of the x86 specializations, same
 * oracle proofs: a broadcast add of +0.0 is the value except zero-
 * exponent inputs which pack to +0.0 (subtract keeps the sign), and
 * a multiply by vf0.w is the value under the same zero-exponent
 * select while the x, y and z lanes give the sign alone. The NEON
 * forms lean on two tricks: an all-ones compare mask shifted right
 * once is exactly the magnitude mask the sign-preserving cases
 * need, and Bic folds the select into one op. Fixed scratch
 * registers, zero allocator pressure. Broadcast forms only - the
 * census puts the whole measured class there. */
static void mVUaddSubVF0(microVU& mVU, const a64::VRegister& reg, bool isSub, bool isSS)
{
	const a64::VRegister& T = RQSCRATCH;
	armAsm->Shl (T.V4S(), reg.V4S(), 1);
	armAsm->Ushr(T.V4S(), T.V4S(), 24);
	armAsm->Cmeq(T.V4S(), T.V4S(), 0);
	if (isSub)
		armAsm->Ushr(T.V4S(), T.V4S(), 1);
	if (isSS)
	{
		const a64::VRegister& U = RQSCRATCH2;
		armAsm->Bic(U.V16B(), reg.V16B(), T.V16B());
		armAsm->Ins(reg.V4S(), 0, U.V4S(), 0);
	}
	else
		armAsm->Bic(reg.V16B(), reg.V16B(), T.V16B());
}

static bool mVUexactMulVF0(microVU& mVU, const a64::VRegister& reg, int opCase, bool isSS)
{
	if (opCase != 2 || _Ft_ != 0)
		return false;
	const a64::VRegister& T = RQSCRATCH;
	if (_bc_ != 3)
	{
		armAsm->Movi(T.V4S(), 0x80, a64::LSL, 24);
		if (isSS)
		{
			const a64::VRegister& U = RQSCRATCH2;
			armAsm->And(U.V16B(), reg.V16B(), T.V16B());
			armAsm->Ins(reg.V4S(), 0, U.V4S(), 0);
		}
		else
			armAsm->And(reg.V16B(), reg.V16B(), T.V16B());
		return true;
	}
	armAsm->Shl (T.V4S(), reg.V4S(), 1);
	armAsm->Ushr(T.V4S(), T.V4S(), 24);
	armAsm->Cmeq(T.V4S(), T.V4S(), 0);
	armAsm->Ushr(T.V4S(), T.V4S(), 1);
	if (isSS)
	{
		const a64::VRegister& U = RQSCRATCH2;
		armAsm->Bic(U.V16B(), reg.V16B(), T.V16B());
		armAsm->Ins(reg.V4S(), 0, U.V4S(), 0);
	}
	else
		armAsm->Bic(reg.V16B(), reg.V16B(), T.V16B());
	return true;
}

static void mVUmaskAddSubPS(mV, const a64::VRegister& a, const a64::VRegister& b,
	const a64::VRegister& vc, const a64::VRegister& vm, const a64::VRegister& vs)
{
	armAsm->Shl (vc.V4S(), a.V4S(), 1);
	armAsm->Ushr(vc.V4S(), vc.V4S(), 24);
	armAsm->Shl (vs.V4S(), b.V4S(), 1);
	armAsm->Ushr(vs.V4S(), vs.V4S(), 24);
	armAsm->Sub (vc.V4S(), vc.V4S(), vs.V4S());     // d
	armAsm->Abs (vs.V4S(), vc.V4S());
	armAsm->Movi(vm.V4S(), 1);
	armAsm->Sub (vs.V4S(), vs.V4S(), vm.V4S());     // c = |d| - 1
	armAsm->Movi(vm.V16B(), 0xff);
	armAsm->Ushl(vm.V4S(), vm.V4S(), vs.V4S());     // keep = ~0 << c
	armAsm->Orr (vm.V4S(), 0x80, 24);               // keep' (sign survives garbage)
	armAsm->Movi(a64::v0.V4S(), 24);
	armAsm->Cmgt(a64::v0.V4S(), a64::v0.V4S(), vs.V4S()); // 24 > c
	armAsm->Orr (a64::v0.V4S(), 0x80, 24);
	armAsm->And (vm.V16B(), vm.V16B(), a64::v0.V16B());   // M
	armAsm->Cmge(a64::v0.V4S(), vc.V4S(), 0);       // d >= 0: a side inactive
	armAsm->Orr (a64::v0.V16B(), a64::v0.V16B(), vm.V16B());
	armAsm->And (a.V16B(), a.V16B(), a64::v0.V16B());
	armAsm->Cmle(a64::v0.V4S(), vc.V4S(), 0);       // d <= 0: b side inactive
	armAsm->Orr (a64::v0.V16B(), a64::v0.V16B(), vm.V16B());
	armAsm->And (vc.V16B(), b.V16B(), a64::v0.V16B());    // masked b copy
}


// to (op)= from, sign-clamping operands and range-clamping the result when the
// extra-overflow gamefix is enabled (x86: clampOp macro). isPS selects 4-lane vs
// single-scalar. t1 is the caller-provided clamp scratch (xEmptyReg ⇒ RQSCRATCH).
enum mVUarithOp { mVU_ADD_OP, mVU_SUB_OP, mVU_MUL_OP, mVU_DIV_OP };

static void mVUclampedArith(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1, int op, bool isPS)
{
	const a64::VRegister ct = t1.IsNone() ? RQSCRATCH : t1;
	const int xyzw = isPS ? 0xf : 0x8;
	const bool isAddSub = (op == mVU_ADD_OP) || (op == mVU_SUB_OP);
	mVUclamp3(mVU, to, ct, xyzw);
	mVUclamp3(mVU, from, ct, xyzw);
	if (isPS)
	{
		if (isAddSub && CHECK_VU_ACC_ADDSUB)
		{
			// the fused mask leaves the masked copy in RQSCRATCH and
			// never touches the cached source register.
			mVUmaskAddSubPS(mVU, to, from, RQSCRATCH, RQSCRATCH2, RQSCRATCH3);
			if (op == mVU_SUB_OP) armAsm->Fsub(to.V4S(), to.V4S(), RQSCRATCH.V4S());
			else                  armAsm->Fadd(to.V4S(), to.V4S(), RQSCRATCH.V4S());
			mVUclamp4(mVU, to, ct, xyzw);
			return;
		}
		switch (op)
		{
			case mVU_ADD_OP: armAsm->Fadd(to.V4S(), to.V4S(), from.V4S()); break;
			case mVU_SUB_OP: armAsm->Fsub(to.V4S(), to.V4S(), from.V4S()); break;
			case mVU_MUL_OP: armAsm->Fmul(to.V4S(), to.V4S(), from.V4S()); break;
			case mVU_DIV_OP: armAsm->Fdiv(to.V4S(), to.V4S(), from.V4S()); break;
		}
	}
	else if (isAddSub && CHECK_VU_ACC_ADDSUB)
	{
		// Lane 0 result only, through the same fused vector mask: compute
		// on a copy of the destination so its upper lanes survive. No
		// GPRs anywhere - the x86 rework's GPR scalar path broke
		// rendering through a mechanism never identified, and the
		// empirical boundary applies here too.
		armAsm->Mov(a64::v1.V16B(), to.V16B());
		mVUmaskAddSubPS(mVU, a64::v1, from, RQSCRATCH, RQSCRATCH2, RQSCRATCH3);
		if (op == mVU_SUB_OP) armAsm->Fsub(a64::v1.S(), a64::v1.S(), RQSCRATCH.S());
		else                  armAsm->Fadd(a64::v1.S(), a64::v1.S(), RQSCRATCH.S());
		/* not ct: with no caller scratch ct aliases RQSCRATCH */
		mVUclamp4(mVU, a64::v1, RQSCRATCH2, 0x8);
		armAsm->Ins(to.V4S(), 0, a64::v1.V4S(), 0);
		return;
	}
	else
	{
		// AArch64 scalar FP ops write the result to Sd and ZERO the upper bits
		// [127:32] of the V register — unlike x86 ADDSS/MULSS, which preserve the
		// upper 3 lanes. The microVU single-scalar (_XYZW_SS) model shuffles the
		// target lane into lane0, operates, then shuffles back, and DEPENDS on the
		// other lanes surviving (e.g. mVU_FMACb's MADDAw.z accumulates into ACC.z
		// while ACC.x/.y must be preserved). Writing `to.S()` in place would wipe
		// them. Compute into a scratch and insert only lane0 of `to`.
		const a64::VRegister sres = RQSCRATCH;
		switch (op)
		{
			case mVU_ADD_OP: armAsm->Fadd(sres.S(), to.S(), from.S()); break;
			case mVU_SUB_OP: armAsm->Fsub(sres.S(), to.S(), from.S()); break;
			case mVU_MUL_OP: armAsm->Fmul(sres.S(), to.S(), from.S()); break;
			case mVU_DIV_OP: armAsm->Fdiv(sres.S(), to.S(), from.S()); break;
		}
		armAsm->Ins(to.V4S(), 0, sres.V4S(), 0);
	}
	mVUclamp4(mVU, to, ct, xyzw);
}

static void SSE_MAXPS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { MIN_MAX_PS(mVU, to, from, t1, t2, false); }
static void SSE_MINPS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { MIN_MAX_PS(mVU, to, from, t1, t2, true); }
static void SSE_MAXSS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { MIN_MAX_SS(mVU, to, from, t1, false); }
static void SSE_MINSS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { MIN_MAX_SS(mVU, to, from, t1, true); }
/* The TriAce ADDi hack is gone: the exponent-distance mask reproduces the
 * discard it approximated, for every distance. */
static void SSE_ADD2SS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg)
{
	mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, false);
}
static void SSE_ADD2PS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, true); }
static void SSE_ADDPS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, true); }
static void SSE_ADDSS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, false); }
static void SSE_SUBPS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_SUB_OP, true); }
static void SSE_SUBSS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_SUB_OP, false); }

//------------------------------------------------------------------
// Hardware-exact multiply (x86: mVUexactMulPS). Same architecture:
// the Booth correction can reach the kept mantissa only when the
// product's bits 22:15 are all zero - about one lane in 256 - so the
// fast path is the widening products, that guard, and a chop-pack;
// a tripped guard drops the quad to the scalar soft model through
// the per-VU buffer. NEON compresses the x86 sequence considerably:
// Umull pairs make the products, Xtn narrows the low words for a
// constant-free guard (shift up, compare against zero), Shrn extracts
// the mantissas, the carry-normalize is one per-lane Ushl by negated
// counts, and Bic with immediates does the field masking.
// Scratch: v0, v1, q29-q31; from is not modified; dst may equal to.
//------------------------------------------------------------------

static void mVUexactMulPS(mV, const a64::VRegister& dst, const a64::VRegister& to, const a64::VRegister& from)
{
	const a64::VRegister vA = a64::v0;
	const a64::VRegister vB = a64::v1;
	const a64::VRegister vC = RQSCRATCH;
	const a64::VRegister vD = RQSCRATCH2;
	const a64::VRegister vE = RQSCRATCH3;
	u32* buf = mVU.exactMulBuf;
	a64::Label slow, pack, done;

	// mantissas with implicit bit: vA = ma, vB = mb
	armAsm->Movi(vC.V4S(), 0x80, a64::LSL, 16);   // implicit
	armAsm->Movi(vD.V4S(), 1);
	armAsm->Sub (vD.V4S(), vC.V4S(), vD.V4S());   // mantissa mask
	armAsm->And (vA.V16B(), to.V16B(), vD.V16B());
	armAsm->Orr (vA.V16B(), vA.V16B(), vC.V16B());
	armAsm->And (vB.V16B(), from.V16B(), vD.V16B());
	armAsm->Orr (vB.V16B(), vB.V16B(), vC.V16B());
	// mantissas for the tree stub's slow path
	armAsm->Str(vA, armAbsMemOperand(RSCRATCHADDR, &buf[0], 128));
	armAsm->Str(vB, armAbsMemOperand(RSCRATCHADDR, &buf[4], 128));

	// products: vC = p01 (lanes 0,1), vA = p23
	armAsm->Umull (vC.V2D(), vA.V2S(), vB.V2S());
	armAsm->Umull2(vA.V2D(), vA.V4S(), vB.V4S());

	// guard: low words narrowed, bits 22:15 tested constant-free
	armAsm->Xtn (vB.V2S(), vC.V2D());
	armAsm->Xtn2(vB.V4S(), vA.V2D());
	armAsm->Ushr(vB.V4S(), vB.V4S(), 15);
	armAsm->Shl (vB.V4S(), vB.V4S(), 24);
	armAsm->Cmeq(vB.V4S(), vB.V4S(), 0);
	/* zero-operand lanes are decided by the final select whatever the
	 * product says; with the implicit bit forced on, 0.0 times anything
	 * zeroes the guard field, and game data is full of zeros. */
	armAsm->Shl (vD.V4S(), to.V4S(), 1);
	armAsm->Ushr(vD.V4S(), vD.V4S(), 24);
	armAsm->Cmeq(vD.V4S(), vD.V4S(), 0);
	armAsm->Shl (vE.V4S(), from.V4S(), 1);
	armAsm->Ushr(vE.V4S(), vE.V4S(), 24);
	armAsm->Cmeq(vE.V4S(), vE.V4S(), 0);
	armAsm->Orr (vD.V16B(), vD.V16B(), vE.V16B());
	/* sixteen or more trailing zeros in the recoded mantissa empty the
	 * whole low half of the recode, so the tree is exact and the fast
	 * path already right - oracle-proven at that threshold with zero
	 * violations and measurably unsound below it (the partial-source
	 * side is not sound at any threshold and is not excluded). */
	armAsm->Movi(vE.V4S(), 0xff, a64::LSL, 8);
	armAsm->Orr (vE.V4S(), 0xff);
	armAsm->And (vE.V16B(), from.V16B(), vE.V16B());
	armAsm->Cmeq(vE.V4S(), vE.V4S(), 0);
	armAsm->Orr (vD.V16B(), vD.V16B(), vE.V16B());
	armAsm->Bic (vB.V16B(), vB.V16B(), vD.V16B());
	armAsm->Umaxv(a64::s2, vB.V4S());             // v2 low used transiently
	armAsm->Fmov(gprT1, a64::s2);
	armAsm->Cbnz(gprT1, &slow);

	// ---- shared pack (fast path falls in; slow path branches back) ----
	armAsm->Bind(&pack);
	// rm merged: (p >> 23) narrowed into 4S
	armAsm->Shrn (vB.V2S(), vC.V2D(), 23);
	armAsm->Shrn2(vB.V4S(), vA.V2D(), 23);
	// carry-normalize with a per-lane negative shift
	armAsm->Ushr(vC.V4S(), vB.V4S(), 24);         // carry
	armAsm->Neg (vD.V4S(), vC.V4S());
	armAsm->Ushl(vB.V4S(), vB.V4S(), vD.V4S());
	// e = expA + expB - 127 + carry
	armAsm->Shl (vD.V4S(), to.V4S(), 1);
	armAsm->Ushr(vD.V4S(), vD.V4S(), 24);
	armAsm->Add (vD.V4S(), vD.V4S(), vC.V4S());
	armAsm->Shl (vC.V4S(), from.V4S(), 1);
	armAsm->Ushr(vC.V4S(), vC.V4S(), 24);
	armAsm->Add (vD.V4S(), vD.V4S(), vC.V4S());
	armAsm->Movi(vC.V4S(), 127);
	armAsm->Sub (vD.V4S(), vD.V4S(), vC.V4S());   // e
	// sign
	armAsm->Eor (vE.V16B(), to.V16B(), from.V16B());
	armAsm->Ushr(vE.V4S(), vE.V4S(), 31);
	armAsm->Shl (vE.V4S(), vE.V4S(), 31);         // sign
	// normal result in vA
	armAsm->Bic (vB.V4S(), 0xff, 24);            // rm &= 0x007fffff
	armAsm->Bic (vB.V4S(), 0x80, 16);
	armAsm->Shl (vA.V4S(), vD.V4S(), 23);
	armAsm->Orr (vA.V16B(), vA.V16B(), vB.V16B());
	armAsm->Orr (vA.V16B(), vA.V16B(), vE.V16B());
	// overflow: e > 255 -> sign | 0x7fffffff
	armAsm->Movi(vC.V4S(), 255);
	armAsm->Cmgt(vC.V4S(), vD.V4S(), vC.V4S());   // mo
	armAsm->Mvni(vB.V4S(), 0x80, a64::LSL, 24);   // 0x7fffffff
	armAsm->Orr (vB.V16B(), vB.V16B(), vE.V16B());
	armAsm->Bsl (vC.V16B(), vB.V16B(), vA.V16B()); // mo ? clamp : normal
	// underflow (e < 1) or a zero-exponent operand -> sign
	armAsm->Cmlt(vD.V4S(), vD.V4S(), 0);          // e <= 0
	armAsm->Shl (vB.V4S(), to.V4S(), 1);
	armAsm->Ushr(vB.V4S(), vB.V4S(), 24);
	armAsm->Cmeq(vB.V4S(), vB.V4S(), 0);
	armAsm->Orr (vD.V16B(), vD.V16B(), vB.V16B());
	armAsm->Shl (vB.V4S(), from.V4S(), 1);
	armAsm->Ushr(vB.V4S(), vB.V4S(), 24);
	armAsm->Cmeq(vB.V4S(), vB.V4S(), 0);
	armAsm->Orr (vD.V16B(), vD.V16B(), vB.V16B());
	armAsm->Bsl (vD.V16B(), vE.V16B(), vC.V16B()); // mzu ? sign : selected
	armAsm->Mov (dst.V16B(), vD.V16B());
	armAsm->B   (&done);

	// ---- slow path: hand the products to the tree stub, take the
	// corrected ones back, and rejoin the pack. The stub saves every
	// vector register itself, so nothing here needs a flush. ----
	armAsm->Bind(&slow);
	armAsm->Str(vC, armAbsMemOperand(RSCRATCHADDR, &buf[8], 128));
	armAsm->Str(vA, armAbsMemOperand(RSCRATCHADDR, &buf[12], 128));
	armEmitCall(reinterpret_cast<const void*>(mVU.exactMulStub));
	armAsm->Ldr(vC, armAbsMemOperand(RSCRATCHADDR, &buf[8], 128));
	armAsm->Ldr(vA, armAbsMemOperand(RSCRATCHADDR, &buf[12], 128));
	armAsm->B(&pack);
	armAsm->Bind(&done);
}

static void SSE_MULPS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg)
{
	if (CHECK_VU_EXACTMUL)
	{
		mVUexactMulPS(mVU, to, to, from);
		return;
	}
	mVUclampedArith(mVU, to, from, t1, mVU_MUL_OP, true);
}
static void SSE_MULSS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg)
{
	if (CHECK_VU_EXACTMUL)
	{
		/* lane 0 through the same pipeline into a scratch, then insert:
		 * the upper lanes of the destination must survive. v2 is free
		 * here alongside the pipeline's own scratch. */
		mVUexactMulPS(mVU, a64::v2, to, from);
		armAsm->Ins(to.V4S(), 0, a64::v2.V4S(), 0);
		return;
	}
	mVUclampedArith(mVU, to, from, t1, mVU_MUL_OP, false);
}
static void SSE_DIVPS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_DIV_OP, true); }
static void SSE_DIVSS(mV, const a64::VRegister& to, const a64::VRegister& from, const a64::VRegister& t1 = xEmptyReg, const a64::VRegister& t2 = xEmptyReg) { mVUclampedArith(mVU, to, from, t1, mVU_DIV_OP, false); }
