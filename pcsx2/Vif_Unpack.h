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

struct vifStruct;

typedef void (*UNPACKFUNCTYPE)(void* dest, const void* src);

typedef void (*UNPACKFUNCTYPE_u32)(u32* dest, const u32* src);
typedef void (*UNPACKFUNCTYPE_u16)(u32* dest, const u16* src);
typedef void (*UNPACKFUNCTYPE_u8) (u32* dest, const  u8* src);
typedef void (*UNPACKFUNCTYPE_s32)(u32* dest, const s32* src);
typedef void (*UNPACKFUNCTYPE_s16)(u32* dest, const s16* src);
typedef void (*UNPACKFUNCTYPE_s8) (u32* dest, const  s8* src);

alignas(16) extern const u8 nVifT[16];

template<int idx> extern int  nVifUnpack (const u8* data);
extern void resetNewVif(int idx);

template< int idx >
extern void vifUnpackSetup(const u32* data);
