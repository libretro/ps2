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

#include "GSScanlineEnvironment.h"
#include "../../GSUtil.h"
#include "../../MultiISA.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif
#include <xbyak/xbyak.h>

MULTI_ISA_UNSHARED_START

class GSSetupPrimCodeGenerator : public Xbyak::CodeGenerator
{
	void operator=(const GSSetupPrimCodeGenerator&);

	GSScanlineSelector m_sel;

	struct
	{
		u32 z : 1, f : 1, t : 1, c : 1;
	} m_en;

public:
	GSSetupPrimCodeGenerator(u64 key, void* code, size_t maxsize);
};

MULTI_ISA_UNSHARED_END
