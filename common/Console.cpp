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

#include "common/Console.h"
#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/RedtapeWindows.h" // OutputDebugString
#include "common/StringUtil.h"

using namespace Threading;

// thread-local console indentation setting.
static thread_local int conlog_Indent(0);

// thread-local console color storage.
static thread_local ConsoleColors conlog_Color(DefaultConsoleColor);

// --------------------------------------------------------------------------------------
//  ConsoleNull
// --------------------------------------------------------------------------------------

static void ConsoleNull_SetTitle(const char* title) {}
static void ConsoleNull_DoSetColor(ConsoleColors color) {}
static void ConsoleNull_Newline() {}
static void ConsoleNull_DoWrite(const char* fmt) {}
static void ConsoleNull_DoWriteLn(const char* fmt) {}

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
	pxAssert(conlog_Indent >= 0);
	return *this;
}

IConsoleWriter IConsoleWriter::Indent(int tabcount) const
{
	IConsoleWriter retval = *this;
	retval._imm_indentation = tabcount;
	return retval;
}

// Changes the active console color.
// This color will be unset by calls to colored text methods
// such as ErrorMsg and Notice.
const IConsoleWriter& IConsoleWriter::SetColor(ConsoleColors color) const
{
	// Ignore current color requests since, well, the current color is already set. ;)
	if (color == Color_Current)
		return *this;

	pxAssertMsg((color > Color_Current) && (color < ConsoleColors_Count), "Invalid ConsoleColor specified.");

	if (conlog_Color != color)
		DoSetColor(conlog_Color = color);

	return *this;
}

ConsoleColors IConsoleWriter::GetColor() const
{
	return conlog_Color;
}

// Restores the console color to default (usually black, or low-intensity white if the console uses a black background)
const IConsoleWriter& IConsoleWriter::ClearColor() const
{
	if (conlog_Color != DefaultConsoleColor)
		DoSetColor(conlog_Color = DefaultConsoleColor);

	return *this;
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
	ConsoleColorScope cs(color);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::Error(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	ConsoleColorScope cs(Color_StrongRed);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::Warning(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	ConsoleColorScope cs(Color_StrongOrange);
	FormatV(fmt, args);
	va_end(args);

	return false;
}

bool IConsoleWriter::WriteLn(ConsoleColors color, const std::string& str) const
{
	ConsoleColorScope cs(color);
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
//  ConsoleColorScope / ConsoleIndentScope
// --------------------------------------------------------------------------------------

ConsoleColorScope::ConsoleColorScope(ConsoleColors newcolor)
{
	m_IsScoped = false;
	m_newcolor = newcolor;
	EnterScope();
}

ConsoleColorScope::~ConsoleColorScope()
{
	LeaveScope();
}

void ConsoleColorScope::EnterScope()
{
	if (!m_IsScoped)
	{
		m_old_color = Console.GetColor();
		Console.SetColor(m_newcolor);
		m_IsScoped = true;
	}
}

void ConsoleColorScope::LeaveScope()
{
	m_IsScoped = m_IsScoped && (Console.SetColor(m_old_color), false);
}

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


ConsoleAttrScope::ConsoleAttrScope(ConsoleColors newcolor, int indent)
{
	m_old_color = Console.GetColor();
	Console.SetIndent(m_tabsize = indent);
	Console.SetColor(newcolor);
}

ConsoleAttrScope::~ConsoleAttrScope()
{
	Console.SetColor(m_old_color);
	Console.SetIndent(-m_tabsize);
}

NullConsoleWriter NullCon = {};
