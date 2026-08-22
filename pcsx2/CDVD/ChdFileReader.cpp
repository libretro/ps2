/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2024 PCSX2 Dev Team
 *
 *  SPDX-License-Identifier: LGPL-3.0+
 */

#include <compat/strl.h>
#include "ChdFileReader.h"

#include "../../common/Console.h"
#include "../../common/FileSystem.h"
#include "../../common/Path.h"
#include "../../common/StringUtil.h"

#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <formats/rchd.h>

#include <cstring>

/* Scratch for unmapped feeds.  One hunk-sized request is the largest
 * thing rchd ever asks for, but requests are re-issued on short supply,
 * so a fixed chunk is fine and keeps this off the stack. */
static constexpr size_t FEED_CHUNK = 64 * 1024;

/* Fetches allowed in flight per read.  Only raised above the default
   for unmapped images, where batching real I/O ahead of the decode is
   the point; a mapped image's feeds are memcpys and staging them would
   only spend memory (each extra slot costs a hunk of staging). */
static constexpr uint32_t CHD_PIPELINE_DEPTH = 4;

ChdFileReader::ChdFileReader() = default;

ChdFileReader::~ChdFileReader()
{
	/* Close() (and thus Close2()) runs from the base class path, but a
	   reader destroyed without Close() must still release the chain. */
	Close2();
}


/* Feed one outstanding request cycle.  Mapped sources feed straight
 * from the mapping - the decoder consumes bytes in place, so a mapped
 * image never copies compressed data at all.  Unmapped sources go
 * through a bounded scratch buffer; short feeds are legal and rchd
 * simply re-requests the remainder. */
bool ChdFileReader::DriveRead(rchd_t* chd, const Source& self, const Source& parent)
{
	rchd_request_t reqs[CHD_PIPELINE_DEPTH];
	rchd_request_t req;
	uint8_t scratch[FEED_CHUNK];

	for (;;)
	{
		/* Gather every fetch the read can use right now and satisfy
		   them all before decoding.  With a mapped image each feed is
		   an in-place hand-over from the page cache; unmapped images
		   batch their filestream reads ahead of the decode they
		   overlap with.  Depth one degenerates to the old ping-pong. */
		const uint32_t n = rchd_read_pending(chd, reqs, CHD_PIPELINE_DEPTH);
		for (uint32_t i = 0; i < n; i++)
		{
			const Source& src = (reqs[i].source == RCHD_SOURCE_PARENT) ? parent : self;
			if (src.base)
			{
				if (reqs[i].offset >= static_cast<uint64_t>(src.len))
					return false;
				const uint64_t avail = static_cast<uint64_t>(src.len) - reqs[i].offset;
				const size_t   len   = static_cast<size_t>(pcsx2_min_i(reqs[i].length, avail));
				if (rchd_feed_at(chd, reqs[i].offset, reqs[i].source, src.base + reqs[i].offset, len) < 0)
					return false;
			}
			else
			{
				if (!src.fp)
					return false;
				if (filestream_seek(src.fp, static_cast<int64_t>(reqs[i].offset), RETRO_VFS_SEEK_POSITION_START) != 0)
					return false;
				const size_t  want = pcsx2_min_sz(reqs[i].length, FEED_CHUNK);
				const int64_t got  = filestream_read(src.fp, scratch, static_cast<int64_t>(want));
				if (got <= 0)
					return false;
				if (rchd_feed_at(chd, reqs[i].offset, reqs[i].source, scratch, static_cast<size_t>(got)) < 0)
					return false;
			}
		}

		const int err = rchd_read_step(chd, &req);
		if (err == RCHD_OK)
			return true;
		if (err != RCHD_PENDING)
		{
			Console.Error("CDVD: rchd read error %d", err);
			return false;
		}
	}
}

bool ChdFileReader::OpenOne(const char* path, rchd_t** out_chd, Source* out_src)
{
	Source src;
	src.fp = filestream_open(path,
			RETRO_VFS_FILE_ACCESS_READ,
			RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
	if (!src.fp)
		return false;
	src.base = filestream_get_mapped_ptr(src.fp, &src.len);
	if (src.base && src.len <= 0)
		src.base = nullptr;

	rchd_t* chd = rchd_new();
	if (!chd)
	{
		filestream_close(src.fp);
		return false;
	}

	/* The open is the same pull loop as a read, without a parent: a
	   header, map, or metadata byte can only live in the file itself. */
	rchd_request_t req;
	uint8_t scratch[FEED_CHUNK];
	for (;;)
	{
		const int err = rchd_open_step(chd, &req);
		if (err == RCHD_OK)
			break;
		if (err != RCHD_PENDING)
		{
			rchd_free(chd);
			filestream_close(src.fp);
			return false;
		}
		if (src.base)
		{
			if (req.offset >= static_cast<uint64_t>(src.len))
			{
				rchd_free(chd);
				filestream_close(src.fp);
				return false;
			}
			const uint64_t avail = static_cast<uint64_t>(src.len) - req.offset;
			rchd_feed(chd, src.base + req.offset,
					static_cast<size_t>(pcsx2_min_i(req.length, avail)));
		}
		else
		{
			if (filestream_seek(src.fp, static_cast<int64_t>(req.offset), RETRO_VFS_SEEK_POSITION_START) != 0)
			{
				rchd_free(chd);
				filestream_close(src.fp);
				return false;
			}
			const size_t  want = pcsx2_min_sz(req.length, FEED_CHUNK);
			const int64_t got  = filestream_read(src.fp, scratch, static_cast<int64_t>(want));
			if (got <= 0)
			{
				rchd_free(chd);
				filestream_close(src.fp);
				return false;
			}
			rchd_feed(chd, scratch, static_cast<size_t>(got));
		}
	}

	*out_chd = chd;
	*out_src = src;
	return true;
}

bool ChdFileReader::Open2(const char* fileName)
{
	Close2();
	strlcpy(m_filename, fileName, sizeof(m_filename));

	rchd_t* chd = nullptr;
	Source src;
	if (!OpenOne(m_filename, &chd, &src))
	{
		Console.Error("CDVD: failed to open CHD: %s", m_filename);
		return false;
	}
	m_chds.push_back(chd);
	m_srcs.push_back(src);

	/* Resolve the parent chain by SHA-1, the same directory scan the
	   libchdr path did - but the match test is the decoder's own
	   (rchd_parent_sha1_matches), and a wrong parent is rejected here
	   instead of decoding to plausible garbage. */
	std::string dirname;
	FileSystem::FindResultsArray results;
	while (rchd_info(m_chds.back())->has_parent)
	{
		if (m_chds.size() >= 8)
		{
			Console.Error("CDVD: CHD parent chain hit recursion limit");
			Close2();
			return false;
		}

		bool found = false;
		dirname = Path::GetDirectory(m_filename);
		if (FileSystem::FindFiles(dirname.c_str(), "*.*",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &results))
		{
			for (const FILESYSTEM_FIND_DATA& fd : results)
			{
				const char* extension = path_get_extension(fd.FileName.c_str());
				if (string_is_empty(extension) || Strncasecmp(extension, "chd", 3) != 0)
					continue;

				rchd_t* cand = nullptr;
				Source cand_src;
				if (!OpenOne(fd.FileName.c_str(), &cand, &cand_src))
					continue;

				if (rchd_parent_sha1_matches(m_chds.back(), rchd_info(cand)->sha1))
				{
					rchd_set_parent(m_chds.back(), cand);
					m_chds.push_back(cand);
					m_srcs.push_back(cand_src);
					found = true;
					break;
				}
				rchd_free(cand);
				filestream_close(cand_src.fp);
			}
		}

		if (!found)
		{
			Console.Error("CDVD: no parent found for: %s", m_filename);
			Close2();
			return false;
		}
	}

	if (!m_srcs[0].base)
		rchd_set_pipeline_depth(m_chds[0], CHD_PIPELINE_DEPTH);

	const rchd_info_t* info = rchd_info(m_chds[0]);
	hunk_size = info->hunk_bytes;
	/* CHD uses full 2448-byte units but PCSX2 passes 2448-byte buffers
	   that can't fit a whole padded hunk; trim via internal block size,
	   exactly as before. */
	m_internalBlockSize = info->unit_bytes;

	/* Track-accurate frame count.  rchd reconstructs the TOC itself -
	   pregap/postgap/padding resolved, DVDs synthesised as one run -
	   which replaces the hand-rolled metadata sscanf ParseTOC and its
	   track-1-only limitation. */
	const u64 total_frames = rchd_total_frames(m_chds[0]);
	if (total_frames > 0)
		file_size = total_frames * static_cast<u64>(info->unit_bytes);
	else
		file_size = info->logical_bytes;

	return true;
}

ThreadedFileReader::Chunk ChdFileReader::ChunkForOffset(u64 offset)
{
	Chunk chunk = {0};
	if (offset >= file_size)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = offset / hunk_size;
		chunk.length = hunk_size;
		chunk.offset = chunk.chunkID * hunk_size;
	}
	return chunk;
}

int ChdFileReader::ReadChunk(void* dst, s64 chunkID)
{
	if (chunkID < 0 || m_chds.empty())
		return -1;

	if (rchd_read_hunk_begin(m_chds[0], static_cast<uint32_t>(chunkID), dst) != RCHD_OK)
		return 0;

	static const Source no_src;
	const Source& parent = (m_srcs.size() > 1) ? m_srcs[1] : no_src;
	if (!DriveRead(m_chds[0], m_srcs[0], parent))
		return 0;

	return static_cast<int>(hunk_size);
}

void ChdFileReader::Close2()
{
	/* Children reference parents; free outward-in. */
	for (size_t i = 0; i < m_chds.size(); i++)
		rchd_free(m_chds[i]);
	m_chds.clear();
	for (Source& s : m_srcs)
	{
		if (s.fp)
			filestream_close(s.fp);
	}
	m_srcs.clear();
	file_size = 0;
	hunk_size = 0;
}

u32 ChdFileReader::GetBlockCount() const
{
	return (file_size - m_dataoffset) / m_internalBlockSize;
}
