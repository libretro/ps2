/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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
#include <algorithm>

#include "SettingsWrapper.h"
#include "Console.h"

static int _calcEnumLength(const char* const* enumArray)
{
	int cnt = 0;
	while (*enumArray != nullptr)
	{
		enumArray++;
		cnt++;
	}

	return cnt;
}

SettingsWrapper::SettingsWrapper(SettingsInterface& si)
	: m_si(si)
{
}

SettingsLoadWrapper::SettingsLoadWrapper(SettingsInterface& si)
	: SettingsWrapper(si)
{
}

bool SettingsLoadWrapper::IsLoading() const
{
	return true;
}

bool SettingsLoadWrapper::IsSaving() const
{
	return false;
}

void SettingsLoadWrapper::Entry(const char* section, const char* var, int& value, const int defvalue /*= 0*/)
{
	value = m_si.GetIntValue(section, var, defvalue);
}

void SettingsLoadWrapper::Entry(const char* section, const char* var, uint& value, const uint defvalue /*= 0*/)
{
	value = m_si.GetUIntValue(section, var, defvalue);
}

void SettingsLoadWrapper::Entry(const char* section, const char* var, bool& value, const bool defvalue /*= false*/)
{
	value = m_si.GetBoolValue(section, var, defvalue);
}

void SettingsLoadWrapper::Entry(const char* section, const char* var, float& value, const float defvalue /*= 0.0*/)
{
	value = m_si.GetFloatValue(section, var, defvalue);
}

void SettingsLoadWrapper::Entry(const char* section, const char* var, std::string& value, const std::string& default_value /*= std::string()*/)
{
	if (!m_si.GetStringValue(section, var, &value) && &value != &default_value)
		value = default_value;
}

void SettingsLoadWrapper::EntryBuf(const char* section, const char* var, char* value, size_t value_size, const char* default_value)
{
	if (!m_si.GetStringValueBuf(section, var, value, value_size))
		strlcpy(value, default_value, value_size);
}

void SettingsLoadWrapper::_EnumEntry(const char* section, const char* var, int& value, const char* const* enumArray, int defvalue)
{
	const int cnt = _calcEnumLength(enumArray);
	defvalue = std::min(std::max(defvalue, 0), cnt);

	const std::string retval(m_si.GetStringValue(section, var, enumArray[defvalue]));

	int i = 0;
	while (enumArray[i] != nullptr && (retval != enumArray[i]))
		i++;

	if (enumArray[i] == nullptr)
	{
		Console.Warning("(LoadSettings) Warning: Unrecognized value '%s' on key '%s'\n\tUsing the default setting of '%s'.",
			retval.c_str(), var, enumArray[defvalue]);
		value = defvalue;
	}
	else
	{
		value = i;
	}
}

bool SettingsLoadWrapper::EntryBitBool(const char* section, const char* var, bool value, const bool defvalue /*= false*/)
{
	return m_si.GetBoolValue(section, var, defvalue);
}

int SettingsLoadWrapper::EntryBitfield(const char* section, const char* var, int value, const int defvalue /*= 0*/)
{
	return m_si.GetIntValue(section, var, defvalue);
}

SettingsSaveWrapper::SettingsSaveWrapper(SettingsInterface& si)
	: SettingsWrapper(si)
{
}

bool SettingsSaveWrapper::IsLoading() const
{
	return false;
}

bool SettingsSaveWrapper::IsSaving() const
{
	return true;
}

void SettingsSaveWrapper::Entry(const char* section, const char* var, int& value, const int defvalue /*= 0*/)
{
	m_si.SetIntValue(section, var, value);
}

void SettingsSaveWrapper::Entry(const char* section, const char* var, uint& value, const uint defvalue /*= 0*/)
{
	m_si.SetUIntValue(section, var, value);
}

void SettingsSaveWrapper::Entry(const char* section, const char* var, bool& value, const bool defvalue /*= false*/)
{
	m_si.SetBoolValue(section, var, value);
}

void SettingsSaveWrapper::Entry(const char* section, const char* var, float& value, const float defvalue /*= 0.0*/)
{
	m_si.SetFloatValue(section, var, value);
}

void SettingsSaveWrapper::Entry(const char* section, const char* var, std::string& value, const std::string& default_value /*= std::string()*/)
{
	m_si.SetStringValue(section, var, value.c_str());
}

void SettingsSaveWrapper::EntryBuf(const char* section, const char* var, char* value, size_t value_size, const char* default_value)
{
	(void)value_size;
	(void)default_value;
	m_si.SetStringValue(section, var, value);
}

bool SettingsSaveWrapper::EntryBitBool(const char* section, const char* var, bool value, const bool defvalue /*= false*/)
{
	m_si.SetBoolValue(section, var, value);
	return value;
}

int SettingsSaveWrapper::EntryBitfield(const char* section, const char* var, int value, const int defvalue /*= 0*/)
{
	m_si.SetIntValue(section, var, value);
	return value;
}

void SettingsSaveWrapper::_EnumEntry(const char* section, const char* var, int& value, const char* const* enumArray, int defvalue)
{
	const int cnt = _calcEnumLength(enumArray);
	const int index = (value < 0 || value >= cnt) ? defvalue : value;
	m_si.SetStringValue(section, var, enumArray[index]);
}
