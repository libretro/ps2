#include "common/emitter/c89ops.h"
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

#include "Config.h"
#include "common/CpuFeatures.h"

//------------------------------------------------------------------
// Dispatcher Functions
//------------------------------------------------------------------
static int mvuNeedsFPCRUpdate(mV)
{
	// always update on the vu1 thread
	if (isVU1 && THREAD_VU1)
		return 1;

	// otherwise only emit when it's different to the EE
	return EmuConfig.Cpu.FPUFPCR.bitmask != (isVU0 ? EmuConfig.Cpu.VU0FPCR.bitmask : EmuConfig.Cpu.VU1FPCR.bitmask);
}

// Generates the code for entering/exit recompiled blocks
void mVUdispatcherAB(mV)
{
	mVU->startFunct = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		// = The caller has already put the needed parameters in ecx/edx:
		if (!isVU1) xe_fastcall2_rr(mVUexecuteVU0, XE_ARG1, XE_ARG2);
		else        xe_fastcall2_rr(mVUexecuteVU1, XE_ARG1, XE_ARG2);

		// Load VU's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask);

		// Load Regs
		xe_movaps_xm(xmmT1, &vuRegs[mVU->index].VI[REG_P].UL);
		xe_movaps_xm(xmmPQ, &vuRegs[mVU->index].VI[REG_Q].UL);
		{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].pending_q); xe_movdzx_xmemg(xmmT2, xm); }
		xe_shufps_xxi(xmmPQ, xmmT1, 0); // wzyx = PPQQ
		//Load in other Q instance
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);
		xe_movss_xx(xmmPQ, xmmT2);
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);

		if (isVU1)
		{
			//Load in other P instance
			{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].pending_p); xe_movdzx_xmemg(xmmT2, xm); }
			xe_pshufd_xxi(xmmPQ, xmmPQ, 0x1B);
			xe_movss_xx(xmmPQ, xmmT2);
			xe_pshufd_xxi(xmmPQ, xmmPQ, 0x1B);
		}

		xe_movaps_xm(xmmT1, &vuRegs[mVU->index].micro_macflags);
		xe_movaps_mx(mVU->macFlag, xmmT1);


		xe_movaps_xm(xmmT1, &vuRegs[mVU->index].micro_clipflags);
		xe_movaps_mx(mVU->clipFlag, xmmT1);

		xe_mov32_rm(gprF0, &vuRegs[mVU->index].micro_statusflags[0]);
		xe_mov32_rm(gprF1, &vuRegs[mVU->index].micro_statusflags[1]);
		xe_mov32_rm(gprF2, &vuRegs[mVU->index].micro_statusflags[2]);
		xe_mov32_rm(gprF3, &vuRegs[mVU->index].micro_statusflags[3]);

		// Jump to Recompiled Code Block
		xe_jmp_r(XE_AX);

		mVU->exitFunct = x86Ptr;

		// Load EE's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);

		// = The first two DWORD or smaller arguments are passed in ECX and EDX registers;
		//              all other arguments are passed right to left.
		if (!isVU1) xe_fastcall0(mVUcleanUpVU0);
		else        xe_fastcall0(mVUcleanUpVU1);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xe_ret();
}

// Generates the code for resuming/exit xgkick
void mVUdispatcherCD(mV)
{
	mVU->startFunctXG = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		// Load VU's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask);

		mVUrestoreRegs(mVU, 0, 0);
		xe_mov32_rm(gprF0, &vuRegs[mVU->index].micro_statusflags[0]);
		xe_mov32_rm(gprF1, &vuRegs[mVU->index].micro_statusflags[1]);
		xe_mov32_rm(gprF2, &vuRegs[mVU->index].micro_statusflags[2]);
		xe_mov32_rm(gprF3, &vuRegs[mVU->index].micro_statusflags[3]);

		// Jump to Recompiled Code Block
		xe_jmp_mem_abs(&mVU->resumePtrXG);

		mVU->exitFunctXG = x86Ptr;

		// Backup Status Flag (other regs were backed up on xgkick)
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[0], gprF0);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[1], gprF1);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[2], gprF2);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[3], gprF3);

		// Load EE's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xe_ret();
}

static void mVUGenerateWaitMTVU(mV)
{
	mVU->waitMTVU = x86Ptr;

	int num_xmms = 0, num_gprs = 0;

	for (int i = 0; i < (int)(iREGCNT_GPR); i++)
	{
		if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
			continue;

		// T1 often contains the address we're loading when waiting for VU1.
		// T2 isn't used until afterwards, so don't bother saving it.
		if (i == gprT2)
			continue;

		xe_push64_r(i);
		num_gprs++;
	}

	for (int i = 0; i < (int)(iREGCNT_XMM); i++)
	{
		if (!XE_XMM_CALLER_SAVED(i))
			continue;

		num_xmms++;
	}

	// We need 16 byte alignment on the stack.
	// Since the stack is unaligned at entry to this function, we add 8 when it's even, not odd.
	const int stack_size = (num_xmms * sizeof(u128)) + ((~num_gprs & 1) * sizeof(u64)) + SHADOW_STACK_SIZE;
	int stack_offset = SHADOW_STACK_SIZE;

	if (stack_size > 0)
	{
		xe_sub64_ri(XE_SP, stack_size);
		for (int i = 0; i < (int)(iREGCNT_XMM); i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_memxg(xm, i); }
			stack_offset += sizeof(u128);
		}
	}

	xe_fastcall0(mVUwaitMTVU);

	stack_offset = (num_xmms - 1) * sizeof(u128) + SHADOW_STACK_SIZE;
	for (int i = (int)(iREGCNT_XMM - 1); i >= 0; i--)
	{
		if (!XE_XMM_CALLER_SAVED(i))
			continue;

		{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_xmemg(i, xm); }
		stack_offset -= sizeof(u128);
	}
	xe_add64_ri(XE_SP, stack_size);

	for (int i = (int)(iREGCNT_GPR - 1); i >= 0; i--)
	{
		if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
			continue;

		if (i == gprT2)
			continue;

		xe_pop64_r(i);
	}

	xe_ret();
}

static void mVUGenerateCopyPipelineState(mV)
{
	mVU->copyPLState = x86Ptr;

	if (CPU_HAS_AVX2)
	{
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 0); xe_vmovaps_xmemg(0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 32); xe_vmovaps_xmemg(1, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 64); xe_vmovaps_xmemg(2, xm, 1); }

		{ struct e_mem xm; XE_MEM_ABS(xm, &mVU->prog.lpState); xe_vmovups_memxg(xm, 0, 1); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 32); xe_vmovups_memxg(xm, 1, 1); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 64); xe_vmovups_memxg(xm, 2, 1); }

		xe_vzeroupper();
	}
	else
	{
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 0); xe_movaps_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 16); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 32); xe_movaps_xmemg(2, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 48); xe_movaps_xmemg(3, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 64); xe_movaps_xmemg(4, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 80); xe_movaps_xmemg(5, xm); }

		{ struct e_mem xm; XE_MEM_ABS(xm, &mVU->prog.lpState); xe_movups_memxg(xm, 0); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 16); xe_movups_memxg(xm, 1); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 32); xe_movups_memxg(xm, 2); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 48); xe_movups_memxg(xm, 3); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 64); xe_movups_memxg(xm, 4); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 80); xe_movups_memxg(xm, 5); }
	}

	xe_ret();
}

//------------------------------------------------------------------
// Micro VU - Custom Quick Search
//------------------------------------------------------------------

// Generates a custom optimized block-search function
// Note: Structs must be 16-byte aligned! (GCC doesn't guarantee this)
static void mVUGenerateCompareState(mV)
{
	mVU->compareStateF = x86Ptr;

	if (CPU_HAS_AVX2)
	{
		// We have to use unaligned loads here, because the blocks are only 16 byte aligned.
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_vmovups_xmemg(0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0); xe_vpcmpeqd_xxmemg(0, 0, xm, 1); }
		xe_vpmovmskb_rx(XE_AX, 0, 1);
		xe_xor32_ri(XE_AX, 0xffffffff);
		uint8_t* exitPoint; xe_fwd_jcc8(Jcc_NotZero, exitPoint);

		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x20); xe_vmovups_xmemg(0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x40); xe_vmovups_xmemg(1, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x20); xe_vpcmpeqd_xxmemg(0, 0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x40); xe_vpcmpeqd_xxmemg(1, 1, xm, 1); }
		xe_vpand_xxx(0, 0, 1, 1);

		xe_vpmovmskb_rx(XE_AX, 0, 1);
		xe_not32_r(XE_AX);

		xe_fwd_set8(exitPoint);
		xe_vzeroupper();
	}
	else
	{
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movaps_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0); xe_pcmpeqd_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x10); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x10); xe_pcmpeqd_xmemg(1, xm); }
		xe_pand_xx(0, 1);

		xe_movmskps_rx(XE_AX, 0);
		xe_xor32_ri(XE_AX, 0xf);
		uint8_t* exitPoint; xe_fwd_jcc8(Jcc_NotZero, exitPoint);

		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x20); xe_movaps_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x20); xe_pcmpeqd_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x30); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x30); xe_pcmpeqd_xmemg(1, xm); }
		xe_pand_xx(0, 1);

		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x40); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x40); xe_pcmpeqd_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x50); xe_movaps_xmemg(2, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x50); xe_pcmpeqd_xmemg(2, xm); }
		xe_pand_xx(1, 2);
		xe_pand_xx(0, 1);

		xe_movmskps_rx(XE_AX, 0);
		xe_xor32_ri(XE_AX, 0xf);

		xe_fwd_set8(exitPoint);
	}

	xe_ret();
}

//------------------------------------------------------------------
// Execution Functions
//------------------------------------------------------------------

// Executes for number of cycles
_mVUt void* mVUexecute(u32 startPC, u32 cycles)
{
	microVU* mVU    = mVUx;
	u32 vuLimit     = vuIndex ? 0x3ff8 : 0xff8;
	mVU->cycles      = cycles;
	mVU->totalCycles = cycles;
	x86Ptr = (u8*)(mVU->prog.codePtr); // Set codePtr to where last program left off
	return mVUsearchProg<vuIndex>(startPC & vuLimit, (uptr)&mVU->prog.lpState); // Find and set correct program
}

//------------------------------------------------------------------
// Cleanup Functions
//------------------------------------------------------------------

_mVUt void mVUcleanUp(void)
{
	microVU* mVU = mVUx;

	mVU->prog.codePtr = x86Ptr;

	if ((x86Ptr < mVU->prog.codeStart) || (x86Ptr >= mVU->prog.codeEnd))
		mVUreset(mVU, 0);

	mVU->cycles = mVU->totalCycles - C89_MAX(0, mVU->cycles);
	vuRegs[mVU->index].cycle += mVU->cycles;

	if (!vuIndex || !THREAD_VU1)
	{
		u32 cycles_passed = C89_MIN(mVU->cycles, 3000) * EmuConfig.Speedhacks.EECycleSkip;
		if (cycles_passed > 0)
		{
			s64 vu0_offset = vuRegs[0].cycle - cpuRegs.cycle;
			cpuRegs.cycle += cycles_passed;

			// VU0 needs to stay in sync with the CPU otherwise things get messy
			// So we need to adjust when VU1 skips cycles also
			if (!vuIndex)
				vuRegs[0].cycle  = cpuRegs.cycle + vu0_offset;
			else
				vuRegs[0].cycle += cycles_passed;
		}
	}
}

//------------------------------------------------------------------
// Caller Functions
//------------------------------------------------------------------

void* mVUexecuteVU0(u32 startPC, u32 cycles) { return mVUexecute<0>(startPC, cycles); }
void* mVUexecuteVU1(u32 startPC, u32 cycles) { return mVUexecute<1>(startPC, cycles); }
void mVUcleanUpVU0() { mVUcleanUp<0>(); }
void mVUcleanUpVU1() { mVUcleanUp<1>(); }
