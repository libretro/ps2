/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014  PCSX2 Dev Team
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

#if !defined(_WIN32) && !defined(__APPLE__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#endif

#include "Threading.h"

#ifdef _WIN32
#include "RedtapeWindows.h"
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <memory>

#ifdef _WIN32
#include <mmsystem.h>
#include <process.h>
#include <timeapi.h>
#else
#include <sched.h>
#include <pthread.h>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/types.h>
#include <sched.h>

// glibc < v2.30 doesn't define gettid...
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif

#elif defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>
#elif defined(__unix__)
#include <pthread_np.h>
#endif
#endif

Threading::ThreadHandle::ThreadHandle() = default;

#ifdef _WIN32
Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
{
	if (handle.m_native_handle)
	{
		HANDLE new_handle;
		if (DuplicateHandle(GetCurrentProcess(),
				(HANDLE)handle.m_native_handle,
				GetCurrentProcess(),
				&new_handle,
				  THREAD_QUERY_INFORMATION 
				| THREAD_SET_LIMITED_INFORMATION,
				FALSE, 0))
			m_native_handle = (void*)new_handle;
	}
}
#else
Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
#ifdef __linux__
	, m_native_id(handle.m_native_id)
#endif
{
}
#endif

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
#ifdef __linux__
	, m_native_id(handle.m_native_id)
#endif
{
	handle.m_native_handle = nullptr;
#ifdef __linux__
	handle.m_native_id     = 0;
#endif
}

Threading::ThreadHandle::~ThreadHandle()
{
#ifdef _WIN32
	if (m_native_handle)
		CloseHandle(m_native_handle);
#endif
}

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
#ifdef _WIN32
	ret.m_native_handle = (void*)OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
#else
	ret.m_native_handle = (void*)pthread_self();
#ifdef __linux__
	ret.m_native_id     = gettid();
#endif
#endif
	return ret;
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
	ThreadHandle::operator=(thread);
	m_stack_size           = thread.m_stack_size;
	thread.m_stack_size    = 0;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
#ifdef _WIN32
	if (m_native_handle)
		CloseHandle((HANDLE)m_native_handle);
#endif
	m_native_handle        = handle.m_native_handle;
	handle.m_native_handle = nullptr;
#ifdef __linux__
	m_native_id            = handle.m_native_id;
	handle.m_native_id     = 0;
#endif
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
#ifdef _WIN32
	HANDLE new_handle;
	if (m_native_handle)
	{
		CloseHandle((HANDLE)m_native_handle);
		m_native_handle = nullptr;
	}

	if (DuplicateHandle(GetCurrentProcess(),
			(HANDLE)handle.m_native_handle,
			GetCurrentProcess(),
			&new_handle,
			  THREAD_QUERY_INFORMATION 
			| THREAD_SET_LIMITED_INFORMATION,
			FALSE, 0))
		m_native_handle = (void*)new_handle;
#else
	m_native_handle         = handle.m_native_handle;
#ifdef __linux__
	m_native_id             = handle.m_native_id;
#endif
#endif
	return *this;
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
#if defined(_WIN32)
	if (processor_mask == 0)
		processor_mask = ~processor_mask;

	return (SetThreadAffinityMask(GetCurrentThread(),
		          (DWORD_PTR)processor_mask) != 0 
			|| GetLastError() != ERROR_SUCCESS);
#elif defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);

	if (processor_mask != 0)
	{
		for (u32 i = 0; i < 64; i++)
		{
			if (processor_mask & (static_cast<u64>(1) << i))
			{
				CPU_SET(i, &set);
			}
		}
	}
	else
	{
		long num_processors = sysconf(_SC_NPROCESSORS_CONF);
		for (long i = 0; i < num_processors; i++)
		{
			CPU_SET(i, &set);
		}
	}

	return sched_setaffinity((pid_t)m_native_id, sizeof(set), &set) >= 0;
#else
	// Doesn't appear to be possible to set affinity.
	return false;
#endif
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

Threading::Thread::~Thread() { }

#if defined(_WIN32)
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

#elif defined(__linux__)
// For Linux, we have to do a bit of trickery here to get 
// the thread's ID back from the thread itself, because 
// it's not part of pthreads. We use a semaphore to signal
// when the thread has started, and filled in thread_id_ptr.
struct ThreadProcParameters
{
	Threading::Thread::EntryPoint func;
	Threading::KernelSemaphore* start_semaphore;
	unsigned int* thread_id_ptr;
};

void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<ThreadProcParameters> entry(static_cast<ThreadProcParameters*>(param));
	*entry->thread_id_ptr = gettid();
	entry->start_semaphore->Post();
	entry->func();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	pthread_attr_t attrs;
	KernelSemaphore start_semaphore;
	std::unique_ptr<ThreadProcParameters> params(std::make_unique<ThreadProcParameters>());
	params->func = std::move(func);
	params->start_semaphore = &start_semaphore;
	params->thread_id_ptr   = &m_native_id;

	bool has_attributes     = false;

	if (m_stack_size != 0)
	{
		has_attributes  = true;
		pthread_attr_init(&attrs);
	}
	if (m_stack_size != 0)
		pthread_attr_setstacksize(&attrs, m_stack_size);

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, params.get());
	if (res != 0)
		return false;

	// wait until it sets our native id
	start_semaphore.Wait();

	// thread started, it'll release the memory
	m_native_handle = (void*)handle;
	params.release();
	return true;
}
#else
void* Threading::Thread::ThreadProc(void* param)
{
	std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
	(*entry.get())();
	return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
	std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));

	pthread_attr_t attrs;
	bool has_attributes = false;

	if (m_stack_size != 0)
	{
		has_attributes = true;
		pthread_attr_init(&attrs);
	}
	if (m_stack_size != 0)
		pthread_attr_setstacksize(&attrs, m_stack_size);

	pthread_t handle;
	const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, func_clone.get());
	if (res != 0)
		return false;

	// thread started, it'll release the memory
	m_native_handle = (void*)handle;
	func_clone.release();
	return true;
}
#endif

void Threading::Thread::Detach()
{
#ifdef _WIN32
	CloseHandle((HANDLE)m_native_handle);
#else
	pthread_detach((pthread_t)m_native_handle);
#ifdef __linux__
	m_native_id     = 0;
#endif
#endif
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
#ifdef _WIN32
	WaitForSingleObject((HANDLE)m_native_handle, INFINITE);
	CloseHandle((HANDLE)m_native_handle);
#else
	void* retval;
	pthread_join((pthread_t)m_native_handle, &retval);
#ifdef __linux__
	m_native_id     = 0;
#endif
#endif
	m_native_handle = nullptr;
}
