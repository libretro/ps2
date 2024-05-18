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

#pragma once

typedef struct zstate Zstate;

#include "ThreadedFileReader.h"
#include "zlib_indexed.h"

class GzippedFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(GzippedFileReader);

public:
	GzippedFileReader();
	~GzippedFileReader();

	bool Open2(std::string filename) override;

	Chunk ChunkForOffset(u64 offset) override;
	int ReadChunk(void* dst, s64 chunkID) override;

	void Close2() override;

	u32 GetBlockCount() const override;

private:
	static constexpr int GZFILE_SPAN_DEFAULT = (1048576 * 4); /* distance between direct access points when creating a new index */
	static constexpr int GZFILE_READ_CHUNK_SIZE = (256 * 1024); /* zlib extraction chunks size (at 0-based boundaries) */
	static constexpr int GZFILE_CACHE_SIZE_MB = 200; /* cache size for extracted data. must be at least GZFILE_READ_CHUNK_SIZE (in MB)*/

	// Verifies that we have an index, or try to create one
	bool LoadOrCreateIndex();

	Access* m_index = nullptr; // Quick access index

	RFILE* m_src = nullptr;

	zstate m_z_state = {};
};
