/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

/* SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
 * SPDX-License-Identifier: LGPL-3.0+ */

#pragma once

#include "../../common/Pcsx2Defs.h"

#include <retro_miscellaneous.h>

#include <string>

/// A file reader for use with compressed formats
/// Chunked/compressed file reader over a two-buffer chunk cache.
/// Fully synchronous: reads decompress on the calling thread, where the
/// emulated drive's own seek/read latency model already paces sector
/// delivery.  (The class name is historical.)
class ThreadedFileReader
{
	ThreadedFileReader(ThreadedFileReader&&) = delete;
protected:
	/* Fixed buffer rather than std::string: a path has a bound, the
	 * libretro path API this tree already uses works on char*, and the
	 * member was only ever read back through c_str(). */
	char m_filename[PATH_MAX_LENGTH];

	u32 m_dataoffset = 0;
	u32 m_blocksize = 2048;

	struct Chunk
	{
		/// Negative block IDs indicate invalid blocks
		s64 chunkID;
		u64 offset;
		u32 length;
	};

	/// Set nonzero to separate block size of read blocks from m_blocksize
	/// Requires that chunk size is a multiple of internal block size
	/// Use to avoid overrunning stack because PCSX2 likes to allocate 2448-byte buffers
	int m_internalBlockSize = 0;

	/// Get the block containing the given offset
	virtual Chunk ChunkForOffset(u64 offset) = 0;
	/// Synchronously read the given block into `dst`
	virtual int ReadChunk(void* dst, s64 chunkID) = 0;
	/// AsyncFileReader open but ThreadedFileReader needs prep work first
	virtual bool Open2(const char* filename) = 0;
	/// AsyncFileReader close but ThreadedFileReader needs prep work first
	virtual void Close2() = 0;

	ThreadedFileReader();

	/// Zero-copy span for readers whose entire logical content is
	/// directly addressable (a memory-mapped flat file).  When set (by
	/// the subclass during Open2), reads bypass the worker thread, the
	/// mutex, and the staging buffers entirely: a read is one memcpy
	/// from the page cache on the calling thread.  The worker thread is
	/// then never even started - readahead is the kernel's job for
	/// mapped files, and a thread that exists to hide synchronous I/O
	/// latency has nothing to hide when there is no I/O.
	void SetDirectSpan(const void* base, u64 size)
	{
		m_direct     = static_cast<const u8*>(base);
		m_directSize = size;
	}

private:
	const u8* m_direct = nullptr;
	u64 m_directSize   = 0;
	int m_directAmt    = 0;
	/// Direct-span read: clamped memcpy, returns bytes copied
	int DirectRead(void* dst, u64 offset, u32 size);

	/* Result of the last BeginRead (bytes, or -1); FinishRead returns it.
	 * The reader is synchronous - the name is historical. */
	int m_amtRead;
	struct Buffer
	{
		void* ptr  = nullptr;
		u64 offset = 0;
		u32 size   = 0;
		u32 cap    = 0;
	};
	/// 2 buffers so a read spanning two chunks can source both halves
	Buffer m_buffer[2];
	u32 m_nextBuffer = 0;

	/// Load the given block into one of the `m_buffer` buffers if necessary and return a pointer to its contents if successful
	Buffer* GetBlockPtr(const Chunk& block);
	/// Decompress from offset to size into
	bool Decompress(void* ptr, u64 offset, u32 size);
	/// Attempt to read from the cache
	/// Adjusts pointer, offset, and size if successful
	/// Returns true if no additional reads are necessary
	bool TryCachedRead(void*& buffer, u64& offset, u32& size);

public:
	virtual ~ThreadedFileReader();

	virtual u32 GetBlockCount() const = 0;

	bool Open(const char* filename);
	int ReadSync(void* pBuffer, u32 sector, u32 count);
	void BeginRead(void* pBuffer, u32 sector, u32 count);
	int FinishRead();
	void CancelRead();
	void Close();
	void SetBlockSize(u32 bytes);
	void SetDataOffset(u32 bytes);
};
