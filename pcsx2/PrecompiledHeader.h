/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#ifndef PCSX2_PRECOMPILED_HEADER
#define PCSX2_PRECOMPILED_HEADER

// Disable some pointless warnings...
#ifdef _MSC_VER
#	pragma warning(disable:4250) //'class' inherits 'method' via dominance
#endif

#include "common/Pcsx2Defs.h"

//////////////////////////////////////////////////////////////////////////////////////////
// Include the STL that's actually handy.

#include <cinttypes>	// Printf format
#include <climits>
#include <cstring>		// string.h under c++
#include <cstdlib>
#include <cmath>
#include <memory>

#if !defined(_MSC_VER) /* must be GCC or Clang */
#include <sys/types.h>
#endif

#include <stddef.h>

#endif
