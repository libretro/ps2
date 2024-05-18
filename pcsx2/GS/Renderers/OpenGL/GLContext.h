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

#include "common/Pcsx2Defs.h"

#include <memory>

class GLContext
{
	public:
		GLContext();
		~GLContext();

		enum class Profile
		{
			NoProfile,
			Core,
			ES
		};

		struct Version
		{
			Profile profile;
			int major_version;
			int minor_version;
		};

		__fi bool IsGLES() const { return (m_version.profile == Profile::ES); }
		static std::unique_ptr<GLContext> Create();
	protected:
		Version m_version = {};
};
