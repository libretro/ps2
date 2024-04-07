/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include <vector>

#include "common/Timer.h"
#include "common/Threading.h"

#include "PerformanceMetrics.h"
#include "System.h"

#include "GS.h"
#include "MTVU.h"
#include "VMManager.h"

static const float UPDATE_INTERVAL = 0.5f;

static float s_vertical_frequency = 0.0f;
static float s_internal_fps = 0.0f;
static u32 s_frames_since_last_update = 0;
static Common::Timer s_last_update_time;
static Common::Timer s_last_frame_time;

// internal fps heuristics
static PerformanceMetrics::InternalFPSMethod s_internal_fps_method = PerformanceMetrics::InternalFPSMethod::None;
static u32 s_gs_framebuffer_blits_since_last_update = 0;
static u32 s_gs_privileged_register_writes_since_last_update = 0;

static Threading::ThreadHandle s_cpu_thread_handle;
static u64 s_last_ticks = 0;

struct GSSWThreadStats
{
	Threading::ThreadHandle handle;
	u64 last_cpu_time = 0;
	double usage = 0.0;
	double time = 0.0;
};
std::vector<GSSWThreadStats> s_gs_sw_threads;

static u32 s_presents_since_last_update = 0;

void PerformanceMetrics::Clear()
{
	Reset();

	s_internal_fps = 0.0f;
	s_internal_fps_method = PerformanceMetrics::InternalFPSMethod::None;

}

void PerformanceMetrics::Reset()
{
	s_frames_since_last_update = 0;
	s_gs_framebuffer_blits_since_last_update = 0;
	s_gs_privileged_register_writes_since_last_update = 0;

	s_presents_since_last_update = 0;

	s_last_update_time.Reset();
	s_last_frame_time.Reset();

	for (GSSWThreadStats& stat : s_gs_sw_threads)
		stat.last_cpu_time = stat.handle.GetCPUTime();
}

void PerformanceMetrics::Update(bool gs_register_write, bool fb_blit, bool is_skipping_present)
{
	s_frames_since_last_update++;
	s_gs_privileged_register_writes_since_last_update += static_cast<u32>(gs_register_write);
	s_gs_framebuffer_blits_since_last_update += static_cast<u32>(fb_blit);

	const Common::Timer::Value now_ticks = Common::Timer::GetCurrentValue();
	const Common::Timer::Value ticks_diff = now_ticks - s_last_update_time.GetStartValue();
	const float time = Common::Timer::ConvertValueToSeconds(ticks_diff);
	if (time < UPDATE_INTERVAL)
		return;

	s_last_update_time.ResetTo(now_ticks);

	// prefer privileged register write based framerate detection, it's less likely to have false positives
	if (s_gs_privileged_register_writes_since_last_update > 0 && !EmuConfig.Gamefixes.BlitInternalFPSHack)
	{
		s_internal_fps = static_cast<float>(s_gs_privileged_register_writes_since_last_update) / time;
		s_internal_fps_method = InternalFPSMethod::GSPrivilegedRegister;
	}
	else if (s_gs_framebuffer_blits_since_last_update > 0)
	{
		s_internal_fps = static_cast<float>(s_gs_framebuffer_blits_since_last_update) / time;
		s_internal_fps_method = InternalFPSMethod::DISPFBBlit;
	}
	else
	{
		s_internal_fps = 0;
		s_internal_fps_method = InternalFPSMethod::None;
	}

	s_gs_privileged_register_writes_since_last_update = 0;
	s_gs_framebuffer_blits_since_last_update = 0;

	const u64 ticks = GetCPUTicks();
	const u64 ticks_delta = ticks - s_last_ticks;
	s_last_ticks = ticks;

	const double pct_divider =
		100.0 * (1.0 / ((static_cast<double>(ticks_delta) * static_cast<double>(Threading::GetThreadTicksPerSecond())) /
						   static_cast<double>(GetTickFrequency())));
	const double time_divider = 1000.0 * (1.0 / static_cast<double>(Threading::GetThreadTicksPerSecond())) *
								(1.0 / static_cast<double>(s_frames_since_last_update));

	for (GSSWThreadStats& thread : s_gs_sw_threads)
	{
		const u64 time = thread.handle.GetCPUTime();
		const u64 delta = time - thread.last_cpu_time;
		thread.last_cpu_time = time;
		thread.usage = static_cast<double>(delta) * pct_divider;
		thread.time = static_cast<double>(delta) * time_divider;
	}

	s_frames_since_last_update = 0;
	s_presents_since_last_update = 0;
}

void PerformanceMetrics::OnGPUPresent(float gpu_time)
{
	s_presents_since_last_update++;
}

void PerformanceMetrics::SetCPUThread(Threading::ThreadHandle thread)
{
	s_cpu_thread_handle = std::move(thread);
}

void PerformanceMetrics::SetGSSWThreadCount(u32 count)
{
	s_gs_sw_threads.clear();
	s_gs_sw_threads.resize(count);
}

void PerformanceMetrics::SetGSSWThread(u32 index, Threading::ThreadHandle thread)
{
	s_gs_sw_threads[index].last_cpu_time = thread ? thread.GetCPUTime() : 0;
	s_gs_sw_threads[index].handle = std::move(thread);
}

void PerformanceMetrics::SetVerticalFrequency(float rate)
{
	s_vertical_frequency = rate;
}

PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
	return s_internal_fps_method;
}

float PerformanceMetrics::GetInternalFPS()
{
	return s_internal_fps;
}
