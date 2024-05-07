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

#include "SaveState.h"
#include "IopCounters.h"

struct Pcsx2Config;

namespace SPU2
{
	/// Initialization/cleanup, call at process startup/shutdown.
	void Initialize(void);
	void Shutdown(void);

	/// Open/close, call at VM startup/shutdown.
	void Open(void);
	void Close(void);

	/// Reset, rebooting VM or going into PSX mode.
	void Reset(bool psxmode);

	/// Returns true if we're currently running in PSX mode.
	bool IsRunningPSXMode(void);

	/// Returns the current sample rate the SPU2 is operating at.
	s32 GetConsoleSampleRate(void);
} // namespace SPU2

void SPU2write(u32 mem, u16 value);
u16 SPU2read(u32 mem);

void SPU2async(u32 cycles);
s32 SPU2freeze(FreezeAction mode, freezeData* data);

void SPU2readDMA4Mem(u16* pMem, u32 size);
void SPU2writeDMA4Mem(u16* pMem, u32 size);
void SPU2interruptDMA4();
void SPU2interruptDMA7();
void SPU2readDMA7Mem(u16* pMem, u32 size);
void SPU2writeDMA7Mem(u16* pMem, u32 size);

extern u32 lClocks;

extern void TimeUpdate(u32 cClocks);
extern void SPU2_FastWrite(u32 rmem, u16 value);

//#define PCM24_S1_INTERLEAVE
