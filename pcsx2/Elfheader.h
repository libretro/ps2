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

#include <vector>

#include "CDVD/IsoFS/SectorSource.h"
#include "CDVD/IsoFS/IsoFS.h"

struct ELF_HEADER {
	u8	e_ident[16];	//0x7f,"ELF"  (ELF file identifier)
	u16	e_type;			//ELF type: 0=NONE, 1=REL, 2=EXEC, 3=SHARED, 4=CORE
	u16	e_machine;	  //Processor: 8=MIPS R3000
	u32	e_version;	  //Version: 1=current
	u32	e_entry;		//Entry point address
	u32	e_phoff;		//Start of program headers (offset from file start)
	u32	e_shoff;		//Start of section headers (offset from file start)
	u32	e_flags;		//Processor specific flags = 0x20924001 noreorder, mips
	u16	e_ehsize;	   //ELF header size (0x34 = 52 bytes)
	u16	e_phentsize;	//Program headers entry size
	u16	e_phnum;		//Number of program headers
	u16	e_shentsize;	//Section headers entry size
	u16	e_shnum;		//Number of section headers
	u16	e_shstrndx;	 //Section header stringtable index
};

class ElfObject final
{
	public:
		ElfObject();
		ElfObject(const ElfObject&) = delete;
		~ElfObject();

		__fi const ELF_HEADER& GetHeader() const { return *reinterpret_cast<const ELF_HEADER*>(data.data()); }
		__fi u32 GetSize() const { return static_cast<u32>(data.size()); }

		bool OpenFile(std::string srcfile);
		bool OpenIsoFile(IsoFile& isofile);

		u32 GetCRC() const;

	private:

		std::vector<u8> data;

		bool CheckElfSize(s64 size);
};

extern u32 ElfCRC;
extern u32 ElfEntry;
extern std::string LastELF;
