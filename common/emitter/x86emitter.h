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

/*
 * ix86 public header v0.9.1
 *
 * Original Authors (v0.6.2 and prior):
 *		linuzappz <linuzappz@pcsx.net>
 *		alexey silinov
 *		goldfinger
 *		zerofrog(@gmail.com)
 *
 * Authors of v0.9.1:
 *		Jake.Stine(@gmail.com)
 *		cottonvibes(@gmail.com)
 *		sudonim(1@gmail.com)
 */

//  PCSX2's New C++ Emitter
// --------------------------------------------------------------------------------------
// To use it just include the x86Emitter namespace into your file/class/function off choice.
//
// This header file is intended for use by public code.  It includes the appropriate
// inlines and macro definitions for efficient codegen.  (code internal to the emitter
// should usually use ix86_internal.h instead, and manually include the
// ix86_inlines.inl file when it is known that inlining of ModSib functions are
// wanted).
//

#pragma once

#include "x86types.h"

/* Every recompiler emits through c89ops.h; the few C++-spelled survivors
 * (forward-jump typedefs, xInvertCond) live in c89compat.h with C89-backed
 * bodies. The reference emitter -- the instruction veneer, the shim, and
 * the object type system -- lives under tests/emitter/reference/ and is
 * compiled only by the byte suites, which include it explicitly. */
#include "c89compat.h"
#include "c89ops.h"


//////////////////////////////////////////////////////////////////////////////////////////
// Helper object to handle ABI frame
// All x86-64 calling conventions ensure/require stack to be 16 bytes aligned
// I couldn't find documentation on when, but compilers would indicate it's before the call: https://gcc.godbolt.org/z/KzTfsz

/* Shadow space and the two extra callee-saved registers are the only
 * Win64/SysV differences; the shared skeleton lives once and the
 * platform inserts its extra chunk at the marked points. */
#ifdef _WIN32
#define SCOPED_STACK_FRAME_WIN_BEGIN(m_offset) \
	xe_push64_r(7); \
	xe_push64_r(6); \
	xe_sub64_ri(4, 32); \
	(m_offset) += 48;
#define SCOPED_STACK_FRAME_WIN_END() \
	xe_add64_ri(4, 32); \
	xe_pop64_r(6); \
	xe_pop64_r(7);
#else
#define SCOPED_STACK_FRAME_WIN_BEGIN(m_offset)
#define SCOPED_STACK_FRAME_WIN_END()
#endif

#define SCOPED_STACK_FRAME_BEGIN(m_offset) \
	(m_offset) = sizeof(void*); \
	xe_push64_r(5); \
	(m_offset) += sizeof(void*); \
	xe_push64_r(3); \
	xe_push64_r(12); \
	xe_push64_r(13); \
	xe_push64_r(14); \
	xe_push64_r(15); \
	(m_offset) += 40; \
	SCOPED_STACK_FRAME_WIN_BEGIN(m_offset) \
	xe_add64_ri(4, (-((16 - ((m_offset) % 16)) % 16)))

#define SCOPED_STACK_FRAME_END(m_offset) \
	xe_add64_ri(4, ((16 - ((m_offset) % 16)) % 16)); \
	SCOPED_STACK_FRAME_WIN_END() \
	xe_pop64_r(15); \
	xe_pop64_r(14); \
	xe_pop64_r(13); \
	xe_pop64_r(12); \
	xe_pop64_r(3); \
	xe_pop64_r(5)
