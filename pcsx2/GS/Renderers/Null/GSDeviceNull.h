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

#pragma once

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"

class GSTextureNull final : public GSTexture
{
	u32* buffer;
public:
	GSTextureNull(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format);
	~GSTextureNull();

	void* GetNativeHandle() const override { return (void*)buffer; }
	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = NULL, int layer = 0) override;
	void Unmap() override;
};

class GSDeviceNull final : public GSDevice
{
public:
	int m_field = 0;
	GSTexture* CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format) override;
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, const GSVector4& c, const bool linear) final;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb) final;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) final;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants) final;

	GSDeviceNull();
	~GSDeviceNull() override;

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;
	void DestroySurface() override;

	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;
	void SetVSync(VsyncMode mode) override;

	std::string GetDriverInfo() const override;

	void ClearRenderTarget(GSTexture* t, const GSVector4& c) override;
	void ClearRenderTarget(GSTexture* t, u32 c) override;
	void InvalidateRenderTarget(GSTexture* t) override;
	void ClearDepth(GSTexture* t) override;
	void ClearStencil(GSTexture* t, u8 c) override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;

	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvert shader = ShaderConvert::COPY, bool linear = true) override;
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red,
		bool green, bool blue, bool alpha) override;
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		PresentShader shader, float shaderTime, bool linear) override;

	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) override;

	void RenderHW(GSHWDrawConfig& config) override;

	void ClearSamplerCache() override;
};
