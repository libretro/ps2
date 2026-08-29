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

#include "Profiler.h"

#ifdef ENABLE_PCSX2_PROFILER

#include <stdio.h>
#include <time.h>

namespace PCSX2Profiler
{
	u64 g_zone_ticks[ZONE_COUNT];
	u64 g_zone_calls[ZONE_COUNT];
	int g_current = -1;
	u64 g_last;

	static const char* const s_zone_names[ZONE_COUNT] =
	{
		"IOP exec",
		"VU1",
		"VIF unpack",
		"SPU2 mix",
		"GS transfer",
		"GS vsync",
		"GS gpu wait",
		"GS drawkick",
		"GS regwrite",
		"GS kick tex",
	};

	/* The instrument is not free, and for a zone entered tens of thousands of
	 * times a frame it is a material share of that zone's own number: VIF
	 * unpack is entered ~36500 times per frame, so at ~58 ns of clock reads
	 * per scope it accrues milliseconds of its own overhead. Calibrate once
	 * and report the corrected time alongside the raw, so a hot zone is not
	 * mistaken for expensive work when it is really expensive measurement. */
	static u64 s_scope_overhead_ticks;
	static double s_ns_per_tick;

	static void Calibrate(void)
	{
		const int N = 200000;
		u64 wall0, wall1, tick0, tick1, sink = 0;
		int i;

		/* tick rate: the counter is not a nanosecond clock on either
		 * architecture, so measure it against the real one once. */
		wall0 = NowNs();
		tick0 = NowTicks();
		for (i = 0; i < N; i++)
		{
			const u64 a = NowTicks();
			const u64 b = NowTicks();
			sink += b - a;
		}
		tick1 = NowTicks();
		wall1 = NowNs();

		s_ns_per_tick = (tick1 > tick0) ?
			(double)(wall1 - wall0) / (double)(tick1 - tick0) : 1.0;
		s_scope_overhead_ticks = (tick1 - tick0) / (u64)N;

		if (sink == ~0ull)
			fprintf(stderr, "unreachable\n");
	}

	u64 NowNs(void)
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
	}

	/* Report every 60 frames. The zones nest -- VU1 and GS transfer happen
	 * inside EE exec -- so the columns do not sum to the frame time and are
	 * not meant to: each is "how long is spent here", and the nesting is what
	 * makes EE exec the whole emulated frame minus renderer time. */
	void FrameEnd(void)
	{
		static u64 last_report;
		static u32 frames;
		u64 now;
		int i;

		frames++;
		if ((frames % 60) != 0)
			return;

		if (s_ns_per_tick == 0.0)
			Calibrate();

		now = NowNs();
		if (last_report != 0)
		{
			const double wall_ms = (double)(now - last_report) / 1e6;
			fprintf(stderr, "[profile] 60 frames in %.1f ms wall (%.2f ms/frame)\n",
			        wall_ms, wall_ms / 60.0);
			double measured_ms = 0.0;
			double measured_overhead_ms = 0.0;
			for (i = 0; i < ZONE_COUNT; i++)
			{
				const double ms = (double)g_zone_ticks[i] * s_ns_per_tick / 1e6;
				measured_ms += ms;
				const double overhead_ms =
					(double)(g_zone_calls[i] * s_scope_overhead_ticks) * s_ns_per_tick / 1e6;
				const double net_ms = ms > overhead_ms ? ms - overhead_ms : 0.0;
				measured_overhead_ms += overhead_ms;
				fprintf(stderr, "[profile]   %-12s %8.2f ms raw  %8.2f ms net  %6.2f%%  %10llu calls  %7.0f ns/call\n",
				        s_zone_names[i], ms, net_ms,
				        wall_ms > 0.0 ? 100.0 * net_ms / wall_ms : 0.0,
				        (unsigned long long)g_zone_calls[i],
				        g_zone_calls[i] ? (double)g_zone_ticks[i] * s_ns_per_tick / (double)g_zone_calls[i] : 0.0);
			}

			/* Everything not inside a zone: EE dispatch and recompiled EE
			 * code, DMA, counters, and the frontend. The EE is not scoped
			 * directly because the libretro core enters VMManager::Execute
			 * once and stays there, so a scope around it would span the whole
			 * session rather than a frame. */
			fprintf(stderr, "[profile]   %-12s %8.2f ms      %6.2f%%  (derived: EE dispatch, DMA, frontend)\n",
			        "EE + rest", wall_ms - measured_ms,
			        wall_ms > 0.0 ? 100.0 * (wall_ms - measured_ms) / wall_ms : 0.0);
			fprintf(stderr, "[profile]   instrument   %8.2f ms      %6.2f%%  at %llu ns/scope -- subtracted from the net column\n",
			        measured_overhead_ms,
			        wall_ms > 0.0 ? 100.0 * measured_overhead_ms / wall_ms : 0.0,
			        (unsigned long long)((double)s_scope_overhead_ticks * s_ns_per_tick));
		}

		for (i = 0; i < ZONE_COUNT; i++)
		{
			g_zone_ticks[i] = 0;
			g_zone_calls[i] = 0;
		}
		last_report = now;
	}
} // namespace PCSX2Profiler

#endif
