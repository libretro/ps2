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

#include "LayeredSettingsInterface.h"

#include <unordered_set>

LayeredSettingsInterface::LayeredSettingsInterface() = default;

LayeredSettingsInterface::~LayeredSettingsInterface() = default;

bool LayeredSettingsInterface::GetIntValue(const char* section, const char* key, int* value) const
{
	for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
	{
		if (SettingsInterface* sif = m_layers[layer])
		{
			if (sif->GetIntValue(section, key, value))
				return true;
		}
	}

	return false;
}

bool LayeredSettingsInterface::GetUIntValue(const char* section, const char* key, uint* value) const
{
	for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
	{
		if (SettingsInterface* sif = m_layers[layer])
		{
			if (sif->GetUIntValue(section, key, value))
				return true;
		}
	}

	return false;
}

bool LayeredSettingsInterface::GetFloatValue(const char* section, const char* key, float* value) const
{
	for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
	{
		if (SettingsInterface* sif = m_layers[layer])
		{
			if (sif->GetFloatValue(section, key, value))
				return true;
		}
	}

	return false;
}

bool LayeredSettingsInterface::GetDoubleValue(const char* section, const char* key, double* value) const
{
	for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
	{
		if (SettingsInterface* sif = m_layers[layer])
		{
			if (sif->GetDoubleValue(section, key, value))
				return true;
		}
	}

	return false;
}

bool LayeredSettingsInterface::GetBoolValue(const char* section, const char* key, bool* value) const
{
	for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
	{
		if (SettingsInterface* sif = m_layers[layer])
		{
			if (sif->GetBoolValue(section, key, value))
				return true;
		}
	}

	return false;
}

bool LayeredSettingsInterface::GetStringValue(const char* section, const char* key, std::string* value) const
{
	for (u32 layer = FIRST_LAYER; layer <= LAST_LAYER; layer++)
	{
		if (SettingsInterface* sif = m_layers[layer])
		{
			if (sif->GetStringValue(section, key, value))
				return true;
		}
	}

	return false;
}

void LayeredSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
}

void LayeredSettingsInterface::SetUIntValue(const char* section, const char* key, uint value)
{
}

void LayeredSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
}

void LayeredSettingsInterface::SetDoubleValue(const char* section, const char* key, double value)
{
}

void LayeredSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
}

void LayeredSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
}

void LayeredSettingsInterface::DeleteValue(const char* section, const char* key)
{
}
