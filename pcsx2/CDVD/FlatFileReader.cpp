// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+

#include <compat/strl.h>
#include "FlatFileReader.h"

#include "../../common/Console.h"
#include "HostFS.h"

#include <streams/file_stream.h>

#include <algorithm> /* std::min */
#include <cstring>

static constexpr size_t CHUNK_SIZE = 128 * 1024;

/* Ops thunks: the base calls these with a ThreadedFileReader*, which is
 * always a FlatFileReader here. */
static ThreadedFileReader::Chunk FlatFileReader_chunk_for_offset(ThreadedFileReader* self, u64 offset)
{ return static_cast<FlatFileReader*>(self)->ChunkForOffset(offset); }
static int FlatFileReader_read_chunk(ThreadedFileReader* self, void* dst, s64 chunkID)
{ return static_cast<FlatFileReader*>(self)->ReadChunk(dst, chunkID); }
static bool FlatFileReader_open2(ThreadedFileReader* self, const char* filename)
{ return static_cast<FlatFileReader*>(self)->Open2(filename); }
static void FlatFileReader_close2(ThreadedFileReader* self)
{ static_cast<FlatFileReader*>(self)->Close2(); }
static u32 FlatFileReader_block_count(const ThreadedFileReader* self)
{ return static_cast<const FlatFileReader*>(self)->GetBlockCount(); }

const ThreadedFileReader::Ops FlatFileReader::s_ops =
{
	FlatFileReader_chunk_for_offset, FlatFileReader_read_chunk, FlatFileReader_open2, FlatFileReader_close2, FlatFileReader_block_count
};

FlatFileReader::FlatFileReader()
{
	m_ops = &s_ops;
}

FlatFileReader::~FlatFileReader()
{
	Close2();
}

bool FlatFileReader::Open2(const char* filename)
{
	strlcpy(m_filename, filename, sizeof(m_filename));
	/* FREQUENT_ACCESS invites the local VFS to memory-map: an ISO is
	   read sector-by-sector for the whole session.  A mapping turns
	   every read into a page-cache memcpy on the calling thread and
	   lets the base class skip its worker thread and staging buffers
	   entirely; without one, the threaded chunk path below runs
	   exactly as before. */
	m_file = filestream_open(m_filename,
			RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
	if (!m_file)
		return false;

	const s64 filesize = filestream_get_size(m_file);
	if (filesize <= 0)
	{
		/* Named, because the caller can only report that the VM failed
		 * to start.  A file that opens but cannot be sized is a VFS
		 * that cannot address it - a 32-bit file offset under a large
		 * image is what this looked like in the field, and it is worth
		 * one line to say so rather than leaving the whole disc open
		 * as a silent false. */
		Console.Error("CDVD: cannot determine size of %s (VFS reported %lld) - image unreadable",
				m_filename, static_cast<long long>(filesize));
		Close2();
		return false;
	}

	m_file_size = static_cast<u64>(filesize);

	{
		int64_t maplen = 0;
		const uint8_t* base = filestream_get_mapped_ptr(m_file, &maplen);
		if (base && maplen >= filesize)
			SetDirectSpan(base, m_file_size);
	}
	return true;
}

ThreadedFileReader::Chunk FlatFileReader::ChunkForOffset(u64 offset)
{
	ThreadedFileReader::Chunk chunk = {};
	if (offset >= m_file_size)
		chunk.chunkID = -1;
	else
	{
		chunk.chunkID = offset / CHUNK_SIZE;
		chunk.length = static_cast<u32>(pcsx2_min_u64(m_file_size - offset, CHUNK_SIZE));
		chunk.offset = static_cast<u64>(chunk.chunkID) * CHUNK_SIZE;
	}

	return chunk;
}

int FlatFileReader::ReadChunk(void* dst, s64 blockID)
{
	if (blockID < 0)
		return -1;

	const u64 file_offset = static_cast<u64>(blockID) * CHUNK_SIZE;
	if (filestream_seek(m_file, file_offset, RETRO_VFS_SEEK_POSITION_START) != 0)
		return -1;

	const u32 read_size = static_cast<u32>(pcsx2_min_u64(m_file_size - file_offset, CHUNK_SIZE));

	return (filestream_read(m_file, dst, read_size) == (int64_t)read_size) ? static_cast<int>(read_size) : 0;
}

void FlatFileReader::Close2()
{
	if (!m_file)
		return;

	filestream_close(m_file);
	m_file = nullptr;
	m_file_size = 0;
}

u32 FlatFileReader::GetBlockCount() const
{
	return static_cast<u32>(m_file_size / m_blocksize);
}
