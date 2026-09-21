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
#include "../FreezeTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Process startup and shutdown. */
void SPU2_Initialize(void);
void SPU2_Shutdown(void);

/* VM startup and shutdown. */
void SPU2_Open(void);
void SPU2_Close(void);

/* Rebooting the VM, or entering PSX mode. */
void SPU2_Reset(bool psxmode);

/* Whether the core is in PSX mode. */
bool SPU2_IsRunningPSXMode(void);

/* The savestate entry point. */
s32 SPU2freeze(FreezeAction mode, freezeData *data);

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
