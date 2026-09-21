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

#include <stdlib.h>
#include "Global.h"
#include "Dma.h"
#include "spu2.h"

#include "../R3000A.h"
#include "../IopHw.h"
#include "../Config.h"

/* mode: 0 = split stereo; 1 = do not split stereo */
void V_Core_AutoDMAReadBuffer(V_Core *c, int mode)
{
	u32 spos = c->InputPosWrite & 0x100; // Starting position passed by TSA
	bool leftbuffer = !(c->InputPosWrite & 0x80);

	if (c->InputPosWrite == 0xFFFF) // Data request not made yet
		return;

	c->AutoDMACtrl &= 0x3;

	int size = pcsx2_min_u(c->InputDataLeft, (u32)0x200);
	if (!leftbuffer)
		size = pcsx2_min_i(size, 0x100);
	// HACKFIX!! DMAPtr can be invalid after a savestate load, so the savestate just forces it
	// to NULL and we ignore it here.  (used to work in old VM editions of PCSX2 with fixed
	// addressing, but new PCSX2s have dynamic memory addressing).
	if (c->DMAPtr == NULL)
	{
		c->DMAPtr = (u16*)&iopMem->Main[MADR(c) & 0x1fffff];
		c->InputDataProgress = 0;
	}

	if (mode)
	{
		if (c->DMAPtr != NULL)
			memcpy(GetMemPtr(0x2000 + (c->Index << 10) + spos), c->DMAPtr + c->InputDataProgress, size);
		MADR(c) += size;
		c->InputDataLeft -= 0x200;
		c->InputDataProgress += 0x200;
	}
	else
	{
		while (size)
		{
			if (!leftbuffer)
				spos |= 0x200;
			else
				spos &= ~0x200;

			if (c->DMAPtr != NULL)
				memcpy(GetMemPtr(0x2000 + (c->Index << 10) + spos), c->DMAPtr + c->InputDataProgress, 0x200);
			c->InputDataTransferred += 0x200;
			c->InputDataLeft -= 0x100;
			c->InputDataProgress += 0x100;
			leftbuffer = !leftbuffer;
			size -= 0x100;
			c->InputPosWrite += 0x80;
		}
	}
	if (!(c->InputPosWrite & 0x80))
		c->InputPosWrite = 0xFFFF;
}

void V_Core_StartADMAWrite(V_Core *c, u16* pMem, u32 sz)
{
	int size = sz;

	TimeUpdate(psxRegs.cycle);

	c->InputDataProgress = 0;
	TADR(c) = MADR(c) + (size << 1);
	if ((c->AutoDMACtrl & (c->Index + 1)) == 0)
	{
		c->ActiveTSA = 0x2000 + (c->Index << 10);
		c->DMAICounter = size * 4;
		c->LastClock = psxRegs.cycle;
	}
	else if (size >= 256)
	{
		c->InputDataLeft = size;
		if (c->InputPosWrite != 0xFFFF)
			V_Core_AutoDMAReadBuffer(c, 0);
		c->AdmaInProgress = 1;
	}
	else
	{
		c->InputDataLeft = 0;
		c->DMAICounter = size * 4;
		c->LastClock = psxRegs.cycle;
	}
}

void V_Core_PlainDMAWrite(V_Core *c, u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	c->ReadSize = size;
	c->IsDMARead = false;
	c->DMAICounter = 0;
	c->LastClock = psxRegs.cycle;
	c->Regs.STATX &= ~0x80;
	c->Regs.STATX |= 0x400;
	TADR(c) = MADR(c) + (size << 1);

	V_Core_FinishDMAwrite(c);
}

void V_Core_FinishDMAwrite(V_Core *c)
{
	if (!c->DMAPtr)
		c->DMAPtr = (u16*)&iopMem->Main[MADR(c) & 0x1fffff];

	c->DMAICounter = c->ReadSize;

	u32 buff1end = c->ActiveTSA + pcsx2_min_u(c->ReadSize, (u32)0x100 + abs(c->DMAICounter / 4));
	u32 buff2end = 0;

	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	const int cacheIdxStart = c->ActiveTSA / pcm_WordsPerBlock;
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

	const u32 buff1size = (buff1end - c->ActiveTSA);
	memcpy(GetMemPtr(c->ActiveTSA), c->DMAPtr, buff1size * 2);

	u32 TDA;

	if (buff2end > 0)
	{
		// second branch needs copied:
		// It starts at the beginning of memory and moves forward to buff2end

		// endpoint cache should be irrelevant, since it's almost certainly dynamic
		// memory below 0x2800 (registers and such)
		const u32 start = c->ActiveTSA;
		TDA = buff1end;

		c->DMAPtr += TDA - c->ActiveTSA;
		c->ReadSize -= TDA - c->ActiveTSA;
		c->ActiveTSA = 0;
		// Emulation Grayarea: Should addresses wrap around to zero, or wrap around to
		// 0x2800?  Hard to know for sure (almost no games depend on this)
		memcpy(GetMemPtr(0), c->DMAPtr, buff2end * 2);
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
				{ has_to_call_irq_dma[i] = true; }
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
			if (Cores[i].IRQEnable && (Cores[i].IRQA > c->ActiveTSA && Cores[i].IRQA < TDA))
				{ has_to_call_irq_dma[i] = true; }
		}
	}

	c->DMAPtr += TDA - c->ActiveTSA;
	c->ReadSize -= TDA - c->ActiveTSA;

	c->DMAICounter = (c->DMAICounter - c->ReadSize) * 4;

	if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)c->DMAICounter)
	{
		psxCounters[6].startCycle = psxRegs.cycle;
		psxCounters[6].deltaCycles = c->DMAICounter;

		psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
		psxNextStartCounter = psxRegs.cycle;
		if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
			psxNextDeltaCounter = psxCounters[6].deltaCycles;
	}

	c->ActiveTSA = TDA;
	c->ActiveTSA &= 0xfffff;
	c->TSA = c->ActiveTSA;
}

void V_Core_FinishDMAread(V_Core *c)
{
	u32 buff1end = c->ActiveTSA + pcsx2_min_u(c->ReadSize, (u32)0x100 + abs(c->DMAICounter / 4));
	u32 buff2end = 0;

	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	if (c->DMAPtr == NULL)
		c->DMAPtr = (u16*)&iopMem->Main[MADR(c) & 0x1fffff];

	const u32 buff1size = (buff1end - c->ActiveTSA);
	memcpy(c->DMARPtr, GetMemPtr(c->ActiveTSA), buff1size * 2);
	// Note on TSA's position after our copy finishes:
	// IRQA should be measured by the end of the writepos+0x20.  But the TDA
	// should be written back at the precise endpoint of the xfer.
	u32 TDA;

	if (buff2end > 0)
	{
		const u32 start = c->ActiveTSA;
		TDA = buff1end;

		c->DMARPtr += TDA - c->ActiveTSA;
		c->ReadSize -= TDA - c->ActiveTSA;
		c->ActiveTSA = 0;

		// second branch needs cleared:
		// It starts at the beginning of memory and moves forward to buff2end
		memcpy(c->DMARPtr, GetMemPtr(0), buff2end * 2);

		TDA = (buff2end) & 0xfffff;

		// Flag interrupt?  If IRQA occurs between start and dest, flag it.
		// Important: Test both core IRQ settings for either DMA!
		// Note: Because this buffer wraps, we use || instead of &&

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
				{ has_to_call_irq_dma[i] = true; }
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
			if (Cores[i].IRQEnable && (Cores[i].IRQA > c->ActiveTSA && Cores[i].IRQA < TDA))
				{ has_to_call_irq_dma[i] = true; }
		}
	}

	c->DMARPtr += TDA - c->ActiveTSA;
	c->ReadSize -= TDA - c->ActiveTSA;

	// DMA Reads are done AFTER the delay, so to get the timing right we need to scheule one last DMA to catch IRQ's
	if (c->ReadSize)
		c->DMAICounter = pcsx2_min_u(c->ReadSize, (u32)0x100) * 4;
	else
		c->DMAICounter = 4;

	if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)c->DMAICounter)
	{
		psxCounters[6].startCycle = psxRegs.cycle;
		psxCounters[6].deltaCycles = c->DMAICounter;

		psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
		psxNextStartCounter = psxRegs.cycle;
		if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
			psxNextDeltaCounter = psxCounters[6].deltaCycles;
	}

	c->ActiveTSA = TDA;
	c->ActiveTSA &= 0xfffff;
	c->TSA = c->ActiveTSA;
}

void V_Core_DoDMAread(V_Core *c, u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	c->DMARPtr = pMem;
	c->ActiveTSA = c->TSA & 0xfffff;
	c->ReadSize = size;
	c->IsDMARead = true;
	c->LastClock = psxRegs.cycle;
	c->DMAICounter = pcsx2_min_u(c->ReadSize, (u32)0x100) * 4;

	c->Regs.STATX &= ~0x80;
	c->Regs.STATX |= 0x400;
	TADR(c) = MADR(c) + (size << 1);

	if (((psxCounters[6].startCycle + psxCounters[6].deltaCycles) - psxRegs.cycle) > (u32)c->DMAICounter)
	{
		psxCounters[6].startCycle  = psxRegs.cycle;
		psxCounters[6].deltaCycles = c->DMAICounter;

		psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
		psxNextStartCounter = psxRegs.cycle;
		if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
			psxNextDeltaCounter = psxCounters[6].deltaCycles;
	}
}

void V_Core_DoDMAwrite(V_Core *c, u16* pMem, u32 size)
{
	c->DMAPtr = pMem;

	if (size < 2)
	{
		c->Regs.STATX &= ~0x80;
		c->DMAICounter = 1 * 4;
		c->LastClock = psxRegs.cycle;
		return;
	}

	c->ActiveTSA = c->TSA & 0xfffff;

	const bool adma_enable = ((c->AutoDMACtrl & (c->Index + 1)) == (c->Index + 1));

	if (adma_enable)
	{
		V_Core_StartADMAWrite(c, pMem, size);
	}
	else
	{
		V_Core_PlainDMAWrite(c, pMem, size);
		c->Regs.STATX &= ~0x80;
		c->Regs.STATX |= 0x400;
	}
}
