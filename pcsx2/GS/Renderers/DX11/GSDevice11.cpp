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
#include "GS.h"
#include "GSDevice11.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/GSExtra.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "ShaderCacheVersion.h"

#include "common/Align.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <VersionHelpers.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>

#include "libretro_d3d.h"
extern retro_environment_t environ_cb;

static bool SupportsTextureFormat(ID3D11Device* dev, DXGI_FORMAT format)
{
	UINT support;
	if (FAILED(dev->CheckFormatSupport(format, &support)))
		return false;

	return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

GSDevice11::GSDevice11()
{
	memset(&m_state, 0, sizeof(m_state));

	m_state.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_state.bf = -1;

	m_features.primitive_id = true;
	m_features.texture_barrier = false;
	m_features.provoking_vertex_last = false;
	m_features.point_expand = false;
	m_features.line_expand = false;
	m_features.prefer_new_textures = false;
	m_features.dxt_textures = false;
	m_features.bptc_textures = false;
	m_features.framebuffer_fetch = false;
	m_features.dual_source_blend = true;
	m_features.stencil_buffer = true;
	m_features.clip_control = true;
	m_features.test_and_sample_depth = false;
}

GSDevice11::~GSDevice11() = default;

RenderAPI GSDevice11::GetRenderAPI() const
{
	return RenderAPI::D3D11;
}

bool GSDevice11::Create()
{
	if (!GSDevice::Create())
		return false;
	retro_hw_render_interface_d3d11 *d3d11 = nullptr;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&d3d11) || !d3d11) {
		printf("Failed to get HW rendering interface!\n");
		return false;
	}

	if (d3d11->interface_version != RETRO_HW_RENDER_INTERFACE_D3D11_VERSION) {
		printf("HW render interface mismatch, expected %u, got %u!\n", RETRO_HW_RENDER_INTERFACE_D3D11_VERSION, d3d11->interface_version);
		return false;
	}

	if (FAILED(d3d11->device->QueryInterface(&m_dev)) || FAILED(d3d11->context->QueryInterface(&m_ctx)))
	{
		Console.Error("Direct3D 11.1 is required and not supported.");
		return false;
	}
	if (!AcquireWindow(true) || (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain()))
		return false;

	D3D11_BUFFER_DESC bd;
	D3D11_SAMPLER_DESC sd;
	D3D11_DEPTH_STENCIL_DESC dsd;
	D3D11_RASTERIZER_DESC rd;
	D3D11_BLEND_DESC bsd;

	D3D_FEATURE_LEVEL level;

	if (GSConfig.UseDebugDevice)
		m_annotation = m_ctx.try_query<ID3DUserDefinedAnnotation>();
	level = m_dev->GetFeatureLevel();
	const bool support_feature_level_11_0 = (level >= D3D_FEATURE_LEVEL_11_0);

	if (!GSConfig.DisableShaderCache)
	{
		if (!m_shader_cache.Open(EmuFolders::Cache, m_dev->GetFeatureLevel(), SHADER_CACHE_VERSION, GSConfig.UseDebugDevice))
			Console.Warning("Shader cache failed to open.");
	}
	else
	{
		m_shader_cache.Open({}, m_dev->GetFeatureLevel(), SHADER_CACHE_VERSION, GSConfig.UseDebugDevice);
		Console.WriteLn("Not using shader cache.");
	}

	// Set maximum texture size limit based on supported feature level.
	if (support_feature_level_11_0)
		m_d3d_texsize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	else
		m_d3d_texsize = D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

	{
		// HACK: check AMD
		// Broken point sampler should be enabled only on AMD.
		wil::com_ptr_nothrow<IDXGIDevice> dxgi_device;
		wil::com_ptr_nothrow<IDXGIAdapter1> dxgi_adapter;
		if (SUCCEEDED(m_dev->QueryInterface(dxgi_device.put())) &&
			SUCCEEDED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.put()))))
			m_features.broken_point_sampler = (D3D::GetVendorID(dxgi_adapter.get()) == D3D::VendorID::AMD);
	}

	SetFeatures();

	std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/dx11/tfx.fx");
	if (!shader.has_value())
		return false;
	m_tfx_source = std::move(*shader);

	// convert

	D3D11_INPUT_ELEMENT_DESC il_convert[] =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};

	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());

	std::optional<std::string> convert_hlsl = Host::ReadResourceFileToString("shaders/dx11/convert.fx");
	if (!convert_hlsl.has_value())
		return false;
	if (!m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), m_convert.vs.put(), m_convert.il.put(),
			il_convert, std::size(il_convert), *convert_hlsl, sm_model.GetPtr(), "vs_main"))
	{
		return false;
	}

	for (size_t i = 0; i < std::size(m_convert.ps); i++)
	{
		m_convert.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *convert_hlsl, sm_model.GetPtr(), shaderName(static_cast<ShaderConvert>(i)));
		if (!m_convert.ps[i])
			return false;
	}

	shader = Host::ReadResourceFileToString("shaders/dx11/present.fx");
	if (!shader.has_value())
		return false;
	if (!m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), m_present.vs.put(), m_present.il.put(),
			il_convert, std::size(il_convert), *shader, sm_model.GetPtr(), "vs_main"))
	{
		return false;
	}

	for (size_t i = 0; i < std::size(m_present.ps); i++)
	{
		m_present.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), shaderName(static_cast<PresentShader>(i)));
		if (!m_present.ps[i])
			return false;
	}

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(DisplayConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_present.ps_cb.put());

	memset(&dsd, 0, sizeof(dsd));

	m_dev->CreateDepthStencilState(&dsd, m_convert.dss.put());

	dsd.DepthEnable = true;
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;

	m_dev->CreateDepthStencilState(&dsd, m_convert.dss_write.put());

	memset(&bsd, 0, sizeof(bsd));

	for (u32 i = 0; i < static_cast<u32>(m_convert.bs.size()); i++)
	{
		bsd.RenderTarget[0].RenderTargetWriteMask = static_cast<u8>(i);
		m_dev->CreateBlendState(&bsd, m_convert.bs[i].put());
	}

	// merge

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(MergeConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_merge.cb.put());

	shader = Host::ReadResourceFileToString("shaders/dx11/merge.fx");
	if (!shader.has_value())
		return false;

	for (size_t i = 0; i < std::size(m_merge.ps); i++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_main%d", i));
		m_merge.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), entry_point.c_str());
		if (!m_merge.ps[i])
			return false;
	}

	memset(&bsd, 0, sizeof(bsd));

	bsd.RenderTarget[0].BlendEnable = true;
	bsd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bsd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bsd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bsd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	m_dev->CreateBlendState(&bsd, m_merge.bs.put());

	// interlace

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(InterlaceConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_interlace.cb.put());

	shader = Host::ReadResourceFileToString("shaders/dx11/interlace.fx");
	if (!shader.has_value())
		return false;
	for (size_t i = 0; i < std::size(m_interlace.ps); i++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_main%d", i));
		m_interlace.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), entry_point.c_str());
		if (!m_interlace.ps[i])
			return false;
	}

	// Vertex/Index Buffer
	bd = {};
	bd.ByteWidth = VERTEX_BUFFER_SIZE;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_vb.put())))
	{
		Console.Error("Failed to create vertex buffer.");
		return false;
	}

	bd.ByteWidth = INDEX_BUFFER_SIZE;
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_ib.put())))
	{
		Console.Error("Failed to create index buffer.");
		return false;
	}
	IASetIndexBuffer(m_ib.get());

	if (m_features.vs_expand)
	{
		bd.ByteWidth = VERTEX_BUFFER_SIZE;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.StructureByteStride = sizeof(GSVertex);
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_expand_vb.put())))
		{
			Console.Error("Failed to create expand vertex buffer.");
			return false;
		}

		const CD3D11_SHADER_RESOURCE_VIEW_DESC vb_srv_desc(
			D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN, 0, VERTEX_BUFFER_SIZE / sizeof(GSVertex));
		if (FAILED(m_dev->CreateShaderResourceView(m_expand_vb.get(), &vb_srv_desc, m_expand_vb_srv.put())))
		{
			Console.Error("Failed to create expand vertex buffer SRV.");
			return false;
		}

		m_ctx->VSSetShaderResources(0, 1, m_expand_vb_srv.addressof());

		bd.ByteWidth = EXPAND_BUFFER_SIZE;
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		bd.StructureByteStride = 0;
		bd.MiscFlags = 0;

		std::unique_ptr<u8[]> expand_data = std::make_unique<u8[]>(EXPAND_BUFFER_SIZE);
		GenerateExpansionIndexBuffer(expand_data.get());

		const D3D11_SUBRESOURCE_DATA srd = {expand_data.get()};
		if (FAILED(m_dev->CreateBuffer(&bd, &srd, m_expand_ib.put())))
		{
			Console.Error("Failed to create expand index buffer.");
			return false;
		}
	}

	//

	memset(&rd, 0, sizeof(rd));

	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.FrontCounterClockwise = false;
	rd.DepthBias = false;
	rd.DepthBiasClamp = 0;
	rd.SlopeScaledDepthBias = 0;
	rd.DepthClipEnable = false; // ???
	rd.ScissorEnable = true;
	rd.MultisampleEnable = false;
	rd.AntialiasedLineEnable = false;

	m_dev->CreateRasterizerState(&rd, m_rs.put());
	m_ctx->RSSetState(m_rs.get());

	//

	memset(&sd, 0, sizeof(sd));

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MinLOD = -FLT_MAX;
	sd.MaxLOD = FLT_MAX;
	sd.MaxAnisotropy = 1;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

	m_dev->CreateSamplerState(&sd, m_convert.ln.put());

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	m_dev->CreateSamplerState(&sd, m_convert.pt.put());

	//

	CreateTextureFX();

	//

	memset(&dsd, 0, sizeof(dsd));

	dsd.DepthEnable = false;
	dsd.StencilEnable = true;
	dsd.StencilReadMask = 1;
	dsd.StencilWriteMask = 1;
	dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsd.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	dsd.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;

	m_dev->CreateDepthStencilState(&dsd, m_date.dss.put());

	D3D11_BLEND_DESC blend;

	memset(&blend, 0, sizeof(blend));

	m_dev->CreateBlendState(&blend, m_date.bs.put());

	for (size_t i = 0; i < std::size(m_date.primid_init_ps); i++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_stencil_image_init_%d", i));
		m_date.primid_init_ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *convert_hlsl, sm_model.GetPtr(), entry_point.c_str());
		if (!m_date.primid_init_ps[i])
			return false;
	}

	return true;
}

void GSDevice11::Destroy()
{
	GSDevice::Destroy();
	DestroySwapChain();
	ReleaseWindow();

	m_convert = {};
	m_present = {};
	m_merge = {};
	m_interlace = {};
	m_date = {};
	m_cas = {};

	m_vb.reset();
	m_ib.reset();
	m_expand_vb_srv.reset();
	m_expand_vb.reset();
	m_expand_ib.reset();

	m_vs.clear();
	m_vs_cb.reset();
	m_gs.clear();
	m_ps.clear();
	m_ps_cb.reset();
	m_ps_ss.clear();
	m_om_dss.clear();
	m_om_bs.clear();
	m_rs.reset();

	if (m_state.rt_view)
		m_state.rt_view->Release();
	if (m_state.dsv)
		m_state.dsv->Release();

	m_shader_cache.Close();

	m_annotation.reset();
	m_ctx.reset();
	m_dev.reset();
	m_dxgi_factory.reset();
}

void GSDevice11::SetFeatures()
{
	// Check all three formats, since the feature means any can be used.
	m_features.dxt_textures = SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC1_UNORM) &&
							  SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC2_UNORM) &&
							  SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC3_UNORM);

	m_features.bptc_textures = SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC7_UNORM);

	const D3D_FEATURE_LEVEL feature_level = m_dev->GetFeatureLevel();
	m_features.vs_expand = (feature_level >= D3D_FEATURE_LEVEL_11_0);
}

bool GSDevice11::HasSurface() const
{
	return true;
}

bool GSDevice11::GetHostRefreshRate(float* refresh_rate)
{
	return GSDevice::GetHostRefreshRate(refresh_rate);
}

void GSDevice11::SetVSync(VsyncMode mode)
{
	m_vsync_mode = mode;
}

bool GSDevice11::CreateSwapChain()
{
	if (!CreateSwapChainRTV())
	{
		DestroySwapChain();
		return false;
	}

	return true;
}

bool GSDevice11::CreateSwapChainRTV()
{
	return true;
}

void GSDevice11::DestroySwapChain()
{
}

void GSDevice11::DestroySurface()
{
	DestroySwapChain();
}

std::string GSDevice11::GetDriverInfo() const
{
	std::string ret = "Unknown Feature Level";

	static constexpr std::array<std::tuple<D3D_FEATURE_LEVEL, const char*>, 4> feature_level_names = {{
		{D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_0"},
		{D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_1"},
		{D3D_FEATURE_LEVEL_11_0, "D3D_FEATURE_LEVEL_11_0"},
		{D3D_FEATURE_LEVEL_11_1, "D3D_FEATURE_LEVEL_11_1"},
	}};

	const D3D_FEATURE_LEVEL fl = m_dev->GetFeatureLevel();
	for (size_t i = 0; i < std::size(feature_level_names); i++)
	{
		if (fl == std::get<0>(feature_level_names[i]))
		{
			ret = std::get<1>(feature_level_names[i]);
			break;
		}
	}

	ret += "\n";

	wil::com_ptr_nothrow<IDXGIDevice> dxgi_dev;
	if (m_dev.try_query_to(&dxgi_dev))
	{
		wil::com_ptr_nothrow<IDXGIAdapter> dxgi_adapter;
		if (SUCCEEDED(dxgi_dev->GetAdapter(dxgi_adapter.put())))
		{
			DXGI_ADAPTER_DESC desc;
			if (SUCCEEDED(dxgi_adapter->GetDesc(&desc)))
			{
				ret += StringUtil::StdStringFromFormat("VID: 0x%04X PID: 0x%04X\n", desc.VendorId, desc.DeviceId);
				ret += StringUtil::WideStringToUTF8String(desc.Description);
				ret += "\n";

				const std::string driver_version(D3D::GetDriverVersionFromLUID(desc.AdapterLuid));
				if (!driver_version.empty())
				{
					ret += "Driver Version: ";
					ret += driver_version;
				}
			}
		}
	}

	return ret;
}

GSDevice::PresentResult GSDevice11::BeginPresent(bool frame_skip)
{
	return PresentResult::OK;
}

void GSDevice11::EndPresent()
{
	// clear out the swap chain view, it might get resized..
	OMSetRenderTargets(nullptr, nullptr, nullptr);
}

void GSDevice11::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	PSUpdateShaderState();
	m_ctx->Draw(m_vertex.count, m_vertex.start);
}

void GSDevice11::DrawIndexedPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	PSUpdateShaderState();
	m_ctx->DrawIndexed(m_index.count, m_index.start, m_vertex.start);
}

void GSDevice11::DrawIndexedPrimitive(int offset, int count)
{
	ASSERT(offset + count <= (int)m_index.count);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	PSUpdateShaderState();
	m_ctx->DrawIndexed(count, m_index.start + offset, m_vertex.start);
}

void GSDevice11::ClearRenderTarget(GSTexture* t, const GSVector4& c)
{
	if (!t)
		return;
	m_ctx->ClearRenderTargetView(*(GSTexture11*)t, c.v);
}

void GSDevice11::ClearRenderTarget(GSTexture* t, u32 c)
{
	if (!t)
		return;
	const GSVector4 color = GSVector4::rgba32(c) * (1.0f / 255);

	m_ctx->ClearRenderTargetView(*(GSTexture11*)t, color.v);
}

void GSDevice11::InvalidateRenderTarget(GSTexture* t)
{
	if (t->IsDepthStencil())
		m_ctx->DiscardView(static_cast<ID3D11DepthStencilView*>(*static_cast<GSTexture11*>(t)));
	else
		m_ctx->DiscardView(static_cast<ID3D11RenderTargetView*>(*static_cast<GSTexture11*>(t)));
}

void GSDevice11::ClearDepth(GSTexture* t)
{
	m_ctx->ClearDepthStencilView(*(GSTexture11*)t, D3D11_CLEAR_DEPTH, 0.0f, 0);
}

void GSDevice11::ClearStencil(GSTexture* t, u8 c)
{
	m_ctx->ClearDepthStencilView(*(GSTexture11*)t, D3D11_CLEAR_STENCIL, 0, c);
}

void GSDevice11::PushDebugGroup(const char* fmt, ...)
{
	if (!m_annotation)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	std::string str(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	m_annotation->BeginEvent(StringUtil::UTF8StringToWideString(str).c_str());
}

void GSDevice11::PopDebugGroup()
{
	if (!m_annotation)
		return;

	m_annotation->EndEvent();
}

void GSDevice11::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
	if (!m_annotation)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	std::string str(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	m_annotation->SetMarker(StringUtil::UTF8StringToWideString(str).c_str());
}

GSTexture* GSDevice11::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	D3D11_TEXTURE2D_DESC desc = {};

	// Texture limit for D3D10/11 min 1, max 8192 D3D10, max 16384 D3D11.
	desc.Width = std::clamp(width, 1, m_d3d_texsize);
	desc.Height = std::clamp(height, 1, m_d3d_texsize);
	desc.Format = GSTexture11::GetDXGIFormat(format);
	desc.MipLevels = levels;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;

	switch (type)
	{
		case GSTexture::Type::RenderTarget:
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GSTexture::Type::DepthStencil:
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GSTexture::Type::Texture:
			desc.BindFlags = (levels > 1 && !GSTexture::IsCompressedFormat(format)) ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) : D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = (levels > 1 && !GSTexture::IsCompressedFormat(format)) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
			break;
		case GSTexture::Type::RWTexture:
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			break;
		default:
			break;
	}

	wil::com_ptr_nothrow<ID3D11Texture2D> texture;
	HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, texture.put());
	if (FAILED(hr))
	{
		Console.Error("DX11: Failed to allocate %dx%d surface", width, height);
		return nullptr;
	}

	return new GSTexture11(std::move(texture), desc, type, format);
}

std::unique_ptr<GSDownloadTexture> GSDevice11::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTexture11::Create(width, height, format);
}

void GSDevice11::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	D3D11_BOX box = {(UINT)r.left, (UINT)r.top, 0U, (UINT)r.right, (UINT)r.bottom, 1U};

	// DX api isn't happy if we pass a box for depth copy
	// It complains that depth/multisample must be a full copy
	// and asks us to use a NULL for the box
	const bool depth = (sTex->GetType() == GSTexture::Type::DepthStencil);
	auto pBox = depth ? nullptr : &box;

	m_ctx->CopySubresourceRegion(*(GSTexture11*)dTex, 0, destX, destY, 0, *(GSTexture11*)sTex, 0, pBox);
}

void GSDevice11::CloneTexture(GSTexture* src, GSTexture** dest, const GSVector4i& rect)
{
	pxAssertMsg(src->GetType() == GSTexture::Type::DepthStencil || src->GetType() == GSTexture::Type::RenderTarget, "Source is RT or DS.");

	const int w = src->GetWidth();
	const int h = src->GetHeight();

	if (src->GetType() == GSTexture::Type::DepthStencil)
	{
		// DX11 requires that you copy the entire depth buffer.
		*dest = CreateDepthStencil(w, h, src->GetFormat(), false);
		CopyRect(src, *dest, GSVector4i(0, 0, w, h), 0, 0);
	}
	else
	{
		*dest = CreateRenderTarget(w, h, src->GetFormat(), false);
		CopyRect(src, *dest, rect, rect.left, rect.top);
	}
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{
	pxAssert(dTex->IsDepthStencil() == HasDepthOutput(shader));
	pxAssert(linear ? SupportsBilinear(shader) : SupportsNearest(shader));
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), nullptr, linear);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, bool linear)
{
	StretchRect(sTex, sRect, dTex, dRect, ps, ps_cb, m_convert.bs[D3D11_COLOR_WRITE_ENABLE_ALL].get(), linear);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha)
{
	const u8 index = static_cast<u8>(red) | (static_cast<u8>(green) << 1) | (static_cast<u8>(blue) << 2) |
					 (static_cast<u8>(alpha) << 3);
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[static_cast<int>(ShaderConvert::COPY)].get(), nullptr,
		m_convert.bs[index].get(), false);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, ID3D11BlendState* bs, bool linear)
{
	ASSERT(sTex);

	const bool draw_in_depth = dTex && dTex->IsDepthStencil();

	GSVector2i ds;
	if (dTex)
	{
		ds = dTex->GetSize();
		if (draw_in_depth)
			OMSetRenderTargets(nullptr, dTex);
		else
			OMSetRenderTargets(dTex, nullptr);
	}
	else
	{
		ds = GSVector2i(m_window_info.surface_width, m_window_info.surface_height);

	}

	// om
	if (draw_in_depth)
		OMSetDepthStencilState(m_convert.dss_write.get(), 0);
	else
		OMSetDepthStencilState(m_convert.dss.get(), 0);

	OMSetBlendState(bs, 0);



	// ia

	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] =
	{
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};



    IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_convert.vs.get(), nullptr);


	// ps

	PSSetShaderResources(sTex, nullptr);
	PSSetSamplerState(linear ? m_convert.ln.get() : m_convert.pt.get());
	PSSetShader(ps, ps_cb);

	//

	DrawPrimitive();

	//

	PSSetShaderResources(nullptr, nullptr);
}

void GSDevice11::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool linear)
{
	ID3D11RenderTargetView *nullView = nullptr;
	m_ctx->OMSetRenderTargets(1, &nullView, nullptr);

	ID3D11ShaderResourceView* srv = *(GSTexture11*)sTex;
	m_ctx->PSSetShaderResources(0, 1, &srv);

	extern retro_video_refresh_t video_cb;
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, sTex->GetWidth(), sTex->GetHeight(), 0);
}

void GSDevice11::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float pad1[3];
		u32 offsetX, offsetY, dOffset;
	};
	const Uniforms cb = {sScale, {}, offsetX, offsetY, dOffset};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float pad1[3];
		u32 SBW, DBW, pad3;
	};

	const Uniforms cb = {sScale, {}, SBW, DBW};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	VSSetShader(m_convert.vs.get(), nullptr);
	PSSetShader(m_convert.ps[static_cast<int>(shader)].get(), nullptr);

	OMSetDepthStencilState(dTex->IsRenderTarget() ? m_convert.dss.get() : m_convert.dss_write.get(), 0);
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr);

	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, ds);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, ds);
}

void GSDevice11::DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds)
{
	// Don't use primitive restart here, it ends up slower on some drivers.
	const u32 vertex_reserve_size = num_rects * 4;
	const u32 index_reserve_size = num_rects * 6;
	GSVertexPT1* verts = static_cast<GSVertexPT1*>(IAMapVertexBuffer(sizeof(GSVertexPT1), vertex_reserve_size));
	u16* idx = IAMapIndexBuffer(index_reserve_size);
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	IAUnmapVertexBuffer(sizeof(GSVertexPT1), vcount);
	IAUnmapIndexBuffer(icount);
	IASetIndexBuffer(m_ib.get());

	PSSetShaderResource(0, rects[0].src);
	PSSetSamplerState(rects[0].linear ? m_convert.ln.get() : m_convert.pt.get());

	OMSetBlendState(m_convert.bs[rects[0].wmask.wrgba].get(), 0.0f);

	DrawIndexedPrimitive();
}


void GSDevice11::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, const GSVector4& c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;

	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	ClearRenderTarget(dTex, c);

	// Upload constant to select YUV algo, but skip constant buffer update if we don't need it
	if (feedback_write_2 || feedback_write_1 || sTex[0])
	{
		const MergeConstantBuffer cb = {c, EXTBUF.EMODA, EXTBUF.EMODC};
		m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);
	}

	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		StretchRect(sTex[1], sRect[1], dTex, PMODE.SLBG ? dRect[2] : dRect[1], ShaderConvert::COPY, linear);
	}

	// Save 2nd output
	if (feedback_write_2)
	{
		StretchRect(dTex, full_r, sTex[2], dRect[2], m_convert.ps[static_cast<int>(ShaderConvert::YUV)].get(),
			m_merge.cb.get(), nullptr, linear);
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg)
		ClearRenderTarget(dTex, c);

	if (sTex[0])
	{
		// 1st output is enabled. It must be blended
		StretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge.ps[PMODE.MMOD].get(), m_merge.cb.get(), m_merge.bs.get(), linear);
	}

	if (feedback_write_1)
	{
		StretchRect(sTex[0], full_r, sTex[2], dRect[2], m_convert.ps[static_cast<int>(ShaderConvert::YUV)].get(),
			m_merge.cb.get(), nullptr, linear);
	}
}

void GSDevice11::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	m_ctx->UpdateSubresource(m_interlace.cb.get(), 0, nullptr, &cb, 0, 0);

	StretchRect(sTex, sRect, dTex, dRect, m_interlace.ps[static_cast<int>(shader)].get(), m_interlace.cb.get(), linear);
}

void GSDevice11::SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, bool datm)
{
	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows

	ClearStencil(ds, 0);

	// om

	OMSetDepthStencilState(m_date.dss.get(), 1);
	OMSetBlendState(m_date.bs.get(), 0);
	OMSetRenderTargets(nullptr, ds);

	// ia

	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_convert.vs.get(), nullptr);

	// ps
	PSSetShaderResources(rt, nullptr);
	PSSetSamplerState(m_convert.pt.get());
	PSSetShader(m_convert.ps[static_cast<int>(datm ? ShaderConvert::DATM_1 : ShaderConvert::DATM_0)].get(), nullptr);

	//

	DrawPrimitive();

	//
}

void* GSDevice11::IAMapVertexBuffer(u32 stride, u32 count)
{
	const u32 size = stride * count;
	if (size > VERTEX_BUFFER_SIZE)
		return nullptr;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_vertex.start = (m_vb_pos + (stride - 1)) / stride;
	m_vb_pos = (m_vertex.start * stride) + size;
	if (m_vb_pos > VERTEX_BUFFER_SIZE)
	{
		m_vertex.start = 0;
		m_vb_pos = size;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_vb.get(), 0, type, 0, &m)))
		return nullptr;

	return static_cast<u8*>(m.pData) + (m_vertex.start * stride);
}

void GSDevice11::IAUnmapVertexBuffer(u32 stride, u32 count)
{
	m_ctx->Unmap(m_vb.get(), 0);

	if (m_state.vb_stride != stride)
	{
		m_state.vb_stride = stride;
		const UINT vb_offset = 0;
		m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &stride, &vb_offset);
	}

	m_vertex.count = count;
}

bool GSDevice11::IASetVertexBuffer(const void* vertex, u32 stride, u32 count)
{
	void* map = IAMapVertexBuffer(stride, count);
	if (!map)
		return false;

	GSVector4i::storent(map, vertex, count * stride);

	IAUnmapVertexBuffer(stride, count);
	return true;
}

bool GSDevice11::IASetExpandVertexBuffer(const void* vertex, u32 stride, u32 count)
{
	const u32 size = stride * count;
	if (size > VERTEX_BUFFER_SIZE)
		return false;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_vertex.start = (m_structured_vb_pos + (stride - 1)) / stride;
	m_structured_vb_pos = (m_vertex.start * stride) + size;
	if (m_structured_vb_pos > VERTEX_BUFFER_SIZE)
	{
		m_vertex.start = 0;
		m_structured_vb_pos = size;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_expand_vb.get(), 0, type, 0, &m)))
		return false;

	void* map = static_cast<u8*>(m.pData) + (m_vertex.start * stride);

	GSVector4i::storent(map, vertex, count * stride);

	m_ctx->Unmap(m_expand_vb.get(), 0);

	m_vertex.count = count;
	return true;
}

u16* GSDevice11::IAMapIndexBuffer(u32 count)
{
	if (count > (INDEX_BUFFER_SIZE / sizeof(u16)))
		return nullptr;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_index.start = m_ib_pos;
	m_ib_pos += count;

	if (m_ib_pos > (INDEX_BUFFER_SIZE / sizeof(u16)))
	{
		m_index.start = 0;
		m_ib_pos = count;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_ib.get(), 0, type, 0, &m)))
		return nullptr;

	return static_cast<u16*>(m.pData) + m_index.start;
}

void GSDevice11::IAUnmapIndexBuffer(u32 count)
{
	m_ctx->Unmap(m_ib.get(), 0);
	m_index.count = count;
}

bool GSDevice11::IASetIndexBuffer(const void* index, u32 count)
{
	u16* map = IAMapIndexBuffer(count);
	if (!map)
		return false;

	std::memcpy(map, index, count * sizeof(u16));
	IAUnmapIndexBuffer(count);
	IASetIndexBuffer(m_ib.get());
	return true;
}

void GSDevice11::IASetIndexBuffer(ID3D11Buffer* buffer)
{
	if (m_state.index_buffer != buffer)
	{
		m_ctx->IASetIndexBuffer(buffer, DXGI_FORMAT_R16_UINT, 0);
		m_state.index_buffer = buffer;
	}
}

void GSDevice11::IASetInputLayout(ID3D11InputLayout* layout)
{
	if (m_state.layout != layout)
	{
		m_state.layout = layout;

		m_ctx->IASetInputLayout(layout);
	}
}

void GSDevice11::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	if (m_state.topology != topology)
	{
		m_state.topology = topology;

		m_ctx->IASetPrimitiveTopology(topology);
	}
}

void GSDevice11::VSSetShader(ID3D11VertexShader* vs, ID3D11Buffer* vs_cb)
{
	if (m_state.vs != vs)
	{
		m_state.vs = vs;

		m_ctx->VSSetShader(vs, nullptr, 0);
	}

	if (m_state.vs_cb != vs_cb)
	{
		m_state.vs_cb = vs_cb;

		m_ctx->VSSetConstantBuffers(0, 1, &vs_cb);
	}
}

void GSDevice11::PSSetShaderResources(GSTexture* sr0, GSTexture* sr1)
{
	PSSetShaderResource(0, sr0);
	PSSetShaderResource(1, sr1);
	PSSetShaderResource(2, nullptr);
}

void GSDevice11::PSSetShaderResource(int i, GSTexture* sr)
{
	m_state.ps_sr_views[i] = sr ? static_cast<ID3D11ShaderResourceView*>(*static_cast<GSTexture11*>(sr)) : nullptr;
}

void GSDevice11::PSSetSamplerState(ID3D11SamplerState* ss0)
{
	m_state.ps_ss[0] = ss0;
}

void GSDevice11::PSSetShader(ID3D11PixelShader* ps, ID3D11Buffer* ps_cb)
{
	if (m_state.ps != ps)
	{
		m_state.ps = ps;

		m_ctx->PSSetShader(ps, nullptr, 0);
	}

	if (m_state.ps_cb != ps_cb)
	{
		m_state.ps_cb = ps_cb;

		m_ctx->PSSetConstantBuffers(0, 1, &ps_cb);
	}
}

void GSDevice11::PSUpdateShaderState()
{
	m_ctx->PSSetShaderResources(0, m_state.ps_sr_views.size(), m_state.ps_sr_views.data());
	m_ctx->PSSetSamplers(0, m_state.ps_ss.size(), m_state.ps_ss.data());
}

void GSDevice11::OMSetDepthStencilState(ID3D11DepthStencilState* dss, u8 sref)
{
	if (m_state.dss != dss || m_state.sref != sref)
	{
		m_state.dss = dss;
		m_state.sref = sref;

		m_ctx->OMSetDepthStencilState(dss, sref);
	}
}

void GSDevice11::OMSetBlendState(ID3D11BlendState* bs, float bf)
{
	if (m_state.bs != bs || m_state.bf != bf)
	{
		m_state.bs = bs;
		m_state.bf = bf;

		const float BlendFactor[] = {bf, bf, bf, 0};

		m_ctx->OMSetBlendState(bs, BlendFactor, 0xffffffff);
	}
}

void GSDevice11::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor)
{
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11DepthStencilView* dsv = nullptr;

	if (rt) rtv = *(GSTexture11*)rt;
	if (ds) dsv = *(GSTexture11*)ds;

	const bool changed = (m_state.rt_view != rtv || m_state.dsv != dsv);
	g_perfmon.Put(GSPerfMon::RenderPasses, static_cast<double>(changed));

	if (m_state.rt_view != rtv)
	{
		if (m_state.rt_view)
			m_state.rt_view->Release();
		if (rtv)
			rtv->AddRef();
		m_state.rt_view = rtv;
	}
	if (m_state.dsv != dsv)
	{
		if (m_state.dsv)
			m_state.dsv->Release();
		if (dsv)
			dsv->AddRef();
		m_state.dsv = dsv;
	}
	if (changed)
		m_ctx->OMSetRenderTargets(1, &rtv, dsv);

	if (rt || ds)
	{
		const GSVector2i size = rt ? rt->GetSize() : ds->GetSize();
		SetViewport(size);
		SetScissor(scissor ? *scissor : GSVector4i::loadh(size));
	}
}

void GSDevice11::SetViewport(const GSVector2i& viewport)
{
	if (m_state.viewport != viewport)
	{
		m_state.viewport = viewport;

		const D3D11_VIEWPORT vp = {
			0.0f, 0.0f, static_cast<float>(viewport.x), static_cast<float>(viewport.y), 0.0f, 1.0f};
		m_ctx->RSSetViewports(1, &vp);
	}
}

void GSDevice11::SetScissor(const GSVector4i& scissor)
{
	static_assert(sizeof(D3D11_RECT) == sizeof(GSVector4i));

	if (!m_state.scissor.eq(scissor))
	{
		m_state.scissor = scissor;
		m_ctx->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&scissor));
	}
}

GSDevice11::ShaderMacro::ShaderMacro(D3D_FEATURE_LEVEL fl)
{
	switch (fl)
	{
	case D3D_FEATURE_LEVEL_10_0:
		mlist.emplace_back("SHADER_MODEL", "0x400");
		break;
	case D3D_FEATURE_LEVEL_10_1:
		mlist.emplace_back("SHADER_MODEL", "0x401");
		break;
	case D3D_FEATURE_LEVEL_11_0:
	default:
		mlist.emplace_back("SHADER_MODEL", "0x500");
		break;
	}
}

void GSDevice11::ShaderMacro::AddMacro(const char* n, int d)
{
	AddMacro(n, std::to_string(d));
}

void GSDevice11::ShaderMacro::AddMacro(const char* n, std::string d)
{
	mlist.emplace_back(n, std::move(d));
}

D3D_SHADER_MACRO* GSDevice11::ShaderMacro::GetPtr(void)
{
	mout.clear();

	for (auto& i : mlist)
		mout.emplace_back(i.name.c_str(), i.def.c_str());

	mout.emplace_back(nullptr, nullptr);
	return (D3D_SHADER_MACRO*)mout.data();
}

static GSDevice11::OMBlendSelector convertSel(GSHWDrawConfig::ColorMaskSelector cm, GSHWDrawConfig::BlendState blend)
{
	GSDevice11::OMBlendSelector out;
	out.wrgba = cm.wrgba;
	if (blend.enable)
	{
		out.blend_enable = true;
		out.blend_src_factor = blend.src_factor;
		out.blend_dst_factor = blend.dst_factor;
		out.blend_op = blend.op;
	}

	return out;
}

/// Checks that we weren't sent things we declared we don't support
/// Clears things we don't support that can be quietly disabled
static void preprocessSel(GSDevice11::PSSelector& sel)
{
	ASSERT(sel.write_rg  == 0); // Not supported, shouldn't be sent
}

void GSDevice11::RenderHW(GSHWDrawConfig& config)
{
	ASSERT(!config.require_full_barrier); // We always specify no support so it shouldn't request this
	preprocessSel(config.ps);

	GSVector2i rtsize = (config.rt ? config.rt : config.ds)->GetSize();

	GSTexture* primid_tex = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		primid_tex = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false);
		StretchRect(config.rt, GSVector4(config.drawarea) / GSVector4(rtsize).xyxy(),
			primid_tex, GSVector4(config.drawarea), m_date.primid_init_ps[config.datm].get(), nullptr, false);
	}
	else if (config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off)
	{
		const GSVector4 src = GSVector4(config.drawarea) / GSVector4(config.ds->GetSize()).xyxy();
		const GSVector4 dst = src * 2.0f - 1.0f;

		GSVertexPT1 vertices[] =
		{
			{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
			{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
			{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
			{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
		};

		SetupDATE(config.rt, config.ds, vertices, config.datm);
	}

	GSTexture* hdr_rt = nullptr;
	if (config.ps.hdr)
	{
		const GSVector4 dRect(config.drawarea);
		const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
		hdr_rt = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::HDRColor);
		// Warning: StretchRect must be called before BeginScene otherwise
		// vertices will be overwritten. Trust me you don't want to do that.
		StretchRect(config.rt, sRect, hdr_rt, dRect, ShaderConvert::HDR_INIT, false);
		g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	}

	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		if (!IASetExpandVertexBuffer(config.verts, sizeof(*config.verts), config.nverts))
		{
			Console.Error("Failed to upload structured vertices (%u)", config.nverts);
			return;
		}

		config.cb_vs.max_depth.y = m_vertex.start;
	}
	else
	{
		if (!IASetVertexBuffer(config.verts, sizeof(*config.verts), config.nverts))
		{
			Console.Error("Failed to upload vertices (%u)", config.nverts);
			return;
		}
	}

	if (config.vs.UseExpandIndexBuffer())
	{
		IASetIndexBuffer(m_expand_ib.get());
		m_index.start = 0;
		m_index.count = config.nindices;
	}
	else
	{
		if (!IASetIndexBuffer(config.indices, config.nindices))
		{
			Console.Error("Failed to upload indices (%u)", config.nindices);
			return;
		}
	}

	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point:    topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;    break;
		case GSHWDrawConfig::Topology::Line:     topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;     break;
		case GSHWDrawConfig::Topology::Triangle: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
	}
	IASetPrimitiveTopology(topology);

	PSSetShaderResources(config.tex, config.pal);

	GSTexture* rt_copy = nullptr;
	if (config.require_one_barrier || (config.tex && config.tex == config.rt)) // Used as "bind rt" flag when texture barrier is unsupported
	{
		// Bind the RT.This way special effect can use it.
		// Do not always bind the rt when it's not needed,
		// only bind it when effects use it such as fbmask emulation currently
		// because we copy the frame buffer and it is quite slow.
		CloneTexture(config.rt, &rt_copy, config.drawarea);
		if (rt_copy)
		{
			if (config.require_one_barrier)
				PSSetShaderResource(2, rt_copy);
			if (config.tex && config.tex == config.rt)
				PSSetShaderResource(0, rt_copy);
		}
	}

	SetupVS(config.vs, &config.cb_vs);
	SetupPS(config.ps, &config.cb_ps, config.sampler);

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		OMDepthStencilSelector dss = config.depth;
		dss.zwe = 0;
		OMBlendSelector blend;
		blend.wrgba = 0;
		blend.wr = 1;
		blend.blend_enable = 1;
		blend.blend_src_factor = CONST_ONE;
		blend.blend_dst_factor = CONST_ONE;
		blend.blend_op = 3; // MIN
		SetupOM(dss, blend, 0);
		OMSetRenderTargets(primid_tex, config.ds, &config.scissor);
		DrawIndexedPrimitive();

		config.ps.date = 3;
		config.alpha_second_pass.ps.date = 3;
		SetupPS(config.ps, nullptr, config.sampler);
		PSSetShaderResource(3, primid_tex);
	}

	SetupOM(config.depth, convertSel(config.colormask, config.blend), config.blend.constant);
	OMSetRenderTargets(hdr_rt ? hdr_rt : config.rt, config.ds, &config.scissor);
	DrawIndexedPrimitive();

	if (config.separate_alpha_pass)
	{
		GSHWDrawConfig::BlendState sap_blend = {};
		SetHWDrawConfigForAlphaPass(&config.ps, &config.colormask, &sap_blend, &config.depth);
		SetupOM(config.depth, convertSel(config.colormask, sap_blend), config.blend.constant);
		SetupPS(config.ps, &config.cb_ps, config.sampler);
		DrawIndexedPrimitive();
	}

	if (config.alpha_second_pass.enable)
	{
		preprocessSel(config.alpha_second_pass.ps);
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetupPS(config.alpha_second_pass.ps, &config.cb_ps, config.sampler);
		}
		else
		{
			// ps cbuffer hasn't changed, so don't bother checking
			SetupPS(config.alpha_second_pass.ps, nullptr, config.sampler);
		}

		SetupOM(config.alpha_second_pass.depth, convertSel(config.alpha_second_pass.colormask, config.blend), config.blend.constant);
		DrawIndexedPrimitive();

		if (config.second_separate_alpha_pass)
		{
			GSHWDrawConfig::BlendState sap_blend = {};
			SetHWDrawConfigForAlphaPass(&config.alpha_second_pass.ps, &config.alpha_second_pass.colormask, &sap_blend, &config.alpha_second_pass.depth);
			SetupOM(config.alpha_second_pass.depth, convertSel(config.alpha_second_pass.colormask, sap_blend), config.blend.constant);
			SetupPS(config.alpha_second_pass.ps, &config.cb_ps, config.sampler);
			DrawIndexedPrimitive();
		}
	}

	if (rt_copy)
		Recycle(rt_copy);
	if (primid_tex)
		Recycle(primid_tex);

	if (hdr_rt)
	{
		const GSVector2i size = config.rt->GetSize();
		const GSVector4 dRect(config.drawarea);
		const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
		StretchRect(hdr_rt, sRect, config.rt, dRect, ShaderConvert::HDR_RESOLVE, false);
		g_perfmon.Put(GSPerfMon::TextureCopies, 1);
		Recycle(hdr_rt);
	}
}

void GSDevice11::ResetAPIState()
{
	// Clear out the GS, since the imgui draw doesn't get rid of it.
	m_ctx->GSSetShader(nullptr, nullptr, 0);
}

void GSDevice11::RestoreAPIState()
{
	const UINT vb_offset = 0;
	m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &m_state.vb_stride, &vb_offset);
	m_ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
	m_ctx->IASetInputLayout(m_state.layout);
	m_ctx->IASetPrimitiveTopology(m_state.topology);
	m_ctx->VSSetShader(m_state.vs, nullptr, 0);
	m_ctx->VSSetConstantBuffers(0, 1, &m_state.vs_cb);
	m_ctx->PSSetShader(m_state.ps, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_state.ps_cb);
	m_ctx->IASetIndexBuffer(m_state.index_buffer, DXGI_FORMAT_R16_UINT, 0);

	const CD3D11_VIEWPORT vp(0.0f, 0.0f,
		static_cast<float>(m_state.viewport.x), static_cast<float>(m_state.viewport.y),
		0.0f, 1.0f);
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&m_state.scissor));
	m_ctx->RSSetState(m_rs.get());

	m_ctx->OMSetDepthStencilState(m_state.dss, m_state.sref);

	const float blend_factors[4] = { m_state.bf, m_state.bf, m_state.bf, m_state.bf };
	m_ctx->OMSetBlendState(m_state.bs, blend_factors, 0xFFFFFFFFu);

	PSUpdateShaderState();

	if (m_state.rt_view)
		m_ctx->OMSetRenderTargets(1, &m_state.rt_view, m_state.dsv);
	else
		m_ctx->OMSetRenderTargets(0, nullptr, m_state.dsv);
}
