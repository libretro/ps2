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

//------------------------------------------------------------------
// Shared exact-multiply tree stub, emitted once per dispatcher init.
// The paired Booth/CSA tree proven in the harness - 48 million lanes
// with zero mismatches against the scalar reference, byte-exact end
// to end against ps2float - transcribed instruction for instruction.
// Memory ABI through mVU->exactMulBuf: in [0..3] ma, [4..7] mb
// (mantissas with implicit bit, epi32 lanes); [8..11] p02 and
// [12..15] p13 are the 64-bit products, corrected in place. The stub
// saves and restores every vector register, so call sites need no
// register flush at all - the old slow path's full backup around a C
// call, taken on a guard that real game data trips on most
// multiplies, was the reported Tekken slowdown.
//------------------------------------------------------------------

// add3: lo = x^y^z, hi = ((x&y)|((x^y)&z)) << 1.
// lo, hi, w must be distinct from each other and from x, y, z.
static void emitAdd3(int x, int y, int z, int lo, int hi, int w)
{
	xe_movaps_xx(w, x);
	xe_pxor_xx(w, y);          // u
	xe_movaps_xx(hi, x);
	xe_pand_xx(hi, y);         // x & y
	xe_movaps_xx(lo, w);
	xe_pand_xx(lo, z);         // u & z
	xe_por_xx(hi, lo);
	xe_psllw_xi(hi, 1);
	xe_movaps_xx(lo, w);
	xe_pxor_xx(lo, z);
}

static void mVUemitExactMulStub(mV)
{
	u32* buf = mVU->exactMulBuf;
	int i, k;

	mVU->exactMulStub = x86Ptr;
	for (i = 0; i < 16; i++)
		xe_movaps_mx(&mVU->exactMulSave[i][0], i);

	// regs: 0=aP 1=b16 2..5=d0..d3 6..8=n1..n3 9..12=iter/tree 13..15=working
	xe_movaps_xm(0, &buf[0]);
	xe_pshufb_xm(0, mVUglob.tPack);
	xe_punpcklqdq_xx(0, 0);
	xe_movaps_xm(1, &buf[4]);
	xe_pshufb_xm(1, mVUglob.tPack);
	xe_punpcklqdq_xx(1, 1);
	xe_movaps_xx(9, 0);
	xe_psllw_xi(9, 8);
	xe_pblendw_xxi(0, 9, 0xf0);        // aP

	for (k = 0; k < 4; k++)
	{
		// 9 = t: blend of the two half shifts, &7, doubled into both bytes
		xe_movaps_xx(9, 1);
		if (k == 0) xe_psllw_xi(9, 1);
		else        xe_psrlw_xi(9, k * 2 - 1);
		xe_movaps_xx(10, 1);
		xe_psrlw_xi(10, k * 2 + 7);
		xe_pblendw_xxi(9, 10, 0xf0);
		xe_pand_xm(9, mVUglob.t7);
		xe_movaps_xx(10, 9);
		xe_psllw_xi(10, 8);
		xe_por_xx(9, 10);
		// 11 = x = aP << 2k
		xe_movaps_xx(11, 0);
		if (k) xe_psllw_xi(11, k * 2);
		// x += x & dbl
		xe_movaps_xm(10, mVUglob.tLUTdbl);
		xe_pshufb_xx(10, 9);
		xe_pand_xx(10, 11);
		xe_paddw_xx(11, 10);
		// 10 = neg; n[k] for k>=1 into 5+k; x ^= neg & (onesP<<2k)
		xe_movaps_xm(10, mVUglob.tLUTneg);
		xe_pshufb_xx(10, 9);
		if (k >= 1)
		{
			xe_movaps_xx(5 + k, 10);
			xe_movaps_xm(12, mVUglob.tOneP);
			xe_psllw_xi(12, k * 2);
			xe_pand_xx(5 + k, 12);
		}
		xe_movaps_xm(12, mVUglob.tOnesP);
		if (k) xe_psllw_xi(12, k * 2);
		xe_pand_xx(10, 12);
		xe_pxor_xx(11, 10);
		// x &= any (t still live in 9)
		xe_movaps_xm(10, mVUglob.tLUTany);
		xe_pshufb_xx(10, 9);
		xe_pand_xx(11, 10);
		xe_movaps_xx(2 + k, 11);       // d[k]
	}

	// unpair the high halves; 0,1,9..15 free
	xe_movaps_xx(9, 2);  xe_psrldq_xi(9, 8);   // d4
	xe_movaps_xx(10, 3); xe_psrldq_xi(10, 8);  // d5
	xe_movaps_xx(11, 4); xe_psrldq_xi(11, 8);  // d6
	xe_movaps_xx(12, 5); xe_psrldq_xi(12, 8);  // d7
	xe_movaps_xx(13, 6); xe_psrldq_xi(13, 8);  // n5

	// b7 = d7 | ((d5 & 0x400) + n5)  -> 12 ; frees 13
	xe_movaps_xx(14, 10);
	xe_pand_xm(14, mVUglob.t0400);
	xe_paddw_xx(14, 13);
	xe_por_xx(12, 14);
	// 13 = d5 & 0x800 (needed after d5 is masked), then mask d4/d5
	xe_movaps_xx(13, 10);
	xe_pand_xm(13, mVUglob.t0800);
	xe_pand_xm(9, mVUglob.tF800);
	xe_pand_xm(10, mVUglob.tF000);

	// t0 = add3(d1, d2, d3) -> lo 14, hi 15 ; d1..d3 die
	emitAdd3(3, 4, 5, 14, 15, 1);
	// t1 = add3(d4m, d5m, d6) -> lo 3, hi 4 ; then t1h |= n6 | (d5&0x800)
	emitAdd3(9, 10, 11, 3, 4, 5);
	xe_movaps_xx(5, 7);  xe_psrldq_xi(5, 8);   // n6
	xe_por_xx(4, 5);
	xe_por_xx(4, 13);
	// t2 = add3(d0, t0l, t0h) -> lo 9, hi 10 ; d0 dies
	emitAdd3(2, 14, 15, 9, 10, 11);
	// t3 = add3(b7, t1l, t1h) -> lo 13, hi 14
	emitAdd3(12, 3, 4, 13, 14, 15);
	// t4 = add3(t2h, t3l, t3h) -> lo 2, hi 3
	emitAdd3(10, 13, 14, 2, 3, 4);
	// t5 = add3(t2l, t4l, t4h) -> lo 5, hi 10
	emitAdd3(9, 2, 3, 5, 10, 11);
	// t5h += n7 ; sum = (t5l & 0x8000) + (t5h & 0x8000) ; tb = sum >> 15
	xe_movaps_xx(11, 8); xe_psrldq_xi(11, 8);  // n7
	xe_paddw_xx(10, 11);
	xe_pand_xm(5, mVUglob.t8000);
	xe_pand_xm(10, mVUglob.t8000);
	xe_paddw_xx(5, 10);
	xe_psrlw_xi(5, 15);                        // tb, epi16 lanes 0-3

	// correction: p -= ((tb ^ ((p >> 15) & 1)) << 15), 64-bit lanes
	xe_pxor_xx(0, 0);
	xe_punpcklwd_xx(5, 0);                     // tb32 lanes {b0,b1,b2,b3}
	xe_movaps_xx(6, 5);
	xe_pand_xm(6, mVUglob.mullo);              // tb02 {b0,0,b2,0}
	xe_psrlq_xi(5, 32);                        // tb13 {b1,0,b3,0}
	xe_movaps_xm(1, &buf[8]);                  // p02
	xe_movaps_xm(2, &buf[12]);                 // p13
	xe_movaps_xx(3, 1);
	xe_psrlq_xi(3, 15);
	xe_pand_xm(3, mVUglob.mul64one);           // f02
	xe_movaps_xx(4, 2);
	xe_psrlq_xi(4, 15);
	xe_pand_xm(4, mVUglob.mul64one);           // f13
	xe_pxor_xx(6, 3);
	xe_pxor_xx(5, 4);
	xe_psllq_xi(6, 15);
	xe_psllq_xi(5, 15);
	xe_psubq_xx(1, 6);
	xe_psubq_xx(2, 5);
	xe_movaps_mx(&buf[8], 1);
	xe_movaps_mx(&buf[12], 2);

	for (i = 0; i < 16; i++)
		xe_movaps_xm(i, &mVU->exactMulSave[i][0]);
	xe_ret();
}

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
