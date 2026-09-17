/* printf into a std::string. The frontend's logger takes a format
 * directly, so this is only for the callers that need the formatted
 * text itself: a shader dump's filename, a device name, a label. */

#ifndef PCSX2_FORMATSTRING_H
#define PCSX2_FORMATSTRING_H

#include <string>
#include <cstdarg>

namespace FormatString
{
#if defined(__GNUC__) || defined(__clang__)
	std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
#else
	std::string Format(const char* format, ...);
#endif
	std::string FormatV(const char* format, std::va_list ap);
}

#endif
