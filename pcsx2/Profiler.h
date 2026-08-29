/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2025 PCSX2 Dev Team
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

/* Where does a frame actually go?
 *
 * Optimization work in this tree has repeatedly been aimed by analogy -- at
 * whatever a different fork profiled, on a different renderer, on a different
 * architecture -- and then measured only after the fact. This is the missing
 * piece: a per-subsystem breakdown of wall time, taken at boundaries that
 * exist for every renderer, every recompiler backend and every platform, so
 * the answer to "what should be optimized" is a measurement rather than an
 * argument.
 *
 * Deliberately not a sampling profiler: those need per-platform thread
 * suspension and cannot attribute recompiled code, which is most of where the
 * time goes. Scoped accumulation at a handful of coarse boundaries costs two
 * clock reads per boundary and attributes JIT code correctly, because the
 * scope brackets the dispatch, not the guest code.
 *
 * Off unless ENABLE_PCSX2_PROFILER is defined: every macro expands to nothing,
 * so an unprofiled build has no accumulators, no clock reads and no branches.
 * Build with -DENABLE_PCSX2_PROFILER to turn it on.
 */

#ifdef ENABLE_PCSX2_PROFILER

#include "common/Pcsx2Types.h"

#if defined(_M_X86) || defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace PCSX2Profiler
{
	enum Zone
	{
		ZONE_IOP_EXEC,    /* IOP recompiler/interpreter dispatch */
		ZONE_VU1,         /* VU1 microprogram execution */
		ZONE_VIF_UNPACK,  /* VIF unpack of vertex data into VU memory */
		ZONE_SPU2_MIX,    /* SPU2 voice mixing */
		ZONE_GS_TRANSFER, /* GIF packets handed to whichever renderer */
		ZONE_GS_VSYNC,    /* end-of-frame renderer work */
		ZONE_GS_SYNC,     /* blocked waiting for the GPU, not working */
		ZONE_GS_DRAWKICK, /* batched packed-vertex handler inside a transfer */
		ZONE_GS_REGWRITE, /* per-register writes inside a transfer */
		ZONE_COUNT
	};

	extern u64 g_zone_ticks[ZONE_COUNT];
	extern u64 g_zone_calls[ZONE_COUNT];

	/* Zones nest: a VU1 microprogram XGKICKs into GS transfer, and the EE
	 * enters all of them. Charging a zone its whole span would count the
	 * nested time twice and make any derived remainder meaningless -- the
	 * first version of this reported -1.4%. Each zone is charged only the
	 * time when it is innermost, so the columns are exclusive and the
	 * remainder is real. */
	extern int g_current;   /* innermost open zone, -1 for none */
	extern u64 g_last;      /* when the innermost zone last started accruing */

	/* The timestamp has to be cheap, because the hottest zone is entered tens
	 * of thousands of times a frame: with clock_gettime this instrument cost
	 * 42 ns per scope and 12% of a real frame on hardware, which distorts
	 * every share it reports. The cycle counter is a few cycles instead, and
	 * is converted to nanoseconds once at report time against a calibrated
	 * tick rate -- no division on the hot path. */
	static inline u64 NowTicks(void)
	{
#if defined(_M_X86) || defined(__x86_64__) || defined(__i386__)
		return (u64)__rdtsc();
#elif defined(__aarch64__) || defined(_M_ARM64)
		u64 v;
		asm volatile("mrs %0, cntvct_el0" : "=r"(v));
		return v;
#else
		return NowNs();
#endif
	}

	u64 NowNs(void);
	void FrameEnd(void);

	struct Scope
	{
		int prev;
		Zone zone;

		explicit Scope(Zone z) : zone(z)
		{
			const u64 now = NowTicks();
			if (g_current >= 0)
				g_zone_ticks[g_current] += now - g_last;
			prev = g_current;
			g_current = (int)z;
			g_last = now;
			g_zone_calls[z]++;
		}

		~Scope()
		{
			const u64 now = NowTicks();
			g_zone_ticks[zone] += now - g_last;
			g_current = prev;
			g_last = now;
		}
	};
} // namespace PCSX2Profiler

#define PROFILE_SCOPE(z) PCSX2Profiler::Scope profile_scope_##__LINE__(PCSX2Profiler::z)
#define PROFILE_FRAME_END() PCSX2Profiler::FrameEnd()

#else

#define PROFILE_SCOPE(z) do {} while (0)
#define PROFILE_FRAME_END() do {} while (0)

#endif
