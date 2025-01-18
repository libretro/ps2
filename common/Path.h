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

#pragma once

#include "Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <vector>

namespace Path
{
	/// Converts any forward slashes to backslashes on Win32.
	std::string ToNativePath(const std::string_view& path);
	void ToNativePath(std::string* path);

	/// Joins path components together, producing a new path.
	std::string Combine(const std::string_view& base, const std::string_view& next);

	/// Removes all .. and . components from a path.
	std::string Canonicalize(const std::string_view& path);
	void Canonicalize(std::string* path);

	/// Returns true if the specified path is an absolute path (C:\Path on Windows or /path on Unix).
	bool IsAbsolute(const std::string_view& path);

	/// Replaces the extension of a filename with another.
	std::string ReplaceExtension(const std::string_view& path, const std::string_view& new_extension);

	/// Returns the directory component of a filename.
	std::string_view GetDirectory(const std::string_view& path);

	/// Returns the filename component of a filename.
	std::string_view GetFileName(const std::string_view& path);

	/// Splits a path into its components, handling both Windows and Unix separators.
	std::vector<std::string_view> SplitWindowsPath(const std::string_view& path);

	/// Splits a path into its components, only handling native separators.
	std::vector<std::string_view> SplitNativePath(const std::string_view& path);
} // namespace Path
