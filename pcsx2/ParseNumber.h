/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

/* Parse a number out of a string view, empty on anything the whole view
 * is not. The integer form takes a base, the floating one does not, and
 * both have a variant that reports where they stopped. Used by the
 * core-option parsing and the patch and cheat readers. */

#ifndef PCSX2_PARSENUMBER_H
#define PCSX2_PARSENUMBER_H

#include <optional>
#include <string_view>
#include <string>
#include <charconv>
#include <cstdlib>
#include <type_traits>
#include "common/Pcsx2Defs.h"

/* Case-insensitive compare of a bounded run, by the platform's name. */
#ifndef Strncasecmp
#ifdef _MSC_VER
#define Strncasecmp(s1, s2, n) _strnicmp(s1, s2, n)
#else
#include <strings.h>
#define Strncasecmp(s1, s2, n) strncasecmp(s1, s2, n)
#endif
#endif
#include <string/rstrtod.h>   /* rstrtof_len, rstrtod_len */

namespace ParseNumber
{
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
	static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);

	size_t used = 0;
	T value;
	if constexpr (std::is_same_v<T, float>)
		value = rstrtof_len(str.data(), str.length(), &used);
	else
		value = rstrtod_len(str.data(), str.length(), &used);
	if (used == 0)
		return std::nullopt;

	return value;
}

	template <typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view& str, std::string_view* endptr)
{
	static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);

	size_t used = 0;
	T value;
	if constexpr (std::is_same_v<T, float>)
		value = rstrtof_len(str.data(), str.length(), &used);
	else
		value = rstrtod_len(str.data(), str.length(), &used);
	if (used == 0)
		return std::nullopt;

	if (endptr)
		*endptr = (used < str.length()) ? std::string_view(str.data() + used, str.length() - used) : std::string_view();

	return value;
}

/* "true"/"yes"/"on"/"1"/"enabled" and their negatives; nothing else. */
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
}

#endif
