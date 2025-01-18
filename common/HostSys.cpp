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
#endif

#ifndef ftruncate64
#define ftruncate64 ftruncate
#endif
#ifndef off64_t
#define off64_t off_t
#endif

#include <mutex>

#include "../3rdparty/fmt/fmt/include/fmt/format.h"

#include <encodings/utf.h>

#include "Align.h"
#include "AlignedMalloc.h"
#include "Console.h"
#include "General.h"
#ifdef _WIN32
#include "RedtapeWindows.h"
#endif
#include "StringUtil.h"

// Apple uses the MAP_ANON define instead of MAP_ANONYMOUS, but they mean
// the same thing.
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
#include <ucontext.h>
#endif

static std::recursive_mutex s_exception_handler_mutex;
static PageFaultHandler s_exception_handler_callback;
#ifdef _WIN32
static void* s_exception_handler_handle;
#endif
static bool s_in_exception_handler;

#ifdef __APPLE__
#include <mach/task.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#endif

#ifdef _WIN32
long __stdcall SysPageFaultExceptionFilter(EXCEPTION_POINTERS* eps)
{
	// Executing the handler concurrently from multiple threads wouldn't go down well.
	std::unique_lock lock(s_exception_handler_mutex);

	// Prevent recursive exception filtering.
	if (s_in_exception_handler)
		return EXCEPTION_CONTINUE_SEARCH;

	// Only interested in page faults.
	if (eps->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;

#if defined(_M_AMD64)
	void* const exception_pc = reinterpret_cast<void*>(eps->ContextRecord->Rip);
#elif defined(_M_ARM64)
	void* const exception_pc = reinterpret_cast<void*>(eps->ContextRecord->Pc);
#else
	void* const exception_pc = nullptr;
#endif

	const PageFaultInfo pfi{(uptr)exception_pc, (uptr)eps->ExceptionRecord->ExceptionInformation[1]};

	s_in_exception_handler = true;

	const bool handled = s_exception_handler_callback(pfi);

	s_in_exception_handler = false;
	
	return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
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
		// Re-raising the signal would just queue it, and since we'd restore the handler back to us,
		// we'd end up right back here again. So just abort, because that's probably what it'd do anyway.
		abort();
	}
	else if (sa.sa_handler != SIG_IGN)
		sa.sa_handler(signal);
}

// Linux implementation of SIGSEGV handler.  Bind it using sigaction().
static void SysPageFaultSignalFilter(int signal, siginfo_t* siginfo, void* ctx)
{
	// Executing the handler concurrently from multiple threads wouldn't go down well.
	std::unique_lock lock(s_exception_handler_mutex);

	// Prevent recursive exception filtering.
	if (s_in_exception_handler)
	{
		lock.unlock();
		CallExistingSignalHandler(signal, siginfo, ctx);
		return;
	}

	// Note: Use of stdio functions isn't safe here.  Avoid console logs, assertions, file logs,
	// or just about anything else useful. However, that's really only a concern if the signal
	// occurred within those functions. The logging which we do only happens when the exception
	// occurred within JIT code.

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

	s_in_exception_handler = true;

	const bool handled = s_exception_handler_callback(pfi);

	s_in_exception_handler = false;

	// Resumes execution right where we left off (re-executes instruction that caused the SIGSEGV).
	if (handled)
		return;

	// Call old signal handler, which will likely dump core.
	lock.unlock();
	CallExistingSignalHandler(signal, siginfo, ctx);
}
#endif

bool HostSys::InstallPageFaultHandler(PageFaultHandler handler)
{
	std::unique_lock lock(s_exception_handler_mutex);
#if defined(_WIN32)
	if (!s_exception_handler_handle)
	{
		s_exception_handler_handle = AddVectoredExceptionHandler(TRUE, SysPageFaultExceptionFilter);
		if (!s_exception_handler_handle)
			return false;
	}
#else
	if (!s_exception_handler_callback)
	{
		struct sigaction sa;

		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO;
		sa.sa_sigaction = SysPageFaultSignalFilter;
#ifdef __linux__
		// Don't block the signal from executing recursively, we want to fire the original handler.
		sa.sa_flags |= SA_NODEFER;
#endif
#if defined(__APPLE__) || defined(__aarch64__)
		// MacOS uses SIGBUS for memory permission violations, as well as SIGSEGV on ARM64.
		if (sigaction(SIGBUS, &sa, &s_old_sigbus_action) != 0)
			return false;
#endif
#if !defined(__APPLE__) || defined(__aarch64__)
		if (sigaction(SIGSEGV, &sa, &s_old_sigsegv_action) != 0)
			return false;
#endif
#if defined(__APPLE__) && defined(__aarch64__)
		// Stops LLDB getting in a EXC_BAD_ACCESS loop when passing page faults to PCSX2.
		task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
	}
#endif

	s_exception_handler_callback = handler;
	return true;
}

void HostSys::RemovePageFaultHandler(PageFaultHandler handler)
{
	std::unique_lock lock(s_exception_handler_mutex);
#ifdef _WIN32
	s_exception_handler_callback = nullptr;

	if (s_exception_handler_handle)
	{
		RemoveVectoredExceptionHandler(s_exception_handler_handle);
		s_exception_handler_handle = {};
	}
#else
	if (!s_exception_handler_callback)
		return;

	s_exception_handler_callback = nullptr;

	struct sigaction sa;
#if defined(__APPLE__) || defined(__aarch64__)
	sigaction(SIGBUS, &s_old_sigbus_action, &sa);
#endif
#if !defined(__APPLE__) || defined(__aarch64__)
	sigaction(SIGSEGV, &s_old_sigsegv_action, &sa);
#endif
#endif
}

#ifdef _WIN32
static DWORD win_prot(const PageProtectionMode mode)
{
	if (mode.m_read)
	{
		if (mode.m_exec)
			return mode.m_write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
		return mode.m_write ? PAGE_READWRITE : PAGE_READONLY;
	}
	return PAGE_NOACCESS;
}
#else
static __ri uint unix_prot(const PageProtectionMode mode)
{
	u32 ret = 0;
	if (mode.m_read)
	{
		ret |= PROT_READ;
		if (mode.m_exec)
			ret |= PROT_EXEC;
	}
	if (mode.m_write)
		ret |= PROT_WRITE;
	return ret;
}
#endif

void* HostSys::Mmap(void* base, size_t size, const PageProtectionMode mode)
{
	if (!mode.m_read && !mode.m_write)
		return nullptr;

#ifdef _WIN32
	return VirtualAlloc(base, size, MEM_RESERVE | MEM_COMMIT, win_prot(mode));
#else
	const u32 prot = unix_prot(mode);
	u32 flags      = MAP_PRIVATE | MAP_ANONYMOUS;
	if (base)
		flags |= MAP_FIXED;

#if defined(__APPLE__) && defined(_M_ARM64)
	if (mode.m_read && mode.m_exec)
		flags |= MAP_JIT;
#endif

	void* res = mmap(base, size, prot, flags, -1, 0);
	if (res == MAP_FAILED)
		return nullptr;
	return res;
#endif
}

void HostSys::Munmap(void* base, size_t size)
{
	if (!base)
		return;

#ifdef _WIN32
	VirtualFree((void*)base, 0, MEM_RELEASE);
#else
	munmap((void*)base, size);
#endif
}

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode mode)
{
#ifdef _WIN32
	DWORD OldProtect; // enjoy my uselessness, yo!
	VirtualProtect(baseaddr, size, win_prot(mode), &OldProtect);
#else
	const u32 prot = unix_prot(mode);
	mprotect(baseaddr, size, prot);
#endif
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
#if defined(_WIN32)
	const unsigned pid = GetCurrentProcessId();
	return fmt::format("{}_{}", prefix, pid);
#else
	const unsigned pid = static_cast<unsigned>(getpid());
#if defined(__FreeBSD__)
	// FreeBSD's shm_open(3) requires name to be absolute
	return fmt::format("/tmp/{}_{}", prefix, pid);
#else
	return fmt::format("{}_{}", prefix, pid);
#endif
#endif
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
#ifdef _WIN32
	wchar_t *wstr = utf8_to_utf16_string_alloc(name);
	void *ptr     = static_cast<void*>(CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
			static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), wstr));
	free(wstr);
	return ptr;
#else
	const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		return nullptr;

	// we're not going to be opening this mapping in other processes, so remove the file
	shm_unlink(name);

	// ensure it's the correct size
#if !defined(__APPLE__) && !defined(__FreeBSD__)
	if (ftruncate64(fd, static_cast<off64_t>(size)) < 0)
		return nullptr;
#else
	if (ftruncate(fd, static_cast<off_t>(size)) < 0)
		return nullptr;
#endif
	return  reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif
}

void HostSys::DestroySharedMemory(void* ptr)
{
#ifdef _WIN32
	CloseHandle(static_cast<HANDLE>(ptr));
#else
	close(static_cast<int>(reinterpret_cast<intptr_t>(ptr)));
#endif
}

void* HostSys::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, const PageProtectionMode mode)
{
#ifdef _WIN32
	void* ptr = MapViewOfFileEx(static_cast<HANDLE>(handle), FILE_MAP_READ | FILE_MAP_WRITE,
		static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), size, baseaddr);
	if (!ptr)
		return nullptr;

	const DWORD prot = win_prot(mode);
	if (prot != PAGE_READWRITE)
	{
		DWORD old_prot;
		VirtualProtect(ptr, size, prot, &old_prot);
	}
#else
	const uint prot = unix_prot(mode);
	const int flags = (baseaddr != nullptr) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
	void* ptr       = mmap(baseaddr, size, prot, flags, static_cast<int>(reinterpret_cast<intptr_t>(handle)), static_cast<off_t>(offset));
	if (ptr == MAP_FAILED)
		return nullptr;
#endif
	return ptr;
}

void HostSys::UnmapSharedMemory(void* baseaddr, size_t size)
{
#ifdef _WIN32
	UnmapViewOfFile(baseaddr);
#else
	mmap(baseaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
}

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
	// hopefully this will be okay, and we don't need to coalesce all the placeholders...
	VirtualFreeEx(GetCurrentProcess(), m_base_ptr, 0, MEM_RELEASE);
#else
	munmap(m_base_ptr, m_size);
#endif
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size)
{
#ifdef _WIN32
	void* alloc = VirtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
	if (!alloc)
		return nullptr;
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
	if (m_placeholder_ranges.empty())
		return m_placeholder_ranges.end();

	// this will give us an iterator equal or after page
	auto it = m_placeholder_ranges.lower_bound(offset);
	if (it == m_placeholder_ranges.end())
	{
		// check the last page
		it = (++m_placeholder_ranges.rbegin()).base();
	}

	// it's the one we found?
	if (offset >= it->first && offset < it->second)
		return it;

	// otherwise try the one before
	if (it == m_placeholder_ranges.begin())
		return m_placeholder_ranges.end();

	--it;
	if (offset >= it->first && offset < it->second)
		return it;
	else
		return m_placeholder_ranges.end();
}
#endif

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode mode)
{
#ifdef _WIN32
	const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
	// should be a placeholder. unless there's some other mapping we didn't free.
	PlaceholderMap::iterator phit = FindPlaceholder(map_offset);

	// do we need to split to the left? (i.e. is there a placeholder before this range)
	const size_t old_ph_end = phit->second;
	if (map_offset != phit->first)
	{
		phit->second = map_offset;

		// split it (i.e. left..start and start..end are now separated)
		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(phit->first),
				(map_offset - phit->first), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
	}
	else
	{
		// start of the placeholder is getting used, we'll split it right below if there's anything left over
		m_placeholder_ranges.erase(phit);
	}

	// do we need to split to the right? (i.e. is there a placeholder after this range)
	if ((map_offset + map_size) != old_ph_end)
	{
		// split out end..ph_end
		m_placeholder_ranges.emplace(map_offset + map_size, old_ph_end);

		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(map_offset), map_size,
				MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
	}

	// actually do the mapping, replacing the placeholder on the range
	if (!MapViewOfFile3(static_cast<HANDLE>(file_handle), GetCurrentProcess(),
			map_base, file_offset, map_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
	{
		Console.Error("(SharedMemoryMappingArea) MapViewOfFile3() failed: %u", GetLastError());
		return nullptr;
	}

	const DWORD prot = win_prot(mode);
	if (prot != PAGE_READWRITE)
	{
		DWORD old_prot;
		VirtualProtect(map_base, map_size, prot, &old_prot);
	}

	m_num_mappings++;
	return static_cast<u8*>(map_base);
#else
	const uint prot    = unix_prot(mode);
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
	// unmap the specified range
	if (!UnmapViewOfFile2(GetCurrentProcess(), map_base, MEM_PRESERVE_PLACEHOLDER))
	{
		Console.Error("(SharedMemoryMappingArea) UnmapViewOfFile2() failed: %u", GetLastError());
		return false;
	}

	// can we coalesce to the left?
	PlaceholderMap::iterator left_it = (map_offset > 0) ? FindPlaceholder(map_offset - 1) : m_placeholder_ranges.end();
	if (left_it != m_placeholder_ranges.end())
	{
		// the left placeholder should end at our start
		left_it->second = map_offset + map_size;

		// combine placeholders before and the range we're unmapping, i.e. to the left
		VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first),
				 left_it->second - left_it->first, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
	}
	else // this is a new placeholder
		left_it = m_placeholder_ranges.emplace(map_offset, map_offset + map_size).first;

	// can we coalesce to the right?
	PlaceholderMap::iterator right_it = ((map_offset + map_size) < m_size) ? FindPlaceholder(map_offset + map_size) : m_placeholder_ranges.end();
	if (right_it != m_placeholder_ranges.end())
	{
		// should start at our end
		left_it->second = right_it->second;
		m_placeholder_ranges.erase(right_it);

		// combine our placeholder and the next, i.e. to the right
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

#ifdef _M_ARM64
void HostSys::FlushInstructionCache(void* address, u32 size)
{
#ifdef _WIN32
	::FlushInstructionCache(GetCurrentProcess(), address, size);
#else
	__builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address) + size);
#endif
}

#if defined(__APPLE__)
static thread_local int s_code_write_depth = 0;

void HostSys::BeginCodeWrite(void)
{
	if ((s_code_write_depth++) == 0)
		pthread_jit_write_protect_np(0);
}

void HostSys::EndCodeWrite(void)
{
	if ((--s_code_write_depth) == 0)
		pthread_jit_write_protect_np(1);
}
#endif

#endif
