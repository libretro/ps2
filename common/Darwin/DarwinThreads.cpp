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

#if defined(__APPLE__)

#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>

#include "common/PrecompiledHeader.h"
#include "common/Threading.h"

// For use in spin/wait loops, acts as a hint to Intel CPUs and should, in theory
// improve performance and reduce cpu power consumption.
__forceinline void Threading::SpinWait()
{
	// If this doesn't compile you can just comment it out (it only serves as a
	// performance hint and isn't required).
#if defined(_M_X86)
	__asm__("pause");
#elif defined(_M_ARM64)
	__asm__ __volatile__("isb");
#endif
}

Threading::ThreadHandle::ThreadHandle() = default;

Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
	: m_native_handle(handle.m_native_handle)
{
}

Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
	: m_native_handle(handle.m_native_handle)
{
	handle.m_native_handle = nullptr;
}

Threading::ThreadHandle::~ThreadHandle() = default;

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
	ThreadHandle ret;
	ret.m_native_handle = pthread_self();
	return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
	m_native_handle = handle.m_native_handle;
	handle.m_native_handle = nullptr;
	return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
	m_native_handle = handle.m_native_handle;
	return *this;
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
	// Doesn't appear to be possible to set affinity.
	return false;
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

void Threading::Thread::Detach()
{
	pthread_detach((pthread_t)m_native_handle);
	m_native_handle = nullptr;
}

void Threading::Thread::Join()
{
	void* retval;
	pthread_join((pthread_t)m_native_handle, &retval);
	m_native_handle = nullptr;
}
#endif
