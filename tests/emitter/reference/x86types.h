/*  The x86 emitter's object world: register classes, address objects, the
 *  operator algebra, and the forward-jump machinery. This is the REFERENCE
 *  emitter's type system, used by the byte suites as the oracle the C89
 *  macros are verified against. The product tree compiles none of this;
 *  common/emitter carries only the C89 core.
 */
#pragma once
#include "common/emitter/x86types.h"

namespace x86Emitter
{
	//------------------------------------------------------------------
	// templated version of is_s8 is required, so that u16's get correct sign extension treatment.
	template <typename T>
	static __fi bool is_s8(T imm)
	{
		return (s8)imm == (typename std::make_signed<T>::type)imm;
	}

/* ============================ THE OBJECT WORLD ============================
 * Everything below -- the register class hierarchy, the singleton tables,
 * the address objects and their operator algebra -- exists for the
 * reference emitter and the byte suites. The switched build emits through
 * the C89 macros and needs none of it; the reference build and byte suites
 * keep it via PCSX2_C89_KEEP_TYPES. */

	class xAddressVoid;

	// --------------------------------------------------------------------------------------
	//  OperandSizedObject
	// --------------------------------------------------------------------------------------
	class OperandSizedObject
	{
	public:
		uint _operandSize = 0;
		OperandSizedObject() = default;
		constexpr OperandSizedObject(uint operandSize)
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
			switch (GetImmSize())
			{
				case 1:
					xWrite8(imm);
					break;
				case 2:
					xWrite16(imm);
					break;
				case 4:
					xWrite32(imm);
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
		constexpr xRegisterBase(uint operandSize, int regId)
			: OperandSizedObject(operandSize)
			, Id(regId)
		{
			// Note: to avoid tons of ifdef, the 32 bits build will instantiate
			// all 16x64 bits registers.
		}

	public:
		int Id;

		constexpr xRegisterBase()
			: OperandSizedObject(0)
			, Id(-2)
		{
		}

		bool IsEmpty() const { return Id < 0; }
		bool IsExtended() const { return (Id >= 0 && (Id & 0x0F) > 7); } // Register 8-15 need an extra bit to be selected
		bool IsReg() const { return true; }

		/// Returns true if the specified register is caller-saved (volatile).
		static inline bool IsCallerSaved(uint id);
	};

	class xRegisterInt : public xRegisterBase
	{
		typedef xRegisterBase _parent;

	protected:
		explicit constexpr xRegisterInt(uint operandSize, int regId)
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
		explicit constexpr xRegister8(int regId)
			: _parent(1, regId)
		{
		}
		explicit xRegister8(const xRegisterInt& other)
			: _parent(1, other.Id)
		{
			if (!other.canMapIDTo(1))
				Id |= 0x10;
		}
		constexpr xRegister8(int regId, bool ext8bit)
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
		explicit constexpr xRegister16(int regId)
			: _parent(2, regId)
		{
		}
		explicit xRegister16(const xRegisterInt& other)
			: _parent(2, other.Id)
		{
		}

		bool operator==(const xRegister16& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister16& src) const { return this->Id != src.Id; }
	};

	class xRegister32 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

	public:
		xRegister32() = default;
		explicit constexpr xRegister32(int regId)
			: _parent(4, regId)
		{
		}
		explicit xRegister32(const xRegisterInt& other)
			: _parent(4, other.Id)
		{
		}

		static const inline xRegister32& GetInstance(uint id);

		bool operator==(const xRegister32& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister32& src) const { return this->Id != src.Id; }
	};

	class xRegister64 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

	public:
		xRegister64() = default;
		explicit constexpr xRegister64(int regId)
			: _parent(8, regId)
		{
		}
		explicit xRegister64(const xRegisterInt& other)
			: _parent(8, other.Id)
		{
		}

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
		explicit constexpr xRegisterSSE(int regId)
			: _parent(16, regId)
		{
		}
		constexpr xRegisterSSE(int regId, xRegisterYMMTag)
			: _parent(32, regId)
		{
		}

		bool operator==(const xRegisterSSE& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegisterSSE& src) const { return this->Id != src.Id; }

		static const inline xRegisterSSE& GetInstance(uint id);
		static const inline xRegisterSSE& GetYMMInstance(uint id);

		/// Returns the register to use when calling a C function.
		/// arg_number is the argument position from the left, starting with 0.
		/// sse_number is the argument position relative to the number of vector registers.
		static const inline xRegisterSSE& GetArgRegister(uint arg_number, uint sse_number, bool ymm = false);

		/// Returns true if the specified register is caller-saved (volatile).
		static inline bool IsCallerSaved(uint id);
	};

	class xRegisterCL : public xRegister8
	{
	public:
		constexpr xRegisterCL()
			: xRegister8(1)
		{
		}
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
	static const int wordsize = sizeof(sptr);

	class xAddressReg : public xRegisterLong
	{
	public:
		xAddressReg() = default;
		explicit xAddressReg(xRegisterInt other)
			: xRegisterLong(other)
		{
		}
		explicit constexpr xAddressReg(int regId)
			: xRegisterLong(regId)
		{
		}

		/// Returns the register to use when calling a C function.
		/// arg_number is the argument position from the left, starting with 0.
		/// sse_number is the argument position relative to the number of vector registers.
		static const inline xAddressReg& GetArgRegister(uint arg_number, uint gpr_number);

		xAddressVoid operator+(const xAddressReg& right) const;
		xAddressVoid operator+(sptr right) const;
		xAddressVoid operator+(const void* right) const;
		xAddressVoid operator-(sptr right) const;
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
			return xRegister8(-1);
		}

		operator xRegister16() const
		{
			return xRegister16(-1);
		}

		operator xRegister32() const
		{
			return xRegister32(-1);
		}

		operator xRegisterSSE() const
		{
			return xRegisterSSE(-1);
		}

		operator xAddressReg() const
		{
			return xAddressReg(-1);
		}
	};

	class xRegister16or32or64
	{
	protected:
		const xRegisterInt& m_convtype;

	public:
		xRegister16or32or64(const xRegister64& src)
			: m_convtype(src)
		{
		}
		xRegister16or32or64(const xRegister32& src)
			: m_convtype(src)
		{
		}
		xRegister16or32or64(const xRegister16& src)
			: m_convtype(src)
		{
		}

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
		xRegister32or64(const xRegister64& src)
			: m_convtype(src)
		{
		}
		xRegister32or64(const xRegister32& src)
			: m_convtype(src)
		{
		}

		operator const xRegisterBase&() const { return m_convtype; }

		const xRegisterInt* operator->() const
		{
			return &m_convtype;
		}
	};

	inline constexpr xRegisterEmpty xEmptyReg = {};

	// clang-format off
	// Register objects. C++17 inline variables with constexpr constructors:
	// compile-time constants in every translation unit, with no out-of-line
	// definition in x86emitter.cpp and no static-initialisation order to
	// reason about.
	inline constexpr xRegisterSSE
    xmm0(0), xmm1(1), xmm2(2), xmm3(3),
    xmm4(4), xmm5(5), xmm6(6), xmm7(7),
    xmm8(8), xmm9(9), xmm10(10), xmm11(11),
    xmm12(12), xmm13(13), xmm14(14), xmm15(15);

	// TODO: This needs to be _M_SSE >= 0x500'ed, but we can't do it atm because common doesn't have variants.
	inline constexpr xRegisterSSE
	  ymm0(0, xRegisterYMMTag()), ymm1(1, xRegisterYMMTag()),
	  ymm2(2, xRegisterYMMTag()), ymm3(3, xRegisterYMMTag()),
	  ymm4(4, xRegisterYMMTag()), ymm5(5, xRegisterYMMTag()),
	  ymm6(6, xRegisterYMMTag()), ymm7(7, xRegisterYMMTag()),
	  ymm8(8, xRegisterYMMTag()), ymm9(9, xRegisterYMMTag()),
	  ymm10(10, xRegisterYMMTag()), ymm11(11, xRegisterYMMTag()),
	  ymm12(12, xRegisterYMMTag()), ymm13(13, xRegisterYMMTag()),
	  ymm14(14, xRegisterYMMTag()), ymm15(15, xRegisterYMMTag());

inline constexpr xAddressReg
    rax(0), rbx(3), rcx(1), rdx(2),
    rsi(6), rdi(7), rbp(5), rsp(4),
    r8(8), r9(9), r10(10), r11(11),
    r12(12), r13(13), r14(14), r15(15);

inline constexpr xRegister32
     eax(0),  ebx(3),  ecx(1),  edx(2),
     esi(6),  edi(7),  ebp(5),  esp(4),
     r8d(8),  r9d(9), r10d(10), r11d(11),
    r12d(12), r13d(13), r14d(14), r15d(15);

inline constexpr xRegister16
    ax(0), bx(3), cx(1), dx(2),
    si(6), di(7), bp(5), sp(4);

	inline constexpr xRegister8
    al(0),
    dl(2), bl(3),
    ah(4), ch(5),
    dh(6), bh(7),
    spl(4, true), bpl(5, true),
    sil(6, true), dil(7, true),
    r8b(8), r9b(9),
    r10b(10), r11b(11),
    r12b(12), r13b(13),
    r14b(14), r15b(15);

// ABI argument registers. The platform conditional moves here with the
// definitions; the copies are constexpr because the copy constructor of a
// literal type is implicitly constexpr.
#if defined(_WIN32)
inline constexpr xAddressReg
    arg1reg = rcx, arg2reg = rdx,
    arg3reg = r8, arg4reg = r9,
    calleeSavedReg1 = rdi,
    calleeSavedReg2 = rsi;

inline constexpr xRegister32
    arg1regd = ecx, arg2regd = edx,
    calleeSavedReg1d = edi,
    calleeSavedReg2d = esi;
#else
inline constexpr xAddressReg
    arg1reg = rdi, arg2reg = rsi,
    arg3reg = rdx, arg4reg = rcx,
    calleeSavedReg1 = r12,
    calleeSavedReg2 = r13;

inline constexpr xRegister32
    arg1regd = edi, arg2regd = esi,
    calleeSavedReg1d = r12d,
    calleeSavedReg2d = r13d;
#endif


	// clang-format on

	inline constexpr xRegisterCL cl; // I'm special!

	bool xRegisterBase::IsCallerSaved(uint id)
	{
#ifdef _WIN32
		// The x64 ABI considers the registers RAX, RCX, RDX, R8, R9, R10, R11, and XMM0-XMM5 volatile.
		return (id <= 2 || (id >= 8 && id <= 11));
#else
		// rax, rdi, rsi, rdx, rcx, r8, r9, r10, r11 are scratch registers.
		return (id <= 2 || id == 6 || id == 7 || (id >= 8 && id <= 11));
#endif
	}

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

	bool xRegisterSSE::IsCallerSaved(uint id)
	{
#ifdef _WIN32
		// XMM6 through XMM15 are saved. Upper 128 bits is always volatile.
		return (id < 6);
#else
		// All vector registers are volatile.
		return true;
#endif
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
		sptr Displacement; // address displacement // 4B max even on 64 bits but keep rest for assertions

	public:
		xAddressVoid(const xAddressReg& base, const xAddressReg& index, int factor = 1, sptr displacement = 0);

		xAddressVoid(const xAddressReg& index, sptr displacement = 0);
		explicit xAddressVoid(const void* displacement);
		explicit xAddressVoid(sptr displacement = 0);

	public:
		xAddressVoid& Add(sptr imm)
		{
			Displacement += imm;
			return *this;
		}

		xAddressVoid& Add(const xAddressReg& src);
		xAddressVoid& Add(const xAddressVoid& src);

		__fi xAddressVoid operator+(const xAddressReg& right) const { return xAddressVoid(*this).Add(right); }
		__fi xAddressVoid operator+(const xAddressVoid& right) const { return xAddressVoid(*this).Add(right); }
		__fi xAddressVoid operator+(sptr imm) const { return xAddressVoid(*this).Add(imm); }
		__fi xAddressVoid operator-(sptr imm) const { return xAddressVoid(*this).Add(-imm); }
		__fi xAddressVoid operator+(const void* addr) const { return xAddressVoid(*this).Add((uptr)addr); }

		__fi void operator+=(const xAddressReg& right) { Add(right); }
		__fi void operator+=(sptr imm) { Add(imm); }
		__fi void operator-=(sptr imm) { Add(-imm); }
	};

	static __fi xAddressVoid operator+(const void* addr, const xAddressVoid& right)
	{
		return right + addr;
	}

	static __fi xAddressVoid operator+(sptr addr, const xAddressVoid& right)
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
		sptr Displacement; // offset applied to the Base/Index registers.
			// Displacement is 8/32 bits even on x86_64
			// However we need the whole pointer to calculate rip-relative offsets

	public:
		// Absolute-address form. No Reduce() is needed (there are no registers
		// to fold), so this is a plain field initialisation and belongs in the
		// header: it is the form every ptrNN[&global] operand goes through, and
		// out-of-line it costs a call per memory operand emitted.
		explicit __fi xIndirectVoid(sptr disp)
		{
			Base = xEmptyReg;
			Index = xEmptyReg;
			Scale = 0;
			Displacement = disp;
		}
		explicit xIndirectVoid(const xAddressVoid& src);
		xIndirectVoid(xAddressReg base, xAddressReg index, int scale = 0, sptr displacement = 0);
		xIndirectVoid& Add(sptr imm);

		bool IsReg() const { return false; }
		bool IsExtended() const { return false; } // Non sense but ease template

		operator xAddressVoid()
		{
			return xAddressVoid(Base, Index, Scale, Displacement);
		}

		__fi xIndirectVoid operator+(const sptr imm) const { return xIndirectVoid(*this).Add(imm); }
		__fi xIndirectVoid operator-(const sptr imm) const { return xIndirectVoid(*this).Add(-imm); }

	protected:
		void Reduce();
	};

	template <typename OperandType>
	class xIndirect : public xIndirectVoid
	{
		typedef xIndirectVoid _parent;

	public:
		explicit xIndirect(sptr disp)
			: _parent(disp)
		{
			_operandSize = sizeof(OperandType);
		}
		xIndirect(xAddressReg base, xAddressReg index, int scale = 0, sptr displacement = 0)
			: _parent(base, index, scale, displacement)
		{
			_operandSize = sizeof(OperandType);
		}
		explicit xIndirect(const xIndirectVoid& other)
			: _parent(other)
		{
		}

		xIndirect<OperandType>& Add(sptr imm)
		{
			Displacement += imm;
			return *this;
		}

		__fi xIndirect<OperandType> operator+(const sptr imm) const { return xIndirect(*this).Add(imm); }
		__fi xIndirect<OperandType> operator-(const sptr imm) const { return xIndirect(*this).Add(-imm); }

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
			return xModSibType((uptr)src);
		}
	};

	// ptr[] - use this form for instructions which can resolve the address operand size from
	// the other register operand sizes.
	// --------------------------------------------------------------------------------------
	//  xAddressReg  (operator overloads)
	// --------------------------------------------------------------------------------------
	inline xAddressVoid xAddressReg::operator+(const xAddressReg& right) const
	{
		return xAddressVoid(*this, right);
	}

	inline xAddressVoid xAddressReg::operator+(sptr right) const
	{
		return xAddressVoid(*this, right);
	}

	inline xAddressVoid xAddressReg::operator+(const void* right) const
	{
		return xAddressVoid(*this, (sptr)right);
	}

	inline xAddressVoid xAddressReg::operator-(sptr right) const
	{
		return xAddressVoid(*this, -right);
	}

	inline xAddressVoid xAddressReg::operator-(const void* right) const
	{
		return xAddressVoid(*this, -(sptr)right);
	}

	inline xAddressVoid xAddressReg::operator*(int factor) const
	{
		return xAddressVoid(xEmptyReg, *this, factor);
	}

	inline xAddressVoid xAddressReg::operator<<(u32 shift) const
	{
		return xAddressVoid(xEmptyReg, *this, 1 << shift);
	}


	// --------------------------------------------------------------------------------------
	//  xAddressVoid  (method implementations)
	// --------------------------------------------------------------------------------------

	inline xAddressVoid::xAddressVoid(const xAddressReg& base, const xAddressReg& index, int factor, sptr displacement)
	{
		Base = base;
		Index = index;
		Factor = factor;
		Displacement = displacement;
	}

	inline xAddressVoid::xAddressVoid(const xAddressReg& index, sptr displacement)
	{
		Base = xEmptyReg;
		Index = index;
		Factor = 0;
		Displacement = displacement;
	}

	inline xAddressVoid::xAddressVoid(sptr displacement)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Factor = 0;
		Displacement = displacement;
	}

	inline xAddressVoid::xAddressVoid(const void* displacement)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Factor = 0;
		Displacement = (sptr)displacement;
	}

	inline xAddressVoid& xAddressVoid::Add(const xAddressReg& src)
	{
		if (src == Index)
			Factor++;
		else if (src == Base)
		{
			// Compound the existing register reference into the Index/Scale pair.
			Base = xEmptyReg;

			if (src == Index)
				Factor++;
			else
			{
				Index = src;
				Factor = 2;
			}
		}
		else if (Base.IsEmpty())
			Base = src;
		else if (Index.IsEmpty())
			Index = src;

		return *this;
	}

	inline xAddressVoid& xAddressVoid::Add(const xAddressVoid& src)
	{
		Add(src.Base);
		Add(src.Displacement);

		// If the factor is 1, we can just treat index like a base register also.
		if (src.Factor == 1)
			Add(src.Index);
		else if (Index.IsEmpty())
		{
			Index = src.Index;
			Factor = src.Factor;
		}
		else if (Index == src.Index)
			Factor += src.Factor;

		return *this;
	}

	inline xIndirectVoid::xIndirectVoid(const xAddressVoid& src)
	{
		Base = src.Base;
		Index = src.Index;
		Scale = src.Factor;
		Displacement = src.Displacement;

		Reduce();
	}

	inline xIndirectVoid::xIndirectVoid(xAddressReg base, xAddressReg index, int scale, sptr displacement)
	{
		Base = base;
		Index = index;
		Scale = scale;
		Displacement = displacement;

		Reduce();
	}

	// Generates a 'reduced' ModSib form, which has valid Base, Index, and Scale values.
	// Necessary because by default ModSib compounds registers into Index when possible.
	//
	// If the ModSib is in illegal form ([Base + Index*5] for example) then an assertion
	// followed by an InvalidParameter Exception will be tossed around in haphazard
	// fashion.
	//
	// Optimization Note: Currently VC does a piss poor job of inlining this, even though
	// constant propagation *should* resove it to little or no code (VC's constprop fails
	// on C++ class initializers).  There is a work around [using array initializers instead]
	// but it's too much trouble for code that isn't performance critical anyway.
	// And, with luck, maybe VC10 will optimize it better and make it a non-issue. :D
	//
	inline void xIndirectVoid::Reduce()
	{
		if (Index.Id == 4)
		{
			// esp cannot be encoded as the index, so move it to the Base, if possible.
			// note: intentionally leave index assigned to esp also (generates correct
			// encoding later, since ESP cannot be encoded 'alone')
			Base = Index;
			return;
		}

		// If no index reg, then load the base register into the index slot.
		if (Index.IsEmpty())
		{
			Index = Base;
			Scale = 0;
			if (Base.Id != 4) // prevent ESP from being encoded 'alone'
				Base = xEmptyReg;
			return;
		}

		// The Scale has a series of valid forms, all shown here:

		switch (Scale)
		{
			case 1:
				Scale = 0;
				break;
			case 2:
				Scale = 1;
				break;

			case 3: // becomes [reg*2+reg]
				Base = Index;
				Scale = 1;
				break;

			case 4:
				Scale = 2;
				break;

			case 5: // becomes [reg*4+reg]
				Base = Index;
				Scale = 2;
				break;

			case 6: // invalid!
			case 7: // so invalid!
				break;

			case 8:
				Scale = 3;
				break;
			case 9: // becomes [reg*8+reg]
				Base = Index;
				Scale = 3;
				break;
			case 0:
			default:
				break;
		}
	}

	// Stateless: inline definitions, no out-of-line storage.
	inline const xAddressIndexer<xIndirectVoid> ptr = {};
	inline const xAddressIndexer<xIndirectNative> ptrNative = {};
	inline const xAddressIndexer<xIndirect128> ptr128 = {};
	inline const xAddressIndexer<xIndirect64> ptr64 = {};
	inline const xAddressIndexer<xIndirect32> ptr32 = {};
	inline const xAddressIndexer<xIndirect16> ptr16 = {};
	inline const xAddressIndexer<xIndirect8> ptr8 = {};

	// --------------------------------------------------------------------------------------
	//  xForwardJump
	// --------------------------------------------------------------------------------------
	// Primary use of this class is through the various xForwardJA8/xForwardJLE32/etc. helpers
	// defined later in this header. :)
	//

	template <typename OperandType>
	class xForwardJump
	{
	public:
		s8* BasePtr;
		static const uint OperandSize = sizeof(OperandType);

		// The jump instruction is emitted at the point of object construction.  The conditional
		// type must be valid (Jcc_Unknown generates an assertion).
		xForwardJump(JccComparisonType cctype = Jcc_Unconditional)
		{
			BasePtr = (s8*)x86Ptr +
				((OperandSize == 1) ? 2 : // j8's are always 2 bytes.
				 ((cctype == Jcc_Unconditional) ? 5 : 6)); // j32's are either 5 or 6 bytes

			if (OperandSize == 1)
			{
				xWrite8((cctype == Jcc_Unconditional) ? 0xeb : (0x70 | cctype));
			}
			else
			{
				if (cctype == Jcc_Unconditional)
				{
					xWrite8(0xe9);
				}
				else
				{
					xWrite8(0x0f);
					xWrite8(0x80 | cctype);
				}
			}

			x86Ptr += OperandSize;
		}

		// Sets the jump target by writing back the current x86Ptr to the jump instruction.
		// This method can be called multiple times, re-writing the jump instruction's target
		// in each case. (the the last call is the one that takes effect).
		void SetTarget() const
		{
			sptr displacement = (sptr)x86Ptr - (sptr)BasePtr;
			if (OperandSize == 1)
				BasePtr[-1] = (s8)displacement;
			else
			{
				// full displacement, no sanity checks needed :D
				const s32 disp32_ = (s32)displacement;
				memcpy(BasePtr - sizeof(s32), &disp32_, sizeof(s32));
			}
		}
	};

	static __fi xAddressVoid operator+(const void* addr, const xAddressReg& reg)
	{
		return reg + (sptr)addr;
	}

	static __fi xAddressVoid operator+(sptr addr, const xAddressReg& reg)
	{
		return reg + (sptr)addr;
	}
} // namespace x86Emitter

// Reference-emitter implementation headers (xImpl_* struct declarations
// and the xOpWrite* encoders). The C89 build has no use for them; the
// reference build and the byte oracles pull them from the harness tree.
#include "tests/emitter/reference/implement/simd_helpers.h"
#include "tests/emitter/reference/implement/simd_moremovs.h"
#include "tests/emitter/reference/implement/simd_arithmetic.h"
#include "tests/emitter/reference/implement/simd_comparisons.h"
#include "tests/emitter/reference/implement/simd_shufflepack.h"

#include "tests/emitter/reference/implement/group1.h"
#include "tests/emitter/reference/implement/group2.h"
#include "tests/emitter/reference/implement/group3.h"
#include "tests/emitter/reference/implement/movs.h" // cmov and movsx/zx
#include "tests/emitter/reference/implement/incdec.h"
#include "tests/emitter/reference/implement/test.h"
#include "tests/emitter/reference/implement/jmpcall.h"

#include "tests/emitter/reference/implement/avx.h"
