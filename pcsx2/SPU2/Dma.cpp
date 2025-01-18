/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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
#include "Dma.h"
#include "spu2.h"

#include "../R3000A.h"
#include "../IopHw.h"
#include "../Config.h"

void V_Core::AutoDMAReadBuffer(int mode) //mode: 0= split stereo; 1 = do not split stereo
{
	u32 spos = InputPosWrite & 0x100; // Starting position passed by TSA
	bool leftbuffer = !(InputPosWrite & 0x80);

	if (InputPosWrite == 0xFFFF) // Data request not made yet
		return;

	AutoDMACtrl &= 0x3;

	int size = std::min(InputDataLeft, (u32)0x200);
	if (!leftbuffer)
		size = std::min(size, 0x100);
	// HACKFIX!! DMAPtr can be invalid after a savestate load, so the savestate just forces it
	// to nullptr and we ignore it here.  (used to work in old VM editions of PCSX2 with fixed
	// addressing, but new PCSX2s have dynamic memory addressing).
	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)&iopMem->Main[MADR & 0x1fffff];
		InputDataProgress = 0;
	}

	if (mode)
	{
		if (DMAPtr != nullptr)
			memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), DMAPtr + InputDataProgress, size);
		MADR += size;
		InputDataLeft -= 0x200;
		InputDataProgress += 0x200;
	}
	else
	{
		while (size)
		{
			if (!leftbuffer)
				spos |= 0x200;
			else
				spos &= ~0x200;

			if (DMAPtr != nullptr)
				memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), DMAPtr + InputDataProgress, 0x200);
			InputDataTransferred += 0x200;
			InputDataLeft -= 0x100;
			InputDataProgress += 0x100;
			leftbuffer = !leftbuffer;
			size -= 0x100;
			InputPosWrite += 0x80;
		}
	}
	if (!(InputPosWrite & 0x80))
		InputPosWrite = 0xFFFF;
}

void V_Core::StartADMAWrite(u16* pMem, u32 sz)
{
	int size = sz;

	TimeUpdate(psxRegs.cycle);

	InputDataProgress = 0;
	TADR = MADR + (size << 1);
	if ((AutoDMACtrl & (Index + 1)) == 0)
	{
		ActiveTSA = 0x2000 + (Index << 10);
		DMAICounter = size * 4;
		LastClock = psxRegs.cycle;
	}
	else if (size >= 256)
	{
		InputDataLeft = size;
		if (InputPosWrite != 0xFFFF)
		{
#ifdef PCM24_S1_INTERLEAVE
			if ((Index == 1) && ((PlayMode & 8) == 8))
			{
				AutoDMAReadBuffer(Index, 1);
			}
			else
			{
				AutoDMAReadBuffer(Index, 0);
			}
#else
			AutoDMAReadBuffer(0);
#endif
		}
		AdmaInProgress = 1;
	}
	else
	{
		InputDataLeft = 0;
		DMAICounter = size * 4;
		LastClock = psxRegs.cycle;
	}
}

void V_Core::PlainDMAWrite(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	ReadSize = size;
	IsDMARead = false;
	DMAICounter = 0;
	LastClock = psxRegs.cycle;
	Regs.STATX &= ~0x80;
	Regs.STATX |= 0x400;
	TADR = MADR + (size << 1);

	FinishDMAwrite();
}

void V_Core::FinishDMAwrite()
{
	if (!DMAPtr)
		DMAPtr = (u16*)&iopMem->Main[MADR & 0x1fffff];

	DMAICounter = ReadSize;

	u32 buff1end = ActiveTSA + std::min(ReadSize, (u32)0x100 + std::abs(DMAICounter / 4));
	u32 buff2end = 0;

	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	const int cacheIdxStart = ActiveTSA / pcm_WordsPerBlock;
	const int cacheIdxEnd = (buff1end + pcm_WordsPerBlock - 1) / pcm_WordsPerBlock;
	PcmCacheEntry* cacheLine = &pcm_cache_data[cacheIdxStart];
	PcmCacheEntry& cacheEnd = pcm_cache_data[cacheIdxEnd];

	do
	{
		cacheLine->Validated = false;
		cacheLine++;
	} while (cacheLine != &cacheEnd);

	// First Branch needs cleared:
	// It starts at TSA and goes to buff1end.

	const u32 buff1size = (buff1end - ActiveTSA);
	memcpy(GetMemPtr(ActiveTSA), DMAPtr, buff1size * 2);

	u32 TDA;

	if (buff2end > 0)
	{
		// second branch needs copied:
		// It starts at the beginning of memory and moves forward to buff2end

		// endpoint cache should be irrelevant, since it's almost certainly dynamic
		// memory below 0x2800 (registers and such)
		const u32 start = ActiveTSA;
		TDA = buff1end;

		DMAPtr += TDA - ActiveTSA;
		ReadSize -= TDA - ActiveTSA;
		ActiveTSA = 0;
		// Emulation Grayarea: Should addresses wrap around to zero, or wrap around to
		// 0x2800?  Hard to know for sure (almost no games depend on this)
		memcpy(GetMemPtr(0), DMAPtr, buff2end * 2);
		TDA = (buff2end) & 0xfffff;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		// Note: Because this buffer wraps, we use || instead of &&

		for (int i = 0; i < 2; i++)
		{
			// Start is exclusive and end is inclusive... maybe? The end is documented to be inclusive,
			// which suggests that memory access doesn't trigger interrupts, incrementing registers does
			// (which would mean that if TSA=IRQA an interrupt doesn't fire... I guess?)
			// Chaos Legion uses interrupt addresses set to the beginning of the two buffers in a double
			// buffer scheme and sets LSA of one of the voices to the start of the opposite buffer.
			// However it transfers to the same address right after setting IRQA, which by our previous
			// understanding would trigger the interrupt early causing it to switch buffers again immediately
			// and an interrupt never fires again, leaving the voices looping the same samples forever.

			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
				has_to_call_irq_dma[i] = true;
		}
	}
	else
	{
		// Buffer doesn't wrap/overflow!
		// Just set the TDA and check for an IRQ...

		TDA = buff1end;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > ActiveTSA && Cores[i].IRQA < TDA))
				has_to_call_irq_dma[i] = true;
		}
	}

	DMAPtr += TDA - ActiveTSA;
	ReadSize -= TDA - ActiveTSA;

	DMAICounter = (DMAICounter - ReadSize) * 4;

	if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)DMAICounter)
	{
		psxCounters[6].startCycle = psxRegs.cycle;
		psxCounters[6].deltaCycles = DMAICounter;

		psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
		psxNextStartCounter = psxRegs.cycle;
		if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
			psxNextDeltaCounter = psxCounters[6].deltaCycles;
	}

	ActiveTSA = TDA;
	ActiveTSA &= 0xfffff;
	TSA = ActiveTSA;
}

void V_Core::FinishDMAread()
{
	u32 buff1end = ActiveTSA + std::min(ReadSize, (u32)0x100 + std::abs(DMAICounter / 4));
	u32 buff2end = 0;

	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	if (DMAPtr == nullptr)
		DMAPtr = (u16*)&iopMem->Main[MADR & 0x1fffff];

	const u32 buff1size = (buff1end - ActiveTSA);
	memcpy(DMARPtr, GetMemPtr(ActiveTSA), buff1size * 2);
	// Note on TSA's position after our copy finishes:
	// IRQA should be measured by the end of the writepos+0x20.  But the TDA
	// should be written back at the precise endpoint of the xfer.
	u32 TDA;

	if (buff2end > 0)
	{
		const u32 start = ActiveTSA;
		TDA = buff1end;

		DMARPtr += TDA - ActiveTSA;
		ReadSize -= TDA - ActiveTSA;
		ActiveTSA = 0;

		// second branch needs cleared:
		// It starts at the beginning of memory and moves forward to buff2end
		memcpy(DMARPtr, GetMemPtr(0), buff2end * 2);

		TDA = (buff2end) & 0xfffff;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		// Note: Because this buffer wraps, we use || instead of &&

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
				has_to_call_irq_dma[i] = true;
		}
	}
	else
	{
		// Buffer doesn't wrap/overflow!
		// Just set the TDA and check for an IRQ...

		TDA = buff1end;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > ActiveTSA && Cores[i].IRQA < TDA))
				has_to_call_irq_dma[i] = true;
		}
	}

	DMARPtr += TDA - ActiveTSA;
	ReadSize -= TDA - ActiveTSA;

	// DMA Reads are done AFTER the delay, so to get the timing right we need to scheule one last DMA to catch IRQ's
	if (ReadSize)
		DMAICounter = std::min(ReadSize, (u32)0x100) * 4;
	else
		DMAICounter = 4;

	if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)DMAICounter)
	{
		psxCounters[6].startCycle = psxRegs.cycle;
		psxCounters[6].deltaCycles = DMAICounter;

		psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
		psxNextStartCounter = psxRegs.cycle;
		if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
			psxNextDeltaCounter = psxCounters[6].deltaCycles;
	}

	ActiveTSA = TDA;
	ActiveTSA &= 0xfffff;
	TSA = ActiveTSA;
}

void V_Core::DoDMAread(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	DMARPtr = pMem;
	ActiveTSA = TSA & 0xfffff;
	ReadSize = size;
	IsDMARead = true;
	LastClock = psxRegs.cycle;
	DMAICounter = std::min(ReadSize, (u32)0x100) * 4;

	Regs.STATX &= ~0x80;
	Regs.STATX |= 0x400;
	TADR = MADR + (size << 1);

	if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)DMAICounter)
	{
		psxCounters[6].startCycle  = psxRegs.cycle;
		psxCounters[6].deltaCycles = DMAICounter;

		psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
		psxNextStartCounter = psxRegs.cycle;
		if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
			psxNextDeltaCounter = psxCounters[6].deltaCycles;
	}
}

void V_Core::DoDMAwrite(u16* pMem, u32 size)
{
	DMAPtr = pMem;

	if (size < 2)
	{
		Regs.STATX &= ~0x80;
		DMAICounter = 1 * 4;
		LastClock = psxRegs.cycle;
		return;
	}

	ActiveTSA = TSA & 0xfffff;

	const bool adma_enable = ((AutoDMACtrl & (Index + 1)) == (Index + 1));

	if (adma_enable)
	{
		StartADMAWrite(pMem, size);
	}
	else
	{
		PlainDMAWrite(pMem, size);
		Regs.STATX &= ~0x80;
		Regs.STATX |= 0x400;
	}
}
