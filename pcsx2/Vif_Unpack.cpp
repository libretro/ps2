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

#include <cstring>

#include "Common.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "MTVU.h"

enum UnpackOffset {
	OFFSET_X = 0,
	OFFSET_Y = 1,
	OFFSET_Z = 2,
	OFFSET_W = 3
};

static __fi u32 setVifRow(vifStruct& vif, u32 reg, u32 data)
{
	vif.MaskRow._u32[reg] = data;
	return data;
}

/* cycle derives from vif.cl
 * mode derives from vifRegs.mode */
template< uint idx, uint mode, bool doMask >
static __ri void writeXYZW(u32 offnum, u32 &dest, u32 data) {
	int n = 0;

	vifStruct& vif = (idx ? ((THREAD_VU1) ? vu1Thread.vif     : vif1)     : (vif0));

	if (doMask) {
		const VIFregisters& regs = (idx ? ((THREAD_VU1) ? vu1Thread.vifRegs : vif1Regs) : (vif0Regs));
		switch (vif.cl) {
			case 0:  n = (regs.mask >> (offnum * 2)) & 0x3;		break;
			case 1:  n = (regs.mask >> ( 8 + (offnum * 2))) & 0x3;	break;
			case 2:  n = (regs.mask >> (16 + (offnum * 2))) & 0x3;	break;
			default: n = (regs.mask >> (24 + (offnum * 2))) & 0x3;	break;
		}
	}

	/* Four possible types of masking are handled below:
	 *   0 - Data
	 *   1 - MaskRow
	 *   2 - MaskCol
	 *   3 - Write protect
	 */

	switch (n) {
		case 0:
			switch (mode) {
				case 1:  dest = data + vif.MaskRow._u32[offnum]; break;
				case 2:  dest = setVifRow(vif, offnum, vif.MaskRow._u32[offnum] + data); break;
				case 3:  dest = setVifRow(vif, offnum, data); break;
				default: dest = data; break;
			}
			break;
		case 1: dest = vif.MaskRow._u32[offnum]; break;
		case 2: dest = vif.MaskCol._u32[std::min(vif.cl,3)]; break;
		case 3: break;
	}
}

template < uint idx, uint mode, bool doMask, class T >
static void UNPACK_S(u32* dest, const T* src)
{
	u32 data = *src;

	/* S-# will always be a complete packet, no matter what. So we can skip the offset bits */
	writeXYZW<idx,mode,doMask>(OFFSET_X, *(dest+0), data);
	writeXYZW<idx,mode,doMask>(OFFSET_Y, *(dest+1), data);
	writeXYZW<idx,mode,doMask>(OFFSET_Z, *(dest+2), data);
	writeXYZW<idx,mode,doMask>(OFFSET_W, *(dest+3), data);
}

/* The PS2 console actually writes v1v0v1v0 for all V2 unpacks -- the second v1v0 pair
 * being officially "indeterminate" but some games very much depend on it. */
template < uint idx, uint mode, bool doMask, class T >
static void UNPACK_V2(u32* dest, const T* src)
{
	writeXYZW<idx,mode,doMask>(OFFSET_X, *(dest+0), *(src+0));
	writeXYZW<idx,mode,doMask>(OFFSET_Y, *(dest+1), *(src+1));
	writeXYZW<idx,mode,doMask>(OFFSET_Z, *(dest+2), *(src+0));
	writeXYZW<idx,mode,doMask>(OFFSET_W, *(dest+3), *(src+1));
}

/* V3 and V4 unpacks both use the V4 unpack logic, even though most of the OFFSET_W fields
 * during V3 unpacking end up being overwritten by the next unpack.  This is confirmed real
 * hardware behavior that games such as Ape Escape 3 depend on. */
template < uint idx, uint mode, bool doMask, class T >
static void UNPACK_V4(u32* dest, const T* src)
{
	writeXYZW<idx,mode,doMask>(OFFSET_X, *(dest+0), *(src+0));
	writeXYZW<idx,mode,doMask>(OFFSET_Y, *(dest+1), *(src+1));
	writeXYZW<idx,mode,doMask>(OFFSET_Z, *(dest+2), *(src+2));
	writeXYZW<idx,mode,doMask>(OFFSET_W, *(dest+3), *(src+3));
}

/* V4_5 unpacks do not support the MODE register, and act as mode==0 always. */
template< uint idx, bool doMask >
static void UNPACK_V4_5(u32 *dest, const u32* src)
{
	u32 data = *src;

	writeXYZW<idx,0,doMask>(OFFSET_X, *(dest+0),	((data & 0x001f) << 3));
	writeXYZW<idx,0,doMask>(OFFSET_Y, *(dest+1),	((data & 0x03e0) >> 2));
	writeXYZW<idx,0,doMask>(OFFSET_Z, *(dest+2),	((data & 0x7c00) >> 7));
	writeXYZW<idx,0,doMask>(OFFSET_W, *(dest+3),	((data & 0x8000) >> 8));
}

/* =====================================================================================================
 *
 * --------------------------------------------------------------------------------------
 *  Main table for function unpacking.
 * --------------------------------------------------------------------------------------
 * The extra data bsize/dsize/etc are all duplicated between the doMask enabled and
 * disabled versions.  This is probably simpler and more efficient than bothering
 * to generate separate tables.
 *
 * The double-cast function pointer nonsense is to appease GCC, which gives some rather
 * cryptic error about being unable to deduce the type parameters (I think it's a bug
 * relating to __fastcall, which I recall having some other places as well).  It's fixed
 * by explicitly casting the function to itself prior to casting it to what we need it
 * to be cast as. --air
 */

#define UnpackFuncSet( vt, idx, mode, usn, doMask ) \
	(UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u32) UNPACK_##vt<idx, mode, doMask, u32>, \
	(UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u16) UNPACK_##vt<idx, mode, doMask, usn##16>, \
	(UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u8)  UNPACK_##vt<idx, mode, doMask, usn##8> \

#define UnpackModeSet(idx, mode) \
	UnpackFuncSet( S,  idx, mode, s, 0 ), NULL,  \
	UnpackFuncSet( V2, idx, mode, s, 0 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, s, 0 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, s, 0 ), (UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u32) UNPACK_V4_5<idx, 0>, \
 \
	UnpackFuncSet( S,  idx, mode, s, 1 ), NULL,  \
	UnpackFuncSet( V2, idx, mode, s, 1 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, s, 1 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, s, 1 ), (UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u32) UNPACK_V4_5<idx, 1>, \
 \
	UnpackFuncSet( S,  idx, mode, u, 0 ), NULL,  \
	UnpackFuncSet( V2, idx, mode, u, 0 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, u, 0 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, u, 0 ), (UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u32) UNPACK_V4_5<idx, 0>, \
 \
	UnpackFuncSet( S,  idx, mode, u, 1 ), NULL,  \
	UnpackFuncSet( V2, idx, mode, u, 1 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, u, 1 ), NULL,  \
	UnpackFuncSet( V4, idx, mode, u, 1 ), (UNPACKFUNCTYPE)(UNPACKFUNCTYPE_u32) UNPACK_V4_5<idx, 1>

/* Array sub-dimension order: [vifidx] [mode] (VN * VL * USN * doMask) */
alignas(16) static const UNPACKFUNCTYPE VIFfuncTable[2][4][4 * 4 * 2 * 2] =
{
	{
		{ UnpackModeSet(0,0) },
		{ UnpackModeSet(0,1) },
		{ UnpackModeSet(0,2) },
		{ UnpackModeSet(0,3) }
	},

	{
		{ UnpackModeSet(1,0) },
		{ UnpackModeSet(1,1) },
		{ UnpackModeSet(1,2) },
		{ UnpackModeSet(1,3) }
	}
};

/*----------------------------------------------------------------------------
 * Unpack Setup Code
 *----------------------------------------------------------------------------
 */
template<int idx> void vifUnpackSetup(const u32 *data)
{
	vifStruct& vifX       = (idx ? (vif1)     : (vif0));
	VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));

	vifX.unpackcalls++;

	if (vifX.unpackcalls > 3)
		vifExecQueue(idx);

	vifX.usn   = (vifRegs.code >> 14) & 0x01;
	int vifNum = (vifRegs.code >> 16) & 0xff;

	if (vifNum == 0)
		vifNum = 256;
	vifRegs.num =  vifNum;

	const u8& gsize = nVifT[vifX.cmd & 0x0f];

	uint wl = vifRegs.cycle.wl ? vifRegs.cycle.wl : 256;

	if (wl <= vifRegs.cycle.cl) /* Skipping write */
		vifX.tag.size = ((vifNum * gsize) + 3) / 4;
	else
	{
		/* Filling write */
		int a   = vifNum % wl;
		int max = vifRegs.cycle.cl;
		int n   = vifRegs.cycle.cl * (vifNum / wl) 
			+ ((a > max) ? max : a);

		vifX.tag.size = ((n * gsize) + 3) >> 2;
	}

	u32 addr           = vifRegs.code;
	if (idx && ((addr>>15)&1)) addr += vif1Regs.tops;
	vifX.tag.addr      = (addr<<4) & (idx ? 0x3ff0 : 0xff0);

	vifX.cl		   = 0;
	vifX.tag.cmd	   = vifX.cmd;
	vifX.pass	   = 1;

	//Ugh things are never easy.
	//Alright, in most cases with V2 and V3 we only need to know if its offset 32bits.
	//However in V3-16 if the data it requires ends on a QW boundary of the source data
	//the W vector becomes 0, so we need to know how far through the current QW the data begins.
	//same happens with V3 8
	vifX.start_aligned = 4-((vifX.vifpacketsize-1) & 0x3);
}

template void vifUnpackSetup<0>(const u32 *data);
template void vifUnpackSetup<1>(const u32 *data);

alignas(16) nVifStruct nVif[2];

// Interpreter-style SSE unpacks.  Array layout matches the interpreter C unpacks.
//  ([USN][Masking][Unpack Type]) [curCycle]
alignas(16) nVifCall nVifUpk[(2 * 2 * 16) * 4];

// This is used by the interpreted SSE unpacks only.  Recompiled SSE unpacks
// and the interpreted C unpacks use the vif.MaskRow/MaskCol members directly.
//  [MaskNumber][CycleNumber][Vector]
alignas(16) u32 nVifMask[3][4][4] = {};

// Number of bytes of data in the source stream needed for each vector.
// [equivalent to ((32 >> VL) * (VN+1)) / 8]
alignas(16) const u8 nVifT[16] = {
	4, // S-32
	2, // S-16
	1, // S-8
	0, // ----
	8, // V2-32
	4, // V2-16
	2, // V2-8
	0, // ----
	12,// V3-32
	6, // V3-16
	3, // V3-8
	0, // ----
	16,// V4-32
	8, // V4-16
	4, // V4-8
	2, // V4-5
};

// ----------------------------------------------------------------------------
template <int idx, bool doMode, bool isFill>
__ri void _nVifUnpackLoop(const u8* data);

typedef void FnType_VifUnpackLoop(const u8* data);
typedef FnType_VifUnpackLoop* Fnptr_VifUnpackLoop;

// ----------------------------------------------------------------------------

void resetNewVif(int idx)
{
	// Safety Reset : Reassign all VIF structure info, just in case the VU1 pointers have
	// changed for some reason.

	nVif[idx].idx   = idx;
	nVif[idx].bSize = 0;
	memset(nVif[idx].buffer, 0, sizeof(nVif[idx].buffer));

	dVifReset(idx);
}

template<int idx> int nVifUnpack(const u8* data)
{
	nVifStruct&   v       = nVif[idx];
	vifStruct&    vif     = (idx ? (vif1)     : (vif0));
	VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));

	const uint wl         = vifRegs.cycle.wl ? vifRegs.cycle.wl : 256;
	const uint ret        = std::min(vif.vifpacketsize, vif.tag.size);
	const bool isFill     = (vifRegs.cycle.cl < wl);
	s32        size       = ret << 2;

	if (ret == vif.tag.size) /* Full Transfer */
	{
		if (v.bSize) /* Last transfer was partial */
		{
			memcpy(&v.buffer[v.bSize], data, size);
			v.bSize += size;
			size = v.bSize;
			data = v.buffer;

			vif.cl = 0;
			vifRegs.num = (vifRegs.code >> 16) & 0xff; /* grab NUM form the original VIFcode input. */
			if (!vifRegs.num)
				vifRegs.num = 256;
		}

		if (!idx || !THREAD_VU1)
			dVifUnpack<idx>(data, isFill);
		else
			vu1Thread.VifUnpack(vif, vifRegs, (u8*)data, (size + 4) & ~0x3);

		vif.pass     = 0;
		vif.tag.size = 0;
		vif.cmd      = 0;
		vifRegs.num  = 0;
		v.bSize      = 0;
	}
	else // Partial Transfer
	{
		memcpy(&v.buffer[v.bSize], data, size);
		v.bSize += size;
		vif.tag.size -= ret;

		const u8& vSize = nVifT[vif.cmd & 0x0f];

		// We need to provide accurate accounting of the NUM register, in case games decided
		// to read back from it mid-transfer.  Since so few games actually use partial transfers
		// of VIF unpacks, this code should not be any bottleneck.

		if (isFill)
		{
			int dataSize = (size / vSize);
			vifRegs.num = vifRegs.num - (((dataSize / vifRegs.cycle.cl) * (vifRegs.cycle.wl - vifRegs.cycle.cl)) + dataSize);
		}
		else
			vifRegs.num -= (size / vSize);
	}

	return ret;
}

template int nVifUnpack<0>(const u8* data);
template int nVifUnpack<1>(const u8* data);

/* This is used by the interpreted SSE unpacks only.  Recompiled SSE unpacks
 * and the interpreted C unpacks use the vif.MaskRow/MaskCol members directly. */
static void setMasks(const vifStruct& vif, const VIFregisters& v)
{
	for (int i = 0; i < 16; i++)
	{
		int m = (v.mask >> (i * 2)) & 3;
		switch (m)
		{
			case 0: /* Data */
				nVifMask[0][i / 4][i % 4] = 0xffffffff;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
			case 1: /* MaskRow */
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif.MaskRow._u32[i % 4];
				break;
			case 2: /* MaskCol */
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif.MaskCol._u32[i / 4];
				break;
			case 3: /* Write Protect */
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0xffffffff;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
		}
	}
}

/* ----------------------------------------------------------------------------
 *  Unpacking Optimization notes:
 * ----------------------------------------------------------------------------
 * Some games send a LOT of single-cycle packets (God of War, SotC, TriAce games, etc),
 * so we always need to be weary of keeping loop setup code optimized.  It's not always
 * a "win" to move code outside the loop, like normally in most other loop scenarios.
 *
 * The biggest bottleneck of the current code is the call/ret needed to invoke the SSE
 * unpackers.  A better option is to generate the entire vifRegs.num loop code as part
 * of the SSE template, and inline the SSE code into the heart of it.  This both avoids
 * the call/ret and opens the door for resolving some register dependency chains in the
 * current emitted functions.  (this is what zero's SSE does to get it's final bit of
 * speed advantage over the new vif). --air
 *
 * The BEST optimization strategy here is to use data available to us from the UNPACK dispatch
 * -- namely the unpack type and mask flag -- in combination mode and usn values -- to
 * generate ~600 special versions of this function.  But since it's an interpreter, who gives
 * a crap?  Really? :p
 */

/* size - size of the packet fragment incoming from DMAC. */
template <int idx, bool doMode, bool isFill>
__ri void _nVifUnpackLoop(const u8* data)
{
	vifStruct& vif          = (idx ? ((THREAD_VU1) ? vu1Thread.vif     : vif1)     : (vif0));
	VIFregisters& vifRegs   = (idx ? ((THREAD_VU1) ? vu1Thread.vifRegs : vif1Regs) : (vif0Regs));

	/* skipSize used for skipping writes only */
	const int skipSize      = (vifRegs.cycle.cl - vifRegs.cycle.wl) * 16;

	if (!doMode && (vif.cmd & 0x10))
		setMasks(vif, vifRegs);

	const int usn           = !!vif.usn;
	const int upkNum        = vif.cmd & 0x1f;
	const u8& vSize         = nVifT[upkNum & 0x0f];

	const nVifCall* fnbase  = &nVifUpk[((usn * 2 * 16) + upkNum) * (4 * 1)];
	const UNPACKFUNCTYPE ft = VIFfuncTable[idx][doMode ? vifRegs.mode : 0][((usn * 2 * 16) + upkNum)];

	do
	{
		u8* dest = (u8*)(vuRegs[idx].Mem + (vif.tag.addr & (idx ? 0x3ff0 : 0xff0)));

		if (doMode)
			ft(dest, data);
		else
		{
			uint cl3 = std::min(vif.cl, 3);
			fnbase[cl3](dest, data);
		}

		vif.tag.addr += 16;
		--vifRegs.num;
		++vif.cl;

		if (isFill)
		{
			if (vif.cl <= vifRegs.cycle.cl)
				data += vSize;
			else if (vif.cl == vifRegs.cycle.wl)
				vif.cl = 0;
		}
		else
		{
			data += vSize;

			if (vif.cl >= vifRegs.cycle.wl)
			{
				vif.tag.addr += skipSize;
				vif.cl = 0;
			}
		}
	} while (vifRegs.num);
}

__fi void _nVifUnpack(int idx, const u8* data, uint mode, bool isFill)
{
	// Unpacks Until 'Num' is 0
	alignas(16) static const Fnptr_VifUnpackLoop UnpackLoopTable[2][2][2] = {
		{
			{_nVifUnpackLoop<0, false, false>, _nVifUnpackLoop<0, false, true>},
			{_nVifUnpackLoop<0, true,  false>, _nVifUnpackLoop<0, true,  true>},
		},
		{
			{_nVifUnpackLoop<1, false, false>, _nVifUnpackLoop<1, false, true>},
			{_nVifUnpackLoop<1, true,  false>, _nVifUnpackLoop<1, true,  true>},
		},
	};
	UnpackLoopTable[idx][!!mode][isFill](data);
}
