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

// newVif Dynarec - Dynamically Recompiles Vif 'unpack' Packets
// authors: cottonvibes(@gmail.com)
//			Jake.Stine (@gmail.com)

#include "newVif_UnpackSSE.h"
#include "MTVU.h"

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
	, dstIndirect(arg1reg)
	, srcIndirect(arg2reg)
	, zeroReg(xmm15)
	, workReg(xmm1)
	, destReg(xmm0)
{
}

void VifUnpackSSE_Base::xMovDest() const
{
	if (!IsWriteProtectedOp())
	{
		if (IsUnmaskedOp())
			xMOVAPS(ptr[dstIndirect], destReg);
		else
			doMaskWrite(destReg);
	}
}

void VifUnpackSSE_Base::xShiftR(const xRegisterSSE& regX, int n) const
{
	if (usn) xPSRL.D(regX, n);
	else     xPSRA.D(regX, n);
}

void VifUnpackSSE_Base::xPMOVXX8(const xRegisterSSE& regX) const
{
	if (usn) xPMOVZX.BD(regX, ptr32[srcIndirect]);
	else     xPMOVSX.BD(regX, ptr32[srcIndirect]);
}

void VifUnpackSSE_Base::xPMOVXX16(const xRegisterSSE& regX) const
{
	if (usn) xPMOVZX.WD(regX, ptr64[srcIndirect]);
	else     xPMOVSX.WD(regX, ptr64[srcIndirect]);
}

void VifUnpackSSE_Base::xUPK_S_32() const
{
	switch (UnpkLoopIteration)
	{
		case 0:
			xMOVUPS(workReg, ptr32[srcIndirect]);
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v0);
			break;
		case 1:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v1);
			break;
		case 2:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v2);
			break;
		case 3:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v3);
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
				xPSHUF.D(destReg, workReg, _v0);
			break;
		case 1:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v1);
			break;
		case 2:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v2);
			break;
		case 3:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v3);
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
				xPSHUF.D(destReg, workReg, _v0);
			break;
		case 1:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v1);
			break;
		case 2:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v2);
			break;
		case 3:
			if (!IsInputMasked())
				xPSHUF.D(destReg, workReg, _v3);
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
		xMOVUPS(workReg, ptr32[srcIndirect]);
		if (IsInputMasked())
			return;
		xPSHUF.D(destReg, workReg, 0x44); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;
		xPSHUF.D(destReg, workReg, 0xEE); //v3v2v3v2
	}
	if (IsAligned)
		xBLEND.PS(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V2_16() const
{
	if (UnpkLoopIteration == 0)
	{
		xPMOVXX16(workReg);

		if (IsInputMasked())
			return;

		xPSHUF.D(destReg, workReg, 0x44); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;
		
		xPSHUF.D(destReg, workReg, 0xEE); //v3v2v3v2
	}
}

void VifUnpackSSE_Base::xUPK_V2_8() const
{
	if (UnpkLoopIteration == 0)
	{
		xPMOVXX8(workReg);

		if (IsInputMasked())
			return;

		xPSHUF.D(destReg, workReg, 0x44); //v1v0v1v0
	}
	else
	{
		if (IsInputMasked())
			return;

		xPSHUF.D(destReg, workReg, 0xEE); //v3v2v3v2
	}
}

void VifUnpackSSE_Base::xUPK_V3_32() const
{
	if (IsInputMasked())
		return;

	xMOVUPS(destReg, ptr128[srcIndirect]);
	if (UnpkLoopIteration != IsAligned)
		xBLEND.PS(destReg, zeroReg, 0x8); //zero last word - tested on ps2
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
		xBLEND.PS(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V3_8() const
{
	if (IsInputMasked())
		return;

	xPMOVXX8(destReg);
	if (UnpkLoopIteration != IsAligned)
		xBLEND.PS(destReg, zeroReg, 0x8); //zero last word - tested on ps2
}

void VifUnpackSSE_Base::xUPK_V4_32() const
{
	if (!IsInputMasked())
		xMOVUPS(destReg, ptr32[srcIndirect]);
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

	xMOVSSZX    (workReg, ptr32[srcIndirect]);
	xPSHUF.D    (workReg, workReg, _v0);
	xPSLL.D     (workReg, 3);           // ABG|R5.000
	xMOVAPS     (destReg, workReg);     // x|x|x|R
	xPSRL.D     (workReg, 8);           // ABG
	xPSLL.D     (workReg, 3);           // AB|G5.000
	mVUmergeRegs(destReg, workReg, 0x4);// x|x|G|R
	xPSRL.D     (workReg, 8);           // AB
	xPSLL.D     (workReg, 3);           // A|B5.000
	mVUmergeRegs(destReg, workReg, 0x2);// x|B|G|R
	xPSRL.D     (workReg, 8);           // A
	xPSLL.D     (workReg, 7);           // A.0000000
	mVUmergeRegs(destReg, workReg, 0x1);// A|B|G|R
	xPSLL.D     (destReg, 24); // can optimize to
	xPSRL.D     (destReg, 24); // single AND...
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

void VifUnpackSSE_Simple::doMaskWrite(const xRegisterSSE& regX) const
{
	xMOVAPS(xmm7, ptr[dstIndirect]);
	int offX = std::min(curCycle, 3);
	xPAND(regX, ptr32[nVifMask[0][offX]]);
	xPAND(xmm7, ptr32[nVifMask[1][offX]]);
	xPOR (regX, ptr32[nVifMask[2][offX]]);
	xPOR (regX, xmm7);
	xMOVAPS(ptr[dstIndirect], regX);
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

		ucall = (nVifCall)x86Ptr;
		vpugen.xUnpack(i);
		vpugen.xMovDest();
		*(u8*)x86Ptr = 0xC3;
		x86Ptr += sizeof(u8);
	}
}

void VifUnpackSSE_Init(void)
{
	if (nVifUpkExec)
		return;

	nVifUpkExec = new RecompiledCodeReserve();
	nVifUpkExec->Assign(GetVmMemory().CodeMemory(), HostMemoryMap::VIFUnpackRecOffset, _1mb);
	x86Ptr = (u8*)*nVifUpkExec;

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

void dVifReserve(int idx)
{
	if (nVif[idx].recReserve)
		return;
	
	const size_t offset = idx ? HostMemoryMap::VIF1recOffset : HostMemoryMap::VIF0recOffset;
	/* VIF Unpack Recompiler Cache */
	nVif[idx].recReserve = new RecompiledCodeReserve();
	nVif[idx].recReserve->Assign(GetVmMemory().CodeMemory(), offset, 8 * _1mb);
}

void dVifReset(int idx)
{
	nVif[idx].vifBlocks.reset();

	nVif[idx].recReserve->Reset();

	nVif[idx].recWritePtr = nVif[idx].recReserve->GetPtr();
}

void dVifRelease(int idx)
{
	if (nVif[idx].recReserve)
		nVif[idx].recReserve->Reset();
	delete nVif[idx].recReserve;
	nVif[idx].recReserve = NULL;
}

VifUnpackSSE_Dynarec::VifUnpackSSE_Dynarec(const nVifStruct& vif_, const nVifBlock& vifBlock_)
	: v(vif_)
	, vB(vifBlock_)
{
	const int wl = vB.wl ? vB.wl : 256; //0 is taken as 256 (KH2)
	isFill    = (vB.cl < wl);
	usn       = (vB.upkType>>5) & 1;
	doMask    = (vB.upkType>>4) & 1;
	doMode    = vB.mode & 3;
	IsAligned = vB.aligned;
	vCL       = 0;
}

#define makeMergeMask(x) ((((x) & 0x40) >> 6) | (((x) & 0x10) >> 3) | ((x) & 4) | (((x) & 1) << 3))

__fi void VifUnpackSSE_Dynarec::SetMasks(int cS) const
{
	const int idx = v.idx;
	const vifStruct& vif = MTVU_VifX;

	//This could have ended up copying the row when there was no row to write.1810080
	u32 m0 = vB.mask; //The actual mask example 0x03020100
	u32 m3 = ((m0 & 0xaaaaaaaa) >> 1) & ~m0; //all the upper bits, so our example 0x01010000 & 0xFCFDFEFF = 0x00010000 just the cols (shifted right for maskmerge)
	u32 m2 = (m0 & 0x55555555) & (~m0 >> 1); // 0x1000100 & 0xFE7EFF7F = 0x00000100 Just the row

	if ((doMask && m2) || doMode)
	{
		xMOVAPS(xmmRow, ptr128[&vif.MaskRow]);
	}
	if (doMask && m3)
	{
		xMOVAPS(xmmCol0, ptr128[&vif.MaskCol]);
		if ((cS >= 2) && (m3 & 0x0000ff00)) xPSHUF.D(xmmCol1, xmmCol0, _v1);
		if ((cS >= 3) && (m3 & 0x00ff0000)) xPSHUF.D(xmmCol2, xmmCol0, _v2);
		if ((cS >= 4) && (m3 & 0xff000000)) xPSHUF.D(xmmCol3, xmmCol0, _v3);
		if ((cS >= 1) && (m3 & 0x000000ff)) xPSHUF.D(xmmCol0, xmmCol0, _v0);
	}
}

void VifUnpackSSE_Dynarec::doMaskWrite(const xRegisterSSE& regX) const
{
	const int cc = std::min(vCL, 3);
	u32 m0 = (vB.mask >> (cc * 8)) & 0xff; //The actual mask example 0xE4 (protect, col, row, clear)
	u32 m3 = ((m0 & 0xaa) >> 1) & ~m0; //all the upper bits (cols shifted right) cancelling out any write protects 0x10
	u32 m2 = (m0 & 0x55) & (~m0 >> 1); // all the lower bits (rows)cancelling out any write protects 0x04
	u32 m4 = (m0 & ~((m3 << 1) | m2)) & 0x55; //  = 0xC0 & 0x55 = 0x40 (for merge mask)

	m2     = makeMergeMask(m2);
	m3     = makeMergeMask(m3);
	m4     = makeMergeMask(m4);

	if (doMask && m2) // Merge MaskRow
	{
		mVUmergeRegs(regX, xmmRow, m2);
	}
	if (doMask && m3) // Merge MaskCol
	{
		mVUmergeRegs(regX, xRegisterSSE(xmmCol0.Id + cc), m3);
	}

	if (doMode)
	{
		u32 m5 = ~(m2 | m3 | m4) & 0xf;

		if (!doMask)
			m5 = 0xf;

		if (m5 < 0xf)
		{
			xPXOR(xmmTemp, xmmTemp);
			if (doMode == 3)
			{
				mVUmergeRegs(xmmRow, regX, m5);
			}
			else
			{
				mVUmergeRegs(xmmTemp, xmmRow, m5);
				xPADD.D(regX, xmmTemp);
				if (doMode == 2)
					mVUmergeRegs(xmmRow, regX, m5);
			}
		}
		else
		{
			if (doMode == 3)
			{
				xMOVAPS(xmmRow, regX);
			}
			else
			{
				xPADD.D(regX, xmmRow);
				if (doMode == 2)
					xMOVAPS(xmmRow, regX);
			}
		}
	}
	
	if (doMask && m4) // Merge Write Protect
		mVUsaveReg(regX, ptr32[dstIndirect], m4 ^ 0xf, false);
	else
		xMOVAPS(ptr32[dstIndirect], regX);
}

static void ShiftDisplacementWindow(xAddressVoid& addr, const xRegisterLong& modReg)
{
	// Shifts the displacement factor of a given indirect address, so that the address
	// remains in the optimal 0xf0 range (which allows for byte-form displacements when
	// generating instructions).

	int addImm = 0;
	while (addr.Displacement >= 0x80)
	{
		addImm += 0xf0;
		addr   -= 0xf0;
	}
	if (addImm)
		xADD(modReg, addImm);
}

void VifUnpackSSE_Dynarec::ModUnpack(int upknum, bool PostOp)
{
	switch (upknum)
	{
		case 0:
		case 1:
		case 2:
			if (PostOp)
			{
				UnpkLoopIteration++;
				UnpkLoopIteration = UnpkLoopIteration & 0x3;
			}
			break;

		case 4:
		case 5:
		case 6:
		case 8:
			if (PostOp)
			{
				UnpkLoopIteration++;
				UnpkLoopIteration = UnpkLoopIteration & 0x1;
			}
			break;
		case 9:
		case 10:
			if (!PostOp)
				UnpkLoopIteration++;
			break;

		case 12:
		case 13:
		case 14:
		case 15:
		case 3:
		case 7:
		case 11:
			break;
	}
}

void VifUnpackSSE_Dynarec::ProcessMasks()
{
	skipProcessing = false;
	inputMasked = false;

	if (!doMask)
		return;

	const int cc = std::min(vCL, 3);
	const u32 full_mask = (vB.mask >> (cc * 8)) & 0xff;
	const u32 rowcol_mask = ((full_mask >> 1) | full_mask) & 0x55; // Rows or Cols being written instead of data, or protected.

	// Every channel is write protected for this cycle, no need to process anything.
	skipProcessing = full_mask == 0xff;

	// All channels are masked, no reason to process anything here.
	inputMasked = rowcol_mask == 0x55;
}

void VifUnpackSSE_Dynarec::CompileRoutine()
{
	const int wl        = vB.wl ? vB.wl : 256; // 0 is taken as 256 (KH2)
	const int upkNum    = vB.upkType & 0xf;
	const u8& vift      = nVifT[upkNum];
	const int cycleSize = isFill ? vB.cl : wl;
	const int blockSize = isFill ? wl : vB.cl;
	const int skipSize  = blockSize - cycleSize;

	uint vNum = vB.num ? vB.num : 256;
	doMode    = (upkNum == 0xf) ? 0 : doMode; // V4_5 has no mode feature.

	// Value passed determines # of col regs we need to load
	SetMasks(isFill ? blockSize : cycleSize);

	// Need a zero register for V2_32/V3 unpacks.
	if ((upkNum >= 8 && upkNum <= 10) || upkNum == 4)
		xXOR.PS(zeroReg, zeroReg);

	while (vNum)
	{
		ShiftDisplacementWindow(dstIndirect, arg1reg);
		ShiftDisplacementWindow(srcIndirect, arg2reg); //Don't need to do this otherwise as we arent reading the source.
		
		// Determine if reads/processing can be skipped.
		ProcessMasks();

		if (vCL < cycleSize)
		{
			ModUnpack(upkNum, false);
			xUnpack(upkNum);
			xMovDest();
			ModUnpack(upkNum, true);

			dstIndirect += 16;
			srcIndirect += vift;

			vNum--;
			if (++vCL == blockSize)
				vCL = 0;
		}
		else if (isFill)
		{
			// Filling doesn't need anything fancy, it's pretty much a normal write, just doesnt increment the source.
			xUnpack(upkNum);
			xMovDest();

			dstIndirect += 16;

			vNum--;
			if (++vCL == blockSize)
				vCL = 0;
		}
		else
		{
			dstIndirect += (16 * skipSize);
			vCL = 0;
		}
	}

	if (doMode >= 2)
	{
		const int idx = v.idx;
		xMOVAPS(ptr128[&(MTVU_VifX.MaskRow)], xmmRow);
	}

	*(u8*)x86Ptr = 0xC3;
	x86Ptr += sizeof(u8);
}

static u16 dVifComputeLength(uint cl, uint wl, u8 num, bool isFill)
{
	uint length = (num > 0) ? (num * 16) : 4096; // 0 = 256

	if (!isFill)
	{
		uint skipSize = (cl - wl) * 16;
		uint blocks   = (num + (wl - 1)) / wl; //Need to round up num's to calculate skip size correctly.
		length += (blocks - 1) * skipSize;
	}

	return std::min(length, 0xFFFFu);
}

_vifT __fi nVifBlock* dVifCompile(nVifBlock& block, bool isFill)
{
	nVifStruct& v  = nVif[idx];

	// Compile the block now
	x86Ptr         = (u8*)v.recWritePtr;

	block.startPtr = (uintptr_t)x86Ptr;
	block.length   = dVifComputeLength(block.cl, block.wl, block.num, isFill);
	v.vifBlocks.add(block);

	VifUnpackSSE_Dynarec(v, block).CompileRoutine();
	v.recWritePtr = x86Ptr;

	return &block;
}

_vifT __fi void dVifUnpack(const u8* data, bool isFill)
{
	nVifStruct&   v       = nVif[idx];
	vifStruct&    vif     = MTVU_VifX;
	VIFregisters& vifRegs = MTVU_VifXRegs;

	const u8  upkType = (vif.cmd & 0x1f) | (vif.usn << 5);
	const int doMask  = isFill ? 1 : (vif.cmd & 0x10);

	nVifBlock block;

	// Performance note: initial code was using u8/u16 field of the struct
	// directly. However reading back the data (as u32) in HashBucket.find
	// leads to various memory stalls. So it is way faster to manually build the data
	// in u32 (aka x86 register).
	//
	// Warning the order of data in hash_key/key0/key1 depends on the nVifBlock struct
	u32 hash_key = (u32)(upkType & 0xFF) << 8 | (vifRegs.num & 0xFF);

	u32 key1 = ((u32)vifRegs.cycle.wl << 24) | ((u32)vifRegs.cycle.cl << 16) | ((u32)(vif.start_aligned & 0xFF) << 8) | ((u32)vifRegs.mode & 0xFF);
	if ((upkType & 0xf) != 9)
		key1 &= 0xFFFF01FF;

	// Zero out the mask parameter if it's unused -- games leave random junk
	// values here which cause false recblock cache misses.
	u32 key0 = doMask ? vifRegs.mask : 0;

	block.hash_key = hash_key;
	block.key0 = key0;
	block.key1 = key1;

	// Seach in cache before trying to compile the block
	nVifBlock* b = v.vifBlocks.find(block);
	if (unlikely(b == nullptr))
		b = dVifCompile<idx>(block, isFill);

	{ // Execute the block
		const VURegs& VU = vuRegs[idx];
		const uint vuMemLimit = idx ? 0x4000 : 0x1000;

		u8* startmem = VU.Mem + (vif.tag.addr & (vuMemLimit - 0x10));
		u8* endmem   = VU.Mem + vuMemLimit;

		// No wrapping, you can run the fast dynarec
		if (likely((startmem + b->length) <= endmem))
			((nVifrecCall)b->startPtr)((uintptr_t)startmem, (uintptr_t)data);
		else
			_nVifUnpack(idx, data, vifRegs.mode, isFill);
	}
}

template void dVifUnpack<0>(const u8* data, bool isFill);
template void dVifUnpack<1>(const u8* data, bool isFill);
