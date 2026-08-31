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
#include "HostFS.h"
#include "../common/StringUtil.h"

#include "GS.h"			// for sending game crc to mtgs
#include "Elfheader.h"

u32 ElfCRC;
u32 ElfEntry;
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


bool ElfObject::OpenIsoFile(IsoFile& isofile)
{
	const u32 length = isofile.getLength();
	if (!CheckElfSize(length))
		return false;

	data.resize(length);

	const s32 rsize = isofile.read(data.data(), static_cast<s32>(length));
	if (rsize < static_cast<s32>(length))
		return false;

	return true;
}

bool ElfObject::OpenFile(std::string srcfile)
{
	RFILE *fp;
	int32_t sd_size = path_get_size(srcfile.c_str());
	if (sd_size == -1)
		return false;

	if (!CheckElfSize(sd_size))
		return false;

	fp = filestream_open(srcfile.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return false;

	data.resize(static_cast<size_t>(sd_size));
	if (filestream_read(fp, data.data(), data.size()) != (int64_t)(data.size()))
	{
		filestream_close(fp);
		return false;
	}

	filestream_close(fp);
	return true;
}

u32 ElfObject::GetCRC() const
{
	u32 CRC = 0;

	const u32* srcdata = (u32*)data.data();
	for(u32 i= static_cast<u32>(data.size()) /4; i; --i, ++srcdata)
		CRC ^= *srcdata;

	return CRC;
}

