/*
 * SLockGuard - hold a libretro-common slock_t for a scope.
 *
 * This replaces common/Threading.h's Mutex and ScopedLock. The mutex
 * itself is gone: owners hold a slock_t* and call slock_new, slock_free,
 * slock_lock and slock_unlock directly. What remains is the one thing C++
 * needs that C does not have -- a hold that an early return or an
 * exception releases -- and it is these lines, in pcsx2/, over the
 * libretro-common primitive, with no class of its own in common/.
 *
 * Unlock() and Lock() are for the few sites that release mid-scope and
 * re-take before leaving; the destructor releases only if held.
 */

#ifndef PCSX2_SLOCK_GUARD_H
#define PCSX2_SLOCK_GUARD_H

#include <rthreads/rthreads.h>

class SLockGuard
{
	slock_t* m_lock;
	bool     m_held;

public:
	explicit SLockGuard(slock_t* lock) : m_lock(lock), m_held(true) { slock_lock(m_lock); }
	/* Deferred: construct unlocked, Lock() selectively, and the destructor
	 * releases only if a Lock() was left held. */
	struct Defer {};
	SLockGuard(slock_t* lock, Defer) : m_lock(lock), m_held(false) {}
	~SLockGuard()
	{
		if (m_held)
			slock_unlock(m_lock);
	}
	SLockGuard(const SLockGuard&) = delete;
	SLockGuard& operator=(const SLockGuard&) = delete;
	void Unlock() { slock_unlock(m_lock); m_held = false; }
	void Lock()   { slock_lock(m_lock);   m_held = true;  }
};

#endif
