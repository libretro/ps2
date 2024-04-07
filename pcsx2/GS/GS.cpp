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

#include "PrecompiledHeader.h"

#include "GS.h"
#include "GSGL.h"
#include "GSUtil.h"
#include "GSExtra.h"
#include "Renderers/Null/GSRendererNull.h"
#include "Renderers/HW/GSRendererHW.h"
#include "Renderers/HW/GSTextureReplacements.h"
#include "MultiISA.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "pcsx2/Config.h"
#include "pcsx2/Counters.h"
#include "pcsx2/Host.h"
#include "pcsx2/HostSettings.h"
#include "pcsx2/Frontend/InputManager.h"
#include "pcsx2/GS.h"

#include "fmt/format.h"

#ifdef ENABLE_OPENGL
#include "Renderers/OpenGL/GSDeviceOGL.h"
#endif

#ifdef __APPLE__
#include "Renderers/Metal/GSMetalCPPAccessible.h"
#endif

#ifdef ENABLE_VULKAN
#include "Renderers/Vulkan/GSDeviceVK.h"
#endif

#ifdef _WIN32

#include "Renderers/DX11/GSDevice11.h"
#include "Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX11/D3D.h"


static HRESULT s_hr = E_FAIL;

#endif

#include "Renderers/Null/GSDeviceNull.h"

// do NOT undefine this/put it above includes, as x11 people love to redefine
// things that make obscure compiler bugs, unless you want to run around and
// debug obscure compiler errors --govanify
#undef None

Pcsx2Config::GSOptions GSConfig;

static u64 s_next_manual_present_time;

int GSinit()
{
	GSVertexSW::InitStatic();

	GSUtil::Init();

	return 0;
}

void GSshutdown()
{
	GSclose();
}

static RenderAPI GetAPIForRenderer(GSRendererType renderer)
{
	switch(hw_render.context_type)
	{
	case RETRO_HW_CONTEXT_D3D11:
		return RenderAPI::D3D11;
	case RETRO_HW_CONTEXT_D3D12:
		return RenderAPI::D3D12;
	case RETRO_HW_CONTEXT_VULKAN:
		return RenderAPI::Vulkan;
	case RETRO_HW_CONTEXT_NONE:
		return RenderAPI::None;
	default:
		break;
	}
	return RenderAPI::OpenGL;
}

static bool OpenGSDevice(GSRendererType renderer, bool clear_state_on_fail, bool recreate_window)
{
	const RenderAPI new_api = GetAPIForRenderer(renderer);
	switch (new_api)
	{
#ifdef _WIN32
		case RenderAPI::D3D11:
			g_gs_device = std::make_unique<GSDevice11>();
			break;
		case RenderAPI::D3D12:
			g_gs_device = std::make_unique<GSDevice12>();
			break;
#endif
#ifdef __APPLE__
		case RenderAPI::Metal:
			g_gs_device = std::unique_ptr<GSDevice>(MakeGSDeviceMTL());
			break;
#endif
#ifdef ENABLE_OPENGL
		case RenderAPI::OpenGL:
			g_gs_device = std::make_unique<GSDeviceOGL>();
			break;
#endif

#ifdef ENABLE_VULKAN
		case RenderAPI::Vulkan:
			g_gs_device = std::make_unique<GSDeviceVK>();
			break;
#endif
		case RenderAPI::None:
			g_gs_device = std::make_unique<GSDeviceNull>();
		break;
		default:
			Console.Error("Unsupported render API %s", GSDevice::RenderAPIToString(new_api));
			return false;
	}

	bool okay = g_gs_device->Create();

	if (!okay)
	{
		g_gs_device->Destroy();
		g_gs_device.reset();
		Host::ReleaseRenderWindow();
		return false;
	}

	Console.WriteLn(Color_StrongGreen, "%s Graphics Driver Info:", GSDevice::RenderAPIToString(new_api));
	Console.Indent().WriteLn(g_gs_device->GetDriverInfo());

	return true;
}

static void CloseGSDevice(bool clear_state)
{
	if (!g_gs_device)
		return;

	g_gs_device->Destroy();
	g_gs_device.reset();
}

static bool OpenGSRenderer(GSRendererType renderer, u8* basemem)
{
	if (renderer == GSRendererType::Null)
	{
		g_gs_renderer = std::make_unique<GSRendererNull>();
	}
	else if (renderer != GSRendererType::SW)
	{
		g_gs_renderer = std::make_unique<GSRendererHW>();
	}
	else
	{
		g_gs_renderer = std::unique_ptr<GSRenderer>(MULTI_ISA_SELECT(makeGSRendererSW)(GSConfig.SWExtraThreads));
	}

	g_gs_renderer->SetRegsMem(basemem);
	g_gs_renderer->ResetPCRTC();
	g_perfmon.Reset();
	return true;
}

static void CloseGSRenderer()
{
	GSTextureReplacements::Shutdown();

	if (g_gs_renderer)
	{
		g_gs_renderer->Destroy();
		g_gs_renderer.reset();
	}
}

bool GSreopen(bool recreate_device, bool recreate_renderer, const Pcsx2Config::GSOptions& old_config)
{
	Console.WriteLn("Reopening GS with %s device and %s renderer", recreate_device ? "new" : "existing",
		recreate_renderer ? "new" : "existing");

	if (recreate_renderer)
		g_gs_renderer->Flush(GSState::GSFlushReason::GSREOPEN);

	if (GSConfig.UserHacks_ReadTCOnClose)
		g_gs_renderer->ReadbackTextureCache();

	u8* basemem = g_gs_renderer->GetRegsMem();
	const u32 gamecrc = g_gs_renderer->GetGameCRC();

	freezeData fd = {};
	std::unique_ptr<u8[]> fd_data;
	if (recreate_renderer)
	{
		if (g_gs_renderer->Freeze(&fd, true) != 0)
		{
			Console.Error("(GSreopen) Failed to get GS freeze size");
			return false;
		}

		fd_data = std::make_unique<u8[]>(fd.size);
		fd.data = fd_data.get();
		if (g_gs_renderer->Freeze(&fd, false) != 0)
		{
			Console.Error("(GSreopen) Failed to freeze GS");
			return false;
		}

		CloseGSRenderer();
	}
	else
	{
		// Make sure nothing is left over.
		g_gs_renderer->PurgeTextureCache();
		g_gs_renderer->PurgePool();
	}

	if (recreate_device)
	{
		// We need a new render window when changing APIs.
		const bool recreate_window = (g_gs_device->GetRenderAPI() != GetAPIForRenderer(GSConfig.Renderer));
		CloseGSDevice(false);

		if (!OpenGSDevice(GSConfig.Renderer, false, recreate_window) ||
			(recreate_renderer && !OpenGSRenderer(GSConfig.Renderer, basemem)))
		{
			Host::AddKeyedOSDMessage(
				"GSReopenFailed", "Failed to reopen, restoring old configuration.", Host::OSD_CRITICAL_ERROR_DURATION);

			CloseGSDevice(false);

			GSConfig = old_config;
			if (!OpenGSDevice(GSConfig.Renderer, false, recreate_window) ||
				(recreate_renderer && !OpenGSRenderer(GSConfig.Renderer, basemem)))
			{
				pxFailRel("Failed to reopen GS on old config");
				Host::ReleaseRenderWindow();
				return false;
			}
		}
	}
	else if (recreate_renderer)
	{
		if (!OpenGSRenderer(GSConfig.Renderer, basemem))
		{
			Console.Error("(GSreopen) Failed to create new renderer");
			return false;
		}
	}

	if (recreate_renderer)
	{
		if (g_gs_renderer->Defrost(&fd) != 0)
		{
			Console.Error("(GSreopen) Failed to defrost");
			return false;
		}

		g_gs_renderer->SetGameCRC(gamecrc);
	}

	return true;
}

bool GSopen(const Pcsx2Config::GSOptions& config, GSRendererType renderer, u8* basemem)
{
	if (renderer == GSRendererType::Auto)
		renderer = GSUtil::GetPreferredRenderer();

	GSConfig = config;
	GSConfig.Renderer = renderer;

	bool res = OpenGSDevice(renderer, true, false);
	if (res)
	{
		res = OpenGSRenderer(renderer, basemem);
		if (!res)
			CloseGSDevice(true);
	}

	if (!res)
	{
		Host::ReportErrorAsync(
			"Error", fmt::format("Failed to create render device. This may be due to your GPU not supporting the "
								 "chosen renderer ({}), or because your graphics drivers need to be updated.",
						 Pcsx2Config::GSOptions::GetRendererName(EmuConfig.GS.Renderer)));
		return false;
	}

	return true;
}

void GSclose()
{
	CloseGSRenderer();
	CloseGSDevice(true);
}

void GSreset(bool hardware_reset)
{
	g_gs_renderer->Reset(hardware_reset);
}

void GSgifSoftReset(u32 mask)
{
	g_gs_renderer->SoftReset(mask);
}

void GSwriteCSR(u32 csr)
{
	g_gs_renderer->WriteCSR(csr);
}

void GSInitAndReadFIFO(u8* mem, u32 size)
{
	g_gs_renderer->InitReadFIFO(mem, size);
	g_gs_renderer->ReadFIFO(mem, size);
}

void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG)
{
	g_gs_renderer->ReadLocalMemoryUnsync(mem, qwc, GIFRegBITBLTBUF{BITBLITBUF}, GIFRegTRXPOS{TRXPOS}, GIFRegTRXREG{TRXREG});
}

void GSgifTransfer(const u8* mem, u32 size)
{
	g_gs_renderer->Transfer<3>(mem, size);
}

void GSgifTransfer1(u8* mem, u32 addr)
{
	g_gs_renderer->Transfer<0>(const_cast<u8*>(mem) + addr, (0x4000 - addr) / 16);
}

void GSgifTransfer2(u8* mem, u32 size)
{
	g_gs_renderer->Transfer<1>(const_cast<u8*>(mem), size);
}

void GSgifTransfer3(u8* mem, u32 size)
{
	g_gs_renderer->Transfer<2>(const_cast<u8*>(mem), size);
}

void GSvsync(u32 field, bool registers_written)
{
	g_gs_renderer->VSync(field, registers_written, g_gs_renderer->IsIdleFrame());
}

int GSfreeze(FreezeAction mode, freezeData* data)
{
	if (mode == FreezeAction::Save)
		return g_gs_renderer->Freeze(data, false);
	else if (mode == FreezeAction::Size)
		return g_gs_renderer->Freeze(data, true);
	else if (mode == FreezeAction::Load)
	{
		// Since Defrost doesn't do a hardware reset (since it would be clearing
		// local memory just before it's overwritten), we have to manually wipe
		// out the current textures.
		g_gs_device->ClearCurrent();
		return g_gs_renderer->Defrost(data);
	}

	return 0;
}

void GSPresentCurrentFrame()
{
	g_gs_renderer->PresentCurrentFrame();
}

void GSThrottlePresentation()
{
	// Let vsync take care of throttling.
	if (g_gs_device->GetVsyncMode() != VsyncMode::Off)
		return;

	// Manually throttle presentation when vsync isn't enabled, so we don't try to render the
	// fullscreen UI at thousands of FPS and make the gpu go brrrrrrrr.
	const float surface_refresh_rate = g_gs_device->GetWindowInfo().surface_refresh_rate;
	const float throttle_rate = (surface_refresh_rate > 0.0f) ? surface_refresh_rate : 60.0f;

	const u64 sleep_period = static_cast<u64>(static_cast<double>(GetTickFrequency()) / static_cast<double>(throttle_rate));
	const u64 current_ts = GetCPUTicks();

	// Allow it to fall behind/run ahead up to 2*period. Sleep isn't that precise, plus we need to
	// allow time for the actual rendering.
	const u64 max_variance = sleep_period * 2;
	if (static_cast<u64>(std::abs(static_cast<s64>(current_ts - s_next_manual_present_time))) > max_variance)
		s_next_manual_present_time = current_ts + sleep_period;
	else
		s_next_manual_present_time += sleep_period;

	Threading::SleepUntil(s_next_manual_present_time);
}

void GSSetGameCRC(u32 crc)
{
	g_gs_renderer->SetGameCRC(crc);
}

void GSSetVSyncMode(VsyncMode mode)
{
	g_gs_device->SetVSync(mode);
}

bool GSGetHostRefreshRate(float* refresh_rate)
{
	if (!g_gs_device)
		return false;

	return g_gs_device->GetHostRefreshRate(refresh_rate);
}

void GSGetAdaptersAndFullscreenModes(
	GSRendererType renderer, std::vector<std::string>* adapters, std::vector<std::string>* fullscreen_modes)
{
	switch (renderer)
	{
#ifdef _WIN32
		case GSRendererType::DX11:
		case GSRendererType::DX12:
		{
			auto factory = D3D::CreateFactory(false);
			if (factory)
			{
				if (adapters)
					*adapters = D3D::GetAdapterNames(factory.get());
				if (fullscreen_modes)
					*fullscreen_modes = D3D::GetFullscreenModes(factory.get(), EmuConfig.GS.Adapter);
			}
		}
		break;
#endif

#ifdef ENABLE_VULKAN
		case GSRendererType::VK:
		{
			GSDeviceVK::GetAdaptersAndFullscreenModes(adapters, fullscreen_modes);
		}
		break;
#endif

#ifdef __APPLE__
		case GSRendererType::Metal:
		{
			if (adapters)
				*adapters = GetMetalAdapterList();
		}
		break;
#endif

		default:
			break;
	}
}

GSVideoMode GSgetDisplayMode()
{
	GSRenderer* gs = g_gs_renderer.get();

	return gs->GetVideoMode();
}

void GSgetInternalResolution(int* width, int* height)
{
	GSRenderer* gs = g_gs_renderer.get();
	if (!gs)
	{
		*width = 0;
		*height = 0;
		return;
	}

	const GSVector2i res(gs->GetInternalResolution());
	*width = res.x;
	*height = res.y;
}

void GSgetStats(std::string& info)
{
	GSPerfMon& pm = g_perfmon;
	const char* api_name = GSDevice::RenderAPIToString(g_gs_device->GetRenderAPI());
	if (GSConfig.Renderer == GSRendererType::SW)
	{
		const double fps = GetVerticalFrequency();
		const double fillrate = pm.Get(GSPerfMon::Fillrate);
		fmt::format_to(std::back_inserter(info), "{} SW | {} S | {} P | {} D | {:.2f} U | {:.2f} D | {:.2f} mpps",
			api_name,
			(int)pm.Get(GSPerfMon::SyncPoint),
			(int)pm.Get(GSPerfMon::Prim),
			(int)pm.Get(GSPerfMon::Draw),
			pm.Get(GSPerfMon::Swizzle) / 1024,
			pm.Get(GSPerfMon::Unswizzle) / 1024,
			fps * fillrate / (1024 * 1024));
	}
	else if (GSConfig.Renderer == GSRendererType::Null)
	{
		fmt::format_to(std::back_inserter(info), "{} Null", api_name);
	}
	else
	{
		fmt::format_to(std::back_inserter(info), "{} HW | {} P | {} D | {} DC | {} B | {} RP | {} RB | {} TC | {} TU",
			api_name,
			(int)pm.Get(GSPerfMon::Prim),
			(int)pm.Get(GSPerfMon::Draw),
			(int)std::ceil(pm.Get(GSPerfMon::DrawCalls)),
			(int)std::ceil(pm.Get(GSPerfMon::Barriers)),
			(int)std::ceil(pm.Get(GSPerfMon::RenderPasses)),
			(int)std::ceil(pm.Get(GSPerfMon::Readbacks)),
			(int)std::ceil(pm.Get(GSPerfMon::TextureCopies)),
			(int)std::ceil(pm.Get(GSPerfMon::TextureUploads)));
	}
}

void GSgetMemoryStats(std::string& info)
{
	if (!g_texture_cache)
		return;

	const u64 targets = g_texture_cache->GetTargetMemoryUsage();
	const u64 sources = g_texture_cache->GetSourceMemoryUsage();
	const u64 hashcache = g_texture_cache->GetHashCacheMemoryUsage();
	const u64 pool = g_gs_device->GetPoolMemoryUsage();
	const u64 total = targets + sources + hashcache + pool;

	if (GSConfig.TexturePreloading == TexturePreloadingLevel::Full)
	{
		fmt::format_to(std::back_inserter(info), "VRAM: {} MB | T: {} MB | S: {} MB | H: {} MB | P: {} MB",
			(int)std::ceil(total / 1048576.0f),
			(int)std::ceil(targets / 1048576.0f),
			(int)std::ceil(sources / 1048576.0f),
			(int)std::ceil(hashcache / 1048576.0f),
			(int)std::ceil(pool / 1048576.0f));
	}
	else
	{
		fmt::format_to(std::back_inserter(info), "VRAM: {} MB | T: {} MB | S: {} MB | P: {} MB",
			(int)std::ceil(total / 1048576.0f),
			(int)std::ceil(targets / 1048576.0f),
			(int)std::ceil(sources / 1048576.0f),
			(int)std::ceil(pool / 1048576.0f));
	}
}

void GSgetTitleStats(std::string& info)
{
	static constexpr const char* deinterlace_modes[] = {
		"Automatic", "None", "Weave tff", "Weave bff", "Bob tff", "Bob bff", "Blend tff", "Blend bff", "Adaptive tff", "Adaptive bff"};

	const char* api_name = GSDevice::RenderAPIToString(g_gs_device->GetRenderAPI());
	const char* hw_sw_name = (GSConfig.Renderer == GSRendererType::Null) ? " Null" : (GSConfig.UseHardwareRenderer() ? " HW" : " SW");
	const char* deinterlace_mode = deinterlace_modes[static_cast<int>(GSConfig.InterlaceMode)];

	const char* interlace_mode = ReportInterlaceMode();
	const char* video_mode = ReportVideoMode();
	info = StringUtil::StdStringFromFormat("%s%s | %s | %s | %s", api_name, hw_sw_name, video_mode, interlace_mode, deinterlace_mode);
}

void GSUpdateConfig(const Pcsx2Config::GSOptions& new_config)
{
	Pcsx2Config::GSOptions old_config(std::move(GSConfig));
	GSConfig = new_config;
	GSConfig.Renderer = (GSConfig.Renderer == GSRendererType::Auto) ? GSUtil::GetPreferredRenderer() : GSConfig.Renderer;
	if (!g_gs_renderer)
		return;

	// Options which aren't using the global struct yet, so we need to recreate all GS objects.
	if (
		GSConfig.SWExtraThreads != old_config.SWExtraThreads ||
		GSConfig.SWExtraThreadsHeight != old_config.SWExtraThreadsHeight)
	{
		if (!GSreopen(false, true, old_config))
			pxFailRel("Failed to do quick GS reopen");

		return;
	}

	// This is where we would do finer-grained checks in the future.
	// For example, flushing the texture cache when mipmap settings change.

	if (GSConfig.CRCHack != old_config.CRCHack ||
		GSConfig.UpscaleMultiplier != old_config.UpscaleMultiplier ||
		GSConfig.GetSkipCountFunctionId != old_config.GetSkipCountFunctionId ||
		GSConfig.BeforeDrawFunctionId != old_config.BeforeDrawFunctionId)
	{
		g_gs_renderer->UpdateCRCHacks();
	}

	// renderer-specific options (e.g. auto flush, TC offset)
	g_gs_renderer->UpdateSettings(old_config);

	// reload texture cache when trilinear filtering or TC options change
	if (
		(GSConfig.UseHardwareRenderer() && GSConfig.HWMipmap != old_config.HWMipmap) ||
		GSConfig.TexturePreloading != old_config.TexturePreloading ||
		GSConfig.TriFilter != old_config.TriFilter ||
		GSConfig.GPUPaletteConversion != old_config.GPUPaletteConversion ||
		GSConfig.PreloadFrameWithGSData != old_config.PreloadFrameWithGSData ||
		GSConfig.UserHacks_CPUFBConversion != old_config.UserHacks_CPUFBConversion ||
		GSConfig.UserHacks_DisableDepthSupport != old_config.UserHacks_DisableDepthSupport ||
		GSConfig.UserHacks_DisablePartialInvalidation != old_config.UserHacks_DisablePartialInvalidation ||
		GSConfig.UserHacks_TextureInsideRt != old_config.UserHacks_TextureInsideRt ||
		GSConfig.UserHacks_CPUSpriteRenderBW != old_config.UserHacks_CPUSpriteRenderBW ||
		GSConfig.UserHacks_CPUCLUTRender != old_config.UserHacks_CPUCLUTRender ||
		GSConfig.UserHacks_GPUTargetCLUTMode != old_config.UserHacks_GPUTargetCLUTMode)
	{
		if (GSConfig.UserHacks_ReadTCOnClose)
			g_gs_renderer->ReadbackTextureCache();
		g_gs_renderer->PurgeTextureCache();
		g_gs_renderer->PurgePool();
	}

	// clear out the sampler cache when AF options change, since the anisotropy gets baked into them
	if (GSConfig.MaxAnisotropy != old_config.MaxAnisotropy)
		g_gs_device->ClearSamplerCache();

	// texture dumping/replacement options
	if (GSConfig.UseHardwareRenderer())
		GSTextureReplacements::UpdateConfig(old_config);

	// clear the hash texture cache since we might have replacements now
	// also clear it when dumping changes, since we want to dump everything being used
	if (GSConfig.LoadTextureReplacements != old_config.LoadTextureReplacements ||
		GSConfig.DumpReplaceableTextures != old_config.DumpReplaceableTextures)
		g_gs_renderer->PurgeTextureCache();
}

void GSSwitchRenderer(GSRendererType new_renderer)
{
	if (new_renderer == GSRendererType::Auto)
		new_renderer = GSUtil::GetPreferredRenderer();

	if (!g_gs_renderer || GSConfig.Renderer == new_renderer)
		return;

	const bool is_software_switch = (new_renderer == GSRendererType::SW || GSConfig.Renderer == GSRendererType::SW);
	const Pcsx2Config::GSOptions old_config(GSConfig);
	GSConfig.Renderer = new_renderer;
	if (!GSreopen(!is_software_switch, true, old_config))
		pxFailRel("Failed to reopen GS for renderer switch.");
}

#ifdef _WIN32

static HANDLE s_fh = NULL;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	pxAssertRel(!s_fh, "Has no file mapping");

	s_fh = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr);
	if (s_fh == NULL)
	{
		Console.Error("Failed to create file mapping of size %zu. WIN API ERROR:%u", size, GetLastError());
		return nullptr;
	}

	// Reserve the whole area with repeats.
	u8* base = static_cast<u8*>(VirtualAlloc2(
		GetCurrentProcess(), nullptr, repeat * size,
		MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
		nullptr, 0));
	if (base)
	{
		bool okay = true;
		for (size_t i = 0; i < repeat; i++)
		{
			// Everything except the last needs the placeholders split to map over them. Then map the same file over the region.
			u8* addr = base + i * size;
			if ((i != (repeat - 1) && !VirtualFreeEx(GetCurrentProcess(), addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) ||
				!MapViewOfFile3(s_fh, GetCurrentProcess(), addr, 0, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
			{
				Console.Error("Failed to map repeat %zu of size %zu.", i, size);
				okay = false;

				for (size_t j = 0; j < i; j++)
					UnmapViewOfFile2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
			}
		}

		if (okay)
			return base;

		VirtualFreeEx(GetCurrentProcess(), base, 0, MEM_RELEASE);
	}

	Console.Error("Failed to reserve VA space of size %zu. WIN API ERROR:%u", size, GetLastError());
	CloseHandle(s_fh);
	s_fh = NULL;
	return nullptr;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	pxAssertRel(s_fh, "Has a file mapping");

	for (size_t i = 0; i < repeat; i++)
	{
		u8* addr = (u8*)ptr + i * size;
		UnmapViewOfFile2(GetCurrentProcess(), addr, MEM_PRESERVE_PLACEHOLDER);
	}

	VirtualFreeEx(GetCurrentProcess(), ptr, 0, MEM_RELEASE);
	s_fh = NULL;
}

#else

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static int s_shm_fd = -1;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	ASSERT(s_shm_fd == -1);

	const char* file_name = "/GS.mem";
	s_shm_fd = shm_open(file_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (s_shm_fd != -1)
	{
		shm_unlink(file_name); // file is deleted but descriptor is still open
	}
	else
	{
		fprintf(stderr, "Failed to open %s due to %s\n", file_name, strerror(errno));
		return nullptr;
	}

	if (ftruncate(s_shm_fd, repeat * size) < 0)
		fprintf(stderr, "Failed to reserve memory due to %s\n", strerror(errno));

	void* fifo = mmap(nullptr, size * repeat, PROT_READ | PROT_WRITE, MAP_SHARED, s_shm_fd, 0);

	for (size_t i = 1; i < repeat; i++)
	{
		void* base = (u8*)fifo + size * i;
		u8* next = (u8*)mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s_shm_fd, 0);
		if (next != base)
			fprintf(stderr, "Fail to mmap contiguous segment\n");
	}

	return fifo;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	ASSERT(s_shm_fd >= 0);

	if (s_shm_fd < 0)
		return;

	munmap(ptr, size * repeat);

	close(s_shm_fd);
	s_shm_fd = -1;
}

#endif
