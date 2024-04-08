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

#include "PrecompiledHeader.h"

#include "Frontend/LogSink.h"
#include "HostSettings.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SettingsInterface.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

void CommonHost::UpdateLogging(SettingsInterface& si)
{
}

void CommonHost::SetDefaultLoggingSettings(SettingsInterface& si)
{
	si.SetBoolValue("Logging", "EnableSystemConsole", false);
	si.SetBoolValue("Logging", "EnableFileLogging", false);
	si.SetBoolValue("Logging", "EnableTimestamps", true);
	si.SetBoolValue("Logging", "EnableVerbose", false);
	si.SetBoolValue("Logging", "EnableEEConsole", false);
	si.SetBoolValue("Logging", "EnableIOPConsole", false);
	si.SetBoolValue("Logging", "EnableInputRecordingLogs", true);
	si.SetBoolValue("Logging", "EnableControllerLogs", false);
}
