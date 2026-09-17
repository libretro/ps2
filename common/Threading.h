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

#pragma once

#include <retro_atomic.h>
#include "Pcsx2Defs.h"
#include "General.h"

#if defined(__APPLE__)
#include <mach/semaphore.h>
#elif !defined(_WIN32)
#include <semaphore.h>
#endif

#include <functional>

namespace Threading
{
	// --------------------------------------------------------------------------------------
	//  Platform Specific External APIs
	// --------------------------------------------------------------------------------------
	// The following set of documented functions have Linux/Win32 specific implementations,
	// which are found in WinThreads.cpp and LnxThreads.cpp

	// --------------------------------------------------------------------------------------
	//  ThreadHandle
	// --------------------------------------------------------------------------------------
	// Abstracts an OS's handle to a thread, closing the handle when necessary. Currently,
	// only used for getting the CPU time for a thread.
	//
	/// Yield the rest of this timeslice to the scheduler (sched_yield /
	/// SwitchToThread); for bounded producer spins on full queues.


}

/* Opaque rthreads primitives; definitions live in rthreads.h, which only
 * Threads.cpp needs to see. */
extern "C" {
	typedef struct slock slock_t;
	typedef struct scond scond_t;
}

namespace Threading
{


	/// A semaphore that may not have a fast userspace path
	/// (Used in other semaphore-based algorithms where the semaphore is just used for its thread sleep/wake ability)
	class KernelSemaphore
	{
#if defined(_WIN32)
		void* m_sema;
#elif defined(__APPLE__)
		semaphore_t m_sema;
#else
		sem_t m_sema;
#endif
	public:
		KernelSemaphore();
		~KernelSemaphore();
		void Post();
		/// Wait with a timeout.  Returns true if the semaphore was
		/// acquired, false on timeout.
		void Wait();
	};


	/// A semaphore that definitely has a fast userspace path
	class UserspaceSemaphore
	{
#if !defined(__aarch64__)
		// On aarch64 we use WFE/SEV-based parking via m_counter alone;
		// the kernel semaphore is unreachable and would just waste space
		// + run a destructor on every UserspaceSemaphore teardown.
		KernelSemaphore m_sema;
#endif
		retro_atomic_int_t m_counter = RETRO_ATOMIC_INT_INITIALIZER(0);

	public:
		UserspaceSemaphore() = default;
		~UserspaceSemaphore() = default;

#if defined(__aarch64__)
		void Post()
		{
			retro_atomic_fetch_add_int(&m_counter, 1);
			__asm__ __volatile__("sev" ::: "memory");
		}

		void Wait()
		{
			int32_t val, res;
			int32_t* ptr = reinterpret_cast<int32_t*>(&m_counter);
			__asm__ __volatile__("sevl");
			for (;;)
			{
				__asm__ __volatile__("wfe" ::: "memory");
				__asm__ __volatile__("ldaxr %w0, [%1]" : "=&r"(val) : "r"(ptr) : "memory");
				if (val <= 0)
					continue;
				__asm__ __volatile__("stlxr %w0, %w1, [%2]" : "=&r"(res) : "r"(val - 1), "r"(ptr) : "memory");
				if (res == 0)
					return;
			}
		}
#else
		void Post()
		{
			if (retro_atomic_fetch_add_int(&m_counter, 1) < 0)
				m_sema.Post();
		}

		void Wait()
		{
			if (retro_atomic_fetch_sub_int(&m_counter, 1) <= 0)
				m_sema.Wait();
		}
#endif

		bool TryWait()
		{
			/* Load-then-attempt: cas_int is strong with no expected-out,
			 * so re-read on failure.  Returns true iff a positive count
			 * was decremented, as before. */
			for (;;)
			{
				const int32_t counter = retro_atomic_load_acquire_int(&m_counter);
				if (counter <= 0)
					return false;
				if (retro_atomic_cas_int(&m_counter, counter, counter - 1))
					return true;
			}
		}
	};
} // namespace Threading
