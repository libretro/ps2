// SPDX-FileCopyrightText: lrps2 arm64 port
// SPDX-License-Identifier: LGPL-3.0+
//
// arm64 IOP (R3000A) recompiler -- Phase C.2b-4: branches + delay slots.
//
// Builds on the C.2b-1..3 code cache, dispatch, ALU and load/store translation.
// Blocks now also translate MIPS branches/jumps natively, each ending the block:
// the (always-executed) delay slot is emitted inline, the condition is evaluated
// from the pre-delay-slot register values, and psxRegs.pc is set to the taken
// target or the fall-through (bpc+8). iopEventTest() is called only on the taken
// path, matching the interpreter's doBranch(). Link (JAL/JALR/B*AL) writes pc+8
// before the delay slot. J (0x02) is native since C.38 except the module-import
// stub form (delay slot `li $zero, fn`), which stays on the interpreter for the
// HLE hook; any opcode not covered by EmitSimple/EmitBranch hands the rest of
// the basic block to the interpreter.
//
// Self-modifying code is handled by granular per-4KiB-page, mirror-normalised
// invalidation in recClearIOP (psxCpu->Clear is called on every IOP write).

#include "R3000A.h"
#include "common/Console.h"
#include "IopMem.h"
#include "IopHw.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>

#include "aarch64/macro-assembler-aarch64.h"


using namespace vixl::aarch64;

extern "C" void iopRunBasicBlock_arm64(void);

namespace
{
	typedef void (*BlockFn)(void);

	constexpr size_t kCodeCacheSize = 16 * 1024 * 1024;
	constexpr int    kMaxInsns      = 64;
	constexpr u32    kPageShift     = 12;

	u8*    s_code     = nullptr;
	size_t s_code_pos = 0;
	bool   s_ok       = false;

	// end = Norm(one past the last byte covered by NATIVE code), plus the
	// breaker word for interpreter-tail blocks (C.70): the tail itself reads
	// live memory, but the DECISION to hand off was made from the breaker word's
	// compile-time value, so a write to it must also invalidate the block.
	struct BlockRec { BlockFn fn; u32 end; };
	std::unordered_map<u32, BlockRec>         s_blocks;
	std::unordered_map<u32, std::vector<u32>> s_page;

	inline u32 Norm(u32 a) { return a & 0x1fffffff; }

	// Direct-mapped block cache for the 2 MB IOP RAM (hot region) + asm block-linking
	// (block-to-block tail-chaining), mirroring the EE recompiler. ROM/scratchpad
	// fall back to the hash.
	constexpr u32 kRamBytes = 0x00200000;

	// C.41: chained blocks keep the 80-byte frame (and x19) live and enter the
	// successor's body directly; only the return to recExecuteBlock pops the
	// frame. Prologue size is fixed: Sub + 5*Stp + exactly-4-instruction x19
	// materialization.
	constexpr u32 kChainEntryOffset = (1 + 5 + 4) * 4;
	constexpr u32 kRamWords = kRamBytes >> 2;
	BlockFn*      s_lut      = nullptr;
	inline bool InRam(u32 np) { return np < kRamBytes; }
	inline void LutClearAll() { if (s_lut) madvise(s_lut, (size_t)kRamWords * sizeof(BlockFn), MADV_DONTNEED); }

	// Word-granular "native code covers this RAM word" bitmap (64KB). Every IOP
	// RAM store lands in recClearIOP, so the common case (word with no compiled
	// code on it) must be a single bit test, not a hash lookup. Bits can go
	// stale after a block is erased; the slow path self-cleans them.
	u8 s_covered[kRamWords >> 3];
	inline void CoverRange(u32 ns, u32 ne)
	{
		for (u32 w = ns >> 2; w < ((ne + 3) >> 2); w++)
			s_covered[w >> 3] |= 1u << (w & 7);
	}

	bool VixlEmitSelfTest()
	{
		MacroAssembler masm;
		masm.Add(x0, x0, 1);
		masm.Ret();
		masm.FinalizeCode();
		vixl::CodeBuffer* buf = masm.GetBuffer();
		buf->SetExecutable();
		auto fn = buf->GetStartAddress<int64_t (*)(int64_t)>();
		const int64_t r = fn(41);
		buf->SetWritable();
		return r == 42;
	}

	// ---- C.32: block-local write-back GPR register cache (the C.27 pattern
	// from the EE rec, sized for the IOP). Guest GPRs are 32-bit, mirrored in
	// the callee-saved w22..w27 (x20/x21 stay reserved for the branch
	// condition/jr-target snapshots that live across the delay slot). Helper
	// C calls (iopMemRead/Write, iopEventTest) preserve x22..x27 (AAPCS64)
	// and never touch psxRegs.GPR, so there are no invalidation sites at all:
	// the only discipline is a write-back flush at every block exit (the
	// branch tail before StorePC/eventtest/epilogue -- the conditional form
	// computes pc with Csel and shares one exit, so there is no fork to
	// snapshot -- and the interpreter-handoff tail, whose callee does read
	// and write GPR memory).
	constexpr int kIopCacheSlots = 6; // w22..w27
	inline Register IopCacheW(int slot) { return WRegister(22 + slot); }
	struct IopRegCache
	{
		int8_t host_of[32];
		int8_t guest_of[kIopCacheSlots];
		bool   dirty[kIopCacheSlots];
		u8 rr;
		void Reset()
		{
			for (int i = 0; i < 32; i++) host_of[i] = -1;
			for (int i = 0; i < kIopCacheSlots; i++) { guest_of[i] = -1; dirty[i] = false; }
			rr = 0;
		}
		int Alloc(MacroAssembler& m, const Register& gpr, u32 g)
		{
			const int s = rr;
			rr = (u8)((rr + 1) % kIopCacheSlots);
			if (guest_of[s] >= 0)
			{
				if (dirty[s]) m.Str(IopCacheW(s), MemOperand(gpr, (u32)guest_of[s] * 4));
				host_of[(int)guest_of[s]] = -1;
				dirty[s] = false;
			}
			guest_of[s] = (int8_t)g;
			host_of[g] = (int8_t)s;
			return s;
		}
		void FlushDirty(MacroAssembler& m, const Register& gpr)
		{
			for (int s = 0; s < kIopCacheSlots; s++)
				if (guest_of[s] >= 0 && dirty[s])
				{
					m.Str(IopCacheW(s), MemOperand(gpr, (u32)guest_of[s] * 4));
					dirty[s] = false;
				}
		}
	};
	IopRegCache s_irc;
	inline void LoadGpr(MacroAssembler& m, const Register& wd, const Register& gpr, u32 idx)
	{
		if (idx == 0) { m.Mov(wd, 0); return; }
		int s = s_irc.host_of[idx];
		if (s >= 0) { m.Mov(wd, IopCacheW(s)); return; }
		m.Ldr(wd, MemOperand(gpr, idx * 4));
		s = s_irc.Alloc(m, gpr, idx);
		m.Mov(IopCacheW(s), wd);
	}
	inline void StoreGpr(MacroAssembler& m, const Register& ws, const Register& gpr, u32 idx)
	{
		if (idx == 0)
		{
			m.Str(ws, MemOperand(gpr, idx * 4));
			return;
		}
		int s = s_irc.host_of[idx];
		if (s < 0) s = s_irc.Alloc(m, gpr, idx);
		m.Mov(IopCacheW(s), ws);
		s_irc.dirty[s] = true;
	}

	// C.68: ADDI/ADD/SUB (psxADDI/psxADD/psxSUB drop the integer-overflow trap
	// and are byte-for-byte the U forms -- same call as EE C.42) and RFE (pure
	// Status bit shuffle, R3000AOpcodeTables.cpp psxRFE; re-enabling IEc has no
	// immediate side effect in the interpreter either, pending interrupts wait
	// for the next event test). Checked in BOTH EmitSimple and IsTranslatable
	// so block building and delay-slot inlining stay consistent.
	// Translate one side-effect-light instruction (ALU + aligned load/store) to
	// native AArch64. Returns false (emitting nothing) for control flow and
	// anything not covered. gpr (x19) is callee-saved so it survives mem helpers.
	bool EmitSimple(MacroAssembler& m, const Register& gpr, u32 insn)
	{
		const u32 op = insn >> 26;
		const u32 rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, rd = (insn >> 11) & 31, sa = (insn >> 6) & 31;
		const u32 funct = insn & 0x3f;
		const int32_t simm = (int16_t)(insn & 0xffff);
		const u32 zimm = insn & 0xffff;

		switch (op)
		{
			case 0x00:
				switch (funct)
				{
					case 0x00: if (rd) { LoadGpr(m, w0, gpr, rt); m.Lsl(w0, w0, sa); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x02: if (rd) { LoadGpr(m, w0, gpr, rt); m.Lsr(w0, w0, sa); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x03: if (rd) { LoadGpr(m, w0, gpr, rt); m.Asr(w0, w0, sa); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x04: if (rd) { LoadGpr(m, w0, gpr, rt); LoadGpr(m, w1, gpr, rs); m.Lsl(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x06: if (rd) { LoadGpr(m, w0, gpr, rt); LoadGpr(m, w1, gpr, rs); m.Lsr(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x07: if (rd) { LoadGpr(m, w0, gpr, rt); LoadGpr(m, w1, gpr, rs); m.Asr(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x20: PCSX2_FALLTHROUGH; // ADD == ADDU (C.68)
					case 0x21: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Add(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x22: PCSX2_FALLTHROUGH; // SUB == SUBU (C.68)
					case 0x23: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Sub(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x24: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.And(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x25: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Orr(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x26: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Eor(w0, w0, w1); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x27: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Orr(w0, w0, w1); m.Mvn(w0, w0); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x2a: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Cmp(w0, w1); m.Cset(w0, lt); StoreGpr(m, w0, gpr, rd); } return true;
					case 0x2b: if (rd) { LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt); m.Cmp(w0, w1); m.Cset(w0, lo); StoreGpr(m, w0, gpr, rd); } return true;
					// ---- C.33: HI/LO pipeline. hi=GPR.r[32], lo=GPR.r[33]
					// (offsets 128/132); accessed directly, never through the
					// register cache (host_of[] only spans guest 0..31).
					case 0x10: if (rd) { m.Ldr(w0, MemOperand(gpr, 32 * 4)); StoreGpr(m, w0, gpr, rd); } return true; // MFHI
					case 0x11: { LoadGpr(m, w0, gpr, rs); m.Str(w0, MemOperand(gpr, 32 * 4)); } return true;          // MTHI
					case 0x12: if (rd) { m.Ldr(w0, MemOperand(gpr, 33 * 4)); StoreGpr(m, w0, gpr, rd); } return true; // MFLO
					case 0x13: { LoadGpr(m, w0, gpr, rs); m.Str(w0, MemOperand(gpr, 33 * 4)); } return true;          // MTLO
					case 0x18: // MULT
					case 0x19: // MULTU
					{
						LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt);
						if (funct == 0x18) m.Smull(x0, w0, w1);
						else               m.Umull(x0, w0, w1);
						m.Str(w0, MemOperand(gpr, 33 * 4)); // lo
						m.Lsr(x0, x0, 32);
						m.Str(w0, MemOperand(gpr, 32 * 4)); // hi
						return true;
					}
					case 0x1a: // DIV (psxDIV semantics)
					{
						LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt);
						m.Sdiv(w2, w0, w1);      // rt==0 -> 0 (fixed below); INT_MIN/-1 -> INT_MIN (= psxDIV's x86-overflow lo)
						m.Msub(w3, w2, w1, w0);  // hi = rs - lo*rt; rt==0 -> rs, overflow -> 0: both match psxDIV
						m.Cmp(w0, 0);
						m.Mov(w4, 1);
						m.Mov(w5, -1);
						m.Csel(w4, w4, w5, lt);  // div-by-0 lo: rs<0 ? 1 : 0xFFFFFFFF
						m.Cmp(w1, 0);
						m.Csel(w2, w2, w4, ne);
						m.Str(w2, MemOperand(gpr, 33 * 4)); // lo
						m.Str(w3, MemOperand(gpr, 32 * 4)); // hi
						return true;
					}
					case 0x1b: // DIVU (psxDIVU semantics)
					{
						LoadGpr(m, w0, gpr, rs); LoadGpr(m, w1, gpr, rt);
						m.Udiv(w2, w0, w1);
						m.Msub(w3, w2, w1, w0);  // rt==0 -> hi=rs
						m.Cmp(w1, 0);
						m.Csinv(w2, w2, wzr, ne); // rt==0 -> lo=0xFFFFFFFF
						m.Str(w2, MemOperand(gpr, 33 * 4)); // lo
						m.Str(w3, MemOperand(gpr, 32 * 4)); // hi
						return true;
					}
					default: return false;
				}
			case 0x08: PCSX2_FALLTHROUGH; // ADDI == ADDIU (C.68)
			case 0x09: if (rt) { LoadGpr(m, w0, gpr, rs); m.Add(w0, w0, simm); StoreGpr(m, w0, gpr, rt); } return true;
			case 0x0a: if (rt) { LoadGpr(m, w0, gpr, rs); m.Mov(w1, (u32)simm); m.Cmp(w0, w1); m.Cset(w0, lt); StoreGpr(m, w0, gpr, rt); } return true;
			case 0x0b: if (rt) { LoadGpr(m, w0, gpr, rs); m.Mov(w1, (u32)simm); m.Cmp(w0, w1); m.Cset(w0, lo); StoreGpr(m, w0, gpr, rt); } return true;
			case 0x0c: if (rt) { LoadGpr(m, w0, gpr, rs); m.Mov(w1, zimm); m.And(w0, w0, w1); StoreGpr(m, w0, gpr, rt); } return true;
			case 0x0d: if (rt) { LoadGpr(m, w0, gpr, rs); m.Mov(w1, zimm); m.Orr(w0, w0, w1); StoreGpr(m, w0, gpr, rt); } return true;
			case 0x0e: if (rt) { LoadGpr(m, w0, gpr, rs); m.Mov(w1, zimm); m.Eor(w0, w0, w1); StoreGpr(m, w0, gpr, rt); } return true;
			case 0x0f: if (rt) { m.Mov(w0, zimm << 16); StoreGpr(m, w0, gpr, rt); } return true;

			case 0x20: case 0x24: case 0x21: case 0x25: case 0x23:
			{
				LoadGpr(m, w0, gpr, rs); m.Add(w0, w0, simm);
				const uint64_t fn = (op == 0x23) ? reinterpret_cast<uint64_t>(&iopMemRead32)
				            : (op == 0x21 || op == 0x25) ? reinterpret_cast<uint64_t>(&iopMemRead16)
				            : reinterpret_cast<uint64_t>(&iopMemRead8);
				// C.36 fastmem: IOP main RAM is the first 8MB of physical
				// space (2MB mirrored 4x into one contiguous host buffer),
				// and iopMemRead* only does LUT bookkeeping to reach it — the
				// top runtime hotspot after the C.35 event gate (iopMemRead32
				// 6.8% self). Inline the RAM read and keep the helper call
				// for everything else (HW/SIF/ROM/unmapped). iopMem->Main is
				// stable for the process lifetime, so it embeds as a code
				// constant. The read happens even for rt==0 (IO side effects
				// on the slow path), matching the interpreter.
				const bool fast = iopMem != nullptr;
				Label slow, done;
				if (fast)
				{
					m.And(w1, w0, 0x1fffffff);           // physical address
					m.Cmp(w1, 0x00800000);
					m.B(&slow, hs);
					m.And(w1, w1, 0x1fffff);             // 2MB mirror fold
					m.Mov(x2, reinterpret_cast<uint64_t>(iopMem->Main));
					switch (op)
					{
						case 0x20: m.Ldrsb(w0, MemOperand(x2, x1)); break;
						case 0x24: m.Ldrb (w0, MemOperand(x2, x1)); break;
						case 0x21: m.Ldrsh(w0, MemOperand(x2, x1)); break;
						case 0x25: m.Ldrh (w0, MemOperand(x2, x1)); break;
						default:   m.Ldr  (w0, MemOperand(x2, x1)); break;
					}
					m.B(&done);
					m.Bind(&slow);
				}
				m.Mov(x16, fn); m.Blr(x16);
				switch (op) // helpers return u8/u16; normalize like the fast path
				{
					case 0x20: m.Sxtb(w0, w0); break;
					case 0x24: m.Uxtb(w0, w0); break;
					case 0x21: m.Sxth(w0, w0); break;
					case 0x25: m.Uxth(w0, w0); break;
					default: break;
				}
				if (fast)
					m.Bind(&done);
				if (rt)
					StoreGpr(m, w0, gpr, rt);
				return true;
			}
			case 0x28: case 0x29: case 0x2b:
			{
				LoadGpr(m, w0, gpr, rs); m.Add(w0, w0, simm);
				LoadGpr(m, w1, gpr, rt);
				uint64_t fn = (op == 0x2b) ? reinterpret_cast<uint64_t>(&iopMemWrite32)
				            : (op == 0x29) ? reinterpret_cast<uint64_t>(&iopMemWrite16)
				            : reinterpret_cast<uint64_t>(&iopMemWrite8);
				m.Mov(x16, fn); m.Blr(x16);
				return true;
			}

			// ---- C.34: unaligned LWL/LWR (psxLWL/psxLWR semantics). The
			// aligned-word read happens even for rt==0 (IO side effects),
			// matching the interpreter. shift = (vaddr&3)*8 and old-rt are
			// recomputed/reloaded AFTER the helper call (rs and the register
			// cache survive it; rt isn't written until the very end, so the
			// rs==rt aliasing case is safe). All shift amounts are <=24, so
			// LSLV/LSRV mod-32 semantics never bite.
			case 0x22: case 0x26:
			{
				LoadGpr(m, w0, gpr, rs); m.Add(w0, w0, simm);
				m.And(w0, w0, 0xfffffffc);
				m.Mov(x16, reinterpret_cast<uint64_t>(&iopMemRead32)); m.Blr(x16);
				if (!rt) return true;
				LoadGpr(m, w1, gpr, rs); m.Add(w1, w1, simm);
				m.And(w1, w1, 3); m.Lsl(w1, w1, 3); // shift
				LoadGpr(m, w2, gpr, rt);            // old rt
				if (op == 0x22) // LWL: rt = (rt & (0x00ffffff >> shift)) | (mem << (24-shift))
				{
					m.Mov(w3, 0x00ffffffu); m.Lsr(w3, w3, w1); m.And(w2, w2, w3);
					m.Mov(w4, 24); m.Sub(w4, w4, w1); m.Lsl(w0, w0, w4);
				}
				else // LWR: rt = (rt & (0xffffff00 << (24-shift))) | (mem >> shift)
				{
					m.Mov(w3, 0xffffff00u); m.Mov(w4, 24); m.Sub(w4, w4, w1);
					m.Lsl(w3, w3, w4); m.And(w2, w2, w3);
					m.Lsr(w0, w0, w1);
				}
				m.Orr(w0, w0, w2);
				StoreGpr(m, w0, gpr, rt);
				return true;
			}

			// ---- C.34: unaligned SWL/SWR (read-modify-write of the aligned
			// word, psxSWL/psxSWR semantics; two helper calls).
			case 0x2a: case 0x2e:
			{
				LoadGpr(m, w0, gpr, rs); m.Add(w0, w0, simm);
				m.And(w0, w0, 0xfffffffc);
				m.Mov(x16, reinterpret_cast<uint64_t>(&iopMemRead32)); m.Blr(x16);
				LoadGpr(m, w1, gpr, rs); m.Add(w1, w1, simm);
				m.And(w1, w1, 3); m.Lsl(w1, w1, 3); // shift
				LoadGpr(m, w2, gpr, rt);
				if (op == 0x2a) // SWL: mem' = (rt >> (24-shift)) | (mem & (0xffffff00 << shift))
				{
					m.Mov(w3, 24); m.Sub(w3, w3, w1); m.Lsr(w2, w2, w3);
					m.Mov(w4, 0xffffff00u); m.Lsl(w4, w4, w1); m.And(w0, w0, w4);
				}
				else // SWR: mem' = (rt << shift) | (mem & (0x00ffffff >> (24-shift)))
				{
					m.Lsl(w2, w2, w1);
					m.Mov(w3, 24); m.Sub(w3, w3, w1);
					m.Mov(w4, 0x00ffffffu); m.Lsr(w4, w4, w3); m.And(w0, w0, w4);
				}
				m.Orr(w1, w0, w2); // value
				LoadGpr(m, w0, gpr, rs); m.Add(w0, w0, simm);
				m.And(w0, w0, 0xfffffffc);
				m.Mov(x16, reinterpret_cast<uint64_t>(&iopMemWrite32)); m.Blr(x16);
				return true;
			}

			// ---- C.34: COP0 moves. psxMFC0/CFC0/MTC0/CTC0 are plain copies
			// against psxRegs.CP0.r[rd] (offset 136 + rd*4 from the GPR base;
			// no side effects anywhere in this interpreter, not even for
			// Status/Cause). RFE (rs=0x10) stays on the interpreter.
			case 0x10:
			{
				const u32 rs_field = rs;
				if (rs_field == 0x00 || rs_field == 0x02) // MFC0/CFC0
				{
					if (rt) { m.Ldr(w0, MemOperand(gpr, 136 + rd * 4)); StoreGpr(m, w0, gpr, rt); }
					return true;
				}
				if (rs_field == 0x04 || rs_field == 0x06) // MTC0/CTC0
				{
					LoadGpr(m, w0, gpr, rt);
					m.Str(w0, MemOperand(gpr, 136 + rd * 4));
					return true;
				}
				// C.68: RFE. psxCOP0 dispatches on the rs field alone (psxCP0[16]
				// = psxRFE for any funct), so match the same way:
				// Status = (Status & ~0xf) | ((Status & 0x3c) >> 2), Status = CP0.r[12].
				if (rs_field == 0x10)
				{
					m.Ldr(w0, MemOperand(gpr, 136 + 12 * 4));
					m.And(w1, w0, 0x3c);
					m.And(w0, w0, 0xfffffff0);
					m.Orr(w0, w0, Operand(w1, LSR, 2));
					m.Str(w0, MemOperand(gpr, 136 + 12 * 4));
					return true;
				}
				return false;
			}
			default: return false;
		}
	}

	// True iff EmitSimple would translate this instruction (no emission). Used to
	// decide whether a branch's delay slot can be inlined.
	bool IsTranslatable(u32 insn)
	{
		const u32 op = insn >> 26, funct = insn & 0x3f;
		if (op == 0x00)
		{
			switch (funct)
			{
				case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
				case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
				case 0x2a: case 0x2b:
				case 0x10: case 0x11: case 0x12: case 0x13: // C.33 HI/LO moves
				case 0x18: case 0x19: case 0x1a: case 0x1b: // C.33 mult/div
					return true;
				case 0x20: case 0x22: return true; // C.68 ADD/SUB (no trap)
				default: return false;
			}
		}
		if (op == 0x10) // C.34 COP0 moves + C.68 RFE
		{
			const u32 rs_field = (insn >> 21) & 31;
			if (rs_field == 0x10)
				return true;
			return rs_field == 0x00 || rs_field == 0x02 || rs_field == 0x04 || rs_field == 0x06;
		}
		switch (op)
		{
			case 0x08: return true; // C.68 ADDI (no trap)
			case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
			case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
			case 0x28: case 0x29: case 0x2b:
			case 0x22: case 0x26: case 0x2a: case 0x2e: return true; // C.34 LWL/LWR/SWL/SWR
			default: return false;
		}
	}

	// C.46: every psxRegisters field lives a short, constant distance from the
	// guest-reg base already pinned in x19, so reach them through it instead of
	// materializing a 64-bit absolute address (4 instructions) per access. The
	// block tail touches pc/cycle/iopCycleEE/iopNextEventCycle several times
	// each, so this is most of the tail's code size. Offsets are well inside the
	// ldr/str immediate window; if one ever isn't, the MacroAssembler falls back
	// to a scratch register on its own, so this stays correct by construction.
	inline MemOperand RegsField(const void* field)
	{
		const intptr_t off = reinterpret_cast<intptr_t>(field) -
		                     reinterpret_cast<intptr_t>(&psxRegs.GPR.r[0]);
		return MemOperand(x19, static_cast<int64_t>(off));
	}

	// cycle += count ; iopCycleEE -= (ICFG&8 ? 9 : 8) * count
	void EmitCycleBookkeeping(MacroAssembler& m, int count)
	{
		m.Ldr(w0, RegsField(&psxRegs.cycle)); m.Add(w0, w0, count); m.Str(w0, RegsField(&psxRegs.cycle));
		m.Mov(x10, reinterpret_cast<uint64_t>(&psxHu32(HW_ICFG)));
		m.Ldr(w1, MemOperand(x10));
		m.Mov(w2, 9 * count); m.Mov(w3, 8 * count);
		m.Tst(w1, 8);
		m.Csel(w2, w2, w3, ne);
		m.Ldr(w0, RegsField(&psxRegs.iopCycleEE)); m.Sub(w0, w0, w2); m.Str(w0, RegsField(&psxRegs.iopCycleEE));
	}

	// Restore the frame, then tail-chain straight to the next block via the RAM LUT
	// (staying in JIT code). IOP has a cycle budget, so only chain while
	// iopCycleEE > 0; otherwise (budget spent / LUT miss / non-RAM pc) return to
	// recExecuteBlock. x30 propagates so the final ret unwinds to the dispatcher.
	void EmitEpilogue(MacroAssembler& m)
	{
		// C.41: dispatch FIRST with the frame (and x19) still live; the
		// successor's chain entry skips its prologue. Only the return to
		// recExecuteBlock pops the frame.
		Label ret_path;
		m.Ldr(w0, RegsField(&psxRegs.iopCycleEE));
		m.Cmp(w0, 0);
		m.B(&ret_path, le);                  // budget exhausted -> return to dispatcher
		m.Ldr(w1, RegsField(&psxRegs.pc));
		m.And(w2, w1, 0x1fffffff);
		m.Mov(w3, kRamBytes);
		m.Cmp(w2, w3);
		m.B(&ret_path, hs);                  // not RAM -> return
		m.Mov(x0, reinterpret_cast<uint64_t>(&s_lut));
		m.Ldr(x0, MemOperand(x0));
		m.Lsr(w2, w2, 2);
		m.Ldr(x3, MemOperand(x0, w2, UXTW, 3));
		m.Cbz(x3, &ret_path);
		m.Add(x3, x3, kChainEntryOffset);    // skip the successor's prologue
		m.Br(x3);
		m.Bind(&ret_path);
		m.Ldp(x19, x20, MemOperand(sp, 0));
		m.Ldp(x21, x30, MemOperand(sp, 16));
		m.Ldp(x22, x23, MemOperand(sp, 32));
		m.Ldp(x24, x25, MemOperand(sp, 48));
		m.Ldp(x26, x27, MemOperand(sp, 64));
		m.Add(sp, sp, 80);
		m.Ret();
	}

	inline void StorePC(MacroAssembler& m, const Register& wsrc)
	{
		m.Str(wsrc, RegsField(&psxRegs.pc));
	}

	// Translate a branch/jump (ending the block). Returns false, emitting nothing,
	// if it can't be handled (then the caller hands the rest to the interpreter).
	bool EmitBranch(MacroAssembler& m, const Register& gpr, u32 bpc, u32 insn, int n_leading)
	{
		const u32 op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, funct = insn & 0x3f;
		const int32_t simm = (int16_t)(insn & 0xffff);

		bool uncond = false, is_jr = false, two = false, one = false;
		int  link = -1;
		Condition cond = al;
		u32  tconst = 0;

		if      (op == 0x00 && funct == 0x08) { uncond = true; is_jr = true; }
		else if (op == 0x00 && funct == 0x09) { uncond = true; is_jr = true; link = (int)((insn >> 11) & 31); }
		else if (op == 0x03) { uncond = true; link = 31; tconst = (((bpc + 4) & 0xf0000000) | ((insn & 0x3ffffff) << 2)); } // JAL
		else if (op == 0x02) { uncond = true; tconst = (((bpc + 4) & 0xf0000000) | ((insn & 0x3ffffff) << 2)); } // J (C.38; import-stub form filtered below)
		else if (op == 0x04) { two = true; cond = eq; tconst = bpc + 4 + simm * 4; } // BEQ
		else if (op == 0x05) { two = true; cond = ne; tconst = bpc + 4 + simm * 4; } // BNE
		else if (op == 0x06) { one = true; cond = le; tconst = bpc + 4 + simm * 4; } // BLEZ
		else if (op == 0x07) { one = true; cond = gt; tconst = bpc + 4 + simm * 4; } // BGTZ
		else if (op == 0x01)
		{
			const u32 t = bpc + 4 + simm * 4;
			if      (rt == 0x00) { one = true; cond = lt; tconst = t; }            // BLTZ
			else if (rt == 0x01) { one = true; cond = ge; tconst = t; }            // BGEZ
			else if (rt == 0x10) { one = true; cond = lt; tconst = t; link = 31; } // BLTZAL
			else if (rt == 0x11) { one = true; cond = ge; tconst = t; link = 31; } // BGEZAL
			else return false;
		}
		else return false;

		const u32 ds = iopMemRead32(bpc + 4);
		if (!IsTranslatable(ds))
			return false;

		// C.38: J was pinned to the interpreter wholesale for the IOP
		// module-import HLE, which dragged every J-heavy loop through the
		// interpreter (~9% of CPU time in the handoff cluster). The hook only
		// fires when the delay slot is the import-stub marker `li $zero, fn`
		// (psxJ checks `delayslot >> 16 == 0x2400`), and the delay slot is
		// compile-time known here — stores still go through iopMemWrite, so a
		// stub patched in later faults the page and recompiles. Only the
		// stub form stays on the interpreter.
		if (op == 0x02 && (ds >> 16) == 0x2400)
			return false;

		// C.37: gate the event test on cycle >= iopNextEventCycle, like the
		// upstream x86 IOP rec dispatcher (the interpreter tests on every
		// taken branch; mirroring that made iopEventTest 5.9% of CPU time).
		// The slice stays bounded by iopCycleEE, and the EE side's
		// _cpuEventTest_Shared still calls iopEventTest unconditionally.
		const auto EmitIopEventTest = [&m]()
		{
			Label skip;
			m.Ldr(w0, RegsField(&psxRegs.cycle));
			m.Ldr(w1, RegsField(&psxRegs.iopNextEventCycle));
			m.Subs(w0, w0, w1); // (s32)(cycle - iopNextEventCycle)
			m.B(&skip, mi);     // not due yet
			m.Mov(x16, reinterpret_cast<uint64_t>(&iopEventTest));
			m.Blr(x16);
			m.Bind(&skip);
		};

		// C.47: the IOP kernel idle loop is a block that is nothing but `j <self>`
		// with a nop delay slot, spinning until an interrupt arrives. It was the
		// single hottest block in the emulator (18% of all JIT time on MMX7):
		// every two guest cycles paid a full block tail plus a chain hop. The
		// loop has no side effects, so instead of running it an iteration at a
		// time, compute how many iterations would elapse before the next IOP
		// event or the end of the cycle budget, whichever comes first, and apply
		// them in one step. cycle and iopCycleEE land on exactly the values the
		// spin would have produced when the event test next fires, so the
		// emulated timing is unchanged -- this is not a speedhack.
		if (op == 0x02 && n_leading == 0 && ds == 0 && tconst == bpc)
		{
			// k = EE cycles charged per iteration = (ICFG & 8 ? 9 : 8) * 2 insns
			m.Mov(x10, reinterpret_cast<uint64_t>(&psxHu32(HW_ICFG)));
			m.Ldr(w5, MemOperand(x10));
			m.Mov(w6, 9 * 2); m.Mov(w7, 8 * 2);
			m.Tst(w5, 8);
			m.Csel(w5, w6, w7, ne);                       // w5 = k

			m.Ldr(w0, RegsField(&psxRegs.cycle));             // w0 = cycle
			m.Ldr(w1, RegsField(&psxRegs.iopNextEventCycle)); // w1 = next event
			m.Ldr(w4, RegsField(&psxRegs.iopCycleEE));        // w4 = budget (> 0 on entry)

			// iterations until the event gate fires: ceil((s32)(next - cycle) / 2),
			// which is <= 0 when the event is already due.
			m.Sub(w6, w1, w0);
			m.Add(w6, w6, 1);
			m.Asr(w6, w6, 1);

			// iterations until the budget is spent: ceil(budget / k). The epilogue
			// stops chaining once iopCycleEE <= 0, so this is where the spin ends.
			m.Add(w7, w4, w5);
			m.Sub(w7, w7, 1);
			m.Udiv(w7, w7, w5);

			// n = max(min(n_evt, n_bud), 1): always make at least one iteration of
			// progress, so a due-but-not-yet-tested event cannot livelock the loop.
			m.Cmp(w6, w7);
			m.Csel(w2, w6, w7, lt);
			m.Mov(w3, 1);
			m.Cmp(w2, 1);
			m.Csel(w2, w2, w3, gt);

			m.Add(w0, w0, Operand(w2, LSL, 1));           // cycle += 2n
			m.Str(w0, RegsField(&psxRegs.cycle));
			m.Mul(w3, w2, w5);                            // k * n
			m.Sub(w4, w4, w3);                            // iopCycleEE -= k*n
			m.Str(w4, RegsField(&psxRegs.iopCycleEE));

			m.Mov(w0, tconst);
			StorePC(m, w0);
			EmitIopEventTest();
			EmitEpilogue(m);
			return true;
		}

		// link (before the delay slot, unconditionally)
		if (link > 0) { m.Mov(w0, bpc + 8); StoreGpr(m, w0, gpr, (u32)link); }
		// snapshot condition operands / jr target before the delay slot can clobber them
		if (two)   { LoadGpr(m, w20, gpr, rs); LoadGpr(m, w21, gpr, rt); }
		else if (one) { LoadGpr(m, w20, gpr, rs); }
		if (is_jr) { LoadGpr(m, w21, gpr, rs); }
		// the always-executed delay slot
		EmitSimple(m, gpr, ds);
		// time: n_leading native ops + the branch + the delay slot
		EmitCycleBookkeeping(m, n_leading + 2);
		// Block exit: the chained successor reads GPR MEMORY; nothing after
		// this point (Csel/StorePC/iopEventTest/epilogue) touches guest GPRs,
		// and both conditional outcomes share this single exit path.
		s_irc.FlushDirty(m, gpr);

		if (uncond)
		{
			if (is_jr) StorePC(m, w21);
			else { m.Mov(w0, tconst); StorePC(m, w0); }
			EmitIopEventTest();
		}
		else
		{
			if (two) m.Cmp(w20, w21);
			else     m.Cmp(w20, 0);
			m.Mov(w0, tconst); m.Mov(w1, bpc + 8);
			m.Csel(w0, w0, w1, cond);
			StorePC(m, w0);
			Label not_taken;
			m.B(&not_taken, InvertCondition(cond));
			EmitIopEventTest();
			m.Bind(&not_taken);
		}
		EmitEpilogue(m);
		return true;
	}

	BlockFn CompileBlock(u32 pc)
	{
		if (s_code_pos + 8192 > kCodeCacheSize)
		{
			s_blocks.clear();
			s_page.clear();
			LutClearAll();
			memset(s_covered, 0, sizeof(s_covered));
			s_code_pos = 0;
		}

		u8* start = s_code + s_code_pos;
		MacroAssembler masm(start, kCodeCacheSize - s_code_pos, PositionDependentCode);
		s_irc.Reset(); // fresh per-block register-cache state (C.32)

		const Register gpr = x19;
		// Uniform 80-byte frame across all blocks (tail-chaining: any block's
		// epilogue must match any successor's prologue). x22..x27 = C.32 cache.
		masm.Sub(sp, sp, 80);
		masm.Stp(x19, x20, MemOperand(sp, 0));
		masm.Stp(x21, x30, MemOperand(sp, 16));
		masm.Stp(x22, x23, MemOperand(sp, 32));
		masm.Stp(x24, x25, MemOperand(sp, 48));
		masm.Stp(x26, x27, MemOperand(sp, 64));
		{
			// Exactly 4 instructions so kChainEntryOffset stays constant.
			const uint64_t gv = reinterpret_cast<uint64_t>(&psxRegs.GPR.r[0]);
			vixl::ExactAssemblyScope scope(&masm, 4 * kInstructionSize);
			masm.movz(gpr, gv & 0xffff);
			masm.movk(gpr, (gv >> 16) & 0xffff, 16);
			masm.movk(gpr, (gv >> 32) & 0xffff, 32);
			masm.movk(gpr, (gv >> 48) & 0xffff, 48);
		}

		u32 p = pc;
		int n = 0;
		bool done = false, branch_end = false;
		while (n < kMaxInsns)
		{
			const u32 insn = iopMemRead32(p);
			if (EmitSimple(masm, gpr, insn)) { p += 4; n++; continue; }
			if (EmitBranch(masm, gpr, p, insn, n)) { done = branch_end = true; break; } // emits bookkeeping + pc + epilogue
			break; // unsupported -> interpreter handles the rest of the block
		}

		// C.69: block ended only because n hit kMaxInsns with every op translated
		// and no branch yet (n == kMaxInsns <=> cap: a break always leaves
		// n <= kMaxInsns-1 since the body only runs while n < kMaxInsns). Mirror
		// the EE's C.44 cap-chain: end the block cleanly at p and chain, instead
		// of handing a fully-translatable tail to the interpreter (LW/ADDIU
		// sitting at position 64 were the residual IOP handoff breakers).
		if (!done && n == kMaxInsns)
		{
			masm.Mov(w0, p);
			StorePC(masm, w0);
			EmitCycleBookkeeping(masm, n);
			s_irc.FlushDirty(masm, gpr);
			EmitEpilogue(masm);
			done = true;
		}

		if (!done)
		{
			if (n > 0)
			{
				masm.Ldr(w0, RegsField(&psxRegs.pc)); masm.Add(w0, w0, 4 * n); masm.Str(w0, RegsField(&psxRegs.pc));
				EmitCycleBookkeeping(masm, n);
			}
			// The interpreter tail reads AND writes guest GPR memory.
			s_irc.FlushDirty(masm, gpr);
			masm.Mov(x16, reinterpret_cast<uint64_t>(&iopRunBasicBlock_arm64));
			masm.Blr(x16);
			s_irc.Reset();
			EmitEpilogue(masm);
		}

		masm.FinalizeCode();
		const size_t sz = masm.GetSizeOfCodeGenerated();
		__builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(start + sz));
		s_code_pos += (sz + 15) & ~size_t(15);

		BlockFn fn = reinterpret_cast<BlockFn>(start);
		// Native code covers the leading run; a translated branch also bakes the
		// branch + delay slot (p still points at the branch instruction there).
		// A cap-chained block (C.69) covers exactly [pc, p) -- no branch baked.
		const u32 ns = Norm(pc), ne = Norm(branch_end ? p + 8 : p);
		// C.70: an interpreter-tail block's shape is decided by what the breaker
		// word held at COMPILE time. If the guest later rewrites that word (IRX
		// module load landing over what was data), the block must recompile to
		// pick up the now-translatable code -- so cover the breaker word too.
		// (MMX7: a block at 0x16480 compiled before the module loaded kept
		// re-handing `lw ra` to the interpreter forever.)
		const u32 cov = (!done && InRam(ne)) ? ne + 4 : ne;

		s_blocks.emplace(pc, BlockRec{fn, cov});
		if (InRam(ns)) s_lut[ns >> 2] = fn;
		if (cov > ns)
		{
			for (u32 pg = ns >> kPageShift; pg <= (cov - 1) >> kPageShift; pg++)
				s_page[pg].push_back(pc);
			if (InRam(cov - 1))
				CoverRange(ns, cov);
		}
		return fn;
	}

	inline BlockFn BlockForPC(u32 pc)
	{
		const u32 np = Norm(pc);
		if (InRam(np))
		{
			BlockFn f = s_lut[np >> 2];
			return f ? f : CompileBlock(pc);
		}
		auto it = s_blocks.find(pc);
		return it != s_blocks.end() ? it->second.fn : CompileBlock(pc);
	}
}

static void recReserve(void)
{
	s_ok = VixlEmitSelfTest();
	if (!s_code)
	{
		s_code = (u8*)mmap(nullptr, kCodeCacheSize, PROT_READ | PROT_WRITE | PROT_EXEC,
		                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (s_code == MAP_FAILED) s_code = nullptr;
	}
	if (!s_lut)
	{
		s_lut = (BlockFn*)mmap(nullptr, (size_t)kRamWords * sizeof(BlockFn), PROT_READ | PROT_WRITE,
		                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (s_lut == MAP_FAILED) s_lut = nullptr;
	}
	s_ok = s_ok && s_code && s_lut;
	Console.WriteLn("arm64 IOP rec (C.2b-4): %s.", s_ok ? "native ALU+mem+branch JIT active" : "FAILED -> interpreter fallback");
}

static void recResetIOP(void)
{
	s_blocks.clear();
	s_page.clear();
	LutClearAll();
	memset(s_covered, 0, sizeof(s_covered));
	s_code_pos = 0;
}

// Erase only the blocks whose NATIVE range [Norm(pc), end) intersects the
// (inclusive) byte range [a0, a1]. Page lists keep the survivors; entries for
// blocks already erased via a neighbouring page are dropped lazily.
static void EraseBlocksInRange(u32 a0, u32 a1)
{
	for (u32 pg = a0 >> kPageShift; pg <= a1 >> kPageShift; pg++)
	{
		auto it = s_page.find(pg);
		if (it == s_page.end())
			continue;
		auto& vec = it->second;
		size_t out = 0;
		for (size_t i = 0; i < vec.size(); i++)
		{
			const u32 spc = vec[i];
			auto bit = s_blocks.find(spc);
			if (bit == s_blocks.end())
				continue; // stale (erased through another page it spanned)
			const u32 ns = Norm(spc), ne = bit->second.end;
			if (ns <= a1 && ne > a0)
			{
				s_blocks.erase(bit);
				if (InRam(ns)) s_lut[ns >> 2] = nullptr;
				continue;
			}
			vec[out++] = spc;
		}
		if (out == 0)
			s_page.erase(it);
		else
			vec.resize(out);
	}
}

static void recClearIOP(u32 addr, u32 size)
{
	const u32 a0 = Norm(addr);
	const u32 a1 = Norm(addr + (size ? size : 1) * 4 - 1);
	if (a0 <= a1 && a1 < kRamBytes)
	{
		// RAM fast path (every IOP store lands here, typically 1 word with no
		// code on it). Ranges are whole words: callers pass addr&~3 + word
		// counts, and block bounds are 4-aligned, so word-level tests match
		// the byte-level intersection exactly.
		bool covered = false;
		for (u32 w = a0 >> 2; w <= (a1 >> 2) && !covered; w++)
			covered = (s_covered[w >> 3] >> (w & 7)) & 1;
		if (!covered)
			return;
		EraseBlocksInRange(a0, a1);
		// Nothing overlaps [a0, a1] anymore -- clear the bits so the next
		// store to these words takes the fast path again (self-cleans bits
		// left stale by blocks erased through other words).
		for (u32 w = a0 >> 2; w <= (a1 >> 2); w++)
			s_covered[w >> 3] &= ~(1u << (w & 7));
		return;
	}
	if (s_blocks.empty())
		return;
	EraseBlocksInRange(a0, a1);
}

static s32 recExecuteBlock(s32 eeCycles)
{
	// If the recompiler failed to initialize, run the IOP interpreter.
	if (!s_ok)
		return psxInt.ExecuteBlock(eeCycles);

	psxRegs.iopBreak   = 0;
	psxRegs.iopCycleEE = eeCycles;
	while (psxRegs.iopCycleEE > 0)
		BlockForPC(psxRegs.pc)();
	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

static void recShutdown(void)
{
	s_blocks.clear();
	s_page.clear();
	memset(s_covered, 0, sizeof(s_covered));
	if (s_code) { munmap(s_code, kCodeCacheSize); s_code = nullptr; }
	s_code_pos = 0;
}

R3000Acpu psxRec = {
	recReserve,
	recResetIOP,
	recExecuteBlock,
	recClearIOP,
	recShutdown,
};

