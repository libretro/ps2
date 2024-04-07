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

#include "GS/Renderers/Null/GSDeviceNull.h"
#include "GS.h"

#include <libretro.h>
extern retro_video_refresh_t video_cb;

GSTextureNull::GSTextureNull(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
    buffer = (u32*)calloc(640 * 448, 4);
    m_size.x = width;
    m_size.y = height;
}
GSTextureNull::~GSTextureNull()
{
    free(buffer);
}
bool GSTextureNull::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
    u32* src = (u32*) data;
//    int field = ((GSDeviceNull*)g_gs_device.get())->m_field;
    int field = (((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1;

    for(int j = field; j < 448; j+=2)
        for(int i = 0; i < 640; i++)
        {
            u32 col = src[i + (j >> 1) * (pitch >> 2)];
            buffer[i + j * 640] = (col & 0xFF00FF00) | (col & 0x00FF0000) >> 16  | (col & 0x000000FF) << 16;
        }

    video_cb(buffer, 640, 448, 640 * 4);

    return true;
}
bool GSTextureNull::Map(GSMap& m, const GSVector4i* r, int layer)
{
    m.bits = (u8*)buffer;
    m.pitch = GetWidth();
    return true;
}
void GSTextureNull::Unmap()
{

}

GSTexture* GSDeviceNull::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
    return new GSTextureNull(type, width, height, levels, format);
}
void GSDeviceNull::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
    const GSRegEXTBUF& EXTBUF, const GSVector4& c, const bool linear)
{

}
void GSDeviceNull::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
    m_field = ((int)cb.ZrH.x & 0x1) ^ 0x1;
}
void GSDeviceNull::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{

}
void GSDeviceNull::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{

}
bool GSDeviceNull::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
    return true;
}
GSDeviceNull::GSDeviceNull()
{

}
GSDeviceNull::~GSDeviceNull()
{

}

RenderAPI GSDeviceNull::GetRenderAPI() const
{
    return RenderAPI::None;
}
bool GSDeviceNull::HasSurface() const
{
    return true;
}
void GSDeviceNull::DestroySurface()
{

}
bool GSDeviceNull::UpdateWindow()
{
    return true;
}
void GSDeviceNull::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{

}
GSDeviceNull::PresentResult GSDeviceNull::BeginPresent(bool frame_skip)
{
    return PresentResult::OK;
}
void GSDeviceNull::EndPresent()
{

}
void GSDeviceNull::SetVSync(VsyncMode mode)
{

}
std::string GSDeviceNull::GetDriverInfo() const
{
    return "";
}

void GSDeviceNull::ClearRenderTarget(GSTexture* t, const GSVector4& c)
{

}
void GSDeviceNull::ClearRenderTarget(GSTexture* t, u32 c)
{

}
void GSDeviceNull::InvalidateRenderTarget(GSTexture* t)
{

}
void GSDeviceNull::ClearDepth(GSTexture* t)
{

}
void GSDeviceNull::ClearStencil(GSTexture* t, u8 c)
{

}
void GSDeviceNull::PushDebugGroup(const char* fmt, ...)
{

}
void GSDeviceNull::PopDebugGroup()
{

}
void GSDeviceNull::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{

}
std::unique_ptr<GSDownloadTexture> GSDeviceNull::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return nullptr;
}
void GSDeviceNull::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{

}
void GSDeviceNull::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool linear)
{

}
void GSDeviceNull::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
	bool green, bool blue, bool alpha)
{

}
void GSDeviceNull::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, bool linear)
{

}
void GSDeviceNull::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{

}
void GSDeviceNull::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{

}
void GSDeviceNull::RenderHW(GSHWDrawConfig& config)
{

}
void GSDeviceNull::ClearSamplerCache()
{

}
