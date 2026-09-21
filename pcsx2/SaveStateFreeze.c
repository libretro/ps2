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

/* What goes into a savestate and in what order. The block machinery this
 * runs on is in SaveStateBase.c; the per-subsystem entry points called from
 * here live with the subsystems themselves. */

#include <string.h>

#include "SaveState.h"

#include "MemoryTypes.h"
#include "R5900.h"
#include "R3000A.h"
#include "Counters.h"
#include "Elfheader.h"
#include "ps2/BiosTools.h"

bool SaveState_FreezeBios(SaveStateBase *s)
{
	char biosdesc[256];
	u32  bioscheck;

	if (!SaveState_FreezeTag(s, "BIOS"))
		return false;

	/* The BIOS this state was made with. A mismatch against the one in use
	 * usually still works, but some games are picky about it. */
	bioscheck = BiosChecksum;
	memset(biosdesc, 0, sizeof(biosdesc));
	memcpy(biosdesc, BiosDescription,
	       pcsx2_min_sz(sizeof(biosdesc), strlen(BiosDescription)));

	SaveState_Freeze(s, bioscheck);
	SaveState_Freeze(s, biosdesc);

	return SaveState_IsOkay(s);
}

bool SaveState_FreezeInternals(SaveStateBase *s)
{
	bool okay;

	/* Second Block - Various CPU Registers and States */
	if (!SaveState_FreezeTag(s, "cpuRegs"))
		return false;

	SaveState_Freeze(s, cpuRegs);		/* cpu regs + COP0 */
	SaveState_Freeze(s, psxRegs);		/* iop regs */
	SaveState_Freeze(s, fpuRegs);
	SaveState_Freeze(s, tlb);		/* tlbs */
	SaveState_Freeze(s, AllowParams1);	/* OSDConfig written (Fast Boot) */
	SaveState_Freeze(s, AllowParams2);
	SaveState_Freeze(s, g_GameStarted);
	SaveState_Freeze(s, g_GameLoading);
	SaveState_Freeze(s, ElfCRC);

	/* Third Block - Cycle Timers and Events */
	if (!SaveState_FreezeTag(s, "Cycles"))
		return false;

	SaveState_Freeze(s, EEsCycle);
	SaveState_Freeze(s, EEoCycle);
	SaveState_Freeze(s, nextDeltaCounter);
	SaveState_Freeze(s, nextStartCounter);
	SaveState_Freeze(s, psxNextStartCounter);
	SaveState_Freeze(s, psxNextDeltaCounter);

	/* Fourth Block - EE-related systems */
	if (!SaveState_FreezeTag(s, "EE-Subsystems"))
		return false;

	okay = rcntFreeze(s);
	okay = okay && gsFreeze(s);
	okay = okay && vuMicroFreeze(s);
#ifndef ARCH_ARM64
	okay = okay && vuJITFreeze(s);	/* no VU JIT state to (de)serialise on arm64 */
#endif
	okay = okay && vif0Freeze(s);
	okay = okay && vif1Freeze(s);
	okay = okay && sifFreeze(s);
	okay = okay && ipuFreeze(s);
	okay = okay && ipuDmaFreeze(s);
	okay = okay && gifFreeze(s);
	okay = okay && gifDmaFreeze(s);
	okay = okay && sprFreeze(s);
	okay = okay && mtvuFreeze(s);
	if (!okay)
		return false;

	/* Fifth Block - iop-related systems */
	if (!SaveState_FreezeTag(s, "IOP-Subsystems"))
		return false;

	/* iop's sif memory (not really needed, but oh well) */
	SaveState_FreezeMem(s, iopMem->Sif, sizeof(iopMem->Sif));

	okay = okay && psxRcntFreeze(s);
	okay = okay && sioFreeze(s);
	okay = okay && sio2Freeze(s);
	okay = okay && cdrFreeze(s);
	okay = okay && cdvdFreeze(s);

	/* technically this is HLE BIOS territory, but we don't have enough
	 * such stuff to merit an HLE Bios sub-section... yet. */
	okay = okay && deci2Freeze(s);

	return okay;
}
