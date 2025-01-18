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

#include <algorithm>
#include <cmath>

#include "common/Align.h"
#include "common/Console.h"

#include "GSDevice.h"
#include "../../GS.h"
#include "../../../Host.h"

int SetDATMShader(SetDATM datm)
{
	switch (datm)
	{
		case SetDATM::DATM1_RTA_CORRECTION:
			return static_cast<int>(ShaderConvert::DATM_1_RTA_CORRECTION);
		case SetDATM::DATM0_RTA_CORRECTION:
			return static_cast<int>(ShaderConvert::DATM_0_RTA_CORRECTION);
		case SetDATM::DATM1:
			return static_cast<int>(ShaderConvert::DATM_1);
		case SetDATM::DATM0:
		default:
			break;
	}
	return static_cast<int>(ShaderConvert::DATM_0);
}

const char* shaderName(ShaderConvert value)
{
	switch (value)
	{
			// clang-format off
		case ShaderConvert::COPY:                   return "ps_copy";
		case ShaderConvert::RGBA8_TO_16_BITS:       return "ps_convert_rgba8_16bits";
		case ShaderConvert::DATM_1:                 return "ps_datm1";
		case ShaderConvert::DATM_0:                 return "ps_datm0";
		case ShaderConvert::DATM_1_RTA_CORRECTION:  return "ps_datm1_rta_correction";
		case ShaderConvert::DATM_0_RTA_CORRECTION:  return "ps_datm0_rta_correction";
		case ShaderConvert::HDR_INIT:               return "ps_hdr_init";
		case ShaderConvert::HDR_RESOLVE:            return "ps_hdr_resolve";
		case ShaderConvert::RTA_CORRECTION:         return "ps_rta_correction";
		case ShaderConvert::RTA_DECORRECTION:       return "ps_rta_decorrection";
		case ShaderConvert::TRANSPARENCY_FILTER:    return "ps_filter_transparency";
		case ShaderConvert::FLOAT32_TO_16_BITS:     return "ps_convert_float32_32bits";
		case ShaderConvert::FLOAT32_TO_32_BITS:     return "ps_convert_float32_32bits";
		case ShaderConvert::FLOAT32_TO_RGBA8:       return "ps_convert_float32_rgba8";
		case ShaderConvert::FLOAT32_TO_RGB8:        return "ps_convert_float32_rgba8";
		case ShaderConvert::FLOAT16_TO_RGB5A1:      return "ps_convert_float16_rgb5a1";
		case ShaderConvert::RGBA8_TO_FLOAT32:       return "ps_convert_rgba8_float32";
		case ShaderConvert::RGBA8_TO_FLOAT24:       return "ps_convert_rgba8_float24";
		case ShaderConvert::RGBA8_TO_FLOAT16:       return "ps_convert_rgba8_float16";
		case ShaderConvert::RGB5A1_TO_FLOAT16:      return "ps_convert_rgb5a1_float16";
		case ShaderConvert::RGBA8_TO_FLOAT32_BILN:  return "ps_convert_rgba8_float32_biln";
		case ShaderConvert::RGBA8_TO_FLOAT24_BILN:  return "ps_convert_rgba8_float24_biln";
		case ShaderConvert::RGBA8_TO_FLOAT16_BILN:  return "ps_convert_rgba8_float16_biln";
		case ShaderConvert::RGB5A1_TO_FLOAT16_BILN: return "ps_convert_rgb5a1_float16_biln";
		case ShaderConvert::FLOAT32_TO_FLOAT24:     return "ps_convert_float32_float24";
		case ShaderConvert::DEPTH_COPY:             return "ps_depth_copy";
		case ShaderConvert::DOWNSAMPLE_COPY:        return "ps_downsample_copy";
		case ShaderConvert::RGBA_TO_8I:             return "ps_convert_rgba_8i";
		case ShaderConvert::CLUT_4:                 return "ps_convert_clut_4";
		case ShaderConvert::CLUT_8:                 return "ps_convert_clut_8";
		case ShaderConvert::YUV:                    return "ps_yuv";
			// clang-format on
		default:
			break;
	}
	return "ShaderConvertUnknownShader";
}

std::unique_ptr<GSDevice> g_gs_device;

GSDevice::GSDevice() = default;

GSDevice::~GSDevice()
{
}

void GSDevice::GenerateExpansionIndexBuffer(void* buffer)
{
	static constexpr u32 MAX_INDEX = EXPAND_BUFFER_SIZE / 6 / sizeof(u16);

	u16* idx_buffer = static_cast<u16*>(buffer);
	for (u32 i = 0; i < MAX_INDEX; i++)
	{
		const u32 base = i * 4;
		*(idx_buffer++) = base + 0;
		*(idx_buffer++) = base + 1;
		*(idx_buffer++) = base + 2;
		*(idx_buffer++) = base + 1;
		*(idx_buffer++) = base + 2;
		*(idx_buffer++) = base + 3;
	}
}

int GSDevice::GetMipmapLevelsForSize(int width, int height)
{
	return std::min(static_cast<int>(std::log2(std::max(width, height))) + 1, MAXIMUM_TEXTURE_MIPMAP_LEVELS);
}

bool GSDevice::Create()
{
	return true;
}

void GSDevice::Destroy()
{
	ClearCurrent();
	PurgePool();
}

void GSDevice::AcquireWindow(void)
{
	std::optional<WindowInfo> wi = Host::AcquireRenderWindow();
	m_window_info = std::move(wi.value());
}

void GSDevice::ClearRenderTarget(GSTexture* t, u32 c)
{
	t->SetClearColor(c);
}

void GSDevice::ClearDepth(GSTexture* t, float d)
{
	t->SetClearDepth(d);
}

void GSDevice::InvalidateRenderTarget(GSTexture* t)
{
	t->SetState(GSTexture::State::Invalidated);
}

void GSDevice::ResetAPIState()
{
}

void GSDevice::RestoreAPIState()
{
}

void GSDevice::TextureRecycleDeleter::operator()(GSTexture* const tex)
{
	g_gs_device->Recycle(tex);
}

GSTexture* GSDevice::FetchSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format, bool clear, bool prefer_reuse)
{
	const GSVector2i size(width, height);
	const bool prefer_new_texture = (m_features.prefer_new_textures && type == GSTexture::Type::Texture && !prefer_reuse);
	FastList<GSTexture*>& pool = m_pool[type != GSTexture::Type::Texture];

	GSTexture* t = nullptr;
	auto fallback = pool.end();

	for (auto i = pool.begin(); i != pool.end(); ++i)
	{
		t = *i;

		if (t->GetType() == type && t->GetFormat() == format && t->GetSize() == size && t->GetMipmapLevels() == levels)
		{
			if (!prefer_new_texture || t->GetLastFrameUsed() != m_frame)
			{
				pool.erase(i);
				break;
			}
			else if (fallback == pool.end())
			{
				fallback = i;
			}
		}

		t = nullptr;
	}

	if (!t)
	{
		if (pool.size() >= ((type == GSTexture::Type::Texture) ? MAX_POOLED_TEXTURES : MAX_POOLED_TARGETS) &&
			fallback != pool.end())
		{
			t = *fallback;
			pool.erase(fallback);
		}
		else
		{
			t = CreateSurface(type, width, height, levels, format);
			if (!t)
			{
				Console.Error("GS: Memory allocation failure for %dx%d texture. Purging pool and retrying.", width, height);
				PurgePool();
				if (!t)
				{
					Console.Error("GS: Memory allocation failure for %dx%d texture after purging pool.", width, height);
					return nullptr;
				}
			}
		}
	}

	switch (type)
	{
	case GSTexture::Type::RenderTarget:
		{
			if (clear)
				ClearRenderTarget(t, 0);
			else
				InvalidateRenderTarget(t);
		}
		break;
	case GSTexture::Type::DepthStencil:
		{
			if (clear)
				ClearDepth(t, 0.0f);
			else
				InvalidateRenderTarget(t);
		}
		break;
	default:
		break;
	}

	return t;
}

void GSDevice::Recycle(GSTexture* t)
{
	if (!t)
		return;

	t->SetLastFrameUsed(m_frame);

	FastList<GSTexture*>& pool = m_pool[!t->IsTexture()];
	pool.push_front(t);

	const u32 max_size = t->IsTexture() ? MAX_POOLED_TEXTURES : MAX_POOLED_TARGETS;
	const u32 max_age = t->IsTexture() ? MAX_TEXTURE_AGE : MAX_TARGET_AGE;
	while (pool.size() > max_size)
	{
		// Don't toss when the texture was last used in this frame.
		// Because we're going to need to keep it alive anyway.
		GSTexture* back = pool.back();
		if ((m_frame - back->GetLastFrameUsed()) < max_age)
			break;

		delete back;

		pool.pop_back();
	}
}

void GSDevice::AgePool()
{
	m_frame++;

	// Toss out textures when they're not too-recently used.
	for (u32 pool_idx = 0; pool_idx < m_pool.size(); pool_idx++)
	{
		const u32 max_age = (pool_idx == 0) ? MAX_TEXTURE_AGE : MAX_TARGET_AGE;
		FastList<GSTexture*>& pool = m_pool[pool_idx];
		while (!pool.empty())
		{
			GSTexture* back = pool.back();
			if ((m_frame - back->GetLastFrameUsed()) < max_age)
				break;

			delete back;

			pool.pop_back();
		}
	}
}

void GSDevice::PurgePool()
{
	for (FastList<GSTexture*>& pool : m_pool)
	{
		for (GSTexture* t : pool)
			delete t;
		pool.clear();
	}
}

GSTexture* GSDevice::CreateRenderTarget(int w, int h, GSTexture::Format format, bool clear)
{
	return FetchSurface(GSTexture::Type::RenderTarget, w, h, 1, format, clear, true);
}

GSTexture* GSDevice::CreateDepthStencil(int w, int h, GSTexture::Format format, bool clear)
{
	return FetchSurface(GSTexture::Type::DepthStencil, w, h, 1, format, clear, true);
}

GSTexture* GSDevice::CreateTexture(int w, int h, int mipmap_levels, GSTexture::Format format, bool prefer_reuse /* = false */)
{
	const int levels = mipmap_levels < 0 ? GetMipmapLevelsForSize(w, h) : mipmap_levels;
	return FetchSurface(GSTexture::Type::Texture, w, h, levels, format, false, prefer_reuse);
}

void GSDevice::StretchRect(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{
	StretchRect(sTex, GSVector4(0, 0, 1, 1), dTex, dRect, shader, linear);
}

void GSDevice::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	for (u32 i = 0; i < num_rects; i++)
	{
		const MultiStretchRect& sr = rects[i];
		if (rects[0].wmask.wrgba != 0xf)
		{
			g_gs_device->StretchRect(sr.src, sr.src_rect, dTex, sr.dst_rect, rects[0].wmask.wr,
				rects[0].wmask.wg, rects[0].wmask.wb, rects[0].wmask.wa);
		}
		else
		{
			g_gs_device->StretchRect(sr.src, sr.src_rect, dTex, sr.dst_rect, shader, sr.linear);
		}
	}
}

void GSDevice::SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects)
{
	// Depending on num_rects, insertion sort may be better here.
	std::sort(rects, rects + num_rects, [](const MultiStretchRect& lhs, const MultiStretchRect& rhs) {
		return lhs.src < rhs.src || lhs.linear < rhs.linear;
	});
}

void GSDevice::ClearCurrent()
{
	m_current = nullptr;

	delete m_merge;
	delete m_weavebob;
	delete m_blend;
	delete m_mad;
	delete m_target_tmp;

	m_merge = nullptr;
	m_weavebob = nullptr;
	m_blend = nullptr;
	m_mad = nullptr;
	m_target_tmp = nullptr;
}

void GSDevice::Merge(GSTexture* sTex[3], GSVector4* sRect, GSVector4* dRect, const GSVector2i& fs, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c)
{
	if (ResizeRenderTarget(&m_merge, fs.x, fs.y, false, false))
		DoMerge(sTex, sRect, m_merge, dRect, PMODE, EXTBUF, c, GSConfig.PCRTCOffsets);

	m_current = m_merge;
}

void GSDevice::Interlace(const GSVector2i& ds, int field, int mode, float yoffset)
{
	static int bufIdx = 0;
	float offset = yoffset * static_cast<float>(field);
	offset = GSConfig.DisableInterlaceOffset ? 0.0f : offset;

	auto do_interlace = [this](GSTexture* sTex, GSTexture* dTex, ShaderInterlace shader, bool linear, float yoffset, int bufIdx) {
		const GSVector2i ds_i = dTex->GetSize();
		const GSVector2 ds = GSVector2(static_cast<float>(ds_i.x), static_cast<float>(ds_i.y));

		GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
		GSVector4 dRect = GSVector4(0.0f, yoffset, ds.x, ds.y + yoffset);

		// Select the top or bottom half for MAD buffering.
		if (shader == ShaderInterlace::MAD_BUFFER)
		{
			const float half_size = ds.y * 0.5f;
			if ((bufIdx >> 1) == 1)
				dRect.y += half_size;
			else
				dRect.w -= half_size;
		}

		const InterlaceConstantBuffer cb = {
			GSVector4(static_cast<float>(bufIdx), 1.0f / ds.y, ds.y, MAD_SENSITIVITY)
		};

		DoInterlace(sTex, sRect, dTex, dRect, shader, linear, cb);
	};

	switch (mode)
	{
		case 0: // Weave
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, true, false);
			do_interlace(m_merge, m_weavebob, ShaderInterlace::WEAVE, false, offset, field);
			m_current = m_weavebob;
			break;
		case 1: // Bob
			// Field is reversed here as we are countering the bounce.
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, true, false);
			do_interlace(m_merge, m_weavebob, ShaderInterlace::BOB, true, yoffset * (1 - field), 0);
			m_current = m_weavebob;
			break;
		case 2: // Blend
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, true, false);
			do_interlace(m_merge, m_weavebob, ShaderInterlace::WEAVE, false, offset, field);
			ResizeRenderTarget(&m_blend, ds.x, ds.y, true, false);
			do_interlace(m_weavebob, m_blend, ShaderInterlace::BLEND, false, 0, 0);
			m_current = m_blend;
			break;
		case 3: // FastMAD Motion Adaptive Deinterlacing
			bufIdx++;
			bufIdx &= ~1;
			bufIdx |= field;
			bufIdx &= 3;
			ResizeRenderTarget(&m_mad, ds.x, ds.y * 2.0f, true, false);
			do_interlace(m_merge, m_mad, ShaderInterlace::MAD_BUFFER, false, offset, bufIdx);
			ResizeRenderTarget(&m_weavebob, ds.x, ds.y, true, false);
			do_interlace(m_mad, m_weavebob, ShaderInterlace::MAD_RECONSTRUCT, false, 0, bufIdx);
			m_current = m_weavebob;
			break;
		default:
			m_current = m_merge;
			break;
	}
}

bool GSDevice::ResizeRenderTarget(GSTexture** t, int w, int h, bool preserve_contents, bool recycle)
{
	GSTexture* orig_tex = *t;
	if (orig_tex && orig_tex->GetWidth() == w && orig_tex->GetHeight() == h)
	{
		if (!preserve_contents)
			InvalidateRenderTarget(orig_tex);

		return true;
	}

	const GSTexture::Format fmt = orig_tex ? orig_tex->GetFormat() : GSTexture::Format::Color;
	const bool really_preserve_contents = (preserve_contents && orig_tex);
	GSTexture* new_tex = FetchSurface(GSTexture::Type::RenderTarget, w, h, 1, fmt, !really_preserve_contents, true);

	if (!new_tex)
	{
		Console.WriteLn("%dx%d texture allocation failed in ResizeTexture()", w, h);
		return false;
	}

	if (really_preserve_contents)
	{
		constexpr GSVector4 sRect = GSVector4::cxpr(0, 0, 1, 1);
		const GSVector4 dRect = GSVector4(orig_tex->GetRect());
		StretchRect(orig_tex, sRect, new_tex, dRect, ShaderConvert::COPY, true);
	}

	if (orig_tex)
	{
		if (recycle)
			Recycle(orig_tex);
		else
			delete orig_tex;
	}

	*t = new_tex;
	return true;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#endif

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool GSHWDrawConfig::BlendState::IsEffective(ColorMaskSelector colormask) const
{
	return enable && (((colormask.key & 7u) && (src_factor != GSDevice::CONST_ONE || dst_factor != GSDevice::CONST_ZERO)) || ((colormask.key & 8u) && (src_factor_alpha != GSDevice::CONST_ONE || dst_factor_alpha != GSDevice::CONST_ZERO)));
}

// clang-format off

const HWBlend GSDevice::m_blendMap[81] =
{
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0000: (Cs - Cs)*As + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 0001: (Cs - Cs)*As + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 0002: (Cs - Cs)*As +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0010: (Cs - Cs)*Ad + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 0011: (Cs - Cs)*Ad + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 0012: (Cs - Cs)*Ad +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0020: (Cs - Cs)*F  + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 0021: (Cs - Cs)*F  + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 0022: (Cs - Cs)*F  +  0 ==> 0
	{ BLEND_A_MAX | BLEND_MIX2 , OP_SUBTRACT     , CONST_ONE       , SRC1_COLOR}      , // 0100: (Cs - Cd)*As + Cs ==> Cs*(As + 1) - Cd*As
	{ BLEND_MIX1               , OP_ADD          , SRC1_COLOR      , INV_SRC1_COLOR}  , // 0101: (Cs - Cd)*As + Cd ==> Cs*As + Cd*(1 - As)
	{ BLEND_MIX1               , OP_SUBTRACT     , SRC1_COLOR      , SRC1_COLOR}      , // 0102: (Cs - Cd)*As +  0 ==> Cs*As - Cd*As
	{ BLEND_A_MAX              , OP_SUBTRACT     , CONST_ONE       , DST_ALPHA}       , // 0110: (Cs - Cd)*Ad + Cs ==> Cs*(Ad + 1) - Cd*Ad
	{ 0                        , OP_ADD          , DST_ALPHA       , INV_DST_ALPHA}   , // 0111: (Cs - Cd)*Ad + Cd ==> Cs*Ad + Cd*(1 - Ad)
	{ BLEND_HW5                , OP_SUBTRACT     , DST_ALPHA       , DST_ALPHA}       , // 0112: (Cs - Cd)*Ad +  0 ==> Cs*Ad - Cd*Ad
	{ BLEND_A_MAX | BLEND_MIX2 , OP_SUBTRACT     , CONST_ONE       , CONST_COLOR}     , // 0120: (Cs - Cd)*F  + Cs ==> Cs*(F + 1) - Cd*F
	{ BLEND_MIX1               , OP_ADD          , CONST_COLOR     , INV_CONST_COLOR} , // 0121: (Cs - Cd)*F  + Cd ==> Cs*F + Cd*(1 - F)
	{ BLEND_MIX1               , OP_SUBTRACT     , CONST_COLOR     , CONST_COLOR}     , // 0122: (Cs - Cd)*F  +  0 ==> Cs*F - Cd*F
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0200: (Cs -  0)*As + Cs ==> Cs*(As + 1)
	{ BLEND_ACCU               , OP_ADD          , SRC1_COLOR      , CONST_ONE}       , // 0201: (Cs -  0)*As + Cd ==> Cs*As + Cd
	{ BLEND_NO_REC             , OP_ADD          , SRC1_COLOR      , CONST_ZERO}      , // 0202: (Cs -  0)*As +  0 ==> Cs*As
	{ BLEND_A_MAX | BLEND_HW8  , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0210: (Cs -  0)*Ad + Cs ==> Cs*(Ad + 1)
	{ BLEND_HW3                , OP_ADD          , DST_ALPHA       , CONST_ONE}       , // 0211: (Cs -  0)*Ad + Cd ==> Cs*Ad + Cd
	{ BLEND_HW3                , OP_ADD          , DST_ALPHA       , CONST_ZERO}      , // 0212: (Cs -  0)*Ad +  0 ==> Cs*Ad
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 0220: (Cs -  0)*F  + Cs ==> Cs*(F + 1)
	{ BLEND_ACCU               , OP_ADD          , CONST_COLOR     , CONST_ONE}       , // 0221: (Cs -  0)*F  + Cd ==> Cs*F + Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_COLOR     , CONST_ZERO}      , // 0222: (Cs -  0)*F  +  0 ==> Cs*F
	{ BLEND_MIX3               , OP_ADD          , INV_SRC1_COLOR  , SRC1_COLOR}      , // 1000: (Cd - Cs)*As + Cs ==> Cd*As + Cs*(1 - As)
	{ BLEND_A_MAX | BLEND_MIX1 , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ONE}       , // 1001: (Cd - Cs)*As + Cd ==> Cd*(As + 1) - Cs*As
	{ BLEND_MIX1               , OP_REV_SUBTRACT , SRC1_COLOR      , SRC1_COLOR}      , // 1002: (Cd - Cs)*As +  0 ==> Cd*As - Cs*As
	{ 0                        , OP_ADD          , INV_DST_ALPHA   , DST_ALPHA}       , // 1010: (Cd - Cs)*Ad + Cs ==> Cd*Ad + Cs*(1 - Ad)
	{ BLEND_A_MAX              , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ONE}       , // 1011: (Cd - Cs)*Ad + Cd ==> Cd*(Ad + 1) - Cs*Ad
	{ BLEND_HW5                , OP_REV_SUBTRACT , DST_ALPHA       , DST_ALPHA}       , // 1012: (Cd - Cs)*Ad +  0 ==> Cd*Ad - Cs*Ad
	{ BLEND_MIX3               , OP_ADD          , INV_CONST_COLOR , CONST_COLOR}     , // 1020: (Cd - Cs)*F  + Cs ==> Cd*F + Cs*(1 - F)
	{ BLEND_A_MAX | BLEND_MIX1 , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ONE}       , // 1021: (Cd - Cs)*F  + Cd ==> Cd*(F + 1) - Cs*F
	{ BLEND_MIX1               , OP_REV_SUBTRACT , CONST_COLOR     , CONST_COLOR}     , // 1022: (Cd - Cs)*F  +  0 ==> Cd*F - Cs*F
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 1100: (Cd - Cd)*As + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 1101: (Cd - Cd)*As + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 1102: (Cd - Cd)*As +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 1110: (Cd - Cd)*Ad + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 1111: (Cd - Cd)*Ad + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 1112: (Cd - Cd)*Ad +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 1120: (Cd - Cd)*F  + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 1121: (Cd - Cd)*F  + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 1122: (Cd - Cd)*F  +  0 ==> 0
	{ BLEND_HW4                , OP_ADD          , CONST_ONE       , SRC1_COLOR}      , // 1200: (Cd -  0)*As + Cs ==> Cs + Cd*As
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , SRC1_COLOR}      , // 1201: (Cd -  0)*As + Cd ==> Cd*(1 + As)
	{ BLEND_HW2                , OP_ADD          , DST_COLOR       , SRC1_COLOR}      , // 1202: (Cd -  0)*As +  0 ==> Cd*As
	{ BLEND_HW6                , OP_ADD          , CONST_ONE       , DST_ALPHA}       , // 1210: (Cd -  0)*Ad + Cs ==> Cs + Cd*Ad
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , DST_ALPHA}       , // 1211: (Cd -  0)*Ad + Cd ==> Cd*(1 + Ad)
	{ BLEND_HW5                , OP_ADD          , CONST_ZERO      , DST_ALPHA}       , // 1212: (Cd -  0)*Ad +  0 ==> Cd*Ad
	{ BLEND_HW4                , OP_ADD          , CONST_ONE       , CONST_COLOR}     , // 1220: (Cd -  0)*F  + Cs ==> Cs + Cd*F
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , CONST_COLOR}     , // 1221: (Cd -  0)*F  + Cd ==> Cd*(1 + F)
	{ BLEND_HW2                , OP_ADD          , DST_COLOR       , CONST_COLOR}     , // 1222: (Cd -  0)*F  +  0 ==> Cd*F
	{ BLEND_NO_REC             , OP_ADD          , INV_SRC1_COLOR  , CONST_ZERO}      , // 2000: (0  - Cs)*As + Cs ==> Cs*(1 - As)
	{ BLEND_ACCU               , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ONE}       , // 2001: (0  - Cs)*As + Cd ==> Cd - Cs*As
	{ BLEND_NO_REC             , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ZERO}      , // 2002: (0  - Cs)*As +  0 ==> 0 - Cs*As
	{ BLEND_HW9                , OP_ADD          , INV_DST_ALPHA   , CONST_ZERO}      , // 2010: (0  - Cs)*Ad + Cs ==> Cs*(1 - Ad)
	{ BLEND_HW3                , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ONE}       , // 2011: (0  - Cs)*Ad + Cd ==> Cd - Cs*Ad
	{ 0                        , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ZERO}      , // 2012: (0  - Cs)*Ad +  0 ==> 0 - Cs*Ad
	{ BLEND_NO_REC             , OP_ADD          , INV_CONST_COLOR , CONST_ZERO}      , // 2020: (0  - Cs)*F  + Cs ==> Cs*(1 - F)
	{ BLEND_ACCU               , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ONE}       , // 2021: (0  - Cs)*F  + Cd ==> Cd - Cs*F
	{ BLEND_NO_REC             , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ZERO}      , // 2022: (0  - Cs)*F  +  0 ==> 0 - Cs*F
	{ BLEND_HW4                , OP_SUBTRACT     , CONST_ONE       , SRC1_COLOR}      , // 2100: (0  - Cd)*As + Cs ==> Cs - Cd*As
	{ 0                        , OP_ADD          , CONST_ZERO      , INV_SRC1_COLOR}  , // 2101: (0  - Cd)*As + Cd ==> Cd*(1 - As)
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , SRC1_COLOR}      , // 2102: (0  - Cd)*As +  0 ==> 0 - Cd*As
	{ BLEND_HW6                , OP_SUBTRACT     , CONST_ONE       , DST_ALPHA}       , // 2110: (0  - Cd)*Ad + Cs ==> Cs - Cd*Ad
	{ BLEND_HW7                , OP_ADD          , CONST_ZERO      , INV_DST_ALPHA}   , // 2111: (0  - Cd)*Ad + Cd ==> Cd*(1 - Ad)
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , DST_ALPHA}       , // 2112: (0  - Cd)*Ad +  0 ==> 0 - Cd*Ad
	{ BLEND_HW4                , OP_SUBTRACT     , CONST_ONE       , CONST_COLOR}     , // 2120: (0  - Cd)*F  + Cs ==> Cs - Cd*F
	{ 0                        , OP_ADD          , CONST_ZERO      , INV_CONST_COLOR} , // 2121: (0  - Cd)*F  + Cd ==> Cd*(1 - F)
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , CONST_COLOR}     , // 2122: (0  - Cd)*F  +  0 ==> 0 - Cd*F
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 2200: (0  -  0)*As + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 2201: (0  -  0)*As + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 2202: (0  -  0)*As +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 2210: (0  -  0)*Ad + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 2211: (0  -  0)*Ad + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 2212: (0  -  0)*Ad +  0 ==> 0
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , // 2220: (0  -  0)*F  + Cs ==> Cs
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , // 2221: (0  -  0)*F  + Cd ==> Cd
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , // 2222: (0  -  0)*F  +  0 ==> 0
};
