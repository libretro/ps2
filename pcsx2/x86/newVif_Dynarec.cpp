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
#include "common/emitter/c89ops.h"
#include "../MTVU.h"

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

void VifUnpackSSE_InitDynarec(struct VifUnpackSSE* p, const nVifStruct* vif_, const nVifBlock* vifBlock_)
{
	int wl;
	VifUnpackSSE_Init_State(p, VIFUNPACK_DYNAREC);
	p->v  = vif_;
	p->vB = vifBlock_;
	wl = p->vB->wl ? p->vB->wl : 256; /* 0 is taken as 256 (KH2) */
	p->isFill    = (p->vB->cl < wl);
	p->usn       = (p->vB->upkType >> 5) & 1;
	p->doMask    = (p->vB->upkType >> 4) & 1;
	p->doMode    = p->vB->mode & 3;
	p->IsAligned = p->vB->aligned;
	p->vCL       = 0;
}

#define makeMergeMask(x) ((((x) & 0x40) >> 6) | (((x) & 0x10) >> 3) | ((x) & 4) | (((x) & 1) << 3))

static void VifUnpackSSE_SetMasks(const struct VifUnpackSSE* p, int cS)
{
	const int idx = p->v->idx;
	const vifStruct& vif = MTVU_VifX;

	//This could have ended up copying the row when there was no row to write.1810080
	u32 m0 = p->vB->mask; //The actual mask example 0x03020100
	u32 m3 = ((m0 & 0xaaaaaaaa) >> 1) & ~m0; //all the upper bits, so our example 0x01010000 & 0xFCFDFEFF = 0x00010000 just the cols (shifted right for maskmerge)
	u32 m2 = (m0 & 0x55555555) & (~m0 >> 1); // 0x1000100 & 0xFE7EFF7F = 0x00000100 Just the row

	if ((p->doMask && m2) || p->doMode)
	{
		xe_movaps_xm(xmmRow, &vif.MaskRow);
	}
	if (p->doMask && m3)
	{
		xe_movaps_xm(xmmCol0, &vif.MaskCol);
		if ((cS >= 2) && (m3 & 0x0000ff00)) xe_pshufd_xxi(xmmCol1, xmmCol0, _v1);
		if ((cS >= 3) && (m3 & 0x00ff0000)) xe_pshufd_xxi(xmmCol2, xmmCol0, _v2);
		if ((cS >= 4) && (m3 & 0xff000000)) xe_pshufd_xxi(xmmCol3, xmmCol0, _v3);
		if ((cS >= 1) && (m3 & 0x000000ff)) xe_pshufd_xxi(xmmCol0, xmmCol0, _v0);
	}
}

void VifUnpackSSE_doMaskWrite_Dynarec(const struct VifUnpackSSE* p, int regX)
{
	const int cc = C89_MIN(p->vCL, 3);
	u32 m0 = (p->vB->mask >> (cc * 8)) & 0xff; //The actual mask example 0xE4 (protect, col, row, clear)
	u32 m3 = ((m0 & 0xaa) >> 1) & ~m0; //all the upper bits (cols shifted right) cancelling out any write protects 0x10
	u32 m2 = (m0 & 0x55) & (~m0 >> 1); // all the lower bits (rows)cancelling out any write protects 0x04
	u32 m4 = (m0 & ~((m3 << 1) | m2)) & 0x55; //  = 0xC0 & 0x55 = 0x40 (for merge mask)

	m2     = makeMergeMask(m2);
	m3     = makeMergeMask(m3);
	m4     = makeMergeMask(m4);

	if (p->doMask && m2) // Merge MaskRow
	{
		mVUmergeRegs(regX, xmmRow, m2, 0);
	}
	if (p->doMask && m3) // Merge MaskCol
	{
		mVUmergeRegs(regX, xmmCol0 + cc, m3, 0);
	}

	if (p->doMode)
	{
		u32 m5 = ~(m2 | m3 | m4) & 0xf;

		if (!p->doMask)
			m5 = 0xf;

		if (m5 < 0xf)
		{
			xe_pxor_xx(xmmTemp, xmmTemp);
			if (p->doMode == 3)
			{
				mVUmergeRegs(xmmRow, regX, m5, 0);
			}
			else
			{
				mVUmergeRegs(xmmTemp, xmmRow, m5, 0);
				xe_paddd_xx(regX, xmmTemp);
				if (p->doMode == 2)
					mVUmergeRegs(xmmRow, regX, m5, 0);
			}
		}
		else
		{
			if (p->doMode == 3)
			{
				xe_movaps_xx(xmmRow, regX);
			}
			else
			{
				xe_paddd_xx(regX, xmmRow);
				if (p->doMode == 2)
					xe_movaps_xx(xmmRow, regX);
			}
		}
	}
	
	if (p->doMask && m4) // Merge Write Protect
		mVUsaveReg(regX, p->dstIndirect, m4 ^ 0xf, 0);
	else
		xe_movaps_memxg(p->dstIndirect, regX);
}

static void ShiftDisplacementWindow(struct e_mem& addr, int modRegId)
{
	// Shifts the displacement factor of a given indirect address, so that the address
	// remains in the optimal 0xf0 range (which allows for byte-form displacements when
	// generating instructions).

	int addImm = 0;
	while (addr.disp >= 0x80)
	{
		addImm    += 0xf0;
		addr.disp -= 0xf0;
	}
	if (addImm)
		xe_add64_ri(modRegId, addImm);
}

void VifUnpackSSE_ModUnpack(struct VifUnpackSSE* p, int upknum, int PostOp)
{
	switch (upknum)
	{
		case 0:
		case 1:
		case 2:
			if (PostOp)
			{
				p->UnpkLoopIteration++;
				p->UnpkLoopIteration = p->UnpkLoopIteration & 0x3;
			}
			break;

		case 4:
		case 5:
		case 6:
		case 8:
			if (PostOp)
			{
				p->UnpkLoopIteration++;
				p->UnpkLoopIteration = p->UnpkLoopIteration & 0x1;
			}
			break;
		case 9:
		case 10:
			if (!PostOp)
				p->UnpkLoopIteration++;
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

void VifUnpackSSE_ProcessMasks(struct VifUnpackSSE* p)
{
	p->skipProcessing = 0;
	p->inputMasked = 0;

	if (!p->doMask)
		return;

	const int cc = C89_MIN(p->vCL, 3);
	const u32 full_mask   = (p->vB->mask >> (cc * 8)) & 0xff;
	const u32 rowcol_mask = ((full_mask >> 1) | full_mask) & 0x55; // Rows or Cols being written instead of data, or protected.

	/* Every channel is write protected for this cycle, no need to process anything. */
	p->skipProcessing = full_mask == 0xff;
	/* All channels are masked, no reason to process anything here. */
	p->inputMasked    = rowcol_mask == 0x55;
}

void VifUnpackSSE_CompileRoutine(struct VifUnpackSSE* p)
{
	const int wl        = p->vB->wl ? p->vB->wl : 256; // 0 is taken as 256 (KH2)
	const int upkNum    = p->vB->upkType & 0xf;
	const u8& vift      = nVifT[upkNum];
	const int cycleSize = p->isFill ? p->vB->cl : wl;
	const int blockSize = p->isFill ? wl : p->vB->cl;
	const int skipSize  = blockSize - cycleSize;

	uint vNum = p->vB->num ? p->vB->num : 256;
	p->doMode    = (upkNum == 0xf) ? 0 : p->doMode; // V4_5 has no mode feature.

	// Value passed determines # of col regs we need to load
	VifUnpackSSE_SetMasks(p, p->isFill ? blockSize : cycleSize);

	// Need a zero register for V2_32/V3 unpacks.
	if ((upkNum >= 8 && upkNum <= 10) || upkNum == 4)
		xe_xorps_xx(p->zeroReg, p->zeroReg);

	while (vNum)
	{
		ShiftDisplacementWindow(p->dstIndirect, XE_ARG1);
		ShiftDisplacementWindow(p->srcIndirect, XE_ARG2); //Don't need to do this otherwise as we arent reading the source.
		
		// Determine if reads/processing can be skipped.
		VifUnpackSSE_ProcessMasks(p);

		if (p->vCL < cycleSize)
		{
			VifUnpackSSE_ModUnpack(p, upkNum, 0);
			VifUnpackSSE_xUnpack(p, upkNum);
			VifUnpackSSE_xMovDest(p);
			VifUnpackSSE_ModUnpack(p, upkNum, 1);

			p->dstIndirect.disp += 16;
			p->srcIndirect.disp += vift;

			vNum--;
			if (++p->vCL == blockSize)
				p->vCL = 0;
		}
		else if (p->isFill)
		{
			// Filling doesn't need anything fancy, it's pretty much a normal write, just doesnt increment the source.
			VifUnpackSSE_xUnpack(p, upkNum);
			VifUnpackSSE_xMovDest(p);

			p->dstIndirect.disp += 16;

			vNum--;
			if (++p->vCL == blockSize)
				p->vCL = 0;
		}
		else
		{
			p->dstIndirect.disp += (16 * skipSize);
			p->vCL = 0;
		}
	}

	if (p->doMode >= 2)
	{
		const int idx = p->v->idx;
		xe_movaps_mx(&(MTVU_VifX.MaskRow), xmmRow);
	}

	xe_ret();
}

static u16 dVifComputeLength(uint cl, uint wl, u8 num, int isFill)
{
	uint length = (num > 0) ? (num * 16) : 4096; // 0 = 256

	if (!isFill)
	{
		uint skipSize = (cl - wl) * 16;
		uint blocks   = (num + (wl - 1)) / wl; //Need to round up num's to calculate skip size correctly.
		length += (blocks - 1) * skipSize;
	}

	return C89_MIN(length, 0xFFFFu);
}

_vifT __fi nVifBlock* dVifCompile(nVifBlock* block, int isFill)
{
	nVifStruct& v = nVif[idx];

	// Compile the block now
	x86Ptr = (u8*)(v.recWritePtr);

	// +1 bias keeps 0 as the empty-cell sentinel; reserve is 8MB so u32 always fits
	block->startOffset = (u32)((u8*)x86Ptr - v.recReserve->GetPtr()) + 1;
	block->length = dVifComputeLength(block->cl, block->wl, block->num, isFill);
	v.vifBlocks.add(*block);

	{
		struct VifUnpackSSE vpu;
		VifUnpackSSE_InitDynarec(&vpu, &v, block);
		VifUnpackSSE_CompileRoutine(&vpu);
	}
	v.recWritePtr = x86Ptr;

	return block;
}

_vifT __fi void dVifUnpack(const u8* data, int isFill)
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
	if (unlikely(b == NULL))
		b = dVifCompile<idx>(&block, isFill);

	/* Execute the block */
	{ 
		const VURegs& VU = vuRegs[idx];
		const uint vuMemLimit = idx ? 0x4000 : 0x1000;

		u8* startmem = VU.Mem + (vif.tag.addr & (vuMemLimit - 0x10));
		u8* endmem   = VU.Mem + vuMemLimit;

		// No wrapping, you can run the fast dynarec
		if (likely((startmem + b->length) <= endmem))
			((nVifrecCall)(v.recReserve->GetPtr() + (b->startOffset - 1)))((uptr)startmem, (uptr)data);
		else
			_nVifUnpack(idx, data, vifRegs.mode, isFill);
	}
}

template void dVifUnpack<0>(const u8* data, int isFill);
template void dVifUnpack<1>(const u8* data, int isFill);
