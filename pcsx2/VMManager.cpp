/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <compat/strl.h>
#include <retro_atomic.h>
#include <features/features_cpu.h>
#include "VMManager.h"

#include <sstream>

#include <file/file_path.h>

#include "../common/Console.h"
#include "HostFS.h"
#include "../common/FPControl.h"
#include "../common/SettingsWrapper.h"
#include "../common/StringUtil.h" /* StdStringFromFormat */
#include "../common/Threading.h"

#include "Counters.h"
#include "CDVD/CDVD.h"
#include "DEV9/DEV9.h"
#include "Elfheader.h"
#include "FW.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "GameDatabase.h"
#include "GS.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "Host.h"
#include "IopBios.h"
#include "MTVU.h"
#include "MemoryCardFile.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "SaveState.h"
#include "R3000A.h"
#include "VUmicro.h"
#include "newVif.h"
#include "R5900.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "Vif_Dynarec.h"
#include "PAD/PAD.h"
#include "Sio.h"
#include "ps2/BiosTools.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <objbase.h>
#include <timeapi.h>
#endif

// Resets all PS2 cpu execution caches, which does not affect that actual PS2 state/condition.
// This can be called at any time outside the context of a Cpu->Execute() block without
// bad things happening (recompilers will slow down for a brief moment since rec code blocks
// are dumped).
// Use this method to reset the recs when important global pointers like the MTGS are re-assigned.

// This function returns part of EXTINFO data of the BIOS rom
// This module contains information about Sony build environment at offst 0x10
// first 15 symbols is build date/time that is unique per rom and can be used as unique serial
// Example for romver 0160EC20010704
// 20010704-160707,ROMconf,PS20160EC20010704.bin,kuma@rom-server/~/f10k/g/app/rom
// 20010704-160707 can be used as unique ID for Bios
static void SysGetBiosDiscID(char* out, size_t out_size)
{
	strlcpy(out, BiosSerial, out_size);
}

// This function always returns a valid DiscID -- using the Sony serial when possible, and
// falling back on the CRC checksum of the ELF binary if the PS2 software being run is
// homebrew or some other serial-less item.
static void SysGetDiscID(char* out, size_t out_size)
{
	if (DiscSerial[0])
	{
		strlcpy(out, DiscSerial, out_size);
		return;
	}

	if (!ElfCRC)
	{
		/* system is currently running the BIOS */
		SysGetBiosDiscID(out, out_size);
		return;
	}

	snprintf(out, out_size, "%08x", ElfCRC);
}

namespace VMManager
{
	static void InitializeCPUProviders();
	static void ShutdownCPUProviders();
	static void UpdateCPUImplementations();

	static void ApplyGameFixes();
	static bool UpdateGameSettingsLayer();
	static void CheckForConfigChanges(const Pcsx2Config& old_config);
	static void CheckForCPUConfigChanges(const Pcsx2Config& old_config);
	static void CheckForGSConfigChanges(const Pcsx2Config& old_config);
	static void CheckForPatchConfigChanges(const Pcsx2Config& old_config);
	static void CheckForDEV9ConfigChanges(const Pcsx2Config& old_config);
	static void CheckForMemoryCardConfigChanges(const Pcsx2Config& old_config);

	static bool AutoDetectSource(const std::string& filename);
	static bool ApplyBootParameters(VMBootParameters params, std::string* state_to_load);
	static void LoadPatches(const std::string& serial, u32 crc);
	static void UpdateRunningGame(bool resetting, bool game_starting, bool swapping_disc);

	static void SetTimerResolutionIncreased(bool enabled);
	static void SetHardwareDependentDefaultSettings(SettingsInterface& si);
	static void EnsureCPUInfoInitialized();
	static void SetEmuThreadAffinities();
} // namespace VMManager

static std::unique_ptr<SysMainMemory> s_vm_memory;

static retro_atomic_int_t s_state = RETRO_ATOMIC_INT_INITIALIZER((int)VMState::Shutdown);
static bool s_cpu_implementation_changed = false;
static Threading::ThreadHandle s_vm_thread_handle;

static Threading::RecursiveMutex s_info_mutex;
static std::string s_disc_path;
static u32 s_game_crc;
static u32 s_patches_crc;
static char s_game_serial[64];
static std::string s_elf_override;
static u32 s_active_game_fixes = 0;
static std::vector<u8> s_widescreen_cheats_data;
static bool s_widescreen_cheats_loaded = false;
static std::vector<u8> s_no_interlacing_cheats_data;
static bool s_no_interlacing_cheats_loaded = false;
static s32 s_active_widescreen_patches = 0;
static u32 s_active_no_interlacing_patches = 0;

VMState VMManager::GetState()
{
	return (VMState)retro_atomic_load_acquire_int(&s_state);
}

void VMManager::SetState(VMState state)
{
	// Some state transitions aren't valid.
	const VMState old_state = (VMState)retro_atomic_load_acquire_int(&s_state);
	SetTimerResolutionIncreased(state == VMState::Running);
	retro_atomic_store_release_int(&s_state, (int)state);

	if (state != VMState::Stopping && (state == VMState::Paused || old_state == VMState::Paused))
	{
		const bool paused = (state == VMState::Paused);
		if (!paused)
			PerformanceMetrics::Reset();
	}
	// If stopping, break execution as soon as possible.
	else if (state == VMState::Stopping && old_state == VMState::Running)
		Cpu->ExitExecution();
}

bool VMManager::HasValidVM()
{
	const VMState state = (VMState)retro_atomic_load_acquire_int(&s_state);
	return (state >= VMState::Running && state <= VMState::Resetting);
}

const char* VMManager::GetDiscSerial()
{
	Threading::ScopedRecursiveLock lock(s_info_mutex);
	return s_game_serial;
}

void VMManager::Internal::UpdateEmuFolders()
{
	const std::string old_cheats_directory(EmuFolders::Cheats);
	const std::string old_cheats_ws_directory(EmuFolders::CheatsWS);
	const std::string old_cheats_ni_directory(EmuFolders::CheatsNI);
	const std::string old_memcards_directory(EmuFolders::MemoryCards);
	const std::string old_textures_directory(EmuFolders::Textures);

	EmuFolders::LoadConfig(*Host::Internal::GetBaseSettingsLayer());

	if (VMManager::HasValidVM())
	{
		if (EmuFolders::Cheats != old_cheats_directory || EmuFolders::CheatsWS != old_cheats_ws_directory ||
			EmuFolders::CheatsNI != old_cheats_ni_directory)
			VMManager::ReloadPatches();

		if (EmuFolders::MemoryCards != old_memcards_directory)
		{
			FileMcd_EmuClose();
			FileMcd_EmuOpen();
			AutoEject::SetAll();
		}

		if (EmuFolders::Textures != old_textures_directory)
			GSTextureReplacements::ReloadReplacementMap();
	}
}

void VMManager::Internal::CPUThreadInitialize(void)
{
	GSinit();
	SPU2::Initialize();
	USBinit();

	s_vm_memory = std::make_unique<SysMainMemory>();

	InitializeCPUProviders();

	s_vm_memory->Allocate();
}

void VMManager::Internal::CPUThreadShutdown(void)
{
	std::vector<u8>().swap(s_widescreen_cheats_data);
	std::vector<u8>().swap(s_no_interlacing_cheats_data);
	s_widescreen_cheats_loaded     = false;
	s_no_interlacing_cheats_loaded = false;

	ShutdownCPUProviders();
	s_vm_memory.reset();

	/* Nothing consults the game database without a VM, and the next
	 * load re-reads it lazily. */
	GameDatabase::unload();

#if defined(__GLIBC__)
	/* Returning the allocations is not the same as returning the
	 * memory: free() hands them back to the allocator's arena, which
	 * keeps them.  Measured over three load/unload cycles, dropping the
	 * database without this made RSS 21 MB *worse* than keeping it -
	 * the entries were freed and re-parsed into a heap that only grew.
	 * With the trim the same three cycles end 9.7 MB below where they
	 * started, so it is the trim doing the work and the unload only
	 * giving it something to reclaim.  Once per VM teardown, where a
	 * heap walk costs nothing anyone can perceive. */
	malloc_trim(0);
#endif

	USBshutdown();
	SPU2::Shutdown();
	GSshutdown();
}

SysMainMemory& GetVmMemory()
{
	return *s_vm_memory;
}

void VMManager::LoadSettings()
{
	// Switch the rounding mode back to the system default for loading settings.
	// We might have a different mode, because this can be called during setting updates while a VM is active,
	// and the rounding mode has an impact on the conversion of floating-point values to/from strings.
	FPControlRegisterBackup fpcr_backup(FPControlRegister::GetDefault());

	SettingsInterface* si             = Host::GetSettingsInterface();
	SettingsLoadWrapper slw(*si);
	EmuConfig.LoadSave(slw);
	PAD::LoadConfig(*si);

	// Remove any user-specified hacks in the config (we don't want stale/conflicting values when it's globally disabled).
	EmuConfig.GS.MaskUserHacks();
	EmuConfig.GS.MaskUpscalingHacks();

	// Disable interlacing if we have no-interlacing patches active.
	if (s_active_no_interlacing_patches > 0 && EmuConfig.GS.InterlaceMode == GSInterlaceMode::Automatic)
		EmuConfig.GS.InterlaceMode = GSInterlaceMode::Off;

	// Switch to 16:9 if widescreen patches are enabled, and AR is auto.
	if (s_active_widescreen_patches > 0)
	{
		/* TODO/FIXME - implement */
	}

	/* Ground truth for the run's memory mode: CHECK_FASTMEM compiles
	 * against exactly this value. Printed here, after the settings layer
	 * has landed in EmuConfig, so a log line is proof of mode - the
	 * option plumbing above it has already been wrong twice in ways only
	 * a consumption-point line would have caught. */
	Console.WriteLn(EmuConfig.Cpu.Recompiler.EnableFastmem
		? "Fastmem: enabled (pcsx2_fastmem)."
		: "Fastmem: DISABLED (pcsx2_fastmem) - all accesses take the software memory handlers.");

	if (HasValidVM())
		ApplyGameFixes();
}

void VMManager::ApplyGameFixes()
{
	s_active_game_fixes = 0;

	if (s_game_crc == 0)
		return;

	const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial);
	if (!game)
		return;

	s_active_game_fixes += game->applyGameFixes(EmuConfig, EmuConfig.EnableGameFixes);
	s_active_game_fixes += game->applyGSHardwareFixes(EmuConfig.GS);
}

bool VMManager::GameFixesNeedApplying()
{
	// If fixes were already applied this session (normal boot path ran
	// UpdateRunningGame -> ApplyGameFixes), s_active_game_fixes is non-zero for
	// any game that has fixes; nothing to do.
	if (s_active_game_fixes != 0)
		return false;

	// No game identified yet (still at BIOS, CRC unknown): can't look up fixes.
	if (s_game_crc == 0)
		return false;

	// Only "necessary" if the loaded game actually has GameDB fixes to apply;
	// otherwise a re-apply would be pure overhead for no behavioural change.
	const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial);
	if (!game)
		return false;

	return !game->gameFixes.empty() || !game->gsHWFixes.empty();
}

bool VMManager::UpdateGameSettingsLayer(void) { return true; }

void VMManager::LoadPatches(const std::string& serial, u32 crc)
{
	std::string message;
	int patch_count = 0;
	const std::string crc_string(StringUtil::StdStringFromFormat("%08X", crc));
	s_patches_crc                   = crc;
	s_active_widescreen_patches     = 0;
	s_active_no_interlacing_patches = 0;
	ForgetLoadedPatches();

	if (EmuConfig.EnablePatches)
	{
		const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(serial);
		if (game)
		{
			const std::string* patches = game->findPatch(crc);
			if (patches && (patch_count = LoadPatchesFromString(patches->c_str())) > 0)
			{
				Console.WriteLn("(GameDB) Patches Loaded: %d", patch_count);
				message += StringUtil::StdStringFromFormat("%d game patches", patch_count);
			}

			LoadDynamicPatches(game->dynaPatches);
		}
	}

	// regular cheat patches
	int cheat_count = 0;
	if (EmuConfig.EnableCheats)
	{
		cheat_count = LoadPatchesFromDir(crc_string, EmuFolders::Cheats, "Cheats", true);
		if (cheat_count > 0)
		{
			Console.WriteLn("Cheats Loaded: %d", cheat_count);
			message += StringUtil::StdStringFromFormat("%s%d cheat patches", (patch_count > 0) ? " and " : "", cheat_count);
		}
	}

	// wide screen patches
	if (EmuConfig.EnableWideScreenPatches && crc != 0)
	{
		if ((s_active_widescreen_patches = LoadPatchesFromDir(crc_string, EmuFolders::CheatsWS, "Widescreen hacks", false)) > 0)
			Console.WriteLn("Found widescreen patches in the cheats_ws folder --> skipping cheats_ws.zip");
		else
		{
			// No ws cheat files found at the cheats_ws folder, try the ws cheats zip file.
			if (!s_widescreen_cheats_loaded)
			{
				s_widescreen_cheats_loaded = true;

				std::optional<std::vector<u8>> data = Host::ReadResourceFile("cheats_ws.zip");
				if (data.has_value())
					s_widescreen_cheats_data = std::move(data.value());
			}

			if (!s_widescreen_cheats_data.empty())
			{
				s_active_widescreen_patches = LoadPatchesFromZip(crc_string, s_widescreen_cheats_data.data(), s_widescreen_cheats_data.size());
				Console.WriteLn("(Wide Screen Cheats DB) Patches Loaded: %d", s_active_widescreen_patches);
			}
		}

		if (s_active_widescreen_patches > 0)
		{
			message += StringUtil::StdStringFromFormat("%s%d widescreen patches", (patch_count > 0 || cheat_count > 0) ? " and " : "", s_active_widescreen_patches);

			// Switch to 16:9 if widescreen patches are enabled, and AR is auto.
			// TODO/FIXME - implement
		}
	}

	// no-interlacing patches
	if (EmuConfig.EnableNoInterlacingPatches && crc != 0)
	{
		if ((s_active_no_interlacing_patches = LoadPatchesFromDir(crc_string, EmuFolders::CheatsNI, "No-interlacing patches", false)) > 0)
		{
			Console.WriteLn("Found no-interlacing patches in the cheats_ni folder --> skipping cheats_ni.zip");
		}
		else
		{
			// No ws cheat files found at the cheats_ws folder, try the ws cheats zip file.
			if (!s_no_interlacing_cheats_loaded)
			{
				s_no_interlacing_cheats_loaded = true;

				std::optional<std::vector<u8>> data = Host::ReadResourceFile("cheats_ni.zip");
				if (data.has_value())
					s_no_interlacing_cheats_data = std::move(data.value());
			}

			if (!s_no_interlacing_cheats_data.empty())
			{
				s_active_no_interlacing_patches = LoadPatchesFromZip(crc_string, s_no_interlacing_cheats_data.data(), s_no_interlacing_cheats_data.size());
				Console.WriteLn("(No-Interlacing Cheats DB) Patches Loaded: %u", s_active_no_interlacing_patches);
			}
		}

		if (s_active_no_interlacing_patches > 0)
		{
			message += StringUtil::StdStringFromFormat("%s%u no-interlacing patches", (patch_count > 0 || cheat_count > 0 || s_active_widescreen_patches > 0) ? " and " : "", s_active_no_interlacing_patches);

			// Disable interlacing in GS if active.
			if (EmuConfig.GS.InterlaceMode == GSInterlaceMode::Automatic)
			{
				EmuConfig.GS.InterlaceMode = GSInterlaceMode::Off;
				MTGS::ApplySettings();
			}
		}
	}
	else
	{
		s_active_no_interlacing_patches = 0;
	}

	if (cheat_count > 0 || s_active_widescreen_patches > 0 || s_active_no_interlacing_patches > 0)
	{
		message += " are active.";
		Console.WriteLn("%s", message.c_str());
	}
}

void VMManager::UpdateRunningGame(bool resetting, bool game_starting, bool swapping_disc)
{
	// The CRC can be known before the game actually starts (at the bios), so when
	// we have the CRC but we're still at the bios and the settings are changed
	// (e.g. the user presses TAB to speed up emulation), we don't want to apply the
	// settings as if the game is already running (title, loadeding patches, etc).
	const bool ingame      = (ElfCRC && (g_GameLoading || g_GameStarted));
	u32 new_crc            = ingame ? ElfCRC : 0;
	char new_serial[64];

	if (ingame)
		SysGetDiscID(new_serial, sizeof(new_serial));
	else
		SysGetBiosDiscID(new_serial, sizeof(new_serial));

	if (!resetting && s_game_crc == new_crc && !strcmp(s_game_serial, new_serial))
		return;

	{
		Threading::ScopedRecursiveLock lock(s_info_mutex);
		strlcpy(s_game_serial, new_serial, sizeof(s_game_serial));
		s_game_crc    = new_crc;

		std::string memcardFilters;

		if (const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial))
		{
			memcardFilters = game->memcardFiltersAsString();
		}
		else
		{
		}

		sioSetGameSerial(memcardFilters.empty() ? s_game_serial : memcardFilters.c_str());

		// If we don't reset the timer here, when using folder memcards the reindex will cause an eject,
		// which a bunch of games don't like since they access the memory card on boot.
		if (game_starting || resetting)
			AutoEject::ClearAll();
	}

	UpdateGameSettingsLayer();
	ApplySettings();

	if (!swapping_disc)
	{
		// Clear the memory card eject notification again when booting for the first time, or starting.
		// Otherwise, games think the card was removed on boot.
		if (game_starting || resetting)
			AutoEject::ClearAll();

		// Check this here, for two cases: dynarec on, and when enable cheats is set per-game.
		if (s_patches_crc != s_game_crc)
			ReloadPatches();
	}

	MTGS::GameChanged();
	Host::OnGameChanged(s_disc_path, s_elf_override, s_game_serial, s_game_crc);
}

void VMManager::ReloadPatches()
{
	LoadPatches(s_game_serial, s_game_crc);
}

bool VMManager::IsElfFileName(const std::string_view path)
{
	return StringUtil::EndsWithNoCase(path, ".elf");
}

std::string VMManager::GetDiscOverrideFromGameSettings(const std::string& elf_path)
{
	std::string iso_path;
	ElfObject elfo;
	if (!elfo.OpenFile(elf_path))
		return iso_path;

	return iso_path;
}

bool VMManager::AutoDetectSource(const std::string& filename)
{
	if (!filename.empty())
	{
		if (!path_is_valid(filename.c_str()))
		{
			Console.Error("Requested filename '{%s}' does not exist.", filename.c_str());
			return false;
		}

		if (IsElfFileName(filename))
		{
			// alternative way of booting an elf, change the elf override, and (optionally) use the disc
			// specified in the game settings.
			std::string disc_path = GetDiscOverrideFromGameSettings(filename);
			if (!disc_path.empty())
			{
				CDVDsys_SetFile(CDVD_SourceType::Iso, disc_path.c_str());
				CDVDsys_ChangeSource(CDVD_SourceType::Iso);
			}
			else
			{
				CDVDsys_ChangeSource(CDVD_SourceType::NoDisc);
			}

			s_elf_override = filename;
			return true;
		}
		else
		{
			// TODO: Maybe we should check if it's a valid iso here...
			CDVDsys_SetFile(CDVD_SourceType::Iso, filename.c_str());
			CDVDsys_ChangeSource(CDVD_SourceType::Iso);
			s_disc_path = filename;
		}
	}
	else
	{
		// make sure we're not fast booting when we have no filename
		CDVDsys_ChangeSource(CDVD_SourceType::NoDisc);
		EmuConfig.UseBOOT2Injection = false;
	}
	return true;
}

bool VMManager::ApplyBootParameters(VMBootParameters params, std::string* state_to_load)
{
	const bool default_fast_boot = Host::GetBoolSettingValue("EmuCore", "EnableFastBoot", true);
	EmuConfig.UseBOOT2Injection = params.fast_boot.value_or(default_fast_boot);

	s_elf_override = std::move(params.elf_override);
	s_disc_path.clear();

	// resolve source type
	if (params.source_type.has_value())
	{
		if (params.source_type.value() == CDVD_SourceType::Iso && !path_is_valid(params.filename.c_str()))
		{
			Console.Error("Requested filename '{%s}' does not exist.", params.filename.c_str());
			return false;
		}

		// Use specified source type.
		s_disc_path = std::move(params.filename);
		CDVDsys_SetFile(params.source_type.value(), s_disc_path.c_str());
		CDVDsys_ChangeSource(params.source_type.value());
	}
	else
	{
		// Automatic type detection of boot parameter based on filename.
		if (!AutoDetectSource(params.filename))
			return false;
	}

	if (!s_elf_override.empty())
	{
		if (!path_is_valid(s_elf_override.c_str()))
		{
			Console.Error("Requested boot ELF '{%s}' does not exist.", s_elf_override.c_str());
			return false;
		}

		Hle_SetElfPath(s_elf_override.c_str());
		EmuConfig.UseBOOT2Injection = true;
	}
	else
	{
		Hle_ClearElfPath();
	}

	return true;
}

bool VMManager::Initialize(VMBootParameters boot_params)
{
	std::string state_to_load;
	retro_atomic_store_release_int(&s_state, (int)VMState::Initializing);
	s_vm_thread_handle = Threading::ThreadHandle::GetForCallingThread();

	if (!ApplyBootParameters(std::move(boot_params), &state_to_load))
	{
		s_vm_thread_handle = {};
		retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);
		return false;
	}

	// early out if we don't have a bios
	char bios_path[PCSX2_PATH_MAX];

	EmuConfig.FullpathToBios(bios_path, sizeof(bios_path));
	if (!IsBIOSAvailable(bios_path))
	{
		s_vm_thread_handle = {};
		retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);
		return false;
	}

	FileMcd_EmuOpen();

	if (!DoCDVDopen())
	{
		/* Every failure below this point unwinds what it opened; this
		 * one used to return with the memory cards still open and the
		 * CDVD source still set, which is the path an unreadable or
		 * oversized disc image takes. */
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_handle = {};
		retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);
		return false;
	}

	SPU2::Open();

	if (PADinit() != 0 || PADopen() != 0)
	{
		SPU2::Close();
		DoCDVDclose();
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_handle = {};
		retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);
		return false;
	}

	if (DEV9init() != 0 || DEV9open() != 0)
	{
		PADclose();
		PADshutdown();
		SPU2::Close();
		DoCDVDclose();
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_handle = {};
		retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);
		return false;
	}

	if (!USBopen())
	{
		DEV9close();
		DEV9shutdown();
		PADclose();
		PADshutdown();
		SPU2::Close();
		DoCDVDclose();
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_handle = {};
		retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);
		return false;
	}

	FWopen();

	s_cpu_implementation_changed = false;
	UpdateCPUImplementations();
	Internal::ClearCPUExecutionCaches();
	FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUFPCR);
	memBindConditionalHandlers();

	ForgetLoadedPatches();
	UpdateVSyncRate(true);

	cpuReset();
	hwReset();

	retro_atomic_store_release_int(&s_state, (int)VMState::Paused);

	UpdateRunningGame(true, false, false);

	SetEmuThreadAffinities();

	PerformanceMetrics::Clear();

	return true;
}

void VMManager::Shutdown()
{
	// we'll probably already be stopping (this is how Qt calls shutdown),
	// but just in case, so any of the stuff we call here knows we don't have a valid VM.
	retro_atomic_store_release_int(&s_state, (int)VMState::Stopping);

	SetTimerResolutionIncreased(false);

	// sync everything
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	{
		LastELF.clear();
		DiscSerial[0] = '\0';
		ElfCRC = 0;
		ElfEntry = 0;

		Threading::ScopedRecursiveLock lock(s_info_mutex);
		s_disc_path.clear();
		s_elf_override.clear();
		s_game_crc = 0;
		s_patches_crc = 0;
		s_game_serial[0] = '\0';
		Host::OnGameChanged(s_disc_path, s_elf_override, s_game_serial, 0);
	}
	s_active_game_fixes = 0;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;

	UpdateGameSettingsLayer();

	std::string().swap(s_elf_override);

	FPControlRegister::SetCurrent(FPControlRegister::GetDefault());

	ForgetLoadedPatches();
	R3000A::ioman::reset();
	vtlb_Shutdown();
	USBclose();
	SPU2::Close();
	PADclose();
	DEV9close();

	cdvdSaveNVRAM();

	DoCDVDclose();
	CDVDsys_ClearFiles();
	FWclose();
	FileMcd_EmuClose();

	MTGS::WaitForClose();

	PADshutdown();
	DEV9shutdown();

	retro_atomic_store_release_int(&s_state, (int)VMState::Shutdown);

	// clear out any potentially-incorrect settings from the last game
	LoadSettings();
}

void VMManager::Reset()
{
	// If we're running, we're probably going to be executing this at event test time,
	// at vsync, which happens in the middle of event handling. Resetting everything
	// immediately here is a bad idea (tm), in fact, it breaks some games (e.g. TC:NYC).
	// So, instead, we tell the rec to exit execution, _then_ reset. Paused is fine here,
	// since the rec won't be running, so it's safe to immediately reset there.
	if ((VMState)retro_atomic_load_acquire_int(&s_state) == VMState::Running)
	{
		retro_atomic_store_release_int(&s_state, (int)VMState::Resetting);
		return;
	}

	vu1Thread.WaitVU();
	vu1Thread.Reset();
	MTGS::WaitGS(false);

	const bool game_was_started = g_GameStarted;

	s_active_game_fixes = 0;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;

	Internal::ClearCPUExecutionCaches();
	memBindConditionalHandlers();
	UpdateVSyncRate(true);

	cpuReset();
	hwReset();

	// gameid change, so apply settings
	if (game_was_started)
		UpdateRunningGame(true, false, false);

	// If we were paused, state won't be resetting, so don't flip back to running.
	if ((VMState)retro_atomic_load_acquire_int(&s_state) == VMState::Resetting)
		retro_atomic_store_release_int(&s_state, (int)VMState::Running);
}

bool VMManager::ChangeDisc(CDVD_SourceType source, std::string path)
{
	CDVDsys_ChangeSource(source);
	if (!path.empty())
		CDVDsys_SetFile(source, path.c_str());

	const bool result = DoCDVDopen();
	if (!result)
	{
		const CDVD_SourceType old_type = CDVDsys_GetSourceType();
		char old_path[PCSX2_PATH_MAX];
		strlcpy(old_path, CDVDsys_GetFile(old_type), sizeof(old_path));

		/* Failed to open new disc image '{}'. Reverting to old image */
		CDVDsys_ChangeSource(old_type);
		if (old_path[0])
			CDVDsys_SetFile(old_type, old_path);
		if (!DoCDVDopen())
		{
			/* Failed to switch back to old disc image. Removing disc. */
			CDVDsys_ChangeSource(CDVD_SourceType::NoDisc);
			DoCDVDopen();
		}
	}
	cdvd.Tray.cdvdActionSeconds = 1;
	cdvd.Tray.trayState         = CDVD_DISC_OPEN;
	return result;
}

void VMManager::InitializeCPUProviders()
{
	// arm64 has no x86 recompilers / SSE VIF unpack; the interpreters are used.
#ifndef ARCH_ARM64
	recCpu.Reserve();
	psxRec.Reserve();

	vucpu_rec_vu0_reserve();
	vucpu_rec_vu1_reserve();

	dVifReserve(0);
	dVifReserve(1);
#else
	// arm64: reserve the IOP recompiler (psxRec, Phase C.2b) and the EE
	// recompiler (recCpu, Phase C.3) so their code caches/self-tests come up.
	psxRec.Reserve();
	recCpu.Reserve();
	// C.28-4: the armsx2 microVU1 transplant is the default VU1 provider.
	// Its Reserve() also opens vu1Thread (idempotent with the Open() below).
	vucpu_rec_vu1_reserve();
	// C.30-1: microVU0 for VU0 micro programs (VCALLMS/VCALLMSR).
	vucpu_rec_vu0_reserve();
	// The MTVU worker thread is normally spawned by vucpu_rec_vu1_reserve()
	// (x86/microVU.cpp), which doesn't exist in this build -- with vuThread
	// enabled the EE would push to vu1Thread's ring and wait on its WorkSema
	// forever (boot hang / black screen). The worker runs VU1 through
	// CpuVU1->Execute(), so it works with the VU interpreter provider too.
	vu1Thread.Open();
	// arm64 VIF unpack dynarec (C.19, NEON port transplanted from ARMSX2).
	dVifReserve(0);
	dVifReserve(1);
#endif

	GSCodeReserve::GetInstance().Assign(GetVmMemory().CodeMemory());

	VifUnpackSSE_Init(); // on arm64 this generates the NEON nVifUpk kernels (C.19)
}

void VMManager::ShutdownCPUProviders()
{
	GSCodeReserve::GetInstance().Release();

#ifndef ARCH_ARM64
	dVifRelease(1);
	dVifRelease(0);

	VifUnpackSSE_Destroy();

	vucpu_rec_vu1.Shutdown();
	vucpu_rec_vu0.Shutdown();

	psxRec.Shutdown();
	recCpu.Shutdown();
#else
	vucpu_rec_vu0.Shutdown();
	vucpu_rec_vu1.Shutdown(); // waits on the MTVU worker, then mVUclose
	vu1Thread.Close();
	dVifRelease(1);
	dVifRelease(0);
	VifUnpackSSE_Destroy();
	psxRec.Shutdown();
	recCpu.Shutdown();
#endif
}

void VMManager::UpdateCPUImplementations()
{
#ifndef ARCH_ARM64
	Cpu    = CHECK_EEREC ? &recCpu : &intCpu;
	psxCpu = CHECK_IOPREC ? &psxRec : &psxInt;
#else
	// arm64: IOP uses the arm64 recompiler (Phase C.2b). EE uses the arm64 EE
	// recompiler (Phase C.3) when CHECK_EEREC is set; C.3-1 blocks call the
	// interpreter so behaviour is identical while the JIT plumbing runs.
	Cpu    = CHECK_EEREC ? &recCpu : &intCpu;
	psxCpu = &psxRec;
#endif

	CpuVU0 = &vucpu_interp_vu0;
	CpuVU1 = &vucpu_interp_vu1;

	/* Soft float per unit: the interpreter computes every VU float op
	 * through the ps2float model, so "soft" for a unit means keeping that
	 * unit on the interpreter even when its recompiler is enabled. Same
	 * routing on both architectures. A per-op soft dispatch inside
	 * microVU can replace this later without changing the option. */
	if (CHECK_VU_SOFT_REC(0) && EmuConfig.Cpu.Recompiler.EnableVU0)
		Console.WriteLn("VU0 soft float: micro programs on the exact interpreter (macro-mode COP2 is not covered).");
	if (CHECK_VU_SOFT_REC(1) && EmuConfig.Cpu.Recompiler.EnableVU1)
		Console.WriteLn("VU1 soft float: running on the exact interpreter.");

#ifdef ARCH_ARM64
	// C.30-1: microVU0 runs VU0 micro programs natively (macro-mode COP2
	// stays on the C.29-1 inline interpreter calls until C.30-2).
	if (EmuConfig.Cpu.Recompiler.EnableVU0 && !CHECK_VU_SOFT_REC(0))
		CpuVU0 = &vucpu_rec_vu0;
#endif

#ifndef ARCH_ARM64
	if (EmuConfig.Cpu.Recompiler.EnableVU0 && !CHECK_VU_SOFT_REC(0))
		CpuVU0 = &vucpu_rec_vu0;

	if (EmuConfig.Cpu.Recompiler.EnableVU1 && !CHECK_VU_SOFT_REC(1))
		CpuVU1 = &vucpu_rec_vu1;
#else
	// C.28-4: microVU1 (the armsx2 transplant, native VU codegen) is the VU1
	// provider -- verified register-exact against the interpreter.
	if (EmuConfig.Cpu.Recompiler.EnableVU1 && !CHECK_VU_SOFT_REC(1))
	{
		CpuVU1 = &vucpu_rec_vu1;
		Console.WriteLn("arm64 VU1 rec: microVU1 (native codegen) is the default provider.");
	}
#endif
}

void VMManager::Internal::ClearCPUExecutionCaches()
{
	Cpu->Reset();
	psxCpu->Reset();

#ifndef ARCH_ARM64
	// mVU's VU0 needs to be properly initialized for macro mode even if it's not used for micro mode!
	if (CHECK_EEREC && !EmuConfig.Cpu.Recompiler.EnableVU0)
		vucpu_rec_vu0.Reset();
#endif

	CpuVU0->Reset();
	CpuVU1->Reset();

#ifndef ARCH_ARM64
	dVifReset(0);
	dVifReset(1);
#endif
}

void VMManager::Execute()
{
	// Check for interpreter<->recompiler switches.
	if (std::exchange(s_cpu_implementation_changed, false))
	{
		// We need to switch the cpus out, and reset the new ones if so.
		UpdateCPUImplementations();
		Internal::ClearCPUExecutionCaches();
		vtlb_ResetFastmem();
	}

	// Execute until we're asked to stop.
	Cpu->Execute();
}

void VMManager::SetPaused(bool paused)
{
	if (!HasValidVM())
		return;

	if (paused)
	{
		Console.Debug("(VMManager) Pausing...");
		SetState(VMState::Paused);
	}
	else
	{
		Console.Debug("(VMManager) Resuming...");
		SetState(VMState::Running);
	}
}

const std::string& VMManager::Internal::GetElfOverride()
{
	return s_elf_override;
}

bool VMManager::Internal::IsExecutionInterrupted()
{
	return (VMState)retro_atomic_load_acquire_int(&s_state) != VMState::Running || s_cpu_implementation_changed;
}

void VMManager::Internal::EntryPointCompilingOnCPUThread()
{
	int i;
	// Classic chicken and egg problem here. We don't want to update the running game
	// until the game entry point actually runs, because that can update settings, which
	// can flush the JIT, etc. But we need to apply patches for games where the entry
	// point is in the patch (e.g. WRC 4). So. Gross, but the only way to handle it really.
	{
		char serial[64];
		SysGetDiscID(serial, sizeof(serial));
		LoadPatches(serial, ElfCRC);
	}
	for (i = 0; static_cast<size_t>(i) < Patch.size(); i++)
	{
		int _place = Patch[i].placetopatch;
		if (_place == PPT_ONCE_ON_LOAD)
			_ApplyPatch(&Patch[i]);
	}
}

void VMManager::Internal::GameStartingOnCPUThread()
{
	int i;
	UpdateRunningGame(false, true, false);
	for (i = 0; static_cast<size_t>(i) < Patch.size(); i++)
	{
		int _place = Patch[i].placetopatch;
		if ( (_place == PPT_ONCE_ON_LOAD)
		  || (_place == PPT_COMBINED_0_1))
			_ApplyPatch(&Patch[i]);
	}
}

void VMManager::CheckForCPUConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.Cpu == old_config.Cpu &&
		EmuConfig.Gamefixes == old_config.Gamefixes &&
		EmuConfig.Speedhacks == old_config.Speedhacks
		)
		return;

	Console.WriteLn("Updating CPU configuration...");
	FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUFPCR);
	Internal::ClearCPUExecutionCaches();
	memBindConditionalHandlers();

	if (EmuConfig.Cpu.Recompiler.EnableFastmem != old_config.Cpu.Recompiler.EnableFastmem)
		vtlb_ResetFastmem();

	// did we toggle recompilers?
	if (EmuConfig.Cpu.CpusChanged(old_config.Cpu))
	{
		// This has to be done asynchronously, since we're still executing the
		// cpu when this function is called. Break the execution as soon as
		// possible and reset next time we're called.
		s_cpu_implementation_changed = true;
	}

	if (EmuConfig.Cpu.AffinityControlMode != old_config.Cpu.AffinityControlMode ||
		EmuConfig.Speedhacks.vuThread != old_config.Speedhacks.vuThread)
		SetEmuThreadAffinities();
}

void VMManager::CheckForGSConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.GS == old_config.GS)
		return;
	UpdateVSyncRate(true);
	MTGS::ApplySettings();
}

void VMManager::CheckForPatchConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.EnableCheats == old_config.EnableCheats &&
		EmuConfig.EnableWideScreenPatches == old_config.EnableWideScreenPatches &&
		EmuConfig.EnablePatches == old_config.EnablePatches)
		return;

	ReloadPatches();
}

void VMManager::CheckForDEV9ConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.DEV9 == old_config.DEV9)
		return;

	DEV9CheckChanges(old_config);
}

void VMManager::CheckForMemoryCardConfigChanges(const Pcsx2Config& old_config)
{
	bool changed = false;

	for (size_t i = 0; i < C89_ARRAY_SIZE(EmuConfig.Mcd); i++)
	{
		if (EmuConfig.Mcd[i].Enabled != old_config.Mcd[i].Enabled ||
			strcmp(EmuConfig.Mcd[i].Filename, old_config.Mcd[i].Filename) != 0)
		{
			changed = true;
			break;
		}
	}

	changed |= (EmuConfig.McdEnableEjection != old_config.McdEnableEjection);
	changed |= (EmuConfig.McdFolderAutoManage != old_config.McdFolderAutoManage);

	if (!changed)
		return;

	FileMcd_EmuClose();
	FileMcd_EmuOpen();

	// force card eject when files change
	for (u32 port = 0; port < 2; port++)
	{
		for (u32 slot = 0; slot < 4; slot++)
		{
			const uint index = FileMcd_ConvertToSlot(port, slot);
			if (EmuConfig.Mcd[index].Enabled != old_config.Mcd[index].Enabled ||
				strcmp(EmuConfig.Mcd[index].Filename, old_config.Mcd[index].Filename) != 0)
				AutoEject::Set(port, slot);
		}
	}

	// force reindexing, mc folder code is janky
	std::string sioSerial;
	{
		Threading::ScopedRecursiveLock lock(s_info_mutex);
		if (const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial))
			sioSerial = game->memcardFiltersAsString();
		if (sioSerial.empty())
			sioSerial = s_game_serial;
	}
	sioSetGameSerial(sioSerial.c_str());
}

void VMManager::CheckForConfigChanges(const Pcsx2Config& old_config)
{
	if (HasValidVM())
	{
		CheckForCPUConfigChanges(old_config);
		CheckForPatchConfigChanges(old_config);
		CheckForDEV9ConfigChanges(old_config);
		CheckForMemoryCardConfigChanges(old_config);
		USB::CheckForConfigChanges(old_config);

		if (EmuConfig.EnableCheats != old_config.EnableCheats ||
			EmuConfig.EnableWideScreenPatches != old_config.EnableWideScreenPatches ||
			EmuConfig.EnableNoInterlacingPatches != old_config.EnableNoInterlacingPatches)
			VMManager::ReloadPatches();

		CheckForGSConfigChanges(old_config);
	}
}

void VMManager::ApplySettings()
{
	// if we're running, ensure the threads are synced
	const bool running = ((VMState)retro_atomic_load_acquire_int(&s_state) == VMState::Running);
	if (running)
	{
		if (THREAD_VU1)
			vu1Thread.WaitVU();
		MTGS::WaitGS(false);
	}

	// Reset to a clean Pcsx2Config. Otherwise things which are optional (e.g. gamefixes)
	// do not use the correct default values when loading.
	Pcsx2Config old_config(std::move(EmuConfig));
	EmuConfig = Pcsx2Config();
	EmuConfig.CopyRuntimeConfig(old_config);
	LoadSettings();
	CheckForConfigChanges(old_config);
}

bool VMManager::g_MtvuMenuDefault = true;
bool VMManager::g_FastmemMenuDefault = true;

void VMManager::SetDefaultSettings(SettingsInterface& si)
{
	FPControlRegisterBackup fpcr_backup(FPControlRegister::GetDefault());

	Pcsx2Config temp_config;
	SettingsSaveWrapper ssw(si);
	temp_config.LoadSave(ssw);

	// Settings not part of the Pcsx2Config struct.
	si.SetBoolValue("EmuCore", "EnableFastBoot", true);

	SetHardwareDependentDefaultSettings(si);
}

#ifdef _WIN32

#include "common/RedtapeWindows.h"

static bool s_timer_resolution_increased = false;

void VMManager::SetTimerResolutionIncreased(bool enabled)
{
	if (s_timer_resolution_increased == enabled)
		return;

	if (enabled)
		s_timer_resolution_increased = (timeBeginPeriod(1) == TIMERR_NOERROR);
	else if (s_timer_resolution_increased)
	{
		timeEndPeriod(1);
		s_timer_resolution_increased = false;
	}
}

#else

void VMManager::SetTimerResolutionIncreased(bool enabled)
{
}

#endif

static std::vector<u32> s_processor_list;
static Threading::Mutex s_processor_list_mutex;
static bool s_processor_list_initialized = false;

static void InitializeCPUInfo(void)
{
	FPControlRegister::SetCurrent(FPControlRegister::GetDefault());

	// features_cpu ranks the processors for us: strongest core first, and an
	// SMT sibling behind the processor it shares a core with, which is the
	// order the affinity assignment below indexes into. Platforms that
	// publish no topology answer with the plain ascending order, and one that
	// cannot name its processors at all answers with nothing, leaving
	// affinity control switched off exactly as before.
	unsigned order[64];
	const size_t count = cpu_features_get_processor_order(order, C89_ARRAY_SIZE(order));
	if (count == 0)
	{
		Console.Error("No processor list available");
		return;
	}

	s_processor_list.reserve(count);
	std::stringstream ss;
	ss << "Ordered processor list: ";
	for (size_t i = 0; i < count; i++)
	{
		if (i != 0)
			ss << ", ";
		ss << order[i];
		s_processor_list.push_back(static_cast<u32>(order[i]));
	}
	Console.WriteLn("%s", ss.str().c_str());
}

/* The MTVU and instant-VU1 defaults keep the platform guard they had: the
 * processor list is available everywhere now, but which platforms write
 * these defaults is a separate question from how the CPU is queried. */
#if defined(__linux__) || defined(_WIN32)
static void SetMTVUAndAffinityControlDefault(SettingsInterface& si)
{
	VMManager::EnsureCPUInfoInitialized();
	// arm64 MTVU status: the worker thread spawns (InitializeCPUProviders) and
	// sets VPU_STAT busy around interpreter VU1 programs (MTVU.cpp), which makes
	// MTVU produce correct XGKICK output. However, games driving a CONTINUOUS
	// VU1 program (an endless microprogram streaming XGKICK packets, e.g. GT3's
	// arcade attract) deadlock the one-packet-per-program MTVU handoff with the
	// VU1 INTERPRETER: the worker drip-feeds gsPack mid-program until path1's
	// 9MB buffer fills (CopyGSPacketData -> WaitGS(true) with an empty
	// gsPackQueue), while MTGS sits in semaXGkick.Wait() for a program end that
	// never comes. Needs a partial-packet flush protocol (or a VU recompiler
	// with upstream's XGKICK handling) -- until C.28-4 MTVU was opt-in
	// (helps VU-light titles; MMX7 verified pixel-identical).
	// C.28-4: with microVU1 as the default VU1 provider (native codegen,
	// verified register-exact against the interpreter), MTVU and instant VU1 are
	// safe defaults again -- the C.13c livelock and the 3M-cycle-budget wall
	// were interpreter-provider problems, and microVU1 is now the only VU1
	// provider.
	const bool mtvu = VMManager::MtvuHardwareAllowed() && VMManager::g_MtvuMenuDefault;
	Console.WriteLn(mtvu ? "  MTVU enabled (pcsx2_mtvu; requires >= 3 hardware threads)."
	                     : "  MTVU disabled.");
	si.SetBoolValue("EmuCore/Speedhacks", "vuThread", mtvu);
	// Instant VU1 assumes the VU1 provider finishes a program quickly (x86
	// microVU). With the VU1 INTERPRETER, a continuous microprogram (endless
	// loop streaming XGKICK -- GT3's arcade attract) burns the full
	// vu1RunCycles=3M budget on EVERY kick: the EE thread spends seconds per
	// frame inside InterpVU1::Execute and the frontend appears hung. Run VU1
	// in small interleaved slices instead (upstream's non-instant scheduling).
	Console.WriteLn("  Instant VU1 enabled (microVU1 native provider).");
	si.SetBoolValue("EmuCore/Speedhacks", "vu1Instant", true);
	/* Fastmem follows the same shape as MTVU: this default-setter can run
	 * again on a settings reset after check_variables(true) has consumed
	 * the option, so the option's value has to ride a VMManager global to
	 * survive it. The truthful "which mode is this run in" line lives in
	 * LoadSettings, printed from the config the recompilers actually
	 * read - never from here, where the global may not be fed yet. */
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", VMManager::g_FastmemMenuDefault);
}

#else
static void SetMTVUAndAffinityControlDefault(SettingsInterface& si) { }
#endif

/* Single source of truth for the MTVU hardware gate.  The worker is only
 * worth spawning (and only spawned) with >= 3 hardware threads; every
 * writer of EmuCore/Speedhacks vuThread must apply this same predicate,
 * otherwise THREAD_VU1 can come up true with no worker alive and
 * MTGS::MainLoop waits on a handoff that never arrives (boot hang). */
bool VMManager::MtvuHardwareAllowed()
{
	return cpu_features_get_core_amount() >= 3;
}

void VMManager::EnsureCPUInfoInitialized()
{
	Threading::ScopedLock lock(s_processor_list_mutex);
	if (!s_processor_list_initialized)
	{
		InitializeCPUInfo();
		s_processor_list_initialized = true;
	}
}

void VMManager::SetEmuThreadAffinities()
{
	EnsureCPUInfoInitialized();

	// not supported on this platform
	if (s_processor_list.empty())
		return;

	if (EmuConfig.Cpu.AffinityControlMode == 0 ||
		s_processor_list.size() < (EmuConfig.Speedhacks.vuThread ? 3 : 2))
	{
		if (EmuConfig.Cpu.AffinityControlMode != 0)
			Console.Error("Insufficient processors for affinity control.");

		vu1Thread.GetThreadHandle().SetAffinity(0);
		s_vm_thread_handle.SetAffinity(0);
		return;
	}

	static constexpr u8 processor_assignment[7][2][3] = {
		//EE xx GS  EE VU GS
		{{0, 2, 1}, {0, 1, 2}}, // Disabled
		{{0, 2, 1}, {0, 1, 2}}, // EE > VU > GS
		{{0, 2, 1}, {0, 2, 1}}, // EE > GS > VU
		{{0, 2, 1}, {1, 0, 2}}, // VU > EE > GS
		{{1, 2, 0}, {2, 0, 1}}, // VU > GS > EE
		{{1, 2, 0}, {1, 2, 0}}, // GS > EE > VU
		{{1, 2, 0}, {2, 1, 0}}, // GS > VU > EE
	};

	// steal vu's thread if mtvu is off
	const u8* this_proc_assigment = processor_assignment[EmuConfig.Cpu.AffinityControlMode][EmuConfig.Speedhacks.vuThread];
	const u32 ee_index = s_processor_list[this_proc_assigment[0]];
	const u32 vu_index = s_processor_list[this_proc_assigment[1]];
	Console.WriteLn("Processor order assignment: EE=%u, VU=%u, GS=%u",
		this_proc_assigment[0], this_proc_assigment[1], this_proc_assigment[2]);

	const u64 ee_affinity = static_cast<u64>(1) << ee_index;
	Console.WriteLn("EE thread is on processor %u (0x%llx)", ee_index, (unsigned long long)ee_affinity);
	s_vm_thread_handle.SetAffinity(ee_affinity);

	if (EmuConfig.Speedhacks.vuThread)
	{
		const u64 vu_affinity = static_cast<u64>(1) << vu_index;
		Console.WriteLn("VU thread is on processor %u (0x%llx)", vu_index, (unsigned long long)vu_affinity);
		vu1Thread.GetThreadHandle().SetAffinity(vu_affinity);
	}
	else
		vu1Thread.GetThreadHandle().SetAffinity(0);
}

void VMManager::SetHardwareDependentDefaultSettings(SettingsInterface& si)
{
	SetMTVUAndAffinityControlDefault(si);
}

const std::vector<u32>& VMManager::GetSortedProcessorList()
{
	EnsureCPUInfoInitialized();
	return s_processor_list;
}
