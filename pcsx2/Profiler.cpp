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
	u64 g_zone_ns[ZONE_COUNT];
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
	};

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

		now = NowNs();
		if (last_report != 0)
		{
			const double wall_ms = (double)(now - last_report) / 1e6;
			fprintf(stderr, "[profile] 60 frames in %.1f ms wall (%.2f ms/frame)\n",
			        wall_ms, wall_ms / 60.0);
			double measured_ms = 0.0;
			for (i = 0; i < ZONE_COUNT; i++)
			{
				const double ms = (double)g_zone_ns[i] / 1e6;
				measured_ms += ms;
				fprintf(stderr, "[profile]   %-12s %8.2f ms  %6.2f%%  %10llu calls  %7.0f ns/call\n",
				        s_zone_names[i], ms, wall_ms > 0.0 ? 100.0 * ms / wall_ms : 0.0,
				        (unsigned long long)g_zone_calls[i],
				        g_zone_calls[i] ? (double)g_zone_ns[i] / (double)g_zone_calls[i] : 0.0);
			}

			/* Everything not inside a zone: EE dispatch and recompiled EE
			 * code, DMA, counters, and the frontend. The EE is not scoped
			 * directly because the libretro core enters VMManager::Execute
			 * once and stays there, so a scope around it would span the whole
			 * session rather than a frame. */
			fprintf(stderr, "[profile]   %-12s %8.2f ms  %6.2f%%  (derived: EE dispatch, DMA, frontend)\n",
			        "EE + rest", wall_ms - measured_ms,
			        wall_ms > 0.0 ? 100.0 * (wall_ms - measured_ms) / wall_ms : 0.0);
		}

		for (i = 0; i < ZONE_COUNT; i++)
		{
			g_zone_ns[i] = 0;
			g_zone_calls[i] = 0;
		}
		last_report = now;
	}
} // namespace PCSX2Profiler

#endif
