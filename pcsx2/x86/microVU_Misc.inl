#include "common/emitter/c89ops.h"
/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

//------------------------------------------------------------------
// Micro VU - Reg Loading/Saving/Shuffling/Unpacking/Merging...
//------------------------------------------------------------------

void mVUunpack_xyzw(int dstreg, int srcreg, int xyzw)
{
	switch (xyzw)
	{
		case 0: xe_pshufd_xxi(dstreg, srcreg, 0x00); break; // XXXX
		case 1: xe_pshufd_xxi(dstreg, srcreg, 0x55); break; // YYYY
		case 2: xe_pshufd_xxi(dstreg, srcreg, 0xaa); break; // ZZZZ
		case 3: xe_pshufd_xxi(dstreg, srcreg, 0xff); break; // WWWW
	}
}

void mVUloadReg(int reg, struct e_mem ptr, int xyzw)
{
	switch (xyzw)
	{
		case 8:  { struct e_mem xm = e_mem_off(ptr, 0); xe_movss_xmemg(reg, xm); } break; // X
		case 4:  { struct e_mem xm = e_mem_off(ptr, 4); xe_movss_xmemg(reg, xm); } break; // Y
		case 2:  { struct e_mem xm = e_mem_off(ptr, 8); xe_movss_xmemg(reg, xm); } break; // Z
		case 1:  { struct e_mem xm = e_mem_off(ptr, 12); xe_movss_xmemg(reg, xm); } break; // W
		default: { struct e_mem xm = e_mem_off(ptr, 0); xe_movaps_xmemg(reg, xm); } break;
	}
}

// Modifies the Source Reg!
void mVUsaveReg(int reg, struct e_mem ptr, int xyzw, int modXYZW)
{
	switch (xyzw)
	{
		case 5: // YW
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_extractps_memxi(xm, reg, 1); }
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_extractps_memxi(xm, reg, 3); }
			break;
		case 6: // YZ
			xe_pshufd_xxi(reg, reg, 0xc9);
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_movlps_memxg(xm, reg); }
			break;
		case 7: // YZW
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movhps_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_extractps_memxi(xm, reg, 1); }
			break;
		case 9: // XW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_extractps_memxi(xm, reg, 3); }
			break;
		case 10: // XZ
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_extractps_memxi(xm, reg, 2); }
			break;
		case 11: // XZW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movhps_memxg(xm, reg); }
			break;
		case 13: // XYW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movlps_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_extractps_memxi(xm, reg, 3); }
			break;
		case 14: // XYZ
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movlps_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_extractps_memxi(xm, reg, 2); }
			break;
		case 4: // Y
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 1);
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_movss_memxg(xm, reg); }
			break;
		case 2: // Z
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 2);
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movss_memxg(xm, reg); }
			break;
		case 1: // W
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 3);
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_movss_memxg(xm, reg); }
			break;
		case 8: // X
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			break;
		case 12: // XY
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movlps_memxg(xm, reg); }
			break;
		case 3: // ZW
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movhps_memxg(xm, reg); }
			break;
		default: // XYZW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movaps_memxg(xm, reg); }
			break;
	}
}

// Modifies the Source Reg! (ToDo: Optimize modXYZW = 1 cases)
void mVUmergeRegs(int dest, int src, int xyzw, int modXYZW)
{
	xyzw &= 0xf;
	if ((dest != src) && (xyzw != 0))
	{
		if (xyzw == 0x8)
			xe_movss_xx(dest, src);
		else if (xyzw == 0xf)
			xe_movaps_xx(dest, src);
		else
		{
			if (modXYZW)
			{
				if      (xyzw == 1) { xe_insertps_xxi(dest, src, _MM_MK_INSERTPS_NDX(0, 3, 0)); return; }
				else if (xyzw == 2) { xe_insertps_xxi(dest, src, _MM_MK_INSERTPS_NDX(0, 2, 0)); return; }
				else if (xyzw == 4) { xe_insertps_xxi(dest, src, _MM_MK_INSERTPS_NDX(0, 1, 0)); return; }
			}
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);
			xe_blendps_xxi(dest, src, xyzw);
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Misc Functions
//------------------------------------------------------------------

// Backup Volatile Regs (EAX, ECX, EDX, MM0~7, XMM0~7, are all volatile according to 32bit Win/Linux ABI)
__fi void mVUbackupRegs(microVU* mVU, int toMemory, int onlyNeeded)
{
	if (toMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		for (int i = 0; i < (int)(iREGCNT_GPR); i++)
		{
			if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
				continue;

			if (!onlyNeeded || mVUra_checkCachedGPR(mVU->regAlloc, i))
			{
				num_gprs++;
				xe_push64_r(i);
			}
		}

		u32 save_xmms = 0; /* bit per xmm */
		for (int i = 0; i < (int)(iREGCNT_XMM); i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			if (!onlyNeeded || mVUra_checkCachedReg(mVU->regAlloc, i) || xmmPQ == i)
			{
				save_xmms |= (1u << i);
				num_xmms++;
			}
		}

		// we need 16 byte alignment on the stack
#ifdef _WIN32
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64)) + 32;
		int stack_offset = 32;
#else
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64));
		int stack_offset = 0;
#endif
		if (stack_size > 0)
		{
			xe_sub64_ri(XE_SP, stack_size);
			for (int i = 0; i < (int)(iREGCNT_XMM); i++)
			{
				if ((save_xmms & (1u << i)))
				{
					{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_memxg(xm, i); }
					stack_offset += sizeof(u128);
				}
			}
		}
	}
	else
	{
		// TODO(Stenzek): get rid of xmmbackup
		mVUra_flushAll(mVU->regAlloc, 1); // Flush Regalloc
		xe_movaps_mx(&mVU->xmmBackup[xmmPQ][0], xmmPQ);
	}
}

/* Restore Volatile Regs */
__fi void mVUrestoreRegs(microVU* mVU, int fromMemory, int onlyNeeded)
{
	if (fromMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		u32 save_gprs = 0; /* bit per gpr */
		for (int i = 0; i < (int)(iREGCNT_GPR); i++)
		{
			if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
				continue;

			if (!onlyNeeded || mVUra_checkCachedGPR(mVU->regAlloc, i))
			{
				save_gprs |= (1u << i);
				num_gprs++;
			}
		}

		u32 save_xmms = 0; /* bit per xmm */
		for (int i = 0; i < (int)(iREGCNT_XMM); i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			if (!onlyNeeded || mVUra_checkCachedReg(mVU->regAlloc, i) || xmmPQ == i)
			{
				save_xmms |= (1u << i);
				num_xmms++;
			}
		}

#ifdef _WIN32
		const int stack_extra = 32;
#else
		const int stack_extra = 0;
#endif
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64)) + stack_extra;
		if (num_xmms > 0)
		{
			int stack_offset = (num_xmms - 1) * sizeof(u128) + stack_extra;
			for (int i = (int)(iREGCNT_XMM - 1); i >= 0; i--)
			{
				if (!(save_xmms & (1u << i)))
					continue;

				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_xmemg(i, xm); }
				stack_offset -= sizeof(u128);
			}
		}
		if (stack_size > 0)
			xe_add64_ri(XE_SP, stack_size);

		for (int i = (int)(iREGCNT_GPR - 1); i >= 0; i--)
		{
			if ((save_gprs & (1u << i)))
				xe_pop64_r(i);
		}
	}
	else
	{
		xe_movaps_xm(xmmPQ, &mVU->xmmBackup[xmmPQ][0]);
	}
}

static void mVUTBit(void)
{
	retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagVUTBit); /* acq_rel >= release */
}

static void mVUEBit(void)
{
	retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagVUEBit); /* acq_rel >= release */
}

static inline u32 branchAddr(const mV)
{
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU->progMemMask) * 4);
}

static void mVUwaitMTVU(void) { vu1Thread.WaitVU(); }

/* Transforms the Address in gprReg to valid VU0/VU1 Address */
__fi void mVUaddrFix(mV, int gprReg)
{
	if (isVU1)
	{
		xe_and32_ri(gprReg, 0x3ff); // wrap around
		xe_shl32_ri(gprReg, 4);
	}
	else
	{
		xe_test32_ri(gprReg, 0x400);
		uint8_t* jmpA; xe_fwd_jcc8(Jcc_NotZero, jmpA); // if addr & 0x4000, reads VU1's VF regs and VI regs
			xe_and32_ri(gprReg, 0xff); // if !(addr & 0x4000), wrap around
			uint8_t* jmpB; xe_fwd_jcc32(Jcc_Unconditional, jmpB);
		xe_fwd_set8(jmpA);
			if (THREAD_VU1)
			{
				xe_fastcall0(mVU->waitMTVU);
			}
			xe_and32_ri(gprReg, 0x3f); // ToDo: theres a potential problem if VU0 overrides VU1's VF0/VI0 regs!
			xe_add64_ri(gprReg, (u128*)vuRegs[1].VF - (u128*)vuRegs[0].Mem);
		xe_fwd_set32(jmpB);
		xe_shl64_ri(gprReg, 4); // multiply by 16 (shift left by 4)
	}
}

__fi struct e_memopt mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	// if we had const prop for VIs, we could do that here..
	if (srcreg != 0)
		return e_memopt_none();
	const s32 addr = 0 + offset;
	if (isVU1)
		return e_memopt_of(e_mem_abs(vuRegs[mVU->index].Mem + ((addr & 0x3FFu) << 4) + offsetSS_));
	if (addr & 0x400)
		return e_memopt_none();
	return e_memopt_of(e_mem_abs(vuRegs[mVU->index].Mem + ((addr & 0xFFu) << 4) + offsetSS_));
}

//------------------------------------------------------------------
// Micro VU - Custom SSE Instructions
//------------------------------------------------------------------

struct SSEMasks
{
	u32 MIN_MAX_1[4], MIN_MAX_2[4], ADD_SS[4];
};

alignas(16) static const SSEMasks sseMasks =
{
	{0xffffffff, 0x80000000, 0xffffffff, 0x80000000},
	{0x00000000, 0x40000000, 0x00000000, 0x40000000},
	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
};


// Warning: Modifies t1 and t2
void MIN_MAX_PS(microVU* mVU, int to, int from, int t1in, int t2in, int min)
{
	int t1 = (t1in < 0) ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : t1in;
	int t2 = (t2in < 0) ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : t2in;

	/* Use integer comparison */
	{
		int c1 = min ? t2 : t1;
		int c2 = min ? t1 : t2;

		xe_movaps_xx(t1, to);
		xe_psrad_xi(t1, 31);
		xe_psrld_xi(t1, 1);
		xe_pxor_xx(t1, to);

		xe_movaps_xx(t2, from);
		xe_psrad_xi(t2, 31);
		xe_psrld_xi(t2, 1);
		xe_pxor_xx(t2, from);

		xe_pcmpgtd_xx(c1, c2);
		xe_pand_xx(to, c1);
		xe_pandn_xx(c1, from);
		xe_por_xx(to, c1);
	}

	if (t1 != t1in) mVUra_clearNeededXMM(mVU->regAlloc, t1);
	if (t2 != t2in) mVUra_clearNeededXMM(mVU->regAlloc, t2);
}

// Warning: Modifies to's upper 3 vectors, and t1
void MIN_MAX_SS(mV, int to, int from, int t1in, int min)
{
	int t1 = (t1in < 0) ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : t1in;
	xe_shufps_xxi(to, from, 0);
	xe_pand_xm(to, sseMasks.MIN_MAX_1);
	xe_por_xm(to, sseMasks.MIN_MAX_2);
	xe_pshufd_xxi(t1, to, 0xee);
	if (min) xe_minpd_xx(to, t1);
	else	 xe_maxpd_xx(to, t1);
	if (t1 != t1in)
		mVUra_clearNeededXMM(mVU->regAlloc, t1);
}

// Turns out only this is needed to get TriAce games booting with mVU
// Modifies from's lower vector

//------------------------------------------------------------------
// Exponent-distance operand masking for VU ADD/SUB. The hardware's
// adder truncates the smaller operand's low bits by exponent distance
// before adding; masking those bits and then doing the IEEE add under
// the VU's chop rounding reproduces it. Measured against the ps2float
// model, the plain clamped addps was wrong on 44.9% of random operand
// pairs and the masked add on 0.79% - the remainder is the
// exponent-255 range single precision cannot represent. The per-lane
// variable mask has no SSE2 shift, so 2^(d-1) comes from building the
// float with exponent field d-1 and truncating it back to integer;
// the identity against the scalar mask measures zero mismatches over
// 16.7 million lanes across every exponent distance. Both stages are
// AND-chains: OR-ing the sign bit into the keep mask makes the
// distance >= 25 stage safe even where the power construction
// overflows, so no blend and no extra register.
// a and b are masked in place; t1..t3 are scratch.
//------------------------------------------------------------------
static void mVUmaskAddSub(mV, int a, int b, int t1, int t2, int t3)
{
	// t1 = exponent difference d = exp(a) - exp(b), per lane
	xe_movaps_xx(t1, a);
	xe_pand_xm(t1, mVUglob.exponent);
	xe_psrld_xi(t1, 23);
	xe_movaps_xx(t2, b);
	xe_pand_xm(t2, mVUglob.exponent);
	xe_psrld_xi(t2, 23);
	xe_psubd_xx(t1, t2);

	// ---- b side: b &= (keep' | (d <= 0)) ; b &= (sign | (d <= 24)) ----
	xe_pcmpeqd_xx(t2, t2);            // -1
	xe_paddd_xx(t2, t1);              // d - 1
	xe_pslld_xi(t2, 23);
	xe_paddd_xm(t2, mVUglob.one);     // float with exponent d-1
	xe_cvttps2dq_xx(t2, t2);          // 2^(d-1)
	xe_psubd_xm(t2, mVUglob.addm1);   // low mask
	xe_pxor_xm(t2, mVUglob.addmall);  // keep = ~low
	xe_por_xm(t2, mVUglob.signbit);   // keep' (sign always survives)
	xe_movaps_xm(t3, mVUglob.addm1);
	xe_pcmpgtd_xx(t3, t1);            // 1 > d  <=>  d <= 0
	xe_por_xx(t2, t3);
	xe_pand_xx(b, t2);
	xe_movaps_xm(t3, mVUglob.addm25);
	xe_pcmpgtd_xx(t3, t1);            // 25 > d  <=>  d <= 24: distance 24 keeps bit 23
	xe_por_xm(t3, mVUglob.signbit);
	xe_pand_xx(b, t3);

	// ---- a side, with -d-1 == ~d ----
	xe_movaps_xx(t2, t1);
	xe_pxor_xm(t2, mVUglob.addmall);  // ~d = -d - 1
	xe_pslld_xi(t2, 23);
	xe_paddd_xm(t2, mVUglob.one);
	xe_cvttps2dq_xx(t2, t2);
	xe_psubd_xm(t2, mVUglob.addm1);
	xe_pxor_xm(t2, mVUglob.addmall);
	xe_por_xm(t2, mVUglob.signbit);
	xe_movaps_xx(t3, t1);
	xe_pcmpgtd_xm(t3, mVUglob.addmall); // d > -1  <=>  d >= 0
	xe_por_xx(t2, t3);
	xe_pand_xx(a, t2);
	xe_movaps_xx(t3, t1);
	xe_pcmpgtd_xm(t3, mVUglob.addmn25); // d > -25  <=>  d >= -24
	xe_por_xm(t3, mVUglob.signbit);
	xe_pand_xx(a, t3);
}

// Fused single-keep mask, packed forms only: (X|~p) & (Y|~p) = (X&Y)|~p
// collapses the two per-side chains around one keep-and-not-huge mask
// computed from |d|-1, and applying the a side first lets the distance
// register die into the masked from-copy - three temps, no source
// mutation. Proven bit-identical to the two-sided mask above by the
// emitted-bytes differential harness: twelve million executed lanes
// under chop, denormals-are-zero and flush-to-zero, across five
// register patterns including xmm15 operands, zero mismatches. Masks
// `a` in place and leaves the masked copy of b in tD; b is untouched.
static void mVUmaskAddSubFused(mV, int a, int b, int tD, int tM, int tS)
{
	xe_movaps_xx(tD, a);
	xe_pand_xm(tD, mVUglob.exponent);
	xe_psrld_xi(tD, 23);
	xe_movaps_xx(tS, b);
	xe_pand_xm(tS, mVUglob.exponent);
	xe_psrld_xi(tS, 23);
	xe_psubd_xx(tD, tS);              // d
	xe_movaps_xx(tM, tD);
	xe_psrad_xi(tM, 31);
	xe_movaps_xx(tS, tD);
	xe_pxor_xx(tS, tM);
	xe_psubd_xx(tS, tM);
	xe_psubd_xm(tS, mVUglob.addm1);   // c = |d| - 1
	xe_movaps_xm(tM, mVUglob.addm24);
	xe_pcmpgtd_xx(tM, tS);            // 24 > c  <=>  |d| <= 24
	xe_por_xm(tM, mVUglob.signbit);
	xe_pslld_xi(tS, 23);
	xe_paddd_xm(tS, mVUglob.one);
	xe_cvttps2dq_xx(tS, tS);
	xe_psubd_xm(tS, mVUglob.addm1);
	xe_pxor_xm(tS, mVUglob.addmall);
	xe_por_xm(tS, mVUglob.signbit);   // keep'
	xe_pand_xx(tM, tS);               // M
	xe_movaps_xx(tS, tD);
	xe_pcmpgtd_xm(tS, mVUglob.addmall); // d >= 0: a side inactive
	xe_por_xx(tS, tM);
	xe_pand_xx(a, tS);
	xe_movaps_xx(tS, tM);
	xe_movaps_xx(tM, tD);
	xe_movaps_xm(tD, mVUglob.addm1);
	xe_pcmpgtd_xx(tD, tM);            // d <= 0: b side inactive
	xe_por_xx(tD, tS);
	xe_movaps_xx(tM, b);
	xe_pand_xx(tM, tD);
	xe_movaps_xx(tD, tM);             // masked b copy
}

// Masked add/sub. The destination may be masked in place, but the
// source is a cached VF register: mask a copy, never the original.
static void mVUmaskedAddSubOp(mV, int to, int from, bool isPS, bool issub)
{
	if (!clampE)
	{
		/* Default mode: clamp3/clamp4 emit nothing, so the lean fused
		 * schedules apply - proven bit-identical by the emitted-bytes
		 * differential harness, and free of GPR use entirely: the
		 * GPR-based scalar rework broke rendering in the wild through a
		 * mechanism still unidentified, so these paths touch none. */
		const int tD = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int tM = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int tS = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		if (isPS)
		{
			mVUmaskAddSubFused(mVU, to, from, tD, tM, tS);
			if (issub) xe_subps_xx(to, tD);
			else       xe_addps_xx(to, tD);
		}
		else
		{
			/* lane 0 result only: compute on a copy of the destination
			 * so its upper lanes survive. */
			const int dst = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
			xe_movaps_xx(dst, to);
			mVUmaskAddSubFused(mVU, dst, from, tD, tM, tS);
			if (issub) xe_subss_xx(dst, tD);
			else       xe_addss_xx(dst, tD);
			xe_movss_xx(to, dst);
			mVUra_clearNeededXMM(mVU->regAlloc, dst);
		}
		mVUra_clearNeededXMM(mVU->regAlloc, tD);
		mVUra_clearNeededXMM(mVU->regAlloc, tM);
		mVUra_clearNeededXMM(mVU->regAlloc, tS);
		return;
	}
	const int tF = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int t3 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	int dst = to;
	if (!isPS)
	{
		// addss/subss preserve the upper lanes of the destination:
		// compute on a copy so the mask stage can't disturb them.
		dst = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		xe_movaps_xx(dst, to);
	}
	xe_movaps_xx(tF, from);
	mVUclamp3(mVU, dst, t1, isPS ? 0xf : 0x8);
	mVUclamp3(mVU, tF, t1, isPS ? 0xf : 0x8);
	mVUmaskAddSub(mVU, dst, tF, t1, t2, t3);
	if (isPS)
	{
		if (issub) xe_subps_xx(dst, tF);
		else       xe_addps_xx(dst, tF);
	}
	else
	{
		if (issub) xe_subss_xx(dst, tF);
		else       xe_addss_xx(dst, tF);
	}
	mVUclamp4(mVU, dst, t1, isPS ? 0xf : 0x8);
	if (!isPS)
	{
		xe_movss_xx(to, dst);
		mVUra_clearNeededXMM(mVU->regAlloc, dst);
	}
	mVUra_clearNeededXMM(mVU->regAlloc, tF);
	mVUra_clearNeededXMM(mVU->regAlloc, t1);
	mVUra_clearNeededXMM(mVU->regAlloc, t2);
	mVUra_clearNeededXMM(mVU->regAlloc, t3);
}


#define clampOp(opX, isPS) \
	do { \
		mVUclamp3(mVU, to, t1, (isPS) ? 0xf : 0x8); \
		mVUclamp3(mVU, from, t1, (isPS) ? 0xf : 0x8); \
		opX((to), (from)); \
		mVUclamp4(mVU, to, t1, (isPS) ? 0xf : 0x8); \
	} while (0)

void SSE_MAXPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, 0);
}
void SSE_MINPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, 1);
}
void SSE_MAXSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_SS(mVU, to, from, t1, 0);
}
void SSE_MINSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_SS(mVU, to, from, t1, 1);
}
/* The TriAce ADDi hack is gone: the exponent-distance mask reproduces
 * the discard the hack approximated, for every distance. */
void SSE_ADD2SS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB)
	{
		mVUmaskedAddSubOp(mVU, to, from, false, false);
		return;
	}
	clampOp(xe_addss_xx, 0);
}
void SSE_ADD2PS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB)
	{
		mVUmaskedAddSubOp(mVU, to, from, true, false);
		return;
	}
	clampOp(xe_addps_xx, 1);
}
void SSE_ADDPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB)
	{
		mVUmaskedAddSubOp(mVU, to, from, true, false);
		return;
	}
	clampOp(xe_addps_xx, 1);
}
void SSE_ADDSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB)
	{
		mVUmaskedAddSubOp(mVU, to, from, false, false);
		return;
	}
	clampOp(xe_addss_xx, 0);
}
void SSE_SUBPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB)
	{
		mVUmaskedAddSubOp(mVU, to, from, true, true);
		return;
	}
	clampOp(xe_subps_xx, 1);
}
void SSE_SUBSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB)
	{
		mVUmaskedAddSubOp(mVU, to, from, false, true);
		return;
	}
	clampOp(xe_subss_xx, 0);
}

//------------------------------------------------------------------
// Hardware-exact multiply. The Booth-truncated multiplier differs
// from the plain 48-bit product only when the correction borrow can
// reach the kept mantissa, which requires the product's bits 22:15
// to be all zero - about one lane in 256. The emitted fast path is
// the product, that guard, and a chop-pack with the model's ranges;
// when any lane trips the guard the whole quad drops to the scalar
// soft model through a per-VU buffer, which recomputes it exactly.
// Measured against ps2float: byte-exact over 12 million lanes, with
// the guard firing on 1.55% of random quads and the fast path
// costing about 9ns per quad in the C model of this sequence.
//------------------------------------------------------------------

// dst receives the 4-lane exact product of to * from; from is not
// modified. For the SS forms the caller passes a temp as dst and
// merges lane 0, so the upper-lane garbage never escapes.
static void mVUexactMulPS(mV, int dst, int to, int from)
{
	const int T1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T3 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T4 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T5 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T6 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T7 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	u32* buf = mVU->exactMulBuf;
	uint8_t *jSlow, *jDone;
	u8* packStart;

	// T1 = zero-operand mask (either exponent field zero)
	xe_pxor_xx(T7, T7);
	xe_movaps_xx(T1, to);
	xe_pand_xm(T1, mVUglob.exponent);
	xe_pcmpeqd_xx(T1, T7);
	xe_movaps_xx(T2, from);
	xe_pand_xm(T2, mVUglob.exponent);
	xe_pcmpeqd_xx(T2, T7);
	xe_por_xx(T1, T2);
	// T2 = result sign
	xe_movaps_xx(T2, to);
	xe_pxor_xx(T2, from);
	xe_pand_xm(T2, mVUglob.signbit);
	// T3 = ma, T4 = mb (mantissa with implicit bit)
	xe_movaps_xx(T3, to);
	xe_pand_xm(T3, mVUglob.mulman);
	xe_por_xm(T3, mVUglob.mulimp);
	xe_movaps_xx(T4, from);
	xe_pand_xm(T4, mVUglob.mulman);
	xe_por_xm(T4, mVUglob.mulimp);
	// mantissas for the tree stub's slow path
	xe_movaps_mx(&buf[0], T3);
	xe_movaps_mx(&buf[4], T4);
	// T5 = biased result exponent
	xe_movaps_xx(T5, to);
	xe_pand_xm(T5, mVUglob.exponent);
	xe_psrld_xi(T5, 23);
	xe_movaps_xx(T6, from);
	xe_pand_xm(T6, mVUglob.exponent);
	xe_psrld_xi(T6, 23);
	xe_paddd_xx(T5, T6);
	xe_psubd_xm(T5, mVUglob.mul127);
	// T6 = p02, T7 = p13
	xe_movaps_xx(T6, T3);
	xe_pmuludq_xx(T6, T4);
	xe_movaps_xx(T7, T3);
	xe_psrlq_xi(T7, 32);
	xe_psrlq_xi(T4, 32);
	xe_pmuludq_xx(T7, T4);
	// guard: any lane with (full & 0x7f8000) == 0
	xe_movaps_xx(T3, T6);
	xe_pand_xm(T3, mVUglob.mulg);
	xe_movaps_xx(T4, T7);
	xe_pand_xm(T4, mVUglob.mulg);
	xe_psllq_xi(T4, 32);
	xe_por_xx(T3, T4);
	xe_pxor_xx(T4, T4);
	xe_pcmpeqd_xx(T3, T4);
	/* zero-operand lanes are decided by the final select whatever the
	 * product says, so they must not trip the guard: with the implicit
	 * bit forced on, 0.0 times anything zeroes the guard field, and
	 * game data is full of zeros. */
	xe_movaps_xx(T4, T1);
	xe_pandn_xx(T4, T3);
	xe_movmskps_rx(XE_AX, T4);
	xe_test32_rr(XE_AX, XE_AX);
	xe_fwd_jcc32(Jcc_NotZero, jSlow);
	// ---- shared pack (fast path falls in; slow path jumps back) ----
	packStart = x86Ptr;
	// rm = (corrected==full) >> 23, merged into epi32 lanes
	xe_movaps_xx(T3, T6);
	xe_psrlq_xi(T3, 23);
	xe_pand_xm(T3, mVUglob.mullo);
	xe_movaps_xx(T4, T7);
	xe_psrlq_xi(T4, 23);
	xe_psllq_xi(T4, 32);
	xe_por_xx(T3, T4);
	// carry-normalize
	xe_movaps_xx(T4, T3);
	xe_psrld_xi(T4, 24);
	xe_movaps_xx(T6, T4);
	xe_pcmpeqd_xm(T6, mVUglob.mul1);
	xe_movaps_xx(T7, T3);
	xe_psrld_xi(T7, 1);
	xe_pand_xx(T7, T6);
	xe_pandn_xx(T6, T3);
	xe_por_xx(T6, T7);
	xe_paddd_xx(T5, T4);
	// normal result in T3
	xe_movaps_xx(T3, T5);
	xe_pslld_xi(T3, 23);
	xe_pand_xm(T6, mVUglob.mulman);
	xe_por_xx(T3, T6);
	xe_por_xx(T3, T2);
	// overflow: e > 255 -> sign | 0x7fffffff
	xe_movaps_xx(T4, T5);
	xe_pcmpgtd_xm(T4, mVUglob.mul255);
	xe_movaps_xx(T7, T2);
	xe_por_xm(T7, mVUglob.absclip);
	xe_pand_xx(T7, T4);
	xe_pandn_xx(T4, T3);
	xe_por_xx(T4, T7);
	// underflow (1 > e) or zero operand -> sign
	xe_movaps_xm(T3, mVUglob.mul1);
	xe_pcmpgtd_xx(T3, T5);
	xe_por_xx(T3, T1);
	xe_pand_xx(T2, T3);
	xe_pandn_xx(T3, T4);
	xe_por_xx(T3, T2);
	xe_movaps_xx(dst, T3);
	xe_fwd_jcc32(Jcc_Unconditional, jDone);
	// ---- slow path: hand the products to the tree stub, take the
	// corrected ones back, and rejoin the pack above. The stub saves
	// every register itself, so nothing here needs a flush. ----
	xe_fwd_set32(jSlow);
	xe_movaps_mx(&buf[8], T6);
	xe_movaps_mx(&buf[12], T7);
	xe_call_ptr(mVU->exactMulStub);
	xe_movaps_xm(T6, &buf[8]);
	xe_movaps_xm(T7, &buf[12]);
	xe_jmp_to(packStart);
	xe_fwd_set32(jDone);

	mVUra_clearNeededXMM(mVU->regAlloc, T1);
	mVUra_clearNeededXMM(mVU->regAlloc, T2);
	mVUra_clearNeededXMM(mVU->regAlloc, T3);
	mVUra_clearNeededXMM(mVU->regAlloc, T4);
	mVUra_clearNeededXMM(mVU->regAlloc, T5);
	mVUra_clearNeededXMM(mVU->regAlloc, T6);
	mVUra_clearNeededXMM(mVU->regAlloc, T7);
}

void SSE_MULPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_EXACTMUL)
	{
		mVUexactMulPS(mVU, to, to, from);
		return;
	}
	clampOp(xe_mulps_xx, 1);
}
void SSE_MULSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_EXACTMUL)
	{
		/* lane 0 through the same pipeline; upper lanes of the
		 * destination must survive, so compute into a temp. */
		const int td = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		mVUexactMulPS(mVU, td, to, from);
		xe_movss_xx(to, td);
		mVUra_clearNeededXMM(mVU->regAlloc, td);
		return;
	}
	clampOp(xe_mulss_xx, 0);
}
void SSE_DIVPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	clampOp(xe_divps_xx, 1);
}
void SSE_DIVSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	clampOp(xe_divss_xx, 0);
}
