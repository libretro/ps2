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

#include "Common.h"

#include <file/file_path.h>

#include "../common/Console.h"
#include "../common/FileSystem.h"
#include "../common/StringUtil.h"

#include "GS.h"			// for sending game crc to mtgs
#include "Elfheader.h"
#include "DebugTools/SymbolMap.h"

#pragma pack(push, 1)
struct PSXEXEHeader
{
	char id[8]; // 0x000-0x007 PS-X EXE
	char pad1[8]; // 0x008-0x00F
	u32 initial_pc; // 0x010
	u32 initial_gp; // 0x014
	u32 load_address; // 0x018
	u32 file_size; // 0x01C excluding 0x800-byte header
	u32 unk0; // 0x020
	u32 unk1; // 0x024
	u32 memfill_start; // 0x028
	u32 memfill_size; // 0x02C
	u32 initial_sp_base; // 0x030
	u32 initial_sp_offset; // 0x034
	u32 reserved[5]; // 0x038-0x04B
	char marker[0x7B4]; // 0x04C-0x7FF
};
#pragma pack(pop)

#define ELF32_ST_TYPE(i) ((i) & 0xf)

u32 ElfCRC;
u32 ElfEntry;
std::pair<u32,u32> ElfTextRange;
std::string LastELF;

ElfObject::ElfObject() = default;

ElfObject::~ElfObject() = default;

bool ElfObject::CheckElfSize(s64 size)
{
	if (size > 0xfffffff)
		Console.Error("Illegal ELF file size over 2GB!");
	else if (size == -1)
		Console.Error("ELF file does not exist!");
	else if (size <= static_cast<s64>(sizeof(ELF_HEADER)))
		Console.Error("Unexpected end of ELF file.");
	else
		return true;
	return false;
}


bool ElfObject::OpenIsoFile(std::string srcfile, IsoFile& isofile, bool isPSXElf_)
{
	const u32 length = isofile.getLength();
	if (!CheckElfSize(length))
		return false;

	data.resize(length);

	const s32 rsize = isofile.read(data.data(), static_cast<s32>(length));
	if (rsize < static_cast<s32>(length))
		return false;

	filename = std::move(srcfile);
	isPSXElf = isPSXElf_;
	InitElfHeaders();
	return true;
}

bool ElfObject::OpenFile(std::string srcfile, bool isPSXElf_)
{
	RFILE *fp;
	int32_t sd_size = path_get_size(srcfile.c_str());
	if (sd_size == -1)
		return false;

	if (!isPSXElf_ && !CheckElfSize(sd_size))
		return false;

	fp = FileSystem::OpenFile(srcfile.c_str(), "rb");
	if (!fp)
		return false;

	data.resize(static_cast<size_t>(sd_size));
	if (rfread(data.data(), data.size(), 1, fp) != 1)
	{
		filestream_close(fp);
		return false;
	}

	filestream_close(fp);

	filename = std::move(srcfile);
	isPSXElf = isPSXElf_;
	InitElfHeaders();
	return true;
}

void ElfObject::InitElfHeaders()
{
	if (isPSXElf)
		return;

	const ELF_HEADER& header = GetHeader();
	if (header.e_phnum > 0)
	{
		if ((header.e_phoff + sizeof(ELF_PHR)) <= static_cast<u32>(data.size()))
			proghead = reinterpret_cast<ELF_PHR*>(&data[header.e_phoff]);
	}

	if (header.e_shnum > 0)
	{
		if ((header.e_shoff + sizeof(ELF_SHR)) <= static_cast<u32>(data.size()))
			secthead = reinterpret_cast<ELF_SHR*>(&data[header.e_shoff]);
	}
}

bool ElfObject::HasValidPSXHeader() const
{
	if (data.size() < sizeof(PSXEXEHeader))
		return false;
	const PSXEXEHeader* header          = reinterpret_cast<const PSXEXEHeader*>(data.data());
	static constexpr char expected_id[] = {'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
	if (std::memcmp(header->id, expected_id, sizeof(expected_id)) != 0)
		return false;
	return true;
}

u32 ElfObject::GetEntryPoint() const
{
	if (isPSXElf)
	{
		if (HasValidPSXHeader())
			return reinterpret_cast<const PSXEXEHeader*>(data.data())->initial_pc;
		return 0xFFFFFFFFu;
	}
	return GetHeader().e_entry;
}

std::pair<u32,u32> ElfObject::GetTextRange() const
{
	if (!isPSXElf)
	{
		const ELF_HEADER& header = GetHeader();
		for (int i = 0; i < header.e_phnum; i++)
		{
			u32 start = proghead[i].p_vaddr;
			u32 size  = proghead[i].p_memsz;

			if (start <= header.e_entry && (start+size) > header.e_entry)
				return std::make_pair(start,size);
		}
	}

	return std::make_pair(0,0);
}

u32 ElfObject::GetCRC() const
{
	u32 CRC = 0;

	const u32* srcdata = (u32*)data.data();
	for(u32 i= static_cast<u32>(data.size()) /4; i; --i, ++srcdata)
		CRC ^= *srcdata;

	return CRC;
}

void ElfObject::LoadSectionHeaders()
{
	const ELF_HEADER& header = GetHeader();
	if (!secthead || header.e_shoff > data.size())
		return;

	int i_st = -1, i_dt = -1;

	for( int i = 0 ; i < header.e_shnum ; i++ )
	{
		// dump symbol table

		if (secthead[ i ].sh_type == 0x02)
		{
			i_st = i;
			i_dt = secthead[i].sh_link;
		}
	}

	if ((i_st >= 0) && (i_dt >= 0))
	{
		const char *SymNames = (char*)(data.data() + secthead[i_dt].sh_offset);
		Elf32_Sym *eS = (Elf32_Sym*)(data.data() + secthead[i_st].sh_offset);
		Console.WriteLn("found %d symbols", secthead[i_st].sh_size / sizeof(Elf32_Sym));

		R5900SymbolMap.Clear();
		for(uint i = 1; i < (secthead[i_st].sh_size / sizeof(Elf32_Sym)); i++) {
			if ((eS[i].st_value != 0) && (ELF32_ST_TYPE(eS[i].st_info) == 2))
				R5900SymbolMap.AddLabel(&SymNames[eS[i].st_name],eS[i].st_value);
		}
	}
}

void ElfObject::LoadHeaders()
{
	LoadSectionHeaders();
}
