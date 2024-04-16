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


#include "PrecompiledHeader.h"

#include "Common.h"
#include "vtlb.h"

#include "iCore.h"
#include "iR5900.h"

using namespace vtlb_private;
using namespace x86Emitter;

// we need enough for a 32-bit jump forwards (5 bytes)
static constexpr u32 LOADSTORE_PADDING = 5;

static u32 GetAllocatedGPRBitmask(void)
{
	u32 mask = 0;
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

static u32 GetAllocatedXMMBitmask()
{
	u32 mask = 0;
	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (xmmregs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

/*
	// Pseudo-Code For the following Dynarec Implementations -->

	u32 vmv = vmap[addr>>VTLB_PAGE_BITS].raw();
	sptr ppf=addr+vmv;
	if (!(ppf<0))
	{
		data[0]=*reinterpret_cast<DataType*>(ppf);
		if (DataSize==128)
			data[1]=*reinterpret_cast<DataType*>(ppf+8);
		return 0;
	}
	else
	{
		//has to: translate, find function, call function
		u32 hand=(u8)vmv;
		u32 paddr=(ppf-hand) << 1;
		//Console.WriteLn("Translated 0x%08X to 0x%08X",params addr,paddr);
		return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);
	}

	// And in ASM it looks something like this -->

	mov eax,ecx;
	shr eax,VTLB_PAGE_BITS;
	mov rax,[rax*wordsize+vmap];
	add rcx,rax;
	js _fullread;

	//these are wrong order, just an example ...
	mov [rax],ecx;
	mov ecx,[rdx];
	mov [rax+4],ecx;
	mov ecx,[rdx+4];
	mov [rax+4+4],ecx;
	mov ecx,[rdx+4+4];
	mov [rax+4+4+4+4],ecx;
	mov ecx,[rdx+4+4+4+4];
	///....

	jmp cont;
	_fullread:
	movzx eax,al;
	sub   ecx,eax;
	call [eax+stuff];
	cont:
	........

*/

namespace vtlb_private
{
	// ------------------------------------------------------------------------
	// Prepares eax, ecx, and, ebx for Direct or Indirect operations.
	// Returns the writeback pointer for ebx (return address from indirect handling)
	//
	static void DynGen_PrepRegs(int addr_reg, int value_reg, u32 sz, bool xmm)
	{
		_freeX86reg(arg1regd);
		xMOV(arg1regd, xRegister32(addr_reg));

		if (value_reg >= 0)
		{
			if (sz == 128)
			{
				_freeXMMreg(xRegisterSSE::GetArgRegister(1, 0).Id);
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			}
			else if (xmm)
			{
				// 32bit xmms are passed in GPRs
				_freeX86reg(arg2regd);
				xMOVD(arg2regd, xRegisterSSE(value_reg));
			}
			else
			{
				_freeX86reg(arg2regd);
				xMOV(arg2reg, xRegister64(value_reg));
			}
		}

		xMOV(eax, arg1regd);
		xSHR(eax, VTLB_PAGE_BITS);
		xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.vmap, rax * wordsize)]);
		xADD(arg1reg, rax);
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectRead(u32 bits, bool sign)
	{
		switch (bits)
		{
			case 8:
				if (sign)
					xMOVSX(rax, ptr8[arg1reg]);
				else
					xMOVZX(rax, ptr8[arg1reg]);
				break;

			case 16:
				if (sign)
					xMOVSX(rax, ptr16[arg1reg]);
				else
					xMOVZX(rax, ptr16[arg1reg]);
				break;

			case 32:
				if (sign)
					xMOVSX(rax, ptr32[arg1reg]);
				else
					xMOV(eax, ptr32[arg1reg]);
				break;

			case 64:
				xMOV(rax, ptr64[arg1reg]);
				break;

			case 128:
				xMOVAPS(xmm0, ptr128[arg1reg]);
				break;
			default:
				break;
		}
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectWrite(u32 bits)
	{
		switch (bits)
		{
			case 8:
				xMOV(ptr[arg1reg], xRegister8(arg2regd));
				break;

			case 16:
				xMOV(ptr[arg1reg], xRegister16(arg2regd));
				break;

			case 32:
				xMOV(ptr[arg1reg], arg2regd);
				break;

			case 64:
				xMOV(ptr[arg1reg], arg2reg);
				break;

			case 128:
				xMOVAPS(ptr[arg1reg], xRegisterSSE::GetArgRegister(1, 0));
				break;
		}
	}
} // namespace vtlb_private

// ------------------------------------------------------------------------
// allocate one page for our naked indirect dispatcher function.
// this *must* be a full page, since we'll give it execution permission later.
// If it were smaller than a page we'd end up allowing execution rights on some
// other vars additionally (bad!).
//
alignas(__pagesize) static u8 m_IndirectDispatchers[__pagesize];

// ------------------------------------------------------------------------
// mode        - 0 for read, 1 for write!
// operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits.
//
static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign = 0)
{
	assert(mode || operandsize >= 3 ? !sign : true);

	// Each dispatcher is aligned to 64 bytes.  The actual dispatchers are only like
	// 20-some bytes each, but 64 byte alignment on functions that are called
	// more frequently than a hot sex hotline at 1:15am is probably a good thing.

	// 7*64? 5 widths with two sign extension modes for 8 and 16 bit reads

	// Gregory: a 32 bytes alignment is likely enough and more cache friendly
	const int A = 32;

	return &m_IndirectDispatchers[(mode * (8 * A)) + (sign * 5 * A) + (operandsize * A)];
}

// ------------------------------------------------------------------------
// Generates a JS instruction that targets the appropriate templated instance of
// the vtlb Indirect Dispatcher.
//

template <typename GenDirectFn>
static void DynGen_HandlerTest(const GenDirectFn& gen_direct, int mode, int bits, bool sign = false)
{
	int szidx = 0;
	switch (bits)
	{
		case   8: szidx = 0; break;
		case  16: szidx = 1; break;
		case  32: szidx = 2; break;
		case  64: szidx = 3; break;
		case 128: szidx = 4; break;
		default:
			  break;
	}
	xForwardJS8 to_handler;
	gen_direct();
	xForwardJump8 done;
	to_handler.SetTarget();
	xFastCall(GetIndirectDispatcherPtr(mode, szidx, sign));
	done.SetTarget();
}

// ------------------------------------------------------------------------
// Generates the various instances of the indirect dispatchers
// In: arg1reg: vtlb entry, arg2reg: data ptr (if mode >= 64), rbx: function return ptr
// Out: eax: result (if mode < 64)
static void DynGen_IndirectTlbDispatcher(int mode, int bits, bool sign)
{
	// fixup stack
#ifdef _WIN32
	xSUB(rsp, 32 + 8);
#else
	xSUB(rsp, 8);
#endif

	xMOVZX(eax, al);
	if (wordsize != 8)
		xSUB(arg1regd, 0x80000000);
	xSUB(arg1regd, eax);

	// jump to the indirect handler, which is a C++ function.
	// [ecx is address, edx is data]
	sptr table = (sptr)vtlbdata.RWFT[bits][mode];
	if (table == (s32)table)
	{
		xFastCall(ptrNative[(rax * wordsize) + table], arg1reg, arg2reg);
	}
	else
	{
		xLEA(arg3reg, ptr[(void*)table]);
		xFastCall(ptrNative[(rax * wordsize) + arg3reg], arg1reg, arg2reg);
	}

	if (!mode)
	{
		if (bits == 0)
		{
			if (sign)
				xMOVSX(rax, al);
			else
				xMOVZX(rax, al);
		}
		else if (bits == 1)
		{
			if (sign)
				xMOVSX(rax, ax);
			else
				xMOVZX(rax, ax);
		}
		else if (bits == 2)
		{
			if (sign)
				xCDQE();
		}
	}

#ifdef _WIN32
	xADD(rsp, 32 + 8);
#else
	xADD(rsp, 8);
#endif

	xRET();
}

// One-time initialization procedure.  Multiple subsequent calls during the lifespan of the
// process will be ignored.
//
void vtlb_dynarec_init(void)
{
	static bool hasBeenCalled = false;
	if (hasBeenCalled)
		return;
	hasBeenCalled = true;

	// In case init gets called multiple times:
	HostSys::MemProtectStatic(m_IndirectDispatchers, PageAccess_ReadWrite());

	// clear the buffer to 0xcc (easier debugging).
	memset(m_IndirectDispatchers, 0xcc, __pagesize);

	for (int mode = 0; mode < 2; ++mode)
	{
		for (int bits = 0; bits < 5; ++bits)
		{
			for (int sign = 0; sign < (!mode && bits < 3 ? 2 : 1); sign++)
			{
				xSetPtr(GetIndirectDispatcherPtr(mode, bits, !!sign));

				DynGen_IndirectTlbDispatcher(mode, bits, !!sign);
			}
		}
	}

	HostSys::MemProtectStatic(m_IndirectDispatchers, PageAccess_ExecOnly());
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Load Implementations
// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
int vtlb_DynGenReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, -1, bits, xmm);
		DynGen_HandlerTest([bits, sign]() { DynGen_DirectRead(bits, sign); }, 0, bits, sign && bits < 64);

		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			xMOV(xRegister64(x86_dest_reg), rax);
		}
		else
		{
			// we shouldn't be loading any FPRs which aren't 32bit..
			// we use MOVD here despite it being floating-point data, because we're going int->float reinterpret.
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
		}

		return x86_dest_reg;
	}

	const u8* codeStart;
	const xAddressReg x86addr(addr_reg);
	if (!xmm)
	{
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
		codeStart = x86Ptr;
		const xRegister64 x86reg(x86_dest_reg);
		switch (bits)
		{
		case 8:
			sign ? xMOVSX(x86reg, ptr8[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr8[RFASTMEMBASE + x86addr]);
			break;
		case 16:
			sign ? xMOVSX(x86reg, ptr16[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr16[RFASTMEMBASE + x86addr]);
			break;
		case 32:
			sign ? xMOVSX(x86reg, ptr32[RFASTMEMBASE + x86addr]) : xMOV(xRegister32(x86reg), ptr32[RFASTMEMBASE + x86addr]);
			break;
		case 64:
			xMOV(x86reg, ptr64[RFASTMEMBASE + x86addr]);
			break;
		default:
			break;
		}
	}
	else
	{
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		codeStart = x86Ptr;
		const xRegisterSSE xmmreg(x86_dest_reg);
		xMOVSSZX(xmmreg, ptr32[RFASTMEMBASE + x86addr]);
	}

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(x86_dest_reg),
		static_cast<u8>(bits), sign, true, xmm);

	return x86_dest_reg;
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
//
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
//
int vtlb_DynGenReadNonQuad_Const(u32 bits, bool sign, bool xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int x86_dest_reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			switch (bits)
			{
			case 8:
				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr8[(u8*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr8[(u8*)ppf]);
				break;

			case 16:
				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr16[(u16*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr16[(u16*)ppf]);
				break;

			case 32:
				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr32[(u32*)ppf]) : xMOV(xRegister32(x86_dest_reg), ptr32[(u32*)ppf]);
				break;

			case 64:
				xMOV(xRegister64(x86_dest_reg), ptr64[(u64*)ppf]);
				break;
			}
		}
		else
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xMOVSSZX(xRegisterSSE(x86_dest_reg), ptr32[(float*)ppf]);
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case  8: szidx = 0; break;
			case 16: szidx = 1; break;
			case 32: szidx = 2; break;
			case 64: szidx = 3; break;
		}

		// Shortcut for the INTC_STAT register, which many games like to spin on heavily.
		if ((bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
			if (!xmm)
			{
				if (sign)
					xMOVSX(xRegister64(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
				else
					xMOV(xRegister32(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
			}
			else
			{
				xMOVDZX(xRegisterSSE(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
			}
		}
		else
		{
			iFlushCall(FLUSH_FULLVTLB);
			xFastCall(vmv.assumeHandlerGetRaw(szidx, false), paddr);

			if (!xmm)
			{
				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.Id);
				switch (bits)
				{
					// save REX prefix by using 32bit dest for zext
				case 8:
					sign ? xMOVSX(xRegister64(x86_dest_reg), al) : xMOVZX(xRegister32(x86_dest_reg), al);
					break;

				case 16:
					sign ? xMOVSX(xRegister64(x86_dest_reg), ax) : xMOVZX(xRegister32(x86_dest_reg), ax);
					break;

				case 32:
					sign ? xMOVSX(xRegister64(x86_dest_reg), eax) : xMOV(xRegister32(x86_dest_reg), eax);
					break;

				case 64:
					xMOV(xRegister64(x86_dest_reg), rax);
					break;
				}
			}
			else
			{
				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
				xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
			}
		}
	}

	return x86_dest_reg;
}

int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(arg1regd.Id, -1, bits, true);
		DynGen_HandlerTest([bits]() {DynGen_DirectRead(bits, false); },  0, bits);

		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
		if (reg >= 0)
			xMOVAPS(xRegisterSSE(reg), xmm0);

		return reg;
	}

	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
	const u8* codeStart = x86Ptr;

	xMOVAPS(xRegisterSSE(reg), ptr128[RFASTMEMBASE + arg1reg]);

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(arg1reg.Id), static_cast<u8>(reg),
		static_cast<u8>(bits), false, true, true);

	return reg;
}


// ------------------------------------------------------------------------
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	int reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		void* ppf = reinterpret_cast<void*>(vmv.assumePtr(addr_const));
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0)
			xMOVAPS(xRegisterSSE(reg), ptr128[ppf]);
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		const int szidx = 4;
		iFlushCall(FLUSH_FULLVTLB);
		xFastCall(vmv.assumeHandlerGetRaw(szidx, 0), paddr);

		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		xMOVAPS(xRegisterSSE(reg), xmm0);
	}

	return reg;
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Store Implementations

void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg)
{
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, value_reg, sz, xmm);
		DynGen_HandlerTest([sz]() { DynGen_DirectWrite(sz); }, 1, sz);
		return;
	}

	const u8* codeStart = x86Ptr;

	const xAddressReg vaddr_reg(addr_reg);
	if (!xmm)
	{
		switch (sz)
		{
		case 8:
			xMOV(ptr8[RFASTMEMBASE + vaddr_reg], xRegister8(xRegister32(value_reg)));
			break;
		case 16:
			xMOV(ptr16[RFASTMEMBASE + vaddr_reg], xRegister16(value_reg));
			break;
		case 32:
			xMOV(ptr32[RFASTMEMBASE + vaddr_reg], xRegister32(value_reg));
			break;
		case 64:
			xMOV(ptr64[RFASTMEMBASE + vaddr_reg], xRegister64(value_reg));
			break;
		default:
			break;
		}
	}
	else
	{
		switch (sz)
		{
		case 32:
			xMOVSS(ptr32[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
			break;
		case 128:
			xMOVAPS(ptr128[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
			break;
		default:
			break;
		}
	}

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(value_reg),
		static_cast<u8>(sz), false, false, xmm);
}


// ------------------------------------------------------------------------
// Generates code for a store instruction, where the address is a known constant.
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg)
{
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
		if (!xmm)
		{
			switch (bits)
			{
				case 8:
					xMOV(ptr[(void*)ppf], xRegister8(xRegister32(value_reg)));
					break;

				case 16:
					xMOV(ptr[(void*)ppf], xRegister16(value_reg));
					break;

				case 32:
					xMOV(ptr[(void*)ppf], xRegister32(value_reg));
					break;

				case 64:
					xMOV(ptr64[(void*)ppf], xRegister64(value_reg));
					break;
				default:
					break;
			}
		}
		else
		{
			switch (bits)
			{
				case 32:
					xMOVSS(ptr[(void*)ppf], xRegisterSSE(value_reg));
					break;

				case 128:
					xMOVAPS(ptr128[(void*)ppf], xRegisterSSE(value_reg));
					break;
				default:
					break;
			}
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				szidx = 0;
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
			case 128:
				szidx = 4;
				break;
		}

		iFlushCall(FLUSH_FULLVTLB);

		_freeX86reg(arg1regd);
		xMOV(arg1regd, paddr);
		if (bits == 128)
		{
			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
			_freeXMMreg(argreg.Id);
			xMOVAPS(argreg, xRegisterSSE(value_reg));
		}
		else if (xmm)
		{
			_freeX86reg(arg2regd);
			xMOVD(arg2regd, xRegisterSSE(value_reg));
		}
		else
		{
			_freeX86reg(arg2regd);
			xMOV(arg2reg, xRegister64(value_reg));
		}

		xFastCall(vmv.assumeHandlerGetRaw(szidx, true));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//							Extra Implementations

//   ecx - virtual address
//   Returns physical address in eax.
//   Clobbers edx
void vtlb_DynV2P(void)
{
	xMOV(eax, ecx);
	xAND(ecx, VTLB_PAGE_MASK); // vaddr & VTLB_PAGE_MASK

	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(eax, ptr[xComplexAddress(rdx, vtlbdata.ppmap, rax * 4)]); // vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS];

	xOR(eax, ecx);
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register,
	u8 size_in_bits, bool is_signed, bool is_load, bool is_xmm)
{
	static constexpr u32 GPR_SIZE = 8;
	static constexpr u32 XMM_SIZE = 16;

	// on win32, we need to reserve an additional 32 bytes shadow space when calling out to C
#ifdef _WIN32
	static constexpr u32 SHADOW_SIZE = 32;
#else
	static constexpr u32 SHADOW_SIZE = 0;
#endif
	u8* thunk = recBeginThunk();

	// save regs
	u32 num_gprs = 0;
	u32 num_fprs = 0;

	const u32 rbxid = static_cast<u32>(rbx.Id);
	const u32 arg1id = static_cast<u32>(arg1reg.Id);
	const u32 arg2id = static_cast<u32>(arg2reg.Id);
	const u32 arg3id = static_cast<u32>(arg3reg.Id);

	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if ((gpr_bitmask & (1u << i)) && (i == rbxid || i == arg1id || i == arg2id || xRegisterBase::IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			num_gprs++;
	}
	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (fpr_bitmask & (1u << i) && xRegisterSSE::IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			num_fprs++;
	}

	const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE) + (num_fprs * XMM_SIZE) + SHADOW_SIZE;

	if (stack_size > 0)
	{
		xSUB(rsp, stack_size);

		u32 stack_offset = SHADOW_SIZE;
		for (u32 i = 0; i < iREGCNT_XMM; i++)
		{
			if (fpr_bitmask & (1u << i) && xRegisterSSE::IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			{
				xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
				stack_offset += XMM_SIZE;
			}
		}

		for (u32 i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || xRegisterBase::IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			{
				xMOV(ptr64[rsp + stack_offset], xRegister64(i));
				stack_offset += GPR_SIZE;
			}
		}
	}

	if (is_load)
	{
		DynGen_PrepRegs(address_register, -1, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits, is_signed]() {DynGen_DirectRead(size_in_bits, is_signed); },  0, size_in_bits, is_signed && size_in_bits <= 32);

		if (size_in_bits == 128)
		{
			if (data_register != xmm0.Id)
				xMOVAPS(xRegisterSSE(data_register), xmm0);
		}
		else
		{
			if (is_xmm)
			{
				xMOVDZX(xRegisterSSE(data_register), rax);
			}
			else
			{
				if (data_register != eax.Id)
					xMOV(xRegister64(data_register), rax);
			}
		}
	}
	else
	{
		if (address_register != arg1reg.Id)
			xMOV(arg1regd, xRegister32(address_register));

		if (size_in_bits == 128)
		{
			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
			if (data_register != argreg.Id)
				xMOVAPS(argreg, xRegisterSSE(data_register));
		}
		else
		{
			if (is_xmm)
			{
				xMOVD(arg2reg, xRegisterSSE(data_register));
			}
			else
			{
				if (data_register != arg2reg.Id)
					xMOV(arg2reg, xRegister64(data_register));
			}
		}

		DynGen_PrepRegs(address_register, data_register, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits]() { DynGen_DirectWrite(size_in_bits); }, 1, size_in_bits);
	}

	// restore regs
	if (stack_size > 0)
	{
		u32 stack_offset = SHADOW_SIZE;
		for (u32 i = 0; i < iREGCNT_XMM; i++)
		{
			if (fpr_bitmask & (1u << i) && xRegisterSSE::IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			{
				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
				stack_offset += XMM_SIZE;
			}
		}

		for (u32 i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || xRegisterBase::IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			{
				xMOV(xRegister64(i), ptr64[rsp + stack_offset]);
				stack_offset += GPR_SIZE;
			}
		}

		xADD(rsp, stack_size);
	}

	xJMP((void*)(code_address + code_size));

	recEndThunk();

	// backpatch to a jump to the slowmem handler
	x86Ptr = (u8*)code_address;
	xJMP(thunk);

	// fill the rest of it with nops, if any
	for (u32 i = static_cast<u32>((uptr)x86Ptr - code_address); i < code_size; i++)
		xNOP();
}
