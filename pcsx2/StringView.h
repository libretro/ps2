/* String-view predicates the tree needs and libretro-common cannot
 * give it: its string_starts_with and friends take a NUL-terminated
 * char*, and a string_view is not one -- a serial out of a YAML
 * document or an ELF path out of SYSTEM.CNF is a window into a larger
 * buffer. Compare by length and content instead. */

#ifndef PCSX2_STRINGVIEW_H
#define PCSX2_STRINGVIEW_H

#include <string>
#include <cstddef>
#include <string_view>
#include <cstring>
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

namespace StringView
{
static inline bool StartsWith(const std::string_view& str, const std::string_view& prefix)
{
	return (str.compare(0, prefix.length(), prefix) == 0);
}

static inline bool EndsWith(const std::string_view& str, const std::string_view& suffix)
{
	const std::size_t suffix_length = suffix.length();
	return (str.length() >= suffix_length && str.compare(str.length() - suffix_length, suffix_length, suffix) == 0);
}

static inline bool StartsWithNoCase(const std::string_view& str, const std::string_view& prefix)
{
	return (!str.empty() && Strncasecmp(str.data(), prefix.data(), prefix.length()) == 0);
}

static inline bool EndsWithNoCase(const std::string_view str, const std::string_view suffix)
{
	const std::size_t suffix_length = suffix.length();
	return (str.length() >= suffix_length && Strncasecmp(str.data() + (str.length() - suffix_length), suffix.data(), suffix_length) == 0);
}

/* Lowercased copy, for a key looked up case-insensitively. */
std::string toLower(const std::string_view& str);

/* "key = value", each side stripped; false when there is no '='. */
bool ParseAssignmentString(const std::string_view& str, std::string_view* key, std::string_view* value);
}

#endif
