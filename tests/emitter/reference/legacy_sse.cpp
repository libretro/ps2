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

#include "tests/emitter/reference/legacy_internal.h"

using namespace x86Emitter;

// ------------------------------------------------------------------------
//                         Begin SSE-Only Part!
// ------------------------------------------------------------------------

/* The SSE_xxSS / SSE2_xxSD C wrappers that lived here had exactly one caller,
 * the newbind byte suite, and it now oracles the xe_* macros the recompilers
 * actually emit through. With no callers the wrappers were dead code in the
 * reference build too, so they are gone; this TU stays in the link because
 * the suites' object list expects it and because it is where any future
 * reference-only SSE helper belongs. */
