/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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
#include <memory>
#include <optional>
#include <string>
#include "zip.h"

static inline std::unique_ptr<zip_t, void (*)(zip_t*)> zip_open_buffer_managed(const void* buffer, size_t size, int flags, int freep, zip_error_t* ze)
{
	zip_source_t* zs = zip_source_buffer_create(buffer, size, freep, ze);
	zip_t* zip = nullptr;
	// have to clean up source
	if (zs && !(zip = zip_open_from_source(zs, flags, ze)))
		zip_source_free(zs);

	return std::unique_ptr<zip_t, void (*)(zip_t*)>(zip, [](zip_t* zf) {
		if (!zf)
			return;

		int err = zip_close(zf);
		if (err != 0)
			zip_discard(zf);
	});
}

static inline std::optional<std::string> ReadFileInZipToString(zip_t* file, const char* name)
{
	std::optional<std::string> ret;
	const zip_int64_t file_index = zip_name_locate(file, name, ZIP_FL_NOCASE);
	if (file_index >= 0)
	{
		zip_stat_t zst;
		if (zip_stat_index(file, file_index, ZIP_FL_NOCASE, &zst) == 0)
		{
			zip_file_t* zf = zip_fopen_index(file, file_index, ZIP_FL_NOCASE);
			if (zf)
			{
				ret = std::string();
				ret->resize(static_cast<size_t>(zst.size));
				if (zip_fread(zf, ret->data(), ret->size()) != static_cast<zip_int64_t>(ret->size()))
					ret.reset();
			}
		}
	}

	return ret;
}

static inline std::optional<std::string> ReadFileInZipToString(zip_file_t* file, u32 chunk_size = 4096)
{
	std::optional<std::string> ret = std::string();
	for (;;)
	{
		const size_t pos = ret->size();
		ret->resize(pos + chunk_size);
		const s64 read = zip_fread(file, ret->data() + pos, chunk_size);
		if (read < 0)
		{
			// read error
			ret.reset();
			break;
		}

		// if less than chunk size, we're EOF
		if (read != static_cast<s64>(chunk_size))
		{
			ret->resize(pos + static_cast<size_t>(read));
			break;
		}
	}

	return ret;
}
