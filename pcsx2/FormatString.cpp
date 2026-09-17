#include "FormatString.h"

#include <cstdio>
#include <vector>

namespace FormatString
{
std::string FormatV(const char* format, std::va_list ap)
{
	std::va_list ap_copy;
	va_copy(ap_copy, ap);

#ifdef _WIN32
	int len = _vscprintf(format, ap_copy);
#else
	int len = std::vsnprintf(nullptr, 0, format, ap_copy);
#endif
	va_end(ap_copy);

	std::string ret;

	// If an encoding error occurs, len is -1. Which we definitely don't want to resize to.
	if (len > 0)
	{
		ret.resize(len + 1);
		std::vsnprintf(ret.data(), ret.size(), format, ap);
		ret.resize(len);
	}

	return ret;
}

std::string Format(const char* format, ...)
{
	std::va_list ap;
	va_start(ap, format);
	std::string ret = FormatV(format, ap);
	va_end(ap);
	return ret;
}
}
