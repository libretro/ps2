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

//------------------------------------------------------------------
// Dispatcher Functions
//------------------------------------------------------------------
static bool mvuNeedsFPCRUpdate(mV)
{
	// always update on the vu1 thread
	if (isVU1 && THREAD_VU1)
		return true;

	// otherwise only emit when it's different to the EE
	return g_sseMXCSR.bitmask != (isVU0 ? g_sseVU0MXCSR.bitmask : g_sseVU1MXCSR.bitmask);
}

// Generates the code for entering/exit recompiled blocks
void mVUdispatcherAB(mV)
{
	mVU.startFunct = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		// = The caller has already put the needed parameters in ecx/edx:
		if (!isVU1) xFastCall((void*)mVUexecuteVU0, arg1reg, arg2reg);
		else        xFastCall((void*)mVUexecuteVU1, arg1reg, arg2reg);

		// Load VU's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(isVU0 ? g_sseVU0MXCSR : g_sseVU1MXCSR);

		// Load Regs
		xMOVAPS (xmmT1, ptr128[&mVU.regs().VI[REG_P].UL]);
		xMOVAPS (xmmPQ, ptr128[&mVU.regs().VI[REG_Q].UL]);
		xMOVDZX (xmmT2, ptr32[&mVU.regs().pending_q]);
		xSHUF.PS(xmmPQ, xmmT1, 0); // wzyx = PPQQ
		//Load in other Q instance
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
		xMOVSS(xmmPQ, xmmT2);
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);

		if (isVU1)
		{
			//Load in other P instance
			xMOVDZX(xmmT2, ptr32[&mVU.regs().pending_p]);
			xPSHUF.D(xmmPQ, xmmPQ, 0x1B);
			xMOVSS(xmmPQ, xmmT2);
			xPSHUF.D(xmmPQ, xmmPQ, 0x1B);
		}

		xMOVAPS(xmmT1, ptr128[&mVU.regs().micro_macflags]);
		xMOVAPS(ptr128[mVU.macFlag], xmmT1);


		xMOVAPS(xmmT1, ptr128[&mVU.regs().micro_clipflags]);
		xMOVAPS(ptr128[mVU.clipFlag], xmmT1);

		xMOV(gprF0, ptr32[&mVU.regs().micro_statusflags[0]]);
		xMOV(gprF1, ptr32[&mVU.regs().micro_statusflags[1]]);
		xMOV(gprF2, ptr32[&mVU.regs().micro_statusflags[2]]);
		xMOV(gprF3, ptr32[&mVU.regs().micro_statusflags[3]]);

		// Jump to Recompiled Code Block
		xJMP(rax);

		mVU.exitFunct = x86Ptr;

		// Load EE's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(g_sseMXCSR);

		// = The first two DWORD or smaller arguments are passed in ECX and EDX registers;
		//              all other arguments are passed right to left.
		if (!isVU1) xFastCall((void*)mVUcleanUpVU0);
		else        xFastCall((void*)mVUcleanUpVU1);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xRET();
}

// Generates the code for resuming/exit xgkick
void mVUdispatcherCD(mV)
{
	mVU.startFunctXG = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		// Load VU's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(isVU0 ? g_sseVU0MXCSR : g_sseVU1MXCSR);

		mVUrestoreRegs(mVU);
		xMOV(gprF0, ptr32[&mVU.regs().micro_statusflags[0]]);
		xMOV(gprF1, ptr32[&mVU.regs().micro_statusflags[1]]);
		xMOV(gprF2, ptr32[&mVU.regs().micro_statusflags[2]]);
		xMOV(gprF3, ptr32[&mVU.regs().micro_statusflags[3]]);

		// Jump to Recompiled Code Block
		xJMP(ptrNative[&mVU.resumePtrXG]);

		mVU.exitFunctXG = x86Ptr;

		// Backup Status Flag (other regs were backed up on xgkick)
		xMOV(ptr32[&mVU.regs().micro_statusflags[0]], gprF0);
		xMOV(ptr32[&mVU.regs().micro_statusflags[1]], gprF1);
		xMOV(ptr32[&mVU.regs().micro_statusflags[2]], gprF2);
		xMOV(ptr32[&mVU.regs().micro_statusflags[3]], gprF3);

		// Load EE's MXCSR state
		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(g_sseMXCSR);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xRET();
}

static void mVUGenerateWaitMTVU(mV)
{
	xAlignCallTarget();
	mVU.waitMTVU = x86Ptr;

	int num_xmms = 0, num_gprs = 0;

	for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
	{
		if (!xRegister32::IsCallerSaved(i) || i == rsp.Id)
			continue;

		// T1 often contains the address we're loading when waiting for VU1.
		// T2 isn't used until afterwards, so don't bother saving it.
		if (i == gprT2.Id)
			continue;

		xPUSH(xRegister64(i));
		num_gprs++;
	}

	for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
	{
		if (!xRegisterSSE::IsCallerSaved(i))
			continue;

		num_xmms++;
	}

	// We need 16 byte alignment on the stack.
	// Since the stack is unaligned at entry to this function, we add 8 when it's even, not odd.
	const int stack_size = (num_xmms * sizeof(u128)) + ((~num_gprs & 1) * sizeof(u64)) + SHADOW_STACK_SIZE;
	int stack_offset = SHADOW_STACK_SIZE;

	if (stack_size > 0)
	{
		xSUB(rsp, stack_size);
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
			stack_offset += sizeof(u128);
		}
	}

	xFastCall((void*)mVUwaitMTVU);

	stack_offset = (num_xmms - 1) * sizeof(u128) + SHADOW_STACK_SIZE;
	for (int i = static_cast<int>(iREGCNT_XMM - 1); i >= 0; i--)
	{
		if (!xRegisterSSE::IsCallerSaved(i))
			continue;

		xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
		stack_offset -= sizeof(u128);
	}
	xADD(rsp, stack_size);

	for (int i = static_cast<int>(iREGCNT_GPR - 1); i >= 0; i--)
	{
		if (!xRegister32::IsCallerSaved(i) || i == rsp.Id)
			continue;

		if (i == gprT2.Id)
			continue;

		xPOP(xRegister64(i));
	}

	xRET();
}

static void mVUGenerateCopyPipelineState(mV)
{
	xAlignCallTarget();
	mVU.copyPLState = x86Ptr;

	if (x86caps.hasAVX2)
	{
		xVMOVAPS(ymm0, ptr[rax]);
		xVMOVAPS(ymm1, ptr[rax + 32u]);
		xVMOVAPS(ymm2, ptr[rax + 64u]);

		xVMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState)], ymm0);
		xVMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 32u], ymm1);
		xVMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 64u], ymm2);

		xVZEROUPPER();
	}
	else
	{
		xMOVAPS(xmm0, ptr[rax]);
		xMOVAPS(xmm1, ptr[rax + 16u]);
		xMOVAPS(xmm2, ptr[rax + 32u]);
		xMOVAPS(xmm3, ptr[rax + 48u]);
		xMOVAPS(xmm4, ptr[rax + 64u]);
		xMOVAPS(xmm5, ptr[rax + 80u]);

		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState)], xmm0);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 16u], xmm1);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 32u], xmm2);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 48u], xmm3);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 64u], xmm4);
		xMOVUPS(ptr[reinterpret_cast<u8*>(&mVU.prog.lpState) + 80u], xmm5);
	}

	xRET();
}

//------------------------------------------------------------------
// Micro VU - Custom Quick Search
//------------------------------------------------------------------

// Generates a custom optimized block-search function
// Note: Structs must be 16-byte aligned! (GCC doesn't guarantee this)
static void mVUGenerateCompareState(mV)
{
	mVU.compareStateF = xGetAlignedCallTarget();

	if (!x86caps.hasAVX2)
	{
		xMOVAPS  (xmm0, ptr32[arg1reg]);
		xPCMP.EQD(xmm0, ptr32[arg2reg]);
		xMOVAPS  (xmm1, ptr32[arg1reg + 0x10]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x10]);
		xPAND    (xmm0, xmm1);

		xMOVMSKPS(eax, xmm0);
		xXOR     (eax, 0xf);
		xForwardJNZ8 exitPoint;

		xMOVAPS  (xmm0, ptr32[arg1reg + 0x20]);
		xPCMP.EQD(xmm0, ptr32[arg2reg + 0x20]);
		xMOVAPS  (xmm1, ptr32[arg1reg + 0x30]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x30]);
		xPAND    (xmm0, xmm1);

		xMOVAPS  (xmm1, ptr32[arg1reg + 0x40]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x40]);
		xMOVAPS  (xmm2, ptr32[arg1reg + 0x50]);
		xPCMP.EQD(xmm2, ptr32[arg2reg + 0x50]);
		xPAND    (xmm1, xmm2);
		xPAND    (xmm0, xmm1);

		xMOVMSKPS(eax, xmm0);
		xXOR(eax, 0xf);

		exitPoint.SetTarget();
	}
	else
	{
		// We have to use unaligned loads here, because the blocks are only 16 byte aligned.
		xVMOVUPS(ymm0, ptr[arg1reg]);
		xVPCMP.EQD(ymm0, ymm0, ptr[arg2reg]);
		xVPMOVMSKB(eax, ymm0);
		xXOR(eax, 0xffffffff);
		xForwardJNZ8 exitPoint;

		xVMOVUPS(ymm0, ptr[arg1reg + 0x20]);
		xVMOVUPS(ymm1, ptr[arg1reg + 0x40]);
		xVPCMP.EQD(ymm0, ymm0, ptr[arg2reg + 0x20]);
		xVPCMP.EQD(ymm1, ymm1, ptr[arg2reg + 0x40]);
		xVPAND(ymm0, ymm0, ymm1);

		xVPMOVMSKB(eax, ymm0);
		xNOT(eax);

		exitPoint.SetTarget();
		xVZEROUPPER();
	}

	xRET();
}

//------------------------------------------------------------------
// Execution Functions
//------------------------------------------------------------------

// Executes for number of cycles
_mVUt void* mVUexecute(u32 startPC, u32 cycles)
{

	microVU& mVU = mVUx;
	u32 vuLimit = vuIndex ? 0x3ff8 : 0xff8;
	mVU.cycles = cycles;
	mVU.totalCycles = cycles;

	xSetPtr(mVU.prog.x86ptr); // Set x86ptr to where last program left off
	return mVUsearchProg<vuIndex>(startPC & vuLimit, (uptr)&mVU.prog.lpState); // Find and set correct program
}

//------------------------------------------------------------------
// Cleanup Functions
//------------------------------------------------------------------

_mVUt void mVUcleanUp()
{
	microVU& mVU = mVUx;

	mVU.prog.x86ptr = x86Ptr;

	if ((xGetPtr() < mVU.prog.x86start) || (xGetPtr() >= mVU.prog.x86end))
		mVUreset(mVU, false);

	mVU.cycles = mVU.totalCycles - mVU.cycles;
	mVU.regs().cycle += mVU.cycles;

	if (!vuIndex || !THREAD_VU1)
	{
		u32 cycles_passed = std::min(mVU.cycles, 3000u) * EmuConfig.Speedhacks.EECycleSkip;
		if (cycles_passed > 0)
		{
			s32 vu0_offset = VU0.cycle - cpuRegs.cycle;
			cpuRegs.cycle += cycles_passed;

			// VU0 needs to stay in sync with the CPU otherwise things get messy
			// So we need to adjust when VU1 skips cycles also
			if (!vuIndex)
				VU0.cycle = cpuRegs.cycle + vu0_offset;
			else
				VU0.cycle += cycles_passed;
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
