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
#include "Console.h"
#include "StringUtil.h"

extern retro_log_printf_t log_cb;

static void DoLog(retro_log_level level, const char* fmt, va_list args)
{
	log_cb(level, "%s\n", StringUtil::StdStringFromFormatV(fmt, args).c_str());
}

bool IConsoleWriter::WriteLn(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_INFO, fmt, args);
	va_end(args);
	return false;
}

bool IConsoleWriter::Error(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_ERROR, fmt, args);
	va_end(args);
	return false;
}

bool IConsoleWriter::Warning(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_WARN, fmt, args);
	va_end(args);
	return false;
}

bool IConsoleWriter::Debug(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_DEBUG, fmt, args);
	va_end(args);
	return false;
}

bool IDevConWriter::WriteLn(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_DEBUG, fmt, args);
	va_end(args);
	return false;
}

bool IDevConWriter::Error(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_ERROR, fmt, args);
	va_end(args);
	return false;
}

bool IDevConWriter::Warning(const char* fmt, ...) const
{
	va_list args;
	va_start(args, fmt);
	DoLog(RETRO_LOG_WARN, fmt, args);
	va_end(args);
	return false;
}

IConsoleWriter Console;
IDevConWriter DevCon;

/* The C translation units (DEV9 ATA) cannot name log_cb directly: it
 * is a C++ global and MSVC would decorate the symbol. This shim gives
 * them a C-linkage entry into the same logger. Unlike the
 * IConsoleWriter methods it appends nothing -- C callers put the
 * newline in the format string. */
extern "C" void pcsx2_log(int level, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_cb((retro_log_level)level, "%s", StringUtil::StdStringFromFormatV(fmt, args).c_str());
	va_end(args);
}
