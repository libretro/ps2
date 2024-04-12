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
#include <string_view>
#include <optional>
#include <vector>

enum class VsyncMode;

namespace Host
{
	/// Typical durations for OSD messages.
	static constexpr float OSD_CRITICAL_ERROR_DURATION = 20.0f;
	static constexpr float OSD_ERROR_DURATION = 15.0f;
	static constexpr float OSD_WARNING_DURATION = 10.0f;
	static constexpr float OSD_INFO_DURATION = 5.0f;
	static constexpr float OSD_QUICK_DURATION = 2.5f;

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
