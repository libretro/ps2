/*  PCSX2 - PS2 Emulator for PCs
*  Copyright (C) 2002-2014  PCSX2 Dev Team
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

#include <compat/strl.h>
#include <encodings/rlz4.h>

#include "../../common/Pcsx2Types.h"
#include "../../common/Console.h"
#include "../../common/FileSystem.h"
#include "../../common/StringUtil.h"

#include "CsoFileReader.h"

// Implementation of CSO compressed ISO reading, based on:
// https://github.com/unknownbrackets/maxcso/blob/master/README_CSO.md
struct CsoHeader
{
	u8 magic[4];
	u32 header_size;
	u64 total_bytes;
	u32 frame_size;
	u8 ver;
	u8 align;
	u8 reserved[2];
};

static const u32 CSO_READ_BUFFER_SIZE = 256 * 1024;

CsoFileReader::CsoFileReader()
	: m_frameSize(0),
	  m_frameShift(0),
	  m_indexShift(0),
	  m_uselz4(false),
	  m_readBuffer(nullptr),
	  m_index(nullptr),
	  m_totalSize(0),
	  m_src(nullptr),
	  m_inflate(nullptr)
{
}

CsoFileReader::~CsoFileReader() { }


bool CsoFileReader::ValidateHeader(const CsoHeader& hdr)
{
	// Invalid magic, definitely a bad file.
	if ((hdr.magic[0] != 'C' && hdr.magic[0] != 'Z') || hdr.magic[1] != 'I' || hdr.magic[2] != 'S' || hdr.magic[3] != 'O')
		return false;
	if (hdr.ver > 1)
	{
		Console.Error("Only CSOv1 files are supported.");
		return false;
	}
	if ((hdr.frame_size & (hdr.frame_size - 1)) != 0)
	{
		Console.Error("CSO frame size must be a power of two.");
		return false;
	}
	if (hdr.frame_size < 2048)
	{
		Console.Error("CSO frame size must be at least one sector.");
		return false;
	}
	/* The three fields below decide how much memory the header can make
	 * us allocate, and none of them was bounded.
	 *
	 * frame_size only had to be a power of two of at least 2048, so
	 * 0x80000000 passed and asked new[] for 2 GB - on a build compiled
	 * -fno-exceptions, where a failed allocation is not something that
	 * can be reported.  16 MiB is already far past any real CSO, whose
	 * frames run 2 KiB to 64 KiB.
	 *
	 * align is a shift.  At 32 or more, 1 << align is undefined; a
	 * little under that it makes the read buffer enormous.  It shifts
	 * index values into file offsets, so anything past 16 already
	 * describes a file no real tool produces.
	 *
	 * total_bytes drives the frame count and with it the index
	 * allocation.  At 0xFFFFFFFFFFFFFFFF the frame count truncates to
	 * u32 and the +1 for the index wraps to zero, which reads nothing,
	 * compares equal to the expected count, and leaves every later
	 * index lookup reading off the end of a zero-length array.  A
	 * double-layer DVD is 8.5 GB; 64 GiB is room to spare. */
	if (hdr.frame_size > (16 * 1024 * 1024))
	{
		Console.Error("CSO frame size of %u is implausible.", hdr.frame_size);
		return false;
	}
	if (hdr.align > 16)
	{
		Console.Error("CSO index alignment shift of %u is implausible.", hdr.align);
		return false;
	}
	if (hdr.total_bytes == 0 || hdr.total_bytes > (64ULL * 1024 * 1024 * 1024))
	{
		Console.Error("CSO total size is zero or implausible.");
		return false;
	}

	// All checks passed, this is a good CSO header.
	return true;
}

bool CsoFileReader::Open2(const char* fileName)
{
	Close2();
	strlcpy(m_filename, fileName, sizeof(m_filename));
	m_src = FileSystem::OpenFile(m_filename, "rb");

	bool success = false;
	if (m_src && ReadFileHeader() && InitializeBuffers())
		success = true;

	if (!success)
	{
		Close2();
		return false;
	}
	return true;
}

bool CsoFileReader::ReadFileHeader()
{
	CsoHeader hdr = {};

	if (FileSystem::FSeek64(m_src, m_dataoffset, SEEK_SET) != 0 || rfread(&hdr, 1, sizeof(hdr), m_src) != sizeof(hdr))
	{
		Console.Error("Failed to read CSO file header.");
		return false;
	}

	if (!ValidateHeader(hdr))
	{
		Console.Error("CSO has invalid header.");
		return false;
	}

	m_frameSize = hdr.frame_size;
	// Determine the translation from bytes to frame.
	m_frameShift = 0;
	for (u32 i = m_frameSize; i > 1; i >>= 1)
		++m_frameShift;

	// This is the index alignment (index values need shifting by this amount.)
	m_indexShift = hdr.align;
	m_totalSize  = hdr.total_bytes;

	// Check compression method (ZSO=lz4)
	m_uselz4 = hdr.magic[0] == 'Z';

	return true;
}

bool CsoFileReader::InitializeBuffers()
{
	// Round up, since part of a frame requires a full frame.
	const u64 frames64 = (m_totalSize + m_frameSize - 1) / m_frameSize;
	/* Bounded by the header checks above, but the cast is only safe
	 * because of them - keep the two together. */
	if (frames64 >= 0xFFFFFFFFULL)
	{
		Console.Error("CSO frame count is implausible.");
		return false;
	}
	u32 numFrames = (u32)frames64;

	// We might read a bit of alignment too, so be prepared.
	if (m_frameSize + (1 << m_indexShift) < CSO_READ_BUFFER_SIZE)
	{
		m_readBuffer = new u8[CSO_READ_BUFFER_SIZE];
	}
	else
	{
		m_readBuffer = new u8[m_frameSize + (1 << m_indexShift)];
	}

	const u32 indexSize = numFrames + 1;
	m_index = new u32[indexSize];
	if (rfread(m_index, sizeof(u32), indexSize, m_src) != indexSize)
	{
		Console.Error("Unable to read index data from CSO.");
		return false;
	}

	// initialize zlib if not a ZSO
	if (!m_uselz4)
	{
		m_inflate = rinflate_new(-15); /* CSO frames are raw deflate */
		if (!m_inflate)
		{
			Console.Error("Unable to initialize inflate for CSO decompression.");
			return false;
		}
	}

	return true;
}

void CsoFileReader::Close2()
{
	m_filename[0] = '\0';

	if (m_src)
	{
		rfclose(m_src);
		m_src = NULL;
	}
	if (m_inflate)
	{
		rinflate_free(m_inflate);
		m_inflate = NULL;
	}

	if (m_readBuffer)
	{
		delete[] m_readBuffer;
		m_readBuffer = NULL;
	}
	if (m_index)
	{
		delete[] m_index;
		m_index = NULL;
	}
}

ThreadedFileReader::Chunk CsoFileReader::ChunkForOffset(u64 offset)
{
	Chunk chunk = {0};
	if (offset >= m_totalSize)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = offset >> m_frameShift;
		chunk.length = m_frameSize;
		chunk.offset = chunk.chunkID << m_frameShift;
	}
	return chunk;
}

int CsoFileReader::ReadChunk(void *dst, s64 chunkID)
{
	if (chunkID < 0)
		return -1;

	const u32 frame = chunkID;

	// Grab the index data for the frame we're about to read.
	const bool compressed = (m_index[frame + 0] & 0x80000000) == 0;
	const u32 index0 = m_index[frame + 0] & 0x7FFFFFFF;
	const u32 index1 = m_index[frame + 1] & 0x7FFFFFFF;

	// Calculate where the compressed payload is (if compressed.)
	const u64 frameRawPos = (u64)index0 << m_indexShift;
	const u64 frameRawSize = (u64)(index1 - index0) << m_indexShift;

	if (!compressed)
	{
		// Just read directly, easy.
		if (FileSystem::FSeek64(m_src, frameRawPos, SEEK_SET) != 0)
		{
			Console.Error("Unable to seek to uncompressed CSO data.");
			return 0;
		}
		return rfread(dst, 1, m_frameSize, m_src);
	}
	else
	{
		if (FileSystem::FSeek64(m_src, frameRawPos, SEEK_SET) != 0)
		{
			Console.Error("Unable to seek to compressed CSO data.");
			return 0;
		}
		// This might be less bytes than frameRawSize in case of padding on the last frame.
		// This is because the index positions must be aligned.
		const u32 readRawBytes = rfread(m_readBuffer, 1, frameRawSize, m_src);
		bool success = false;

		if (m_uselz4)
		{
			const int64_t res     = rlz4_decode(static_cast<uint8_t*>(dst), m_frameSize,
					m_readBuffer, readRawBytes);
			success               = (res > 0);
		}
		else
		{
			size_t rd = 0, wr = 0;
			rinflate_set_in(m_inflate, m_readBuffer, readRawBytes);
			rinflate_set_out(m_inflate, static_cast<uint8_t*>(dst), m_frameSize);
			const int status      = rinflate_process(m_inflate, &rd, &wr);
			success               = status == RDEFLATE_PROCESS_END && wr == m_frameSize;
		}

		if (!success)
			Console.Error("Unable to decompress CSO frame using zlib.");

		if (!m_uselz4)
			rinflate_reset(m_inflate, -15);

		return success ? m_frameSize : 0;
	}
}
