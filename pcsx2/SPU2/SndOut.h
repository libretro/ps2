/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "../../common/Pcsx2Types.h"

typedef struct StereoOut32
{
	s32 Left;
	s32 Right;
} StereoOut32;

typedef struct StereoOut16
{
	s16 Left;
	s16 Right;
} StereoOut16;

/* Carries the DC blocker's state, which is the emulator's own filter and
 * so is kept finer than an output step. */
typedef struct StereoOut64
{
	s64 Left;
	s64 Right;
} StereoOut64;
