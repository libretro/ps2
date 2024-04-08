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


#include "PrecompiledHeader.h"
#include "SaveState.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SafeArray.inl"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"

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
#include "Host.h"
#include "GS.h"
#include "GS/GS.h"
#include "SPU2/spu2.h"
#include "StateWrapper.h"
#include "PAD/Host/PAD.h"
#include "USB/USB.h"
#include "VMManager.h"

#include "fmt/core.h"

#include <csetjmp>
#include <png.h>

using namespace R5900;

static tlbs s_tlb_backup[std::size(tlb)];

// --------------------------------------------------------------------------------------
//  SaveStateBase  (implementations)
// --------------------------------------------------------------------------------------
SaveStateBase::SaveStateBase( SafeArray<u8>& memblock )
{
	Init( &memblock );
}

SaveStateBase::SaveStateBase( SafeArray<u8>* memblock )
{
	Init( memblock );
}

void SaveStateBase::Init( SafeArray<u8>* memblock )
{
	m_memory	= memblock;
	m_version	= g_SaveVersion;
	m_idx		= 0;
}

void SaveStateBase::PrepBlock( int size )
{
	pxAssertDev( m_memory, "Savestate memory/buffer pointer is null!" );

	const int end = m_idx+size;
	if( IsSaving() )
		m_memory->MakeRoomFor( end );
	else
	{
		if( m_memory->GetSizeInBytes() < end )
			throw Exception::SaveStateLoadError();
	}
}

void SaveStateBase::FreezeTag( const char* src )
{
	const uint allowedlen = sizeof( m_tagspace )-1;
	pxAssertDev(strlen(src) < allowedlen, "Tag name exceeds the allowed length");

	memzero( m_tagspace );
	strcpy( m_tagspace, src );
	Freeze( m_tagspace );

	if( strcmp( m_tagspace, src ) != 0 )
	{
		std::string msg(fmt::format("Savestate data corruption detected while reading tag: {}", src));
		pxFail( msg.c_str() );
		throw Exception::SaveStateLoadError().SetDiagMsg(std::move(msg));
	}
}

SaveStateBase& SaveStateBase::FreezeBios()
{
	FreezeTag( "BIOS" );

	// Check the BIOS, and issue a warning if the bios for this state
	// doesn't match the bios currently being used (chances are it'll still
	// work fine, but some games are very picky).

	u32 bioscheck = BiosChecksum;
	char biosdesc[256];
	memzero( biosdesc );
	memcpy( biosdesc, BiosDescription.c_str(), std::min( sizeof(biosdesc), BiosDescription.length() ) );

	Freeze( bioscheck );
	Freeze( biosdesc );

	if (bioscheck != BiosChecksum)
	{
		Console.Newline();
		Console.Indent(1).Error( "Warning: BIOS Version Mismatch, savestate may be unstable!" );
		Console.Indent(2).Error(
			"Current BIOS:   %s (crc=0x%08x)\n"
			"Savestate BIOS: %s (crc=0x%08x)\n",
			BiosDescription.c_str(), BiosChecksum,
			biosdesc, bioscheck
		);
	}

	return *this;
}

SaveStateBase& SaveStateBase::FreezeInternals()
{
	const u32 previousCRC = ElfCRC;

	// Second Block - Various CPU Registers and States
	// -----------------------------------------------
	FreezeTag( "cpuRegs" );
	Freeze(cpuRegs);		// cpu regs + COP0
	Freeze(psxRegs);		// iop regs
	Freeze(fpuRegs);
	Freeze(tlb);			// tlbs
	Freeze(AllowParams1);	//OSDConfig written (Fast Boot)
	Freeze(AllowParams2);
	Freeze(g_GameStarted);
	Freeze(g_GameLoading);
	Freeze(ElfCRC);

	char localDiscSerial[256];
	StringUtil::Strlcpy(localDiscSerial, DiscSerial.c_str(), sizeof(localDiscSerial));
	Freeze(localDiscSerial);
	if (IsLoading())
	{
		DiscSerial = localDiscSerial;

		if (ElfCRC != previousCRC)
		{
			// HACK: LastELF isn't in the save state... Load it before we go too far into restoring state.
			// When we next bump save states, we should include it. We need this for achievements, because
			// we want to load and activate achievements before restoring any of their tracked state.
			if (const std::string& elf_override = VMManager::Internal::GetElfOverride(); !elf_override.empty())
				cdvdReloadElfInfo(fmt::format("host:{}", elf_override));
			else
				cdvdReloadElfInfo();
		}
	}


	// Third Block - Cycle Timers and Events
	// -------------------------------------
	FreezeTag( "Cycles" );
	Freeze(EEsCycle);
	Freeze(EEoCycle);
	Freeze(nextCounter);
	Freeze(nextsCounter);
	Freeze(psxNextsCounter);
	Freeze(psxNextCounter);

	// Fourth Block - EE-related systems
	// ---------------------------------
	FreezeTag( "EE-Subsystems" );
	rcntFreeze();
	gsFreeze();
	vuMicroFreeze();
	vuJITFreeze();
	vif0Freeze();
	vif1Freeze();
	sifFreeze();
	ipuFreeze();
	ipuDmaFreeze();
	gifFreeze();
	gifDmaFreeze();
	sprFreeze();
	mtvuFreeze();

	// Fifth Block - iop-related systems
	// ---------------------------------
	FreezeTag( "IOP-Subsystems" );
	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif));		// iop's sif memory (not really needed, but oh well)

	psxRcntFreeze();
	sioFreeze();
	sio2Freeze();
	cdrFreeze();
	cdvdFreeze();

	// technically this is HLE BIOS territory, but we don't have enough such stuff
	// to merit an HLE Bios sub-section... yet.
	deci2Freeze();

	return *this;
}


// --------------------------------------------------------------------------------------
//  memSavingState (implementations)
// --------------------------------------------------------------------------------------
// uncompressed to/from memory state saves implementation

memSavingState::memSavingState( SafeArray<u8>& save_to )
	: SaveStateBase( save_to )
{
}

memSavingState::memSavingState( SafeArray<u8>* save_to )
	: SaveStateBase( save_to )
{
}

// Saving of state data
void memSavingState::FreezeMem( void* data, int size )
{
	if (!size) return;

	m_memory->MakeRoomFor( m_idx + size );
	memcpy( m_memory->GetPtr(m_idx), data, size );
	m_idx += size;
}

void memSavingState::MakeRoomForData()
{
	pxAssertDev( m_memory, "Savestate memory/buffer pointer is null!" );

	m_memory->ChunkSize = ReallocThreshold;
	m_memory->MakeRoomFor( m_idx + MemoryBaseAllocSize );
}

// --------------------------------------------------------------------------------------
//  memLoadingState  (implementations)
// --------------------------------------------------------------------------------------
memLoadingState::memLoadingState( const SafeArray<u8>& load_from )
	: SaveStateBase( const_cast<SafeArray<u8>&>(load_from) )
{
}

memLoadingState::memLoadingState( const SafeArray<u8>* load_from )
	: SaveStateBase( const_cast<SafeArray<u8>*>(load_from) )
{
}

// Loading of state data from a memory buffer...
void memLoadingState::FreezeMem( void* data, int size )
{
	const u8* const src = m_memory->GetPtr(m_idx);
	m_idx += size;
	memcpy( data, src, size );
}

// Used to hold the current state backup (fullcopy of PS2 memory and subcomponents states).
//static VmStateBuffer state_buffer( L"Public Savestate Buffer" );

struct SysState_Component
{
	const char* name;
	int (*freeze)(FreezeAction, freezeData*);
};

static int SysState_MTGSFreeze(FreezeAction mode, freezeData* fP)
{
	MTGS_FreezeData sstate = { fP, 0 };
	GetMTGS().Freeze(mode, sstate);
	return sstate.retval;
}

static constexpr SysState_Component SPU2_{ "SPU2", SPU2freeze };
static constexpr SysState_Component PAD_{ "PAD", PADfreeze };
static constexpr SysState_Component GS{ "GS", SysState_MTGSFreeze };


static void SysState_ComponentFreezeOutRoot(void* dest, SysState_Component comp)
{
	freezeData fP = { 0, (u8*)dest };
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
		return;
	if (!fP.size)
		return;

	Console.Indent().WriteLn("Saving %s", comp.name);

	if (comp.freeze(FreezeAction::Save, &fP) != 0)
		throw std::runtime_error(std::string(" * ") + comp.name + std::string(": Error saving state!\n"));
}

static void SysState_ComponentFreezeOut(SaveStateBase& writer, SysState_Component comp)
{
	freezeData fP = { 0, NULL };
	if (comp.freeze(FreezeAction::Size, &fP) == 0)
	{
		const int size = fP.size;
		writer.PrepBlock(size);
		SysState_ComponentFreezeOutRoot(writer.GetBlockPtr(), comp);
		writer.CommitBlock(size);
	}
	return;
}

static void SysState_ComponentFreezeOutNew(SaveStateBase& writer, const char* name, u32 reserve, bool (*do_state_func)(StateWrapper&))
{
	StateWrapper::VectorMemoryStream stream(reserve);
	StateWrapper sw(&stream, StateWrapper::Mode::Write, g_SaveVersion);

	// TODO: Get rid of the bloody exceptions.
	if (!do_state_func(sw))
		throw std::runtime_error(fmt::format(" * {}: Error saving state!", name));

	const int size = static_cast<int>(stream.GetBuffer().size());
	if (size > 0)
	{
		writer.PrepBlock(size);
		std::memcpy(writer.GetBlockPtr(), stream.GetBuffer().data(), size);
		writer.CommitBlock(size);
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

	virtual const char* GetFilename() const = 0;
	virtual void FreezeOut(SaveStateBase& writer) const = 0;
};

class MemorySavestateEntry : public BaseSavestateEntry
{
protected:
	MemorySavestateEntry() {}
	virtual ~MemorySavestateEntry() = default;

public:
	virtual void FreezeOut(SaveStateBase& writer) const;

protected:
	virtual u8* GetDataPtr() const = 0;
	virtual u32 GetDataSize() const = 0;
};

void MemorySavestateEntry::FreezeOut(SaveStateBase& writer) const
{
	writer.FreezeMem(GetDataPtr(), GetDataSize());
}
