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

#if defined(_WIN32)

#include "common/Threading.h"
#include "common/emitter/tools.h"
#include "common/RedtapeWindows.h"
#include <mmsystem.h>
#include <process.h>
#include <timeapi.h>

// For use in spin/wait loops,  Acts as a hint to Intel CPUs and should, in theory
// improve performance and reduce cpu power consumption.
__fi void Threading::SpinWait()
{
	_mm_pause();
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
{
	if (handle.m_native_handle)
	{
		HANDLE new_handle;
		if (DuplicateHandle(GetCurrentProcess(), (HANDLE)handle.m_native_handle,
				GetCurrentProcess(), &new_handle, THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, 0))
		{
			m_native_handle = (void*)new_handle;
		}
	}
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}


Threading::ThreadHandle::~ThreadHandle()
{
	if (m_native_handle)
		CloseHandle(m_native_handle);
}

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = (void*)OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	if (m_native_handle)
		CloseHandle((HANDLE)m_native_handle);
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	if (m_native_handle)
	{
		CloseHandle((HANDLE)m_native_handle);
		m_native_handle = nullptr;
	}

	HANDLE new_handle;
	if (DuplicateHandle(GetCurrentProcess(), (HANDLE)handle.m_native_handle,
			GetCurrentProcess(), &new_handle, THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, 0))
	{
		m_native_handle = (void*)new_handle;
	}

	return *this;
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	if (processor_mask == 0)
		processor_mask = ~processor_mask;

	return (SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)processor_mask) != 0 || GetLastError() != ERROR_SUCCESS);
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread)
	: ThreadHandle(thread)
	, m_stack_size(thread.m_stack_size)
{
	thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func)
	: ThreadHandle()
{
	Start(std::move(func));
}

Threading::Thread::~Thread()
{
}

unsigned Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	(*entry.get())();
	return 0;
}

bool Threading::Thread::Start(EntryPoint func)
{
	std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));
	unsigned thread_id;
	m_native_handle = reinterpret_cast<void*>(_beginthreadex(nullptr, m_stack_size, ThreadProc, func_clone.get(), 0, &thread_id));
	if (!m_native_handle)
		return false;

	// thread started, it'll release the memory
	func_clone.release();
	return true;
}

void Threading::Thread::Detach()
{
	CloseHandle((HANDLE)m_native_handle);
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	WaitForSingleObject((HANDLE)m_native_handle, INFINITE);
	CloseHandle((HANDLE)m_native_handle);
	m_native_handle = nullptr;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size = thread.m_stack_size;
	thread.m_stack_size = 0;
	return *this;
}
#endif
