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

using namespace x86Emitter;

typedef xRegisterSSE xmm;
typedef xRegister32 x32;

#include "../microVU/microVU_Const.h"

#define xmmT1  xmm0 // Used for regAlloc
#define xmmT2  xmm1 // Used for regAlloc
#define xmmT3  xmm2 // Used for regAlloc
#define xmmT4  xmm3 // Used for regAlloc
#define xmmT5  xmm4 // Used for regAlloc
#define xmmT6  xmm5 // Used for regAlloc
#define xmmT7  xmm6 // Used for regAlloc
#define xmmPQ  xmm15 // Holds the Value and Backup Values of P and Q regs

#define gprT1  eax // eax - Temp Reg
#define gprT2  ecx // ecx - Temp Reg
#define gprT1q rax // eax - Temp Reg
#define gprT2q rcx // ecx - Temp Reg
#define gprT1b ax  // Low 16-bit of gprT1 (eax)
#define gprT2b cx  // Low 16-bit of gprT2 (ecx)

#define gprF0  ebx // Status Flag 0
#define gprF1 r12d // Status Flag 1
#define gprF2 r13d // Status Flag 2
#define gprF3 r14d // Status Flag 3

extern void mVUmergeRegs(const xmm& dest, const xmm& src, int xyzw, bool modXYZW = false);
extern void mVUsaveReg(const xmm& reg, struct e_mem ptr, int xyzw, bool modXYZW);
extern void mVUloadReg(const xmm& reg, struct e_mem ptr, int xyzw);
