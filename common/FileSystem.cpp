/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include "FileSystem.h"
#include "Path.h"
#include "Console.h"
#include "StringUtil.h"
#include "Path.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <limits>
#include <utility>

#include <file/file_path.h>
#include <retro_dirent.h>
#include <encodings/utf.h>
#include <string/stdstring.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/param.h>
#endif

#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#if defined(_WIN32)
#include "RedtapeWindows.h"
#include <winioctl.h>
#include <share.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <streams/file_stream.h>

#ifdef __cplusplus
extern "C" {
#endif

int rferror(RFILE* stream)
{
   return filestream_error(stream);
}

int rfgetc(RFILE* stream)
{
   if (!stream)
      return EOF;

   return filestream_getc(stream);
}

RFILE* rfopen(const char *path, const char *mode)
{
   RFILE          *output  = NULL;
   unsigned int retro_mode = RETRO_VFS_FILE_ACCESS_READ;
   bool position_to_end    = false;

   if (strstr(mode, "r"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_READ;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }
   else if (strstr(mode, "w"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE;
      if (strstr(mode, "+"))
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
   }
   else if (strstr(mode, "a"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE |
         RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      position_to_end = true;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }

   output = filestream_open(path, retro_mode,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (output && position_to_end)
      filestream_seek(output, 0, RETRO_VFS_SEEK_POSITION_END);

   return output;
}

int rfeof(RFILE* stream)
{
   return filestream_eof(stream);
}

char *rfgets(char *buffer, int maxCount, RFILE* stream)
{
   if (!stream)
      return NULL;

   return filestream_gets(stream, buffer, maxCount);
}

int rfclose(RFILE* stream)
{
   if (!stream)
      return EOF;

   return filestream_close(stream);
}

int64_t rftell(RFILE* stream)
{
   if (!stream)
      return -1;

   return filestream_tell(stream);
}

int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream)
{
   if (!stream || (elem_size == 0) || (elem_count == 0))
      return 0;

   return (filestream_read(stream, buffer, elem_size * elem_count) / elem_size);
}

int64_t rfseek(RFILE* stream, int64_t offset, int origin)
{
   int seek_position = -1;

   if (!stream)
      return -1;

   switch (origin)
   {
      case SEEK_SET:
         seek_position = RETRO_VFS_SEEK_POSITION_START;
         break;
      case SEEK_CUR:
         seek_position = RETRO_VFS_SEEK_POSITION_CURRENT;
         break;
      case SEEK_END:
         seek_position = RETRO_VFS_SEEK_POSITION_END;
         break;
   }

   return filestream_seek(stream, offset, seek_position);
}

int64_t rfwrite(void const* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream)
{
   if (!stream || (elem_size == 0) || (elem_count == 0))
      return 0;

   return (filestream_write(stream, buffer, elem_size * elem_count) / elem_size);
}

#ifdef __cplusplus
}
#endif

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

bool Path::IsAbsolute(const std::string_view& path)
{
#ifdef _WIN32
	return (path.length() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
			   path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
		   (path.length() >= 3 && path[0] == '\\' && path[1] == '\\');
#else
	return (path.length() >= 1 && path[0] == '/');
#endif
}

std::string Path::ToNativePath(const std::string_view& path)
{
	std::string ret;
	PathAppendString(ret, path);

	/* remove trailing slashes */
	if (ret.length() > 1)
	{
		while (ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
			ret.pop_back();
	}

	return ret;
}

void Path::ToNativePath(std::string* path)
{
	*path = Path::ToNativePath(*path);
}

/* Joins a string together using the specified delimiter. */
template <typename T>
static inline std::string JoinString(const T& start, const T& end, char delimiter)
{
	std::string ret;
	for (auto it = start; it != end; ++it)
	{
		if (it != start)
			ret += delimiter;
		ret.append(*it);
	}
	return ret;
}

std::string Path::Canonicalize(const std::string_view& path)
{
	std::vector<std::string_view> components = Path::SplitNativePath(path);
	std::vector<std::string_view> new_components;
	new_components.reserve(components.size());
	for (const std::string_view& component : components)
	{
		if (component == ".")
		{
			/* current directory, so it can be skipped, unless it's the only component */
			if (components.size() == 1)
				new_components.push_back(std::move(component));
		}
		else if (component == "..")
		{
			/* parent directory, pop one off if we're not at the beginning, otherwise preserve. */
			if (!new_components.empty())
				new_components.pop_back();
			else
				new_components.push_back(std::move(component));
		}
		else
		{
			/* anything else, preserve */
			new_components.push_back(std::move(component));
		}
	}

	return JoinString(new_components.begin(), new_components.end(), FS_OSPATH_SEPARATOR_CHARACTER);
}

void Path::Canonicalize(std::string* path)
{
	*path = Canonicalize(*path);
}

std::string Path::ReplaceExtension(const std::string_view& path, const std::string_view& new_extension)
{
	const std::string_view::size_type pos = path.rfind('.');
	if (pos == std::string_view::npos)
		return std::string(path);

	std::string ret(path, 0, pos + 1);
	ret.append(new_extension);
	return ret;
}

static std::string_view::size_type GetLastSeperatorPosition(const std::string_view& filename, bool include_separator)
{
	std::string_view::size_type last_separator = filename.rfind('/');
	if (include_separator && last_separator != std::string_view::npos)
		last_separator++;

#if defined(_WIN32)
	std::string_view::size_type other_last_separator = filename.rfind('\\');
	if (other_last_separator != std::string_view::npos)
	{
		if (include_separator)
			other_last_separator++;
		if (last_separator == std::string_view::npos || other_last_separator > last_separator)
			last_separator = other_last_separator;
	}
#endif

	return last_separator;
}

std::string_view Path::GetDirectory(const std::string_view& path)
{
	const std::string::size_type pos = GetLastSeperatorPosition(path, false);
	if (pos == std::string_view::npos)
		return {};

	return path.substr(0, pos);
}

std::string_view Path::GetFileName(const std::string_view& path)
{
	const std::string_view::size_type pos = GetLastSeperatorPosition(path, true);
	if (pos == std::string_view::npos)
		return path;

	return path.substr(pos);
}

std::vector<std::string_view> Path::SplitWindowsPath(const std::string_view& path)
{
	std::vector<std::string_view> parts;

	std::string::size_type start = 0;
	std::string::size_type pos = 0;

	/* preserve UNC paths */
	if (path.size() > 2 && path[0] == '\\' && path[1] == '\\')
		pos = 2;

	while (pos < path.size())
	{
		if (path[pos] != '/' && path[pos] != '\\')
		{
			pos++;
			continue;
		}

		/* skip consecutive separators */
		if (pos != start)
			parts.push_back(path.substr(start, pos - start));

		pos++;
		start = pos;
	}

	if (start != pos)
		parts.push_back(path.substr(start));

	return parts;
}

std::vector<std::string_view> Path::SplitNativePath(const std::string_view& path)
{
#ifdef _WIN32
	return SplitWindowsPath(path);
#else
	std::vector<std::string_view> parts;

	std::string::size_type start = 0;
	std::string::size_type pos = 0;
	while (pos < path.size())
	{
		if (path[pos] != '/')
		{
			pos++;
			continue;
		}

		/* skip consecutive separators
		 * for unix, we create an empty element at the beginning when it's an absolute path
		 * that way, when it's re-joined later, we preserve the starting slash. */
		if (pos != start || pos == 0)
			parts.push_back(path.substr(start, pos - start));

		pos++;
		start = pos;
	}

	if (start != pos)
		parts.push_back(path.substr(start));

	return parts;
#endif
}

std::string Path::Combine(const std::string_view& base, const std::string_view& next)
{
	std::string ret;
	ret.reserve(base.length() + next.length() + 1);

	PathAppendString(ret, base);
	while (!ret.empty() && ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
		ret.pop_back();

	ret += FS_OSPATH_SEPARATOR_CHARACTER;
	PathAppendString(ret, next);
	while (!ret.empty() && ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
		ret.pop_back();

	return ret;
}

int FileSystem::OpenFDFile(const char* filename, int flags, int mode)
{
#ifdef _WIN32
	if (string_is_empty(filename))
		return -1;
	wchar_t *wfilename = utf8_to_utf16_string_alloc(filename);
	int ret = _wopen(wfilename, flags, mode);
	free(wfilename);
	return ret;
#else
	return open(filename, flags, mode);
#endif
}

RFILE* FileSystem::OpenFile(const char *filename, const char *mode)
{
   RFILE          *output  = NULL;
   unsigned int retro_mode = RETRO_VFS_FILE_ACCESS_READ;
   bool position_to_end    = false;

   if (strstr(mode, "r"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_READ;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }
   else if (strstr(mode, "w"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE;
      if (strstr(mode, "+"))
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
   }
   else if (strstr(mode, "a"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE |
         RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      position_to_end = true;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }

   output = filestream_open(filename, retro_mode,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (output && position_to_end)
      filestream_seek(output, 0, RETRO_VFS_SEEK_POSITION_END);

   return output;
}

int FileSystem::FSeek64(RFILE* fp, s64 offset, int whence)
{
   int seek_position = -1;

   if (!fp)
      return -1;

   switch (whence)
   {
      case SEEK_SET:
         seek_position = RETRO_VFS_SEEK_POSITION_START;
         break;
      case SEEK_CUR:
         seek_position = RETRO_VFS_SEEK_POSITION_CURRENT;
         break;
      case SEEK_END:
         seek_position = RETRO_VFS_SEEK_POSITION_END;
         break;
   }

   return filestream_seek(fp, offset, seek_position);
}

s64 FileSystem::FTell64(RFILE* fp)
{
	return filestream_tell(fp);
}

s64 FileSystem::FSize64(RFILE* fp)
{
	const s64 pos = filestream_tell(fp);
	if (pos >= 0)
	{
		if (filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_END) == 0)
		{
			const s64 size = filestream_tell(fp);
			if (filestream_seek(fp, pos, RETRO_VFS_SEEK_POSITION_START) == 0)
				return size;
		}
	}

	return -1;
}

std::optional<std::vector<u8>> FileSystem::ReadBinaryFile(const char* filename)
{
	int64_t size;
	RFILE *fp = OpenFile(filename, "rb");
	if (!fp)
		return std::nullopt;
	rfseek(fp, 0, SEEK_END);
	size = rftell(fp);
	rfseek(fp, 0, SEEK_SET);
	if (size < 0)
	{
		rfclose(fp);
		return std::nullopt;
	}

	std::vector<u8> res(static_cast<size_t>(size));
	if (size > 0 && static_cast<size_t>(rfread(res.data(), 1u, static_cast<size_t>(size), fp)) != static_cast<size_t>(size))
	{
		rfclose(fp);
		return std::nullopt;
	}
	rfclose(fp);
	return res;
}

std::optional<std::string> FileSystem::ReadFileToString(const char* filename)
{
	int64_t size;
	std::string res;
	RFILE *fp = OpenFile(filename, "rb");
	if (!fp)
		return std::nullopt;
	rfseek(fp, 0, SEEK_END);
	size = rftell(fp);
	rfseek(fp, 0, SEEK_SET);
	if (size < 0)
	{
		rfclose(fp);
		return std::nullopt;
	}

	res.resize(static_cast<size_t>(size));
	/* NOTE - assumes mode 'rb', for example, this will fail over missing Windows carriage return bytes */
	if (size > 0 && static_cast<size_t>(rfread(res.data(), 1u, static_cast<size_t>(size), fp)) != static_cast<size_t>(size))
	{
		rfclose(fp);
		return std::nullopt;
	}
	rfclose(fp);
	return res;
}

#ifdef _WIN32
static void TranslateStat64(struct stat* st, const struct _stat64& st64)
{
	static constexpr __int64 MAX_SIZE = static_cast<__int64>(std::numeric_limits<decltype(st->st_size)>::max());
	st->st_dev = st64.st_dev;
	st->st_ino = st64.st_ino;
	st->st_mode = st64.st_mode;
	st->st_nlink = st64.st_nlink;
	st->st_uid = st64.st_uid;
	st->st_rdev = st64.st_rdev;
	st->st_size = static_cast<decltype(st->st_size)>((st64.st_size > MAX_SIZE) ? MAX_SIZE : st64.st_size);
	st->st_atime = static_cast<time_t>(st64.st_atime);
	st->st_mtime = static_cast<time_t>(st64.st_mtime);
	st->st_ctime = static_cast<time_t>(st64.st_ctime);
}

bool FileSystem::StatFile(const char* path, struct stat* st)
{
	struct _stat64 st64;
	if (path[0] == '\0')
		return false;
	wchar_t *wpath = utf8_to_utf16_string_alloc(path);
	if (_wstat64(wpath, &st64) != 0)
	{
		free(wpath);
		return false;
	}
	free(wpath);
	TranslateStat64(st, st64);
	return true;
}

bool FileSystem::DeleteFilePath(const char* path)
{
	if (path[0] == '\0')
		return false;
	wchar_t *wpath             = utf8_to_utf16_string_alloc(path);
	const DWORD fileAttributes = GetFileAttributesW(wpath);
	if (fileAttributes == INVALID_FILE_ATTRIBUTES || fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		free(wpath);
		return false;
	}
	bool ret = (DeleteFileW(wpath) == TRUE);
	free(wpath);
	return ret;
}

bool FileSystem::DeleteDirectory(const char* path)
{
	wchar_t *wpath = utf8_to_utf16_string_alloc(path);
	bool ret       = RemoveDirectoryW(wpath);
	free(wpath);
	return ret;
}
#else
bool FileSystem::StatFile(const char* path, struct stat* st)
{
	return stat(path, st) == 0;
}

bool FileSystem::DeleteFilePath(const char* path)
{
	if (path[0] == '\0')
		return false;
	if (path_is_directory(path))
		return false;
	return (unlink(path) == 0);
}

bool FileSystem::DeleteDirectory(const char* path)
{
	if (path[0] == '\0')
		return false;
	if (!path_is_directory(path))
		return false;
	return (unlink(path) == 0);
}
#endif

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
			if (!match_all && !StringUtil::WildcardMatch(name, pattern))
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
