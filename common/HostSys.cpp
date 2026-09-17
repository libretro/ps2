/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2024  PCSX2 Dev Team
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
#define _XOPEN_SOURCE
#endif

#if !defined(_WIN32)
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/syscall.h> /* memfd_create via syscall: bionic has no shm_open */
#endif
#endif

#ifndef ftruncate64
#define ftruncate64 ftruncate
#endif
#ifndef off64_t
#define off64_t off_t
#endif


#include <encodings/utf.h>

#include <retro_atomic.h>
#include "Align.h"
#include "AlignedMalloc.h"
#include "General.h"
#ifdef _WIN32
#include "RedtapeWindows.h"
#endif
#include "StringUtil.h"

/* Apple uses the MAP_ANON define instead of MAP_ANONYMOUS, but they mean
 * the same thing. */
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
#include <ucontext.h>
#endif
/* pcsx2/, on the include path; common/ is being folded into it. Every
 * platform: the registration mutex below is used on all of them. */
#include "SLockGuard.h"
#include "HostMem.h"   /* host_prot; pcsx2/, on the include path */

/* Registration-side mutex only.  The fault filter itself takes NO
 * lock: pthread mutexes are not async-signal-safe, and a global lock
 * serialized every JIT fault process-wide (EE + MTVU fault storms
 * during memory-clear phases contended through one futex). */
/* Plain, not recursive: it serialises Install against Remove, both of
 * which call only into the OS. The fault dispatch below never takes it. */
static slock_t* s_exception_handler_mutex(void)
{
	static slock_t* lock = slock_new();
	return lock;
}
/* Dispatch-side state, all lock-free:
 * - callback pointer: release-published by Install/Remove (under the
 *   registration mutex), acquire-loaded by the filter.  Remove only
 *   runs with JIT threads quiesced (VM shutdown), which is the
 *   lifecycle guarantee that makes the plain pointer swap safe.
 * - thread slots: threads that execute fastmem-faulting JIT register
 *   an identity here; the filter linear-scans it.  A fault on an
 *   unregistered thread is not ours and chains to the old handler.
 *   Identity is uintptr: GetCurrentThreadId on Windows, pthread_self
 *   on POSIX (scalar on every supported libc; static-asserted).
 *   NOT thread_local: this object is dlopen'd, so its TLS is
 *   global-dynamic and __tls_get_addr may allocate on a thread's
 *   first access - async-signal-unsafe at exactly the wrong moment.
 * - per-slot in-handler flag: recursion guard so a fault inside our
 *   own callback (a genuine crash) chains out for a core dump
 *   instead of looping. */
static retro_atomic_ptr_t s_exception_handler_callback_atomic;
#define FAULT_THREAD_SLOTS 4
static retro_atomic_int_t s_fault_slot_claimed[FAULT_THREAD_SLOTS];
static retro_atomic_ptr_t s_fault_slot_id[FAULT_THREAD_SLOTS];
static retro_atomic_int_t s_fault_slot_inhandler[FAULT_THREAD_SLOTS];
#ifdef _WIN32
static void* s_exception_handler_handle;
#endif

static uintptr_t fault_thread_identity(void)
{
#ifdef _WIN32
	return (uintptr_t)GetCurrentThreadId();
#else
	static_assert(sizeof(pthread_t) <= sizeof(uintptr_t),
		"pthread_t must be scalar-sized for the fault slot table");
	return (uintptr_t)pthread_self();
#endif
}

void HostSys::RegisterFaultHandlerThread()
{
	const uintptr_t self = fault_thread_identity();
	int i;
	for (i = 0; i < FAULT_THREAD_SLOTS; i++)
	{
		if ((uintptr_t)retro_atomic_load_acquire_ptr(&s_fault_slot_id[i]) == self)
			return; /* already registered */
	}
	for (i = 0; i < FAULT_THREAD_SLOTS; i++)
	{
		if (!retro_atomic_exchange_int(&s_fault_slot_claimed[i], 1))
		{
			retro_atomic_store_release_int(&s_fault_slot_inhandler[i], 0);
			retro_atomic_store_release_ptr(&s_fault_slot_id[i], (void*)self);
			return;
		}
	}
	/* More JIT threads than slots is a programming error; fail hard
	 * (cold path, not signal context). */
	abort();
}

void HostSys::UnregisterFaultHandlerThread()
{
	const uintptr_t self = fault_thread_identity();
	for (int i = 0; i < FAULT_THREAD_SLOTS; i++)
	{
		if ((uintptr_t)retro_atomic_load_acquire_ptr(&s_fault_slot_id[i]) == self)
		{
			retro_atomic_store_release_ptr(&s_fault_slot_id[i], (void*)0);
			/* Release the claim last so a concurrent register cannot
			 * observe a claimed slot with our stale id. */
			retro_atomic_store_release_int(&s_fault_slot_claimed[i], 0);
			return;
		}
	}
}

static int fault_slot_lookup(uintptr_t self)
{
	for (int i = 0; i < FAULT_THREAD_SLOTS; i++)
	{
		if ((uintptr_t)retro_atomic_load_acquire_ptr(&s_fault_slot_id[i]) == self)
			return i;
	}
	return -1;
}

#ifdef __APPLE__
#include <mach/task.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#endif

#ifdef _WIN32
long __stdcall SysPageFaultExceptionFilter(EXCEPTION_POINTERS* eps)
{
	/* Lock-free, mirroring the POSIX filter: slot lookup, per-slot
	 * recursion guard, acquire-loaded callback.  Vectored handlers
	 * have the same reentrancy hazards as signal handlers. */
	const int slot = fault_slot_lookup(fault_thread_identity());
	if (slot >= 0 && !retro_atomic_exchange_int(&s_fault_slot_inhandler[slot], 1))
	{
		bool handled = false;
		/* Only interested in page faults. */
		if (eps->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
#if defined(_M_AMD64) || defined(__x86_64__)
			void* const exception_pc = reinterpret_cast<void*>(eps->ContextRecord->Rip);
#elif (defined(_M_ARM64) || defined(__aarch64__)) || defined(__aarch64__)
			void* const exception_pc = reinterpret_cast<void*>(eps->ContextRecord->Pc);
#else
			void* const exception_pc = nullptr;
#endif

			const PageFaultInfo pfi{(uptr)exception_pc, (uptr)eps->ExceptionRecord->ExceptionInformation[1]};
			const PageFaultHandler callback =
				(PageFaultHandler)retro_atomic_load_acquire_ptr(&s_exception_handler_callback_atomic);
			handled = callback ? callback(pfi) : false;
		}
		/* Balance the recursion-guard claim on every exit path. */
		retro_atomic_store_release_int(&s_fault_slot_inhandler[slot], 0);
		if (handled)
			return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
#else
#if defined(__APPLE__) || defined(__aarch64__)
static struct sigaction s_old_sigbus_action;
#endif
#if !defined(__APPLE__) || defined(__aarch64__)
static struct sigaction s_old_sigsegv_action;
#endif

static void CallExistingSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
#if defined(__aarch64__)
	const struct sigaction& sa = (signal == SIGBUS) ? s_old_sigbus_action : s_old_sigsegv_action;
#elif defined(__APPLE__)
	const struct sigaction& sa = s_old_sigbus_action;
#else
	const struct sigaction& sa = s_old_sigsegv_action;
#endif

	if (sa.sa_flags & SA_SIGINFO)
		sa.sa_sigaction(signal, siginfo, ctx);
	else if (sa.sa_handler == SIG_DFL)
	{
		/* Re-raising the signal would just queue it, 
		 * and since we'd restore the handler back to us,
		 * we'd end up right back here again. So just abort, 
		 * because that's probably what it'd do anyway. */
		abort();
	}
	else if (sa.sa_handler != SIG_IGN)
		sa.sa_handler(signal);
}

/* Linux implementation of SIGSEGV handler. Bind it using sigaction() */
static void SysPageFaultSignalFilter(int signal, siginfo_t* siginfo, void* ctx)
{
	/* Async-signal-safe, lock-free dispatch: identify the faulting
	 * thread's slot; unregistered threads (not running fastmem JIT)
	 * are not ours - chain immediately.  Cross-thread mutual
	 * exclusion for the callback's bookkeeping lives inside vtlb as
	 * an AS-safe spinlock; concurrent faults on EE and MTVU no longer
	 * serialize here. */
	const int slot = fault_slot_lookup(fault_thread_identity());
	if (slot < 0)
	{
		CallExistingSignalHandler(signal, siginfo, ctx);
		return;
	}

	/* Recursion guard: a fault inside our own callback is a genuine
	 * crash - chain out so the old handler can dump core. */
	if (retro_atomic_exchange_int(&s_fault_slot_inhandler[slot], 1))
	{
		CallExistingSignalHandler(signal, siginfo, ctx);
		return;
	}

	/* Note: Use of stdio functions isn't safe here.  Avoid console logs, 
	 * assertions, file logs, or just about anything else useful. 
	 * However, that's really only a concern if the signal occurred within 
	 * those functions. The logging which we do only happens when the exception
	 * occurred within JIT code. */

#if defined(__APPLE__) && defined(__x86_64__)
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
#elif defined(__FreeBSD__) && defined(__x86_64__)
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
#elif defined(__x86_64__)
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
	#ifndef __APPLE__
		void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
	#else
		void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
	#endif
#else
	void* const exception_pc = nullptr;
#endif

	const PageFaultInfo pfi{(uptr)exception_pc, (uptr)siginfo->si_addr & ~__pagemask};

	const PageFaultHandler callback =
		(PageFaultHandler)retro_atomic_load_acquire_ptr(&s_exception_handler_callback_atomic);
	const bool handled = callback ? callback(pfi) : false;

	retro_atomic_store_release_int(&s_fault_slot_inhandler[slot], 0);

	/* Resumes execution right where we left off 
	 * (re-executes instruction that caused the SIGSEGV). */
	if (handled)
		return;

	/* Call old signal handler, which will likely dump core. */
	CallExistingSignalHandler(signal, siginfo, ctx);
}
#endif

bool HostSys::InstallPageFaultHandler(PageFaultHandler handler)
{
	SLockGuard lock(s_exception_handler_mutex());
#if defined(_WIN32)
	if (!s_exception_handler_handle)
	{
		s_exception_handler_handle = AddVectoredExceptionHandler(TRUE, SysPageFaultExceptionFilter);
		if (!s_exception_handler_handle)
			return false;
	}
#else
	if (!retro_atomic_load_acquire_ptr(&s_exception_handler_callback_atomic))
	{
		struct sigaction sa;

		sigemptyset(&sa.sa_mask);
		sa.sa_flags     = SA_SIGINFO;
		sa.sa_sigaction = SysPageFaultSignalFilter;
#ifdef __linux__
		/* Don't block the signal from executing recursively, 
		 * we want to fire the original handler. */
		sa.sa_flags    |= SA_NODEFER;
#endif
#if defined(__APPLE__) || defined(__aarch64__)
		/* MacOS uses SIGBUS for memory permission violations, 
		 * as well as SIGSEGV on ARM64. */
		if (sigaction(SIGBUS, &sa, &s_old_sigbus_action) != 0)
			return false;
#endif
#if !defined(__APPLE__) || defined(__aarch64__)
		if (sigaction(SIGSEGV, &sa, &s_old_sigsegv_action) != 0)
			return false;
#endif
#if defined(__APPLE__) && defined(__aarch64__)
		/* Stops LLDB getting in a EXC_BAD_ACCESS loop 
		 * when passing page faults to PCSX2. */
		task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
	}
#endif

	retro_atomic_store_release_ptr(&s_exception_handler_callback_atomic, (void*)handler);
	return true;
}

void HostSys::RemovePageFaultHandler(PageFaultHandler handler)
{
	SLockGuard lock(s_exception_handler_mutex());
#ifdef _WIN32
	retro_atomic_store_release_ptr(&s_exception_handler_callback_atomic, (void*)0);

	if (s_exception_handler_handle)
	{
		RemoveVectoredExceptionHandler(s_exception_handler_handle);
		s_exception_handler_handle = {};
	}
#else
	struct sigaction sa;
	if (!retro_atomic_load_acquire_ptr(&s_exception_handler_callback_atomic))
		return;

	retro_atomic_store_release_ptr(&s_exception_handler_callback_atomic, (void*)0);

#if defined(__APPLE__) || defined(__aarch64__)
	sigaction(SIGBUS, &s_old_sigbus_action, &sa);
#endif
#if !defined(__APPLE__) || defined(__aarch64__)
	sigaction(SIGSEGV, &s_old_sigsegv_action, &sa);
#endif
#endif
}

#ifdef _WIN32
#else
#endif









SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
#ifdef _WIN32
	m_placeholder_ranges.emplace(0, size);
#endif
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
#ifdef _WIN32
	/* hopefully this will be okay, and we don't need to coalesce all the placeholders... */
	VirtualFreeEx(GetCurrentProcess(), m_base_ptr, 0, MEM_RELEASE);
#else
	munmap(m_base_ptr, m_size);
#endif
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size)
{
#ifdef _WIN32
	/* Ask for a base above 4 GB. Below that, the 4 GB fastmem region
	 * either crosses the 32-bit boundary (so it overlaps Windows' system
	 * reservations around KUSER_SHARED_DATA at 0x7FFE0000) or - on a
	 * fragmented address space - actually starts at a low VA and aliases
	 * into mapped DLL/heap pages.
	 *
	 * Observed under MinGW: VirtualAlloc2(nullptr) returned 0x7FFF0000;
	 * the 4 GB span [0x7FFF0000, 0x17FFF0000) immediately overlaps system
	 * pages and the JIT crashes on the first fastmem load with no
	 * possibility of recovery (the page-fault handler can't distinguish
	 * a real system-page fault from a legitimate fastmem miss, because
	 * fastmem_end = fastmem_start + 0xFFFFFFFF puts both in range).
	 *
	 * MEM_ADDRESS_REQUIREMENTS + MEM_EXTENDED_PARAMETER were added in
	 * Win10 1803, same release that introduced the placeholder flags we
	 * already use, so this does not narrow the supported Windows range.
	 *
	 * Rather than make a single constrained request, sweep a ladder of
	 * floors. A single VirtualAlloc2 with LowestStartingAddress at 4 GB
	 * can fail outright on a fragmented address space even when plenty of
	 * room exists higher up; falling straight back to an unconstrained
	 * request then tends to land below 4 GB (rejected below) and disables
	 * fastmem entirely. Asking for progressively higher floors recovers a
	 * usable high placement in cases a single attempt would give up on -
	 * the same strategy the Beetle PSX dynarec uses to place its
	 * mappings. The ladder starts at 4 GB (the minimum usable base, see
	 * the rejection below) and runs to 36 GB, matching that range. */
	PCSX2_VirtualAlloc2_t pVirtualAlloc2 = nullptr;
	if (!PCSX2_HasPlaceholderAPIs(&pVirtualAlloc2, nullptr, nullptr))
	{
		/* Windows 8 / 8.1: no placeholder APIs. The Map()/Unmap()
		 * workflow below relies on them, so there is no usable fastmem
		 * area here. Return null and let vtlb_Core_Alloc fall back to
		 * no-fastmem mode (slower, but fully functional). */
		return nullptr;
	}

	void* alloc = nullptr;
	for (uintptr_t floor = 0x100000000ULL; floor <= 0x900000000ULL;
		floor += 0x100000000ULL)
	{
		MEM_ADDRESS_REQUIREMENTS req = {};
		req.LowestStartingAddress = reinterpret_cast<void*>(floor);

		MEM_EXTENDED_PARAMETER param = {};
		param.Type = MemExtendedParameterAddressRequirements;
		param.Pointer = &req;

		alloc = pVirtualAlloc2(GetCurrentProcess(), nullptr, size,
			MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
			&param, 1);
		if (alloc)
			break;
	}

	/* If every constrained request failed, fall back to an unconstrained
	 * one and sanity-check the result. A returned base below 4 GB on a
	 * 4 GB allocation is unusable for fastmem - reject it and let the
	 * caller fall back to no-fastmem mode. */
	if (!alloc)
	{
		alloc = pVirtualAlloc2(GetCurrentProcess(), nullptr, size,
			MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
			nullptr, 0);
	}
	if (!alloc)
		return nullptr;
	if (reinterpret_cast<uintptr_t>(alloc) < 0x100000000ULL)
	{
		/* Bad placement - the 4 GB span would overlap KUSER_SHARED_DATA
		 * and other Windows system reservations near 0x7FFE0000, and
		 * the page-fault handler cannot distinguish those faults from
		 * genuine fastmem misses (fastmem_end = fastmem_start +
		 * 0xFFFFFFFF reaches well past 4 GB). Free it and let the
		 * caller (vtlb_Core_Alloc) fall back to no-fastmem mode and
		 * log the user-visible warning. */
		VirtualFreeEx(GetCurrentProcess(), alloc, 0, MEM_RELEASE);
		return nullptr;
	}
#else
	void* alloc = mmap(nullptr, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (alloc == MAP_FAILED)
		return nullptr;
#endif
	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(static_cast<u8*>(alloc), size, size / __pagesize));
}

#ifdef _WIN32
SharedMemoryMappingArea::PlaceholderMap::iterator SharedMemoryMappingArea::FindPlaceholder(size_t offset)
{
	if (!m_placeholder_ranges.empty())
	{
		/* This will give us an iterator equal or after page */
		auto it = m_placeholder_ranges.lower_bound(offset);
		if (it == m_placeholder_ranges.end()) /* check the last page */
			it = (++m_placeholder_ranges.rbegin()).base();

		/* It's the one we found? */
		if (offset >= it->first && offset < it->second)
			return it;

		/* otherwise try the one before */
		if (it != m_placeholder_ranges.begin())
		{
			--it;
			if (offset >= it->first && offset < it->second)
				return it;
		}
	}
	return m_placeholder_ranges.end();
}
#endif

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode mode)
{
#ifdef _WIN32
	const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
	/* should be a placeholder. unless there's some other mapping we didn't free. */
	PlaceholderMap::iterator phit = FindPlaceholder(map_offset);

	/* do we need to split to the left? (i.e. is there a placeholder before this range) */
	const size_t old_ph_end = phit->second;
	if (map_offset != phit->first)
	{
		phit->second = map_offset;

		/* split it (i.e. left..start and start..end are now separated) */
		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(phit->first),
				(map_offset - phit->first), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
	}
	else
	{
		/* start of the placeholder is getting used, we'll split it right below 
		 * if there's anything left over */
		m_placeholder_ranges.erase(phit);
	}

	/* do we need to split to the right? (i.e. is there a placeholder after this range) */
	if ((map_offset + map_size) != old_ph_end)
	{
		/* split out end..ph_end */
		m_placeholder_ranges.emplace(map_offset + map_size, old_ph_end);

		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(map_offset), map_size,
				MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
	}

	/* actually do the mapping, replacing the placeholder on the range.
	 * The pointer is guaranteed non-null: this area only exists when
	 * Create() found all three placeholder APIs. */
	PCSX2_MapViewOfFile3_t pMapViewOfFile3 = nullptr;
	PCSX2_HasPlaceholderAPIs(nullptr, &pMapViewOfFile3, nullptr);
	if (!pMapViewOfFile3(static_cast<HANDLE>(file_handle), GetCurrentProcess(),
			map_base, file_offset, map_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
		return nullptr;

	DWORD prot;
	{
		const int p = host_prot(mode);
		if (p & PROT_READ)
			prot = (p & PROT_EXEC) ? ((p & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
			                       : ((p & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
		else
			prot = PAGE_NOACCESS;
	}
	if (prot != PAGE_READWRITE)
	{
		DWORD old_prot;
		VirtualProtect(map_base, map_size, prot, &old_prot);
	}

	m_num_mappings++;
	return static_cast<u8*>(map_base);
#else
	const int prot     = host_prot(mode);
	void* const ptr    = mmap(map_base, map_size, prot, MAP_SHARED | MAP_FIXED,
		static_cast<int>(reinterpret_cast<intptr_t>(file_handle)), static_cast<off_t>(file_offset));
	if (ptr == MAP_FAILED)
		return nullptr;

	m_num_mappings++;
	return static_cast<u8*>(ptr);
#endif
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
#ifdef _WIN32
	const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
	/* unmap the specified range. The pointer is guaranteed non-null:
	 * this area only exists when Create() found all three APIs. */
	PCSX2_UnmapViewOfFile2_t pUnmapViewOfFile2 = nullptr;
	PCSX2_HasPlaceholderAPIs(nullptr, nullptr, &pUnmapViewOfFile2);
	if (!pUnmapViewOfFile2(GetCurrentProcess(), map_base, MEM_PRESERVE_PLACEHOLDER))
		return false;

	/* can we coalesce to the left? */
	PlaceholderMap::iterator left_it = (map_offset > 0) ? FindPlaceholder(map_offset - 1) : m_placeholder_ranges.end();
	if (left_it != m_placeholder_ranges.end())
	{
		/* the left placeholder should end at our start */
		left_it->second = map_offset + map_size;

		/* combine placeholders before and the range we're unmapping, i.e. to the left */
		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first),
				 left_it->second - left_it->first, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
	}
	else /* this is a new placeholder */
		left_it = m_placeholder_ranges.emplace(map_offset, map_offset + map_size).first;

	/* can we coalesce to the right? */
	PlaceholderMap::iterator right_it = ((map_offset + map_size) < m_size) ? FindPlaceholder(map_offset + map_size) : m_placeholder_ranges.end();
	if (right_it != m_placeholder_ranges.end())
	{
		/* should start at our end */
		left_it->second = right_it->second;
		m_placeholder_ranges.erase(right_it);

		/* combine our placeholder and the next, i.e. to the right */
		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first),
				left_it->second - left_it->first, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
	}
#else
	if (mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
		return false;
#endif

	m_num_mappings--;
	return true;
}

#if (defined(_M_ARM64) || defined(__aarch64__)) || defined(__aarch64__)

#if defined(__APPLE__)


#endif

#endif
