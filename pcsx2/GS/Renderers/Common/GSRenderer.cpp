/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#include <algorithm>
#include <array>
#include <deque>
#include <thread>
#include <mutex>
#include <cmath>

#include "GSRenderer.h"

#include "../../../Host.h"
#include "../../../PerformanceMetrics.h"
#include "../../../Config.h"

std::unique_ptr<GSRenderer> g_gs_renderer;

GSRenderer::GSRenderer() { }

GSRenderer::~GSRenderer() = default;

void GSRenderer::Reset(bool hardware_reset)
{
	// clear the current display texture
	if (hardware_reset)
		g_gs_device->ClearCurrent();

	GSState::Reset(hardware_reset);
}

void GSRenderer::Destroy()
{
}

void GSRenderer::PurgePool()
{
	g_gs_device->PurgePool();
}

void GSRenderer::UpdateRenderFixes()
{
}

bool GSRenderer::Merge(int field)
{
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = { nullptr, nullptr, nullptr };
	float tex_scale[3] = { 0.0f, 0.0f, 0.0f };
	int y_offset[3] = { 0, 0, 0 };
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	PCRTCDisplays.SetVideoMode(GetVideoMode());
	PCRTCDisplays.EnableDisplays(m_regs->PMODE, m_regs->SMODE2, isReallyInterlaced());
	PCRTCDisplays.CheckSameSource();

	if (!PCRTCDisplays.PCRTCDisplays[0].enabled && !PCRTCDisplays.PCRTCDisplays[1].enabled)
		return false;

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (m_regs->DISP[0].DISPFB.DBY != PCRTCDisplays.PCRTCDisplays[0].prevFramebufferReg.DBY) !=
									(m_regs->DISP[1].DISPFB.DBY != PCRTCDisplays.PCRTCDisplays[1].prevFramebufferReg.DBY);

	PCRTCDisplays.SetRects(0, m_regs->DISP[0].DISPLAY, m_regs->DISP[0].DISPFB);
	PCRTCDisplays.SetRects(1, m_regs->DISP[1].DISPLAY, m_regs->DISP[1].DISPFB);
	PCRTCDisplays.CalculateDisplayOffset(m_scanmask_used);
	PCRTCDisplays.CalculateFramebufferOffset(m_scanmask_used);

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		if (PCRTCDisplays.PCRTCDisplays[0].enabled)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (PCRTCDisplays.PCRTCDisplays[1].enabled)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
		return false;

	s_n++;

	GSVector4 src_gs_read[2];
	GSVector4 dst[3];

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	int field2 = 0;
	int mode = 3; // If the game is manually deinterlacing then we need to bob (if we want to get away with no deinterlacing).
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic || (!m_regs->SMODE2.FFMD && !scanmask_frame))
	{
		// If the game is offsetting each frame itself and we're using full height buffers, we can offset this with Bob.
		if (game_deinterlacing && !scanmask_frame && GSConfig.InterlaceMode == GSInterlaceMode::Automatic)
		{
			mode = 1; // Bob.
			is_bob = true;
		}
		else
		{
			field2 = ((static_cast<int>(GSConfig.InterlaceMode) - 2) & 1);
			mode = ((static_cast<int>(GSConfig.InterlaceMode) - 2) >> 1);
		}
	}

	for (int i = 0; i < 2; i++)
	{
		 const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();
		
		float interlace_offset = 0.0f;
		if (isReallyInterlaced() && m_regs->SMODE2.FFMD && !is_bob && !GSConfig.DisableInterlaceOffset && GSConfig.InterlaceMode != GSInterlaceMode::Off)
			interlace_offset = (scale.y) * static_cast<float>(field ^ field2);
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;
			
			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / ((m_regs->EXTDATA.SMPH - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGH) + 1));
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / ((m_regs->EXTDATA.SMPV - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGV) + 1);

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if (isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset);
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

float GSRenderer::GetModXYOffset()
{
	// Scaled Bilinear HPO depending on the gradient.
	if (GSConfig.UserHacks_HalfPixelOffset == 4)
	{
		const GSVector2 pos_range(m_vt.m_max.p.x - m_vt.m_min.p.x, m_vt.m_max.p.y - m_vt.m_min.p.y);
		const GSVector2 uv_range(m_vt.m_max.t.x - m_vt.m_min.t.x, m_vt.m_max.t.y - m_vt.m_min.t.y);
		const GSVector2 grad(uv_range / pos_range);
		const float avg_grad = (grad.x + grad.y) / 2;

		if (avg_grad >= 0.5f && m_draw_env->CTXT[m_draw_env->PRIM.CTXT].TEX1.MMIN == 1)
		{
			float mod_xy = GetUpscaleMultiplier();
			return mod_xy += (mod_xy / 2.0f) * avg_grad;
		}
	}
	else if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal)
	{
		float mod_xy = GetUpscaleMultiplier();
		const int rounded_mod_xy = static_cast<int>(std::round(mod_xy));
		if (rounded_mod_xy > 1)
		{
			if (!(rounded_mod_xy & 1))
				return mod_xy += 0.2f;
			else if (!(rounded_mod_xy & 2))
				return mod_xy += 0.3f;
			return mod_xy += 0.1f;
		}
	}

	return 0.0f;
}

bool GSRenderer::BeginPresentFrame(bool frame_skip)
{
	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
		return false;
	else if (res == GSDevice::PresentResult::OK) /* All good */
		return true;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSConfig))
		return false;

	// First frame after reopening is definitely going to be trash, so skip it.
	return false;
}

void GSRenderer::VSync(u32 field, bool registers_written, bool idle_frame)
{
	bool is_unique_frame       = false;
	const int fb_sprite_blits  = m_disp_fb_sprite_blits;
	const bool fb_sprite_frame = (fb_sprite_blits > 0);
	bool skip_frame            = false;

	m_disp_fb_sprite_blits     = 0;

	if (GSConfig.SkipDuplicateFrames)
	{
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
		case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
			is_unique_frame = registers_written;
			break;
		case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
			is_unique_frame = fb_sprite_frame;
			break;
		default:
			is_unique_frame = true;
			break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
			m_skipped_duplicate_frames = 0;
	}

	const bool blank_frame = !Merge(field);

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	if (skip_frame)
	{
		g_gs_device->ResetAPIState();
		if (BeginPresentFrame(true))
			g_gs_device->EndPresent();
		goto end;
	}

	if (!idle_frame)
		g_gs_device->AgePool();

	g_gs_device->ResetAPIState();
	if (BeginPresentFrame(false))
	{
		GSTexture* current = g_gs_device->GetCurrent();
		if (current && !blank_frame)
		{
			GSVector4 src_uv        = GSVector4(0, 0, 1, 1);
			GSVector4 draw_rect     = GSVector4(0, 0, current->GetWidth(), current->GetHeight());

			g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect);
		}

		g_gs_device->EndPresent();
	}

end:
	g_gs_device->RestoreAPIState();
	PerformanceMetrics::Update(registers_written, fb_sprite_frame);
}

GSTexture* GSRenderer::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return nullptr;
}

bool GSRenderer::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}
