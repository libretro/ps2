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
#define x86SetJ32(j32) (*(j32) = (x86Ptr - (u8*)(j32)) - 4)

#define x86SetJ32A(j32) \
	while ((uptr)x86Ptr & 0xf) \
		*x86Ptr++ = 0x90; \
	x86SetJ32((j32))

//------------------------------------------------------------------

////////////////////////////////////
// jump instructions              //
////////////////////////////////////

// jmp rel8
extern u8* JMP8(u8 to);

// jmp rel32
extern u32* JMP32(uptr to);

// je rel8
extern u8* JE8(u8 to);
// jz rel8
extern u8* JZ8(u8 to);
// jg rel8
extern u8* JG8(u8 to);
// jge rel8
extern u8* JGE8(u8 to);
// jns rel8
extern u8* JNS8(u8 to);
// jl rel8
extern u8* JL8(u8 to);
// jae rel8
extern u8* JAE8(u8 to);
// jb rel8
extern u8* JB8(u8 to);
// jbe rel8
extern u8* JBE8(u8 to);
// jle rel8
extern u8* JLE8(u8 to);
// jne rel8
extern u8* JNE8(u8 to);
// jnz rel8
extern u8* JNZ8(u8 to);

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
extern void SSE_MAXSS_XMM_to_XMM(int to, int from);
extern void SSE_MINSS_XMM_to_XMM(int to, int from);
extern void SSE_ADDSS_XMM_to_XMM(int to, int from);
extern void SSE_SUBSS_XMM_to_XMM(int to, int from);

//*********************
//  SSE 2 Instructions*
//*********************

extern void SSE2_MAXSD_XMM_to_XMM(int to, int from);
extern void SSE2_MINSD_XMM_to_XMM(int to, int from);
extern void SSE2_ADDSD_XMM_to_XMM(int to, int from);
extern void SSE2_SUBSD_XMM_to_XMM(int to, int from);
