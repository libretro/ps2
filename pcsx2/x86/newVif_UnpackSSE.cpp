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

#include "newVif_UnpackSSE.h"
#include "common/emitter/c89ops.h"

//alignas(__pagesize) static u8 nVifUpkExec[__pagesize*4];
static RecompiledCodeReserve* nVifUpkExec = NULL;

// =====================================================================================================
//  VifUnpackSSE_Base Section
// =====================================================================================================
VifUnpackSSE_Base::VifUnpackSSE_Base()
	: usn(false)
	, doMask(false)
	, UnpkLoopIteration(0)
	, IsAligned(0)
	, dstIndirect(e_mem_bd(XE_ARG1, 0))
	, srcIndirect(e_mem_bd(XE_ARG2, 0))
	, zeroReg(15)
	, workReg(1)
	, destReg(0)
{
}

void VifUnpackSSE_Base::xMovDest() const
{
	if (!IsWriteProtectedOp())
	{
		if (IsUnmaskedOp())
			xe_movaps_memxg(dstIndirect, destReg);
		else
			doMaskWrite(destReg);
	}
}

void VifUnpackSSE_Base::xPMOVXX8(int regX) const
{
	if (usn) xe_pmovzxbd_xmemg(regX, srcIndirect);
	else     xe_pmovsxbd_xmemg(regX, srcIndirect);
}

void VifUnpackSSE_Base::xPMOVXX16(int regX) const
{
	if (usn) xe_pmovzxwd_xmemg(regX, srcIndirect);
	else     xe_pmovsxwd_xmemg(regX, srcIndirect);
}

void VifUnpackSSE_Base::xUPK_S_32() const
{
	switch (UnpkLoopIteration)
	{
		case 0:
			xe_movups_xmemg(workReg, srcIndirect);
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v0);
			break;
		case 1:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v1);
			break;
		case 2:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v2);
			break;
		case 3:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v3);
			break;
	}
}

void VifUnpackSSE_Base::xUPK_S_16() const
{
	switch (UnpkLoopIteration)
	{
		case 0:
			xPMOVXX16(workReg);
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v0);
			break;
		case 1:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v1);
			break;
		case 2:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v2);
			break;
		case 3:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v3);
			break;
	}
}

void VifUnpackSSE_Base::xUPK_S_8() const
{
	switch (UnpkLoopIteration)
	{
		case 0:
			xPMOVXX8(workReg);
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v0);
			break;
		case 1:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v1);
			break;
		case 2:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v2);
			break;
		case 3:
			if (!IsInputMasked())
				xe_pshufd_xxi(destReg, workReg, _v3);
			break;
	}
}

// The V2 + V3 unpacks have freaky behaviour, the manual claims "indeterminate".
// After testing on the PS2, it's very much determinate in 99% of cases
// and games like Lemmings, And1 Streetball rely on this data to be like this!
// I have commented after each shuffle to show what data is going where - Ref

void VifUnpackSSE_Base::xUPK_V2_32() const
{
	if (UnpkLoopIteration == 0)
	{
		xe_movups_xmemg(workReg, srcIndirect);
		if (IsInputMasked())
			return;
		xe_pshufd_xxi(destReg, workReg, 0x44); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;
		xe_pshufd_xxi(destReg, workReg, 0xEE); //v3v2v3v2
	}
	if (IsAligned)
		xe_blendps_xxi(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V2_16() const
{
	if (UnpkLoopIteration == 0)
	{
		xPMOVXX16(workReg);

		if (IsInputMasked())
			return;

		xe_pshufd_xxi(destReg, workReg, 0x44); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;
		
		xe_pshufd_xxi(destReg, workReg, 0xEE); //v3v2v3v2
	}
}

void VifUnpackSSE_Base::xUPK_V2_8() const
{
	if (UnpkLoopIteration == 0)
	{
		xPMOVXX8(workReg);

		if (!IsInputMasked())
			xe_pshufd_xxi(destReg, workReg, 0x44); //v1v0v1v0
	}
	else
	{
		if (!IsInputMasked())
			xe_pshufd_xxi(destReg, workReg, 0xEE); //v3v2v3v2
	}
}

void VifUnpackSSE_Base::xUPK_V3_32() const
{
	if (IsInputMasked())
		return;

	xe_movups_xmemg(destReg, srcIndirect);
	if (UnpkLoopIteration != IsAligned)
		xe_blendps_xxi(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V3_16() const
{
	if (IsInputMasked())
		return;

	xPMOVXX16(destReg);

	//With V3-16, it takes the first vector from the next position as the W vector
	//However - IF the end of this iteration of the unpack falls on a quadword boundary, W becomes 0
	//IsAligned is the position through the current QW in the vif packet
	//Iteration counts where we are in the packet.
	int result = (((UnpkLoopIteration / 4) + 1 + (4 - IsAligned)) & 0x3);

	if ((UnpkLoopIteration & 0x1) == 0 && result == 0)
		xe_blendps_xxi(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V3_8() const
{
	if (IsInputMasked())
		return;

	xPMOVXX8(destReg);
	if (UnpkLoopIteration != IsAligned)
		xe_blendps_xxi(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V4_32() const
{
	if (!IsInputMasked())
		xe_movups_xmemg(destReg, srcIndirect);
}

void VifUnpackSSE_Base::xUPK_V4_16() const
{
	if (!IsInputMasked())
		xPMOVXX16(destReg);
}

void VifUnpackSSE_Base::xUPK_V4_8() const
{
	if (!IsInputMasked())
		xPMOVXX8(destReg);
}

void VifUnpackSSE_Base::xUPK_V4_5() const
{
	if (IsInputMasked())
		return;

	xe_movss_xmemg(workReg, srcIndirect);
	xe_pshufd_xxi(workReg, workReg, _v0);
	xe_pslld_xi(workReg, 3);           // ABG|R5.000
	xe_movaps_xx(destReg, workReg);     // x|x|x|R
	xe_psrld_xi(workReg, 8);           // ABG
	xe_pslld_xi(workReg, 3);           // AB|G5.000
	mVUmergeRegs(destReg, workReg, 0x4);// x|x|G|R
	xe_psrld_xi(workReg, 8);           // AB
	xe_pslld_xi(workReg, 3);           // A|B5.000
	mVUmergeRegs(destReg, workReg, 0x2);// x|B|G|R
	xe_psrld_xi(workReg, 8);           // A
	xe_pslld_xi(workReg, 7);           // A.0000000
	mVUmergeRegs(destReg, workReg, 0x1);// A|B|G|R
	xe_pslld_xi(destReg, 24); // can optimize to
	xe_psrld_xi(destReg, 24); // single AND...
}

void VifUnpackSSE_Base::xUnpack(int upknum) const
{
	switch (upknum)
	{
		case 0:  xUPK_S_32();  break;
		case 1:  xUPK_S_16();  break;
		case 2:  xUPK_S_8();   break;

		case 4:  xUPK_V2_32(); break;
		case 5:  xUPK_V2_16(); break;
		case 6:  xUPK_V2_8();  break;

		case 8:  xUPK_V3_32(); break;
		case 9:  xUPK_V3_16(); break;
		case 10: xUPK_V3_8();  break;

		case 12: xUPK_V4_32(); break;
		case 13: xUPK_V4_16(); break;
		case 14: xUPK_V4_8();  break;
		case 15: xUPK_V4_5();  break;

		case 3:
		case 7:
		case 11:
			break;
	}
}

// =====================================================================================================
//  VifUnpackSSE_Simple
// =====================================================================================================

void VifUnpackSSE_Simple::doMaskWrite(int regX) const
{
	xe_movaps_xmemg(7, dstIndirect);
	int offX = C89_MIN(curCycle, 3);
	xe_pand_xm(regX, nVifMask[0][offX]);
	xe_pand_xm(7, nVifMask[1][offX]);
	xe_por_xm(regX, nVifMask[2][offX]);
	xe_por_xx(regX, 7);
	xe_movaps_memxg(dstIndirect, regX);
}

// ecx = dest, edx = src
static void nVifGen(int usn, int mask, int curCycle)
{
	VifUnpackSSE_Simple vpugen;
	int usnpart      = usn * 2 * 16;
	int maskpart     = mask * 16;

	vpugen.usn       = !!usn;
	vpugen.doMask    = !!mask;
	vpugen.curCycle  = curCycle;
	vpugen.IsAligned = true;

	for (int i = 0; i < 16; ++i)
	{
		nVifCall& ucall(nVifUpk[((usnpart + maskpart + i) * 4) + curCycle]);
		ucall = NULL;
		if (nVifT[i] == 0)
			continue;

		ucall = (nVifCall)xGetAlignedCallTarget();
		vpugen.xUnpack(i);
		vpugen.xMovDest();
		xe_ret();
	}
}

void VifUnpackSSE_Init(void)
{
	if (nVifUpkExec)
		return;

	nVifUpkExec = new RecompiledCodeReserve();
	nVifUpkExec->Assign(GetVmMemory().CodeMemory(), HostMemoryMap::VIFUnpackRecOffset, _1mb);
	xSetPtr(*nVifUpkExec);

	for (int a = 0; a < 2; a++)
		for (int b = 0; b < 2; b++)
			for (int c = 0; c < 4; c++)
				nVifGen(a, b, c);

	nVifUpkExec->ForbidModification();
}

void VifUnpackSSE_Destroy(void)
{
	delete nVifUpkExec;
	nVifUpkExec = NULL;
}
