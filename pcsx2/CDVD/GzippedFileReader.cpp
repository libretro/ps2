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
#include "../../common/Console.h"
#include "HostFS.h"
#include "../../common/Path.h"
#include "../../common/StringUtil.h"

#include <compat/strl.h>
#include <file/file_path.h>

#include "GzippedFileReader.h"
#include "zlib_indexed.h"
#include "../Config.h"
#include "../Host.h"

#define CLAMP(val, minval, maxval) (pcsx2_min_i(maxval, pcsx2_max_i(minval, val)))

#define GZIP_ID "PCSX2.index.gzip.v1|"
#define GZIP_ID_LEN (sizeof(GZIP_ID) - 1) /* sizeof includes the \0 terminator */

// File format is:
// - [GZIP_ID_LEN] GZIP_ID (no \0)
// - [sizeof(Access)] index (should be allocated, contains various sizes)
// - [rest] the indexed data points (should be allocated, index->list should then point to it)
static Access* ReadIndexFromFile(const char* filename)
{
	s64 size;
	RFILE *fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return nullptr;

	if ((size = filestream_get_size(fp)) <= 0)
	{
		filestream_close(fp);
		return nullptr;
	}

	/* Everything below is derived from the file's own size, so the file
	 * has to be big enough to hold what it is describing.  A shorter
	 * one made datasize negative, and index->have is a signed int read
	 * straight out of that file: a matching negative value satisfied
	 * the equality check, malloc() was then called with a negative
	 * size that converts to an enormous size_t, and the read went
	 * ahead through the null it returned. */
	if (size < static_cast<s64>(GZIP_ID_LEN + sizeof(Access)))
	{
		filestream_close(fp);
		return nullptr;
	}

	char fileId[GZIP_ID_LEN + 1] = {0};
	if (filestream_read(fp, fileId, GZIP_ID_LEN) != (int64_t)GZIP_ID_LEN || std::memcmp(fileId, GZIP_ID, 4) != 0)
	{
		filestream_close(fp);
		return nullptr;
	}

	Access* const index = static_cast<Access*>(std::malloc(sizeof(Access)));
	if (!index)
	{
		filestream_close(fp);
		return nullptr;
	}

	const s64 datasize = size - GZIP_ID_LEN - sizeof(Access);
	if (filestream_read(fp, index, sizeof(Access)) != (int64_t)sizeof(Access) ||
		index->have <= 0 ||
		datasize != static_cast<s64>(index->have) * static_cast<s64>(sizeof(Point)))
	{
		filestream_close(fp);
		std::free(index);
		return 0;
	}

	char* buffer = static_cast<char*>(std::malloc(datasize));
	if (!buffer || filestream_read(fp, buffer, datasize) != (int64_t)datasize)
	{
		filestream_close(fp);
		std::free(buffer);
		std::free(index);
		return 0;
	}

	index->list = reinterpret_cast<Point*>(buffer); // adjust list pointer
	filestream_close(fp);
	return index;
}

static void WriteIndexToFile(Access* index, const char* filename)
{
	RFILE *fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return;

	bool success = (filestream_write(fp, GZIP_ID, GZIP_ID_LEN) == (int64_t)GZIP_ID_LEN);

	Point* tmp = index->list;
	index->list = 0; // current pointer is useless on disk, normalize it as 0.
	filestream_write(fp, (char*)index, sizeof(Access));
	index->list = tmp;

	success = success && (filestream_write(fp, (char*)index->list, sizeof(Point) * index->have) == (int64_t)(sizeof(Point) * index->have));
	filestream_close(fp);
}

static const char* INDEX_TEMPLATE_KEY = "$(f)";

// template:
// must contain one and only one instance of '$(f)' (without the quotes)
// if if !canEndWithKey -> must not end with $(f)
// if starts with $(f) then it expands to the full path + file name.
// if doesn't start with $(f) then it's expanded to file name only (with extension)
// if doesn't start with $(f) and ends up relative,
//   then it's relative to base (not to cwd)
// No checks are performed if the result file name can be created.
// If this proves useful, we can move it into Path:: . Right now there's no need.
/* Expand the index-file template into a caller buffer.
 *
 * The template is a user setting containing exactly one $(f), replaced by
 * the image's filename -- or its leaf name when the key is not at the
 * start, since a template with a directory prefix wants the bare name.
 * A relative result is joined under base. An empty output means the
 * template was malformed: no key, more than one, or ending with the key
 * when that is not allowed.
 *
 * Was three std::strings and a find/rfind pair; the same rules on a
 * fixed buffer, since the result is a path and was only ever read back
 * with c_str(). */
static void ApplyTemplate(char* out, size_t out_size,
	const char* name, const char* base,
	const char* fileTemplate, const char* filename,
	bool canEndWithKey)
{
	const size_t keylen = strlen(INDEX_TEMPLATE_KEY);
	char   tmpl[PCSX2_PATH_MAX];
	char*  key;
	char*  start;
	char*  end;
	size_t len;

	out[0] = '\0';
	(void)name;

	/* StripWhitespace, in place. */
	strlcpy(tmpl, fileTemplate ? fileTemplate : "", sizeof(tmpl));
	start = tmpl;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		start++;
	end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'
				|| end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
	len = (size_t)(end - start);

	key = strstr(start, INDEX_TEMPLATE_KEY);
	if (!key)
		return;                                   /* not found */
	if (strstr(key + 1, INDEX_TEMPLATE_KEY))
		return;                                   /* more than one */
	if (!canEndWithKey && (size_t)(key - start) == len - keylen)
		return;                                   /* ends with the key */

	{
		const char* fname = filename;
		char        joined[PCSX2_PATH_MAX];

		/* A key past the start means the template carries its own
		 * directory, so only the leaf name goes in. */
		if (key != start)
		{
			const char* slash = strrchr(filename, '/');
#ifdef _WIN32
			const char* bslash = strrchr(filename, '\\');
			if (bslash > slash)
				slash = bslash;
#endif
			if (slash)
				fname = slash + 1;
		}

		/* Substitute: prefix, filename, suffix. */
		*key = '\0';
		snprintf(joined, sizeof(joined), "%s%s%s", start, fname, key + keylen);

		if (path_is_absolute(joined))
			strlcpy(out, joined, out_size);
		else
			pcsx2_path_join(out, out_size, base, joined);
	}
}

static void iso2indexname(char* out, size_t out_size, const char* isoname)
{
	ApplyTemplate(out, out_size, "gzip index", EmuFolders::DataRoot,
			Host::GetBaseStringSettingValue("EmuCore", "GzipIsoIndexTemplate",
				"$(f).pindex.tmp").c_str(),
			isoname, false);
}

/* Ops thunks: the base calls these with a ThreadedFileReader*, which is
 * always a GzippedFileReader here. */
static ThreadedFileReader::Chunk GzippedFileReader_chunk_for_offset(ThreadedFileReader* self, u64 offset)
{ return static_cast<GzippedFileReader*>(self)->ChunkForOffset(offset); }
static int GzippedFileReader_read_chunk(ThreadedFileReader* self, void* dst, s64 chunkID)
{ return static_cast<GzippedFileReader*>(self)->ReadChunk(dst, chunkID); }
static bool GzippedFileReader_open2(ThreadedFileReader* self, const char* filename)
{ return static_cast<GzippedFileReader*>(self)->Open2(filename); }
static void GzippedFileReader_close2(ThreadedFileReader* self)
{ static_cast<GzippedFileReader*>(self)->Close2(); }
static u32 GzippedFileReader_block_count(const ThreadedFileReader* self)
{ return static_cast<const GzippedFileReader*>(self)->GetBlockCount(); }

const ThreadedFileReader::Ops GzippedFileReader::s_ops =
{
	GzippedFileReader_chunk_for_offset, GzippedFileReader_read_chunk, GzippedFileReader_open2, GzippedFileReader_close2, GzippedFileReader_block_count
};

GzippedFileReader::GzippedFileReader()
{
	m_ops = &s_ops;
}

GzippedFileReader::~GzippedFileReader() = default;

bool GzippedFileReader::LoadOrCreateIndex()
{
	// Try to read index from disk
	char indexfile[PCSX2_PATH_MAX];

	iso2indexname(indexfile, sizeof(indexfile), m_filename);
	// iso2indexname(...) will set errors if it can't apply the template
	if (indexfile[0] == '\0')
		return false;

	if ((m_index = ReadIndexFromFile(indexfile)) != nullptr)
		return true;

	// No valid index file. Generate an index
	Console.Warning("This may take a while (but only once). Scanning compressed file to generate a quick access index...");

	const s64 prevoffset = filestream_tell(m_src);
	Access* index = nullptr;
	int len = build_index(m_src, GZFILE_SPAN_DEFAULT, &index);
	printf("\n"); // build_index prints progress without \n's
	filestream_seek(m_src, prevoffset, RETRO_VFS_SEEK_POSITION_START);

	/* build_index returns the number of access points, and zero is not
	 * success: it takes that path when no point was ever added, and on
	 * that path it never writes through `built' at all.  index stays
	 * null, and >= 0 sent it to WriteIndexToFile, which dereferences it
	 * immediately - index->list, on a null pointer, from the load
	 * thread. */
	if (len > 0 && index != nullptr)
	{
		m_index = index;
		WriteIndexToFile(m_index, indexfile);
	}
	else
	{
		free_index(index);
		return false;
	}

	return true;
}

bool GzippedFileReader::Open2(const char* filename)
{
	Close();

	strlcpy(m_filename, filename, sizeof(m_filename));
	if (!(m_src = filestream_open(m_filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE)) || !LoadOrCreateIndex())
	{
		Close();
		return false;
	}

	return true;
}

void GzippedFileReader::Close2()
{
	if (m_z_state.isValid)
	{
		zstate_free_strm(&m_z_state);
		m_z_state = {};
	}

	if (m_src)
	{
		filestream_close(m_src);
		m_src = nullptr;
	}

	if (m_index)
	{
		free_index(m_index);
		m_index = nullptr;
	}
}

ThreadedFileReader::Chunk GzippedFileReader::ChunkForOffset(u64 offset)
{
	ThreadedFileReader::Chunk chunk = {};
	if (static_cast<s64>(offset) >= m_index->uncompressed_size)
		chunk.chunkID = -1;
	else
	{
		chunk.chunkID = static_cast<s64>(offset) / m_index->span;
		chunk.offset  = static_cast<u64>(chunk.chunkID) * m_index->span;
		/* Measured from the start of the chunk, not from the offset that
		 * happened to be asked for.  ReadChunk always decompresses the
		 * whole chunk starting at chunkID * span, so a request landing
		 * partway into one described a buffer shorter than what would be
		 * written into it - by exactly how far into the chunk the caller
		 * had asked. */
		chunk.length  = static_cast<u32>(pcsx2_min_u64(m_index->uncompressed_size - chunk.offset, m_index->span));
	}

	return chunk;
}

int GzippedFileReader::ReadChunk(void* dst, s64 chunkID)
{
	if (chunkID < 0)
		return -1;

	const s64 file_offset = chunkID * m_index->span;
	const u32 read_len = static_cast<u32>(pcsx2_min_s64(m_index->uncompressed_size - file_offset, m_index->span));
	return extract(m_src, m_index, file_offset, static_cast<unsigned char*>(dst), read_len, &m_z_state);
}

u32 GzippedFileReader::GetBlockCount() const
{
	return (m_index->uncompressed_size + (m_blocksize - 1)) / m_blocksize;
}
