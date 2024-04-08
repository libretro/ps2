/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022 PCSX2 Dev Team
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
#include "common/Assertions.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"
#include "common/Threading.h"
#include "Frontend/CommonHost.h"
#include "Frontend/LayeredSettingsInterface.h"
#include "Frontend/InputManager.h"
#include "Frontend/LogSink.h"
#include "GS.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "Host.h"
#include "HostSettings.h"
#include "MemoryCardFile.h"
#include "PAD/Host/PAD.h"
#include "PerformanceMetrics.h"
#include "Sio.h"
#include "VMManager.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

namespace CommonHost
{
	static void SetCommonDefaultSettings(SettingsInterface& si);
} // namespace CommonHost

bool CommonHost::InitializeCriticalFolders()
{
	// logging of directories in case something goes wrong super early
	Console.WriteLn("AppRoot Directory: %s", EmuFolders::AppRoot.c_str());
	Console.WriteLn("DataRoot Directory: %s", EmuFolders::DataRoot.c_str());
	Console.WriteLn("Resources Directory: %s", EmuFolders::Resources.c_str());

	// the resources directory should exist, bail out if not
	if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
	{
		Console.Error("Resources directory is missing.");
		return false;
	}

	return true;
}

void CommonHost::LoadStartupSettings()
{
	SettingsInterface* bsi = Host::Internal::GetBaseSettingsLayer();
	EmuFolders::LoadConfig(*bsi);
	EmuFolders::EnsureFoldersExist();
	UpdateLogging(*bsi);
}

void CommonHost::SetDefaultSettings(SettingsInterface& si, bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	if (folders)
		EmuFolders::SetDefaults(si);
	if (core)
	{
		VMManager::SetDefaultSettings(si);
		SetCommonDefaultSettings(si);
	}
	if (ui)
		Host::SetDefaultUISettings(si);
}

void CommonHost::SetCommonDefaultSettings(SettingsInterface& si)
{
	SetDefaultLoggingSettings(si);
}

void CommonHost::CPUThreadInitialize()
{
	Threading::SetNameOfCurrentThread("CPU Thread");
	PerformanceMetrics::SetCPUThread(Threading::ThreadHandle::GetForCallingThread());

	// neither of these should ever fail.
	if (!VMManager::Internal::InitializeGlobals() || !VMManager::Internal::InitializeMemory())
		pxFailRel("Failed to allocate memory map");

	// We want settings loaded so we choose the correct renderer for big picture mode.
	// This also sorts out input sources.
	VMManager::LoadSettings();
}

void CommonHost::CPUThreadShutdown()
{
	InputManager::CloseSources();
	VMManager::WaitForSaveStateFlush();
	VMManager::Internal::ReleaseMemory();
	VMManager::Internal::ReleaseGlobals();
	PerformanceMetrics::SetCPUThread(Threading::ThreadHandle());
}

void CommonHost::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
	SettingsInterface* binding_si = Host::GetSettingsInterfaceForBindings();
	InputManager::ReloadSources(si, lock);
	InputManager::ReloadBindings(si, *binding_si);

	UpdateLogging(si);
}

void CommonHost::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

void CommonHost::OnVMStarting()
{
}

void CommonHost::OnVMStarted()
{
}

void CommonHost::OnVMDestroyed()
{
}

void CommonHost::OnVMPaused()
{
	InputManager::PauseVibration();
}

void CommonHost::OnVMResumed()
{
}

static void stub_OnRunningGameChanged(std::string path, std::string serial, std::string title, u32 crc)
{
}

void CommonHost::OnGameChanged(const std::string& disc_path, const std::string& elf_override, const std::string& game_serial,
	const std::string& game_name, u32 game_crc)
{
		GetMTGS().RunOnGSThread([disc_path, game_serial, game_name, game_crc]() {
			stub_OnRunningGameChanged(std::move(disc_path), std::move(game_serial), std::move(game_name), game_crc);
		});
}

void CommonHost::CPUThreadVSync()
{
	InputManager::PollSources();
}
