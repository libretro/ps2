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

#include "StringUtil.h"
#include <cctype>
#include <codecvt>
#include <cstring>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include "RedtapeWindows.h"
#endif

namespace StringUtil
{
	std::string StdStringFromFormat(const char* format, ...)
	{
		std::va_list ap;
		va_start(ap, format);
		std::string ret = StdStringFromFormatV(format, ap);
		va_end(ap);
		return ret;
	}

	std::string StdStringFromFormatV(const char* format, std::va_list ap)
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
			ret.resize(len);
			std::vsnprintf(ret.data(), ret.size() + 1, format, ap);
		}

		return ret;
	}

	bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive /*= true*/)
	{
		if (case_sensitive)
		{
			const char* cp = nullptr;
			const char* mp = nullptr;

			while ((*subject) && (*mask != '*'))
			{
				if ((*mask != '?') && (std::tolower(*mask) != std::tolower(*subject)))
					return false;

				mask++;
				subject++;
			}

			while (*subject)
			{
				if (*mask == '*')
				{
					if (*++mask == 0)
						return true;

					mp = mask;
					cp = subject + 1;
				}
				else
				{
					if ((*mask == '?') || (std::tolower(*mask) == std::tolower(*subject)))
					{
						mask++;
						subject++;
					}
					else
					{
						mask = mp;
						subject = cp++;
					}
				}
			}

			while (*mask == '*')
				mask++;

			return *mask == 0;
		}
		else
		{
			const char* cp = nullptr;
			const char* mp = nullptr;

			while ((*subject) && (*mask != '*'))
			{
				if ((*mask != *subject) && (*mask != '?'))
					return false;

				mask++;
				subject++;
			}

			while (*subject)
			{
				if (*mask == '*')
				{
					if (*++mask == 0)
						return true;

					mp = mask;
					cp = subject + 1;
				}
				else
				{
					if ((*mask == *subject) || (*mask == '?'))
					{
						mask++;
						subject++;
					}
					else
					{
						mask = mp;
						subject = cp++;
					}
				}
			}

			while (*mask == '*')
				mask++;

			return *mask == 0;
		}
	}

	std::string toLower(const std::string_view& input)
	{
		std::string newStr;
		std::transform(input.begin(), input.end(), std::back_inserter(newStr),
			[](unsigned char c) { return std::tolower(c); });
		return newStr;
	}

	std::string toUpper(const std::string_view& input)
	{
		std::string newStr;
		std::transform(input.begin(), input.end(), std::back_inserter(newStr),
			[](unsigned char c) { return std::toupper(c); });
		return newStr;
	}

	std::string_view StripWhitespace(const std::string_view& str)
	{
		std::string_view::size_type start = 0;
		while (start < str.size() && std::isspace(str[start]))
			start++;
		if (start == str.size())
			return {};

		std::string_view::size_type end = str.size() - 1;
		while (end > start && std::isspace(str[end]))
			end--;

		return str.substr(start, end - start + 1);
	}

	void StripWhitespace(std::string* str)
	{
		{
			const char* cstr = str->c_str();
			std::string_view::size_type start = 0;
			while (start < str->size() && std::isspace(cstr[start]))
				start++;
			if (start != 0)
				str->erase(0, start);
		}

		{
			const char* cstr = str->c_str();
			std::string_view::size_type start = str->size();
			while (start > 0 && std::isspace(cstr[start - 1]))
				start--;
			if (start != str->size())
				str->erase(start);
		}
	}

	std::vector<std::string_view> SplitString(const std::string_view& str, char delimiter, bool skip_empty /*= true*/)
	{
		std::vector<std::string_view> res;
		std::string_view::size_type last_pos = 0;
		std::string_view::size_type pos;
		while (last_pos < str.size() && (pos = str.find(delimiter, last_pos)) != std::string_view::npos)
		{
			std::string_view part(StripWhitespace(str.substr(last_pos, pos - last_pos)));
			if (!skip_empty || !part.empty())
				res.push_back(std::move(part));

			last_pos = pos + 1;
		}

		if (last_pos < str.size())
		{
			std::string_view part(StripWhitespace(str.substr(last_pos)));
			if (!skip_empty || !part.empty())
				res.push_back(std::move(part));
		}

		return res;
	}

	std::string ReplaceAll(const std::string_view& subject, const std::string_view& search, const std::string_view& replacement)
	{
		std::string ret(subject);
		ReplaceAll(&ret, search, replacement);
		return ret;
	}

	void ReplaceAll(std::string* subject, const std::string_view& search, const std::string_view& replacement)
	{
		if (!subject->empty())
		{
			std::string::size_type start_pos = 0;
			while ((start_pos = subject->find(search, start_pos)) != std::string::npos)
			{
				subject->replace(start_pos, search.length(), replacement);
				start_pos += replacement.length();
			}
		}
	}

	bool ParseAssignmentString(const std::string_view& str, std::string_view* key, std::string_view* value)
	{
		const std::string_view::size_type pos = str.find('=');
		if (pos == std::string_view::npos)
		{
			*key = std::string_view();
			*value = std::string_view();
			return false;
		}

		*key = StripWhitespace(str.substr(0, pos));
		if (pos != (str.size() - 1))
			*value = StripWhitespace(str.substr(pos + 1));
		else
			*value = std::string_view();

		return true;
	}

#ifdef _WIN32
	std::wstring UTF8StringToWideString(const std::string_view& str)
	{
		std::wstring ret;
		if (!UTF8StringToWideString(ret, str))
			return {};

		return ret;
	}

	bool UTF8StringToWideString(std::wstring& dest, const std::string_view& str)
	{
		int wlen = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0);
		if (wlen < 0)
			return false;

		dest.resize(wlen);
		if (wlen > 0 && MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), dest.data(), wlen) < 0)
			return false;

		return true;
	}

	std::string WideStringToUTF8String(const std::wstring_view& str)
	{
		std::string ret;
		if (!WideStringToUTF8String(ret, str))
			ret.clear();

		return ret;
	}

	bool WideStringToUTF8String(std::string& dest, const std::wstring_view& str)
	{
		int mblen = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0, nullptr, nullptr);
		if (mblen < 0)
			return false;

		dest.resize(mblen);
		if (mblen > 0 && WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), dest.data(), mblen,
							 nullptr, nullptr) < 0)
		{
			return false;
		}

		return true;
	}
#endif
} // namespace StringUtil
