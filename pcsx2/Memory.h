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

#ifdef __linux__
#include <signal.h>
#endif

#include "vtlb.h"

#include "common/emitter/x86_intrin.h"

// [TODO] This *could* be replaced with an assignment operator on u128 that implicitly
// uses _mm_store and _mm_load internally.  However, there are alignment concerns --
// u128 is not alignment strict.  (we would need a u128 and u128a for types known to
// be strictly 128-bit aligned).
#define CopyQWC(dest, src) _mm_store_ps( (float*)(dest), _mm_load_ps((const float*)(src)))

#define ZeroQWC(dest ) _mm_store_ps((float*)(dest), _mm_setzero_ps())

#define PSM(mem)	(vtlb_GetPhyPtr((mem)&0x1fffffff)) //pcsx2 is a competition.The one with most hacks wins :D

#define psHu8(mem)	(*(u8 *)&eeHw[(mem) & 0xffff])
#define psHu16(mem)	(*(u16*)&eeHw[(mem) & 0xffff])
#define psHu32(mem)	(*(u32*)&eeHw[(mem) & 0xffff])
#define psHu64(mem)	(*(u64*)&eeHw[(mem) & 0xffff])
#define psHu128(mem)(*(u128*)&eeHw[(mem) & 0xffff])

#define psSu32(mem)	(*(u32 *)&eeMem->Scratch[(mem) & 0x3fff])
#define psSu64(mem)	(*(u64 *)&eeMem->Scratch[(mem) & 0x3fff])
#define psSu128(mem)	(*(u128*)&eeMem->Scratch[(mem) & 0x3fff])


extern void memSetPageAddr(u32 vaddr, u32 paddr);
extern void memClearPageAddr(u32 vaddr);
extern void memBindConditionalHandlers(void);

extern void memMapVUmicro(void);

#define memRead8 vtlb_memRead<mem8_t>
#define memRead16 vtlb_memRead<mem16_t>
#define memRead32 vtlb_memRead<mem32_t>
#define memRead64 vtlb_memRead<mem64_t>

#define memWrite8 vtlb_memWrite<mem8_t>
#define memWrite16 vtlb_memWrite<mem16_t>
#define memWrite32 vtlb_memWrite<mem32_t>
#define memWrite64 vtlb_memWrite<mem64_t>

static __fi void memRead128(u32 mem, mem128_t* out) { _mm_store_si128((__m128i*)out, vtlb_memRead128(mem)); }
static __fi void memRead128(u32 mem, mem128_t& out) { memRead128(mem, &out); }

static __fi void memWrite128(u32 mem, const mem128_t* val)	{ vtlb_memWrite128(mem, r128_load(val)); }
static __fi void memWrite128(u32 mem, const mem128_t& val)	{ vtlb_memWrite128(mem, r128_load(&val)); }

extern u16 ba0R16(u32 mem);
