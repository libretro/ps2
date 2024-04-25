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

#pragma once
#include <cstdint>

namespace Common
{
	class Timer
	{
	public:
		Timer();

		static uint64_t GetCurrentValue();
		static double ConvertValueToSeconds(uint64_t value);

		void Reset();
		void ResetTo(uint64_t value) { m_tvStartValue = value; }

		uint64_t GetStartValue() const { return m_tvStartValue; }
	private:
		uint64_t m_tvStartValue;
	};
} // namespace Common
