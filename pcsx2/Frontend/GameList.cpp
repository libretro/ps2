/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include "PrecompiledHeader.h"

#include "Frontend/GameList.h"
#include "HostSettings.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HeterogeneousContainers.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/StringUtil.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string_view>
#include <utility>

#include "CDVD/CDVD.h"
#include "Elfheader.h"
#include "VMManager.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

namespace GameList
{
	enum : u32
	{
		GAME_LIST_CACHE_SIGNATURE = 0x45434C47,
		GAME_LIST_CACHE_VERSION = 32,


		PLAYED_TIME_SERIAL_LENGTH = 32,
		PLAYED_TIME_LAST_TIME_LENGTH = 20, // uint64
		PLAYED_TIME_TOTAL_TIME_LENGTH = 20, // uint64
		PLAYED_TIME_LINE_LENGTH = PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1 + PLAYED_TIME_TOTAL_TIME_LENGTH,
	};

	using CacheMap = UnorderedStringMap<Entry>;

	static bool GetIsoSerialAndCRC(const std::string& path, s32* disc_type, std::string* serial, u32* crc);
	static Region ParseDatabaseRegion(const std::string_view& db_region);
	static bool GetElfListEntry(const std::string& path, GameList::Entry* entry);
	static bool GetIsoListEntry(const std::string& path, GameList::Entry* entry);
} // namespace GameList

static std::vector<GameList::Entry> s_entries;
static std::recursive_mutex s_mutex;

bool GameList::GetIsoSerialAndCRC(const std::string& path, s32* disc_type, std::string* serial, u32* crc)
{
	// This isn't great, we really want to make it all thread-local...
	CDVD = &CDVDapi_Iso;
	if (CDVD->open(path.c_str()) != 0)
		return false;

	*disc_type = DoCDVDdetectDiskType();
	cdvdReloadElfInfo();

	*serial = DiscSerial;
	*crc = ElfCRC;

	DoCDVDclose();

	// TODO(Stenzek): These globals are **awful**. Clean it up.
	DiscSerial.clear();
	ElfCRC = 0;
	ElfEntry = -1;
	LastELF.clear();
	return true;
}

bool GameList::GetElfListEntry(const std::string& path, GameList::Entry* entry)
{
	const s64 file_size = FileSystem::GetPathFileSize(path.c_str());
	if (file_size <= 0)
		return false;

	ElfObject eo(path, static_cast<uint>(file_size), false);
	entry->crc = eo.getCRC();

	entry->path = path;
	entry->serial.clear();
	entry->title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));
	entry->region = Region::Other;
	entry->total_size = static_cast<u64>(file_size);
	entry->type = EntryType::ELF;
	entry->compatibility_rating = CompatibilityRating::Unknown;

	std::string disc_path(VMManager::GetDiscOverrideFromGameSettings(path));
	if (!disc_path.empty())
	{
		s32 disc_type;
		u32 disc_crc;
		if (GetIsoSerialAndCRC(disc_path, &disc_type, &entry->serial, &disc_crc))
		{
			// use serial/region/compat info from the db
			if (const GameDatabaseSchema::GameEntry* db_entry = GameDatabase::findGame(entry->serial))
			{
				entry->compatibility_rating = db_entry->compat;
				entry->region = ParseDatabaseRegion(db_entry->region);
			}
		}
	}

	return true;
}

GameList::Region GameList::ParseDatabaseRegion(const std::string_view& db_region)
{
	// clang-format off
						////// NTSC //////
						//////////////////
	if (StringUtil::StartsWith(db_region, "NTSC-B"))
		return Region::NTSC_B;
	else if (StringUtil::StartsWith(db_region, "NTSC-C"))
		return Region::NTSC_C;
	else if (StringUtil::StartsWith(db_region, "NTSC-HK"))
		return Region::NTSC_HK;
	else if (StringUtil::StartsWith(db_region, "NTSC-J"))
		return Region::NTSC_J;
	else if (StringUtil::StartsWith(db_region, "NTSC-K"))
		return Region::NTSC_K;
	else if (StringUtil::StartsWith(db_region, "NTSC-T"))
		return Region::NTSC_T;
	else if (StringUtil::StartsWith(db_region, "NTSC-U"))
		return Region::NTSC_U;
						////// PAL //////
						//////////////////
	else if (StringUtil::StartsWith(db_region, "PAL-AF"))
		return Region::PAL_AF;
	else if (StringUtil::StartsWith(db_region, "PAL-AU"))
		return Region::PAL_AU;
	else if (StringUtil::StartsWith(db_region, "PAL-A"))
		return Region::PAL_A;
	else if (StringUtil::StartsWith(db_region, "PAL-BE"))
		return Region::PAL_BE;
	else if (StringUtil::StartsWith(db_region, "PAL-E"))
		return Region::PAL_E;
	else if (StringUtil::StartsWith(db_region, "PAL-FI"))
		return Region::PAL_FI;
	else if (StringUtil::StartsWith(db_region, "PAL-F"))
		return Region::PAL_F;
	else if (StringUtil::StartsWith(db_region, "PAL-GR"))
		return Region::PAL_GR;
	else if (StringUtil::StartsWith(db_region, "PAL-G"))
		return Region::PAL_G;
	else if (StringUtil::StartsWith(db_region, "PAL-IN"))
		return Region::PAL_IN;
	else if (StringUtil::StartsWith(db_region, "PAL-I"))
		return Region::PAL_I;
	else if (StringUtil::StartsWith(db_region, "PAL-M"))
		return Region::PAL_M;
	else if (StringUtil::StartsWith(db_region, "PAL-NL"))
		return Region::PAL_NL;
	else if (StringUtil::StartsWith(db_region, "PAL-NO"))
		return Region::PAL_NO;
	else if (StringUtil::StartsWith(db_region, "PAL-P"))
		return Region::PAL_P;
	else if (StringUtil::StartsWith(db_region, "PAL-R"))
		return Region::PAL_R;
	else if (StringUtil::StartsWith(db_region, "PAL-SC"))
		return Region::PAL_SC;
	else if (StringUtil::StartsWith(db_region, "PAL-SWI"))
		return Region::PAL_SWI;
	else if (StringUtil::StartsWith(db_region, "PAL-SW"))
		return Region::PAL_SW;
	else if (StringUtil::StartsWith(db_region, "PAL-S"))
		return Region::PAL_S;
	else if (StringUtil::StartsWith(db_region, "PAL-UK"))
		return Region::PAL_UK;
	else
		return Region::Other;
	// clang-format on
}

bool GameList::GetIsoListEntry(const std::string& path, GameList::Entry* entry)
{
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(path.c_str(), &sd))
		return false;

	s32 disc_type;
	if (!GetIsoSerialAndCRC(path, &disc_type, &entry->serial, &entry->crc))
		return false;

	switch (disc_type)
	{
		case CDVD_TYPE_PSCD:
		case CDVD_TYPE_PSCDDA:
			entry->type = EntryType::PS1Disc;
			break;

		case CDVD_TYPE_PS2CD:
		case CDVD_TYPE_PS2CDDA:
		case CDVD_TYPE_PS2DVD:
			entry->type = EntryType::PS2Disc;
			break;

		case CDVD_TYPE_ILLEGAL:
		default:
			return false;
	}

	entry->path = path;
	entry->total_size = sd.Size;
	entry->compatibility_rating = CompatibilityRating::Unknown;

	if (const GameDatabaseSchema::GameEntry* db_entry = GameDatabase::findGame(entry->serial))
	{
		entry->title = std::move(db_entry->name);
		entry->compatibility_rating = db_entry->compat;
		entry->region = ParseDatabaseRegion(db_entry->region);
	}
	else
	{
		entry->title = Path::GetFileTitle(path);
		entry->region = Region::Other;
	}

	return true;
}

bool GameList::PopulateEntryFromPath(const std::string& path, GameList::Entry* entry)
{
	if (VMManager::IsElfFileName(path.c_str()))
		return GetElfListEntry(path, entry);
	return GetIsoListEntry(path, entry);
}

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
	return std::unique_lock<std::recursive_mutex>(s_mutex);
}

const GameList::Entry* GameList::GetEntryForPath(const char* path)
{
	const size_t path_length = std::strlen(path);
	for (const Entry& entry : s_entries)
	{
		if (entry.path.size() == path_length && StringUtil::Strcasecmp(entry.path.c_str(), path) == 0)
			return &entry;
	}

	return nullptr;
}
