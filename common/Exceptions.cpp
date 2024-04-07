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
#include "CrashHandler.h"

#include <mutex>
#include "fmt/core.h"

#ifdef _WIN32
#include "RedtapeWindows.h"
#include <intrin.h>
#include <tlhelp32.h>
#endif

#ifdef __UNIX__
#include <signal.h>
#endif

static std::mutex s_assertion_failed_mutex;

static inline void FreezeThreads(void** handle)
{
#if defined(_WIN32)
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot != INVALID_HANDLE_VALUE)
	{
		THREADENTRY32 threadEntry;
		if (Thread32First(snapshot, &threadEntry))
		{
			do
			{
				if (threadEntry.th32ThreadID == GetCurrentThreadId())
					continue;

				HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.th32ThreadID);
				if (hThread != nullptr)
				{
					SuspendThread(hThread);
					CloseHandle(hThread);
				}
			} while (Thread32Next(snapshot, &threadEntry));
		}
	}

	*handle = static_cast<void*>(snapshot);
#else
	* handle = nullptr;
#endif
}

static inline void ResumeThreads(void* handle)
{
#if defined(_WIN32)
	if (handle != INVALID_HANDLE_VALUE)
	{
		THREADENTRY32 threadEntry;
		if (Thread32First(reinterpret_cast<HANDLE>(handle), &threadEntry))
		{
			do
			{
				if (threadEntry.th32ThreadID == GetCurrentThreadId())
					continue;

				HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.th32ThreadID);
				if (hThread != nullptr)
				{
					ResumeThread(hThread);
					CloseHandle(hThread);
				}
			} while (Thread32Next(reinterpret_cast<HANDLE>(handle), &threadEntry));
		}
		CloseHandle(reinterpret_cast<HANDLE>(handle));
	}
#else
#endif
}

void pxOnAssertFail(const char* file, int line, const char* func, const char* msg)
{
	std::unique_lock guard(s_assertion_failed_mutex);

	void* handle;
	FreezeThreads(&handle);

	char full_msg[512];
	std::snprintf(full_msg, sizeof(full_msg), "%s:%d: assertion failed in function %s: %s\n", file, line, func, msg);

#if defined(_WIN32)
	HANDLE error_handle = GetStdHandle(STD_ERROR_HANDLE);
	if (error_handle != INVALID_HANDLE_VALUE)
		WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), full_msg, static_cast<DWORD>(std::strlen(full_msg)), NULL, NULL);
	OutputDebugStringA(full_msg);

	std::snprintf(
		full_msg, sizeof(full_msg),
		"Assertion failed in function %s (%s:%d):\n\n%s\n\nPress Abort to exit, Retry to break to debugger, or Ignore to attempt to continue.",
		func, file, line, msg);

	int result = MessageBoxA(NULL, full_msg, NULL, MB_ABORTRETRYIGNORE | MB_ICONERROR);
	if (result == IDRETRY)
	{
		__debugbreak();
	}
	else if (result != IDIGNORE)
	{
		// try to save a crash dump before exiting
		CrashHandler::WriteDumpForCaller();
		TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
	}
#else
	fputs(full_msg, stderr);
	fputs("\nAborting application.\n", stderr);
	fflush(stderr);
	AbortWithMessage(full_msg);
#endif

	ResumeThreads(handle);
}

// --------------------------------------------------------------------------------------
//  BaseException  (implementations)
// --------------------------------------------------------------------------------------

BaseException& BaseException::SetBothMsgs(const char* msg_diag)
{
	m_message_user = msg_diag ? std::string(msg_diag) : std::string();
	return SetDiagMsg(msg_diag);
}

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
