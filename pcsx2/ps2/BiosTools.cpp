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

#include <cstring>

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "../../common/Console.h"
#include "../../common/FileSystem.h"
#include "../../common/Path.h"
#include "../../common/StringUtil.h"

#include "../Common.h"
#include "../Config.h"

#include "BiosTools.h"

static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;

// --------------------------------------------------------------------------------------
// romdir structure (packing required!)
// --------------------------------------------------------------------------------------
//
#pragma pack(push, 1)

struct romdir
{
	char fileName[10];
	u16 extInfoSize;
	u32 fileSize;
};

#pragma pack(pop)

u32 BiosVersion;
u32 BiosChecksum;
u32 BiosRegion;
bool NoOSD;
bool AllowParams1;
bool AllowParams2;
std::string BiosDescription;
std::string BiosSerial;
std::string BiosPath;
BiosDebugInformation CurrentBiosInformation;

static bool LoadBiosVersion(RFILE* fp, u32& version, std::string& description, u32& region, std::string& zone, std::string& serial)
{
	romdir rd;
	for (u32 i = 0; i < 512 * 1024; i++)
	{
		if (rfread(&rd, sizeof(rd), 1, fp) != 1)
			return false;

		if (std::strncmp(rd.fileName, "RESET", sizeof(rd.fileName)) == 0)
			break; /* found romdir */
	}

	s64 fileOffset = 0;
	s64 fileSize = FileSystem::FSize64(fp);
	bool foundRomVer = false;
	char romver[14 + 1] = {}; // ascii version loaded from disk.
	char extinfo[15 + 1] = {}; // ascii version loaded from disk.

	// ensure it's a null-terminated and not zero-length string
	while (rd.fileName[0] != '\0' && strnlen(rd.fileName, sizeof(rd.fileName)) != sizeof(rd.fileName))
	{
		if (std::strncmp(rd.fileName, "EXTINFO", sizeof(rd.fileName)) == 0)
		{
			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset + 0x10, SEEK_SET) != 0 ||
				rfread(extinfo, 15, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
				break;
			serial = extinfo;
		}

		if (std::strncmp(rd.fileName, "ROMVER", sizeof(rd.fileName)) == 0)
		{

			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset, SEEK_SET) != 0 ||
				rfread(romver, 14, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
				break;

			foundRomVer = true;
		}

		if ((rd.fileSize % 0x10) == 0)
			fileOffset += rd.fileSize;
		else
			fileOffset += (rd.fileSize + 0x10) & 0xfffffff0;

		if (rfread(&rd, sizeof(rd), 1, fp) != 1)
			break;
	}

	fileOffset -= ((rd.fileSize + 0x10) & 0xfffffff0) - rd.fileSize;

	if (foundRomVer)
	{
		switch (romver[4])
		{
			// clang-format off
			case 'J': zone = "Japan";  region = 0;  break;
			case 'A': zone = "USA";    region = 1;  break;
			case 'E': zone = "Europe"; region = 2;  break;
			// case 'E': zone = "Oceania";region = 3;  break; // Not implemented
			case 'H': zone = "Asia";   region = 4;  break;
			// case 'E': zone = "Russia"; region = 3;  break; // Not implemented
			case 'C': zone = "China";  region = 6;  break;
			// case 'A': zone = "Mexico"; region = 7;  break; // Not implemented
			case 'T': zone = (romver[5]=='Z') ? "COH-H" : "T10K";   region = 8;  break;
			case 'X': zone = "Test";   region = 9;  break;
			case 'P': zone = "Free";   region = 10; break;
			// clang-format on
			default:
				zone.clear();
				zone += romver[4];
				region = 0;
				break;
		}
		// TODO: some regions can be detected only from rom1
		/* switch (rom1:DVDID[4])
		{
			// clang-format off
			case 'O': zone = "Oceania";region = 3;  break;
			case 'R': zone = "Russia"; region = 5;  break;
			case 'M': zone = "Mexico"; region = 7;  break;
			// clang-format on
		} */

		char vermaj[3] = {romver[0], romver[1], 0};
		char vermin[3] = {romver[2], romver[3], 0};
		description = StringUtil::StdStringFromFormat("%-7s v%s.%s(%c%c/%c%c/%c%c%c%c)  %s %s",
			zone.c_str(),
			vermaj, vermin,
			romver[12], romver[13], // day
			romver[10], romver[11], // month
			romver[6], romver[7], romver[8], romver[9], // year!
			(romver[5] == 'C') ? "Console" : (romver[5] == 'D') ? "Devel" :
																  "",
			serial.c_str());

		version = strtol(vermaj, (char**)NULL, 0) << 8;
		version |= strtol(vermin, (char**)NULL, 0);

		Console.WriteLn("Bios Found: %s", description.c_str());
	}
	else
		return false;

	if (fileSize < (int)fileOffset)
	{
		description += StringUtil::StdStringFromFormat(" %d%%", ((fileSize * 100) / (int)fileOffset));
		// we force users to have correct bioses,
		// not that lame scph10000 of 513KB ;-)
	}

	return true;
}

template <size_t _size>
static void ChecksumIt(u32& result, const u8 (&srcdata)[_size])
{
	for (size_t i = 0; i < _size / 4; ++i)
		result ^= ((u32*)srcdata)[i];
}

// Attempts to load a BIOS rom sub-component, by trying multiple combinations of base
// filename and extension.  The bios specified in the user's configuration is used as
// the base.
//
// Parameters:
//   ext - extension of the sub-component to load. Valid options are ROM1 and ROM2.
//
template <size_t _size>
static void LoadExtraRom(const char* ext, u8 (&dest)[_size])
{
	// Try first a basic extension concatenation (normally results in something like name.bin.rom1)
	std::string Bios1(StringUtil::StdStringFromFormat("%s.%s", BiosPath.c_str(), ext));

	s64 filesize;
	if ((filesize = path_get_size(Bios1.c_str())) <= 0)
	{
		// Try the name properly extensioned next (name.rom1)
		Bios1 = Path::ReplaceExtension(BiosPath, ext);
		if ((filesize = path_get_size(Bios1.c_str())) <= 0)
		{
			Console.WriteLn(Color_Gray, "BIOS %s module not found, skipping...", ext);
			return;
		}
	}

	RFILE *fp = FileSystem::OpenFile(Bios1.c_str(), "rb");
	if (!fp || rfread(dest, static_cast<size_t>(std::min<s64>(_size, filesize)), 1, fp) != 1)
		Console.Warning("BIOS Warning: %s could not be read (permission denied?)", ext);
	filestream_close(fp);
}

static void LoadIrx(const std::string& filename, u8* dest, size_t maxSize)
{
	RFILE *fp = FileSystem::OpenFile(filename.c_str(), "rb");
	if (!fp)
		return;

	const s64 filesize = FileSystem::FSize64(fp);
	const s64 readSize = std::min(filesize, static_cast<s64>(maxSize));
	if (rfread(dest, readSize, 1, fp) == 1)
	{
		filestream_close(fp);
		return;
	}

	Console.Warning("IRX Warning: %s could not be read", filename.c_str());
	filestream_close(fp);
}

static std::string FindBiosImage(void)
{
	Console.WriteLn("Searching for a BIOS image in '%s'...", EmuFolders::Bios.c_str());

	FileSystem::FindResultsArray results;
	if (!FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		return std::string();

	u32 version, region;
	std::string description, zone;
	for (const FILESYSTEM_FIND_DATA& fd : results)
	{
		if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
			continue;

		if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
		{
			Console.WriteLn("Using BIOS '%s' (%s %s)", fd.FileName.c_str(), description.c_str(), zone.c_str());
			return std::move(fd.FileName);
		}
	}

	Console.Error("Unable to auto locate a BIOS image");
	return std::string();
}

// Loads the configured BIOS ROM file into PS2 memory.  PS2 memory must be allocated prior to
// this method being called.
//
// Remarks:
//   This function does not fail if ROM1 or ROM2 files are missing, since none are
//   explicitly required for most emulation tasks.
//
// Exceptions:
//   BadStream - Thrown if the primary BIOS file (usually .bin) is not found, corrupted, etc.
//
bool LoadBIOS(void)
{
	std::string path = EmuConfig.FullpathToBios();
	if (path.empty() || !path_is_valid(path.c_str()))
	{
		if (!path.empty())
			Console.Warning("Configured BIOS '%s' does not exist, trying to find an alternative.",
				EmuConfig.BaseFilenames.Bios.c_str());

		path = FindBiosImage();
		if (path.empty())
			return false;
	}

	RFILE *fp = FileSystem::OpenFile(path.c_str(), "rb");
	if (!fp)
		return false;

	const s64 filesize = FileSystem::FSize64(fp);
	if (filesize <= 0)
	{
		filestream_close(fp);
		return false;
	}

	std::string zone;

	LoadBiosVersion(fp, BiosVersion, BiosDescription, BiosRegion, zone, BiosSerial);

	if (FileSystem::FSeek64(fp, 0, SEEK_SET) ||
		rfread(eeMem->ROM, static_cast<size_t>(std::min<s64>(Ps2MemSize::Rom, filesize)), 1, fp) != 1)
	{
		filestream_close(fp);
		return false;
	}

	// If file is less than 2MB it doesn't have an OSD (Devel consoles)
	// So skip HLEing OSDSys Param stuff
	if (filesize < 2465792)
		NoOSD = true;
	else
		NoOSD = false;

	BiosChecksum = 0;
	ChecksumIt(BiosChecksum, eeMem->ROM);
	BiosPath = std::move(path);

	LoadExtraRom("rom1", eeMem->ROM1);
	LoadExtraRom("rom2", eeMem->ROM2);

	if (EmuConfig.CurrentIRX.length() > 3)
		LoadIrx(EmuConfig.CurrentIRX, &eeMem->ROM[0x3C0000], sizeof(eeMem->ROM) - 0x3C0000);

	CurrentBiosInformation.eeThreadListAddr = 0;
	filestream_close(fp);
	return true;
}

bool IsBIOS(const char* filename, u32& version, std::string& description, u32& region, std::string& zone)
{
	std::string serial;
	RFILE *fp = FileSystem::OpenFile(filename, "rb");
	if (!fp)
		return false;
	// FPS2BIOS is smaller and of variable size
	bool ret = LoadBiosVersion(fp, version, description, region, zone, serial);
	filestream_close(fp);
	return ret;
}

bool IsBIOSAvailable(const std::string& full_path)
{
	// We can't use EmuConfig here since it may not be loaded yet.
	if (!full_path.empty() && path_is_valid(full_path.c_str()))
		return true;

	// No bios configured or the configured name is missing, check for one in the BIOS directory.
	const std::string auto_path(FindBiosImage());
	return !auto_path.empty() && path_is_valid(auto_path.c_str());
}
