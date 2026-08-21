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

#pragma once

/* Reads one 2048-byte sector by LBA.
 *
 * This was an abstract class with a pure-virtual readSector and exactly one
 * implementation, IsoFSCDVD, which forwards to DoCDVDreadSector. The vtable
 * bought nothing, so it is a free function: the IsoFS walkers call it
 * directly and no longer carry a reference to a source object. */
bool isofs_read_sector(unsigned char* buffer, int lba);
