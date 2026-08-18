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

namespace x86Emitter
{


	// --------------------------------------------------------------------------------------
	//  xImpl_Group1
	// --------------------------------------------------------------------------------------
	struct xImpl_Group1
	{
		G1Type InstType;

		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;

		void operator()(const xIndirectVoid& to, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& from) const;
		void operator()(const xRegisterInt& to, int imm) const;
		void operator()(const xIndirect64orLess& to, int imm) const;
	};

	// ------------------------------------------------------------------------
	// This class combines x86 with SSE/SSE2 logic operations (ADD, OR, and NOT).
	// Note: ANDN [AndNot] is handled below separately.
	//
	struct xImpl_G1Logic : public xImpl_Group1
	{
		xImplSimd_DestRegSSE PS; // packed single precision
		xImplSimd_DestRegSSE PD; // packed double precision
	};

	// ------------------------------------------------------------------------
	// This class combines x86 with SSE/SSE2 arithmetic operations (ADD/SUB).
	//
	struct xImpl_G1Arith : public xImpl_Group1
	{
		xImplSimd_DestRegSSE PS; // packed single precision
		xImplSimd_DestRegSSE PD; // packed double precision
		xImplSimd_DestRegSSE SS; // scalar single precision
		xImplSimd_DestRegSSE SD; // scalar double precision
	};
} /* End namespace x86Emitter */
