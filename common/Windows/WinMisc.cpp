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

#if defined(_WIN32)

#include "common/Pcsx2Defs.h"
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/General.h"
#include "common/WindowInfo.h"

#include <mmsystem.h>
#include <timeapi.h>
#include <VersionHelpers.h>

alignas(16) static LARGE_INTEGER lfreq;

void InitCPUTicks()
{
	QueryPerformanceFrequency(&lfreq);
}

u64 GetTickFrequency()
{
	return lfreq.QuadPart;
}

u64 GetCPUTicks()
{
	LARGE_INTEGER count;
	QueryPerformanceCounter(&count);
	return count.QuadPart;
}

void Threading::Sleep(int ms)
{
	::Sleep(ms);
}
#endif
