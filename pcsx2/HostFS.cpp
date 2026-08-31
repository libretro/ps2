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

#include "HostFS.h"

#include <cstring>
#include <utility>

#include <compat/fnmatch.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>


template <typename T>
static inline void PathAppendString(std::string& dst, const T& src)
{
	if (dst.capacity() < (dst.length() + src.length()))
		dst.reserve(dst.length() + src.length());

	bool last_separator = (!dst.empty() && dst.back() == FS_OSPATH_SEPARATOR_CHARACTER);

	size_t index = 0;

#ifdef _WIN32
	/* special case for UNC paths here */
	if (dst.empty() && src.length() >= 3 && src[0] == '\\' && src[1] == '\\' && src[2] != '\\')
	{
		dst.append("\\\\");
		index = 2;
	}
#endif

	for (; index < src.length(); index++)
	{
		const char ch = src[index];

#ifdef _WIN32
		/* convert forward slashes to backslashes */
		if (ch == '\\' || ch == '/')
#else
		if (ch == '/')
#endif
		{
			if (last_separator)
				continue;
			last_separator = true;
			dst.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
		}
		else
		{
			last_separator = false;
			dst.push_back(ch);
		}
	}
}

/* One directory walk for every platform.  retro_dirent answers "is this a
 * directory" out of the directory entry itself wherever the filesystem
 * fills d_type (ext4, APFS) and out of WIN32_FIND_DATA on Windows, so
 * classification costs no syscall, and a size is fetched only for the
 * entries that survive the filters rather than for everything the walk
 * passes over. */
static u32 RecursiveFindFiles(const std::string& dir_path, const std::string& rel_prefix,
	const char* pattern, u32 flags, FileSystem::FindResultsArray* results)
{
	struct RDIR* dir = retro_opendir_include_hidden(dir_path.c_str(), true);
	if (!dir)
		return 0;
	if (retro_dirent_error(dir))
	{
		retro_closedir(dir);
		return 0;
	}

	/* Small speed optimization for the '*' case. */
	const bool has_wildcards = (std::strpbrk(pattern, "*?") != nullptr);
	const bool match_all     = has_wildcards && !std::strcmp(pattern, "*");
	u32 found                = 0;

	while (retro_readdir(dir))
	{
		const char* name = retro_dirent_get_name(dir);

		if (name[0] == '.')
		{
			if (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))
				continue;
			if (!(flags & FILESYSTEM_FIND_HIDDEN_FILES))
				continue;
		}

		const bool is_dir = retro_dirent_is_dir(dir, nullptr);
		std::string full_path(dir_path);
		full_path += FS_OSPATH_SEPARATOR_CHARACTER;
		full_path += name;

		if (is_dir)
		{
			if (flags & FILESYSTEM_FIND_RECURSIVE)
			{
				std::string child_prefix(rel_prefix);
				child_prefix += name;
				child_prefix += FS_OSPATH_SEPARATOR_CHARACTER;
				found += RecursiveFindFiles(full_path, child_prefix, pattern, flags, results);
			}

			if (!(flags & FILESYSTEM_FIND_FOLDERS))
				continue;
		}
		else if (!(flags & FILESYSTEM_FIND_FILES))
			continue;

		if (has_wildcards)
		{
			if (!match_all && rl_fnmatch(pattern, name, 0) != 0)
				continue;
		}
		else if (std::strcmp(name, pattern) != 0)
			continue;

		/* Only an entry that survived the filters is sized, and an
		 * entry that cannot be sized is not an entry - a broken
		 * symlink has nothing behind it to report. */
		const s64 size = (s64)path_get_size(full_path.c_str());
		if (size < 0)
			continue;

		FILESYSTEM_FIND_DATA out_data;
		out_data.Attributes = is_dir ? FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY : 0;
		out_data.Size       = size;
		if (flags & FILESYSTEM_FIND_RELATIVE_PATHS)
		{
			out_data.FileName = rel_prefix;
			out_data.FileName += name;
		}
		else
			out_data.FileName = std::move(full_path);

		results->push_back(std::move(out_data));
		found++;
	}

	retro_closedir(dir);
	return found;
}

bool FileSystem::FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results)
{
	if (path[0] == '\0')
		return false;
	if (!(flags & FILESYSTEM_FIND_KEEP_ARRAY))
		results->clear();
	return RecursiveFindFiles(path, std::string(), pattern, flags, results) > 0;
}
