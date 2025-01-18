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

#include "../common/Pcsx2Defs.h"

#include <ctime>
#include <memory>
#include <string>
#include <optional>
#include <vector>
#include <mutex>

class SettingsInterface;

namespace Host
{
	// Base setting retrieval, bypasses layers.
	std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value = "");

	// Allows the emucore to write settings back to the frontend. Use with care.
	
	// Settings access, thread-safe.
	bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false);
	int GetIntSettingValue(const char* section, const char* key, int default_value = 0);
	uint GetUIntSettingValue(const char* section, const char* key, uint default_value = 0);
	float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f);
	double GetDoubleSettingValue(const char* section, const char* key, double default_value = 0.0);

	/// Direct access to settings interface. Must hold the lock when calling GetSettingsInterface() and while using it.
	SettingsInterface* GetSettingsInterface();

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
} // namespace Host
