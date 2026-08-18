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

#include <cstring>

#ifdef FSCALE
# undef FSCALE // Defined in a macOS header
#endif

static __fi u32* J32Rel(int cc, u32 to)
{
	xWrite8(0x0F);
	xWrite8(cc);
	xWrite32(to);
	return (u32*)(x86Ptr - 4);
}

//------------------------------------------------------------------
// legacy jump/align functions
//------------------------------------------------------------------
#define x86SetJ8(j8) (*(j8)      = (u8)((x86Ptr - (j8)) - 1))
/* memcpy: the patch target is an arbitrary code-buffer address, and a
 * raw typed store there is UB (misaligned-store, UBSan).  GCC folds
 * the fixed-size memcpy to the same single mov. */
#define x86SetJ32(j32) \
	{ \
		u32 xsj32_val_ = (u32)((x86Ptr - (u8*)(j32)) - 4); \
		memcpy((j32), &xsj32_val_, sizeof(u32)); \
	}

#define x86SetJ32A(j32) \
	while ((uptr)x86Ptr & 0xf) \
		*x86Ptr++ = 0x90; \
	x86SetJ32((j32))

//------------------------------------------------------------------

////////////////////////////////////
// jump instructions              //
////////////////////////////////////

// Legacy jump helpers: two static helpers carry the byte writes (internal
// linkage per TU -- no ODR question to get wrong), and the per-condition
// entry points are macros over them, same as the C89 core's idiom. Each
// returns a pointer to the displacement it wrote, for x86SetJ8/x86SetJ32.
static __fi u8* J8Rel(int cc, int to)
{
	xWrite8(cc);
	xWrite8(to);
	return (u8*)(x86Ptr - 1);
}

static __fi u32* J32Rel(u32 to)
{
	xWrite8(0xE9);
	xWrite32(to);
	return (u32*)(x86Ptr - 4);
}

/* jmp rel8 / rel32 */
#define JMP8(to) J8Rel(0xEB, (to))
#define JMP32(to) J32Rel((u32)(uptr)(to))

/* conditional rel8 */
#define JE8(to) J8Rel(0x74, (to))
#define JZ8(to) J8Rel(0x74, (to))
#define JNS8(to) J8Rel(0x79, (to))
#define JG8(to) J8Rel(0x7F, (to))
#define JGE8(to) J8Rel(0x7D, (to))
#define JL8(to) J8Rel(0x7C, (to))
#define JAE8(to) J8Rel(0x73, (to))
#define JB8(to) J8Rel(0x72, (to))
#define JBE8(to) J8Rel(0x76, (to))
#define JLE8(to) J8Rel(0x7E, (to))
#define JNE8(to) J8Rel(0x75, (to))
#define JNZ8(to) J8Rel(0x75, (to))


// je rel32
#define JE32(to) J32Rel(0x84, to)
// jg rel32
#define JG32(to) J32Rel(0x8F, to)
// jge rel32
#define JGE32(to) J32Rel(0x8D, to)
// jl rel32
#define JL32(to) J32Rel(0x8C, to)
// jle rel32
#define JLE32(to) J32Rel(0x8E, to)
// jne rel32
#define JNE32(to) J32Rel(0x85, to)
// jz rel32
static __fi u32 *JZ32(u32 to)  { return J32Rel(0x84, to); }
// jnz rel32
static __fi u32 *JNZ32(u32 to) { return J32Rel(0x85, to); }

//*********************
// SSE   instructions *
//*********************
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_MAXSS_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf3, 0x5f, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE_MAXSS_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_MINSS_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf3, 0x5d, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE_MINSS_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_ADDSS_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf3, 0x58, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE_ADDSS_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_SUBSS_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf3, 0x5c, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE_SUBSS_XMM_to_XMM(int to, int from);
#endif

//*********************
//  SSE 2 Instructions*
//*********************

#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_MAXSD_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf2, 0x5f, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE2_MAXSD_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_MINSD_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf2, 0x5d, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE2_MINSD_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_ADDSD_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf2, 0x58, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE2_ADDSD_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_SUBSD_XMM_to_XMM(int to, int from) { e_u8* p_ = (e_u8*)x86Ptr; E_SSE_RR(p_, 0xf2, 0x5c, to, from); x86Ptr = (u8*)p_; }
#else
extern void SSE2_SUBSD_XMM_to_XMM(int to, int from);
#endif
