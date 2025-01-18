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

#ifdef _WIN32
#include "RedtapeWindows.h"
#endif

#if defined(__APPLE__)
#include <pthread.h> // pthread_setcancelstate()
#include <sys/time.h> // gettimeofday()
#include <mach/mach.h>
#include <mach/task.h> // semaphore_create() and semaphore_destroy()
#include <mach/semaphore.h> // semaphore_*()
#include <mach/mach_error.h> // mach_error_string()
#include <mach/mach_time.h> // mach_absolute_time()
#endif

#include <limits>

// --------------------------------------------------------------------------------------
//  Semaphore Implementations
// --------------------------------------------------------------------------------------

bool Threading::WorkSema::CheckForWork()
{
	s32 value = m_state.load(std::memory_order_relaxed);

	// we want to switch to the running state, but preserve the waiting empty bit for RUNNING_N -> RUNNING_0
	// otherwise, we clear the waiting flag (since we're notifying the waiter that we're empty below)
	while (!m_state.compare_exchange_weak(value,
		((value & (STATE_FLAG_WAITING_EMPTY - 1)) == STATE_RUNNING_0) ? STATE_RUNNING_0 : (value & STATE_FLAG_WAITING_EMPTY),
		std::memory_order_acq_rel, std::memory_order_relaxed)) { }

	// if we're not empty, we have work to do
	s32 waiting_empty_cleared = value & (STATE_FLAG_WAITING_EMPTY - 1);
	if (waiting_empty_cleared != STATE_RUNNING_0)
		return true;

	// this means we're empty, so notify any waiters
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();

	// no work to do
	return false;
}

void Threading::WorkSema::WaitForWork()
{
	// State change:
	// SLEEPING, SPINNING: This is the worker thread and it's clearly not asleep or spinning, so these states should be impossible
	// RUNNING_0: Change state to SLEEPING, wake up thread if WAITING_EMPTY
	// RUNNING_N: Change state to RUNNING_0 (and preserve WAITING_EMPTY flag)
	s32 value = m_state.load(std::memory_order_relaxed);
	while (!m_state.compare_exchange_weak(value, NextStateWaitForWork(value), std::memory_order_acq_rel, std::memory_order_relaxed))
		;

	s32 waiting_empty_cleared = value & (STATE_FLAG_WAITING_EMPTY - 1);
	if (waiting_empty_cleared == STATE_RUNNING_0)
	{
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();
		m_sema.Wait();
		// Acknowledge any additional work added between wake up request and getting here
		m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
	}
}

bool Threading::WorkSema::WaitForEmpty()
{
	s32 value = m_state.load(std::memory_order_acquire);
	for (;;)
	{
		if (value < 0)
			return !(value < STATE_SPINNING); // STATE_SPINNING, queue is empty!
		// Note: We technically only need memory_order_acquire on *failure* (because that's when we could leave without sleeping), but libstdc++ still asserts on failure < success
		if (m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
	}
	m_empty_sema.Wait();
	return !(m_state.load(std::memory_order_relaxed) < STATE_SPINNING);
}

void Threading::WorkSema::Kill()
{
	s32 value = m_state.exchange(std::numeric_limits<s32>::min(), std::memory_order_release);
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();
}

void Threading::WorkSema::Reset()
{
	m_state = STATE_RUNNING_0;
}

Threading::KernelSemaphore::KernelSemaphore()
{
#if defined(_WIN32)
	m_sema = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
#elif defined(__APPLE__)
	semaphore_create(mach_task_self(), &m_sema, SYNC_POLICY_FIFO, 0);
#else
	sem_init(&m_sema, false, 0);
#endif
}

Threading::KernelSemaphore::~KernelSemaphore()
{
#if defined(_WIN32)
	CloseHandle(m_sema);
#elif defined(__APPLE__)
	semaphore_destroy(mach_task_self(), m_sema);
#else
	sem_destroy(&m_sema);
#endif
}

void Threading::KernelSemaphore::Post()
{
#if defined(_WIN32)
	ReleaseSemaphore(m_sema, 1, nullptr);
#elif defined(__APPLE__)
	semaphore_signal(m_sema);
#else
	sem_post(&m_sema);
#endif
}

void Threading::KernelSemaphore::Wait()
{
#if defined(_WIN32)
	WaitForSingleObject(m_sema, INFINITE);
#elif defined(__APPLE__)
	semaphore_wait(m_sema);
#else
	sem_wait(&m_sema);
#endif
}
