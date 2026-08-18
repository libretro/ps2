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

// jmp rel8
// Defined inline: raw byte writers over x86Ptr, shared by both emitters.
// Explicit inline, not __fi -- __fi is empty on MinGW and cannot carry
// ODR linkage for a header definition.
static __fi u8* J8Rel(int cc, int to)
{
	xWrite8(cc);
	xWrite8(to);
	return (u8*)(x86Ptr - 1);
}


/********************/
/* IX86 instructions */
/********************/

////////////////////////////////////
// jump instructions		   /
////////////////////////////////////

/* jmp rel8 */
inline u8* JMP8(u8 to)
{
	xWrite8(0xEB);
	xWrite8(to);
	return x86Ptr - 1;
}

/* jmp rel32 */
inline u32* JMP32(uptr to)
{
	xWrite8(0xE9);
	xWrite32(to);
	return (u32*)(x86Ptr - 4);
}

/* je rel8 */
inline u8* JE8(u8 to)
{
	return J8Rel(0x74, to);
}

/* jz rel8 */
inline u8* JZ8(u8 to)
{
	return J8Rel(0x74, to);
}

/* jns rel8 */
inline u8* JNS8(u8 to)
{
	return J8Rel(0x79, to);
}

/* jg rel8 */
inline u8* JG8(u8 to)
{
	return J8Rel(0x7F, to);
}

/* jge rel8 */
inline u8* JGE8(u8 to)
{
	return J8Rel(0x7D, to);
}

/* jl rel8 */
inline u8* JL8(u8 to)
{
	return J8Rel(0x7C, to);
}

inline u8* JAE8(u8 to)
{
	return J8Rel(0x73, to);
}

/* jb rel8 */
inline u8* JB8(u8 to)
{
	return J8Rel(0x72, to);
}

/* jbe rel8 */
inline u8* JBE8(u8 to)
{
	return J8Rel(0x76, to);
}

/* jle rel8 */
inline u8* JLE8(u8 to)
{
	return J8Rel(0x7E, to);
}

/* jne rel8 */
inline u8* JNE8(u8 to)
{
	return J8Rel(0x75, to);
}

/* jnz rel8 */
inline u8* JNZ8(u8 to)
{
	return J8Rel(0x75, to);
}


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
static __fi void SSE_MAXSS_XMM_to_XMM(int to, int from) { x86Emitter::xMAX.SS(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE_MAXSS_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_MINSS_XMM_to_XMM(int to, int from) { x86Emitter::xMIN.SS(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE_MINSS_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_ADDSS_XMM_to_XMM(int to, int from) { x86Emitter::xADD.SS(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE_ADDSS_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE_SUBSS_XMM_to_XMM(int to, int from) { x86Emitter::xSUB.SS(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE_SUBSS_XMM_to_XMM(int to, int from);
#endif

//*********************
//  SSE 2 Instructions*
//*********************

#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_MAXSD_XMM_to_XMM(int to, int from) { x86Emitter::xMAX.SD(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE2_MAXSD_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_MINSD_XMM_to_XMM(int to, int from) { x86Emitter::xMIN.SD(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE2_MINSD_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_ADDSD_XMM_to_XMM(int to, int from) { x86Emitter::xADD.SD(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE2_ADDSD_XMM_to_XMM(int to, int from);
#endif
#ifdef PCSX2_C89_EMITTER
static __fi void SSE2_SUBSD_XMM_to_XMM(int to, int from) { x86Emitter::xSUB.SD(x86Emitter::xRegisterSSE(to), x86Emitter::xRegisterSSE(from)); }
#else
extern void SSE2_SUBSD_XMM_to_XMM(int to, int from);
#endif
