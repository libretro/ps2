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

#include <cstring> /* memset, memcpy, strlen */

#include "SaveState.h"

#include "HostFS.h"

#include "ps2/BiosTools.h"
#include "COP0.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "Cache.h"
#include "Config.h"
#include "CDVD/CDVD.h"
#include "R3000A.h"
#include "Elfheader.h"
#include "Counters.h"
#include "Patch.h"
#include "SPU2/spu2.h"
#include "PAD/PAD.h"
#include "USB/USB.h"

bool SaveState_FreezeBios(SaveStateBase *s)
{
	char biosdesc[256];
	if (!SaveState_FreezeTag(s, "BIOS"))
		return false;

	// Check the BIOS, and issue a warning if the bios for this state
	// doesn't match the bios currently being used (chances are it'll still
	// work fine, but some games are very picky).
	u32 bioscheck = BiosChecksum;
	memset(biosdesc, 0, sizeof(biosdesc));
	memcpy( biosdesc, BiosDescription, pcsx2_min_sz(sizeof(biosdesc), strlen(BiosDescription)) );

	SaveState_Freeze(s, bioscheck);
	SaveState_Freeze(s, biosdesc);

	return SaveState_IsOkay(s);
}

bool SaveState_FreezeInternals(SaveStateBase *s)
{
	// Second Block - Various CPU Registers and States
	// -----------------------------------------------
	if (!SaveState_FreezeTag(s,  "cpuRegs" ))
		return false;

	SaveState_Freeze(s, cpuRegs);		// cpu regs + COP0
	SaveState_Freeze(s, psxRegs);		// iop regs
	SaveState_Freeze(s, fpuRegs);
	SaveState_Freeze(s, tlb);			// tlbs
	SaveState_Freeze(s, AllowParams1);	//OSDConfig written (Fast Boot)
	SaveState_Freeze(s, AllowParams2);
	SaveState_Freeze(s, g_GameStarted);
	SaveState_Freeze(s, g_GameLoading);
	SaveState_Freeze(s, ElfCRC);

	// Third Block - Cycle Timers and Events
	// -------------------------------------
	if (!(SaveState_FreezeTag(s,  "Cycles" )))
		return false;
	SaveState_Freeze(s, EEsCycle);
	SaveState_Freeze(s, EEoCycle);
	SaveState_Freeze(s, nextDeltaCounter);
	SaveState_Freeze(s, nextStartCounter);
	SaveState_Freeze(s, psxNextStartCounter);
	SaveState_Freeze(s, psxNextDeltaCounter);

	// Fourth Block - EE-related systems
	// ---------------------------------
	if (!(SaveState_FreezeTag(s,  "EE-Subsystems" )))
		return false;

	bool okay = rcntFreeze(s);
	okay = okay && gsFreeze(s);
	okay = okay && vuMicroFreeze(s);
#ifndef ARCH_ARM64
	okay = okay && vuJITFreeze(s);	// no VU JIT state to (de)serialise on arm64
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

	// Fifth Block - iop-related systems
	// ---------------------------------
	if (!(SaveState_FreezeTag(s,  "IOP-Subsystems" )))
		return false;

	SaveState_FreezeMem(s, iopMem->Sif, sizeof(iopMem->Sif));		// iop's sif memory (not really needed, but oh well)

	okay = okay && psxRcntFreeze(s);
	okay = okay && sioFreeze(s);
	okay = okay && sio2Freeze(s);
	okay = okay && cdrFreeze(s);
	okay = okay && cdvdFreeze(s);

	// technically this is HLE BIOS territory, but we don't have enough such stuff
	// to merit an HLE Bios sub-section... yet.
	okay = okay && deci2Freeze(s);

	return okay;
}
