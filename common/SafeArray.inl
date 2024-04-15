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

#include "common/SafeArray.h"

// Internal constructor for use by derived classes.  This allows a derived class to
// use its own memory allocation (with an aligned memory, for example).
// Throws:
//   Exception::OutOfMemory if the allocated_mem pointer is NULL.
template <typename T>
SafeArray<T>::SafeArray(std::string name, T* allocated_mem, int initSize)
	: Name(std::move(name))
{
	ChunkSize = DefaultChunkSize;
	m_ptr = allocated_mem;
	m_size = initSize;
}

template <typename T>
T* SafeArray<T>::_virtual_realloc(int newsize)
{
	T* retval = (T*)((m_ptr == NULL) ?
                         malloc(newsize * sizeof(T)) :
                         realloc(m_ptr, newsize * sizeof(T)));

#ifndef NDEBUG
	if (retval)
	{
		// Zero everything out to 0xbaadf00d, so that its obviously uncleared
		// to a debuggee

		u32* fill = (u32*)&retval[m_size];
		const u32* end = (u32*)((((uptr)&retval[newsize - 1]) - 3) & ~0x3);
		for (; fill < end; ++fill)
			*fill = 0xbaadf00d;
	}
#endif

	return retval;
}

template <typename T>
SafeArray<T>::~SafeArray()
{
	if (m_ptr)
		free(m_ptr);
	m_ptr = NULL;
}

template <typename T>
SafeArray<T>::SafeArray(std::string name)
	: Name(std::move(name))
{
	ChunkSize = DefaultChunkSize;
	m_ptr = NULL;
	m_size = 0;
}

template <typename T>
SafeArray<T>::SafeArray(int initialSize, std::string name)
	: Name(std::move(name))
{
	ChunkSize = DefaultChunkSize;
	m_ptr = (initialSize == 0) ? NULL : (T*)malloc(initialSize * sizeof(T));
	m_size = initialSize;
}

// Clears the contents of the array to zero, and frees all memory allocations.
template <typename T>
void SafeArray<T>::Dispose()
{
	m_size = 0;
	if (m_ptr)
		free(m_ptr);
	m_ptr = NULL;
}

template <typename T>
T* SafeArray<T>::_getPtr(uint i) const
{
	return &m_ptr[i];
}

// reallocates the array to the explicit size.  Can be used to shrink or grow an
// array, and bypasses the internal threshold growth indicators.
template <typename T>
void SafeArray<T>::ExactAlloc(int newsize)
{
	if (newsize == m_size)
		return;

	m_ptr  = _virtual_realloc(newsize);
	m_size = newsize;
}
