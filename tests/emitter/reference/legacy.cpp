// The reference emitter's ModRM helper. The C-API wrappers this file used
// to carry moved into common/emitter/legacy_instructions.h and were retired
// with it; what is left is what only the reference build needs.
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
/*
 * ix86 core v0.6.2
 * Authors: linuzappz <linuzappz@pcsx.net>
 *			alexey silinov
 *			goldfinger
 *			zerofrog(@gmail.com)
 *			cottonvibes(@gmail.com)
 */

//------------------------------------------------------------------
// ix86 legacy emitter functions
//------------------------------------------------------------------

#include "tests/emitter/reference/legacy_internal.h"

__fi void ModRM(uint mod, uint reg, uint rm)
{
	xWrite8((mod << 6) | (reg << 3) | rm);
}

using namespace x86Emitter;

//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
// From here on are instructions that have NOT been implemented in the new emitter.
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////


