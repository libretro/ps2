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
#include "Pcsx2Types.h"
#include <charconv>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Work around us defining _M_ARM64 but fast_float thinking that it means MSVC.
#if defined(_M_ARM64) && !defined(_WIN32)
#define HAD_M_ARM64 _M_ARM64
#undef _M_ARM64
#endif
#include "../3rdparty/rapidyaml/rapidyaml/ext/c4core/src/c4/ext/fast_float/include/fast_float/fast_float.h"
#if defined(HAD_M_ARM64) && !defined(_WIN32)
#define _M_ARM64 HAD_M_ARM64
#undef HAD_M_ARM64
#endif

// Older versions of libstdc++ are missing support for from_chars() with floats, and was only recently
// merged in libc++. So, just fall back to stringstream (yuck!) on everywhere except MSVC.
#if !defined(_MSC_VER)
#include <locale>
#include <sstream>
#ifdef __APPLE__
#include <Availability.h>
#endif
#endif

#ifdef _MSC_VER
#define Strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#else
#define Strncasecmp(s1, s2, n) strncasecmp(s1, s2, n)
#endif

namespace StringUtil
{
	/// Constructs a std::string from a format string.
#ifdef __GNUC__
	std::string StdStringFromFormat(const char* format, ...) __attribute__((format(printf, 1, 2)));
#else
	std::string StdStringFromFormat(const char* format, ...);
#endif
	std::string StdStringFromFormatV(const char* format, std::va_list ap);

	/// Checks if a wildcard matches a search string.
	bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive = true);

	/// Wrapper around std::from_chars
	template <typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
	inline std::optional<T> FromChars(const std::string_view& str, int base = 10)
	{
		T value;

		const std::from_chars_result result = std::from_chars(str.data(), str.data() + str.length(), value, base);
		if (result.ec != std::errc())
			return std::nullopt;

		return value;
	}
	template <typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
	inline std::optional<T> FromChars(const std::string_view& str, int base, std::string_view* endptr)
	{
		T value;

		const char* ptr = str.data();
		const char* end = ptr + str.length();
		const std::from_chars_result result = std::from_chars(ptr, end, value, base);
		if (result.ec != std::errc())
			return std::nullopt;

		if (endptr)
			*endptr = (result.ptr < end) ? std::string_view(result.ptr, end - result.ptr) : std::string_view();

		return value;
	}

	template <typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
	inline std::optional<T> FromChars(const std::string_view& str)
	{
		T value;

		const fast_float::from_chars_result result = fast_float::from_chars(str.data(), str.data() + str.length(), value);
		if (result.ec != std::errc())
			return std::nullopt;

		return value;
	}
	template <typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
	inline std::optional<T> FromChars(const std::string_view& str, std::string_view* endptr)
	{
		T value;

		const char* ptr = str.data();
		const char* end = ptr + str.length();
		const fast_float::from_chars_result result = fast_float::from_chars(ptr, end, value);
		if (result.ec != std::errc())
			return std::nullopt;

		if (endptr)
			*endptr = (result.ptr < end) ? std::string_view(result.ptr, end - result.ptr) : std::string_view();

		return value;
	}

	/// Explicit override for booleans
	template <>
	inline std::optional<bool> FromChars(const std::string_view& str, int base)
	{
		if (Strncasecmp("true", str.data(), str.length()) == 0 || Strncasecmp("yes", str.data(), str.length()) == 0 ||
			Strncasecmp("on", str.data(), str.length()) == 0 || Strncasecmp("1", str.data(), str.length()) == 0 ||
			Strncasecmp("enabled", str.data(), str.length()) == 0 || Strncasecmp("1", str.data(), str.length()) == 0)
			return true;
		if (Strncasecmp("false", str.data(), str.length()) == 0 || Strncasecmp("no", str.data(), str.length()) == 0 ||
			Strncasecmp("off", str.data(), str.length()) == 0 || Strncasecmp("0", str.data(), str.length()) == 0 ||
			Strncasecmp("disabled", str.data(), str.length()) == 0 || Strncasecmp("0", str.data(), str.length()) == 0)
			return false;
		return std::nullopt;
	}

	/// starts_with from C++20
	static inline bool StartsWith(const std::string_view& str, const std::string_view& prefix)
	{
		return (str.compare(0, prefix.length(), prefix) == 0);
	}
	static inline bool EndsWith(const std::string_view& str, const std::string_view& suffix)
	{
		const std::size_t suffix_length = suffix.length();
		return (str.length() >= suffix_length && str.compare(str.length() - suffix_length, suffix_length, suffix) == 0);
	}

	/// StartsWith/EndsWith variants which aren't case sensitive.
	static inline bool StartsWithNoCase(const std::string_view& str, const std::string_view& prefix)
	{
		return (!str.empty() && Strncasecmp(str.data(), prefix.data(), prefix.length()) == 0);
	}

	/// Strip whitespace from the start/end of the string.
	std::string_view StripWhitespace(const std::string_view& str);
	void StripWhitespace(std::string* str);

	/// Splits a string based on a single character delimiter.
	std::vector<std::string_view> SplitString(const std::string_view& str, char delimiter, bool skip_empty = true);

	/// Replaces all instances of search in subject with replacement.
	std::string ReplaceAll(const std::string_view& subject, const std::string_view& search, const std::string_view& replacement);
	void ReplaceAll(std::string* subject, const std::string_view& search, const std::string_view& replacement);

	/// Parses an assignment string (Key = Value) into its two components.
	bool ParseAssignmentString(const std::string_view& str, std::string_view* key, std::string_view* value);

	/// Strided memcpy/memcmp.
	static inline void StrideMemCpy(void* dst, std::size_t dst_stride, const void* src, std::size_t src_stride,
		std::size_t copy_size, std::size_t count)
	{
		if (src_stride == dst_stride && src_stride == copy_size)
		{
			memcpy(dst, src, src_stride * count);
			return;
		}

		const u8* src_ptr = static_cast<const u8*>(src);
		u8* dst_ptr = static_cast<u8*>(dst);
		for (std::size_t i = 0; i < count; i++)
		{
			memcpy(dst_ptr, src_ptr, copy_size);
			src_ptr += src_stride;
			dst_ptr += dst_stride;
		}
	}

	std::string toLower(const std::string_view& str);
	std::string toUpper(const std::string_view& str);

#ifdef _WIN32
	/// Converts the specified UTF-8 string to a wide string.
	std::wstring UTF8StringToWideString(const std::string_view& str);
	bool UTF8StringToWideString(std::wstring& dest, const std::string_view& str);

	/// Converts the specified wide string to a UTF-8 string.
	std::string WideStringToUTF8String(const std::wstring_view& str);
	bool WideStringToUTF8String(std::string& dest, const std::wstring_view& str);
#endif
} // namespace StringUtil
