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

#pragma once

#include "GLContext.h"

namespace GL
{
	class ContextRetroGL : public Context
	{
	public:
		ContextRetroGL(const WindowInfo& wi);
		~ContextRetroGL() override;

		static std::unique_ptr<Context> Create(const WindowInfo& wi, gsl::span<const Version> versions_to_try);

		void* GetProcAddress(const char* name) override;
		virtual bool ChangeSurface(const WindowInfo& new_wi) override;
		virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
		bool SwapBuffers() override;
		bool MakeCurrent() override;
		bool DoneCurrent() override;
		virtual std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;
	};

} // namespace GL
