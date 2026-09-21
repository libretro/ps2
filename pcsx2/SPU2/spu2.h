/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "../IopCounters.h"

/* The savestate entry point and the SPU2 namespace are the emulator's
 * side of the boundary; the sources below this header build as C, so
 * they see only what C can read. SaveState.h is not included here --
 * forward declarations keep <deque> and friends out of every SPU2 unit. */
#ifdef __cplusplus

struct Pcsx2Config;
enum class FreezeAction;
struct freezeData;

namespace SPU2
{
	/*/ Initialization/cleanup, call at process startup/shutdown. */
	void Initialize(void);
	void Shutdown(void);

	/*/ Open/close, call at VM startup/shutdown. */
	void Open(void);
	void Close(void);

	/*/ Reset, rebooting VM or going into PSX mode. */
	void Reset(bool psxmode);

	/*/ Returns true if we're currently running in PSX mode. */
	bool IsRunningPSXMode(void);
}

s32 SPU2freeze(FreezeAction mode, freezeData* data);

#endif

#ifdef __cplusplus
extern "C" {
#endif

void SPU2write(u32 mem, u16 value);
u16 SPU2read(u32 mem);

extern u32 lClocks;
typedef void RegWriteHandler(u32 arg, u16 value);
extern RegWriteHandler* const tbl_reg_writes[0x401];
extern const u16 tbl_reg_args[0x401];

extern void TimeUpdate(u64 cClocks);

#ifdef __cplusplus
}
#endif
