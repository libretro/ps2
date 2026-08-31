/* Copyright (c) 2024 The libretro team
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Granite's threading, on rthreads rather than <thread>/<mutex>/
 * <condition_variable>.
 *
 * pcsx2 proper already runs on rthreads through common/Threading.h, but
 * Granite is built as its own set of static libraries (granite-util,
 * granite-vulkan, parallel-gs) that do not see pcsx2's headers, so the
 * same RAII lives here.  The shapes deliberately mirror the std types
 * they replace -- lock() / unlock() / try_lock(), a scoped lock_guard, a
 * unique_lock the condition variable can release and re-take, wait() with
 * and without a predicate -- so that call sites change in name only and
 * the diff stays reviewable.
 *
 * Semantics match the std pair: wait() releases the mutex while sleeping
 * and re-acquires before returning, and spurious wakeups are possible, so
 * always wait on a predicate.
 */

#ifndef PGS_THREAD_PRIMS_HPP
#define PGS_THREAD_PRIMS_HPP

#include <rthreads/rthreads.h>

#include <utility>

namespace PGS
{
class mutex
{
public:
	mutex() : m_lock(slock_new()) {}
	~mutex() { if (m_lock) slock_free(m_lock); }
	mutex(const mutex &) = delete;
	mutex &operator=(const mutex &) = delete;

	void lock() { slock_lock(m_lock); }
	void unlock() { slock_unlock(m_lock); }
	bool try_lock() { return slock_try_lock(m_lock); }

	slock_t *native() { return m_lock; }

private:
	slock_t *m_lock;
};

class lock_guard
{
public:
	explicit lock_guard(mutex &m) : m_mutex(m) { m_mutex.lock(); }
	~lock_guard() { m_mutex.unlock(); }
	lock_guard(const lock_guard &) = delete;
	lock_guard &operator=(const lock_guard &) = delete;

private:
	mutex &m_mutex;
};

/* Holds the mutex, but can hand it back and re-take it, which is what a
 * condition variable needs and what the std::unique_lock call sites in
 * Granite use it for. */
class unique_lock
{
public:
	explicit unique_lock(mutex &m) : m_mutex(&m), m_held(true) { m_mutex->lock(); }
	~unique_lock() { if (m_held) m_mutex->unlock(); }
	unique_lock(const unique_lock &) = delete;
	unique_lock &operator=(const unique_lock &) = delete;

	unique_lock(unique_lock &&other) noexcept
		: m_mutex(other.m_mutex), m_held(other.m_held)
	{
		other.m_mutex = nullptr;
		other.m_held  = false;
	}

	void lock() { m_mutex->lock(); m_held = true; }
	void unlock() { m_mutex->unlock(); m_held = false; }
	bool owns_lock() const { return m_held; }

	mutex *mutex_ptr() const { return m_mutex; }

private:
	mutex *m_mutex;
	bool m_held;
};

class condition_variable
{
public:
	condition_variable() : m_cond(scond_new()) {}
	~condition_variable() { if (m_cond) scond_free(m_cond); }
	condition_variable(const condition_variable &) = delete;
	condition_variable &operator=(const condition_variable &) = delete;

	void wait(unique_lock &holder)
	{
		scond_wait(m_cond, holder.mutex_ptr()->native());
	}

	template <typename Predicate>
	void wait(unique_lock &holder, Predicate pred)
	{
		while (!pred())
			scond_wait(m_cond, holder.mutex_ptr()->native());
	}

	void notify_one() { scond_signal(m_cond); }
	void notify_all() { scond_broadcast(m_cond); }

private:
	scond_t *m_cond;
};

/* A joinable thread over sthread_create.  rthreads takes a C entry point
 * and a void*, so the callable is heap-allocated and adopted by the new
 * thread's trampoline; join() reaps it.  Only the operations Granite
 * actually performs are offered: start on construction, joinable(),
 * join(). */
class thread
{
public:
	thread() : m_thread(nullptr) {}

	template <typename Fn>
	explicit thread(Fn &&fn)
	{
		auto *holder = new callable_holder<typename std::decay<Fn>::type>(std::forward<Fn>(fn));
		m_thread     = sthread_create(&trampoline<typename std::decay<Fn>::type>, holder);
		if (!m_thread)
			delete holder;
	}

	~thread() { join(); }

	thread(const thread &) = delete;
	thread &operator=(const thread &) = delete;

	thread(thread &&other) noexcept : m_thread(other.m_thread) { other.m_thread = nullptr; }

	thread &operator=(thread &&other) noexcept
	{
		if (this != &other)
		{
			join();
			m_thread       = other.m_thread;
			other.m_thread = nullptr;
		}
		return *this;
	}

	bool joinable() const { return m_thread != nullptr; }

	void join()
	{
		if (!m_thread)
			return;
		sthread_join(m_thread);
		m_thread = nullptr;
	}

private:
	template <typename Fn>
	struct callable_holder
	{
		explicit callable_holder(Fn &&f) : fn(std::move(f)) {}
		explicit callable_holder(const Fn &f) : fn(f) {}
		Fn fn;
	};

	template <typename Fn>
	static void trampoline(void *data)
	{
		auto *holder = static_cast<callable_holder<Fn> *>(data);
		holder->fn();
		delete holder;
	}

	sthread_t *m_thread;
};
} // namespace PGS

#endif
