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

#include "VirtualMemory.h"

#include "../common/Align.h"
#include "../common/Console.h"

#include <cinttypes>

// --------------------------------------------------------------------------------------
//  VirtualMemoryManager  (implementations)
// --------------------------------------------------------------------------------------

VirtualMemoryManager::VirtualMemoryManager(const char* file_mapping_name, uptr base, size_t size, uptr upper_bounds, bool strict)
	: m_file_handle(nullptr)
	, m_baseptr(0)
	, m_pageuse(nullptr)
	, m_pages_reserved(0)
{
	if (!size)
		return;

	size_t reserved_bytes = Common::PageAlign(size);
	m_pages_reserved = reserved_bytes / __pagesize;

	if (file_mapping_name && file_mapping_name[0])
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = false;
		std::string real_file_mapping_name(HostSys::GetFileMappingName(file_mapping_name));
		m_file_handle = HostSys::CreateSharedMemory(real_file_mapping_name.c_str(), reserved_bytes);
		if (!m_file_handle)
			return;

		m_baseptr = static_cast<u8*>(HostSys::MapSharedMemory(m_file_handle, 0, (void*)base, reserved_bytes, mode));
		if (!m_baseptr || (upper_bounds != 0 && (((uptr)m_baseptr + reserved_bytes) > upper_bounds)))
		{
			HostSys::Munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			// Let's try again at an OS-picked memory area, and then hope it meets needed
			// boundschecking criteria below.
			if (base)
				m_baseptr = static_cast<u8*>(HostSys::MapSharedMemory(m_file_handle, 0, nullptr, reserved_bytes, mode));
		}
	}
	else
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = true;
		m_baseptr    = static_cast<u8*>(HostSys::Mmap((void*)base, reserved_bytes, mode));

		if (!m_baseptr || (upper_bounds != 0 && (((uptr)m_baseptr + reserved_bytes) > upper_bounds)))
		{
			HostSys::Munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			// Let's try again at an OS-picked memory area, and then hope it meets needed
			// boundschecking criteria below.
			if (base)
				m_baseptr = static_cast<u8*>(HostSys::Mmap(0, reserved_bytes, mode));
		}
	}

	bool fulfillsRequirements = true;
	if (strict && (uptr)m_baseptr != base)
		fulfillsRequirements = false;
	if ((upper_bounds != 0) && ((uptr)(m_baseptr + reserved_bytes) > upper_bounds))
		fulfillsRequirements = false;
	if (!fulfillsRequirements)
	{
		if (m_file_handle)
		{
			if (m_baseptr)
				HostSys::UnmapSharedMemory(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			HostSys::DestroySharedMemory(m_file_handle);
			m_file_handle = nullptr;
		}
		else
		{
			HostSys::Munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;
		}
	}

	if (!m_baseptr)
		return;

	m_pageuse = new std::atomic<bool>[m_pages_reserved]();
}

VirtualMemoryManager::~VirtualMemoryManager()
{
	if (m_pageuse)
		delete[] m_pageuse;
	if (m_baseptr)
	{
		if (m_file_handle)
			HostSys::UnmapSharedMemory((void*)m_baseptr, m_pages_reserved * __pagesize);
		else
			HostSys::Munmap(m_baseptr, m_pages_reserved * __pagesize);
	}
	if (m_file_handle)
		HostSys::DestroySharedMemory(m_file_handle);
}

static bool VMMMarkPagesAsInUse(std::atomic<bool>* begin, std::atomic<bool>* end)
{
	for (auto current = begin; current < end; current++)
	{
		bool expected = false;
		if (!current->compare_exchange_strong(expected, true, std::memory_order_relaxed))
		{
			// This was already allocated!  Undo the things we've set until this point
			while (--current >= begin)
			{
				// In the time we were doing this, someone set one of the things we just set to true back to false
				// This should never happen, but if it does we'll just stop and hope nothing bad happens
				if (!current->compare_exchange_strong(expected, false, std::memory_order_relaxed))
					return false;
			}
			return false;
		}
	}
	return true;
}

u8* VirtualMemoryManager::Alloc(uptr offsetLocation, size_t size) const
{
	size = Common::PageAlign(size);
	if (!(offsetLocation % __pagesize == 0))
		return nullptr;
	if (!(size + offsetLocation <= m_pages_reserved * __pagesize))
		return nullptr;
	if (m_baseptr == 0)
		return nullptr;
	auto puStart = &m_pageuse[offsetLocation / __pagesize];
	auto puEnd = &m_pageuse[(offsetLocation + size) / __pagesize];
	if (!(VMMMarkPagesAsInUse(puStart, puEnd)))
		return nullptr;
	return m_baseptr + offsetLocation;
}

void VirtualMemoryManager::Free(void* address, size_t size) const
{
	uptr offsetLocation = (uptr)address - (uptr)m_baseptr;
	if (!(offsetLocation % __pagesize == 0))
	{
		uptr newLoc = Common::PageAlign(offsetLocation);
		size -= (offsetLocation - newLoc);
		offsetLocation = newLoc;
	}
	if (!(size % __pagesize == 0))
		size -= size % __pagesize;
	if (!(size + offsetLocation <= m_pages_reserved * __pagesize))
		return;
	auto puStart = &m_pageuse[offsetLocation / __pagesize];
	auto puEnd = &m_pageuse[(offsetLocation + size) / __pagesize];
	for (; puStart < puEnd; puStart++)
	{
		bool expected = true;
		if (!puStart->compare_exchange_strong(expected, false, std::memory_order_relaxed)) { }
	}
}

// --------------------------------------------------------------------------------------
//  VirtualMemoryBumpAllocator  (implementations)
// --------------------------------------------------------------------------------------
VirtualMemoryBumpAllocator::VirtualMemoryBumpAllocator(VirtualMemoryManagerPtr allocator, uptr offsetLocation, size_t size)
	: m_allocator(std::move(allocator))
	, m_baseptr(m_allocator->Alloc(offsetLocation, size))
	, m_endptr(m_baseptr + size)
{
}

u8* VirtualMemoryBumpAllocator::Alloc(size_t size)
{
	if (m_baseptr.load() == 0) // True if constructed from bad VirtualMemoryManager (assertion was on initialization)
		return nullptr;

	size_t reservedSize = Common::PageAlign(size);

	u8* out = m_baseptr.fetch_add(reservedSize, std::memory_order_relaxed);

	return out;
}

// --------------------------------------------------------------------------------------
//  VirtualMemoryReserve  (implementations)
// --------------------------------------------------------------------------------------
VirtualMemoryReserve::VirtualMemoryReserve() { }
VirtualMemoryReserve::~VirtualMemoryReserve() { }

// Notes:
//  * This method should be called if the object is already in an released (unreserved) state.
//    Subsequent calls will be ignored, and the existing reserve will be returned.
//
// Parameters:
//   baseptr - the new base pointer that's about to be assigned
//   size - size of the region pointed to by baseptr
//
void VirtualMemoryReserve::Assign(VirtualMemoryManagerPtr allocator, u8* baseptr, size_t size)
{
	m_allocator = std::move(allocator);
	m_baseptr = baseptr;
	m_size = size;
}

u8* VirtualMemoryReserve::BumpAllocate(VirtualMemoryBumpAllocator& allocator, size_t size)
{
	u8* base = allocator.Alloc(size);
	if (base)
		Assign(allocator.GetAllocator(), base, size);

	return base;
}

void VirtualMemoryReserve::Release()
{
	if (!m_baseptr)
		return;

	m_allocator->Free(m_baseptr, m_size);
	m_allocator.reset();
	m_baseptr = nullptr;
	m_size = 0;
}

// --------------------------------------------------------------------------------------
//  RecompiledCodeReserve  (implementations)
// --------------------------------------------------------------------------------------

// Constructor!
// Parameters:
//   name - a nice long name that accurately describes the contents of this reserve.
RecompiledCodeReserve::RecompiledCodeReserve() : VirtualMemoryReserve() { }
RecompiledCodeReserve::~RecompiledCodeReserve() { Release(); }

void RecompiledCodeReserve::Assign(VirtualMemoryManagerPtr allocator, size_t offset, size_t size)
{
	// Anything passed to the memory allocator must be page aligned.
	size = Common::PageAlign(size);

	// Since the memory has already been allocated as part of the main memory map, this should never fail.
	u8* base = allocator->Alloc(offset, size);
	if (!base)
		Console.Error("(RecompiledCodeReserve) Failed to allocate %zu bytes at offset %zu", size, offset);

	VirtualMemoryReserve::Assign(std::move(allocator), base, size);
}

void RecompiledCodeReserve::Reset()
{
}

void RecompiledCodeReserve::AllowModification()
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = true;
	HostSys::MemProtect(m_baseptr, m_size, pg);
}

void RecompiledCodeReserve::ForbidModification()
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = false;
	HostSys::MemProtect(m_baseptr, m_size, pg);
}
