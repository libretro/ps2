/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#pragma once

#include "common/Pcsx2Defs.h"

#include <ctime>
#include <functional>
#include <string>
#include <optional>
#include <vector>
#include <mutex>

class SettingsInterface;

namespace Host
{
	// Base setting retrieval, bypasses layers.
	std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value = "");
	bool GetBaseBoolSettingValue(const char* section, const char* key, bool default_value = false);
	int GetBaseIntSettingValue(const char* section, const char* key, int default_value = 0);
	uint GetBaseUIntSettingValue(const char* section, const char* key, uint default_value = 0);
	float GetBaseFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
	double GetBaseDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
	std::vector<std::string> GetBaseStringListSetting(const char* section, const char* key);

	// Allows the emucore to write settings back to the frontend. Use with care.
	// You should call CommitBaseSettingChanges() after finishing writing, or it may not be written to disk.
	void SetBaseBoolSettingValue(const char* section, const char* key, bool value);
	void SetBaseIntSettingValue(const char* section, const char* key, int value);
	void SetBaseUIntSettingValue(const char* section, const char* key, uint value);
	void SetBaseFloatSettingValue(const char* section, const char* key, float value);
	void SetBaseStringSettingValue(const char* section, const char* key, const char* value);
	void SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values);
	bool AddBaseValueToStringList(const char* section, const char* key, const char* value);
	bool RemoveBaseValueFromStringList(const char* section, const char* key, const char* value);
	void RemoveBaseSettingValue(const char* section, const char* key);
	void CommitBaseSettingChanges();

	// Settings access, thread-safe.
	std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "");
	bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false);
	int GetIntSettingValue(const char* section, const char* key, int default_value = 0);
	uint GetUIntSettingValue(const char* section, const char* key, uint default_value = 0);
	float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
	double GetDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);
	std::vector<std::string> GetStringListSetting(const char* section, const char* key);

	/// Direct access to settings interface. Must hold the lock when calling GetSettingsInterface() and while using it.
	std::unique_lock<std::mutex> GetSettingsLock();
	SettingsInterface* GetSettingsInterface();

	/// Returns the settings interface that controller bindings should be loaded from.
	/// If an input profile is being used, this will be the input layer, otherwise the layered interface.
	SettingsInterface* GetSettingsInterfaceForBindings();

	namespace Internal
	{
		/// Retrieves the base settings layer. Must call with lock held.
		SettingsInterface* GetBaseSettingsLayer();

		/// Sets the base settings layer. Should be called by the host at initialization time.
		void SetBaseSettingsLayer(SettingsInterface* sif);

	} // namespace Internal
	/// Reads a file from the resources directory of the application.
	/// This may be outside of the "normal" filesystem on platforms such as Mac.
	std::optional<std::vector<u8>> ReadResourceFile(const char* filename);

	/// Reads a resource file file from the resources directory as a string.
	std::optional<std::string> ReadResourceFileToString(const char* filename);

	/// Requests settings reset. Can be called from any thread, will call back and apply on the CPU thread.
	bool RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui);

	/// Safely executes a function on the VM thread.
	void RunOnCPUThread(std::function<void()> function, bool block = false);

	/// Requests shut down of the current virtual machine.
	void RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state);
} // namespace Host
