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

#include "../../common/Console.h"
#include "../../common/FileSystem.h"
#include "../../common/Path.h"

#include "GS.h"
#include "GSUtil.h"
#include "GSExtra.h"
#include "Renderers/HW/GSRendererHW.h"
#include "Renderers/HW/GSTextureReplacements.h"
#include "MultiISA.h"

#include "../Config.h"
#include "../Counters.h"
#include "../GS.h"

#ifdef HAVE_PARALLEL_GS
#include "Renderers/parallel-gs/GSRendererPGS.h"
std::unique_ptr<GSRendererPGS> g_pgs_renderer;
#endif

#ifdef ENABLE_OPENGL
#include "Renderers/OpenGL/GSDeviceOGL.h"
#endif

#ifdef ENABLE_VULKAN
#include "Renderers/Vulkan/GSDeviceVK.h"
#endif

#ifdef _WIN32
#include "Renderers/DX11/GSDevice11.h"
#include "Renderers/DX11/D3D.h"
#include "Renderers/DX12/GSDevice12.h"
#endif

#include <libretro.h>

extern retro_hw_render_callback hw_render;

int m_disp_fb_sprite_blits = 0;

Pcsx2Config::GSOptions GSConfig;

void GSinit(void)
{
	GSVertexSW::InitStatic();
	GSUtil::Init();
	m_disp_fb_sprite_blits = 0;
}

void GSshutdown(void)
{
	GSclose();
	m_disp_fb_sprite_blits = 0;
}

bool GSIsHardwareRenderer(void)
{
	// Null gets flagged as hw.
	return (GSConfig.Renderer != GSRendererType::SW);
}

static GSRendererType GSsetRenderer(enum retro_hw_context_type api)
{
	switch (api)
	{
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES2:        /* TODO/FIXME */
		case RETRO_HW_CONTEXT_OPENGLES3:        /* TODO/FIXME */
		case RETRO_HW_CONTEXT_OPENGLES_VERSION: /* TODO/FIXME */
			return GSRendererType::OGL;
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef HAVE_PARALLEL_GS
			return GSRendererType::ParallelGS;
#else
			return GSRendererType::VK;
#endif
		case RETRO_HW_CONTEXT_D3D11:
			return GSRendererType::DX11;
		case RETRO_HW_CONTEXT_D3D12:
			return GSRendererType::DX12;
#if 0
		case RETRO_HW_CONTEXT_D3D10: 
		case RETRO_HW_CONTEXT_D3D9: 
#endif
		case RETRO_HW_CONTEXT_NONE:
		default:
			break;
	}
	return  GSRendererType::SW;
}

static bool OpenGSDevice(GSRendererType renderer, bool clear_state_on_fail)
{
	switch (hw_render.context_type)
	{
		case RETRO_HW_CONTEXT_D3D11:
#ifdef _WIN32
			g_gs_device = std::make_unique<GSDevice11>();
			if (!g_gs_device->Create())
			{
				g_gs_device->Destroy();
				g_gs_device.reset();
				return false;
			}
#endif
			break;
		case RETRO_HW_CONTEXT_D3D12:
#ifdef _WIN32
			g_gs_device = std::make_unique<GSDevice12>();
			if (!g_gs_device->Create())
			{
				g_gs_device->Destroy();
				g_gs_device.reset();
				return false;
			}
#endif
			break;
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef ENABLE_VULKAN
			// PGS fuses device and renderer together.
			if (renderer == GSRendererType::VK)
			{
				g_gs_device = std::make_unique<GSDeviceVK>();
				if (!g_gs_device->Create())
				{
					g_gs_device->Destroy();
					g_gs_device.reset();
					return false;
				}
			}
#endif
			break;
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES2:        /* TODO/FIXME */
		case RETRO_HW_CONTEXT_OPENGLES3:        /* TODO/FIXME */
		case RETRO_HW_CONTEXT_OPENGLES_VERSION: /* TODO/FIXME */
#ifdef ENABLE_OPENGL
			g_gs_device = std::make_unique<GSDeviceOGL>();
			if (!g_gs_device->Create())
			{
				g_gs_device->Destroy();
				g_gs_device.reset();
				return false;
			}
#endif
			break;
		case RETRO_HW_CONTEXT_NONE:
			break;
		default:
			return false;
	}

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
#ifdef HAVE_PARALLEL_GS
	if (renderer == GSRendererType::ParallelGS)
	{
		g_pgs_renderer = std::make_unique<GSRendererPGS>(basemem);
		return g_pgs_renderer->Init();
	}
	else
#endif
	{
		if (renderer != GSRendererType::SW)
			g_gs_renderer = std::make_unique<GSRendererHW>();
		else
			g_gs_renderer = std::unique_ptr<GSRenderer>(MULTI_ISA_SELECT(makeGSRendererSW)(GSConfig.SWExtraThreads));

		g_gs_renderer->SetRegsMem(basemem);
		g_gs_renderer->ResetPCRTC();
		g_gs_renderer->UpdateRenderFixes();
	}
	return true;
}

static void CloseGSRenderer(void)
{
	GSTextureReplacements::Shutdown();

#ifdef HAVE_PARALLEL_GS
	g_pgs_renderer.reset();
#endif

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

	if (g_gs_renderer)
	{
		g_gs_renderer->Flush(GSState::GSFlushReason::GSREOPEN);

		if (recreate_device && !recreate_renderer)
		{
			// Keeping the renderer around, make sure nothing is left over.
			g_gs_renderer->PurgeTextureCache(true, true, true);
			g_gs_renderer->PurgePool();
		}
		else if (GSConfig.UserHacks_ReadTCOnClose)
			g_gs_renderer->ReadbackTextureCache();
	}

	u8* basemem;
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
	{
		basemem = g_pgs_renderer->GetRegsMem();
	}
	else
#endif
	{
		basemem = g_gs_renderer->GetRegsMem();
	}

	freezeData fd = {};
	std::unique_ptr<u8[]> fd_data;
	if (recreate_renderer)
	{
#ifdef HAVE_PARALLEL_GS
		if (g_pgs_renderer)
		{
			if (g_pgs_renderer->Freeze(&fd, true) != 0)
			{
				Console.Error("(GSreopen) Failed to get GS freeze size");
				return false;
			}
		}
		else
#endif
		{
			if (g_gs_renderer->Freeze(&fd, true) != 0)
			{
				Console.Error("(GSreopen) Failed to get GS freeze size");
				return false;
			}
		}

		fd_data = std::make_unique<u8[]>(fd.size);
		fd.data = fd_data.get();

#ifdef HAVE_PARALLEL_GS
		if (g_pgs_renderer)
		{
			if (g_pgs_renderer->Freeze(&fd, false) != 0)
			{
				Console.Error("(GSreopen) Failed to freeze GS");
				return false;
			}
		}
		else
#endif
		{
			if (g_gs_renderer->Freeze(&fd, false) != 0)
			{
				Console.Error("(GSreopen) Failed to freeze GS");
				return false;
			}
		}

		CloseGSRenderer();
	}

	if (recreate_device)
	{
		// We need a new render window when changing APIs.
		CloseGSDevice(false);

		if (!OpenGSDevice(GSConfig.Renderer, false))
		{
			Console.Warning("Failed to reopen, restoring old configuration.");
			CloseGSDevice(false);

			GSConfig = old_config;
			if (!OpenGSDevice(GSConfig.Renderer, false))
			{
				Console.Error("Failed to reopen GS on old config");
				return false;
			}
		}
	}

	if (recreate_renderer)
	{
		if (!OpenGSRenderer(GSConfig.Renderer, basemem))
		{
			Console.Error("(GSreopen) Failed to create new renderer");
			return false;
		}

#ifdef HAVE_PARALLEL_GS
		if (g_pgs_renderer)
		{
			if (g_pgs_renderer->Defrost(&fd) != 0)
			{
				Console.Error("(GSreopen) Failed to defrost");
				return false;
			}
		}
		else
#endif
		{
			if (g_gs_renderer->Defrost(&fd) != 0)
			{
				Console.Error("(GSreopen) Failed to defrost");
				return false;
			}
		}
	}

	return true;
}

void GSopen(const Pcsx2Config::GSOptions& config, GSRendererType renderer, enum retro_hw_context_type api, u8* basemem)
{
	if (renderer == GSRendererType::Auto)
		renderer  = GSsetRenderer(api);

	GSConfig          = config;
	GSConfig.Renderer = renderer;

	if (OpenGSDevice(renderer, true))
		if (!OpenGSRenderer(renderer, basemem))
			CloseGSDevice(true);
}

void GSclose(void)
{
	CloseGSRenderer();
	CloseGSDevice(true);
}

void GSreset(bool hardware_reset)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
		g_pgs_renderer->Reset(hardware_reset);
#endif
	if (g_gs_renderer)
		g_gs_renderer->Reset(hardware_reset);
}

void GSgifSoftReset(u32 mask)
{
	if (g_gs_renderer)
		g_gs_renderer->SoftReset(mask);
}

void GSwriteCSR(u32 csr)
{
	if (g_gs_renderer)
		g_gs_renderer->WriteCSR(csr);
}

void GSInitAndReadFIFO(u8* mem, u32 size)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
		g_pgs_renderer->ReadFIFO(mem, size);
#endif

	if (g_gs_renderer)
	{
		g_gs_renderer->InitReadFIFO(mem, size);
		g_gs_renderer->ReadFIFO(mem, size);
	}
}

void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG)
{
	if (g_gs_renderer)
		g_gs_renderer->ReadLocalMemoryUnsync(mem, qwc, GIFRegBITBLTBUF{BITBLITBUF}, GIFRegTRXPOS{TRXPOS}, GIFRegTRXREG{TRXREG});
}

void GSgifTransfer(const u8* mem, u32 size)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
		g_pgs_renderer->Transfer(mem, size);
#endif

	if (g_gs_renderer)
		g_gs_renderer->Transfer(mem, size);
}

void GSvsync(u32 field, bool registers_written)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
		g_pgs_renderer->VSync(field, registers_written);
#endif

	if (g_gs_renderer)
	{
		// Do not move the flush into the VSync() method. It's here because EE transfers
		// get cleared in HW VSync, and may be needed for a buffered draw (FFX FMVs).
		g_gs_renderer->Flush(GSState::VSYNC);
		g_gs_renderer->VSync(field, registers_written, g_gs_renderer->IsIdleFrame());
	}
}

int GSfreeze(FreezeAction mode, freezeData* data)
{
#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
	{
		if (mode == FreezeAction::Save)
			return g_pgs_renderer->Freeze(data, false);
		else if (mode == FreezeAction::Size)
			return g_pgs_renderer->Freeze(data, true);
		else // if (mode == FreezeAction::Load)
			return g_pgs_renderer->Defrost(data);
	}
#endif

	if (!g_gs_renderer)
		return -1;

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

void GSGameChanged()
{
	if (GSConfig.UseHardwareRenderer())
		GSTextureReplacements::GameChanged();
}

void GSUpdateConfig(const Pcsx2Config::GSOptions& new_config, enum retro_hw_context_type api)
{
	Pcsx2Config::GSOptions old_config(std::move(GSConfig));
	GSConfig = new_config;

#ifdef HAVE_PARALLEL_GS
	if (g_pgs_renderer)
		g_pgs_renderer->UpdateConfig();
#endif

	if (GSConfig.Renderer == GSRendererType::Auto)
		GSConfig.Renderer = GSsetRenderer(api);
	if (!g_gs_renderer)
		return;

	// Options which aren't using the global struct yet, so we need to recreate all GS objects.
	if (
		   GSConfig.SWExtraThreads       != old_config.SWExtraThreads
		|| GSConfig.SWExtraThreadsHeight != old_config.SWExtraThreadsHeight)
	{
		if (!GSreopen(false, true, old_config))
			Console.Error("Failed to do quick GS reopen");

		return;
	}

	if (       GSConfig.UserHacks_DisableRenderFixes                != old_config.UserHacks_DisableRenderFixes
		|| GSConfig.UpscaleMultiplier      != old_config.UpscaleMultiplier
		|| GSConfig.GetSkipCountFunctionId != old_config.GetSkipCountFunctionId
		|| GSConfig.BeforeDrawFunctionId   != old_config.BeforeDrawFunctionId          ||
		GSConfig.MoveHandlerFunctionId != old_config.MoveHandlerFunctionId)
		g_gs_renderer->UpdateRenderFixes();

	// renderer-specific options (e.g. auto flush, TC offset)
	g_gs_renderer->UpdateSettings(old_config);

	// reload texture cache when trilinear filtering or TC options change
	if (
		(GSConfig.UseHardwareRenderer() && GSConfig.HWMipmapMode != old_config.HWMipmapMode) ||
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
		g_gs_renderer->PurgeTextureCache(true, true, true);
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
	if (GSConfig.LoadTextureReplacements != old_config.LoadTextureReplacements)
		g_gs_renderer->PurgeTextureCache(true, false, true);
}

void GSSwitchRenderer(GSRendererType new_renderer, enum retro_hw_context_type api, GSInterlaceMode new_interlace)
{
	if (new_renderer == GSRendererType::Auto)
		new_renderer = GSsetRenderer(api);

	if (!g_gs_renderer || GSConfig.Renderer == new_renderer)
		return;

	const bool is_software_switch = (new_renderer == GSRendererType::SW || GSConfig.Renderer == GSRendererType::SW);
	const Pcsx2Config::GSOptions old_config(GSConfig);
	GSConfig.Renderer = new_renderer;
	GSConfig.InterlaceMode = new_interlace;
	if (!GSreopen(!is_software_switch, true, old_config))
		Console.Error("Failed to reopen GS for renderer switch.");
}

#ifdef _WIN32

static HANDLE s_fh = NULL;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
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
	const char* file_name = "/GS.mem";
	s_shm_fd = shm_open(file_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (s_shm_fd == -1)
		return nullptr;
	shm_unlink(file_name); // file is deleted but descriptor is still open

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
	if (s_shm_fd < 0)
		return;

	munmap(ptr, size * repeat);

	close(s_shm_fd);
	s_shm_fd = -1;
}

#endif

std::pair<u8, u8> GSGetRGBA8AlphaMinMax(const void* data, u32 width, u32 height, u32 stride)
{
	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	const u8* ptr = static_cast<const u8*>(data);
	if ((width % 4) == 0)
	{
		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			ptr += stride;
		}
	}
	else
	{
		const u32 aligned_width = Common::AlignDownPow2(width, 4);
		static constexpr const GSVector4i masks[3][2] = {
			{GSVector4i::cxpr(0xFFFFFFFF, 0, 0, 0), GSVector4i::cxpr(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0, 0), GSVector4i::cxpr(0, 0, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0), GSVector4i::cxpr(0, 0, 0, 0xFFFFFFFF)},
		};
		const u32 unaligned_pixels = width & 3;
		const GSVector4i last_mask_and = masks[unaligned_pixels - 1][0];
		const GSVector4i last_mask_or = masks[unaligned_pixels - 1][1];

		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < aligned_width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			GSVector4i v;
			u32 vu;
			if (unaligned_pixels == 3)
			{
				v = GSVector4i::loadl(rptr);
				memcpy(&vu, rptr + sizeof(u32) * 2, sizeof(vu));
				v = v.insert32<2>(vu);
			}
			else if (unaligned_pixels == 2)
				v = GSVector4i::loadl(rptr);
			else
			{
				memcpy(&vu, rptr, sizeof(vu));
				v = GSVector4i::load(vu);
			}

			minc = minc.min_u32(v | last_mask_or);
			maxc = maxc.max_u32(v & last_mask_and);

			ptr += stride;
		}
	}

	return std::make_pair<u8, u8>(static_cast<u8>(minc.minv_u32() >> 24),
		static_cast<u8>(maxc.maxv_u32() >> 24));
}
