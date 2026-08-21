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

#include <algorithm> /* std::max, std::min */
#include <cstring>

#include "ThreadedFileReader.h"
#include "ThreadedFileReader.h"

#include <cstring>
#include <algorithm>

// Make sure buffer size is bigger than the cutoff where PCSX2 emulates a seek
// If buffers are smaller than that, we can't keep up with linear reads
static constexpr u32 MINIMUM_SIZE = 128 * 1024;

ThreadedFileReader::ThreadedFileReader() = default;

int ThreadedFileReader::DirectRead(void* dst, u64 offset, u32 size)
{
	if (offset >= m_directSize)
		return 0;
	const u64 avail = m_directSize - offset;
	const u32 len   = static_cast<u32>(pcsx2_min_u64(size, avail));
	memcpy(dst, m_direct + offset, len);
	return static_cast<int>(len);
}

ThreadedFileReader::~ThreadedFileReader()
{
	for (auto& buffer : m_buffer)
		if (buffer.ptr)
			free(buffer.ptr);
}

ThreadedFileReader::Buffer* ThreadedFileReader::GetBlockPtr(const Chunk& block)
{
	for (int i = 0; i < static_cast<int>(C89_ARRAY_SIZE(m_buffer)); i++)
	{
		u32 size   = m_buffer[i].size;
		u64 offset = m_buffer[i].offset;
		if (size && offset <= block.offset && offset + size >= block.offset + block.length)
		{
			m_nextBuffer = (i + 1) % C89_ARRAY_SIZE(m_buffer);
			return m_buffer + i;
		}
	}

	Buffer& buf = m_buffer[m_nextBuffer];
	u32 size = pcsx2_max_i(block.length, MINIMUM_SIZE);
	if (buf.cap < size)
	{
		void* new_ptr = realloc(buf.ptr, size);
		if (!new_ptr)
			return nullptr;
		buf.ptr = new_ptr;
		buf.cap = size;
	}
	buf.size = 0;
	int amt = ReadChunk(buf.ptr, block.chunkID);
	if (amt > 0)
	{
		buf.offset = block.offset;
		buf.size   = amt;
		m_nextBuffer = (m_nextBuffer + 1) % C89_ARRAY_SIZE(m_buffer);
		return &buf;
	}
	return nullptr;
}

bool ThreadedFileReader::Decompress(void* target, u64 begin, u32 size)
{
	char* write   = static_cast<char*>(target);
	u32 remaining = size;
	u64 off       = begin;
	if (m_internalBlockSize)
	{
		while (remaining)
		{
			Chunk chunk = ChunkForOffset(off);
			Buffer* buf = GetBlockPtr(chunk);
			if (!buf)
				return false;
			u32 bufoff  = off - buf->offset;
			u32 bufsize = buf->size;
			if (bufsize <= bufoff)
				return false;
			u32 len     = pcsx2_min_i(bufsize - bufoff, remaining);
			char* cdst       = static_cast<char*>(write);
			const char* csrc = static_cast<const char*>(static_cast<char*>(buf->ptr) + bufoff);
			const char* cend = csrc + len;
			for (; csrc < cend; csrc += m_internalBlockSize, cdst += m_blocksize)
				memcpy(cdst, csrc, m_blocksize);
			write      += cdst - static_cast<char*>(write);
			remaining  -= len;
			off        += len;
		}
	}
	else
	{
		while (remaining)
		{
			Chunk chunk = ChunkForOffset(off);
			if (chunk.offset != off || chunk.length > remaining)
			{
				Buffer* buf = GetBlockPtr(chunk);
				if (!buf)
					return false;
				u32 bufoff  = off - buf->offset;
				u32 bufsize = buf->size;
				if (bufsize <= bufoff)
					return false;
				u32 len     = pcsx2_min_i(bufsize - bufoff, remaining);
				memcpy(write, static_cast<char*>(buf->ptr) + bufoff, len);
				write      += len;
				remaining  -= len;
				off        += len;
			}
			else
			{
				int amt    = ReadChunk(write, chunk.chunkID);
				if (amt < static_cast<int>(chunk.length))
					return false;
				write     += chunk.length;
				remaining -= chunk.length;
				off       += chunk.length;
			}
		}
	}
	m_amtRead += write - static_cast<char*>(target);
	return true;
}

bool ThreadedFileReader::TryCachedRead(void*& buffer, u64& offset, u32& size)
{
	// Run through twice so that if m_buffer[1] contains the first half and m_buffer[0] contains the second half it still works
	m_amtRead    = 0;
	u64 end      = 0;
	bool allDone = false;
	if (m_internalBlockSize)
	{
		for (int i = 0; i < static_cast<int>(C89_ARRAY_SIZE(m_buffer) * 2); i++)
		{
			Buffer& buf = m_buffer[i % C89_ARRAY_SIZE(m_buffer)];
			u32 bufsize = buf.size;
			if (!bufsize)
				continue;
			if (buf.offset <= offset && buf.offset + bufsize > offset)
			{
				size_t read;
				u32 off          = offset - buf.offset;
				u32 cpysize      = pcsx2_min_i(size, bufsize - off);
				char* cdst       = static_cast<char*>(buffer);
				const char* csrc = static_cast<const char*>(static_cast<char*>(buf.ptr) + off);
				const char* cend = csrc + cpysize;
				for (; csrc < cend; csrc += m_internalBlockSize, cdst += m_blocksize)
					memcpy(cdst, csrc, m_blocksize);
				read = cdst - static_cast<char*>(buffer);
				m_amtRead   += read;
				size        -= cpysize;
				offset      += cpysize;
				buffer       = static_cast<char*>(buffer) + read;
				if (size == 0)
					end  = buf.offset + bufsize;
			}
			// Do buffers contain the current and next block?
			if (end > 0 && buf.offset == end)
				allDone = true;
		}
	}
	else
	{
		for (int i = 0; i < static_cast<int>(C89_ARRAY_SIZE(m_buffer) * 2); i++)
		{
			Buffer& buf = m_buffer[i % C89_ARRAY_SIZE(m_buffer)];
			u32 bufsize = buf.size;
			if (!bufsize)
				continue;
			if (buf.offset <= offset && buf.offset + bufsize > offset)
			{
				size_t read;
				u32 off      = offset - buf.offset;
				u32 cpysize  = pcsx2_min_i(size, bufsize - off);
				memcpy(buffer, static_cast<char*>(buf.ptr) + off, cpysize);
				read         = cpysize;
				m_amtRead   += read;
				size        -= cpysize;
				offset      += cpysize;
				buffer       = static_cast<char*>(buffer) + read;
				if (size == 0)
					end  = buf.offset + bufsize;
			}
			// Do buffers contain the current and next block?
			if (end > 0 && buf.offset == end)
				allDone = true;
		}
	}
	return allDone;
}

bool ThreadedFileReader::Open(std::string filename)
{
	m_direct     = nullptr;
	m_directSize = 0;
	return Open2(std::move(filename));
}

int ThreadedFileReader::ReadSync(void* pBuffer, u32 sector, u32 count)
{
	u32 blocksize = m_internalBlockSize ? m_internalBlockSize : m_blocksize;
	u64 offset    = (u64)sector * (u64)blocksize + m_dataoffset;
	u32 size      = count * blocksize;
	if (m_direct && !m_internalBlockSize)
		return DirectRead(pBuffer, offset, size);
	if (TryCachedRead(pBuffer, offset, size))
		return m_amtRead;
	if (Decompress(pBuffer, offset, size))
		return m_amtRead;
	return -1;
}

void ThreadedFileReader::BeginRead(void* pBuffer, u32 sector, u32 count)
{
	s32 blocksize = m_internalBlockSize ? m_internalBlockSize : m_blocksize;
	u64 offset    = (u64)sector * (u64)blocksize + m_dataoffset;
	u32 size      = count * blocksize;
	if (m_direct && !m_internalBlockSize)
	{
		m_directAmt = DirectRead(pBuffer, offset, size);
		return;
	}
	/* Synchronous: the read completes here; FinishRead only reports it.
	 * The emulated drive's seek and rotation model (cdvdBlockReadTime)
	 * spaces sector deliveries by far more virtual time than one chunk
	 * decode costs in real time, and the two-buffer cache means a chunk
	 * is decoded once and then serves every sector inside it. */
	if (TryCachedRead(pBuffer, offset, size))
		return;
	if (!Decompress(pBuffer, offset, size))
		m_amtRead = -1;
}

int ThreadedFileReader::FinishRead(void)
{
	if (m_direct && !m_internalBlockSize)
		return m_directAmt;
	return m_amtRead;
}

void ThreadedFileReader::CancelRead(void)
{
	/* Nothing is ever in flight. */
}

void ThreadedFileReader::Close(void)
{
	for (auto& buf : m_buffer)
		buf.size = 0;
	Close2();
}

void ThreadedFileReader::SetBlockSize(u32 bytes)
{
	m_blocksize = bytes;
}

void ThreadedFileReader::SetDataOffset(u32 bytes)
{
	m_dataoffset = bytes;
}
