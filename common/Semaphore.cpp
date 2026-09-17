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

Threading::WorkSema::WorkSema()
{
	/* Both inits decide their protocol once and keep it. The work
	 * eventcount picks asymmetric wherever retro_procbarrier has a tier,
	 * symmetric elsewhere; a WorkSema never needs to know which. */
	retro_asym_eventcount_init(&m_work);
	retro_eventcount_init(&m_empty);
	retro_atomic_store_relaxed_int(&m_seen, retro_atomic_load_relaxed_int(&m_work.epoch));
}

Threading::WorkSema::~WorkSema()
{
	retro_eventcount_free(&m_empty);
	retro_asym_eventcount_free(&m_work);
}

void Threading::WorkSema::GoIdle()
{
	/* Order matters for WaitForEmpty's re-check: it reads m_idle, then
	 * m_seen against the work epoch. Publish idle last, with release, so
	 * a reader that sees idle=1 also sees the m_seen that goes with it. */
	retro_atomic_store_release_int(&m_idle, 1);
	retro_eventcount_notify(&m_empty);
}

bool Threading::WorkSema::Consume(int epoch)
{
	retro_atomic_store_release_int(&m_seen, epoch);
	retro_atomic_store_release_int(&m_idle, 0);
	return true;
}

bool Threading::WorkSema::CheckForWork()
{
	int epoch;
	if (retro_atomic_load_acquire_int(&m_dead))
		return false;
	/* The epoch is the asym eventcount's own; reading it is what a
	 * non-blocking check is, and cheaper than a prepare/cancel pair,
	 * which would pay the barrier. */
	epoch = retro_atomic_load_acquire_int(&m_work.epoch);
	if (epoch != retro_atomic_load_relaxed_int(&m_seen))
		return Consume(epoch);
	GoIdle();
	return false;
}

void Threading::WorkSema::WaitForWork()
{
	const s32 spin_budget = Threading::SpinBudget();
	int epoch, key;

	if (retro_atomic_load_acquire_int(&m_dead))
		return;

	/* Anything already notified: take it and go. */
	epoch = retro_atomic_load_acquire_int(&m_work.epoch);
	if (epoch != retro_atomic_load_relaxed_int(&m_seen))
	{
		Consume(epoch);
		return;
	}

	/* Nothing pending: the worker is idle from here until something
	 * arrives, and WaitForEmpty callers are told so. */
	GoIdle();

	/* Spin first where it is worth it -- a kernel sleep/wake pair costs
	 * more than SpinBudget() reads -- and only then park. */
	if (spin_budget)
	{
		s32 spins = spin_budget;
		while (spins-- > 0)
		{
			epoch = retro_atomic_load_acquire_int(&m_work.epoch);
			if (epoch != retro_atomic_load_relaxed_int(&m_seen))
			{
				Consume(epoch);
				return;
			}
			if (retro_atomic_load_relaxed_int(&m_dead))
				return;
			THREADING_CPU_RELAX();
		}
	}

	/* Park. prepare_wait is where the process-wide barrier is paid:
	 * after it, either the producer's notify is visible in the key or
	 * the producer will see our registration and wake us. */
	key = retro_asym_eventcount_prepare_wait(&m_work);
	if (key != retro_atomic_load_relaxed_int(&m_seen)
	 || retro_atomic_load_acquire_int(&m_dead))
	{
		retro_asym_eventcount_cancel_wait(&m_work);
		if (!retro_atomic_load_acquire_int(&m_dead))
			Consume(key);
		return;
	}
	retro_asym_eventcount_commit_wait(&m_work, key);
	if (!retro_atomic_load_acquire_int(&m_dead))
		Consume(retro_atomic_load_acquire_int(&m_work.epoch));
}

bool Threading::WorkSema::WaitForWorkTimed(u32 timeout_ms)
{
	int epoch, key;

	if (retro_atomic_load_acquire_int(&m_dead))
		return true;

	epoch = retro_atomic_load_acquire_int(&m_work.epoch);
	if (epoch != retro_atomic_load_relaxed_int(&m_seen))
		return Consume(epoch);

	GoIdle();

	key = retro_asym_eventcount_prepare_wait(&m_work);
	if (key != retro_atomic_load_relaxed_int(&m_seen)
	 || retro_atomic_load_acquire_int(&m_dead))
	{
		retro_asym_eventcount_cancel_wait(&m_work);
		if (!retro_atomic_load_acquire_int(&m_dead))
			Consume(key);
		return true;
	}
	if (!retro_asym_eventcount_commit_wait_timeout(&m_work, key,
			(int64_t)timeout_ms * 1000))
	{
		/* Timed out. If something arrived in the meantime it is work,
		 * not a timeout; only a still-unchanged epoch is the false case. */
		epoch = retro_atomic_load_acquire_int(&m_work.epoch);
		if (epoch == key && !retro_atomic_load_acquire_int(&m_dead))
			return false;
	}
	if (!retro_atomic_load_acquire_int(&m_dead))
		Consume(retro_atomic_load_acquire_int(&m_work.epoch));
	return true;
}

bool Threading::WorkSema::WaitForEmpty()
{
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&m_dead))
			return false;
		/* Empty means: the worker is idle AND it went idle having seen
		 * everything notified so far. Idle alone is not enough -- a notify
		 * can land after the worker's last check and before its park, and
		 * that work is still outstanding. */
		if (retro_atomic_load_acquire_int(&m_idle)
		 && retro_atomic_load_acquire_int(&m_seen)
		    == retro_atomic_load_acquire_int(&m_work.epoch))
			return true;
		key = retro_eventcount_prepare_wait(&m_empty);
		if (retro_atomic_load_acquire_int(&m_dead))
		{
			retro_eventcount_cancel_wait(&m_empty);
			return false;
		}
		if (retro_atomic_load_acquire_int(&m_idle)
		 && retro_atomic_load_acquire_int(&m_seen)
		    == retro_atomic_load_acquire_int(&m_work.epoch))
		{
			retro_eventcount_cancel_wait(&m_empty);
			return true;
		}
		retro_eventcount_commit_wait(&m_empty, key);
	}
}

void Threading::WorkSema::Kill()
{
	retro_atomic_store_release_int(&m_dead, 1);
	/* Wake whoever is parked on either side so they see it. */
	retro_asym_eventcount_notify(&m_work);
	retro_eventcount_notify(&m_empty);
}

void Threading::WorkSema::Reset()
{
	retro_atomic_store_release_int(&m_dead, 0);
	retro_atomic_store_release_int(&m_seen, retro_atomic_load_acquire_int(&m_work.epoch));
	retro_atomic_store_release_int(&m_idle, 1);
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
