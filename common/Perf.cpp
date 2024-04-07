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

#include "common/Perf.h"
#include "common/Pcsx2Defs.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Perf
{
	// Warning object aren't thread safe
	InfoVector any("");
	InfoVector ee("EE");
	InfoVector iop("IOP");
	InfoVector vu("VU");
	InfoVector vif("VIF");

	////////////////////////////////////////////////////////////////////////////////
	// Dummy implementation
	////////////////////////////////////////////////////////////////////////////////

	InfoVector::InfoVector(const char* prefix) { }
	void InfoVector::map(uptr x86, u32 size, const char* symbol) {}
	void InfoVector::map(uptr x86, u32 size, u32 pc) {}
	void InfoVector::reset() {}
} // namespace Perf
