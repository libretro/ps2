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

/* What a subsystem's freeze entry point is being asked for, and the block it
 * is handed. Both languages call those entry points, so they live apart from
 * SaveState.h and its standard-library includes. */

#include "../common/Pcsx2Types.h"

typedef enum FreezeAction
{
	FREEZE_LOAD,
	FREEZE_SAVE,
	FREEZE_SIZE
} FreezeAction;

/* The block passed between the core and a subsystem. This dates from before
 * the plugin merge and is sized in int, so a state written by one build is
 * not portable to another with a different int. */
typedef struct freezeData
{
	int size;
	u8 *data;
} freezeData;
