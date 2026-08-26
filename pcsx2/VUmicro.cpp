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

#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include <xmmintrin.h>

const struct VUmicroCpu* CpuVU0 = NULL;
const struct VUmicroCpu* CpuVU1 = NULL;

__inline u32 CalculateMinRunCycles(u32 cycles, bool requiresAccurateCycles)
{
	// If we're running an interlocked COP2 operation
	// run for an exact amount of cycles
	if(requiresAccurateCycles)
		return cycles;

	// Allow a minimum of 16 cycles to avoid running small blocks
	// Running a block of like 3 cycles is highly inefficient
	// so while sync isn't tight, it's okay to run ahead a little bit.
	return pcsx2_max_i(16U, cycles);
}

// Executes a Block based on EE delta time
void vucpu_execute_block(const struct VUmicroCpu* cpu, int startUp)
{
	const u32& stat = vuRegs[0].VI[REG_VPU_STAT].UL;
	const int test  = cpu->idx ? 0x100 : 1;

	if (cpu->idx && THREAD_VU1)
	{
		vu1Thread.Get_MTVUChanges();
		return;
	}

	if (!(stat & test))
		return;

	if (startUp)
	{
		cpu->Execute(16U);
	}
	else // Continue Executing
	{
		u64 cycle = cpu->idx ? vuRegs[1].cycle : vuRegs[0].cycle;
		s32 delta = (s32)(u32)(cpuRegs.cycle - cycle);

		if (delta > 0)
			cpu->Execute((delta > 16 ? (u32)delta : 16u));
	}
}

// This function is called by VU0 Macro (COP2) after transferring some
// EE data to VU0's registers. We want to run VU0 Micro right after this
// to ensure that the register is used at the correct time.
// This fixes spinning/hanging in some games like Ratchet and Clank's Intro.
void vucpu_execute_block_jit(const struct VUmicroCpu* cpu, int interlocked)
{
	const u32& stat = vuRegs[0].VI[REG_VPU_STAT].UL;
	constexpr int test = 1;

	if (stat & test) // VU is running
	{ 
		s64 delta = (s64)(u64)(cpuRegs.cycle - vuRegs[0].cycle);
		if (delta > 0)
			cpu->Execute(CalculateMinRunCycles(delta, interlocked)); // Execute the time since the last call
	}
}

/* --------------------------------------------------------------------------
 * Per-op correctness audit exports (dlsym'd by the fpaudit VU harness).
 *
 * The audit runs the same micro program through the actual recompiler and
 * the actual ps2float interpreter -- never through a mirror of either --
 * and diffs the full architectural state. prep loads a program into VU
 * micro memory under the chosen provider (full reset, so the recompiler
 * cache holds exactly this program); exec sets the register file, runs
 * from PC 0 until the E-bit program ends, and reads the state back.
 * regs_io layout, both directions:
 *   VF[0..31] 16 bytes each | VI[0..31].UL 4 bytes each | ACC 16 | Q 4 | P 4
 * -------------------------------------------------------------------------- */
extern "C" __attribute__((visibility("default")))
void ps2_audit_vu_prep(int unit, int use_interp, const unsigned char* prog, unsigned prog_bytes)
{
	const struct VUmicroCpu* cpu = use_interp
		? (unit ? &vucpu_interp_vu1 : &vucpu_interp_vu0)
		: (unit ? &vucpu_rec_vu1    : &vucpu_rec_vu0);
	VURegs* vu = &vuRegs[unit];
	const unsigned cap = unit ? VU1_PROGSIZE : VU0_PROGSIZE;
	cpu->Reset();
	memset(vu->Micro, 0, cap);
	memcpy(vu->Micro, prog, prog_bytes < cap ? prog_bytes : cap);
	cpu->Clear(0, cap);
}

extern "C" __attribute__((visibility("default")))
void ps2_audit_vu_exec(int unit, int use_interp, unsigned char* regs_io, unsigned max_cycles)
{
	const struct VUmicroCpu* cpu = use_interp
		? (unit ? &vucpu_interp_vu1 : &vucpu_interp_vu0)
		: (unit ? &vucpu_rec_vu1    : &vucpu_rec_vu0);
	VURegs* vu = &vuRegs[unit];
	int i;
	unsigned char* p = regs_io;
	for (i = 0; i < 32; i++) { memcpy(&vu->VF[i], p, 16); p += 16; }
	/* VF0 is architecturally (0,0,0,1) */
	vu->VF[0].f.x = 0; vu->VF[0].f.y = 0; vu->VF[0].f.z = 0; vu->VF[0].f.w = 1.0f;
	for (i = 0; i < 32; i++) { memcpy(&vu->VI[i].UL, p, 4); p += 4; }
	vu->VI[0].UL = 0;
	memcpy(&vu->ACC, p, 16); p += 16;
	memcpy(&vu->q.UL, p, 4); p += 4;
	memcpy(&vu->p.UL, p, 4); p += 4;
	vu->VI[REG_TPC].UL = 0;
	vu->cycle = cpuRegs.cycle;
	cpu->SetStartPC(0);
	vuRegs[0].VI[REG_VPU_STAT].UL |= (unit ? 0x100 : 0x001);
	if (getenv("PS2_AUDIT_TRACE"))
		fprintf(stderr, "[ATRACE] unit=%d interp=%d cpu->is_interp=%d micro0=%08x/%08x vf3in=%08x tpc=%d stat=%x\n",
			unit, use_interp, cpu->is_interpreter,
			((u32*)vu->Micro)[0], ((u32*)vu->Micro)[1],
			vu->VF[3].UL[0], vu->VI[REG_TPC].UL, vuRegs[0].VI[REG_VPU_STAT].UL);
	{
		/* Direct-entry runs bypass the EE-side context the dispatcher's
		 * FPCR-update elision assumes: force the unit's rounding mode
		 * around the run and restore the host's after. The interpreter
		 * computes in ps2float integer code and ignores MXCSR. */
		const u32 host_csr = _mm_getcsr();
		if (!use_interp)
			_mm_setcsr((unit ? EmuConfig.Cpu.VU1FPCR : EmuConfig.Cpu.VU0FPCR).bitmask);
		cpu->Execute(max_cycles ? max_cycles : 2048);
		_mm_setcsr(host_csr);
	}
	if (getenv("PS2_AUDIT_TRACE"))
		fprintf(stderr, "[ATRACE] after: vf3=%08x tpc=%d stat=%x\n",
			vu->VF[3].UL[0], vu->VI[REG_TPC].UL, vuRegs[0].VI[REG_VPU_STAT].UL);
	p = regs_io;
	for (i = 0; i < 32; i++) { memcpy(p, &vu->VF[i], 16); p += 16; }
	for (i = 0; i < 32; i++) { memcpy(p, &vu->VI[i].UL, 4); p += 4; }
	memcpy(p, &vu->ACC, 16); p += 16;
	memcpy(p, &vu->q.UL, 4); p += 4;
	memcpy(p, &vu->p.UL, 4); p += 4;
}
