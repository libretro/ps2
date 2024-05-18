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

#include "Timer.h"
#include <cstdlib>

#if defined(_WIN32)
#include "RedtapeWindows.h"
#else
#include <time.h>
#endif

namespace Common
{
#ifdef _WIN32
	static double s_counter_frequency;
	static bool s_counter_initialized = false;

	uint64_t Timer::GetCurrentValue()
	{
		// even if this races, it should still result in the same value..
		if (!s_counter_initialized)
		{
			LARGE_INTEGER Freq;
			QueryPerformanceFrequency(&Freq);
			s_counter_frequency = static_cast<double>(Freq.QuadPart) / 1000000000.0;
			s_counter_initialized = true;
		}

		uint64_t ReturnValue;
		QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&ReturnValue));
		return ReturnValue;
	}

	double Timer::ConvertValueToSeconds(uint64_t value)
	{
		return ((static_cast<double>(value) / s_counter_frequency) / 1000000000.0);
	}
#else
	uint64_t Timer::GetCurrentValue()
	{
		struct timespec tv;
		clock_gettime(CLOCK_MONOTONIC, &tv);
		return ((uint64_t)tv.tv_nsec + (uint64_t)tv.tv_sec * 1000000000);
	}

	double Timer::ConvertValueToSeconds(uint64_t value)
	{
		return (static_cast<double>(value) / 1000000000.0);
	}
#endif

	Timer::Timer()
	{
		Reset();
	}

	void Timer::Reset()
	{
		m_tvStartValue = GetCurrentValue();
	}
} // namespace Common
