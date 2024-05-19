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

#include "IsoFS.h"
#include "IsoFile.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <memory>
#include <cstring>

//////////////////////////////////////////////////////////////////////////
// IsoDirectory
//////////////////////////////////////////////////////////////////////////

// Used to load the Root directory from an image
IsoDirectory::IsoDirectory(SectorSource& r)
	: internalReader(r)
{
}

IsoDirectory::~IsoDirectory() = default;

bool IsoDirectory::OpenRootDirectory()
{
	IsoFileDescriptor rootDirEntry;
	bool isValid = false;
	bool done    = false;
	uint i       = 16;

	while (!done)
	{
		u8 sector[2048];
		// If this fails, we're not reading an iso, or it's bad.
		if (!internalReader.readSector(sector, i))
			break;

		if (memcmp(&sector[1], "CD001", 5) == 0)
		{
			switch (sector[0])
			{
				case 0:
					break;

				case 1:
					rootDirEntry.Load(sector + 156, 38);
					isValid = true;
					break;

				case 2:
					// Probably means Joliet (long filenames support), which PCSX2 doesn't care about.
					break;

				case 0xff:
					// Null terminator.  End of partition information.
					done = true;
					break;

				default:
					Console.Error("(IsoFS) Unknown partition type ID=%d, encountered at block 0x%x", sector[0], i);
					break;
			}
		}
		else
		{
			sector[9] = 0;
			Console.Error("(IsoFS) Invalid partition descriptor encountered at block 0x%x: '%s'", i, &sector[1]);
			break; // if no valid root partition was found, an exception will be thrown below.
		}

		++i;
	}

	if (!isValid)
	{
		Console.Error("IsoFS could not find the root directory on the ISO image.");
		return false;
	}

	return Open(rootDirEntry);
}

bool IsoDirectory::Open(const IsoFileDescriptor& directoryEntry)
{
	u8 b[257];
	// parse directory sector
	IsoFile dataStream(internalReader, directoryEntry);

	files.clear();

	uint remainingSize = directoryEntry.size;

	while (remainingSize >= 4) // hm hack :P
	{
		b[0] = dataStream.read<u8>();

		if (b[0] == 0)
			break; // or continue?

		remainingSize -= b[0];

		dataStream.read(b + 1, b[0] - 1);

		IsoFileDescriptor isoFile = IsoFileDescriptor(b, b[0]); 

		files.push_back(isoFile);
	}

	b[0] = 0;
	return true;
}

const IsoFileDescriptor& IsoDirectory::GetEntry(size_t index) const
{
	return files[index];
}

int IsoDirectory::GetIndexOf(const std::string_view& fileName) const
{
	for (unsigned int i = 0; i < files.size(); i++)
	{
		if (files[i].name == fileName)
			return i;
	}

	return -1;
}

std::optional<IsoFileDescriptor> IsoDirectory::FindFile(const std::string_view& filePath) const
{
	IsoFileDescriptor info;
	if (filePath.empty())
		return std::nullopt;

	// DOS-style parser should work fine for ISO 9660 path names.  Only practical difference
	// is case sensitivity, and that won't matter for path splitting.
	std::vector<std::string_view> parts(Path::SplitWindowsPath(filePath));
	const IsoDirectory* dir = this;
	IsoDirectory subdir(internalReader);

	// walk through path ("." and ".." entries are in the directories themselves, so even if the
	// path included . and/or .., it still works)

	// ignore the device (cdrom0:\)
	const bool has_device = (parts.front().back() == ':');

	for (size_t index = has_device ? 1 : 0; index < (parts.size() - 1); index++)
	{
		const int subdir_index = GetIndexOf(parts[index]);
		if (subdir_index < 0)
			return std::nullopt;

		const IsoFileDescriptor& subdir_entry = GetEntry(static_cast<size_t>(index));
		if (subdir_entry.IsFile() || !subdir.Open(subdir_entry))
			return std::nullopt;

		dir = &subdir;
	}

	const int file_index = dir->GetIndexOf(parts.back());
	if (file_index < 0)
		return std::nullopt;

	return GetEntry(static_cast<size_t>(file_index));
}

bool IsoDirectory::Exists(const std::string_view& filePath) const
{
	if (filePath.empty())
		return false;

	const std::optional<IsoFileDescriptor> fd(FindFile(filePath));
	return fd.has_value();
}

bool IsoDirectory::IsFile(const std::string_view& filePath) const
{
	if (filePath.empty())
		return false;

	const std::optional<IsoFileDescriptor> fd(FindFile(filePath));
	if (fd.has_value())
		return false;

	return ((fd->flags & 2) != 2);
}

IsoFileDescriptor::IsoFileDescriptor()
	: lba(0)
	, size(0)
	, flags(0)
{
	memset(&date, 0, sizeof(date));
}

IsoFileDescriptor::IsoFileDescriptor(const u8* data, int length)
{
	Load(data, length);
}

void IsoFileDescriptor::Load(const u8* data, int length)
{
	lba = (u32&)data[2];
	size = (u32&)data[10];

	date.year = data[18] + 1900;
	date.month = data[19];
	date.day = data[20];
	date.hour = data[21];
	date.minute = data[22];
	date.second = data[23];
	date.gmtOffset = data[24];

	flags = data[25];

	int fileNameLength = data[32];

	if (fileNameLength == 1)
	{
		u8 c = data[33];

		switch (c)
		{
			case 0:
				name = ".";
				break;
			case 1:
				name = "..";
				break;
			default:
				name = static_cast<char>(c);
				break;
		}
	}
	else
	{
		// copy string and up-convert from ascii to wxChar
		const u8* fnsrc = data + 33;
		name.assign(reinterpret_cast<const char*>(fnsrc), fileNameLength);
	}
}
