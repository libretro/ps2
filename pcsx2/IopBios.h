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

#pragma once

#define IOP_ENOENT 2
#define IOP_EIO 5
#define IOP_ENOMEM 12
#define IOP_EACCES 13
#define IOP_ENODEV 19
#define IOP_EISDIR 21
#define IOP_EMFILE 24
#define IOP_EROFS 30

#define IOP_O_RDONLY 0x001
#define IOP_O_WRONLY 0x002
#define IOP_O_RDWR 0x003
#define IOP_O_APPEND 0x100
#define IOP_O_CREAT 0x200
#define IOP_O_TRUNC 0x400
#define IOP_O_EXCL 0x800

#define IOP_SEEK_SET 0
#define IOP_SEEK_CUR 1
#define IOP_SEEK_END 2

/* The IOP HLE file and directory objects.
 *
 * These were two abstract classes with pure-virtual close() and defaulted
 * virtual read/write/lseek returning -IOP_EIO. Each had exactly one
 * subclass -- HostFile and HostDir in IopBios.cpp -- so every virtual call
 * resolved to a single implementation and the defaulted overrides were
 * never reached. The abstraction is gone: the concrete types are declared
 * here and the call sites call them directly. Both types are local to
 * IopBios.cpp -- nothing outside it ever named them -- so the header no
 * longer declares them at all.
 */
typedef int (*irxHLE)(); // return 1 if handled, otherwise 0
typedef void (*irxDEBUG)();

namespace R3000A
{
	u32 irxImportTableAddr(u32 entrypc);
	const char* irxImportFuncname(const std::string& libname, u16 index);
	irxHLE irxImportHLE(const std::string& libnam, u16 index);
	irxHLE irxImportHLECh(const char* libname, u16 index); /* C89-callable */
	irxDEBUG irxImportDebug(const std::string& libname, u16 index);
	void irxImportLog(const std::string& libnameptr, u16 index, const char* funcname);
	void irxImportLog_rec(u32 import_table, u16 index, const char* funcname);
	int irxImportExec(u32 import_table, u16 index);
	int irxImportExecCached(u32 stubpc, u16 index); // C.71: cached psxJ hook path

	namespace ioman
	{
		void reset();
	}
} // namespace R3000A

extern void Hle_SetElfPath(const char* elfFileName);
extern void Hle_ClearElfPath();

