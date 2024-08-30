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
#include "Hardware.h"
#include "MTVU.h"

#include "IPUdma.h"
#include "HwInternal.h"
#include "Gif_Unit.h"
#include "R3000A.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"

#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

/* Believe it or not, making this const can generate compiler warnings in gcc. */
static __fi int ChannelNumber(u32 addr)
{
    switch (addr)
    {
        case D0_CHCR: return 0;
        case D1_CHCR: return 1;
        case D2_CHCR: return 2;
        case D3_CHCR: return 3;
        case D4_CHCR: return 4;
        case D5_CHCR: return 5;
        case D6_CHCR: return 6;
        case D7_CHCR: return 7;
        case D8_CHCR: return 8;
        case D9_CHCR: return 9;
		default:
		      break;
    }
    return 51; /* some value */
}

bool DMACh::transfer(tDMA_TAG* ptag)
{
	if (ptag == NULL)  /* Is ptag empty? */
	{
		dmacRegs.stat.BEIS = true;
		return false;
	}
	chcr.TAG = ptag[0]._u32 >> 16;
	qwc      = ptag[0].QWC;
	return true;
}

void DMACh::unsafeTransfer(tDMA_TAG* ptag)
{
    chcr.TAG = ptag[0]._u32 >> 16;
    qwc      = ptag[0].QWC;
}

tDMA_TAG *DMACh::getAddr(u32 addr, u32 num, bool write)
{
	tDMA_TAG *ptr = dmaGetAddr(addr, write);
	if (ptr == NULL)
	{
		dmacRegs.stat.BEIS  = true;
		dmacRegs.stat._u32 |= (1 << num);
		chcr.STR = false;
	}

	return ptr;
}

tDMA_TAG *DMACh::DMAtransfer(u32 addr, u32 num)
{
	tDMA_TAG *tag = getAddr(addr, num, false);

	if (!tag) return NULL;

	chcr.TAG = tag[0]._u32 >> 16;
	qwc      = tag[0].QWC;
	return tag;
}

/* Note: DMA addresses are guaranteed to be aligned to 16 bytes (128 bits) */
__ri tDMA_TAG *dmaGetAddr(u32 addr, bool write)
{
	tDMA_TAG tmp;
	tmp._u32 = addr;
	if (tmp.SPR) return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];

	/* FIXME: Why??? DMA uses physical addresses */
	addr &= 0x1ffffff0;

	if (addr < Ps2MemSize::MainRam)
		return (tDMA_TAG*)&eeMem->Main[addr];
	else if (addr < 0x10000000)
		return (tDMA_TAG*)(write ? eeMem->ZeroWrite : eeMem->ZeroRead);
	else if (addr < 0x10004000)
		/* Secret scratchpad address for DMA = end of maximum main memory? */
		return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];
	return NULL;
}


/* Returns true if the DMA is enabled and executed successfully.  Returns false if execution
 * was blocked (DMAE or master DMA enabler). */
static bool QuickDmaExec( void (*func)(), u32 mem)
{
	DMACh& reg = (DMACh&)psHu32(mem);

	if (reg.chcr.STR && dmacRegs.ctrl.DMAE && !psHu8(DMAC_ENABLER+2))
	{
		func();
		return true;
	}

	return false;
}

static tDMAC_QUEUE QueuedDMA;

static void StartQueuedDMA(void)
{
	if (QueuedDMA.VIF0) { QueuedDMA.VIF0 = !QuickDmaExec(dmaVIF0, D0_CHCR); }
	if (QueuedDMA.VIF1) { QueuedDMA.VIF1 = !QuickDmaExec(dmaVIF1, D1_CHCR); }
	if (QueuedDMA.GIF ) { QueuedDMA.GIF  = !QuickDmaExec(dmaGIF , D2_CHCR); }
	if (QueuedDMA.IPU0) { QueuedDMA.IPU0 = !QuickDmaExec(dmaIPU0, D3_CHCR); }
	if (QueuedDMA.IPU1) { QueuedDMA.IPU1 = !QuickDmaExec(dmaIPU1, D4_CHCR); }
	if (QueuedDMA.SIF0) { QueuedDMA.SIF0 = !QuickDmaExec(dmaSIF0, D5_CHCR); }
	if (QueuedDMA.SIF1) { QueuedDMA.SIF1 = !QuickDmaExec(dmaSIF1, D6_CHCR); }
	if (QueuedDMA.SIF2) { QueuedDMA.SIF2 = !QuickDmaExec(dmaSIF2, D7_CHCR); }
	if (QueuedDMA.SPR0) { QueuedDMA.SPR0 = !QuickDmaExec(dmaSPR0, D8_CHCR); }
	if (QueuedDMA.SPR1) { QueuedDMA.SPR1 = !QuickDmaExec(dmaSPR1, D9_CHCR); }
}

static __ri void DmaExec( void (*func)(), u32 mem, u32 value )
{
	tDMA_CHCR chcr;
	DMACh& reg = (DMACh&)psHu32(mem);
	chcr._u32  = value;

	/* It's invalid for the hardware to write a DMA while it is active, not without Suspending the DMAC */
	if (reg.chcr.STR)
	{
		/*As the manual states "Fields other than STR can only be written to when the DMA is stopped"
		 *Also "The DMA may not stop properly just by writing 0 to STR"
		 *So the presumption is that STR can be written to (ala force stop the DMA) but nothing else
		 *If the developer wishes to alter any of the other fields, it must be done AFTER the STR has been written,
		 *it will not work before or during this event. */
		if(chcr.STR == 0)
		{
			const uint channel = ChannelNumber(mem);

			reg.chcr.STR = 0;
			/* We need to clear any existing DMA loops that are in progress else they will continue! */

			if(channel == 1)
			{
				cpuRegs.interrupt &= ~(1 << 10);
				cpuRegs.dmastall  &= ~(1 << 10);
				QueuedDMA._u16 &= ~(1 << 10); /* Clear any queued DMA requests for this channel */
			}
			else if(channel == 2)
			{
				cpuRegs.interrupt &= ~(1 << 11);
				cpuRegs.dmastall  &= ~(1 << 11);
				QueuedDMA._u16 &= ~(1 << 11); /* Clear any queued DMA requests for this channel */
			}

			cpuRegs.interrupt &= ~(1 << channel);
			cpuRegs.dmastall  &= ~(1 << channel);
			QueuedDMA._u16 &= ~(1 << channel); /* Clear any queued DMA requests for this channel */
		}
		return;
	}

	reg.chcr._u32 = value;

	/* Final Fantasy XII sets the DMA Mode to 3 which doesn't exist. 
	 * On some channels (like SPR) this will break logic completely. so lets assume they mean chain. */
	if (reg.chcr.MOD == 0x3)
		reg.chcr.MOD = 0x1;

	/* As tested on hardware, if NORMAL mode is started with 0 QWC it will actually transfer 1 QWC then underflows and transfer another 0xFFFF QWC's
	 * The easiest way to handle this is to just say 0x10000 QWC */
	if (reg.chcr.STR && !reg.chcr.MOD && reg.qwc == 0)
		reg.qwc = 0x10000;

	if (reg.chcr.STR && dmacRegs.ctrl.DMAE && !psHu8(DMAC_ENABLER+2))
		func();
	else if(reg.chcr.STR)
		QueuedDMA._u16 |= (1 << ChannelNumber(mem)); /* Queue the DMA up to be started then the DMA's are Enabled and or the Suspend is lifted */
}

template< uint page >
static __fi u32 dmacRead32( u32 mem )
{
	/* Fixme: OPH hack. Toggle the flag on GIF_STAT access. (rama) */
	if ((CHECK_OPHFLAGHACK) && (page << 12) == (mem & (0xf << 12)) && (mem == GIF_STAT))
	{
		static unsigned counter = 1;
		if (++counter == 8)
			counter = 2;
		/* Set OPH and APATH from counter, cycling paths and alternating OPH */
		return (gifRegs.stat._u32 & ~(7 << 9)) | ((counter & 1) ? (counter << 9) : 0);
	}

	return psHu32(mem);
}

/* Returns TRUE if the caller should do writeback of the register to eeHw; false if the
 * register has no writeback, or if the writeback is handled internally. */
template<uint page> __fi static bool dmacWrite32( u32 mem, mem32_t& value )
{
	/* DMA Writes are invalid to everything except the STR on CHCR when it is busy
	 * However this isn't completely confirmed and this might vary depending on if
	 * using chain or normal modes, DMA's may be handled internally.
	 * Metal Saga requires the QWC during IPU_FROM to be written but not MADR
	 * similar happens with Mana Khemia.
	 * In other cases such as Pilot Down Behind Enemy Lines, it seems to expect the DMA
	 * to have finished before it writes the new information, otherwise the game breaks.
	 */
	if (CHECK_DMABUSYHACK && (mem & 0xf0) && mem >= 0x10008000 && mem <= 0x1000E000)
	{
		if ((psHu32(mem & ~0xff) & 0x100) && dmacRegs.ctrl.DMAE && !psHu8(DMAC_ENABLER + 2))
		{
			while (psHu32(mem & ~0xff) & 0x100)
			{
				switch ((mem >> 8) & 0xFF)
				{
					case 0x80: /* VIF0 */
						vif0Interrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_VIF0);
						break;
					case 0x90: /* VIF1 */
						if (vif1Regs.stat.VEW)
						{
							vu1Finish(false);
							vif1VUFinish();
						}
						else
							vif1Interrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_VIF1);
						break;
					case 0xA0: /* GIF */
						gifInterrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_GIF);
						break;
					case 0xB0: /* IPUFROM */
						/* fallthrough */
					case 0xB4: /* IPUTO */
						if ((mem & 0xff) == 0x20)
							goto allow_write; /* I'm so sorry */
						return false;
					case 0xD0: /* SPRFROM */
						SPRFROMinterrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_FROM_SPR);
						break;
					case 0xD4: /* SPRTO */
						SPRTOinterrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_TO_SPR);
						break;
					default:
						return false;
				}
			}
		}
		allow_write:;
	}

	switch(mem) {

		case (D0_QWC): /* dma0 - vif0 */
		case (D1_QWC): /* dma1 - vif1 */
		case (D2_QWC): /* dma2 - gif */
		case (D3_QWC): /* dma3 - fromIPU */
		case (D4_QWC): /* dma4 - toIPU */
		case (D5_QWC): /* dma5 - sif0 */
		case (D6_QWC): /* dma6 - sif1 */
		case (D7_QWC): /* dma7 - sif2 */
		case (D8_QWC): /* dma8 - fromSPR */
		case (D9_QWC): /* dma9 - toSPR */
			psHu32(mem) = (u16)value;
			return false;

		case (D0_CHCR): /* dma0 - vif0 */
			DmaExec(dmaVIF0, mem, value);
			return false;

		case (D1_CHCR): /* dma1 - vif1 - chcr */
			DmaExec(dmaVIF1, mem, value);
			return false;

		case (D2_CHCR): /* dma2 - gif */
			DmaExec(dmaGIF, mem, value);
			return false;

		case (D3_CHCR): /* dma3 - fromIPU */
			DmaExec(dmaIPU0, mem, value);
			return false;

		case (D4_CHCR): /* dma4 - toIPU */
			DmaExec(dmaIPU1, mem, value);
			return false;

		case (D5_CHCR): /* dma5 - sif0 */
			DmaExec(dmaSIF0, mem, value);
			return false;

		case (D6_CHCR): /* dma6 - sif1 */
			DmaExec(dmaSIF1, mem, value);
			return false;

		case (D7_CHCR): /* dma7 - sif2 */
			DmaExec(dmaSIF2, mem, value);
			return false;

		case (D8_CHCR): /* dma8 - fromSPR */
			DmaExec(dmaSPR0, mem, value);
			return false;

		case (D9_CHCR): /* dma9 - toSPR */
			DmaExec(dmaSPR1, mem, value);
			return false;

		case (fromSPR_MADR):
		case (toSPR_MADR):
			/* SPR bit is fixed at 0 for this channel */
			psHu32(mem) = value & 0x7FFFFFFF;
			return false;

		case (fromSPR_SADR):
		case (toSPR_SADR):
			/* Address must be QW aligned and fit in the 16K range of SPR */
			psHu32(mem) = value & 0x3FF0;
			return false;

		case (DMAC_CTRL):
		{
			u32 oldvalue = psHu32(mem);

			psHu32(mem) = value;
			/* Check for DMAS that were started while the DMAC was disabled */
			if (((oldvalue & 0x1) == 0) && ((value & 0x1) == 1))
			{
				if (QueuedDMA._u16 != 0) StartQueuedDMA();
			}
			return false;
		}

		/* Midway are a bunch of idiots, writing to E100 (reserved) instead of E010
		 * Which causes a CPCOND0 to fail. */
		case (DMAC_FAKESTAT):
		case (DMAC_STAT):
			/* lower 16 bits: clear on 1
			 * upper 16 bits: reverse on 1 */

			psHu16(0xe010) &= ~(value & 0xffff);
			psHu16(0xe012) ^= (u16)(value >> 16);

			cpuTestDMACInts();
			return false;

		case (DMAC_ENABLEW):
		{
			u32 oldvalue = psHu8(DMAC_ENABLEW + 2);
			psHu32(DMAC_ENABLEW) = value;
			psHu32(DMAC_ENABLER) = value;
			if (((oldvalue & 0x1) == 1) && (((value >> 16) & 0x1) == 0))
			{
				if (QueuedDMA._u16 != 0) StartQueuedDMA();
			}
			return false;
		}
		default:
			break;
	}

	return true;
}

template u32 dmacRead32<0x03>( u32 mem );

template bool dmacWrite32<0x00>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x01>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x02>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x03>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x04>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x05>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x06>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x07>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x08>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x09>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0a>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0b>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0c>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0d>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0e>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0f>( u32 mem, mem32_t& value );

#include "IopHw.h"
#include "IopMem.h"
#include "IopPgpuGif.h"
const int rdram_devices = 2;	/* put 8 for TOOL and 2 for PS2 and PSX */
int rdram_sdevid = 0;

/* Make sure framelimiter options are in sync with GS capabilities. */
static void gsReset(void)
{
	MTGS::ResetGS(true);
	gsVideoMode = GS_VideoMode::Uninitialized;
	memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	UpdateVSyncRate(true);
}

void hwReset(void)
{
	memset(eeHw, 0, sizeof(eeHw));

	psHu32(SBUS_F260) = 0x1D000060;

	/* i guess this is kinda a version, it's used by some bioses */
	psHu32(DMAC_ENABLEW) = 0x1201;
	psHu32(DMAC_ENABLER) = 0x1201;

	/* Sets SPU2 sample rate to PS2 standard (48KHz) whenever emulator is reset.
	 * For PSX mode sample rate setting, see HwWrite.cpp */
	SPU2::Reset(false);

	sifReset();

	gsReset();
	gifUnit.Reset();
	ipuReset();
	vif0Reset();
	vif1Reset();
	gif_fifo.init();
	rcntInit();
	USBreset();
}

__fi uint intcInterrupt(void)
{
	u32 intc_stat = psHu32(INTC_STAT);
	if (intc_stat == 0)
		return 0;
	if ((intc_stat & psHu32(INTC_MASK)) == 0)
		return 0;

	if (intc_stat & 0x2)
	{
		counters[0].hold = rcntRcount(0);
		counters[1].hold = rcntRcount(1);
	}

	return 0x400;
}

__fi uint dmacInterrupt(void)
{
	if( ((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) == 0 ) &&
		( psHu16(DMAC_STAT) & 0x8000) == 0 )
		return 0;

	if (!dmacRegs.ctrl.DMAE || psHu8(DMAC_ENABLER+2) == 1)
		return 0;

	return 0x800;
}

void hwIntcIrq(int n)
{
	psHu32(INTC_STAT) |= 1<<n;
	if(psHu32(INTC_MASK) & (1<<n))cpuTestINTCInts();
}

void hwDmacIrq(int n)
{
	psHu32(DMAC_STAT) |= 1<<n;
	if(psHu16(DMAC_STAT+2) & (1<<n))cpuTestDMACInts();
}

void FireMFIFOEmpty(void)
{
	hwDmacIrq(DMAC_MFIFO_EMPTY);

	if (dmacRegs.ctrl.MFD == MFD_VIF1) vif1Regs.stat.FQC = 0;
	if (dmacRegs.ctrl.MFD == MFD_GIF)  gifRegs.stat.FQC  = 0;
}

__ri bool hwDmacSrcChainWithStack(DMACh& dma, int id) {
	switch (id) {
		case TAG_REFE: 
			// Refe - Transfer Packet According to ADDR field
			dma.tadr += 16;
			//End Transfer
			return true;

		case TAG_CNT: 
			// CNT - Transfer QWC following the tag.
			// Set MADR to QW afer tag, and set TADR to QW following the data.
			dma.tadr += 16;
			dma.madr = dma.tadr;
			break;

		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
			{
				// Set MADR to QW following the tag, and set TADR to the address formerly in MADR.
				u32 temp = dma.madr;
				dma.madr = dma.tadr + 16;
				dma.tadr = temp;
			}
			break;
		case TAG_REF: // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
			//Set TADR to next tag
			dma.tadr += 16;
			break;

		case TAG_CALL: // Call - Transfer QWC following the tag, save succeeding tag
			{
				// Store the address in MADR in temp, and set MADR to the data following the tag.
				u32 temp = dma.madr;
				dma.madr = dma.tadr + 16;

				// Stash an address on the address stack pointer.
				switch(dma.chcr.ASP)
				{
					case 0: //Check if ASR0 is empty
						// Store the succeeding tag in asr0, and mark chcr as having 1 address.
						dma.asr0 = dma.madr + (dma.qwc << 4);
						dma.chcr.ASP++;
						break;

					case 1:
						// Store the succeeding tag in asr1, and mark chcr as having 2 addresses.
						dma.asr1 = dma.madr + (dma.qwc << 4);
						dma.chcr.ASP++;
						break;

					default:
						return true;
				}

				// Set TADR to the address from MADR we stored in temp.
				dma.tadr = temp;

				return false;
			}

		case TAG_RET: // Ret - Transfer QWC following the tag, load next tag
			//Set MADR to data following the tag.
			dma.madr = dma.tadr + 16;

			// Snag an address from the address stack pointer.
			switch(dma.chcr.ASP)
			{
				case 2:
					// Pull asr1 from the stack, give it to TADR, and decrease the # of addresses.
					dma.tadr = dma.asr1;
					dma.asr1 = 0;
					dma.chcr.ASP--;
					break;

				case 1:
					// Pull asr0 from the stack, give it to TADR, and decrease the # of addresses.
					dma.tadr = dma.asr0;
					dma.asr0 = 0;
					dma.chcr.ASP--;
					break;

				case 0:
					// There aren't any addresses to pull, so end the transfer.
				default:
					// If ASR1 and ASR0 are messed up, end the transfer.
					return true;
			}
			break;

		case TAG_END: // End - Transfer QWC following the tag
            //Set MADR to data following the tag, and end the transfer.
			dma.madr = dma.tadr + 16;
			//Don't Increment tadr; breaks Soul Calibur II and III
			return true;
	}

	return false;
}


/********TADR NOTES***********
From what i've gathered from testing tadr increment stuff (with CNT) is that we might not be 100% accurate in what
increments it and what doesnt. Previously we presumed REFE and END didn't increment the tag, but SIF and IPU never
liked this.

From what i've deduced, REFE does in fact increment, but END doesn't, after much testing, i've concluded this is how
we can standardize DMA chains, so i've modified the code to work like this.   The below function controls the increment
of the TADR along with the MADR on VIF, GIF and SPR1 when using the CNT tag, the others don't use it yet, but they
can probably be modified to do so now.

Reason for this:- Many games  (such as clock tower 3 and FFX Videos) watched the TADR to see when a transfer has finished,
so we need to simulate this wherever we can!  Even the FFX video gets corruption and tries to fire multiple DMA Kicks
if this doesnt happen, which was the reasoning for the hacked up SPR timing we had, that is no longer required.

-Refraction
******************************/

void hwDmacSrcTadrInc(DMACh& dma)
{
	//Don't touch it if in normal/interleave mode.
	if (dma.chcr.STR == 0) return;
	if (dma.chcr.MOD != 1) return;

	u16 tagid = (dma.chcr.TAG >> 12) & 0x7;

	if (tagid == TAG_CNT)
		dma.tadr = dma.madr;
}

bool hwDmacSrcChain(DMACh& dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE: // Refe - Transfer Packet According to ADDR field
			dma.tadr += 16;
			// End the transfer.
			return true;
		case TAG_CNT: // CNT - Transfer QWC following the tag.
			      // Set MADR to QW after the tag, and TADR to QW following the data.
			dma.madr = dma.tadr + 16;
			dma.tadr = dma.madr;
			break;
		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
			       // Set MADR to QW following the tag, and set TADR to the address formerly in MADR.
			temp = dma.madr;
			dma.madr = dma.tadr + 16;
			dma.tadr = temp;
			break;
		case TAG_REF:  // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
			       //Set TADR to next tag
			dma.tadr += 16;
			break;
		case TAG_END: // End - Transfer QWC following the tag
			      //Set MADR to data following the tag, and end the transfer.
			dma.madr = dma.tadr + 16;
			//Don't Increment tadr; breaks Soul Calibur II and III
			// Undefined Tag handling ends the DMA, maintaining the bad TADR and Tag in upper CHCR
			// Some games such as DT racer try to use RET tags on IPU, which it doesn't support
		default:
			return true;
	}

	return false;
}

template< uint page >
RETURNS_R128 hwRead128(u32 mem)
{
	alignas(16) mem128_t result;

	// FIFOs are the only "legal" 128 bit registers, so we Handle them first.
	// All other registers fall back on the 64-bit handler (and from there
	// all non-IPU reads fall back to the 32-bit handler).

	switch (page)
	{
		case 0x05:
			ReadFIFO_VIF1(&result);
			break;

		case 0x07:
			if (mem & 0x10)
				return r128_zero(); // IPUin is write-only
			ReadFIFO_IPUout(&result);
			break;

		case 0x04:
		case 0x06:
			// VIF0 and GIF are write-only.
			// [Ps2Confirm] Reads from these FIFOs (and IPUin) do one of the following:
			// return zero, leave contents of the dest register unchanged, or in some
			// indeterminate state.  The actual behavior probably isn't important.
			return r128_zero();
		case 0x0F:
			// TODO/FIXME: PSX mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				PGIFrQword((mem & 0x1FFFFFFF), &result);
				break;
			}

			// WARNING: this code is never executed anymore due to previous condition.
			// It requires investigation of what to do.
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 part0 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part1 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part2 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part3 = psHu32(0x1000f3E0);
					return r128_from_u32x4(part0, part1, part2, part3);
				}
			}
			break;

		default:
			return r128_from_u64_dup(hwRead64<page>(mem));
	}
	return r128_load(&result);
}

/* Internal hwRead32 which does not log reads, used by hwWrite8/16 to perform
 * read-modify-write operations. */
template< uint page, bool intcstathack >
static uint32_t _hwRead32(u32 mem)
{
	switch( page )
	{
		case 0x00:	return rcntRead32<0x00>( mem );
		case 0x01:	return rcntRead32<0x01>( mem );

		case 0x02:	return ipuRead32( mem );

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
					return vifRead32<1>(mem);
				return vifRead32<0>(mem);
			}
			return dmacRead32<0x03>( mem );

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			// No game is known to attempt such a thing (yay!), so probably nothing for us to
			// worry about.  Chances are, though, doing so is "legal" and yields some sort
			// of reproducible behavior.  Candidate for real hardware testing.
			// Current assumption: Reads 128 bits and discards the unused portion.

			r128 out128 = hwRead128<page>(mem & ~0x0f);
			return reinterpret_cast<u32*>(&out128)[(mem >> 2) & 0x3];
		}
		break;

		case 0x0f:
		{
			// INTC_STAT shortcut for heavy spinning.
			// Performance Note: Visual Studio handles this best if we just manually check for it here,
			// outside the context of the switch statement below.  This is likely fixed by PGO also,
			// but it's an easy enough conditional to account for anyways.

			if (mem == INTC_STAT)
			{
				// Disable INTC hack when in PS1 mode as it seems to break games.
				if (intcstathack && !(psxHu32(HW_ICFG) & (1 << 3)))
				{
					/* Sanity check: To protect from accidentally "rewinding" 
					 * the cyclecount on the few times nextBranchCycle 
					 * can be behind our current cycle. */
					s32 diff = cpuRegs.nextEventCycle - cpuRegs.cycle;
					if (diff > 0 && (cpuRegs.cycle - cpuRegs.lastEventCycle) > 8)
						cpuRegs.cycle = cpuRegs.nextEventCycle;
				}
				return psHu32(INTC_STAT);
			}

			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End))
				return PGIFr((mem & 0x1FFFFFFF));

			// WARNING: this code is never executed anymore due to previous condition.
			// It requires investigation of what to do.
			if ((mem & 0x1000ff00) == 0x1000f300)
			{
				int ret = 0;
				u32 sif2fifosize = std::min(sif2.fifo.size, 7);

				switch (mem & 0xf0)
				{
					case 0x00:
						return psxHu32(0x1f801814);
					case 0x80:
						ret = psHu32(mem) | (sif2fifosize << 16);
						if (sif2.fifo.size > 0)
							ret |= 0x80000000;
						return ret;
					case 0xc0:
						ReadFifoSingleWord();
						return psHu32(mem);
					case 0xe0:
						//ret = 0xa000e1ec;
						if (sif2.fifo.size > 0)
						{
							ReadFifoSingleWord();
							return psHu32(mem);
						}
						break;
				}
				return 0;


			}
			switch( mem )
			{
				case SIO_ISR:

				case 0x1000f410:
				case MCH_RICM:
					return 0;

				case SBUS_F240:
					return psHu32(SBUS_F240) | 0xF0000102;
				case SBUS_F260:
					return psHu32(SBUS_F260);
				case MCH_DRD:
					if( !((psHu32(MCH_RICM) >> 6) & 0xF) )
					{
						switch ((psHu32(MCH_RICM)>>16) & 0xFFF)
						{
							//MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5

							case 0x21: /* INIT */
								if (rdram_sdevid < rdram_devices)
								{
									rdram_sdevid++;
									return 0x1F;
								}
								return 0;

							case 0x23: /* CNFGA */
								return 0x0D0D;	//PVER=3 | MVER=16 | DBL=1 | REFBIT=5

							case 0x24: /* CNFGB */
								//0x0110 for PSX  SVER=0 | CORG=8(5x9x7) | SPT=1 | DEVTYP=0 | BYTE=0
								return 0x0090;	//SVER=0 | CORG=4(5x9x6) | SPT=1 | DEVTYP=0 | BYTE=0

							case 0x40://DEVID
								return psHu32(MCH_RICM) & 0x1F;	// =SDEV
						}
					}
					return 0;
			}
		}
		break;
		default: break;
	}
	//Hack for Transformers and Test Drive Unlimited to simulate filling the VIF FIFO
	//It actually stalls VIF a few QW before the end of the transfer, so we need to pretend its all gone
	//else itll take aaaaaaaaages to boot.
	if(mem == (D1_CHCR + 0x10) && CHECK_VIFFIFOHACK)
		return psHu32(mem) + (vif1ch.qwc * 16);

	return psHu32(mem);
}

template< uint page >
uint32_t hwRead32(u32 mem)
{
	return _hwRead32<page,false>(mem);
}

uint32_t hwRead32_page_0F_INTC_HACK(u32 mem)
{
	return _hwRead32<0x0f,true>(mem);
}

// --------------------------------------------------------------------------------------
//  hwRead8 / hwRead16 / hwRead64 / hwRead128
// --------------------------------------------------------------------------------------

template< uint page >
uint8_t hwRead8(u32 mem)
{
	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u8*)&ret32)[mem & 0x03];
}

template< uint page >
uint16_t hwRead16(u32 mem)
{
	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u16*)&ret32)[(mem>>1) & 0x01];
}

uint16_t hwRead16_page_0F_INTC_HACK(u32 mem)
{
	u32 ret32 = _hwRead32<0x0f, true>(mem & ~0x03);
	return ((u16*)&ret32)[(mem>>1) & 0x01];
}

template< uint page >
mem64_t hwRead64(u32 mem)
{
	switch (page)
	{
		case 0x02:
			return ipuRead64(mem);

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			// No game is known to attempt such a thing (yay!), so probably nothing for us to
			// worry about.  Chances are, though, doing so is "legal" and yields some sort
			// of reproducible behavior.  Candidate for real hardware testing.

			// Current assumption: Reads 128 bits and discards the unused portion.

			uint wordpart = (mem >> 3) & 0x1;
			r128 full = hwRead128<page>(mem & ~0x0f);
			return *(reinterpret_cast<u64*>(&full) + wordpart);
		}
		case 0x0F:
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 lo = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 hi = psHu32(0x1000f3E0);
					return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
				}
			}
		default: break;
	}

	return static_cast<u64>(_hwRead32<page, false>(mem));
}

// Shift the middle 8 bits (bits 4-12) into the lower 8 bits.
// This helps the compiler optimize the switch statement into a lookup table. :)

#define HELPSWITCH(m) (((m)>>4) & 0xff)

template< uint page >
void TAKES_R128 hwWrite128(u32 mem, r128 srcval)
{
	// FIFOs are the only "legal" 128 bit registers.  Handle them first.
	// all other registers fall back on the 64-bit handler (and from there
	// most of them fall back to the 32-bit handler).

	switch (page)
	{
		case 0x04:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF0(&usrcval);
			}
			break;

		case 0x05:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF1(&usrcval);
			}
			break;

		case 0x06:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_GIF(&usrcval);
			}
			break;

		case 0x07:
			if (mem & 0x10)
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_IPUin(&usrcval);
			}
			break;

		case 0x0F:
			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				PGIFwQword((mem & 0x1FFFFFFF), (void*)&usrcval);
				return;
			}
			// fallthrough
		default:
			hwWrite64<page>(mem, r128_to_u64(srcval));
			break;
	}
}


// --------------------------------------------------------------------------------------
//  hwWrite8 / hwWrite16 / hwWrite64 / hwWrite128
// --------------------------------------------------------------------------------------

template< uint page >
void hwWrite8(u32 mem, u8 value)
{
	if (mem == SIO_TXFIFO)
	{
		static bool included_newline = false;
		static char sio_buffer[1024];
		static int sio_count;

		if (value == '\r')
		{
			included_newline        = true;
			sio_buffer[sio_count++] = '\n';
		}
		else if (!included_newline || (value != '\n'))
		{
			included_newline        = false;
			sio_buffer[sio_count++] = value;
		}

		if ((sio_count == std::size(sio_buffer)-1) || (sio_count != 0 && sio_buffer[sio_count-1] == '\n'))
		{
			sio_buffer[sio_count]   = 0;
			sio_count               = 0;
		}
		return;
	}

	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			break;
		default:
			{
				u32 merged = _hwRead32<page,false>(mem & ~0x03);
				((u8*)&merged)[mem & 0x3] = value;

				hwWrite32<page>(mem & ~0x03, merged);
			}
			break;
	}

}


template<uint page>
void hwWrite32( u32 mem, u32 value )
{
	// Notes:
	// All unknown registers on the EE are "reserved" as discarded writes and indeterminate
	// reads.  Bus error is only generated for registers outside the first 16k of mapped
	// register space (which is handled by the VTLB mapping, so no need for checks here).

	switch (page)
	{
		case 0x00:
			if (!rcntWrite32<0x00>(mem, value))
				return;
			break;
		case 0x01:
			if (!rcntWrite32<0x01>(mem, value))
				return;
			break;
		case 0x02:
			if (!ipuWrite32(mem, value))
				return;
			break;
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Direct FIFO read/write behavior.  We need to create a test that writes
			// data to one of the FIFOs and determine the result.  I'm not quite sure offhand a good
			// way to do that --air
			// Current assumption is that 32-bit and 64-bit writes likely do 128-bit zero-filled
			// writes (upper 96 bits are 0, lower 32 bits are effective).
			u128 zerofill;
			zerofill._u32[0]                 = 0;
			zerofill._u32[1]                 = 0;
			zerofill.hi                      = 0;
			zerofill._u32[(mem >> 2) & 0x03] = value;

			hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
		}
		return;

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
				{
					if (!vifWrite32<1>(mem, value)) return;
				}
				else
				{
					if (!vifWrite32<0>(mem, value)) return;
				}
			}
			else switch(mem)
			{
				case (GIF_CTRL):
					// Not exactly sure what RST needs to do
					gifRegs.ctrl._u32 = value & 9;
					if (gifRegs.ctrl.RST)
						gifUnit.Reset(true); // Should it reset gsSIGNAL?
					gifRegs.stat.PSE = gifRegs.ctrl.PSE;
					return;
				case (GIF_MODE):
					gifRegs.mode._u32 = value;
					//Need to kickstart the GIF if the M3R mask comes off
					if (               gifRegs.stat.M3R == 1 
							&& gifRegs.mode.M3R == 0 
							&& (gifch.chcr.STR || gif_fifo.fifoSize))
					{
						CPU_INT(DMAC_GIF, 8);
					}


					gifRegs.stat.M3R = gifRegs.mode.M3R;
					gifRegs.stat.IMT = gifRegs.mode.IMT;
					return;
			}
			break;

		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0b:
		case 0x0c:
		case 0x0d:
		case 0x0e:
			if (!dmacWrite32<page>(mem, value))
				return;
			break;

		case 0x0f:
		{
			switch( HELPSWITCH(mem) )
			{
				case HELPSWITCH(INTC_STAT):
					psHu32(INTC_STAT) &= ~value;
					return;

				case HELPSWITCH(INTC_MASK):
					psHu32(INTC_MASK) ^= (u16)value;
					cpuTestINTCInts();
					return;

				case HELPSWITCH(SIO_TXFIFO):
				{
					u8* woot = (u8*)&value;
					// [Ps2Confirm] What happens when we write 32bit 
					// values to SIO_TXFIFO?
					// If it works like the IOP, then all 32bits are 
					// written to the FIFO in  order.  
					// PCSX2 up to this point simply ignored 
					// non-8bit writes to this port.
					hwWrite8<0x0f>(SIO_TXFIFO, woot[0]);
					hwWrite8<0x0f>(SIO_TXFIFO, woot[1]);
					hwWrite8<0x0f>(SIO_TXFIFO, woot[2]);
					hwWrite8<0x0f>(SIO_TXFIFO, woot[3]);
				}
				return;

				case HELPSWITCH(SBUS_F220):
					psHu32(mem) |= value;
					return;

				case HELPSWITCH(SBUS_F230):
					psHu32(mem) &= ~value;
					return;

				case HELPSWITCH(SBUS_F240) :
					if (value & (1 << 19))
					{
						u32 cycle = psxRegs.cycle;
						psxReset();
						PSXCLK =  33868800;
						SPU2::Reset(true);
						setPs1CDVDSpeed(cdvd.Speed);
						psxHu32(0x1f801450) = 0x8;
						psxHu32(0x1f801078) = 1;
						psxRegs.cycle = cycle;
					}
					if(!(value & 0x100))
						psHu32(mem) &= ~0x100;
					else
						psHu32(mem) |= 0x100;
					return;

				case HELPSWITCH(SBUS_F260):
					psHu32(mem) = value;
					return;

				case HELPSWITCH(MCH_RICM)://MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5
					if ((((value >> 16) & 0xFFF) == 0x21) && (((value >> 6) & 0xF) == 1) && (((psHu32(0xf440) >> 7) & 1) == 0))//INIT & SRP=0
						rdram_sdevid = 0;	// if SIO repeater is cleared, reset sdevid
					psHu32(mem) = value & ~0x80000000;	//kill the busy bit
				return;

				case HELPSWITCH(SBUS_F200):
				case HELPSWITCH(MCH_DRD):
					break;

				case HELPSWITCH(DMAC_ENABLEW):
					if (!dmacWrite32<0x0f>(DMAC_ENABLEW, value)) return;
					break;

				default:
					// TODO: psx add the real address in a sbus HELPSWITCH
					if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
						// Tharr be console spam here! Need to figure out how to print what mode
						PGIFw((mem & 0x1FFFFFFF), value);
						return;
					}
#if 0
				case HELPSWITCH(SIO_ISR):
				case HELPSWITCH(0x1000f410):
				// Mystery Regs!  No one knows!?
				// (unhandled so fall through to default)
#endif

			}
		}
		break;
	}

	psHu32(mem) = value;
}

template< uint page >
void hwWrite16(u32 mem, u16 value)
{
	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			break;
		default:
			{
				u32 merged = _hwRead32<page,false>(mem & ~0x03);
				((u16*)&merged)[(mem>>1) & 0x1] = value;

				hwWrite32<page>(mem & ~0x03, merged);
			}
			break;
	}

}

template<uint page>
void hwWrite64( u32 mem, u64 value )
{
	// * Only the IPU has true 64 bit registers.
	// * FIFOs have 128 bit registers that are probably zero-fill.
	// * All other registers likely disregard the upper 32-bits and simply act as normal
	//   32-bit writes.
	switch (page)
	{
		case 0x02:
			if (!ipuWrite64(mem, value))
				return;
			memcpy(&eeHw[(mem) & 0xffff], &value, sizeof(value));
			break;
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
			{
				u128 zerofill;
				zerofill._u32[0] = 0;
				zerofill._u32[1] = 0;
				zerofill.hi      = 0;
				zerofill._u64[(mem >> 3) & 0x01] = value;
				hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
			}
			break;

		default:
			// disregard everything except the lower 32 bits.
			// ... and skip the 64 bit writeback since the 32-bit one will suffice.
			hwWrite32<page>( mem, value );
			break;
	}

}

#define InstantizeHwWrite(pageidx) \
	template void hwWrite8<pageidx>(u32 mem, uint8_t value); \
	template void hwWrite16<pageidx>(u32 mem, uint16_t value); \
	template void hwWrite32<pageidx>(u32 mem, uint32_t value); \
	template void hwWrite64<pageidx>(u32 mem, uint64_t value); \
	template void TAKES_R128 hwWrite128<pageidx>(u32 mem, r128 srcval);

#define InstantizeHwRead(pageidx) \
	template uint8_t hwRead8<pageidx>(u32 mem); \
	template uint16_t hwRead16<pageidx>(u32 mem); \
	template uint32_t hwRead32<pageidx>(u32 mem); \
	template mem64_t hwRead64<pageidx>(u32 mem); \
	template RETURNS_R128 hwRead128<pageidx>(u32 mem); \
	template uint32_t _hwRead32<pageidx, false>(u32 mem);

InstantizeHwRead(0x00);
InstantizeHwRead(0x01);
InstantizeHwRead(0x02);
InstantizeHwRead(0x03);
InstantizeHwRead(0x04);
InstantizeHwRead(0x05);
InstantizeHwRead(0x06);
InstantizeHwRead(0x07);
InstantizeHwRead(0x08);
InstantizeHwRead(0x09);
InstantizeHwRead(0x0a);
InstantizeHwRead(0x0b);
InstantizeHwRead(0x0c);
InstantizeHwRead(0x0d);
InstantizeHwRead(0x0e);
InstantizeHwRead(0x0f);


InstantizeHwWrite(0x00);
InstantizeHwWrite(0x01);
InstantizeHwWrite(0x02);
InstantizeHwWrite(0x03);
InstantizeHwWrite(0x04);
InstantizeHwWrite(0x05);
InstantizeHwWrite(0x06);
InstantizeHwWrite(0x07);
InstantizeHwWrite(0x08);
InstantizeHwWrite(0x09);
InstantizeHwWrite(0x0a);
InstantizeHwWrite(0x0b);
InstantizeHwWrite(0x0c);
InstantizeHwWrite(0x0d);
InstantizeHwWrite(0x0e);
InstantizeHwWrite(0x0f);
