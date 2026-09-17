/*
 * WorkEventCount - the "tell a worker thread there is new work" handshake,
 * as two libretro-common eventcounts and nothing else.
 *
 * This replaces common/Threading.h's WorkSema class. The state it needs is
 * two eventcounts and three ints, and the caller owns them: an MTGS or an
 * MTVU embeds a WorkEventCount and calls these free functions. There is no
 * pcsx2 wrapper class, no common/ helper, no hand-rolled state machine --
 * the whole point of the move is to call straight into libretro-common.
 *
 * Two directions, each an eventcount:
 *
 *   work  (producer -> worker, "work was added"): retro_asym_eventcount.
 *         Its notify is a release store and a relaxed load, no lock
 *         prefix -- that is what the EE pays per GS packet, and it is the
 *         reason for the whole change. Where retro_procbarrier has no
 *         tier the eventcount runs its symmetric protocol for life,
 *         decided at init; the caller never has to know which.
 *
 *   empty (worker -> producer, "I drained everything so far and went
 *         idle"): the plain retro_eventcount. Read once a frame, so its
 *         cost does not matter, and its prepare/commit contract is what
 *         the old STATE_FLAG_WAITING_EMPTY handshake was reinventing.
 *
 * Contract, matching the class it replaces exactly:
 *   - NotifyOfWork:      publish that work was added.
 *   - CheckForWork:      true once per burst of notifies; a false return
 *                        means the worker is now idle and empty-waiters
 *                        are told so.
 *   - WaitForWork:       return at once with work pending; else spin
 *                        SpinBudget() reads and park -- the one place the
 *                        process-wide barrier is paid.
 *   - WaitForWorkTimed:  as WaitForWork, false only on a timeout with
 *                        nothing pending (true means work or dead).
 *   - WaitForEmpty:      block until the worker consumed every notify so
 *                        far AND went idle; false if the worker is dead.
 *   - Kill / Reset:      dead threads process nothing and make every wait
 *                        return; Reset revives.
 */

#ifndef PCSX2_WORK_EVENTCOUNT_H
#define PCSX2_WORK_EVENTCOUNT_H

#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#include <rthreads/retro_asym_eventcount.h>

/* CPU relax hint for the bounded pre-park spin. Self-contained on
 * purpose: this header must not pull in common/Threading.h, which is the
 * dependency being removed. */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#if defined(_MSC_VER)
#define WORK_EVENTCOUNT_RELAX() _mm_pause()
#else
#define WORK_EVENTCOUNT_RELAX() __builtin_ia32_pause()
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)
#if defined(_MSC_VER)
#define WORK_EVENTCOUNT_RELAX() __yield()
#else
#define WORK_EVENTCOUNT_RELAX() __asm__ __volatile__("yield" ::: "memory")
#endif
#else
#define WORK_EVENTCOUNT_RELAX() ((void)0)
#endif

typedef struct WorkEventCount
{
	retro_asym_eventcount_t work;    /* producer -> worker            */
	retro_eventcount_t      empty;   /* worker -> producer            */
	retro_atomic_int_t      seen;    /* work epoch the worker consumed */
	retro_atomic_int_t      idle;    /* worker is between "no work" and "told of work" */
	retro_atomic_int_t      dead;    /* Kill sets, Reset clears        */
} WorkEventCount;

/* Iterations of a read-only spin worth spending to dodge a kernel
 * sleep/wake pair; 0 on single-core hosts. Defined in MTGS.cpp. */
extern s32 WorkEventCount_SpinBudget(void);

static INLINE void work_eventcount_init(WorkEventCount *w)
{
	retro_asym_eventcount_init(&w->work);
	retro_eventcount_init(&w->empty);
	retro_atomic_store_relaxed_int(&w->seen,
			retro_atomic_load_relaxed_int(&w->work.epoch));
	retro_atomic_store_relaxed_int(&w->idle, 1);
	retro_atomic_store_relaxed_int(&w->dead, 0);
}

static INLINE void work_eventcount_free(WorkEventCount *w)
{
	retro_eventcount_free(&w->empty);
	retro_asym_eventcount_free(&w->work);
}

/* Worker took everything up to `epoch`; it is processing, not idle. */
static INLINE int work_eventcount_consume(WorkEventCount *w, int epoch)
{
	retro_atomic_store_release_int(&w->seen, epoch);
	retro_atomic_store_release_int(&w->idle, 0);
	return 1;
}

/* Worker found nothing to do: publish idle for the empty-waiters. Order
 * matters -- WaitForEmpty reads idle then seen, so idle is stored last,
 * with release, so a reader seeing idle=1 also sees the matching seen. */
static INLINE void work_eventcount_go_idle(WorkEventCount *w)
{
	retro_atomic_store_release_int(&w->idle, 1);
	retro_eventcount_notify(&w->empty);
}

static INLINE void work_eventcount_notify(WorkEventCount *w)
{
	retro_asym_eventcount_notify(&w->work);
}

static INLINE int work_eventcount_check(WorkEventCount *w)
{
	int epoch;
	if (retro_atomic_load_acquire_int(&w->dead))
		return 0;
	/* Reading the epoch is what a non-blocking check is; cheaper than a
	 * prepare/cancel pair, which would pay the barrier. */
	epoch = retro_atomic_load_acquire_int(&w->work.epoch);
	if (epoch != retro_atomic_load_relaxed_int(&w->seen))
		return work_eventcount_consume(w, epoch);
	work_eventcount_go_idle(w);
	return 0;
}

static INLINE void work_eventcount_wait(WorkEventCount *w)
{
	const s32 spin_budget = WorkEventCount_SpinBudget();
	int epoch, key;

	if (retro_atomic_load_acquire_int(&w->dead))
		return;

	epoch = retro_atomic_load_acquire_int(&w->work.epoch);
	if (epoch != retro_atomic_load_relaxed_int(&w->seen))
	{
		work_eventcount_consume(w, epoch);
		return;
	}

	work_eventcount_go_idle(w);

	if (spin_budget)
	{
		s32 spins = spin_budget;
		while (spins-- > 0)
		{
			epoch = retro_atomic_load_acquire_int(&w->work.epoch);
			if (epoch != retro_atomic_load_relaxed_int(&w->seen))
			{
				work_eventcount_consume(w, epoch);
				return;
			}
			if (retro_atomic_load_relaxed_int(&w->dead))
				return;
			WORK_EVENTCOUNT_RELAX();
		}
	}

	key = retro_asym_eventcount_prepare_wait(&w->work);
	if (key != retro_atomic_load_relaxed_int(&w->seen)
	 || retro_atomic_load_acquire_int(&w->dead))
	{
		retro_asym_eventcount_cancel_wait(&w->work);
		if (!retro_atomic_load_acquire_int(&w->dead))
			work_eventcount_consume(w, key);
		return;
	}
	retro_asym_eventcount_commit_wait(&w->work, key);
	if (!retro_atomic_load_acquire_int(&w->dead))
		work_eventcount_consume(w,
				retro_atomic_load_acquire_int(&w->work.epoch));
}

static INLINE int work_eventcount_wait_timed(WorkEventCount *w, u32 timeout_ms)
{
	int epoch, key;

	if (retro_atomic_load_acquire_int(&w->dead))
		return 1;

	epoch = retro_atomic_load_acquire_int(&w->work.epoch);
	if (epoch != retro_atomic_load_relaxed_int(&w->seen))
		return work_eventcount_consume(w, epoch);

	work_eventcount_go_idle(w);

	key = retro_asym_eventcount_prepare_wait(&w->work);
	if (key != retro_atomic_load_relaxed_int(&w->seen)
	 || retro_atomic_load_acquire_int(&w->dead))
	{
		retro_asym_eventcount_cancel_wait(&w->work);
		if (!retro_atomic_load_acquire_int(&w->dead))
			work_eventcount_consume(w, key);
		return 1;
	}
	if (!retro_asym_eventcount_commit_wait_timeout(&w->work, key,
			(int64_t)timeout_ms * 1000))
	{
		epoch = retro_atomic_load_acquire_int(&w->work.epoch);
		if (epoch == key && !retro_atomic_load_acquire_int(&w->dead))
			return 0;   /* timed out with nothing pending */
	}
	if (!retro_atomic_load_acquire_int(&w->dead))
		work_eventcount_consume(w,
				retro_atomic_load_acquire_int(&w->work.epoch));
	return 1;
}

static INLINE int work_eventcount_wait_empty(WorkEventCount *w)
{
	for (;;)
	{
		int key;
		if (retro_atomic_load_acquire_int(&w->dead))
			return 0;
		/* Idle alone is not empty: a notify can land after the worker's
		 * last check and before its park, and that work is outstanding.
		 * Empty is idle AND the worker went idle having seen everything. */
		if (retro_atomic_load_acquire_int(&w->idle)
		 && retro_atomic_load_acquire_int(&w->seen)
		    == retro_atomic_load_acquire_int(&w->work.epoch))
			return 1;
		key = retro_eventcount_prepare_wait(&w->empty);
		if (retro_atomic_load_acquire_int(&w->dead))
		{
			retro_eventcount_cancel_wait(&w->empty);
			return 0;
		}
		if (retro_atomic_load_acquire_int(&w->idle)
		 && retro_atomic_load_acquire_int(&w->seen)
		    == retro_atomic_load_acquire_int(&w->work.epoch))
		{
			retro_eventcount_cancel_wait(&w->empty);
			return 1;
		}
		retro_eventcount_commit_wait(&w->empty, key);
	}
}

static INLINE void work_eventcount_kill(WorkEventCount *w)
{
	retro_atomic_store_release_int(&w->dead, 1);
	retro_asym_eventcount_notify(&w->work);
	retro_eventcount_notify(&w->empty);
}

static INLINE void work_eventcount_reset(WorkEventCount *w)
{
	retro_atomic_store_release_int(&w->dead, 0);
	retro_atomic_store_release_int(&w->seen,
			retro_atomic_load_acquire_int(&w->work.epoch));
	retro_atomic_store_release_int(&w->idle, 1);
}

#endif
