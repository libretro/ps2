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

#include "common/PrecompiledHeader.h"

#include "common/Console.h"
#include "GLContextRetroGL.h"
#include <optional>
#include <vector>
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSVector.h"

#include <libretro.h>
extern retro_video_refresh_t video_cb;
extern retro_hw_render_callback hw_render;

ContextRetroGL::ContextRetroGL() : GLContext()
{
}

ContextRetroGL::~ContextRetroGL()
{
}

std::unique_ptr<GLContext> ContextRetroGL::Create(gsl::span<const Version> versions_to_try)
{
	std::unique_ptr<ContextRetroGL> context = std::make_unique<ContextRetroGL>();
	return context;
}

void* ContextRetroGL::GetProcAddress(const char* name)
{
	return reinterpret_cast<void*>(hw_render.get_proc_address(name));
}

bool ContextRetroGL::SwapBuffers()
{
	if(g_gs_device->GetCurrent())
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, g_gs_device->GetCurrent()->GetWidth(), g_gs_device->GetCurrent()->GetHeight(), 0);
	else
		video_cb(NULL, 0, 0, 0);
	return true;
}

std::unique_ptr<GLContext> ContextRetroGL::CreateSharedContext()
{
	std::unique_ptr<ContextRetroGL> context = std::make_unique<ContextRetroGL>();
	return context;
}
