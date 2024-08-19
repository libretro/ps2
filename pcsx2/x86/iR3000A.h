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
#ifndef _R3000A_SUPERREC_
#define _R3000A_SUPERREC_

#include "R3000A.h"
#include "iCore.h"

/* to be consistent with EE */
#define PSX_HI XMMGPR_HI
#define PSX_LO XMMGPR_LO

extern u32 g_iopCyclePenalty;

////////////////////////////////////////////////////////////////////
// IOP Constant Propagation Defines, Vars, and API - From here down!

#define PSX_IS_CONST1(reg) ((reg) < 32 && (g_psxHasConstReg & (1 << (reg))))
#define PSX_IS_CONST2(reg1, reg2) ((g_psxHasConstReg & (1 << (reg1))) && (g_psxHasConstReg & (1 << (reg2))))
#define PSX_IS_DIRTY_CONST(reg) ((reg) < 32 && (g_psxHasConstReg & (1 << (reg))) && (!(g_psxFlushedConstReg & (1 << (reg)))))
#define PSX_SET_CONST(reg) \
	{ \
		if ((reg) < 32) \
		{ \
			g_psxHasConstReg |= (1u << (reg)); \
			g_psxFlushedConstReg &= ~(1u << (reg)); \
		} \
	}

#define PSX_DEL_CONST(reg) \
	{ \
		if ((reg) < 32) \
			g_psxHasConstReg &= ~(1 << (reg)); \
	}

extern u32 g_psxConstRegs[32];
extern u32 g_psxHasConstReg, g_psxFlushedConstReg;

typedef void (*R3000AFNPTR)();
typedef void (*R3000AFNPTR_INFO)(int info);

/* non mmx/xmm version, slower
 *
 * rd = rs op rt */
#define PSXRECOMPILE_CONSTCODE0(fn, info) \
	void rpsx##fn(void) \
	{ \
		psxRecompileCodeConst0(rpsx##fn##_const, rpsx##fn##_consts, rpsx##fn##_constt, rpsx##fn##_, info); \
	}

// rt = rs op imm16
#define PSXRECOMPILE_CONSTCODE1(fn, info) \
	void rpsx##fn(void) \
	{ \
		psxRecompileCodeConst1(rpsx##fn##_const, rpsx##fn##_, info); \
	}

// rd = rt op sa
#define PSXRECOMPILE_CONSTCODE2(fn, info) \
	void rpsx##fn(void) \
	{ \
		psxRecompileCodeConst2(rpsx##fn##_const, rpsx##fn##_, info); \
	}

// [lo,hi] = rt op rs
#define PSXRECOMPILE_CONSTCODE3(fn, LOHI) \
	void rpsx##fn(void) \
	{ \
		psxRecompileCodeConst3(rpsx##fn##_const, rpsx##fn##_consts, rpsx##fn##_constt, rpsx##fn##_, LOHI); \
	}

#define PSXRECOMPILE_CONSTCODE3_PENALTY(fn, LOHI, cycles) \
	void rpsx##fn(void) \
	{ \
		psxRecompileCodeConst3(rpsx##fn##_const, rpsx##fn##_consts, rpsx##fn##_constt, rpsx##fn##_, LOHI); \
		g_iopCyclePenalty = cycles; \
	}

// rd = rs op rt
void psxRecompileCodeConst0(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int xmminfo);
// rt = rs op imm16
void psxRecompileCodeConst1(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo);
// rd = rt op sa
void psxRecompileCodeConst2(R3000AFNPTR constcode, R3000AFNPTR_INFO noconstcode, int xmminfo);
// [lo,hi] = rt op rs
void psxRecompileCodeConst3(R3000AFNPTR constcode, R3000AFNPTR_INFO constscode, R3000AFNPTR_INFO consttcode, R3000AFNPTR_INFO noconstcode, int LOHI);

#endif
