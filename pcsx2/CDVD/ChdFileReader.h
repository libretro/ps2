/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2024 PCSX2 Dev Team
 *
 *  SPDX-License-Identifier: LGPL-3.0+
 */

#pragma once
#include "ThreadedFileReader.h"

#include <string>
#include <vector>

struct rchd;
typedef struct rchd rchd_t;
typedef struct RFILE RFILE;

/// CHD reader on libretro-common's clean-room rchd decoder.  rchd does
/// no I/O of its own: every byte it needs is requested from this class,
/// which serves it from a memory-mapped image when the VFS provides one
/// (zero-copy feed straight from the page cache) or from filestream
/// reads when it does not.  Parent images difference-chain the same way.
class ChdFileReader final : public ThreadedFileReader
{
public:
	ChdFileReader();
	~ChdFileReader() override;

	static bool CanHandle(const std::string& fileName, const std::string& displayName);

	u32 GetBlockCount() const override;

protected:
	bool Open2(const char* fileName) override;
	Chunk ChunkForOffset(u64 offset) override;
	int ReadChunk(void* dst, s64 chunkID) override;
	void Close2() override;

private:
	struct Source
	{
		RFILE*         fp   = nullptr;
		const uint8_t* base = nullptr; /* non-NULL when mapped */
		int64_t        len  = 0;
	};

	bool OpenOne(const std::string& path, rchd_t** out_chd, Source* out_src);
	bool DriveRead(rchd_t* chd, const Source& self, const Source& parent);

	/// index 0 = the image itself, 1.. = parent chain outward
	std::vector<rchd_t*> m_chds;
	std::vector<Source>  m_srcs;

	u64 file_size = 0;
	u32 hunk_size = 0;
};
