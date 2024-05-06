/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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
#include "MultiISA.h"
#include <xbyak/xbyak_util.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

static Xbyak::util::Cpu s_cpu;

static ProcessorFeatures::VectorISA getCurrentISA()
{
	if (s_cpu.has(Xbyak::util::Cpu::tAVX2) && s_cpu.has(Xbyak::util::Cpu::tBMI1) && s_cpu.has(Xbyak::util::Cpu::tBMI2))
		return ProcessorFeatures::VectorISA::AVX2;
	else if (s_cpu.has(Xbyak::util::Cpu::tAVX))
		return ProcessorFeatures::VectorISA::AVX;
	else if (s_cpu.has(Xbyak::util::Cpu::tSSE41))
		return ProcessorFeatures::VectorISA::SSE4;
	return ProcessorFeatures::VectorISA::None;
}

static ProcessorFeatures getProcessorFeatures()
{
	ProcessorFeatures features = {};
	features.vectorISA         = getCurrentISA();
	features.hasFMA            = s_cpu.has(Xbyak::util::Cpu::tFMA);
	features.hasSlowGather     = false;
	if (features.vectorISA == ProcessorFeatures::VectorISA::AVX2)
	{
		if (s_cpu.has(Xbyak::util::Cpu::tINTEL))
		{
			// Slow on Haswell
			// CPUID data from https://en.wikichip.org/wiki/intel/cpuid
			features.hasSlowGather = s_cpu.displayModel == 0x46 || s_cpu.displayModel == 0x45 || s_cpu.displayModel == 0x3c;
		}
		else
		{
			// Currently no Zen CPUs with fast VPGATHERDD
			// Check https://uops.info/table.html as new CPUs come out for one that doesn't split it into like 40 µops
			// Doing it manually is about 28 µops (8x xmm -> gpr, 6x extr, 8x load, 6x insr)
			features.hasSlowGather = true;
		}
	}
	return features;
}

const ProcessorFeatures g_cpu = getProcessorFeatures();

// Keep init order by defining these here

#include "GSXXH.h"

u64 (&MultiISAFunctions::GSXXH3_64_Long)(const void* data, size_t len) = MULTI_ISA_SELECT(GSXXH3_64_Long);
u32 (&MultiISAFunctions::GSXXH3_64_Update)(void* state, const void* data, size_t len) = MULTI_ISA_SELECT(GSXXH3_64_Update);
u64 (&MultiISAFunctions::GSXXH3_64_Digest)(void* state) = MULTI_ISA_SELECT(GSXXH3_64_Digest);
