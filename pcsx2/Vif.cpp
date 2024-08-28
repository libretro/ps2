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

#include <cstring> /* memset */

#include "Common.h"
#include "GS.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "VUmicro.h"
#include "x86/newVif.h"

/* Generic constants */
#define VIF0INTC 4
#define VIF1INTC 5

#define VifStallEnable(vif) (vif.chcr.STR)
#define vif1InternalIrq() ((dmacRegs.ctrl.MFD == MFD_VIF1) ? DMAC_MFIFO_VIF : DMAC_VIF1)
#define QWCTAG(mask) (dmacRegs.rbor.ADDR + ((mask) & dmacRegs.rbsr.RMSK))

enum VifModes
{
	VIF_NORMAL_TO_MEM_MODE   = 0,
	VIF_NORMAL_FROM_MEM_MODE = 1,
	VIF_CHAIN_MODE 		 = 2
};

enum UnpackOffset {
	OFFSET_X = 0,
	OFFSET_Y = 1,
	OFFSET_Z = 2,
	OFFSET_W = 3
};

typedef int FnType_VifCmdHandler(int pass, const u32 *data);
typedef FnType_VifCmdHandler* Fnptr_VifCmdHandler;

alignas(16) vifStruct vif0, vif1;

static u32 g_vif0Cycles = 0;
static u32 g_vif1Cycles = 0;

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

static __ri void vifExecQueue(int idx)
{
	vifStruct& vifX = (idx ? (vif1) : (vif0));
	if (!vifX.queued_program || (vuRegs[0].VI[REG_VPU_STAT].UL & 1 << (idx * 8)))
		return;

	if (vifX.queued_gif_wait)
	{
		if (gifUnit.checkPaths(1, 1, 0))
			return;
	}

	vifX.queued_program = false;

	if (!idx)
		vu0ExecMicro(vif0.queued_pc);
	else
		vu1ExecMicro(vif1.queued_pc);
}

//------------------------------------------------------------------
// Vif0/Vif1 Misc Functions
//------------------------------------------------------------------

static __fi void vifFlush(int idx)
{
	vifExecQueue(idx);

	if (idx)
	{
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x500) /* T bit stop or Busy */
		{
			vif1.waitforvu          = true;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
			vif1Regs.stat.VEW       = true;
		}
	}
	else
	{
		/* Run VU0 until finish, don't add cycles to EE
		 * because its vif stalling not the EE core... */
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x5) /* T bit stop or Busy */
		{
			vif0.waitforvu          = true;
			vif0.vifstalled.enabled = VifStallEnable(vif0ch);
			vif0.vifstalled.value   = VIF_TIMING_BREAK;
			vif0Regs.stat.VEW       = true;
		}
	}

	vifExecQueue(idx);
}

template <int idx> static __fi void vuExecMicro(u32 addr, bool requires_wait)
{
	vifStruct& vifX       = (idx ? (vif1) : (vif0));
	VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));

	vifFlush(idx);
	if (vifX.waitforvu)
	{
		CPU_SET_DMASTALL(idx ? vif1InternalIrq() : DMAC_VIF0, true);
		return;
	}

	if (vifRegs.itops > (idx ? 0x3ffu : 0xffu))
		vifRegs.itops &= (idx ? 0x3ffu : 0xffu);

	vifRegs.itop = vifRegs.itops;

	if (idx)
	{
		// in case we're handling a VIF1 execMicro, set the top with the tops value
		vifRegs.top = vifRegs.tops & 0x3ff;

		// is DBF flag set in VIF_STAT?
		if (vifRegs.stat.DBF)
		{
			// it is, so set tops with base, and clear the stat DBF flag
			vifRegs.tops = vifRegs.base;
			vifRegs.stat.DBF = false;
		}
		else
		{
			// it is not, so set tops with base + offset, and set stat DBF flag
			vifRegs.tops = vifRegs.base + vifRegs.ofst;
			vifRegs.stat.DBF = true;
		}
	}

	vifX.queued_program = true;
	if ((s32)addr == -1)
		vifX.queued_pc = addr;
	else
		vifX.queued_pc = addr & (idx ? 0x7ffu : 0x1ffu);
	vifX.unpackcalls = 0;

	vifX.queued_gif_wait = requires_wait;

	if (!idx || (!THREAD_VU1 && !INSTANT_VU1))
		vifExecQueue(idx);
}

void vif0Reset(void)
{
	/* Reset the whole VIF, meaning the internal pcsx2 vars and all the registers */
	memset(&vif0, 0, sizeof(vif0));
	memset(&vif0Regs, 0, sizeof(vif0Regs));

	resetNewVif(0);
}

void vif1Reset(void)
{
	/* Reset the whole VIF, meaning the internal pcsx2 vars, and all the registers */
	memset(&vif1, 0, sizeof(vif1));
	memset(&vif1Regs, 0, sizeof(vif1Regs));

	resetNewVif(1);
}

bool SaveStateBase::vif0Freeze()
{
	if (!(FreezeTag("VIF0dma")))
		return false;

	Freeze(g_vif0Cycles);

	Freeze(vif0);

	Freeze(nVif[0].bSize);
	FreezeMem(nVif[0].buffer, nVif[0].bSize);

	return IsOkay();
}

bool SaveStateBase::vif1Freeze()
{
	if (!(FreezeTag("VIF1dma")))
		return false;

	Freeze(g_vif1Cycles);

	Freeze(vif1);

	Freeze(nVif[1].bSize);
	FreezeMem(nVif[1].buffer, nVif[1].bSize);

	return IsOkay();
}


//------------------------------------------------------------------
// Vif0/Vif1 Code Implementations
//------------------------------------------------------------------

/* TODO/FIXME: Review Flags */
template<int idx> static int vifCode_Null(int pass, const u32* data)
{
	vifStruct& vifX = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		/* if ME1, then force the vif to interrupt */
		if (!(vifRegs.err.ME1)) /* Ignore vifcode and tag mismatch error */
		{
			vifRegs.stat.ER1        = true;
			vifX.vifstalled.enabled = VifStallEnable((idx ? (vif1ch)   : (vif0ch)));
			vifX.vifstalled.value   = VIF_IRQ_STALL;
		}
		vifX.cmd = 0;
		vifX.pass = 0;

		//If the top bit was set to interrupt, 
		//we don't want it to take commands from a bad code
		if (vifRegs.code & 0x80000000)
			vifX.irq = 0;
	}
	return 1;
}

static int vifCode_Base(int pass, const u32* data)
{
	if (pass == 0)
	{
		vif1Regs.base = vif1Regs.code & 0x3ff;
		vif1.cmd      = 0;
		vif1.pass     = 0;
	}
	return 1;
}

static int vifCode_Direct_VU1(int pass, const u32* _data)
{
	const u8 *data = (u8*)_data;
	if (pass == 0)
	{
		int vifImm    = (u16)vif1Regs.code;
		vif1.tag.size = vifImm ? (vifImm * 4) : (65536 * 4);
		vif1.pass     = 1;
		return 1;
	}
	else if (pass == 1)
	{
		uint size = std::min(vif1.vifpacketsize, vif1.tag.size) * 4; // Get size in bytes
		uint ret  = gifUnit.TransferGSPacketData(GIF_TRANS_DIRECT, (u8*)data, size);

		vif1.tag.size -= ret / 4; // Convert to u32's
		vif1Regs.stat.VGW = false;

		if (size != ret) // Stall if GIF didn't process all the data (path2 queued)
		{
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
			vif1Regs.stat.VGW       = true;
			return 0;
		}
		if (vif1.tag.size == 0)
		{
			vif1.cmd                = 0;
			vif1.pass               = 0;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
		}
		return ret / 4;
	}
	return 0;
}

static int vifCode_DirectHL_VU1(int pass, const u32* data)
{
	if (pass == 0)
	{
		int vifImm = (u16)vif1Regs.code;
		vif1.tag.size = vifImm ? (vifImm * 4) : (65536 * 4);
		vif1.pass = 1;
		return 1;
	}
	else if (pass == 1)
	{
		uint size = std::min(vif1.vifpacketsize, vif1.tag.size) * 4; // Get size in bytes
		uint ret = gifUnit.TransferGSPacketData(GIF_TRANS_DIRECTHL, (u8*)data, size);

		vif1.tag.size -= ret / 4; // Convert to u32's
		vif1Regs.stat.VGW = false;

		if (size != ret) // Stall if GIF didn't process all the data (path2 queued)
		{
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
			vif1Regs.stat.VGW       = true;
			return 0;
		}
		if (vif1.tag.size == 0)
		{
			vif1.cmd                = 0;
			vif1.pass               = 0;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
		}
		return ret / 4;
	}
	return 0;
}

static int vifCode_Flush_VU1(int pass, const u32* data)
{
	if (pass == 0 || pass == 1)
	{
		bool p1or2        = (gifRegs.stat.APATH != 0 && gifRegs.stat.APATH != 3);
		vif1Regs.stat.VGW = false;

		vifExecQueue(1);
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x500) /* T bit stop or Busy */
		{
			vif1.waitforvu          = true;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
			vif1Regs.stat.VEW       = true;
		}
		vifExecQueue(1);

		if (gifUnit.checkPaths(1, 1, 0) || p1or2)
		{
			vif1Regs.stat.VGW       = true;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
		}

		if (vif1.waitforvu || vif1Regs.stat.VGW)
		{
			CPU_SET_DMASTALL(vif1InternalIrq(), true);
			return 0;
		}

		vif1.cmd  = 0;
		vif1.pass = 0;
	}
	return 1;
}

static int vifCode_FlushA_VU1(int pass, const u32* data)
{
	if (pass == 0 || pass == 1)
	{
		u32 gifBusy = gifUnit.checkPaths(1, 1, 1) || (gifRegs.stat.APATH != 0);
		vif1Regs.stat.VGW = false;

		vifExecQueue(1);
		if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x500) /* T bit stop or Busy */
		{
			vif1.waitforvu          = true;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
			vif1Regs.stat.VEW       = true;
		}
		vifExecQueue(1);

		if (gifBusy)
		{
			vif1Regs.stat.VGW       = true;
			vif1.vifstalled.enabled = VifStallEnable(vif1ch);
			vif1.vifstalled.value   = VIF_TIMING_BREAK;
		}

		if (vif1.waitforvu || vif1Regs.stat.VGW)
		{
			CPU_SET_DMASTALL(vif1InternalIrq(), true);
			return 0;
		}

		vif1.cmd = 0;
		vif1.pass = 0;
	}
	return 1;
}

// ToDo: FixMe
template<int idx> static int vifCode_FlushE(int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct& vifX       = (idx ? (vif1) : (vif0));
		vifFlush(idx);

		if (vifX.waitforvu)
		{
			CPU_SET_DMASTALL(idx ? vif1InternalIrq() : DMAC_VIF0, true);
			return 0;
		}

		vifX.cmd = 0;
		vifX.pass = 0;
	}
	return 1;
}

template<int idx> static int vifCode_ITop(int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct& vifX       = (idx ? (vif1) : (vif0));
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifRegs.itops         = vifRegs.code & 0x3ff;
		vifX.cmd              = 0;
		vifX.pass             = 0;
	}
	return 1;
}

template<int idx> static int vifCode_Mark(int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct& vifX       = (idx ? (vif1) : (vif0));
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifRegs.mark          = (u16)vifRegs.code;
		vifRegs.stat.MRK      = true;
		vifX.cmd              = 0;
		vifX.pass             = 0;
	}
	return 1;
}

template<int idx> static __fi void _vifCode_MPG(u32 addr, const u32* data, int size)
{
	VURegs& VUx     = idx ? vuRegs[1] : vuRegs[0];
	vifStruct& vifX = (idx ? (vif1) : (vif0));
	u16 vuMemSize   = idx ? 0x4000 : 0x1000;

	vifExecQueue(idx);

	if (idx && THREAD_VU1)
	{
		if ((addr + size * 4) > vuMemSize)
		{
			vu1Thread.WriteMicroMem(addr, (u8*)data, vuMemSize - addr);
			size -= (vuMemSize - addr) / 4;
			data += (vuMemSize - addr) / 4;
			vu1Thread.WriteMicroMem(0, (u8*)data, size * 4);
			vifX.tag.addr = size * 4;
		}
		else
		{
			vu1Thread.WriteMicroMem(addr, (u8*)data, size * 4);
			vifX.tag.addr += size * 4;
		}
		return;
	}

	// Don't forget the Unsigned designator for these checks
	if ((addr + size * 4) > vuMemSize)
	{
		if (!idx)
			CpuVU0->Clear(addr, vuMemSize - addr);
		else
			CpuVU1->Clear(addr, vuMemSize - addr);

		memcpy(VUx.Micro + addr, data, vuMemSize - addr);
		size -= (vuMemSize - addr) / 4;
		data += (vuMemSize - addr) / 4;
		memcpy(VUx.Micro, data, size * 4);

		vifX.tag.addr = size * 4;
	}
	else
	{
		//The compare is pretty much a waste of time, likelyhood is that the program isnt there, thats why its copying it.
		//Faster without.
		//if (memcmp(VUx.Micro + addr, data, size*4)) {
		// Clear VU memory before writing!
		if (!idx)
			CpuVU0->Clear(addr, size * 4);
		else
			CpuVU1->Clear(addr, size * 4);
		memcpy(VUx.Micro + addr, data, size * 4); //from tests, memcpy is 1fps faster on Grandia 3 than memcpy

		vifX.tag.addr += size * 4;
	}
}

template<int idx> static int vifCode_MPG(int pass, const u32* data)
{
	vifStruct& vifX = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		int vifNum            = (u8)(vifRegs.code >> 16);
		vifX.tag.addr         = (u16)(vifRegs.code << 3) & (idx ? 0x3fff : 0xfff);
		vifX.tag.size         = vifNum ? (vifNum * 2) : 512;
		vifFlush(idx);

		if (vifX.waitforvu)
		{
			CPU_SET_DMASTALL(idx ? vif1InternalIrq() : DMAC_VIF0, true);
			return 0;
		}
		vifX.pass = 1;
		return 1;
	}
	else if (pass == 1)
	{
		if (vifX.vifpacketsize < vifX.tag.size) // Partial Transfer
		{
			_vifCode_MPG<idx>(vifX.tag.addr, data, vifX.vifpacketsize);
			vifX.tag.size -= vifX.vifpacketsize; //We can do this first as its passed as a pointer
			return vifX.vifpacketsize;
		}
		// Full Transfer
		_vifCode_MPG<idx>(vifX.tag.addr, data, vifX.tag.size);
		int ret = vifX.tag.size;
		vifX.tag.size = 0;
		vifX.cmd = 0;
		vifX.pass = 0;
		return ret;
	}
	return 0;
}

template<int idx> static int vifCode_MSCAL(int pass, const u32* data)
{
	vifStruct& vifX = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifFlush(idx);

		if (vifX.waitforvu)
		{
			CPU_SET_DMASTALL(idx ? vif1InternalIrq() : DMAC_VIF0, true);
			return 0;
		}

		vuExecMicro<idx>((u16)(vifRegs.code), false);
		vifX.cmd = 0;
		vifX.pass = 0;

		if (vifX.vifpacketsize > 1)
		{
			//Warship Gunner 2 has a rather big dislike for the delays
			if (((data[1] >> 24) & 0x60) == 0x60) // Immediate following Unpack
			{
				//Snowblind games only use MSCAL, so other MS kicks force the program directly.
				vifExecQueue(idx);
			}
		}
	}
	return 1;
}

template<int idx> static int vifCode_MSCALF(int pass, const u32* data)
{
	vifStruct& vifX = (idx ? (vif1) : (vif0));
	if (pass == 0 || pass == 1)
	{
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifRegs.stat.VGW = false;
		vifFlush(idx);
		if (u32 a = gifUnit.checkPaths(1, 1, 0))
		{
			vif1Regs.stat.VGW       = true;
			vifX.vifstalled.enabled = VifStallEnable((idx ? (vif1ch)   : (vif0ch)));
			vifX.vifstalled.value   = VIF_TIMING_BREAK;
		}

		if (vifX.waitforvu || vif1Regs.stat.VGW)
		{
			CPU_SET_DMASTALL(idx ? vif1InternalIrq() : DMAC_VIF0, true);
			return 0;
		}

		vuExecMicro<idx>((u16)(vifRegs.code), true);
		vifX.cmd = 0;
		vifX.pass = 0;
		vifExecQueue(idx);
	}
	return 1;
}

template<int idx> static int vifCode_MSCNT(int pass, const u32* data)
{
	vifStruct& vifX   = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		vifFlush(idx);

		if (vifX.waitforvu)
		{
			CPU_SET_DMASTALL(idx ? vif1InternalIrq() : DMAC_VIF0, true);
			return 0;
		}

		vuExecMicro<idx>(-1, false);
		vifX.cmd  = 0;
		vifX.pass = 0;
		if (vifX.vifpacketsize > 1)
		{
			if (((data[1] >> 24) & 0x60) == 0x60) // Immediate following Unpack
			{
				vifExecQueue(idx);
			}
		}
	}
	return 1;
}

// ToDo: FixMe
static int vifCode_MskPath3_VU1(int pass, const u32* data)
{
	if (pass == 0)
	{
		vif1Regs.mskpath3 = (vif1Regs.code >> 15) & 0x1;
		gifRegs.stat.M3P  = (vif1Regs.code >> 15) & 0x1;
		if (!vif1Regs.mskpath3)
			gifInterrupt();
		vif1.cmd          = 0;
		vif1.pass         = 0;
	}
	return 1;
}

template<int idx> static int vifCode_Nop(int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct& vifX  = (idx ? (vif1) : (vif0));
		vifX.cmd         = 0;
		vifX.pass        = 0;
		vifExecQueue(idx);

		if (vifX.vifpacketsize > 1)
		{
			if (((data[1] >> 24) & 0x7f) == 0x6 && (data[1] & 0x1)) //is mskpath3 next
			{
				vifX.vifstalled.enabled = VifStallEnable((idx ? (vif1ch)   : (vif0ch)));
				vifX.vifstalled.value   = VIF_TIMING_BREAK;
			}
		}
	}
	return 1;
}

static int vifCode_Offset_VU1(int pass, const u32* data)
{
	if (pass == 0)
	{
		vif1Regs.stat.DBF = false;
		vif1Regs.ofst     = vif1Regs.code & 0x3ff;
		vif1Regs.tops     = vif1Regs.base;
		vif1.cmd          = 0;
		vif1.pass         = 0;
	}
	return 1;
}

template <int idx>
static __fi int _vifCode_STColRow(const u32* data, u32* pmem2)
{
	vifStruct& vifX  = (idx ? (vif1) : (vif0));
	int ret          = std::min(4 - vifX.tag.addr, vifX.vifpacketsize);

	switch (ret)
	{
		case 4:
			pmem2[3] = data[3];
			/* fallthrough */
		case 3:
			pmem2[2] = data[2];
			/* fallthrough */
		case 2:
			pmem2[1] = data[1];
			/* fallthrough */
		case 1:
			pmem2[0] = data[0];
			break;
		default:
			break;
	}

	vifX.tag.addr += ret;
	vifX.tag.size -= ret;
	if (!vifX.tag.size)
	{
		vifX.pass = 0;
		vifX.cmd = 0;
	}



	return ret;
}

template<int idx> static int vifCode_STCol(int pass, const u32* data)
{
	vifStruct& vifX  = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		vifX.tag.addr = 0;
		vifX.tag.size = 4;
		vifX.pass     = 1;
		return 1;
	}
	else if (pass == 1)
	{
		u32 ret = _vifCode_STColRow<idx>(data, &vifX.MaskCol._u32[vifX.tag.addr]);
		if (idx && vifX.tag.size == 0)
			vu1Thread.WriteCol(vifX);
		return ret;
	}
	return 0;
}

template<int idx> static int vifCode_STRow(int pass, const u32* data)
{
	vifStruct& vifX  = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		vifX.tag.addr = 0;
		vifX.tag.size = 4;
		vifX.pass     = 1;
	}
	else if (pass == 1)
	{
		u32 ret = _vifCode_STColRow<idx>(data, &vifX.MaskRow._u32[vifX.tag.addr]);
		if (idx && vifX.tag.size == 0)
			vu1Thread.WriteRow(vifX);
		return ret;
	}
	return 1;
}

template<int idx> static int vifCode_STCycl(int pass, const u32* data)
{
	vifStruct& vifX  = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifRegs.cycle.cl = (u8)(vifRegs.code);
		vifRegs.cycle.wl = (u8)(vifRegs.code >> 8);
		vifX.cmd          = 0;
		vifX.pass         = 0;
	}
	return 1;
}

template<int idx> static int vifCode_STMask(int pass, const u32* data)
{
	vifStruct& vifX  = (idx ? (vif1) : (vif0));
	if (pass == 0)
	{
		vifX.tag.size = 1;
		vifX.pass     = 1;
	}
	else if (pass == 1)
	{
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifRegs.mask  = data[0];
		vifX.tag.size = 0;
		vifX.cmd      = 0;
		vifX.pass     = 0;
	}
	return 1;
}

template<int idx> static int vifCode_STMod(int pass, const u32* data)
{
	if (pass == 0)
	{
		vifStruct& vifX       = (idx ? (vif1) : (vif0));
		VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
		vifRegs.mode          = vifRegs.code & 0x3;
		vifX.cmd              = 0;
		vifX.pass             = 0;
	}
	return 1;
}

template<int idx> static int vifCode_Unpack(int pass, const u32* data)
{
	if (pass == 0)
	{
		vifUnpackSetup<idx>(data);
		return 1;
	}
	else if (pass == 1)
		return nVifUnpack<idx>((u8*)data);
	return 0;
}

//------------------------------------------------------------------
// Vif0/Vif1 Code Tables
//------------------------------------------------------------------

alignas(16) FnType_VifCmdHandler* const vifCmdHandler[2][128] =
{
	{
		vifCode_Nop<0>     , vifCode_STCycl<0>  , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_ITop<0>   , vifCode_STMod<0>  , vifCode_Null<0>    , vifCode_Mark<0>,   /*0x00*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x08*/
		vifCode_FlushE<0>  , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_MSCAL<0>  , vifCode_MSCALF<0> , vifCode_Null<0>	 , vifCode_MSCNT<0>,  /*0x10*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x18*/
		vifCode_STMask<0>  , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>	 , vifCode_Null<0>,   /*0x20*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>	 , vifCode_Null<0>,   /*0x28*/
		vifCode_STRow<0>   , vifCode_STCol<0>	, vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>	 , vifCode_Null<0>,   /*0x30*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x38*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x40*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_MPG<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x48*/
		vifCode_Null<0>    , vifCode_Null<0>    , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x50*/
		vifCode_Null<0>	   , vifCode_Null<0>	, vifCode_Null<0>	, vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>   , vifCode_Null<0>    , vifCode_Null<0>,   /*0x58*/
		vifCode_Unpack<0>  , vifCode_Unpack<0>  , vifCode_Unpack<0>	, vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0>  , vifCode_Null<0>,   /*0x60*/
		vifCode_Unpack<0>  , vifCode_Unpack<0>  , vifCode_Unpack<0>	, vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0>  , vifCode_Unpack<0>, /*0x68*/
		vifCode_Unpack<0>  , vifCode_Unpack<0>  , vifCode_Unpack<0>	, vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0>  , vifCode_Null<0>,   /*0x70*/
		vifCode_Unpack<0>  , vifCode_Unpack<0>  , vifCode_Unpack<0>	, vifCode_Null<0>   , vifCode_Unpack<0> , vifCode_Unpack<0> , vifCode_Unpack<0>  , vifCode_Unpack<0>  /*0x78*/
	},
	{
		vifCode_Nop<1>     , vifCode_STCycl<1>  , vifCode_Offset_VU1	, vifCode_Base      , vifCode_ITop<1>   , vifCode_STMod<1>  , vifCode_MskPath3_VU1, vifCode_Mark<1>,   /*0x00*/
		vifCode_Null<1>    , vifCode_Null<1>    , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x08*/
		vifCode_FlushE<1>  , vifCode_Flush_VU1  , vifCode_Null<1>	, vifCode_FlushA_VU1, vifCode_MSCAL<1>  , vifCode_MSCALF<1> , vifCode_Null<1>	 , vifCode_MSCNT<1>,  /*0x10*/
		vifCode_Null<1>    , vifCode_Null<1>    , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x18*/
		vifCode_STMask<1>  , vifCode_Null<1>    , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>	 , vifCode_Null<1>,   /*0x20*/
		vifCode_Null<1>    , vifCode_Null<1>    , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>	 , vifCode_Null<1>,   /*0x28*/
		vifCode_STRow<1>   , vifCode_STCol<1>	, vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>	 , vifCode_Null<1>,   /*0x30*/
		vifCode_Null<1>    , vifCode_Null<1>    , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x38*/
		vifCode_Null<1>    , vifCode_Null<1>    , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x40*/
		vifCode_Null<1>    , vifCode_Null<1>    , vifCode_MPG<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x48*/
		vifCode_Direct_VU1 , vifCode_DirectHL_VU1, vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x50*/
		vifCode_Null<1>	   , vifCode_Null<1>	, vifCode_Null<1>	, vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>   , vifCode_Null<1>    , vifCode_Null<1>,   /*0x58*/
		vifCode_Unpack<1>  , vifCode_Unpack<1>  , vifCode_Unpack<1>	, vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1>  , vifCode_Null<1>,   /*0x60*/
		vifCode_Unpack<1>  , vifCode_Unpack<1>  , vifCode_Unpack<1>	, vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1>  , vifCode_Unpack<1>, /*0x68*/
		vifCode_Unpack<1>  , vifCode_Unpack<1>  , vifCode_Unpack<1>	, vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1>  , vifCode_Null<1>,   /*0x70*/
		vifCode_Unpack<1>  , vifCode_Unpack<1>  , vifCode_Unpack<1>	, vifCode_Null<1>   , vifCode_Unpack<1> , vifCode_Unpack<1> , vifCode_Unpack<1>  , vifCode_Unpack<1>  /*0x78*/
	}
};

static u32 QWCinVIFMFIFO(u32 DrainADDR, u32 qwc)
{
	u32 limit;
	/*Calculate what we have in the fifo. */
	if (DrainADDR <= spr0ch.madr) /* Drain is below the TADR, calculate the difference between them */
		return (spr0ch.madr - DrainADDR) >> 4;
	limit = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
	/* Drain is higher than SPR so it has looped round,
	 * calculate from base to the SPR tag addr and what is left in the top of the ring */
	return ((spr0ch.madr - dmacRegs.rbor.ADDR) + (limit - DrainADDR)) >> 4;
}

static __fi bool mfifoVIF1rbTransfer(void)
{
	bool ret;
	u32* src;
	u32 msize    = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
	u32 mfifoqwc = std::min(QWCinVIFMFIFO(vif1ch.madr, vif1ch.qwc), vif1ch.qwc);

	if (mfifoqwc == 0)
		return true; /* Cant do anything, lets forget it */

	/* Check if the transfer should wrap around the ring buffer */
	if ((vif1ch.madr + (mfifoqwc << 4)) > (msize))
	{
		int s1 = ((msize)-vif1ch.madr) >> 2;

		/* it does, so first copy 's1' bytes from 'addr' to 'data' */
		vif1ch.madr = QWCTAG(vif1ch.madr);

		if (!(src = (u32*)PSM(vif1ch.madr)))
			return false;

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer(src + vif1.irqoffset.value, s1 - vif1.irqoffset.value, false);
		else
			ret = VIF1transfer(src, s1, false);

		if (ret)
		{
			/* and second copy 's2' bytes from 'maddr' to '&data[s1]' */
			vif1ch.tadr = QWCTAG(vif1ch.tadr);
			vif1ch.madr = QWCTAG(vif1ch.madr);

			if (!(src = (u32*)PSM(vif1ch.madr)))
				return false;
			VIF1transfer(src, ((mfifoqwc << 2) - s1), false);
		}
	}
	else
	{
		/* it doesn't, so just transfer 'qwc*4' words */
		if (!(src = (u32*)PSM(vif1ch.madr)))
			return false;

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer(src + vif1.irqoffset.value, mfifoqwc * 4 - vif1.irqoffset.value, false);
		else
			ret = VIF1transfer(src, mfifoqwc << 2, false);
	}
	return ret;
}

static __fi void mfifo_VIF1chain(void)
{
	/* Is QWC = 0? if so there is nothing to transfer */
	if (vif1ch.qwc == 0)
	{
		vif1.inprogress &= ~1;
		return;
	}

	if (vif1ch.madr >= dmacRegs.rbor.ADDR &&
		vif1ch.madr < (dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16u))
	{
		if (QWCinVIFMFIFO(vif1ch.madr, vif1ch.qwc) == 0)
		{
			vif1.inprogress |= 0x10;
			g_vif1Cycles += 4;
			return;
		}

		mfifoVIF1rbTransfer();
		vif1ch.madr = QWCTAG(vif1ch.madr);

		/* When transferring direct from the MFIFO, the TADR needs to be after the data last read
		 * FF7 DoC Expects the transfer to end with an Empty interrupt, so the TADR has to match SPR0_MADR
		 * It does an END tag (which normally doesn't increment TADR because it breaks Soul Calibur 2)
		 * with a QWC of 1 (rare) so we need to increment the TADR in the case of MFIFO. */
		vif1ch.tadr = vif1ch.madr;
	}
	else
	{
		tDMA_TAG* pMem = dmaGetAddr(vif1ch.madr, !vif1ch.chcr.DIR);

		/* No need to exit on non-mfifo as it is indirect anyway, so it can be transferring this while spr refills the mfifo */

		if (pMem == NULL)
			return;

		if (vif1.irqoffset.enabled)
			VIF1transfer((u32*)pMem + vif1.irqoffset.value, vif1ch.qwc * 4 - vif1.irqoffset.value, false);
		else
			VIF1transfer((u32*)pMem, vif1ch.qwc << 2, false);
	}
}

static void mfifoVIF1transfer(void)
{
	tDMA_TAG* ptag;

	g_vif1Cycles = 0;

	if (vif1ch.qwc == 0)
	{
		if (QWCinVIFMFIFO(vif1ch.tadr, 1) == 0)
		{
			vif1.inprogress |= 0x10;
			g_vif1Cycles += 4;
			return;
		}

		vif1ch.tadr = QWCTAG(vif1ch.tadr);
		ptag = dmaGetAddr(vif1ch.tadr, false);

		if (vif1ch.chcr.TTE)
		{
			bool ret;

			alignas(16) static u128 masked_tag;

			masked_tag._u64[0] = 0;
			masked_tag._u64[1] = *((u64*)ptag + 1);

			if (vif1.irqoffset.enabled)
				ret = VIF1transfer((u32*)&masked_tag + vif1.irqoffset.value, 4 - vif1.irqoffset.value, true); /* Transfer Tag on stall */
			else
			{
				vif1.irqoffset.value = 2;
				vif1.irqoffset.enabled = true;
				ret = VIF1transfer((u32*)&masked_tag + 2, 2, true); /* Transfer Tag */
			}

			if (!ret && vif1.irqoffset.enabled)
			{
				vif1.inprogress &= ~1;
				return; /* IRQ set by VIFTransfer */
			}
			g_vif1Cycles += 2;
		}

		vif1.irqoffset.value = 0;
		vif1.irqoffset.enabled = false;

		vif1ch.unsafeTransfer(ptag);

		vif1ch.madr = ptag[1]._u32;

		vif1.done |= hwDmacSrcChainWithStack(vif1ch, ptag->ID);

		switch (ptag->ID)
		{
			/* These five transfer data following the tag, need to check its within the buffer (Front Mission 4) */
			case TAG_CNT:
			case TAG_NEXT:
			case TAG_CALL:
			case TAG_RET:
			case TAG_END:
				if (vif1ch.madr < dmacRegs.rbor.ADDR) /* probably not needed but we will check anyway. */
					vif1ch.madr = QWCTAG(vif1ch.madr);
				if (vif1ch.madr > (dmacRegs.rbor.ADDR + (u32)dmacRegs.rbsr.RMSK)) /* Usual scenario is the tag is near the end (Front Mission 4) */
					vif1ch.madr = QWCTAG(vif1ch.madr);
				break;
			default:
				/* Do nothing as the MADR could be outside */
				break;
		}

		if (vif1ch.chcr.TIE && ptag->IRQ)
			vif1.done = true;

		vif1ch.tadr = QWCTAG(vif1ch.tadr);

		if (vif1ch.qwc > 0)
			vif1.inprogress |= 1;
	}
}

void vifMFIFOInterrupt(void)
{
	g_vif1Cycles = 0;

	if (dmacRegs.ctrl.MFD != MFD_VIF1)
	{
		vif1Interrupt();
		return;
	}

	if (gifRegs.stat.APATH == 2 && gifUnit.gifPath[1].isDone())
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;

		if (gifUnit.checkPaths(1, 0, 1))
			gifUnit.Execute(false, true);
	}

	if (vif1ch.chcr.DIR)
	{
		bool isDirect   = (vif1.cmd & 0x7f) == 0x50;
		bool isDirectHL = (vif1.cmd & 0x7f) == 0x51;
		if ((isDirect && !gifUnit.CanDoPath2()) || (isDirectHL && !gifUnit.CanDoPath2HL()))
		{
			CPU_INT(DMAC_MFIFO_VIF, 128);
			CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
			return;
		}
	}
	if (vif1.waitforvu)
	{
		CPU_INT(VIF_VU1_FINISH, std::max(16, cpuGetCycles(VU_MTVU_BUSY)));
		CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
		return;
	}

	/* We need to check the direction, if it is downloading from the GS,
	 * we handle that separately (KH2 for testing)

	 * Simulated GS transfer time done, clear the flags */

	if (vif1.irq && vif1.vifstalled.enabled && vif1.vifstalled.value == VIF_IRQ_STALL)
	{
		vif1Regs.stat.INT = true;

		if (((vif1Regs.code >> 24) & 0x7f) != 0x7)
			vif1Regs.stat.VIS = true;

		hwIntcIrq(INTC_VIF1);
		--vif1.irq;

		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
			/* Used to check if the MFIFO was empty, there's really no need if it's finished what it needed. */
			if ((vif1ch.qwc > 0 || !vif1.done))
			{
				vif1Regs.stat.VPS = VPS_DECODING; /* If there's more data you need to say it's decoding the next VIF CMD (Onimusha - Blade Warriors) */
				CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
				return;
			}
		}
	}

	/* Mirroring change to VIF0 */
	if (vif1.cmd)
	{
		if (vif1.done && vif1ch.qwc == 0)
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
		vif1Regs.stat.VPS = VPS_IDLE;

	if (vif1.inprogress & 0x10)
	{
		FireMFIFOEmpty();
		CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
		return;
	}

	vif1.vifstalled.enabled = false;

	if (!vif1.done || vif1ch.qwc)
	{
		switch (vif1.inprogress & 1)
		{
			case 0: /* Set up transfer */
				mfifoVIF1transfer();
				vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
				/* fallthrough  */

			case 1: /* Transfer data */
				if (vif1.inprogress & 0x1) /* Just in case the tag breaks early (or something wierd happens)! */
					mfifo_VIF1chain();
				/* Sanity check! making sure we always have non-zero values */
				if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE)) /* If we're waiting on GIF, stop looping, (can be over 1000 loops!) */
				{
					if (vif1.waitforvu)
					{
						CPU_INT(DMAC_MFIFO_VIF, std::max(static_cast<int>((g_vif1Cycles == 0 ? 4 : g_vif1Cycles)), cpuGetCycles(VU_MTVU_BUSY)));
					}
					else
						CPU_INT(DMAC_MFIFO_VIF, (g_vif1Cycles == 0 ? 4 : g_vif1Cycles));
				}

				vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
				return;
		}
		return;
	}

	vif1.vifstalled.enabled = false;
	vif1.irqoffset.enabled = false;
	vif1.done = 1;

	if (spr0ch.madr == vif1ch.tadr)
	{
		FireMFIFOEmpty();
	}

	g_vif1Cycles = 0;
	vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
	vif1ch.chcr.STR = false;
	hwDmacIrq(DMAC_VIF1);
	CPU_SET_DMASTALL(DMAC_VIF1, false);
	vif1Regs.stat.FQC = 0;
}

static void vif1TransferToMemory(void)
{
	u128* pMem = (u128*)dmaGetAddr(vif1ch.madr, false);

	/* VIF from gsMemory */
	if (!pMem) /* Is vif0ptag empty? */
	{ 
		dmacRegs.stat.BEIS = true; /* Bus Error */
		vif1Regs.stat.FQC  = 0;

		vif1ch.qwc         = 0;
		vif1.done          = true;
		CPU_INT(DMAC_VIF1, 0);
		return; /* An error has occurred. */
	}

	/* MTGS concerns:  The MTGS is inherently disagreeable with the idea of downloading
	 * stuff from the GS.  The *only* way to handle this case safely is to flush the GS
	 * completely and execute the transfer there-after. */
	const u32 size = std::min(vif1.GSLastDownloadSize, (u32)vif1ch.qwc);

	MTGS::InitAndReadFIFO(reinterpret_cast<u8*>(pMem), size);

	g_vif1Cycles += size * 2;
	vif1ch.madr  += size * 16; /* MGS3 scene changes */
	if (vif1.GSLastDownloadSize >= vif1ch.qwc)
	{
		vif1.GSLastDownloadSize -= vif1ch.qwc;
		vif1Regs.stat.FQC = std::min((u32)16, vif1.GSLastDownloadSize);
		vif1ch.qwc = 0;
	}
	else
	{
		vif1Regs.stat.FQC = 0;
		vif1ch.qwc -= vif1.GSLastDownloadSize;
		vif1.GSLastDownloadSize = 0;
	}
}

bool _VIF1chain(void)
{
	u32* pMem;

	if (vif1ch.qwc == 0)
	{
		vif1.inprogress &= ~1;
		vif1.irqoffset.value = 0;
		vif1.irqoffset.enabled = false;
		return true;
	}

	/* Clarification - this is TO memory mode, for some reason i used the other way round >.< */
	if (vif1.dmamode == VIF_NORMAL_TO_MEM_MODE)
	{
		vif1TransferToMemory();
		vif1.inprogress &= ~1;
		return true;
	}

	if (!(pMem = (u32*)dmaGetAddr(vif1ch.madr, !vif1ch.chcr.DIR)))
	{
		vif1.cmd = 0;
		vif1.tag.size = 0;
		vif1ch.qwc = 0;
		return true;
	}

	if (vif1.irqoffset.enabled)
		return VIF1transfer(pMem + vif1.irqoffset.value, vif1ch.qwc * 4 - vif1.irqoffset.value, false);
	return VIF1transfer(pMem, vif1ch.qwc * 4, false);
}

__fi void vif1SetupTransfer(void)
{
	tDMA_TAG* ptag = dmaGetAddr(vif1ch.tadr, false); /* Set memory pointer to TADR */

	if (!(vif1ch.transfer(ptag)))
		return;

	vif1ch.madr = ptag[1]._u32; /* MADR = ADDR field + SPR */
	g_vif1Cycles    +=  1; /* Add 1 g_vifCycles from the QW read for the tag */
	vif1.inprogress &= ~1;

	if (!vif1.done && ((dmacRegs.ctrl.STD == STD_VIF1) && (ptag->ID == TAG_REFS))) /* STD == VIF1 */
	{
		/* there are still bugs, need to also check if gif->madr +16*qwc >= stadr, if not, stall */
		if ((vif1ch.madr + vif1ch.qwc * 16) > dmacRegs.stadr.ADDR)
		{
			/* stalled */
			hwDmacIrq(DMAC_STALL_SIS);
			CPU_SET_DMASTALL(DMAC_VIF1, true);
			return;
		}
	}

	if (vif1ch.chcr.TTE)
	{
		/* Transfer dma tag if tte is set */

		bool ret;

		alignas(16) static u128 masked_tag;

		masked_tag._u64[0] = 0;
		masked_tag._u64[1] = *((u64*)ptag + 1);

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer((u32*)&masked_tag + vif1.irqoffset.value, 4 - vif1.irqoffset.value, true); /* Transfer Tag on stall */
		else
		{
			/* Some games (like killzone) do Tags mid unpack, the nops will just write blank data
			 * to the VU's, which breaks stuff, this is where the 128bit packet will fail, so we ignore the first 2 words */
			vif1.irqoffset.value = 2;
			vif1.irqoffset.enabled = true;
			ret = VIF1transfer((u32*)&masked_tag + 2, 2, true); /* Transfer Tag */
		}

		if (!ret && vif1.irqoffset.enabled)
		{
			vif1.inprogress &= ~1; /* Better clear this so it has to do it again (Jak 1) */
			vif1ch.qwc       = 0;  /* Gumball 3000 pauses the DMA when the tag stalls so we need to reset the QWC, it'll be gotten again later */
			return; /* IRQ set by VIFTransfer */
		}
	}
	vif1.irqoffset.value   = 0;
	vif1.irqoffset.enabled = false;

	vif1.done |= hwDmacSrcChainWithStack(vif1ch, ptag->ID);

	if (vif1ch.qwc > 0)
		vif1.inprogress |= 1;

	/* Check TIE bit of CHCR and IRQ bit of tag */
	if (vif1ch.chcr.TIE && ptag->IRQ)
	{
		/* End Transfer */
		vif1.done = true;
		return;
	}
}

__fi void vif1VUFinish(void)
{
	/* Sync up VU1 so we don't errantly wait. */
	while (!THREAD_VU1 && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
	{
		const int cycle_diff = static_cast<int>(cpuRegs.cycle - vuRegs[1].cycle);

		if ((EmuConfig.Gamefixes.VUSyncHack && cycle_diff < vuRegs[1].nextBlockCycles) || cycle_diff <= 0)
			break;
		CpuVU1->ExecuteBlock();
	}

	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x500)
	{
		vu1Thread.Get_MTVUChanges();

		if (THREAD_VU1 && !INSTANT_VU1 && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			CPU_INT(VIF_VU1_FINISH, cpuGetCycles(VU_MTVU_BUSY));
		else
			CPU_INT(VIF_VU1_FINISH, 128);
		CPU_SET_DMASTALL(VIF_VU1_FINISH, true);
		return;
	}

	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
	{
		u32 _cycles = vuRegs[1].cycle;
		vu1Finish(false);
		if (THREAD_VU1 && !INSTANT_VU1 && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			CPU_INT(VIF_VU1_FINISH, cpuGetCycles(VU_MTVU_BUSY));
		else
			CPU_INT(VIF_VU1_FINISH, vuRegs[1].cycle - _cycles);
		CPU_SET_DMASTALL(VIF_VU1_FINISH, true);
		return;
	}

	vif1Regs.stat.VEW = false;

	if (vif1.waitforvu)
	{
		vif1.waitforvu = false;
		/* Check if VIF is already scheduled to interrupt, if it's waiting, kick it :P */
		if ((cpuRegs.interrupt & ((1 << DMAC_VIF1) | (1 << DMAC_MFIFO_VIF))) == 0 && vif1ch.chcr.STR && !VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			if (dmacRegs.ctrl.MFD == MFD_VIF1)
				vifMFIFOInterrupt();
			else
				vif1Interrupt();
		}
	}
}

__fi void vif1Interrupt(void)
{
	g_vif1Cycles = 0;

	if (gifRegs.stat.APATH == 2 && gifUnit.gifPath[GIF_PATH_2].isDone())
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH   = 0;
		vif1Regs.stat.VGW  = false; /* Let VIF continue if it's stuck on a flush */

		if (gifUnit.checkPaths(1, 0, 1))
			gifUnit.Execute(false, true);
	}

	/* Some games (Fahrenheit being one) start VIF first, let it loop through blankness while it sets MFIFO mode, so we need to check it here. */
	if (dmacRegs.ctrl.MFD == MFD_VIF1)
	{
		/* Test changed because the Final Fantasy 12 opening somehow has the tag in *Undefined* mode, which is not in the documentation that I saw. */
		vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
		vifMFIFOInterrupt();
		return;
	}

	/* We need to check the direction, if it is downloading
	 * from the GS then we handle that separately (KH2 for testing) */
	if (vif1ch.chcr.DIR)
	{
		bool isDirect   = (vif1.cmd & 0x7f) == 0x50;
		bool isDirectHL = (vif1.cmd & 0x7f) == 0x51;
		if ((isDirect && !gifUnit.CanDoPath2()) || (isDirectHL && !gifUnit.CanDoPath2HL()))
		{
			CPU_INT(DMAC_VIF1, 128);
			if (gifRegs.stat.APATH == 3)
				vif1Regs.stat.VGW = 1; /* We're waiting for path 3. Gunslinger II */
			CPU_SET_DMASTALL(DMAC_VIF1, true);
			return;
		}
		vif1Regs.stat.VGW = 0; /* Path 3 isn't busy so we don't need to wait for it. */
		vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);
		/* Simulated GS transfer time done, clear the flags */
	}

	if (vif1.waitforvu)
	{
		CPU_INT(VIF_VU1_FINISH, std::max(16, cpuGetCycles(VU_MTVU_BUSY)));
		CPU_SET_DMASTALL(DMAC_VIF1, true);
		return;
	}

	if (vif1Regs.stat.VGW)
	{
		CPU_SET_DMASTALL(DMAC_VIF1, true);
		return;
	}

	if (!vif1ch.chcr.STR)
		return;

	if (vif1.irq && vif1.vifstalled.enabled && vif1.vifstalled.value == VIF_IRQ_STALL)
	{
		if (!vif1Regs.stat.ER1)
			vif1Regs.stat.INT = true;

		/* Yakuza watches VIF_STAT so lets do this here. */
		if (((vif1Regs.code >> 24) & 0x7f) != 0x7)
			vif1Regs.stat.VIS = true;

		hwIntcIrq(VIF1INTC);
		--vif1.irq;

		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			/* NFSHPS stalls when the whole packet has gone across (it stalls in the last 32bit cmd)
			 * In this case VIF will end */
			vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
			if ((vif1ch.qwc > 0 || !vif1.done) && !CHECK_VIF1STALLHACK)
			{
				vif1Regs.stat.VPS = VPS_DECODING; /* If there's more data you need to say it's decoding the next VIF CMD (Onimusha - Blade Warriors) */
				CPU_SET_DMASTALL(DMAC_VIF1, true);
				return;
			}
		}
	}

	vif1.vifstalled.enabled = false;

	/* Mirroring change to VIF0 */
	if (vif1.cmd)
	{
		if (vif1.done && (vif1ch.qwc == 0))
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
		vif1Regs.stat.VPS = VPS_IDLE;

	if (vif1.inprogress & 0x1)
	{
		_VIF1chain();
		/* VIF_NORMAL_FROM_MEM_MODE is a very slow operation.
		 * Timesplitters 2 depends on this beeing a bit higher than 128. */
		if (vif1ch.chcr.DIR)
			vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);

		if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE)) /* If we're waiting on GIF, stop looping, (can be over 1000 loops!) */
		{
			if (vif1.waitforvu)
			{
				CPU_INT(DMAC_VIF1, std::max(static_cast<int>(g_vif1Cycles), cpuGetCycles(VU_MTVU_BUSY)));
			}
			else
				CPU_INT(DMAC_VIF1, g_vif1Cycles);
		}
		return;
	}

	if (!vif1.done)
	{

		if (!(dmacRegs.ctrl.DMAE) || vif1Regs.stat.VSS) /* Stopped or DMA Disabled */
			return;

		if ((vif1.inprogress & 0x1) == 0)
			vif1SetupTransfer();
		if (vif1ch.chcr.DIR)
			vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);

		if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE)) /* If we're waiting on GIF, stop looping, (can be over 1000 loops!) */
		{
			if (vif1.waitforvu)
			{
				CPU_INT(DMAC_VIF1, std::max(static_cast<int>(g_vif1Cycles), cpuGetCycles(VU_MTVU_BUSY)));
			}
			else
				CPU_INT(DMAC_VIF1, g_vif1Cycles);
		}
		return;
	}

	if (vif1.vifstalled.enabled && vif1.done)
	{
		CPU_INT(DMAC_VIF1, 0);
		CPU_SET_DMASTALL(DMAC_VIF1, true);
		return; /* Dont want to end if VIF is stalled. */
	}

	/* Reverse fifo has finished and nothing is left, so lets clear the outputting flag */
	if ((vif1ch.chcr.DIR == VIF_NORMAL_TO_MEM_MODE) && vif1.GSLastDownloadSize <= 16)
		gifRegs.stat.OPH = false;

	if (vif1ch.chcr.DIR)
		vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);

	vif1ch.chcr.STR = false;
	vif1.vifstalled.enabled = false;
	vif1.irqoffset.enabled = false;
	if (vif1.queued_program)
		vifExecQueue(1);
	g_vif1Cycles = 0;
	hwDmacIrq(DMAC_VIF1);
	CPU_SET_DMASTALL(DMAC_VIF1, false);
}

void dmaVIF1(void)
{
	g_vif1Cycles = 0;
	vif1.inprogress = 0;
	CPU_SET_DMASTALL(DMAC_VIF1, false);

	if (vif1ch.qwc > 0) /* Normal Mode */
	{
		/* ignore tag if it's a GS download (Def Jam Fight for NY) */
		if (vif1ch.chcr.MOD == CHAIN_MODE && vif1ch.chcr.DIR)
		{
			tDMA_TAG tmp;
			tmp._u32     = vif1ch.chcr._u32;
			vif1.dmamode = VIF_CHAIN_MODE;

			if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && vif1ch.chcr.TIE))
				vif1.done = true;
			else
				vif1.done = false;
		}
		else /* Assume normal mode for reverse FIFO and Normal. */
		{
			if (vif1ch.chcr.DIR) /* from Memory */
				vif1.dmamode = VIF_NORMAL_FROM_MEM_MODE;
			else
				vif1.dmamode = VIF_NORMAL_TO_MEM_MODE;

			vif1.done = true;
		}

		vif1.inprogress |= 1;
	}
	else
	{
		vif1.inprogress &= ~0x1;
		vif1.dmamode = VIF_CHAIN_MODE;
		vif1.done = false;
	}

	if (vif1ch.chcr.DIR)
		vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);

	/* Check VIF isn't stalled before starting the loop.
	 * Batman Vengence does something stupid and instead of cancelling a stall it tries to restart VIF, THEN check the stall
	 * However if VIF FIFO is reversed, it can continue */
	if (!vif1ch.chcr.DIR || !VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		CPU_INT(DMAC_VIF1, 4);
}

bool _VIF0chain(void)
{
	u32 *pMem;

	if (vif0ch.qwc == 0)
	{
		vif0.inprogress = 0;
		return true;
	}

	if (!(pMem = (u32*)dmaGetAddr(vif0ch.madr, false)))
	{
		vif0.cmd = 0;
		vif0.tag.size = 0;
		vif0ch.qwc = 0;
		return true;
	}

	if (vif0.irqoffset.enabled)
		return VIF0transfer(pMem + vif0.irqoffset.value, vif0ch.qwc * 4 - vif0.irqoffset.value, false);
	return VIF0transfer(pMem, vif0ch.qwc * 4, false);
}

__fi void vif0SetupTransfer(void)
{
	tDMA_TAG *ptag = dmaGetAddr(vif0ch.tadr, false); /* Set memory pointer to TADR */

	if (!(vif0ch.transfer(ptag))) return;

	vif0ch.madr   = ptag[1]._u32;            /* MADR = ADDR field + SPR */
	g_vif0Cycles += 1; /* Add 1 g_vifCycles from the QW read for the tag */

	/* Transfer dma tag if TTE is set */
	vif0.inprogress = 0;

	if (vif0ch.chcr.TTE)
	{
		/* Transfer dma tag if TTE is set */

		bool ret;

		alignas(16) static u128 masked_tag;

		masked_tag._u64[0] = 0;
		masked_tag._u64[1] = *((u64*)ptag + 1);

		if (vif0.irqoffset.enabled)
			ret = VIF0transfer((u32*)&masked_tag + vif0.irqoffset.value, 4 - vif0.irqoffset.value, true);  /* Transfer Tag on stall */
		else
		{
			/* Some games (like Killzone) do Tags mid unpack, the nops will just write blank data
			 * to the VU's, which breaks stuff, this is where the 128bit packet will fail, so we ignore the first 2 words */
			vif0.irqoffset.value   = 2;
			vif0.irqoffset.enabled = true;
			ret = VIF0transfer((u32*)&masked_tag + 2, 2, true);  /* Transfer Tag */
		}

		if (!ret && vif0.irqoffset.enabled)
		{
			vif0.inprogress = 0; /* Better clear this so it has to do it again (Jak 1) */
			vif0ch.qwc      = 0; /* Gumball 3000 pauses the DMA when the tag stalls so we need to reset the QWC, it'll be gotten again later */
			return;        /* IRQ set by VIFTransfer */
		}
	}

	vif0.irqoffset.value = 0;
	vif0.irqoffset.enabled = false;
	vif0.done |= hwDmacSrcChainWithStack(vif0ch, ptag->ID);

	if(vif0ch.qwc > 0)
		vif0.inprogress = 1;
	/* Check TIE bit of CHCR and IRQ bit of tag */
	if (vif0ch.chcr.TIE && ptag->IRQ)
		vif0.done = true; /* End Transfer */
}

__fi void vif0VUFinish(void)
{
	/* Sync up VU0 so we don't errantly wait. */
	while (vuRegs[0].VI[REG_VPU_STAT].UL & 0x1)
	{
		const int cycle_diff = static_cast<int>(cpuRegs.cycle - vuRegs[0].cycle);

		if ((EmuConfig.Gamefixes.VUSyncHack && cycle_diff < vuRegs[0].nextBlockCycles) || cycle_diff <= 0)
			break;
		CpuVU0->ExecuteBlock();
	}

	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x5)
	{
		CPU_INT(VIF_VU0_FINISH, 128);
		CPU_SET_DMASTALL(VIF_VU0_FINISH, true);
		return;
	}

	if ((vuRegs[0].VI[REG_VPU_STAT].UL & 1))
	{
		int _cycles = vuRegs[0].cycle;
		vu0Finish();
		_cycles = vuRegs[0].cycle - _cycles;
		CPU_INT(VIF_VU0_FINISH, _cycles * BIAS);
		CPU_SET_DMASTALL(VIF_VU0_FINISH, true);
		return;
	}
	vif0Regs.stat.VEW = false;
	if(vif0.waitforvu)
	{
		vif0.waitforvu = false;
		/* Make sure VIF0 isnt already scheduled to spin. */
		if(!(cpuRegs.interrupt & 0x1) && vif0ch.chcr.STR && !VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
			vif0Interrupt();
	}
}

__fi void vif0Interrupt(void)
{
	g_vif0Cycles = 0;

	vif0Regs.stat.FQC = std::min(vif0ch.qwc, (u32)8);

	if(vif0.waitforvu)
	{
		CPU_INT(VIF_VU0_FINISH, 16);
		CPU_SET_DMASTALL(DMAC_VIF0, true);
		return;
	}

	if (vif0.irq && vif0.vifstalled.enabled && vif0.vifstalled.value == VIF_IRQ_STALL)
	{
		if (!vif0Regs.stat.ER1)
			vif0Regs.stat.INT = true;

		/* Yakuza watches VIF_STAT so lets do this here. */
		if (((vif0Regs.code >> 24) & 0x7f) != 0x7)
			vif0Regs.stat.VIS = true;

		hwIntcIrq(VIF0INTC);
		--vif0.irq;

		if (VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
		{
			/* One game doesn't like VIF stalling at end, can't remember what. Spiderman isn't keen on it tho */
			vif0Regs.stat.FQC = std::min((u32)0x8, vif0ch.qwc);
			if (vif0ch.qwc > 0 || !vif0.done)
			{
				vif0Regs.stat.VPS = VPS_DECODING; /* If there's more data you need to say it's decoding the next VIF CMD (Onimusha - Blade Warriors) */
				CPU_SET_DMASTALL(DMAC_VIF0, true);
				return;
			}
		}
	}

	vif0.vifstalled.enabled = false;

	/* Must go after the Stall, incase it's still in progress, GTC africa likes to see it still transferring. */
	if (vif0.cmd)
	{
		if(vif0.done && vif0ch.qwc == 0)
			vif0Regs.stat.VPS = VPS_WAITING;
	}
	else
		vif0Regs.stat.VPS = VPS_IDLE;

	if (vif0.inprogress & 0x1)
	{
		_VIF0chain();
		vif0Regs.stat.FQC = std::min(vif0ch.qwc, (u32)8);
		CPU_INT(DMAC_VIF0, g_vif0Cycles);
		return;
	}

	if (!vif0.done)
	{
		if (!(dmacRegs.ctrl.DMAE) || vif0Regs.stat.VSS) /* Stopped or DMA Disabled */
			return;

		if ((vif0.inprogress & 0x1) == 0) vif0SetupTransfer();
		vif0Regs.stat.FQC = std::min(vif0ch.qwc, (u32)8);
		CPU_INT(DMAC_VIF0, g_vif0Cycles);
		return;
	}

	if (vif0.vifstalled.enabled && vif0.done)
	{
		CPU_INT(DMAC_VIF0, 0);
		return; /* Dont want to end if VIF is stalled. */
	}

	vif0ch.chcr.STR = false;
	vif0Regs.stat.FQC = std::min((u32)0x8, vif0ch.qwc);
	vif0.vifstalled.enabled = false;
	vif0.irqoffset.enabled = false;
	if(vif0.queued_program) vifExecQueue(0);
	g_vif0Cycles = 0;
	hwDmacIrq(DMAC_VIF0);
	CPU_SET_DMASTALL(DMAC_VIF0, false);
	vif0Regs.stat.FQC = 0;
}

void dmaVIF0(void)
{
	g_vif0Cycles = 0;
	CPU_SET_DMASTALL(DMAC_VIF0, false);

	if (vif0ch.qwc > 0)   /* Normal Mode */
	{
		if (vif0ch.chcr.MOD == CHAIN_MODE)
		{
			tDMA_TAG tmp;
			tmp._u32     = vif0ch.chcr._u32;
			vif0.dmamode = VIF_CHAIN_MODE;

			if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && vif0ch.chcr.TIE))
				vif0.done = true;
			else
				vif0.done = false;
		}
		else /* Assume Normal mode. */
		{
			vif0.dmamode = VIF_NORMAL_FROM_MEM_MODE;
			vif0.done    = true;
		}

		vif0.inprogress |= 1;
	}
	else
	{
		vif0.dmamode     = VIF_CHAIN_MODE;
		vif0.done        = false;
		vif0.inprogress &= ~0x1;
	}

	vif0Regs.stat.FQC = std::min((u32)0x8, vif0ch.qwc);

	/* Using a delay as Beyond Good and Evil does the DMA twice with 2 different TADR's (no checks in the middle, all one block of code),
	 * the first bit it sends isnt required for it to work.
	 * Also being an end chain it ignores the second lot, this causes infinite loops ;p */
	if (!VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
		CPU_INT(DMAC_VIF0, 4);
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
			switch (mode)
			{
				case 1:
					dest = data + vif.MaskRow._u32[offnum];
					break;
				case 2:
					dest = vif.MaskRow._u32[offnum] = vif.MaskRow._u32[offnum] + data;
					break;
				case 3:
					dest = vif.MaskRow._u32[offnum] = data;
					break;
				default:
					dest = data;
					break;
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
template<int idx> static void vifUnpackSetup(const u32 *data)
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
	{
		/* This is used by the interpreted SSE unpacks only.  Recompiled SSE unpacks
		 * and the interpreted C unpacks use the vif.MaskRow/MaskCol members directly. */
		for (int i = 0; i < 16; i++)
		{
			int m = (vifRegs.mask >> (i * 2)) & 3;
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

/*------------------------------------------------------------------
 * VifCode Transfer Interpreter (Vif0/Vif1)
 *------------------------------------------------------------------
 */

template<int idx> static __fi bool vifTransfer(u32 *data, int size, bool TTE)
{
	vifStruct& vifX       = (idx ? (vif1)     : (vif0));
	VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));
	DMACh& vifXch         = (idx ? (vif1ch)   : (vif0ch));

	/* irqoffset necessary to add up the right qws, or else will spin (spiderman) */
	int transferred       = vifX.irqoffset.enabled ? vifX.irqoffset.value : 0;

	vifX.vifpacketsize    = size;

	/* Interprets packet */
	u32& pSize            = vifX.vifpacketsize;

	vifRegs.stat.VPS     |= VPS_TRANSFERRING;
	vifRegs.stat.ER1      = false;

	while (pSize > 0 && !vifX.vifstalled.enabled)
	{
		int ret = 0;
		if(!vifX.cmd)
		{
			/* Get new VifCode */
			if(!vifRegs.err.MII)
			{
				if(vifX.irq && !CHECK_VIF1STALLHACK)
					break;

				vifX.irq      |= data[0] >> 31;
			}

			vifRegs.code    = data[0];
			vifX.cmd	= data[0] >> 24;
		}

		ret     = vifCmdHandler[idx][vifX.cmd & 0x7f](vifX.pass, data);
		data   += ret;
		pSize  -= ret;
	}

	transferred += size - vifX.vifpacketsize;

	/* Make this a minimum of 1 cycle so if it's the end of the packet it doesnt just fall through.
	 * Metal Saga can do this, just to be safe :) */
	if (idx)
		g_vif1Cycles += std::max(1, (int)((transferred * BIAS) >> 2));
	else
		g_vif0Cycles += std::max(1, (int)((transferred * BIAS) >> 2));

	vifX.irqoffset.value  = transferred % 4; /* cannot lose the offset */

	if (vifX.irq && vifX.cmd == 0)
	{
		/* Always needs to be set to return to the correct offset if there is data left. */
		vifX.vifstalled.enabled = VifStallEnable(vifXch);
		vifX.vifstalled.value   = VIF_IRQ_STALL;
	}

	if (TTE) /* *WARNING* - Tags CAN have interrupts! so lets just ignore the dma modifying stuffs (GT4) */
	{
		if(vifX.irqoffset.value != 0)
			vifX.irqoffset.enabled = true;
		else
			vifX.irqoffset.enabled = false;
	}
	else
	{
		transferred  = transferred >> 2;
		transferred  = std::min((int)vifXch.qwc, transferred);
		vifXch.madr += (transferred << 4);
		vifXch.qwc  -=  transferred;

		hwDmacSrcTadrInc(vifXch);

		vifX.irqoffset.enabled = false;

		if(!vifXch.qwc)
			vifX.inprogress &= ~0x1;
		else if(vifX.irqoffset.value != 0)
			vifX.irqoffset.enabled = true;
	}

	vifExecQueue(idx);

	return !vifX.vifstalled.enabled;
}

/* When TTE is set to 1, MADR and QWC are not updated as part of the transfer. */
bool VIF0transfer(u32 *data, int size, bool TTE)
{
	return vifTransfer<0>(data, size, TTE);
}

bool VIF1transfer(u32 *data, int size, bool TTE)
{
	return vifTransfer<1>(data, size, TTE);
}

/*------------------------------------------------------------------
 * Vif0/Vif1 Write32
 *------------------------------------------------------------------
 */

static __fi void vif0FBRST(u32 value)
{
	/* Fixme: Forcebreaks are pretty unknown for operation, presumption is 
	 * it just stops it what its doing usually accompanied by a reset, 
	 * but if we find a broken game which falls here, we need to see it! (Refraction) */
	if (value & 0x2) /* Forcebreak Vif, */
	{
		/* I guess we should stop the VIF dma here, but not 100% sure (linuz) */
		cpuRegs.interrupt &= ~1; /* Stop all VIF0 DMA's */
		vif0Regs.stat.VFS = true;
		vif0Regs.stat.VPS = VPS_IDLE;
	}

	if (value & 0x4) /* Stop Vif. */
	{
		/* Not completely sure about this, can't remember what game, 
		 * used this, but 'draining' the VIF helped it, instead of
		 * just stopping the VIF (linuz). */
		vif0Regs.stat.VSS       = true;
		vif0Regs.stat.VPS       = VPS_IDLE;
		vif0.vifstalled.enabled = VifStallEnable(vif0ch);
		vif0.vifstalled.value   = VIF_IRQ_STALL;
	}

	if (value & 0x8) /* Cancel Vif Stall. */
	{
		bool cancel = false;

		/* Cancel stall, first check if there is a stall to cancel, 
		 * and then clear VIF0_STAT VSS|VFS|VIS|INT|ER0|ER1 bits */
		if (VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
			cancel = true;

		vif0Regs.stat._u32 &= ~(VIF0_STAT_VSS | VIF0_STAT_VFS | VIF0_STAT_VIS |
				        VIF0_STAT_INT | VIF0_STAT_ER0 | VIF0_STAT_ER1);
		if (cancel)
		{
			g_vif0Cycles = 0;
			/* loop necessary for spiderman */
			if (vif0ch.chcr.STR)
				CPU_INT(DMAC_VIF0, 0); /* Gets the timing right - Flatout */
		}
	}

	if (value & 0x1) /* Reset VIF. */
	{
		u128 SaveCol;
		u128 SaveRow;

		/* Must Preserve Row/Col registers! (Downhill Domination for testing) */
		SaveCol._u64[0] = vif0.MaskCol._u64[0];
		SaveCol._u64[1] = vif0.MaskCol._u64[1];
		SaveRow._u64[0] = vif0.MaskRow._u64[0];
		SaveRow._u64[1] = vif0.MaskRow._u64[1];
		memset(&vif0, 0, sizeof(vif0));
		vif0.MaskCol._u64[0] = SaveCol._u64[0];
		vif0.MaskCol._u64[1] = SaveCol._u64[1];
		vif0.MaskRow._u64[0] = SaveRow._u64[0];
		vif0.MaskRow._u64[1] = SaveRow._u64[1];
		vif0ch.qwc = 0; /* ? */
		cpuRegs.interrupt &= ~1; /* Stop all VIF0 DMA's */
		psHu64(VIF0_FIFO) = 0;
		psHu64(VIF0_FIFO + 8) = 0;
		vif0.vifstalled.enabled = false;
		vif0.irqoffset.enabled = false;
		vif0.inprogress = 0;
		vif0.cmd = 0;
		vif0.done = true;
		vif0ch.chcr.STR = false;
		vif0Regs.err._u32   = 0;
		vif0Regs.stat._u32 &= ~(VIF0_STAT_FQC | VIF0_STAT_INT | VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS | VIF0_STAT_VPS); /* FQC=0 */
	}
}

static __fi void vif1FBRST(u32 value)
{
	tVIF_FBRST tmp;
	tmp._u32 = value;
	/* Fixme: Forcebreaks are pretty unknown for operation, 
	 * presumption is it just stops it what its doing
	 * usually accompanied by a reset, but if we find a 
	 * broken game which falls here, we need to see it! (Refraction) */

	if (tmp.FBK) /* Forcebreak VIF. */
	{
		/* I guess we should stop the VIF dma here, but not 100% sure (linuz) */
		vif1Regs.stat.VFS        = true;
		vif1Regs.stat.VPS        = VPS_IDLE;
		cpuRegs.interrupt       &= ~((1 << 1) | (1 << 10)); /* Stop all VIF1 DMA's */
		vif1.vifstalled.enabled  = VifStallEnable(vif1ch);
		vif1.vifstalled.value    = VIF_IRQ_STALL;
	}

	if (tmp.STP) /* Stop VIF. */
	{
		/* Not completely sure about this, can't remember 
		 * what game used this, but 'draining' the VIF helped it, instead of
		 * just stopping the VIF (linuz). */
		vif1Regs.stat.VSS       = true;
		vif1Regs.stat.VPS       = VPS_IDLE;
		vif1.vifstalled.enabled = VifStallEnable(vif1ch);
		vif1.vifstalled.value   = VIF_IRQ_STALL;
	}

	if (tmp.STC) /* Cancel VIF Stall. */
	{
		bool cancel = false;
		/* Cancel stall, first check if there is a stall to cancel, 
		 * and then clear VIF1_STAT VSS|VFS|VIS|INT|ER0|ER1 bits */
		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
			cancel = true;

		vif1Regs.stat._u32 &= ~(VIF1_STAT_VSS | VIF1_STAT_VFS | VIF1_STAT_VIS |
		         		VIF1_STAT_INT | VIF1_STAT_ER0 | VIF1_STAT_ER1);

		if (cancel)
		{
			g_vif1Cycles = 0;
			/* loop necessary for spiderman */
			switch (dmacRegs.ctrl.MFD)
			{
				case MFD_VIF1:
					/* MFIFO active and not empty */
					if (vif1ch.chcr.STR && !VIF_TEST(vif1Regs.stat, VIF1_STAT_FDR))
						CPU_INT(DMAC_MFIFO_VIF, 0);
					break;

				case NO_MFD:
				case MFD_RESERVED:
				case MFD_GIF: /* Wonder if this should be with VIF?
					       * Gets the timing right - Flatout */
					if (vif1ch.chcr.STR && !VIF_TEST(vif1Regs.stat, VIF1_STAT_FDR))
						CPU_INT(DMAC_VIF1, 0);
					break;
			}
		}
	}

	if (tmp.RST) /* Reset VIF. */
	{
		u128 SaveCol;
		u128 SaveRow;
		/* Must Preserve Row/Col registers! 
		 * (Downhill Domination for testing) - Really shouldnt be part of the vifstruct. */
		SaveCol._u64[0] = vif1.MaskCol._u64[0];
		SaveCol._u64[1] = vif1.MaskCol._u64[1];
		SaveRow._u64[0] = vif1.MaskRow._u64[0];
		SaveRow._u64[1] = vif1.MaskRow._u64[1];
		u8 mfifo_empty = vif1.inprogress & 0x10;
		memset(&vif1, 0, sizeof(vif1));
		vif1.MaskCol._u64[0] = SaveCol._u64[0];
		vif1.MaskCol._u64[1] = SaveCol._u64[1];
		vif1.MaskRow._u64[0] = SaveRow._u64[0];
		vif1.MaskRow._u64[1] = SaveRow._u64[1];

		vif1Regs.mskpath3 = false;
		gifRegs.stat.M3P = 0;
		vif1Regs.err._u32 = 0;
		vif1.inprogress = mfifo_empty;
		vif1.cmd = 0;
		vif1.vifstalled.enabled = false;
		vif1Regs.stat._u32 = 0;
	}
}

static __fi void vif1STAT(u32 value)
{
	/* Only FDR bit is writable, so mask the rest */
	if ((vif1Regs.stat.FDR) ^ ((tVIF_STAT&)value).FDR)
	{
		bool isStalled = false;
		/* different so can't be stalled */
		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_INT | VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
			isStalled = true;

		/* Hack!! Hotwheels seems to leave 1QW in the fifo and expect the DMA to be ready for a reverse FIFO
		 * There's no important data in there so for it to work, we will just end it.
		 * Hotwheels had this in the "direction when stalled" area, however Sled Storm seems to keep an eye on the dma
		 * position, as we clear it and set it to the end well before the interrupt, the game assumes it's finished,
		 * then proceeds to reverse the dma before we have even done it ourselves. So lets just make sure VIF is ready :) */
		if (vif1ch.qwc > 0 || isStalled == false)
		{
			if (vif1ch.chcr.STR)
			{
				vif1ch.qwc = 0;
				hwDmacIrq(DMAC_VIF1);
				vif1ch.chcr.STR = false;
			}
			cpuRegs.interrupt &= ~((1 << DMAC_VIF1) | (1 << DMAC_MFIFO_VIF));
		}
		/* This is actually more important for our handling, else the DMA for reverse fifo doesnt start properly. */
	}

	tVIF_STAT tmp;
	tmp._u32 = value;
	vif1Regs.stat.FDR = tmp.FDR;

	if (vif1Regs.stat.FDR) /* VIF transferring to memory. */
	{
		/* Hack but it checks this is true before transfer? (fatal frame)
		 * Update Refraction: Use of this function has been investigated and understood.
		 * Before this ever happens, a DIRECT/HL command takes place sending the transfer info to the GS
		 * One of the registers told about this is TRXREG which tells us how much data is going to transfer (th x tw) in words
		 * As far as the GS is concerned, the transfer starts as soon as TRXDIR is accessed, which is why fatal frame
		 * was expecting data, the GS should already be sending it over (buffering in the FIFO)
		 */

		vif1Regs.stat.FQC = std::min((u32)16, vif1.GSLastDownloadSize);
	}
	else /* Memory transferring to VIF. */
	{
		/* Sometimes the value from the GS is bigger than vif wanted, so it just sets it back and cancels it.
		 * Other times it can read it off ;) */
		vif1Regs.stat.FQC = 0;
		if (vif1ch.chcr.STR)
			CPU_INT(DMAC_VIF1, 0);
	}
}

#define caseVif(x) (idx ? VIF1_##x : VIF0_##x)

template<int idx> __fi u32 vifRead32(u32 mem)
{
	vifStruct& vif = (idx ? ((THREAD_VU1) ? vu1Thread.vif     : vif1)     : (vif0));
	bool wait      = idx && THREAD_VU1;

	switch (mem)
	{
		case caseVif(ROW0):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[0];
		case caseVif(ROW1):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[1];
		case caseVif(ROW2):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[2];
		case caseVif(ROW3):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[3];

		case caseVif(COL0):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[0];
		case caseVif(COL1):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[1];
		case caseVif(COL2):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[2];
		case caseVif(COL3):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[3];
	}

	return psHu32(mem);
}

/* returns FALSE if no writeback is needed (or writeback is handled internally)
 * returns TRUE if the caller should writeback the value to the eeHw register map. */
template<int idx> __fi bool vifWrite32(u32 mem, u32 value)
{
	vifStruct&        vif = (idx ? (vif1)     : (vif0));
	VIFregisters& vifRegs = (idx ? (vif1Regs) : (vif0Regs));

	switch (mem)
	{
		case caseVif(MARK):
			vifRegs.stat.MRK = false;
			break;

		case caseVif(FBRST):
			if (!idx)
				vif0FBRST(value);
			else
				vif1FBRST(value);
			return false;

		case caseVif(STAT):
			if (idx)
			{ /* Only Vif1 does this stuff? */
				vif1STAT(value);
			}
			return false;

		case caseVif(ERR):
		case caseVif(MODE):
			/* standard register writes -- handled by caller. */
			break;

		case caseVif(ROW0):
			vif.MaskRow._u32[0] = value;
			vu1Thread.WriteRow(vif);
			return false;
		case caseVif(ROW1):
			vif.MaskRow._u32[1] = value;
			vu1Thread.WriteRow(vif);
			return false;
		case caseVif(ROW2):
			vif.MaskRow._u32[2] = value;
			vu1Thread.WriteRow(vif);
			return false;
		case caseVif(ROW3):
			vif.MaskRow._u32[3] = value;
			vu1Thread.WriteRow(vif);
			return false;

		case caseVif(COL0):
			vif.MaskCol._u32[0] = value;
			vu1Thread.WriteCol(vif);
			return false;
		case caseVif(COL1):
			vif.MaskCol._u32[1] = value;
			vu1Thread.WriteCol(vif);
			return false;
		case caseVif(COL2):
			vif.MaskCol._u32[2] = value;
			vu1Thread.WriteCol(vif);
			return false;
		case caseVif(COL3):
			vif.MaskCol._u32[3] = value;
			vu1Thread.WriteCol(vif);
			return false;
	}

	/* fall-through case: issue standard writeback behavior. */
	return true;
}

template u32 vifRead32<0>(u32 mem);
template u32 vifRead32<1>(u32 mem);

template bool vifWrite32<0>(u32 mem, u32 value);
template bool vifWrite32<1>(u32 mem, u32 value);
