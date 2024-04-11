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

#include <libretro.h>
#include "common/Console.h"
#include "common/StringUtil.h"

// thread-local console indentation setting.
static thread_local int conlog_Indent(0);

extern retro_log_printf_t log_cb;
static ConsoleColors log_color = Color_Default;

// --------------------------------------------------------------------------------------
//  ConsoleWriter_Libretro
// --------------------------------------------------------------------------------------
static void RetroLog_DoSetColor(ConsoleColors color)
{
	if (color != Color_Current)
		log_color = color;
}

static void RetroLog_DoWrite(const char* fmt)
{
	retro_log_level level = RETRO_LOG_INFO;
	switch (log_color)
	{
		case Color_StrongRed: // intended for errors
			level = RETRO_LOG_ERROR;
			break;
		case Color_StrongOrange: // intended for warnings
			level = RETRO_LOG_WARN;
			break;
		case Color_Cyan:   // faint visibility, intended for logging PS2/IOP output
		case Color_Yellow: // faint visibility, intended for logging PS2/IOP output
		case Color_White:  // faint visibility, intended for logging PS2/IOP output
			level = RETRO_LOG_DEBUG;
			break;
		default:
		case Color_Default:
		case Color_Black:
		case Color_Green:
		case Color_Red:
		case Color_Blue:
		case Color_Magenta:
		case Color_Orange:
		case Color_Gray:
		case Color_StrongBlack:
		case Color_StrongGreen: // intended for infrequent state information
		case Color_StrongBlue:  // intended for block headings
		case Color_StrongMagenta:
		case Color_StrongGray:
		case Color_StrongCyan:
		case Color_StrongYellow:
		case Color_StrongWhite:
			break;
	}

	log_cb(level, "%s", fmt);
}

static void RetroLog_SetTitle(const char* title)
{
	log_cb(RETRO_LOG_INFO, "%s\n", title);
}

static void RetroLog_Newline(void)
{
	RetroLog_DoWrite("\n");
}

static void RetroLog_DoWriteLn(const char* fmt)
{
	retro_log_level level = RETRO_LOG_INFO;
	switch (log_color)
	{
		case Color_StrongRed: // intended for errors
			level = RETRO_LOG_ERROR;
			break;
		case Color_StrongOrange: // intended for warnings
			level = RETRO_LOG_WARN;
			break;
		case Color_Cyan:   // faint visibility, intended for logging PS2/IOP output
		case Color_Yellow: // faint visibility, intended for logging PS2/IOP output
		case Color_White:  // faint visibility, intended for logging PS2/IOP output
			level = RETRO_LOG_DEBUG;
			break;
		default:
		case Color_Default:
		case Color_Black:
		case Color_Green:
		case Color_Red:
		case Color_Blue:
		case Color_Magenta:
		case Color_Orange:
		case Color_Gray:
		case Color_StrongBlack:
		case Color_StrongGreen: // intended for infrequent state information
		case Color_StrongBlue:  // intended for block headings
		case Color_StrongMagenta:
		case Color_StrongGray:
		case Color_StrongCyan:
		case Color_StrongYellow:
		case Color_StrongWhite:
			break;
	}

	log_cb(level, "%s\n", fmt);
}

static const IConsoleWriter ConsoleWriter_Libretro =
	{
		RetroLog_DoWrite,
		RetroLog_DoWriteLn,
		RetroLog_DoSetColor,

		RetroLog_DoWrite,
		RetroLog_Newline,
		RetroLog_SetTitle,

		0, // instance-level indentation (should always be 0)
};

// =====================================================================================================
//  IConsoleWriter  (implementations)
// =====================================================================================================
// (all non-virtual members that do common work and then pass the result through DoWrite
//  or DoWriteLn)

// Parameters:
//   glob_indent - this parameter is used to specify a global indentation setting.  It is used by
//      WriteLn function, but defaults to 0 for Warning and Error calls.  Local indentation always
//      applies to all writes.
std::string IConsoleWriter::_addIndentation(const std::string& src, int glob_indent = 0) const
{
	const int indent = glob_indent + _imm_indentation;

	std::string indentStr;
	for (int i = 0; i < indent; i++)
		indentStr += '\t';

	std::string result;
	result.reserve(src.length() + 16 * indent);
	result.append(indentStr);
	result.append(src);

	std::string::size_type pos = result.find('\n');
	while (pos != std::string::npos)
	{
		result.insert(pos + 1, indentStr);
		pos = result.find('\n', pos + 1);
	}

	return result;
}

// Sets the indentation to be applied to all WriteLn's.  The indentation is added to the
// primary write, and to any newlines specified within the write.  Note that this applies
// to calls to WriteLn *only* -- calls to Write bypass the indentation parser.
const IConsoleWriter& IConsoleWriter::SetIndent(int tabcount) const
{
	conlog_Indent += tabcount;
	return *this;
}

IConsoleWriter IConsoleWriter::Indent(int tabcount) const
{
	IConsoleWriter retval = *this;
	retval._imm_indentation = tabcount;
	return retval;
}

// --------------------------------------------------------------------------------------
//  ASCII/UTF8 (char*)
// --------------------------------------------------------------------------------------

bool IConsoleWriter::FormatV(const char* fmt, va_list args) const
{
	// TODO: Make this less rubbish
	if ((_imm_indentation + conlog_Indent) > 0)
		DoWriteLn(_addIndentation(StringUtil::StdStringFromFormatV(fmt, args), conlog_Indent).c_str());
	else
		DoWriteLn(StringUtil::StdStringFromFormatV(fmt, args).c_str());

	return false;
}

bool IConsoleWriter::WriteLn(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::WriteLn(ConsoleColors color, const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::Error(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::Warning(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::WriteLn(ConsoleColors color, const std::string& str) const
{
	return WriteLn(str);
}

bool IConsoleWriter::WriteLn(const std::string& str) const
{
	// TODO: Make this less rubbish
	if ((_imm_indentation + conlog_Indent) > 0)
		DoWriteLn(_addIndentation(str, conlog_Indent).c_str());
	else
		DoWriteLn(str.c_str());

	return false;
}

bool IConsoleWriter::Error(const std::string& str) const
{
	return WriteLn(Color_StrongRed, str);
}

bool IConsoleWriter::Warning(const std::string& str) const
{
	return WriteLn(Color_StrongOrange, str);
}

// --------------------------------------------------------------------------------------
//  ConsoleIndentScope
// --------------------------------------------------------------------------------------

ConsoleIndentScope::ConsoleIndentScope(int tabs)
{
	m_IsScoped = false;
	m_amount = tabs;
	EnterScope();
}

ConsoleIndentScope::~ConsoleIndentScope()
{
	LeaveScope();
}

void ConsoleIndentScope::EnterScope()
{
	m_IsScoped = m_IsScoped || (Console.SetIndent(m_amount), true);
}

void ConsoleIndentScope::LeaveScope()
{
	m_IsScoped = m_IsScoped && (Console.SetIndent(-m_amount), false);
}


IConsoleWriter Console = ConsoleWriter_Libretro;
const IConsoleWriter* PatchesCon = &ConsoleWriter_Libretro;
