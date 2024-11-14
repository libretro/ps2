/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#include "Global.h"
#include "spu2.h"
#include "Dma.h"

#include "../R3000A.h"

static bool s_psxmode = false;

u32 lClocks = 0;

// --------------------------------------------------------------------------------------
//  DMA 4/7 Callbacks from Core Emulator
// --------------------------------------------------------------------------------------

static void SPU2_InternalReset(bool psxmode)
{
	s_psxmode = psxmode;
	if (!s_psxmode)
	{
		memset(spu2regs, 0, 0x010000);
		memset(_spu2mem, 0, 0x200000);
		memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
		memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.

		Spdif.Info = 0; // Reset IRQ Status if it got set in a previously run game

		Cores[0].Init(0);
		Cores[1].Init(1);
	}
}

void SPU2::Reset(bool psxmode) { SPU2_InternalReset(psxmode); }
void SPU2::Initialize(void)    { }

void SPU2::Open()
{
	lClocks = psxRegs.cycle;

	SPU2_InternalReset(false);
}

void SPU2::Close() { }
void SPU2::Shutdown() { }
bool SPU2::IsRunningPSXMode() { return s_psxmode; }

u16 SPU2read(u32 rmem)
{
	u16 ret = 0xDEAD;
	u32 core = 0;
	const u32 mem = rmem & 0xFFFF;
	u32 omem = mem;

	if (mem & 0x400)
	{
		omem ^= 0x400;
		core = 1;
	}

	if (omem == 0x1f9001AC)
	{
		Cores[core].ActiveTSA = Cores[core].TSA;
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
				has_to_call_irq[i] = true;
		}
		ret = Cores[core].DmaRead();
	}
	else
	{
		TimeUpdate(psxRegs.cycle);

		if (rmem >> 16 == 0x1f80)
			ret = Cores[0].ReadRegPS1(rmem);
		else if (mem >= 0x800)
			ret = spu2Ru16(mem);
		else
			ret = *(regtable[(mem >> 1)]);
	}

	return ret;
}

void SPU2write(u32 rmem, u16 value)
{
	// Note: Reverb/Effects are very sensitive to having precise update timings.
	// If the SPU2 isn't in in sync with the IOP, samples can end up playing at rather
	// incorrect pitches and loop lengths.

	TimeUpdate(psxRegs.cycle);

	if (rmem >> 16 == 0x1f80)
		Cores[0].WriteRegPS1(rmem, value);
	else
		tbl_reg_writes[(rmem & 0x7ff) / 2](value);
}

s32 SPU2freeze(FreezeAction mode, freezeData* data)
{
	if (!data)
		return -1;

	if (mode == FreezeAction::Size)
		data->size = SPU2Savestate::SizeIt();
	else
	{
		if (data->data == nullptr)
			return -1;

		auto& spud = (SPU2Savestate::DataBlock&)*(data->data);

		switch (mode)
		{
			case FreezeAction::Load:
				return SPU2Savestate::ThawIt(spud);
			case FreezeAction::Save:
				SPU2Savestate::FreezeIt(spud);
				break;
			default:
				break;
		}
	}

	return 0;
}
