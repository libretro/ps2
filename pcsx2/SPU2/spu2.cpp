/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include "PrecompiledHeader.h"
#include "SPU2/Global.h"
#include "SPU2/spu2.h"
#include "SPU2/Dma.h"
#include "R3000A.h"

namespace SPU2
{
	static void InitSndBuffer();
	static void UpdateSampleRate();
	static void InternalReset(bool psxmode);
} // namespace SPU2

static double s_device_sample_rate_multiplier = 1.0;
static bool s_psxmode = false;

int SampleRate = 48000;

u32 lClocks = 0;

s32 SPU2::GetConsoleSampleRate(void)
{
	return s_psxmode ? 44100 : 48000;
}

// --------------------------------------------------------------------------------------
//  DMA 4/7 Callbacks from Core Emulator
// --------------------------------------------------------------------------------------


void SPU2readDMA4Mem(u16* pMem, u32 size) // size now in 16bit units
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAread(pMem, size);
}

void SPU2writeDMA4Mem(u16* pMem, u32 size) // size now in 16bit units
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAwrite(pMem, size);
}

void SPU2interruptDMA4()
{
	if (Cores[0].DmaMode)
		Cores[0].Regs.STATX |= 0x80;
	Cores[0].Regs.STATX &= ~0x400;
	Cores[0].TSA = Cores[0].ActiveTSA;
}

void SPU2interruptDMA7()
{
	if (Cores[1].DmaMode)
		Cores[1].Regs.STATX |= 0x80;
	Cores[1].Regs.STATX &= ~0x400;
	Cores[1].TSA = Cores[1].ActiveTSA;
}

void SPU2readDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAread(pMem, size);
}

void SPU2writeDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAwrite(pMem, size);
}

void SPU2::InitSndBuffer()
{
	SndBuffer::Init();

	if (SampleRate != GetConsoleSampleRate())
	{
		// It'll get stretched instead..
		const int original_sample_rate = SampleRate;
		Console.Error("Failed to init SPU2 at adjusted sample rate %u, trying console rate.", SampleRate);
		SampleRate = GetConsoleSampleRate();
		SndBuffer::Init();
		SampleRate = original_sample_rate;
	}
}

void SPU2::UpdateSampleRate()
{
	const int new_sample_rate = static_cast<int>(std::round(static_cast<double>(GetConsoleSampleRate()) * s_device_sample_rate_multiplier));
	if (SampleRate == new_sample_rate)
		return;

	SampleRate = new_sample_rate;
	InitSndBuffer();
}

void SPU2::InternalReset(bool psxmode)
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

void SPU2::Reset(bool psxmode)
{
	InternalReset(psxmode);
	UpdateSampleRate();
}

void SPU2::Initialize(void)
{
	// Patch up a copy of regtable that directly maps "nullptrs" to SPU2 memory.
	memcpy(regtable, regtable_original, sizeof(regtable));

	for (uint mem = 0; mem < 0x800; mem++)
	{
		u16* ptr = regtable[mem >> 1];
		if (!ptr)
			regtable[mem >> 1] = &(spu2Ru16(mem));
	}
}


bool SPU2::Open()
{
	lClocks = psxRegs.cycle;

	InternalReset(false);

	SampleRate = static_cast<int>(std::round(static_cast<double>(GetConsoleSampleRate()) * s_device_sample_rate_multiplier));
	InitSndBuffer();
	return true;
}

void SPU2::Close()
{
}

void SPU2::Shutdown()
{
}

bool SPU2::IsRunningPSXMode()
{
	return s_psxmode;
}

void SPU2async(u32 cycles)
{
	TimeUpdate(psxRegs.cycle);
}

u16 SPU2read(u32 rmem)
{
	u16 ret        = 0xDEAD;
	u32 core       = 0;
	const u32 mem  = rmem & 0xFFFF;
	u32 omem       = mem;

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
				SetIrqCall(i);
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
		SPU2_FastWrite(rmem, value);
}

s32 SPU2freeze(FreezeAction mode, freezeData* data)
{
	if (!data)
		return -1;

	if (mode == FreezeAction::Size)
	{
		data->size = SPU2Savestate::SizeIt();
		return 0;
	}

	if (data->data == nullptr)
		return -1;

	auto& spud = (SPU2Savestate::DataBlock&)*(data->data);

	switch (mode)
	{
		case FreezeAction::Load:
			return SPU2Savestate::ThawIt(spud);
		case FreezeAction::Save:
			return SPU2Savestate::FreezeIt(spud);
		default:
			break;
	}

	// technically unreachable, but kills a warning:
	return 0;
}
