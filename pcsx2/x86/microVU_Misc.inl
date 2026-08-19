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
#include <bitset>
#include <optional>

//------------------------------------------------------------------
// Micro VU - Reg Loading/Saving/Shuffling/Unpacking/Merging...
//------------------------------------------------------------------

void mVUunpack_xyzw(const xmm& dstreg, const xmm& srcreg, int xyzw)
{
	switch (xyzw)
	{
		case 0: xe_pshufd_xxi(dstreg.Id, srcreg.Id, 0x00); break; // XXXX
		case 1: xe_pshufd_xxi(dstreg.Id, srcreg.Id, 0x55); break; // YYYY
		case 2: xe_pshufd_xxi(dstreg.Id, srcreg.Id, 0xaa); break; // ZZZZ
		case 3: xe_pshufd_xxi(dstreg.Id, srcreg.Id, 0xff); break; // WWWW
	}
}

void mVUloadReg(const xmm& reg, xAddressVoid ptr, int xyzw)
{
	switch (xyzw)
	{
		case 8:  { struct e_mem xm; XE_MEM_XAV(xm, ptr, 0);  xe_movss_xmemg(reg.Id, xm); } break; // X
		case 4:  { struct e_mem xm; XE_MEM_XAV(xm, ptr, 4);  xe_movss_xmemg(reg.Id, xm); } break; // Y
		case 2:  { struct e_mem xm; XE_MEM_XAV(xm, ptr, 8);  xe_movss_xmemg(reg.Id, xm); } break; // Z
		case 1:  { struct e_mem xm; XE_MEM_XAV(xm, ptr, 12); xe_movss_xmemg(reg.Id, xm); } break; // W
		default: xMOVAPS (reg, ptr128[ptr]);     break;
	}
}

// Modifies the Source Reg!
void mVUsaveReg(const xmm& reg, xAddressVoid ptr, int xyzw, bool modXYZW)
{
	switch (xyzw)
	{
		case 5: // YW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 4);  xe_extractps_memxi(xm, reg.Id, 1); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 12); xe_extractps_memxi(xm, reg.Id, 3); }
			break;
		case 6: // YZ
			xe_pshufd_xxi(reg.Id, reg.Id, 0xc9);
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 4); xe_movlps_memxg(xm, reg.Id); }
			break;
		case 7: // YZW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 8); xe_movhps_memxg(xm, reg.Id); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 4);  xe_extractps_memxi(xm, reg.Id, 1); }
			break;
		case 9: // XW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movss_memxg(xm, reg.Id); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 12); xe_extractps_memxi(xm, reg.Id, 3); }
			break;
		case 10: // XZ
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movss_memxg(xm, reg.Id); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 8);  xe_extractps_memxi(xm, reg.Id, 2); }
			break;
		case 11: // XZW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movss_memxg(xm, reg.Id); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 8); xe_movhps_memxg(xm, reg.Id); }
			break;
		case 13: // XYW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movlps_memxg(xm, reg.Id); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 12); xe_extractps_memxi(xm, reg.Id, 3); }
			break;
		case 14: // XYZ
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movlps_memxg(xm, reg.Id); }
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 8);  xe_extractps_memxi(xm, reg.Id, 2); }
			break;
		case 4: // Y
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 1);
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 4); xe_movss_memxg(xm, reg.Id); }
			break;
		case 2: // Z
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 2);
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 8); xe_movss_memxg(xm, reg.Id); }
			break;
		case 1: // W
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 3);
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 12); xe_movss_memxg(xm, reg.Id); }
			break;
		case 8: // X
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movss_memxg(xm, reg.Id); }
			break;
		case 12: // XY
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movlps_memxg(xm, reg.Id); }
			break;
		case 3: // ZW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 8); xe_movhps_memxg(xm, reg.Id); }
			break;
		default: // XYZW
			{ struct e_mem xm; XE_MEM_XAV(xm, ptr, 0); xe_movaps_memxg(xm, reg.Id); }
			break;
	}
}

// Modifies the Source Reg! (ToDo: Optimize modXYZW = 1 cases)
void mVUmergeRegs(const xmm& dest, const xmm& src, int xyzw, bool modXYZW)
{
	xyzw &= 0xf;
	if ((dest != src) && (xyzw != 0))
	{
		if (xyzw == 0x8)
			xe_movss_xx(dest.Id, src.Id);
		else if (xyzw == 0xf)
			xe_movaps_xx(dest.Id, src.Id);
		else
		{
			if (modXYZW)
			{
				if      (xyzw == 1) { xe_insertps_xxi(dest.Id, src.Id, _MM_MK_INSERTPS_NDX(0, 3, 0)); return; }
				else if (xyzw == 2) { xe_insertps_xxi(dest.Id, src.Id, _MM_MK_INSERTPS_NDX(0, 2, 0)); return; }
				else if (xyzw == 4) { xe_insertps_xxi(dest.Id, src.Id, _MM_MK_INSERTPS_NDX(0, 1, 0)); return; }
			}
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);
			xe_blendps_xxi(dest.Id, src.Id, xyzw);
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Misc Functions
//------------------------------------------------------------------

// Backup Volatile Regs (EAX, ECX, EDX, MM0~7, XMM0~7, are all volatile according to 32bit Win/Linux ABI)
__fi void mVUbackupRegs(microVU& mVU, bool toMemory = false, bool onlyNeeded = false)
{
	if (toMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!xRegister32::IsCallerSaved(i) || i == rsp.Id)
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i))
			{
				num_gprs++;
				xe_push64_r(i);
			}
		}

		std::bitset<iREGCNT_XMM> save_xmms;
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedReg(i) || xmmPQ.Id == i)
			{
				save_xmms[i] = true;
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
			for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
			{
				if (save_xmms[i])
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
		mVU.regAlloc->flushAll(); // Flush Regalloc
		xe_movaps_mx(&mVU.xmmBackup[xmmPQ.Id][0], xmmPQ.Id);
	}
}

/* Restore Volatile Regs */
__fi void mVUrestoreRegs(microVU& mVU, bool fromMemory = false, bool onlyNeeded = false)
{
	if (fromMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		std::bitset<iREGCNT_GPR> save_gprs;
		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!xRegister32::IsCallerSaved(i) || i == rsp.Id)
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i))
			{
				save_gprs[i] = true;
				num_gprs++;
			}
		}

		std::bitset<iREGCNT_XMM> save_xmms;
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedReg(i) || xmmPQ.Id == i)
			{
				save_xmms[i] = true;
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
			for (int i = static_cast<int>(iREGCNT_XMM - 1); i >= 0; i--)
			{
				if (!save_xmms[i])
					continue;

				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_xmemg(i, xm); }
				stack_offset -= sizeof(u128);
			}
		}
		if (stack_size > 0)
			xe_add64_ri(XE_SP, stack_size);

		for (int i = static_cast<int>(iREGCNT_GPR - 1); i >= 0; i--)
		{
			if (save_gprs[i])
				xe_pop64_r(i);
		}
	}
	else
	{
		xe_movaps_xm(xmmPQ.Id, &mVU.xmmBackup[xmmPQ.Id][0]);
	}
}

static void mVUTBit(void)
{
	vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUTBit, std::memory_order_release);
}

static void mVUEBit(void)
{
	vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUEBit, std::memory_order_release);
}

static inline u32 branchAddr(const mV)
{
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU.progMemMask) * 4);
}

static void mVUwaitMTVU(void) { vu1Thread.WaitVU(); }

/* Transforms the Address in gprReg to valid VU0/VU1 Address */
__fi void mVUaddrFix(mV, const xAddressReg& gprReg)
{
	if (isVU1)
	{
		xe_and32_ri(gprReg.Id, 0x3ff); // wrap around
		xe_shl32_ri(gprReg.Id, 4);
	}
	else
	{
		xe_test32_ri(gprReg.Id, 0x400);
		xForwardJNZ8 jmpA; // if addr & 0x4000, reads VU1's VF regs and VI regs
			xe_and32_ri(gprReg.Id, 0xff); // if !(addr & 0x4000), wrap around
			xForwardJump32 jmpB;
		jmpA.SetTarget();
			if (THREAD_VU1)
			{
				xe_fastcall0(mVU.waitMTVU);
			}
			xe_and32_ri(gprReg.Id, 0x3f); // ToDo: theres a potential problem if VU0 overrides VU1's VF0/VI0 regs!
			xe_add64_ri(gprReg.Id, (u128*)vuRegs[1].VF - (u128*)vuRegs[0].Mem);
		jmpB.SetTarget();
		xe_shl64_ri(gprReg.Id, 4); // multiply by 16 (shift left by 4)
	}
}

__fi std::optional<xAddressVoid> mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	// if we had const prop for VIs, we could do that here..
	if (srcreg != 0)
		return std::nullopt;
	const s32 addr = 0 + offset;
	if (isVU1)
		return ptr[vuRegs[mVU.index].Mem + ((addr & 0x3FFu) << 4) + offsetSS_];
	if (addr & 0x400)
		return std::nullopt;
	return ptr[vuRegs[mVU.index].Mem + ((addr & 0xFFu) << 4) + offsetSS_];
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
void MIN_MAX_PS(microVU& mVU, const xmm& to, const xmm& from, const xmm& t1in, const xmm& t2in, bool min)
{
	const xmm& t1 = t1in.IsEmpty() ? mVU.regAlloc->allocReg() : t1in;
	const xmm& t2 = t2in.IsEmpty() ? mVU.regAlloc->allocReg() : t2in;

	/* Use integer comparison */
	{
		const xmm& c1 = min ? t2 : t1;
		const xmm& c2 = min ? t1 : t2;

		xMOVAPS  (t1, to);
		xPSRA.D  (t1, 31);
		xPSRL.D  (t1,  1);
		xPXOR    (t1, to);

		xMOVAPS  (t2, from);
		xPSRA.D  (t2, 31);
		xPSRL.D  (t2,  1);
		xPXOR    (t2, from);

		xe_pcmpgtd_xx(c1.Id, c2.Id);
		xPAND    (to, c1);
		xPANDN   (c1, from);
		xPOR     (to, c1);
	}

	if (t1 != t1in) mVU.regAlloc->clearNeeded(t1);
	if (t2 != t2in) mVU.regAlloc->clearNeeded(t2);
}

// Warning: Modifies to's upper 3 vectors, and t1
void MIN_MAX_SS(mV, const xmm& to, const xmm& from, const xmm& t1in, bool min)
{
	const xmm& t1 = t1in.IsEmpty() ? mVU.regAlloc->allocReg() : t1in;
	xe_shufps_xxi(to.Id, from.Id, 0);
	xPAND   (to, ptr128[sseMasks.MIN_MAX_1]);
	xPOR    (to, ptr128[sseMasks.MIN_MAX_2]);
	xe_pshufd_xxi(t1.Id, to.Id, 0xee);
	if (min) xe_minpd_xx(to.Id, t1.Id);
	else	 xe_maxpd_xx(to.Id, t1.Id);
	if (t1 != t1in)
		mVU.regAlloc->clearNeeded(t1);
}

// Turns out only this is needed to get TriAce games booting with mVU
// Modifies from's lower vector
void ADD_SS_TriAceHack(microVU& mVU, const xmm& to, const xmm& from)
{
	xe_movd_rx(XE_AX, to.Id);
	xe_movd_rx(XE_CX, from.Id);
	xSHR (eax, 23);
	xSHR (ecx, 23);
	xAND (eax, 0xff);
	xAND (ecx, 0xff);
	xSUB (ecx, eax); // Exponent Difference

	xCMP (ecx, -25);
	xForwardJLE8 case_neg_big;
	xCMP (ecx,  25);
	xForwardJL8  case_end1;

	// case_pos_big:
	xe_pand_xm(to.Id, sseMasks.ADD_SS);
	xForwardJump8 case_end2;

	case_neg_big.SetTarget();
	xe_pand_xm(from.Id, sseMasks.ADD_SS);

	case_end1.SetTarget();
	case_end2.SetTarget();

	xe_addss_xx(to.Id, from.Id);
}

#define clampOp(opX, isPS) \
	do { \
		mVUclamp3(mVU, to, t1, (isPS) ? 0xf : 0x8); \
		mVUclamp3(mVU, from, t1, (isPS) ? 0xf : 0x8); \
		opX(to, from); \
		mVUclamp4(mVU, to, t1, (isPS) ? 0xf : 0x8); \
	} while (0)

void SSE_MAXPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, false);
}
void SSE_MINPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, true);
}
void SSE_MAXSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_SS(mVU, to, from, t1, false);
}
void SSE_MINSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_SS(mVU, to, from, t1, true);
}
void SSE_ADD2SS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	if (!CHECK_VUADDSUBHACK)
		clampOp(xADD.SS, false);
	else
		ADD_SS_TriAceHack(mVU, to, from);
}

// Does same as SSE_ADDPS since tri-ace games only need SS implementation of VUADDSUBHACK...
void SSE_ADD2PS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xADD.PS, true);
}
void SSE_ADDPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xADD.PS, true);
}
void SSE_ADDSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xADD.SS, false);
}
void SSE_SUBPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xSUB.PS, true);
}
void SSE_SUBSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xSUB.SS, false);
}
void SSE_MULPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xMUL.PS, true);
}
void SSE_MULSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xMUL.SS, false);
}
void SSE_DIVPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xDIV.PS, true);
}
void SSE_DIVSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xDIV.SS, false);
}
