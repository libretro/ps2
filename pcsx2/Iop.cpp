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

#include <math.h>

#include "common/AlignedMalloc.h"

#include "R3000A.h"
#include "Common.h"

#include "Sio.h"
#include "Sif.h"
#include "Mdec.h"
#include "IopCounters.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopPgpuGif.h" /* for PSX kernel TTY in iopMemWrite32 */
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "SPU2/spu2.h"

uintptr_t *psxMemWLUT = NULL;
const uintptr_t *psxMemRLUT = NULL;

IopVM_MemoryAllocMess* iopMem = NULL;

alignas(__pagealignsize) u8 iopHw[Ps2MemSize::IopHardware];

/* Note on INTC usage: All counters code is always called from inside the context of an
 * event test, so instead of using the iopTestIntc we just set the 0x1070 flags directly.
 * The EventText function will pick it up. */

/* Config.PsxType == 1: PAL:
	 VBlank interlaced	50.00 Hz
	 VBlank non-interlaced	49.76 Hz
	 HBlank			15.625 KHz
   Config.PsxType == 0: NSTC
	 VBlank interlaced	59.94 Hz
	 VBlank non-interlaced	59.82 Hz
	 HBlank			15.73426573 KHz */

/* Misc IOP Clocks */
#define PSXPIXEL ((int)(PSXCLK / 13500000))
#define PSXSOUNDCLK ((int)(48000))

psxCounter psxCounters[NUM_COUNTERS];
int32_t psxNextDeltaCounter;
u32 psxNextStartCounter;
u8 psxhblankgate = 0;
u8 psxvblankgate = 0;

/* flags when the gate is off or counter disabled. (do not count) */
#define IOPCNT_STOPPED (0x10000000ul)

/* used to disable targets until after an overflow */
#define IOPCNT_FUTURE_TARGET (0x1000000000ULL)
#define IOPCNT_MODE_WRITE_MSK 0x63FF
#define IOPCNT_MODE_FLAG_MSK 0x1800

#define IOPCNT_ENABLE_GATE (1 << 0)    /* enables gate-based counters */
#define IOPCNT_MODE_GATE (3 << 1)      /* 0x6  Gate mode (dependant on counter) */
#define IOPCNT_MODE_RESET_CNT (1 << 3) // 0x8  resets the counter on target (if interrupt only?)
#define IOPCNT_INT_TARGET (1 << 4)     // 0x10  triggers an interrupt on targets
#define IOPCNT_INT_OVERFLOW (1 << 5)   // 0x20  triggers an interrupt on overflows
#define IOPCNT_INT_REPEAT (1 << 6)     // 0x40  0=One shot (ignore TOGGLE bit 7) 1=Repeat Fire (Check TOGGLE bit 7)
#define IOPCNT_INT_TOGGLE (1 << 7)     // 0x80  0=Pulse (reset on read), 1=toggle each interrupt condition (in 1 shot not reset after fired)
#define IOPCNT_ALT_SOURCE (1 << 8)     // 0x100 uses hblank on counters 1 and 3, and PSXCLOCK on counter 0
#define IOPCNT_INT_REQ (1 << 10)       // 0x400 1=Can fire interrupt, 0=Interrupt Fired (reset on read if not 1 shot)
#define IOPCNT_INT_CMPFLAG  (1 << 11)  // 0x800 1=Target interrupt raised
#define IOPCNT_INT_OFLWFLAG (1 << 12)  // 0x1000 1=Overflow interrupt raised

/* Use an arbitrary value to flag HBLANK counters.
 * These counters will be counted by the hblank gates coming from the EE,
 * which ensures they stay 100% in sync with the EE's hblank counters. */
#define PSXHBLANK 0x2001

static void _rcntSet(int cntidx)
{
	uint64_t c;
	uint64_t overflowCap      = (cntidx >= 3) ? 0x100000000ULL : 0x10000;
	const psxCounter& counter = psxCounters[cntidx];

	// psxNextDeltaCounter is relative to the psxRegs.cycle when rcntUpdate() was last called.
	// However, the current _rcntSet could be called at any cycle count, so we need to take
	// that into account.  Adding the difference from that cycle count to the current one
	// will do the trick!

	if (counter.mode & IOPCNT_STOPPED || counter.rate == PSXHBLANK)
		return;

	if (!(counter.mode & (IOPCNT_INT_TARGET | IOPCNT_INT_OVERFLOW)))
		return;
	// check for special cases where the overflow or target has just passed
	// (we probably missed it because we're doing/checking other things)
	if (counter.count > overflowCap || counter.count > counter.target)
	{
		psxNextDeltaCounter = 4;
		return;
	}

	c = (uint64_t)((overflowCap - counter.count) * counter.rate) - (psxRegs.cycle - counter.startCycle);
	c += psxRegs.cycle - psxNextStartCounter; // adjust for time passed since last rcntUpdate();

	if (c < (uint64_t)psxNextDeltaCounter)
	{
		psxNextDeltaCounter = (u32)c;
		psxSetNextBranch(psxNextStartCounter, psxNextDeltaCounter); //Need to update on counter resets/target changes
	}

	if (counter.target & IOPCNT_FUTURE_TARGET)
		return;

	c = (int64_t)((counter.target - counter.count) * counter.rate) - (psxRegs.cycle - counter.startCycle);
	c += psxRegs.cycle - psxNextStartCounter; // adjust for time passed since last rcntUpdate();

	if (c < (uint64_t)psxNextDeltaCounter)
	{
		psxNextDeltaCounter = (u32)c;
		psxSetNextBranch(psxNextStartCounter, psxNextDeltaCounter); //Need to update on counter resets/target changes
	}
}

void psxRcntInit(void)
{
	int i;

	memset(psxCounters, 0, sizeof(psxCounters));

	for (i = 0; i < 3; i++)
	{
		psxCounters[i].rate = 1;
		psxCounters[i].mode |= IOPCNT_INT_REQ;
		psxCounters[i].target = IOPCNT_FUTURE_TARGET;
	}
	for (i = 3; i < 6; i++)
	{
		psxCounters[i].rate = 1;
		psxCounters[i].mode |= IOPCNT_INT_REQ;
		psxCounters[i].target = IOPCNT_FUTURE_TARGET;
	}

	psxCounters[0].interrupt = 0x10;
	psxCounters[1].interrupt = 0x20;
	psxCounters[2].interrupt = 0x40;

	psxCounters[3].interrupt = 0x04000;
	psxCounters[4].interrupt = 0x08000;
	psxCounters[5].interrupt = 0x10000;

	psxCounters[6].rate      = 768;
	psxCounters[6].deltaCycles    = psxCounters[6].rate;
	psxCounters[6].mode      = 0x8;

	psxCounters[7].rate      = PSXCLK / 1000;
	psxCounters[7].deltaCycles    = psxCounters[7].rate;
	psxCounters[7].mode      = 0x8;

	for (i = 0; i < 8; i++)
		psxCounters[i].startCycle = psxRegs.cycle;

	/* Tell the IOP to branch ASAP, so that timers can get
	 * configured properly. */
	psxNextDeltaCounter = 1;
	psxNextStartCounter = psxRegs.cycle;
}

static bool _rcntFireInterrupt(int i, bool isOverflow)
{
	bool ret = false;

	if (psxCounters[i].mode & IOPCNT_INT_REQ)
	{
		/* IRQ fired */
		psxHu32(0x1070) |= psxCounters[i].interrupt;
		iopTestIntc();
		ret = true;
	}
	else
	{
		if (!(psxCounters[i].mode & IOPCNT_INT_REPEAT)) /* One shot */
			return false;
	}

	if (psxCounters[i].mode & IOPCNT_INT_TOGGLE) /* Toggle mode */
		psxCounters[i].mode ^= IOPCNT_INT_REQ;  /* Interrupt flag inverted */
	else
		psxCounters[i].mode &= ~IOPCNT_INT_REQ; /* Interrupt flag set low */

	return ret;
}

static void _rcntTestTarget(int i)
{
	if (psxCounters[i].count < psxCounters[i].target)
		return;

	if (psxCounters[i].mode & IOPCNT_INT_TARGET)
	{
		/* Target interrupt */
		if (_rcntFireInterrupt(i, false))
			psxCounters[i].mode |= IOPCNT_INT_CMPFLAG;
	}

	if (psxCounters[i].mode & IOPCNT_MODE_RESET_CNT) /* Reset on target */
		psxCounters[i].count -= psxCounters[i].target;
	else
		psxCounters[i].target |= IOPCNT_FUTURE_TARGET;
}


static __fi void _rcntTestOverflow(int i)
{
	uint64_t maxTarget = (i < 3) ? 0xffff : 0xfffffffful;
	if (psxCounters[i].count <= maxTarget)
		return;

	if ((psxCounters[i].mode & IOPCNT_INT_OVERFLOW))
	{
		/* Overflow interrupt */
		if (_rcntFireInterrupt(i, true))
			psxCounters[i].mode |= IOPCNT_INT_OFLWFLAG; /* Overflow flag */
	}

	/* Update count.
	 * Count wraps around back to zero, while the target is restored (if not in one shot mode).
	 * (high bit of the target gets set by rcntWtarget when the target is behind
	 * the counter value, and thus should not be flagged until after an overflow) */

	psxCounters[i].count  -= maxTarget + 1;
	psxCounters[i].target &= maxTarget;
}

/*
Gate:
   TM_NO_GATE                   000
   TM_GATE_ON_Count             001
   TM_GATE_ON_ClearStart        011
   TM_GATE_ON_Clear_OFF_Start   101
   TM_GATE_ON_Start             111

   V-blank  ----+    +----------------------------+    +------
                |    |                            |    |
                |    |                            |    |
                +----+                            +----+
 TM_NO_GATE:

                0================================>============

 TM_GATE_ON_Count:

                <---->0==========================><---->0=====

 TM_GATE_ON_ClearStart:

                0====>0================================>0=====

 TM_GATE_ON_Clear_OFF_Start:

                0====><-------------------------->0====><-----

 TM_GATE_ON_Start:

                <---->0==========================>============
*/

static void _psxCheckStartGate(int i)
{
	if (!(psxCounters[i].mode & IOPCNT_ENABLE_GATE))
		return; /* Ignore Gate */

	switch ((psxCounters[i].mode & 0x6) >> 1)
	{
		case 0x0: /* GATE_ON_count - stop count on gate start:
			   * get the current count at the time of stoppage: */
			if (i < 3)
				psxCounters[i].count = psxRcntRcount16(i);
			else
				psxCounters[i].count = psxRcntRcount32(i);
			psxCounters[i].mode |= IOPCNT_STOPPED;
			return;
		case 0x2: /* GATE_ON_Clear_OFF_Start - start counting on gate start, stop on gate end */
			psxCounters[i].count = 0;
			psxCounters[i].startCycle = psxRegs.cycle;
			psxCounters[i].mode &= ~IOPCNT_STOPPED;
			break;
		case 0x1: /* GATE_ON_ClearStart - count normally with resets after every end gate
			   * do nothing - All counting will be done on a need-to-count basis. */
		case 0x3: /*GATE_ON_Start - start and count normally on gate end (no restarts or stops or clears)
			   * do nothing! */
			return;
	}
	_rcntSet(i);
}

static void _psxCheckEndGate(int i)
{
	if (!(psxCounters[i].mode & IOPCNT_ENABLE_GATE))
		return; /* Ignore Gate */

	switch ((psxCounters[i].mode & 0x6) >> 1)
	{
		case 0x0: // GATE_ON_count - reset and start counting
		case 0x1: // GATE_ON_ClearStart - count normally with resets after every end gate
			psxCounters[i].count = 0;
			psxCounters[i].startCycle = psxRegs.cycle;
			psxCounters[i].mode &= ~IOPCNT_STOPPED;
			break;

		case 0x2: // GATE_ON_Clear_OFF_Start - start counting on gate start, stop on gate end
			psxCounters[i].count = (i < 3) ? psxRcntRcount16(i) : psxRcntRcount32(i);
			psxCounters[i].mode |= IOPCNT_STOPPED;
			return; // do not set the counter

		case 0x3: // GATE_ON_Start - start and count normally (no restarts or stops or clears)
			if (psxCounters[i].mode & IOPCNT_STOPPED)
			{
				psxCounters[i].count = 0;
				psxCounters[i].startCycle = psxRegs.cycle;
				psxCounters[i].mode &= ~IOPCNT_STOPPED;
			}
			break;
	}
	_rcntSet(i);
}

void psxCheckStartGate16(int i)
{
	if (i == 0) // hSync counting
	{
		// AlternateSource/scanline counters for Gates 1 and 3.
		// We count them here so that they stay nicely synced with the EE's hsync.

		const u32 altSourceCheck = IOPCNT_ALT_SOURCE | IOPCNT_ENABLE_GATE;
		const u32 stoppedGateCheck = (IOPCNT_STOPPED | altSourceCheck);

		// count if alt source is enabled and either:
		//  * the gate is enabled and not stopped.
		//  * the gate is disabled.

		if ((psxCounters[1].mode & altSourceCheck) == IOPCNT_ALT_SOURCE ||
			(psxCounters[1].mode & stoppedGateCheck) == altSourceCheck)
		{
			psxCounters[1].count++;
			_rcntTestOverflow(1);
			_rcntTestTarget(1);
		}

		if ((psxCounters[3].mode & altSourceCheck) == IOPCNT_ALT_SOURCE ||
			(psxCounters[3].mode & stoppedGateCheck) == altSourceCheck)
		{
			psxCounters[3].count++;
			_rcntTestOverflow(3);
			_rcntTestTarget(3);
		}
	}

	_psxCheckStartGate(i);
}

void psxCheckEndGate16(int i)          { _psxCheckEndGate(i); }
/* 32bit gate is called for gate 3 only.  Ever. */
static void psxCheckStartGate32(int i) { _psxCheckStartGate(i); }
static void psxCheckEndGate32(int i)   { _psxCheckEndGate(i); }

void psxVBlankStart(void)
{
	cdvdVsync();
	iopIntcIrq(0);
	if (psxvblankgate & (1 << 1))
		psxCheckStartGate16(1);
	if (psxvblankgate & (1 << 3))
		psxCheckStartGate32(3);
}

void psxVBlankEnd(void)
{
	iopIntcIrq(11);
	if (psxvblankgate & (1 << 1))
		psxCheckEndGate16(1);
	if (psxvblankgate & (1 << 3))
		psxCheckEndGate32(3);
}

void psxRcntUpdate(void)
{
	int i;

	psxNextDeltaCounter = 0x7fffffff;
	psxNextStartCounter = psxRegs.cycle;

	for (i = 0; i <= 5; i++)
	{
		// don't count disabled or hblank counters...
		// We can't check the ALTSOURCE flag because the PSXCLOCK source *should*
		// be counted here.

		if (psxCounters[i].mode & IOPCNT_STOPPED)
			continue;

		//Repeat IRQ mode Pulsed, resets a few cycles after the interrupt, this should do.
		if ((psxCounters[i].mode & IOPCNT_INT_REPEAT) && !(psxCounters[i].mode & IOPCNT_INT_TOGGLE))
			psxCounters[i].mode |= IOPCNT_INT_REQ;

		if (psxCounters[i].rate == PSXHBLANK)
			continue;


		if (psxCounters[i].rate != 1)
		{
			const u32 change = (psxRegs.cycle - psxCounters[i].startCycle) / psxCounters[i].rate;

			if (change <= 0)
				continue;

			psxCounters[i].count      += change;
			psxCounters[i].startCycle += change * psxCounters[i].rate;
		}
		else
		{
			psxCounters[i].count      += psxRegs.cycle - psxCounters[i].startCycle;
			psxCounters[i].startCycle  = psxRegs.cycle;
		}
	}

	// Do target/overflow testing
	// Optimization Note: This approach is very sound.  Please do not try to unroll it
	// as the size of the Test functions will cause code cache clutter and slowness.

	for (i = 0; i < 6; i++)
	{
		// don't do target/oveflow checks for hblankers.  Those
		// checks are done when the counters are updated.
		if (psxCounters[i].rate == PSXHBLANK)
			continue;
		if (psxCounters[i].mode & IOPCNT_STOPPED)
			continue;

		_rcntTestOverflow(i);
		_rcntTestTarget(i);
	}

	const u32 spu2_delta = (psxRegs.cycle - lClocks) % 768;
	psxCounters[6].startCycle = psxRegs.cycle;
	psxCounters[6].deltaCycles = psxCounters[6].rate - spu2_delta;
	SPU2async();
	psxNextDeltaCounter = psxCounters[6].deltaCycles;

	DEV9async(1);
	const int32_t diffusb = psxRegs.cycle - psxCounters[7].startCycle;
	int32_t cusb          = psxCounters[7].deltaCycles;

	if (diffusb >= psxCounters[7].deltaCycles)
	{
		USBasync(diffusb);
		psxCounters[7].startCycle += psxCounters[7].rate * (diffusb / psxCounters[7].rate);
		psxCounters[7].deltaCycles   = psxCounters[7].rate;
	}
	else
		cusb -= diffusb;

	if (cusb < psxNextDeltaCounter)
		psxNextDeltaCounter = cusb;

	for (i = 0; i < 6; i++)
		_rcntSet(i);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
void psxRcntWcount16(int index, u16 value)
{
	if (psxCounters[index].rate != PSXHBLANK)
	{
		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}

	psxCounters[index].count   = value & 0xffff;
	psxCounters[index].target &= 0xffff;

	if (psxCounters[index].count > psxCounters[index].target)
	{
		// Count already higher than Target
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;
	}

	_rcntSet(index);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
void psxRcntWcount32(int index, u32 value)
{
	if (psxCounters[index].rate != PSXHBLANK)
	{
		// Re-adjust the startCycle to match where the counter is currently
		// (remainder of the rate divided into the time passed will do the trick)

		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}

	psxCounters[index].count   = value;
	psxCounters[index].target &= 0xffffffff;

	if (psxCounters[index].count > psxCounters[index].target)
	{
		// Count already higher than Target
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;
	}

	_rcntSet(index);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
__fi void psxRcntWmode16(int index, u32 value)
{
	int irqmode = 0;

	psxCounter& counter = psxCounters[index];

	counter.mode = (value & IOPCNT_MODE_WRITE_MSK) | (counter.mode & IOPCNT_MODE_FLAG_MSK); // Write new value, preserve flags
	counter.mode |= IOPCNT_INT_REQ; // IRQ Enable

	if (value & (1 << 4))
		irqmode += 1;
	if (value & (1 << 5))
		irqmode += 2;
	if (index == 2)
	{
		switch (value & 0x200)
		{
			case 0x000:
				psxCounters[2].rate = 1;
				break;
			case 0x200:
				psxCounters[2].rate = 8;
				break;
			default:
				break;
		}

		if ((counter.mode & 0x7) == 0x7 || (counter.mode & 0x7) == 0x1)
			counter.mode |= IOPCNT_STOPPED;
	}
	else
	{
		// Counters 0 and 1 can select PIXEL or HSYNC as an alternate source:
		counter.rate = 1;

		if (value & IOPCNT_ALT_SOURCE)
			counter.rate = (index == 0) ? PSXPIXEL : PSXHBLANK;

		if (counter.mode & IOPCNT_ENABLE_GATE)
		{
			// gated counters are added up as per the h/vblank timers.
			// (the PIXEL alt source becomes a vsync gate)
			counter.mode |= IOPCNT_STOPPED;
			if (index == 0)
				psxhblankgate |= 1; // fixme: these gate flags should be one var >_<
			else
				psxvblankgate |= 1 << 1;
		}
		else
		{
			if (index == 0)
				psxhblankgate &= ~1;
			else
				psxvblankgate &= ~(1 << 1);
		}
	}

	counter.count = 0;
	counter.startCycle = psxRegs.cycle;

	counter.target &= 0xffff;

	_rcntSet(index);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
__fi void psxRcntWmode32(int index, u32 value)
{
	int irqmode = 0;
	psxCounter& counter = psxCounters[index];

	counter.mode = (value & IOPCNT_MODE_WRITE_MSK) | (counter.mode & IOPCNT_MODE_FLAG_MSK); // Write new value, preserve flags
	counter.mode |= IOPCNT_INT_REQ; // IRQ Enable

	if (value & (1 << 4))
		irqmode += 1;
	if (value & (1 << 5))
		irqmode += 2;
	if (index == 3)
	{
		// Counter 3 has the HBlank as an alternate source.
		counter.rate = 1;
		if (value & IOPCNT_ALT_SOURCE)
			counter.rate = PSXHBLANK;

		if (counter.mode & IOPCNT_ENABLE_GATE)
		{
			counter.mode |= IOPCNT_STOPPED;
			psxvblankgate |= 1 << 3;
		}
		else
			psxvblankgate &= ~(1 << 3);
	}
	else
	{
		switch (value & 0x6000)
		{
			case 0x0000:
				counter.rate = 1;
				break;
			case 0x2000:
				counter.rate = 8;
				break;
			case 0x4000:
				counter.rate = 16;
				break;
			case 0x6000:
				counter.rate = 256;
				break;
		}

		// Need to set a rate and target
		if ((counter.mode & 0x7) == 0x7 || (counter.mode & 0x7) == 0x1)
			counter.mode |= IOPCNT_STOPPED;
	}

	counter.count = 0;
	counter.startCycle = psxRegs.cycle;
	counter.target &= 0xffffffff;
	_rcntSet(index);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
void psxRcntWtarget16(int index, u32 value)
{
	psxCounters[index].target = value & 0xffff;

	// Pulse mode reset
	if (!(psxCounters[index].mode & IOPCNT_INT_TOGGLE))
		psxCounters[index].mode |= IOPCNT_INT_REQ; // Interrupt flag reset to high

	if (!(psxCounters[index].mode & IOPCNT_STOPPED) &&
		(psxCounters[index].rate != PSXHBLANK))
	{
		// Re-adjust the startCycle to match where the counter is currently
		// (remainder of the rate divided into the time passed will do the trick)

		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].count += change;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}

	// protect the target from an early arrival.
	// if the target is behind the current count, then set the target overflow
	// flag, so that the target won't be active until after the next overflow.

	if (psxCounters[index].target <= psxCounters[index].count)
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;

	_rcntSet(index);
}

void psxRcntWtarget32(int index, u32 value)
{
	psxCounters[index].target = value;

	// Pulse mode reset
	if (!(psxCounters[index].mode & IOPCNT_INT_TOGGLE))
		psxCounters[index].mode |= IOPCNT_INT_REQ; // Interrupt flag reset to high
					

	if (!(psxCounters[index].mode & IOPCNT_STOPPED) &&
		(psxCounters[index].rate != PSXHBLANK))
	{
		// Re-adjust the startCycle to match where the counter is currently
		// (remainder of the rate divided into the time passed will do the trick)

		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].count += change;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}
	// protect the target from an early arrival.
	// if the target is behind the current count, then set the target overflow
	// flag, so that the target won't be active until after the next overflow.

	if (psxCounters[index].target <= psxCounters[index].count)
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;

	_rcntSet(index);
}

u16 psxRcntRcount16(int index)
{
	u32 retval = (u32)psxCounters[index].count;

	// Don't count HBLANK timers
	// Don't count stopped gates either.

	if (!(psxCounters[index].mode & IOPCNT_STOPPED) &&
		(psxCounters[index].rate != PSXHBLANK))
	{
		u32 delta = (u32)((psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate);
		retval += delta;
	}

	return (u16)retval;
}

u32 psxRcntRcount32(int index)
{
	u32 retval = (u32)psxCounters[index].count;

	if (!(psxCounters[index].mode & IOPCNT_STOPPED) &&
		(psxCounters[index].rate != PSXHBLANK))
	{
		u32 delta = (u32)((psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate);
		retval += delta;
	}

	return retval;
}

void psxRcntSetGates(void)
{
	if (psxCounters[0].mode & IOPCNT_ENABLE_GATE)
		psxhblankgate |= 1;
	else
		psxhblankgate &= ~1;

	if (psxCounters[1].mode & IOPCNT_ENABLE_GATE)
		psxvblankgate |= 1 << 1;
	else
		psxvblankgate &= ~(1 << 1);

	if (psxCounters[3].mode & IOPCNT_ENABLE_GATE)
		psxvblankgate |= 1 << 3;
	else
		psxvblankgate &= ~(1 << 3);
}

bool SaveStateBase::psxRcntFreeze()
{
	if (!(FreezeTag("iopCounters")))
		return false;

	Freeze(psxCounters);
	Freeze(psxNextDeltaCounter);
	Freeze(psxNextStartCounter);
	Freeze(psxvblankgate);
	Freeze(psxhblankgate);

	if (!IsOkay())
		return false;

	if (IsLoading())
		psxRcntUpdate();

	return true;
}

void psxHwReset(void)
{
	memset(iopHw, 0, 0x10000);

	mdecInit(); // Initialize MDEC decoder
	cdrReset();
	cdvdReset();
	psxRcntInit();
	sio0.FullReset();
	sio2.FullReset();
}

__fi u8 psxHw4Read8(u32 add)
{
	u16 mem = add & 0xFF;
	return cdvdRead(mem);
}

__fi void psxHw4Write8(u32 add, u8 value)
{
	u8 mem = (u8)add;	// only lower 8 bits are relevant (cdvd regs mirror across the page)
	cdvdWrite(mem, value);
}

void psxDmaInterrupt(int n)
{
	if(n == 33) {
		for (int i = 0; i < 6; i++) {
			if (HW_DMA_ICR & (1 << (16 + i))) {
				if (HW_DMA_ICR & (1 << (24 + i))) {
					if (HW_DMA_ICR & (1 << 23)) {
						HW_DMA_ICR |= 0x80000000; //Set master IRQ condition met
					}
					psxRegs.CP0.n.Cause &= ~0x7C;
					iopIntcIrq(3);
					break;
				}
			}
		}
	} else if (HW_DMA_ICR & (1 << (16 + n)))
	{
		HW_DMA_ICR |= (1 << (24 + n));
		if (HW_DMA_ICR & (1 << 23)) {
			HW_DMA_ICR |= 0x80000000; //Set master IRQ condition met
		}
		iopIntcIrq(3);
	}
}

void psxDmaInterrupt2(int n)
{
	// SIF0 and SIF1 DMA IRQ's cannot be supressed due to a mask flag for "tag" interrupts being available which cannot be disabled.
	// The hardware can't disinguish between the DMA End and Tag Interrupt flags on these channels so interrupts always fire
	bool fire_interrupt = n == 2 || n == 3;

	if (n == 33) {
		for (int i = 0; i < 6; i++) {
			if (HW_DMA_ICR2 & (1 << (24 + i))) {
				if (HW_DMA_ICR2 & (1 << (16 + i)) || i == 2 || i == 3) {
					fire_interrupt = true;
					break;
				}
			}
		}
	}
	else if (HW_DMA_ICR2 & (1 << (16 + n)))
		fire_interrupt = true;

	if (fire_interrupt)
	{
		if(n != 33)
			HW_DMA_ICR2 |= (1 << (24 + n));

		if (HW_DMA_ICR2 & (1 << 23)) {
			HW_DMA_ICR2 |= 0x80000000; //Set master IRQ condition met
		}
		iopIntcIrq(3);
	}
}

void dev9Interrupt(void) { if (DEV9irqHandler() == 1) iopIntcIrq(13); }
void dev9Irq(int cycles) { PSX_INT(IopEvt_DEV9, cycles); }
void usbInterrupt(void)  { iopIntcIrq(22); }
void usbIrq(int cycles)  { PSX_INT(IopEvt_USB, cycles); }
void fwIrq(void)         { iopIntcIrq(24); }
void spu2Irq(void)       { iopIntcIrq(9); }

void iopIntcIrq(uint irqType)
{
	psxHu32(0x1070) |= 1 << irqType;
	iopTestIntc();
}

/* --------------------------------------------------------------------------------------
 *  iopMemoryReserve
 * --------------------------------------------------------------------------------------
 *
 * IOP Main Memory (2MB) */
iopMemoryReserve::iopMemoryReserve() : _parent() { }
iopMemoryReserve::~iopMemoryReserve() { Release(); }

void iopMemoryReserve::Assign(VirtualMemoryManagerPtr allocator)
{
	psxMemWLUT = (uintptr_t*)_aligned_malloc(0x2000 * sizeof(uintptr_t) * 2, 16);
	psxMemRLUT = psxMemWLUT + 0x2000; /* (uintptr_t*)_aligned_malloc(0x10000 * sizeof(uintptr_t),16); */

	VtlbMemoryReserve::Assign(std::move(allocator), HostMemoryMap::IOPmemOffset, sizeof(*iopMem));
	iopMem = reinterpret_cast<IopVM_MemoryAllocMess*>(GetPtr());
}

void iopMemoryReserve::Release()
{
	_parent::Release();

	safe_aligned_free(psxMemWLUT);
	psxMemRLUT = nullptr;
	iopMem = nullptr;
}

/* Note!  Resetting the IOP's memory state is dependent on having *all* psx memory allocated,
 * which is performed by MemInit and PsxMemInit() */
void iopMemoryReserve::Reset()
{
	_parent::Reset();

	memset(psxMemWLUT, 0, 0x2000 * sizeof(uintptr_t) * 2);	/* clears both allocations, RLUT and WLUT */

	/* Trick!  We're accessing RLUT here through WLUT, since it's the non-const pointer.
	 * So the ones with a 0x2000 prefixed are RLUT tables.
	 *
	 * Map IOP main memory, which is Read/Write, and mirrored three times
	 * at 0x0, 0x8000, and 0xa000: */
	for (int i=0; i<0x0080; i++)
	{
		psxMemWLUT[i + 0x0000] = (uintptr_t)&iopMem->Main[(i & 0x1f) << 16];

		/* RLUTs, accessed through WLUT. */
		psxMemWLUT[i + 0x2000] = (uintptr_t)&iopMem->Main[(i & 0x1f) << 16];
	}

	/* A few single-page allocations for things we store in special locations. */
	psxMemWLUT[0x2000 + 0x1f00] = (uintptr_t)iopMem->P;
	psxMemWLUT[0x2000 + 0x1f80] = (uintptr_t)iopHw;
#if 0
	psxMemWLUT[0x1bf80] = (uintptr_t)iopHw;
#endif

	psxMemWLUT[0x1f00] = (uintptr_t)iopMem->P;
	psxMemWLUT[0x1f80] = (uintptr_t)iopHw;

	/* Read-only memory areas, so don't map WLUT for these... */
	for (int i = 0; i < 0x0040; i++)
		psxMemWLUT[i + 0x2000 + 0x1fc0] = (uintptr_t)&eeMem->ROM[i << 16];

	for (int i = 0; i < 0x0040; i++)
		psxMemWLUT[i + 0x2000 + 0x1e00] = (uintptr_t)&eeMem->ROM1[i << 16];

	for (int i = 0; i < 0x0008; i++)
		psxMemWLUT[i + 0x2000 + 0x1e40] = (uintptr_t)&eeMem->ROM2[i << 16];

	/* sif!! (which is read only? (air)) */
	psxMemWLUT[0x2000 + 0x1d00] = (uintptr_t)iopMem->Sif;
#if 0
	psxMemWLUT[0x1bd00] = (uintptr_t)iopMem->Sif;

	/* this one looks like an old hack for some special write-only memory area,
	 * but leaving it in for reference (air) */
	for (i=0; i<0x0008; i++)
		psxMemWLUT[i + 0xbfc0] = (uintptr_t)&psR[i << 16];
#endif
}

u8 iopMemRead8(u32 mem)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: return IopMemory::iopHwRead8_Page1(mem);
			case 0x3000: return IopMemory::iopHwRead8_Page3(mem);
			case 0x8000: return IopMemory::iopHwRead8_Page8(mem);

			default:
				break;
		}
		return psxHu8(mem);
	}
	else if (t == 0x1f40)
		return psxHw4Read8(mem);
	else
	{
		const u8* p = (const u8*)(psxMemRLUT[mem >> 16]);
		if (p != NULL)
			return *(const u8 *)(p + (mem & 0xffff));
		if (t == 0x1000)
			return DEV9read8(mem);
		return 0;
	}
}

u16 iopMemRead16(u32 mem)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: return IopMemory::iopHwRead16_Page1(mem);
			case 0x3000: return IopMemory::iopHwRead16_Page3(mem);
			case 0x8000: return IopMemory::iopHwRead16_Page8(mem);

			default:
				break;
		}
		return psxHu16(mem);
	}
	else
	{
		const u8* p = (const u8*)(psxMemRLUT[mem >> 16]);
		if (p != NULL)
		{
			if (t == 0x1d00)
			{
				u16 ret;
				switch(mem & 0xF0)
				{
				case 0x00:
					ret= psHu16(SBUS_F200);
					break;
				case 0x10:
					ret= psHu16(SBUS_F210);
					break;
				case 0x40:
					ret= psHu16(SBUS_F240) | 0x0002;
					break;
				case 0x60:
					ret = 0;
					break;
				default:
					ret = psxHu16(mem);
					break;
				}
				return ret;
			}
			return *(const u16 *)(p + (mem & 0xffff));
		}
		else
		{
			if (t == 0x1F90)
				return SPU2read(mem);
			if (t == 0x1000)
				return DEV9read16(mem);
			return 0;
		}
	}
}

u32 iopMemRead32(u32 mem)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: return IopMemory::iopHwRead32_Page1(mem);
			case 0x3000: return IopMemory::iopHwRead32_Page3(mem);
			case 0x8000: return IopMemory::iopHwRead32_Page8(mem);

			default:
				break;
		}
		return psxHu32(mem);
	}
	else
	{
		/* see also Hw.c */
		const u8* p = (const u8*)(psxMemRLUT[mem >> 16]);
		if (p != NULL)
		{
			if (t == 0x1d00)
			{
				u32 ret;
				switch(mem & 0x8F0)
				{
					case 0x00:
						ret= psHu32(SBUS_F200);
						break;
					case 0x10:
						ret= psHu32(SBUS_F210);
						break;
					case 0x20:
						ret= psHu32(SBUS_F220);
						break;
					case 0x30: /* EE Side */
						ret= psHu32(SBUS_F230);
						break;
					case 0x40:
						ret= psHu32(SBUS_F240) | 0xF0000002;
						break;
					case 0x60:
						ret = 0;
						break;

					default:
						ret = psxHu32(mem);
						break;
				}
				return ret;
			}
			return *(const u32 *)(p + (mem & 0xffff));
		}
		else
		{
			if (t == 0x1000)
				return DEV9read32(mem);
			return 0;
		}
	}
}

void iopMemWrite8(u32 mem, u8 value)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: IopMemory::iopHwWrite8_Page1(mem,value); break;
			case 0x3000: IopMemory::iopHwWrite8_Page3(mem,value); break;
			case 0x8000: IopMemory::iopHwWrite8_Page8(mem,value); break;

			default:
				psxHu8(mem) = value;
			break;
		}
	}
	else if (t == 0x1f40)
	{
		psxHw4Write8(mem, value);
	}
	else
	{
		u8* p = (u8 *)(psxMemWLUT[mem >> 16]);
		if (p != NULL && !(psxRegs.CP0.n.Status & 0x10000) )
		{
			*(u8  *)(p + (mem & 0xffff)) = value;
			psxCpu->Clear(mem&~3, 1);
		}
		else
		{
			if (t == 0x1d00)
			{
				psxSu8(mem) = value;
				return;
			}
			if (t == 0x1000)
			{
				DEV9write8(mem, value); return;
			}
		}
	}
}

void iopMemWrite16(u32 mem, u16 value)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: IopMemory::iopHwWrite16_Page1(mem,value); break;
			case 0x3000: IopMemory::iopHwWrite16_Page3(mem,value); break;
			case 0x8000: IopMemory::iopHwWrite16_Page8(mem,value); break;

			default:
				psxHu16(mem) = value;
			break;
		}
	} else
	{
		u8* p = (u8 *)(psxMemWLUT[mem >> 16]);
		if (p != NULL && !(psxRegs.CP0.n.Status & 0x10000) )
		{
			*(u16 *)(p + (mem & 0xffff)) = value;
			psxCpu->Clear(mem&~3, 1);
		}
		else
		{
			if (t == 0x1d00)
			{
				switch (mem & 0x8f0)
				{
					case 0x10:
						/* write to ps2 mem */
						psHu16(SBUS_F210) = value;
						return;
					case 0x40:
					{
						u32 temp = value & 0xF0;
						/* write to ps2 mem */
						if(value & 0x20 || value & 0x80)
						{
							psHu16(SBUS_F240) &= ~0xF000;
							psHu16(SBUS_F240) |= 0x2000;
						}


						if(psHu16(SBUS_F240) & temp)
							psHu16(SBUS_F240) &= ~temp;
						else
							psHu16(SBUS_F240) |= temp;
						return;
					}
					case 0x60:
						psHu32(SBUS_F260) = 0;
						return;
				}
				psxSu16(mem) = value; return;
			}
			if (t == 0x1F90) {
				SPU2write(mem, value); return;
			}
			if (t == 0x1000) {
				DEV9write16(mem, value); return;
			}
		}
	}
}

void iopMemWrite32(u32 mem, u32 value)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: IopMemory::iopHwWrite32_Page1(mem,value); break;
			case 0x3000: IopMemory::iopHwWrite32_Page3(mem,value); break;
			case 0x8000: IopMemory::iopHwWrite32_Page8(mem,value); break;

			default:
				psxHu32(mem) = value;
			break;
		}
	}
	else
	{
		/* see also Hw.c */
		u8* p = (u8 *)(psxMemWLUT[mem >> 16]);
		if( p != NULL && !(psxRegs.CP0.n.Status & 0x10000) )
		{
			*(u32 *)(p + (mem & 0xffff)) = value;
			psxCpu->Clear(mem&~3, 1);
		}
		else
		{
			if (t == 0x1d00)
			{
				switch (mem & 0x8f0)
				{
					case 0x00:		/* EE write path (EE/IOP readable) */
						return;		/* this is the IOP, so read-only (do nothing) */

					case 0x10:		/* IOP write path (EE/IOP readable) */
						psHu32(SBUS_F210) = value;
						return;

					case 0x20:		/* Bits cleared when written from IOP. */
						psHu32(SBUS_F220) &= ~value;
						return;

					case 0x30:		/* bits set when written from IOP */
						psHu32(SBUS_F230) |= value;
						return;

					case 0x40:		/* Control Register */
					{
						u32 temp = value & 0xF0;
						if (value & 0x20 || value & 0x80)
						{
							psHu32(SBUS_F240) &= ~0xF000;
							psHu32(SBUS_F240) |= 0x2000;
						}


						if (psHu32(SBUS_F240) & temp)
							psHu32(SBUS_F240) &= ~temp;
						else
							psHu32(SBUS_F240) |= temp;
						return;
					}

					case 0x60:
						psHu32(SBUS_F260) = 0;
					return;

				}
				psxSu32(mem) = value;

				/* wtf?  why were we writing to the EE's sif space?  Commenting this out doesn't
				 * break any of my games, and should be more correct, but I guess we'll see.  --air */
#if 0
				*(u32*)(eeHw+0xf200+(mem&0xf0)) = value;
#endif
				return;
			}
			else if (t == 0x1000)
			{
				DEV9write32(mem, value); return;
			}
		}
	}
}

std::string iopMemReadString(u32 mem, int maxlen)
{
    std::string ret;
    char c;

    while ((c = iopMemRead8(mem++)) && maxlen--)
        ret.push_back(c);

    return ret;
}

static void psxDmaGeneric(u32 madr, u32 bcr, u32 chcr, u32 spuCore)
{
	const char dmaNum = spuCore ? 7 : 4;
	const int size = (bcr >> 16) * (bcr & 0xFFFF);

	// Update the SPU2 to the current cycle before initiating the DMA

	SPU2async();

	psxCounters[6].startCycle  = psxRegs.cycle;
	psxCounters[6].deltaCycles = size * 4;

	psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
	psxNextStartCounter  = psxRegs.cycle;
	if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
		psxNextDeltaCounter = psxCounters[6].deltaCycles;

	if ((psxRegs.iopNextEventCycle - psxNextStartCounter) > (u32)psxNextDeltaCounter)
		psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;

	switch (chcr)
	{
		case 0x01000201: //cpu to spu2 transfer
			if (dmaNum == 7)
				SPU2writeDMA7Mem((u16*)iopPhysMem(madr), size * 2);
			else if (dmaNum == 4)
				SPU2writeDMA4Mem((u16*)iopPhysMem(madr), size * 2);
			break;

		case 0x01000200: //spu2 to cpu transfer
			if (dmaNum == 7)
				SPU2readDMA7Mem((u16*)iopPhysMem(madr), size * 2);
			else if (dmaNum == 4)
				SPU2readDMA4Mem((u16*)iopPhysMem(madr), size * 2);
			psxCpu->Clear(spuCore ? HW_DMA7_MADR : HW_DMA4_MADR, size);
			break;

		default:
			break;
	}
}

void psxDma4(u32 madr, u32 bcr, u32 chcr) // SPU2's Core 0
{
	psxDmaGeneric(madr, bcr, chcr, 0);
}

int psxDma4Interrupt(void)
{
	HW_DMA4_CHCR &= ~0x01000000;
	psxDmaInterrupt(4);
	iopIntcIrq(9);
	return 1;
}

void spu2DMA4Irq(void)
{
	SPU2interruptDMA4();
	if (HW_DMA4_CHCR & 0x01000000)
	{
		HW_DMA4_CHCR &= ~0x01000000;
		psxDmaInterrupt(4);
	}
}

void psxDma7(u32 madr, u32 bcr, u32 chcr) // SPU2's Core 1
{
	psxDmaGeneric(madr, bcr, chcr, 1);
}

int psxDma7Interrupt(void)
{
	HW_DMA7_CHCR &= ~0x01000000;
	psxDmaInterrupt2(0);
	return 1;
}

void spu2DMA7Irq(void)
{
	SPU2interruptDMA7();
	if (HW_DMA7_CHCR & 0x01000000)
	{
		HW_DMA7_CHCR &= ~0x01000000;
		psxDmaInterrupt2(0);
	}
}

void psxDma2(u32 madr, u32 bcr, u32 chcr) // GPU
{
	sif2.iop.busy = true;
	sif2.iop.end = false;
}

void psxDma6(u32 madr, u32 bcr, u32 chcr)
{
	u32* mem = (u32*)iopPhysMem(madr);

	if (chcr == 0x11000002)
	{
		while (bcr--)
		{
			*mem-- = (madr - 4) & 0xffffff;
			madr -= 4;
		}
		mem++;
		*mem = 0xffffff;
	}
	HW_DMA6_CHCR &= ~0x01000000;
	psxDmaInterrupt(6);
}

void psxDma8(u32 madr, u32 bcr, u32 chcr)
{
	const int size = (bcr >> 16) * (bcr & 0xFFFF) * 8;

	switch (chcr & 0x01000201)
	{
		case 0x01000201: //cpu to dev9 transfer
			DEV9writeDMA8Mem((u32*)iopPhysMem(madr), size);
			break;

		case 0x01000200: //dev9 to cpu transfer
			DEV9readDMA8Mem((u32*)iopPhysMem(madr), size);
			break;

		default:
			break;
	}
	HW_DMA8_CHCR &= ~0x01000000;
	psxDmaInterrupt2(1);
}

void psxDma9(u32 madr, u32 bcr, u32 chcr)
{
	sif0.iop.busy = true;
	sif0.iop.end = false;

	SIF0Dma();
}

void psxDma10(u32 madr, u32 bcr, u32 chcr)
{
	sif1.iop.busy = true;
	sif1.iop.end = false;

	SIF1Dma();
}

void psxDma11(u32 madr, u32 bcr, u32 chcr)
{
	unsigned int i, j;
	int size = (bcr >> 16) * (bcr & 0xffff);
	// Set dmaBlockSize, so SIO2 knows to count based on the DMA block rather than SEND3 length.
	// When SEND3 is written, SIO2 will automatically reset this to zero.
	sio2.dmaBlockSize = (bcr & 0xffff) * 4;

	if (chcr != 0x01000201)
	{
		return;
	}

	for (i = 0; i < (bcr >> 16); i++)
	{
		for (j = 0; j < ((bcr & 0xFFFF) * 4); j++)
		{
			const u8 data = iopMemRead8(madr);
			sio2.Write(data);
			madr++;
		}
	}

	HW_DMA11_MADR = madr;
	PSX_INT(IopEvt_Dma11, (size >> 2));
}

void psxDMA11Interrupt(void)
{
	if (HW_DMA11_CHCR & 0x01000000)
	{
		HW_DMA11_CHCR &= ~0x01000000;
		psxDmaInterrupt2(4);
	}
}

void psxDma12(u32 madr, u32 bcr, u32 chcr)
{
	int size = ((bcr >> 16) * (bcr & 0xFFFF)) * 4;

	if (chcr != 0x41000200)
	{
		return;
	}

	bcr = size;

	while (bcr > 0)
	{
		const u8 data = sio2.Read();
		iopMemWrite8(madr, data);
		bcr--;
		madr++;
	}

	HW_DMA12_MADR = madr;
	PSX_INT(IopEvt_Dma12, (size >> 2));
}

void psxDMA12Interrupt(void)
{
	if (HW_DMA12_CHCR & 0x01000000)
	{
		HW_DMA12_CHCR &= ~0x01000000;
		psxDmaInterrupt2(5);
	}
}

