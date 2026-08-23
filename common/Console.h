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

#pragma once

#include "Pcsx2Defs.h"

// ----------------------------------------------------------------------------------------
//  IConsoleWriter -- For printing messages to the libretro log.
// ----------------------------------------------------------------------------------------
// PCSX2 is a threaded environment and multiple threads can write to the log
// asynchronously. Individual calls are written atomically by the underlying
// retro_log_callback, but a multi-line block may be interrupted by logs from
// other threads; compound such blocks into a single string before issuing them.
//
// Each method maps to a fixed retro_log_level:
//   WriteLn  -> RETRO_LOG_INFO
//   Error    -> RETRO_LOG_ERROR
//   Warning  -> RETRO_LOG_WARN
//   Debug    -> RETRO_LOG_DEBUG
//
// DevCon is the developer console: the same sink, but WriteLn logs at
// RETRO_LOG_DEBUG, because DevCon call sites are per-packet and
// per-transfer chatter that the frontend's log level should gate
// rather than the build configuration, which is how the donor tree
// silenced them.
//
// All functions return false so logs can be disabled at compile time with the
// "0 && Console.WriteLn(...)" trick.
// Upstream PCSX2 colorizes console output; the libretro log has no color
// support, so the color argument is accepted (for source compatibility with
// transplanted upstream code) and ignored.
enum ConsoleColors
{
	Color_Default = 0,
	Color_Black,
	Color_Green,
	Color_Red,
	Color_Blue,
	Color_Magenta,
	Color_Orange,
	Color_Gray,
	Color_Cyan,
	Color_Yellow,
	Color_White,
	Color_StrongBlack,
	Color_StrongRed,
	Color_StrongGreen,
	Color_StrongBlue,
	Color_StrongMagenta,
	Color_StrongOrange,
	Color_StrongGray,
	Color_StrongCyan,
	Color_StrongYellow,
	Color_StrongWhite,
};

/* printf-style checking.  These are varargs functions with a format
 * string and nothing was verifying the two against each other, which is
 * how a std::string_view reached a %s in IsoFile::open() and segfaulted
 * on every unreadable disc.  Member functions count `this' as argument
 * one, hence 2,3. */
#if defined(__GNUC__) || defined(__clang__)
#define CONSOLE_PRINTF_FMT(f, a) __attribute__((format(printf, f, a)))
#else
#define CONSOLE_PRINTF_FMT(f, a)
#endif

struct IConsoleWriter
{
	bool WriteLn(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);
	bool Error(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);
	bool Warning(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);
	bool Debug(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);

	// Color-taking overloads: the color is ignored (see ConsoleColors).
	template <typename... Args>
	bool WriteLn(ConsoleColors, const char* fmt, Args... args) const { return WriteLn(fmt, args...); }
	template <typename... Args>
	bool Error(ConsoleColors, const char* fmt, Args... args) const { return Error(fmt, args...); }
	template <typename... Args>
	bool Warning(ConsoleColors, const char* fmt, Args... args) const { return Warning(fmt, args...); }
};

struct IDevConWriter
{
	bool WriteLn(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);
	bool Error(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);
	bool Warning(const char* fmt, ...) const CONSOLE_PRINTF_FMT(2, 3);

	// Color-taking overloads: the color is ignored (see ConsoleColors).
	template <typename... Args>
	bool WriteLn(ConsoleColors, const char* fmt, Args... args) const { return WriteLn(fmt, args...); }
	template <typename... Args>
	bool Error(ConsoleColors, const char* fmt, Args... args) const { return Error(fmt, args...); }
	template <typename... Args>
	bool Warning(ConsoleColors, const char* fmt, Args... args) const { return Warning(fmt, args...); }
};

extern IConsoleWriter Console;
extern IDevConWriter DevCon;
