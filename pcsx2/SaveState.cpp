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
#include <compat/strl.h>

#include "SaveState.h"

#include "HostFS.h"
#include "../common/Path.h"
#include "../common/StringUtil.h"

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

// --------------------------------------------------------------------------------------
//  SaveStateBase  (implementations)
// --------------------------------------------------------------------------------------
SaveStateBase::SaveStateBase( std::vector<u8>& memblock, bool is_saving )
	: m_memory(memblock) {
	m_is_saving = is_saving; }

void SaveStateBase::PrepBlock(int size)
{
	if (m_error)
		return;
	const int end = m_idx+size;
	if (IsSaving())
	{
		if (static_cast<u32>(end) >= m_memory.size())
			m_memory.resize(static_cast<u32>(end));
	}
	else
	{
		if (m_memory.size() < static_cast<u32>(end))
			m_error = true;
	}
}

bool SaveStateBase::FreezeTag(const char *src)
{
	if (m_error)
		return false;

	memset(m_tagspace, 0, sizeof(m_tagspace));
	strlcpy( m_tagspace, src, sizeof(m_tagspace) );
	Freeze( m_tagspace );

	if(strcmp( m_tagspace, src ) != 0 )
	{
		m_error = true;
		return false;
	}

	return true;
}

bool SaveStateBase::FreezeBios()
{
	char biosdesc[256];
	if (!FreezeTag("BIOS"))
		return false;

	// Check the BIOS, and issue a warning if the bios for this state
	// doesn't match the bios currently being used (chances are it'll still
	// work fine, but some games are very picky).
	u32 bioscheck = BiosChecksum;
	memset(biosdesc, 0, sizeof(biosdesc));
	memcpy( biosdesc, BiosDescription, pcsx2_min_sz(sizeof(biosdesc), strlen(BiosDescription)) );

	Freeze( bioscheck );
	Freeze( biosdesc );

	return IsOkay();
}

bool SaveStateBase::FreezeInternals()
{
	// Second Block - Various CPU Registers and States
	// -----------------------------------------------
	if (!FreezeTag( "cpuRegs" ))
		return false;

	Freeze(cpuRegs);		// cpu regs + COP0
	Freeze(psxRegs);		// iop regs
	Freeze(fpuRegs);
	Freeze(tlb);			// tlbs
	Freeze(AllowParams1);	//OSDConfig written (Fast Boot)
	Freeze(AllowParams2);
	Freeze(g_GameStarted);
	Freeze(g_GameLoading);
	Freeze(ElfCRC);

	// Third Block - Cycle Timers and Events
	// -------------------------------------
	if (!(FreezeTag( "Cycles" )))
		return false;
	Freeze(EEsCycle);
	Freeze(EEoCycle);
	Freeze(nextDeltaCounter);
	Freeze(nextStartCounter);
	Freeze(psxNextStartCounter);
	Freeze(psxNextDeltaCounter);

	// Fourth Block - EE-related systems
	// ---------------------------------
	if (!(FreezeTag( "EE-Subsystems" )))
		return false;

	bool okay = rcntFreeze();
	okay = okay && gsFreeze();
	okay = okay && vuMicroFreeze();
#ifndef ARCH_ARM64
	okay = okay && vuJITFreeze();	// no VU JIT state to (de)serialise on arm64
#endif
	okay = okay && vif0Freeze();
	okay = okay && vif1Freeze();
	okay = okay && sifFreeze();
	okay = okay && ipuFreeze();
	okay = okay && ipuDmaFreeze();
	okay = okay && gifFreeze();
	okay = okay && gifDmaFreeze();
	okay = okay && sprFreeze();
	okay = okay && mtvuFreeze();
	if (!okay)
		return false;

	// Fifth Block - iop-related systems
	// ---------------------------------
	if (!(FreezeTag( "IOP-Subsystems" )))
		return false;

	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif));		// iop's sif memory (not really needed, but oh well)

	okay = okay && psxRcntFreeze();
	okay = okay && sioFreeze();
	okay = okay && sio2Freeze();
	okay = okay && cdrFreeze();
	okay = okay && cdvdFreeze();

	// technically this is HLE BIOS territory, but we don't have enough such stuff
	// to merit an HLE Bios sub-section... yet.
	okay = okay && deci2Freeze();

	return okay;
}


// --------------------------------------------------------------------------------------
//  SaveStateBase memory I/O
// --------------------------------------------------------------------------------------
// uncompressed to/from memory state saves implementation

/* One FreezeMem for both directions. Saving grows the buffer and copies in;
 * loading copies out, and after an error it zero-fills rather than reading
 * past whatever went wrong. */
void SaveStateBase::FreezeMem(void* data, int size)
{
	if (!size)
		return;

	if (m_is_saving)
	{
		const int new_size = m_idx + size;
		if ((u32)new_size > m_memory.size())
			m_memory.resize((u32)new_size);

		memcpy(&m_memory[m_idx], data, size);
		m_idx += size;
		return;
	}

	if (m_error)
	{
		memset(data, 0, size);
		return;
	}

	{
		const u8* const src = &m_memory[m_idx];
		m_idx += size;
		memcpy(data, src, size);
	}
}

// --------------------------------------------------------------------------------------
//  BaseSavestateEntry
// --------------------------------------------------------------------------------------
class BaseSavestateEntry
{
protected:
	BaseSavestateEntry() = default;
public:
	virtual ~BaseSavestateEntry() = default;
};

class MemorySavestateEntry : public BaseSavestateEntry
{
protected:
	MemorySavestateEntry() {}
	virtual ~MemorySavestateEntry() = default;
};
