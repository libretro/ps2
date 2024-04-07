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

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/Pcsx2Defs.h"
#include "Frontend/InputManager.h"

class SettingsInterface;

class InputSource
{
public:
	InputSource();
	virtual ~InputSource();

	virtual bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) = 0;
	virtual void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) = 0;
	virtual void Shutdown() = 0;

	virtual void PollEvents() = 0;
};
