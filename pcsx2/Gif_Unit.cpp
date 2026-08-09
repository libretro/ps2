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

#include "../common/Threading.h"
#include "Common.h"

#include "Gif_Unit.h"
#include "Vif_Dma.h"
#include "MTVU.h"

Gif_Unit gifUnit;

// Returns true on stalling SIGNAL
bool Gif_HandlerAD(u8* pMem)
{
	u32 reg = pMem[8];
	u32* data = (u32*)pMem;
	if (reg >= GIF_A_D_REG_BITBLTBUF && reg <= GIF_A_D_REG_TRXREG)
	{
		vif1.transfer_registers[reg - GIF_A_D_REG_BITBLTBUF] = *(u64*)pMem;
	}
	else if (reg == GIF_A_D_REG_TRXDIR)
	{ // TRXDIR
		if ((pMem[0] & 3) == 1)
		{                // local -> host
			u8 bpp = 32; // Onimusha does TRXDIR without BLTDIVIDE first, assume 32bit
			switch (vif1.BITBLTBUF.SPSM & 7)
			{
				case 0:
					bpp = 32;
					break;
				case 1:
					bpp = 24;
					break;
				case 2:
					bpp = 16;
					break;
				case 3:
					bpp = 8;
					break;
				default: // 4 is 4 bit but this is forbidden
					break;
			}
			// qwords, rounded down; any extra bits are lost
			// games must take care to ensure transfer rectangles are exact multiples of a qword
			vif1.GSLastDownloadSize = vif1.TRXREG.RRW * vif1.TRXREG.RRH * bpp >> 7;
		}
	}
	else if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		if (gsCSRload() & GS_CSR_SIGNAL)
		{ // Time to ignore all subsequent drawing operations.
			if (!gifUnit.gsSIGNAL.queued)
			{
				gifUnit.gsSIGNAL.queued = true;
				gifUnit.gsSIGNAL.data[0] = data[0];
				gifUnit.gsSIGNAL.data[1] = data[1];
				return true; // Stalling SIGNAL
			}
		}
		else
		{
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~data[1]) | (data[0] & data[1]);
			if (!GSIMR.SIGMSK)
				hwIntcIrq(INTC_GS);
			gsCSRset(GS_CSR_SIGNAL);
		}
	}
	else if (reg == GIF_A_D_REG_FINISH) /* FINISH */
	{
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;
	}
	else if (reg == GIF_A_D_REG_LABEL) /* LABEL */
	{
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~data[1]) | (data[0] & data[1]);
	}
	return false;
}

void Gif_HandlerAD_MTVU(u8* pMem)
{
	// Note: Atomic communication is with MTVU.cpp Get_GSChanges
	const u8 reg    = pMem[8] & 0x7f;
	const u32* data = (u32*)pMem;

	if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		/* gsSignal is single-producer (this MTVU thread) /
		 * single-consumer (cpu_thread). If the consumer hasn't
		 * cleared InterruptFlagSignal yet, the previous signal
		 * value still belongs to it - overwriting would drop the
		 * pending signal entirely (the consumer's fetch_and would
		 * succeed against our new flag-set, but our gsSignal
		 * value would be lost since the consumer already captured
		 * the prior value).
		 *
		 * Bounded spin to give the consumer time to drain. If
		 * cpu_thread is actively running EE between VU dispatches
		 * it will hit one of the polling Get_MTVUChanges sites
		 * within a few yields and clear the flag. The bound is
		 * critical: if cpu_thread is asleep in WaitVU /
		 * WaitForEmpty, it cannot drain the flag until MTVU
		 * thread fully completes its current ring-buffer item -
		 * spinning forever here while cpu_thread waits there
		 * would deadlock. Falling out of the bound restores the
		 * upstream behavior of losing the second SIGNAL, which
		 * is rare and at worst recovers via the game's own
		 * timeout/poll paths.
		 *
		 * Upstream PCSX2 logged "Double SIGNAL Not Handled" here
		 * instead of waiting; this closes the race in the common
		 * case while keeping liveness for the deadlock case. */
		for (int spin = 0; spin < 16; spin++)
		{
			if (!((u32)retro_atomic_load_acquire_int(&vu1Thread.mtvuInterrupts) & VU_Thread::InterruptFlagSignal))
				break;
			Threading::Timeslice();
		}
		retro_atomic_store_release_64(&vu1Thread.gsSignal, (int64_t)(((u64)data[1] << 32) | data[0]));
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagSignal);
	}
	else if (reg == GIF_A_D_REG_FINISH)
	{ // FINISH
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagFinish);
	}
	else if (reg == GIF_A_D_REG_LABEL)
	{ // LABEL
		// It's okay to coalesce label updates
		u32 labelData = data[0];
		u32 labelMsk = data[1];
		/* Merge-coalesce under CAS.  cas_64 is strong with no
		 * expected-out parameter, so the loop is load-then-attempt:
		 * recompute the merge against the freshly observed value each
		 * round.  Same convergence as the old weak-CAS form (which
		 * refreshed 'existing' through the expected-out channel), one
		 * doomed first attempt fewer. */
		for (;;)
		{
			const u64 existing = (u64)retro_atomic_load_acquire_64(&vu1Thread.gsLabel);
			const u32 existingData = (u32)existing;
			const u32 existingMsk = (u32)(existing >> 32);
			const u32 wantedData = (existingData & ~labelMsk) | (labelData & labelMsk);
			const u32 wantedMsk = existingMsk | labelMsk;
			const u64 wanted = ((u64)wantedMsk << 32) | wantedData;
			if (retro_atomic_cas_64(&vu1Thread.gsLabel, (int64_t)existing, (int64_t)wanted))
				break;
		}
		retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagLabel);
	}
}

void Gif_FinishIRQ(void)
{
	if (gifUnit.gsFINISH.gsFINISHPending)
	{
		gsCSRset(GS_CSR_FINISH);
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if ((gsCSRload() & GS_CSR_FINISH) && !GSIMR.FINISHMSK && !gifUnit.gsFINISH.gsFINISHFired)
	{
		hwIntcIrq(INTC_GS);
		gifUnit.gsFINISH.gsFINISHFired = true;
	}
}

bool SaveStateBase::gifPathFreeze(u32 path)
{
	Gif_Path& gifPath = gifUnit.gifPath[path];

	if (!gifPath.isMTVU()) // FixMe: savestate freeze bug (Gust games) with MTVU enabled
	{ 
		if (IsSaving()) // Move all the buffered data to the start of buffer
			gifPath.RealignPacket(); // May add readAmount which we need to clear on load
	}
	u8* bufferPtr = gifPath.buffer; // Backup current buffer ptr
	Freeze(gifPath.mtvu.fakePackets);
	FreezeMem(&gifPath, sizeof(gifPath) - sizeof(gifPath.mtvu));
	FreezeMem(bufferPtr, gifPath.curSize);
	gifPath.buffer = bufferPtr;
	if (!IsSaving())
	{
		gifPath.readAmount = 0;
		gifPath.gsPack.readAmount = 0;
	}

	return IsOkay();
}

bool SaveStateBase::gifFreeze(void)
{
	bool mtvuMode = THREAD_VU1;
	MTGS::WaitGS(false);
	if (!(FreezeTag("Gif Unit")))
		return false;

	Freeze(mtvuMode);
	Freeze(gifUnit.stat);
	Freeze(gifUnit.gsSIGNAL);
	Freeze(gifUnit.gsFINISH);
	Freeze(gifUnit.lastTranType);
	gifPathFreeze(GIF_PATH_1);
	gifPathFreeze(GIF_PATH_2);
	gifPathFreeze(GIF_PATH_3);

	return true;
}
