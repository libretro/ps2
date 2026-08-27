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

#ifndef _WIN32
#include <unistd.h> /* sysconf */
#endif

/* Iterations of the read-only spin a worker performs in STATE_SPINNING
 * before parking on the kernel semaphore.  A notify that lands inside
 * the window is caught with zero syscalls and zero context switches on
 * either side: NotifyOfWork's fetch_add moves SPINNING to RUNNING and
 * skips the post, and the spinner never blocks.  Sized to a few
 * microseconds - long enough to bridge the producer's typical
 * inter-packet gaps, short enough that a miss costs less than the
 * futex round trip it tried to avoid. */
#define WORKSEMA_SPIN_COUNT 2000

static int32_t worksema_compute_spin_budget(void)
{
	/* Spinning on a single-core host only steals the producer's
	 * timeslice; the spin always times out there.  Park immediately
	 * instead - the pre-spin behavior. */
#if defined(_WIN32)
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (si.dwNumberOfProcessors >= 2) ? WORKSEMA_SPIN_COUNT : 0;
#else
	return (sysconf(_SC_NPROCESSORS_ONLN) >= 2) ? WORKSEMA_SPIN_COUNT : 0;
#endif
}

s32 Threading::SpinBudget()
{
	static const int32_t budget = worksema_compute_spin_budget();
	return budget;
}

// --------------------------------------------------------------------------------------
//  Semaphore Implementations
// --------------------------------------------------------------------------------------

bool Threading::KernelSemaphore::WaitFor(u32 timeout_ms)
{
#if defined(_WIN32)
	return WaitForSingleObject(m_sema, timeout_ms) == WAIT_OBJECT_0;
#elif defined(__APPLE__)
	mach_timespec_t ts;
	ts.tv_sec  = timeout_ms / 1000;
	ts.tv_nsec = (timeout_ms % 1000) * 1000000;
	return semaphore_timedwait(m_sema, ts) == KERN_SUCCESS;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec  += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	while (sem_timedwait(&m_sema, &ts) == -1)
	{
		if (errno != EINTR)
			return false;
	}
	return true;
#endif
}

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
	// RUNNING_0: Change state to SPINNING (multi-core) or SLEEPING, wake up thread if WAITING_EMPTY
	// RUNNING_N: Change state to RUNNING_0 (and preserve WAITING_EMPTY flag)
	const s32 spin_budget = Threading::SpinBudget();
	const s32 idle_state  = spin_budget ? STATE_SPINNING : STATE_SLEEPING;
	s32 value;
	for (;;)
	{
		s32 waiting_empty_cleared;
		s32 new_state;
		value = retro_atomic_load_acquire_int(&m_state);
		if (value < STATE_SPINNING)
			return;
		/* The empty-waiter flag cannot survive on the (negative) idle
		 * states; the post below is what consumes it. */
		waiting_empty_cleared = value & (STATE_FLAG_WAITING_EMPTY - 1);
		new_state = (waiting_empty_cleared == STATE_RUNNING_0) ? idle_state : (STATE_RUNNING_0 | (value & STATE_FLAG_WAITING_EMPTY));
		if (retro_atomic_cas_int(&m_state, value, new_state))
			break;
	}

	s32 waiting_empty_cleared = value & (STATE_FLAG_WAITING_EMPTY - 1);
	if (waiting_empty_cleared == STATE_RUNNING_0)
	{
		/* Wake any WaitForEmpty sleeper before idling, never after:
		 * the producer's wakeup must not wait out the spin window. */
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();

		if (spin_budget)
		{
			/* Read-only spin in STATE_SPINNING.  NotifyOfWork's
			 * fetch_add moves the state to RUNNING and skips the
			 * kernel post, so a notify inside this window costs
			 * neither side a syscall.  On timeout, demote to
			 * SLEEPING and park; a notify racing the demotion makes
			 * the CAS fail, and the state it left behind is already
			 * RUNNING. */
			s32 spins = spin_budget;
			bool have_work = false;
			while (spins-- > 0)
			{
				if (retro_atomic_load_acquire_int(&m_state) != STATE_SPINNING)
				{
					have_work = true;
					break;
				}
				THREADING_CPU_RELAX();
			}
			if (!have_work && retro_atomic_cas_int(&m_state, STATE_SPINNING, STATE_SLEEPING))
				m_sema.Wait();
		}
		else
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

bool Threading::WorkSema::WaitForWorkTimed(u32 timeout_ms)
{
	/* Same state walk as WaitForWork, minus the spin phase (a frontend
	 * thread must park cheaply, not burn a core), plus a bounded sleep.
	 * On timeout the state is CASed from SLEEPING back to RUNNING_0
	 * before returning, so no thread is ever recorded as parked while
	 * running off - the invariant WaitForEmpty depends on.  If a notify
	 * wins that CAS it has already posted m_sema; the stray count makes
	 * one future wait return early, which the state machine absorbs the
	 * same way it absorbs any spurious wake. */
	s32 value;
	for (;;)
	{
		s32 waiting_empty_cleared;
		s32 new_state;
		value = retro_atomic_load_acquire_int(&m_state);
		if (value < STATE_SPINNING)
			return true;
		waiting_empty_cleared = value & (STATE_FLAG_WAITING_EMPTY - 1);
		new_state = (waiting_empty_cleared == STATE_RUNNING_0) ? STATE_SLEEPING : (STATE_RUNNING_0 | (value & STATE_FLAG_WAITING_EMPTY));
		if (retro_atomic_cas_int(&m_state, value, new_state))
			break;
	}

	if ((value & (STATE_FLAG_WAITING_EMPTY - 1)) == STATE_RUNNING_0)
	{
		/* Wake any WaitForEmpty sleeper before parking, exactly as
		 * WaitForWork does. */
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();

		if (!m_sema.WaitFor(timeout_ms))
		{
			if (retro_atomic_cas_int(&m_state, STATE_SLEEPING, STATE_RUNNING_0))
				return false;
			/* A notify raced the timeout: state is RUNNING and a sema
			 * post is in flight.  Treat it as a wake. */
		}
	}

	/* Acknowledge any additional work added between wake up request and getting here */
	retro_atomic_fetch_and_int(&m_state, STATE_FLAG_WAITING_EMPTY);
	return true;
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
