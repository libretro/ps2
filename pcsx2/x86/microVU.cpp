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

// Micro VU recompiler! - author: cottonvibes(@gmail.com)

#include <cstring> /* memset */
#include <bitset>
#include <optional>

#include <cpuinfo.h>

#include "microVU.h"

#include "common/AlignedMalloc.h"

//------------------------------------------------------------------
// Micro VU - Clamp Functions
//------------------------------------------------------------------

alignas(16) const u32 sse4_minvals[2][4] = {
	{0xff7fffff, 0xffffffff, 0xffffffff, 0xffffffff}, //1000
	{0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff}, //1111
};
alignas(16) const u32 sse4_maxvals[2][4] = {
	{0x7f7fffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}, //1000
	{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}, //1111
};

// Used for Result Clamping
// Note: This function will not preserve NaN values' sign.
// The theory behind this is that when we compute a result, and we've
// gotten a NaN value, then something went wrong; and the NaN's sign
// is not to be trusted. Games like positive values better usually,
// and its faster... so just always make NaNs into positive infinity.
void mVUclamp1(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw, bool bClampE = 0)
{
	if (((!clampE && CHECK_VU_OVERFLOW(mVU.index)) || (clampE && bClampE)) && mVU.regAlloc->checkVFClamp(reg.Id))
	{
		switch (xyzw)
		{
			case 1: case 2: case 4: case 8:
				xMIN.SS(reg, ptr32[mVUglob.maxvals]);
				xMAX.SS(reg, ptr32[mVUglob.minvals]);
				break;
			default:
				xMIN.PS(reg, ptr32[mVUglob.maxvals]);
				xMAX.PS(reg, ptr32[mVUglob.minvals]);
				break;
		}
	}
}

// Used for Operand Clamping
// Note 1: If 'preserve sign' mode is on, it will preserve the sign of NaN values.
// Note 2: Using regalloc here seems to contaminate some regs in certain games.
// Must be some specific case I've overlooked (or I used regalloc improperly on an opcode)
// so we just use a temporary mem location for our backup for now... (non-sse4 version only)
void mVUclamp2(microVU& mVU, const xmm& reg, const xmm& regT1in, int xyzw, bool bClampE = 0)
{
	if (((!clampE && CHECK_VU_SIGN_OVERFLOW(mVU.index)) || (clampE && bClampE && CHECK_VU_SIGN_OVERFLOW(mVU.index))) && mVU.regAlloc->checkVFClamp(reg.Id))
	{
		int i = (xyzw == 1 || xyzw == 2 || xyzw == 4 || xyzw == 8) ? 0 : 1;
		xPMIN.SD(reg, ptr128[&sse4_maxvals[i][0]]);
		xPMIN.UD(reg, ptr128[&sse4_minvals[i][0]]);
		return;
	}
	else
		mVUclamp1(mVU, reg, regT1in, xyzw, bClampE);
}

// Used for operand clamping on every SSE instruction (add/sub/mul/div)
void mVUclamp3(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw)
{
	if (clampE && mVU.regAlloc->checkVFClamp(reg.Id))
		mVUclamp2(mVU, reg, regT1, xyzw, 1);
}

// Used for result clamping on every SSE instruction (add/sub/mul/div)
// Note: Disabled in "preserve sign" mode because in certain cases it
// makes too much code-gen, and you get jump8-overflows in certain
// emulated opcodes (causing crashes). Since we're clamping the operands
// with mVUclamp3, we should almost never be getting a NaN result,
// but this clamp is just a precaution just-in-case.
void mVUclamp4(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw)
{
	if (clampE && !CHECK_VU_SIGN_OVERFLOW(mVU.index) && mVU.regAlloc->checkVFClamp(reg.Id))
		mVUclamp1(mVU, reg, regT1, xyzw, 1);
}

//------------------------------------------------------------------
// Micro VU - Reg Loading/Saving/Shuffling/Unpacking/Merging...
//------------------------------------------------------------------

void mVUunpack_xyzw(const xmm& dstreg, const xmm& srcreg, int xyzw)
{
	switch (xyzw)
	{
		case 0: xPSHUF.D(dstreg, srcreg, 0x00); break; // XXXX
		case 1: xPSHUF.D(dstreg, srcreg, 0x55); break; // YYYY
		case 2: xPSHUF.D(dstreg, srcreg, 0xaa); break; // ZZZZ
		case 3: xPSHUF.D(dstreg, srcreg, 0xff); break; // WWWW
	}
}

void mVUloadReg(const xmm& reg, xAddressVoid ptr, int xyzw)
{
	switch (xyzw)
	{
		case 8:  xMOVSSZX(reg, ptr32[ptr     ]); break; // X
		case 4:  xMOVSSZX(reg, ptr32[ptr +  4]); break; // Y
		case 2:  xMOVSSZX(reg, ptr32[ptr +  8]); break; // Z
		case 1:  xMOVSSZX(reg, ptr32[ptr + 12]); break; // W
		default: xMOVAPS (reg, ptr128[ptr]);     break;
	}
}

void mVUloadIreg(const xmm& reg, int xyzw, VURegs* vuRegs)
{
	xMOVSSZX(reg, ptr32[&vuRegs->VI[REG_I].UL]);
	if (!_XYZWss(xyzw))
		xSHUF.PS(reg, reg, 0);
}

// Modifies the Source Reg!
void mVUsaveReg(const xmm& reg, xAddressVoid ptr, int xyzw, bool modXYZW)
{
	switch (xyzw)
	{
		case 5: // YW
			xEXTRACTPS(ptr32[ptr + 4], reg, 1);
			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
			break;
		case 6: // YZ
			xPSHUF.D(reg, reg, 0xc9);
			xMOVL.PS(ptr64[ptr + 4], reg);
			break;
		case 7: // YZW
			xMOVH.PS(ptr64[ptr + 8], reg);
			xEXTRACTPS(ptr32[ptr + 4], reg, 1);
			break;
		case 9: // XW
			xMOVSS(ptr32[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
			break;
		case 10: // XZ
			xMOVSS(ptr32[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 8], reg, 2);
			break;
		case 11: // XZW
			xMOVSS(ptr32[ptr], reg);
			xMOVH.PS(ptr64[ptr + 8], reg);
			break;
		case 13: // XYW
			xMOVL.PS(ptr64[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
			break;
		case 14: // XYZ
			xMOVL.PS(ptr64[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 8], reg, 2);
			break;
		case 4: // Y
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 1);
			xMOVSS(ptr32[ptr + 4], reg);
			break;
		case 2: // Z
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 2);
			xMOVSS(ptr32[ptr + 8], reg);
			break;
		case 1: // W
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 3);
			xMOVSS(ptr32[ptr + 12], reg);
			break;
		case 8: // X
			xMOVSS(ptr32[ptr], reg);
			break;
		case 12: // XY
			xMOVL.PS(ptr64[ptr], reg);
			break;
		case 3: // ZW
			xMOVH.PS(ptr64[ptr + 8], reg);
			break;
		default: // XYZW
			xMOVAPS(ptr128[ptr], reg);
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
			xMOVSS(dest, src);
		else if (xyzw == 0xf)
			xMOVAPS(dest, src);
		else
		{
			if (modXYZW)
			{
				if      (xyzw == 1) { xINSERTPS(dest, src, _MM_MK_INSERTPS_NDX(0, 3, 0)); return; }
				else if (xyzw == 2) { xINSERTPS(dest, src, _MM_MK_INSERTPS_NDX(0, 2, 0)); return; }
				else if (xyzw == 4) { xINSERTPS(dest, src, _MM_MK_INSERTPS_NDX(0, 1, 0)); return; }
			}
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);
			xBLEND.PS(dest, src, xyzw);
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
			if (!Register_IsCallerSaved(i) || i == rsp.Id)
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i))
			{
				num_gprs++;
				xPUSH(xRegister64(i));
			}
		}

		std::bitset<iREGCNT_XMM> save_xmms;
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!RegisterSSE_IsCallerSaved(i))
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
			xSUB(rsp, stack_size);
			for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
			{
				if (save_xmms[i])
				{
					xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
					stack_offset += sizeof(u128);
				}
			}
		}
	}
	else
	{
		// TODO(Stenzek): get rid of xmmbackup
		mVU.regAlloc->flushAll(); // Flush Regalloc
		xMOVAPS(ptr128[&mVU.xmmBackup[xmmPQ.Id][0]], xmmPQ);
	}
}

// Restore Volatile Regs
__fi void mVUrestoreRegs(microVU& mVU, bool fromMemory = false, bool onlyNeeded = false)
{
	if (fromMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		std::bitset<iREGCNT_GPR> save_gprs;
		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!Register_IsCallerSaved(i) || i == rsp.Id)
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
			if (!RegisterSSE_IsCallerSaved(i))
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

				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
				stack_offset -= sizeof(u128);
			}
		}
		if (stack_size > 0)
			xADD(rsp, stack_size);

		for (int i = static_cast<int>(iREGCNT_GPR - 1); i >= 0; i--)
		{
			if (save_gprs[i])
				xPOP(xRegister64(i));
		}
	}
	else
	{
		xMOVAPS(xmmPQ, ptr128[&mVU.xmmBackup[xmmPQ.Id][0]]);
	}
}

static void mVUTBit(void)
{
	u32 old = vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUTBit, std::memory_order_release);
}

static void mVUEBit(void)
{
	vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUEBit, std::memory_order_release);
}

static inline u32 branchAddr(const mV)
{
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU.progMemMask) * 4);
}

static void mVUwaitMTVU()
{
	vu1Thread.WaitVU();
}

// Transforms the Address in gprReg to valid VU0/VU1 Address
__fi void mVUaddrFix(mV, const xAddressReg& gprReg)
{
	if (isVU1)
	{
		xAND(xRegister32(gprReg.Id), 0x3ff); // wrap around
		xSHL(xRegister32(gprReg.Id), 4);
	}
	else
	{
		xTEST(xRegister32(gprReg.Id), 0x400);
		xForwardJNZ8 jmpA; // if addr & 0x4000, reads VU1's VF regs and VI regs
			xAND(xRegister32(gprReg.Id), 0xff); // if !(addr & 0x4000), wrap around
			xForwardJump32 jmpB;
		jmpA.SetTarget();
			if (THREAD_VU1)
			{
				xFastCall((void*)mVU.waitMTVU);
			}
			xAND(xRegister32(gprReg.Id), 0x3f); // ToDo: theres a potential problem if VU0 overrides VU1's VF0/VI0 regs!
			xADD(gprReg, (u128*)vuRegs[1].VF - (u128*)vuRegs[0].Mem);
		jmpB.SetTarget();
		xSHL(gprReg, 4); // multiply by 16 (shift left by 4)
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

	// use integer comparison
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

		xPCMP.GTD(c1, c2);
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
	xSHUF.PS(to, from, 0);
	xPAND   (to, ptr128[sseMasks.MIN_MAX_1]);
	xPOR    (to, ptr128[sseMasks.MIN_MAX_2]);
	xPSHUF.D(t1, to, 0xee);
	if (min) xMIN.PD(to, t1);
	else	 xMAX.PD(to, t1);
	if (t1 != t1in)
		mVU.regAlloc->clearNeeded(t1);
}

// Turns out only this is needed to get TriAce games booting with mVU
// Modifies from's lower vector
void ADD_SS_TriAceHack(microVU& mVU, const xmm& to, const xmm& from)
{
	xMOVD(eax, to);
	xMOVD(ecx, from);
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
	xPAND(to, ptr128[sseMasks.ADD_SS]);
	xForwardJump8 case_end2;

	case_neg_big.SetTarget();
	xPAND(from, ptr128[sseMasks.ADD_SS]);

	case_end1.SetTarget();
	case_end2.SetTarget();

	xADD.SS(to, from);
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

//------------------------------------------------------------------
// Micro VU - Pass 1 Functions
//------------------------------------------------------------------

//------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------

// Read a VF reg
__ri void analyzeReg1(mV, int xReg, microVFreg& vfRead) {
	if (xReg) {
		if (_X) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
		if (_Y) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
		if (_Z) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
		if (_W) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
	}
}

// Write to a VF reg
__ri void analyzeReg2(mV, int xReg, microVFreg& vfWrite, bool isLowOp)
{
	if (xReg)
	{
		#define bReg(x, y) mVUregsTemp.VFreg[y] = x; mVUregsTemp.VF[y]
		if (_X) { bReg(xReg, isLowOp).x = 4; vfWrite.reg = xReg; vfWrite.x = 4; }
		if (_Y) { bReg(xReg, isLowOp).y = 4; vfWrite.reg = xReg; vfWrite.y = 4; }
		if (_Z) { bReg(xReg, isLowOp).z = 4; vfWrite.reg = xReg; vfWrite.z = 4; }
		if (_W) { bReg(xReg, isLowOp).w = 4; vfWrite.reg = xReg; vfWrite.w = 4; }
	}
}

// Read a VF reg (BC opcodes)
__ri void analyzeReg3(mV, int xReg, microVFreg& vfRead)
{
	if (xReg)
	{
		if (_bc_x)
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x);
			vfRead.reg = xReg;
			vfRead.x = 1;
		}
		else if (_bc_y)
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y);
			vfRead.reg = xReg;
			vfRead.y = 1;
		}
		else if (_bc_z)
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z);
			vfRead.reg = xReg;
			vfRead.z = 1;
		}
		else
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w);
			vfRead.reg = xReg;
			vfRead.w = 1;
		}
	}
}

// For Clip Opcode
__ri void analyzeReg4(mV, int xReg, microVFreg& vfRead)
{
	if (xReg)
	{
		mVUstall   = std::max(mVUstall, mVUregs.VF[xReg].w);
		vfRead.reg = xReg;
		vfRead.w   = 1;
	}
}

// Read VF reg (FsF/FtF)
__ri void analyzeReg5(mV, int xReg, int fxf, microVFreg& vfRead)
{
	if (xReg)
	{
		switch (fxf)
		{
			case 0: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; break;
			case 1: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; break;
			case 2: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; break;
			case 3: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; break;
		}
	}
}

// Flips xyzw stalls to yzwx (MR32 Opcode)
__ri void analyzeReg6(mV, int xReg, microVFreg& vfRead)
{
	if (xReg)
	{
		if (_X) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
		if (_Y) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
		if (_Z) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
		if (_W) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
	}
}

// Reading a VI reg
__ri void analyzeVIreg1(mV, int xReg, microVIreg& viRead)
{
	if (xReg)
	{
		mVUstall    = std::max(mVUstall, mVUregs.VI[xReg]);
		viRead.reg  = xReg;
		viRead.used = 1;
	}
}

// Writing to a VI reg
__ri void analyzeVIreg2(mV, int xReg, microVIreg& viWrite, int aCycles)
{
	if (xReg)
	{
		mVUconstReg[xReg].isValid = 0;
		mVUregsTemp.VIreg = xReg;
		mVUregsTemp.VI    = aCycles;
		viWrite.reg  = xReg;
		viWrite.used = aCycles;
	}
}

#define analyzeQreg(x) \
	{ \
		mVUregsTemp.q = x; \
		mVUstall = std::max(mVUstall, mVUregs.q); \
	}
#define analyzePreg(x) \
	{ \
		mVUregsTemp.p = x; \
		mVUstall = std::max(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0)); \
	}
#define analyzeRreg() \
	{ \
		mVUregsTemp.r = 1; \
	}
#define analyzeXGkick1() \
	{ \
		mVUstall = std::max(mVUstall, mVUregs.xgkick); \
	}
#define analyzeXGkick2(x) \
	{ \
		mVUregsTemp.xgkick = x; \
	}
#define setConstReg(x, v) \
	{ \
		if (x) \
		{ \
			mVUconstReg[x].isValid = 1; \
			mVUconstReg[x].regValue = v; \
		} \
	}

//------------------------------------------------------------------
// FMAC1 - Normal FMAC Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeFMAC1(mV, int Fd, int Fs, int Ft)
{
	sFLAG.doFlag = 1;
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg1(mVU, Ft, mVUup.VF_read[1]);
	analyzeReg2(mVU, Fd, mVUup.VF_write, 0);
}

//------------------------------------------------------------------
// FMAC2 - ABS/FTOI/ITOF Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeFMAC2(mV, int Fs, int Ft)
{
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg2(mVU, Ft, mVUup.VF_write, 0);
}

//------------------------------------------------------------------
// FMAC3 - BC(xyzw) FMAC Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeFMAC3(mV, int Fd, int Fs, int Ft)
{
	sFLAG.doFlag = 1;
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg3(mVU, Ft, mVUup.VF_read[1]);
	analyzeReg2(mVU, Fd, mVUup.VF_write, 0);
}

//------------------------------------------------------------------
// FMAC4 - Clip FMAC Opcode
//------------------------------------------------------------------

__fi void mVUanalyzeFMAC4(mV, int Fs, int Ft)
{
	cFLAG.doFlag = 1;
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg4(mVU, Ft, mVUup.VF_read[1]);
}

//------------------------------------------------------------------
// IALU - IALU Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeIALU1(mV, int Id, int Is, int It)
{
	if (!Id)
		mVUlow.isNOP = 1;
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg1(mVU, It, mVUlow.VI_read[1]);
	analyzeVIreg2(mVU, Id, mVUlow.VI_write, 1);
}

__fi void mVUanalyzeIALU2(mV, int Is, int It)
{
	if (!It)
		mVUlow.isNOP = 1;
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
}

__fi void mVUanalyzeIADDI(mV, int Is, int It, s16 imm)
{
	mVUanalyzeIALU2(mVU, Is, It);
	if (!Is)
	{
		setConstReg(It, imm);
	}
}

//------------------------------------------------------------------
// MR32 - MR32 Opcode
//------------------------------------------------------------------

__fi void mVUanalyzeMR32(mV, int Fs, int Ft)
{
	if (!Ft)
	{
		mVUlow.isNOP = 1;
	}
	analyzeReg6(mVU, Fs, mVUlow.VF_read[0]);
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
}

//------------------------------------------------------------------
// FDIV - DIV/SQRT/RSQRT Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeFDIV(mV, int Fs, int Fsf, int Ft, int Ftf, u8 xCycles)
{
	analyzeReg5(mVU, Fs, Fsf, mVUlow.VF_read[0]);
	analyzeReg5(mVU, Ft, Ftf, mVUlow.VF_read[1]);
	analyzeQreg(xCycles);
}

//------------------------------------------------------------------
// EFU - EFU Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeEFU1(mV, int Fs, int Fsf, u8 xCycles)
{
	analyzeReg5(mVU, Fs, Fsf, mVUlow.VF_read[0]);
	analyzePreg(xCycles);
}

__fi void mVUanalyzeEFU2(mV, int Fs, u8 xCycles)
{
	analyzeReg1(mVU, Fs, mVUlow.VF_read[0]);
	analyzePreg(xCycles);
}

//------------------------------------------------------------------
// MFP - MFP Opcode
//------------------------------------------------------------------

__fi void mVUanalyzeMFP(mV, int Ft)
{
	if (!Ft)
		mVUlow.isNOP = 1;
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
}

//------------------------------------------------------------------
// MOVE - MOVE Opcode
//------------------------------------------------------------------

__fi void mVUanalyzeMOVE(mV, int Fs, int Ft)
{
	if (!Ft || (Ft == Fs))
		mVUlow.isNOP = 1;
	analyzeReg1(mVU, Fs, mVUlow.VF_read[0]);
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
}

//------------------------------------------------------------------
// LQx - LQ/LQD/LQI Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeLQ(mV, int Ft, int Is, bool writeIs)
{
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
	if (!Ft)
	{
		if (writeIs && Is)
		{
			mVUlow.noWriteVF = 1;
		}
		else
		{
			mVUlow.isNOP = 1;
		}
	}
	if (writeIs)
	{
		analyzeVIreg2(mVU, Is, mVUlow.VI_write, 1);
	}
}

//------------------------------------------------------------------
// SQx - SQ/SQD/SQI Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeSQ(mV, int Fs, int It, bool writeIt)
{
	mVUlow.isMemWrite = true;
	analyzeReg1(mVU, Fs, mVUlow.VF_read[0]);
	analyzeVIreg1(mVU, It, mVUlow.VI_read[0]);
	if (writeIt)
	{
		analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
	}
}

//------------------------------------------------------------------
// R*** - R Reg Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeR1(mV, int Fs, int Fsf)
{
	analyzeReg5(mVU, Fs, Fsf, mVUlow.VF_read[0]);
	analyzeRreg();
}

__fi void mVUanalyzeR2(mV, int Ft, bool canBeNOP)
{
	if (!Ft)
	{
		if (canBeNOP)
			mVUlow.isNOP = 1;
		else
			mVUlow.noWriteVF = 1;
	}
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
	analyzeRreg();
}

//------------------------------------------------------------------
// Sflag - Status Flag Opcodes
//------------------------------------------------------------------
__ri void flagSet(mV, bool setMacFlag)
{
	int curPC = iPC;
	int calcOPS = 0;

	//Check which ops need to do the flag settings, also check for runs of ops as they can do multiple calculations to get the sticky status flags (VP2)
	//Make sure we get the last 4 calculations (Bloody Roar 3, possibly others)
	for (int i = mVUcount, j = 0; i > 0; i--, j++)
	{
		j += mVUstall;
		incPC(-2);

		if (calcOPS >= 4 && mVUup.VF_write.reg)
			break;

		if (sFLAG.doFlag && (j >= 3))
		{
			if (setMacFlag)
				mFLAG.doFlag = 1;
			sFLAG.doNonSticky = 1;
			calcOPS++;
		}
	}

	iPC = curPC;
	setCode();
}

__ri void mVUanalyzeSflag(mV, int It)
{
	mVUlow.readFlags = true;
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
	if (!It)
	{
		mVUlow.isNOP = 1;
	}
	else
	{
		mVUinfo.swapOps = 1;
		flagSet(mVU, 0);
	}
}

__ri void mVUanalyzeFSSET(mV)
{
	mVUlow.isFSSET = 1;
	mVUlow.readFlags = true;
}

//------------------------------------------------------------------
// Mflag - Mac Flag Opcodes
//------------------------------------------------------------------

__ri void mVUanalyzeMflag(mV, int Is, int It)
{
	mVUlow.readFlags = true;
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
	if (!It)
	{
		mVUlow.isNOP = 1;
	}
	else
	{
		mVUinfo.swapOps = 1;
		flagSet(mVU, 1);
	}
}

//------------------------------------------------------------------
// Cflag - Clip Flag Opcodes
//------------------------------------------------------------------

__fi void mVUanalyzeCflag(mV, int It)
{
	mVUinfo.swapOps = 1;
	mVUlow.readFlags = true;
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
}

//------------------------------------------------------------------
// XGkick
//------------------------------------------------------------------

__fi void mVUanalyzeXGkick(mV, int Fs, int xCycles)
{
	mVUlow.isKick = true;
	mVUregs.xgkickcycles = 0;
	mVUlow.kickcycles = 0;
	analyzeVIreg1(mVU, Fs, mVUlow.VI_read[0]);
	if (!CHECK_XGKICKHACK)
	{
		analyzeXGkick1(); // Stall will cause mVUincCycles() to trigger pending xgkick
		analyzeXGkick2(xCycles);
	}
	// Note: Technically XGKICK should stall on the next instruction,
	// this code stalls on the same instruction. The only case where this
	// will be a problem with, is if you have very-specifically placed
	// FMxxx or FSxxx opcodes checking flags near this instruction AND
	// the XGKICK instruction stalls. No-game should be effected by
	// this minor difference.
}

//------------------------------------------------------------------
// Branches - Branch Opcodes
//------------------------------------------------------------------

// If the VI reg is modified directly before the branch, then the VI
// value read by the branch is the value the VI reg had at the start
// of the instruction 4 instructions ago (assuming no stalls).
// See: https://forums.pcsx2.net/Thread-blog-PS2-VU-Vector-Unit-Documentation-Part-1
static void analyzeBranchVI(mV, int xReg, bool& infoVar)
{
	if (!xReg)
		return;
	if (mVUstall) // I assume a stall on branch means the vi reg is not modified directly b4 the branch...
		return;
	int i, j = 0;
	int cyc  = 0;
	int iEnd = 4;
	int bPC  = iPC;
	incPC2(-2);
	for (i = 0; i < iEnd && cyc < iEnd; i++)
	{
		if (i == (int)mVUcount)
		{
			bool warn = false;

			if (i == 1)
				warn = true;

			if (mVUpBlock->pState.viBackUp == xReg)
			{
				if (i == 0)
					warn = true;

				infoVar = true;
				j = i;
				i++;
			}
			break; // if (warn), we don't have enough information to always guarantee the correct result.
		}
		if ((mVUlow.VI_write.reg == xReg) && mVUlow.VI_write.used)
		{
			if (mVUlow.readFlags)
				break; // Not sure if on the above "if (i)" case, if we need to "continue" or if we should "break"
			j = i;
		}
		else if (i == 0)
			break;
		cyc += mVUstall + 1;
		incPC2(-2);
	}

	if (i)
	{
		if (!infoVar)
		{
			iPC = bPC;
			incPC2(-2 * (j + 1));
			mVUlow.backupVI = true;
			infoVar = true;
		}
		iPC = bPC;
	}
	else
	{
		iPC = bPC;
	}
}

// Branch in Branch Delay-Slots
__ri int mVUbranchCheck(mV)
{
	if (!mVUcount && !isEvilBlock)
		return 0;

	// This means we have jumped from an evil branch situation, so this is another branch in delay slot
	if (isEvilBlock)
	{
		mVUlow.evilBranch = true;
		mVUregs.blockType = 2;
		mVUregs.needExactMatch |= 7; // This might not be necessary, but w/e...
		mVUregs.flagInfo = 0;
		return 1;
	}

	incPC(-2);

	if (mVUlow.branch)
	{
		u32 branchType = mVUlow.branch;
		if (doBranchInDelaySlot)
		{
			mVUlow.badBranch = true;
			incPC(2);
			mVUlow.evilBranch = true;

			mVUregs.blockType = 2; // Second branch doesn't need linking, so can let it run its evil block course (MGS2 for testing)

			mVUregs.needExactMatch |= 7; // This might not be necessary, but w/e...
			mVUregs.flagInfo = 0;
			return 1;
		}
		incPC(2);
		mVUlow.isNOP = true;
		return 0;
	}
	incPC(2);
	return 0;
}

__fi void mVUanalyzeCondBranch1(mV, int Is)
{
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	if (!mVUbranchCheck(mVU))
	{
		analyzeBranchVI(mVU, Is, mVUlow.memReadIs);
	}
}

__fi void mVUanalyzeCondBranch2(mV, int Is, int It)
{
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg1(mVU, It, mVUlow.VI_read[1]);
	if (!mVUbranchCheck(mVU))
	{
		analyzeBranchVI(mVU, Is, mVUlow.memReadIs);
		analyzeBranchVI(mVU, It, mVUlow.memReadIt);
	}
}

__fi void mVUanalyzeNormBranch(mV, int It, bool isBAL)
{
	mVUbranchCheck(mVU);
	if (isBAL)
	{
		analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
		if(!mVUlow.evilBranch)
			setConstReg(It, bSaveAddr);
	}
}

__ri void mVUanalyzeJump(mV, int Is, int It, bool isJALR)
{
	mVUlow.branch = (isJALR) ? 10 : 9;
	mVUbranchCheck(mVU);
	if (mVUconstReg[Is].isValid && doConstProp)
	{
		mVUlow.constJump.isValid  = 1;
		mVUlow.constJump.regValue = mVUconstReg[Is].regValue;
	}
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	if (isJALR)
	{
		analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
		if (!mVUlow.evilBranch)
			setConstReg(It, bSaveAddr);
	}
}

//------------------------------------------------------------------
// Micro VU - Pass 2 Functions
//------------------------------------------------------------------

//------------------------------------------------------------------
// Flag Allocators
//------------------------------------------------------------------

__fi static const x32& getFlagReg(uint fInst)
{
	static const x32* const gprFlags[4] = {&gprF0, &gprF1, &gprF2, &gprF3};
	return *gprFlags[fInst];
}

__fi void setBitSFLAG(const x32& reg, const x32& regT, int bitTest, int bitSet)
{
	xTEST(regT, bitTest);
	xForwardJZ8 skip;
	xOR(reg, bitSet);
	skip.SetTarget();
}

__fi void setBitFSEQ(const x32& reg, int bitX)
{
	xTEST(reg, bitX);
	xForwardJump8 skip(Jcc_Zero);
	xOR(reg, bitX);
	skip.SetTarget();
}

__fi void mVUallocSFLAGa(const x32& reg, int fInstance)
{
	xMOV(reg, getFlagReg(fInstance));
}

__fi void mVUallocSFLAGb(const x32& reg, int fInstance)
{
	xMOV(getFlagReg(fInstance), reg);
}

// Normalize Status Flag
__ri void mVUallocSFLAGc(const x32& reg, const x32& regT, int fInstance)
{
	xXOR(reg, reg);
	mVUallocSFLAGa(regT, fInstance);
	setBitSFLAG(reg, regT, 0x0f00, 0x0001); // Z  Bit
	setBitSFLAG(reg, regT, 0xf000, 0x0002); // S  Bit
	setBitSFLAG(reg, regT, 0x000f, 0x0040); // ZS Bit
	setBitSFLAG(reg, regT, 0x00f0, 0x0080); // SS Bit
	xAND(regT, 0xffff0000); // DS/DI/OS/US/D/I/O/U Bits
	xSHR(regT, 14);
	xOR(reg, regT);
}

// Denormalizes Status Flag; destroys tmp1/tmp2
__ri void mVUallocSFLAGd(u32* memAddr, const x32& reg = eax, const x32& tmp1 = ecx, const x32& tmp2 = edx)
{
	xMOV(tmp2, ptr32[memAddr]);
	xMOV(reg, tmp2);
	xSHR(reg, 3);
	xAND(reg, 0x18);

	xMOV(tmp1, tmp2);
	xSHL(tmp1, 11);
	xAND(tmp1, 0x1800);
	xOR(reg, tmp1);

	xSHL(tmp2, 14);
	xAND(tmp2, 0x3cf0000);
	xOR(reg, tmp2);
}

__fi void mVUallocMFLAGa(mV, const x32& reg, int fInstance)
{
	xMOVZX(reg, ptr16[&mVU.macFlag[fInstance]]);
}

__fi void mVUallocMFLAGb(mV, const x32& reg, int fInstance)
{
	//xAND(reg, 0xffff);
	if (fInstance < 4) xMOV(ptr32[&mVU.macFlag[fInstance]], reg);         // microVU
	else               xMOV(ptr32[&vuRegs[mVU.index].VI[REG_MAC_FLAG].UL], reg); // macroVU
}

__fi void mVUallocCFLAGa(mV, const x32& reg, int fInstance)
{
	if (fInstance < 4) xMOV(reg, ptr32[&mVU.clipFlag[fInstance]]);         // microVU
	else               xMOV(reg, ptr32[&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL]); // macroVU
}

__fi void mVUallocCFLAGb(mV, const x32& reg, int fInstance)
{
	if (fInstance < 4) xMOV(ptr32[&mVU.clipFlag[fInstance]], reg);         // microVU
	else               xMOV(ptr32[&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL], reg); // macroVU
}

//------------------------------------------------------------------
// VI Reg Allocators
//------------------------------------------------------------------

void microRegAlloc::writeVIBackup(const xRegisterInt& reg)
{
	microVU& mVU = index ? microVU1 : microVU0;
	xMOV(ptr32[&mVU.VIbackup], xRegister32(reg));
}

//------------------------------------------------------------------
// P/Q Reg Allocators
//------------------------------------------------------------------

__fi void getPreg(mV, const xmm& reg)
{
	mVUunpack_xyzw(reg, xmmPQ, (2 + mVUinfo.readP));
}

__fi void getQreg(const xmm& reg, int qInstance)
{
	mVUunpack_xyzw(reg, xmmPQ, qInstance);
}

__ri void writeQreg(const xmm& reg, int qInstance)
{
	if (qInstance)
		xINSERTPS(xmmPQ, reg, _MM_MK_INSERTPS_NDX(0, 1, 0));
	else
		xMOVSS(xmmPQ, reg);
}

//------------------------------------------------------------------
// mVUupdateFlags() - Updates status/mac flags
//------------------------------------------------------------------

#define AND_XYZW ((_XYZW_SS && modXYZW) ? (1) : (mFLAG.doFlag ? (_X_Y_Z_W) : (flipMask[_X_Y_Z_W])))
#define ADD_XYZW ((_XYZW_SS && modXYZW) ? (_X ? 3 : (_Y ? 2 : (_Z ? 1 : 0))) : 0)
#define SHIFT_XYZW(gprReg) \
	do { \
		if (_XYZW_SS && modXYZW && !_W) \
		{ \
			xSHL(gprReg, ADD_XYZW); \
		} \
	} while (0)


// Note: If modXYZW is true, then it adjusts XYZW for Single Scalar operations
static void mVUupdateFlags(mV, const xmm& reg, const xmm& regT1in = xEmptyReg, const xmm& regT2in = xEmptyReg, bool modXYZW = 1)
{
	const x32& mReg = gprT1;
	const x32& sReg = getFlagReg(sFLAG.write);
	bool regT1b = regT1in.IsEmpty(), regT2b = false;
	static const u16 flipMask[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

	if (!sFLAG.doFlag && !mFLAG.doFlag)
		return;

	const xmm& regT1 = regT1b ? mVU.regAlloc->allocReg() : regT1in;

	xmm regT2 = reg;
	if ((mFLAG.doFlag && !(_XYZW_SS && modXYZW)))
	{
		regT2 = regT2in;
		if (regT2.IsEmpty())
		{
			regT2 = mVU.regAlloc->allocReg();
			regT2b = true;
		}
		xPSHUF.D(regT2, reg, 0x1B); // Flip wzyx to xyzw
	}
	else
		regT2 = reg;

	if (sFLAG.doFlag)
	{
		mVUallocSFLAGa(sReg, sFLAG.lastWrite); // Get Prev Status Flag
		if (sFLAG.doNonSticky)
			xAND(sReg, 0xfffc00ff); // Clear O,U,S,Z flags
	}

	//-------------------------Check for Signed flags------------------------------

	xMOVMSKPS(mReg,  regT2); // Move the Sign Bits of the t2reg
	xXOR.PS  (regT1, regT1); // Clear regT1
	xCMPEQ.PS(regT1, regT2); // Set all F's if each vector is zero
	xMOVMSKPS(gprT2, regT1); // Used for Zero Flag Calculation

	xAND(mReg, AND_XYZW); // Grab "Is Signed" bits from the previous calculation
	xSHL(mReg, 4);

	//-------------------------Check for Zero flags------------------------------

	xAND(gprT2, AND_XYZW); // Grab "Is Zero" bits from the previous calculation
	xOR(mReg, gprT2);

	//-------------------------Overflow Flags-----------------------------------
	// We can't really do this because of the limited range of x86 and the value MIGHT genuinely be FLT_MAX (x86)
	// so this will need to remain as a gamefix for the one game that needs it (Superman Returns)
	// until some sort of soft float implementation.
	if (sFLAG.doFlag && CHECK_VUOVERFLOWHACK)
	{
		alignas(16) const u32 sse4_compvals[2][4] = {
			{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}, //1111
			{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}, //1111
		};
		//Calculate overflow
		xMOVAPS(regT1, regT2);
		xAND.PS(regT1, ptr128[&sse4_compvals[1][0]]); // Remove sign flags (we don't care)
		xCMPNLT.PS(regT1, ptr128[&sse4_compvals[0][0]]); // Compare if T1 == FLT_MAX
		xMOVMSKPS(gprT2, regT1); // Grab sign bits  for equal results
		xAND(gprT2, AND_XYZW); // Grab "Is FLT_MAX" bits from the previous calculation
		xForwardJump32 oJMP(Jcc_Zero);

		xOR(sReg, 0x820000);
		if (mFLAG.doFlag)
		{
			xSHL(gprT2, 12); // Add the results to the MAC Flag
			xOR(mReg, gprT2);
		}

		oJMP.SetTarget();
	}

	//-------------------------Write back flags------------------------------
	if (mFLAG.doFlag)
	{
		SHIFT_XYZW(mReg); // If it was Single Scalar, move the flags in to the correct position
		mVUallocMFLAGb(mVU, mReg, mFLAG.write); // Set Mac Flag
	}
	if (sFLAG.doFlag)
	{
		xAND(mReg, 0xFF); // Ignore overflow bits, they're handled separately
		xOR(sReg, mReg);
		if (sFLAG.doNonSticky)
		{
			xSHL(mReg, 8);
			xOR(sReg, mReg);
		}
	}
	if (regT1b)
		mVU.regAlloc->clearNeeded(regT1);
	if (regT2b)
		mVU.regAlloc->clearNeeded(regT2);
}

//------------------------------------------------------------------
// Helper Macros and Functions
//------------------------------------------------------------------

static void (*const SSE_PS[])(microVU&, const xmm&, const xmm&, const xmm&, const xmm&) = {
	SSE_ADDPS, // 0
	SSE_SUBPS, // 1
	SSE_MULPS, // 2
	SSE_MAXPS, // 3
	SSE_MINPS, // 4
	SSE_ADD2PS // 5
};

static void (*const SSE_SS[])(microVU&, const xmm&, const xmm&, const xmm&, const xmm&) = {
	SSE_ADDSS, // 0
	SSE_SUBSS, // 1
	SSE_MULSS, // 2
	SSE_MAXSS, // 3
	SSE_MINSS, // 4
	SSE_ADD2SS // 5
};

enum clampModes
{
	cFt = 0x01, // Clamp Ft / I-reg / Q-reg
	cFs = 0x02, // Clamp Fs
	cACC = 0x04, // Clamp ACC
};

// Sets Up Pass1 Info for Normal, BC, I, and Q Cases
static void setupPass1(microVU& mVU, int opCase, bool isACC, bool noFlagUpdate)
{
	opCase1 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, _Ft_); }
	opCase2 { mVUanalyzeFMAC3(mVU, ((isACC) ? 0 : _Fd_), _Fs_, _Ft_); }
	opCase3 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, 0); }
	opCase4 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, 0); }

	if (noFlagUpdate) //Max/Min Ops
		sFLAG.doFlag = false;
}

// Safer to force 0 as the result for X minus X than to do actual subtraction
static bool doSafeSub(microVU& mVU, int opCase, int opType, bool isACC)
{
	opCase1
	{
		if ((opType == 1) && (_Ft_ == _Fs_) && (opCase == 1)) // Don't do this with BC's!
		{
			const xmm& Fs = mVU.regAlloc->allocReg(-1, isACC ? 32 : _Fd_, _X_Y_Z_W);
			xPXOR(Fs, Fs); // Set to Positive 0
			mVUupdateFlags(mVU, Fs);
			mVU.regAlloc->clearNeeded(Fs);
			return true;
		}
	}
	return false;
}

// Sets Up Ft Reg for Normal, BC, I, and Q Cases
static void setupFtReg(microVU& mVU, xmm& Ft, xmm& tempFt, int opCase, int clampType)
{
	opCase1
	{
		// Based on mVUclamp2 -> mVUclamp1 below.
		const bool willClamp = (clampE || ((clampType & cFt) && !clampE && (CHECK_VU_OVERFLOW(mVU.index) || CHECK_VU_SIGN_OVERFLOW(mVU.index))));

		if (_XYZW_SS2)      { Ft = mVU.regAlloc->allocReg(_Ft_, 0, _X_Y_Z_W); tempFt = Ft; }
		else if (willClamp) { Ft = mVU.regAlloc->allocReg(_Ft_, 0, 0xf);      tempFt = Ft; }
		else                { Ft = mVU.regAlloc->allocReg(_Ft_);              tempFt = xEmptyReg;  }
	}
	opCase2
	{
		tempFt = mVU.regAlloc->allocReg(_Ft_);
		Ft     = mVU.regAlloc->allocReg();
		mVUunpack_xyzw(Ft, tempFt, _bc_);
		mVU.regAlloc->clearNeeded(tempFt);
		tempFt = Ft;
	}
	opCase3
	{
		Ft = mVU.regAlloc->allocReg(33, 0, _X_Y_Z_W);
		tempFt = Ft;
	}
	opCase4
	{
		if (!clampE && _XYZW_SS && !mVUinfo.readQ)
		{
			Ft = xmmPQ;
			tempFt = xEmptyReg;
		}
		else
		{
			Ft = mVU.regAlloc->allocReg();
			tempFt = Ft;
			getQreg(Ft, mVUinfo.readQ);
		}
	}
}

// Normal FMAC Opcodes
static void mVU_FMACa(microVU& mVU, int recPass, int opCase, int opType, bool isACC, int clampType)
{
	pass1 { setupPass1(mVU, opCase, isACC, ((opType == 3) || (opType == 4))); }
	pass2
	{
		if (doSafeSub(mVU, opCase, opType, isACC))
			return;

		xmm Fs, Ft, ACC, tempFt;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType);

		if (isACC)
		{
			Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
			ACC = mVU.regAlloc->allocReg((_X_Y_Z_W == 0xf) ? -1 : 32, 32, 0xf, 0);
			if (_XYZW_SS2)
				xPSHUF.D(ACC, ACC, shuffleSS(_X_Y_Z_W));
		}
		else
		{
			Fs = mVU.regAlloc->allocReg(_Fs_, _Fd_, _X_Y_Z_W);
		}

		if (clampType & cFt) mVUclamp2(mVU, Ft, xEmptyReg, _X_Y_Z_W);
		if (clampType & cFs) mVUclamp2(mVU, Fs, xEmptyReg, _X_Y_Z_W);

		if (_XYZW_SS) SSE_SS[opType](mVU, Fs, Ft, xEmptyReg, xEmptyReg);
		else          SSE_PS[opType](mVU, Fs, Ft, xEmptyReg, xEmptyReg);

		if (isACC)
		{
			if (_XYZW_SS)
				xMOVSS(ACC, Fs);
			else
				mVUmergeRegs(ACC, Fs, _X_Y_Z_W);
			mVUupdateFlags(mVU, ACC, Fs, tempFt);
			if (_XYZW_SS2)
				xPSHUF.D(ACC, ACC, shuffleSS(_X_Y_Z_W));
			mVU.regAlloc->clearNeeded(ACC);
		}
		else if (opType < 3 || opType == 5) // Not Min/Max or is ADDi(5) (TODO: Reorganise this so its < 4 including ADDi)
			mVUupdateFlags(mVU, Fs, tempFt);

		mVU.regAlloc->clearNeeded(Fs); // Always Clear Written Reg First
		mVU.regAlloc->clearNeeded(Ft);
	}
	pass4
	{
		if ((opType != 3) && (opType != 4))
			mVUregs.needExactMatch |= 8;
	}
}

// MADDA/MSUBA Opcodes
static void mVU_FMACb(microVU& mVU, int recPass, int opCase, int opType, int clampType)
{
	pass1 { setupPass1(mVU, opCase, true, false); }
	pass2
	{
		xmm Fs, Ft, ACC, tempFt;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType);

		Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		ACC = mVU.regAlloc->allocReg(32, 32, 0xf, false);

		if (_XYZW_SS2)
			xPSHUF.D(ACC, ACC, shuffleSS(_X_Y_Z_W));

		if (clampType & cFt) mVUclamp2(mVU, Ft, xEmptyReg, _X_Y_Z_W);
		if (clampType & cFs) mVUclamp2(mVU, Fs, xEmptyReg, _X_Y_Z_W);

		if (_XYZW_SS) SSE_SS[2](mVU, Fs, Ft, xEmptyReg, xEmptyReg);
		else          SSE_PS[2](mVU, Fs, Ft, xEmptyReg, xEmptyReg);

		if (_XYZW_SS || _X_Y_Z_W == 0xf)
		{
			if (_XYZW_SS) SSE_SS[opType](mVU, ACC, Fs, tempFt, xEmptyReg);
			else          SSE_PS[opType](mVU, ACC, Fs, tempFt, xEmptyReg);
			mVUupdateFlags(mVU, ACC, Fs, tempFt);
			if (_XYZW_SS && _X_Y_Z_W != 8)
				xPSHUF.D(ACC, ACC, shuffleSS(_X_Y_Z_W));
		}
		else
		{
			const xmm& tempACC = mVU.regAlloc->allocReg();
			xMOVAPS(tempACC, ACC);
			SSE_PS[opType](mVU, tempACC, Fs, tempFt, xEmptyReg);
			mVUmergeRegs(ACC, tempACC, _X_Y_Z_W);
			mVUupdateFlags(mVU, ACC, Fs, tempFt);
			mVU.regAlloc->clearNeeded(tempACC);
		}

		mVU.regAlloc->clearNeeded(ACC);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

// MADD Opcodes
static void mVU_FMACc(microVU& mVU, int recPass, int opCase, int clampType)
{
	pass1 { setupPass1(mVU, opCase, false, false); }
	pass2
	{
		xmm Fs, Ft, ACC, tempFt;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType);

		ACC = mVU.regAlloc->allocReg(32);
		Fs = mVU.regAlloc->allocReg(_Fs_, _Fd_, _X_Y_Z_W);

		if (_XYZW_SS2)
			xPSHUF.D(ACC, ACC, shuffleSS(_X_Y_Z_W));

		if (clampType & cFt)  mVUclamp2(mVU, Ft,  xEmptyReg, _X_Y_Z_W);
		if (clampType & cFs)  mVUclamp2(mVU, Fs,  xEmptyReg, _X_Y_Z_W);
		if (clampType & cACC) mVUclamp2(mVU, ACC, xEmptyReg, _X_Y_Z_W);


		if (_XYZW_SS) { SSE_SS[2](mVU, Fs, Ft, xEmptyReg, xEmptyReg); SSE_SS[0](mVU, Fs, ACC, tempFt, xEmptyReg); }
		else          { SSE_PS[2](mVU, Fs, Ft, xEmptyReg, xEmptyReg); SSE_PS[0](mVU, Fs, ACC, tempFt, xEmptyReg); }

		if (_XYZW_SS2)
			xPSHUF.D(ACC, ACC, shuffleSS(_X_Y_Z_W));

		mVUupdateFlags(mVU, Fs, tempFt);

		mVU.regAlloc->clearNeeded(Fs); // Always Clear Written Reg First
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(ACC);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

// MSUB Opcodes
static void mVU_FMACd(microVU& mVU, int recPass, int opCase, int clampType)
{
	pass1 { setupPass1(mVU, opCase, false, false); }
	pass2
	{
		xmm Fs, Ft, Fd, tempFt;
		setupFtReg(mVU, Ft, tempFt, opCase, clampType);

		Fs = mVU.regAlloc->allocReg(_Fs_,  0, _X_Y_Z_W);
		Fd = mVU.regAlloc->allocReg(32, _Fd_, _X_Y_Z_W);

		if (clampType & cFt)  mVUclamp2(mVU, Ft, xEmptyReg, _X_Y_Z_W);
		if (clampType & cFs)  mVUclamp2(mVU, Fs, xEmptyReg, _X_Y_Z_W);
		if (clampType & cACC) mVUclamp2(mVU, Fd, xEmptyReg, _X_Y_Z_W);

		if (_XYZW_SS) { SSE_SS[2](mVU, Fs, Ft, xEmptyReg, xEmptyReg); SSE_SS[1](mVU, Fd, Fs, tempFt, xEmptyReg); }
		else          { SSE_PS[2](mVU, Fs, Ft, xEmptyReg, xEmptyReg); SSE_PS[1](mVU, Fd, Fs, tempFt, xEmptyReg); }

		mVUupdateFlags(mVU, Fd, Fs, tempFt);

		mVU.regAlloc->clearNeeded(Fd); // Always Clear Written Reg First
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(Fs);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

// ABS Opcode
mVUop(mVU_ABS)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		if (!_Ft_)
			return;
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));
		xAND.PS(Fs, ptr128[mVUglob.absclip]);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

// OPMULA Opcode
mVUop(mVU_OPMULA)
{
	pass1 { mVUanalyzeFMAC1(mVU, 0, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, _X_Y_Z_W);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 32, _X_Y_Z_W);

		xPSHUF.D(Fs, Fs, 0xC9); // WXZY
		xPSHUF.D(Ft, Ft, 0xD2); // WYXZ
		SSE_MULPS(mVU, Fs, Ft);
		mVU.regAlloc->clearNeeded(Ft);
		mVUupdateFlags(mVU, Fs);
		mVU.regAlloc->clearNeeded(Fs);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

// OPMSUB Opcode
mVUop(mVU_OPMSUB)
{
	pass1 { mVUanalyzeFMAC1(mVU, _Fd_, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& ACC = mVU.regAlloc->allocReg(32, _Fd_, _X_Y_Z_W);

		xPSHUF.D(Fs, Fs, 0xC9); // WXZY
		xPSHUF.D(Ft, Ft, 0xD2); // WYXZ
		SSE_MULPS(mVU, Fs,  Ft);
		SSE_SUBPS(mVU, ACC, Fs);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVUupdateFlags(mVU, ACC);
		mVU.regAlloc->clearNeeded(ACC);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

// FTOI0/FTIO4/FTIO12/FTIO15 Opcodes
static void mVU_FTOIx(mP, const float* addr)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		if (!_Ft_)
			return;
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();

		// Note: For help understanding this algorithm see recVUMI_FTOI_Saturate()
		xMOVAPS(t1, Fs);
		if (addr)
			xMUL.PS(Fs, ptr128[addr]);
		xCVTTPS2DQ(Fs, Fs);
		xPXOR(t1, ptr128[mVUglob.signbit]);
		xPSRA.D(t1, 31);
		xMOVAPS(t2, Fs);
		xPCMP.EQD(t2, ptr128[mVUglob.signbit]);
		xAND.PS(t1, t2);
		xPADD.D(Fs, t1);

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

// ITOF0/ITOF4/ITOF12/ITOF15 Opcodes
static void mVU_ITOFx(mP, const float* addr)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		if (!_Ft_)
			return;
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));

		xCVTDQ2PS(Fs, Fs);
		if (addr)
			xMUL.PS(Fs, ptr128[addr]);
		//mVUclamp2(Fs, xmmT1, 15); // Clamp (not sure if this is needed)

		mVU.regAlloc->clearNeeded(Fs);
	}
}

// Clip Opcode
mVUop(mVU_CLIP)
{
	pass1 { mVUanalyzeFMAC4(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, 0x1);
		const xmm& t1 = mVU.regAlloc->allocReg();

		mVUunpack_xyzw(Ft, Ft, 0);
		mVUallocCFLAGa(mVU, gprT1, cFLAG.lastWrite);
		xSHL(gprT1, 6);

		xAND.PS(Ft, ptr128[mVUglob.absclip]);
		xMOVAPS(t1, Ft);
		xPOR(t1, ptr128[mVUglob.signbit]);

		xCMPNLE.PS(t1, Fs); // -w, -z, -y, -x
		xCMPLT.PS(Ft, Fs);  // +w, +z, +y, +x

		xMOVAPS(Fs, Ft);    // Fs = +w, +z, +y, +x
		xUNPCK.LPS(Ft, t1); // Ft = -y,+y,-x,+x
		xUNPCK.HPS(Fs, t1); // Fs = -w,+w,-z,+z

		xMOVMSKPS(gprT2, Fs); // -w,+w,-z,+z
		xAND(gprT2, 0x3);
		xSHL(gprT2, 4);
		xOR(gprT1, gprT2);

		xMOVMSKPS(gprT2, Ft); // -y,+y,-x,+x
		xAND(gprT2, 0xf);
		xOR(gprT1, gprT2);
		xAND(gprT1, 0xffffff);

		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
	}
}

//------------------------------------------------------------------
// Micro VU Micromode Upper instructions
//------------------------------------------------------------------

mVUop(mVU_ADD)    { mVU_FMACa(mVU, recPass, 1, 0, false,    0);  }
mVUop(mVU_ADDi)   { mVU_FMACa(mVU, recPass, 3, 5, false,   0);  }
mVUop(mVU_ADDq)   { mVU_FMACa(mVU, recPass, 4, 0, false,   0);  }
mVUop(mVU_ADDx)   { mVU_FMACa(mVU, recPass, 2, 0, false,   0);  }
mVUop(mVU_ADDy)   { mVU_FMACa(mVU, recPass, 2, 0, false,   0);  }
mVUop(mVU_ADDz)   { mVU_FMACa(mVU, recPass, 2, 0, false,   0);  }
mVUop(mVU_ADDw)   { mVU_FMACa(mVU, recPass, 2, 0, false,   0);  }
mVUop(mVU_ADDA)   { mVU_FMACa(mVU, recPass, 1, 0, true,   0);  }
mVUop(mVU_ADDAi)  { mVU_FMACa(mVU, recPass, 3, 0, true,  0);  }
mVUop(mVU_ADDAq)  { mVU_FMACa(mVU, recPass, 4, 0, true,  0);  }
mVUop(mVU_ADDAx)  { mVU_FMACa(mVU, recPass, 2, 0, true,  0);  }
mVUop(mVU_ADDAy)  { mVU_FMACa(mVU, recPass, 2, 0, true,  0);  }
mVUop(mVU_ADDAz)  { mVU_FMACa(mVU, recPass, 2, 0, true,  0);  }
mVUop(mVU_ADDAw)  { mVU_FMACa(mVU, recPass, 2, 0, true,  0);  }
mVUop(mVU_SUB)    { mVU_FMACa(mVU, recPass, 1, 1, false,  (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBi)   { mVU_FMACa(mVU, recPass, 3, 1, false, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBq)   { mVU_FMACa(mVU, recPass, 4, 1, false, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBx)   { mVU_FMACa(mVU, recPass, 2, 1, false, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBy)   { mVU_FMACa(mVU, recPass, 2, 1, false, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBz)   { mVU_FMACa(mVU, recPass, 2, 1, false, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBw)   { mVU_FMACa(mVU, recPass, 2, 1, false, (_XYZW_PS)?(cFs|cFt):0);   } // Clamp (Kingdom Hearts I (VU0))
mVUop(mVU_SUBA)   { mVU_FMACa(mVU, recPass, 1, 1, true,   0);  }
mVUop(mVU_SUBAi)  { mVU_FMACa(mVU, recPass, 3, 1, true,  0);  }
mVUop(mVU_SUBAq)  { mVU_FMACa(mVU, recPass, 4, 1, true,  0);  }
mVUop(mVU_SUBAx)  { mVU_FMACa(mVU, recPass, 2, 1, true,  0);  }
mVUop(mVU_SUBAy)  { mVU_FMACa(mVU, recPass, 2, 1, true,  0);  }
mVUop(mVU_SUBAz)  { mVU_FMACa(mVU, recPass, 2, 1, true,  0);  }
mVUop(mVU_SUBAw)  { mVU_FMACa(mVU, recPass, 2, 1, true,  0);  }
mVUop(mVU_MUL)    { mVU_FMACa(mVU, recPass, 1, 2, false,  (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULi)   { mVU_FMACa(mVU, recPass, 3, 2, false, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULq)   { mVU_FMACa(mVU, recPass, 4, 2, false, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULx)   { mVU_FMACa(mVU, recPass, 2, 2, false, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (vu0))
mVUop(mVU_MULy)   { mVU_FMACa(mVU, recPass, 2, 2, false, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULz)   { mVU_FMACa(mVU, recPass, 2, 2, false, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULw)   { mVU_FMACa(mVU, recPass, 2, 2, false, (_XYZW_PS)?(cFs|cFt):cFs); } // Clamp (TOTA, DoM, Ice Age (VU0))
mVUop(mVU_MULA)   { mVU_FMACa(mVU, recPass, 1, 2, true,   0);  }
mVUop(mVU_MULAi)  { mVU_FMACa(mVU, recPass, 3, 2, true,  0);  }
mVUop(mVU_MULAq)  { mVU_FMACa(mVU, recPass, 4, 2, true,  0);  }
mVUop(mVU_MULAx)  { mVU_FMACa(mVU, recPass, 2, 2, true,  cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MULAy)  { mVU_FMACa(mVU, recPass, 2, 2, true,  cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MULAz)  { mVU_FMACa(mVU, recPass, 2, 2, true,  cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MULAw)  { mVU_FMACa(mVU, recPass, 2, 2, true, (_XYZW_PS) ? (cFs | cFt) : cFs); } // Clamp (TOTA, DoM, ...)- Ft for Superman - Shadow Of Apokolips
mVUop(mVU_MADD)   { mVU_FMACc(mVU, recPass, 1,   0); }
mVUop(mVU_MADDi)  { mVU_FMACc(mVU, recPass, 3,  0); }
mVUop(mVU_MADDq)  { mVU_FMACc(mVU, recPass, 4,  0); }
mVUop(mVU_MADDx)  { mVU_FMACc(mVU, recPass, 2,  cFs); } // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDy)  { mVU_FMACc(mVU, recPass, 2,  cFs); } // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDz)  { mVU_FMACc(mVU, recPass, 2,  cFs); } // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDw)  { mVU_FMACc(mVU, recPass, 2, (isCOP2)?(cACC|cFt|cFs):cFs);} // Clamp (ICO (COP2), TOTA, DoM)
mVUop(mVU_MADDA)  { mVU_FMACb(mVU, recPass, 1, 0, 0);  }
mVUop(mVU_MADDAi) { mVU_FMACb(mVU, recPass, 3, 0, 0);  }
mVUop(mVU_MADDAq) { mVU_FMACb(mVU, recPass, 4, 0, 0);  }
mVUop(mVU_MADDAx) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDAy) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDAz) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MADDAw) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} // Clamp (TOTA, DoM, ...)
mVUop(mVU_MSUB)   { mVU_FMACd(mVU, recPass, 1, (isCOP2) ? cFs : 0); } // Clamp ( Superman - Shadow Of Apokolips)
mVUop(mVU_MSUBi)  { mVU_FMACd(mVU, recPass, 3, 0);  }
mVUop(mVU_MSUBq)  { mVU_FMACd(mVU, recPass, 4, 0);  }
mVUop(mVU_MSUBx)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBy)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBz)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBw)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBA)  { mVU_FMACb(mVU, recPass, 1, 1, 0);  }
mVUop(mVU_MSUBAi) { mVU_FMACb(mVU, recPass, 3, 1, 0);  }
mVUop(mVU_MSUBAq) { mVU_FMACb(mVU, recPass, 4, 1, 0);  }
mVUop(mVU_MSUBAx) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MSUBAy) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MSUBAz) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MSUBAw) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MAX)    { mVU_FMACa(mVU, recPass, 1, 3, false,    0);  }
mVUop(mVU_MAXi)   { mVU_FMACa(mVU, recPass, 3, 3, false,   0);  }
mVUop(mVU_MAXx)   { mVU_FMACa(mVU, recPass, 2, 3, false,   0);  }
mVUop(mVU_MAXy)   { mVU_FMACa(mVU, recPass, 2, 3, false,   0);  }
mVUop(mVU_MAXz)   { mVU_FMACa(mVU, recPass, 2, 3, false,   0);  }
mVUop(mVU_MAXw)   { mVU_FMACa(mVU, recPass, 2, 3, false,   0);  }
mVUop(mVU_MINI)   { mVU_FMACa(mVU, recPass, 1, 4, false,   0);  }
mVUop(mVU_MINIi)  { mVU_FMACa(mVU, recPass, 3, 4, false,  0);  }
mVUop(mVU_MINIx)  { mVU_FMACa(mVU, recPass, 2, 4, false,  0);  }
mVUop(mVU_MINIy)  { mVU_FMACa(mVU, recPass, 2, 4, false,  0);  }
mVUop(mVU_MINIz)  { mVU_FMACa(mVU, recPass, 2, 4, false,  0);  }
mVUop(mVU_MINIw)  { mVU_FMACa(mVU, recPass, 2, 4, false,  0);  }
mVUop(mVU_FTOI0)  { mVU_FTOIx(mX, NULL);      }
mVUop(mVU_FTOI4)  { mVU_FTOIx(mX, mVUglob.FTOI_4);      }
mVUop(mVU_FTOI12) { mVU_FTOIx(mX, mVUglob.FTOI_12);     }
mVUop(mVU_FTOI15) { mVU_FTOIx(mX, mVUglob.FTOI_15);     }
mVUop(mVU_ITOF0)  { mVU_ITOFx(mX, NULL);      }
mVUop(mVU_ITOF4)  { mVU_ITOFx(mX, mVUglob.ITOF_4);      }
mVUop(mVU_ITOF12) { mVU_ITOFx(mX, mVUglob.ITOF_12);     }
mVUop(mVU_ITOF15) { mVU_ITOFx(mX, mVUglob.ITOF_15);     }
mVUop(mVU_NOP)    { }

//------------------------------------------------------------------
// Micro VU Micromode Lower instructions
//------------------------------------------------------------------

//------------------------------------------------------------------
// DIV/SQRT/RSQRT
//------------------------------------------------------------------

// Test if Vector is +/- Zero
static __fi void testZero(const xmm& xmmReg, const xmm& xmmTemp, const x32& gprTemp)
{
	xXOR.PS(xmmTemp, xmmTemp);
	xCMPEQ.SS(xmmTemp, xmmReg);
	xPTEST(xmmTemp, xmmTemp);
}

// Test if Vector is Negative (Set Flags and Makes Positive)
static __fi void testNeg(mV, const xmm& xmmReg, const x32& gprTemp)
{
	xMOVMSKPS(gprTemp, xmmReg);
	xTEST(gprTemp, 1);
	xForwardJZ8 skip;
		xMOV(ptr32[&mVU.divFlag], divI);
		xAND.PS(xmmReg, ptr128[mVUglob.absclip]);
	skip.SetTarget();
}

mVUop(mVU_DIV)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 7); }
	pass2
	{
		xmm Ft;
		if (_Ftf_) Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		else       Ft = mVU.regAlloc->allocReg(_Ft_);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		testZero(Ft, t1, gprT1); // Test if Ft is zero
		xForwardJZ8 cjmp; // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			xForwardJZ8 ajmp;
				xMOV(ptr32[&mVU.divFlag], divI); // Set invalid flag (0/0)
				xForwardJump8 bjmp;
			ajmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD); // Zero divide (only when not 0/0)
			bjmp.SetTarget();

			xXOR.PS(Fs, Ft);
			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS (Fs, ptr128[mVUglob.maxvals]); // If division by zero, then xmmFs = +/- fmax

			xForwardJump8 djmp;
		cjmp.SetTarget();
			xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
	}
}

mVUop(mVU_SQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, 0, 0, _Ft_, _Ftf_, 7); }
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));

		xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		if (CHECK_VU_OVERFLOW(mVU.index)) // Clamp infinities (only need to do positive clamp since xmmFt is positive)
			xMIN.SS(Ft, ptr32[mVUglob.maxvals]);
		xSQRT.SS(Ft, Ft);
		writeQreg(Ft, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_RSQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 13); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		xMOV(ptr32[&mVU.divFlag], 0); // Clear I/D flags
		testNeg(mVU, Ft, gprT1); // Check for negative sqrt

		xSQRT.SS(Ft, Ft);
		testZero(Ft, t1, gprT1); // Test if Ft is zero
		xForwardJZ8 ajmp; // Skip if not zero

			testZero(Fs, t1, gprT1); // Test if Fs is zero
			xForwardJZ8 bjmp; // Skip if none are
				xMOV(ptr32[&mVU.divFlag], divI); // Set invalid flag (0/0)
				xForwardJump8 cjmp;
			bjmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD); // Zero divide flag (only when not 0/0)
			cjmp.SetTarget();

			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS(Fs, ptr128[mVUglob.maxvals]); // xmmFs = +/-Max

			xForwardJump8 djmp;
		ajmp.SetTarget();
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
	}
}

//------------------------------------------------------------------
// EATAN/EEXP/ELENG/ERCPR/ERLENG/ERSADD/ERSQRT/ESADD/ESIN/ESQRT/ESUM
//------------------------------------------------------------------

#define EATANhelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, PQ, t1); \
	}

// ToDo: Can Be Optimized Further? (takes approximately (~115 cycles + mem access time) on a c2d)
static __fi void mVU_EATAN_(mV, const xmm& PQ, const xmm& Fs, const xmm& t1, const xmm& t2)
{
	xMOVSS(PQ, Fs);
	xMUL.SS(PQ, ptr32[mVUglob.T1]);
	xMOVAPS(t2, Fs);
	EATANhelper(mVUglob.T2);
	EATANhelper(mVUglob.T3);
	EATANhelper(mVUglob.T4);
	EATANhelper(mVUglob.T5);
	EATANhelper(mVUglob.T6);
	EATANhelper(mVUglob.T7);
	EATANhelper(mVUglob.T8);
	xADD.SS(PQ, ptr32[mVUglob.Pi4]);
	xPSHUF.D(PQ, PQ, mVUinfo.writeP ? 0x27 : 0xC6);
}

mVUop(mVU_EATAN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 54);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS (xmmPQ, Fs);
		xSUB.SS(Fs,    ptr32[mVUglob.one]);
		xADD.SS(xmmPQ, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

mVUop(mVU_EATANxy)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x01);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1); // y-x, not y-1? ><
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

mVUop(mVU_EATANxz)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x02);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1);
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

#define eexpHelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, xmmPQ, t1); \
	}

mVUop(mVU_EEXP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 44);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS  (xmmPQ, Fs);
		xMUL.SS (xmmPQ, ptr32[mVUglob.E1]);
		xADD.SS (xmmPQ, ptr32[mVUglob.one]);
		xMOVAPS(t1, Fs);
		SSE_MULSS(mVU, t1, Fs);
		xMOVAPS(t2, t1);
		xMUL.SS(t1, ptr32[mVUglob.E2]);
		SSE_ADDSS(mVU, xmmPQ, t1);
		eexpHelper(&mVUglob.E3);
		eexpHelper(&mVUglob.E4);
		eexpHelper(&mVUglob.E5);
		SSE_MULSS(mVU, t2, Fs);
		xMUL.SS(t2, ptr32[mVUglob.E6]);
		SSE_ADDSS(mVU, xmmPQ, t2);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		xMOVSSZX(t2, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, t2, xmmPQ);
		xMOVSS(xmmPQ, t2);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

// sumXYZ(): PQ.x = x ^ 2 + y ^ 2 + z ^ 2
static __fi void mVU_sumXYZ(mV, const xmm& PQ, const xmm& Fs)
{
	xDP.PS(Fs, Fs, 0x71);
	xMOVSS(PQ, Fs);
}

mVUop(mVU_ELENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ERCPR)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS        (xmmPQ, Fs);
		xMOVSSZX      (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xMOVSS        (xmmPQ, Fs);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ERLENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 24);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ERSADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ERSQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xAND.PS       (Fs, ptr128[mVUglob.absclip]);
		xSQRT.SS      (xmmPQ, Fs);
		xMOVSSZX      (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xMOVSS        (xmmPQ, Fs);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ESADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 11);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ESIN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 29);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xMOVSS        (xmmPQ, Fs); // pq = X
		SSE_MULSS(mVU, Fs, Fs);    // fs = X^2
		xMOVAPS       (t1, Fs);    // t1 = X^2
		SSE_MULSS(mVU, Fs, xmmPQ); // fs = X^3
		xMOVAPS       (t2, Fs);    // t2 = X^3
		xMUL.SS       (Fs, ptr32[mVUglob.S2]); // fs = s2 * X^3
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3

		SSE_MULSS(mVU, t2, t1);    // t2 = X^3 * X^2
		xMOVAPS       (Fs, t2);    // fs = X^5
		xMUL.SS       (Fs, ptr32[mVUglob.S3]); // ps = s3 * X^5
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5

		SSE_MULSS(mVU, t2, t1);    // t2 = X^5 * X^2
		xMOVAPS       (Fs, t2);    // fs = X^7
		xMUL.SS       (Fs, ptr32[mVUglob.S4]); // fs = s4 * X^7
		SSE_ADDSS(mVU, xmmPQ, Fs); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7

		SSE_MULSS(mVU, t2, t1);    // t2 = X^7 * X^2
		xMUL.SS       (t2, ptr32[mVUglob.S5]); // t2 = s5 * X^9
		SSE_ADDSS(mVU, xmmPQ, t2); // pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7 + s5 * X^9
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
	}
}

mVUop(mVU_ESQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xAND.PS (Fs, ptr128[mVUglob.absclip]);
		xSQRT.SS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_ESUM)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		const xmm& t1 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip xmmPQ to get Valid P instance
		xPSHUF.D(t1, Fs, 0x1b);
		SSE_ADDPS(mVU, Fs, t1);
		xPSHUF.D(t1, Fs, 0x01);
		SSE_ADDSS(mVU, Fs, t1);
		xMOVSS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); // Flip back
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
	}
}

//------------------------------------------------------------------
// FCAND/FCEQ/FCGET/FCOR/FCSET
//------------------------------------------------------------------

mVUop(mVU_FCAND)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xAND(dst, _Imm24_);
		xADD(dst, 0xffffff);
		xSHR(dst, 24);
		mVU.regAlloc->clearNeeded(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCEQ)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xXOR(dst, _Imm24_);
		xSUB(dst, 1);
		xSHR(dst, 31);
		mVU.regAlloc->clearNeeded(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCGET)
{
	pass1 { mVUanalyzeCflag(mVU, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, regT, cFLAG.read);
		xAND(regT, 0xfff);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCOR)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xOR(dst, _Imm24_);
		xADD(dst, 1);  // If 24 1's will make 25th bit 1, else 0
		xSHR(dst, 24); // Get the 25th bit (also clears the rest of the garbage in the reg)
		mVU.regAlloc->clearNeeded(dst);
	}
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCSET)
{
	pass1 { cFLAG.doFlag = true; }
	pass2
	{
		xMOV(gprT1, _Imm24_);
		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
	}
}

//------------------------------------------------------------------
// FMAND/FMEQ/FMOR
//------------------------------------------------------------------

mVUop(mVU_FMAND)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xAND(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMEQ)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xXOR(regT, gprT1);
		xSUB(regT, 1);
		xSHR(regT, 31);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMOR)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xOR(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
	}
	pass4 { mVUregs.needExactMatch |= 2; }
}

//------------------------------------------------------------------
// FSAND/FSEQ/FSOR/FSSET
//------------------------------------------------------------------

mVUop(mVU_FSAND)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT1, sFLAG.read);
		xAND(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSOR)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT2, sFLAG.read);
		xOR(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSEQ)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0001) imm |= 0x0000f00; // Z
		if (_Imm12_ & 0x0002) imm |= 0x000f000; // S
		if (_Imm12_ & 0x0004) imm |= 0x0010000; // U
		if (_Imm12_ & 0x0008) imm |= 0x0020000; // O
		if (_Imm12_ & 0x0010) imm |= 0x0040000; // I
		if (_Imm12_ & 0x0020) imm |= 0x0080000; // D
		if (_Imm12_ & 0x0040) imm |= 0x000000f; // ZS
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; // SS
		if (_Imm12_ & 0x0100) imm |= 0x0400000; // US
		if (_Imm12_ & 0x0200) imm |= 0x0800000; // OS
		if (_Imm12_ & 0x0400) imm |= 0x1000000; // IS
		if (_Imm12_ & 0x0800) imm |= 0x2000000; // DS

		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGa(reg, sFLAG.read);
		setBitFSEQ(reg, 0x0f00); // Z  bit
		setBitFSEQ(reg, 0xf000); // S  bit
		setBitFSEQ(reg, 0x000f); // ZS bit
		setBitFSEQ(reg, 0x00f0); // SS bit
		xXOR(reg, imm);
		xSUB(reg, 1);
		xSHR(reg, 31);
		mVU.regAlloc->clearNeeded(reg);
	}
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSSET)
{
	pass1 { mVUanalyzeFSSET(mVU); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0040) imm |= 0x000000f; // ZS
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; // SS
		if (_Imm12_ & 0x0100) imm |= 0x0400000; // US
		if (_Imm12_ & 0x0200) imm |= 0x0800000; // OS
		if (_Imm12_ & 0x0400) imm |= 0x1000000; // IS
		if (_Imm12_ & 0x0800) imm |= 0x2000000; // DS
		if (!(sFLAG.doFlag || mVUinfo.doDivFlag))
		{
			mVUallocSFLAGa(getFlagReg(sFLAG.write), sFLAG.lastWrite); // Get Prev Status Flag
		}
		xAND(getFlagReg(sFLAG.write), 0xfff00); // Keep Non-Sticky Bits
		if (imm)
			xOR(getFlagReg(sFLAG.write), imm);
	}
}

//------------------------------------------------------------------
// IADD/IADDI/IADDIU/IAND/IOR/ISUB/ISUBIU
//------------------------------------------------------------------

mVUop(mVU_IADD)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_Is_ == 0 || _It_ == 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_ ? _Is_ : _It_, -1);
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xMOV(regD, regS);
			mVU.regAlloc->clearNeeded(regD);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xADD(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
	}
}

mVUop(mVU_IADDI)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm5_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (_Imm5_ != 0)
				xMOV(regT, _Imm5_);
			else
				xXOR(regT, regT);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (_Imm5_ != 0)
				xADD(regS, _Imm5_);
			mVU.regAlloc->clearNeeded(regS);
		}
	}
}

mVUop(mVU_IADDIU)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm15_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (_Imm15_ != 0)
				xMOV(regT, _Imm15_);
			else
				xXOR(regT, regT);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (_Imm15_ != 0)
				xADD(regS, _Imm15_);
			mVU.regAlloc->clearNeeded(regS);
		}
	}
}

mVUop(mVU_IAND)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xAND(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_IOR)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xOR(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_ISUB)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_It_ != _Is_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xSUB(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xXOR(regD, regD);
			mVU.regAlloc->clearNeeded(regD);
		}
	}
}

mVUop(mVU_ISUBIU)
{
	pass1 { mVUanalyzeIALU2(mVU, _Is_, _It_); }
	pass2
	{
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		if (_Imm15_ != 0)
			xSUB(regS, _Imm15_);
		mVU.regAlloc->clearNeeded(regS);
	}
}

//------------------------------------------------------------------
// MFIR/MFP/MOVE/MR32/MTIR
//------------------------------------------------------------------

mVUop(mVU_MFIR)
{
	pass1
	{
		if (!_Ft_)
		{
			mVUlow.isNOP = true;
		}
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeReg2  (mVU, _Ft_, mVUlow.VF_write, 1);
	}
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_Is_ != 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
			xMOVSX(xRegister32(regS), xRegister16(regS));
			// TODO: Broadcast instead
			xMOVDZX(Ft, regS);
			if (!_XYZW_SS)
				mVUunpack_xyzw(Ft, Ft, 0);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			xPXOR(Ft, Ft);
		}
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_MFP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeMFP(mVU, _Ft_);
	}
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		getPreg(mVU, Ft);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_MOVE)
{
	pass1 { mVUanalyzeMOVE(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_MR32)
{
	pass1 { mVUanalyzeMR32(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_);
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_XYZW_SS)
			mVUunpack_xyzw(Ft, Fs, (_X ? 1 : (_Y ? 2 : (_Z ? 3 : 0))));
		else
			xPSHUF.D(Ft, Fs, 0x39);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_MTIR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeReg5(mVU, _Fs_, _Fsf_, mVUlow.VF_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVD(regT, Fs);
		mVU.regAlloc->clearNeeded(regT);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

//------------------------------------------------------------------
// ILW/ILWR
//------------------------------------------------------------------

mVUop(mVU_ILW)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem + offsetSS;
		std::optional<xAddressVoid> optaddr(mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, offsetSS));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, ptr, gprT1q)]);
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_ILWR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem + offsetSS;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix (mVU, gprT1q);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[xComplexAddress(gprT2q, ptr, gprT1q)]);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[ptr]);
			mVU.regAlloc->clearNeeded(regT);
		}
	}
}

//------------------------------------------------------------------
// ISW/ISWR
//------------------------------------------------------------------

mVUop(mVU_ISW)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		std::optional<xAddressVoid> optaddr(mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		// If regT is dirty, the high bits might not be zero.
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		const xAddressVoid ptr(optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, vuRegs[mVU.index].Mem, gprT1q));
		if (_X) xMOV(ptr32[ptr], regT);
		if (_Y) xMOV(ptr32[ptr + 4], regT);
		if (_Z) xMOV(ptr32[ptr + 8], regT);
		if (_W) xMOV(ptr32[ptr + 12], regT);
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_ISWR)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		void* base = vuRegs[mVU.index].Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		if (!is.IsEmpty() && (intptr_t)base != (s32)(intptr_t)base)
		{
			int register_offset = -1;
			auto writeBackAt = [&](int offset)
			{
				if (register_offset == -1)
				{
					xLEA(gprT2q, ptr[(void*)((intptr_t)base + offset)]);
					register_offset = offset;
				}
				xMOV(ptr32[gprT2q + is + (offset - register_offset)], regT);
			};
			if (_X) writeBackAt(0);
			if (_Y) writeBackAt(4);
			if (_Z) writeBackAt(8);
			if (_W) writeBackAt(12);
		}
		else if (is.IsEmpty())
		{
			if (_X) xMOV(ptr32[(void*)((uintptr_t)base)], regT);
			if (_Y) xMOV(ptr32[(void*)((uintptr_t)base + 4)], regT);
			if (_Z) xMOV(ptr32[(void*)((uintptr_t)base + 8)], regT);
			if (_W) xMOV(ptr32[(void*)((uintptr_t)base + 12)], regT);
		}
		else
		{
			if (_X) xMOV(ptr32[base + is], regT);
			if (_Y) xMOV(ptr32[base + is + 4], regT);
			if (_Z) xMOV(ptr32[base + is + 8], regT);
			if (_W) xMOV(ptr32[base + is + 12], regT);
		}
		mVU.regAlloc->clearNeeded(regT);
	}
}

//------------------------------------------------------------------
// LQ/LQD/LQI
//------------------------------------------------------------------

mVUop(mVU_LQ)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, false); }
	pass2
	{
		const std::optional<xAddressVoid> optaddr(mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		mVUloadReg(Ft, optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, vuRegs[mVU.index].Mem, gprT1q), _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_LQD)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_ || isVU0) // Access VU1 regs mem-map in !_Is_ case
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xDEC(regS);
			xMOVSX(gprT1, xRegister16(regS)); // TODO: Confirm
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		else
			ptr = (void*)((intptr_t)ptr + (0xffff & (mVU.microMemSize - 8)));
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			if (is.IsEmpty())
			{
				mVUloadReg(Ft, xAddressVoid(ptr), _X_Y_Z_W);
			}
			else
			{
				mVUloadReg(Ft, xComplexAddress(gprT2q, ptr, is), _X_Y_Z_W);
			}
			mVU.regAlloc->clearNeeded(Ft);
		}
	}
}

mVUop(mVU_LQI)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xMOVSX(gprT1, xRegister16(regS)); // TODO: Confirm
			xINC(regS);
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q);
			is = gprT1q;
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			if (is.IsEmpty())
				mVUloadReg(Ft, xAddressVoid(ptr), _X_Y_Z_W);
			else
				mVUloadReg(Ft, xComplexAddress(gprT2q, ptr, is), _X_Y_Z_W);
			mVU.regAlloc->clearNeeded(Ft);
		}
	}
}

//------------------------------------------------------------------
// SQ/SQD/SQI
//------------------------------------------------------------------

mVUop(mVU_SQ)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, false); }
	pass2
	{
		const std::optional<xAddressVoid> optptr(mVUoptimizeConstantAddr(mVU, _It_, _Imm11_, 0));
		if (!optptr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _It_);
			if (_Imm11_ != 0)
				xADD(gprT1, _Imm11_);
			mVUaddrFix(mVU, gprT1q);
		}

		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		mVUsaveReg(Fs, optptr.has_value() ? optptr.value() : xComplexAddress(gprT2q, vuRegs[mVU.index].Mem, gprT1q), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_SQD)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		xAddressReg it = xEmptyReg;
		if (_It_ || isVU0) // Access VU1 regs mem-map in !_It_ case
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xDEC(regT);
			xMOVZX(gprT1, xRegister16(regT));
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q);
			it = gprT1q;
		}
		else
			ptr = (void*)((intptr_t)ptr + (0xffff & (mVU.microMemSize - 8)));
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		if (it.IsEmpty())
			mVUsaveReg(Fs, xAddressVoid(ptr), _X_Y_Z_W, 1);
		else
			mVUsaveReg(Fs, xComplexAddress(gprT2q, ptr, it), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

mVUop(mVU_SQI)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = vuRegs[mVU.index].Mem;
		if (_It_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xMOVZX(gprT1, xRegister16(regT));
			xINC(regT);
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q);
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		if (_It_)
			mVUsaveReg(Fs, xComplexAddress(gprT2q, ptr, gprT1q), _X_Y_Z_W, 1);
		else
			mVUsaveReg(Fs, xAddressVoid(ptr), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
	}
}

//------------------------------------------------------------------
// RINIT/RGET/RNEXT/RXOR
//------------------------------------------------------------------

mVUop(mVU_RINIT)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x007fffff);
			xOR (gprT1, 0x3f800000);
			xMOV(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
		else
			xMOV(ptr32[Rmem], 0x3f800000);
	}
}

static __fi void mVU_RGET_(mV, const x32& Rreg)
{
	if (!mVUlow.noWriteVF)
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		xMOVDZX(Ft, Rreg);
		if (!_XYZW_SS)
			mVUunpack_xyzw(Ft, Ft, 0);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_RGET)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, true); }
	pass2
	{
		xMOV(gprT1, ptr32[Rmem]);
		mVU_RGET_(mVU, gprT1);
	}
}

mVUop(mVU_RNEXT)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, false); }
	pass2
	{
		// algorithm from www.project-fao.org
		const xRegister32& temp3 = mVU.regAlloc->allocGPR();
		xMOV(temp3, ptr32[Rmem]);
		xMOV(gprT1, temp3);
		xSHR(gprT1, 4);
		xAND(gprT1, 1);

		xMOV(gprT2, temp3);
		xSHR(gprT2, 22);
		xAND(gprT2, 1);

		xSHL(temp3, 1);
		xXOR(gprT1, gprT2);
		xXOR(temp3, gprT1);
		xAND(temp3, 0x007fffff);
		xOR (temp3, 0x3f800000);
		xMOV(ptr32[Rmem], temp3);
		mVU_RGET_(mVU, temp3);
		mVU.regAlloc->clearNeeded(temp3);
	}
}

mVUop(mVU_RXOR)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x7fffff);
			xXOR(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
	}
}

//------------------------------------------------------------------
// WaitP/WaitQ
//------------------------------------------------------------------

mVUop(mVU_WAITP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUstall = std::max(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0));
	}
}

mVUop(mVU_WAITQ)
{
	pass1 { mVUstall = std::max(mVUstall, mVUregs.q); }
}

//------------------------------------------------------------------
// XTOP/XITOP
//------------------------------------------------------------------

mVUop(mVU_XTOP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}

		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		if (mVU.index && THREAD_VU1)
		{
			xMOVZX(regT, ptr16[&vu1Thread.vifRegs.top]);
		}
		else
		{
			if (&::vuRegs[mVU.index] == &vuRegs[1])
				xMOVZX(regT, ptr16[&vif1Regs.top]);
			else
				xMOVZX(regT, ptr16[&vif0Regs.top]);
		}
		mVU.regAlloc->clearNeeded(regT);
	}
}

mVUop(mVU_XITOP)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		if (mVU.index && THREAD_VU1)
		{
			xMOVZX(regT, ptr16[&vu1Thread.vifRegs.itop]);
		}
		else
		{
			if (&::vuRegs[mVU.index] == &vuRegs[1])
				xMOVZX(regT, ptr16[&vif1Regs.itop]);
			else
				xMOVZX(regT, ptr16[&vif0Regs.itop]);
		}
		xAND(regT, isVU1 ? 0x3ff : 0xff);
		mVU.regAlloc->clearNeeded(regT);
	}
}

//------------------------------------------------------------------
// XGkick
//------------------------------------------------------------------

void mVU_XGKICK_(u32 addr)
{
	addr = (addr & 0x3ff) * 16;
	u32 diff = 0x4000 - addr;
	u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, addr, ~0u, true);

	if (size > diff)
	{
		gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[addr], diff, true);
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[0], size - diff, true);
	}
	else
	{
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[addr], size, true);
	}
}

void _vuXGKICKTransfermVU(bool flush)
{
	while (vuRegs[1].xgkickenable && (flush || vuRegs[1].xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;

		if (vuRegs[1].xgkicksizeremaining == 0)
		{
			u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, vuRegs[1].xgkickaddr, ~0u, flush);
			vuRegs[1].xgkicksizeremaining = size & 0xFFFF;
			vuRegs[1].xgkickendpacket = size >> 31;
			vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

			if (vuRegs[1].xgkicksizeremaining == 0)
			{
				vuRegs[1].xgkickenable = false;
				break;
			}
		}

		if (!flush)
		{
			transfersize = std::min(vuRegs[1].xgkicksizeremaining, vuRegs[1].xgkickcyclecount * 8);
			transfersize = std::min(transfersize, vuRegs[1].xgkickdiff);
		}
		else
		{
			transfersize = vuRegs[1].xgkicksizeremaining;
			transfersize = std::min(transfersize, vuRegs[1].xgkickdiff);
		}

		// Would be "nicer" to do the copy until it's all up, however this really screws up PATH3 masking stuff
		// So lets just do it the other way :)
		if (THREAD_VU1)
		{
			if (transfersize < vuRegs[1].xgkicksizeremaining)
				gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize, true);
			else
				gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize, true);
		}
		else
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize, true);

		if (flush)
			vuRegs[1].cycle += transfersize / 8;

		vuRegs[1].xgkickcyclecount -= transfersize / 8;

		vuRegs[1].xgkickaddr = (vuRegs[1].xgkickaddr + transfersize) & 0x3FFF;
		vuRegs[1].xgkicksizeremaining -= transfersize;
		vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

		if (vuRegs[1].xgkickendpacket && !vuRegs[1].xgkicksizeremaining)
			vuRegs[1].xgkickenable = false;
	}
}

static __fi void mVU_XGKICK_SYNC(mV, bool flush)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	// Add the single cycle remainder after this instruction, some games do the store
	// on the second instruction after the kick and that needs to go through first
	// but that's VERY close..
	xTEST(ptr32[&vuRegs[1].xgkickenable], 0x1);
	xForwardJZ32 skipxgkick;
	xADD(ptr32[&vuRegs[1].xgkickcyclecount], mVUlow.kickcycles-1);
	xCMP(ptr32[&vuRegs[1].xgkickcyclecount], 2);
	xForwardJL32 needcycles;
	mVUbackupRegs(mVU, true, true);
	xFastCall(_vuXGKICKTransfermVU, flush);
	mVUrestoreRegs(mVU, true, true);
	needcycles.SetTarget();
	xADD(ptr32[&vuRegs[1].xgkickcyclecount], 1);
	skipxgkick.SetTarget();
}

static __fi void mVU_XGKICK_DELAY(mV)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	mVUbackupRegs(mVU, true, true);
#if 0 // XGkick Break - ToDo: Change "SomeGifPathValue" to w/e needs to be tested
	xTEST (ptr32[&SomeGifPathValue], 1); // If '1', breaks execution
	xMOV  (ptr32[&mVU.resumePtrXG], (uintptr_t)x86Ptr + 10 + 6);
	xJcc32(Jcc_NotZero, (uintptr_t)mVU.exitFunctXG - ((uintptr_t)x86Ptr + 6));
#endif
	xFastCall(mVU_XGKICK_, ptr32[&mVU.VIxgkick]);
	mVUrestoreRegs(mVU, true, true);
}

mVUop(mVU_XGKICK)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeXGkick(mVU, _Is_, 1);
	}
		pass2
	{
		if (CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, true);
			mVUlow.kickcycles = 0;
		}
		if (mVUinfo.doXGKICK) // check for XGkick Transfer
		{
			mVU_XGKICK_DELAY(mVU);
			mVUinfo.doXGKICK = false;
		}

		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
		if (!CHECK_XGKICKHACK)
		{
			xMOV(ptr32[&mVU.VIxgkick], regS);
		}
		else
		{
			xMOV(ptr32[&vuRegs[1].xgkickenable], 1);
			xMOV(ptr32[&vuRegs[1].xgkickendpacket], 0);
			xMOV(ptr32[&vuRegs[1].xgkicksizeremaining], 0);
			xMOV(ptr32[&vuRegs[1].xgkickcyclecount], 0);
			xMOV(gprT2, ptr32[&mVU.totalCycles]);
			xSUB(gprT2, ptr32[&mVU.cycles]);
			xADD(gprT2, ptr32[&vuRegs[1].cycle]);
			xMOV(ptr32[&vuRegs[1].xgkicklastcycle], gprT2);
			xMOV(gprT1, regS);
			xAND(gprT1, 0x3FF);
			xSHL(gprT1, 4);
			xMOV(ptr32[&vuRegs[1].xgkickaddr], gprT1);
		}
		mVU.regAlloc->clearNeeded(regS);
	}
}

//------------------------------------------------------------------
// Branches/Jumps
//------------------------------------------------------------------

void setBranchA(mP, int x, int _x_)
{
	bool isBranchDelaySlot = false;

	incPC(-2);
	if (mVUlow.branch)
		isBranchDelaySlot = true;
	incPC(2);

	pass1
	{
		if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUbranch     = x;
		mVUlow.branch = x;
	}
	pass2 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
	pass4 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
}

void condEvilBranch(mV, int JMPcc)
{
	if (mVUlow.badBranch)
	{
		xMOV(ptr32[&mVU.branch], gprT1);
		xMOV(ptr32[&mVU.badBranch], branchAddr(mVU));

		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
			incPC(4); // Branch Not Taken Addr
			xMOV(ptr32[&mVU.badBranch], xPC);
			incPC(-4);
		cJMP.SetTarget();
		return;
	}
	if (isEvilBlock)
	{
		xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.evilBranch]); // Branch Not Taken
		xADD(gprT1, 8); // We have already executed 1 instruction from the original branch
		xMOV(ptr32[&mVU.evilevilBranch], gprT1);
		cJMP.SetTarget();
	}
	else
	{
		xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.badBranch]); // Branch Not Taken
		xADD(gprT1, 8); // We have already executed 1 instruction from the original branch
		xMOV(ptr32[&mVU.evilBranch], gprT1);
		cJMP.SetTarget();
		incPC(-2);
		incPC(2);
	}
}

mVUop(mVU_B)
{
	setBranchA(mX, 1, 0);
	pass1 { mVUanalyzeNormBranch(mVU, 0, false); }
	pass2
	{
		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if(isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
	}
}

mVUop(mVU_BAL)
{
	setBranchA(mX, 2, _It_);
	pass1 { mVUanalyzeNormBranch(mVU, _It_, true); }
	pass2
	{
		if (!mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			incPC(-2);
			incPC(2);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
				xMOV(regT, ptr32[&mVU.evilBranch]);
			else
				xMOV(regT, ptr32[&mVU.badBranch]);

			xADD(regT, 8);
			xSHR(regT, 3);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
	}
}

mVUop(mVU_IBEQ)
{
	setBranchA(mX, 3, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Equal);
	}
}

mVUop(mVU_IBGEZ)
{
	setBranchA(mX, 4, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_GreaterOrEqual);
	}
}

mVUop(mVU_IBGTZ)
{
	setBranchA(mX, 5, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Greater);
	}
}

mVUop(mVU_IBLEZ)
{
	setBranchA(mX, 6, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_LessOrEqual);
	}
}

mVUop(mVU_IBLTZ)
{
	setBranchA(mX, 7, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Less);
	}
}

mVUop(mVU_IBNE)
{
	setBranchA(mX, 8, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_NotEqual);
	}
}

void normJumpPass2(mV)
{
	if (!mVUlow.constJump.isValid || mVUlow.evilBranch)
	{
		mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		xSHL(gprT1, 3);
		xAND(gprT1, mVU.microMemSize - 8);

		if (!mVUlow.evilBranch)
		{
			xMOV(ptr32[&mVU.branch], gprT1);
		}
		else
		{
			if(isEvilBlock)
				xMOV(ptr32[&mVU.evilevilBranch], gprT1);
			else
				xMOV(ptr32[&mVU.evilBranch], gprT1);
		}
		//If delay slot is conditional, it uses badBranch to go to its target
		if (mVUlow.badBranch)
		{
			xMOV(ptr32[&mVU.badBranch], gprT1);
		}
	}
}

mVUop(mVU_JR)
{
	mVUbranch = 9;
	pass1 { mVUanalyzeJump(mVU, _Is_, 0, false); }
	pass2
	{
		normJumpPass2(mVU);
	}
}

mVUop(mVU_JALR)
{
	mVUbranch = 10;
	pass1 { mVUanalyzeJump(mVU, _Is_, _It_, 1); }
	pass2
	{
		normJumpPass2(mVU);
		if (!mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		if (mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
			{
				xMOV(regT, ptr32[&mVU.evilBranch]);
				xADD(regT, 8);
				xSHR(regT, 3);
			}
			else
			{
				incPC(-2);
				incPC(2);

				xMOV(regT, ptr32[&mVU.badBranch]);
				xADD(regT, 8);
				xSHR(regT, 3);
			}
			mVU.regAlloc->clearNeeded(regT);
		}
	}
}

//------------------------------------------------------------------
// Declarations
//------------------------------------------------------------------
mVUop(mVU_UPPER_FD_00);
mVUop(mVU_UPPER_FD_01);
mVUop(mVU_UPPER_FD_10);
mVUop(mVU_UPPER_FD_11);
mVUop(mVULowerOP);
mVUop(mVULowerOP_T3_00);
mVUop(mVULowerOP_T3_01);
mVUop(mVULowerOP_T3_10);
mVUop(mVULowerOP_T3_11);
mVUop(mVUunknown);
//------------------------------------------------------------------

//------------------------------------------------------------------
// Opcode Tables
//------------------------------------------------------------------
static const Fnptr_mVUrecInst mVULOWER_OPCODE[128] = {
	mVU_LQ     , mVU_SQ     , mVUunknown , mVUunknown,
	mVU_ILW    , mVU_ISW    , mVUunknown , mVUunknown,
	mVU_IADDIU , mVU_ISUBIU , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVU_FCEQ   , mVU_FCSET  , mVU_FCAND  , mVU_FCOR,
	mVU_FSEQ   , mVU_FSSET  , mVU_FSAND  , mVU_FSOR,
	mVU_FMEQ   , mVUunknown , mVU_FMAND  , mVU_FMOR,
	mVU_FCGET  , mVUunknown , mVUunknown , mVUunknown,
	mVU_B      , mVU_BAL    , mVUunknown , mVUunknown,
	mVU_JR     , mVU_JALR   , mVUunknown , mVUunknown,
	mVU_IBEQ   , mVU_IBNE   , mVUunknown , mVUunknown,
	mVU_IBLTZ  , mVU_IBGTZ  , mVU_IBLEZ  , mVU_IBGEZ,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVULowerOP , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
};

static const Fnptr_mVUrecInst mVULowerOP_T3_00_OPCODE[32] = {
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVU_MOVE   , mVU_LQI    , mVU_DIV    , mVU_MTIR,
	mVU_RNEXT  , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVU_MFP    , mVU_XTOP   , mVU_XGKICK,
	mVU_ESADD  , mVU_EATANxy, mVU_ESQRT  , mVU_ESIN,
};

static const Fnptr_mVUrecInst mVULowerOP_T3_01_OPCODE[32] = {
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVU_MR32   , mVU_SQI    , mVU_SQRT   , mVU_MFIR,
	mVU_RGET   , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVU_XITOP  , mVUunknown,
	mVU_ERSADD , mVU_EATANxz, mVU_ERSQRT , mVU_EATAN,
};

static const Fnptr_mVUrecInst mVULowerOP_T3_10_OPCODE[32] = {
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVU_LQD    , mVU_RSQRT  , mVU_ILWR,
	mVU_RINIT  , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVU_ELENG  , mVU_ESUM   , mVU_ERCPR  , mVU_EEXP,
};

const Fnptr_mVUrecInst mVULowerOP_T3_11_OPCODE [32] = {
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVU_SQD    , mVU_WAITQ  , mVU_ISWR,
	mVU_RXOR   , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVU_ERLENG , mVUunknown , mVU_WAITP  , mVUunknown,
};

static const Fnptr_mVUrecInst mVULowerOP_OPCODE[64] = {
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVU_IADD   , mVU_ISUB   , mVU_IADDI  , mVUunknown,
	mVU_IAND   , mVU_IOR    , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVULowerOP_T3_00, mVULowerOP_T3_01, mVULowerOP_T3_10, mVULowerOP_T3_11,
};

static const Fnptr_mVUrecInst mVU_UPPER_OPCODE[64] = {
	mVU_ADDx   , mVU_ADDy   , mVU_ADDz   , mVU_ADDw,
	mVU_SUBx   , mVU_SUBy   , mVU_SUBz   , mVU_SUBw,
	mVU_MADDx  , mVU_MADDy  , mVU_MADDz  , mVU_MADDw,
	mVU_MSUBx  , mVU_MSUBy  , mVU_MSUBz  , mVU_MSUBw,
	mVU_MAXx   , mVU_MAXy   , mVU_MAXz   , mVU_MAXw,
	mVU_MINIx  , mVU_MINIy  , mVU_MINIz  , mVU_MINIw,
	mVU_MULx   , mVU_MULy   , mVU_MULz   , mVU_MULw,
	mVU_MULq   , mVU_MAXi   , mVU_MULi   , mVU_MINIi,
	mVU_ADDq   , mVU_MADDq  , mVU_ADDi   , mVU_MADDi,
	mVU_SUBq   , mVU_MSUBq  , mVU_SUBi   , mVU_MSUBi,
	mVU_ADD    , mVU_MADD   , mVU_MUL    , mVU_MAX,
	mVU_SUB    , mVU_MSUB   , mVU_OPMSUB , mVU_MINI,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown  , mVUunknown , mVUunknown,
	mVU_UPPER_FD_00, mVU_UPPER_FD_01, mVU_UPPER_FD_10, mVU_UPPER_FD_11,
};

static const Fnptr_mVUrecInst mVU_UPPER_FD_00_TABLE [32] = {
	mVU_ADDAx  , mVU_SUBAx  , mVU_MADDAx , mVU_MSUBAx,
	mVU_ITOF0  , mVU_FTOI0  , mVU_MULAx  , mVU_MULAq,
	mVU_ADDAq  , mVU_SUBAq  , mVU_ADDA   , mVU_SUBA,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
};

static const Fnptr_mVUrecInst mVU_UPPER_FD_01_TABLE [32] = {
	mVU_ADDAy  , mVU_SUBAy  , mVU_MADDAy , mVU_MSUBAy,
	mVU_ITOF4  , mVU_FTOI4  , mVU_MULAy  , mVU_ABS,
	mVU_MADDAq , mVU_MSUBAq , mVU_MADDA  , mVU_MSUBA,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
};

static const Fnptr_mVUrecInst mVU_UPPER_FD_10_TABLE [32] = {
	mVU_ADDAz  , mVU_SUBAz  , mVU_MADDAz , mVU_MSUBAz,
	mVU_ITOF12 , mVU_FTOI12 , mVU_MULAz  , mVU_MULAi,
	mVU_ADDAi  , mVU_SUBAi  , mVU_MULA   , mVU_OPMULA,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
};

static const Fnptr_mVUrecInst mVU_UPPER_FD_11_TABLE [32] = {
	mVU_ADDAw  , mVU_SUBAw  , mVU_MADDAw , mVU_MSUBAw,
	mVU_ITOF15 , mVU_FTOI15 , mVU_MULAw  , mVU_CLIP,
	mVU_MADDAi , mVU_MSUBAi , mVUunknown , mVU_NOP,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
	mVUunknown , mVUunknown , mVUunknown , mVUunknown,
};


//------------------------------------------------------------------
// Table Functions
//------------------------------------------------------------------

mVUop(mVU_UPPER_FD_00)  { mVU_UPPER_FD_00_TABLE   [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVU_UPPER_FD_01)  { mVU_UPPER_FD_01_TABLE   [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVU_UPPER_FD_10)  { mVU_UPPER_FD_10_TABLE   [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVU_UPPER_FD_11)  { mVU_UPPER_FD_11_TABLE   [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVULowerOP)       { mVULowerOP_OPCODE       [ (mVU.code & 0x3f) ](mX); }
mVUop(mVULowerOP_T3_00) { mVULowerOP_T3_00_OPCODE [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVULowerOP_T3_01) { mVULowerOP_T3_01_OPCODE [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVULowerOP_T3_10) { mVULowerOP_T3_10_OPCODE [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVULowerOP_T3_11) { mVULowerOP_T3_11_OPCODE [((mVU.code >> 6) & 0x1f)](mX); }
mVUop(mVUopU)           { mVU_UPPER_OPCODE        [ (mVU.code & 0x3f) ](mX); } // Gets Upper Opcode
mVUop(mVUopL)           { mVULOWER_OPCODE         [ (mVU.code >>  25) ](mX); } // Gets Lower Opcode
mVUop(mVUunknown)
{
	pass1
	{
		if (mVU.code != 0x8000033c)
			mVUinfo.isBadOp = true;
	}
}

// Sets FDIV Flags at the proper time
__fi void mVUdivSet(mV)
{
	if (mVUinfo.doDivFlag)
	{
		if (!sFLAG.doFlag)
			xMOV(getFlagReg(sFLAG.write), getFlagReg(sFLAG.lastWrite));
		xAND(getFlagReg(sFLAG.write), 0xfff3ffff);
		xOR(getFlagReg(sFLAG.write), ptr32[&mVU.divFlag]);
	}
}

// Optimizes out unneeded status flag updates
// This can safely be done when there is an FSSET opcode
__fi void mVUstatusFlagOp(mV)
{
	int curPC = iPC;
	int i = mVUcount;
	bool runLoop = true;

	if (sFLAG.doFlag)
	{
		sFLAG.doNonSticky = true;
	}
	else
	{
		for (; i > 0; i--)
		{
			incPC2(-2);
			if (sFLAG.doNonSticky)
			{
				runLoop = false;
				break;
			}
			else if (sFLAG.doFlag)
			{
				sFLAG.doNonSticky = true;
				break;
			}
		}
	}
	if (runLoop)
	{
		for (; i > 0; i--)
		{
			incPC2(-2);

			if (sFLAG.doNonSticky)
				break;

			sFLAG.doFlag = false;
		}
	}
	iPC = curPC;
}

int findFlagInst(int* fFlag, int cycles)
{
	int j = 0, jValue = -1;
	for (int i = 0; i < 4; i++)
	{
		if ((fFlag[i] <= cycles) && (fFlag[i] > jValue))
		{
			j = i;
			jValue = fFlag[i];
		}
	}
	return j;
}

// Setup Last 4 instances of Status/Mac/Clip flags (needed for accurate block linking)
int sortFlag(int* fFlag, int* bFlag, int cycles)
{
	int lFlag = -5;
	int x = 0;
	for (int i = 0; i < 4; i++)
	{
		bFlag[i] = findFlagInst(fFlag, cycles);
		if (lFlag != bFlag[i])
			x++;
		lFlag = bFlag[i];
		cycles++;
	}
	return x; // Returns the number of Valid Flag Instances
}

void sortFullFlag(int* fFlag, int* bFlag)
{
	int m = std::max(std::max(fFlag[0], fFlag[1]), std::max(fFlag[2], fFlag[3]));
	for (int i = 0; i < 4; i++)
	{
		int t = 3 - (m - fFlag[i]);
		bFlag[i] = (t < 0) ? 0 : t + 1;
	}
}

#define sFlagCond (sFLAG.doFlag || mVUlow.isFSSET || mVUinfo.doDivFlag)
#define sHackCond (mVUsFlagHack && !sFLAG.doNonSticky)

// Note: Flag handling is 'very' complex, it requires full knowledge of how microVU recs work, so don't touch!
__fi void mVUsetFlags(mV, microFlagCycles& mFC)
{
	int endPC = iPC;
	u32 aCount = 0; // Amount of instructions needed to get valid mac flag instances for block linking
	//bool writeProtect = false;

	// Ensure last ~4+ instructions update mac/status flags (if next block's first 4 instructions will read them)
	for (int i = mVUcount; i > 0; i--, aCount++)
	{
		if (sFLAG.doFlag)
		{

			if (__Mac)
			{
				mFLAG.doFlag = true;
				//writeProtect = true;
			}

			if (__Status)
			{
				sFLAG.doNonSticky = true;
				//writeProtect = true;
			}

			if (aCount >= 3)
			{
				break;
			}
		}
		incPC2(-2);
	}

	// Status/Mac Flags Setup Code
	int xS = 0, xM = 0, xC = 0;

	for (int i = 0; i < 4; i++)
	{
		mFC.xStatus[i] = i;
		mFC.xMac   [i] = i;
		mFC.xClip  [i] = i;
	}

	if (!(mVUpBlock->pState.needExactMatch & 1))
	{
		xS = (mVUpBlock->pState.flagInfo >> 2) & 3;
		mFC.xStatus[0] = -1;
		mFC.xStatus[1] = -1;
		mFC.xStatus[2] = -1;
		mFC.xStatus[3] = -1;
		mFC.xStatus[(xS - 1) & 3] = 0;
	}

	if (!(mVUpBlock->pState.needExactMatch & 2))
	{
		mFC.xMac[0] = -1;
		mFC.xMac[1] = -1;
		mFC.xMac[2] = -1;
		mFC.xMac[3] = -1;
	}

	if (!(mVUpBlock->pState.needExactMatch & 4))
	{
		xC = (mVUpBlock->pState.flagInfo >> 6) & 3;
		mFC.xClip[0] = -1;
		mFC.xClip[1] = -1;
		mFC.xClip[2] = -1;
		mFC.xClip[3] = -1;
		mFC.xClip[(xC - 1) & 3] = 0;
	}

	mFC.cycles = 0;
	u32 xCount = mVUcount; // Backup count
	iPC = mVUstartPC;
	for (mVUcount = 0; mVUcount < xCount; mVUcount++)
	{
		if (mVUlow.isFSSET && !noFlagOpts)
		{
			if (__Status) // Don't Optimize out on the last ~4+ instructions
			{
				if ((xCount - mVUcount) > aCount)
					mVUstatusFlagOp(mVU);
			}
			else
				mVUstatusFlagOp(mVU);
		}
		mFC.cycles += mVUstall;

		sFLAG.read = doSFlagInsts ? findFlagInst(mFC.xStatus, mFC.cycles) : 0;
		mFLAG.read = doMFlagInsts ? findFlagInst(mFC.xMac,    mFC.cycles) : 0;
		cFLAG.read = doCFlagInsts ? findFlagInst(mFC.xClip,   mFC.cycles) : 0;

		sFLAG.write = doSFlagInsts ? xS : 0;
		mFLAG.write = doMFlagInsts ? xM : 0;
		cFLAG.write = doCFlagInsts ? xC : 0;

		sFLAG.lastWrite = doSFlagInsts ? (xS - 1) & 3 : 0;
		mFLAG.lastWrite = doMFlagInsts ? (xM - 1) & 3 : 0;
		cFLAG.lastWrite = doCFlagInsts ? (xC - 1) & 3 : 0;

		if (sHackCond)
		{
			sFLAG.doFlag = false;
		}

		if (sFLAG.doFlag)
		{
			if (noFlagOpts)
			{
				sFLAG.doNonSticky = true;
				mFLAG.doFlag = true;
			}
		}

		if (sFlagCond)
		{
			mFC.xStatus[xS] = mFC.cycles + 4;
			xS = (xS + 1) & 3;
		}

		if (mFLAG.doFlag)
		{
			mFC.xMac[xM] = mFC.cycles + 4;
			xM = (xM + 1) & 3;
		}

		if (cFLAG.doFlag)
		{
			mFC.xClip[xC] = mFC.cycles + 4;
			xC = (xC + 1) & 3;
		}

		mFC.cycles++;
		incPC2(2);
	}

	mVUregs.flagInfo |= ((__Status) ? 0 : (xS << 2));
	mVUregs.flagInfo |= /*((__Mac||1) ? 0 :*/ (xM << 4)/*)*/; //TODO: Optimise this? Might help with number of blocks.
	mVUregs.flagInfo |= ((__Clip)   ? 0 : (xC << 6));
	iPC = endPC;
}

#define getFlagReg2(x) ((bStatus[0] == x) ? getFlagReg(x) : gprT1)
#define getFlagReg3(x) ((gFlag == x) ? gprT1 : getFlagReg(x))
#define getFlagReg4(x) ((gFlag == x) ? gprT1 : gprT2)
#define shuffleMac     ((bMac[3] << 6) | (bMac[2] << 4) | (bMac[1] << 2) | bMac[0])
#define shuffleClip    ((bClip[3] << 6) | (bClip[2] << 4) | (bClip[1] << 2) | bClip[0])

// Recompiles Code for Proper Flags on Block Linkings
__fi void mVUsetupFlags(mV, microFlagCycles& mFC)
{
	const bool pf = false; // Print Flag Info

	if (doSFlagInsts && __Status)
	{
		int bStatus[4];
		int sortRegs = sortFlag(mFC.xStatus, bStatus, mFC.cycles);
		// Note: Emitter will optimize out mov(reg1, reg1) cases...
		if (sortRegs == 1)
		{
			xMOV(gprF0, getFlagReg(bStatus[0]));
			xMOV(gprF1, getFlagReg(bStatus[1]));
			xMOV(gprF2, getFlagReg(bStatus[2]));
			xMOV(gprF3, getFlagReg(bStatus[3]));
		}
		else if (sortRegs == 2)
		{
			xMOV(gprT1, getFlagReg (bStatus[3]));
			xMOV(gprF0, getFlagReg (bStatus[0]));
			xMOV(gprF1, getFlagReg2(bStatus[1]));
			xMOV(gprF2, getFlagReg2(bStatus[2]));
			xMOV(gprF3, gprT1);
		}
		else if (sortRegs == 3)
		{
			int gFlag = (bStatus[0] == bStatus[1]) ? bStatus[2] : bStatus[1];
			xMOV(gprT1, getFlagReg (gFlag));
			xMOV(gprT2, getFlagReg (bStatus[3]));
			xMOV(gprF0, getFlagReg (bStatus[0]));
			xMOV(gprF1, getFlagReg3(bStatus[1]));
			xMOV(gprF2, getFlagReg4(bStatus[2]));
			xMOV(gprF3, gprT2);
		}
		else
		{
			const xRegister32& temp3 = mVU.regAlloc->allocGPR();
			xMOV(gprT1, getFlagReg(bStatus[0]));
			xMOV(gprT2, getFlagReg(bStatus[1]));
			xMOV(temp3, getFlagReg(bStatus[2]));
			xMOV(gprF3, getFlagReg(bStatus[3]));
			xMOV(gprF0, gprT1);
			xMOV(gprF1, gprT2);
			xMOV(gprF2, temp3);
			mVU.regAlloc->clearNeeded(temp3);
		}
	}

	if (doMFlagInsts && __Mac)
	{
		int bMac[4];
		sortFlag(mFC.xMac, bMac, mFC.cycles);
		xMOVAPS(xmmT1, ptr128[mVU.macFlag]);
		xSHUF.PS(xmmT1, xmmT1, shuffleMac);
		xMOVAPS(ptr128[mVU.macFlag], xmmT1);
	}

	if (doCFlagInsts && __Clip)
	{
		int bClip[4];
		sortFlag(mFC.xClip, bClip, mFC.cycles);
		xMOVAPS(xmmT2, ptr128[mVU.clipFlag]);
		xSHUF.PS(xmmT2, xmmT2, shuffleClip);
		xMOVAPS(ptr128[mVU.clipFlag], xmmT2);
	}
}

#define shortBranch() \
	{ \
		if ((branch == 3) || (branch == 4)) /*Branches*/ \
		{ \
			_mVUflagPass(mVU, aBranchAddr, sCount + found, found, v); \
			if (branch == 3) /*Non-conditional Branch*/ \
				break; \
			branch = 0; \
		} \
		else if (branch == 5) /*JR/JARL*/ \
		{ \
			if (sCount + found < 4) \
				mVUregs.needExactMatch |= 7; \
			break; \
		} \
		else /*E-Bit End*/ \
			break; \
	}

// Scan through instructions and check if flags are read (FSxxx, FMxxx, FCxxx opcodes)
void _mVUflagPass(mV, u32 startPC, u32 sCount, u32 found, std::vector<u32>& v)
{

	for (u32 i = 0; i < v.size(); i++)
	{
		if (v[i] == startPC)
			return; // Prevent infinite recursion
	}
	v.push_back(startPC);

	int oldPC = iPC;
	int oldBranch = mVUbranch;
	int aBranchAddr = 0;
	iPC = startPC / 4;
	mVUbranch = 0;
	for (int branch = 0; sCount < 4; sCount += found)
	{
		mVUregs.needExactMatch &= 7;
		incPC(1);
		mVUopU(mVU, 3);
		found |= (mVUregs.needExactMatch & 8) >> 3;
		mVUregs.needExactMatch &= 7;
		if (curI & _Ebit_)
		{
			branch = 1;
		}
		if (curI & _Tbit_)
		{
			branch = 6;
		}
		if ((curI & _Dbit_) && doDBitHandling)
		{
			branch = 6;
		}
		if (!(curI & _Ibit_))
		{
			incPC(-1);
			mVUopL(mVU, 3);
			incPC(1);
		}

		if (branch >= 2)
		{
			shortBranch();
		}
		else if (branch == 1)
		{
			branch = 2;
		}
		if (mVUbranch)
		{
			branch = ((mVUbranch > 8) ? (5) : ((mVUbranch < 3) ? 3 : 4));
			incPC(-1);
			aBranchAddr = branchAddr(mVU);
			incPC(1);
			mVUbranch = 0;
		}
		incPC(1);
		if ((mVUregs.needExactMatch & 7) == 7)
			break;
	}
	iPC = oldPC;
	mVUbranch = oldBranch;
	mVUregs.needExactMatch &= 7;
	setCode();
}

void mVUflagPass(mV, u32 startPC, u32 sCount = 0, u32 found = 0)
{
	std::vector<u32> v;
	_mVUflagPass(mVU, startPC, sCount, found, v);
}

// Checks if the first ~4 instructions of a block will read flags
void mVUsetFlagInfo(mV)
{
	if (noFlagOpts)
	{
		mVUregs.needExactMatch = 0x7;
		mVUregs.flagInfo = 0x0;
		return;
	}
	if (mVUbranch <= 2) // B/BAL
	{
		incPC(-1);
		mVUflagPass(mVU, branchAddr(mVU));
		incPC(1);

		mVUregs.needExactMatch &= 0x7;
	}
	else if (mVUbranch <= 8) // Conditional Branch
	{
		incPC(-1); // Branch Taken
		mVUflagPass(mVU, branchAddr(mVU));
		int backupFlagInfo = mVUregs.needExactMatch;
		mVUregs.needExactMatch = 0;

		incPC(4); // Branch Not Taken
		mVUflagPass(mVU, xPC);
		incPC(-3);

		mVUregs.needExactMatch |= backupFlagInfo;
		mVUregs.needExactMatch &= 0x7;
	}
	else // JR/JALR
	{
		if (!doConstProp || !mVUlow.constJump.isValid)
		{
			mVUregs.needExactMatch |= 0x7;
		}
		else
		{
			mVUflagPass(mVU, (mVUlow.constJump.regValue * 8) & (mVU.microMemSize - 8));
		}
		mVUregs.needExactMatch &= 0x7;
	}
}

/* Prototypes */
extern void mVUincCycles(microVU& mVU, int x);
extern void* mVUcompile(microVU& mVU, u32 startPC, uintptr_t pState);

static __fi int getLastFlagInst(microRegInfo& pState, int* xFlag, int flagType, int isEbit)
{
	if (isEbit)
		return findFlagInst(xFlag, 0x7fffffff);
	if (pState.needExactMatch & (1 << flagType))
		return 3;
	return (((pState.flagInfo >> (2 * flagType + 2)) & 3) - 1) & 3;
}

static void mVU0clearlpStateJIT(void) { if (!microVU0.prog.cleared) memset(&microVU0.prog.lpState, 0, sizeof(microVU1.prog.lpState)); }
static void mVU1clearlpStateJIT(void) { if (!microVU1.prog.cleared) memset(&microVU1.prog.lpState, 0, sizeof(microVU1.prog.lpState)); }

static void mVUDTendProgram(mV, microFlagCycles* mFC, int isEbit)
{
	int fStatus = getLastFlagInst(mVUpBlock->pState, mFC->xStatus, 0, isEbit);
	int fMac    = getLastFlagInst(mVUpBlock->pState, mFC->xMac, 1, isEbit);
	int fClip   = getLastFlagInst(mVUpBlock->pState, mFC->xClip, 2, isEbit);
	int qInst   = 0;
	int pInst   = 0;
	microBlock stateBackup;
	memcpy(&stateBackup, &mVUregs, sizeof(mVUregs)); //backup the state, it's about to get screwed with.

	mVU.regAlloc->TDwritebackAll(); //Writing back ok, invalidating early kills the rec, so don't do it :P

	if (isEbit)
	{
		mVUincCycles(mVU, 100); // Ensures Valid P/Q instances (And sets all cycle data to 0)
		mVUcycles -= 100;
		qInst = mVU.q;
		pInst = mVU.p;
		mVUregs.xgkickcycles = 0;
		if (mVUinfo.doDivFlag)
		{
			sFLAG.doFlag = true;
			sFLAG.write = fStatus;
			mVUdivSet(mVU);
		}
		//Run any pending XGKick, providing we've got to it.
		if (mVUinfo.doXGKICK && xPC >= mVUinfo.XGKICKPC)
		{
			mVU_XGKICK_DELAY(mVU);
		}
		if (isVU1)
		{
			if (CHECK_XGKICKHACK)
			{
				mVUlow.kickcycles = 99;
				mVU_XGKICK_SYNC(mVU, true);
			}
			xFastCall((void*)mVU1clearlpStateJIT);
		}
		else
			xFastCall((void*)mVU0clearlpStateJIT);
	}

	// Save P/Q Regs
	if (qInst)
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
	xMOVSS(ptr32[&vuRegs[mVU.index].VI[REG_Q].UL], xmmPQ);
	xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
	xMOVSS(ptr32[&vuRegs[mVU.index].pending_q], xmmPQ);
	xPSHUF.D(xmmPQ, xmmPQ, 0xe1);

	if (isVU1)
	{
		if (pInst)
			xPSHUF.D(xmmPQ, xmmPQ, 0xb4); // Swap Pending/Active P
		xPSHUF.D(xmmPQ, xmmPQ, 0xC6); // 3 0 1 2
		xMOVSS(ptr32[&vuRegs[mVU.index].VI[REG_P].UL], xmmPQ);
		xPSHUF.D(xmmPQ, xmmPQ, 0x87); // 0 2 1 3
		xMOVSS(ptr32[&vuRegs[mVU.index].pending_p], xmmPQ);
		xPSHUF.D(xmmPQ, xmmPQ, 0x27); // 3 2 1 0
	}

	// Save MAC, Status and CLIP Flag Instances
	mVUallocSFLAGc(gprT1, gprT2, fStatus);
	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_STATUS_FLAG].UL], gprT1);
	mVUallocMFLAGa(mVU, gprT1, fMac);
	mVUallocCFLAGa(mVU, gprT2, fClip);
	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_MAC_FLAG].UL], gprT1);
	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL], gprT2);

	if (isEbit) // Flush flag instances
	{
		xMOVDZX(xmmT1, ptr32[&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL]);
		xSHUF.PS(xmmT1, xmmT1, 0);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_clipflags], xmmT1);

		xMOVDZX(xmmT1, ptr32[&vuRegs[mVU.index].VI[REG_MAC_FLAG].UL]);
		xSHUF.PS(xmmT1, xmmT1, 0);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_macflags], xmmT1);

		xMOVDZX(xmmT1, getFlagReg(fStatus));
		xSHUF.PS(xmmT1, xmmT1, 0);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_statusflags], xmmT1);
	}
	else // Backup flag instances
	{
		xMOVAPS(xmmT1, ptr128[mVU.macFlag]);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_macflags], xmmT1);
		xMOVAPS(xmmT1, ptr128[mVU.clipFlag]);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_clipflags], xmmT1);

		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[0]], gprF0);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[1]], gprF1);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[2]], gprF2);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[3]], gprF3);
	}

	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		xMOV(ptr32[&vuRegs[mVU.index].nextBlockCycles], 0);

	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);

	if (isEbit) // Clear 'is busy' Flags
	{
		if (!mVU.index || !THREAD_VU1)
		{
			xAND(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? ~0x100 : ~0x001)); // VBS0/VBS1 flag
		}
	}

	if (isEbit != 2) // Save PC, and Jump to Exit Point
	{
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUTBit);
		xJMP(mVU.exitFunct);
	}

	memcpy(&mVUregs, &stateBackup, sizeof(mVUregs)); //Restore the state for the rest of the recompile
}

void mVUendProgram(mV, microFlagCycles* mFC, int isEbit)
{

	int fStatus = getLastFlagInst(mVUpBlock->pState, mFC->xStatus, 0, isEbit && isEbit != 3);
	int fMac    = getLastFlagInst(mVUpBlock->pState, mFC->xMac, 1, isEbit && isEbit != 3);
	int fClip   = getLastFlagInst(mVUpBlock->pState, mFC->xClip, 2, isEbit && isEbit != 3);
	int qInst   = 0;
	int pInst   = 0;
	microBlock stateBackup;
	memcpy(&stateBackup, &mVUregs, sizeof(mVUregs)); //backup the state, it's about to get screwed with.
	if (!isEbit || isEbit == 3)
		mVU.regAlloc->TDwritebackAll(); //Writing back ok, invalidating early kills the rec, so don't do it :P
	else
		mVU.regAlloc->flushAll();

	if (isEbit && isEbit != 3)
	{
		memset(&mVUinfo, 0, sizeof(mVUinfo));
		memset(&mVUregsTemp, 0, sizeof(mVUregsTemp));
		mVUincCycles(mVU, 100); // Ensures Valid P/Q instances (And sets all cycle data to 0)
		mVUcycles -= 100;
		qInst = mVU.q;
		pInst = mVU.p;
		mVUregs.xgkickcycles = 0;
		if (mVUinfo.doDivFlag)
		{
			sFLAG.doFlag = true;
			sFLAG.write = fStatus;
			mVUdivSet(mVU);
		}
		if (mVUinfo.doXGKICK)
		{
			mVU_XGKICK_DELAY(mVU);
		}
		if (isVU1)
		{
			if (CHECK_XGKICKHACK)
			{
				mVUlow.kickcycles = 99;
				mVU_XGKICK_SYNC(mVU, true);
			}
			xFastCall((void*)mVU1clearlpStateJIT);
		}
		else
			xFastCall((void*)mVU0clearlpStateJIT);
	}

	// Save P/Q Regs
	if (qInst)
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
	xMOVSS(ptr32[&vuRegs[mVU.index].VI[REG_Q].UL], xmmPQ);
	xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
	xMOVSS(ptr32[&vuRegs[mVU.index].pending_q], xmmPQ);
	xPSHUF.D(xmmPQ, xmmPQ, 0xe1);

	if (isVU1)
	{
		if (pInst)
			xPSHUF.D(xmmPQ, xmmPQ, 0xb4); // Swap Pending/Active P
		xPSHUF.D(xmmPQ, xmmPQ, 0xC6); // 3 0 1 2
		xMOVSS(ptr32[&vuRegs[mVU.index].VI[REG_P].UL], xmmPQ);
		xPSHUF.D(xmmPQ, xmmPQ, 0x87); // 0 2 1 3
		xMOVSS(ptr32[&vuRegs[mVU.index].pending_p], xmmPQ);
		xPSHUF.D(xmmPQ, xmmPQ, 0x27); // 3 2 1 0
	}

	// Save MAC, Status and CLIP Flag Instances
	mVUallocSFLAGc(gprT1, gprT2, fStatus);
	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_STATUS_FLAG].UL], gprT1);
	mVUallocMFLAGa(mVU, gprT1, fMac);
	mVUallocCFLAGa(mVU, gprT2, fClip);
	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_MAC_FLAG].UL], gprT1);
	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL], gprT2);

	if (!isEbit || isEbit == 3) // Backup flag instances
	{
		xMOVAPS(xmmT1, ptr128[mVU.macFlag]);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_macflags], xmmT1);
		xMOVAPS(xmmT1, ptr128[mVU.clipFlag]);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_clipflags], xmmT1);

		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[0]], gprF0);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[1]], gprF1);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[2]], gprF2);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[3]], gprF3);
	}
	else // Flush flag instances
	{
		xMOVDZX(xmmT1, ptr32[&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL]);
		xSHUF.PS(xmmT1, xmmT1, 0);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_clipflags], xmmT1);

		xMOVDZX(xmmT1, ptr32[&vuRegs[mVU.index].VI[REG_MAC_FLAG].UL]);
		xSHUF.PS(xmmT1, xmmT1, 0);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_macflags], xmmT1);

		xMOVDZX(xmmT1, getFlagReg(fStatus));
		xSHUF.PS(xmmT1, xmmT1, 0);
		xMOVAPS(ptr128[&vuRegs[mVU.index].micro_statusflags], xmmT1);
	}

	xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);

	if ((isEbit && isEbit != 3)) // Clear 'is busy' Flags
	{
		if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
			xMOV(ptr32[&vuRegs[mVU.index].nextBlockCycles], 0);
		if (!mVU.index || !THREAD_VU1)
		{
			xAND(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? ~0x100 : ~0x001)); // VBS0/VBS1 flag
		}
	}
	else if(isEbit)
	{
		if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
			xMOV(ptr32[&vuRegs[mVU.index].nextBlockCycles], 0);
	}

	if (isEbit != 2 && isEbit != 3) // Save PC, and Jump to Exit Point
	{
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
	}
	memcpy(&mVUregs, &stateBackup, sizeof(mVUregs)); //Restore the state for the rest of the recompile
}

// Recompiles Code for Proper Flags and Q/P regs on Block Linkings
void mVUsetupBranch(mV, microFlagCycles& mFC)
{
	mVU.regAlloc->flushAll(); // Flush Allocated Regs
	mVUsetupFlags(mVU, mFC);  // Shuffle Flag Instances

	// Shuffle P/Q regs since every block starts at instance #0
	if (mVU.p || mVU.q)
		xPSHUF.D(xmmPQ, xmmPQ, shufflePQ);
	mVU.p = 0, mVU.q = 0;
}

void normBranchCompile(microVU& mVU, u32 branchPC)
{
	microBlock* pBlock;
	blockCreate(branchPC / 8);
	pBlock = mVUblocks[branchPC / 8]->search(mVU, (microRegInfo*)&mVUregs);
	if (pBlock)
		xJMP(pBlock->x86ptrStart);
	else
		mVUcompile(mVU, branchPC, (uintptr_t)&mVUregs);
}

void normJumpCompile(mV, microFlagCycles& mFC, bool isEvilJump)
{
	memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
	mVUsetupBranch(mVU, mFC);
	mVUbackupRegs(mVU);

	if (!mVUpBlock->jumpCache) // Create the jump cache for this block
		mVUpBlock->jumpCache = new microJumpCache[mProgSize / 2];

	if (isEvilJump)
	{
		xMOV(arg1regd, ptr32[&mVU.evilBranch]);
		xMOV(gprT1, ptr32[&mVU.evilevilBranch]);
		xMOV(ptr32[&mVU.evilBranch], gprT1);
	}
	else
		xMOV(arg1regd, ptr32[&mVU.branch]);
	if (doJumpCaching)
		xLoadFarAddr(arg2reg, mVUpBlock);
	else
		xLoadFarAddr(arg2reg, &mVUpBlock->pStateEnd);

	if (mVUup.eBit && isEvilJump) // E-bit EvilJump
	{
		//Xtreme G 3 does 2 conditional jumps, the first contains an E Bit on the first instruction
		//So if it is taken, you need to end the program, else you get infinite loops.
		mVUendProgram(mVU, &mFC, 2);
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], arg1regd);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
	}

	if (mVU.index)
		xFastCall((void*)(void (*)())mVUcompileJIT<1>, arg1reg, arg2reg);
	else
		xFastCall((void*)(void (*)())mVUcompileJIT<0>, arg1reg, arg2reg); //(u32 startPC, uintptr_t pState)

	mVUrestoreRegs(mVU);
	xJMP(gprT1q); // Jump to rec-code address
}

void normBranch(mV, microFlagCycles& mFC)
{
	// E-bit or T-Bit or D-Bit Branch
	if (mVUup.dBit && doDBitHandling)
	{
		// Flush register cache early to avoid double flush on both paths
		mVU.regAlloc->flushAll(false);

		u32 tempPC = iPC;
		if (mVU.index && THREAD_VU1)
			xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x400 : 0x4));
		else
			xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x400 : 0x4));
		xForwardJump32 eJMP(Jcc_Zero);
		if (!mVU.index || !THREAD_VU1)
		{
			xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x200 : 0x2));
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
		}
		iPC = branchAddr(mVU) / 4;
		mVUDTendProgram(mVU, &mFC, 1);
		eJMP.SetTarget();
		iPC = tempPC;
	}
	if (mVUup.tBit)
	{
		// Flush register cache early to avoid double flush on both paths
		mVU.regAlloc->flushAll(false);

		u32 tempPC = iPC;
		if (mVU.index && THREAD_VU1)
			xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x800 : 0x8));
		else
			xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x800 : 0x8));
		xForwardJump32 eJMP(Jcc_Zero);
		if (!mVU.index || !THREAD_VU1)
		{
			xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x400 : 0x4));
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
		}
		iPC = branchAddr(mVU) / 4;
		mVUDTendProgram(mVU, &mFC, 1);
		eJMP.SetTarget();
		iPC = tempPC;
	}
	if (mVUup.mBit)
	{
		u32 tempPC = iPC;

		memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
		xLoadFarAddr(rax, &mVUpBlock->pStateEnd);
		xCALL((void*)mVU.copyPLState);

		mVUsetupBranch(mVU, mFC);
		mVUendProgram(mVU, &mFC, 3);
		iPC = branchAddr(mVU) / 4;
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
		iPC = tempPC;
	}
	if (mVUup.eBit)
	{
		iPC = branchAddr(mVU) / 4;
		mVUendProgram(mVU, &mFC, 1);
		return;
	}

	// Normal Branch
	mVUsetupBranch(mVU, mFC);
	normBranchCompile(mVU, branchAddr(mVU));
}

void condBranch(mV, microFlagCycles& mFC, int JMPcc)
{
	mVUsetupBranch(mVU, mFC);

	if (mVUup.tBit)
	{
		u32 tempPC = iPC;
		if (mVU.index && THREAD_VU1)
			xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x800 : 0x8));
		else
			xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x800 : 0x8));
		xForwardJump32 eJMP(Jcc_Zero);
		if (!mVU.index || !THREAD_VU1)
		{
			xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x400 : 0x4));
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, &mFC, 2);
		xCMP(ptr16[&mVU.branch], 0);
		xForwardJump32 tJMP(xInvertCond((JccComparisonType)JMPcc));
			incPC(4); // Set PC to First instruction of Non-Taken Side
			xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
			if (mVU.index && THREAD_VU1)
				xFastCall((void*)mVUTBit);
			xJMP(mVU.exitFunct);
		tJMP.SetTarget();
		incPC(-4); // Go Back to Branch Opcode to get branchAddr
		iPC = branchAddr(mVU) / 4;
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUTBit);
		xJMP(mVU.exitFunct);
		eJMP.SetTarget();
		iPC = tempPC;
	}
	if (mVUup.dBit && doDBitHandling)
	{
		u32 tempPC = iPC;
		if (mVU.index  && THREAD_VU1)
			xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x400 : 0x4));
		else
			xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x400 : 0x4));
		xForwardJump32 eJMP(Jcc_Zero);
		if (!mVU.index || !THREAD_VU1)
		{
			xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x200 : 0x2));
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, &mFC, 2);
		xCMP(ptr16[&mVU.branch], 0);
		xForwardJump32 dJMP(xInvertCond((JccComparisonType)JMPcc));
			incPC(4); // Set PC to First instruction of Non-Taken Side
			xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
			xJMP(mVU.exitFunct);
		dJMP.SetTarget();
		incPC(-4); // Go Back to Branch Opcode to get branchAddr
		iPC = branchAddr(mVU) / 4;
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
		xJMP(mVU.exitFunct);
		eJMP.SetTarget();
		iPC = tempPC;
	}
	if (mVUup.mBit)
	{
		u32 tempPC = iPC;

		memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
		xLoadFarAddr(rax, &mVUpBlock->pStateEnd);
		xCALL((void*)mVU.copyPLState);

		mVUendProgram(mVU, &mFC, 3);
		xCMP(ptr16[&mVU.branch], 0);
		xForwardJump32 dJMP((JccComparisonType)JMPcc);
		incPC(4); // Set PC to First instruction of Non-Taken Side
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
		dJMP.SetTarget();
		incPC(-4); // Go Back to Branch Opcode to get branchAddr
		iPC = branchAddr(mVU) / 4;
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
		iPC = tempPC;
	}
	if (mVUup.eBit) // Conditional Branch With E-Bit Set
	{
		mVUendProgram(mVU, &mFC, 2);
		xCMP(ptr16[&mVU.branch], 0);

		incPC(3);
		xForwardJump32 eJMP(((JccComparisonType)JMPcc));
			incPC(1); // Set PC to First instruction of Non-Taken Side
			xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
			if (mVU.index && THREAD_VU1)
				xFastCall((void*)mVUEBit);
			xJMP(mVU.exitFunct);
		eJMP.SetTarget();
		incPC(-4); // Go Back to Branch Opcode to get branchAddr

		iPC = branchAddr(mVU) / 4;
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], xPC);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
		return;
	}
	else // Normal Conditional Branch
	{
		xCMP(ptr16[&mVU.branch], 0);

		incPC(3);
		microBlock* bBlock;
		incPC2(1); // Check if Branch Non-Taken Side has already been recompiled
		blockCreate(iPC / 2);
		bBlock = mVUblocks[iPC / 2]->search(mVU, (microRegInfo*)&mVUregs);
		incPC2(-1);
		if (bBlock) // Branch non-taken has already been compiled
		{
			xJccKnownTarget(xInvertCond((JccComparisonType)JMPcc), bBlock->x86ptrStart);
			incPC(-3); // Go back to branch opcode (to get branch imm addr)
			normBranchCompile(mVU, branchAddr(mVU));
		}
		else
		{
			s32* ajmp = xJcc32((JccComparisonType)JMPcc, 0);
			u32 bPC = iPC; // mVUcompile can modify iPC, mVUpBlock, and mVUregs so back them up

			microRegInfo regBackup;
			memcpy(&regBackup, &mVUregs, sizeof(microRegInfo));

			incPC2(1); // Get PC for branch not-taken
			mVUcompile(mVU, xPC, (uintptr_t)&mVUregs);

			iPC = bPC;
			incPC(-3); // Go back to branch opcode (to get branch imm addr)
			uintptr_t jumpAddr = (uintptr_t)mVUblockFetch(mVU, branchAddr(mVU), (uintptr_t)&regBackup);
			*ajmp = (jumpAddr - ((uintptr_t)ajmp + 4));
		}
	}
}

void normJump(mV, microFlagCycles& mFC)
{
	if (mVUlow.constJump.isValid) // Jump Address is Constant
	{
		if (mVUup.eBit) // E-bit Jump
		{
			iPC = (mVUlow.constJump.regValue * 2) & (mVU.progMemMask);
			mVUendProgram(mVU, &mFC, 1);
			return;
		}
		int jumpAddr = (mVUlow.constJump.regValue * 8) & (mVU.microMemSize - 8);
		mVUsetupBranch(mVU, mFC);
		normBranchCompile(mVU, jumpAddr);
		return;
	}
	if (mVUup.dBit && doDBitHandling)
	{
		// Flush register cache early to avoid double flush on both paths
		mVU.regAlloc->flushAll(false);

		if (THREAD_VU1)
			xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x400 : 0x4));
		else
			xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x400 : 0x4));
		xForwardJump32 eJMP(Jcc_Zero);
		if (!mVU.index || !THREAD_VU1)
		{
			xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x200 : 0x2));
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, &mFC, 2);
		xMOV(gprT1, ptr32[&mVU.branch]);
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], gprT1);
		xJMP(mVU.exitFunct);
		eJMP.SetTarget();
	}
	if (mVUup.tBit)
	{
		// Flush register cache early to avoid double flush on both paths
		mVU.regAlloc->flushAll(false);

		if (mVU.index && THREAD_VU1)
			xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x800 : 0x8));
		else
			xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x800 : 0x8));
		xForwardJump32 eJMP(Jcc_Zero);
		if (!mVU.index || !THREAD_VU1)
		{
			xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x400 : 0x4));
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, &mFC, 2);
		xMOV(gprT1, ptr32[&mVU.branch]);
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], gprT1);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUTBit);
		xJMP(mVU.exitFunct);
		eJMP.SetTarget();
	}
	if (mVUup.eBit) // E-bit Jump
	{
		mVUendProgram(mVU, &mFC, 2);
		xMOV(gprT1, ptr32[&mVU.branch]);
		xMOV(ptr32[&vuRegs[mVU.index].VI[REG_TPC].UL], gprT1);
		if (mVU.index && THREAD_VU1)
			xFastCall((void*)mVUEBit);
		xJMP(mVU.exitFunct);
	}
	else
	{
		normJumpCompile(mVU, mFC, false);
	}
}

//------------------------------------------------------------------
// Program Range Checking and Setting up Ranges
//------------------------------------------------------------------

// Used by mVUsetupRange
__fi void mVUcheckIsSame(mV)
{
	if (mVU.prog.isSame == -1)
		mVU.prog.isSame = !memcmp((u8*)mVUcurProg.data, vuRegs[mVU.index].Micro, mVU.microMemSize);
	if (mVU.prog.isSame == 0)
	{
		mVUcacheProg(mVU, *mVU.prog.cur);
		mVU.prog.isSame = 1;
	}
}

// Sets up microProgram PC ranges based on whats been recompiled
void mVUsetupRange(microVU& mVU, s32 pc, bool isStartPC)
{
	std::deque<microRange>*& ranges = mVUcurProg.ranges;

	// The PC handling will prewrap the PC so we need to set the end PC to the end of the micro memory, but only if it wraps, no more.
	const s32 cur_pc = (!isStartPC && mVUrange.start > pc && pc == 0) ? mVU.microMemSize : pc;

	if (isStartPC) // Check if startPC is already within a block we've recompiled
	{
		std::deque<microRange>::const_iterator it(ranges->begin());
		for (; it != ranges->end(); ++it)
		{
			if ((cur_pc >= it[0].start) && (cur_pc <= it[0].end))
			{
				if (it[0].start != it[0].end)
				{
					microRange mRange = {it[0].start, it[0].end};
					ranges->erase(it);
					ranges->push_front(mRange);
					return; // new start PC is inside the range of another range
				}
			}
		}
	}
	else if (mVUrange.end >= cur_pc)
	{
		// existing range covers more area than current PC so no need to process it
		return;
	}
	
	if (doWholeProgCompare)
		mVUcheckIsSame(mVU);

	if (isStartPC)
	{
		microRange mRange = {cur_pc, -1};
		ranges->push_front(mRange);
		return;
	}

	if (mVUrange.start <= cur_pc)
	{
		mVUrange.end = cur_pc;
		s32& rStart = mVUrange.start;
		s32& rEnd = mVUrange.end;
		std::deque<microRange>::iterator it(ranges->begin());
		it++;
		for (;it != ranges->end();)
		{
			if (((it->start >= rStart) && (it->start <= rEnd)) || ((it->end >= rStart) && (it->end <= rEnd))) // Starts after this prog but starts before the end of current prog
			{
				rStart = std::min(it->start, rStart); // Choose the earlier start
				rEnd = std::max(it->end, rEnd);
				it = ranges->erase(it);
			}
			else
				it++;
		}
	}
	else
	{
		mVUrange.end = mVU.microMemSize;
		microRange mRange = {0, cur_pc };
		ranges->push_front(mRange);
	}

	if(!doWholeProgCompare)
		mVUcacheProg(mVU, *mVU.prog.cur);
}

//------------------------------------------------------------------
// Execute VU Opcode/Instruction (Upper and Lower)
//------------------------------------------------------------------

__ri void doUpperOp(mV)
{
	mVUopU(mVU, 1);
	mVUdivSet(mVU);
}
__ri void doLowerOp(mV)
{
	incPC(-1);
	mVUopL(mVU, 1);
	incPC(1);
}
__ri void flushRegs(mV)
{
	if (!doRegAlloc)
		mVU.regAlloc->flushAll();
}

void doIbit(mV)
{
	if (mVUup.iBit)
	{
		incPC(-1);
		mVU.regAlloc->clearRegVF(33);
		if (EmuConfig.Gamefixes.IbitHack)
		{
			xMOV(gprT1, ptr32[&curI]);
			xMOV(ptr32[&::vuRegs[mVU.index].VI[REG_I]], gprT1);
		}
		else
		{
			u32 tempI;
			if (CHECK_VU_OVERFLOW(mVU.index) && ((curI & 0x7fffffff) >= 0x7f800000))
				tempI = (0x80000000 & curI) | 0x7f7fffff; // Clamp I Reg
			else
				tempI = curI;

			xMOV(ptr32[&::vuRegs[mVU.index].VI[REG_I]], tempI);
		}
		incPC(1);
	}
}

void doSwapOp(mV)
{
	if (mVUinfo.backupVF && !mVUlow.noWriteVF)
	{
		// Allocate t1 first for better chance of reg-alloc
		const xmm& t1 = mVU.regAlloc->allocReg(mVUlow.VF_write.reg);
		const xmm& t2 = mVU.regAlloc->allocReg();
		xMOVAPS(t2, t1); // Backup VF reg
		mVU.regAlloc->clearNeeded(t1);

		mVUopL(mVU, 1);

		const xmm& t3 = mVU.regAlloc->allocReg(mVUlow.VF_write.reg, mVUlow.VF_write.reg, 0xf, 0);
		xXOR.PS(t2, t3); // Swap new and old values of the register
		xXOR.PS(t3, t2); // Uses xor swap trick...
		xXOR.PS(t2, t3);
		mVU.regAlloc->clearNeeded(t3);

		incPC(1);
		doUpperOp(mVU);

		const xmm& t4 = mVU.regAlloc->allocReg(-1, mVUlow.VF_write.reg, 0xf);
		xMOVAPS(t4, t2);
		mVU.regAlloc->clearNeeded(t4);
		mVU.regAlloc->clearNeeded(t2);
	}
	else
	{
		mVUopL(mVU, 1);
		incPC(1);
		flushRegs(mVU);
		doUpperOp(mVU);
	}
}

void mVUexecuteInstruction(mV)
{
	if (mVUlow.isNOP)
	{
		incPC(1);
		doUpperOp(mVU);
		flushRegs(mVU);
		doIbit(mVU);
	}
	else if (!mVUinfo.swapOps)
	{
		incPC(1);
		doUpperOp(mVU);
		flushRegs(mVU);
		doLowerOp(mVU);
	}
	else
	{
		doSwapOp(mVU);
	}

	flushRegs(mVU);
}

//------------------------------------------------------------------
// Warnings / Errors / Illegal Instructions
//------------------------------------------------------------------

// If 1st op in block is a bad opcode, then don't compile rest of block (Dawn of Mana Level 2)
__fi void mVUcheckBadOp(mV)
{

	// The BIOS writes upper and lower NOPs in reversed slots (bug)
	//So to prevent spamming we ignore these, however its possible the real VU will bomb out if
	//this happens, so we will bomb out without warning.
	if (mVUinfo.isBadOp && mVU.code != 0x8000033c)
		mVUinfo.isEOB = true;
}

__ri void branchWarning(mV)
{
	incPC(-2);
	incPC(2);
	if (mVUup.eBit && mVUbranch)
		mVUlow.isNOP = true;

	if (mVUinfo.isBdelay && !mVUlow.evilBranch) // Check if VI Reg Written to on Branch Delay Slot Instruction
	{
		if (mVUlow.VI_write.reg && mVUlow.VI_write.used && !mVUlow.readFlags)
		{
			mVUlow.backupVI = true;
			mVUregs.viBackUp = mVUlow.VI_write.reg;
		}
	}
}

__fi void eBitPass1(mV, int& branch)
{
	if (mVUregs.blockType != 1)
	{
		branch = 1;
		mVUup.eBit = true;
	}
}

__ri void eBitWarning(mV)
{
	incPC(2);
	if (curI & _Ebit_)
		mVUregs.blockType = 1;
	incPC(-2);
}

//------------------------------------------------------------------
// Cycles / Pipeline State / Early Exit from Execution
//------------------------------------------------------------------
__fi u8 optimizeReg(u8 rState) { return (rState == 1) ? 0 : rState; }
__fi u8 calcCycles(u8 reg, u8 x) { return ((reg > x) ? (reg - x) : 0); }
__fi void incP(mV) { mVU.p ^= 1; }
__fi void incQ(mV) { mVU.q ^= 1; }

// Optimizes the End Pipeline State Removing Unnecessary Info
// If the cycles remaining is just '1', we don't have to transfer it to the next block
// because mVU automatically decrements this number at the start of its loop,
// so essentially '1' will be the same as '0'...
void mVUoptimizePipeState(mV)
{
	for (int i = 0; i < 32; i++)
	{
		mVUregs.VF[i].x = optimizeReg(mVUregs.VF[i].x);
		mVUregs.VF[i].y = optimizeReg(mVUregs.VF[i].y);
		mVUregs.VF[i].z = optimizeReg(mVUregs.VF[i].z);
		mVUregs.VF[i].w = optimizeReg(mVUregs.VF[i].w);
	}
	for (int i = 0; i < 16; i++)
		mVUregs.VI[i] = optimizeReg(mVUregs.VI[i]);
	if (mVUregs.q) { mVUregs.q = optimizeReg(mVUregs.q); if (!mVUregs.q) { incQ(mVU); } }
	if (mVUregs.p) { mVUregs.p = optimizeReg(mVUregs.p); if (!mVUregs.p) { incP(mVU); } }
	mVUregs.r = 0; // There are no stalls on the R-reg, so its Safe to discard info
}

void mVUincCycles(mV, int x)
{
	mVUcycles += x;
	// VF[0] is a constant value (0.0 0.0 0.0 1.0)
	for (int z = 31; z > 0; z--)
	{
		mVUregs.VF[z].x = calcCycles(mVUregs.VF[z].x, x);
		mVUregs.VF[z].y = calcCycles(mVUregs.VF[z].y, x);
		mVUregs.VF[z].z = calcCycles(mVUregs.VF[z].z, x);
		mVUregs.VF[z].w = calcCycles(mVUregs.VF[z].w, x);
	}
	// VI[0] is a constant value (0)
	for (int z = 15; z > 0; z--)
		mVUregs.VI[z] = calcCycles(mVUregs.VI[z], x);
	if (mVUregs.q)
	{
		if (mVUregs.q > 4)
		{
			mVUregs.q = calcCycles(mVUregs.q, x);
			if (mVUregs.q <= 4)
				mVUinfo.doDivFlag = 1;
		}
		else
			mVUregs.q = calcCycles(mVUregs.q, x);
		if (!mVUregs.q)
			incQ(mVU);
	}
	if (mVUregs.p)
	{
		mVUregs.p = calcCycles(mVUregs.p, x);
		if (!mVUregs.p || mVUregsTemp.p)
			incP(mVU);
	}
	if (mVUregs.xgkick)
	{
		mVUregs.xgkick = calcCycles(mVUregs.xgkick, x);
		if (!mVUregs.xgkick)
		{
			mVUinfo.doXGKICK = 1;
			mVUinfo.XGKICKPC = xPC;
		}
	}
	mVUregs.r = calcCycles(mVUregs.r, x);
}

// Helps check if upper/lower ops read/write to same regs...
void cmpVFregs(microVFreg& VFreg1, microVFreg& VFreg2, bool& xVar)
{
	if (VFreg1.reg == VFreg2.reg)
	{
		if ((VFreg1.x && VFreg2.x) || (VFreg1.y && VFreg2.y)
		 || (VFreg1.z && VFreg2.z) || (VFreg1.w && VFreg2.w))
			xVar = 1;
	}
}

void mVUsetCycles(mV)
{
	mVUincCycles(mVU, mVUstall);
	// If upper Op && lower Op write to same VF reg:
	if ((mVUregsTemp.VFreg[0] == mVUregsTemp.VFreg[1]) && mVUregsTemp.VFreg[0])
	{
		if (mVUregsTemp.r || mVUregsTemp.VI)
			mVUlow.noWriteVF = true;
		else
			mVUlow.isNOP = true; // If lower Op doesn't modify anything else, then make it a NOP
	}
	// If lower op reads a VF reg that upper Op writes to:
	if ((mVUlow.VF_read[0].reg || mVUlow.VF_read[1].reg) && mVUup.VF_write.reg)
	{
		cmpVFregs(mVUup.VF_write, mVUlow.VF_read[0], mVUinfo.swapOps);
		cmpVFregs(mVUup.VF_write, mVUlow.VF_read[1], mVUinfo.swapOps);
	}
	// If above case is true, and upper op reads a VF reg that lower Op Writes to:
	if (mVUinfo.swapOps && ((mVUup.VF_read[0].reg || mVUup.VF_read[1].reg) && mVUlow.VF_write.reg))
	{
		cmpVFregs(mVUlow.VF_write, mVUup.VF_read[0], mVUinfo.backupVF);
		cmpVFregs(mVUlow.VF_write, mVUup.VF_read[1], mVUinfo.backupVF);
	}

	mVUregs.VF[mVUregsTemp.VFreg[0]].x = std::max(mVUregs.VF[mVUregsTemp.VFreg[0]].x, mVUregsTemp.VF[0].x);
	mVUregs.VF[mVUregsTemp.VFreg[0]].y = std::max(mVUregs.VF[mVUregsTemp.VFreg[0]].y, mVUregsTemp.VF[0].y);
	mVUregs.VF[mVUregsTemp.VFreg[0]].z = std::max(mVUregs.VF[mVUregsTemp.VFreg[0]].z, mVUregsTemp.VF[0].z);
	mVUregs.VF[mVUregsTemp.VFreg[0]].w = std::max(mVUregs.VF[mVUregsTemp.VFreg[0]].w, mVUregsTemp.VF[0].w);

	mVUregs.VF[mVUregsTemp.VFreg[1]].x = std::max(mVUregs.VF[mVUregsTemp.VFreg[1]].x, mVUregsTemp.VF[1].x);
	mVUregs.VF[mVUregsTemp.VFreg[1]].y = std::max(mVUregs.VF[mVUregsTemp.VFreg[1]].y, mVUregsTemp.VF[1].y);
	mVUregs.VF[mVUregsTemp.VFreg[1]].z = std::max(mVUregs.VF[mVUregsTemp.VFreg[1]].z, mVUregsTemp.VF[1].z);
	mVUregs.VF[mVUregsTemp.VFreg[1]].w = std::max(mVUregs.VF[mVUregsTemp.VFreg[1]].w, mVUregsTemp.VF[1].w);

	mVUregs.VI[mVUregsTemp.VIreg]  = std::max(mVUregs.VI[mVUregsTemp.VIreg], mVUregsTemp.VI);
	mVUregs.q                      = std::max(mVUregs.q,                     mVUregsTemp.q);
	mVUregs.p                      = std::max(mVUregs.p,                     mVUregsTemp.p);
	mVUregs.r                      = std::max(mVUregs.r,                     mVUregsTemp.r);
	mVUregs.xgkick                 = std::max(mVUregs.xgkick,                mVUregsTemp.xgkick);
}

// Test cycles to see if we need to exit-early...
void mVUtestCycles(microVU& mVU, microFlagCycles& mFC)
{
	iPC = mVUstartPC;

	// If the VUSyncHack is on, we want the VU to run behind, to avoid conditions where the VU is sped up.
	if (isVU0 && EmuConfig.Speedhacks.EECycleRate != 0 && (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
	{
		switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<int>(mVUcycles)))
		{
			case -3: // 50%
				mVUcycles *= 2.0f;
				break;
			case -2: // 60%
				mVUcycles *= 1.6666667f;
				break;
			case -1: // 75%
				mVUcycles *= 1.3333333f;
				break;
			case 1: // 130%
				mVUcycles /= 1.3f;
				break;
			case 2: // 180%
				mVUcycles /= 1.8f;
				break;
			case 3: // 300%
				mVUcycles /= 3.0f;
				break;
			default:
				break;
		}
	}
	xMOV(eax, ptr32[&mVU.cycles]);
	if (EmuConfig.Gamefixes.VUSyncHack)
		xSUB(eax, mVUcycles); // Running behind, make sure we have time to run the block
	else
		xSUB(eax, 1); // Running ahead, make sure cycles left are above 0

	xForwardJNS32 skip;

	xLoadFarAddr(rax, &mVUpBlock->pState);
	xCALL((void*)mVU.copyPLState);

	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		xMOV(ptr32[&vuRegs[mVU.index].nextBlockCycles], mVUcycles);
	mVUendProgram(mVU, &mFC, 0);

	skip.SetTarget();

	xSUB(ptr32[&mVU.cycles], mVUcycles);
}

//------------------------------------------------------------------
// Initializing
//------------------------------------------------------------------

// This gets run at the start of every loop of mVU's first pass
__fi void startLoop(mV)
{
	memset(&mVUinfo, 0, sizeof(mVUinfo));
	memset(&mVUregsTemp, 0, sizeof(mVUregsTemp));
}

// Initialize VI Constants (vi15 propagates through blocks)
__fi void mVUinitConstValues(microVU& mVU)
{
	for (int i = 0; i < 16; i++)
	{
		mVUconstReg[i].isValid  = 0;
		mVUconstReg[i].regValue = 0;
	}
	mVUconstReg[15].isValid = mVUregs.vi15v;
	mVUconstReg[15].regValue = mVUregs.vi15v ? mVUregs.vi15 : 0;
}

// Initialize Variables
__fi void mVUinitFirstPass(microVU& mVU, uintptr_t pState, u8* thisPtr)
{
	mVUstartPC = iPC; // Block Start PC
	mVUbranch  = 0;   // Branch Type
	mVUcount   = 0;   // Number of instructions ran
	mVUcycles  = 0;   // Skips "M" phase, and starts counting cycles at "T" stage
	mVU.p      = 0;   // All blocks start at p index #0
	mVU.q      = 0;   // All blocks start at q index #0
	if ((uintptr_t)&mVUregs != pState) // Loads up Pipeline State Info
	{
		memcpy((u8*)&mVUregs, (u8*)pState, sizeof(microRegInfo));
	}
	if (((uintptr_t)&mVU.prog.lpState != pState))
	{
		memcpy((u8*)&mVU.prog.lpState, (u8*)pState, sizeof(microRegInfo));
	}
	mVUblock.x86ptrStart = thisPtr;
	mVUpBlock = mVUblocks[mVUstartPC / 2]->add(mVU, &mVUblock); // Add this block to block manager
	mVUregs.needExactMatch = (mVUpBlock->pState.blockType) ? 7 : 0; // ToDo: Fix 1-Op block flag linking (MGS2:Demo/Sly Cooper)
	mVUregs.blockType = 0;
	mVUregs.viBackUp  = 0;
	mVUregs.flagInfo  = 0;
	mVUsFlagHack = CHECK_VU_FLAGHACK;
	mVUinitConstValues(mVU);
}

//------------------------------------------------------------------
// Recompiler
//------------------------------------------------------------------

void mVUDoDBit(microVU& mVU, microFlagCycles* mFC)
{
	if (mVU.index && THREAD_VU1)
		xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x400 : 0x4));
	else
		xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x400 : 0x4));
	xForwardJump32 eJMP(Jcc_Zero);
	if (!isVU1 || !THREAD_VU1)
	{
		xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x200 : 0x2));
		xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
	}
	incPC(1);
	mVUDTendProgram(mVU, mFC, 1);
	incPC(-1);
	eJMP.SetTarget();
}

void mVUDoTBit(microVU& mVU, microFlagCycles* mFC)
{
	if (mVU.index && THREAD_VU1)
		xTEST(ptr32[&vu1Thread.vuFBRST], (isVU1 ? 0x800 : 0x8));
	else
		xTEST(ptr32[&vuRegs[0].VI[REG_FBRST].UL], (isVU1 ? 0x800 : 0x8));
	xForwardJump32 eJMP(Jcc_Zero);
	if (!isVU1 || !THREAD_VU1)
	{
		xOR(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], (isVU1 ? 0x400 : 0x4));
		xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_INTCINTERRUPT);
	}
	incPC(1);
	mVUDTendProgram(mVU, mFC, 1);
	incPC(-1);

	eJMP.SetTarget();
}

void mVUSaveFlags(microVU& mVU, microFlagCycles& mFC, microFlagCycles& mFCBackup)
{
	memcpy(&mFCBackup, &mFC, sizeof(microFlagCycles));
	mVUsetFlags(mVU, mFCBackup); // Sets Up Flag instances
}

static void mvuPreloadRegisters(microVU& mVU, u32 endCount)
{
	static constexpr const int REQUIRED_FREE_XMMS = 3; // some space for temps
	static constexpr const int REQUIRED_FREE_GPRS = 1; // some space for temps

	u32 vfs_loaded = 0;
	u32 vis_loaded = 0;

	for (int reg = 0; reg < mVU.regAlloc->getXmmCount(); reg++)
	{
		const int vf = mVU.regAlloc->getRegVF(reg);
		if (vf >= 0)
			vfs_loaded |= (1u << vf);
	}

	for (int reg = 0; reg < mVU.regAlloc->getGPRCount(); reg++)
	{
		const int vi = mVU.regAlloc->getRegVI(reg);
		if (vi >= 0)
			vis_loaded |= (1u << vi);
	}

	const u32 orig_pc = iPC;
	const u32 orig_code = mVU.code;
	int free_regs = mVU.regAlloc->getFreeXmmCount();
	int free_gprs = mVU.regAlloc->getFreeGPRCount();

	auto preloadVF = [&mVU, &vfs_loaded, &free_regs](u8 reg)
	{
		if (free_regs <= REQUIRED_FREE_XMMS || reg == 0 || (vfs_loaded & (1u << reg)) != 0)
			return;

		mVU.regAlloc->clearNeeded(mVU.regAlloc->allocReg(reg));
		vfs_loaded |= (1u << reg);
		free_regs--;
	};

	auto preloadVI = [&mVU, &vis_loaded, &free_gprs](u8 reg)
	{
		if (free_gprs <= REQUIRED_FREE_GPRS || reg == 0 || (vis_loaded & (1u << reg)) != 0)
			return;

		mVU.regAlloc->clearNeeded(mVU.regAlloc->allocGPR(reg));
		vis_loaded |= (1u << reg);
		free_gprs--;
	};

	auto canPreload = [&free_regs, &free_gprs]() {
		return (free_regs >= REQUIRED_FREE_XMMS || free_gprs >= REQUIRED_FREE_GPRS);
	};

	for (u32 x = 0; x < endCount && canPreload(); x++)
	{
		incPC(1);

		const microOp* info = &mVUinfo;
		if (info->doXGKICK)
			break;

		for (u32 i = 0; i < 2; i++)
		{
			preloadVF(info->uOp.VF_read[i].reg);
			preloadVF(info->lOp.VF_read[i].reg);
			if (info->lOp.VI_read[i].used)
				preloadVI(info->lOp.VI_read[i].reg);
		}

		const microVFreg& uvfr = info->uOp.VF_write;
		if (uvfr.reg != 0 && (!uvfr.x || !uvfr.y || !uvfr.z || !uvfr.w))
		{
			// not writing entire vector
			preloadVF(uvfr.reg);
		}

		const microVFreg& lvfr = info->lOp.VF_write;
		if (lvfr.reg != 0 && (!lvfr.x || !lvfr.y || !lvfr.z || !lvfr.w))
		{
			// not writing entire vector
			preloadVF(lvfr.reg);
		}

		if (info->lOp.branch)
			break;
	}

	iPC = orig_pc;
	mVU.code = orig_code;
}

void* mVUcompile(microVU& mVU, u32 startPC, uintptr_t pState)
{
	microFlagCycles mFC;
	u8* thisPtr = x86Ptr;
	const u32 endCount = (((microRegInfo*)pState)->blockType) ? 1 : (mVU.microMemSize / 8);

	// First Pass
	iPC = startPC / 4;
	mVUsetupRange(mVU, startPC, 1); // Setup Program Bounds/Range
	mVU.regAlloc->reset(false);          // Reset regAlloc
	mVUinitFirstPass(mVU, pState, thisPtr);
	mVUbranch = 0;
	for (int branch = 0; mVUcount < endCount;)
	{
		incPC(1);
		startLoop(mVU);
		mVUincCycles(mVU, 1);
		mVUopU(mVU, 0);
		mVUcheckBadOp(mVU);
		if (curI & _Ebit_)
		{
			eBitPass1(mVU, branch);
			// VU0 end of program MAC results can be read by COP2, so best to make sure the last instance is valid
			// Needed for State of Emergency 2 and Driving Emotion Type-S
			if (isVU0)
				mVUregs.needExactMatch |= 7;
		}

		if ((curI & _Mbit_) && isVU0)
		{
			if (xPC > 0)
			{
				incPC(-2);
				if (!(curI & _Mbit_)) //If the last instruction was also M-Bit we don't need to sync again
				{
					incPC(2);
					mVUup.mBit = true;
				}
				else
					incPC(2);
			}
			else
				mVUup.mBit = true;
		}

		if (curI & _Ibit_)
		{
			mVUlow.isNOP = true;
			mVUup.iBit = true;
			if (EmuConfig.Gamefixes.IbitHack)
			{
				mVUsetupRange(mVU, xPC, false);
				if (branch < 2)
					mVUsetupRange(mVU, xPC + 8, true); // Ideally we'd do +4 but the mmx compare only works in 64bits, this should be fine
			}
		}
		else
		{
			incPC(-1);
			mVUopL(mVU, 0);
			incPC(1);
		}
		if (curI & _Dbit_)
			mVUup.dBit = true;
		if (curI & _Tbit_)
			mVUup.tBit = true;
		mVUsetCycles(mVU);
		// Update XGKick information
		if (!mVUlow.isKick)
		{
			mVUregs.xgkickcycles += 1 + mVUstall;
			if (mVUlow.isMemWrite)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
		}
		else
		{
			// XGKick command counts as one cycle for the transfer.
			// Can be tested with Resident Evil: Outbreak, Kingdom Hearts, CART Fury.
			mVUregs.xgkickcycles = 1;
			mVUlow.kickcycles = 0;
		}

		mVUinfo.readQ = mVU.q;
		mVUinfo.writeQ = !mVU.q;
		mVUinfo.readP = mVU.p && isVU1;
		mVUinfo.writeP = !mVU.p && isVU1;
		mVUcount++;

		if (branch >= 2)
		{
			mVUinfo.isEOB = true;

			if (branch == 3)
			{
				mVUinfo.isBdelay = true;
			}

			branchWarning(mVU);
			if (mVUregs.xgkickcycles)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
			break;
		}
		else if (branch == 1)
		{
			branch = 2;
		}

		if (mVUbranch)
		{
			mVUsetFlagInfo(mVU);
			eBitWarning(mVU);
			branch = 3;
			mVUbranch = 0;
		}

		if (mVUup.mBit && !branch && !mVUup.eBit)
		{
			mVUregs.needExactMatch |= 7;
			if (mVUregs.xgkickcycles)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
			break;
		}

		if (mVUinfo.isEOB)
		{
			if (mVUregs.xgkickcycles)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
			break;
		}

		incPC(1);
	}

	// Fix up vi15 const info for propagation through blocks
	mVUregs.vi15 = (doConstProp && mVUconstReg[15].isValid) ? (u16)mVUconstReg[15].regValue : 0;
	mVUregs.vi15v = (doConstProp && mVUconstReg[15].isValid) ? 1 : 0;
	mVUsetFlags(mVU, mFC);           // Sets Up Flag instances
	mVUoptimizePipeState(mVU);       // Optimize the End Pipeline State for nicer Block Linking
	mVUtestCycles(mVU, mFC);         // Update VU Cycles and Exit Early if Necessary

	// Second Pass
	iPC = mVUstartPC;
	setCode();
	mVUbranch = 0;
	u32 x = 0;

	mvuPreloadRegisters(mVU, endCount);

	for (; x < endCount; x++)
	{
		if (mVUinfo.isEOB)
			x = 0xffff;
		if (mVUup.mBit)
		{
			xOR(ptr32[&vuRegs[mVU.index].flags], VUFLAG_MFLAGSET);
		}

		if (isVU1 && mVUlow.kickcycles && CHECK_XGKICKHACK)
		{
			mVU_XGKICK_SYNC(mVU, false);
		}

		mVUexecuteInstruction(mVU);
		if (!mVUinfo.isBdelay && !mVUlow.branch) //T/D Bit on branch is handled after the branch, branch delay slots are executed.
		{
			if (mVUup.tBit)
			{
				mVUDoTBit(mVU, &mFC);
			}
			else if (mVUup.dBit && doDBitHandling)
			{
				mVUDoDBit(mVU, &mFC);
			}
			else if (mVUup.mBit && !mVUup.eBit && !mVUinfo.isEOB)
			{
				// Need to make sure the flags are exact, Gungrave does FCAND with Mbit, then directly after FMAND with M-bit
				// Also call setupBranch to sort flag instances

				mVUsetupBranch(mVU, mFC);
				// Make sure we save the current state so it can come back to it
				u32* cpS = (u32*)&mVUregs;
				u32* lpS = (u32*)&mVU.prog.lpState;
				for (size_t i = 0; i < (sizeof(microRegInfo) - 4) / 4; i++, lpS++, cpS++)
				{
					xMOV(ptr32[lpS], cpS[0]);
				}
				incPC(2);
				mVUsetupRange(mVU, xPC, false);
				if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
					xMOV(ptr32[&vuRegs[mVU.index].nextBlockCycles], 0);
				mVUendProgram(mVU, &mFC, 0);
				normBranchCompile(mVU, xPC);
				incPC(-2);
				return thisPtr;
			}
		}

		if (mVUinfo.doXGKICK)
		{
			mVU_XGKICK_DELAY(mVU);
		}

		if (isEvilBlock)
		{
			mVUsetupRange(mVU, xPC + 8, false);
			normJumpCompile(mVU, mFC, true);
			return thisPtr;
		}
		else if (!mVUinfo.isBdelay)
		{
			// Handle range wrapping
			if ((xPC + 8) == mVU.microMemSize)
			{
				mVUsetupRange(mVU, xPC + 8, false);
				mVUsetupRange(mVU, 0, 1);
			}
			incPC(1);
		}
		else
		{
			incPC(1);
			mVUsetupRange(mVU, xPC, false);
			incPC(-4); // Go back to branch opcode

			switch (mVUlow.branch)
			{
				case 1: // B/BAL
				case 2:
					normBranch(mVU, mFC);
					return thisPtr;
				case 9: // JR/JALR
				case 10:
					normJump(mVU, mFC);
					return thisPtr;
				case 3: // IBEQ
					condBranch(mVU, mFC, Jcc_Equal);
					return thisPtr;
				case 4: // IBGEZ
					condBranch(mVU, mFC, Jcc_GreaterOrEqual);
					return thisPtr;
				case 5: // IBGTZ
					condBranch(mVU, mFC, Jcc_Greater);
					return thisPtr;
				case 6: // IBLEQ
					condBranch(mVU, mFC, Jcc_LessOrEqual);
					return thisPtr;
				case 7: // IBLTZ
					condBranch(mVU, mFC, Jcc_Less);
					return thisPtr;
				case 8: // IBNEQ
					condBranch(mVU, mFC, Jcc_NotEqual);
					return thisPtr;
			}
		}
	}

	// E-bit End
	mVUsetupRange(mVU, xPC, false);
	mVUendProgram(mVU, &mFC, 1);

	return thisPtr;
}

// Returns the entry point of the block (compiles it if not found)
__fi void* mVUentryGet(microVU& mVU, microBlockManager* block, u32 startPC, uintptr_t pState)
{
	microBlock* pBlock = block->search(mVU, (microRegInfo*)pState);
	if (pBlock)
		return pBlock->x86ptrStart;
	return mVUcompile(mVU, startPC, pState);
}

// Search for Existing Compiled Block (if found, return x86ptr; else, compile and return x86ptr)
__fi void* mVUblockFetch(microVU& mVU, u32 startPC, uintptr_t pState)
{
	startPC &= mVU.microMemSize - 8;

	blockCreate(startPC / 8);
	return mVUentryGet(mVU, mVUblocks[startPC / 8], startPC, pState);
}

// mVUcompileJIT() - Called By JR/JALR during execution
_mVUt void* mVUcompileJIT(u32 startPC, uintptr_t ptr)
{
	if (doJumpAsSameProgram) // Treat jump as part of same microProgram
	{
		if (doJumpCaching) // When doJumpCaching, ptr is a microBlock pointer
		{
			microVU& mVU = mVUx;
			microBlock* pBlock = (microBlock*)ptr;
			microJumpCache& jc = pBlock->jumpCache[startPC / 8];
			if (jc.prog && jc.prog == mVU.prog.quick[startPC / 8].prog)
				return jc.x86ptrStart;
			void* v = mVUblockFetch(mVUx, startPC, (uintptr_t)&pBlock->pStateEnd);
			jc.prog = mVU.prog.quick[startPC / 8].prog;
			jc.x86ptrStart = v;
			return v;
		}
		return mVUblockFetch(mVUx, startPC, ptr);
	}
	vuRegs[mVUx.index].start_pc = startPC;
	if (doJumpCaching) // When doJumpCaching, ptr is a microBlock pointer
	{
		microVU& mVU = mVUx;
		microBlock* pBlock = (microBlock*)ptr;
		microJumpCache& jc = pBlock->jumpCache[startPC / 8];
		if (jc.prog && jc.prog == mVU.prog.quick[startPC / 8].prog)
			return jc.x86ptrStart;
		void* v = mVUsearchProg<vuIndex>(startPC, (uintptr_t)&pBlock->pStateEnd);
		jc.prog = mVU.prog.quick[startPC / 8].prog;
		jc.x86ptrStart = v;
		return v;
	}
	else // When !doJumpCaching, pBlock param is really a microRegInfo pointer
	{
		return mVUsearchProg<vuIndex>(startPC, ptr); // Find and set correct program
	}
}

//------------------------------------------------------------------
// Dispatcher Functions
//------------------------------------------------------------------
static bool mvuNeedsFPCRUpdate(mV)
{
	// always update on the vu1 thread
	if (isVU1 && THREAD_VU1)
		return true;

	// otherwise only emit when it's different to the EE
	return EmuConfig.Cpu.FPUFPCR.bitmask != (isVU0 ? EmuConfig.Cpu.VU0FPCR.bitmask : EmuConfig.Cpu.VU1FPCR.bitmask);
}

// Generates the code for entering/exit recompiled blocks
void mVUdispatcherAB(mV)
{
	mVU.startFunct = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		// = The caller has already put the needed parameters in ecx/edx:
		if (!isVU1) xFastCall((void*)mVUexecuteVU0, arg1reg, arg2reg);
		else        xFastCall((void*)mVUexecuteVU1, arg1reg, arg2reg);

		// Load VU's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);

		// Load Regs
		xMOVAPS (xmmT1, ptr128[&vuRegs[mVU.index].VI[REG_P].UL]);
		xMOVAPS (xmmPQ, ptr128[&vuRegs[mVU.index].VI[REG_Q].UL]);
		xMOVDZX (xmmT2, ptr32 [&vuRegs[mVU.index].pending_q]);
		xSHUF.PS(xmmPQ, xmmT1, 0); // wzyx = PPQQ
		//Load in other Q instance
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
		xMOVSS(xmmPQ, xmmT2);
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);

		if (isVU1)
		{
			//Load in other P instance
			xMOVDZX(xmmT2, ptr32[&vuRegs[mVU.index].pending_p]);
			xPSHUF.D(xmmPQ, xmmPQ, 0x1B);
			xMOVSS(xmmPQ, xmmT2);
			xPSHUF.D(xmmPQ, xmmPQ, 0x1B);
		}

		xMOVAPS(xmmT1, ptr128[&vuRegs[mVU.index].micro_macflags]);
		xMOVAPS(ptr128[mVU.macFlag], xmmT1);


		xMOVAPS(xmmT1, ptr128[&vuRegs[mVU.index].micro_clipflags]);
		xMOVAPS(ptr128[mVU.clipFlag], xmmT1);

		xMOV(gprF0, ptr32[&vuRegs[mVU.index].micro_statusflags[0]]);
		xMOV(gprF1, ptr32[&vuRegs[mVU.index].micro_statusflags[1]]);
		xMOV(gprF2, ptr32[&vuRegs[mVU.index].micro_statusflags[2]]);
		xMOV(gprF3, ptr32[&vuRegs[mVU.index].micro_statusflags[3]]);

		// Jump to Recompiled Code Block
		xJMP(rax);

		mVU.exitFunct = x86Ptr;

		// Load EE's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

		// = The first two DWORD or smaller arguments are passed in ECX and EDX registers;
		//              all other arguments are passed right to left.
		if (!isVU1) xFastCall((void*)mVUcleanUpVU0);
		else        xFastCall((void*)mVUcleanUpVU1);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

// Generates the code for resuming/exit xgkick
void mVUdispatcherCD(mV)
{
	mVU.startFunctXG = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		// Load VU's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);

		mVUrestoreRegs(mVU);
		xMOV(gprF0, ptr32[&vuRegs[mVU.index].micro_statusflags[0]]);
		xMOV(gprF1, ptr32[&vuRegs[mVU.index].micro_statusflags[1]]);
		xMOV(gprF2, ptr32[&vuRegs[mVU.index].micro_statusflags[2]]);
		xMOV(gprF3, ptr32[&vuRegs[mVU.index].micro_statusflags[3]]);

		// Jump to Recompiled Code Block
		xJMP(ptrNative[&mVU.resumePtrXG]);

		mVU.exitFunctXG = x86Ptr;

		// Backup Status Flag (other regs were backed up on xgkick)
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[0]], gprF0);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[1]], gprF1);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[2]], gprF2);
		xMOV(ptr32[&vuRegs[mVU.index].micro_statusflags[3]], gprF3);

		// Load EE's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

static void mVUGenerateWaitMTVU(mV)
{
	mVU.waitMTVU = x86Ptr;

	int num_xmms = 0, num_gprs = 0;

	for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
	{
		if (!Register_IsCallerSaved(i) || i == rsp.Id)
			continue;

		// T1 often contains the address we're loading when waiting for VU1.
		// T2 isn't used until afterwards, so don't bother saving it.
		if (i == gprT2.Id)
			continue;

		xPUSH(xRegister64(i));
		num_gprs++;
	}

	for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
	{
		if (!RegisterSSE_IsCallerSaved(i))
			continue;

		num_xmms++;
	}

	// We need 16 byte alignment on the stack.
	// Since the stack is unaligned at entry to this function, we add 8 when it's even, not odd.
	const int stack_size = (num_xmms * sizeof(u128)) + ((~num_gprs & 1) * sizeof(u64)) + SHADOW_STACK_SIZE;
	int stack_offset = SHADOW_STACK_SIZE;

	if (stack_size > 0)
	{
		xSUB(rsp, stack_size);
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!RegisterSSE_IsCallerSaved(i))
				continue;

			xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
			stack_offset += sizeof(u128);
		}
	}

	xFastCall((void*)mVUwaitMTVU);

	stack_offset = (num_xmms - 1) * sizeof(u128) + SHADOW_STACK_SIZE;
	for (int i = static_cast<int>(iREGCNT_XMM - 1); i >= 0; i--)
	{
		if (!RegisterSSE_IsCallerSaved(i))
			continue;

		xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
		stack_offset -= sizeof(u128);
	}
	xADD(rsp, stack_size);

	for (int i = static_cast<int>(iREGCNT_GPR - 1); i >= 0; i--)
	{
		if (!Register_IsCallerSaved(i) || i == rsp.Id)
			continue;

		if (i == gprT2.Id)
			continue;

		xPOP(xRegister64(i));
	}

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

static void mVUGenerateCopyPipelineState(mV)
{
	mVU.copyPLState = x86Ptr;

	if (cpuinfo_has_x86_avx2())
	{
		xVMOVAPS(ymm0, ptr[rax]);
		xVMOVAPS(ymm1, ptr[rax + 32u]);
		xVMOVAPS(ymm2, ptr[rax + 64u]);

		xVMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState)], ymm0);
		xVMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 32u], ymm1);
		xVMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 64u], ymm2);

		xVZEROUPPER();
	}
	else
	{
		xMOVAPS(xmm0, ptr[rax]);
		xMOVAPS(xmm1, ptr[rax + 16u]);
		xMOVAPS(xmm2, ptr[rax + 32u]);
		xMOVAPS(xmm3, ptr[rax + 48u]);
		xMOVAPS(xmm4, ptr[rax + 64u]);
		xMOVAPS(xmm5, ptr[rax + 80u]);

		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState)], xmm0);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 16u], xmm1);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 32u], xmm2);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 48u], xmm3);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 64u], xmm4);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 80u], xmm5);
	}

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

//------------------------------------------------------------------
// Micro VU - Custom Quick Search
//------------------------------------------------------------------

// Generates a custom optimized block-search function
// Note: Structs must be 16-byte aligned! (GCC doesn't guarantee this)
static void mVUGenerateCompareState(mV)
{
	mVU.compareStateF = x86Ptr;

	if (cpuinfo_has_x86_avx2())
	{
		// We have to use unaligned loads here, because the blocks are only 16 byte aligned.
		xVMOVUPS(ymm0, ptr[arg1reg]);
		xVPCMP.EQD(ymm0, ymm0, ptr[arg2reg]);
		xOpWriteC5(0x66, 0xd7, eax, xRegister32(), ymm0);
		xXOR(eax, 0xffffffff);
		xForwardJNZ8 exitPoint;

		xVMOVUPS(ymm0, ptr[arg1reg + 0x20]);
		xVMOVUPS(ymm1, ptr[arg1reg + 0x40]);
		xVPCMP.EQD(ymm0, ymm0, ptr[arg2reg + 0x20]);
		xVPCMP.EQD(ymm1, ymm1, ptr[arg2reg + 0x40]);
		xVPAND(ymm0, ymm0, ymm1);

		xOpWriteC5(0x66, 0xd7, eax, xRegister32(), ymm0);
		xNOT(eax);

		exitPoint.SetTarget();
		xVZEROUPPER();
	}
	else
	{
		xMOVAPS  (xmm0, ptr32[arg1reg]);
		xPCMP.EQD(xmm0, ptr32[arg2reg]);
		xMOVAPS  (xmm1, ptr32[arg1reg + 0x10]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x10]);
		xPAND    (xmm0, xmm1);

		xMOVMSKPS(eax, xmm0);
		xXOR     (eax, 0xf);
		xForwardJNZ8 exitPoint;

		xMOVAPS  (xmm0, ptr32[arg1reg + 0x20]);
		xPCMP.EQD(xmm0, ptr32[arg2reg + 0x20]);
		xMOVAPS  (xmm1, ptr32[arg1reg + 0x30]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x30]);
		xPAND    (xmm0, xmm1);

		xMOVAPS  (xmm1, ptr32[arg1reg + 0x40]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x40]);
		xMOVAPS  (xmm2, ptr32[arg1reg + 0x50]);
		xPCMP.EQD(xmm2, ptr32[arg2reg + 0x50]);
		xPAND    (xmm1, xmm2);
		xPAND    (xmm0, xmm1);

		xMOVMSKPS(eax, xmm0);
		xXOR(eax, 0xf);

		exitPoint.SetTarget();
	}

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

//------------------------------------------------------------------
// Execution Functions
//------------------------------------------------------------------

// Executes for number of cycles
_mVUt void* mVUexecute(u32 startPC, u32 cycles)
{
	microVU& mVU    = mVUx;
	u32 vuLimit     = vuIndex ? 0x3ff8 : 0xff8;
	mVU.cycles      = cycles;
	mVU.totalCycles = cycles;
	x86Ptr = (u8*)mVU.prog.x86ptr; // Set x86ptr to where last program left off
	return mVUsearchProg<vuIndex>(startPC & vuLimit, (uintptr_t)&mVU.prog.lpState); // Find and set correct program
}

//------------------------------------------------------------------
// Cleanup Functions
//------------------------------------------------------------------

_mVUt void mVUcleanUp(void)
{
	microVU& mVU    = mVUx;

	mVU.prog.x86ptr = x86Ptr;

	if ((x86Ptr < mVU.prog.x86start) || (x86Ptr >= mVU.prog.x86end))
		mVUreset(mVU, false);

	mVU.cycles = mVU.totalCycles - std::max(0, mVU.cycles);
	vuRegs[mVU.index].cycle += mVU.cycles;

	if (!vuIndex || !THREAD_VU1)
	{
		u32 cycles_passed = std::min(mVU.cycles, 3000) * EmuConfig.Speedhacks.EECycleSkip;
		if (cycles_passed > 0)
		{
			s32 vu0_offset = vuRegs[0].cycle - cpuRegs.cycle;
			cpuRegs.cycle += cycles_passed;

			// VU0 needs to stay in sync with the CPU otherwise things get messy
			// So we need to adjust when VU1 skips cycles also
			if (!vuIndex)
				vuRegs[0].cycle  = cpuRegs.cycle + vu0_offset;
			else
				vuRegs[0].cycle += cycles_passed;
		}
	}
}

//------------------------------------------------------------------
// Caller Functions
//------------------------------------------------------------------

void* mVUexecuteVU0(u32 startPC, u32 cycles) { return mVUexecute<0>(startPC, cycles); }
void* mVUexecuteVU1(u32 startPC, u32 cycles) { return mVUexecute<1>(startPC, cycles); }
void mVUcleanUpVU0() { mVUcleanUp<0>(); }
void mVUcleanUpVU1() { mVUcleanUp<1>(); }

extern void _vu0WaitMicro();
extern void _vu0FinishMicro();

static VURegs& vu0Regs = vuRegs[0];

//------------------------------------------------------------------
// Macro VU - Helper Macros / Functions
//------------------------------------------------------------------

using namespace R5900::Dynarec;

// For now, we need to free all XMMs. Because we're not saving the nonvolatile registers when
// we enter micro mode, they will get overriden otherwise...
#define FLUSH_FOR_POSSIBLE_MICRO_EXEC (FLUSH_FREE_XMM | FLUSH_FREE_VU0)

static void setupMacroOp(int mode, const char* opName)
{
	// Set up reg allocation
	microVU0.regAlloc->reset(true);

	if (mode & 0x03) // Q will be read/written
		_freeXMMreg(xmmPQ.Id);

	// Set up MicroVU ready for new op
	microVU0.cop2 = 1;
	microVU0.prog.IRinfo.curPC = 0;
	microVU0.code = cpuRegs.code;
	memset(&microVU0.prog.IRinfo.info[0], 0, sizeof(microVU0.prog.IRinfo.info[0]));

	if (mode & 0x01) // Q-Reg will be Read
	{
		xMOVSSZX(xmmPQ, ptr32[&vuRegs[0].VI[REG_Q].UL]);
	}
	if (mode & 0x08 && (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_CLIP_FLAG)) // Clip Instruction
	{
		microVU0.prog.IRinfo.info[0].cFlag.write     = 0xff;
		microVU0.prog.IRinfo.info[0].cFlag.lastWrite = 0xff;
	}
	if (mode & 0x10)
	{
		if ((!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_STATUS_FLAG)) // Update Status Flag
		{
			microVU0.prog.IRinfo.info[0].sFlag.doFlag      = true;
			microVU0.prog.IRinfo.info[0].sFlag.doNonSticky = true;
			microVU0.prog.IRinfo.info[0].sFlag.write       = 0;
			microVU0.prog.IRinfo.info[0].sFlag.lastWrite   = 0;
		}
		if ((!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_MAC_FLAG)) // Update Mac Flags
		{
			microVU0.prog.IRinfo.info[0].mFlag.doFlag      = true;
			microVU0.prog.IRinfo.info[0].mFlag.write       = 0xff;
		}
		if ((!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & (EEINST_COP2_STATUS_FLAG | EEINST_COP2_DENORMALIZE_STATUS_FLAG)))
		{
			_freeX86reg(gprF0);

			if (!CHECK_VU_FLAGHACK || (g_pCurInstInfo->info & EEINST_COP2_DENORMALIZE_STATUS_FLAG))
			{
				// flags are normalized, so denormalize before running the first instruction
				mVUallocSFLAGd(&vuRegs[0].VI[REG_STATUS_FLAG].UL, gprF0, eax, ecx);
			}
			else
			{
				// load denormalized status flag
				// ideally we'd keep this in a register, but 32-bit...
				xMOV(gprF0, ptr32[&vuRegs->VI[REG_STATUS_FLAG].UL]);
			}
		}
	}
}

void endMacroOp(int mode)
{
	if (mode & 0x02) // Q-Reg was Written To
	{
		xMOVSS(ptr32[&vuRegs[0].VI[REG_Q].UL], xmmPQ);
	}

	microVU0.regAlloc->flushPartialForCOP2();

	if (mode & 0x10)
	{
		if (!CHECK_VU_FLAGHACK || g_pCurInstInfo->info & EEINST_COP2_NORMALIZE_STATUS_FLAG)
		{
			// Normalize
			mVUallocSFLAGc(eax, gprF0, 0);
			xMOV(ptr32[&vuRegs[0].VI[REG_STATUS_FLAG].UL], eax);
		}
		else if (g_pCurInstInfo->info & (EEINST_COP2_STATUS_FLAG | EEINST_COP2_DENORMALIZE_STATUS_FLAG))
		{
			// backup denormalized flags for the next instruction
			// this is fine, because we'll normalize them again before this reg is accessed
			xMOV(ptr32[&vuRegs->VI[REG_STATUS_FLAG].UL], gprF0);
		}
	}

	microVU0.cop2 = 0;
	microVU0.regAlloc->reset(false);
}

void mVUFreeCOP2XMMreg(int hostreg)
{
	microVU0.regAlloc->clearRegCOP2(hostreg);
}

void mVUFreeCOP2GPR(int hostreg)
{
	microVU0.regAlloc->clearGPRCOP2(hostreg);
}

bool mVUIsReservedCOP2(int hostreg)
{
	// gprF1 through 3 is not correctly used in COP2 mode.
	return (hostreg == gprT1.Id || hostreg == gprT2.Id || hostreg == gprF0.Id);
}

#define REC_COP2_mVU0(f, opName, mode) \
	void recV##f() \
	{ \
		int _mode = (mode); \
		setupMacroOp(_mode, opName); \
		if (_mode & 4) \
		{ \
			mVU_##f(microVU0, 0); \
			if (!microVU0.prog.IRinfo.info[0].lOp.isNOP) \
			{ \
				mVU_##f(microVU0, 1); \
			} \
		} \
		else \
		{ \
			mVU_##f(microVU0, 1); \
		} \
		endMacroOp(_mode); \
	}

#define INTERPRETATE_COP2_FUNC(f) \
	void recV##f() \
	{ \
		iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC); \
		xADD(ptr32[&cpuRegs.cycle], scaleblockcycles_clear()); \
		recCall(V##f); \
	}

//------------------------------------------------------------------
// Macro VU - Instructions
//------------------------------------------------------------------

//------------------------------------------------------------------
// Macro VU - Redirect Upper Instructions
//------------------------------------------------------------------

/* Mode information
0x1  reads Q reg
0x2  writes Q reg
0x4  requires analysis pass
0x8  write CLIP
0x10 writes status/mac
0x100 requires x86 regs
*/

REC_COP2_mVU0(ABS,    "ABS",    0x0);
REC_COP2_mVU0(ITOF0,  "ITOF0",  0x0);
REC_COP2_mVU0(ITOF4,  "ITOF4",  0x0);
REC_COP2_mVU0(ITOF12, "ITOF12", 0x0);
REC_COP2_mVU0(ITOF15, "ITOF15", 0x0);
REC_COP2_mVU0(FTOI0,  "FTOI0",  0x0);
REC_COP2_mVU0(FTOI4,  "FTOI4",  0x0);
REC_COP2_mVU0(FTOI12, "FTOI12", 0x0);
REC_COP2_mVU0(FTOI15, "FTOI15", 0x0);
REC_COP2_mVU0(ADD,    "ADD",    0x110);
REC_COP2_mVU0(ADDi,   "ADDi",   0x110);
REC_COP2_mVU0(ADDq,   "ADDq",   0x111);
REC_COP2_mVU0(ADDx,   "ADDx",   0x110);
REC_COP2_mVU0(ADDy,   "ADDy",   0x110);
REC_COP2_mVU0(ADDz,   "ADDz",   0x110);
REC_COP2_mVU0(ADDw,   "ADDw",   0x110);
REC_COP2_mVU0(ADDA,   "ADDA",   0x110);
REC_COP2_mVU0(ADDAi,  "ADDAi",  0x110);
REC_COP2_mVU0(ADDAq,  "ADDAq",  0x111);
REC_COP2_mVU0(ADDAx,  "ADDAx",  0x110);
REC_COP2_mVU0(ADDAy,  "ADDAy",  0x110);
REC_COP2_mVU0(ADDAz,  "ADDAz",  0x110);
REC_COP2_mVU0(ADDAw,  "ADDAw",  0x110);
REC_COP2_mVU0(SUB,    "SUB",    0x110);
REC_COP2_mVU0(SUBi,   "SUBi",   0x110);
REC_COP2_mVU0(SUBq,   "SUBq",   0x111);
REC_COP2_mVU0(SUBx,   "SUBx",   0x110);
REC_COP2_mVU0(SUBy,   "SUBy",   0x110);
REC_COP2_mVU0(SUBz,   "SUBz",   0x110);
REC_COP2_mVU0(SUBw,   "SUBw",   0x110);
REC_COP2_mVU0(SUBA,   "SUBA",   0x110);
REC_COP2_mVU0(SUBAi,  "SUBAi",  0x110);
REC_COP2_mVU0(SUBAq,  "SUBAq",  0x111);
REC_COP2_mVU0(SUBAx,  "SUBAx",  0x110);
REC_COP2_mVU0(SUBAy,  "SUBAy",  0x110);
REC_COP2_mVU0(SUBAz,  "SUBAz",  0x110);
REC_COP2_mVU0(SUBAw,  "SUBAw",  0x110);
REC_COP2_mVU0(MUL,    "MUL",    0x110);
REC_COP2_mVU0(MULi,   "MULi",   0x110);
REC_COP2_mVU0(MULq,   "MULq",   0x111);
REC_COP2_mVU0(MULx,   "MULx",   0x110);
REC_COP2_mVU0(MULy,   "MULy",   0x110);
REC_COP2_mVU0(MULz,   "MULz",   0x110);
REC_COP2_mVU0(MULw,   "MULw",   0x110);
REC_COP2_mVU0(MULA,   "MULA",   0x110);
REC_COP2_mVU0(MULAi,  "MULAi",  0x110);
REC_COP2_mVU0(MULAq,  "MULAq",  0x111);
REC_COP2_mVU0(MULAx,  "MULAx",  0x110);
REC_COP2_mVU0(MULAy,  "MULAy",  0x110);
REC_COP2_mVU0(MULAz,  "MULAz",  0x110);
REC_COP2_mVU0(MULAw,  "MULAw",  0x110);
REC_COP2_mVU0(MAX,    "MAX",    0x0);
REC_COP2_mVU0(MAXi,   "MAXi",   0x0);
REC_COP2_mVU0(MAXx,   "MAXx",   0x0);
REC_COP2_mVU0(MAXy,   "MAXy",   0x0);
REC_COP2_mVU0(MAXz,   "MAXz",   0x0);
REC_COP2_mVU0(MAXw,   "MAXw",   0x0);
REC_COP2_mVU0(MINI,   "MINI",   0x0);
REC_COP2_mVU0(MINIi,  "MINIi",  0x0);
REC_COP2_mVU0(MINIx,  "MINIx",  0x0);
REC_COP2_mVU0(MINIy,  "MINIy",  0x0);
REC_COP2_mVU0(MINIz,  "MINIz",  0x0);
REC_COP2_mVU0(MINIw,  "MINIw",  0x0);
REC_COP2_mVU0(MADD,   "MADD",   0x110);
REC_COP2_mVU0(MADDi,  "MADDi",  0x110);
REC_COP2_mVU0(MADDq,  "MADDq",  0x111);
REC_COP2_mVU0(MADDx,  "MADDx",  0x110);
REC_COP2_mVU0(MADDy,  "MADDy",  0x110);
REC_COP2_mVU0(MADDz,  "MADDz",  0x110);
REC_COP2_mVU0(MADDw,  "MADDw",  0x110);
REC_COP2_mVU0(MADDA,  "MADDA",  0x110);
REC_COP2_mVU0(MADDAi, "MADDAi", 0x110);
REC_COP2_mVU0(MADDAq, "MADDAq", 0x111);
REC_COP2_mVU0(MADDAx, "MADDAx", 0x110);
REC_COP2_mVU0(MADDAy, "MADDAy", 0x110);
REC_COP2_mVU0(MADDAz, "MADDAz", 0x110);
REC_COP2_mVU0(MADDAw, "MADDAw", 0x110);
REC_COP2_mVU0(MSUB,   "MSUB",   0x110);
REC_COP2_mVU0(MSUBi,  "MSUBi",  0x110);
REC_COP2_mVU0(MSUBq,  "MSUBq",  0x111);
REC_COP2_mVU0(MSUBx,  "MSUBx",  0x110);
REC_COP2_mVU0(MSUBy,  "MSUBy",  0x110);
REC_COP2_mVU0(MSUBz,  "MSUBz",  0x110);
REC_COP2_mVU0(MSUBw,  "MSUBw",  0x110);
REC_COP2_mVU0(MSUBA,  "MSUBA",  0x110);
REC_COP2_mVU0(MSUBAi, "MSUBAi", 0x110);
REC_COP2_mVU0(MSUBAq, "MSUBAq", 0x111);
REC_COP2_mVU0(MSUBAx, "MSUBAx", 0x110);
REC_COP2_mVU0(MSUBAy, "MSUBAy", 0x110);
REC_COP2_mVU0(MSUBAz, "MSUBAz", 0x110);
REC_COP2_mVU0(MSUBAw, "MSUBAw", 0x110);
REC_COP2_mVU0(OPMULA, "OPMULA", 0x110);
REC_COP2_mVU0(OPMSUB, "OPMSUB", 0x110);
REC_COP2_mVU0(CLIP,   "CLIP",   0x108);

//------------------------------------------------------------------
// Macro VU - Redirect Lower Instructions
//------------------------------------------------------------------

REC_COP2_mVU0(DIV,   "DIV",   0x112);
REC_COP2_mVU0(SQRT,  "SQRT",  0x112);
REC_COP2_mVU0(RSQRT, "RSQRT", 0x112);
REC_COP2_mVU0(IADD,  "IADD",  0x104);
REC_COP2_mVU0(IADDI, "IADDI", 0x104);
REC_COP2_mVU0(IAND,  "IAND",  0x104);
REC_COP2_mVU0(IOR,   "IOR",   0x104);
REC_COP2_mVU0(ISUB,  "ISUB",  0x104);
REC_COP2_mVU0(ILWR,  "ILWR",  0x104);
REC_COP2_mVU0(ISWR,  "ISWR",  0x100);
REC_COP2_mVU0(LQI,   "LQI",   0x104);
REC_COP2_mVU0(LQD,   "LQD",   0x104);
REC_COP2_mVU0(SQI,   "SQI",   0x100);
REC_COP2_mVU0(SQD,   "SQD",   0x100);
REC_COP2_mVU0(MFIR,  "MFIR",  0x104);
REC_COP2_mVU0(MTIR,  "MTIR",  0x104);
REC_COP2_mVU0(MOVE,  "MOVE",  0x0);
REC_COP2_mVU0(MR32,  "MR32",  0x0);
REC_COP2_mVU0(RINIT, "RINIT", 0x100);
REC_COP2_mVU0(RGET,  "RGET",  0x104);
REC_COP2_mVU0(RNEXT, "RNEXT", 0x104);
REC_COP2_mVU0(RXOR,  "RXOR",  0x100);

//------------------------------------------------------------------
// Macro VU - Misc...
//------------------------------------------------------------------

void recVNOP() {}
void recVWAITQ() {}
INTERPRETATE_COP2_FUNC(CALLMS);
INTERPRETATE_COP2_FUNC(CALLMSR);

//------------------------------------------------------------------
// Macro VU - Branches
//------------------------------------------------------------------
static u32 *branch_jnz32(u32 to)
{
	*(u8*)x86Ptr = 0x0F; 
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = JNZ32; 
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = to;
	x86Ptr += sizeof(u32);
	return (u32*)(x86Ptr - 4);
}

static u32 *branch_jz32(u32 to)
{
	*(u8*)x86Ptr = 0x0F; 
	x86Ptr += sizeof(u8);
	*(u8*)x86Ptr = JZ32; 
	x86Ptr += sizeof(u8);
	*(u32*)x86Ptr = to;
	x86Ptr += sizeof(u32);
	return (u32*)(x86Ptr - 4);
}

static void _setupBranchTest(u32*(jmpType)(u32), bool isLikely)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = isLikely ? false : TrySwapDelaySlot(0, 0, 0, false);
	_eeFlushAllDirty();
	//xTEST(ptr32[&vif1Regs.stat._u32], 0x4);
	xTEST(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], 0x100);
	recDoBranchImm(branchTo, jmpType(0), isLikely, swap);
}

void recBC2F(void)  { _setupBranchTest(branch_jnz32, false); }
void recBC2T(void)  { _setupBranchTest(branch_jz32,  false); }
void recBC2FL(void) { _setupBranchTest(branch_jnz32, true);  }
void recBC2TL(void) { _setupBranchTest(branch_jz32,  true);  }

//------------------------------------------------------------------
// Macro VU - COP2 Transfer Instructions
//------------------------------------------------------------------

static void COP2_Interlock(bool mBitSync)
{
	s_nBlockInterlocked = true;

	// We can safely skip the _vu0FinishMicro() call, when there's nothing
	// that can trigger a VU0 program between CFC2/CTC2/COP2 instructions.
	if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
	{
		iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC);
		_freeX86reg(eax);
		xMOV(eax, ptr32[&cpuRegs.cycle]);
		xADD(eax, scaleblockcycles_clear());
		xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles

		xTEST(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], 0x1);
		xForwardJZ32 skipvuidle;
		if (mBitSync)
		{
			xSUB(eax, ptr32[&vuRegs[0].cycle]);

			// Why do we check this here? Ratchet games, maybe others end up with flickering polygons
			// when we use lazy COP2 sync, otherwise. The micro resumption getting deferred an extra
			// EE block is apparently enough to cause issues.
			if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
				xSUB(eax, ptr32[&vuRegs[0].nextBlockCycles]);
			xCMP(eax, 4);
			xForwardJL32 skip;
			xLoadFarAddr(arg1reg, CpuVU0);
			xMOV(arg2reg, s_nBlockInterlocked);
			xFastCall((void*)BaseVUmicroCPU::ExecuteBlockJIT, arg1reg, arg2reg);
			skip.SetTarget();

			xFastCall((void*)_vu0WaitMicro);
		}
		else
			xFastCall((void*)_vu0FinishMicro);
		skipvuidle.SetTarget();
	}
}

static void mVUSyncVU0(void)
{
	iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC);
	_freeX86reg(eax);
	xMOV(eax, ptr32[&cpuRegs.cycle]);
	xADD(eax, scaleblockcycles_clear());
	xMOV(ptr32[&cpuRegs.cycle], eax); // update cycles

	xTEST(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], 0x1);
	xForwardJZ32 skipvuidle;
	xSUB(eax, ptr32[&vuRegs[0].cycle]);
	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		xSUB(eax, ptr32[&vuRegs[0].nextBlockCycles]);
	xCMP(eax, 4);
	xForwardJL32 skip;
	xLoadFarAddr(arg1reg, CpuVU0);
	xMOV(arg2reg, s_nBlockInterlocked);
	xFastCall((void*)BaseVUmicroCPU::ExecuteBlockJIT, arg1reg, arg2reg);
	skip.SetTarget();
	skipvuidle.SetTarget();
}

static void mVUFinishVU0(void)
{
	iFlushCall(FLUSH_FOR_POSSIBLE_MICRO_EXEC);
	xTEST(ptr32[&vuRegs[0].VI[REG_VPU_STAT].UL], 0x1);
	xForwardJZ32 skipvuidle;
	xFastCall((void*)_vu0FinishMicro);
	skipvuidle.SetTarget();
}

static void TEST_FBRST_RESET(int flagreg, void(*resetFunct)(), int vuIndex)
{
	xTEST(xRegister32(flagreg), (vuIndex) ? 0x200 : 0x002);
	xForwardJZ8 skip;
		xFastCall((void*)resetFunct);
	skip.SetTarget();
}

static void recCFC2(void)
{
	if (cpuRegs.code & 1)
		COP2_Interlock(false);

	if (!_Rt_)
		return;

	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);

	if (_Rd_ == 0) // why would you read vi00?
	{
		xXOR(xRegister32(regt), xRegister32(regt));
	}
	else if (_Rd_ == REG_I)
	{
		const int xmmreg = _checkXMMreg(XMMTYPE_VFREG, 33, MODE_READ);
		if (xmmreg >= 0)
		{
			xMOVD(xRegister32(regt), xRegisterSSE(xmmreg));
			xMOVSX(xRegister64(regt), xRegister32(regt));
		}
		else
		{
			xMOVSX(xRegister64(regt), ptr32[&vu0Regs.VI[_Rd_].UL]);
		}
	}
	else if (_Rd_ == REG_R)
	{
		xMOVSX(xRegister64(regt), ptr32[&vu0Regs.VI[REG_R].UL]);
		xAND(xRegister64(regt), 0x7FFFFF);
	}
	else if (_Rd_ >= REG_STATUS_FLAG) // FixMe: Should R-Reg have upper 9 bits 0?
	{
		xMOVSX(xRegister64(regt), ptr32[&vu0Regs.VI[_Rd_].UL]);
	}
	else
	{
		const int vireg = _allocIfUsedVItoX86(_Rd_, MODE_READ);
		if (vireg >= 0)
			xMOVZX(xRegister32(regt), xRegister16(vireg));
		else
			xMOVZX(xRegister32(regt), ptr16[&vu0Regs.VI[_Rd_].UL]);
	}
}

static void recCTC2(void)
{
	if (cpuRegs.code & 1)
		COP2_Interlock(true);

	if (!_Rd_)
		return;

	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	switch (_Rd_)
	{
		case REG_MAC_FLAG:
		case REG_TPC:
		case REG_VPU_STAT:
			break; // Read Only Regs
		case REG_R:
			_eeMoveGPRtoR(eax, _Rt_);
			xAND(eax, 0x7FFFFF);
			xOR(eax, 0x3f800000);
			xMOV(ptr32[&vu0Regs.VI[REG_R].UL], eax);
			break;
		case REG_STATUS_FLAG:
		{
			if (_Rt_)
			{
				_eeMoveGPRtoR(eax, _Rt_);
				xAND(eax, 0xFC0);
				xAND(ptr32[&vu0Regs.VI[REG_STATUS_FLAG].UL], 0x3F);
				xOR(ptr32[&vu0Regs.VI[REG_STATUS_FLAG].UL], eax);
			}
			else
				xAND(ptr32[&vu0Regs.VI[REG_STATUS_FLAG].UL], 0x3F);

			const int xmmtemp = _allocTempXMMreg(XMMT_INT);

			//Need to update the sticky flags for microVU
			mVUallocSFLAGd(&vu0Regs.VI[REG_STATUS_FLAG].UL);
			xMOVDZX(xRegisterSSE(xmmtemp), eax); // TODO(Stenzek): This can be a broadcast.
			xSHUF.PS(xRegisterSSE(xmmtemp), xRegisterSSE(xmmtemp), 0);
			// Make sure the values are everywhere the need to be
			xMOVAPS(ptr128[&vu0Regs.micro_statusflags], xRegisterSSE(xmmtemp));
			_freeXMMreg(xmmtemp);
			break;
		}
		case REG_CMSAR1: // Execute VU1 Micro SubRoutine
			iFlushCall(FLUSH_NONE);
			xMOV(arg1regd, 1);
			xFastCall((void*)vu1Finish);
			_eeMoveGPRtoR(arg1regd, _Rt_);
			iFlushCall(FLUSH_NONE);
			xFastCall((void*)vu1ExecMicro);
			break;
		case REG_FBRST:
			{
				if (!_Rt_)
				{
					xMOV(ptr32[&vu0Regs.VI[REG_FBRST].UL], 0);
					return;
				}

				const int flagreg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
				_eeMoveGPRtoR(xRegister32(flagreg), _Rt_);

				iFlushCall(FLUSH_FREE_VU0);
				TEST_FBRST_RESET(flagreg, vu0ResetRegs, 0);
				TEST_FBRST_RESET(flagreg, vu1ResetRegs, 1);

				xAND(xRegister32(flagreg), 0x0C0C);
				xMOV(ptr32[&vu0Regs.VI[REG_FBRST].UL], xRegister32(flagreg));
				_freeX86reg(flagreg);
			}
			break;
		case 0:
			// Ignore writes to vi00.
			break;
		default:
			// Executing vu0 block here fixes the intro of Ratchet and Clank
			// sVU's COP2 has a comment that "Donald Duck" needs this too...
			if (_Rd_ < REG_STATUS_FLAG)
			{
				// Little bit nasty, but optimal codegen.
				const int gprreg = _allocIfUsedGPRtoX86(_Rt_, MODE_READ);
				const int vireg = _allocIfUsedVItoX86(_Rd_, MODE_WRITE);
				if (vireg >= 0)
				{
					if (gprreg >= 0)
					{
						xMOVZX(xRegister32(vireg), xRegister16(gprreg));
					}
					else
					{
						// it could be in an xmm..
						const int gprxmmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
						if (gprxmmreg >= 0)
						{
							xMOVD(xRegister32(vireg), xRegisterSSE(gprxmmreg));
							xMOVZX(xRegister32(vireg), xRegister16(vireg));
						}
						else if (GPR_IS_CONST1(_Rt_))
						{
							if (_Rt_ != 0)
								xMOV(xRegister32(vireg), (g_cpuConstRegs[_Rt_].UL[0] & 0xFFFFu));
							else
								xXOR(xRegister32(vireg), xRegister32(vireg));
						}
						else
						{
							xMOVZX(xRegister32(vireg), ptr16[&cpuRegs.GPR.r[_Rt_].US[0]]);
						}
					}
				}
				else
				{
					if (gprreg >= 0)
					{
						xMOV(ptr16[&vu0Regs.VI[_Rd_].US[0]], xRegister16(gprreg));
					}
					else
					{
						const int gprxmmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
						if (gprxmmreg >= 0)
						{
							xMOVD(eax, xRegisterSSE(gprxmmreg));
							xMOV(ptr16[&vu0Regs.VI[_Rd_].US[0]], ax);
						}
						else if (GPR_IS_CONST1(_Rt_))
						{
							xMOV(ptr16[&vu0Regs.VI[_Rd_].US[0]], (g_cpuConstRegs[_Rt_].UL[0] & 0xFFFFu));
						}
						else
						{
							_eeMoveGPRtoR(eax, _Rt_);
							xMOV(ptr16[&vu0Regs.VI[_Rd_].US[0]], ax);
						}
					}
				}
			}
			else
			{
				// Move I direct to FPR if used.
				if (_Rd_ == REG_I)
				{
					const int xmmreg = _allocVFtoXMMreg(33, MODE_WRITE);
					if (_Rt_ == 0)
					{
						xPXOR(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg));
					}
					else
					{
						const int xmmgpr = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
						if (xmmgpr >= 0)
						{
							xPSHUF.D(xRegisterSSE(xmmreg), xRegisterSSE(xmmgpr), 0);
						}
						else
						{
							const int gprreg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
							if (gprreg >= 0)
								xMOVDZX(xRegisterSSE(xmmreg), xRegister32(gprreg));
							else
								xMOVSSZX(xRegisterSSE(xmmreg), ptr32[&cpuRegs.GPR.r[_Rt_].SD[0]]);
							xSHUF.PS(xRegisterSSE(xmmreg), xRegisterSSE(xmmreg), 0);
						}
					}
				}
				else
				{
					_eeMoveGPRtoM((uintptr_t)&vu0Regs.VI[_Rd_].UL, _Rt_);
				}
			}
			break;
	}
}

static void recQMFC2(void)
{
	if (cpuRegs.code & 1)
		COP2_Interlock(false);

	if (!_Rt_)
		return;
	
	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	const bool vf_used = EEINST_VFUSEDTEST(_Rd_);
	const int ftreg = _allocVFtoXMMreg(_Rd_, MODE_READ);
	_deleteEEreg128(_Rt_);

	if (vf_used)
	{
		// store direct to state if rt is not used
		const int rtreg = _allocIfUsedGPRtoXMM(_Rt_, MODE_WRITE);
		if (rtreg >= 0)
			xMOVAPS(xRegisterSSE(rtreg), xRegisterSSE(ftreg));
		else
			xMOVAPS(ptr128[&cpuRegs.GPR.r[_Rt_].UQ], xRegisterSSE(ftreg));

		// don't cache vf00, microvu doesn't like it
		if (_Rd_ == 0)
			_freeXMMreg(ftreg);
	}
	else
	{
		_reallocateXMMreg(ftreg, XMMTYPE_GPRREG, _Rt_, MODE_WRITE, true);
	}
}

static void recQMTC2(void)
{
	if (cpuRegs.code & 1)
		COP2_Interlock(true);

	if (!_Rd_)
		return;
	
	if (!(cpuRegs.code & 1))
	{
		if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
			mVUSyncVU0();
		else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
			mVUFinishVU0();
	}

	if (_Rt_)
	{
		// if we have to flush to memory anyway (has a constant or is x86), force load.
		[[maybe_unused]] const bool vf_used = EEINST_VFUSEDTEST(_Rd_);
		const bool can_rename = EEINST_RENAMETEST(_Rt_);
		const int rtreg = (GPR_IS_DIRTY_CONST(_Rt_) || _hasX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE)) ?
							  _allocGPRtoXMMreg(_Rt_, MODE_READ) :
                              _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
		
		// NOTE: can't transfer xmm15 to VF, it's reserved for PQ.
		int vfreg = _checkXMMreg(XMMTYPE_VFREG, _Rd_, MODE_WRITE);
		if (can_rename && rtreg >= 0 && rtreg != xmmPQ.Id)
		{
			// rt is no longer needed, so transfer to VF.
			if (vfreg >= 0)
				_freeXMMregWithoutWriteback(vfreg);
			_reallocateXMMreg(rtreg, XMMTYPE_VFREG, _Rd_, MODE_WRITE, true);
		}
		else
		{
			// copy to VF.
			if (vfreg < 0)
				vfreg = _allocVFtoXMMreg(_Rd_, MODE_WRITE);
			if (rtreg >= 0)
				xMOVAPS(xRegisterSSE(vfreg), xRegisterSSE(rtreg));
			else
				xMOVAPS(xRegisterSSE(vfreg), ptr128[&cpuRegs.GPR.r[_Rt_].UQ]);
		}
	}
	else
	{
		const int vfreg = _allocVFtoXMMreg(_Rd_, MODE_WRITE);
		xPXOR(xRegisterSSE(vfreg), xRegisterSSE(vfreg));
	}
}

//------------------------------------------------------------------
// Macro VU - Tables
//------------------------------------------------------------------

void recCOP2();
void recCOP2_BC2();
void recCOP2_SPEC1();
void recCOP2_SPEC2();
void rec_C2UNK(void) { }

// Recompilation
void (*recCOP2t[32])() = {
	rec_C2UNK,     recQMFC2,      recCFC2,       rec_C2UNK,     rec_C2UNK,     recQMTC2,      recCTC2,       rec_C2UNK,
	recCOP2_BC2,   rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,     rec_C2UNK,
	recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1,
	recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1, recCOP2_SPEC1,
};

void (*recCOP2_BC2t[32])() = {
	recBC2F,   recBC2T,   recBC2FL,  recBC2TL,  rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
	rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
	rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
	rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK, rec_C2UNK,
};

void (*recCOP2SPECIAL1t[64])() = {
	recVADDx,   recVADDy,   recVADDz,  recVADDw,  recVSUBx,      recVSUBy,      recVSUBz,      recVSUBw,
	recVMADDx,  recVMADDy,  recVMADDz, recVMADDw, recVMSUBx,     recVMSUBy,     recVMSUBz,     recVMSUBw,
	recVMAXx,   recVMAXy,   recVMAXz,  recVMAXw,  recVMINIx,     recVMINIy,     recVMINIz,     recVMINIw,
	recVMULx,   recVMULy,   recVMULz,  recVMULw,  recVMULq,      recVMAXi,      recVMULi,      recVMINIi,
	recVADDq,   recVMADDq,  recVADDi,  recVMADDi, recVSUBq,      recVMSUBq,     recVSUBi,      recVMSUBi,
	recVADD,    recVMADD,   recVMUL,   recVMAX,   recVSUB,       recVMSUB,      recVOPMSUB,    recVMINI,
	recVIADD,   recVISUB,   recVIADDI, rec_C2UNK, recVIAND,      recVIOR,       rec_C2UNK,     rec_C2UNK,
	recVCALLMS, recVCALLMSR,rec_C2UNK, rec_C2UNK, recCOP2_SPEC2, recCOP2_SPEC2, recCOP2_SPEC2, recCOP2_SPEC2,
};

void (*recCOP2SPECIAL2t[128])() = {
	recVADDAx,  recVADDAy, recVADDAz,  recVADDAw,  recVSUBAx,  recVSUBAy,  recVSUBAz,  recVSUBAw,
	recVMADDAx,recVMADDAy, recVMADDAz, recVMADDAw, recVMSUBAx, recVMSUBAy, recVMSUBAz, recVMSUBAw,
	recVITOF0,  recVITOF4, recVITOF12, recVITOF15, recVFTOI0,  recVFTOI4,  recVFTOI12, recVFTOI15,
	recVMULAx,  recVMULAy, recVMULAz,  recVMULAw,  recVMULAq,  recVABS,    recVMULAi,  recVCLIP,
	recVADDAq,  recVMADDAq,recVADDAi,  recVMADDAi, recVSUBAq,  recVMSUBAq, recVSUBAi,  recVMSUBAi,
	recVADDA,   recVMADDA, recVMULA,   rec_C2UNK,  recVSUBA,   recVMSUBA,  recVOPMULA, recVNOP,
	recVMOVE,   recVMR32,  rec_C2UNK,  rec_C2UNK,  recVLQI,    recVSQI,    recVLQD,    recVSQD,
	recVDIV,    recVSQRT,  recVRSQRT,  recVWAITQ,  recVMTIR,   recVMFIR,   recVILWR,   recVISWR,
	recVRNEXT,  recVRGET,  recVRINIT,  recVRXOR,   rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
	rec_C2UNK,  rec_C2UNK, rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,  rec_C2UNK,
};

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
void recCOP2() { recCOP2t[_Rs_](); }

#if defined(LOADSTORE_RECOMPILE) && defined(CP2_RECOMPILE)

/*********************************************************
* Load and store for COP2 (VU0 unit)                     *
* Format:  OP rt, offset(base)                           *
*********************************************************/

void recLQC2()
{
	if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
		mVUSyncVU0();
	else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
		mVUFinishVU0();

	vtlb_ReadRegAllocCallback alloc_cb = nullptr;
	if (_Rt_)
	{
		// init regalloc after flush
		alloc_cb = []() { return _allocVFtoXMMreg(_Rt_, MODE_WRITE); };
	}

	int xmmreg;
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0xFu;
		xmmreg = vtlb_DynGenReadQuad_Const(128, addr, alloc_cb);
	}
	else
	{
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);
		xAND(arg1regd, ~0xF);

		xmmreg = vtlb_DynGenReadQuad(128, arg1regd.Id, alloc_cb);
	}

	// toss away if loading to vf00
	if (!_Rt_)
		_freeXMMreg(xmmreg);
}

////////////////////////////////////////////////////

void recSQC2()
{
	if (g_pCurInstInfo->info & EEINST_COP2_SYNC_VU0)
		mVUSyncVU0();
	else if (g_pCurInstInfo->info & EEINST_COP2_FINISH_VU0)
		mVUFinishVU0();

	// vf00 has to be special cased here, because of the microvu temps...
	const int ftreg = _Rt_ ? _allocVFtoXMMreg(_Rt_, MODE_READ) : _allocTempXMMreg(XMMT_FPS);
	if (!_Rt_)
		xMOVAPS(xRegisterSSE(ftreg), ptr128[&vu0Regs.VF[0].F]);

	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0xFu;
		vtlb_DynGenWrite_Const(128, true, addr, ftreg);
	}
	else
	{
		_eeMoveGPRtoR(arg1regd, _Rs_);
		if (_Imm_ != 0)
			xADD(arg1regd, _Imm_);
		xAND(arg1regd, ~0xF);

		vtlb_DynGenWrite(128, true, arg1regd.Id, ftreg);
	}

	if (!_Rt_)
		_freeXMMreg(ftreg);
}

#else
namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC(LQC2);
REC_FUNC(SQC2);

#endif

} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
void recCOP2_BC2() { recCOP2_BC2t[_Rt_](); }
void recCOP2_SPEC1()
{
	if (g_pCurInstInfo->info & (EEINST_COP2_SYNC_VU0 | EEINST_COP2_FINISH_VU0))
		mVUFinishVU0();

	recCOP2SPECIAL1t[_Funct_]();

}
void recCOP2_SPEC2() { recCOP2SPECIAL2t[(cpuRegs.code & 3) | ((cpuRegs.code >> 4) & 0x7c)](); }

//------------------------------------------------------------------
// Micro VU - Main Functions
//------------------------------------------------------------------
alignas(__pagesize) static uint8_t vu0_RecDispatchers[mVUdispCacheSize];
alignas(__pagesize) static uint8_t vu1_RecDispatchers[mVUdispCacheSize];

static void mVUreserveCache(microVU& mVU)
{
	/* Micro VU Recompiler Cache */
	mVU.cache_reserve = new RecompiledCodeReserve();

	const size_t alloc_offset = mVU.index ? HostMemoryMap::mVU0recOffset : HostMemoryMap::mVU1recOffset;
	mVU.cache_reserve->Assign(GetVmMemory().CodeMemory(), alloc_offset, mVU.cacheSize * _1mb);
	mVU.cache = mVU.cache_reserve->GetPtr();
}

// Only run this once per VU! ;)
void mVUinit(microVU& mVU, uint vuIndex)
{
	memset(&mVU.prog, 0, sizeof(mVU.prog));

	mVU.index        =  vuIndex;
	mVU.cop2         =  0;
	mVU.vuMemSize    = (mVU.index ? 0x4000 : 0x1000);
	mVU.microMemSize = (mVU.index ? 0x4000 : 0x1000);
	mVU.progSize     = (mVU.index ? 0x4000 : 0x1000) / 4;
	mVU.progMemMask  =  mVU.progSize-1;
	mVU.cacheSize    =  mVUcacheReserve;
	mVU.cache        = NULL;
	mVU.dispCache    = NULL;
	mVU.startFunct   = NULL;
	mVU.exitFunct    = NULL;

	mVUreserveCache(mVU);

	if (vuIndex)
		mVU.dispCache = vu1_RecDispatchers;
	else
		mVU.dispCache = vu0_RecDispatchers;

	mVU.regAlloc.reset(new microRegAlloc(mVU.index));
}

// Resets Rec Data
void mVUreset(microVU& mVU, bool resetReserve)
{
	PageProtectionMode mode;
	if (THREAD_VU1)
	{
		// If MTVU is toggled on during gameplay we need 
		// to flush the running VU1 program, else it gets in a mess
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
			CpuVU1->Execute(vu1RunCycles);
		vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100;
	}
	// Restore reserve to uncommitted state
	if (resetReserve)
		mVU.cache_reserve->Reset();

	mode.m_read  = true;
	mode.m_write = true;
	mode.m_exec  = false;
	HostSys::MemProtect(mVU.dispCache, mVUdispCacheSize, mode);
	memset(mVU.dispCache, 0xcc, mVUdispCacheSize);

	x86Ptr = (uint8_t*)mVU.dispCache;
	mVUdispatcherAB(mVU);
	mVUdispatcherCD(mVU);
	mVUGenerateWaitMTVU(mVU);
	mVUGenerateCopyPipelineState(mVU);
	mVUGenerateCompareState(mVU);

	vuRegs[mVU.index].nextBlockCycles = 0;
	memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));

	// Program Variables
	mVU.prog.cleared  =  1;
	mVU.prog.isSame   = -1;
	mVU.prog.cur      = NULL;
	mVU.prog.total    =  0;
	mVU.prog.curFrame =  0;

	// Setup Dynarec Cache Limits for Each Program
	uint8_t* z = mVU.cache;
	mVU.prog.x86start = z;
	mVU.prog.x86ptr   = z;
	mVU.prog.x86end   = z + ((mVU.cacheSize - mVUcacheSafeZone) * _1mb);

	for (uint32_t i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
		{
			mVU.prog.prog[i] = new std::deque<microProgram*>();
			continue;
		}
		std::deque<microProgram*>::iterator it(mVU.prog.prog[i]->begin());
		for (; it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		mVU.prog.prog[i]->clear();
		mVU.prog.quick[i].block = NULL;
		mVU.prog.quick[i].prog = NULL;
	}

	mode.m_write = false;
	mode.m_exec  = true;
	HostSys::MemProtect(mVU.dispCache, mVUdispCacheSize, mode);
}

// Free Allocated Resources
void mVUclose(microVU& mVU)
{

	delete mVU.cache_reserve;
	mVU.cache_reserve = NULL;

	// Delete Programs and Block Managers
	for (uint32_t i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
			continue;
		std::deque<microProgram*>::iterator it(mVU.prog.prog[i]->begin());
		for (; it != mVU.prog.prog[i]->end(); ++it)
			mVUdeleteProg(mVU, it[0]);
		delete mVU.prog.prog[i];
		mVU.prog.prog[i] = NULL;
	}
}

// Clears Block Data in specified range
__fi void mVUclear(mV, uint32_t addr, uint32_t size)
{
	if (!mVU.prog.cleared)
	{
		mVU.prog.cleared = 1; // Next execution searches/creates a new microprogram
		memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState)); // Clear pipeline state
		for (uint32_t i = 0; i < (mVU.progSize / 2); i++)
		{
			mVU.prog.quick[i].block = NULL; // Clear current quick-reference block
			mVU.prog.quick[i].prog = NULL; // Clear current quick-reference prog
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Private Functions
//------------------------------------------------------------------

// Deletes a program
__ri void mVUdeleteProg(microVU& mVU, microProgram*& prog)
{
	for (uint32_t i = 0; i < (mVU.progSize / 2); i++)
	{
		delete prog->block[i];
		prog->block[i] = NULL;
	}
	delete prog->ranges;
	prog->ranges = NULL;
	safe_aligned_free(prog);
}

// Creates a new Micro Program
__ri microProgram* mVUcreateProg(microVU& mVU, int startPC)
{
	microProgram* prog = (microProgram*)_aligned_malloc(sizeof(microProgram), 64);
	memset(prog, 0, sizeof(microProgram));
	prog->idx = mVU.prog.total++;
	prog->ranges = new std::deque<microRange>();
	prog->startPC = startPC;
	if(doWholeProgCompare)
		mVUcacheProg(mVU, *prog); // Cache Micro Program
	return prog;
}

// Caches Micro Program
__ri void mVUcacheProg(microVU& mVU, microProgram& prog)
{
	if (!doWholeProgCompare)
	{
		auto cmpOffset = [&](void* x) { return (uint8_t*)x + mVUrange.start; };
		memcpy(cmpOffset(prog.data), cmpOffset(vuRegs[mVU.index].Micro), (mVUrange.end - mVUrange.start));
	}
	else
	{
		if (!mVU.index)
			memcpy(prog.data, vuRegs[mVU.index].Micro, 0x1000);
		else
			memcpy(prog.data, vuRegs[mVU.index].Micro, 0x4000);
	}
}

/* Generate Hash for partial program based on compiled ranges... */
uint64_t mVUrangesHash(microVU& mVU, microProgram& prog)
{
	union
	{
		uint64_t v64;
		uint32_t v32[2];
	} hash = {0};

	std::deque<microRange>::const_iterator it(prog.ranges->begin());
	for (; it != prog.ranges->end(); ++it)
	{
		for (int i = it[0].start / 4; i < it[0].end / 4; i++)
		{
			hash.v32[0] -= prog.data[i];
			hash.v32[1] ^= prog.data[i];
		}
	}
	return hash.v64;
}

// Compare Cached microProgram to vuRegs[mVU.index].Micro
__fi bool mVUcmpProg(microVU& mVU, microProgram& prog)
{
	if (doWholeProgCompare)
	{
		if (memcmp((uint8_t*)prog.data, vuRegs[mVU.index].Micro, mVU.microMemSize))
			return false;
	}
	else
	{
		for (const auto& range : *prog.ranges)
		{
			auto cmpOffset = [&](void* x) { return (uint8_t*)x + range.start; };

			if (memcmp(cmpOffset(prog.data), cmpOffset(vuRegs[mVU.index].Micro), (range.end - range.start)))
				return false;
		}
	}
	mVU.prog.cleared = 0;
	mVU.prog.cur = &prog;
	mVU.prog.isSame = doWholeProgCompare ? 1 : -1;
	return true;
}

// Searches for Cached Micro Program and sets prog.cur to it (returns entry-point to program)
_mVUt __fi void* mVUsearchProg(uint32_t startPC, uintptr_t pState)
{
	microVU& mVU = mVUx;
	microProgramQuick& quick = mVU.prog.quick[vuRegs[mVU.index].start_pc / 8];
	microProgramList*  list  = mVU.prog.prog [vuRegs[mVU.index].start_pc / 8];

	if (!quick.prog) // If null, we need to search for new program
	{
		std::deque<microProgram*>::iterator it(list->begin());
		for (; it != list->end(); ++it)
		{
			bool b = mVUcmpProg(mVU, *it[0]);

			if (b)
			{
				quick.block = it[0]->block[startPC / 8];
				quick.prog  = it[0];
				list->erase(it);
				list->push_front(quick.prog);

				// Sanity check, in case for some reason the program compilation aborted half way through (JALR for example)
				if (quick.block == nullptr)
				{
					void* entryPoint = mVUblockFetch(mVU, startPC, pState);
					return entryPoint;
				}
				return mVUentryGet(mVU, quick.block, startPC, pState);
			}
		}

		// If cleared and program not found, make a new program instance
		mVU.prog.cleared = 0;
		mVU.prog.isSame  = 1;
		mVU.prog.cur     = mVUcreateProg(mVU, vuRegs[mVU.index].start_pc/8);
		void* entryPoint = mVUblockFetch(mVU,  startPC, pState);
		quick.block      = mVU.prog.cur->block[startPC/8];
		quick.prog       = mVU.prog.cur;
		list->push_front(mVU.prog.cur);
		return entryPoint;
	}

	// If list.quick, then we've already found and recompiled the program ;)
	mVU.prog.isSame = -1;
	mVU.prog.cur = quick.prog;
	// Because the VU's can now run in sections and not whole programs at once
	// we need to set the current block so it gets the right program back
	quick.block = mVU.prog.cur->block[startPC / 8];

	// Sanity check, in case for some reason the program compilation aborted half way through
	if (quick.block == nullptr)
	{
		void* entryPoint = mVUblockFetch(mVU, startPC, pState);
		return entryPoint;
	}
	return mVUentryGet(mVU, quick.block, startPC, pState);
}

//------------------------------------------------------------------
// recMicroVU0 / recMicroVU1
//------------------------------------------------------------------

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;

recMicroVU0::recMicroVU0() { m_Idx = 0; IsInterpreter = false; }
recMicroVU1::recMicroVU1() { m_Idx = 1; IsInterpreter = false; }

void recMicroVU0::Reserve()
{
	mVUinit(microVU0, 0);
}
void recMicroVU1::Reserve()
{
	mVUinit(microVU1, 1);
	vu1Thread.Open();
}

void recMicroVU0::Shutdown()
{
	mVUclose(microVU0);
}
void recMicroVU1::Shutdown()
{
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	mVUclose(microVU1);
}

void recMicroVU0::Reset()
{
	mVUreset(microVU0, true);
}

void recMicroVU1::Reset()
{
	vu1Thread.WaitVU();
	vu1Thread.Get_MTVUChanges();
	mVUreset(microVU1, true);
}

void recMicroVU0::SetStartPC(uint32_t startPC)
{
	vuRegs[0].start_pc = startPC;
}

void recMicroVU0::Execute(uint32_t cycles)
{
	vuRegs[0].flags &= ~VUFLAG_MFLAGSET;

	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 1))
		return;
	vuRegs[0].VI[REG_TPC].UL <<= 3;

	((mVUrecCall)microVU0.startFunct)(vuRegs[0].VI[REG_TPC].UL, cycles);
	vuRegs[0].VI[REG_TPC].UL >>= 3;
	if (vuRegs[microVU0.index].flags & 0x4)
	{
		vuRegs[microVU0.index].flags &= ~0x4;
		hwIntcIrq(6);
	}
}

void recMicroVU1::SetStartPC(uint32_t startPC)
{
	vuRegs[1].start_pc = startPC;
}

void recMicroVU1::Execute(uint32_t cycles)
{
	if (!THREAD_VU1)
	{
		if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			return;
	}
	vuRegs[1].VI[REG_TPC].UL <<= 3;
	((mVUrecCall)microVU1.startFunct)(vuRegs[1].VI[REG_TPC].UL, cycles);
	vuRegs[1].VI[REG_TPC].UL >>= 3;
	if (vuRegs[microVU1.index].flags & 0x4 && !THREAD_VU1)
	{
		vuRegs[microVU1.index].flags &= ~0x4;
		hwIntcIrq(7);
	}
}

void recMicroVU0::Clear(uint32_t addr, uint32_t size)
{
	mVUclear(microVU0, addr, size);
}
void recMicroVU1::Clear(uint32_t addr, uint32_t size)
{
	mVUclear(microVU1, addr, size);
}

void recMicroVU1::ResumeXGkick()
{
	if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
		return;
	((mVUrecCallXG)microVU1.startFunctXG)();
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Freeze(microVU0.prog.lpState);
	Freeze(microVU1.prog.lpState);

	return IsOkay();
}
