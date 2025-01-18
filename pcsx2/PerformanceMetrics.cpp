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

#include "../common/Timer.h"

#include "Config.h"
#include "PerformanceMetrics.h"

static Common::Timer s_last_update_time;

// internal fps heuristics
static PerformanceMetrics::InternalFPSMethod s_internal_fps_method = PerformanceMetrics::InternalFPSMethod::None;
static u32 s_gs_framebuffer_blits_since_last_update = 0;
static u32 s_gs_privileged_register_writes_since_last_update = 0;

void PerformanceMetrics::Clear()
{
	Reset();

	s_internal_fps_method = PerformanceMetrics::InternalFPSMethod::None;

}

void PerformanceMetrics::Reset()
{
	s_gs_framebuffer_blits_since_last_update = 0;
	s_gs_privileged_register_writes_since_last_update = 0;

	s_last_update_time.Reset();
}

void PerformanceMetrics::Update(bool gs_register_write, bool fb_blit)
{
	s_gs_privileged_register_writes_since_last_update += static_cast<u32>(gs_register_write);
	s_gs_framebuffer_blits_since_last_update += static_cast<u32>(fb_blit);

	const uint64_t now_ticks  = Common::Timer::GetCurrentValue();
	const uint64_t ticks_diff = now_ticks - s_last_update_time.GetStartValue();
	const float time          = Common::Timer::ConvertValueToSeconds(ticks_diff);
	if (time < 0.5f)
		return;

	s_last_update_time.ResetTo(now_ticks);

	// prefer privileged register write based framerate detection, it's less likely to have false positives
	if (s_gs_privileged_register_writes_since_last_update > 0 && !EmuConfig.Gamefixes.BlitInternalFPSHack)
		s_internal_fps_method = InternalFPSMethod::GSPrivilegedRegister;
	else if (s_gs_framebuffer_blits_since_last_update > 0)
		s_internal_fps_method = InternalFPSMethod::DISPFBBlit;
	else
		s_internal_fps_method = InternalFPSMethod::None;

	s_gs_privileged_register_writes_since_last_update = 0;
	s_gs_framebuffer_blits_since_last_update = 0;
}

PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
	return s_internal_fps_method;
}
