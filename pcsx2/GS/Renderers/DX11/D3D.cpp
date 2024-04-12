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
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/GSExtra.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <fstream>

wil::com_ptr_nothrow<IDXGIFactory5> D3D::CreateFactory(bool debug)
{
	UINT flags = 0;
	if (debug)
		flags |= DXGI_CREATE_FACTORY_DEBUG;

	wil::com_ptr_nothrow<IDXGIFactory5> factory;
	const HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(factory.put()));
	if (FAILED(hr))
		Console.Error("D3D: Failed to create DXGI factory: %08X", hr);

	return factory;
}

static std::string FixupDuplicateAdapterNames(const std::vector<std::string>& adapter_names, std::string adapter_name)
{
	if (std::any_of(adapter_names.begin(), adapter_names.end(),
			[&adapter_name](const std::string& other) { return (adapter_name == other); }))
	{
		std::string original_adapter_name = std::move(adapter_name);

		u32 current_extra = 2;
		do
		{
			adapter_name = StringUtil::StdStringFromFormat("%s (%u)", original_adapter_name.c_str(), current_extra);
			current_extra++;
		} while (std::any_of(adapter_names.begin(), adapter_names.end(),
			[&adapter_name](const std::string& other) { return (adapter_name == other); }));
	}

	return adapter_name;
}

std::vector<std::string> D3D::GetAdapterNames(IDXGIFactory5* factory)
{
	std::vector<std::string> adapter_names;

	wil::com_ptr_nothrow<IDXGIAdapter1> adapter;
	for (u32 index = 0;; index++)
	{
		const HRESULT hr = factory->EnumAdapters1(index, adapter.put());
		if (hr == DXGI_ERROR_NOT_FOUND)
			break;

		if (FAILED(hr))
		{
			Console.Error(fmt::format("IDXGIFactory2::EnumAdapters() returned %08X", hr));
			continue;
		}

		adapter_names.push_back(FixupDuplicateAdapterNames(adapter_names, GetAdapterName(adapter.get())));
	}

	return adapter_names;
}

wil::com_ptr_nothrow<IDXGIAdapter1> D3D::GetAdapterByName(IDXGIFactory5* factory, const std::string_view& name)
{
	if (name.empty())
		return {};

	// This might seem a bit odd to cache the names.. but there's a method to the madness.
	// We might have two GPUs with the same name... :)
	std::vector<std::string> adapter_names;

	wil::com_ptr_nothrow<IDXGIAdapter1> adapter;
	for (u32 index = 0;; index++)
	{
		const HRESULT hr = factory->EnumAdapters1(index, adapter.put());
		if (hr == DXGI_ERROR_NOT_FOUND)
			break;

		if (FAILED(hr))
		{
			Console.Error(fmt::format("IDXGIFactory2::EnumAdapters() returned %08X", hr));
			continue;
		}

		std::string adapter_name = FixupDuplicateAdapterNames(adapter_names, GetAdapterName(adapter.get()));
		if (adapter_name == name)
		{
			Console.WriteLn(fmt::format("D3D: Found adapter '{}'", adapter_name));
			return adapter;
		}

		adapter_names.push_back(std::move(adapter_name));
	}

	Console.Warning(fmt::format("Adapter '{}' not found.", name));
	return {};
}

wil::com_ptr_nothrow<IDXGIAdapter1> D3D::GetFirstAdapter(IDXGIFactory5* factory)
{
	wil::com_ptr_nothrow<IDXGIAdapter1> adapter;
	HRESULT hr = factory->EnumAdapters1(0, adapter.put());
	if (FAILED(hr))
		Console.Error(fmt::format("IDXGIFactory2::EnumAdapters() for first adapter returned %08X", hr));

	return adapter;
}

wil::com_ptr_nothrow<IDXGIAdapter1> D3D::GetChosenOrFirstAdapter(IDXGIFactory5* factory, const std::string_view& name)
{
	wil::com_ptr_nothrow<IDXGIAdapter1> adapter = GetAdapterByName(factory, name);
	if (!adapter)
		adapter = GetFirstAdapter(factory);

	return adapter;
}

std::string D3D::GetAdapterName(IDXGIAdapter1* adapter)
{
	std::string ret;

	DXGI_ADAPTER_DESC1 desc;
	HRESULT hr = adapter->GetDesc1(&desc);
	if (SUCCEEDED(hr))
		ret = StringUtil::WideStringToUTF8String(desc.Description);

	if (ret.empty())
		ret = "(Unknown)";

	return ret;
}

D3D::VendorID D3D::GetVendorID(IDXGIAdapter1* adapter)
{
	DXGI_ADAPTER_DESC1 desc;
	const HRESULT hr = adapter->GetDesc1(&desc);
	if (FAILED(hr))
	{
		Console.Error(fmt::format("IDXGIAdapter1::GetDesc() returned {:08X}", hr));
	}
	else
	{
		switch (desc.VendorId)
		{
			case 0x10DE:
				return VendorID::Nvidia;
			case 0x1002:
			case 0x1022:
				return VendorID::AMD;
			case 0x163C:
			case 0x8086:
			case 0x8087:
				return VendorID::Intel;
			default:
				break;
		}
	}
	return VendorID::Unknown;
}

GSRendererType D3D::GetPreferredRenderer(void)
{
	auto factory = CreateFactory(false);
	auto adapter = GetChosenOrFirstAdapter(factory.get(), GSConfig.Adapter);

	// If we somehow can't get a D3D11 device, it's unlikely any of the renderers are going to work.
	if (adapter)
	{
		D3D_FEATURE_LEVEL feature_level;

		static const D3D_FEATURE_LEVEL check[] = {
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_0,
		};

		const HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, std::data(check),
				std::size(check), D3D11_SDK_VERSION, nullptr, &feature_level, nullptr);

		if (SUCCEEDED(hr))
		{
			switch (GetVendorID(adapter.get()))
			{
				case VendorID::Nvidia:
					if (feature_level == D3D_FEATURE_LEVEL_12_0)
						return GSRendererType::VK;
					else if (feature_level == D3D_FEATURE_LEVEL_11_0)
						return GSRendererType::OGL;
					break;

				case VendorID::AMD:
					if (feature_level == D3D_FEATURE_LEVEL_12_0)
						return GSRendererType::VK;
					break;

				case VendorID::Intel:
					// Older Intel GPUs prior to Xe seem to have broken OpenGL drivers which choke
					// on some of our shaders, causing what appears to be GPU timeouts+device removals.
					// Vulkan has broken barriers, also prior to Xe. So just fall back to DX11 everywhere,
					// unless we have Arc, which is easy to identify.
					if (StringUtil::StartsWith(GetAdapterName(adapter.get()), "Intel(R) Arc(TM) "))
						return GSRendererType::VK;
					break;
				default:
					break;
			}
		}
	}

	return GSRendererType::DX11;
}

static unsigned s_next_bad_shader_id = 1;

wil::com_ptr_nothrow<ID3DBlob> D3D::CompileShader(ShaderType type, D3D_FEATURE_LEVEL feature_level, bool debug,
	const std::string_view& code, const D3D_SHADER_MACRO* macros /* = nullptr */, const char* entry_point /* = "main" */)
{
	const char* target;
	switch (feature_level)
	{
		case D3D_FEATURE_LEVEL_10_0:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_4_0", "ps_4_0", "cs_4_0"}};
			target = targets[static_cast<int>(type)];
		}
		break;

		case D3D_FEATURE_LEVEL_10_1:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_4_1", "ps_4_1", "cs_4_1"}};
			target = targets[static_cast<int>(type)];
		}
		break;

		case D3D_FEATURE_LEVEL_11_0:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_5_0", "ps_5_0", "cs_5_0"}};
			target = targets[static_cast<int>(type)];
		}
		break;

		case D3D_FEATURE_LEVEL_11_1:
		default:
		{
			static constexpr std::array<const char*, 4> targets = {{"vs_5_1", "ps_5_1", "cs_5_1"}};
			target = targets[static_cast<int>(type)];
		}
		break;
	}

	static constexpr UINT flags_non_debug = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	static constexpr UINT flags_debug = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;

	wil::com_ptr_nothrow<ID3DBlob> blob;
	wil::com_ptr_nothrow<ID3DBlob> error_blob;
	const HRESULT hr =
		D3DCompile(code.data(), code.size(), "0", macros, nullptr, entry_point, target, debug ? flags_debug : flags_non_debug,
			0, blob.put(), error_blob.put());

	std::string error_string;
	if (error_blob)
	{
		error_string.append(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
		error_blob.reset();
	}

	if (FAILED(hr))
	{
		Console.WriteLn("Failed to compile '%s':\n%s", target, error_string.c_str());

		std::ofstream ofs(StringUtil::StdStringFromFormat("pcsx2_bad_shader_%u.txt", s_next_bad_shader_id++).c_str(),
			std::ofstream::out | std::ofstream::binary);
		if (ofs.is_open())
		{
			ofs << code;
			ofs << "\n\nCompile as " << target << " failed: " << hr << "\n";
			ofs.write(error_string.c_str(), error_string.size());
			ofs.close();
		}

		return {};
	}

	if (!error_string.empty())
		Console.Warning("'%s' compiled with warnings:\n%s", target, error_string.c_str());

	return blob;
}
