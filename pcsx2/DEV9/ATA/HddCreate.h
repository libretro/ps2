/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#ifndef __DEV9_HDDCREATE_H__
#define __DEV9_HDDCREATE_H__

#include <stdint.h>

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Create a sparse-where-possible HDD image of size_bytes at path.
 * Fails if the path already exists. Progress goes to the log; the
 * class machinery the C++ version had for GUI callbacks and
 * cancellation folded into the return value, as nothing in the tree
 * constructs one. Returns 0 on success, -1 on error. */
int hdd_create(const char* path, uint64_t size_bytes);

RETRO_END_DECLS

#endif /* __DEV9_HDDCREATE_H__ */
