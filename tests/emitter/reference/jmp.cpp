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
 * ix86 core v0.9.1
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

#include "tests/emitter/reference/internal.h"

namespace x86Emitter
{

	void xImpl_JmpCall::operator()(const xAddressReg& absreg) const
	{
		// Jumps are always wide and don't need the rex.W
		xOpWrite(0, 0xff, isJmp ? 4 : 2, absreg.GetNonWide());
	}
	void xImpl_JmpCall::operator()(const xIndirectNative& src) const
	{
		// Jumps are always wide and don't need the rex.W
		EmitRex(0, xIndirect32(src.Base, src.Index, 1, 0));
		xWrite8(0xff);
		EmitSibMagic(isJmp ? 4 : 2, src);
	}

#ifndef PCSX2_C89_EMITTER
	const xImpl_JmpCall xJMP = {true};
#endif
#ifndef PCSX2_C89_EMITTER
	const xImpl_JmpCall xCALL = {false};


#endif
	template <typename Reg1, typename Reg2>
	void prepareRegsForFastcall(const Reg1& a1, const Reg2& a2)
	{
		if (a1.IsEmpty())
			return;

		// Make sure we don't mess up if someone tries to fastcall with a1 in arg2reg and a2 in arg1reg
		if (a2.Id != arg1reg.Id)
		{
			xMOV(Reg1(arg1reg), a1);
			if (!a2.IsEmpty())
			{
				xMOV(Reg2(arg2reg), a2);
			}
		}
		else if (a1.Id != arg2reg.Id)
		{
			xMOV(Reg2(arg2reg), a2);
			xMOV(Reg1(arg1reg), a1);
		}
		else
		{
			xPUSH(a1);
			xMOV(Reg2(arg2reg), a2);
			xPOP(Reg1(arg1reg));
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegister32& a1, const xRegister32& a2) const
	{
		prepareRegsForFastcall(a1, a2);
		uptr disp = ((uptr)x86Ptr + 5) - (uptr)f;
		if ((sptr)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		prepareRegsForFastcall(a1, a2);
		uptr disp = ((uptr)x86Ptr + 5) - (uptr)f;
		if ((sptr)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegisterLong& a2) const
	{
		if (!a2.IsEmpty())
		{
			xMOV(arg2reg, a2);
		}
		xMOV(arg1reg, a1);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, void* a1) const
	{
		xLEA(arg1reg, ptr[a1]);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegister32& a2) const
	{
		if (!a2.IsEmpty())
		{
			xMOV(arg2regd, a2);
		}
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const void* f, const xIndirect32& a1) const
	{
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, u32 a2) const
	{
		xMOV(arg1regd, a1);
		xMOV(arg2regd, a2);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const xIndirectNative& f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		prepareRegsForFastcall(a1, a2);
		xCALL(f);
	}

#ifndef PCSX2_C89_EMITTER
	const xImpl_FastCall xFastCall = {};
#endif

} // namespace x86Emitter
