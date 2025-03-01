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

#define _PC_	// disables MIPS opcode macros.

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"
#include "IopHw.h"

_sif sif1;

static bool sif1_dma_stall = false;

static __fi void Sif1Init(void)
{
	sif1.ee.cycles  = 0;
	sif1.iop.cycles = 0;
}

// Write from the EE to Fifo.
static __fi bool WriteEEtoFifo(void)
{
	// There's some data ready to transfer into the fifo..
	const int writeSize = std::min((s32)sif1ch.qwc, (FIFO_SIF_W - sif1.fifo.size) >> 2);
	tDMA_TAG *ptag      = sif1ch.getAddr(sif1ch.madr, DMAC_SIF1, false);
	if (!ptag)
		return false;

	if ((writeSize << 2) > 0)
		sif1.fifo.write((u32*)ptag, writeSize << 2);

	sif1ch.madr += writeSize << 4;
	hwDmacSrcTadrInc(sif1ch);
	sif1.ee.cycles += writeSize;		// fixme : BIAS is factored in above
	sif1ch.qwc -= writeSize;

	return true;
}

// Read from the fifo and write to IOP
static __fi void WriteFifoToIOP(void)
{
	// If we're reading something, continue to do so.

	const int readSize = std::min(sif1.iop.counter, sif1.fifo.size);

	if (readSize > 0)
		sif1.fifo.read((u32*)&iopMem->Main[hw_dma10.madr & 0x1fffff], readSize);
	psxCpu->Clear(hw_dma10.madr, readSize);
	hw_dma10.madr    += readSize << 2;
	sif1.iop.cycles  += readSize >> 2;		// fixme: should be >> 4
	sif1.iop.counter -= readSize;
}

// Get a tag and process it.
static __fi bool ProcessEETag(void)
{
	// Chain mode
	// Process DMA tag at sif1ch.tadr
	tDMA_TAG *ptag = sif1ch.DMAtransfer(sif1ch.tadr, DMAC_SIF1);
	if (!ptag)
		return false;

	if (sif1ch.chcr.TTE)
		sif1.fifo.write((u32*)ptag + 2, 2);

	sif1ch.madr = ptag[1]._u32;

	sif1.ee.end = hwDmacSrcChain(sif1ch, ptag->ID);

	if (sif1ch.chcr.TIE && ptag->IRQ)
		sif1.ee.end = true;

	return true;
}

// Write fifo to data, and put it in IOP.
static __fi void SIFIOPReadTag(void)
{
	tDMA_TAG sif1dat_tmp;
	// Read a tag.
	sif1.fifo.read((u32*)&sif1.iop.data, 4);

	// Only use the first 24 bits.
	hw_dma10.madr = sif1data & 0xffffff;

	//Maximum transfer amount 1mb-16 also masking out top part which is a "Mode" cache stuff, we don't care :)
	sif1.iop.counter = sif1words & 0xFFFFC;

	sif1dat_tmp._u32 = sif1data;
	if (sif1dat_tmp.IRQ  || (sif1dat_tmp.ID & 4)) sif1.iop.end = true;
}

// Stop processing EE, and signal an interrupt.
static __fi void EndEE(void)
{
	sif1.ee.end = false;
	sif1.ee.busy = false;

	// Voodoocycles : Okami wants around 100 cycles when booting up
	// Other games reach like 50k cycles here, but the EE will long have given up by then and just retry.
	// (Cause of double interrupts on the EE)
	if (sif1.ee.cycles == 0)
		sif1.ee.cycles = 1;

	cpuRegs.dmastall &= ~(1 << DMAC_SIF1);
	CPU_INT(DMAC_SIF1, sif1.ee.cycles * BIAS);
}

// Stop processing IOP, and signal an interrupt.
static __fi void EndIOP(void)
{
	sif1data = 0;
	sif1.iop.end = false;
	sif1.iop.busy = false;

	//Fixme ( voodoocycles ):
	//The *24 are needed for ecco the dolphin (CDVD hangs) and silver surfer (Pad not detected)
	//Greater than *35 break rebooting when trying to play Tekken5 arcade history
	//Total cycles over 1024 makes SIF too slow to keep up the sound stream in so3...
	if (sif1.iop.cycles == 0)
		sif1.iop.cycles = 1;
	// IOP is 1/8th the clock rate of the EE and psxcycles is in words (not quadwords)
	PSX_INT(IopEvt_SIF1, /*std::min((*/sif1.iop.cycles/* * 26*//*), 1024)*/);
}

// Handle the EE transfer.
static __fi void HandleEETransfer(void)
{
	if(!sif1ch.chcr.STR)
	{
		sif1.ee.end  = false;
		sif1.ee.busy = false;
		return;
	}

	// If there's no more to transfer.
	if (sif1ch.qwc <= 0)
	{
		// If NORMAL mode or end of CHAIN then stop DMA.
		if ((sif1ch.chcr.MOD == NORMAL_MODE) || sif1.ee.end)
		{
			EndEE();
		}
		else
		{
			if (!ProcessEETag()) return;
		}
	}
	else
	{
		if (dmacRegs.ctrl.STD == STD_SIF1)
		{
			if ((sif1ch.chcr.MOD == NORMAL_MODE) || ((sif1ch.chcr.TAG >> 28) & 0x7) == TAG_REFS)
			{
				const int writeSize = std::min((s32)sif1ch.qwc, (FIFO_SIF_W - sif1.fifo.size) >> 2);
				if ((sif1ch.madr + (writeSize * 16)) > dmacRegs.stadr.ADDR)
				{
					hwDmacIrq(DMAC_STALL_SIS);
					sif1_dma_stall    = true;
					cpuRegs.dmastall |= 1 << DMAC_SIF1;
					return;
				}
			}
		}
		if ((FIFO_SIF_W - sif1.fifo.size) > 0)
		{
			WriteEEtoFifo();
		}
	}
}

// Handle the IOP transfer.
static __fi void HandleIOPTransfer(void)
{
	if (sif1.iop.counter > 0)
	{
		if (sif1.fifo.size > 0)
			WriteFifoToIOP();
	}

	if (sif1.iop.counter <= 0)
	{
		if (sif1.iop.end)
		{
			EndIOP();
		}
		else if (sif1.fifo.size >= 4)
		{
			SIFIOPReadTag();
		}
	}
}

// Transfer EE to IOP, putting data in the fifo as an intermediate step.
__fi void SIF1Dma(void)
{
	int BusyCheck = 0;

	if (sif1_dma_stall)
	{
		const int writeSize = std::min((s32)sif1ch.qwc, (FIFO_SIF_W - sif1.fifo.size) >> 2);
		if ((sif1ch.madr + (writeSize * 16)) > dmacRegs.stadr.ADDR)
			return;
	}

	sif1_dma_stall = false;
	Sif1Init();

	do
	{
		//I realise this is very hacky in a way but its an easy way of checking if both are doing something
		BusyCheck = 0;

		if (sif1.ee.busy && !sif1_dma_stall)
		{
			if((FIFO_SIF_W - sif1.fifo.size) > 0 || (sif1.ee.end && sif1ch.qwc == 0))
			{
				BusyCheck++;
				HandleEETransfer();
			}
		}

		if (sif1.iop.busy)
		{
			if(sif1.fifo.size >= 4 || (sif1.iop.end && sif1.iop.counter == 0))
			{
				BusyCheck++;
				HandleIOPTransfer();
			}
		}

	} while (BusyCheck > 0);

	psHu32(SBUS_F240) &= ~0x40;
	psHu32(SBUS_F240) &= ~0x4000;
}

__fi void  sif1Interrupt(void)
{
	HW_DMA10_CHCR &= ~0x01000000; //reset TR flag
	psxDmaInterrupt2(3);
}

__fi void EEsif1Interrupt(void)
{
	hwDmacIrq(DMAC_SIF1);
	sif1ch.chcr.STR = false;
}

// Do almost exactly the same thing as psxDma10 in IopDma.cpp.
// Main difference is this checks for IOP, where psxDma10 checks for ee.
__fi void dmaSIF1(void)
{
	psHu32(SBUS_F240) |= 0x4000;
	sif1.ee.busy       = true;

	cpuRegs.dmastall  &= ~(1 << DMAC_SIF1);
	// Okay, this here is needed currently (r3644).
	// FFX battles in the thunder plains map die otherwise, Phantasy Star 4 as well
	// These 2 games could be made playable again by increasing the time the EE or the IOP run,
	// showing that this is very timing sensible.
	// Doing this DMA unfortunately brings back an old warning in Legend of Legaia though, but it still works.

	//Updated 23/08/2011: The hangs are caused by the EE suspending SIF1 DMA and restarting it when in the middle
	//of processing a "REFE" tag, so the hangs can be solved by forcing the ee.end to be false
	// (as it should always be at the beginning of a DMA).  using "if IOP is busy" flags breaks Tom Clancy Rainbow Six.
	// Legend of Legaia doesn't throw a warning either :)
	sif1.ee.end        = false;

	if (sif1ch.chcr.MOD == CHAIN_MODE && sif1ch.qwc > 0)
	{
		tDMA_TAG tmp;
		tmp._u32 = sif1ch.chcr._u32;
		if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && vif1ch.chcr.TIE))
			sif1.ee.end = true;
	}

	SIF1Dma();

}
