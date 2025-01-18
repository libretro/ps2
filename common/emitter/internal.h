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

#include "x86types.h"
#include "instructions.h"

namespace x86Emitter
{

#define OpWriteSSE(pre, op) xOpWrite0F(pre, op, to, from)

	extern void EmitSibMagic(uint regfield, const void* address, int extraRIPOffset = 0);
	extern void EmitSibMagic(uint regfield, const xIndirectVoid& info, int extraRIPOffset = 0);
	extern void EmitSibMagic(uint reg1, const xRegisterBase& reg2, int = 0);
	extern void EmitSibMagic(const xRegisterBase& reg1, const xRegisterBase& reg2, int = 0);
	extern void EmitSibMagic(const xRegisterBase& reg1, const void* src, int extraRIPOffset = 0);
	extern void EmitSibMagic(const xRegisterBase& reg1, const xIndirectVoid& sib, int extraRIPOffset = 0);

	extern void EmitRex(uint regfield, const void* address);
	extern void EmitRex(uint regfield, const xIndirectVoid& info);
	extern void EmitRex(uint reg1, const xRegisterBase& reg2);
	extern void EmitRex(const xRegisterBase& reg1, const xRegisterBase& reg2);
	extern void EmitRex(const xRegisterBase& reg1, const void* src);
	extern void EmitRex(const xRegisterBase& reg1, const xIndirectVoid& sib);

	extern void _xMovRtoR(const xRegisterInt& to, const xRegisterInt& from);

	template <typename T1, typename T2>
	__fi void xOpWrite(u8 prefix, u8 opcode, const T1& param1, const T2& param2, int extraRIPOffset = 0)
	{
		if (prefix != 0)
			xWrite8(prefix);
		EmitRex(param1, param2);

		xWrite8(opcode);

		EmitSibMagic(param1, param2, extraRIPOffset);
	}

	template <typename T1, typename T2>
	__fi void xOpAccWrite(u8 prefix, u8 opcode, const T1& param1, const T2& param2)
	{
		if (prefix != 0)
			xWrite8(prefix);
		EmitRex(param1, param2);

		xWrite8(opcode);
	}


	//////////////////////////////////////////////////////////////////////////////////////////
	// emitter helpers for xmm instruction with prefixes, most of which are using
	// the basic opcode format (items inside braces denote optional or conditional
	// emission):
	//
	//   [Prefix] / 0x0f / [OpcodePrefix] / Opcode / ModRM+[SibSB]
	//
	// Prefixes are typically 0x66, 0xf2, or 0xf3.  OpcodePrefixes are either 0x38 or
	// 0x3a [and other value will result in assertion failue].
	//
	template <typename T1, typename T2>
	__fi void xOpWrite0F(u8 prefix, u16 opcode, const T1& param1, const T2& param2)
	{
		if (prefix != 0)
			xWrite8(prefix);
		EmitRex(param1, param2);

		const bool is16BitOpcode = ((opcode & 0xff) == 0x38) || ((opcode & 0xff) == 0x3a);

		if (is16BitOpcode)
		{
			xWrite8(0x0f);
			xWrite16(opcode);
		}
		else
			xWrite16((opcode << 8) | 0x0f);

		EmitSibMagic(param1, param2);
	}

	template <typename T1, typename T2>
	__fi void xOpWrite0F(u8 prefix, u16 opcode, const T1& param1, const T2& param2, u8 imm8)
	{
		if (prefix != 0)
			xWrite8(prefix);
		EmitRex(param1, param2);

		const bool is16BitOpcode = ((opcode & 0xff) == 0x38) || ((opcode & 0xff) == 0x3a);

		if (is16BitOpcode)
		{
			xWrite8(0x0f);
			xWrite16(opcode);
		}
		else
			xWrite16((opcode << 8) | 0x0f);

		EmitSibMagic(param1, param2, 1);
		xWrite8(imm8);
	}

	// VEX 2 Bytes Prefix
	template <typename T1, typename T2, typename T3>
	__fi void xOpWriteC5(u8 prefix, u8 opcode, const T1& param1, const T2& param2, const T3& param3)
	{
		const xRegisterBase& reg = param1.IsReg() ? param1 : param2;

		u8 nR = reg.IsExtended() ? 0x00 : 0x80;
		u8 L;

		// Needed for 256-bit movemask.
		if constexpr (std::is_same_v<T3, xRegisterSSE>)
			L = param3._operandSize == 32 ? 4 : 0;
		else
			L = reg._operandSize == 32 ? 4 : 0;

		u8 nv = (param2.IsEmpty() ? 0xF : ((~param2.Id & 0xF))) << 3;

		u8 p =
			prefix == 0xF2 ? 3 :
			prefix == 0xF3 ? 2 :
			prefix == 0x66 ? 1 :
                             0;

		xWrite8(0xC5);
		xWrite8(nR | nv | L | p);
		xWrite8(opcode);
		EmitSibMagic(param1, param3);
	}
} // namespace x86Emitter
