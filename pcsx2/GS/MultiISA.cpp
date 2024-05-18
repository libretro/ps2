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

#include "MultiISA.h"

// Keep init order by defining these here

#include "GSXXH.h"

u64 (&MultiISAFunctions::GSXXH3_64_Long)(const void* data, size_t len) = MULTI_ISA_SELECT(GSXXH3_64_Long);
u32 (&MultiISAFunctions::GSXXH3_64_Update)(void* state, const void* data, size_t len) = MULTI_ISA_SELECT(GSXXH3_64_Update);
u64 (&MultiISAFunctions::GSXXH3_64_Digest)(void* state) = MULTI_ISA_SELECT(GSXXH3_64_Digest);
