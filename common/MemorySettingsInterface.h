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

#pragma once
#include "SettingsInterface.h"
#include <unordered_map>
#include <string>

class MemorySettingsInterface final : public SettingsInterface
{
public:
	MemorySettingsInterface();
	~MemorySettingsInterface();

	bool GetIntValue(const char* section, const char* key, s32* value) const override;
	bool GetUIntValue(const char* section, const char* key, u32* value) const override;
	bool GetFloatValue(const char* section, const char* key, float* value) const override;
	bool GetDoubleValue(const char* section, const char* key, double* value) const override;
	bool GetBoolValue(const char* section, const char* key, bool* value) const override;
	bool GetStringValue(const char* section, const char* key, std::string* value) const override;

	void SetIntValue(const char* section, const char* key, s32 value) override;
	void SetUIntValue(const char* section, const char* key, u32 value) override;
	void SetFloatValue(const char* section, const char* key, float value) override;
	void SetDoubleValue(const char* section, const char* key, double value) override;
	void SetBoolValue(const char* section, const char* key, bool value) override;
	void SetStringValue(const char* section, const char* key, const char* value) override;

	void DeleteValue(const char* section, const char* key) override;

	// default parameter overloads
	using SettingsInterface::GetBoolValue;
	using SettingsInterface::GetDoubleValue;
	using SettingsInterface::GetFloatValue;
	using SettingsInterface::GetIntValue;
	using SettingsInterface::GetStringValue;
	using SettingsInterface::GetUIntValue;

private:
	using KeyMap = std::unordered_multimap<std::string, std::string>;

	void SetValue(const char* section, const char* key, std::string value);

	std::unordered_map<std::string, KeyMap> m_sections;
};
