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
#include "common/ZipHelpers.h"

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
#include "DebugTools/Breakpoints.h"
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

static void PreLoadPrep()
{
	// ensure everything is in sync before we start overwriting stuff.
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	GetMTGS().WaitGS(false);

	// backup current TLBs, since we're going to overwrite them all
	std::memcpy(s_tlb_backup, tlb, sizeof(s_tlb_backup));

	// clear protected pages, since we don't want to fault loading EE memory
	mmap_ResetBlockTracking();

	SysClearExecutionCache();
}

static void PostLoadPrep()
{
	resetCache();
//	WriteCP0Status(cpuRegs.CP0.n.Status.val);
	for (int i = 0; i < 48; i++)
	{
		if (std::memcmp(&s_tlb_backup[i], &tlb[i], sizeof(tlbs)) != 0)
		{
			UnmapTLB(s_tlb_backup[i], i);
			MapTLB(tlb[i], i);
		}
	}

	if (EmuConfig.Gamefixes.GoemonTlbHack) GoemonPreloadTlb();
	CBreakPoints::SetSkipFirst(BREAKPOINT_EE, 0);
	CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, 0);

	UpdateVSyncRate(true);
}

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

	// Print this until the MTVU problem in gifPathFreeze is taken care of (rama)
	if (THREAD_VU1) Console.Warning("MTVU speedhack is enabled, saved states may not be stable");

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

std::string Exception::SaveStateLoadError::FormatDiagnosticMessage() const
{
	std::string retval = "Savestate is corrupt or incomplete!\n";
	Host::AddOSDMessage("Error: Savestate is corrupt or incomplete!", 15.0f);
	_formatDiagMsg(retval);
	return retval;
}

std::string Exception::SaveStateLoadError::FormatDisplayMessage() const
{
	std::string retval = "The savestate cannot be loaded, as it appears to be corrupt or incomplete.\n";
	Host::AddOSDMessage("Error: The savestate cannot be loaded, as it appears to be corrupt or incomplete.", 15.0f);
	_formatUserMsg(retval);
	return retval;
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

static void SysState_ComponentFreezeIn(zip_file_t* zf, SysState_Component comp)
{
	if (!zf)
		return;

	freezeData fP = { 0, nullptr };
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
		fP.size = 0;

	Console.Indent().WriteLn("Loading %s", comp.name);

	auto data = std::make_unique<u8[]>(fP.size);
	fP.data = data.get();

	if (zip_fread(zf, data.get(), fP.size) != static_cast<zip_int64_t>(fP.size) || comp.freeze(FreezeAction::Load, &fP) != 0)
		throw std::runtime_error(std::string(" * ") + comp.name + std::string(": Error loading state!\n"));
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

static void SysState_ComponentFreezeInNew(zip_file_t* zf, const char* name, bool(*do_state_func)(StateWrapper&))
{
	// TODO: We could decompress on the fly here for a little bit more speed.
	std::vector<u8> data;
	if (zf)
	{
		std::optional<std::vector<u8>> optdata(ReadBinaryFileInZip(zf));
		if (optdata.has_value())
			data = std::move(optdata.value());
	}

	StateWrapper::ReadOnlyMemoryStream stream(data.empty() ? nullptr : data.data(), data.size());
	StateWrapper sw(&stream, StateWrapper::Mode::Read, g_SaveVersion);

	// TODO: Get rid of the bloody exceptions.
	if (!do_state_func(sw))
		throw std::runtime_error(fmt::format(" * {}: Error loading state!", name));
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
	virtual void FreezeIn(zip_file_t* zf) const = 0;
	virtual void FreezeOut(SaveStateBase& writer) const = 0;
	virtual bool IsRequired() const = 0;
};

class MemorySavestateEntry : public BaseSavestateEntry
{
protected:
	MemorySavestateEntry() {}
	virtual ~MemorySavestateEntry() = default;

public:
	virtual void FreezeIn(zip_file_t* zf) const;
	virtual void FreezeOut(SaveStateBase& writer) const;
	virtual bool IsRequired() const { return true; }

protected:
	virtual u8* GetDataPtr() const = 0;
	virtual u32 GetDataSize() const = 0;
};

void MemorySavestateEntry::FreezeIn(zip_file_t* zf) const
{
	const u32 expectedSize = GetDataSize();
	const s64 bytesRead = zip_fread(zf, GetDataPtr(), expectedSize);
	if (bytesRead != static_cast<s64>(expectedSize))
	{
		Console.WriteLn(Color_Yellow, " '%s' is incomplete (expected 0x%x bytes, loading only 0x%x bytes)",
			GetFilename(), expectedSize, static_cast<u32>(bytesRead));
	}
}

void MemorySavestateEntry::FreezeOut(SaveStateBase& writer) const
{
	writer.FreezeMem(GetDataPtr(), GetDataSize());
}

// --------------------------------------------------------------------------------------
//  SavestateEntry_* (EmotionMemory, IopMemory, etc)
// --------------------------------------------------------------------------------------
// Implementation Rationale:
//  The address locations of PS2 virtual memory components is fully dynamic, so we need to
//  resolve the pointers at the time they are requested (eeMem, iopMem, etc).  Thusly, we
//  cannot use static struct member initializers -- we need virtual functions that compute
//  and resolve the addresses on-demand instead... --air

class SavestateEntry_EmotionMemory : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_EmotionMemory() = default;

	const char* GetFilename() const { return "eeMemory.bin"; }
	u8* GetDataPtr() const { return eeMem->Main; }
	uint GetDataSize() const { return sizeof(eeMem->Main); }

	virtual void FreezeIn(zip_file_t* zf) const
	{
		SysClearExecutionCache();
		MemorySavestateEntry::FreezeIn(zf);
	}
};

class SavestateEntry_IopMemory : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_IopMemory() = default;

	const char* GetFilename() const { return "iopMemory.bin"; }
	u8* GetDataPtr() const { return iopMem->Main; }
	uint GetDataSize() const { return sizeof(iopMem->Main); }
};

class SavestateEntry_HwRegs : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_HwRegs() = default;

	const char* GetFilename() const { return "eeHwRegs.bin"; }
	u8* GetDataPtr() const { return eeHw; }
	uint GetDataSize() const { return sizeof(eeHw); }
};

class SavestateEntry_IopHwRegs : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_IopHwRegs() = default;

	const char* GetFilename() const { return "iopHwRegs.bin"; }
	u8* GetDataPtr() const { return iopHw; }
	uint GetDataSize() const { return sizeof(iopHw); }
};

class SavestateEntry_Scratchpad : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_Scratchpad() = default;

	const char* GetFilename() const { return "Scratchpad.bin"; }
	u8* GetDataPtr() const { return eeMem->Scratch; }
	uint GetDataSize() const { return sizeof(eeMem->Scratch); }
};

class SavestateEntry_VU0mem : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU0mem() = default;

	const char* GetFilename() const { return "vu0Memory.bin"; }
	u8* GetDataPtr() const { return vuRegs[0].Mem; }
	uint GetDataSize() const { return VU0_MEMSIZE; }
};

class SavestateEntry_VU1mem : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU1mem() = default;

	const char* GetFilename() const { return "vu1Memory.bin"; }
	u8* GetDataPtr() const { return vuRegs[1].Mem; }
	uint GetDataSize() const { return VU1_MEMSIZE; }
};

class SavestateEntry_VU0prog : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU0prog() = default;

	const char* GetFilename() const { return "vu0MicroMem.bin"; }
	u8* GetDataPtr() const { return vuRegs[0].Micro; }
	uint GetDataSize() const { return VU0_PROGSIZE; }
};

class SavestateEntry_VU1prog : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU1prog() = default;

	const char* GetFilename() const { return "vu1MicroMem.bin"; }
	u8* GetDataPtr() const { return vuRegs[1].Micro; }
	uint GetDataSize() const { return VU1_PROGSIZE; }
};

class SavestateEntry_SPU2 : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_SPU2() = default;

	const char* GetFilename() const { return "SPU2.bin"; }
	void FreezeIn(zip_file_t* zf) const { return SysState_ComponentFreezeIn(zf, SPU2_); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, SPU2_); }
	bool IsRequired() const { return true; }
};

class SavestateEntry_USB : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_USB() = default;

	const char* GetFilename() const { return "USB.bin"; }
	void FreezeIn(zip_file_t* zf) const { return SysState_ComponentFreezeInNew(zf, "USB", &USB::DoState); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOutNew(writer, "USB", 16 * 1024, &USB::DoState); }
	bool IsRequired() const { return false; }
};

class SavestateEntry_PAD : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_PAD() = default;

	const char* GetFilename() const { return "PAD.bin"; }
	void FreezeIn(zip_file_t* zf) const { return SysState_ComponentFreezeIn(zf, PAD_); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, PAD_); }
	bool IsRequired() const { return true; }
};

class SavestateEntry_GS : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_GS() = default;

	const char* GetFilename() const { return "GS.bin"; }
	void FreezeIn(zip_file_t* zf) const { return SysState_ComponentFreezeIn(zf, GS); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, GS); }
	bool IsRequired() const { return true; }
};
