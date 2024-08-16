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


#pragma once

#include "common/Threading.h"
#include "common/Pcsx2Defs.h"

#define _xMovRtoR(to, from) xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0x88 : 0x89, from, to, 0)

/* rather than dealing with nonexistant operands.. */
#define xVZEROUPPER() \
	*(u8*)x86Ptr = 0xc5; \
	x86Ptr += sizeof(u8); \
	*(u8*)x86Ptr = 0xf8; \
	x86Ptr += sizeof(u8); \
	*(u8*)x86Ptr = 0x77; \
	x86Ptr += sizeof(u8)

/* Load Streaming SIMD Extension Control/Status from Mem32. */
#define xLDMXCSR(src) xOpWrite0F(0, 0xae, 2, src)

/* ------------------------------------------------------------------------
 * Conditional jumps to fixed targets.
 * Jumps accept any pointer as a valid target (function or data), and will generate either
 * 8 or 32 bit displacement versions of the jump, depending on relative displacement of
 * the target (efficient!)
 * Low-level jump instruction!  Specify a comparison type and a target in void* form, and
 * a jump (either 8 or 32 bit) is generated.
 */

#define xJE(func)  xJccKnownTarget(Jcc_Equal, (void*)(uintptr_t)func) 
#define xJZ(func)  xJccKnownTarget(Jcc_Zero,  (void*)(uintptr_t)func)
#define xJNE(func) xJccKnownTarget(Jcc_NotEqual, (void*)(uintptr_t)func)
#define xJNZ(func) xJccKnownTarget(Jcc_NotZero, (void*)(uintptr_t)func)
#define xJO(func)  xJccKnownTarget(Jcc_Overflow, (void*)(uintptr_t)func)
#define xJNO(func) xJccKnownTarget(Jcc_NotOverflow, (void*)(uintptr_t)func)
#define xJC(func)  xJccKnownTarget(Jcc_Carry, (void*)(uintptr_t)func)
#define xJNC(func) xJccKnownTarget(Jcc_NotCarry, (void*)(uintptr_t)func)
#define xJS(func)  xJccKnownTarget(Jcc_Signed, (void*)(uintptr_t)func)
#define xJNS(func) xJccKnownTarget(Jcc_Unsigned, (void*)(uintptr_t)func)
#define xJPE(func) xJccKnownTarget(Jcc_ParityEven, (void*)(uintptr_t)func)
#define xJPO(func) xJccKnownTarget(Jcc_ParityOdd, (void*)(uintptr_t)func)
#define xJL(func)  xJccKnownTarget(Jcc_Less, (void*)(uintptr_t)func)
#define xJLE(func) xJccKnownTarget(Jcc_LessOrEqual, (void*)(uintptr_t)func)
#define xJG(func)  xJccKnownTarget(Jcc_Greater, (void*)(uintptr_t)func)
#define xJGE(func) xJccKnownTarget(Jcc_GreaterOrEqual, (void*)(uintptr_t)func)
#define xJB(func)  xJccKnownTarget(Jcc_Below, (void*)(uintptr_t)func)
#define xJBE(func) xJccKnownTarget(Jcc_BelowOrEqual, (void*)(uintptr_t)func)
#define xJA(func)  xJccKnownTarget(Jcc_Above, (void*)(uintptr_t)func)
#define xJAE(func) xJccKnownTarget(Jcc_AboveOrEqual, (void*)(uintptr_t)func)

/* =====================================================================================================
 * SSE Conversion Operations, as looney as they are.
 * =====================================================================================================
 * These enforce pointer strictness for Indirect forms, due to the otherwise completely confusing
 * nature of the functions.  (so if a function expects an m32, you must use (u32*) or ptr32[]). */
#define xCVTDQ2PS(to, from) xOpWrite0F(0x00, 0x5b, to, from)
#define xCVTPD2DQ(to, from) xOpWrite0F(0xf2, 0xe6, to, from)
#define xCVTPD2PS(to, from) xOpWrite0F(0x66, 0x5a, to, from)
#define xCVTPI2PD(to, from) xOpWrite0F(0x66, 0x2a, to, from)
#define xCVTPS2DQ(to, from) xOpWrite0F(0x66, 0x5b, to, from)
#define xCVTPS2PD(to, from) xOpWrite0F(0x00, 0x5a, to, from)
#define xCVTSD2SI(to, from) xOpWrite0F(0xf2, 0x2d, to, from)
#define xCVTSD2SS(to, from) xOpWrite0F(0xf2, 0x5a, to, from)
#define xCVTSI2SS(to, from) xOpWrite0F(0xf3, 0x2a, to, from)
#define xCVTSS2SD(to, from) xOpWrite0F(0xf3, 0x5a, to, from)
#define xCVTSS2SI(to, from) xOpWrite0F(0xf3, 0x2d, to, from)
#define xCVTTPD2DQ(to, from) xOpWrite0F(0x66, 0xe6, to, from)
#define xCVTTPS2DQ(to, from) xOpWrite0F(0xf3, 0x5b, to, from)
#define xCVTTSD2SI(to, from) xOpWrite0F(0xf2, 0x2c, to, from)
#define xCVTTSS2SI(to, from) xOpWrite0F(0xf3, 0x2c, to, from)

/* =====================================================================================================
 * MMX Mov Instructions (MOVD, MOVQ, MOVSS).
 *
 * Notes:
 *  * Some of the functions have been renamed to more clearly reflect what they actually
 *    do.  Namely we've affixed "ZX" to several MOVs that take a register as a destination
 *    since that's what they do (MOVD clears upper 32/96 bits, etc).
 *
 *  * MOVD has valid forms for MMX and XMM registers. */

#define xMOVDZX(to, from) xOpWrite0F(0x66, 0x6e, to, from)
#define xMOVD(to, from) xOpWrite0F(0x66, 0x7e, from, to)

/* Moves from XMM to XMM, with the *upper 64 bits* of the destination register
 * being cleared to zero. */
#define xMOVQZX(to, from) xOpWrite0F(0xf3, 0x7e, to, from)

/* Moves lower quad of XMM to ptr64 (no bits are cleared) */
#define xMOVQ(dest, from) xOpWrite0F(0x66, 0xd6, from, dest)

/* ===================================================================================================== */

#define xMOVMSKPS(to, from) xOpWrite0F(0, 0x50, to, from)

#define iREGCNT_XMM 16
#define iREGCNT_GPR 16

enum XMMSSEType
{
	XMMT_INT = 0, // integer (SSE2 only)
	XMMT_FPS = 1  // floating point
};

/* Prototypes */
extern thread_local u8* x86Ptr;
extern thread_local XMMSSEType g_xmmtypes[iREGCNT_XMM];

/* Represents an unused or "empty" register assignment.  If encountered by the emitter, this
 * will be ignored (in some cases it is disallowed and generates an assertion) */
#define xRegId_Empty -1

/* Represents an invalid or uninitialized register.  If this is encountered by the emitter it
 * will generate an assertion. */
#define xRegId_Invalid -2

#ifdef _WIN32
/* Win32 requires 32 bytes of shadow stack in the caller's frame. */
#define SHADOW_STACK_SIZE 32

/* The x64 ABI considers the registers RAX, RCX, RDX, R8, R9, R10, R11, and XMM0-XMM5 volatile. */
#define Register_IsCallerSaved(id) (((id) <= 2 || ((id) >= 8 && (id) <= 11)))
/* XMM6 through XMM15 are saved. Upper 128 bits is always volatile. */
#define RegisterSSE_IsCallerSaved(id) ((id) < 6)
#else

#define SHADOW_STACK_SIZE 0

/* rax, rdi, rsi, rdx, rcx, r8, r9, r10, r11 are scratch registers. */
#define Register_IsCallerSaved(id) (((id) <= 2 || (id) == 6 || (id) == 7 || ((id) >= 8 && (id) <= 11)))
/* All vector registers are volatile. */
#define RegisterSSE_IsCallerSaved(id) (true)
#endif

/* returns the inverted conditional type for this Jcc condition.  Ie, JNS will become JS.
 * x86 conditionals are clever!  To invert conditional types, just invert the lower bit: */
#define xInvertCond(src) (Jcc_Unconditional == (src)) ? Jcc_Unconditional : (JccComparisonType)((int)(src) ^ 1)

namespace x86Emitter
{
	//------------------------------------------------------------------
	// templated version of is_s8 is required, so that u16's get correct sign extension treatment.
	template <typename T>
		static __fi bool is_s8(T imm)
		{
			return (s8)imm == (typename std::make_signed<T>::type)imm;
		}

	// ModRM 'mod' field enumeration.   Provided mostly for reference:
	enum ModRm_ModField
	{
		Mod_NoDisp = 0, // effective address operation with no displacement, in the form of [reg] (or uses special Disp32-only encoding in the case of [ebp] form)
		Mod_Disp8, // effective address operation with 8 bit displacement, in the form of [reg+disp8]
		Mod_Disp32, // effective address operation with 32 bit displacement, in the form of [reg+disp32],
		Mod_Direct, // direct reg/reg operation
	};

	// ----------------------------------------------------------------------------
	// JccComparisonType - enumerated possibilities for inspired code branching!
	//
	enum JccComparisonType
	{
		Jcc_Unknown = -2,
		Jcc_Unconditional = -1,
		Jcc_Overflow = 0x0,
		Jcc_NotOverflow = 0x1,
		Jcc_Below = 0x2,
		Jcc_Carry = 0x2,
		Jcc_AboveOrEqual = 0x3,
		Jcc_NotCarry = 0x3,
		Jcc_Zero = 0x4,
		Jcc_Equal = 0x4,
		Jcc_NotZero = 0x5,
		Jcc_NotEqual = 0x5,
		Jcc_BelowOrEqual = 0x6,
		Jcc_Above = 0x7,
		Jcc_Signed = 0x8,
		Jcc_Unsigned = 0x9,
		Jcc_ParityEven = 0xa,
		Jcc_ParityOdd = 0xb,
		Jcc_Less = 0xc,
		Jcc_GreaterOrEqual = 0xd,
		Jcc_LessOrEqual = 0xe,
		Jcc_Greater = 0xf,
	};

	// Not supported yet:
	//E3 cb 	JECXZ rel8 	Jump short if ECX register is 0.

	// ----------------------------------------------------------------------------
	// SSE2_ComparisonType - enumerated possibilities for SIMD data comparison!
	//
	enum SSE2_ComparisonType
	{
		SSE2_Equal = 0,
		SSE2_Less,
		SSE2_LessOrEqual,
		SSE2_Unordered,
		SSE2_NotEqual,
		SSE2_NotLess,
		SSE2_NotLessOrEqual,
		SSE2_Ordered
	};

	static const int ModRm_UseSib = 4; // same index value as ESP (used in RM field)
	static const int ModRm_UseDisp32 = 5; // same index value as EBP (used in Mod field)
	static const int Sib_EIZ = 4; // same index value as ESP (used in Index field)
	static const int Sib_UseDisp32 = 5; // same index value as EBP (used in Base field)

	class xAddressVoid;

	// --------------------------------------------------------------------------------------
	//  OperandSizedObject
	// --------------------------------------------------------------------------------------
	class OperandSizedObject
	{
		public:
			uint _operandSize = 0;
			OperandSizedObject() = default;
			OperandSizedObject(uint operandSize)
				: _operandSize(operandSize)
			{
			}

			bool Is8BitOp() const { return _operandSize == 1; }
			u8 GetPrefix16() const { return _operandSize == 2 ? 0x66 : 0; }

			int GetImmSize() const
			{
				switch (_operandSize)
				{
					case 1:
						return 1;
					case 2:
						return 2;
					case 4:
					case 8:
						return 4; // Only mov's take 64-bit immediates
					default:
						break;
				}
				return 0;
			}

			void xWriteImm(int imm) const
			{
				switch (_operandSize)
				{
					case 1:
						*(u8*)x86Ptr = imm;
						x86Ptr += sizeof(u8);
						break;
					case 2:
						*(u16*)x86Ptr = imm;
						x86Ptr += sizeof(u16);
						break;
					case 4:
					case 8: /* Only mov's take 64-bit immediates */
						*(u32*)x86Ptr = imm;
						x86Ptr += sizeof(u32);
						break;
					default:
						break;
				}
			}
	};

	// --------------------------------------------------------------------------------------
	//  xRegisterBase  -  type-unsafe x86 register representation.
	// --------------------------------------------------------------------------------------
	// Unless doing some fundamental stuff, use the friendly xRegister32/16/8 and xRegisterSSE
	// instead, which are built using this class and provide strict register type safety when
	// passed into emitter instructions.
	//
	class xRegisterBase : public OperandSizedObject
	{
		protected:
			xRegisterBase(uint operandSize, int regId)
				: OperandSizedObject(operandSize)
				  , Id(regId)
		{
			// Note: to avoid tons of ifdef, the 32 bits build will instantiate
			// all 16x64 bits registers.
		}

		public:
			int Id;

			xRegisterBase() : OperandSizedObject(0) , Id(xRegId_Invalid) { }
			bool IsEmpty() const { return Id < 0; }
			bool IsExtended() const { return (Id >= 0 && (Id & 0x0F) > 7); } // Register 8-15 need an extra bit to be selected
			bool IsReg() const { return true; }
	};

	class xRegisterInt : public xRegisterBase
	{
		typedef xRegisterBase _parent;

		protected:
		explicit xRegisterInt(uint operandSize, int regId)
			: _parent(operandSize, regId)
		{
		}

		public:
		xRegisterInt() = default;

		/// Checks if mapping the ID directly would be a good idea
		bool canMapIDTo(int otherSize) const
		{
			if ((otherSize == 1) == (_operandSize == 1))
				return true;
			/// IDs in [4, 8) are h registers in 8-bit
			return Id < 4 || Id >= 8;
		}

		/// Get a non-wide version of the register (for use with e.g. mov, where `mov eax, 3` and `mov rax, 3` are functionally identical but `mov eax, 3` is shorter)
		xRegisterInt GetNonWide() const
		{
			return _operandSize == 8 ? xRegisterInt(4, Id) : *this;
		}

		xRegisterInt MatchSizeTo(xRegisterInt other) const;

		bool operator==(const xRegisterInt& src) const { return Id == src.Id && (_operandSize == src._operandSize); }
		bool operator!=(const xRegisterInt& src) const { return !operator==(src); }
	};

	// --------------------------------------------------------------------------------------
	//  xRegister8/16/32/64  -  Represents a basic 8/16/32/64 bit GPR on the x86
	// --------------------------------------------------------------------------------------
	class xRegister8 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

		public:
		xRegister8() = default;
		explicit xRegister8(int regId)
			: _parent(1, regId)
		{
		}
		explicit xRegister8(const xRegisterInt& other)
			: _parent(1, other.Id)
		{
			if (!other.canMapIDTo(1))
				Id |= 0x10;
		}
		xRegister8(int regId, bool ext8bit)
			: _parent(1, regId)
		{
			if (ext8bit)
				Id |= 0x10;
		}

		bool operator==(const xRegister8& src) const { return Id == src.Id; }
		bool operator!=(const xRegister8& src) const { return Id != src.Id; }
	};

	class xRegister16 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

		public:
		xRegister16() = default;
		explicit xRegister16(int regId) : _parent(2, regId) { }
		explicit xRegister16(const xRegisterInt& other)
			: _parent(2, other.Id) { }
		bool operator==(const xRegister16& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister16& src) const { return this->Id != src.Id; }
	};

	class xRegister32 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

		public:
		xRegister32() = default;
		explicit xRegister32(int regId) : _parent(4, regId) { }
		explicit xRegister32(const xRegisterInt& other)
			: _parent(4, other.Id) { }
		static const inline xRegister32& GetInstance(uint id);
		bool operator==(const xRegister32& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister32& src) const { return this->Id != src.Id; }
	};

	class xRegister64 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

		public:
		xRegister64() = default;
		explicit xRegister64(int regId) : _parent(8, regId) { }
		explicit xRegister64(const xRegisterInt& other)
			: _parent(8, other.Id) { }
		static const inline xRegister64& GetInstance(uint id);
		bool operator==(const xRegister64& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister64& src) const { return this->Id != src.Id; }
	};

	// --------------------------------------------------------------------------------------
	//  xRegisterSSE  -  Represents either a 64 bit or 128 bit SIMD register
	// --------------------------------------------------------------------------------------
	// This register type is provided to allow legal syntax for instructions that accept
	// an XMM register as a parameter, but do not allow for a GPR.

	struct xRegisterYMMTag {};

	class xRegisterSSE : public xRegisterBase
	{
		typedef xRegisterBase _parent;

		public:
		xRegisterSSE() = default;
		explicit xRegisterSSE(int regId) : _parent(16, regId) { }
		xRegisterSSE(int regId, xRegisterYMMTag)
			: _parent(32, regId) { }

		bool operator==(const xRegisterSSE& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegisterSSE& src) const { return this->Id != src.Id; }

		static const inline xRegisterSSE& GetInstance(uint id);
		static const inline xRegisterSSE& GetYMMInstance(uint id);

		/// Returns the register to use when calling a C function.
		/// arg_number is the argument position from the left, starting with 0.
		/// sse_number is the argument position relative to the number of vector registers.
		static const inline xRegisterSSE& GetArgRegister(uint arg_number, uint sse_number, bool ymm = false);
	};

	class xRegisterCL : public xRegister8
	{
		public:
			xRegisterCL() : xRegister8(1) { }
	};

	// --------------------------------------------------------------------------------------
	//  xAddressReg
	// --------------------------------------------------------------------------------------
	// Use 32/64 bit registers as our index registers (for ModSib-style memory address calculations).
	// This type is implicitly exchangeable with xRegister32/64.
	//
	// Only xAddressReg provides operators for constructing xAddressInfo types.  These operators
	// could have been added to xRegister32/64 directly instead, however I think this design makes
	// more sense and allows the programmer a little more type protection if needed.
	//

#define xRegisterLong xRegister64
	class xAddressReg : public xRegisterLong
	{
		public:
			xAddressReg() = default;
			explicit xAddressReg(xRegisterInt other)
				: xRegisterLong(other) { }
			explicit xAddressReg(int regId) : xRegisterLong(regId) { }

			/// Returns the register to use when calling a C function.
			/// arg_number is the argument position from the left, starting with 0.
			/// sse_number is the argument position relative to the number of vector registers.
			static const inline xAddressReg& GetArgRegister(uint arg_number, uint gpr_number);

			xAddressVoid operator+(const xAddressReg& right) const;
			xAddressVoid operator+(intptr_t right) const;
			xAddressVoid operator+(const void* right) const;
			xAddressVoid operator-(intptr_t right) const;
			xAddressVoid operator-(const void* right) const;
			xAddressVoid operator*(int factor) const;
			xAddressVoid operator<<(u32 shift) const;
	};

	// --------------------------------------------------------------------------------------
	//  xRegisterEmpty
	// --------------------------------------------------------------------------------------
	struct xRegisterEmpty
	{
		operator xRegister8() const
		{
			return xRegister8(xRegId_Empty);
		}

		operator xRegister16() const
		{
			return xRegister16(xRegId_Empty);
		}

		operator xRegister32() const
		{
			return xRegister32(xRegId_Empty);
		}

		operator xRegisterSSE() const
		{
			return xRegisterSSE(xRegId_Empty);
		}

		operator xAddressReg() const
		{
			return xAddressReg(xRegId_Empty);
		}
	};

	class xRegister16or32or64
	{
		protected:
			const xRegisterInt& m_convtype;

		public:
			xRegister16or32or64(const xRegister64& src)
				: m_convtype(src) { }
			xRegister16or32or64(const xRegister32& src)
				: m_convtype(src) { }
			xRegister16or32or64(const xRegister16& src)
				: m_convtype(src) { }

			operator const xRegisterBase&() const { return m_convtype; }

			const xRegisterInt* operator->() const
			{
				return &m_convtype;
			}
	};

	class xRegister32or64
	{
		protected:
			const xRegisterInt& m_convtype;

		public:
			xRegister32or64(const xRegister64& src) : m_convtype(src) { }
			xRegister32or64(const xRegister32& src) : m_convtype(src) { }

			operator const xRegisterBase&() const { return m_convtype; }

			const xRegisterInt* operator->() const
			{
				return &m_convtype;
			}
	};

	extern const xRegisterEmpty xEmptyReg;

	// clang-format off
	extern const xRegisterSSE
		xmm0, xmm1, xmm2, xmm3,
		xmm4, xmm5, xmm6, xmm7,
		xmm8, xmm9, xmm10, xmm11,
		xmm12, xmm13, xmm14, xmm15;

	// TODO: This needs to be _M_SSE >= 0x500'ed, but we can't do it atm because common doesn't have variants.
	extern const xRegisterSSE
		ymm0, ymm1, ymm2, ymm3,
		ymm4, ymm5, ymm6, ymm7,
		ymm8, ymm9, ymm10, ymm11,
		ymm12, ymm13, ymm14, ymm15;

	extern const xAddressReg
		rax, rbx, rcx, rdx,
		rsi, rdi, rbp, rsp,
		r8, r9, r10, r11,
		r12, r13, r14, r15;

	extern const xRegister32
		eax,  ebx,  ecx,  edx,
		esi,  edi,  ebp,  esp,
		r8d,  r9d, r10d, r11d,
		r12d, r13d, r14d, r15d;

	extern const xRegister16
		ax, bx, cx, dx,
		si, di, bp, sp;

	extern const xRegister8
		al, dl, bl,
		ah, ch, dh, bh,
		spl, bpl, sil, dil,
		r8b, r9b, r10b, r11b,
		r12b, r13b, r14b, r15b;

	extern const xAddressReg
		arg1reg, arg2reg,
		arg3reg, arg4reg,
		calleeSavedReg1,
		calleeSavedReg2;


	extern const xRegister32
		arg1regd, arg2regd,
		calleeSavedReg1d,
		calleeSavedReg2d;


	// clang-format on

	extern const xRegisterCL cl; // I'm special!

	const xRegister32& xRegister32::GetInstance(uint id)
	{
		static const xRegister32* const m_tbl_x86Regs[] =
		{
			&eax, &ecx, &edx, &ebx,
			&esp, &ebp, &esi, &edi,
			&r8d, &r9d, &r10d, &r11d,
			&r12d, &r13d, &r14d, &r15d,
		};
		return *m_tbl_x86Regs[id];
	}

	const xRegister64& xRegister64::GetInstance(uint id)
	{
		static const xRegister64* const m_tbl_x86Regs[] =
		{
			&rax, &rcx, &rdx, &rbx,
			&rsp, &rbp, &rsi, &rdi,
			&r8, &r9, &r10, &r11,
			&r12, &r13, &r14, &r15
		};
		return *m_tbl_x86Regs[id];
	}

	const xRegisterSSE& xRegisterSSE::GetInstance(uint id)
	{
		static const xRegisterSSE* const m_tbl_xmmRegs[] =
		{
			&xmm0, &xmm1, &xmm2, &xmm3,
			&xmm4, &xmm5, &xmm6, &xmm7,
			&xmm8, &xmm9, &xmm10, &xmm11,
			&xmm12, &xmm13, &xmm14, &xmm15};
		return *m_tbl_xmmRegs[id];
	}

	const xRegisterSSE& xRegisterSSE::GetYMMInstance(uint id)
	{
		static const xRegisterSSE* const m_tbl_ymmRegs[] =
		{
			&ymm0, &ymm1, &ymm2, &ymm3,
			&ymm4, &ymm5, &ymm6, &ymm7,
			&ymm8, &ymm9, &ymm10, &ymm11,
			&ymm12, &ymm13, &ymm14, &ymm15};
		return *m_tbl_ymmRegs[id];
	}

	const xRegisterSSE& xRegisterSSE::GetArgRegister(uint arg_number, uint sse_number, bool ymm)
	{
#ifdef _WIN32
		// Windows passes arguments according to their position from the left.
		return ymm ? GetYMMInstance(arg_number) : GetInstance(arg_number);
#else
		// Linux counts the number of vector parameters.
		return ymm ? GetYMMInstance(sse_number) : GetInstance(sse_number);
#endif
	}

	const xAddressReg& xAddressReg::GetArgRegister(uint arg_number, uint gpr_number)
	{
#ifdef _WIN32
		// Windows passes arguments according to their position from the left.
		static constexpr const xAddressReg* regs[] = {&rcx, &rdx, &r8, &r9};
		return *regs[arg_number];
#else
		// Linux counts the number of GPR parameters.
		static constexpr const xAddressReg* regs[] = {&rdi, &rsi, &rdx, &rcx};
		return *regs[gpr_number];
#endif
	}

	// --------------------------------------------------------------------------------------
	//  xAddressVoid
	// --------------------------------------------------------------------------------------
	class xAddressVoid
	{
		public:
			xAddressReg Base; // base register (no scale)
			xAddressReg Index; // index reg gets multiplied by the scale
			int Factor; // scale applied to the index register, in factor form (not a shift!)
			intptr_t Displacement; // address displacement // 4B max even on 64 bits but keep rest for assertions

		public:
			xAddressVoid(const xAddressReg& base, const xAddressReg& index, int factor = 1, intptr_t displacement = 0);

			xAddressVoid(const xAddressReg& index, intptr_t displacement = 0);
			explicit xAddressVoid(const void* displacement);
			explicit xAddressVoid(intptr_t displacement = 0);

		public:
			xAddressVoid& Add(intptr_t imm)
			{
				Displacement += imm;
				return *this;
			}

			xAddressVoid& Add(const xAddressReg& src);
			xAddressVoid& Add(const xAddressVoid& src);

			__fi xAddressVoid operator+(const xAddressReg& right) const { return xAddressVoid(*this).Add(right); }
			__fi xAddressVoid operator+(const xAddressVoid& right) const { return xAddressVoid(*this).Add(right); }
			__fi xAddressVoid operator+(intptr_t imm) const { return xAddressVoid(*this).Add(imm); }
			__fi xAddressVoid operator-(intptr_t imm) const { return xAddressVoid(*this).Add(-imm); }
			__fi xAddressVoid operator+(const void* addr) const { return xAddressVoid(*this).Add((uintptr_t)addr); }

			__fi void operator+=(const xAddressReg& right) { Add(right); }
			__fi void operator+=(intptr_t imm) { Add(imm); }
			__fi void operator-=(intptr_t imm) { Add(-imm); }
	};

	static __fi xAddressVoid operator+(const void* addr, const xAddressVoid& right)
	{
		return right + addr;
	}

	static __fi xAddressVoid operator+(intptr_t addr, const xAddressVoid& right)
	{
		return right + addr;
	}

	// --------------------------------------------------------------------------------------
	//  xImmReg< typename xRegType >
	// --------------------------------------------------------------------------------------
	// Used to represent an immediate value which can also be optimized to a register. Note
	// that the immediate value represented by this structure is *always* legal.  The register
	// assignment is an optional optimization which can be implemented in cases where an
	// immediate is used enough times to merit allocating it to a register.
	//
	// Note: not all instructions support this operand type (yet).  You can always implement it
	// manually by checking the status of IsReg() and generating the xOP conditionally.
	//
	template <typename xRegType>
		class xImmReg
		{
			xRegType m_reg;
			int m_imm;

			public:
			xImmReg()
				: m_reg()
			{
				m_imm = 0;
			}

			xImmReg(int imm, const xRegType& reg = xEmptyReg)
			{
				m_reg = reg;
				m_imm = imm;
			}

			bool IsReg() const { return !m_reg.IsEmpty(); }
		};

	// --------------------------------------------------------------------------------------
	//  xIndirectVoid - Internal low-level representation of the ModRM/SIB information.
	// --------------------------------------------------------------------------------------
	// This class serves two purposes:  It houses 'reduced' ModRM/SIB info only, which means
	// that the Base, Index, Scale, and Displacement values are all in the correct arrange-
	// ments, and it serves as a type-safe layer between the xRegister's operators (which
	// generate xAddressInfo types) and the emitter's ModSib instruction forms.  Without this,
	// the xRegister would pass as a ModSib type implicitly, and that would cause ambiguity
	// on a number of instructions.
	//
	// End users should always use xAddressInfo instead.
	//
	class xIndirectVoid : public OperandSizedObject
	{
		public:
			xAddressReg Base; // base register (no scale)
			xAddressReg Index; // index reg gets multiplied by the scale
			uint Scale; // scale applied to the index register, in scale/shift form
			intptr_t Displacement; // offset applied to the Base/Index registers.
					       // Displacement is 8/32 bits even on x86_64
					       // However we need the whole pointer to calculate rip-relative offsets

		public:
			explicit xIndirectVoid(intptr_t disp);
			explicit xIndirectVoid(const xAddressVoid& src);
			xIndirectVoid(xAddressReg base, xAddressReg index, int scale = 0, intptr_t displacement = 0);
			xIndirectVoid& Add(intptr_t imm);

			bool IsReg() const { return false; }
			bool IsExtended() const { return false; } // Non sense but ease template

			operator xAddressVoid()
			{
				return xAddressVoid(Base, Index, Scale, Displacement);
			}

			__fi xIndirectVoid operator+(const intptr_t imm) const { return xIndirectVoid(*this).Add(imm); }
			__fi xIndirectVoid operator-(const intptr_t imm) const { return xIndirectVoid(*this).Add(-imm); }

		protected:
			void Reduce();
	};

	template <typename OperandType>
		class xIndirect : public xIndirectVoid
	{
		typedef xIndirectVoid _parent;

		public:
		explicit xIndirect(intptr_t disp)
			: _parent(disp)
		{
			_operandSize = sizeof(OperandType);
		}
		xIndirect(xAddressReg base, xAddressReg index, int scale = 0, intptr_t displacement = 0)
			: _parent(base, index, scale, displacement)
		{
			_operandSize = sizeof(OperandType);
		}
		explicit xIndirect(const xIndirectVoid& other)
			: _parent(other)
		{
		}

		xIndirect<OperandType>& Add(intptr_t imm)
		{
			Displacement += imm;
			return *this;
		}

		__fi xIndirect<OperandType> operator+(const intptr_t imm) const { return xIndirect(*this).Add(imm); }
		__fi xIndirect<OperandType> operator-(const intptr_t imm) const { return xIndirect(*this).Add(-imm); }

		bool operator==(const xIndirect<OperandType>& src) const
		{
			return (Base == src.Base) && (Index == src.Index) &&
				(Scale == src.Scale) && (Displacement == src.Displacement);
		}

		bool operator!=(const xIndirect<OperandType>& src) const
		{
			return !operator==(src);
		}

		protected:
		void Reduce();
	};

	typedef xIndirect<u128> xIndirect128;
	typedef xIndirect<u64> xIndirect64;
	typedef xIndirect<u32> xIndirect32;
	typedef xIndirect<u16> xIndirect16;
	typedef xIndirect<u8> xIndirect8;
	typedef xIndirect<u64> xIndirectNative;

	// --------------------------------------------------------------------------------------
	//  xIndirect64orLess  -  base class 64, 32, 16, and 8 bit operand types
	// --------------------------------------------------------------------------------------
	class xIndirect64orLess : public xIndirectVoid
	{
		typedef xIndirectVoid _parent;

		public:
		xIndirect64orLess(const xIndirect8& src)  : _parent(src) { }
		xIndirect64orLess(const xIndirect16& src) : _parent(src) { }
		xIndirect64orLess(const xIndirect32& src) : _parent(src) { }
		xIndirect64orLess(const xIndirect64& src) : _parent(src) { }
	};

	// --------------------------------------------------------------------------------------
	//  xAddressIndexer
	// --------------------------------------------------------------------------------------
	// This is a type-translation "interface class" which provisions our ptr[] syntax.
	// xAddressReg types go in, and xIndirectVoid derived types come out.
	//
	template <typename xModSibType>
		class xAddressIndexer
		{
			public:
				// passthrough instruction, allows ModSib to pass silently through ptr translation
				// without doing anything and without compiler error.
				const xModSibType& operator[](const xModSibType& src) const { return src; }

				xModSibType operator[](const xAddressReg& src) const
				{
					return xModSibType(src, xEmptyReg);
				}

				xModSibType operator[](const xAddressVoid& src) const
				{
					return xModSibType(src.Base, src.Index, src.Factor, src.Displacement);
				}

				xModSibType operator[](const void* src) const
				{
					return xModSibType((uintptr_t)src);
				}
		};

	// ptr[] - use this form for instructions which can resolve the address operand size from
	// the other register operand sizes.
	extern const xAddressIndexer<xIndirectVoid> ptr;
	extern const xAddressIndexer<xIndirectNative> ptrNative;
	extern const xAddressIndexer<xIndirect128> ptr128;
	extern const xAddressIndexer<xIndirect64> ptr64;
	extern const xAddressIndexer<xIndirect32> ptr32;
	extern const xAddressIndexer<xIndirect16> ptr16;
	extern const xAddressIndexer<xIndirect8> ptr8;

	// --------------------------------------------------------------------------------------
	//  xForwardJump
	// --------------------------------------------------------------------------------------
	// Primary use of this class is through the various xForwardJA8/xForwardJLE32/etc. helpers
	// defined later in this header. :)
	//

	class xForwardJumpBase
	{
		public:
			// pointer to base of the instruction *Following* the jump.  The jump address will be
			// relative to this address.
			s8* BasePtr;

		public:
			xForwardJumpBase(uint opsize, JccComparisonType cctype);
	};

	template <typename OperandType>
		class xForwardJump : public xForwardJumpBase
	{
		public:
			static const uint OperandSize = sizeof(OperandType);

			// The jump instruction is emitted at the point of object construction.  The conditional
			// type must be valid (Jcc_Unknown generates an assertion).
			xForwardJump(JccComparisonType cctype = Jcc_Unconditional)
				: xForwardJumpBase(OperandSize, cctype) { }

			// Sets the jump target by writing back the current x86Ptr to the jump instruction.
			// This method can be called multiple times, re-writing the jump instruction's target
			// in each case. (the the last call is the one that takes effect).
			void SetTarget() const
			{
				intptr_t displacement = (intptr_t)x86Ptr - (intptr_t)BasePtr;
				if (OperandSize == 1)
					BasePtr[-1] = (s8)displacement;
				else // full displacement, no sanity checks needed :D
					((s32*)BasePtr)[-1] = displacement;
			}
	};

	static __fi xAddressVoid operator+(const void* addr, const xAddressReg& reg)
	{
		return reg + (intptr_t)addr;
	}

	static __fi xAddressVoid operator+(intptr_t addr, const xAddressReg& reg)
	{
		return reg + (intptr_t)addr;
	}

	// =====================================================================================================
	//  xImpl_SIMD Types (template free!)
	// =====================================================================================================

	// ------------------------------------------------------------------------
	// For implementing SSE-only logic operations that have xmmreg,xmmreg/rm forms only,
	// like ANDPS/ANDPD
	//
	struct xImplSimd_DestRegSSE
	{
		u8 Prefix;
		u16 Opcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
	};

	// ------------------------------------------------------------------------
	// For implementing SSE-only logic operations that have xmmreg,reg/rm,imm forms only
	// (PSHUFD / PSHUFHW / etc).
	//
	struct xImplSimd_DestRegImmSSE
	{
		u8 Prefix;
		u16 Opcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from, u8 imm) const;
	};

	struct xImplSimd_DestSSE_CmpImm
	{
		u8 Prefix;
		u16 Opcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from, SSE2_ComparisonType imm) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from, SSE2_ComparisonType imm) const;
	};

	// ------------------------------------------------------------------------
	// For implementing SSE operations that have reg,reg/rm forms only,
	// but accept either MM or XMM destinations (most PADD/PSUB and other P arithmetic ops).
	//
	struct xImplSimd_DestRegEither
	{
		u8 Prefix;
		u16 Opcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_MovHL
	// --------------------------------------------------------------------------------------
	// Moves to/from high/low portions of an xmm register.
	// These instructions cannot be used in reg/reg form.
	//
	struct xImplSimd_MovHL
	{
		u16 Opcode;

		void PS(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void PS(const xIndirectVoid& to, const xRegisterSSE& from) const;

		void PD(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void PD(const xIndirectVoid& to, const xRegisterSSE& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_MovHL_RtoR
	// --------------------------------------------------------------------------------------
	// RegtoReg forms of MOVHL/MOVLH -- these are the same opcodes as MOVH/MOVL but
	// do something kinda different! Fun!
	//
	struct xImplSimd_MovHL_RtoR
	{
		u16 Opcode;

		void PS(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void PD(const xRegisterSSE& to, const xRegisterSSE& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_MoveSSE
	// --------------------------------------------------------------------------------------
	// Legends in their own right: MOVAPS / MOVAPD / MOVUPS / MOVUPD
	//
	// All implementations of Unaligned Movs will, when possible, use aligned movs instead.
	// This happens when using Mem,Reg or Reg,Mem forms where the address is simple displacement
	// which can be checked for alignment at runtime.
	//
	struct xImplSimd_MoveSSE
	{
		u8 Prefix;
		bool isAligned;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_MoveDQ
	// --------------------------------------------------------------------------------------
	// Implementations for MOVDQA / MOVDQU
	//
	// All implementations of Unaligned Movs will, when possible, use aligned movs instead.
	// This happens when using Mem,Reg or Reg,Mem forms where the address is simple displacement
	// which can be checked for alignment at runtime.

	struct xImplSimd_MoveDQ
	{
		u8 Prefix;
		bool isAligned;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_Blend
	// --------------------------------------------------------------------------------------
	// Blend - Conditional copying of values in src into dest.
	//
	struct xImplSimd_Blend
	{
		// [SSE-4.1] Conditionally copies dword values from src to dest, depending on the
		// mask bits in the immediate operand (bits [3:0]).  Each mask bit corresponds to a
		// dword element in a 128-bit operand.
		//
		// If a mask bit is 1, then the corresponding dword in the source operand is copied
		// to dest, else the dword element in dest is left unchanged.
		//
		xImplSimd_DestRegImmSSE PS;

		// [SSE-4.1] Conditionally copies quadword values from src to dest, depending on the
		// mask bits in the immediate operand (bits [1:0]).  Each mask bit corresponds to a
		// quadword element in a 128-bit operand.
		//
		// If a mask bit is 1, then the corresponding dword in the source operand is copied
		// to dest, else the dword element in dest is left unchanged.
		//
		xImplSimd_DestRegImmSSE PD;

		// [SSE-4.1] Conditionally copies dword values from src to dest, depending on the
		// mask (bits [3:0]) in XMM0 (yes, the fixed register).  Each mask bit corresponds
		// to a dword element in the 128-bit operand.
		//
		// If a mask bit is 1, then the corresponding dword in the source operand is copied
		// to dest, else the dword element in dest is left unchanged.
		//
		xImplSimd_DestRegSSE VPS;

		// [SSE-4.1] Conditionally copies quadword values from src to dest, depending on the
		// mask (bits [1:0]) in XMM0 (yes, the fixed register).  Each mask bit corresponds
		// to a quadword element in the 128-bit operand.
		//
		// If a mask bit is 1, then the corresponding dword in the source operand is copied
		// to dest, else the dword element in dest is left unchanged.
		//
		xImplSimd_DestRegSSE VPD;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_PMove
	// --------------------------------------------------------------------------------------
	// Packed Move with Sign or Zero extension.
	//
	struct xImplSimd_PMove
	{
		u16 OpcodeBase;

		// [SSE-4.1] Zero/Sign-extend the low byte values in src into word integers
		// and store them in dest.
		void BW(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void BW(const xRegisterSSE& to, const xIndirect64& from) const;

		// [SSE-4.1] Zero/Sign-extend the low byte values in src into dword integers
		// and store them in dest.
		void BD(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void BD(const xRegisterSSE& to, const xIndirect32& from) const;

		// [SSE-4.1] Zero/Sign-extend the low byte values in src into qword integers
		// and store them in dest.
		void BQ(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void BQ(const xRegisterSSE& to, const xIndirect16& from) const;

		// [SSE-4.1] Zero/Sign-extend the low word values in src into dword integers
		// and store them in dest.
		void WD(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void WD(const xRegisterSSE& to, const xIndirect64& from) const;

		// [SSE-4.1] Zero/Sign-extend the low word values in src into qword integers
		// and store them in dest.
		void WQ(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void WQ(const xRegisterSSE& to, const xIndirect32& from) const;

		// [SSE-4.1] Zero/Sign-extend the low dword values in src into qword integers
		// and store them in dest.
		void DQ(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void DQ(const xRegisterSSE& to, const xIndirect64& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  _SimdShiftHelper
	// --------------------------------------------------------------------------------------
	struct _SimdShiftHelper
	{
		u8 Prefix;
		u16 Opcode;
		u16 OpcodeImm;
		u8 Modcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void operator()(const xRegisterSSE& to, u8 imm8) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_Shift / xImplSimd_ShiftWithoutQ
	// --------------------------------------------------------------------------------------

	// Used for PSRA, which lacks the Q form.
	//
	struct xImplSimd_ShiftWithoutQ
	{
		const _SimdShiftHelper W;
		const _SimdShiftHelper D;
	};

	// Implements PSRL and PSLL
	//
	struct xImplSimd_Shift
	{
		const _SimdShiftHelper W;
		const _SimdShiftHelper D;
		const _SimdShiftHelper Q;

		void DQ(const xRegisterSSE& to, u8 imm8) const;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	struct xImplSimd_AddSub
	{
		const xImplSimd_DestRegEither B;
		const xImplSimd_DestRegEither W;
		const xImplSimd_DestRegEither D;
		const xImplSimd_DestRegEither Q;

		// Add/Sub packed signed byte [8bit] integers from src into dest, and saturate the results.
		const xImplSimd_DestRegEither SB;

		// Add/Sub packed signed word [16bit] integers from src into dest, and saturate the results.
		const xImplSimd_DestRegEither SW;

		// Add/Sub packed unsigned byte [8bit] integers from src into dest, and saturate the results.
		const xImplSimd_DestRegEither USB;

		// Add/Sub packed unsigned word [16bit] integers from src into dest, and saturate the results.
		const xImplSimd_DestRegEither USW;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	struct xImplSimd_PMul
	{
		const xImplSimd_DestRegEither LW;
		const xImplSimd_DestRegEither HW;
		const xImplSimd_DestRegEither HUW;
		const xImplSimd_DestRegEither UDQ;

		// [SSE-3] PMULHRSW multiplies vertically each signed 16-bit integer from dest with the
		// corresponding signed 16-bit integer of source, producing intermediate signed 32-bit
		// integers. Each intermediate 32-bit integer is truncated to the 18 most significant
		// bits. Rounding is always performed by adding 1 to the least significant bit of the
		// 18-bit intermediate result. The final result is obtained by selecting the 16 bits
		// immediately to the right of the most significant bit of each 18-bit intermediate
		// result and packed to the destination operand.
		//
		// Both operands can be MMX or XMM registers.  Source can be register or memory.
		//
		const xImplSimd_DestRegEither HRSW;

		// [SSE-4.1] Multiply the packed dword signed integers in dest with src, and store
		// the low 32 bits of each product in xmm1.
		const xImplSimd_DestRegSSE LD;

		// [SSE-4.1] Multiply the packed signed dword integers in dest with src.
		const xImplSimd_DestRegSSE DQ;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// For instructions that have PS/SS form only (most commonly reciprocal Sqrt functions)
	//
	struct xImplSimd_rSqrt
	{
		const xImplSimd_DestRegSSE PS;
		const xImplSimd_DestRegSSE SS;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// SQRT has PS/SS/SD forms, but not the PD form.
	//
	struct xImplSimd_Sqrt
	{
		const xImplSimd_DestRegSSE PS;
		const xImplSimd_DestRegSSE SS;
		const xImplSimd_DestRegSSE SD;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	struct xImplSimd_AndNot
	{
		const xImplSimd_DestRegSSE PS;
		const xImplSimd_DestRegSSE PD;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Packed absolute value. [sSSE3 only]
	//
	struct xImplSimd_PAbsolute
	{
		// [sSSE-3] Computes the absolute value of bytes in the src, and stores the result
		// in dest, as UNSIGNED.
		const xImplSimd_DestRegEither B;

		// [sSSE-3] Computes the absolute value of word in the src, and stores the result
		// in dest, as UNSIGNED.
		const xImplSimd_DestRegEither W;

		// [sSSE-3] Computes the absolute value of doublewords in the src, and stores the
		// result in dest, as UNSIGNED.
		const xImplSimd_DestRegEither D;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Packed Sign [sSSE3 only] - Negate/zero/preserve packed integers in dest depending on the
	// corresponding sign in src.
	//
	struct xImplSimd_PSign
	{
		// [sSSE-3] negates each byte element of dest if the signed integer value of the
		// corresponding data element in src is less than zero. If the signed integer value
		// of a data element in src is positive, the corresponding data element in dest is
		// unchanged. If a data element in src is zero, the corresponding data element in
		// dest is set to zero.
		const xImplSimd_DestRegEither B;

		// [sSSE-3] negates each word element of dest if the signed integer value of the
		// corresponding data element in src is less than zero. If the signed integer value
		// of a data element in src is positive, the corresponding data element in dest is
		// unchanged. If a data element in src is zero, the corresponding data element in
		// dest is set to zero.
		const xImplSimd_DestRegEither W;

		// [sSSE-3] negates each doubleword element of dest if the signed integer value
		// of the corresponding data element in src is less than zero. If the signed integer
		// value of a data element in src is positive, the corresponding data element in dest
		// is unchanged. If a data element in src is zero, the corresponding data element in
		// dest is set to zero.
		const xImplSimd_DestRegEither D;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Packed Multiply and Add!!
	//
	struct xImplSimd_PMultAdd
	{
		// Multiplies the individual signed words of dest by the corresponding signed words
		// of src, producing temporary signed, doubleword results. The adjacent doubleword
		// results are then summed and stored in the destination operand.
		//
		//   DEST[31:0]  = ( DEST[15:0]  * SRC[15:0])  + (DEST[31:16] * SRC[31:16] );
		//   DEST[63:32] = ( DEST[47:32] * SRC[47:32]) + (DEST[63:48] * SRC[63:48] );
		//   [.. repeat in the case of XMM src/dest operands ..]
		//
		const xImplSimd_DestRegEither WD;

		// [sSSE-3] multiplies vertically each unsigned byte of dest with the corresponding
		// signed byte of src, producing intermediate signed 16-bit integers. Each adjacent
		// pair of signed words is added and the saturated result is packed to dest.
		// For example, the lowest-order bytes (bits 7-0) in src and dest are multiplied
		// and the intermediate signed word result is added with the corresponding
		// intermediate result from the 2nd lowest-order bytes (bits 15-8) of the operands;
		// the sign-saturated result is stored in the lowest word of dest (bits 15-0).
		// The same operation is performed on the other pairs of adjacent bytes.
		//
		// In Coder Speak:
		//   DEST[15-0]  = SaturateToSignedWord( SRC[15-8]  * DEST[15-8]  + SRC[7-0]   * DEST[7-0]   );
		//   DEST[31-16] = SaturateToSignedWord( SRC[31-24] * DEST[31-24] + SRC[23-16] * DEST[23-16] );
		//   [.. repeat for each 16 bits up to 64 (mmx) or 128 (xmm) ..]
		//
		const xImplSimd_DestRegEither UBSW;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Packed Horizontal Add [SSE3 only]
	//
	struct xImplSimd_HorizAdd
	{
		// [SSE-3] Horizontal Add of Packed Data.  A three step process:
		// * Adds the single-precision floating-point values in the first and second dwords of
		//   dest and stores the result in the first dword of dest.
		// * Adds single-precision floating-point values in the third and fourth dword of dest
		//   stores the result in the second dword of dest.
		// * Adds single-precision floating-point values in the first and second dword of *src*
		//   and stores the result in the third dword of dest.
		const xImplSimd_DestRegSSE PS;

		// [SSE-3] Horizontal Add of Packed Data.  A two step process:
		// * Adds the double-precision floating-point values in the high and low quadwords of
		//   dest and stores the result in the low quadword of dest.
		// * Adds the double-precision floating-point values in the high and low quadwords of
		//   *src* stores the result in the high quadword of dest.
		const xImplSimd_DestRegSSE PD;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// DotProduct calculation (SSE4.1 only!)
	//
	struct xImplSimd_DotProduct
	{
		// [SSE-4.1] Conditionally multiplies the packed single precision floating-point
		// values in dest with the packed single-precision floats in src depending on a
		// mask extracted from the high 4 bits of the immediate byte. If a condition mask
		// bit in Imm8[7:4] is zero, the corresponding multiplication is replaced by a value
		// of 0.0.	The four resulting single-precision values are summed into an inter-
		// mediate result.
		//
		// The intermediate result is conditionally broadcasted to the destination using a
		// broadcast mask specified by bits [3:0] of the immediate byte. If a broadcast
		// mask bit is 1, the intermediate result is copied to the corresponding dword
		// element in dest.  If a broadcast mask bit is zero, the corresponding element in
		// the destination is set to zero.
		//
		xImplSimd_DestRegImmSSE PS;

		// [SSE-4.1]
		xImplSimd_DestRegImmSSE PD;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Rounds floating point values (packed or single scalar) by an arbitrary rounding mode.
	// (SSE4.1 only!)
	struct xImplSimd_Round
	{
		// [SSE-4.1] Rounds the 4 packed single-precision src values and stores them in dest.
		//
		// Imm8 specifies control fields for the rounding operation:
		//   Bit  3 - processor behavior for a precision exception (0: normal, 1: inexact)
		//   Bit  2 - If enabled, use MXCSR.RC, else use RC specified in bits 1:0 of this Imm8.
		//   Bits 1:0 - Specifies a rounding mode for this instruction only.
		//
		// Rounding Mode Reference:
		//   0 - Nearest, 1 - Negative Infinity, 2 - Positive infinity, 3 - Truncate.
		//
		const xImplSimd_DestRegImmSSE PS;

		// [SSE-4.1] Rounds the 2 packed double-precision src values and stores them in dest.
		//
		// Imm8 specifies control fields for the rounding operation:
		//   Bit  3 - processor behavior for a precision exception (0: normal, 1: inexact)
		//   Bit  2 - If enabled, use MXCSR.RC, else use RC specified in bits 1:0 of this Imm8.
		//   Bits 1:0 - Specifies a rounding mode for this instruction only.
		//
		// Rounding Mode Reference:
		//   0 - Nearest, 1 - Negative Infinity, 2 - Positive infinity, 3 - Truncate.
		//
		const xImplSimd_DestRegImmSSE PD;

		// [SSE-4.1] Rounds the single-precision src value and stores in dest.
		//
		// Imm8 specifies control fields for the rounding operation:
		//   Bit  3 - processor behavior for a precision exception (0: normal, 1: inexact)
		//   Bit  2 - If enabled, use MXCSR.RC, else use RC specified in bits 1:0 of this Imm8.
		//   Bits 1:0 - Specifies a rounding mode for this instruction only.
		//
		// Rounding Mode Reference:
		//   0 - Nearest, 1 - Negative Infinity, 2 - Positive infinity, 3 - Truncate.
		//
		const xImplSimd_DestRegImmSSE SS;

		// [SSE-4.1] Rounds the double-precision src value and stores in dest.
		//
		// Imm8 specifies control fields for the rounding operation:
		//   Bit  3 - processor behavior for a precision exception (0: normal, 1: inexact)
		//   Bit  2 - If enabled, use MXCSR.RC, else use RC specified in bits 1:0 of this Imm8.
		//   Bits 1:0 - Specifies a rounding mode for this instruction only.
		//
		// Rounding Mode Reference:
		//   0 - Nearest, 1 - Negative Infinity, 2 - Positive infinity, 3 - Truncate.
		//
		const xImplSimd_DestRegImmSSE SD;
	};

	struct xImplSimd_MinMax
	{
		const xImplSimd_DestRegSSE PS; // packed single precision
		const xImplSimd_DestRegSSE PD; // packed double precision
		const xImplSimd_DestRegSSE SS; // scalar single precision
		const xImplSimd_DestRegSSE SD; // scalar double precision
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	struct xImplSimd_Compare
	{
		SSE2_ComparisonType CType;

		void PS(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void PS(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void PD(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void PD(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void SS(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void SS(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void SD(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void SD(const xRegisterSSE& to, const xIndirectVoid& from) const;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// Compare scalar floating point values and set EFLAGS (Ordered or Unordered)
	//
	struct xImplSimd_COMI
	{
		const xImplSimd_DestRegSSE SS;
		const xImplSimd_DestRegSSE SD;
	};


	//////////////////////////////////////////////////////////////////////////////////////////
	//
	struct xImplSimd_PCompare
	{
		public:
			// Compare packed bytes for equality.
			// If a data element in dest is equal to the corresponding date element src, the
			// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
			const xImplSimd_DestRegEither EQB;

			// Compare packed words for equality.
			// If a data element in dest is equal to the corresponding date element src, the
			// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
			const xImplSimd_DestRegEither EQW;

			// Compare packed doublewords [32-bits] for equality.
			// If a data element in dest is equal to the corresponding date element src, the
			// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
			const xImplSimd_DestRegEither EQD;

			// Compare packed signed bytes for greater than.
			// If a data element in dest is greater than the corresponding date element src, the
			// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
			const xImplSimd_DestRegEither GTB;

			// Compare packed signed words for greater than.
			// If a data element in dest is greater than the corresponding date element src, the
			// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
			const xImplSimd_DestRegEither GTW;

			// Compare packed signed doublewords [32-bits] for greater than.
			// If a data element in dest is greater than the corresponding date element src, the
			// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
			const xImplSimd_DestRegEither GTD;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	//
	struct xImplSimd_PMinMax
	{
		// Compare packed unsigned byte integers in dest to src and store packed min/max
		// values in dest.
		const xImplSimd_DestRegEither UB;

		// Compare packed signed word integers in dest to src and store packed min/max
		// values in dest.
		const xImplSimd_DestRegEither SW;

		// [SSE-4.1] Compare packed signed byte integers in dest to src and store
		// packed min/max values in dest. (SSE operands only)
		const xImplSimd_DestRegSSE SB;

		// [SSE-4.1] Compare packed signed doubleword integers in dest to src and store
		// packed min/max values in dest. (SSE operands only)
		const xImplSimd_DestRegSSE SD;

		// [SSE-4.1] Compare packed unsigned word integers in dest to src and store
		// packed min/max values in dest. (SSE operands only)
		const xImplSimd_DestRegSSE UW;

		// [SSE-4.1] Compare packed unsigned doubleword integers in dest to src and store
		// packed min/max values in dest. (SSE operands only)
		const xImplSimd_DestRegSSE UD;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_Shuffle
	// --------------------------------------------------------------------------------------
	struct xImplSimd_Shuffle
	{
		inline void _selector_assertion_check(u8 selector) const;

		void PS(const xRegisterSSE& to, const xRegisterSSE& from, u8 selector) const;
		void PS(const xRegisterSSE& to, const xIndirectVoid& from, u8 selector) const;

		void PD(const xRegisterSSE& to, const xRegisterSSE& from, u8 selector) const;
		void PD(const xRegisterSSE& to, const xIndirectVoid& from, u8 selector) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImplSimd_PShuffle
	// --------------------------------------------------------------------------------------
	struct xImplSimd_PShuffle
	{
		// Copies doublewords from src and inserts them into dest at dword locations selected
		// with the order operand (8 bit immediate).
		const xImplSimd_DestRegImmSSE D;

		// Copies words from the low quadword of src and inserts them into the low quadword
		// of dest at word locations selected with the order operand (8 bit immediate).
		// The high quadword of src is copied to the high quadword of dest.
		const xImplSimd_DestRegImmSSE LW;

		// Copies words from the high quadword of src and inserts them into the high quadword
		// of dest at word locations selected with the order operand (8 bit immediate).
		// The low quadword of src is copied to the low quadword of dest.
		const xImplSimd_DestRegImmSSE HW;

		// [sSSE-3] Performs in-place shuffles of bytes in dest according to the shuffle
		// control mask in src.  If the most significant bit (bit[7]) of each byte of the
		// shuffle control mask is set, then constant zero is written in the result byte.
		// Each byte in the shuffle control mask forms an index to permute the corresponding
		// byte in dest. The value of each index is the least significant 4 bits (128-bit
		// operation) or 3 bits (64-bit operation) of the shuffle control byte.
		//
		const xImplSimd_DestRegEither B;

		// below is my test bed for a new system, free of subclasses.  Was supposed to improve intellisense
		// but it doesn't (makes it worse).  Will try again in MSVC 2010. --air
	};

	// --------------------------------------------------------------------------------------
	//  SimdImpl_PUnpack
	// --------------------------------------------------------------------------------------
	struct SimdImpl_PUnpack
	{
		// Unpack and interleave low-order bytes from src and dest into dest.
		const xImplSimd_DestRegEither LBW;
		// Unpack and interleave low-order words from src and dest into dest.
		const xImplSimd_DestRegEither LWD;
		// Unpack and interleave low-order doublewords from src and dest into dest.
		const xImplSimd_DestRegEither LDQ;
		// Unpack and interleave low-order quadwords from src and dest into dest.
		const xImplSimd_DestRegSSE LQDQ;

		// Unpack and interleave high-order bytes from src and dest into dest.
		const xImplSimd_DestRegEither HBW;
		// Unpack and interleave high-order words from src and dest into dest.
		const xImplSimd_DestRegEither HWD;
		// Unpack and interleave high-order doublewords from src and dest into dest.
		const xImplSimd_DestRegEither HDQ;
		// Unpack and interleave high-order quadwords from src and dest into dest.
		const xImplSimd_DestRegSSE HQDQ;
	};

	// --------------------------------------------------------------------------------------
	//  SimdImpl_Pack
	// --------------------------------------------------------------------------------------
	// Pack with Signed or Unsigned Saturation
	//
	struct SimdImpl_Pack
	{
		// Converts packed signed word integers from src and dest into packed signed
		// byte integers in dest, using signed saturation.
		const xImplSimd_DestRegEither SSWB;

		// Converts packed signed dword integers from src and dest into packed signed
		// word integers in dest, using signed saturation.
		const xImplSimd_DestRegEither SSDW;

		// Converts packed unsigned word integers from src and dest into packed unsigned
		// byte integers in dest, using unsigned saturation.
		const xImplSimd_DestRegEither USWB;

		// [SSE-4.1] Converts packed unsigned dword integers from src and dest into packed
		// unsigned word integers in dest, using signed saturation.
		const xImplSimd_DestRegSSE USDW;
	};

	// --------------------------------------------------------------------------------------
	//  SimdImpl_Unpack
	// --------------------------------------------------------------------------------------
	struct xImplSimd_Unpack
	{
		// Unpacks the high doubleword [single-precision] values from src and dest into
		// dest, such that the result of dest looks like this:
		//    dest[0] <- dest[2]
		//    dest[1] <- src[2]
		//    dest[2] <- dest[3]
		//    dest[3] <- src[3]
		//
		const xImplSimd_DestRegSSE HPS;

		// Unpacks the high quadword [double-precision] values from src and dest into
		// dest, such that the result of dest looks like this:
		//    dest.lo <- dest.hi
		//    dest.hi <- src.hi
		//
		const xImplSimd_DestRegSSE HPD;

		// Unpacks the low doubleword [single-precision] values from src and dest into
		// dest, such that the result of dest looks like this:
		//    dest[3] <- src[1]
		//    dest[2] <- dest[1]
		//    dest[1] <- src[0]
		//    dest[0] <- dest[0]
		//
		const xImplSimd_DestRegSSE LPS;

		// Unpacks the low quadword [double-precision] values from src and dest into
		// dest, effectively moving the low portion of src into the upper portion of dest.
		// The result of dest is loaded as such:
		//    dest.hi <- src.lo
		//    dest.lo <- dest.lo  [remains unchanged!]
		//
		const xImplSimd_DestRegSSE LPD;
	};


	// --------------------------------------------------------------------------------------
	//  SimdImpl_PInsert
	// --------------------------------------------------------------------------------------
	// PINSRW/B/D [all but Word form are SSE4.1 only!]
	//
	struct xImplSimd_PInsert
	{
		void B(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const;
		void B(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const;

		void W(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const;
		void W(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const;

		void D(const xRegisterSSE& to, const xRegister32& from, u8 imm8) const;
		void D(const xRegisterSSE& to, const xIndirect32& from, u8 imm8) const;

		void Q(const xRegisterSSE& to, const xRegister64& from, u8 imm8) const;
		void Q(const xRegisterSSE& to, const xIndirect64& from, u8 imm8) const;
	};

	//////////////////////////////////////////////////////////////////////////////////////////
	// PEXTRW/B/D [all but Word form are SSE4.1 only!]
	//
	// Note: Word form's indirect memory form is only available in SSE4.1.
	//
	struct SimdImpl_PExtract
	{
		// [SSE-4.1] Copies the byte element specified by imm8 from src to dest.  The upper bits
		// of dest are zero-extended (cleared).  This can be used to extract any single packed
		// byte value from src into an x86 32 bit register.
		void B(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const;
		void B(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const;

		// Copies the word element specified by imm8 from src to dest.  The upper bits
		// of dest are zero-extended (cleared).  This can be used to extract any single packed
		// word value from src into an x86 32 bit register.
		//
		// [SSE-4.1] Note: Indirect memory forms of this instruction are an SSE-4.1 extension!
		//
		void W(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const;
		void W(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const;

		// [SSE-4.1] Copies the dword element specified by imm8 from src to dest.  This can be
		// used to extract any single packed dword value from src into an x86 32 bit register.
		void D(const xRegister32& to, const xRegisterSSE& from, u8 imm8) const;
		void D(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8) const;

		// Insert a qword integer value from r/m64 into the xmm1 at the destination element specified by imm8.
		void Q(const xRegister64& to, const xRegisterSSE& from, u8 imm8) const;
		void Q(const xIndirect64& dest, const xRegisterSSE& from, u8 imm8) const;
	};

	enum G1Type
	{
		G1Type_ADD = 0,
		G1Type_OR,
		G1Type_ADC,
		G1Type_SBB,
		G1Type_AND,
		G1Type_SUB,
		G1Type_XOR,
		G1Type_CMP
	};

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
	struct xImpl_G1Logic
	{
		G1Type InstType;

		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;

		void operator()(const xIndirectVoid& to, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& from) const;
		void operator()(const xRegisterInt& to, int imm) const;

		void operator()(const xIndirect64orLess& to, int imm) const;

		xImplSimd_DestRegSSE PS; // packed single precision
		xImplSimd_DestRegSSE PD; // packed double precision
	};

	// ------------------------------------------------------------------------
	// This class combines x86 with SSE/SSE2 arithmetic operations (ADD/SUB).
	//
	struct xImpl_G1Arith
	{
		G1Type InstType;

		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;

		void operator()(const xIndirectVoid& to, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& from) const;
		void operator()(const xRegisterInt& to, int imm) const;

		void operator()(const xIndirect64orLess& to, int imm) const;

		xImplSimd_DestRegSSE PS; // packed single precision
		xImplSimd_DestRegSSE PD; // packed double precision
		xImplSimd_DestRegSSE SS; // scalar single precision
		xImplSimd_DestRegSSE SD; // scalar double precision
	};

	// ------------------------------------------------------------------------
	struct xImpl_G1Compare
	{
		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;

		void operator()(const xIndirectVoid& to, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& from) const;
		void operator()(const xRegisterInt& to, int imm) const;

		void operator()(const xIndirect64orLess& to, int imm) const;

		xImplSimd_DestSSE_CmpImm PS;
		xImplSimd_DestSSE_CmpImm PD;
		xImplSimd_DestSSE_CmpImm SS;
		xImplSimd_DestSSE_CmpImm SD;
	};

	enum G2Type
	{
		G2Type_ROL = 0,
		G2Type_ROR,
		G2Type_RCL,
		G2Type_RCR,
		G2Type_SHL,
		G2Type_SHR,
		G2Type_Unused,
		G2Type_SAR
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_Group2
	// --------------------------------------------------------------------------------------
	// Group 2 (shift) instructions have no Sib/ModRM forms.
	// Optimization Note: For Imm forms, we ignore the instruction if the shift count is zero.
	// This is a safe optimization since any zero-value shift does not affect any flags.
	//
	struct xImpl_Group2
	{
		G2Type InstType;

		void operator()(const xRegisterInt& to, const xRegisterCL& from) const;
		void operator()(const xIndirect64orLess& to, const xRegisterCL& from) const;
		void operator()(const xRegisterInt& to, u8 imm) const;
		void operator()(const xIndirect64orLess& to, u8 imm) const;
	};

	enum G3Type
	{
		G3Type_NOT  = 2,
		G3Type_NEG  = 3,
		G3Type_MUL  = 4,
		G3Type_iMUL = 5, // partial implementation, iMul has additional forms in ix86.cpp
		G3Type_DIV  = 6,
		G3Type_iDIV = 7
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_Group3
	// --------------------------------------------------------------------------------------
	struct xImpl_Group3
	{
		G3Type InstType;

		void operator()(const xRegisterInt& from) const;
		void operator()(const xIndirect64orLess& from) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_MulDivBase
	// --------------------------------------------------------------------------------------
	// This class combines x86 and SSE/SSE2 instructions for iMUL and iDIV.
	//
	struct xImpl_MulDivBase
	{
		G3Type InstType;
		u16 OpcodeSSE;

		void operator()(const xRegisterInt& from) const;
		void operator()(const xIndirect64orLess& from) const;

		const xImplSimd_DestRegSSE PS;
		const xImplSimd_DestRegSSE PD;
		const xImplSimd_DestRegSSE SS;
		const xImplSimd_DestRegSSE SD;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_iDiv
	// --------------------------------------------------------------------------------------
	struct xImpl_iDiv
	{
		void operator()(const xRegisterInt& from) const;
		void operator()(const xIndirect64orLess& from) const;

		const xImplSimd_DestRegSSE PS;
		const xImplSimd_DestRegSSE PD;
		const xImplSimd_DestRegSSE SS;
		const xImplSimd_DestRegSSE SD;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_iMul
	// --------------------------------------------------------------------------------------
	//
	struct xImpl_iMul
	{
		void operator()(const xRegisterInt& from) const;
		void operator()(const xIndirect64orLess& from) const;

		// The following iMul-specific forms are valid for 16 and 32 bit register operands only!

		void operator()(const xRegister32& to, const xRegister32& from) const;
		void operator()(const xRegister32& to, const xIndirectVoid& src) const;
		void operator()(const xRegister16& to, const xRegister16& from) const;
		void operator()(const xRegister16& to, const xIndirectVoid& src) const;

		void operator()(const xRegister32& to, const xRegister32& from, s32 imm) const;
		void operator()(const xRegister32& to, const xIndirectVoid& from, s32 imm) const;
		void operator()(const xRegister16& to, const xRegister16& from, s16 imm) const;
		void operator()(const xRegister16& to, const xIndirectVoid& from, s16 imm) const;

		const xImplSimd_DestRegSSE PS;
		const xImplSimd_DestRegSSE PD;
		const xImplSimd_DestRegSSE SS;
		const xImplSimd_DestRegSSE SD;
	};

	// --------------------------------------------------------------------------------------
	//  MovImplAll
	// --------------------------------------------------------------------------------------
	// MOV instruction Implementation, plus many SIMD sub-mov variants.
	//
	struct xImpl_Mov
	{
		xImpl_Mov() {} // Satisfy GCC's whims.

		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;
		void operator()(const xIndirectVoid& dest, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& src) const;
		void operator()(const xIndirect64orLess& dest, intptr_t imm) const;
		void operator()(const xRegisterInt& to, intptr_t imm, bool preserve_flags = false) const;

	};

	// --------------------------------------------------------------------------------------
	//  xImpl_MovImm64
	// --------------------------------------------------------------------------------------
	// Mov with 64-bit immediates (only available on 64-bit platforms)
	//
	struct xImpl_MovImm64
	{
		xImpl_MovImm64() {} // Satisfy GCC's whims.

		void operator()(const xRegister64& to, s64 imm, bool preserve_flags = false) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_CMov
	// --------------------------------------------------------------------------------------
	// CMOVcc !!  [in all of it's disappointing lack-of glory]  .. and ..
	// SETcc !!  [more glory, less lack!]
	//
	// CMOV Disclaimer: Caution!  This instruction can look exciting and cool, until you
	// realize that it cannot load immediate values into registers. -_-
	//
	// I use explicit method declarations here instead of templates, in order to provide
	// *only* 32 and 16 bit register operand forms (8 bit registers are not valid in CMOV).
	//

	struct xImpl_CMov
	{
		JccComparisonType ccType;
		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const;
		void operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const;

		//void operator()( const xDirectOrIndirect32& to, const xDirectOrIndirect32& from );
		//void operator()( const xDirectOrIndirect16& to, const xDirectOrIndirect16& from ) const;
	};

	struct xImpl_Set
	{
		JccComparisonType ccType;

		void operator()(const xRegister8& to) const;
		void operator()(const xIndirect8& dest) const;
	};


	// --------------------------------------------------------------------------------------
	//  xImpl_MovExtend
	// --------------------------------------------------------------------------------------
	// Mov with sign/zero extension implementations (movsx / movzx)
	//
	struct xImpl_MovExtend
	{
		bool SignExtend;

		void operator()(const xRegister16or32or64& to, const xRegister8& from) const;
		void operator()(const xRegister16or32or64& to, const xIndirect8& sibsrc) const;
		void operator()(const xRegister32or64& to, const xRegister16& from) const;
		void operator()(const xRegister32or64& to, const xIndirect16& sibsrc) const;
		void operator()(const xRegister64& to, const xRegister32& from) const;
		void operator()(const xRegister64& to, const xIndirect32& sibsrc) const;
	};

	// Implementations here cover SHLD and SHRD.

	// --------------------------------------------------------------------------------------
	//  xImpl_DowrdShift
	// --------------------------------------------------------------------------------------
	// I use explicit method declarations here instead of templates, in order to provide
	// *only* 32 and 16 bit register operand forms (8 bit registers are not valid in SHLD/SHRD).
	//
	// Optimization Note: Imm shifts by 0 are ignore (no code generated).  This is a safe optimization
	// because shifts by 0 do *not* affect flags status (intel docs cited).
	//
	struct xImpl_DwordShift
	{
		u16 OpcodeBase;

		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, const xRegisterCL& clreg) const;

		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, u8 shiftcnt) const;

		void operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, const xRegisterCL& clreg) const;
		void operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, u8 shiftcnt) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_IncDec
	// --------------------------------------------------------------------------------------
	struct xImpl_IncDec
	{
		bool isDec;

		void operator()(const xRegisterInt& to) const;
		void operator()(const xIndirect64orLess& to) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_Test
	// --------------------------------------------------------------------------------------
	//
	struct xImpl_Test
	{
		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;
		void operator()(const xIndirect64orLess& dest, int imm) const;
		void operator()(const xRegisterInt& to, int imm) const;
	};

	enum G8Type
	{
		G8Type_BT = 4,
		G8Type_BTS,
		G8Type_BTR,
		G8Type_BTC,
	};

	// --------------------------------------------------------------------------------------
	//  BSF / BSR
	// --------------------------------------------------------------------------------------
	// 16/32 operands are available.  No 8 bit ones, not that any of you cared, I bet.
	//
	struct xImpl_BitScan
	{
		// 0xbc [fwd] / 0xbd [rev]
		u16 Opcode;

		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const;
		void operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const;
	};

	// --------------------------------------------------------------------------------------
	//  xImpl_Group8
	// --------------------------------------------------------------------------------------
	// Bit Test Instructions - Valid on 16/32 bit instructions only.
	//
	struct xImpl_Group8
	{
		G8Type InstType;

		void operator()(const xRegister16or32or64& bitbase, const xRegister16or32or64& bitoffset) const;
		void operator()(const xRegister16or32or64& bitbase, u8 bitoffset) const;

		void operator()(const xIndirectVoid& bitbase, const xRegister16or32or64& bitoffset) const;

		void operator()(const xIndirect64& bitbase, u8 bitoffset) const;
		void operator()(const xIndirect32& bitbase, u8 bitoffset) const;
		void operator()(const xIndirect16& bitbase, u8 bitoffset) const;
	};

	extern void xJccKnownTarget(JccComparisonType comparison, const void* target);

	// ------------------------------------------------------------------------
	struct xImpl_JmpCall
	{
		bool isJmp;

		void operator()(const xAddressReg& absreg) const;
		void operator()(const xIndirectNative& src) const;

		// Special form for calling functions.  This form automatically resolves the
		// correct displacement based on the size of the instruction being generated.
		void operator()(const void* func) const
		{
			if (isJmp)
				xJccKnownTarget(Jcc_Unconditional, (const void*)(uintptr_t)func); // double cast to/from (uintptr_t) needed to appease GCC
			else
			{
				// calls are relative to the instruction after this one, and length is
				// always 5 bytes (16 bit calls are bad mojo, so no bother to do special logic).

				intptr_t dest = (intptr_t)func - ((intptr_t)x86Ptr + 5);
				*(u8*)x86Ptr = 0xe8;
				x86Ptr += sizeof(u8);
				*(u32*)x86Ptr = dest;
				x86Ptr += sizeof(u32);
			}
		}
	};

	// yes it is awful. Due to template code is in a header with a nice circular dep.
	extern const xImpl_Mov xMOV;
	extern const xImpl_JmpCall xCALL;

	struct xImpl_FastCall
	{
		// FIXME: current 64 bits is mostly a copy/past potentially it would require to push/pop
		// some registers. But I think it is enough to handle the first call.

		void operator()(const void* f, const xRegister32& a1 = xEmptyReg, const xRegister32& a2 = xEmptyReg) const;

		void operator()(const void* f, u32 a1, const xRegister32& a2) const;
		void operator()(const void* f, const xIndirect32& a1) const;
		void operator()(const void* f, u32 a1, u32 a2) const;
		void operator()(const void* f, void* a1) const;

		void operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2 = xEmptyReg) const;
		void operator()(const void* f, u32 a1, const xRegisterLong& a2) const;

		template <typename T>
			__fi void operator()(T* func, u32 a1, const xRegisterLong& a2 = xEmptyReg) const
			{
				(*this)((const void*)func, a1, a2);
			}

		template <typename T>
			__fi void operator()(T* func, const xIndirect32& a1) const
			{
				(*this)((const void*)func, a1);
			}

		template <typename T>
			__fi void operator()(T* func, u32 a1, u32 a2) const
			{
				(*this)((const void*)func, a1, a2);
			}

		void operator()(const xIndirectNative& f, const xRegisterLong& a1 = xEmptyReg, const xRegisterLong& a2 = xEmptyReg) const;
	};

	struct xImplAVX_Move
	{
		u8 Prefix;
		u8 LoadOpcode;
		u8 StoreOpcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const;
	};

	struct xImplAVX_ThreeArg
	{
		u8 Prefix;
		u8 Opcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
	};

	struct xImplAVX_ThreeArgYMM : xImplAVX_ThreeArg
	{
		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
	};

	struct xImplAVX_ArithFloat
	{
		xImplAVX_ThreeArgYMM PS;
		xImplAVX_ThreeArgYMM PD;
		xImplAVX_ThreeArg SS;
		xImplAVX_ThreeArg SD;
	};

	struct xImplAVX_CmpFloatHelper
	{
		SSE2_ComparisonType CType;

		void PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
		void PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;

		void SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
		void SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
	};

	struct xImplAVX_CmpFloat
	{
		xImplAVX_CmpFloatHelper EQ;
		xImplAVX_CmpFloatHelper LT;
		xImplAVX_CmpFloatHelper LE;
		xImplAVX_CmpFloatHelper UO;
		xImplAVX_CmpFloatHelper NE;
		xImplAVX_CmpFloatHelper GE;
		xImplAVX_CmpFloatHelper GT;
		xImplAVX_CmpFloatHelper OR;
	};

	struct xImplAVX_CmpInt
	{
		// Compare packed bytes for equality.
		// If a data element in dest is equal to the corresponding date element src, the
		// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
		const xImplAVX_ThreeArgYMM EQB;

		// Compare packed words for equality.
		// If a data element in dest is equal to the corresponding date element src, the
		// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
		const xImplAVX_ThreeArgYMM EQW;

		// Compare packed doublewords [32-bits] for equality.
		// If a data element in dest is equal to the corresponding date element src, the
		// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
		const xImplAVX_ThreeArgYMM EQD;

		// Compare packed signed bytes for greater than.
		// If a data element in dest is greater than the corresponding date element src, the
		// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
		const xImplAVX_ThreeArgYMM GTB;

		// Compare packed signed words for greater than.
		// If a data element in dest is greater than the corresponding date element src, the
		// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
		const xImplAVX_ThreeArgYMM GTW;

		// Compare packed signed doublewords [32-bits] for greater than.
		// If a data element in dest is greater than the corresponding date element src, the
		// corresponding data element in dest is set to all 1s; otherwise, it is set to all 0s.
		const xImplAVX_ThreeArgYMM GTD;
	};

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

	template <typename T1, typename T2>
	__fi void xOpWrite(u8 prefix, u8 opcode, const T1& param1, const T2& param2, int extraRIPOffset)
	{
		if (prefix != 0)
		{
			*(u8*)x86Ptr = prefix;
			x86Ptr += sizeof(u8);
		}
		EmitRex(param1, param2);

		*(u8*)x86Ptr = opcode;
		x86Ptr += sizeof(u8);

		EmitSibMagic(param1, param2, extraRIPOffset);
	}

	template <typename T1, typename T2>
	__fi void xOpAccWrite(u8 prefix, u8 opcode, const T1& param1, const T2& param2)
	{
		if (prefix != 0)
		{
			*(u8*)x86Ptr = prefix;
			x86Ptr += sizeof(u8);
		}
		EmitRex(param1, param2);

		*(u8*)x86Ptr = opcode;
		x86Ptr += sizeof(u8);
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
		{
			*(u8*)x86Ptr = prefix;
			x86Ptr += sizeof(u8);
		}
		EmitRex(param1, param2);

		const bool is16BitOpcode = ((opcode & 0xff) == 0x38) || ((opcode & 0xff) == 0x3a);

		// ------------------------------------------------------------------------
		// If the lower byte of the opcode is 0x38 or 0x3a, then the opcode is
		// treated as a 16 bit value (in SSE 0x38 and 0x3a denote prefixes for extended SSE3/4
		// instructions).  Any other lower value assumes the upper value is 0 and ignored.
		// Non-zero upper bytes, when the lower byte is not the 0x38 or 0x3a prefix, will
		// generate an assertion.
		if (is16BitOpcode)
		{
			*(u8*)x86Ptr = 0x0f;
			x86Ptr += sizeof(u8);
			*(u16*)x86Ptr = opcode;
		}
		else
			*(u16*)x86Ptr = (opcode << 8) | 0x0f;
		x86Ptr += sizeof(u16);

		EmitSibMagic(param1, param2);
	}

	template <typename T1, typename T2>
	__fi void xOpWrite0F(u8 prefix, u16 opcode, const T1& param1, const T2& param2, u8 imm8)
	{
		if (prefix != 0)
		{
			*(u8*)x86Ptr = prefix;
			x86Ptr += sizeof(u8);
		}
		EmitRex(param1, param2);

		const bool is16BitOpcode = ((opcode & 0xff) == 0x38) || ((opcode & 0xff) == 0x3a);

		// ------------------------------------------------------------------------
		// If the lower byte of the opcode is 0x38 or 0x3a, then the opcode is
		// treated as a 16 bit value (in SSE 0x38 and 0x3a denote prefixes for extended SSE3/4
		// instructions).  Any other lower value assumes the upper value is 0 and ignored.
		// Non-zero upper bytes, when the lower byte is not the 0x38 or 0x3a prefix, will
		// generate an assertion.
		if (is16BitOpcode)
		{
			*(u8*)x86Ptr = 0x0f;
			x86Ptr += sizeof(u8);
			*(u16*)x86Ptr = opcode;
		}
		else
			*(u16*)x86Ptr = (opcode << 8) | 0x0f;
		x86Ptr += sizeof(u16);

		EmitSibMagic(param1, param2, 1);
		*(u8*)x86Ptr = imm8;
		x86Ptr += sizeof(u8);
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

		*(u8*)x86Ptr = 0xC5;
		x86Ptr += sizeof(u8);
		*(u8*)x86Ptr = nR | nv | L | p;
		x86Ptr += sizeof(u8);
		*(u8*)x86Ptr = opcode;
		x86Ptr += sizeof(u8);
		EmitSibMagic(param1, param3);
	}

	// ------------------------------------------------------------------------
	// Group 1 Instruction Class

	extern const xImpl_Group1 xADC;
	extern const xImpl_Group1 xSBB;

	extern const xImpl_G1Logic xAND;
	extern const xImpl_G1Logic xOR;
	extern const xImpl_G1Logic xXOR;

	extern const xImpl_G1Arith xADD;
	extern const xImpl_G1Arith xSUB;
	extern const xImpl_G1Compare xCMP;

	// ------------------------------------------------------------------------
	// Group 2 Instruction Class
	//
	// Optimization Note: For Imm forms, we ignore the instruction if the shift count is
	// zero.  This is a safe optimization since any zero-value shift does not affect any
	// flags.

	extern const xImpl_Mov xMOV;
	extern const xImpl_MovImm64 xMOV64;
	extern const xImpl_Test xTEST;
	extern const xImpl_Group2 xROL, xROR,
		xRCL, xRCR,
		xSHL, xSHR,
		xSAR;

	// ------------------------------------------------------------------------
	// Group 3 Instruction Class

	extern const xImpl_Group3 xNOT, xNEG;
	extern const xImpl_Group3 xUMUL, xUDIV;
	extern const xImpl_iDiv xDIV;
	extern const xImpl_iMul xMUL;

	extern const xImpl_IncDec xINC, xDEC;

	extern const xImpl_MovExtend xMOVZX, xMOVSX;

	extern const xImpl_DwordShift xSHLD, xSHRD;

	extern const xImpl_Group8 xBT;
	extern const xImpl_Group8 xBTR;
	extern const xImpl_Group8 xBTS;
	extern const xImpl_Group8 xBTC;

	extern const xImpl_BitScan xBSF, xBSR;

	extern const xImpl_JmpCall xJMP;
	extern const xImpl_JmpCall xCALL;
	extern const xImpl_FastCall xFastCall;

	// ------------------------------------------------------------------------
	extern const xImpl_CMov xCMOVA, xCMOVAE,
		xCMOVB, xCMOVBE,
		xCMOVG, xCMOVGE,
		xCMOVL, xCMOVLE,

		xCMOVZ, xCMOVE,
		xCMOVNZ, xCMOVNE,
		xCMOVO, xCMOVNO,
		xCMOVC, xCMOVNC,

		xCMOVS, xCMOVNS,
		xCMOVPE, xCMOVPO;

	// ------------------------------------------------------------------------
	extern const xImpl_Set xSETA, xSETAE,
		xSETB, xSETBE,
		xSETG, xSETGE,
		xSETL, xSETLE,

		xSETZ, xSETE,
		xSETNZ, xSETNE,
		xSETO, xSETNO,
		xSETC, xSETNC,

		xSETS, xSETNS,
		xSETPE, xSETPO;

	//////////////////////////////////////////////////////////////////////////////////////////
	// Miscellaneous Instructions
	// These are all defined inline or in ix86.cpp.
	//

	// ----- Lea Instructions (Load Effective Address) -----
	// Note: alternate (void*) forms of these instructions are not provided since those
	// forms are functionally equivalent to Mov reg,imm, and thus better written as MOVs
	// instead.

	extern void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags = false);
	extern void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags = false);
	extern void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags = false);

	// ----- Push / Pop Instructions  -----
	// Note: pushad/popad implementations are intentionally left out.  The instructions are
	// invalid in x64, and are super slow on x32.  Use multiple Push/Pop instructions instead.

	extern void xPOP(const xIndirectVoid& from);
	extern void xPUSH(const xIndirectVoid& from);

	extern void xPOP(xRegister32or64 from);

	extern void xPUSH(u32 imm);
	extern void xPUSH(xRegister32or64 from);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to calculate base+offset taking into account the limitations of x86-64's RIP-relative addressing
	/// (Will either return `base+offset` or LEA `base` into `tmpRegister` and return `tmpRegister+offset`)
	xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to load addresses that may be far from the current instruction pointer
	/// On i386, resolves to `mov dst, (intptr_t)addr`
	/// On x86-64, resolves to either `mov dst, (inptr_t)addr` or `lea dst, [addr]` depending on the distance from RIP
	void xLoadFarAddr(const xAddressReg& dst, void* addr);

	//////////////////////////////////////////////////////////////////////////////////////////
	/// Helper function to run operations with large immediates
	/// If the immediate fits in 32 bits, runs op(target, imm)
	/// Otherwise, loads imm into tmpRegister and then runs op(dst, tmp)
	template <typename Op, typename Dst>
	void xImm64Op(const Op& op, const Dst& dst, const xRegister64& tmpRegister, s64 imm)
	{
		if (imm == (s32)imm)
		{
			op(dst, imm);
		}
		else
		{
			xMOV64(tmpRegister, imm);
			op(dst, tmpRegister);
		}
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// JMP / Jcc Instructions!

	extern void xJccKnownTarget(JccComparisonType comparison, const void* target);
	extern s32* xJcc32(JccComparisonType comparison, s32 displacement);

// ------------------------------------------------------------------------
// Forward Jump Helpers (act as labels!)

#define DEFINE_FORWARD_JUMP(label, cond) \
	template <typename OperandType> \
	class xForward##label : public xForwardJump<OperandType> \
	{ \
	public: \
		xForward##label() \
			: xForwardJump<OperandType>(cond) { }\
	};

	// ------------------------------------------------------------------------
	// Note: typedefs below  are defined individually in order to appease Intellisense
	// resolution.  Including them into the class definition macro above breaks it.

	typedef xForwardJump<s8> xForwardJump8;
	typedef xForwardJump<s32> xForwardJump32;

	DEFINE_FORWARD_JUMP(JA, Jcc_Above);
	DEFINE_FORWARD_JUMP(JB, Jcc_Below);
	DEFINE_FORWARD_JUMP(JAE, Jcc_AboveOrEqual);
	DEFINE_FORWARD_JUMP(JBE, Jcc_BelowOrEqual);

	typedef xForwardJA<s8> xForwardJA8;
	typedef xForwardJA<s32> xForwardJA32;
	typedef xForwardJB<s8> xForwardJB8;
	typedef xForwardJB<s32> xForwardJB32;
	typedef xForwardJAE<s8> xForwardJAE8;
	typedef xForwardJAE<s32> xForwardJAE32;
	typedef xForwardJBE<s8> xForwardJBE8;
	typedef xForwardJBE<s32> xForwardJBE32;

	DEFINE_FORWARD_JUMP(JG, Jcc_Greater);
	DEFINE_FORWARD_JUMP(JL, Jcc_Less);
	DEFINE_FORWARD_JUMP(JGE, Jcc_GreaterOrEqual);
	DEFINE_FORWARD_JUMP(JLE, Jcc_LessOrEqual);

	typedef xForwardJG<s8> xForwardJG8;
	typedef xForwardJG<s32> xForwardJG32;
	typedef xForwardJL<s8> xForwardJL8;
	typedef xForwardJL<s32> xForwardJL32;
	typedef xForwardJGE<s8> xForwardJGE8;
	typedef xForwardJGE<s32> xForwardJGE32;
	typedef xForwardJLE<s8> xForwardJLE8;
	typedef xForwardJLE<s32> xForwardJLE32;

	DEFINE_FORWARD_JUMP(JZ, Jcc_Zero);
	DEFINE_FORWARD_JUMP(JE, Jcc_Equal);
	DEFINE_FORWARD_JUMP(JNZ, Jcc_NotZero);
	DEFINE_FORWARD_JUMP(JNE, Jcc_NotEqual);

	typedef xForwardJZ<s8> xForwardJZ8;
	typedef xForwardJZ<s32> xForwardJZ32;
	typedef xForwardJE<s8> xForwardJE8;
	typedef xForwardJE<s32> xForwardJE32;
	typedef xForwardJNZ<s8> xForwardJNZ8;
	typedef xForwardJNZ<s32> xForwardJNZ32;
	typedef xForwardJNE<s8> xForwardJNE8;
	typedef xForwardJNE<s32> xForwardJNE32;

	DEFINE_FORWARD_JUMP(JS, Jcc_Signed);
	DEFINE_FORWARD_JUMP(JNS, Jcc_Unsigned);

	typedef xForwardJS<s8> xForwardJS8;
	typedef xForwardJS<s32> xForwardJS32;
	typedef xForwardJNS<s8> xForwardJNS8;
	typedef xForwardJNS<s32> xForwardJNS32;

	DEFINE_FORWARD_JUMP(JO, Jcc_Overflow);
	DEFINE_FORWARD_JUMP(JNO, Jcc_NotOverflow);

	typedef xForwardJO<s8> xForwardJO8;
	typedef xForwardJO<s32> xForwardJO32;
	typedef xForwardJNO<s8> xForwardJNO8;
	typedef xForwardJNO<s32> xForwardJNO32;

	DEFINE_FORWARD_JUMP(JC, Jcc_Carry);
	DEFINE_FORWARD_JUMP(JNC, Jcc_NotCarry);

	typedef xForwardJC<s8> xForwardJC8;
	typedef xForwardJC<s32> xForwardJC32;
	typedef xForwardJNC<s8> xForwardJNC8;
	typedef xForwardJNC<s32> xForwardJNC32;

	DEFINE_FORWARD_JUMP(JPE, Jcc_ParityEven);
	DEFINE_FORWARD_JUMP(JPO, Jcc_ParityOdd);

	typedef xForwardJPE<s8> xForwardJPE8;
	typedef xForwardJPE<s32> xForwardJPE32;
	typedef xForwardJPO<s8> xForwardJPO8;
	typedef xForwardJPO<s32> xForwardJPO32;

	// ------------------------------------------------------------------------
	extern void xMOVSS(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xMOVSS(const xIndirectVoid& to, const xRegisterSSE& from);
	extern void xMOVSD(const xRegisterSSE& to, const xRegisterSSE& from);
	extern void xMOVSD(const xIndirectVoid& to, const xRegisterSSE& from);

	extern void xMOVSSZX(const xRegisterSSE& to, const xIndirectVoid& from);
	extern void xMOVSDZX(const xRegisterSSE& to, const xIndirectVoid& from);

	// ------------------------------------------------------------------------

	extern const xImplSimd_MoveSSE xMOVAPS;
	extern const xImplSimd_MoveSSE xMOVUPS;
	extern const xImplSimd_MoveSSE xMOVAPD;
	extern const xImplSimd_MoveSSE xMOVUPD;

#ifdef ALWAYS_USE_MOVAPS
	extern const xImplSimd_MoveSSE xMOVDQA;
	extern const xImplSimd_MoveSSE xMOVDQU;
#else
	extern const xImplSimd_MoveDQ xMOVDQA;
	extern const xImplSimd_MoveDQ xMOVDQU;
#endif

	extern const xImplSimd_MovHL xMOVH;
	extern const xImplSimd_MovHL xMOVL;
	extern const xImplSimd_MovHL_RtoR xMOVLH;
	extern const xImplSimd_MovHL_RtoR xMOVHL;

	extern const xImplSimd_Blend xBLEND;
	extern const xImplSimd_PMove xPMOVSX;
	extern const xImplSimd_PMove xPMOVZX;

	extern const xImplSimd_DestRegSSE xMOVSLDUP;
	extern const xImplSimd_DestRegSSE xMOVSHDUP;

	extern void xINSERTPS(const xRegisterSSE& to, const xRegisterSSE& from, u8 imm8);
	extern void xINSERTPS(const xRegisterSSE& to, const xIndirect32& from, u8 imm8);

	extern void xEXTRACTPS(const xRegister32or64& to, const xRegisterSSE& from, u8 imm8);
	extern void xEXTRACTPS(const xIndirect32& dest, const xRegisterSSE& from, u8 imm8);

	// ------------------------------------------------------------------------

	extern const xImplSimd_DestRegEither xPAND;
	extern const xImplSimd_DestRegEither xPANDN;
	extern const xImplSimd_DestRegEither xPOR;
	extern const xImplSimd_DestRegEither xPXOR;

	extern const xImplSimd_Shuffle xSHUF;

	// ------------------------------------------------------------------------

	extern const xImplSimd_DestRegSSE xPTEST;

	extern const xImplSimd_MinMax xMIN;
	extern const xImplSimd_MinMax xMAX;

	extern const xImplSimd_Compare xCMPEQ, xCMPLT,
		xCMPLE, xCMPUNORD,
		xCMPNE, xCMPNLT,
		xCMPNLE, xCMPORD;

	extern const xImplSimd_COMI xCOMI;
	extern const xImplSimd_COMI xUCOMI;

	extern const xImplSimd_PCompare xPCMP;
	extern const xImplSimd_PMinMax xPMIN;
	extern const xImplSimd_PMinMax xPMAX;

	// ------------------------------------------------------------------------

	extern const xImplSimd_AndNot xANDN;
	extern const xImplSimd_rSqrt xRCP;
	extern const xImplSimd_rSqrt xRSQRT;
	extern const xImplSimd_Sqrt xSQRT;

	extern const xImplSimd_Shift xPSLL;
	extern const xImplSimd_Shift xPSRL;
	extern const xImplSimd_ShiftWithoutQ xPSRA;
	extern const xImplSimd_AddSub xPADD;
	extern const xImplSimd_AddSub xPSUB;
	extern const xImplSimd_PMul xPMUL;
	extern const xImplSimd_PAbsolute xPABS;
	extern const xImplSimd_PSign xPSIGN;
	extern const xImplSimd_PMultAdd xPMADD;
	extern const xImplSimd_HorizAdd xHADD;
	extern const xImplSimd_DotProduct xDP;
	extern const xImplSimd_Round xROUND;

	extern const xImplSimd_PShuffle xPSHUF;
	extern const SimdImpl_PUnpack xPUNPCK;
	extern const xImplSimd_Unpack xUNPCK;
	extern const SimdImpl_Pack xPACK;
	extern const xImplSimd_PInsert xPINSR;
	extern const SimdImpl_PExtract xPEXTR;

	// ------------------------------------------------------------------------

	extern const xImplAVX_Move xVMOVAPS;
	extern const xImplAVX_Move xVMOVUPS;
	extern const xImplAVX_ArithFloat xVADD;
	extern const xImplAVX_ArithFloat xVSUB;
	extern const xImplAVX_ArithFloat xVMUL;
	extern const xImplAVX_ArithFloat xVDIV;
	extern const xImplAVX_CmpFloat xVCMP;
	extern const xImplAVX_ThreeArgYMM xVPAND;
	extern const xImplAVX_ThreeArgYMM xVPANDN;
	extern const xImplAVX_ThreeArgYMM xVPOR;
	extern const xImplAVX_ThreeArgYMM xVPXOR;
	extern const xImplAVX_CmpInt xVPCMP;
}

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

//////////////////////////////////////////////////////////////////////////////////////////
// Helper object to handle ABI frame
// All x86-64 calling conventions ensure/require stack to be 16 bytes aligned
// I couldn't find documentation on when, but compilers would indicate it's before the call: https://gcc.godbolt.org/z/KzTfsz

#ifdef _WIN32
#define SCOPED_STACK_FRAME_BEGIN(m_offset) \
	(m_offset) = sizeof(void*); \
	xPUSH(rbp); \
	(m_offset) += sizeof(void*); \
	xPUSH(rbx); \
	xPUSH(r12); \
	xPUSH(r13); \
	xPUSH(r14); \
	xPUSH(r15); \
	m_offset += 40; \
	xPUSH(rdi); \
	xPUSH(rsi); \
	xSUB(rsp, 32); \
	m_offset += 48; \
	xADD(rsp, (-((16 - ((m_offset) % 16)) % 16)))

#define SCOPED_STACK_FRAME_END(m_offset) \
	xADD(rsp, ((16 - ((m_offset) % 16)) % 16)); \
	xADD(rsp, 32); \
	xPOP(rsi); \
	xPOP(rdi); \
	xPOP(r15); \
	xPOP(r14); \
	xPOP(r13); \
	xPOP(r12); \
	xPOP(rbx); \
	xPOP(rbp)
#else
#define SCOPED_STACK_FRAME_BEGIN(m_offset) \
	(m_offset) = sizeof(void*); \
	xPUSH(rbp); \
	(m_offset) += sizeof(void*); \
	xPUSH(rbx); \
	xPUSH(r12); \
	xPUSH(r13); \
	xPUSH(r14); \
	xPUSH(r15); \
	m_offset += 40; \
	xADD(rsp, (-((16 - ((m_offset) % 16)) % 16)))

#define SCOPED_STACK_FRAME_END(m_offset) \
	xADD(rsp, ((16 - ((m_offset) % 16)) % 16)); \
	xPOP(r15); \
	xPOP(r14); \
	xPOP(r13); \
	xPOP(r12); \
	xPOP(rbx); \
	xPOP(rbp)
#endif
