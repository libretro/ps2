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

// ------------------------------------------------------------------------
// Notes on Thread Local Storage:
//  * TLS is pretty simple, and "just works" from a programmer perspective, with only
//    some minor additional computational overhead (see performance notes below).
//
//  * MSVC and GCC handle TLS differently internally, but behavior to the programmer is
//    generally identical.
//
// Performance Considerations:
//  * GCC's implementation involves an extra dereference from normal storage (possibly
//    applies to x86-32 only -- x86-64 is untested).
//
//  * MSVC's implementation involves *two* extra dereferences from normal storage because
//    it has to look up the TLS heap pointer from the Windows Thread Storage Area.  (in
//    generated ASM code, this dereference is denoted by access to the fs:[2ch] address),
//
//  * However, in either case, the optimizer usually optimizes it to a register so the
//    extra overhead is minimal over a series of instructions.
//
// MSVC Notes:
//  * Important!! the Full Optimization [/Ox] option effectively disables TLS optimizations
//    in MSVC 2008 and earlier, causing generally significant code bloat.  Not tested in
//    VC2010 yet.
//
//  * VC2010 generally does a superior job of optimizing TLS across inlined functions and
//    class methods, compared to predecessors.
//


#define ModRM(mod, reg, rm) xWrite8(((mod) << 6) | ((reg) << 3) | (rm))
#define SibSB(ss, index, base) xWrite8(((ss) << 6) | ((index) << 3) | (base))



namespace x86Emitter
{

	// Empty initializers are due to frivolously pointless GCC errors (it demands the
	// objects be initialized even though they have no actual variable members).



	// ------------------------------------------------------------------------



	// clang-format off














	// clang-format on



	//////////////////////////////////////////////////////////////////////////////////////////
	// Performance note: VC++ wants to use byte/word register form for the following
	// ModRM/SibSB constructors when we use xWrite<u8>, and furthermore unrolls the
	// the shift using a series of ADDs for the following results:
	//   add cl,cl
	//   add cl,cl
	//   add cl,cl
	//   or  cl,bl
	//   add cl,cl
	//  ... etc.
	//
	// This is unquestionably bad optimization by Core2 standard, an generates tons of
	// register aliases and false dependencies. (although may have been ideal for early-
	// brand P4s with a broken barrel shifter?).  The workaround is to do our own manual
	// x86Ptr access and update using a u32 instead of u8.  Thanks to little endianness,
	// the same end result is achieved and no false dependencies are generated.  The draw-
	// back is that it clobbers 3 bytes past the end of the write, which could cause a
	// headache for someone who himself is doing some kind of headache-inducing amount of
	// recompiler SMC.  So we don't do a work-around, and just hope for the compiler to
	// stop sucking someday instead. :)
	//
	// (btw, I know this isn't a critical performance item by any means, but it's
	//  annoying simply because it *should* be an easy thing to optimize)

	void EmitSibMagic(uint regfield, const void* address, int extraRIPOffset)
	{
		// Leaf encoder: cache the thread-local emit cursor in a local so the
		// 2-3 writes below don't each re-resolve x86Ptr's TLS address. No
		// nested emit calls occur between the load and the write-back, so the
		// local can't go stale.
		u8* p = x86Ptr;
		sptr displacement = (sptr)address;
		sptr ripRelative = (sptr)address - ((sptr)p + sizeof(s8) + sizeof(s32) + extraRIPOffset);
		// Can we use a rip-relative address?  (Prefer this over eiz because it's a byte shorter)
		if (ripRelative == (s32)ripRelative)
		{
			*p++ = (u8)((0 << 6) | (regfield << 3) | ModRm_UseDisp32);
			displacement = ripRelative;
		}
		else
		{
			*p++ = (u8)((0 << 6) | (regfield << 3) | ModRm_UseSib);
			*p++ = (u8)((0 << 6) | (Sib_EIZ << 3) | Sib_UseDisp32);
		}

		{
			const s32 disp32_ = (s32)displacement;
			memcpy(p, &disp32_, sizeof(s32));
		}
		p       += sizeof(s32);
		x86Ptr   = p;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// returns TRUE if this instruction requires SIB to be encoded, or FALSE if the
	// instruction ca be encoded as ModRm alone.
	static __fi bool NeedsSibMagic(const xIndirectVoid& info)
	{
		// no registers? no sibs!
		// (xIndirectVoid::Reduce always places a register in Index, and optionally leaves
		// Base empty if only register is specified)
		if (!info.Index.IsEmpty())
		{
			// A scaled register needs a SIB
			if (info.Scale != 0)
				return true;
			// two registers needs a SIB
			if (!info.Base.IsEmpty())
				return true;
		}
		return false;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// Conditionally generates Sib encoding information!
	//
	// regfield - register field to be written to the ModRm.  This is either a register specifier
	//   or an opcode extension.  In either case, the instruction determines the value for us.
	//
	void EmitSibMagic(uint regfield, const xIndirectVoid& info, int extraRIPOffset)
	{
		// 3 bits also on x86_64 (so max is 8)
		// We might need to mask it on x86_64
		int displacement_size = (info.Displacement == 0)   ? 0 :
                                       ((is_s8(info.Displacement)) ? 1 : 2);

		if (!NeedsSibMagic(info))
		{
			// Use ModRm-only encoding, with the rm field holding an index/base register, if
			// one has been specified.  If neither register is specified then use Disp32 form,
			// which is encoded as "EBP w/o displacement" (which is why EBP must always be
			// encoded *with* a displacement of 0, if it would otherwise not have one).

			if (info.Index.IsEmpty())
			{
				EmitSibMagic(regfield, (void*)info.Displacement, extraRIPOffset);
				return;
			}
			else
			{
				// Compare the low 3 bits, not the register id: r13 encodes as
				// rm=101 just like rbp and needs the same forced displacement,
				// otherwise mod=00/rm=101 means RIP-relative and the disp32 it
				// requires is never written.
				if ((info.Index.Id & 7) == 5 && displacement_size == 0)
					displacement_size = 1; // forces [ebp]/[r13] to be encoded as [reg+0]!

				ModRM(displacement_size, regfield, info.Index.Id & 7);

				// Likewise rm=100 always means "a SIB byte follows". rsp never
				// reaches here (Reduce diverts it into the SIB path below), but
				// r12 does, and without this the encoding is truncated and the
				// next instruction's bytes are consumed as the SIB.
				if ((info.Index.Id & 7) == 4)
					SibSB(0, Sib_EIZ, info.Index.Id & 7);
			}
		}
		else
		{
			// In order to encode "just" index*scale (and no base), we have to encode
			// it as a special [index*scale + displacement] form, which is done by
			// specifying EBP as the base register and setting the displacement field
			// to zero. (same as ModRm w/o SIB form above, basically, except the
			// ModRm_UseDisp flag is specified in the SIB instead of the ModRM field).

			if (info.Base.IsEmpty())
			{
				ModRM(0, regfield, ModRm_UseSib);
				SibSB(info.Scale, info.Index.Id, Sib_UseDisp32);
				*(s32*)x86Ptr = info.Displacement;
				x86Ptr += sizeof(s32);
				return;
			}
			else
			{
				if ((info.Base.Id & 7) == 5 && displacement_size == 0)
					displacement_size = 1; // forces [ebp]/[r13] to be encoded as [reg+0]!

				ModRM(displacement_size, regfield, ModRm_UseSib);
				SibSB(info.Scale, info.Index.Id & 7, info.Base.Id & 7);
			}
		}

		if (displacement_size != 0)
		{
			if (displacement_size == 1)
			{
				*(s8*)x86Ptr = info.Displacement;
				x86Ptr += sizeof(s8);
			}
			else
			{
				const s32 disp32_ = info.Displacement;
				memcpy(x86Ptr, &disp32_, sizeof(s32));
				x86Ptr += sizeof(s32);
			}
		}
	}

	// Writes a ModRM byte for "Direct" register access forms, which is used for all
	// instructions taking a form of [reg,reg].
	void EmitSibMagic(uint reg1, const xRegisterBase& reg2, int)
	{
		xWrite8((Mod_Direct << 6) | (reg1 << 3) | (reg2.Id & 7));
	}

	void EmitSibMagic(const xRegisterBase& reg1, const xRegisterBase& reg2, int)
	{
		xWrite8((Mod_Direct << 6) | ((reg1.Id & 7) << 3) | (reg2.Id & 7));
	}

	void EmitSibMagic(const xRegisterBase& reg1, const void* src, int extraRIPOffset)
	{
		EmitSibMagic(reg1.Id & 7, src, extraRIPOffset);
	}

	void EmitSibMagic(const xRegisterBase& reg1, const xIndirectVoid& sib, int extraRIPOffset)
	{
		EmitSibMagic(reg1.Id & 7, sib, extraRIPOffset);
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	void EmitRex(uint regfield, const void* address)
	{
	}

	void EmitRex(uint regfield, const xIndirectVoid& info)
	{
		bool w = info._operandSize == 8;
		bool r = false;
		bool x = info.Index.IsExtended();
		bool b = info.Base.IsExtended();
		if (!NeedsSibMagic(info))
		{
			b = x;
			x = false;
		}
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (x << 1) | (u8)b;
		if (rex != 0x40)
			xWrite8(rex);
	}

	void EmitRex(uint reg1, const xRegisterBase& reg2)
	{
		bool w       = reg2._operandSize == 8;
		bool b       = reg2.IsExtended();
		bool ext8bit = (reg2._operandSize == 1 && reg2.Id >= 0x10);
		const u8 rex = 0x40 | (w << 3) | (u8)b;
		if (rex != 0x40 || ext8bit)
			xWrite8(rex);
	}

	void EmitRex(const xRegisterBase& reg1, const xRegisterBase& reg2)
	{
		bool w       = (reg1._operandSize == 8) || (reg2._operandSize == 8);
		bool r       = reg1.IsExtended();
		bool b       = reg2.IsExtended();
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (u8)b;
		// Either operand can be spl/bpl/sil/dil, and those need a REX byte
		// present even when it would otherwise be an empty 0x40 -- without one
		// the same encoding names ah/ch/dh/bh instead.
		bool ext8bit = (reg1._operandSize == 1 && reg1.Id >= 0x10) ||
		               (reg2._operandSize == 1 && reg2.Id >= 0x10);
		if (rex != 0x40 || ext8bit)
			xWrite8(rex);
	}

	void EmitRex(const xRegisterBase& reg1, const void* src)
	{
		bool w       = reg1._operandSize == 8;
		bool r       = reg1.IsExtended();
		const u8 rex = 0x40 | (w << 3) | (r << 2);
		bool ext8bit = (reg1._operandSize == 1 && reg1.Id >= 0x10);
		if (rex != 0x40 || ext8bit)
			xWrite8(rex);
	}

	void EmitRex(const xRegisterBase& reg1, const xIndirectVoid& sib)
	{
		bool w = reg1._operandSize == 8 || sib._operandSize == 8;
		bool r = reg1.IsExtended();
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (x << 1) | (u8)b;
		bool ext8bit = (reg1._operandSize == 1 && reg1.Id >= 0x10);
		if (rex != 0x40 || ext8bit)
			xWrite8(rex);
	}

	// For use by instructions that are implicitly wide
	void EmitRexImplicitlyWide(const xRegisterBase& reg)
	{
		const u8 rex = 0x40 | (u8)reg.IsExtended();
		if (rex != 0x40)
			xWrite8(rex);
	}

	void EmitRexImplicitlyWide(const xIndirectVoid& sib)
	{
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		const u8 rex = 0x40 | (x << 1) | (u8)b;
		if (rex != 0x40)
			xWrite8(rex);
	}

	// --------------------------------------------------------------------------------------
	//  xRegisterInt  (method implementations)
	// --------------------------------------------------------------------------------------
	xRegisterInt xRegisterInt::MatchSizeTo(xRegisterInt other) const
	{
		return other._operandSize == 1 ? xRegisterInt(xRegister8(*this)) : xRegisterInt(other._operandSize, Id);
	}


	xIndirectVoid& xIndirectVoid::Add(sptr imm)
	{
		Displacement += imm;
		return *this;
	}

	// ------------------------------------------------------------------------
	// Internal implementation of EmitSibMagic which has been custom tailored
	// to optimize special forms of the Lea instructions accordingly, such
	// as when a LEA can be replaced with a "MOV reg,imm" or "MOV reg,reg".
	//
	// preserve_flags - set to ture to disable use of SHL on [Index*Base] forms
	// of LEA, which alters flags states.
	//
	static void EmitLeaMagic(const xRegisterInt& to, const xIndirectVoid& src, bool preserve_flags)
	{
		int displacement_size = (src.Displacement == 0)    ? 0 :
                                        ((is_s8(src.Displacement)) ? 1 : 2);

		// See EmitSibMagic for commenting on SIB encoding.

		if (!NeedsSibMagic(src) && src.Displacement == (s32)src.Displacement)
		{
			// LEA Land: means we have either 1-register encoding or just an offset.
			// offset is encodable as an immediate MOV, and a register is encodable
			// as a register MOV.

			if (src.Index.IsEmpty())
			{
				xMOV(to, src.Displacement);
				return;
			}
			else if (displacement_size == 0)
			{
				_xMovRtoR(to, src.Index.MatchSizeTo(to));
				return;
			}
			else if (!preserve_flags)
			{
				// encode as MOV and ADD combo.  Make sure to use the immediate on the
				// ADD since it can encode as an 8-bit sign-extended value.

				_xMovRtoR(to, src.Index.MatchSizeTo(to));
				xADD(to, src.Displacement);
				return;
			}
		}
		else
		{
			if (src.Base.IsEmpty())
			{
				if (!preserve_flags && (displacement_size == 0))
				{
					// Encode [Index*Scale] as a combination of Mov and Shl.
					// This is more efficient because of the bloated LEA format which requires
					// a 32 bit displacement, and the compact nature of the alternative.
					//
					// (this does not apply to older model P4s with the broken barrel shifter,
					//  but we currently aren't optimizing for that target anyway).

					_xMovRtoR(to, src.Index.MatchSizeTo(to));
					xSHL(to, src.Scale);
					return;
				}
			}
			else
			{
				if (src.Scale == 0)
				{
					if (!preserve_flags)
					{
						if (src.Index == rsp)
						{
							// ESP is not encodable as an index (ix86 ignores it), thus:
							_xMovRtoR(to, src.Base.MatchSizeTo(to)); // will do the trick!
							if (src.Displacement)
								xADD(to, src.Displacement);
							return;
						}
						else if (src.Displacement == 0)
						{
							_xMovRtoR(to, src.Base.MatchSizeTo(to));
							xADD(to, src.Index.MatchSizeTo(to));
							return;
						}
					}
					else if ((src.Index == rsp) && (src.Displacement == 0))
					{
						// special case handling of ESP as Index, which is replaceable with
						// a single MOV even when preserve_flags is set! :D

						_xMovRtoR(to, src.Base.MatchSizeTo(to));
						return;
					}
				}
			}
		}

		xOpWrite(0, 0x8d, to, src);
	}

#ifndef PCSX2_C89_EMITTER
	__fi void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags)
	{
		EmitLeaMagic(to, src, preserve_flags);
	}

	__fi void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags)
	{
		EmitLeaMagic(to, src, preserve_flags);
	}

#endif

	// Always built: the 16-bit form is not switched (see instructions.h).
	__fi void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags)
	{
		xWrite8(0x66);
		EmitLeaMagic(to, src, preserve_flags);
	}

	// =====================================================================================================
	//  TEST / INC / DEC
	// =====================================================================================================
	void xImpl_Test::operator()(const xRegisterInt& to, const xRegisterInt& from) const
	{
		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0x84 : 0x85, from, to);
	}

	void xImpl_Test::operator()(const xIndirect64orLess& dest, int imm) const
	{
		xOpWrite(dest.GetPrefix16(), dest.Is8BitOp() ? 0xf6 : 0xf7, 0, dest, dest.GetImmSize());
		dest.xWriteImm(imm);
	}

	void xImpl_Test::operator()(const xRegisterInt& to, int imm) const
	{
		if (to.Id == 0)
		{
			xOpAccWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xa8 : 0xa9, 0, to);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xf6 : 0xf7, 0, to);
		}
		to.xWriteImm(imm);
	}

	void xImpl_BitScan::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
	{
		xOpWrite0F(from->GetPrefix16(), Opcode, to, from);
	}
	void xImpl_BitScan::operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const
	{
		xOpWrite0F(to->GetPrefix16(), Opcode, to, sibsrc);
	}

	void xImpl_IncDec::operator()(const xRegisterInt& to) const
	{
		if (to.Is8BitOp())
		{
			u8 regfield = isDec ? 1 : 0;
			xOpWrite(to.GetPrefix16(), 0xfe, regfield, to);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), 0xff, isDec ? 1 : 0, to);
		}
	}

	void xImpl_IncDec::operator()(const xIndirect64orLess& to) const
	{
		if (to._operandSize == 2)
			xWrite8(0x66);
		xWrite8(to.Is8BitOp() ? 0xfe : 0xff);
		EmitSibMagic(isDec ? 1 : 0, to);
	}

#ifndef PCSX2_C89_EMITTER
	const xImpl_Test xTEST = {};
#endif

#ifndef PCSX2_C89_EMITTER
	const xImpl_BitScan xBSR = {0xbd};

#endif
#ifndef PCSX2_C89_EMITTER
	const xImpl_IncDec xINC = {false};
	const xImpl_IncDec xDEC = {true};
#endif

	//////////////////////////////////////////////////////////////////////////////////////////
	// Push / Pop Emitters
	//
	// Note: pushad/popad implementations are intentionally left out.  The instructions are
	// invalid in x64, and are super slow on x32.  Use multiple Push/Pop instructions instead.

#ifndef PCSX2_C89_EMITTER
	__fi void xPOP(const xIndirectVoid& from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0x8f);
		EmitSibMagic(0, from);
	}
#endif

#ifndef PCSX2_C89_EMITTER
	__fi void xPUSH(const xIndirectVoid& from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0xff);
		EmitSibMagic(6, from);
	}
#endif

#ifndef PCSX2_C89_EMITTER
	__fi void xPOP(xRegister32or64 from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0x58 | (from->Id & 7));
	}
#endif

#ifndef PCSX2_C89_EMITTER
	__fi void xPUSH(u32 imm)
	{
		if (is_s8(imm))
		{
			xWrite8(0x6a);
			xWrite8(imm);
		}
		else
		{
			xWrite8(0x68);
			xWrite32(imm);
		}
	}
#endif
#ifndef PCSX2_C89_EMITTER
	__fi void xPUSH(xRegister32or64 from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0x50 | (from->Id & 7));
	}
#endif

	//////////////////////////////////////////////////////////////////////////////////////////
	//


#ifndef PCSX2_C89_EMITTER
	xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset)
	{
		if ((sptr)base == (s32)(sptr)base)
		{
			return offset + base;
		}
		else
		{
			xLEA(tmpRegister, ptr[base]);
			return offset + tmpRegister;
		}
	}

	void xLoadFarAddr(const xAddressReg& dst, void* addr)
	{
		sptr iaddr = (sptr)addr;
		sptr rip = (sptr)x86Ptr + 7; // LEA will be 7 bytes
		sptr disp = iaddr - rip;
		if (disp == (s32)disp)
		{
			xLEA(dst, ptr[addr]);
		}
		else
		{
			xMOV64(dst, iaddr);
		}
	}

	void xWriteImm64ToMem(u64* addr, const xAddressReg& tmp, u64 imm)
	{
		xImm64Op(xMOV, ptr64[addr], tmp, imm);
	}
#endif

} // End namespace x86Emitter
