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
	xe_test32_ri(regT.Id, bitTest);
	e_u8* skip; xe_fwd_jcc8(Jcc_Zero, skip);
	xe_or32_ri(reg.Id, bitSet);
	xe_fwd_set8(skip);
}

__fi void setBitFSEQ(const x32& reg, int bitX)
{
	xe_test32_ri(reg.Id, bitX);
	e_u8* skip; xe_fwd_jcc8(Jcc_Zero, skip);
	xe_or32_ri(reg.Id, bitX);
	xe_fwd_set8(skip);
}

__fi void mVUallocSFLAGa(const x32& reg, int fInstance)
{
	xe_mov32_rr(reg.Id, getFlagReg(fInstance).Id);
}

__fi void mVUallocSFLAGb(const x32& reg, int fInstance)
{
	xe_mov32_rr(getFlagReg(fInstance).Id, reg.Id);
}

// Normalize Status Flag
__ri void mVUallocSFLAGc(const x32& reg, const x32& regT, int fInstance)
{
	xe_xor32_rr(reg.Id, reg.Id);
	mVUallocSFLAGa(regT, fInstance);
	setBitSFLAG(reg, regT, 0x0f00, 0x0001); // Z  Bit
	setBitSFLAG(reg, regT, 0xf000, 0x0002); // S  Bit
	setBitSFLAG(reg, regT, 0x000f, 0x0040); // ZS Bit
	setBitSFLAG(reg, regT, 0x00f0, 0x0080); // SS Bit
	xe_and32_ri(regT.Id, 0xffff0000); // DS/DI/OS/US/D/I/O/U Bits
	xe_shr32_ri(regT.Id, 14);
	xe_or32_rr(reg.Id, regT.Id);
}

// Denormalizes Status Flag; destroys tmp1/tmp2
__ri void mVUallocSFLAGd(u32* memAddr, const x32& reg = eax, const x32& tmp1 = ecx, const x32& tmp2 = edx)
{
	xe_mov32_rm(tmp2.Id, memAddr);
	xe_mov32_rr(reg.Id, tmp2.Id);
	xe_shr32_ri(reg.Id, 3);
	xe_and32_ri(reg.Id, 0x18);

	xe_mov32_rr(tmp1.Id, tmp2.Id);
	xe_shl32_ri(tmp1.Id, 11);
	xe_and32_ri(tmp1.Id, 0x1800);
	xe_or32_rr(reg.Id, tmp1.Id);

	xe_shl32_ri(tmp2.Id, 14);
	xe_and32_ri(tmp2.Id, 0x3cf0000);
	xe_or32_rr(reg.Id, tmp2.Id);
}

__fi void mVUallocMFLAGa(mV, const x32& reg, int fInstance)
{
	xe_movzx32_rm16(reg.Id, &mVU.macFlag[fInstance]);
}

__fi void mVUallocMFLAGb(mV, const x32& reg, int fInstance)
{
	//xAND(reg, 0xffff);
	if (fInstance < 4) xe_mov32_mr(&mVU.macFlag[fInstance], reg.Id);         // microVU
	else               xe_mov32_mr(&vuRegs[mVU.index].VI[REG_MAC_FLAG].UL, reg.Id); // macroVU
}

__fi void mVUallocCFLAGa(mV, const x32& reg, int fInstance)
{
	if (fInstance < 4) xe_mov32_rm(reg.Id, &mVU.clipFlag[fInstance]);         // microVU
	else               xe_mov32_rm(reg.Id, &vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL); // macroVU
}

__fi void mVUallocCFLAGb(mV, const x32& reg, int fInstance)
{
	if (fInstance < 4) xe_mov32_mr(&mVU.clipFlag[fInstance], reg.Id);         // microVU
	else               xe_mov32_mr(&vuRegs[mVU.index].VI[REG_CLIP_FLAG].UL, reg.Id); // macroVU
}

//------------------------------------------------------------------
// VI Reg Allocators
//------------------------------------------------------------------

void microRegAlloc::writeVIBackup(const xRegisterInt& reg)
{
	microVU& mVU = index ? microVU1 : microVU0;
	xe_mov32_mr(&mVU.VIbackup, reg.Id);
}

//------------------------------------------------------------------
// P/Q Reg Allocators
//------------------------------------------------------------------

#define writeQreg(reg, qInstance) \
	if (qInstance) \
		xe_insertps_xxi(xmmPQ.Id, (reg).Id, _MM_MK_INSERTPS_NDX(0, 1, 0)); \
	else \
		xe_movss_xx(xmmPQ.Id, (reg).Id)
