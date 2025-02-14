/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include "internal.h"

// warning: suggest braces around initialization of subobject [-Wmissing-braces]
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif

namespace x86Emitter
{
	const xImplAVX_Move xVMOVAPS = {0x00, 0x28, 0x29};
	const xImplAVX_Move xVMOVUPS = {0x00, 0x10, 0x11};

	const xImplAVX_ThreeArgYMM xVPAND = {0x66, 0xDB};
	const xImplAVX_CmpInt xVPCMP = {
		{0x66, 0x74}, // VPCMPEQB
		{0x66, 0x75}, // VPCMPEQW
		{0x66, 0x76}, // VPCMPEQD
		{0x66, 0x64}, // VPCMPGTB
		{0x66, 0x65}, // VPCMPGTW
		{0x66, 0x66}, // VPCMPGTD
	};

	void xVMOVMSKPS(const xRegister32& to, const xRegisterSSE& from)
	{
		xOpWriteC5(0x00, 0x50, to, xRegister32(), from);
	}

	void xVMOVMSKPD(const xRegister32& to, const xRegisterSSE& from)
	{
		xOpWriteC5(0x66, 0x50, to, xRegister32(), from);
	}

	void xImplAVX_Move::operator()(const xRegisterSSE& to, const xRegisterSSE& from) const
	{
		if (to != from)
			xOpWriteC5(Prefix, LoadOpcode, to, xRegisterSSE(), from);
	}

	void xImplAVX_Move::operator()(const xRegisterSSE& to, const xIndirectVoid& from) const
	{
		xOpWriteC5(Prefix, LoadOpcode, to, xRegisterSSE(), from);
	}

	void xImplAVX_Move::operator()(const xIndirectVoid& to, const xRegisterSSE& from) const
	{
		xOpWriteC5(Prefix, StoreOpcode, from, xRegisterSSE(), to);
	}

	void xImplAVX_ThreeArg::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_ThreeArg::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_ThreeArgYMM::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_ThreeArgYMM::operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(Prefix, Opcode, to, from1, from2);
	}

	void xImplAVX_CmpFloatHelper::PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0x00, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0x00, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0x66, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0x66, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0xF3, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0xF3, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const
	{
		xOpWriteC5(0xF2, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}

	void xImplAVX_CmpFloatHelper::SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const
	{
		xOpWriteC5(0xF2, 0xC2, to, from1, from2);
		xWrite8(static_cast<u8>(CType));
	}
} // namespace x86Emitter
