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

#pragma once
#include "Pcsx2Defs.h"
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <streams/file_stream.h>

#ifndef stat64
#define stat64 stat
#endif
#ifndef fstat64
#define fstat64 fstat
#endif

#ifdef _WIN32
#define FS_OSPATH_SEPARATOR_CHARACTER '\\'
#define FS_OSPATH_SEPARATOR_STR "\\"
#else
#define FS_OSPATH_SEPARATOR_CHARACTER '/'
#define FS_OSPATH_SEPARATOR_STR "/"
#endif

enum FILESYSTEM_FILE_ATTRIBUTES
{
	FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1,
	FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = 2,
	FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = 4,
};

enum FILESYSTEM_FIND_FLAGS
{
	FILESYSTEM_FIND_RECURSIVE = (1 << 0),
	FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
	FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
	FILESYSTEM_FIND_FOLDERS = (1 << 3),
	FILESYSTEM_FIND_FILES = (1 << 4),
	FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5),
};

struct FILESYSTEM_FIND_DATA
{
	std::time_t ModificationTime;
	std::string FileName;
	s64 Size;
	u32 Attributes;
};

namespace FileSystem
{
	using FindResultsArray = std::vector<FILESYSTEM_FIND_DATA>;

	/// Search for files
	bool FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results);

	/// Stat file
	bool StatFile(const char* path, struct stat* st);

	/// Delete file
	bool DeleteFilePath(const char* path);

	/// Rename file
	bool RenamePath(const char* OldPath, const char* NewPath);

	/// open files
	RFILE *OpenFile(const char* filename, const char* mode);
	int FSeek64(RFILE* fp, s64 offset, int whence);
	s64 FTell64(RFILE* fp);
	s64 FSize64(RFILE* fp);

	int OpenFDFile(const char* filename, int flags, int mode);

	std::optional<std::vector<u8>> ReadBinaryFile(const char* filename);
	std::optional<std::string> ReadFileToString(const char* filename);
	bool WriteBinaryFile(const char* filename, const void* data, size_t data_length);

	/// Removes a directory.
	bool DeleteDirectory(const char* path);
}; // namespace FileSystem
   
#ifdef __cplusplus
extern "C" {
#endif

int rferror(RFILE* stream);
RFILE* rfopen(const char *path, const char *mode);
int rfclose(RFILE* stream);
int64_t rftell(RFILE* stream);
int64_t rfseek(RFILE* stream, int64_t offset, int origin);
int64_t rfwrite(void const* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int rfgetc(RFILE* stream);
int rfeof(RFILE* stream);
char *rfgets(char *buffer, int maxCount, RFILE* stream);

#ifdef __cplusplus
}
#endif
