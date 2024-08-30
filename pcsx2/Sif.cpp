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

#define _PC_	/* disables MIPS opcode macros. */

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"
#include "IopHw.h"

#define sif0data sif0.iop.data.data
#define sif1data sif1.iop.data.data
#define sif2data sif2.iop.data.data

#define sif0words sif0.iop.data.words
#define sif1words sif1.iop.data.words

_sif sif0;
_sif sif1;
_sif sif2;
static bool sif1_dma_stall = false;

// Junk data writing
// 
// If there is not enough data produced from the IOP, it will always use the previous full quad word to
// fill in the missing data.
// One thing to note, when the IOP transfers the EE tag, it transfers a whole QW of data, which will include
// the EE Tag and the next IOP tag, since the EE reads 1QW of data for DMA tags.
//
// So the data used will be as follows:
// Less than 1QW = Junk data is made up of the EE tag + address (64 bits) and the following IOP tag (64 bits).
// More than 1QW = Junk data is made up of the last complete QW of data that was transferred in this packet.
//
// Data is always offset in to the junk by the amount the IOP actually transferred, so if it sent 2 words
// it will read words 3 and 4 out of the junk to fill the space.
//
// PS2 test results:
//
// Example of less than 1QW being sent with the only data being set being 0x69
//
//	addr 0x1500a0 value 0x69        <-- actual data (junk behind this would be the EE tag)
//	addr 0x1500a4 value 0x1500a0    <-- EE address
//	addr 0x1500a8 value 0x8001a170  <-- following IOP tag
//	addr 0x1500ac value 0x10        <-- following IOP tag word count
//
// Example of more than 1QW being sent with the data going from 0x20 to 0x25
//
//	addr 0x150080 value 0x21 <-- start of previously completed QW
//	addr 0x150084 value 0x22
//	addr 0x150088 value 0x23
//	addr 0x15008c value 0x24 <-- end of previously completed QW
//	addr 0x150090 value 0x25 <-- end of recorded data
//	addr 0x150094 value 0x22 <-- from position 2 of the previously completed quadword
//	addr 0x150098 value 0x23 <-- from position 3 of the previously completed quadword
//	addr 0x15009c value 0x24 <-- from position 4 of the previously completed quadword
static void writeJunk(sifFifo *fifo, int words)
{
	/* Get the start position of the previously completed whole QW.
	 * Position is in word (32bit) units. */
	const int transferredWords = 4 - words;
	const int prevQWPos = (fifo->writePos - (4 + transferredWords)) & (FIFO_SIF_W - 1);

	/* Read the old data in to our junk array in case of wrapping. */
	const int rP0 = std::min((FIFO_SIF_W - prevQWPos), 4);
	const int rP1 = 4 - rP0;
	memcpy(&fifo->junk[0], &fifo->data[prevQWPos], rP0 << 2);
	memcpy(&fifo->junk[rP0], &fifo->data[0], rP1 << 2);

	/* Fill the missing words to fill the QW. */
	const int wP0 = std::min((FIFO_SIF_W - fifo->writePos), words);
	const int wP1 = words - wP0;

	memcpy(&fifo->data[fifo->writePos], &fifo->junk[4-wP0], wP0 << 2);
	memcpy(&fifo->data[0], &fifo->junk[wP0], wP1 << 2);

	fifo->writePos  = (fifo->writePos + words) & (FIFO_SIF_W - 1);
	fifo->size     += words;
}

static void sifWrite(sifFifo *fifo, u32 *from, int words)
{
	const int wP0 = std::min((FIFO_SIF_W - fifo->writePos), words);
	const int wP1 = words - wP0;

	memcpy(&fifo->data[fifo->writePos], from, wP0 << 2);
	memcpy(&fifo->data[0], &from[wP0], wP1 << 2);

	fifo->writePos = (fifo->writePos + words) & (FIFO_SIF_W - 1);
	fifo->size += words;
}

static void sifRead(sifFifo *fifo, u32 *to, int words)
{
	const int wP0 = std::min((FIFO_SIF_W - fifo->readPos), words);
	const int wP1 = words - wP0;

	memcpy(to, &fifo->data[fifo->readPos], wP0 << 2);
	memcpy(&to[wP0], &fifo->data[0], wP1 << 2);

	fifo->readPos = (fifo->readPos + words) & (FIFO_SIF_W - 1);
	fifo->size -= words;
}


/* Handle the EE transfer. */
static __fi void SIF0HandleEETransfer(void)
{
	if(!sif0ch.chcr.STR)
	{
		sif0.ee.end = false;
		sif0.ee.busy = false;
		return;
	}

	if (sif0ch.qwc <= 0)
	{
		if ((sif0ch.chcr.MOD == NORMAL_MODE) || sif0.ee.end)
		{
			/* Stop transferring EE, and signal an interrupt. */
			sif0.ee.end  = false;
			sif0.ee.busy = false;
			if (sif0.ee.cycles == 0)
				sif0.ee.cycles = 1;
			CPU_SET_DMASTALL(DMAC_SIF0, false);
			CPU_INT(DMAC_SIF0, sif0.ee.cycles*BIAS);
		}
		else if (sif0.fifo.size >= 4) /* Read a tag */
		{
			/* Read FIFO into an EE tag, transfer it to sif0ch
			 * and process it. */
			alignas(16) static u32 tag[4];
			tDMA_TAG& ptag(*(tDMA_TAG*)tag);

			sifRead(&sif0.fifo, (u32*)&tag[0], 4); /* Tag */

			sif0ch.unsafeTransfer(&ptag);
			sif0ch.madr = tag[1];

			if (sif0ch.chcr.TIE && ptag.IRQ)
				sif0.ee.end = true;

			if (ptag.ID == TAG_END)
				sif0.ee.end = true;
			else if (ptag.ID == TAG_CNTS && dmacRegs.ctrl.STS == STS_SIF0)
				dmacRegs.stadr.ADDR = sif0ch.madr; /* STS == SIF0 - Initial Value */
		}
	}

	if (sif0ch.qwc > 0) /* If we're writing something, continue to do so. */
	{
		/* Write from FIFO to EE. */
		if (sif0.fifo.size >= 4)
		{
			tDMA_TAG *ptag     = sif0ch.getAddr(sif0ch.madr, DMAC_SIF0, true);
			if (ptag)
			{
				const int readSize = std::min((s32)sif0ch.qwc, sif0.fifo.size >> 2);
				if ((readSize << 2) > 0)
					sifRead(&sif0.fifo, (u32*)ptag, readSize << 2);

				sif0ch.madr    += readSize << 4;
				sif0.ee.cycles += readSize; /* FIXME : BIAS is factored in above */
				sif0ch.qwc     -= readSize;

				if (sif0ch.qwc == 0 && dmacRegs.ctrl.STS == STS_SIF0)
				{
					if ((sif0ch.chcr.MOD == NORMAL_MODE) || ((sif0ch.chcr.TAG >> 28) & 0x7) == TAG_CNTS)
						dmacRegs.stadr.ADDR = sif0ch.madr;
				}
			}
		}
	}
}

/* Handle the IOP transfer.
 * Note: Test any changes in this function against Grandia III.
 * What currently happens is this:
 * SIF0 DMA start...
 * SIF + 4 = 4 (pos=4)
 * SIF0 IOP Tag: madr=19870, tadr=179cc, counter=8 (00000008_80019870)
 * SIF - 4 = 0 (pos=4)
 * SIF0 EE read tag: 90000002 935c0 0 0
 * SIF0 EE dest chain tag madr:000935C0 qwc:0002 id:1 irq:1(000935C0_90000002)
 * Write FIFO to EE: ----------- 0 of 8
 * SIF - 0 = 0 (pos=4)
 * Write IOP to FIFO: +++++++++++ 8 of 8
 * SIF + 8 = 8 (pos=12)
 * Write FIFO to EE: ----------- 8 of 8
 * SIF - 8 = 0 (pos=12)
 * Sif0: End IOP
 * Sif0: End EE
 * SIF0 DMA end...
 *
 * What happens if (sif0.iop.counter > 0) is handled first is this
 *
 * SIF0 DMA start...
 * ...
 * SIF + 8 = 8 (pos=12)
 * Sif0: End IOP
 * Write FIFO to EE: ----------- 8 of 8
 * SIF - 8 = 0 (pos=12)
 * SIF0 DMA end...
 */

static __fi void SIF0HandleIOPTransfer(void)
{
	if (sif0.iop.counter <= 0) /* If there's no more to transfer */
	{
		if (sif0.iop.end)
		{
			/* Stop transferring IOP, and signal an interrupt. */
			sif0data      = 0;
			sif0.iop.end  = false;
			sif0.iop.busy = false;

			if (sif0.iop.cycles == 0)
				sif0.iop.cycles = 1;

			/* Hack alert
			 * Parappa The Rapper hates SIF0 taking the length of time 
			 * it should do on bigger packets.
			 *
			 * I logged it and couldn't work out why, changing any 
			 * other SIF timing (EE or IOP) seems to have no effect. */
			if (sif0.iop.cycles > 1000)
				sif0.iop.cycles >>= 1; /* 2 word per cycle */

			PSX_INT(IopEvt_SIF0, sif0.iop.cycles);
		}
		else
		{
			/* Read FIFO into an IOP tag, and transfer it to hw_dma9.
			 * And presumably process it. */
			tDMA_TAG sif0dat_tmp;
			/* Process DMA tag at hw_dma9.tadr */
			sif0.iop.data       = *(sifData *)iopPhysMem(hw_dma9.tadr);
			sif0.iop.data.words = sif0.iop.data.words;

			/* Send the EE's side of the DMAtag.  The tag is only 64 bits, 
			 * with the upper 64 bits being the next IOP
			 * The tag is only 64 bits, with the upper 64 bits
			 * ignored by the EE, however required for alignment 
			 * and used as junk data in small packets. */
			sifWrite(&sif0.fifo, (u32*)iopPhysMem(hw_dma9.tadr + 8), 4);

			/* I know we just sent 1QW, because of the size of the EE read, 
			 * but only 64bits was valid.
			 * So we advance by 64bits after the EE tag to get the next IOP tag. */
			hw_dma9.tadr += 16;

			/* We're only copying the first 24 bits.
			 * Bits 30 and 31 (checked below) are Stop/IRQ bits. */
			hw_dma9.madr = sif0data & 0xFFFFFF;
			/* Maximum transfer amount 1mb-16 also masking out top 
			 * part which is a "Mode" cache stuff, we don't care */
			sif0.iop.counter = sif0words & 0xFFFFF;

			/* Save the number of words we need to write 
			 * to make up 1QW from this packet. (See "Junk data writing" in Sif.h) */
			sif0.iop.writeJunk = (sif0.iop.counter & 0x3) ? (4 - sif0.iop.counter & 0x3) : 0;
			/* IOP tags have an IRQ bit and an End of Transfer bit: */
			sif0dat_tmp._u32 = sif0data;
			if (sif0dat_tmp.IRQ  || (sif0dat_tmp.ID & 4))
				sif0.iop.end = true;
		}
	}
	else
	{
		/* Write IOP to FIFO. */
		if ((FIFO_SIF_W - sif0.fifo.size) > 0)
		{
			/* There's some data ready to transfer into the fifo.. */
			const int writeSize = std::min(sif0.iop.counter, FIFO_SIF_W - sif0.fifo.size);
			if (writeSize > 0)
				sifWrite(&sif0.fifo, (u32*)iopPhysMem(hw_dma9.madr), writeSize);
			hw_dma9.madr       += writeSize << 2;
			/* IOP is 1/8th the clock rate of the EE and 
			 * psxcycles is in words (not quadwords). */
			sif0.iop.cycles    += writeSize; /* 1 word per cycle */
			sif0.iop.counter   -= writeSize;
		}
	}
}

/* Get a tag and process it. */
static __fi bool SIF1ProcessEETag(void)
{
	/* Chain mode
	 * Process DMA tag at sif1ch.tadr */
	tDMA_TAG *ptag = sif1ch.DMAtransfer(sif1ch.tadr, DMAC_SIF1);
	if (!ptag)
		return false;

	if (sif1ch.chcr.TTE)
		sifWrite(&sif1.fifo, (u32*)ptag + 2, 2);

	sif1ch.madr = ptag[1]._u32;

	sif1.ee.end = hwDmacSrcChain(sif1ch, ptag->ID);

	if (sif1ch.chcr.TIE && ptag->IRQ)
		sif1.ee.end = true;

	return true;
}

/* Handle the EE transfer. */
static __fi void SIF1HandleEETransfer(void)
{
	if(!sif1ch.chcr.STR)
	{
		sif1.ee.end  = false;
		sif1.ee.busy = false;
		return;
	}

	/* If there's no more to transfer. */
	if (sif1ch.qwc <= 0)
	{
		/* If NORMAL mode or end of CHAIN then stop DMA. */
		if ((sif1ch.chcr.MOD == NORMAL_MODE) || sif1.ee.end)
		{
			/* Stop processing EE, and signal an interrupt. */

			sif1.ee.end  = false;
			sif1.ee.busy = false;

			/* Voodoocycles : Okami wants around 100 cycles when booting up
			 * Other games reach like 50k cycles here, but the EE will 
			 * long have given up by then and just retry.
			 * (Cause of double interrupts on the EE) */
			if (sif1.ee.cycles == 0)
				sif1.ee.cycles = 1;

			CPU_SET_DMASTALL(DMAC_SIF1, false);
			CPU_INT(DMAC_SIF1, sif1.ee.cycles*BIAS);
		}
		else
		{
			if (!SIF1ProcessEETag())
				return;
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
					sif1_dma_stall = true;
					CPU_SET_DMASTALL(DMAC_SIF1, true);
					return;
				}
			}
		}
		if ((FIFO_SIF_W - sif1.fifo.size) > 0)
		{
			/* Write from the EE to the FIFO */
			/* There's some data ready to transfer into the fifo.. */
			tDMA_TAG *ptag = sif1ch.getAddr(sif1ch.madr, DMAC_SIF1, false);
			if (ptag)
			{
				const int writeSize = std::min((s32)sif1ch.qwc, (FIFO_SIF_W - sif1.fifo.size) >> 2);
				if ((writeSize << 2) > 0)
					sifWrite(&sif1.fifo, (u32*)ptag, writeSize << 2);

				sif1ch.madr    += writeSize << 4;
				hwDmacSrcTadrInc(sif1ch);
				sif1.ee.cycles += writeSize; /* FIXME : BIAS is factored in above */
				sif1ch.qwc     -= writeSize;
			}
		}
	}
}

/* Handle the IOP transfer. */
static __fi void SIF1HandleIOPTransfer(void)
{
	if (sif1.iop.counter > 0)
	{
		if (sif1.fifo.size > 0)
		{
			/* Read from the fifo and write to IOP */
			/* If we're reading something, continue to do so. */
			const int readSize = std::min(sif1.iop.counter, sif1.fifo.size);
			if (readSize > 0)
				sifRead(&sif1.fifo, (u32*)iopPhysMem(hw_dma10.madr), readSize);
			psxCpu->Clear(hw_dma10.madr, readSize);
			hw_dma10.madr    += readSize << 2;
			sif1.iop.cycles  += readSize >> 2; /* fixme: should be >> 4 */
			sif1.iop.counter -= readSize;
		}
	}

	if (sif1.iop.counter <= 0)
	{
		if (sif1.iop.end)
		{
			/* Stop processing IOP, and signal an interrupt. */
			sif1data = 0;
			sif1.iop.end = false;
			sif1.iop.busy = false;

			/* Fixme ( voodoocycles ):
			 * The *24 are needed for Ecco The Dolphin (CDVD hangs) 
			 * and Silver Surfer (Pad not detected)
			 *
			 * Greater than *35 break rebooting when trying to 
			 * play Tekken 5 Arcade History
			 * Total cycles over 1024 makes SIF too slow to keep 
			 * up the sound stream in Star Ocean 3... */
			if (sif1.iop.cycles == 0)
				sif1.iop.cycles = 1;
			/* IOP is 1/8th the clock rate of the EE and psxcycles is in words (not quadwords) */
			PSX_INT(IopEvt_SIF1, sif1.iop.cycles);
		}
		else if (sif1.fifo.size >= 4)
		{
			/* Write FIFO to data, and put it in IOP. */
			tDMA_TAG sif1dat_tmp;
			/* Read a tag. */
			sifRead(&sif1.fifo, (u32*)&sif1.iop.data, 4);

			/* Only use the first 24 bits. */
			hw_dma10.madr = sif1data & 0xffffff;

			/* Maximum transfer amount 1mb-16 also masking out 
			 * top part which is a "Mode" cache stuff, we don't care */
			sif1.iop.counter = sif1words & 0xFFFFC;

			sif1dat_tmp._u32 = sif1data;
			if (sif1dat_tmp.IRQ  || (sif1dat_tmp.ID & 4))
				sif1.iop.end = true;
		}
	}
}

/* Transfer EE to IOP, putting data in the fifo as an intermediate step. */
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
	sif1.ee.cycles  = 0;
	sif1.iop.cycles = 0;

	do
	{
		/* I realise this is very hacky in a way, but its an easy way 
		 * of checking if both are doing something */
		BusyCheck = 0;

		if (sif1.ee.busy && !sif1_dma_stall)
		{
			if((FIFO_SIF_W - sif1.fifo.size) > 0 || (sif1.ee.end && sif1ch.qwc == 0))
			{
				BusyCheck++;
				SIF1HandleEETransfer();
			}
		}

		if (sif1.iop.busy)
		{
			if(sif1.fifo.size >= 4 || (sif1.iop.end && sif1.iop.counter == 0))
			{
				BusyCheck++;
				SIF1HandleIOPTransfer();
			}
		}

	} while (BusyCheck > 0);

	psHu32(SBUS_F240) &= ~0x40;
	psHu32(SBUS_F240) &= ~0x4000;
}

__fi void ReadFifoSingleWord(void)
{
	u32 ptag[4];

	sifRead(&sif2.fifo, (u32*)&ptag[0], 1);
	psHu32(0x1000f3e0) = ptag[0];
	if (sif2.fifo.size == 0) psxHu32(0x1000f300) |= 0x4000000;
	if (sif2.iop.busy && sif2.fifo.size <= 8) SIF2Dma();
}

/* Write IOP to FIFO. */
static __fi void SIF2WriteIOPtoFifo(void)
{
	/* There's some data ready to transfer into the FIFO.. */
	const int writeSize = std::min(sif2.iop.counter, FIFO_SIF_W - sif2.fifo.size);

	if (writeSize > 0)
		sifWrite(&sif2.fifo, (u32*)iopPhysMem(hw_dma2.madr), writeSize);
	hw_dma2.madr += writeSize << 2;

	/* IOP is 1/8th the clock rate of the EE and psxcycles is in words (not quadwords). */
	sif2.iop.cycles  += (writeSize >> 2);		/* FIXME : should be >> 4 */
	sif2.iop.counter -= writeSize;
	if (sif2.iop.counter == 0)
		hw_dma2.madr = sif2data & 0xffffff;
	if (sif2.fifo.size > 0)
		psxHu32(0x1000f300) &= ~0x4000000;
}

/* Handle the EE transfer. */
static __fi void SIF2HandleEETransfer(void)
{
	if (!sif2dma.chcr.STR)
	{
		sif2.ee.end = false;
		sif2.ee.busy = false;
		return;
	}

	if (sif2dma.qwc <= 0)
	{
		/* Stop transferring EE, and signal an interrupt. */
		if ((sif2dma.chcr.MOD == NORMAL_MODE) || sif2.ee.end)
		{
			sif2.ee.end  = false;
			sif2.ee.busy = false;
			if (sif2.ee.cycles == 0)
				sif2.ee.cycles = 1;

			CPU_INT(DMAC_SIF2, sif2.ee.cycles*BIAS);
		}
		else if (sif2.fifo.size >= 4) /* Read a tag */
		{
			/* Read FIFO into an EE tag, transfer it to SIF2DMA
			 * and process it. */
			alignas(16) static u32 tag[4];
			tDMA_TAG& ptag(*(tDMA_TAG*)tag);

			sifRead(&sif2.fifo, (u32*)&tag[0], 4); /* Tag */

			sif2dma.unsafeTransfer(&ptag);
			sif2dma.madr = tag[1];

			if (sif2dma.chcr.TIE && ptag.IRQ)
				sif2.ee.end = true;

			if (ptag.ID == TAG_END)
				sif2.ee.end = true;
		}
	}

	if (sif2dma.qwc > 0) /* If we're writing something, continue to do so. */
	{
		/* Write from Fifo to EE. */
		if (sif2.fifo.size > 0)
		{
			const int readSize = std::min((s32)sif2dma.qwc, sif2.fifo.size >> 2);
			tDMA_TAG *ptag     = sif2dma.getAddr(sif2dma.madr, DMAC_SIF2, true);
			if (ptag == NULL)
				return;

			if ((readSize << 2) > 0)
				sifRead(&sif2.fifo, (u32*)ptag, readSize << 2);

			sif2dma.madr   += readSize << 4;
			sif2.ee.cycles += readSize;	/* FIXME : BIAS is factored in above */
			sif2dma.qwc    -= readSize;
		}
	}
}

// Handle the IOP transfer.
// Note: Test any changes in this function against Grandia III.
// What currently happens is this:
// SIF2 DMA start...
// SIF + 4 = 4 (pos=4)
// SIF2 IOP Tag: madr=19870, tadr=179cc, counter=8 (00000008_80019870)
// SIF - 4 = 0 (pos=4)
// SIF2 EE read tag: 90000002 935c0 0 0
// SIF2 EE dest chain tag madr:000935C0 qwc:0002 id:1 irq:1(000935C0_90000002)
// Write Fifo to EE: ----------- 0 of 8
// SIF - 0 = 0 (pos=4)
// Write IOP to Fifo: +++++++++++ 8 of 8
// SIF + 8 = 8 (pos=12)
// Write Fifo to EE: ----------- 8 of 8
// SIF - 8 = 0 (pos=12)
// Sif0: End IOP
// Sif0: End EE
// SIF2 DMA end...

// What happens if (sif2.iop.counter > 0) is handled first is this

// SIF2 DMA start...
// ...
// SIF + 8 = 8 (pos=12)
// Sif0: End IOP
// Write Fifo to EE: ----------- 8 of 8
// SIF - 8 = 0 (pos=12)
// SIF2 DMA end...

static __fi void SIF2HandleIOPTransfer(void)
{
	if (sif2.iop.counter <= 0) /* If there's no more to transfer */
	{
		if (sif2.iop.end)
		{
			/* Stop transferring IOP, and signal an interrupt. */
			sif2data      = 0;
			sif2.iop.busy = false;

			if (sif2.iop.cycles == 0)
				sif2.iop.cycles = 1;
			/* IOP is 1/8th the clock rate of the EE and psxcycles is in words (not quadwords)
			 * So when we're all done, the equation looks like thus: */
			PSX_INT(IopEvt_SIF2, sif2.iop.cycles);
		}
		else
		{
			/* Read FIFO into an IOP tag, and transfer it to hw_dma9.
			 * And presumably process it. */
			/* Process DMA tag at hw_dma9.tadr */
			sif2.iop.data.words = sif2.iop.data.data >> 24; /* Round up to nearest 4. */
			/* send the EE's side of the DMAtag.  The tag is only 64 bits, with the upper 64 bits
			 * ignored by the EE.
			 *
			 * We're only copying the first 24 bits.  Bits 30 and 31 (checked below) are Stop/IRQ bits. */
			sif2.iop.counter    =  (HW_DMA2_BCR_H16 * HW_DMA2_BCR_L16); /* makes it do more stuff?? */
			sif2.iop.end        = true;
		}
	}
	else
	{
		/* Write IOP to Fifo. */
		if ((FIFO_SIF_W - sif2.fifo.size) > 0)
		{
			SIF2WriteIOPtoFifo();
		}
	}
}

/* Transfer IOP to EE, putting data in the FIFO as an intermediate step. */
__fi void SIF2Dma(void)
{
	int BusyCheck   = 0;
	sif2.ee.cycles  = 0;
	sif2.iop.cycles = 0;

	do
	{
		/* I realise this is very hacky in a way but its an easy way of checking if both are doing something */
		BusyCheck = 0;

		if (sif2.iop.busy)
		{
			if ((FIFO_SIF_W - sif2.fifo.size) > 0 || (sif2.iop.end && sif2.iop.counter == 0))
			{
				BusyCheck++;
				SIF2HandleIOPTransfer();
			}
		}
		if (sif2.ee.busy)
		{
			if (sif2.fifo.size >= 4 || (sif2.ee.end && sif2dma.qwc == 0))
			{
				BusyCheck++;
				SIF2HandleEETransfer();
			}
		}
	} while (BusyCheck > 0); /* Substituting (sif2.ee.busy || sif2.iop.busy) breaks things. */

	psHu32(SBUS_F240) &= ~0x80;
	psHu32(SBUS_F240) &= ~0x8000;
}

__fi void  sif2Interrupt(void)
{
	if (!sif2.iop.end || sif2.iop.counter > 0)
	{
		SIF2Dma();
		return;
	}

	HW_DMA2_CHCR &= ~0x01000000;
	psxDmaInterrupt2(2);
}

__fi void dmaSIF2(void)
{
	psHu32(SBUS_F240) |= 0x8000;
	sif2.ee.busy = true;

	/* Okay, this here is needed currently (r3644).
	 * FFX battles in the thunder plains map die otherwise, Phantasy Star 4 as well
	 * These 2 games could be made playable again by increasing the time the EE or the IOP run,
	 * showing that this is very timing sensible.
	 * Doing this DMA unfortunately brings back an old warning in Legend of Legaia though, 
	 * but it still works.
	 *
	 * Updated 23/08/2011: The hangs are caused by the EE suspending SIF1 DMA and 
	 * restarting it when in the middle of processing a "REFE" tag, so the hangs 
	 * can be solved by forcing the ee.end to be false (as it should always be at 
	 * the beginning of a DMA).  using "if IOP is busy" flags breaks Tom Clancy Rainbow Six.
	 * Legend of Legaia doesn't throw a warning either */

	SIF2Dma();
}

/* Transfer IOP to EE, putting data in the FIFO as an intermediate step. */
__fi void SIF0Dma(void)
{
	int BusyCheck   = 0;
	sif0.ee.cycles  = 0;
	sif0.iop.cycles = 0;

	do
	{
		/* I realise this is very hacky in a way but its an easy way 
		 * of checking if both are doing something */
		BusyCheck = 0;

		if (               sif0.iop.counter == 0 
				&& sif0.iop.writeJunk 
				&& (FIFO_SIF_W - sif0.fifo.size) >= sif0.iop.writeJunk)
		{
			if (sif0.iop.writeJunk > 0)
				writeJunk(&sif0.fifo, sif0.iop.writeJunk);
			sif0.iop.writeJunk = 0;
		}

		if (sif0.iop.busy)
		{
			if((FIFO_SIF_W - sif0.fifo.size) > 0 || (sif0.iop.end && sif0.iop.counter == 0))
			{
				BusyCheck++;
				SIF0HandleIOPTransfer();
			}
		}
		if (sif0.ee.busy)
		{
			if(sif0.fifo.size >= 4 || (sif0.ee.end && sif0ch.qwc == 0))
			{
				BusyCheck++;
				SIF0HandleEETransfer();
			}
		}
	} while (BusyCheck > 0); /* Substituting (sif0.ee.busy || sif0.iop.busy) breaks things. */

	psHu32(SBUS_F240) &= ~0x20;
	psHu32(SBUS_F240) &= ~0x2000;
}

__fi void  sif0Interrupt(void)
{
	HW_DMA9_CHCR &= ~0x01000000;
	psxDmaInterrupt2(2);
}

__fi void  EEsif0Interrupt(void)
{
	hwDmacIrq(DMAC_SIF0);
	sif0ch.chcr.STR = false;
}

__fi void dmaSIF0(void)
{
	psHu32(SBUS_F240) |= 0x2000;
	sif0.ee.busy = true;

	/* Okay, this here is needed currently (r3644).
	 * FFX battles in the thunder plains map die otherwise, Phantasy Star 4 as well
	 * These 2 games could be made playable again by increasing the time the EE or the IOP run,
	 * showing that this is very timing sensible.
	 * Doing this DMA unfortunately brings back an old warning in Legend of Legaia though, 
	 * but it still works.
	 *
	 * Updated 23/08/2011: The hangs are caused by the EE suspending SIF1 DMA and 
	 * restarting it when in the middle of processing a "REFE" tag, so the hangs can be 
	 * solved by forcing the ee.end to be false (as it should always be at the beginning of a DMA).
	 *
	 * Using "if IOP is busy" flags breaks Tom Clancy Rainbow Six.
	 * Legend of Legaia doesn't throw a warning either. */
	sif0.ee.end = false;
	CPU_SET_DMASTALL(DMAC_SIF0, false);
	SIF0Dma();
}

__fi void sif1Interrupt(void)
{
	HW_DMA10_CHCR &= ~0x01000000; /* Reset TR flag */
	psxDmaInterrupt2(3);
}

__fi void EEsif1Interrupt(void)
{
	hwDmacIrq(DMAC_SIF1);
	sif1ch.chcr.STR = false;
}

/* Do almost exactly the same thing as psxDma10 in IopDma.cpp.
 * Main difference is this checks for IOP, where psxDma10 checks for EE. */
__fi void dmaSIF1(void)
{
	psHu32(SBUS_F240) |= 0x4000;
	sif1.ee.busy = true;

	CPU_SET_DMASTALL(DMAC_SIF1, false);
	/* Okay, this here is needed currently (r3644).
	 * Final Fantasy X battles in the Thunder Plains map die otherwise, 
	 * Phantasy Star 4 as well
	 *
	 * These 2 games could be made playable again by increasing 
	 * the time the EE or the IOP run, showing that this is very timing sensible.
	 * Doing this DMA unfortunately brings back an old warning 
	 * in Legend of Legaia though, but it still works. */

	/* Updated 23/08/2011: The hangs are caused by the EE suspending SIF1 DMA and 
	 * restarting it when in the middle of processing a "REFE" tag, so the hangs 
	 * can be solved by forcing the ee.end to be false (as it should always be at 
	 * the beginning of a DMA).  using "if IOP is busy" flags breaks Tom Clancy Rainbow Six.
	 * Legend of Legaia doesn't throw a warning either */
	sif1.ee.end = false;

	if (sif1ch.chcr.MOD == CHAIN_MODE && sif1ch.qwc > 0)
	{
		tDMA_TAG tmp;
		tmp._u32 = sif1ch.chcr._u32;
		if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && vif1ch.chcr.TIE))
			sif1.ee.end = true;
	}

	SIF1Dma();
}

void sifReset(void)
{
	memset(&sif0, 0, sizeof(sif0));
	memset(&sif1, 0, sizeof(sif1));
}

bool SaveStateBase::sifFreeze()
{
	if (!(FreezeTag("SIFdma")))
		return false;

	Freeze(sif0);
	Freeze(sif1);

	return IsOkay();
}
