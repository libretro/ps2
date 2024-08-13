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

/***********************************
** jump instructions              **
***********************************/

#define JE8   0x74	/* je  rel8 */
#define JZ8   0x74	/* jz  rel8 */
#define JNS8  0x79	/* jns rel8 */
#define JG8   0x7F	/* jg  rel8 */
#define JGE8  0x7D	/* jge rel8 */
#define JL8   0x7C	/* jl  rel8 */
#define JAE8  0x73	/* jl  rel8 */
#define JB8   0x72	/* jb  rel8 */
#define JBE8  0x76	/* jbe rel8 */
#define JLE8  0x7E	/* jle rel8 */
#define JNE8  0x75	/* jne rel8 */
#define JNZ8  0x75	/* jnz rel8 */
#define JE32  0x84	/* je  rel32 */
#define JZ32  0x84	/* jz  rel32 */
#define JG32  0x8F	/* jg  rel32 */
#define JL32  0x8C	/* jl  rel32 */
#define JGE32 0x8D	/* jge rel32 */
#define JLE32 0x8E	/* jle rel32 */
#define JNZ32 0x85	/* jnz rel32 */
#define JNE32 0x85	/* jne rel32 */

//*********************
// SSE1  instructions *
//*********************
extern void SSE_MAXSS_XMM_to_XMM(int to, int from);
extern void SSE_MINSS_XMM_to_XMM(int to, int from);
extern void SSE_ADDSS_XMM_to_XMM(int to, int from);
extern void SSE_SUBSS_XMM_to_XMM(int to, int from);

//*********************
//  SSE2 Instructions *
//*********************
extern void SSE2_ADDSD_XMM_to_XMM(int to, int from);
extern void SSE2_SUBSD_XMM_to_XMM(int to, int from);
