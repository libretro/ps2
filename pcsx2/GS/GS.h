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

#pragma once

#include "../../common/WindowInfo.h"
#include "../SaveState.h"
#include "../Config.h"

#include <optional>
#include <string>
#include <string_view>

#include <libretro.h>

enum class RenderAPI
{
	None,
	D3D11,
	D3D12,
	Vulkan,
	OpenGL
};

enum class GSVideoMode : u8
{
	Unknown,
	NTSC,
	PAL,
	VESA,
	SDTV_480P,
	HDTV_720P,
	HDTV_1080I
};

enum class GSDisplayAlignment
{
	Center,
	LeftOrTop,
	RightOrBottom
};

extern Pcsx2Config::GSOptions GSConfig;

// Returns the ID for the specified function, otherwise -1.
s16 GSLookupGetSkipCountFunctionId(const std::string_view& name);
s16 GSLookupBeforeDrawFunctionId(const std::string_view& name);
s16 GSLookupMoveHandlerFunctionId(const std::string_view& name);

void GSinit(void);
void GSshutdown(void);
bool GSIsHardwareRenderer(void);
void GSopen(const Pcsx2Config::GSOptions& config, GSRendererType renderer, enum retro_hw_context_type api, u8* basemem);
bool GSreopen(bool recreate_device, bool recreate_renderer, const Pcsx2Config::GSOptions& old_config);
void GSreset(bool hardware_reset);
void GSclose(void);
void GSgifSoftReset(u32 mask);
void GSwriteCSR(u32 csr);
void GSInitAndReadFIFO(u8* mem, u32 size);
void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG);
void GSgifTransfer(const u8* mem, u32 size);
void GSvsync(u32 field, bool registers_written);
int GSfreeze(FreezeAction mode, freezeData* data);
void GSGameChanged(void);

void GSUpdateConfig(const Pcsx2Config::GSOptions& new_config, enum retro_hw_context_type api);
void GSSwitchRenderer(GSRendererType new_renderer, enum retro_hw_context_type api, GSInterlaceMode new_interlace);

namespace Host
{
	/// Called when the GS is creating a render device.
	/// This could also be fullscreen transition.
	std::optional<WindowInfo> AcquireRenderWindow(void);
}

extern int m_disp_fb_sprite_blits;
