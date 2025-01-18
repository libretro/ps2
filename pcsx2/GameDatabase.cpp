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

#include <sstream>
#include <fstream>
#include <optional>

#include <fmt/format.h>

#include "../3rdparty/rapidyaml/rapidyaml/src/ryml_std.hpp"
#include "../3rdparty/rapidyaml/rapidyaml/src/ryml.hpp"

#include "../common/Console.h"
#include "../common/FileSystem.h"
#include "../common/Path.h"
#include "../common/StringUtil.h"

#include "GameDatabase.h"
#include "GS/GS.h"
#include "Host.h"
#include "vtlb.h"

namespace GameDatabaseSchema
{
	static const char* getHWFixName(GSHWFixId id);
	static std::optional<GSHWFixId> parseHWFixName(const std::string_view& name);
	static bool isUserHackHWFix(GSHWFixId id);
} // namespace GameDatabaseSchema

namespace GameDatabase
{
	static void parseAndInsert(const char *serial, const c4::yml::NodeRef& node);
	static void initDatabase();
} // namespace GameDatabase

static constexpr char GAMEDB_YAML_FILE_NAME[] = "GameIndex.yaml";

static std::unordered_map<std::string, GameDatabaseSchema::GameEntry> s_game_db;
static std::once_flag s_load_once_flag;

std::string GameDatabaseSchema::GameEntry::memcardFiltersAsString() const
{
	return fmt::to_string(fmt::join(memcardFilters, "/"));
}

const std::string* GameDatabaseSchema::GameEntry::findPatch(u32 crc) const
{
	if (crc == 0)
		return nullptr;

	Console.WriteLn(fmt::format("[GameDB] Searching for patch with CRC '{:08X}'", crc).c_str());

	auto it = patches.find(crc);
	if (it != patches.end())
	{
		Console.WriteLn(fmt::format("[GameDB] Found patch with CRC '{:08X}'", crc).c_str());
		return &it->second;
	}

	it = patches.find(0);
	if (it != patches.end())
	{
		Console.WriteLn("[GameDB] Found and falling back to default patch");
		return &it->second;
	}
	Console.WriteLn("[GameDB] No CRC-specific patch or default patch found");
	return nullptr;
}

void GameDatabase::parseAndInsert(const char *serial, const c4::yml::NodeRef& node)
{
	GameDatabaseSchema::GameEntry gameEntry;
	if (node.has_child("name"))
		node["name"] >> gameEntry.name;
	if (node.has_child("region"))
		node["region"] >> gameEntry.region;
	if (node.has_child("roundModes"))
	{
		if (node["roundModes"].has_child("eeRoundMode"))
		{
			int eeVal = -1;
			node["roundModes"]["eeRoundMode"] >> eeVal;
			if (eeVal >= 0 && eeVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.eeRoundMode = static_cast<FPRoundMode>(eeVal);
		}
		if (node["roundModes"].has_child("eeDivRoundMode"))
		{
			int eeVal = -1;
			node["roundModes"]["eeDivRoundMode"] >> eeVal;
			if (eeVal >= 0 && eeVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.eeDivRoundMode = static_cast<FPRoundMode>(eeVal);
		}
		if (node["roundModes"].has_child("vuRoundMode"))
		{
			int vuVal = -1;
			node["roundModes"]["vuRoundMode"] >> vuVal;
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
			{
				gameEntry.vu0RoundMode = static_cast<FPRoundMode>(vuVal);
				gameEntry.vu1RoundMode = static_cast<FPRoundMode>(vuVal);
			}
		}
		if (node["roundModes"].has_child("vu0RoundMode"))
		{
			int vuVal = -1;
			node["roundModes"]["vu0RoundMode"] >> vuVal;
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.vu0RoundMode = static_cast<FPRoundMode>(vuVal);
		}
		if (node["roundModes"].has_child("vu1RoundMode"))
		{
			int vuVal = -1;
			node["roundModes"]["vu1RoundMode"] >> vuVal;
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.vu1RoundMode = static_cast<FPRoundMode>(vuVal);
		}
	}
	if (node.has_child("clampModes"))
	{
		if (node["clampModes"].has_child("eeClampMode"))
		{
			int eeVal = -1;
			node["clampModes"]["eeClampMode"] >> eeVal;
			gameEntry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(eeVal);
		}
		if (node["clampModes"].has_child("vuClampMode"))
		{
			int vuVal = -1;
			node["clampModes"]["vuClampMode"] >> vuVal;
			gameEntry.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
			gameEntry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
		if (node["clampModes"].has_child("vu0ClampMode"))
		{
			int vuVal = -1;
			node["clampModes"]["vu0ClampMode"] >> vuVal;
			gameEntry.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
		if (node["clampModes"].has_child("vu1ClampMode"))
		{
			int vuVal = -1;
			node["clampModes"]["vu1ClampMode"] >> vuVal;
			gameEntry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
	}

	// Validate game fixes, invalid ones will be dropped!
	if (node.has_child("gameFixes") && node["gameFixes"].has_children())
	{
		for (const auto& n : node["gameFixes"].children())
		{
			bool fixValidated = false;
			auto fix = std::string(n.val().str, n.val().len);

			// Enum values don't end with Hack, but gamedb does, so remove it before comparing.
			if (StringUtil::EndsWith(fix, "Hack"))
			{
				fix.erase(fix.size() - 4);
				for (GamefixId id = GamefixId_FIRST; id < pxEnumEnd; ++id)
				{
					if (fix.compare(EnumToString(id)) == 0 &&
						std::find(gameEntry.gameFixes.begin(), gameEntry.gameFixes.end(), id) == gameEntry.gameFixes.end())
					{
						gameEntry.gameFixes.push_back(id);
						fixValidated = true;
						break;
					}
				}
			}

			if (!fixValidated)
				Console.Error("[GameDB] Invalid gamefix: '{%s}', specified for serial: '{%s}'. Dropping!", fix.c_str(), serial);
		}
	}

	if (node.has_child("speedHacks") && node["speedHacks"].has_children())
	{
		for (const auto& n : node["speedHacks"].children())
		{
			const std::string_view id_view = std::string_view(n.key().str, n.key().len);
			const std::string_view value_view = std::string_view(n.val().str, n.val().len);
			const std::optional<SpeedHack> id = Pcsx2Config::SpeedhackOptions::ParseSpeedHackName(id_view);
			const std::optional<int> value = StringUtil::FromChars<int>(value_view);

			if (id.has_value() && value.has_value() &&
				std::none_of(gameEntry.speedHacks.begin(), gameEntry.speedHacks.end(),
					[&id](const auto& it) { return it.first == id.value(); }))
				gameEntry.speedHacks.emplace_back(id.value(), value.value());
			else
				Console.Error("[GameDB] Invalid speedhack: '{%s}', specified for serial: '{%s}'. Dropping!", std::string(id_view).c_str(), value_view);
		}
	}

	if (node.has_child("gsHWFixes"))
	{
		for (const auto& n : node["gsHWFixes"].children())
		{
			const std::string_view id_name(n.key().data(), n.key().size());
			std::optional<GameDatabaseSchema::GSHWFixId> id = GameDatabaseSchema::parseHWFixName(id_name);
			std::optional<s32> value;
			if (id.has_value() && (id.value() == GameDatabaseSchema::GSHWFixId::GetSkipCount ||
									  id.value() == GameDatabaseSchema::GSHWFixId::BeforeDraw ||
									  id.value() == GameDatabaseSchema::GSHWFixId::MoveHandler))
			{
				const std::string_view str_value(n.has_val() ? std::string_view(n.val().data(), n.val().size()) : std::string_view());
				if (id.value() == GameDatabaseSchema::GSHWFixId::GetSkipCount)
					value = GSLookupGetSkipCountFunctionId(str_value);
				else if (id.value() == GameDatabaseSchema::GSHWFixId::BeforeDraw)
					value = GSLookupBeforeDrawFunctionId(str_value);
				else if (id.value() == GameDatabaseSchema::GSHWFixId::MoveHandler)
					value = GSLookupMoveHandlerFunctionId(str_value);

				if (value.value_or(-1) < 0)
				{
					Console.Error("[GameDB] Invalid GS HW Fix Value for '{%s}' in '{%s}': '{%s}'", std::string(id_name).c_str(), serial, std::string(str_value).c_str());
					continue;
				}
			}
			else
				value = n.has_val() ? StringUtil::FromChars<s32>(std::string_view(n.val().data(), n.val().size())) : 1;

			if (!id.has_value() || !value.has_value())
			{
				Console.Error("[GameDB] Invalid GS HW Fix: '{%s}' specified for serial '{%s}'. Dropping!", std::string(id_name).c_str(), serial);
				continue;
			}

			gameEntry.gsHWFixes.emplace_back(id.value(), value.value());
		}
	}

	// Memory Card Filters - Store as a vector to allow flexibility in the future
	// - currently they are used as a '\n' delimited string in the app
	if (node.has_child("memcardFilters") && node["memcardFilters"].has_children())
	{
		for (const auto& n : node["memcardFilters"].children())
		{
			auto memcardFilter = std::string(n.val().str, n.val().len);
			gameEntry.memcardFilters.emplace_back(std::move(memcardFilter));
		}
	}

	// Game Patches
	if (node.has_child("patches") && node["patches"].has_children())
	{
		for (const auto& n : node["patches"].children())
		{
			// use a crc of 0 for default patches
			const std::string_view crc_str(n.key().str, n.key().len);
			const std::optional<u32> crc = ((crc_str.length() == 7) && (Strncasecmp(crc_str.data(), "default", 7) == 0)) ? std::optional<u32>(0) : StringUtil::FromChars<u32>(crc_str, 16);
			if (!crc.has_value())
			{
				Console.Error("[GameDB] Invalid CRC '{%s}' found for serial: '{%s}'. Skipping!", std::string(crc_str).c_str(), serial);
				continue;
			}
			if (gameEntry.patches.find(crc.value()) != gameEntry.patches.end())
			{
				Console.Error("[GameDB] Duplicate CRC '{%s}' found for serial: '{%s}'. Skipping, CRCs are case-insensitive!", std::string(crc_str).c_str(), std::string(serial).c_str());
				continue;
			}

			std::string patch;
			if (n.has_child("content"))
				n["content"] >> patch;
			gameEntry.patches.emplace(crc.value(), std::move(patch));
		}
	}

	if (node.has_child("dynaPatches") && node["dynaPatches"].has_children())
	{
		for (const auto& n : node["dynaPatches"].children())
		{
			DynamicPatch patch;

			if (n.has_child("pattern") && n["pattern"].has_children())
			{
				for (const auto& db_pattern : n["pattern"].children())
				{
					DynamicPatchEntry entry;
					db_pattern["offset"] >> entry.offset;
					db_pattern["value"] >> entry.value;

					patch.pattern.push_back(entry);
				}
				for (const auto& db_replacement : n["replacement"].children())
				{
					DynamicPatchEntry entry;
					db_replacement["offset"] >> entry.offset;
					db_replacement["value"] >> entry.value;

					patch.replacement.push_back(entry);
				}
			}
			gameEntry.dynaPatches.push_back(patch);
		}
	}

	s_game_db.emplace(std::move(serial), std::move(gameEntry));
}

static const char* s_round_modes[static_cast<u32>(FPRoundMode::MaxCount)] = {
	"Nearest",
	"NegativeInfinity",
	"PositiveInfinity",
	"Chop"
};

static const char* s_gs_hw_fix_names[] = {
	"autoFlush",
	"cpuFramebufferConversion",
	"readTCOnClose",
	"disableDepthSupport",
	"preloadFrameData",
	"disablePartialInvalidation",
	"textureInsideRT",
	"alignSprite",
	"mergeSprite",
	"mipmap",
	"forceEvenSpritePosition",
	"bilinearUpscale",
	"nativePaletteDraw",
	"estimateTextureRegion",
	"PCRTCOffsets",
	"PCRTCOverscan",
	"trilinearFiltering",
	"skipDrawStart",
	"skipDrawEnd",
	"halfBottomOverride",
	"halfPixelOffset",
	"roundSprite",
	"nativeScaling",
	"texturePreloading",
	"deinterlace",
	"cpuSpriteRenderBW",
	"cpuSpriteRenderLevel",
	"cpuCLUTRender",
	"gpuTargetCLUT",
	"gpuPaletteConversion",
	"minimumBlendingLevel",
	"maximumBlendingLevel",
	"recommendedBlendingLevel",
	"getSkipCount",
	"beforeDraw",
	"moveHandler",
};

const char* GameDatabaseSchema::getHWFixName(GSHWFixId id)
{
	return s_gs_hw_fix_names[static_cast<u32>(id)];
}

static std::optional<GameDatabaseSchema::GSHWFixId> GameDatabaseSchema::parseHWFixName(const std::string_view& name)
{
	for (u32 i = 0; i < std::size(s_gs_hw_fix_names); i++)
	{
		if (name.compare(s_gs_hw_fix_names[i]) == 0)
			return static_cast<GameDatabaseSchema::GSHWFixId>(i);
	}

	return std::nullopt;
}

bool GameDatabaseSchema::isUserHackHWFix(GSHWFixId id)
{
	switch (id)
	{
		case GSHWFixId::Deinterlace:
		case GSHWFixId::Mipmap:
		case GSHWFixId::TexturePreloading:
		case GSHWFixId::TrilinearFiltering:
		case GSHWFixId::MinimumBlendingLevel:
		case GSHWFixId::MaximumBlendingLevel:
		case GSHWFixId::RecommendedBlendingLevel:
		case GSHWFixId::PCRTCOffsets:
		case GSHWFixId::PCRTCOverscan:
		case GSHWFixId::GetSkipCount:
		case GSHWFixId::BeforeDraw:
		case GSHWFixId::MoveHandler:
			return false;
		default:
			break;
	}
	return true;
}

u32 GameDatabaseSchema::GameEntry::applyGameFixes(Pcsx2Config& config, bool applyAuto) const
{
	// Only apply core game fixes if the user has enabled them.
	if (!applyAuto)
		Console.Warning("[GameDB] Game Fixes are disabled");

	u32 num_applied_fixes = 0;

	if (eeDivRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing EE/FPU divison roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeDivRoundMode)]);
			config.Cpu.FPUDivFPCR.SetRoundMode(eeDivRoundMode);
		}
		else
		{
			Console.Warning("[GameDB] Skipping changing EE/FPU roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
		}
	}

	if (eeRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing EE/FPU roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
			config.Cpu.FPUFPCR.SetRoundMode(eeRoundMode);
		}
		else
			Console.Warning("[GameDB] Skipping changing EE/FPU roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
	}

	if (vu0RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing VU0 roundmode to %d [%s]", vu0RoundMode, s_round_modes[static_cast<u8>(vu0RoundMode)]);
			config.Cpu.VU0FPCR.SetRoundMode(vu0RoundMode);
		}
		else
			Console.Warning("[GameDB] Skipping changing VU0 roundmode to %d [%s]", vu0RoundMode, s_round_modes[static_cast<u8>(vu0RoundMode)]);
	}

	if (vu1RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing VU1 roundmode to %d [%s]", vu1RoundMode, s_round_modes[static_cast<u8>(vu1RoundMode)]);
			config.Cpu.VU1FPCR.SetRoundMode(vu1RoundMode);
		}
		else
			Console.Warning("[GameDB] Skipping changing VU1 roundmode to %d [%s]", vu1RoundMode, s_round_modes[static_cast<u8>(vu1RoundMode)]);
	}

	if (eeClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(eeClampMode);
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing EE/FPU clamp mode [mode=%d]", clampMode);
			config.Cpu.Recompiler.fpuOverflow = (clampMode >= 1);
			config.Cpu.Recompiler.fpuExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.fpuFullMode = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			Console.Warning("[GameDB] Skipping changing EE/FPU clamp mode [mode=%d]", clampMode);
	}

	if (vu0ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(vu0ClampMode);
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing VU0 clamp mode [mode=%d]", clampMode);
			config.Cpu.Recompiler.vu0Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu0ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu0SignOverflow = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			Console.Warning("[GameDB] Skipping changing VU0 clamp mode [mode=%d]", clampMode);
	}

	if (vu1ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(vu1ClampMode);
		if (applyAuto)
		{
			Console.WriteLn("(GameDB) Changing VU1 clamp mode [mode=%d]", clampMode);
			config.Cpu.Recompiler.vu1Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu1ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu1SignOverflow = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			Console.Warning("[GameDB] Skipping changing VU1 clamp mode [mode=%d]", clampMode);
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (const auto& it : speedHacks)
	{
		if (!applyAuto)
		{
			Console.Warning("[GameDB] Skipping setting Speedhack '%s' to [mode=%d]",
					Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it.first), it.second);
			continue;
		}
		// Legacy note - speedhacks are setup in the GameDB as integer values, but
		// are effectively booleans like the gamefixes
		config.Speedhacks.Set(it.first, it.second);
		Console.WriteLn("(GameDB) Setting Speedhack '%s' to [mode=%d]",
				Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it.first), it.second);
		num_applied_fixes++;
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (const GamefixId id : gameFixes)
	{
		if (!applyAuto)
		{
			Console.Warning("[GameDB] Skipping Gamefix: %s", EnumToString(id));
			continue;
		}
		// if the fix is present, it is said to be enabled
		config.Gamefixes.Set(id, true);
		Console.WriteLn("(GameDB) Enabled Gamefix: %s", EnumToString(id));
		num_applied_fixes++;

		// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
		if (id == Fix_GoemonTlbMiss && true)
			vtlb_Alloc_Ppmap();
	}

	return num_applied_fixes;
}

bool GameDatabaseSchema::GameEntry::configMatchesHWFix(const Pcsx2Config::GSOptions& config, GSHWFixId id, int value)
{
	switch (id)
	{
		case GSHWFixId::AutoFlush:
			return (static_cast<int>(config.UserHacks_AutoFlush) == value);

		case GSHWFixId::CPUFramebufferConversion:
			return (static_cast<int>(config.UserHacks_CPUFBConversion) == value);

		case GSHWFixId::FlushTCOnClose:
			return (static_cast<int>(config.UserHacks_ReadTCOnClose) == value);

		case GSHWFixId::DisableDepthSupport:
			return (static_cast<int>(config.UserHacks_DisableDepthSupport) == value);

		case GSHWFixId::PreloadFrameData:
			return (static_cast<int>(config.PreloadFrameWithGSData) == value);

		case GSHWFixId::DisablePartialInvalidation:
			return (static_cast<int>(config.UserHacks_DisablePartialInvalidation) == value);

		case GSHWFixId::TextureInsideRT:
			return (static_cast<int>(config.UserHacks_TextureInsideRt) == value);

		case GSHWFixId::AlignSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_AlignSpriteX) == value);

		case GSHWFixId::MergeSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_MergePPSprite) == value);

		case GSHWFixId::ForceEvenSpritePosition:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_ForceEvenSpritePosition) == value);

		case GSHWFixId::BilinearUpscale:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_BilinearHack) == value);

		case GSHWFixId::NativePaletteDraw:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_NativePaletteDraw) == value);

		case GSHWFixId::EstimateTextureRegion:
			return (static_cast<int>(config.UserHacks_EstimateTextureRegion) == value);

		case GSHWFixId::PCRTCOffsets:
			return (static_cast<int>(config.PCRTCOffsets) == value);

		case GSHWFixId::PCRTCOverscan:
			return (static_cast<int>(config.PCRTCOverscan) == value);

		case GSHWFixId::Mipmap:
			return (static_cast<int>(config.HWMipmapMode) == value);

		case GSHWFixId::TrilinearFiltering:
			return (config.TriFilter == TriFiltering::Automatic || static_cast<int>(config.TriFilter) == value);

		case GSHWFixId::SkipDrawStart:
			return (config.SkipDrawStart == value);

		case GSHWFixId::SkipDrawEnd:
			return (config.SkipDrawEnd == value);

		case GSHWFixId::HalfPixelOffset:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_HalfPixelOffset == static_cast<GSHalfPixelOffset>(value));

		case GSHWFixId::RoundSprite:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_RoundSprite == value);

		case GSHWFixId::NativeScaling:
			return (static_cast<int>(config.UserHacks_NativeScaling) == value);

		case GSHWFixId::TexturePreloading:
			return (static_cast<int>(config.TexturePreloading) <= value);

		case GSHWFixId::Deinterlace:
			return (config.InterlaceMode == GSInterlaceMode::Automatic || static_cast<int>(config.InterlaceMode) == value);

		case GSHWFixId::CPUSpriteRenderBW:
			return (config.UserHacks_CPUSpriteRenderBW == value);

		case GSHWFixId::CPUSpriteRenderLevel:
			return (config.UserHacks_CPUSpriteRenderLevel == value);

		case GSHWFixId::CPUCLUTRender:
			return (config.UserHacks_CPUCLUTRender == value);

		case GSHWFixId::GPUTargetCLUT:
			return (static_cast<int>(config.UserHacks_GPUTargetCLUTMode) == value);

		case GSHWFixId::GPUPaletteConversion:
			return (config.GPUPaletteConversion == ((value > 1) ? (config.TexturePreloading == TexturePreloadingLevel::Full) : (value != 0)));

		case GSHWFixId::MinimumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) >= value);

		case GSHWFixId::MaximumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) <= value);

		case GSHWFixId::RecommendedBlendingLevel:
			return true;

		case GSHWFixId::GetSkipCount:
			return (static_cast<int>(config.GetSkipCountFunctionId) == value);

		case GSHWFixId::BeforeDraw:
			return (static_cast<int>(config.BeforeDrawFunctionId) == value);
		case GSHWFixId::MoveHandler:
			return (static_cast<int>(config.MoveHandlerFunctionId) == value);

		default:
			return false;
	}
}

u32 GameDatabaseSchema::GameEntry::applyGSHardwareFixes(Pcsx2Config::GSOptions& config) const
{
	std::string disabled_fixes;

	// Only apply GS HW fixes if the user hasn't manually enabled HW fixes.
	const bool apply_auto_fixes = !config.ManualUserHacks;
	if (!apply_auto_fixes)
		Console.Warning("[GameDB] Manual GS hardware renderer fixes are enabled, not using automatic hardware renderer fixes from GameDB.");

	u32 num_applied_fixes = 0;
	for (const auto& [id, value] : gsHWFixes)
	{
		if (isUserHackHWFix(id) && !apply_auto_fixes)
		{
			if (configMatchesHWFix(config, id, value))
				continue;

			Console.Warning("[GameDB] Skipping GS Hardware Fix: %s to [mode=%d]", getHWFixName(id), value);
			fmt::format_to(std::back_inserter(disabled_fixes), "{} {} = {}", disabled_fixes.empty() ? "  " : "\n  ", getHWFixName(id), value);
			continue;
		}

		switch (id)
		{
			case GSHWFixId::AutoFlush:
			{
				if (value >= 0 && value <= static_cast<int>(GSHWAutoFlushLevel::Enabled))
					config.UserHacks_AutoFlush = static_cast<GSHWAutoFlushLevel>(value);
			}
			break;

			case GSHWFixId::CPUFramebufferConversion:
				config.UserHacks_CPUFBConversion = (value > 0);
				break;

			case GSHWFixId::FlushTCOnClose:
				config.UserHacks_ReadTCOnClose = (value > 0);
				break;

			case GSHWFixId::DisableDepthSupport:
				config.UserHacks_DisableDepthSupport = (value > 0);
				break;

			case GSHWFixId::PreloadFrameData:
				config.PreloadFrameWithGSData = (value > 0);
				break;

			case GSHWFixId::DisablePartialInvalidation:
				config.UserHacks_DisablePartialInvalidation = (value > 0);
				break;

			case GSHWFixId::TextureInsideRT:
			{
				if (value >= 0 && value <= static_cast<int>(GSTextureInRtMode::MergeTargets))
					config.UserHacks_TextureInsideRt = static_cast<GSTextureInRtMode>(value);
			}
			break;

			case GSHWFixId::AlignSprite:
				config.UserHacks_AlignSpriteX = (value > 0);
				break;

			case GSHWFixId::MergeSprite:
				config.UserHacks_MergePPSprite = (value > 0);
				break;

			case GSHWFixId::ForceEvenSpritePosition:
				config.UserHacks_ForceEvenSpritePosition = (value > 0);
				break;

			case GSHWFixId::BilinearUpscale:
				if (value >= 0 && value < static_cast<int>(GSBilinearDirtyMode::MaxCount))
					config.UserHacks_BilinearHack = static_cast<GSBilinearDirtyMode>(value);
				break;

			case GSHWFixId::NativePaletteDraw:
				config.UserHacks_NativePaletteDraw = (value > 0);
				break;

			case GSHWFixId::EstimateTextureRegion:
				config.UserHacks_EstimateTextureRegion = (value > 0);
				break;

			case GSHWFixId::PCRTCOffsets:
				config.PCRTCOffsets = (value > 0);
				break;

			case GSHWFixId::PCRTCOverscan:
				config.PCRTCOverscan = (value > 0);
				break;

			case GSHWFixId::Mipmap:
				if (value >= 0 && value < static_cast<int>(GSHWMipmapMode::MaxCount))
					config.HWMipmapMode = static_cast<GSHWMipmapMode>(value > 0);
				break;

			case GSHWFixId::TrilinearFiltering:
			{
				if (value >= 0 && value <= static_cast<int>(TriFiltering::Forced))
				{
					if (config.TriFilter == TriFiltering::Automatic)
						config.TriFilter = static_cast<TriFiltering>(value);
					else if (config.TriFilter > TriFiltering::Off)
						Console.Warning("[GameDB] Game requires trilinear filtering to be disabled.");
				}
			}
			break;

			case GSHWFixId::SkipDrawStart:
				config.SkipDrawStart = value;
				break;

			case GSHWFixId::SkipDrawEnd:
				config.SkipDrawEnd = value;
				break;

			case GSHWFixId::HalfPixelOffset:
				if (value >= 0 && value < static_cast<int>(GSHalfPixelOffset::MaxCount))
					config.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(value);
				break;

			case GSHWFixId::RoundSprite:
				config.UserHacks_RoundSprite = value;
				break;

			case GSHWFixId::NativeScaling:
				config.UserHacks_NativeScaling = static_cast<GSNativeScaling>(value);
				break;

			case GSHWFixId::TexturePreloading:
			{
				if (value >= 0 && value <= static_cast<int>(TexturePreloadingLevel::Full))
					config.TexturePreloading = std::min(config.TexturePreloading, static_cast<TexturePreloadingLevel>(value));
			}
			break;

			case GSHWFixId::Deinterlace:
			{
				if (value >= static_cast<int>(GSInterlaceMode::Automatic) && value < static_cast<int>(GSInterlaceMode::Count))
				{
					if (config.InterlaceMode == GSInterlaceMode::Automatic)
						config.InterlaceMode = static_cast<GSInterlaceMode>(value);
					else
						Console.Warning("[GameDB] Game requires different deinterlace mode but it has been overridden by user setting.");
				}
			}
			break;

			case GSHWFixId::CPUSpriteRenderBW:
				config.UserHacks_CPUSpriteRenderBW = value;
				break;

			case GSHWFixId::CPUSpriteRenderLevel:
				config.UserHacks_CPUSpriteRenderLevel = value;
				break;

			case GSHWFixId::CPUCLUTRender:
				config.UserHacks_CPUCLUTRender = value;
				break;

			case GSHWFixId::GPUTargetCLUT:
			{
				if (value >= 0 && value <= static_cast<int>(GSGPUTargetCLUTMode::InsideTarget))
					config.UserHacks_GPUTargetCLUTMode = static_cast<GSGPUTargetCLUTMode>(value);
			}
			break;

			case GSHWFixId::GPUPaletteConversion:
			{
				// if 2, enable paltex when preloading is full, otherwise leave as-is
				if (value > 1)
					config.GPUPaletteConversion = (config.TexturePreloading == TexturePreloadingLevel::Full) ? true : config.GPUPaletteConversion;
				else
					config.GPUPaletteConversion = (value != 0);
			}
			break;

			case GSHWFixId::MinimumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = std::max(config.AccurateBlendingUnit, static_cast<AccBlendLevel>(value));
			}
			break;

			case GSHWFixId::MaximumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = std::min(config.AccurateBlendingUnit, static_cast<AccBlendLevel>(value));
			}
			break;

			case GSHWFixId::RecommendedBlendingLevel:
				break;

			case GSHWFixId::GetSkipCount:
				config.GetSkipCountFunctionId = static_cast<s16>(value);
				break;

			case GSHWFixId::BeforeDraw:
				config.BeforeDrawFunctionId = static_cast<s16>(value);
				break;

			case GSHWFixId::MoveHandler:
				config.MoveHandlerFunctionId = static_cast<s16>(value);
				break;

			default:
				break;
		}

		Console.WriteLn("[GameDB] Enabled GS Hardware Fix: %s to [mode=%d]", getHWFixName(id), value);
		num_applied_fixes++;
	}

	// fixup skipdraw range just in case the db has a bad range (but the linter should catch this)
	config.SkipDrawEnd = std::max(config.SkipDrawStart, config.SkipDrawEnd);

	return num_applied_fixes;
}

void GameDatabase::initDatabase()
{
	ryml::Callbacks rymlCallbacks = ryml::get_callbacks();
	rymlCallbacks.m_error = [](const char* msg, size_t msg_len, ryml::Location loc, void*) {
		Console.Error("[YAML] Parsing error at {%s}:{%s} (bufpos={%s}): {%s}",
			loc.line, loc.col, loc.offset, msg);
	};
	ryml::set_callbacks(rymlCallbacks);
	c4::set_error_callback([](const char* msg, size_t msg_size) {
		Console.Error("[YAML] Internal Parsing error: {%s}",
			msg);
	});
	auto buf = Host::ReadResourceFileToString(GAMEDB_YAML_FILE_NAME);
	if (!buf.has_value())
	{
		Console.Error("[GameDB] Unable to open GameDB file, file does not exist.");
		return;
	}

	ryml::Tree tree = ryml::parse_in_arena(c4::to_csubstr(buf.value()));
	ryml::NodeRef root = tree.rootref();

	for (const auto& n : root.children())
	{
		auto serial = StringUtil::toLower(std::string(n.key().str, n.key().len));

		// Serials and CRCs must be inserted as lower-case, as that is how they are retrieved
		// this is because the application may pass a lowercase CRC or serial along
		//
		// However, YAML's keys are as expected case-sensitive, so we have to explicitly do our own duplicate checking
		if (s_game_db.count(serial) == 1)
		{
			Console.Error("[GameDB] Duplicate serial '{%s}' found in GameDB. Skipping, Serials are case-insensitive!", serial.c_str());
			continue;
		}
		if (n.is_map())
			parseAndInsert(serial.c_str(), n);
	}

	ryml::reset_callbacks();
}

void GameDatabase::ensureLoaded()
{
	std::call_once(s_load_once_flag, []() {
		Console.WriteLn("[GameDB] Has not been initialized yet, initializing...");
		initDatabase();
		Console.WriteLn("[GameDB] %zu games on record", s_game_db.size());
	});
}

const GameDatabaseSchema::GameEntry* GameDatabase::findGame(const std::string_view& serial)
{
	GameDatabase::ensureLoaded();

	auto iter = s_game_db.find(StringUtil::toLower(serial));
	return (iter != s_game_db.end()) ? &iter->second : nullptr;
}
