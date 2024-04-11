/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include "Threading.h"
#include "General.h"
#include "Exceptions.h"

#include "fmt/core.h"

#ifdef _WIN32
#include "RedtapeWindows.h"
#include <intrin.h>
#endif

#ifdef __UNIX__
#include <signal.h>
#endif

// --------------------------------------------------------------------------------------
//  BaseException  (implementations)
// --------------------------------------------------------------------------------------

BaseException& BaseException::SetDiagMsg(std::string msg_diag)
{
	m_message_diag = std::move(msg_diag);
	return *this;
}

BaseException& BaseException::SetUserMsg(std::string msg_user)
{
	m_message_user = std::move(msg_user);
	return *this;
}

// --------------------------------------------------------------------------------------
//  Exception::RuntimeError   (implementations)
// --------------------------------------------------------------------------------------
Exception::RuntimeError::RuntimeError(const std::runtime_error& ex, const char* prefix /* = nullptr */)
{
	IsSilent = false;

	const bool has_prefix = prefix && prefix[0] != 0;

	SetDiagMsg(fmt::format("STL Runtime Error{}{}{}: {}",
		has_prefix ? " (" : "", prefix ? prefix : "", has_prefix ? ")" : "",
		ex.what()));
}

Exception::RuntimeError::RuntimeError(const std::exception& ex, const char* prefix /* = nullptr */)
{
	IsSilent = false;

	const bool has_prefix = prefix && prefix[0] != 0;

	SetDiagMsg(fmt::format("STL Exception{}{}{}: {}",
		has_prefix ? " (" : "", prefix ? prefix : "", has_prefix ? ")" : "",
		ex.what()));
}

void Exception::BadStream::_formatDiagMsg(std::string& dest) const
{
	fmt::format_to(std::back_inserter(dest), "Path: ");
	if (!StreamName.empty())
		fmt::format_to(std::back_inserter(dest), "{}", StreamName);
	else
		dest += "[Unnamed or unknown]";

	if (!m_message_diag.empty())
		fmt::format_to(std::back_inserter(dest), "\n{}", m_message_diag);
}

void Exception::BadStream::_formatUserMsg(std::string& dest) const
{
	fmt::format_to(std::back_inserter(dest), "Path: ");
	if (!StreamName.empty())
		fmt::format_to(std::back_inserter(dest), "{}", StreamName);
	else
		dest += "[Unnamed or unknown]";

	if (!m_message_user.empty())
		fmt::format_to(std::back_inserter(dest), "\n{}", m_message_user);
}
