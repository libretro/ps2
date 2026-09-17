#include "StringView.h"
#include <string_view>
#include <string>

#include <algorithm>
#include <iterator>   /* back_inserter; libstdc++ gets it via <algorithm>, MSVC does not */
#include <cctype>

namespace StringView
{
static std::string_view StripWhitespace(const std::string_view& str)
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

	std::string toLower(const std::string_view& input)
{
	std::string newStr;
	std::transform(input.begin(), input.end(), std::back_inserter(newStr),
		[](unsigned char c) { return std::tolower(c); });
	return newStr;
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
}
