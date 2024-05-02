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

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include "common/Pcsx2Defs.h"

// This macro is actually useful for about any and every possible application of C++
// equality operators.
#define OpEqu(field) (field == right.field)

// Macro used for removing some of the redtape involved in defining bitfield/union helpers.
//
#define BITFIELD32() \
	union \
	{ \
		u32 bitset; \
		struct \
		{

#define BITFIELD_END \
		}; \
	};

// --------------------------------------------------------------------------------------
//  PageProtectionMode
// --------------------------------------------------------------------------------------
struct PageProtectionMode
{
	bool m_read;
	bool m_write;
	bool m_exec;
};

struct PageFaultInfo
{
	uptr pc;
	uptr addr;
};

using PageFaultHandler = bool(*)(const PageFaultInfo& info);

// --------------------------------------------------------------------------------------
//  HostSys
// --------------------------------------------------------------------------------------
namespace HostSys
{
	// Maps a block of memory for use as a recompiled code buffer.
	// Returns NULL on allocation failure.
	extern void* Mmap(void* base, size_t size, const PageProtectionMode mode);

	// Unmaps a block allocated by SysMmap
	extern void Munmap(void* base, size_t size);

	extern void MemProtect(void* baseaddr, size_t size, const PageProtectionMode mode);

	extern std::string GetFileMappingName(const char* prefix);
	extern void* CreateSharedMemory(const char* name, size_t size);
	extern void DestroySharedMemory(void* ptr);
	extern void* MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, const PageProtectionMode mode);
	extern void UnmapSharedMemory(void* baseaddr, size_t size);

	/// Installs the specified page fault handler. Only one handler can be active at once.
	bool InstallPageFaultHandler(PageFaultHandler handler);

	/// Removes the page fault handler. handler is only specified to check against the active callback.
	void RemovePageFaultHandler(PageFaultHandler handler);

	/// Flushes the instruction cache on the host for the specified range.
	/// Only needed on ARM64, X86 has coherent D/I cache.
#ifdef _M_X86
	__fi static void FlushInstructionCache(void* address, u32 size) {}
#else
	void FlushInstructionCache(void* address, u32 size);
#endif
}

class SharedMemoryMappingArea
{
public:
	static std::unique_ptr<SharedMemoryMappingArea> Create(size_t size);

	~SharedMemoryMappingArea();

	__fi size_t GetSize() const { return m_size; }
	__fi size_t GetNumPages() const { return m_num_pages; }

	__fi u8* BasePointer() const { return m_base_ptr; }
	__fi u8* OffsetPointer(size_t offset) const { return m_base_ptr + offset; }
	__fi u8* PagePointer(size_t page) const { return m_base_ptr + __pagesize * page; }

	u8* Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode mode);
	bool Unmap(void* map_base, size_t map_size);

private:
	SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages);

	u8* m_base_ptr;
	size_t m_size;
	size_t m_num_pages;
	size_t m_num_mappings = 0;

#ifdef _WIN32
	using PlaceholderMap = std::map<size_t, size_t>;

	PlaceholderMap::iterator FindPlaceholder(size_t page);

	PlaceholderMap m_placeholder_ranges;
#endif
};
