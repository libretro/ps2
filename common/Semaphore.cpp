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

#include <stdint.h>

// --------------------------------------------------------------------------------------
//  Semaphore Implementations
// --------------------------------------------------------------------------------------

bool Threading::WorkSema::CheckForWork()
{
	/* Load-then-attempt: cas_int is strong with no expected-out
	 * parameter, so each failed attempt re-reads and re-checks the
	 * death sentinel - the same guarantee the old expected-out refresh
	 * provided. */
	s32 value;
	for (;;)
	{
		value = retro_atomic_load_acquire_int(&m_state);

		// Dead semas stay dead: don't let the CAS below silently rewrite
		// INT_MIN to STATE_RUNNING_0 and resurrect a killed worker.
		if (value < STATE_SPINNING)
			return false;

		// we want to switch to the running state, but preserve the waiting empty bit for RUNNING_N -> RUNNING_0
		// otherwise, we clear the waiting flag (since we're notifying the waiter that we're empty below)
		if (retro_atomic_cas_int(&m_state, value,
				((value & (STATE_FLAG_WAITING_EMPTY - 1)) == STATE_RUNNING_0) ? STATE_RUNNING_0 : (value & STATE_FLAG_WAITING_EMPTY)))
			break;
	}

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
	// DEAD (any value < STATE_SPINNING): also impossible if invariants hold, but guard so a stray call after Kill()
	//   doesn't silently resurrect us to STATE_RUNNING_0 and then sleep forever on m_sema.
	// RUNNING_0: Change state to SLEEPING, wake up thread if WAITING_EMPTY
	// RUNNING_N: Change state to RUNNING_0 (and preserve WAITING_EMPTY flag)
	s32 value;
	for (;;)
	{
		value = retro_atomic_load_acquire_int(&m_state);
		if (value < STATE_SPINNING)
			return;
		if (retro_atomic_cas_int(&m_state, value, NextStateWaitForWork(value)))
			break;
	}

	s32 waiting_empty_cleared = value & (STATE_FLAG_WAITING_EMPTY - 1);
	if (waiting_empty_cleared == STATE_RUNNING_0)
	{
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();
		m_sema.Wait();
		// Acknowledge any additional work added between wake up request and getting here
		retro_atomic_fetch_and_int(&m_state, STATE_FLAG_WAITING_EMPTY);
	}
}

bool Threading::WorkSema::WaitForEmpty()
{
	for (;;)
	{
		const s32 value = retro_atomic_load_acquire_int(&m_state);
		if (value < 0)
			return !(value < STATE_SPINNING); // STATE_SPINNING, queue is empty!
		if (retro_atomic_cas_int(&m_state, value, value | STATE_FLAG_WAITING_EMPTY))
			break;
	}
	m_empty_sema.Wait();
	return !(retro_atomic_load_acquire_int(&m_state) < STATE_SPINNING);
}

void Threading::WorkSema::Kill()
{
	s32 value = retro_atomic_exchange_int(&m_state, INT32_MIN);
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();
}

void Threading::WorkSema::Reset()
{
	retro_atomic_store_release_int(&m_state, STATE_RUNNING_0);
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
